//
// FmChannel.cpp: 4 op + 8 アルゴリズム + op1 フィードバック。
//

#include "FmChannel.h"

#include <cmath>

namespace {
// FB 値 → スケール係数 (実機 OPN/OPNA 準拠)。
// 実機: op1 output (14-bit signed) を `>> (10 - FB)` で位相加算。
// OPN 位相は 10-bit fractional (1 周期 = 1024 unit)、output ±8192 に対し
// FB=7 → shift=3 → ±1024 = 「1 周期分シフト」が最大。
// 当実装の正規化 [-1, 1] / 位相 [0, 1) では FB=7 で ±1.0 (= 1 周期) 加算が物理等価。
// 旧テーブル (1/128 .. 1/2) は約 2 倍弱く、PIANO/BELL 系の倍音が出ない原因。
// FB=0 は 0 (フィードバックなし)、FB=1 で 1/64、FB=k で 2^(k-7)。
constexpr float k_fb_scale[8] = {
	0.0f,
	1.0f /  64.0f,  // FB=1
	1.0f /  32.0f,  // FB=2
	1.0f /  16.0f,  // FB=3
	1.0f /   8.0f,  // FB=4
	1.0f /   4.0f,  // FB=5
	1.0f /   2.0f,  // FB=6
	1.0f,           // FB=7
};
}  // namespace

// ---- 構築 ----

FmChannel::FmChannel(uint32_t sample_rate)
	: m_sample_rate(sample_rate) {
	for (int i = 0; i < k_op_count; ++i) {
		m_ops[i] = std::make_unique<FmOperator>(sample_rate);
	}
}

FmChannel::~FmChannel() = default;

// ---- setter ----

void FmChannel::set_frequency(double base_hz) {
	for (int i = 0; i < k_op_count; ++i) {
		m_ops[i]->set_frequency(base_hz);
	}
}

void FmChannel::set_algorithm(uint8_t alg_3bit) {
	m_algorithm.store(alg_3bit & 0x07, std::memory_order_relaxed);
}

void FmChannel::set_feedback(uint8_t fb_3bit) {
	m_feedback.store(fb_3bit & 0x07, std::memory_order_relaxed);
}

void FmChannel::set_lfo(uint8_t waveform, uint8_t speed, int16_t depth) {
	m_lfo_waveform.store(waveform & 0x03, std::memory_order_relaxed);
	m_lfo_speed.store(speed, std::memory_order_relaxed);
	m_lfo_depth.store(depth, std::memory_order_relaxed);
}

void FmChannel::set_pms(uint8_t pms_3bit) {
	m_pms.store(pms_3bit & 0x07, std::memory_order_relaxed);
}

void FmChannel::set_ams(uint8_t ams_2bit) {
	m_ams.store(ams_2bit & 0x03, std::memory_order_relaxed);
}

void FmChannel::update_chip_lfo_value(float lfo_value) {
	m_chip_lfo_value = lfo_value;
}

void FmChannel::key_on() {
	// フィードバック履歴はあえてリセットしない。
	// 実機もキーオン時に履歴は保持されるため、「ノートオフ後に即キーオン」等で
	// 音色の連続性が保たれる。
	for (int i = 0; i < k_op_count; ++i) {
		m_ops[i]->key_on();
	}
}

void FmChannel::key_off() {
	for (int i = 0; i < k_op_count; ++i) {
		m_ops[i]->key_off();
	}
}

FmOperator& FmChannel::op(int index) {
	return *m_ops[index];
}

bool FmChannel::is_idle() const {
	for (int i = 0; i < k_op_count; ++i) {
		if (!m_ops[i]->is_idle()) return false;
	}
	return true;
}

// ---- LFO ----

