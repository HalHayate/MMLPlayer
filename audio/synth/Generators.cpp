//
// Generators.cpp: 5 種類の基本波形ジェネレータ実装。
//

#include "Generators.h"

#include <cmath>

namespace {
constexpr double k_two_pi = 6.28318530717958647692;
}

// ---- PhaseBasedGenerator --------------------------------------------

void PhaseBasedGenerator::set_frequency(double hz, uint32_t sample_rate) {
	m_phase_inc = (sample_rate > 0) ? (hz / static_cast<double>(sample_rate)) : 0.0;
}

void PhaseBasedGenerator::reset() {
	m_phase = 0.0;
}

void PhaseBasedGenerator::advance_phase() {
	m_phase += m_phase_inc;
	// 1.0 を跨いだら巻き戻す。1.0 をはるかに超えるケースは set_frequency の
	// 異常値以外起きないので floor を呼ぶのは保険。
	if (m_phase >= 1.0) {
		m_phase -= std::floor(m_phase);
	}
}

// ---- Sine ------------------------------------------------------------

float SineGenerator::next() {
	const float v = static_cast<float>(std::sin(m_phase * k_two_pi));
	advance_phase();
	return v;
}

// ---- Square ----------------------------------------------------------

void SquareGenerator::set_duty(float duty) {
	m_duty = duty;
}

float SquareGenerator::next() {
	const float v = (m_phase < static_cast<double>(m_duty)) ? 1.0f : -1.0f;
	advance_phase();
	return v;
}

// ---- Triangle --------------------------------------------------------

float TriangleGenerator::next() {
	// 0..0.5 で -1 → +1、0.5..1 で +1 → -1
	const float v = (m_phase < 0.5)
		? static_cast<float>(-1.0 + 4.0 * m_phase)
		: static_cast<float>( 3.0 - 4.0 * m_phase);
	advance_phase();
	return v;
}

// ---- Sawtooth --------------------------------------------------------

float SawtoothGenerator::next() {
	const float v = static_cast<float>(2.0 * m_phase - 1.0);
	advance_phase();
	return v;
}

// ---- Noise (暫定: 16bit Galois LFSR) --------------------------------

NoiseGenerator::NoiseGenerator() = default;

void NoiseGenerator::set_frequency(double, uint32_t) {
	// 当面のホワイトノイズ実装では周波数を持たない。
	// PSG 互換ではここで「ノイズ周期 (NP)」相当の分周を更新する予定。
}

void NoiseGenerator::reset() {
	m_lfsr = 0xACE1u;
}

float NoiseGenerator::next() {
	const uint32_t lsb = m_lfsr & 1u;
	m_lfsr >>= 1;
	if (lsb) {
		m_lfsr ^= 0xB400u;  // x^16 + x^14 + x^13 + x^11 + 1
	}
	return (m_lfsr & 1u) ? 1.0f : -1.0f;
}
