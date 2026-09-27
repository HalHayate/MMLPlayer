//
// OpnaChip.cpp: PSG + FM × 6 + パン制御 + ステレオミックス。
//

#include "OpnaChip.h"

#include <cmath>
#include <cstdlib>

namespace {
// SSG (PSG) クロックは OPNA マスターの 1/4。
constexpr double k_psg_clock_divisor = 4.0;

// F-Number 計算で使う定数。FM 内部分周比は 144 (= 6 * 24)。
constexpr double k_fm_fnum_div = 144.0;
constexpr int    k_fm_fnum_shift = 20;  // 位相累算 20-bit

// 実機 OPNA の LFO スピード 0..7 → Hz テーブル。
// YM2608 データシート / fmgen 系資料準拠。
constexpr float k_opna_lfo_hz[8] = {
	3.98f, 5.56f, 6.02f, 6.37f, 6.88f, 9.63f, 48.1f, 72.2f,
};
}  // namespace

OpnaChip::OpnaChip(uint32_t sample_rate, double master_clock_hz)
	: m_sample_rate(sample_rate)
	, m_master_clock_hz(master_clock_hz) {
	// SSG はマスターの 1/4 クロックで駆動 (OPNA 既定の分周)。
	m_psg = std::make_unique<PsgChip>(sample_rate, master_clock_hz / k_psg_clock_divisor);
	for (int i = 0; i < k_fm_count; ++i) {
		m_fm[i] = std::make_unique<FmChannel>(sample_rate);
	}
	// FM 各 ch のデフォルトパン配置 (PMD/FMP/MUSIC.COM 等で慣例的な配置)。
	// AGM01.MML 等のリファレンス録音はこの配置で鳴っているため
	// 何も pan 指定がない場合でもステレオ感が出る。
	//   ch1 = Both (中央)
	//   ch2 = Left
	//   ch3 = Right
	//   ch4..6 = Both (使わない FM ch を初期化)
	m_fm_pan[0].store(static_cast<uint8_t>(Pan::Both),  std::memory_order_relaxed);
	m_fm_pan[1].store(static_cast<uint8_t>(Pan::Left),  std::memory_order_relaxed);
	m_fm_pan[2].store(static_cast<uint8_t>(Pan::Right), std::memory_order_relaxed);
	for (int i = 3; i < k_fm_count; ++i) {
		m_fm_pan[i].store(static_cast<uint8_t>(Pan::Both), std::memory_order_relaxed);
	}
	// 典型的なバッファサイズ (MeAudioStream は 20ms ~960 frames @ 48kHz) より大きめに確保。
	m_psg_temp.resize(4096);
}

OpnaChip::~OpnaChip() = default;

// ---- アクセサ ----

FmChannel& OpnaChip::fm(int index) {
	return *m_fm[index];
}

PsgChip& OpnaChip::psg() {
	return *m_psg;
}

// ---- パン ----

void OpnaChip::set_fm_pan(int index, Pan pan) {
	if (index < 0 || index >= k_fm_count) return;
	m_fm_pan[index].store(static_cast<uint8_t>(pan), std::memory_order_relaxed);
}

// ---- 周波数 ----

void OpnaChip::set_fm_frequency_hz(int index, double hz) {
	if (index < 0 || index >= k_fm_count) return;
	m_fm[index]->set_frequency(hz);
}

// ---- IOpnaChip op-level / ch-level 委譲 ----
// MmlPlayer から共通インターフェイス越しに呼び出される薄いラッパ。
// 実体は FmChannel / FmOperator / PsgChip にそのまま委譲する。

void OpnaChip::fm_key_on(int ch) {
	if (ch < 0 || ch >= k_fm_count) return;
	m_fm[ch]->key_on();
}

void OpnaChip::fm_key_off(int ch) {
	if (ch < 0 || ch >= k_fm_count) return;
	m_fm[ch]->key_off();
}

void OpnaChip::set_fm_algorithm(int ch, uint8_t alg_3bit) {
	if (ch < 0 || ch >= k_fm_count) return;
	m_fm[ch]->set_algorithm(alg_3bit);
}

void OpnaChip::set_fm_feedback(int ch, uint8_t fb_3bit) {
	if (ch < 0 || ch >= k_fm_count) return;
	m_fm[ch]->set_feedback(fb_3bit);
}

void OpnaChip::set_fm_algorithm_feedback(int ch, uint8_t alg_3bit, uint8_t fb_3bit) {
	// Legacy はレジスタ書き込みではなくオペレータモデル直接設定なので
	// 中間値問題は無い。個別設定をそのまま呼ぶ。
	if (ch < 0 || ch >= k_fm_count) return;
	m_fm[ch]->set_algorithm(alg_3bit);
	m_fm[ch]->set_feedback(fb_3bit);
}

