#pragma once
//
// OpngenOpnaChip: np21w 内蔵の OPN generator (opngen) + PSG (psggen) を OPNA
// バックエンドとして使うラッパ。NP2 のリファレンス録音と同一エンジン。
//
// 設計:
//   - YmfmOpnaChip と同じく「ドメイン API → OPNA レジスタ値 → write_reg」方式。
//     変換式は OpnaRegEncode.h を共有。
//   - opngen は FM 専用、psggen は SSG 専用なので、 render で別々に getpcm して
//     FM/SSG に独立ゲインを掛けて合算する (ymfm 系と同じ独立制御)。
//   - opngen/psggen の C 構造体はヘッダに漏らさず PIMPL で隠蔽。
//
#include <cstdint>

#include "audio/synth/opna/IOpnaChip.h"

class OpngenOpnaChip : public IOpnaChip {
public:
	static constexpr double k_default_master_clock_hz = 7987200.0;

	OpngenOpnaChip(uint32_t output_rate, double master_clock_hz);
	~OpngenOpnaChip() override;

	void fm_key_on (int ch) override;
	void fm_key_off(int ch) override;
	void set_fm_frequency_hz(int ch, double hz) override;
	void set_fm_algorithm   (int ch, uint8_t alg_3bit) override;
	void set_fm_feedback    (int ch, uint8_t fb_3bit)  override;
	void set_fm_algorithm_feedback(int ch, uint8_t alg_3bit, uint8_t fb_3bit) override;
	void set_fm_lfo(int ch, uint8_t waveform, uint8_t speed, uint8_t depth) override;
	void set_fm_pan(int ch, Pan pan) override;

	void set_fm_op_attack_rate   (int ch, int op, float seconds)   override;
	void set_fm_op_decay1_rate   (int ch, int op, float seconds)   override;
	void set_fm_op_sustain_level (int ch, int op, float sl_0_to_1) override;
	void set_fm_op_decay2_rate   (int ch, int op, float seconds)   override;
	void set_fm_op_release_rate  (int ch, int op, float seconds)   override;
	void set_fm_op_total_level_db(int ch, int op, float db)        override;
	void set_fm_op_multiplier    (int ch, int op, uint8_t mul_4bit) override;
	void set_fm_op_detune_cents  (int ch, int op, float cents)     override;
	void set_fm_op_key_scaling   (int ch, int op, uint8_t ks_2bit) override;

	void psg_reset() override;
	void set_psg_tone_frequency (int ch, double hz)             override;
	void set_psg_channel_volume (int ch, uint8_t volume_5bit)   override;
	void set_psg_mixer          (int ch, bool tone, bool noise) override;
	void set_psg_envelope_shape (uint8_t shape_4bit)            override;
	void set_psg_envelope_period(uint16_t period_16bit)         override;

	void set_fm_volume (float linear) override;
	void set_psg_volume(float linear) override;

	void render(int16_t* dst, uint32_t frames, uint32_t channels) override;

	void write_reg(uint8_t port, uint8_t addr, uint8_t val) override;

private:
	struct Impl;
	Impl* m_impl = nullptr;

	double   m_master_clock_hz = k_default_master_clock_hz;
	uint32_t m_output_rate     = 48000;
	float    m_fm_volume       = 1.0f;
	float    m_ssg_volume      = 1.0f;

	void emit(int port, uint8_t addr, uint8_t val);
	void flush_dt_mul(int ch, int op);
	void flush_tl(int ch, int op);
	void flush_ks_ar(int ch, int op);
	void flush_d1r(int ch, int op);
	void flush_d2r(int ch, int op);
	void flush_sl_rr(int ch, int op);
	void flush_fb_alg(int ch);
	void flush_lr_ams_pms(int ch);
	void flush_freq(int ch);
};
