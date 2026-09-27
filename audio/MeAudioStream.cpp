//
// MeAudioStream.cpp: ストリーミング再生の実装。
//
// バッファ戦略:
//   - リングバッファ N 段 (k_buffer_count)、各段 ~20ms (sample_rate / 50 フレーム)
//   - 全段を初期フィル → 全段を投入 → Start
//   - OnBufferEnd で free_buffers をインクリメントしワーカーに通知
//   - ワーカーは空きが出るたびにフィル & 投入 を繰り返す
//

#include "audio_internal.h"
#include "MeAudioStream.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace {
// バッファ段数とバッファ長の設計値。
// k_buffer_count = 3 は「再生中・投入待機・フィル中」を同時に持てる最小構成。
constexpr uint32_t k_buffer_count       = 3;
// 1 秒を 50 等分 = 20ms / バッファ。レイテンシと安定性の現実的な妥協点。
constexpr uint32_t k_buffers_per_second = 50;
}  // namespace

// ---- Impl --------------------------------------------------------------

struct MeAudioStream::Impl : public IXAudio2VoiceCallback {
	IXAudio2SourceVoice*             source_voice = nullptr;
	std::vector<std::vector<int16_t>> buffers;          // リングバッファ実体
	uint32_t                         sample_rate       = 0;
	uint32_t                         channels          = 0;
	uint32_t                         frames_per_buffer = 0;

	FillCallback                     fill_cb;

	std::thread                      worker;
	std::mutex                       mtx;
	std::condition_variable          cv;
	uint32_t                         free_buffers      = 0;  // ワーカーが埋めて良いバッファ数
	uint32_t                         next_buffer_index = 0;  // ワーカーが次に使うインデックス
	std::atomic<bool>                running{false};

	// ---- IXAudio2VoiceCallback (XAudio2 内部スレッドから呼ばれる) ----
	void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
	void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
	void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
	void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
	void STDMETHODCALLTYPE OnBufferEnd(void*) noexcept override {
		// 1 バッファ再生完了 → 空き 1 増 → ワーカーに通知。
		// XAudio2 の音声スレッドからの呼び出しなので最小限の処理に留める。
		{
			std::lock_guard<std::mutex> lk(mtx);
			++free_buffers;
		}
		cv.notify_one();
	}
	void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
	void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) noexcept override {}

	void worker_loop() {
		while (true) {
			uint32_t index = 0;
			{
				std::unique_lock<std::mutex> lk(mtx);
				cv.wait(lk, [&] { return !running.load() || free_buffers > 0; });
				if (!running.load()) {
					return;
				}
				--free_buffers;
				index = next_buffer_index;
				next_buffer_index = (next_buffer_index + 1) % k_buffer_count;
			}

			// フィル & 投入。ロック外で行うことで OnBufferEnd を阻害しない。
			fill_one_buffer(index);
		}
	}

	void fill_one_buffer(uint32_t index) {
		auto& buf = buffers[index];
		// 万一フィルコールバックが書き残しても無音化されるよう先にゼロクリア。
		std::fill(buf.begin(), buf.end(), int16_t{0});
		if (fill_cb) {
			fill_cb(buf.data(), frames_per_buffer, channels);
		}

		XAUDIO2_BUFFER xb = {};
		xb.AudioBytes = static_cast<UINT32>(buf.size() * sizeof(int16_t));
		xb.pAudioData = reinterpret_cast<const BYTE*>(buf.data());
		// XAudio2 はバッファをコピーせず参照する点に注意 (buf は Impl が所有し続ける)。
		source_voice->SubmitSourceBuffer(&xb);
	}
};

// ---- 公開メンバ -------------------------------------------------------

MeAudioStream::MeAudioStream() = default;

MeAudioStream::~MeAudioStream() {
	stop();
}

bool MeAudioStream::start(MeAudio& audio, FillCallback fill) {
	if (m_impl) {
		stop();
	}
	auto* audio_impl = audio.impl();
	if (!audio_impl || !audio_impl->xaudio2) {
		return false;
	}

	m_impl = new Impl();
	m_impl->sample_rate       = audio_impl->sample_rate;
	m_impl->channels          = audio_impl->channels;
	m_impl->frames_per_buffer = m_impl->sample_rate / k_buffers_per_second;
	m_impl->fill_cb           = std::move(fill);

	// バッファ確保 (ゼロ初期化)。
	m_impl->buffers.assign(
		k_buffer_count,
		std::vector<int16_t>(static_cast<size_t>(m_impl->frames_per_buffer) * m_impl->channels, 0));

	// フォーマット記述。
	WAVEFORMATEX wave_format = {};
	wave_format.wFormatTag      = WAVE_FORMAT_PCM;
	wave_format.nChannels       = static_cast<WORD>(m_impl->channels);
	wave_format.nSamplesPerSec  = m_impl->sample_rate;
	wave_format.wBitsPerSample  = 16;
	wave_format.nBlockAlign     = static_cast<WORD>((wave_format.nChannels * wave_format.wBitsPerSample) / 8);
	wave_format.nAvgBytesPerSec = wave_format.nSamplesPerSec * wave_format.nBlockAlign;
	wave_format.cbSize          = 0;

	// ソースボイス生成。コールバックとして Impl 自身を渡す。
	if (FAILED(audio_impl->xaudio2->CreateSourceVoice(
			&m_impl->source_voice,
			&wave_format,
			0,
			XAUDIO2_DEFAULT_FREQ_RATIO,
			m_impl))) {
		delete m_impl;
		m_impl = nullptr;
		return false;
	}

	// 全バッファを初期フィルして投入。これにより再生開始直後から音切れしない。
	for (uint32_t i = 0; i < k_buffer_count; ++i) {
		m_impl->fill_one_buffer(i);
	}
	m_impl->next_buffer_index = 0;
	m_impl->free_buffers      = 0;  // 全段投入済み = 空きなし

	m_impl->running.store(true);
	m_impl->worker = std::thread([impl = m_impl] { impl->worker_loop(); });

	if (FAILED(m_impl->source_voice->Start(0))) {
		stop();
		return false;
	}
	return true;
}

void MeAudioStream::stop() {
	if (!m_impl) {
		return;
	}

	// ワーカー停止: フラグ → 通知 → join。
	{
		std::lock_guard<std::mutex> lk(m_impl->mtx);
		m_impl->running.store(false);
	}
	m_impl->cv.notify_all();
	if (m_impl->worker.joinable()) {
		m_impl->worker.join();
	}

	// ボイス停止 & 破棄。DestroyVoice は処理完了までブロックするため、
	// この後コールバックは発火せず安全に Impl を解放できる。
	if (m_impl->source_voice) {
		m_impl->source_voice->Stop(0);
		m_impl->source_voice->FlushSourceBuffers();
		m_impl->source_voice->DestroyVoice();
		m_impl->source_voice = nullptr;
	}

	delete m_impl;
	m_impl = nullptr;
}

bool MeAudioStream::is_playing() const {
	return m_impl && m_impl->running.load();
}