namespace {
// LFO スピード値 (0..100) を Hz に変換。SOUND: SPEED は MUSIC.COM 仕様で 1..100。
// 経験的なスケール: speed/16 で 0.06〜6.25 Hz。ビブラート/トレモロ用域として妥当。
float lfo_speed_to_hz(uint8_t speed) {
	return static_cast<float>(speed) / 16.0f;
}

// 現在の LFO 値を計算 ([-1, 1])。
// 副作用として m_lfo_phase を進める。
float compute_lfo_value(double& phase, uint8_t waveform, float hz, uint32_t sr) {
	if (sr == 0 || hz <= 0.0f) return 0.0f;
	phase += static_cast<double>(hz) / static_cast<double>(sr);
	if (phase >= 1.0) phase -= std::floor(phase);
	switch (waveform & 0x3) {
	case 0:  // 矩形波
		return (phase < 0.5) ? 1.0f : -1.0f;
	case 1:  // のこぎり波 (上昇)
		return static_cast<float>(2.0 * phase - 1.0);
	case 2:  // 三角波
		return (phase < 0.5)
			? static_cast<float>(-1.0 + 4.0 * phase)
			: static_cast<float>( 3.0 - 4.0 * phase);
	case 3:  // ワンショット (簡易: 鋸と同じ、本来は 1 周期で停止)
		return static_cast<float>(2.0 * phase - 1.0);
	}
	return 0.0f;
}
}  // namespace

// ---- レンダリング ----

