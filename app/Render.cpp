#include "Render.h"

// ESC キー監視のため Win32 API (GetAsyncKeyState) を使う。 stdin に依存しないので
// 別コンソール (D&D 等) から起動した場合も誤検出しにくい。
// std::min/max マクロ汚染を避けるため NOMINMAX + WIN32_LEAN_AND_MEAN を必須化。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "CliPrint.h"
#include "WaveWriter.h"
#include "audio/synth/opna/IOpnaChip.h"
#include "mml/MmlPlayer.h"

namespace app {

void sleep_ms(int ms) {
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void render_to_wav(const std::string& wav_path, double seconds,
                   uint32_t sample_rate, IOpnaChip& opna, mml::Player& player,
                   bool normalize) {
	const bool write_wav = !wav_path.empty();

	// バッファ粒度 = 1ms (48 frames @ 48kHz)。
	// ymfm のような「連続生成型」エンジンは generate(N) の N 内でレジスタ書き込みを
	// 反映できないため、 N を 1ms 程度まで下げないと MML の細かいノート (64分=15ms 等) が
	// 同一バッファに詰まって「ずっと鳴る」状態になる。 Legacy は 1 サンプル粒度で内部
	// 計算するため大きな N でも問題なかったが、 ymfm 切替で粒度合わせが必須に。
	constexpr uint32_t k_buf_frames = 48;
	std::vector<int16_t> buf(k_buf_frames * 2);

	const uint32_t total_frames = static_cast<uint32_t>(seconds * sample_rate);
	uint32_t       written      = 0;

	// 全 PCM をメモリに溜めて、 ピーク正規化してから書き出す。
	// FM master volume を REF と一致するレベルに下げた (1.5f) ので、 単体 WAV としては
	// ピークが低い (~0.3) ことが多い。 -3 dBFS (peak 0.707) を target に正規化することで、
	// 「単体WAV として聞きやすい音量」 を保ちつつ、 REF との相対バランスは維持する。
	std::vector<int16_t> all_pcm;
	if (write_wav) all_pcm.reserve(static_cast<size_t>(total_frames) * 2);

	player.prepare_offline();

	const double sec_per_buf = static_cast<double>(k_buf_frames) / sample_rate;

	while (written < total_frames) {
		opna.render(buf.data(), k_buf_frames, 2);
		if (write_wav) {
			all_pcm.insert(all_pcm.end(), buf.begin(), buf.begin() + k_buf_frames * 2);
		}
		written += k_buf_frames;
		player.step_offline_seconds(sec_per_buf);
	}

	player.finish_offline();

	if (write_wav) {
		// ピーク測定 → normalize to -3 dBFS (= 32767 * 0.707 ≒ 23170)
		int peak = 0;
		for (auto v : all_pcm) {
			int a = (v < 0) ? -v : v;
			if (a > peak) peak = a;
		}
		constexpr int k_target_peak = 23170;  // -3 dBFS
		float gain = 1.0f;
		// peak を -3 dBFS へ「上下両方向」 で正規化する。 master volume を上げて生 peak が
		// target を超える曲 (INT33 等) も target に揃え、 offline WAV を master volume から
		// 独立に保つ (旧実装は peak < target のブースト専用で、 大音量曲が target を
		// 超えたまま出力されていた)。
		if (normalize && peak > 1024) {
			gain = static_cast<float>(k_target_peak) / static_cast<float>(peak);
			for (auto& v : all_pcm) {
				int new_v = static_cast<int>(static_cast<float>(v) * gain);
				if (new_v >  32767) new_v =  32767;
				if (new_v < -32768) new_v = -32768;
				v = static_cast<int16_t>(new_v);
			}
		}

		WaveWriter ww;
		if (!ww.open(wav_path, sample_rate)) {
			cli_fprintf(stderr, "Cannot open WAV: %s\n", wav_path.c_str());
			return;
		}
		ww.write_frames(all_pcm.data(), static_cast<uint32_t>(all_pcm.size() / 2));
		ww.close();
		cli_printf("Wrote %u frames (%.2f sec) to %s (peak=%.3f, gain=%.2fx)\n",
		            written, seconds, wav_path.c_str(),
		            static_cast<double>(peak) / 32768.0, static_cast<double>(gain));
	}
}

void play_until_esc_or_finish(mml::Player& player) {
	player.play();
	// 起動直後の残留状態をクリアして偽陽性 (= 即停止) を防ぐ。
	(void)GetAsyncKeyState(VK_ESCAPE);
	while (player.is_playing()) {
		// 高位 bit (0x8000) が立っていれば「今この瞬間に押下中」。
		if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
			player.stop();
			break;
		}
		sleep_ms(50);
	}
}

void wait_until_esc_or_flag(const std::atomic<bool>& done) {
	// 起動直後の残留状態をクリアして偽陽性 (= 即停止) を防ぐ。
	(void)GetAsyncKeyState(VK_ESCAPE);
	while (!done.load(std::memory_order_relaxed)) {
		if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) break;
		sleep_ms(50);
	}
}

}  // namespace app
