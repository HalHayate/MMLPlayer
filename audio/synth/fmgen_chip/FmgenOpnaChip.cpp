// FmgenOpnaChip: cisc fmgen (np21w 同梱) を OPNA バックエンドにするラッパ実装。
// SUPPORT_FMGEN は CMake で全体定義済み。
#include "audio/synth/fmgen_chip/FmgenOpnaChip.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "audio/synth/opna/OpnaRegEncode.h"
#include "audio/synth/fmgen/fmgen_opna.h"

namespace {

// ch (0..5) → (port, ch_low(0..2))。FM ch4-6 は port1。
inline void ch_to_port(int ch, int& port, int& ch_low) {
	if (ch < 3) { port = 0; ch_low = ch; }
	else         { port = 1; ch_low = ch - 3; }
}

// MML op 0..3 (op1..op4) → 実機 OPNA slot 配置 (S1/S3/S2/S4 順) の addr オフセット。
inline uint8_t op_addr_offset(int op) {
	static constexpr uint8_t k_mml_op_to_slot[4] = { 0, 2, 1, 3 };
	return static_cast<uint8_t>(k_mml_op_to_slot[op] << 2);
}

}  // namespace

// ============================================================
// PIMPL: fmgen OPNA インスタンスと shadow レジスタ状態
// ============================================================
struct FmgenOpnaChip::Impl {
	FM::OPNA opna;

	// FM op shadow (1 ch = 4 op)
	struct Op {
		uint8_t dt = 0, mul = 0;   // 0x30
		uint8_t tl = 0;            // 0x40
		uint8_t ks = 0, ar = 0;    // 0x50
		uint8_t d1r = 0;           // 0x60
		uint8_t dt2 = 0, d2r = 0;  // 0x70 (dt2 は未使用)
		uint8_t sl = 0, rr = 0;    // 0x80
	} op[6][4];

	// FM ch shadow
	struct Ch {
		uint16_t fnum = 0;
		uint8_t  block = 0;
		uint8_t  alg = 0, fb = 0;          // 0xB0
		uint8_t  pan_l = 1, pan_r = 1;     // 0xB4 (既定 both)
		uint8_t  ams = 0, pms = 0;
	} ch[6];

	// SSG shadow
	uint8_t  ssg_mixer    = 0xBF;
	uint16_t ssg_tone_tp[3] = { 0, 0, 0 };
	uint8_t  ssg_volume[3]  = { 0, 0, 0 };
	uint16_t ssg_env_period = 0;
	uint8_t  ssg_env_shape  = 0;

	bool    lfo_enable = false;
	uint8_t lfo_speed  = 0;

	std::vector<int32_t> mix_buf;  // fmgen Mix 用 int32 ステレオ
};

// ============================================================
// 構築 / 破棄
// ============================================================
FmgenOpnaChip::FmgenOpnaChip(uint32_t output_rate, double master_clock_hz)
	: m_master_clock_hz(master_clock_hz), m_output_rate(output_rate) {
	m_impl = new Impl();
	// fmgen OPNA を 48kHz 直接出力で初期化。rhythm は使わないので path=0。
	m_impl->opna.Init(static_cast<uint>(master_clock_hz + 0.5),
	                  static_cast<uint>(output_rate), false, nullptr);
	m_impl->opna.Reset();
	m_impl->opna.SetVolumeFM(0);
	m_impl->opna.SetVolumePSG(0);
}

FmgenOpnaChip::~FmgenOpnaChip() {
	delete m_impl;
}

// ============================================================
// レジスタ書き込み: 全経路をここに集約 → fmgen SetReg
//   SetReg(addr, data): addr の上位 (0x100~) が port1。
// ============================================================
void FmgenOpnaChip::emit(int port, uint8_t addr, uint8_t val) {
	const uint reg = (port == 0 ? 0u : 0x100u) | static_cast<uint>(addr);
	m_impl->opna.SetReg(reg, static_cast<uint>(val));
}

void FmgenOpnaChip::write_reg(uint8_t port, uint8_t addr, uint8_t val) {
	emit(port == 0 ? 0 : 1, addr, val);
}

