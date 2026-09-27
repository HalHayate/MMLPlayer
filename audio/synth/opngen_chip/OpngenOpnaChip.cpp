// OpngenOpnaChip: np21w 内蔵 opngen (FM) + psggen (SSG) を OPNA バックエンドにする
// ラッパ実装。NP2 リファレンス録音と同一エンジン。
#include "audio/synth/opngen_chip/OpngenOpnaChip.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "audio/synth/opna/OpnaRegEncode.h"

extern "C" {
#include "audio/synth/opngen/opngen.h"
#include "audio/synth/opngen/psggen.h"
}

// np21w の sound_sync (ストリーミング同期) スタブ。チャンク単位同期レンダのため不要。
extern "C" void sound_sync(void) {}

namespace {

inline void ch_to_port(int ch, int& port, int& ch_low) {
	if (ch < 3) { port = 0; ch_low = ch; }
	else         { port = 1; ch_low = ch - 3; }
}
inline uint8_t op_addr_offset(int op) {
	static constexpr uint8_t k_mml_op_to_slot[4] = { 0, 2, 1, 3 };
	return static_cast<uint8_t>(k_mml_op_to_slot[op] << 2);
}

// opngen/psggen の内部音量基準。これを固定し、 FM/SSG ゲインは render 側で適用する。
constexpr unsigned k_opngen_vol = 64;
constexpr unsigned k_psggen_vol = 64;

}  // namespace

struct OpngenOpnaChip::Impl {
	_OPNGEN opn;
	_PSGGEN psg;

	struct Op { uint8_t dt=0,mul=0,tl=0,ks=0,ar=0,d1r=0,dt2=0,d2r=0,sl=0,rr=0; } op[6][4];
	struct Ch {
		uint16_t fnum=0; uint8_t block=0, alg=0, fb=0, pan_l=1, pan_r=1, ams=0, pms=0;
	} ch[6];

	uint8_t  ssg_mixer = 0xBF;
	uint16_t ssg_tone_tp[3] = {0,0,0};
	uint8_t  ssg_volume[3]  = {0,0,0};
	uint16_t ssg_env_period = 0;
	uint8_t  ssg_env_shape  = 0;
	bool     lfo_enable = false;
	uint8_t  lfo_speed  = 0;

	std::vector<int32_t> fm_buf;
	std::vector<int32_t> psg_buf;
};

OpngenOpnaChip::OpngenOpnaChip(uint32_t output_rate, double master_clock_hz)
	: m_master_clock_hz(master_clock_hz), m_output_rate(output_rate) {
	m_impl = new Impl();
	opngen_initialize(static_cast<UINT>(output_rate));
	psggen_initialize(static_cast<UINT>(output_rate));
	opngen_setvol(k_opngen_vol);
	psggen_setvol(k_psggen_vol);
	opngen_reset(&m_impl->opn);
	psggen_reset(&m_impl->psg);
	// 6ch・ステレオ有効化 (OPN_STEREO | 全 ch ビット)。
	// OPN_STEREO (0x80000000) は enum 値が int 扱いのため、UINT32 へ明示変換する
	// (暗黙変換だと C4245 signed/unsigned 警告)。
	opngen_setcfg(&m_impl->opn, OPNCH_MAX, static_cast<UINT32>(OPN_STEREO | 0x3F));
}

OpngenOpnaChip::~OpngenOpnaChip() { delete m_impl; }

// ============================================================
// レジスタ書き込み: opngen (FM) / psggen (SSG) へ振り分け (np21w opna.c 準拠)
// ============================================================
void OpngenOpnaChip::emit(int port, uint8_t addr, uint8_t val) {
	if (addr == 0x28) {                 // FM キーオン (常に port0)
		const int cch = val & 0x0f;
		if (cch < 3)                opngen_keyon(&m_impl->opn, cch, val);
		else if (cch >= 4 && cch < 7) opngen_keyon(&m_impl->opn, cch - 1, val);
	} else if (addr == 0x27) {          // ch3 効果音モード
		opngen_setextch(&m_impl->opn, 2, val & 0xc0);
	} else if (addr < 0x10) {           // SSG
		psggen_setreg(&m_impl->psg, addr, val);
	} else if (addr >= 0x30) {          // FM op/ch レジスタ
		opngen_setreg(&m_impl->opn, port == 0 ? 0 : 3, addr, val);
	}
	// 0x10-0x26 / 0x29-0x2f (rhythm/timer/LFO) は opngen では不要。
}

void OpngenOpnaChip::write_reg(uint8_t port, uint8_t addr, uint8_t val) {
	emit(port == 0 ? 0 : 1, addr, val);
}

