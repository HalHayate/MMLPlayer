#pragma once
//
// FM 音色プリセット型。
// MUSIC.COM の SOUND: ブロックから生成され、Player が OPNA FM ch に適用する。
//
// OPN レジスタの raw 値ではなく、すでに「秒」「dB」「cents」に変換した
// 形で保持する。FmOperator のセッタへ直接渡せるよう揃えてある。
// raw 値 ⇔ 物理量の換算は MmlFileParser 内で行う。
//

#include <cstdint>
#include <vector>

namespace mml {

// SSG ソフトウェアエンベロープ。SSGENV:@N, period, v1, v2, ... の解析結果。
// 各 step で 1 つの音量 (0..15) を発音し、period × 64分音符 だけ持続する。
// 配列末尾に達したら最後の値で固定 (= 持続)。
struct SsgEnvelope {
	uint16_t              period_64th = 1;  // 1..64 (×64分音符)
	std::vector<uint8_t>  volumes;          // 0..15
};

struct FmPreset {
	uint8_t algorithm = 4;   // 0..7
	uint8_t feedback  = 0;   // 0..7

	struct Op {
		// EG (秒単位)
		float ar_sec   = 0.005f;  // Attack
		float d1r_sec  = 0.10f;   // Decay 1
		float sl       = 0.7f;    // Sustain Level (0..1)
		float d2r_sec  = 2.0f;    // Decay 2 (Sustain Rate)
		float rr_sec   = 0.10f;   // Release
		// 出力減衰 (dB、0 = 最大)
		float tl_db    = 0.0f;
		// 周波数倍率・微調整
		uint8_t mul    = 1;       // 0=×0.5, 1..15=×N
		float   detune = 0.0f;    // cents
		// Key Scaling (0..3)。高音ほど EG が速くなる効果の強さ。
		uint8_t ks     = 0;
		// DT2 (0..3): ch3 特殊モード専用 (MUSIC.COM OPM DT2 エミュレーション)
		uint8_t dt2    = 0;
	} ops[4];

	// LFO
	uint8_t lfo_waveform = 0;
	uint8_t lfo_speed    = 0;
	int16_t lfo_depth    = 0;

	// いずれかの OP に DT2 != 0 が設定されているとき true。
	// ch3 でのみ有効。このフラグが立っているとき ch3 特殊モードを使う。
	bool has_dt2 = false;
};

}  // namespace mml