// ---- shadow → レジスタ flush ヘルパ ----
void FmgenOpnaChip::flush_dt_mul(int ch, int op) {
	int port, ch_low; ch_to_port(ch, port, ch_low);
	const auto& o = m_impl->op[ch][op];
	emit(port, static_cast<uint8_t>(0x30 + op_addr_offset(op) + ch_low),
	     static_cast<uint8_t>((o.dt << 4) | (o.mul & 0x0F)));
}
void FmgenOpnaChip::flush_tl(int ch, int op) {
	int port, ch_low; ch_to_port(ch, port, ch_low);
	emit(port, static_cast<uint8_t>(0x40 + op_addr_offset(op) + ch_low),
	     m_impl->op[ch][op].tl & 0x7F);
}
void FmgenOpnaChip::flush_ks_ar(int ch, int op) {
	int port, ch_low; ch_to_port(ch, port, ch_low);
	const auto& o = m_impl->op[ch][op];
	emit(port, static_cast<uint8_t>(0x50 + op_addr_offset(op) + ch_low),
	     static_cast<uint8_t>((o.ks << 6) | (o.ar & 0x1F)));
}
void FmgenOpnaChip::flush_d1r(int ch, int op) {
	int port, ch_low; ch_to_port(ch, port, ch_low);
	emit(port, static_cast<uint8_t>(0x60 + op_addr_offset(op) + ch_low),
	     m_impl->op[ch][op].d1r & 0x1F);
}
void FmgenOpnaChip::flush_d2r(int ch, int op) {
	int port, ch_low; ch_to_port(ch, port, ch_low);
	emit(port, static_cast<uint8_t>(0x70 + op_addr_offset(op) + ch_low),
	     m_impl->op[ch][op].d2r & 0x1F);
}
void FmgenOpnaChip::flush_sl_rr(int ch, int op) {
	int port, ch_low; ch_to_port(ch, port, ch_low);
	const auto& o = m_impl->op[ch][op];
	emit(port, static_cast<uint8_t>(0x80 + op_addr_offset(op) + ch_low),
	     static_cast<uint8_t>((o.sl << 4) | (o.rr & 0x0F)));
}
void FmgenOpnaChip::flush_fb_alg(int ch) {
	int port, ch_low; ch_to_port(ch, port, ch_low);
	const auto& c = m_impl->ch[ch];
	emit(port, static_cast<uint8_t>(0xB0 + ch_low),
	     static_cast<uint8_t>((c.fb << 3) | (c.alg & 0x07)));
}
void FmgenOpnaChip::flush_lr_ams_pms(int ch) {
	int port, ch_low; ch_to_port(ch, port, ch_low);
	const auto& c = m_impl->ch[ch];
	emit(port, static_cast<uint8_t>(0xB4 + ch_low), static_cast<uint8_t>(
		(c.pan_l ? 0x80 : 0) | (c.pan_r ? 0x40 : 0) |
		((c.ams & 0x03) << 4) | (c.pms & 0x07)));
}
void FmgenOpnaChip::flush_freq(int ch) {
	int port, ch_low; ch_to_port(ch, port, ch_low);
	const auto& c = m_impl->ch[ch];
	const uint8_t hi = static_cast<uint8_t>((c.block << 3) | ((c.fnum >> 8) & 0x07));
	const uint8_t lo = static_cast<uint8_t>(c.fnum & 0xFF);
	emit(port, static_cast<uint8_t>(0xA4 + ch_low), hi);
	emit(port, static_cast<uint8_t>(0xA0 + ch_low), lo);
	// ch3 (idx 2) は効果音モード運用で A8-AE へ同値ミラー (YmfmOpnaChip と同じ)。
	if (ch == 2) {
		static constexpr uint8_t hi_regs[3] = { 0xAE, 0xAC, 0xAD };
		static constexpr uint8_t lo_regs[3] = { 0xAA, 0xA8, 0xA9 };
		for (int i = 0; i < 3; ++i) {
			emit(0, hi_regs[i], hi);
			emit(0, lo_regs[i], lo);
		}
	}
}

