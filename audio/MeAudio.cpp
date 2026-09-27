//
// MeAudio.cpp: XAudio2 を用いた音声出力の実装。
// windows.h / xaudio2.h は audio_internal.h 内に閉じ込める。
//

#include "audio_internal.h"

#include <cmath>
#include <vector>
#include <thread>
#include <chrono>

using Microsoft::WRL::ComPtr;

namespace {
constexpr double k_pi = 3.14159265358979323846;
}

// ---- 公開メンバ ----------------------------------------------------------

MeAudio::MeAudio() = default;

MeAudio::~MeAudio() {
	shutdown();
}

bool MeAudio::initialize(uint32_t sample_rate, uint32_t channels) {
	if (m_impl) {
		shutdown();
	}
	m_impl = new Impl();
	m_impl->sample_rate = sample_rate;
	m_impl->channels    = channels;

	// XAudio2 2.8+ では COM 初期化は必須ではないが、他コンポーネントとの整合のため行う。
	// S_FALSE (既に初期化済み) も成功扱い。RPC_E_CHANGED_MODE は無視。
	const HRESULT hr_co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	m_impl->com_initialized = SUCCEEDED(hr_co);

	if (FAILED(XAudio2Create(m_impl->xaudio2.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR))) {
		return false;
	}

	if (FAILED(m_impl->xaudio2->CreateMasteringVoice(
			&m_impl->mastering_voice,
			channels,
			sample_rate))) {
		return false;
	}

	return true;
}

bool MeAudio::play_sine(double frequency_hz, uint32_t duration_ms, float amplitude) {
	if (!m_impl || !m_impl->xaudio2) {
		return false;
	}

	const uint32_t sample_rate   = m_impl->sample_rate;
	const uint32_t channels      = m_impl->channels;
	const uint32_t total_samples = (sample_rate * duration_ms) / 1000;
	if (total_samples == 0) {
		return false;
	}

	// 16bit PCM フォーマット記述。
	WAVEFORMATEX wave_format = {};
	wave_format.wFormatTag      = WAVE_FORMAT_PCM;
	wave_format.nChannels       = static_cast<WORD>(channels);
	wave_format.nSamplesPerSec  = sample_rate;
	wave_format.wBitsPerSample  = 16;
	wave_format.nBlockAlign     = static_cast<WORD>((wave_format.nChannels * wave_format.wBitsPerSample) / 8);
	wave_format.nAvgBytesPerSec = wave_format.nSamplesPerSec * wave_format.nBlockAlign;
	wave_format.cbSize          = 0;

	// サイン波サンプル生成 (全チャンネル同位相)。
	std::vector<int16_t> samples(static_cast<size_t>(total_samples) * channels);
	const double  step = 2.0 * k_pi * frequency_hz / static_cast<double>(sample_rate);
	const double  peak = 32767.0 * static_cast<double>(amplitude);
	for (uint32_t i = 0; i < total_samples; ++i) {
		const int16_t value = static_cast<int16_t>(std::sin(step * static_cast<double>(i)) * peak);
		for (uint32_t c = 0; c < channels; ++c) {
			samples[static_cast<size_t>(i) * channels + c] = value;
		}
	}

	// ソースボイス作成。
	IXAudio2SourceVoice* source_voice = nullptr;
	if (FAILED(m_impl->xaudio2->CreateSourceVoice(&source_voice, &wave_format))) {
		return false;
	}

	XAUDIO2_BUFFER buffer = {};
	buffer.AudioBytes = static_cast<UINT32>(samples.size() * sizeof(int16_t));
	buffer.pAudioData = reinterpret_cast<const BYTE*>(samples.data());
	buffer.Flags      = XAUDIO2_END_OF_STREAM;

	if (FAILED(source_voice->SubmitSourceBuffer(&buffer))) {
		source_voice->DestroyVoice();
		return false;
	}

	if (FAILED(source_voice->Start(0))) {
		source_voice->DestroyVoice();
		return false;
	}

	// 再生終了 (キュー枯渇) までポーリング待機。
	// XAudio2 はサンプルバッファをコピーせず参照するため、ここで samples を解放してはならない。
	XAUDIO2_VOICE_STATE state = {};
	for (;;) {
		source_voice->GetState(&state);
		if (state.BuffersQueued == 0) {
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	source_voice->DestroyVoice();
	return true;
}

void MeAudio::shutdown() {
	if (!m_impl) {
		return;
	}
	if (m_impl->mastering_voice) {
		m_impl->mastering_voice->DestroyVoice();
		m_impl->mastering_voice = nullptr;
	}
	m_impl->xaudio2.Reset();
	if (m_impl->com_initialized) {
		CoUninitialize();
	}
	delete m_impl;
	m_impl = nullptr;
}
