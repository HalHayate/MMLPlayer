//
// Envelope.cpp: ADSR 状態機械の実装。
//

#include "Envelope.h"

void Envelope::set_attack(float seconds) {
	m_attack_sec.store(seconds, std::memory_order_relaxed);
}

void Envelope::set_decay(float seconds) {
	m_decay_sec.store(seconds, std::memory_order_relaxed);
}

void Envelope::set_sustain(float level) {
	if (level < 0.0f) level = 0.0f;
	if (level > 1.0f) level = 1.0f;
	m_sustain_level.store(level, std::memory_order_relaxed);
}

void Envelope::set_release(float seconds) {
	m_release_sec.store(seconds, std::memory_order_relaxed);
}

void Envelope::set_adsr(float attack, float decay, float sustain, float release) {
	set_attack(attack);
	set_decay(decay);
	set_sustain(sustain);
	set_release(release);
}

void Envelope::note_on() {
	// 値を 0 にリセットせず、Attack に入れる。
	// これにより「リリース中の音を再アタック」した場合も滑らかに繋がる。
	m_state = State::Attack;
}

void Envelope::note_off() {
	if (m_state != State::Idle) {
		m_state = State::Release;
	}
}

float Envelope::next(uint32_t sample_rate) {
	if (sample_rate == 0) {
		return m_value;
	}
	const float dt = 1.0f / static_cast<float>(sample_rate);

	switch (m_state) {
	case State::Idle:
		m_value = 0.0f;
		break;

	case State::Attack: {
		const float a = m_attack_sec.load(std::memory_order_relaxed);
		if (a <= 0.0f) {
			m_value = 1.0f;
			m_state = State::Decay;
		} else {
			m_value += dt / a;  // 0 → 1 を a 秒で
			if (m_value >= 1.0f) {
				m_value = 1.0f;
				m_state = State::Decay;
			}
		}
		break;
	}

	case State::Decay: {
		const float d = m_decay_sec.load(std::memory_order_relaxed);
		const float s = m_sustain_level.load(std::memory_order_relaxed);
		if (d <= 0.0f) {
			m_value = s;
			m_state = State::Sustain;
		} else {
			// 1.0 → s を d 秒で
			m_value -= (1.0f - s) * dt / d;
			if (m_value <= s) {
				m_value = s;
				m_state = State::Sustain;
			}
		}
		break;
	}

	case State::Sustain:
		// パラメータがリアルタイム変更された場合に追従。
		m_value = m_sustain_level.load(std::memory_order_relaxed);
		break;

	case State::Release: {
		const float r = m_release_sec.load(std::memory_order_relaxed);
		if (r <= 0.0f) {
			m_value = 0.0f;
			m_state = State::Idle;
		} else {
			// 「1.0 → 0 を r 秒で」の絶対速度で減衰。
			// よって現在値が低ければ早く 0 に到達する (OPNA RR と同流儀)。
			m_value -= dt / r;
			if (m_value <= 0.0f) {
				m_value = 0.0f;
				m_state = State::Idle;
			}
		}
		break;
	}
	}

	return m_value;
}