// ============================================================
// FM ch レベル
// ============================================================
void FmgenOpnaChip::fm_key_on(int ch) {
	if (ch < 0 || ch >= k_fm_count) return;
	const uint8_t ch_sel = (ch < 3) ? static_cast<uint8_t>(ch)
	                                : static_cast<uint8_t>(ch - 3 + 4);
	emit(0, 0x28, static_cast<uint8_t>(0xF0 | ch_sel));  // 0x28 は常に port0
	m_impl->ch[ch].block = 0;  // 次の周波数設定で block 再選定
}
void FmgenOpnaChip::fm_key_off(int ch) {
	if (ch < 0 || ch >= k_fm_count) return;
	const uint8_t ch_sel = (ch < 3) ? static_cast<uint8_t>(ch)
	                                : static_cast<uint8_t>(ch - 3 + 4);
	emit(0, 0x28, ch_sel);
}
void FmgenOpnaChip::set_fm_frequency_hz(int ch, double hz) {
	if (ch < 0 || ch >= k_fm_count) return;
	opna_enc::hz_to_fnum_block_hysteresis(hz, m_master_clock_hz,
		m_impl->ch[ch].fnum, m_impl->ch[ch].block, m_impl->ch[ch].block);
	flush_freq(ch);
}
void FmgenOpnaChip::set_fm_algorithm(int ch, uint8_t alg_3bit) {
	if (ch < 0 || ch >= k_fm_count) return;
	m_impl->ch[ch].alg = alg_3bit & 0x07;
	flush_fb_alg(ch);
}
void FmgenOpnaChip::set_fm_feedback(int ch, uint8_t fb_3bit) {
	if (ch < 0 || ch >= k_fm_count) return;
	m_impl->ch[ch].fb = fb_3bit & 0x07;
	flush_fb_alg(ch);
}
void FmgenOpnaChip::set_fm_algorithm_feedback(int ch, uint8_t alg_3bit, uint8_t fb_3bit) {
	if (ch < 0 || ch >= k_fm_count) return;
	m_impl->ch[ch].alg = alg_3bit & 0x07;
	m_impl->ch[ch].fb  = fb_3bit  & 0x07;
	flush_fb_alg(ch);
}
void FmgenOpnaChip::set_fm_lfo(int ch, uint8_t /*waveform*/, uint8_t speed, uint8_t depth) {
	if (ch < 0 || ch >= k_fm_count) return;
	m_impl->ch[ch].pms = depth & 0x07;
	flush_lr_ams_pms(ch);
	const bool new_enable = (speed > 0);
	const uint8_t new_speed = speed & 0x07;
	if (new_enable != m_impl->lfo_enable || new_speed != m_impl->lfo_speed) {
		m_impl->lfo_enable = new_enable;
		m_impl->lfo_speed  = new_speed;
		emit(0, 0x22, static_cast<uint8_t>((new_enable ? 0x08 : 0) | new_speed));
	}
}
void FmgenOpnaChip::set_fm_pan(int ch, Pan pan) {
	if (ch < 0 || ch >= k_fm_count) return;
	const uint8_t p = static_cast<uint8_t>(pan);
	m_impl->ch[ch].pan_l = (p & 0x2) ? 1 : 0;
	m_impl->ch[ch].pan_r = (p & 0x1) ? 1 : 0;
	flush_lr_ams_pms(ch);
}

// ============================================================
// FM op レベル
// ============================================================
void FmgenOpnaChip::set_fm_op_attack_rate(int ch, int op, float seconds) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_impl->op[ch][op].ar = opna_enc::seconds_to_ar(seconds); flush_ks_ar(ch, op);
}
void FmgenOpnaChip::set_fm_op_decay1_rate(int ch, int op, float seconds) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_impl->op[ch][op].d1r = opna_enc::seconds_to_dr(seconds); flush_d1r(ch, op);
}
void FmgenOpnaChip::set_fm_op_sustain_level(int ch, int op, float sl_0_to_1) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_impl->op[ch][op].sl = opna_enc::sl_to_4bit(sl_0_to_1); flush_sl_rr(ch, op);
}
void FmgenOpnaChip::set_fm_op_decay2_rate(int ch, int op, float seconds) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_impl->op[ch][op].d2r = opna_enc::seconds_to_dr(seconds); flush_d2r(ch, op);
}
void FmgenOpnaChip::set_fm_op_release_rate(int ch, int op, float seconds) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_impl->op[ch][op].rr = opna_enc::seconds_to_rr(seconds); flush_sl_rr(ch, op);
}
void FmgenOpnaChip::set_fm_op_total_level_db(int ch, int op, float db) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_impl->op[ch][op].tl = opna_enc::db_to_tl(db); flush_tl(ch, op);
}
void FmgenOpnaChip::set_fm_op_multiplier(int ch, int op, uint8_t mul_4bit) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_impl->op[ch][op].mul = mul_4bit & 0x0F; flush_dt_mul(ch, op);
}
void FmgenOpnaChip::set_fm_op_detune_cents(int ch, int op, float cents) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_impl->op[ch][op].dt = opna_enc::cents_to_dt(cents); flush_dt_mul(ch, op);
}
void FmgenOpnaChip::set_fm_op_key_scaling(int ch, int op, uint8_t ks_2bit) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_impl->op[ch][op].ks = ks_2bit & 0x03; flush_ks_ar(ch, op);
}