// ---- shadow → レジスタ flush (FmgenOpnaChip と同一ロジック) ----
void OpngenOpnaChip::flush_dt_mul(int ch, int op) {
	int p, c; ch_to_port(ch, p, c);
	const auto& o = m_impl->op[ch][op];
	emit(p, static_cast<uint8_t>(0x30 + op_addr_offset(op) + c),
	     static_cast<uint8_t>((o.dt << 4) | (o.mul & 0x0F)));
}
void OpngenOpnaChip::flush_tl(int ch, int op) {
	int p, c; ch_to_port(ch, p, c);
	emit(p, static_cast<uint8_t>(0x40 + op_addr_offset(op) + c), m_impl->op[ch][op].tl & 0x7F);
}
void OpngenOpnaChip::flush_ks_ar(int ch, int op) {
	int p, c; ch_to_port(ch, p, c);
	const auto& o = m_impl->op[ch][op];
	emit(p, static_cast<uint8_t>(0x50 + op_addr_offset(op) + c),
	     static_cast<uint8_t>((o.ks << 6) | (o.ar & 0x1F)));
}
void OpngenOpnaChip::flush_d1r(int ch, int op) {
	int p, c; ch_to_port(ch, p, c);
	emit(p, static_cast<uint8_t>(0x60 + op_addr_offset(op) + c), m_impl->op[ch][op].d1r & 0x1F);
}
void OpngenOpnaChip::flush_d2r(int ch, int op) {
	int p, c; ch_to_port(ch, p, c);
	emit(p, static_cast<uint8_t>(0x70 + op_addr_offset(op) + c), m_impl->op[ch][op].d2r & 0x1F);
}
void OpngenOpnaChip::flush_sl_rr(int ch, int op) {
	int p, c; ch_to_port(ch, p, c);
	const auto& o = m_impl->op[ch][op];
	emit(p, static_cast<uint8_t>(0x80 + op_addr_offset(op) + c),
	     static_cast<uint8_t>((o.sl << 4) | (o.rr & 0x0F)));
}
void OpngenOpnaChip::flush_fb_alg(int ch) {
	int p, c; ch_to_port(ch, p, c);
	const auto& cc = m_impl->ch[ch];
	emit(p, static_cast<uint8_t>(0xB0 + c), static_cast<uint8_t>((cc.fb << 3) | (cc.alg & 0x07)));
}
void OpngenOpnaChip::flush_lr_ams_pms(int ch) {
	int p, c; ch_to_port(ch, p, c);
	const auto& cc = m_impl->ch[ch];
	emit(p, static_cast<uint8_t>(0xB4 + c), static_cast<uint8_t>(
		(cc.pan_l ? 0x80 : 0) | (cc.pan_r ? 0x40 : 0) | ((cc.ams & 0x03) << 4) | (cc.pms & 0x07)));
}
void OpngenOpnaChip::flush_freq(int ch) {
	int p, c; ch_to_port(ch, p, c);
	const auto& cc = m_impl->ch[ch];
	const uint8_t hi = static_cast<uint8_t>((cc.block << 3) | ((cc.fnum >> 8) & 0x07));
	const uint8_t lo = static_cast<uint8_t>(cc.fnum & 0xFF);
	emit(p, static_cast<uint8_t>(0xA4 + c), hi);
	emit(p, static_cast<uint8_t>(0xA0 + c), lo);
	if (ch == 2) {  // ch3 効果音モード: A8-AE へ同値ミラー
		static constexpr uint8_t hi_regs[3] = { 0xAE, 0xAC, 0xAD };
		static constexpr uint8_t lo_regs[3] = { 0xAA, 0xA8, 0xA9 };
		for (int i = 0; i < 3; ++i) { emit(0, hi_regs[i], hi); emit(0, lo_regs[i], lo); }
	}
}

