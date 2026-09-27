//
// FmOperator.cpp: OPN 系 FM オペレータ単体の実装。
//

#include "FmOperator.h"

#include <cmath>

#include "OpnTables.h"

namespace {
constexpr float  k_eg_silent_db    = 96.0f;  // 実質無音とみなす上限
constexpr float  k_db_to_log2_div  = 6.0205999132f;  // 20 / log2(10): dB → 2^x 換算用

// 位相の整数表現: 1 周期 = 2^20 (実機 OPN/OPNA 仕様)。
// 上位 2 bit (bits 19,18) が象限、続く 8 bit (bits 17..10) が LUT index。
constexpr uint32_t k_phase_bits      = 20;
constexpr uint32_t k_phase_one_cycle = 1u << k_phase_bits;        // 1048576
constexpr uint32_t k_phase_mask      = k_phase_one_cycle - 1u;    // 0x000FFFFF
constexpr uint32_t k_phase_quadrant_shift = 18;                    // (>> 18) & 3
constexpr uint32_t k_phase_lut_shift      = 10;                    // (>> 10) & 0xFF

// FM 変調入力 → 位相シフト のスケール。
// 実機 OPN: modulator output は 14-bit signed で、carrier phase の bit10..0 域へ加算される。
// 当実装のモジュレータ出力は normalized [-1, 1]、carrier の位相も [0, 1) で扱うため、
// 物理的に等価な変換係数は 1.0 (= 「1 周期の半分まで揺らせる」)。
// 旧 0.5 は経験値だったが、AGM01/COP02 比較で「倍音が薄い (peak count 不足)」原因
// として効いていたため、実機準拠の 1.0 に戻す。
constexpr float  k_fm_modulation_scale = 1.0f;

// dB 減衰 → 線形ゲイン変換。10^(-db/20) を exp2 で高速化。
inline float db_to_linear(float db) {
	if (db <= 0.0f)            return 1.0f;
	if (db >= k_eg_silent_db)  return 0.0f;
	return std::exp2(-db / k_db_to_log2_div);
}

// SL (線形 0..1) を dB 減衰に変換。0 → 96dB、1 → 0dB。
inline float sl_linear_to_db(float sl) {
	if (sl >= 1.0f) return 0.0f;
	if (sl <= 0.0f) return k_eg_silent_db;
	const float db = -20.0f * std::log10(sl);
	return (db > k_eg_silent_db) ? k_eg_silent_db : db;
}
}  // namespace

// ---- 構築 ------------------------------------------------------------

FmOperator::FmOperator(uint32_t sample_rate)
	: m_sample_rate(sample_rate) {
}

// ---- 位相生成 setter -----------------------------------------------

void FmOperator::set_frequency(double base_hz) {
	m_base_hz.store(base_hz, std::memory_order_relaxed);
}

void FmOperator::set_multiplier(uint8_t mul_4bit) {
	m_multiplier.store(mul_4bit & 0x0F, std::memory_order_relaxed);
}

void FmOperator::set_detune_cents(float cents) {
	m_detune_cents.store(cents, std::memory_order_relaxed);
}

void FmOperator::set_key_scaling(uint8_t ks_2bit) {
	m_ks.store(ks_2bit & 0x3, std::memory_order_relaxed);
}

// ---- EG setter ------------------------------------------------------

void FmOperator::set_attack_rate(float seconds) {
	m_ar_sec.store(seconds, std::memory_order_relaxed);
}

void FmOperator::set_decay1_rate(float seconds) {
	m_d1r_sec.store(seconds, std::memory_order_relaxed);
}

void FmOperator::set_sustain_level(float level) {
	if (level < 0.0f) level = 0.0f;
	if (level > 1.0f) level = 1.0f;
	m_sl.store(level, std::memory_order_relaxed);
}

void FmOperator::set_decay2_rate(float seconds) {
	m_d2r_sec.store(seconds, std::memory_order_relaxed);
}

void FmOperator::set_release_rate(float seconds) {
	m_rr_sec.store(seconds, std::memory_order_relaxed);
}

void FmOperator::set_total_level_db(float attenuation_db) {
	m_tl_db.store(attenuation_db, std::memory_order_relaxed);
}

