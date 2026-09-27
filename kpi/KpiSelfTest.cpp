//
// KpiSelfTest: KbMedia 本体なしで .kpi プラグインを end-to-end 検証するコンソール。
//
//   LoadLibrary → kmp_GetTestModule → Open → Render(PCM 非無音確認)
//   → SetPosition(0) → Close
//
// 使い方: KpiSelfTest [plugin.kpi] [sample.mml] [out.wav]
//   既定: plugin = "MMLPlayer.kpi", mml = "Materials/MMLSamples/AGM/AGM01.MML"
//   (リポジトリルートを cwd にして実行する想定)。
//   out.wav を指定すると、 プラグインを先頭から全曲レンダして WAV に書き出す
//   (1 秒のピーク確認では捉えられない曲中盤以降の挙動の検証用)。
//
// .kpi と本テストはビット数が一致している必要がある (同一ビルドディレクトリで生成)。
//

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <vector>

#include "kmp_pi.h"

int main(int argc, char* argv[]) {
	const char* plugin = (argc > 1) ? argv[1] : "MMLPlayer.kpi";
	const char* mml    = (argc > 2) ? argv[2]
	                                : "Materials/MMLSamples/AGM/AGM01.MML";
	const char* wav_out = (argc > 3) ? argv[3] : nullptr;

	HMODULE module = LoadLibraryA(plugin);
	if (!module) {
		std::printf("FAIL: LoadLibrary(%s) error=%lu\n", plugin, GetLastError());
		return 1;
	}

	auto get_module = reinterpret_cast<pfnGetKMPModule>(
		GetProcAddress(module, "kmp_GetTestModule"));
	if (!get_module) {
		std::printf("FAIL: GetProcAddress(kmp_GetTestModule) not found\n");
		FreeLibrary(module);
		return 1;
	}

	KMPMODULE* kmp = get_module();
	if (!kmp || !kmp->Open || !kmp->Render || !kmp->Close) {
		std::printf("FAIL: KMPMODULE incomplete\n");
		FreeLibrary(module);
		return 1;
	}
	std::printf("module: ver=%lu plugin=%lu desc=%s\n",
		kmp->dwVersion, kmp->dwPluginVersion,
		kmp->pszDescription ? kmp->pszDescription : "(null)");

	SOUNDINFO info;
	ZeroMemory(&info, sizeof(info));
	HKMP handle = kmp->Open(mml, &info);
	if (!handle) {
		std::printf("FAIL: Open(%s) returned NULL\n", mml);
		FreeLibrary(module);
		return 1;
	}
	std::printf("SOUNDINFO: %luHz %luch %lubit length=%lums seekable=%lu\n",
		info.dwSamplesPerSec, info.dwChannels, info.dwBitsPerSample,
		info.dwLength, info.dwSeekable);

	bool ok = (info.dwBitsPerSample == 16 && info.dwChannels == 2 &&
	           info.dwSamplesPerSec == 48000 && info.dwLength > 0);

	// 約 1 秒分を 100ms ずつレンダしてピークを測る (無音でないことの確認)。
	const uint32_t bytes_per_frame = info.dwChannels * (info.dwBitsPerSample / 8);
	const uint32_t chunk_frames    = info.dwSamplesPerSec / 10;
	std::vector<uint8_t> buf(static_cast<size_t>(chunk_frames) * bytes_per_frame);
	int      peak        = 0;
	uint64_t total_bytes = 0;
	for (int i = 0; i < 10; ++i) {
		DWORD got = kmp->Render(handle, buf.data(),
		                        static_cast<DWORD>(buf.size()));
		total_bytes += got;
		const int16_t* samples = reinterpret_cast<const int16_t*>(buf.data());
		const uint32_t count = got / 2;  // int16 サンプル数
		for (uint32_t k = 0; k < count; ++k) {
			int a = samples[k] < 0 ? -samples[k] : samples[k];
			if (a > peak) peak = a;
		}
		if (got < buf.size()) break;  // 曲終了
	}
	std::printf("rendered %llu bytes, peak=%d\n",
		static_cast<unsigned long long>(total_bytes), peak);
	if (peak <= 0) {
		std::printf("FAIL: silent output\n");
		ok = false;
	}

	// シーク往復が落ちないことだけ確認。
	if (kmp->SetPosition) {
		DWORD pos = kmp->SetPosition(handle, 0);
		std::printf("SetPosition(0) -> %lu\n", pos);
	}

	// out.wav 指定時: 先頭へ巻き戻して全曲をレンダし WAV へ書き出す。
	if (wav_out) {
		if (kmp->SetPosition) kmp->SetPosition(handle, 0);
		std::vector<uint8_t> pcm;
		// 曲終了は Render が要求未満を返した時点で検出する。 safety は暴走防止の
		// バックストップなので、 プラグインが申告した曲長 (dwLength) + 5 秒を上限に取る。
		const uint64_t safety_frames =
			static_cast<uint64_t>(info.dwSamplesPerSec) *
			(static_cast<uint64_t>(info.dwLength) / 1000 + 5);
		uint64_t frames_done = 0;
		while (frames_done < safety_frames) {
			DWORD got = kmp->Render(handle, buf.data(),
			                        static_cast<DWORD>(buf.size()));
			pcm.insert(pcm.end(), buf.begin(), buf.begin() + got);
			frames_done += got / bytes_per_frame;
			if (got < buf.size()) break;  // 曲終了 (要求未満 = それ以上 PCM 無し)
		}

		FILE* fp = std::fopen(wav_out, "wb");
		if (fp) {
			const uint32_t data_bytes = static_cast<uint32_t>(pcm.size());
			const uint32_t rate       = info.dwSamplesPerSec;
			const uint16_t channels   = static_cast<uint16_t>(info.dwChannels);
			const uint16_t bits       = static_cast<uint16_t>(info.dwBitsPerSample);
			const uint16_t block_align = static_cast<uint16_t>(channels * (bits / 8));
			const uint32_t byte_rate   = rate * block_align;
			const uint32_t riff_size   = 36 + data_bytes;
			auto put32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, fp); };
			auto put16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, fp); };
			std::fwrite("RIFF", 1, 4, fp); put32(riff_size); std::fwrite("WAVE", 1, 4, fp);
			std::fwrite("fmt ", 1, 4, fp); put32(16); put16(1); put16(channels);
			put32(rate); put32(byte_rate); put16(block_align); put16(bits);
			std::fwrite("data", 1, 4, fp); put32(data_bytes);
			std::fwrite(pcm.data(), 1, data_bytes, fp);
			std::fclose(fp);
			std::printf("wav: wrote %s (%llu frames)\n", wav_out,
				static_cast<unsigned long long>(frames_done));
		} else {
			std::printf("wav: cannot open %s for write\n", wav_out);
		}
	}

	kmp->Close(handle);
	FreeLibrary(module);

	std::printf("%s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
