#pragma once
//
// MeAudioStream: 連続再生 (ストリーミング) 用の音声出力ストリーム。
//
// MeAudio が保持する XAudio2 エンジン上にソースボイスを 1 本生成し、
// リングバッファ + ワーカースレッドで FillCallback を回しながら鳴らし続ける。
//
// 公開ヘッダなので windows.h / xaudio2.h は流入させない。
//

#include <cstdint>
#include <functional>

class MeAudio;

class MeAudioStream {
public:
	// フィルコールバック: ワーカースレッドから呼ばれ、frames フレーム分の
	// インターリーブド 16bit PCM を dst に書き込む。
	//   dst       : サンプル書き込み先 (サイズ = frames * channels)
	//   frames    : このコールバックで埋めるべきフレーム数
	//   channels  : チャンネル数
	using FillCallback = std::function<void(int16_t* dst, uint32_t frames, uint32_t channels)>;

	struct Impl;

	MeAudioStream();
	~MeAudioStream();

	MeAudioStream(const MeAudioStream&) = delete;
	MeAudioStream& operator=(const MeAudioStream&) = delete;

	// audio が initialize 済みであること。fill は再生中保持されるため、
	// キャプチャしたオブジェクトの寿命に注意。
	// 戻り値: 成功 true / 失敗 false。
	bool start(MeAudio& audio, FillCallback fill);

	// 再生を停止しソースボイスを破棄する。多重呼び出し可。
	void stop();

	// 現在再生中か。
	bool is_playing() const;

private:
	Impl* m_impl = nullptr;
};
