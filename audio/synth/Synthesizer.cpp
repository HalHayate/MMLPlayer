//
// Synthesizer.cpp: 複数 Voice をミックスして PCM を書き出す。
//

#include "Synthesizer.h"

#include "IWaveGenerator.h"
#include "Voice.h"

Synthesizer::Synthesizer(uint32_t sample_rate)
	: m_sample_rate(sample_rate) {
}

Synthesizer::~Synthesizer() = default;

void Synthesizer::set_voice(uint32_t index, std::unique_ptr<IWaveGenerator> generator) {
	if (m_voices.size() <= index) {
		m_voices.resize(index + 1);
	}
	m_voices[index] = std::make_unique<Voice>(m_sample_rate, std::move(generator));
}

Voice& Synthesizer::voice(uint32_t index) {
	return *m_voices.at(index);
}

uint32_t Synthesizer::voice_count() const {
	return static_cast<uint32_t>(m_voices.size());
}

void Synthesizer::set_master_volume(float v) {
	m_master_volume = v;
}

void Synthesizer::render(int16_t* dst, uint32_t frames, uint32_t channels) {
	for (uint32_t i = 0; i < frames; ++i) {
		// 全ボイスをモノラル和算。
		float mix = 0.0f;
		for (auto& v : m_voices) {
			if (v) {
				mix += v->next();
			}
		}
		mix *= m_master_volume;

		// クリッピング (Phase 1 では素朴にハードクリップ)。
		if (mix >  1.0f) mix =  1.0f;
		if (mix < -1.0f) mix = -1.0f;

		const int16_t sample = static_cast<int16_t>(mix * 32767.0f);
		for (uint32_t c = 0; c < channels; ++c) {
			dst[i * channels + c] = sample;
		}
	}
}
