#include "YmfmTests.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "CliPrint.h"
#include "WaveWriter.h"
#include "audio/synth/ymfm_chip/YmfmOpnaChip.h"

// ymfm 直接駆動用 (リサンプラを介さず ymfm::ym2608 を直接叩く)
#ifdef _MSC_VER
#pragma warning(push, 1)
#endif
#include "audio/synth/ymfm/ymfm.h"
#include "audio/synth/ymfm/ymfm_opn.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace app {

namespace {
class DirectIntf : public ymfm::ymfm_interface {};
}

int run_ymfm_direct() {
	DirectIntf intf;
	ymfm::ym2608 chip(intf);
	chip.set_fidelity(ymfm::OPN_FIDELITY_MIN);
	chip.reset();

	auto write0 = [&](uint8_t addr, uint8_t val) {
		chip.write_address(addr);
		chip.write_data(val);
	};
	// FM ch1 A4 セットアップ (--ymfm-test と同じ)
	write0(0xB0, 0x07);  // ALG=7, FB=0
	write0(0xB4, 0xC0);  // L+R
	// initializer_list 内の整数リテラルは int 推論されるため、 uint8_t で受けると
	// /W4 で C4244 (縮小変換警告) が出る。 ループ変数を int で受けてキャストで渡す。
	for (int a : { 0x30, 0x34, 0x38, 0x3C }) write0(static_cast<uint8_t>(a), 0x01);  // DT=0 MUL=1
	for (int a : { 0x40, 0x44, 0x48, 0x4C }) write0(static_cast<uint8_t>(a), 20);    // TL=20
	for (int a : { 0x50, 0x54, 0x58, 0x5C }) write0(static_cast<uint8_t>(a), 0x1F);  // AR=31
	for (int a : { 0x60, 0x64, 0x68, 0x6C }) write0(static_cast<uint8_t>(a), 10);    // D1R=10
	for (int a : { 0x70, 0x74, 0x78, 0x7C }) write0(static_cast<uint8_t>(a), 4);     // D2R=4
	for (int a : { 0x80, 0x84, 0x88, 0x8C }) write0(static_cast<uint8_t>(a), 0x2F);  // SL=2 RR=15
	// A4 = F=1039 block=4 (公式実機値)
	write0(0xA4, (4 << 3) | (1039 >> 8));  // 0x24
	write0(0xA0, 1039 & 0xFF);             // 0x0F

	const uint32_t sr = chip.sample_rate(
		static_cast<uint32_t>(YmfmOpnaChip::k_default_master_clock_hz));
	cli_printf("ymfm sample_rate = %u Hz\n", sr);

	WaveWriter ww;
	ww.open("ymfm_direct.wav", sr);
	const uint32_t total = sr * 1;  // 1 秒
	std::vector<ymfm::ym2608::output_data> ymfm_buf(total);
	write0(0x28, 0xF0);  // key on
	chip.generate(ymfm_buf.data(), total);
	// 16bit に変換 + write
	std::vector<int16_t> out(total * 2);
	for (uint32_t i = 0; i < total; ++i) {
		int32_t l = ymfm_buf[i].data[0];
		int32_t r = ymfm_buf[i].data[1];
		if (l > 32767) l = 32767; if (l < -32768) l = -32768;
		if (r > 32767) r = 32767; if (r < -32768) r = -32768;
		out[i*2]   = static_cast<int16_t>(l);
		out[i*2+1] = static_cast<int16_t>(r);
	}
	ww.write_frames(out.data(), total);
	ww.close();
	cli_printf("Wrote ymfm_direct.wav (1.0 sec @ %u Hz)\n", sr);
	return 0;
}

