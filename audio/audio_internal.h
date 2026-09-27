#pragma once
//
// audio_internal.h: audio/ モジュール **専用** のプライベートヘッダ。
//
// 重要:
//   このヘッダは windows.h / xaudio2.h を引き込むため、
//   audio/*.cpp 以外からは include しないこと。
//   公開ヘッダ (MeAudio.h / MeAudioStream.h) には絶対に流入させない。
//

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xaudio2.h>
#include <wrl/client.h>

// windows.h の GetCurrentTime マクロが Main 層と衝突するため除去。
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include "MeAudio.h"

// MeAudio の実体定義。MeAudioStream など同モジュール内の他 TU から参照される。
struct MeAudio::Impl {
	Microsoft::WRL::ComPtr<IXAudio2> xaudio2;
	IXAudio2MasteringVoice*          mastering_voice = nullptr;  // xaudio2 が所有
	uint32_t                         sample_rate     = 0;
	uint32_t                         channels        = 0;
	bool                             com_initialized = false;
};
