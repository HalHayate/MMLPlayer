//
// Voice.cpp: Envelope 駆動の単一発音単位。
//

#include "Voice.h"

Voice::Voice(uint32_t sample_rate, std::unique_ptr<IWaveGenerator> generator)
	: m_sample_rate(sample_rate)
	, m_generator(std::move(generator)) {
}

Voice::~Voice() = default;

void Voice::set_frequency(double hz) {
	m_frequency_hz.store(hz, std::memory_order_relaxed);
}

void Voice::set_amplitude(float amp) {
	m_amplitude.store(amp, std::memory_order_relaxed);
}

void Voice::note_on() {
	m_note_on_request.store(true, std::memory_order_relaxed);
}

void Voice::note_off() {
	m_note_off_request.store(true, std::memory_order_relaxed);
}

bool Voice::is_active() const {
	return !m_envelope.is_idle();
}

float Voice::next() {
	// イベント消化: note_off → note_on の順で処理し、両方立っていれば
	// note_on を優先する (Release を上書きして Attack へ)。
	if (m_note_off_request.exchange(false, std::memory_order_relaxed)) {
		m_envelope.note_off();
	}
	if (m_note_on_request.exchange(false, std::memory_order_relaxed)) {
		// ノートオンでは位相をリセットしてアタック先頭の波形が揃うようにする。
		m_generator->reset();
		m_cached_frequency = -1.0;
		m_envelope.note_on();
	}

	const float env = m_envelope.next(m_sample_rate);

	// Idle になったら波形計算自体スキップ (CPU 削減)。
	if (m_envelope.is_idle()) {
		return 0.0f;
	}

	// 周波数が変わっていればジェネレータに反映。
	const double freq = m_frequency_hz.load(std::memory_order_relaxed);
	if (freq != m_cached_frequency) {
		m_generator->set_frequency(freq, m_sample_rate);
		m_cached_frequency = freq;
	}

	return m_generator->next() * m_amplitude.load(std::memory_order_relaxed) * env;
}
