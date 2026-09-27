//
// PsgChip.cpp: YM2149 / OPNA SSG 互換チップエミュレーション。
//
// レンダリング方針:
//   - トーン位相は浮動小数点位相累算で進める (PSG 内部 16 分周相当を吸収)。
//   - ノイズ・エンベロープのステップは「秒/サンプル」単位の累算カウンタ。
//   - 1 バッファ単位で mutex を 1 回だけ取り、内部で advance_one_sample を回す。
//

#include "PsgChip.h"

#include <algorithm>
#include <cmath>

namespace {
// 4-bit 音量 (0..15) → 振幅 (0..1) の対数テーブル。
// 約 3 dB / step (sqrt(2) 比)。v=15 で 1.0、v=0 で完全無音。
// AY-3-8910 の実機テーブルとは厳密一致しないが OPNA SSG 風の聴感を出す。
constexpr float k_volume_table[16] = {
	0.0000f, 0.0078125f, 0.0110485f, 0.0156250f,
	0.0220971f, 0.0312500f, 0.0441942f, 0.0625000f,
	0.0883883f, 0.1250000f, 0.1767767f, 0.2500000f,
	0.3535534f, 0.5000000f, 0.7071068f, 1.0000000f,
};

// PSG の内部分周比。
constexpr double k_tone_div     = 16.0;
constexpr double k_noise_div    = 16.0;
constexpr double k_envelope_div = 256.0;

// LFSR 初期値 (非ゼロ必須)。
constexpr uint32_t k_lfsr_init  = 0x00001;
constexpr uint32_t k_lfsr_mask  = 0x1FFFF;  // 17-bit
}  // namespace

// ---- 構築・破棄 -----------------------------------------------------

PsgChip::PsgChip(uint32_t sample_rate, double clock_hz)
	: m_sample_rate(sample_rate)
	, m_clock_hz(clock_hz) {
	reset();
}

PsgChip::~PsgChip() = default;

void PsgChip::reset() {
	std::lock_guard<std::mutex> lk(m_mutex);
	for (int ch = 0; ch < k_channel_count; ++ch) {
		m_tones[ch] = ToneState{};
		recompute_tone_phase_inc(ch);
	}
	m_noise = NoiseState{};
	m_noise.lfsr = k_lfsr_init;
	recompute_noise_step_inc();

	m_envelope = EnvelopeState{};
	recompute_envelope_step_inc();
}

// ---- レジスタ風 setter --------------------------------------------

void PsgChip::set_tone_period(int channel, uint16_t tp_12bit) {
	if (channel < 0 || channel >= k_channel_count) return;
	std::lock_guard<std::mutex> lk(m_mutex);
	// PSG 実機では TP=0 と TP=1 は同じ挙動 (最高周波数)。
	const uint16_t tp = (tp_12bit == 0) ? 1 : (tp_12bit & 0x0FFF);
	m_tones[channel].period = tp;
	recompute_tone_phase_inc(channel);
}

void PsgChip::set_noise_period(uint8_t np_5bit) {
	std::lock_guard<std::mutex> lk(m_mutex);
	const uint8_t np = (np_5bit == 0) ? 1 : (np_5bit & 0x1F);
	m_noise.period = np;
	recompute_noise_step_inc();
}

void PsgChip::set_envelope_period(uint16_t ep_16bit) {
	std::lock_guard<std::mutex> lk(m_mutex);
	const uint16_t ep = (ep_16bit == 0) ? 1 : ep_16bit;
	m_envelope.period = ep;
	recompute_envelope_step_inc();
}

void PsgChip::set_envelope_shape(uint8_t shape_4bit) {
	std::lock_guard<std::mutex> lk(m_mutex);
	m_envelope.shape = shape_4bit & 0x0F;
	restart_envelope();
}

void PsgChip::set_mixer(int channel, bool enable_tone, bool enable_noise) {
	if (channel < 0 || channel >= k_channel_count) return;
	std::lock_guard<std::mutex> lk(m_mutex);
	m_tones[channel].enable_tone  = enable_tone;
	m_tones[channel].enable_noise = enable_noise;
}

void PsgChip::set_channel_volume(int channel, uint8_t volume_5bit) {
	if (channel < 0 || channel >= k_channel_count) return;
	std::lock_guard<std::mutex> lk(m_mutex);
	m_tones[channel].volume_raw = volume_5bit & 0x1F;
}

