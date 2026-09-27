#pragma once
//
// PsgChip: YM2149 / OPNA SSG 部 (3声 + ノイズ + ハードウェアエンベロープ) 互換。
//
// 内部構成:
//   - トーン   : 3 ch、12-bit Tone Period (TP)。f = clock_hz / (16 * TP)
//   - ノイズ   : 17-bit LFSR (x^17 + x^14 + 1)、5-bit Noise Period (NP)
//                f_noise = clock_hz / (16 * NP)
//   - エンベロープ: 16 段、4-bit shape (10 種類)、16-bit Period (EP)
//                  f_step = clock_hz / (256 * EP)
//   - ミキサ   : チャンネル毎に tone/noise を独立 enable
//   - 音量     : 4-bit (16段の対数テーブル) もしくは HW エンベロープ追従
//
// チャンネル 0..2 は AY 用語の A/B/C に対応。
//
// 設計上の理由でチップ単位の抽象としている:
//   PSG は 3 ch がノイズ/エンベロープを共有するため、
//   独立した Voice の集合では正確に表現できない。
//
// スレッドモデル:
//   全 setter とレンダーは内部 mutex で保護。
//   レンダーは 1 ロックで 1 バッファ全体を処理するため接触時間は短い。
//

#include <cstdint>
#include <mutex>

class PsgChip {
public:
	static constexpr int k_channel_count = 3;

	// shape (4-bit) の代表値。
	// コメント末尾が '\' にならないよう波形は文字列で記述している
	// (C/C++ では行末 '\' が行連結扱いになり次行を吸収するため)。
	enum EnvelopeShape : uint8_t {
		shape_decay_hold_low   = 0x0,  // decay then hold low     (\___)
		shape_attack_hold_low  = 0x4,  // attack then hold low    (/___)
		shape_sawtooth_down    = 0x8,  // sawtooth down (repeat decay)
		shape_triangle_down    = 0xA,  // triangle decay-attack   (\/\/)
		shape_decay_hold_high  = 0xB,  // decay then hold high    (\---)
		shape_sawtooth_up      = 0xC,  // sawtooth up (repeat attack)
		shape_attack_hold_high = 0xD,  // attack then hold high   (/---)
		shape_triangle_up      = 0xE,  // triangle attack-decay   (/\/\)
		shape_attack_hold_low2 = 0xF,  // attack then hold low    (same as 0x4)
	};

	// PC-9801-86 (YM2608) の SSG クロック近似値。
	static constexpr double k_default_clock_hz = 1996800.0;

	// volume 5-bit 値: 下位 4 bit が音量、bit 4 (0x10) で「HW エンベロープ追従」。
	static constexpr uint8_t volume_envelope_bit = 0x10;

	explicit PsgChip(uint32_t sample_rate, double clock_hz = k_default_clock_hz);
	~PsgChip();

	PsgChip(const PsgChip&)            = delete;
	PsgChip& operator=(const PsgChip&) = delete;

	void reset();

	// ---- レジスタ風 API (低レベル) ----
	void set_tone_period(int channel, uint16_t tp_12bit);   // 0..4095
	void set_noise_period(uint8_t np_5bit);                  // 0..31
	void set_envelope_period(uint16_t ep_16bit);             // 0..65535
	void set_envelope_shape(uint8_t shape_4bit);             // 書込みでエンベロープ再起動
	void set_mixer(int channel, bool enable_tone, bool enable_noise);
	void set_channel_volume(int channel, uint8_t volume_5bit);  // bit 4 で HW エンベロープ追従

	// ---- 音楽向けヘルパ (Hz 直接指定) ----
	void set_tone_frequency(int channel, double hz);
	void set_noise_frequency(double hz);
	void set_envelope_frequency(double cycle_hz);  // shape 一周期の周波数

	// ---- レンダー ----
	// frames フレーム × channels (1=モノ / 2=ステレオで同値複製) を dst に書き込む。
	// MeAudioStream::FillCallback と互換。
	void render(int16_t* dst, uint32_t frames, uint32_t channels);

	// ミックス用: モノラル float [-1, 1] を frames 個 dst に書き込む。
	// OpnaChip など他チップと混ぜる用途で 1 ロックで一括処理する。
	void render_mono_float(float* dst, uint32_t frames);

private:
	struct ToneState {
		uint16_t period       = 1;     // 12-bit、0/1 はどちらも最高音相当
		double   phase        = 0.0;   // [0.0, 1.0)
		double   phase_inc    = 0.0;   // 1 サンプルあたりの位相増分
		bool     enable_tone  = false;
		bool     enable_noise = false;
		uint8_t  volume_raw   = 0;     // 5-bit (bit 4 で HW envelope)
	};

	struct NoiseState {
		uint8_t  period   = 1;       // 5-bit
		double   counter  = 0.0;     // ノイズステップ用カウンタ (秒換算)
		double   step_inc = 0.0;     // 1 サンプルあたりの進み量 (Hz/sr)
		uint32_t lfsr     = 1;       // 17-bit
		bool     bit      = false;
	};

	struct EnvelopeState {
		uint16_t period      = 1;     // 16-bit
		double   counter     = 0.0;
		double   step_inc    = 0.0;
		uint8_t  shape       = 0;     // 4-bit
		uint8_t  step_index  = 0;     // 0..15
		bool     attacking   = false;
		bool     holding     = false;
	};

	// 内部メソッド (mutex 取得済み前提)。
	void recompute_tone_phase_inc(int channel);
	void recompute_noise_step_inc();
	void recompute_envelope_step_inc();
	void restart_envelope();
	void advance_envelope_one_step();
	void advance_one_sample_locked();
	float current_sample_locked();

	std::mutex      m_mutex;
	uint32_t        m_sample_rate;
	double          m_clock_hz;
	ToneState       m_tones[k_channel_count];
	NoiseState      m_noise;
	EnvelopeState   m_envelope;
};
