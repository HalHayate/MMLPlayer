#pragma once
//
// IOpnaChip: OPNA 互換チップの統一インターフェイス。
//
// 目的:
//   既存の自前 OPNA (LegacyOpnaChip = 旧 OpnaChip 実装) と
//   ymfm 由来の YmfmOpnaChip を、MmlPlayer から差し替えて使えるようにする。
//
// 設計方針:
//   - メソッドは「ch / op を引数で取る直接 API」のみ。 FmChannel&/PsgChip& のような
//     サブオブジェクト参照は返さない (ymfm 側で互換シムを書きづらいため)。
//   - 単位は「秒 / Hz / dB / 0..1」のドメイン値を維持。実装側 (ymfm wrapper) で
//     OPNA レジスタ値 (AR=0..31 等) に逆換算する。
//   - render() は int16 ステレオ (channels=2) を前提。MmlPlayer / WAV ライタは
//     OpnaChip 時代から同じ呼び出し規約。
//
// スレッドモデル:
//   全 setter は任意スレッドから可。render() はレンダースレッド専用。
//

#include <cstdint>

class IOpnaChip {
public:
	// FM 各 ch のステレオパン (実機 OPNA の R/L ビットと同配置)。
	enum class Pan : uint8_t {
		Off   = 0b00,
		Right = 0b01,
		Left  = 0b10,
		Both  = 0b11,
	};

	static constexpr int k_fm_count  = 6;
	static constexpr int k_psg_count = 3;
	static constexpr int k_fm_op     = 4;  // 1 ch あたり 4 op

	virtual ~IOpnaChip() = default;

	// ---- FM ch レベル ----
	virtual void fm_key_on (int ch) = 0;
	virtual void fm_key_off(int ch) = 0;
	virtual void set_fm_frequency_hz(int ch, double hz) = 0;
	virtual void set_fm_algorithm   (int ch, uint8_t alg_3bit) = 0;
	virtual void set_fm_feedback    (int ch, uint8_t fb_3bit)  = 0;
	// ALG と FB を 1 回のレジスタ書き込みでまとめて設定する。
	// 個別に呼ぶと FB/ALG レジスタ (0xB0) が 2 回書かれ、 音色変更時に
	// 「新 ALG + 旧 FB」 の中間値が一瞬書かれる (同 tick 上書きで無音だが
	// MUSIC.COM には無い余分な書き込み。 ドラム ch 等の高速音色変更で顕著)。
	virtual void set_fm_algorithm_feedback(int ch, uint8_t alg_3bit, uint8_t fb_3bit) = 0;
	// MUSIC.COM 等の LFO 指定 (waveform/speed/depth)。ymfm 側はチップ全体 LFO に
	// マップされるため、各 ch の depth は AMS/PMS レジスタへ反映される。
	virtual void set_fm_lfo(int ch, uint8_t waveform, uint8_t speed, uint8_t depth) = 0;
	virtual void set_fm_pan(int ch, Pan pan) = 0;

	// ---- FM op レベル ----
	virtual void set_fm_op_attack_rate   (int ch, int op, float seconds)   = 0;
	virtual void set_fm_op_decay1_rate   (int ch, int op, float seconds)   = 0;
	virtual void set_fm_op_sustain_level (int ch, int op, float sl_0_to_1) = 0;
	virtual void set_fm_op_decay2_rate   (int ch, int op, float seconds)   = 0;
	virtual void set_fm_op_release_rate  (int ch, int op, float seconds)   = 0;
	virtual void set_fm_op_total_level_db(int ch, int op, float db)        = 0;
	virtual void set_fm_op_multiplier    (int ch, int op, uint8_t mul_4bit) = 0;
	virtual void set_fm_op_detune_cents  (int ch, int op, float cents)     = 0;
	virtual void set_fm_op_key_scaling   (int ch, int op, uint8_t ks_2bit) = 0;

	// ---- PSG (SSG 部) ----
	virtual void psg_reset() = 0;
	virtual void set_psg_tone_frequency (int ch, double hz)             = 0;
	virtual void set_psg_channel_volume (int ch, uint8_t volume_5bit)   = 0;
	virtual void set_psg_mixer          (int ch, bool tone, bool noise) = 0;
	virtual void set_psg_envelope_shape (uint8_t shape_4bit)            = 0;
	virtual void set_psg_envelope_period(uint16_t period_16bit)         = 0;

	// ---- ミックス制御 ----
	virtual void set_fm_volume (float linear) = 0;
	virtual void set_psg_volume(float linear) = 0;

	// ---- レンダー (int16 ステレオ前提) ----
	virtual void render(int16_t* dst, uint32_t frames, uint32_t channels) = 0;

	// ---- レジスタ直接書き込み (MUSIC.COM の Y コマンド) ----
	// ymfm 系: 実機レジスタへ直接書き込む。 Legacy 系: 「秒/Hz/dB」 のドメインで
	// 動作するため逆換算が必要な箇所が多く、 デフォルトは no-op。
	virtual void write_reg(uint8_t /*port*/, uint8_t /*addr*/, uint8_t /*val*/) {}
};