// ============================================================
// FM ch レベル
// ============================================================
void OpngenOpnaChip::fm_key_on(int ch) {
	if (ch < 0 || ch >= k_fm_count) return;
	const uint8_t ch_sel = (ch < 3) ? static_cast<uint8_t>(ch) : static_cast<uint8_t>(ch - 3 + 4);
	emit(0, 0x28, static_cast<uint8_t>(0xF0 | ch_sel));
	m_impl->ch[ch].block = 0;
}
void OpngenOpnaChip::fm_key_off(int ch) {
	if (ch < 0 || ch >= k_fm_count) return;
	const uint8_t ch_sel = (ch < 3) ? static_cast<uint8_t>(ch) : static_cast<uint8_t>(ch - 3 + 4);
	emit(0, 0x28, ch_sel);
}
void OpngenOpnaChip::set_fm_frequency_hz(int ch, double hz) {
	if (ch < 0 || ch >= k_fm_count) return;
	opna_enc::hz_to_fnum_block_hysteresis(hz, m_master_clock_hz,
		m_impl->ch[ch].fnum, m_impl->ch[ch].block, m_impl->ch[ch].block);
	flush_freq(ch);
}
void OpngenOpnaChip::set_fm_algorithm(int ch, uint8_t alg_3bit) {
	if (ch < 0 || ch >= k_fm_count) return;
	m_impl->ch[ch].alg = alg_3bit & 0x07; flush_fb_alg(ch);
}
void OpngenOpnaChip::set_fm_feedback(int ch, uint8_t fb_3bit) {
	if (ch < 0 || ch >= k_fm_count) return;
	m_impl->ch[ch].fb = fb_3bit & 0x07; flush_fb_alg(ch);
}
void OpngenOpnaChip::set_fm_algorithm_feedback(int ch, uint8_t alg_3bit, uint8_t fb_3bit) {
	if (ch < 0 || ch >= k_fm_count) return;
	m_impl->ch[ch].alg = alg_3bit & 0x07; m_impl->ch[ch].fb = fb_3bit & 0x07; flush_fb_alg(ch);
}
void OpngenOpnaChip::set_fm_lfo(int ch, uint8_t, uint8_t speed, uint8_t depth) {
	if (ch < 0 || ch >= k_fm_count) return;
	m_impl->ch[ch].pms = depth & 0x07; flush_lr_ams_pms(ch);
	const bool en = (speed > 0); const uint8_t sp = speed & 0x07;
	if (en != m_impl->lfo_enable || sp != m_impl->lfo_speed) {
		m_impl->lfo_enable = en; m_impl->lfo_speed = sp;
		emit(0, 0x22, static_cast<uint8_t>((en ? 0x08 : 0) | sp));
	}
}
void OpngenOpnaChip::set_fm_pan(int ch, Pan pan) {
	if (ch < 0 || ch >= k_fm_count) return;
	const uint8_t p = static_cast<uint8_t>(pan);
	m_impl->ch[ch].pan_l = (p & 0x2) ? 1 : 0;
	m_impl->ch[ch].pan_r = (p & 0x1) ? 1 : 0;
	flush_lr_ams_pms(ch);
}

// ============================================================
// FM op レベル
// ============================================================
void OpngenOpnaChip::set_fm_op_attack_rate(int ch, int op, float s) {
	if (ch<0||ch>=k_fm_count||op<0||op>=k_fm_op) return;
	m_impl->op[ch][op].ar = opna_enc::seconds_to_ar(s); flush_ks_ar(ch, op);
}
void OpngenOpnaChip::set_fm_op_decay1_rate(int ch, int op, float s) {
	if (ch<0||ch>=k_fm_count||op<0||op>=k_fm_op) return;
	m_impl->op[ch][op].d1r = opna_enc::seconds_to_dr(s); flush_d1r(ch, op);
}
void OpngenOpnaChip::set_fm_op_sustain_level(int ch, int op, float sl) {
	if (ch<0||ch>=k_fm_count||op<0||op>=k_fm_op) return;
	m_impl->op[ch][op].sl = opna_enc::sl_to_4bit(sl); flush_sl_rr(ch, op);
}
void OpngenOpnaChip::set_fm_op_decay2_rate(int ch, int op, float s) {
	if (ch<0||ch>=k_fm_count||op<0||op>=k_fm_op) return;
	m_impl->op[ch][op].d2r = opna_enc::seconds_to_dr(s); flush_d2r(ch, op);
}
void OpngenOpnaChip::set_fm_op_release_rate(int ch, int op, float s) {
	if (ch<0||ch>=k_fm_count||op<0||op>=k_fm_op) return;
	m_impl->op[ch][op].rr = opna_enc::seconds_to_rr(s); flush_sl_rr(ch, op);
}
void OpngenOpnaChip::set_fm_op_total_level_db(int ch, int op, float db) {
	if (ch<0||ch>=k_fm_count||op<0||op>=k_fm_op) return;
	m_impl->op[ch][op].tl = opna_enc::db_to_tl(db); flush_tl(ch, op);
}
void OpngenOpnaChip::set_fm_op_multiplier(int ch, int op, uint8_t mul) {
	if (ch<0||ch>=k_fm_count||op<0||op>=k_fm_op) return;
	m_impl->op[ch][op].mul = mul & 0x0F; flush_dt_mul(ch, op);
}
void OpngenOpnaChip::set_fm_op_detune_cents(int ch, int op, float cents) {
	if (ch<0||ch>=k_fm_count||op<0||op>=k_fm_op) return;
	m_impl->op[ch][op].dt = opna_enc::cents_to_dt(cents); flush_dt_mul(ch, op);
}
void OpngenOpnaChip::set_fm_op_key_scaling(int ch, int op, uint8_t ks) {
	if (ch<0||ch>=k_fm_count||op<0||op>=k_fm_op) return;
	m_impl->op[ch][op].ks = ks & 0x03; flush_ks_ar(ch, op);
}

