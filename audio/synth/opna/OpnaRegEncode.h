#pragma once
//
// OpnaRegEncode: ドメイン値 (秒/Hz/dB/0..1) → OPNA レジスタ値の純変換ヘルパ群。
//
// YmfmOpnaChip.cpp が anon namespace に持つ同名ヘルパと「同一の式」。
// FmgenOpnaChip が同じレジスタ値を生成するために共有する。
//   注: 現状 YmfmOpnaChip は自前コピーを使い続けている (実機照合済みで安定のため
//   無闇に触らない方針)。式を調整する際は両方を更新すること。
//
#include <cstdint>
#include <cmath>

namespace opna_enc {

// 秒 → AR (0..31)。実機準拠: T_AR = 30 × 2^((4-AR)/2) を逆解き。
inline uint8_t seconds_to_ar(float sec) {
	if (sec <= 0.0001f) return 31;
	const float ar_f = 4.0f - 2.0f * std::log2(sec / 30.0f);
	if (ar_f <= 0.0f)  return 0;
	if (ar_f >= 31.0f) return 31;
	return static_cast<uint8_t>(ar_f + 0.5f);
}

// 秒 → DR/SR (D1R/D2R, 0..31)。AR と同じ式。
inline uint8_t seconds_to_dr(float sec) { return seconds_to_ar(sec); }

// 秒 → RR (4-bit 0..15)。eff = 4*RR、T_RR = 30 × 2^((4-eff)/2) を逆解き。
inline uint8_t seconds_to_rr(float sec) {
	if (sec <= 0.0f) return 15;
	const float eff_f = 4.0f - 2.0f * std::log2(sec / 30.0f);
	if (eff_f >= 60.0f) return 15;
	if (eff_f <= 0.0f)  return 0;
	const float rr_f = eff_f / 4.0f;
	return static_cast<uint8_t>(rr_f + 0.5f);
}

// 線形 SL (0..1) → 4-bit SL レジスタ値 (1 step = -3 dB)。
inline uint8_t sl_to_4bit(float sl_0_1) {
	if (sl_0_1 >= 1.0f) return 0;
	if (sl_0_1 <= 0.0f) return 15;
	const float db = -20.0f * std::log10(sl_0_1);
	const float sl_f = db / 3.0f;
	if (sl_f >= 15.0f) return 15;
	if (sl_f <= 0.0f)  return 0;
	return static_cast<uint8_t>(sl_f + 0.5f);
}

// dB → TL (7-bit, 0..127、1 step = 0.75 dB)。
inline uint8_t db_to_tl(float db) {
	if (db <= 0.0f)   return 0;
	if (db >= 95.0f)  return 127;
	const float tl_f = db / 0.75f;
	return static_cast<uint8_t>(tl_f + 0.5f);
}

// cents → DT (3-bit: bit2 = sign, bits 1-0 = magnitude)。絶対 cents の量子化近似。
inline uint8_t cents_to_dt(float cents) {
	const float abs_c = std::abs(cents);
	uint8_t mag = (abs_c < 2.5f) ? 0
	            : (abs_c < 7.5f) ? 1
	            : (abs_c < 15.0f) ? 2
	            : 3;
	const uint8_t sign = (cents < 0.0f) ? 0x4 : 0x0;
	return sign | mag;
}

// Hz → F-Number (11-bit) + Block (3-bit)、実機 OPNA 準拠式。
// A4=440 → F=1039 block=4、A1=55 → F=617 block=1 になる規約。
inline void hz_to_fnum_block(double hz, double master_clock_hz,
                             uint16_t& fnum, uint8_t& block) {
	if (hz <= 0.0 || master_clock_hz <= 0.0) { fnum = 0; block = 1; return; }
	constexpr double k_div = 144.0;
	const double k_inv = k_div * static_cast<double>(1 << 20) / master_clock_hz;
	double f0 = hz * k_inv;
	int blk = 1;
	while (f0 >= 1234.0 && blk < 7) { f0 *= 0.5; ++blk; }
	while (f0 < 617.0 && blk > 1)   { f0 *= 2.0; --blk; }
	if (f0 < 0.0)    f0 = 0.0;
	if (f0 > 2047.0) f0 = 2047.0;
	fnum  = static_cast<uint16_t>(f0 + 0.5);
	block = static_cast<uint8_t>(blk);
}

// 前回 block を考慮して block 切替にヒステリシスを入れた版 (ビブラートの不連続防止)。
inline void hz_to_fnum_block_hysteresis(double hz, double master_clock_hz,
                                        uint16_t& fnum, uint8_t& block,
                                        uint8_t prev_block) {
	if (hz <= 0.0 || master_clock_hz <= 0.0) { fnum = 0; block = 1; return; }
	constexpr double k_div = 144.0;
	const double k_inv = k_div * static_cast<double>(1 << 20) / master_clock_hz;
	const double f_base = hz * k_inv;
	if (prev_block >= 1 && prev_block <= 7) {
		const double f_at_prev = f_base / static_cast<double>(1 << (prev_block - 1));
		if (f_at_prev >= 256.0 && f_at_prev <= 2047.0) {
			fnum  = static_cast<uint16_t>(f_at_prev + 0.5);
			block = prev_block;
			return;
		}
	}
	hz_to_fnum_block(hz, master_clock_hz, fnum, block);
}

// SSG トーン Hz → tone period (12-bit)。MUSIC.COM は整数除算 (floor)。
// clock = master/4 (OPNA 既定)。[[musiccom_pitch_table]] の SSG floor 実測準拠。
inline uint16_t psg_hz_to_tp(double hz, double master_clock_hz) {
	const double psg_clock = master_clock_hz / 4.0;
	if (hz <= 0.0) return 0;
	uint16_t tp = static_cast<uint16_t>(psg_clock / (16.0 * hz));
	if (tp > 0xFFF) tp = 0xFFF;
	return tp;
}

}  // namespace opna_enc