void OpnaChip::set_fm_lfo(int ch, uint8_t waveform, uint8_t speed, uint8_t depth) {
	if (ch < 0 || ch >= k_fm_count) return;
	m_fm[ch]->set_lfo(waveform, speed, depth);
}

void OpnaChip::set_fm_op_attack_rate(int ch, int op, float seconds) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_fm[ch]->op(op).set_attack_rate(seconds);
}

void OpnaChip::set_fm_op_decay1_rate(int ch, int op, float seconds) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_fm[ch]->op(op).set_decay1_rate(seconds);
}

void OpnaChip::set_fm_op_sustain_level(int ch, int op, float sl_0_to_1) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_fm[ch]->op(op).set_sustain_level(sl_0_to_1);
}

void OpnaChip::set_fm_op_decay2_rate(int ch, int op, float seconds) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_fm[ch]->op(op).set_decay2_rate(seconds);
}

void OpnaChip::set_fm_op_release_rate(int ch, int op, float seconds) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_fm[ch]->op(op).set_release_rate(seconds);
}

void OpnaChip::set_fm_op_total_level_db(int ch, int op, float db) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_fm[ch]->op(op).set_total_level_db(db);
}

void OpnaChip::set_fm_op_multiplier(int ch, int op, uint8_t mul_4bit) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_fm[ch]->op(op).set_multiplier(mul_4bit);
}

void OpnaChip::set_fm_op_detune_cents(int ch, int op, float cents) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_fm[ch]->op(op).set_detune_cents(cents);
}

void OpnaChip::set_fm_op_key_scaling(int ch, int op, uint8_t ks_2bit) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_fm[ch]->op(op).set_key_scaling(ks_2bit);
}

void OpnaChip::psg_reset() {
	m_psg->reset();
}

void OpnaChip::set_psg_tone_frequency(int ch, double hz) {
	if (ch < 0 || ch >= k_psg_count) return;
	m_psg->set_tone_frequency(ch, hz);
}

void OpnaChip::set_psg_channel_volume(int ch, uint8_t volume_5bit) {
	if (ch < 0 || ch >= k_psg_count) return;
	m_psg->set_channel_volume(ch, volume_5bit);
}

void OpnaChip::set_psg_mixer(int ch, bool tone, bool noise) {
	if (ch < 0 || ch >= k_psg_count) return;
	m_psg->set_mixer(ch, tone, noise);
}

void OpnaChip::set_psg_envelope_shape(uint8_t shape_4bit) {
	m_psg->set_envelope_shape(shape_4bit);
}

void OpnaChip::set_psg_envelope_period(uint16_t period_16bit) {
	m_psg->set_envelope_period(period_16bit);
}

void OpnaChip::set_fm_frequency_fnum(int index, uint16_t f_number, uint8_t block) {
	if (index < 0 || index >= k_fm_count) return;
	const double hz = fnum_block_to_hz(f_number, block, m_master_clock_hz);
	m_fm[index]->set_frequency(hz);
}

OpnaChip::FNumBlock OpnaChip::hz_to_fnum_block(double hz, double master_clock_hz) {
	if (hz <= 0.0 || master_clock_hz <= 0.0) {
		return { 0, 0 };
	}
	// f = F * 2^Block * master / 144 / 2^20
	// → F (block=0 のときの値) = f * 144 * 2^20 / master
	const double constant = k_fm_fnum_div * static_cast<double>(1 << k_fm_fnum_shift) / master_clock_hz;
	double f0 = hz * constant;

	// F-Number を 11-bit 範囲に収めるため block を上げて 1/2 ずつ縮める。
	// 同時に「F が 1024 未満なら block を下げて精度を稼ぐ」も適用 (オクターブ正規化)。
	int block = 0;
	while (f0 >= 2048.0 && block < 7) {
		f0 *= 0.5;
		++block;
	}
	while (f0 < 1024.0 && block > 0) {
		f0 *= 2.0;
		--block;
	}

	if (f0 > 2047.0) f0 = 2047.0;
	if (f0 < 0.0)    f0 = 0.0;

	return {
		static_cast<uint16_t>(f0 + 0.5),
		static_cast<uint8_t>(block),
	};
}

double OpnaChip::fnum_block_to_hz(uint16_t f_number, uint8_t block, double master_clock_hz) {
	const double f_shifted = static_cast<double>(f_number) * static_cast<double>(1ULL << block);
	return f_shifted * master_clock_hz / k_fm_fnum_div / static_cast<double>(1 << k_fm_fnum_shift);
}

OpnaChip::FNumBlock OpnaChip::hz_to_fnum_block(double hz) const {
	return hz_to_fnum_block(hz, m_master_clock_hz);
}

double OpnaChip::fnum_block_to_hz(uint16_t f_number, uint8_t block) const {
	return fnum_block_to_hz(f_number, block, m_master_clock_hz);
}