// ============================================================
// PSG (SSG)
// ============================================================
void OpngenOpnaChip::psg_reset() {
	for (uint8_t reg = 0x00; reg <= 0x0D; ++reg) emit(0, reg, 0);
	m_impl->ssg_mixer = 0xBF;
	for (int i = 0; i < 3; ++i) { m_impl->ssg_tone_tp[i] = 0; m_impl->ssg_volume[i] = 0; }
	m_impl->ssg_env_period = 0; m_impl->ssg_env_shape = 0;
	emit(0, 0x07, m_impl->ssg_mixer);
}
void OpngenOpnaChip::set_psg_tone_frequency(int ch, double hz) {
	if (ch < 0 || ch >= k_psg_count) return;
	const uint16_t tp = opna_enc::psg_hz_to_tp(hz, m_master_clock_hz);
	m_impl->ssg_tone_tp[ch] = tp;
	emit(0, static_cast<uint8_t>(0x00 + ch * 2),     static_cast<uint8_t>(tp & 0xFF));
	emit(0, static_cast<uint8_t>(0x00 + ch * 2 + 1), static_cast<uint8_t>((tp >> 8) & 0x0F));
}
void OpngenOpnaChip::set_psg_channel_volume(int ch, uint8_t v) {
	if (ch < 0 || ch >= k_psg_count) return;
	m_impl->ssg_volume[ch] = v & 0x1F;
	emit(0, static_cast<uint8_t>(0x08 + ch), m_impl->ssg_volume[ch]);
}
void OpngenOpnaChip::set_psg_mixer(int ch, bool tone, bool noise) {
	if (ch < 0 || ch >= k_psg_count) return;
	uint8_t m = m_impl->ssg_mixer;
	const uint8_t tb = static_cast<uint8_t>(1 << ch), nb = static_cast<uint8_t>(1 << (ch + 3));
	if (tone)  m &= ~tb; else m |= tb;
	if (noise) m &= ~nb; else m |= nb;
	m_impl->ssg_mixer = m;
	emit(0, 0x07, m);
}
void OpngenOpnaChip::set_psg_envelope_shape(uint8_t shape_4bit) {
	m_impl->ssg_env_shape = shape_4bit & 0x0F;
	emit(0, 0x0D, m_impl->ssg_env_shape);
}
void OpngenOpnaChip::set_psg_envelope_period(uint16_t period_16bit) {
	m_impl->ssg_env_period = period_16bit;
	emit(0, 0x0B, static_cast<uint8_t>(period_16bit & 0xFF));
	emit(0, 0x0C, static_cast<uint8_t>((period_16bit >> 8) & 0xFF));
}

// ============================================================
// ミックス制御 / レンダー
// ============================================================
void OpngenOpnaChip::set_fm_volume (float linear) { m_fm_volume  = linear; }
void OpngenOpnaChip::set_psg_volume(float linear) { m_ssg_volume = linear; }

void OpngenOpnaChip::render(int16_t* dst, uint32_t frames, uint32_t channels) {
	if (frames == 0) return;
	const size_t n2 = static_cast<size_t>(frames) * 2;
	if (m_impl->fm_buf.size() < n2)  m_impl->fm_buf.resize(n2);
	if (m_impl->psg_buf.size() < n2) m_impl->psg_buf.resize(n2);
	// getpcm は加算なのでクリアしてから (FM/SSG を別バッファで取得し独立ゲイン適用)。
	std::fill_n(m_impl->fm_buf.data(),  n2, 0);
	std::fill_n(m_impl->psg_buf.data(), n2, 0);
	opngen_getpcm(&m_impl->opn, m_impl->fm_buf.data(),  static_cast<UINT>(frames));
	psggen_getpcm(&m_impl->psg, m_impl->psg_buf.data(), static_cast<UINT>(frames));

	const float fmg = m_fm_volume, psgg = m_ssg_volume;
	auto clip = [](float v) -> int16_t {
		if (v >  32767.0f) return  32767;
		if (v < -32768.0f) return -32768;
		return static_cast<int16_t>(v);
	};
	for (uint32_t i = 0; i < frames; ++i) {
		const int16_t l = clip(static_cast<float>(m_impl->fm_buf[i*2+0]) * fmg
		                     + static_cast<float>(m_impl->psg_buf[i*2+0]) * psgg);
		const int16_t r = clip(static_cast<float>(m_impl->fm_buf[i*2+1]) * fmg
		                     + static_cast<float>(m_impl->psg_buf[i*2+1]) * psgg);
		if (channels >= 2) {
			dst[i*channels+0] = l; dst[i*channels+1] = r;
			for (uint32_t c = 2; c < channels; ++c) dst[i*channels+c] = 0;
		} else {
			dst[i] = static_cast<int16_t>((static_cast<int32_t>(l) + r) / 2);
		}
	}
}