// ---- ノート ---------------------------------------------------------

void FmOperator::key_on() {
	m_key_on_request.store(true, std::memory_order_relaxed);
}

void FmOperator::key_off() {
	m_key_off_request.store(true, std::memory_order_relaxed);
}

bool FmOperator::is_idle() const {
	return m_eg_state == EgState::Idle;
}

// ---- レンダリング ---------------------------------------------------

float FmOperator::next(float mod_input, float pitch_factor) {
	// 1) イベント消化: off → on の順で (両方立っていれば on が勝つ)。
	if (m_key_off_request.exchange(false, std::memory_order_relaxed)) {
		if (m_eg_state != EgState::Idle) {
			m_eg_state = EgState::Release;
		}
	}
	if (m_key_on_request.exchange(false, std::memory_order_relaxed)) {
		// ノートオン: 位相リセット。EG attenuation は保持 (リアタック対応)。
		// 現在の atten が小さければ Attack で素早くピークに達する (実機準拠)。
		m_phase_int = 0;
		m_eg_state = EgState::Attack;
		m_cached_base_hz = -1.0;  // 位相増分の再反映を強制
	}

	// 2) 位相増分キャッシュ更新: パラメータが変わったときだけ pow/乗算。
	const double  base_hz = m_base_hz.load(std::memory_order_relaxed);
	const uint8_t mul     = m_multiplier.load(std::memory_order_relaxed);
	const float   det     = m_detune_cents.load(std::memory_order_relaxed);
	const uint8_t ks      = m_ks.load(std::memory_order_relaxed);
	if (base_hz != m_cached_base_hz || mul != m_cached_mul || det != m_cached_detune ||
	    ks != m_cached_ks) {
		const double mul_factor    = (mul == 0) ? 0.5 : static_cast<double>(mul);
		const double detune_factor = std::pow(2.0, static_cast<double>(det) / 1200.0);
		const double eff_hz        = base_hz * mul_factor * detune_factor;
		// 位相増分: 1 サンプルあたりの (1 周期 = 2^20) 単位での進み量。
		// 整数化により浮動小数の round-to-nearest が消え、実機 OPN と同じ
		// 「位相累積誤差ゼロ + LUT 量子化のみ」の振る舞いになる。
		if (m_sample_rate > 0) {
			const double inc = eff_hz * static_cast<double>(k_phase_one_cycle) /
			                   static_cast<double>(m_sample_rate);
			m_phase_inc_int = static_cast<uint32_t>(inc + 0.5);
		} else {
			m_phase_inc_int = 0;
		}

		// KS (Key Scale) effective rate addition - 実機 OPN/OPNA 準拠。
		//
		// 実機 keycode (5-bit, 0..31) の構成:
		//   keycode = (Block << 2) | N4
		//   N4 = (F-Number bit 10) << 1 | OR(F-Number bit 9, F-Number bit 8 & bit 7 & bit 6)
		// この N4 計算は実機テーブル (4 段階の不規則な値) で、概略「block 内で
		// 1 オクターブを 4 段階に細分化」する。
		// 当実装では eff_hz から逆算して block と F-Number を取り、N4 を厳密計算する。
		//
		// EG rate addition: addition = keycode >> (3 - KS)
		//   KS=0: 常に 0、KS=1: >>2、KS=2: >>1、KS=3: >>0
		// この addition を 2 で割った値だけ EG 時間が短縮される (高音ほど速い)。
		int keycode = 0;
		if (eff_hz > 0.0) {
			// hz から block, F-Number 上位を逆算 (OpnaChip と同じ式の簡略版)。
			// f = F * 2^block * master / 144 / 2^20、master ≈ 7.987 MHz、144 は OPNA 定数。
			// → F0 (block=0 相当) = hz × (144 × 2^20 / master)
			constexpr double k_inv = 144.0 * static_cast<double>(1 << 20) / 7987200.0;
			double f0 = eff_hz * k_inv;
			int    block = 0;
			while (f0 >= 2048.0 && block < 7) { f0 *= 0.5; ++block; }
			while (f0 < 1024.0 && block > 0) { f0 *= 2.0; --block; }
			const int f_num = static_cast<int>(f0 + 0.5);
			// N4 計算 (実機表に従う):
			//   F11 = bit 10、bit f_num & 0x400
			//   N4 上位 = F11
			//   N4 下位 = (F10 & F9 & F8) | (!F11 & (F10 | F9 | F8))
			//     ※ここでは fmgen 風に簡略化: F10 (bit 9) で 1/0 に分岐
			const int f11 = (f_num >> 10) & 1;
			const int f10 = (f_num >>  9) & 1;
			const int f9  = (f_num >>  8) & 1;
			const int f8  = (f_num >>  7) & 1;
			int n4_low;
			if (f11) {
				n4_low = (f10 | f9 | f8);  // F11=1 のとき: F10..F8 のいずれか
			} else {
				n4_low = (f10 & f9 & f8);  // F11=0 のとき: F10..F8 全て
			}
			const int n4 = (f11 << 1) | n4_low;
			keycode = (block << 2) | n4;  // 0..31
		}
		int addition = 0;
		if (ks > 0) {
			addition = keycode >> (3 - ks);
		}
		// 実機 EG rate の上限 (63) を超える組み合わせはあるが、当実装では時間 → 秒換算
		// 経由のため、addition >12 で破綻 (1/4096 倍速度) する。8 で頭打ち。
		if (addition > 8) addition = 8;
		m_cached_ks_factor = std::exp2(static_cast<float>(addition) / 2.0f);

		m_cached_base_hz = base_hz;
		m_cached_mul     = mul;
		m_cached_detune  = det;
		m_cached_ks      = ks;
	}

	// 3) EG 更新 (dB ドメイン)。
	// Attack: 指数減衰で atten が 96dB → 0dB へ近づく (現在値に比例した速度)。
	// Decay1/2/Release: 線形 dB 増加 (= 線形領域では指数減衰)。
	const float dt = (m_sample_rate > 0) ? (1.0f / static_cast<float>(m_sample_rate)) : 0.0f;
	// KS 効果: 時間を ks_factor で割って高音ほど EG を速くする (factor>=1.0)。
	const float ks_factor = m_cached_ks_factor;
	switch (m_eg_state) {
	case EgState::Idle:
		m_eg_atten_db = k_eg_silent_db;
		break;

	case EgState::Attack: {
		const float a = m_ar_sec.load(std::memory_order_relaxed) / ks_factor;
		if (a <= 0.0f) {
			m_eg_atten_db = 0.0f;
			m_eg_state = EgState::Decay1;
		} else {
			// OPN 実機 Attack カーブ: atten -= ((atten >> 4) + 1) × step
			// 「atten が大きい域では加速 (~指数)、小さい域では +1 が支配的で線形収束」
			// → 立ち上がり初期は鋭く、ピーク近傍で滑らかに収束する独特の感触。
			// 旧 atten *= exp(-dt × 4 / a) は「全域で同じ比率」=「鈍い立ち上がり」だった。
			//
			// 校正: rate=24 で 30ms、rate=8 で 7.5s 等の実機タイミング表に合わせ、
			// step を `96 dB / a × dt × accel` で計算。
			// accel=0.5 で 96dB→1dB が約 a 秒に到達 (atten/16 + 1 の平均値 ~3.5 を考慮)。
			const float step = (96.0f / a) * dt * 0.5f;
			m_eg_atten_db -= (m_eg_atten_db / 16.0f + 1.0f) * step;
			if (m_eg_atten_db <= 0.0f) {
				m_eg_atten_db = 0.0f;
				m_eg_state = EgState::Decay1;
			}
		}
		break;
	}

	case EgState::Decay1: {
		const float d1 = m_d1r_sec.load(std::memory_order_relaxed) / ks_factor;
		const float sl_db = sl_linear_to_db(m_sl.load(std::memory_order_relaxed));
		if (d1 <= 0.0f) {
			m_eg_atten_db = sl_db;
			m_eg_state = EgState::Decay2;
		} else {
			const float rate_db_per_sec = k_eg_silent_db / d1;
			m_eg_atten_db += rate_db_per_sec * dt;
			if (m_eg_atten_db >= sl_db) {
				m_eg_atten_db = sl_db;
				m_eg_state = EgState::Decay2;
			}
		}
		break;
	}

	case EgState::Decay2: {
		const float d2 = m_d2r_sec.load(std::memory_order_relaxed) / ks_factor;
		if (d2 <= 0.0f) {
			m_eg_atten_db = k_eg_silent_db;
			m_eg_state = EgState::Idle;
		} else {
			const float rate_db_per_sec = k_eg_silent_db / d2;
			m_eg_atten_db += rate_db_per_sec * dt;
			if (m_eg_atten_db >= k_eg_silent_db) {
				m_eg_atten_db = k_eg_silent_db;
				m_eg_state = EgState::Idle;
			}
		}
		break;
	}

	case EgState::Release: {
		const float r = m_rr_sec.load(std::memory_order_relaxed) / ks_factor;
		if (r <= 0.0f) {
			m_eg_atten_db = k_eg_silent_db;
			m_eg_state = EgState::Idle;
		} else {
			const float rate_db_per_sec = k_eg_silent_db / r;
			m_eg_atten_db += rate_db_per_sec * dt;
			if (m_eg_atten_db >= k_eg_silent_db) {
				m_eg_atten_db = k_eg_silent_db;
				m_eg_state = EgState::Idle;
			}
		}
		break;
	}
	}

	// 4) 波形計算 (log-domain): 変調入力で位相を揺らし、log_sin LUT を引く。
	// 実機 OPN は sin 値そのものではなく |sin| の log (dB) を保存していて、
	// EG/TL と log 領域で加算してから exp2 で linear 化する。
	// この log_sin LUT の量子化が OPN 特有の音色感の源。
	//
	// 位相は 20-bit 整数。mod_input ([-1, 1]) は 2^20 単位の int32 シフトに変換。
	// uint32 加算の自然な wrap-around (+ mask) で mod 1 を実現する。
	const float scaled_mod_f = mod_input * k_fm_modulation_scale *
	                           static_cast<float>(k_phase_one_cycle);
	const int32_t mod_phase  = static_cast<int32_t>(scaled_mod_f);
	const uint32_t full_phase = (m_phase_int + static_cast<uint32_t>(mod_phase)) & k_phase_mask;

	// 4 象限を上位 2 bit で判定。1/4 周期 LUT の対称性で全周期をカバーする:
	//   quadrant 0 (00): sin > 0、上昇 (LUT 順)
	//   quadrant 1 (01): sin > 0、下降 (LUT 逆順)
	//   quadrant 2 (10): sin < 0、下降 (LUT 順)
	//   quadrant 3 (11): sin < 0、上昇 (LUT 逆順)
	const uint32_t quadrant      = (full_phase >> k_phase_quadrant_shift) & 0x3u;
	const bool     sign_negative = (quadrant & 0x2u) != 0;
	const bool     descending    = (quadrant & 0x1u) != 0;
	const uint32_t base_idx      = (full_phase >> k_phase_lut_shift) & 0xFFu;
	const uint32_t idx           = descending ? (0xFFu - base_idx) : base_idx;

	const float* log_sin_db = opn::get_log_sin_table_db();

	// 5) 位相を進める。pitch_factor は LFO PMS 等の per-sample 倍率 (~1.0)。
	// 整数増分に float 倍率を掛けるため一旦 float で計算、整数キャストで戻す。
	const uint32_t inc = (pitch_factor == 1.0f)
		? m_phase_inc_int
		: static_cast<uint32_t>(static_cast<float>(m_phase_inc_int) * pitch_factor);
	m_phase_int = (m_phase_int + inc) & k_phase_mask;

	// 6) 全 attenuation (dB) を log 領域で合算 → exp2 で 1 回だけ linear 化。
	// total_db ≥ 96 のときは無音 (LUT も EG も silent 値で頭打ち)。
	const float tl_db = m_tl_db.load(std::memory_order_relaxed);
	const float total_db = tl_db + m_eg_atten_db + log_sin_db[idx];
	if (total_db >= k_eg_silent_db) {
		return 0.0f;
	}
	const float magnitude = std::exp2(-total_db / k_db_to_log2_div);
	return sign_negative ? -magnitude : magnitude;
}
