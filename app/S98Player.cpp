#include "S98Player.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "CliPrint.h"
#include "WaveWriter.h"
#include "audio/synth/ymfm_chip/YmfmOpnaChip.h"

namespace app {

int run_play_s98(const char* in_path, const char* out_path) {
	std::FILE* fh = std::fopen(in_path, "rb");
	if (!fh) {
		cli_fprintf(stderr, "cannot open %s\n", in_path);
		return 1;
	}
	std::fseek(fh, 0, SEEK_END);
	const long file_size = std::ftell(fh);
	std::fseek(fh, 0, SEEK_SET);
	std::vector<uint8_t> data(file_size);
	std::fread(data.data(), 1, file_size, fh);
	std::fclose(fh);
	if (file_size < 0x80 || std::memcmp(data.data(), "S98", 3) != 0) {
		cli_fprintf(stderr, "not a valid S98 file\n");
		return 1;
	}

	// ヘッダ: timerinfo (numerator @ 0x04), timerinfo2 (denom @ 0x08)、 dumpdata (= 0x14)。
	auto rd32 = [&](size_t off) {
		return uint32_t(data[off])
		     | (uint32_t(data[off+1]) << 8)
		     | (uint32_t(data[off+2]) << 16)
		     | (uint32_t(data[off+3]) << 24);
	};
	const uint32_t timer_num = rd32(0x04);
	uint32_t       timer_den = rd32(0x08);
	if (timer_den == 0) timer_den = 1000;  // np21w default
	const uint32_t dumpdata    = rd32(0x14);
	const double   sec_per_tick = static_cast<double>(timer_num) / static_cast<double>(timer_den);
	cli_printf("[S98] %s: 1 tick = %.6f sec (= %u/%u)\n",
	            in_path, sec_per_tick, timer_num, timer_den);

	YmfmOpnaChip y(48000, YmfmOpnaChip::k_default_master_clock_hz,
	               YmfmOpnaChip::Fidelity::Low);
	// レンダ経路 (main.cpp) と同じ校正値に揃える。S98 再生はリファレンス試聴・
	// 比較用途なので、 ゲイン差で帯域バランスの偽差が出ないようにする。
	y.set_fm_volume(1.06f);
	y.set_psg_volume(0.55f);

	const char* wav_out = out_path ? out_path : "mine_s98.wav";
	WaveWriter ww;
	ww.open(wav_out, 48000);
	constexpr uint32_t k_buf = 48;
	std::vector<int16_t> buf(k_buf * 2);
	double pending_sec = 0.0;
	auto render_sec = [&](double sec) {
		pending_sec += sec;
		while (pending_sec >= k_buf / 48000.0) {
			y.render(buf.data(), k_buf, 2);
			ww.write_frames(buf.data(), k_buf);
			pending_sec -= k_buf / 48000.0;
		}
	};

	size_t   i           = dumpdata;
	uint64_t total_ticks = 0;
	while (i < data.size()) {
		const uint8_t op = data[i++];
		if (op == 0x00 || op == 0x01) {
			if (i + 1 >= data.size()) break;
			const uint8_t addr = data[i++], val = data[i++];
			y.write_reg(op == 0 ? 0 : 1, addr, val);
		} else if (op == 0xFF) {
			render_sec(sec_per_tick);
			total_ticks += 1;
		} else if (op == 0xFE) {
			uint64_t n = 0;
			int      shift = 0;
			while (i < data.size()) {
				const uint8_t b = data[i++];
				n |= uint64_t(b & 0x7F) << shift;
				if (!(b & 0x80)) break;
				shift += 7;
			}
			n += 2;
			render_sec(sec_per_tick * static_cast<double>(n));
			total_ticks += n;
		} else if (op == 0xFD) {
			break;
		} else {
			cli_fprintf(stderr, "[S98] unknown opcode 0x%02x at offset %zu\n", op, i - 1);
			break;
		}
	}
	// 末尾 0.5 秒の余韻
	render_sec(0.5);
	ww.close();
	cli_printf("[S98] decoded %llu ticks (%.2f sec), wrote %s\n",
	            static_cast<unsigned long long>(total_ticks),
	            static_cast<double>(total_ticks) * sec_per_tick,
	            wav_out);
	return 0;
}

}  // namespace app
