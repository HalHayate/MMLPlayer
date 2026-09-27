#pragma once
//
// 5 種の基本波形ジェネレータ。
//
// Sine/Square/Triangle/Sawtooth は位相 [0.0, 1.0) を共有するため
// PhaseBasedGenerator にまとめている。Noise だけは位相を持たない別経路。
//
// PSG / FM 用の特殊な発振器 (LFSR ノイズ、サインテーブル参照、etc.) は
// 後の Phase で別ファイルに追加する。
//

#include <cstdint>

#include "IWaveGenerator.h"

// ---- 共通基底: 位相 [0.0, 1.0) を保持し増分を進める。 -----------------
class PhaseBasedGenerator : public IWaveGenerator {
public:
	void set_frequency(double hz, uint32_t sample_rate) override;
	void reset() override;
protected:
	void advance_phase();
	double m_phase     = 0.0;  // [0.0, 1.0)
	double m_phase_inc = 0.0;  // 1 サンプルあたりの位相増分
};

// ---- 各波形 ----------------------------------------------------------
class SineGenerator : public PhaseBasedGenerator {
public:
	float next() override;
};

class SquareGenerator : public PhaseBasedGenerator {
public:
	// デューティ比 (0.0..1.0)。PSG では 0.5 固定だが FM/PWM 用に可変としておく。
	void  set_duty(float duty);
	float next() override;
private:
	float m_duty = 0.5f;
};

class TriangleGenerator : public PhaseBasedGenerator {
public:
	float next() override;
};

class SawtoothGenerator : public PhaseBasedGenerator {
public:
	float next() override;
};

// ---- ノイズ (素朴な 16bit Galois LFSR / 当面のテスト用) --------------
//
// 注: PSG (YM2149) 互換の 17bit LFSR + 周期分周は Phase 3 で別実装する。
//     ここでは「ジェネレータ抽象が音色種別を問わず受けられる」ことの検証用。
class NoiseGenerator : public IWaveGenerator {
public:
	NoiseGenerator();
	void  set_frequency(double hz, uint32_t sample_rate) override;
	float next() override;
	void  reset() override;
private:
	uint32_t m_lfsr = 0xACE1u;
};