// ============================================================
// PSG (SSG)
// ============================================================
void FmgenOpnaChip::psg_reset() {
	for (uint8_t reg = 0x00; reg <= 0x0D; ++reg) emit(0, reg, 0);
	m_impl->ssg_mixer = 0xBF;
	for (int i = 0; i < 3; ++i) { m_impl->ssg_tone_tp[i] = 0; m_impl->ssg_volume[i] = 0; }
	m_impl->ssg_env_period = 0;
	m_impl->ssg_env_shape  = 0;
	emit(0, 0x07, m_impl->ssg_mixer);
}
void FmgenOpnaChip::set_psg_tone_frequency(int ch, double hz) {
	if (ch < 0 || ch >= k_psg_count) return;
	const uint16_t tp = opna_enc::psg_hz_to_tp(hz, m_master_clock_hz);
	m_impl->ssg_tone_tp[ch] = tp;
	emit(0, static_cast<uint8_t>(0x00 + ch * 2),     static_cast<uint8_t>(tp & 0xFF));
	emit(0, static_cast<uint8_t>(0x00 + ch * 2 + 1), static_cast<uint8_t>((tp >> 8) & 0x0F));
}
void FmgenOpnaChip::set_psg_channel_volume(int ch, uint8_t volume_5bit) {
	if (ch < 0 || ch >= k_psg_count) return;
	m_impl->ssg_volume[ch] = volume_5bit & 0x1F;
	emit(0, static_cast<uint8_t>(0x08 + ch), m_impl->ssg_volume[ch]);
}
void FmgenOpnaChip::set_psg_mixer(int ch, bool tone, bool noise) {
	if (ch < 0 || ch >= k_psg_count) return;
	uint8_t m = m_impl->ssg_mixer;
	const uint8_t tone_bit  = static_cast<uint8_t>(1 << ch);
	const uint8_t noise_bit = static_cast<uint8_t>(1 << (ch + 3));
	if (tone)  m &= ~tone_bit;  else m |= tone_bit;
	if (noise) m &= ~noise_bit; else m |= noise_bit;
	m_impl->ssg_mixer = m;
	emit(0, 0x07, m);
}
void FmgenOpnaChip::set_psg_envelope_shape(uint8_t shape_4bit) {
	m_impl->ssg_env_shape = shape_4bit & 0x0F;
	emit(0, 0x0D, m_impl->ssg_env_shape);
}
void FmgenOpnaChip::set_psg_envelope_period(uint16_t period_16bit) {
	m_impl->ssg_env_period = period_16bit;
	emit(0, 0x0B, static_cast<uint8_t>(period_16bit & 0xFF));
	emit(0, 0x0C, static_cast<uint8_t>((period_16bit >> 8) & 0xFF));
}

// ============================================================
// ミックス制御 / レンダー
// ============================================================
void FmgenOpnaChip::set_fm_volume (float linear) { m_fm_volume  = linear; }

void FmgenOpnaChip::set_psg_volume(float linear) {
	m_ssg_volume = linear;
	// fmgen は FM/SSG を Mix 内部で合成するため、SSG の相対音量は SetVolumePSG(dB) で
	// 制御する (FM 全体レベルは render の master gain = m_fm_volume が担う)。
	// fmgen の db は fmvolume = 16384×10^(db/40)、 すなわち振幅 dB の 2 倍スケール。
	// ymfm の psg gain と歩調を合わせるため、 基準 0.55 を SetVolumePSG(0) とし、
	// linear に応じて増減する (0.28 → 約 -12 = 振幅 -6dB = 半分)。
	constexpr double k_ref = 0.55;
	int db = -192;
	if (linear > 0.0) {
		const double d = 40.0 * std::log10(linear / k_ref);
		db = static_cast<int>(d < 0.0 ? d - 0.5 : d + 0.5);
		if (db > 20) db = 20;
	}
	m_impl->opna.SetVolumePSG(db);
}

void FmgenOpnaChip::render(int16_t* dst, uint32_t frames, uint32_t channels) {
	if (frames == 0) return;
	// fmgen Mix はステレオ加算なのでバッファをクリアしてから呼ぶ。
	if (m_impl->mix_buf.size() < static_cast<size_t>(frames) * 2) {
		m_impl->mix_buf.resize(static_cast<size_t>(frames) * 2);
	}
	std::fill_n(m_impl->mix_buf.data(), static_cast<size_t>(frames) * 2, 0);
	m_impl->opna.Mix(m_impl->mix_buf.data(), static_cast<int>(frames));

	// マスタ音量: ymfm 系の set_fm_volume と概ね揃うよう線形係数を掛ける。
	// fmgen の Mix 出力は OPNA フルスケール (±数千~) なので、 ここでは
	// fm_volume を基準にスケール (SSG との比は SetVolumePSG で別途。 暫定で同一係数)。
	const float gain = m_fm_volume;
	auto clip = [](float v) -> int16_t {
		if (v >  32767.0f) return  32767;
		if (v < -32768.0f) return -32768;
		return static_cast<int16_t>(v);
	};
	for (uint32_t i = 0; i < frames; ++i) {
		const int16_t l = clip(static_cast<float>(m_impl->mix_buf[i * 2 + 0]) * gain);
		const int16_t r = clip(static_cast<float>(m_impl->mix_buf[i * 2 + 1]) * gain);
		if (channels >= 2) {
			dst[i * channels + 0] = l;
			dst[i * channels + 1] = r;
			for (uint32_t c = 2; c < channels; ++c) dst[i * channels + c] = 0;
		} else {
			dst[i] = static_cast<int16_t>((static_cast<int32_t>(l) + r) / 2);
		}
	}
}
