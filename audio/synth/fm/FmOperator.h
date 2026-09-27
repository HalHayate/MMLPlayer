#pragma once
//
// FmOperator: OPN/OPNA 系 FM オペレータ単体。
//
// 1 オペレータ = 「位相生成 (PG)」+「サイン関数」+「エンベロープ (EG)」。
// FM 合成自体は複数オペレータを Phase 5 で束ねた FmChannel が行う。
// 本クラスは
//   - キャリア用途 (mod_input = 0 で純粋なサイン+EG)
//   - モジュレータ用途 (出力を別 op の next(mod_input) に渡す)
// のいずれにも使える。
//
// EG の流儀 (OPN 系):
//   Idle --key_on--> Attack --(到達)--> Decay1 --(SL 到達)--> Decay2
//                                                     `--key_off--> Release
//   Sustain は「状態」ではなく「Decay1 → Decay2 の遷移閾値」レベル。
//   これにより「強い初期減衰の後、ゆっくり減衰し続ける」音が自然に出る。
//
// Phase 4 の単純化:
//   - Detune は OPNA の DT3 ビット表ではなく cents 指定 (Phase 5/6 で OPNA 風に拡張)。
//   - EG レートは 5/10/Rate ではなく秒指定 (キースケーリングは未実装)。
//   - sin は std::sin。LUT 化は性能要件が出てから検討。
//

#include <atomic>
#include <cstdint>

class FmOperator {
public:
	enum class EgState : uint8_t {
		Idle,
		Attack,
		Decay1,
		Decay2,
		Release,
	};

	explicit FmOperator(uint32_t sample_rate);
	~FmOperator() = default;

	FmOperator(const FmOperator&)            = delete;
	FmOperator& operator=(const FmOperator&) = delete;

	// ---- 位相生成 (任意スレッド) ----
	void set_frequency(double base_hz);          // チャンネルの基準周波数
	void set_multiplier(uint8_t mul_4bit);       // 0=×0.5、1..15=×N (OPN 互換)
	void set_detune_cents(float cents);          // ±100 セント程度を想定
	void set_key_scaling(uint8_t ks_2bit);       // 0..3。高音ほど EG 速くなる係数

	// ---- EG (任意スレッド) ----
	void set_attack_rate(float seconds);         // 0 → 1.0 までの時間
	void set_decay1_rate(float seconds);         // 1.0 → SL までの時間
	void set_sustain_level(float level_0_to_1);  // SL: D1 と D2 の境
	void set_decay2_rate(float seconds);         // SL → 0 までの時間
	void set_release_rate(float seconds);        // 1.0 → 0 までの絶対速度
	void set_total_level_db(float attenuation_db);  // TL: 0=full, 大きいほど減衰

	// ---- ノート (任意スレッド) ----
	void key_on();
	void key_off();

	// ---- レンダースレッド専用 ----
	// mod_input は前段オペレータからの位相変調量 ([-1, 1] 程度の範囲を想定)。
	// 位相に直接加算する形で「比例変調」を行う。
	// pitch_factor は LFO PMS 等の per-sample な位相増分倍率 (1.0 で変化なし)。
	float next(float mod_input = 0.0f, float pitch_factor = 1.0f);

	bool    is_idle() const;
	EgState eg_state() const { return m_eg_state; }

private:
	uint32_t m_sample_rate;

	// ---- atomic パラメータ (任意スレッドから書き込み) ----
	std::atomic<double>  m_base_hz{440.0};
	std::atomic<uint8_t> m_multiplier{1};
	std::atomic<float>   m_detune_cents{0.0f};
	std::atomic<uint8_t> m_ks{0};  // 0..3

	std::atomic<float>   m_ar_sec{0.005f};
	std::atomic<float>   m_d1r_sec{0.10f};
	std::atomic<float>   m_sl{0.7f};
	std::atomic<float>   m_d2r_sec{2.0f};
	std::atomic<float>   m_rr_sec{0.05f};
	std::atomic<float>   m_tl_db{0.0f};

	std::atomic<bool>    m_key_on_request{false};
	std::atomic<bool>    m_key_off_request{false};

	// ---- レンダースレッド専用状態 ----
	// 位相は 20-bit 整数 (1 周期 = 2^20)。実機 OPN/OPNA 仕様準拠。
	// 整数の自然な wrap-around (32-bit & mask) で mod 1 を実現し、
	// 浮動小数点の round-to-nearest と異なる量子化アーティファクトを生む。
	uint32_t m_phase_int     = 0;
	uint32_t m_phase_inc_int = 0;  // = eff_hz × 2^20 / sample_rate

	// 位相増分の再計算を避けるためのキャッシュ。
	double  m_cached_base_hz = -1.0;
	uint8_t m_cached_mul     = 0xFF;
	float   m_cached_detune  = -9999.0f;
	// KS の効果係数キャッシュ (1.0 = 効果なし、>1 で EG 短縮)。
	// effective_hz が変わったとき or m_ks が変わったときに再計算。
	uint8_t m_cached_ks       = 0xFF;
	float   m_cached_ks_factor = 1.0f;

	EgState m_eg_state = EgState::Idle;
	// EG は dB 領域で保持。0 dB = 全開、96 dB ≈ 完全無音 (FmOperator 内部の閾値)。
	// 線形領域での減衰は耳に「ランプ」的に聴こえるが、dB 領域 (= 指数減衰) が
	// FM 音源特有の「弾むような立ち上がり、しなやかな余韻」を生む。
	float   m_eg_atten_db = 96.0f;
};