float FmChannel::next() {
	const uint8_t alg = m_algorithm.load(std::memory_order_relaxed);
	const uint8_t fb  = m_feedback.load(std::memory_order_relaxed);

	// LFO PMS 計算: depth 0 または speed 0 なら無効。
	const uint8_t lfo_speed = m_lfo_speed.load(std::memory_order_relaxed);
	const int16_t lfo_depth = m_lfo_depth.load(std::memory_order_relaxed);
	float pitch_factor = 1.0f;
	if (lfo_speed > 0 && lfo_depth != 0) {
		const uint8_t lfo_wf  = m_lfo_waveform.load(std::memory_order_relaxed);
		const float   lfo_hz  = lfo_speed_to_hz(lfo_speed);
		const float   lfo_val = compute_lfo_value(m_lfo_phase, lfo_wf, lfo_hz, m_sample_rate);
		// depth -4095..4095 を「半音単位」にスケール。
		// PMD/MUSIC.COM の typical depth=100..500 でビブラート的になるよう
		// depth/4096 → ±1 半音 (depth=4096 で octave/12 半音) に近い感じ。
		const float depth_norm = static_cast<float>(lfo_depth) / 4096.0f;
		const float semitones  = lfo_val * depth_norm * 1.0f;  // 最大 ±1 半音
		pitch_factor = std::exp2(semitones / 12.0f);
	}

	// 実機 OPNA の PMS (Phase Modulation Sensitivity, 0..7) と AMS (Amplitude
	// Modulation Sensitivity, 0..3) をチップ LFO 値に対して適用する。
	// MUSIC.COM ドライバはこれらを 0 のまま使うため、AGM01/COP02 では実質無効。
	const uint8_t pms = m_pms.load(std::memory_order_relaxed);
	const uint8_t ams = m_ams.load(std::memory_order_relaxed);
	if (pms > 0 || ams > 0) {
		// PMS=1..7 → ±cents 表 (実機 OPNA / fmgen 準拠)。F-Number の上位 4-bit に対する
		// shift 量で定義されるが、cents 換算するとおよそ ±3.4 〜 ±100 cents。
		// PMS=0 → 0 cents (無効)
		static constexpr float k_pms_cents[8] = {
			0.0f, 3.4f, 6.7f, 10.0f, 14.0f, 20.0f, 40.0f, 80.0f,
		};
		const float cents = k_pms_cents[pms] * m_chip_lfo_value;
		pitch_factor *= std::exp2(cents / 1200.0f);

		// AMS=1..3 → 最大 dB 減衰量。実機: 1.4 / 5.9 / 11.8 dB。
		static constexpr float k_ams_db[4] = { 0.0f, 1.4f, 5.9f, 11.8f };
		const float ams_db = k_ams_db[ams] * 0.5f * (1.0f - m_chip_lfo_value);
		// AMS は dB を carrier 系の TL に加算するのが実機準拠だが、当実装では
		// 出力に linear gain を掛ける近似で代替 (位相は変わらない、振幅のみ変化)。
		(void)ams_db;  // TODO: carrier op の TL に動的加算する方法を整備したら使用
	}

	// op1 へのフィードバック入力 (前 2 サンプル平均 × FB スケール)。
	const float fb_in = (fb == 0)
		? 0.0f
		: (m_op1_prev + m_op1_prev2) * 0.5f * k_fb_scale[fb];

	const float o1 = m_ops[0]->next(fb_in, pitch_factor);
	// 履歴更新: 次サンプルのフィードバック計算に使う。
	m_op1_prev2 = m_op1_prev;
	m_op1_prev  = o1;

	float o2 = 0.0f;
	float o3 = 0.0f;
	float o4 = 0.0f;
	float carrier_sum   = 0.0f;
	int   carrier_count = 1;

	switch (alg) {
	case 0:
		// 1 → 2 → 3 → 4
		o2 = m_ops[1]->next(o1, pitch_factor);
		o3 = m_ops[2]->next(o2, pitch_factor);
		o4 = m_ops[3]->next(o3, pitch_factor);
		carrier_sum = o4;
		carrier_count = 1;
		break;

	case 1:
		// (1 + 2) → 3 → 4
		o2 = m_ops[1]->next(0.0f, pitch_factor);
		o3 = m_ops[2]->next(o1 + o2, pitch_factor);
		o4 = m_ops[3]->next(o3, pitch_factor);
		carrier_sum = o4;
		carrier_count = 1;
		break;

	case 2:
		// 2 → 3、 (1 + 3) → 4
		o2 = m_ops[1]->next(0.0f, pitch_factor);
		o3 = m_ops[2]->next(o2, pitch_factor);
		o4 = m_ops[3]->next(o1 + o3, pitch_factor);
		carrier_sum = o4;
		carrier_count = 1;
		break;

	case 3:
		// 1 → 2、 (2 + 3) → 4
		o2 = m_ops[1]->next(o1, pitch_factor);
		o3 = m_ops[2]->next(0.0f, pitch_factor);
		o4 = m_ops[3]->next(o2 + o3, pitch_factor);
		carrier_sum = o4;
		carrier_count = 1;
		break;

	case 4:
		// 1 → 2 (carrier)、 3 → 4 (carrier)
		o2 = m_ops[1]->next(o1, pitch_factor);
		o3 = m_ops[2]->next(0.0f, pitch_factor);
		o4 = m_ops[3]->next(o3, pitch_factor);
		carrier_sum = o2 + o4;
		carrier_count = 2;
		break;

	case 5:
		// 1 → {2, 3, 4} (全てキャリア)
		o2 = m_ops[1]->next(o1, pitch_factor);
		o3 = m_ops[2]->next(o1, pitch_factor);
		o4 = m_ops[3]->next(o1, pitch_factor);
		carrier_sum = o2 + o3 + o4;
		carrier_count = 3;
		break;

	case 6:
		// 1 → 2 (carrier)、 3 (carrier)、 4 (carrier)
		o2 = m_ops[1]->next(o1, pitch_factor);
		o3 = m_ops[2]->next(0.0f, pitch_factor);
		o4 = m_ops[3]->next(0.0f, pitch_factor);
		carrier_sum = o2 + o3 + o4;
		carrier_count = 3;
		break;

	case 7:
	default:
		// 1 + 2 + 3 + 4 (全キャリア)
		o2 = m_ops[1]->next(0.0f, pitch_factor);
		o3 = m_ops[2]->next(0.0f, pitch_factor);
		o4 = m_ops[3]->next(0.0f, pitch_factor);
		carrier_sum = o1 + o2 + o3 + o4;
		carrier_count = 4;
		break;
	}

	// 実機 OPN/OPNA はキャリアを単純加算 → DAC 段で飽和。
	// 旧 `/carrier_count` 正規化はアルゴリズム間の音量を揃える効果があったが
	// 倍音 (peak count) も同じ比率で抑えてしまい、ALG=4..7 (multi-carrier) の
	// 厚みが出ない原因だった。OpnaChip 側のマスター音量で総量調整する方針へ。
	(void)carrier_count;
	return carrier_sum;
}
