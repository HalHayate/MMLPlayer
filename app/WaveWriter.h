#pragma once
//
// WaveWriter: 16bit PCM ステレオの簡易 WAV ライタ。
//
// 用途: オフライン WAV 出力 (--render / --play-effect / --play-s98) と
// デバッグツール (--debug ymfm-test 等) で共通利用。
// ヘッダは close() 時に書き直して total size を反映する遅延書き戻し方式。
//

#include <cstdint>
#include <fstream>
#include <string>

namespace app {

class WaveWriter {
public:
	bool open(const std::string& path, uint32_t sample_rate);
	void write_frames(const int16_t* samples, uint32_t frame_count);
	void close();

private:
	std::ofstream m_ofs;
	uint32_t      m_sample_rate = 48000;
	uint32_t      m_data_bytes  = 0;
};

}  // namespace app
