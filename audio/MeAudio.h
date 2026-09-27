#pragma once
//
// MeAudio: XAudio2 をラップする音声出力クラス。
//
// 設計上の注意:
//   公開ヘッダには windows.h / xaudio2.h を絶対に流入させない。
//   D3D12 と同様、すべての実装詳細は MeAudio.cpp 内の Impl に隠蔽する。
//

#include <cstdint>

class MeAudio {
public:
	// 自由関数や別 TU から Impl* を扱える余地を残すため public 前方宣言。
	struct Impl;

	MeAudio();
	~MeAudio();

	// コピー禁止 (内部に COM オブジェクトを保持するため)。
	MeAudio(const MeAudio&) = delete;
	MeAudio& operator=(const MeAudio&) = delete;

	// XAudio2 を初期化しマスタリングボイスを作成する。
	// 戻り値: 成功 true / 失敗 false。
	bool initialize(uint32_t sample_rate = 48000, uint32_t channels = 2);

	// 指定周波数の正弦波を duration_ms ミリ秒だけ再生する (再生終了までブロック)。
	// amplitude は 0.0〜1.0 の範囲で振幅を指定。
	bool play_sine(double frequency_hz, uint32_t duration_ms, float amplitude = 0.3f);

	// XAudio2 を破棄。デストラクタからも呼ばれるため二重呼び出し可。
	void shutdown();

	// audio/ モジュール内部から Impl を参照するためのアクセサ。
	// 公開ヘッダ上は Impl は前方宣言のみで実体不可視のため、
	// 外部モジュールからは何も触れず、結果として漏れない。
	Impl* impl() const { return m_impl; }

private:
	Impl* m_impl = nullptr;
};
