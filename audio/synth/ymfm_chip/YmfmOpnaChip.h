#pragma once
//
// YmfmOpnaChip: ymfm::ym2608 (Aaron Giles, BSD-3) の最小ラッパ。
//
// 役割:
//   - ymfm のコアを保持し、レジスタ書き込み API (write_reg) を公開
//   - 内部 sample_rate (ymfm が決める固定レート) → 利用側 sample_rate へ線形補間
//   - ステレオ int16 PCM を render() で返す (OpnaChip と同じ呼び出し規約)
//
// スコープ (Phase J2a):
//   - レジスタ書き込み + 出力。MML パスとの統合は J2c で実施。
//   - 実機 ADPCM-A の Rhythm ROM は供給しないため Rhythm 音は鳴らない。
//     (MUSIC.COM 配下の MML はリズム未使用なので問題なし)
//
// スレッドモデル:
//   write_reg / render とも単一スレッド前提 (MMLPlayer は worker からのみ書き、
//   render() は audio スレッドのみ。アクセスは時刻的に重ならない)。
//

#include <cstdint>
#include <memory>
#include <vector>

#include "audio/synth/opna/IOpnaChip.h"

// 前方宣言で ymfm のテンプレ実装ヘッダを露出させない (コンパイル時間とインクル拡散の抑制)。
namespace ymfm { class ym2608; }

class YmfmOpnaChip : public IOpnaChip {
public:
	// OPNA マスタークロック (PC-9801-86): 7,987,200 Hz。
	static constexpr double k_default_master_clock_hz = 7987200.0;

	// ymfm の内部出力レート選択。
	//   Low  : input/48 (~ 166.4 kHz @ 7.987 MHz) — 速い、48kHz への変換比 ~3.47
	//   Med  : input/24 (~ 332.8 kHz)
	//   High : input/8  (~ 998.4 kHz) — 最高品質、変換比 ~20.8
	enum class Fidelity {
		Low,
		Medium,
		High,
	};

	explicit YmfmOpnaChip(uint32_t output_sample_rate,
	                      double   master_clock_hz = k_default_master_clock_hz,
	                      Fidelity fidelity        = Fidelity::Low);
	~YmfmOpnaChip();

	YmfmOpnaChip(const YmfmOpnaChip&)            = delete;
	YmfmOpnaChip& operator=(const YmfmOpnaChip&) = delete;

	// ---- レジスタ書き込み (低レベル / テスト用) ----
	// port: 0 (アドレス 0x00..0xFF, FM ch1-3 + SSG + ADPCM) / 1 (アドレス 0x100..0x1FF, FM ch4-6)
	// IOpnaChip インターフェイス経由の virtual もエイリアス用に override。
	void write_reg(int port, uint8_t addr, uint8_t value);
	void write_reg(uint8_t port, uint8_t addr, uint8_t value) override {
		write_reg(static_cast<int>(port), addr, value);
	}

	// ---- S98 録音 (np21w v1 互換フォーマット) ----
	// 録音中は write_reg / 各 op-level setter 経由のレジスタ書き込みが S98 に記録される。
	// 時間進行は render() で消費した サンプル数 × 1000 / output_sample_rate ms。
	bool start_s98_recording(const char* path);
	void stop_s98_recording();
	bool is_recording_s98() const;

	// ---- IOpnaChip 実装 ----
	// FM ch level
	void fm_key_on (int ch) override;
	void fm_key_off(int ch) override;
	void set_fm_frequency_hz(int ch, double hz) override;
	void set_fm_algorithm   (int ch, uint8_t alg_3bit) override;
	void set_fm_feedback    (int ch, uint8_t fb_3bit)  override;
	void set_fm_algorithm_feedback(int ch, uint8_t alg_3bit, uint8_t fb_3bit) override;
	void set_fm_lfo(int ch, uint8_t waveform, uint8_t speed, uint8_t depth) override;
	void set_fm_pan(int ch, Pan pan) override;
	// FM op level
	void set_fm_op_attack_rate   (int ch, int op, float seconds)   override;
	void set_fm_op_decay1_rate   (int ch, int op, float seconds)   override;
	void set_fm_op_sustain_level (int ch, int op, float sl_0_to_1) override;
	void set_fm_op_decay2_rate   (int ch, int op, float seconds)   override;
	void set_fm_op_release_rate  (int ch, int op, float seconds)   override;
	void set_fm_op_total_level_db(int ch, int op, float db)        override;
	void set_fm_op_multiplier    (int ch, int op, uint8_t mul_4bit) override;
	void set_fm_op_detune_cents  (int ch, int op, float cents)     override;
	void set_fm_op_key_scaling   (int ch, int op, uint8_t ks_2bit) override;
	// PSG
	void psg_reset() override;
	void set_psg_tone_frequency (int ch, double hz)             override;
	void set_psg_channel_volume (int ch, uint8_t volume_5bit)   override;
	void set_psg_mixer          (int ch, bool tone, bool noise) override;
	void set_psg_envelope_shape (uint8_t shape_4bit)            override;
	void set_psg_envelope_period(uint16_t period_16bit)         override;

	// ---- ミックスゲイン (linear、render 時に適用) ----
	void  set_fm_volume(float linear)  override;
	void  set_psg_volume(float linear) override;
	float fm_volume()  const { return m_fm_volume; }
	float ssg_volume() const { return m_ssg_volume; }

	// ---- レンダ (ステレオ推奨) ----
	void render(int16_t* dst, uint32_t frames, uint32_t channels) override;

	// ---- 内部状態確認 (テスト用) ----
	uint32_t output_sample_rate() const { return m_output_rate; }
	uint32_t ymfm_sample_rate()   const { return m_ymfm_rate; }

	// Impl は public 前方宣言 (.cpp 内の自由関数から shadow state を扱えるように)。
	// 実体定義も .cpp 側にあるため、ymfm のテンプレ展開を公開ヘッダに漏らさない。
	struct Impl;

private:
	std::unique_ptr<Impl> m_impl;

	// ホットパスでアクセスする値のキャッシュ (Impl の中まで都度入らないように)。
	uint32_t m_output_rate     = 0;
	uint32_t m_ymfm_rate       = 0;
	double   m_master_clock_hz = 0.0;
	float    m_fm_volume       = 1.0f;
	float    m_ssg_volume      = 1.0f;
};