int run_ymfm_test() {
	YmfmOpnaChip y(48000, YmfmOpnaChip::k_default_master_clock_hz,
	               YmfmOpnaChip::Fidelity::Low);
	y.set_fm_volume(0.5f);
	y.set_psg_volume(0.0f);
	// ALG=7 (全 op carrier)、 FB=0
	y.write_reg(0, 0xB0, 0x07);
	y.write_reg(0, 0xB4, 0xC0);
	for (int addr : { 0x30, 0x34, 0x38, 0x3C }) y.write_reg(0, static_cast<uint8_t>(addr), 0x01);
	for (int addr : { 0x40, 0x44, 0x48, 0x4C }) y.write_reg(0, static_cast<uint8_t>(addr), 20);
	for (int addr : { 0x50, 0x54, 0x58, 0x5C }) y.write_reg(0, static_cast<uint8_t>(addr), 0x1F);
	for (int addr : { 0x60, 0x64, 0x68, 0x6C }) y.write_reg(0, static_cast<uint8_t>(addr), 10);
	for (int addr : { 0x70, 0x74, 0x78, 0x7C }) y.write_reg(0, static_cast<uint8_t>(addr), 4);
	for (int addr : { 0x80, 0x84, 0x88, 0x8C }) y.write_reg(0, static_cast<uint8_t>(addr), 0x2F);
	// A4 (440 Hz): block=3, F-Num=1038
	y.write_reg(0, 0xA4, (3 << 3) | (1038 >> 8));
	y.write_reg(0, 0xA0, 1038 & 0xFF);

	WaveWriter ww;
	ww.open("ymfm_test.wav", 48000);
	constexpr uint32_t k_buf = 960;
	std::vector<int16_t> buf(k_buf * 2);
	auto render = [&](uint32_t total_frames) {
		uint32_t done = 0;
		while (done < total_frames) {
			const uint32_t n = std::min(k_buf, total_frames - done);
			y.render(buf.data(), n, 2);
			ww.write_frames(buf.data(), n);
			done += n;
		}
	};
	for (int i = 0; i < 3; ++i) {
		y.write_reg(0, 0x28, 0xF0);  // key on
		render(24000);                // 0.5s 発音
		y.write_reg(0, 0x28, 0x00);  // key off
		render(24000);                // 0.5s 無音
	}
	ww.close();
	cli_printf("Wrote ymfm_test.wav (3.0 sec, FM ch1 A4 with key on/off cycles)\n");
	return 0;
}

int run_ymfm_ssg_test() {
	YmfmOpnaChip y(48000, YmfmOpnaChip::k_default_master_clock_hz,
	               YmfmOpnaChip::Fidelity::Low);
	y.set_fm_volume(0.0f);
	y.set_psg_volume(1.0f);
	// SSG ch A: 440 Hz @ master/4 = 1996800 Hz → TP = 1996800 / (16 * 440) ≒ 284
	y.write_reg(0, 0x00, 284 & 0xFF);
	y.write_reg(0, 0x01, (284 >> 8) & 0x0F);
	// Mixer: ch A tone enable (bit 0 = 0)、 noise disable (bit 3 = 1)、 他 ch disable
	y.write_reg(0, 0x07, 0x3E);
	y.write_reg(0, 0x08, 0);  // 初期 volume 0

	WaveWriter ww;
	ww.open("ymfm_ssg_test.wav", 48000);
	constexpr uint32_t k_buf = 480;
	std::vector<int16_t> buf(k_buf * 2);
	auto render = [&](uint32_t total_frames) {
		uint32_t done = 0;
		while (done < total_frames) {
			const uint32_t n = std::min(k_buf, total_frames - done);
			y.render(buf.data(), n, 2);
			ww.write_frames(buf.data(), n);
			done += n;
		}
	};
	for (int i = 0; i < 3; ++i) {
		y.write_reg(0, 0x08, 12);  // volume = 12
		render(24000);
		y.write_reg(0, 0x08, 0);   // volume = 0 (無音)
		render(24000);
	}
	ww.close();
	cli_printf("Wrote ymfm_ssg_test.wav (3.0 sec, SSG ch A 440Hz volume on/off cycles)\n");
	return 0;
}

}  // namespace app