// ---- 音楽向けヘルパ ------------------------------------------------

void PsgChip::set_tone_frequency(int channel, double hz) {
	if (channel < 0 || channel >= k_channel_count) return;
	if (hz <= 0.0) return;
	// f = clock / (16 * TP)  →  TP = clock / (16 * f)
	double tp = m_clock_hz / (k_tone_div * hz);
	if (tp < 1.0)    tp = 1.0;
	if (tp > 4095.0) tp = 4095.0;
	set_tone_period(channel, static_cast<uint16_t>(tp + 0.5));
}

void PsgChip::set_noise_frequency(double hz) {
	if (hz <= 0.0) return;
	double np = m_clock_hz / (k_noise_div * hz);
	if (np < 1.0)  np = 1.0;
	if (np > 31.0) np = 31.0;
	set_noise_period(static_cast<uint8_t>(np + 0.5));
}

void PsgChip::set_envelope_frequency(double cycle_hz) {
	if (cycle_hz <= 0.0) return;
	// 1 周期 = shape の種別に依らず 32 ステップ (triangle) または 16 ステップ (saw)。
	// シンプル化のため「ステップレート / 16 = cycle_hz」と定義 (saw 系基準)。
	// f_step = clock / (256 * EP)、cycle_hz = f_step / 16
	// → EP = clock / (256 * 16 * cycle_hz)
	double ep = m_clock_hz / (k_envelope_div * 16.0 * cycle_hz);
	if (ep < 1.0)     ep = 1.0;
	if (ep > 65535.0) ep = 65535.0;
	set_envelope_period(static_cast<uint16_t>(ep + 0.5));
}

// ---- 内部: 増分再計算 (mutex 取得済み前提) -------------------------

void PsgChip::recompute_tone_phase_inc(int channel) {
	const auto& t = m_tones[channel];
	if (t.period == 0 || m_sample_rate == 0) {
		m_tones[channel].phase_inc = 0.0;
		return;
	}
	// 出力周波数 (Hz)
	const double hz = m_clock_hz / (k_tone_div * static_cast<double>(t.period));
	m_tones[channel].phase_inc = hz / static_cast<double>(m_sample_rate);
}

void PsgChip::recompute_noise_step_inc() {
	if (m_noise.period == 0 || m_sample_rate == 0) {
		m_noise.step_inc = 0.0;
		return;
	}
	const double hz = m_clock_hz / (k_noise_div * static_cast<double>(m_noise.period));
	m_noise.step_inc = hz / static_cast<double>(m_sample_rate);
}

void PsgChip::recompute_envelope_step_inc() {
	if (m_envelope.period == 0 || m_sample_rate == 0) {
		m_envelope.step_inc = 0.0;
		return;
	}
	const double hz = m_clock_hz / (k_envelope_div * static_cast<double>(m_envelope.period));
	m_envelope.step_inc = hz / static_cast<double>(m_sample_rate);
}

// ---- エンベロープ状態機械 -----------------------------------------

void PsgChip::restart_envelope() {
	// shape の bit 2 (ATT) が attacking 方向。
	const bool attacking = (m_envelope.shape & 0x4) != 0;
	m_envelope.attacking  = attacking;
	m_envelope.holding    = false;
	m_envelope.step_index = attacking ? 0 : 15;
	m_envelope.counter    = 0.0;
}

void PsgChip::advance_envelope_one_step() {
	auto& e = m_envelope;
	if (e.holding) return;

	// 1 ステップ進める。
	if (e.attacking) {
		if (e.step_index < 15) {
			++e.step_index;
			return;
		}
	} else {
		if (e.step_index > 0) {
			--e.step_index;
			return;
		}
	}

	// サイクル末端: shape に従って次状態を決定。
	const uint8_t shape = e.shape;
	const bool cont = (shape & 0x8) != 0;  // CONT
	const bool alt  = (shape & 0x2) != 0;  // ALT
	const bool hold = (shape & 0x1) != 0;  // HOLD

	if (!cont) {
		// CONT=0: 常にここで 0 に固定して停止。
		e.step_index = 0;
		e.holding    = true;
		return;
	}

	if (hold) {
		// HOLD=1: 端点で固定。ALT で固定値を反転。
		if (alt) {
			e.step_index = e.attacking ? 0 : 15;
		} else {
			e.step_index = e.attacking ? 15 : 0;
		}
		e.holding = true;
		return;
	}

	// 連続モード。
	if (alt) {
		// ALT=1: 方向反転 (triangle)。
		e.attacking = !e.attacking;
		e.step_index = e.attacking ? 0 : 15;
	} else {
		// ALT=0: 同方向に折り返し (sawtooth)。
		e.step_index = e.attacking ? 0 : 15;
	}
}

