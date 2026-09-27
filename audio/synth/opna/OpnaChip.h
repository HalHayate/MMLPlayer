#pragma once
//
// OpnaChip: YM2608 (OPNA) 統合チップ。PC-9801-86 音源ボード相当。
//
// 内部構成:
//   - PsgChip   : 3ch SSG (トーン+ノイズ+HW エンベロープ)
//   - FmChannel : 6ch (各 4 オペレータ + 8 アルゴリズム + フィードバック)
//   - パン制御 : FM 各 ch を Off / Right / Left / Both に分配 (ステレオ)
//   - PSG      : センター固定 (実機の SSG はモノラル出力のため)
//
// マスタークロックから派生:
//   PC-9801-86 既定: 7,987,200 Hz
//   - SSG クロック = master / 4 = 1,996,800 Hz
//
// スコープ外 (将来 Phase で追加予定):
//   - ADPCM (1 ch)
//   - Rhythm (6 ch)
//   - レジスタ書き込み風 API (write(addr, data))
//
// スレッドモデル:
//   全 setter は内部の各サブチップ/atomic に伝搬するため任意スレッドから可。
//   render() はレンダースレッド専用 (1 バッファあたり PSG ロックは 1 回だけ)。
//

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "audio/synth/fm/FmChannel.h"
#include "audio/synth/psg/PsgChip.h"
#include "audio/synth/opna/IOpnaChip.h"

// OpnaChip: 自前実装 (Legacy) の OPNA 互換チップ。IOpnaChip を継承し、
// MmlPlayer などからは IOpnaChip インターフェイス越しに使える。
// (ymfm 版と切替可能にするため。)
class OpnaChip : public IOpnaChip {
public:
	// IOpnaChip の k_fm_count / k_psg_count / Pan を再公開 (旧 OpnaChip API 互換)。
	using Pan = IOpnaChip::Pan;

	// PC-9801-86 の YM2608 マスタークロック。
	static constexpr double k_default_master_clock_hz = 7987200.0;

	// F-Number (11-bit) と Block (3-bit) のペア。
	struct FNumBlock {
		uint16_t f_number;  // 0..2047
		uint8_t  block;     // 0..7
	};

	explicit OpnaChip(uint32_t sample_rate,
	                  double   master_clock_hz = k_default_master_clock_hz);
	~OpnaChip();

	OpnaChip(const OpnaChip&)            = delete;
	OpnaChip& operator=(const OpnaChip&) = delete;

	// ---- サブチップへのアクセサ (Legacy 内部用) ----
	// 外部からは IOpnaChip の直接 API を経由するのが望ましい。MmlPlayer も
	// IOpnaChip 経由に統一されたため、これは内部/ユニットテスト向け。
	FmChannel& fm(int index);
	PsgChip&   psg();

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

	// ---- F-Number 系 (Legacy 固有、IOpnaChip には含まれない) ----
	void set_fm_frequency_fnum(int index, uint16_t f_number, uint8_t block);

	// ---- F-Number/Block ⇔ Hz 変換 (master_clock 依存のため static は受け取り型) ----
	static FNumBlock hz_to_fnum_block(double hz, double master_clock_hz);
	static double    fnum_block_to_hz(uint16_t f_number, uint8_t block,
	                                  double master_clock_hz);

	// インスタンスのクロックでの便宜版。
	FNumBlock hz_to_fnum_block(double hz) const;
	double    fnum_block_to_hz(uint16_t f_number, uint8_t block) const;

	// ---- ミックス制御 ----
	void set_fm_volume(float linear)  override;   // 既定 0.5
	void set_psg_volume(float linear) override;  // 既定 0.5

	// ---- レンダー (ステレオ推奨) ----
	// channels=1: L/R を平均してモノに、=2: L,R 個別出力、>=3: 余剰チャンネルは 0。
	void render(int16_t* dst, uint32_t frames, uint32_t channels) override;

	double master_clock_hz() const { return m_master_clock_hz; }

	// ---- チップ全体 LFO (実機 OPNA 仕様) ----
	// OPNA は LFO をチップ単位で 1 つ持ち、各 FM ch が PMS/AMS で感度を調整する。
	// MUSIC.COM ドライバはこれを使わずソフトウェア LFO で代替するため、当チップ LFO は
	// 既定で無効 (speed_index=0 = enable bit OFF)。実機準拠の構造を将来拡張用に整備。
	//
	// speed_index: 0..7 で LFO 周波数を切替 (実機表: 3.98/5.56/6.02/6.37/6.88/9.63/48.1/72.2 Hz)
	//   speed_index < 0 で LFO 無効化 (出力固定 0)。
	void  set_chip_lfo_speed_index(int speed_index);
	int   chip_lfo_speed_index() const;

	// 現在の LFO 値 ([-1, 1])。レンダースレッド専用。
	// 実機 OPNA は 32-step ピラミッド (0,1,2,...,15,15,14,...,0,-0,-1,...,-15,-15,...) で
	// 8 サイクル繰り返す形だが、当実装は -1..1 の三角波で近似 (PMS/AMS の影響範囲は同等)。
	float chip_lfo_value() const;

private:
	uint32_t m_sample_rate;
	double   m_master_clock_hz;

	std::unique_ptr<PsgChip>   m_psg;
	std::unique_ptr<FmChannel> m_fm[k_fm_count];

	std::atomic<uint8_t> m_fm_pan[k_fm_count];
	std::atomic<float>   m_fm_volume{0.5f};
	std::atomic<float>   m_psg_volume{0.5f};

	// PSG レンダーの一時バッファ (レンダースレッド専用)。
	std::vector<float>   m_psg_temp;

	// ---- チップ全体 LFO 状態 ----
	// speed_index < 0 で無効。レンダースレッドが per-sample で位相を進める。
	std::atomic<int>     m_chip_lfo_speed_index{-1};
	double               m_chip_lfo_phase = 0.0;  // [0, 1) レンダースレッド専用
	float                m_chip_lfo_value = 0.0f; // 最後に計算した値 (FmChannel が参照可)
};