// ---- ミックス制御 ----

void OpnaChip::set_fm_volume(float linear) {
	m_fm_volume.store(linear, std::memory_order_relaxed);
}

void OpnaChip::set_psg_volume(float linear) {
	m_psg_volume.store(linear, std::memory_order_relaxed);
}

// ---- チップ全体 LFO ----

void OpnaChip::set_chip_lfo_speed_index(int speed_index) {
	m_chip_lfo_speed_index.store(speed_index, std::memory_order_relaxed);
}

int OpnaChip::chip_lfo_speed_index() const {
	return m_chip_lfo_speed_index.load(std::memory_order_relaxed);
}

float OpnaChip::chip_lfo_value() const {
	return m_chip_lfo_value;
}

// ---- レンダリング本体 ----

void OpnaChip::render(int16_t* dst, uint32_t frames, uint32_t channels) {
	if (m_psg_temp.size() < frames) {
		m_psg_temp.resize(frames);
	}

	// PSG を 1 ロックで一括レンダ → temp バッファへ。
	m_psg->render_mono_float(m_psg_temp.data(), frames);

	const float fm_vol  = m_fm_volume.load(std::memory_order_relaxed);
	const float psg_vol = m_psg_volume.load(std::memory_order_relaxed);
	// チャンネル数による事前 attenuation はせず、各 ch のフル出力を活かす。
	// 全 6ch 同時発音時は最終クリップに任せる (実機 OPNA も DAC 段で同様)。
	// これにより 1-2ch のドラム hit が伴奏に埋もれず鋭く立ち上がる。
	constexpr float k_fm_scale = 1.0f;

	// チップ全体 LFO の per-sample 位相増分を計算 (有効時のみ)。
	const int   lfo_idx = m_chip_lfo_speed_index.load(std::memory_order_relaxed);
	const float lfo_inc = (lfo_idx >= 0 && lfo_idx < 8 && m_sample_rate > 0)
		? (k_opna_lfo_hz[lfo_idx] / static_cast<float>(m_sample_rate))
		: 0.0f;

	for (uint32_t i = 0; i < frames; ++i) {
		// チップ LFO 値を更新 (三角波で実機 8 サイクル 32-step に近似)。
		// FmChannel は OpnaChip::chip_lfo_value() で読み取り PMS/AMS を適用する。
		if (lfo_inc > 0.0f) {
			m_chip_lfo_phase += lfo_inc;
			if (m_chip_lfo_phase >= 1.0) m_chip_lfo_phase -= 1.0;
			m_chip_lfo_value = (m_chip_lfo_phase < 0.5)
				? static_cast<float>(-1.0 + 4.0 * m_chip_lfo_phase)
				: static_cast<float>( 3.0 - 4.0 * m_chip_lfo_phase);
		} else {
			m_chip_lfo_value = 0.0f;
		}

		float fm_l = 0.0f;
		float fm_r = 0.0f;
		for (int ch = 0; ch < k_fm_count; ++ch) {
			// チップ LFO 値を ch に渡す (PMS/AMS 適用に使う)。
			m_fm[ch]->update_chip_lfo_value(m_chip_lfo_value);
			const float   s   = m_fm[ch]->next();
			const uint8_t pan = m_fm_pan[ch].load(std::memory_order_relaxed);
			// bit 1 = Left, bit 0 = Right (Pan enum と一致)。
			if (pan & 0x2) fm_l += s;
			if (pan & 0x1) fm_r += s;
		}

		const float psg_mono = m_psg_temp[i];

		float l = fm_l * k_fm_scale * fm_vol + psg_mono * psg_vol;
		float r = fm_r * k_fm_scale * fm_vol + psg_mono * psg_vol;

		// ハードクリップに戻す (soft clip は dynamic range を圧縮しすぎ、
		// AGM01 等で staccato 感を阻害していた)。
		// 実機 OPNA も DAC 段でハードクリップ的挙動。
		if (l >  1.0f) l =  1.0f;
		if (l < -1.0f) l = -1.0f;
		if (r >  1.0f) r =  1.0f;
		if (r < -1.0f) r = -1.0f;

		const int16_t lv = static_cast<int16_t>(l * 32767.0f);
		const int16_t rv = static_cast<int16_t>(r * 32767.0f);

		if (channels >= 2) {
			dst[i * channels + 0] = lv;
			dst[i * channels + 1] = rv;
			// 3 ch 以上指定された場合の余剰は 0 埋め (通常起きない)。
			for (uint32_t c = 2; c < channels; ++c) {
				dst[i * channels + c] = 0;
			}
		} else {
			// モノ要求: L/R を平均。
			dst[i] = static_cast<int16_t>((static_cast<int32_t>(lv) + static_cast<int32_t>(rv)) / 2);
		}
	}
}