// ---- レンダリング本体 ---------------------------------------------

void PsgChip::advance_one_sample_locked() {
	// トーン: 位相 [0..1) を進めて、0..0.5 を high、0.5..1 を low とみなす。
	for (int ch = 0; ch < k_channel_count; ++ch) {
		auto& t = m_tones[ch];
		t.phase += t.phase_inc;
		if (t.phase >= 1.0) {
			t.phase -= std::floor(t.phase);
		}
	}

	// ノイズ: カウンタを進めて 1 を超える毎に LFSR を回す。
	m_noise.counter += m_noise.step_inc;
	while (m_noise.counter >= 1.0) {
		m_noise.counter -= 1.0;
		// 17-bit LFSR、taps at bit0 と bit3 (= polynomial x^17 + x^14 + 1)。
		const uint32_t feedback = (m_noise.lfsr ^ (m_noise.lfsr >> 3)) & 1u;
		m_noise.lfsr = ((m_noise.lfsr >> 1) | (feedback << 16)) & k_lfsr_mask;
		m_noise.bit = (m_noise.lfsr & 1u) != 0;
	}

	// エンベロープ: 同様にカウンタを進めて step を消化。
	if (!m_envelope.holding) {
		m_envelope.counter += m_envelope.step_inc;
		while (m_envelope.counter >= 1.0) {
			m_envelope.counter -= 1.0;
			advance_envelope_one_step();
			if (m_envelope.holding) break;
		}
	}
}

float PsgChip::current_sample_locked() {
	const float env_amp = k_volume_table[m_envelope.step_index];
	float mix = 0.0f;

	for (int ch = 0; ch < k_channel_count; ++ch) {
		const auto& t = m_tones[ch];

		// トーンビット: phase の前半で 1、後半で 0。
		const bool tone_bit = (t.phase < 0.5);

		// PSG ミキサ: 「無効化された」ソースは 1 (常時 high) として AND される。
		const bool tone_high  = t.enable_tone  ? tone_bit       : true;
		const bool noise_high = t.enable_noise ? m_noise.bit    : true;
		const bool out_high   = tone_high && noise_high;

		// 音量: bit 4 が立っていれば HW エンベロープ追従。
		float amp;
		if (t.volume_raw & volume_envelope_bit) {
			amp = env_amp;
		} else {
			amp = k_volume_table[t.volume_raw & 0x0F];
		}

		// 1bit 出力を [-amp, +amp] にマップ。
		mix += out_high ? amp : -amp;
	}

	// 旧 `mix *= 1/3` 正規化は撤去。実機 YM2149 は 3 ch を単純加算 → DAC で飽和。
	// 1 ch だけ強い時のフル振幅を確保することで、SSG ch のソロ打音感を保つ。
	// 全体音量は OpnaChip の psg_volume で 1/3 抑制 (旧 0.12 → 0.04 程度に下げる)。
	if (mix >  1.0f) mix =  1.0f;
	if (mix < -1.0f) mix = -1.0f;
	return mix;
}

void PsgChip::render(int16_t* dst, uint32_t frames, uint32_t channels) {
	std::lock_guard<std::mutex> lk(m_mutex);
	for (uint32_t i = 0; i < frames; ++i) {
		const float s = current_sample_locked();
		const int16_t v = static_cast<int16_t>(s * 32767.0f);
		for (uint32_t c = 0; c < channels; ++c) {
			dst[i * channels + c] = v;
		}
		advance_one_sample_locked();
	}
}

void PsgChip::render_mono_float(float* dst, uint32_t frames) {
	std::lock_guard<std::mutex> lk(m_mutex);
	for (uint32_t i = 0; i < frames; ++i) {
		dst[i] = current_sample_locked();
		advance_one_sample_locked();
	}
}
