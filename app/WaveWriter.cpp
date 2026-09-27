#include "WaveWriter.h"

#include <cstdint>

namespace app {

bool WaveWriter::open(const std::string& path, uint32_t sample_rate) {
	m_sample_rate = sample_rate;
	m_ofs.open(path, std::ios::binary);
	if (!m_ofs) return false;
	// 44 byte のプレースホルダ。 close 時に書き直す。
	uint8_t hdr[44] = {};
	m_ofs.write(reinterpret_cast<const char*>(hdr), 44);
	m_data_bytes = 0;
	return true;
}

void WaveWriter::write_frames(const int16_t* samples, uint32_t frame_count) {
	const uint32_t bytes = frame_count * 2 * static_cast<uint32_t>(sizeof(int16_t));
	m_ofs.write(reinterpret_cast<const char*>(samples), bytes);
	m_data_bytes += bytes;
}

void WaveWriter::close() {
	m_ofs.seekp(0);
	const uint32_t fmt_chunk_size = 16;
	const uint16_t audio_format   = 1;     // PCM
	const uint16_t channels       = 2;
	const uint32_t byte_rate      = m_sample_rate * channels * 2;
	const uint16_t block_align    = channels * 2;
	const uint16_t bits           = 16;
	const uint32_t riff_size      = 36 + m_data_bytes;

	auto put = [&](const void* p, size_t n) {
		m_ofs.write(static_cast<const char*>(p), n);
	};
	put("RIFF", 4);
	put(&riff_size, 4);
	put("WAVE", 4);
	put("fmt ", 4);
	put(&fmt_chunk_size, 4);
	put(&audio_format, 2);
	put(&channels, 2);
	put(&m_sample_rate, 4);
	put(&byte_rate, 4);
	put(&block_align, 2);
	put(&bits, 2);
	put("data", 4);
	put(&m_data_bytes, 4);
	m_ofs.close();
}

}  // namespace app
