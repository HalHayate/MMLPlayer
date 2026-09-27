//
// CliPrint 実装: UTF-8 → CP932 (Shift_JIS) 変換つきの printf ラッパ。
// windows.h を含むため、 規約に従いこの .cpp のみで API を完結させる。
//

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include "CliPrint.h"

#include <string>
#include <vector>

namespace app {

namespace {

// UTF-8 → UTF-16 → CP932 と二段変換。
// CP932 で表現できない文字は WideCharToMultiByte の既定置換 ('?') に任せる。
std::string utf8_to_cp932(const char* utf8, int byte_len) {
	if (!utf8 || byte_len <= 0) return {};
	const int wchar_len = ::MultiByteToWideChar(CP_UTF8, 0, utf8, byte_len, nullptr, 0);
	if (wchar_len <= 0) {
		// 変換失敗時は原文をそのまま返す (バイナリ崩しを避ける)。
		return std::string(utf8, static_cast<size_t>(byte_len));
	}
	std::wstring wide(static_cast<size_t>(wchar_len), L'\0');
	::MultiByteToWideChar(CP_UTF8, 0, utf8, byte_len, wide.data(), wchar_len);

	const int ansi_len = ::WideCharToMultiByte(932, 0, wide.data(), wchar_len,
	                                            nullptr, 0, nullptr, nullptr);
	if (ansi_len <= 0) return std::string(utf8, static_cast<size_t>(byte_len));
	std::string ansi(static_cast<size_t>(ansi_len), '\0');
	::WideCharToMultiByte(932, 0, wide.data(), wchar_len,
	                       ansi.data(), ansi_len, nullptr, nullptr);
	return ansi;
}

}  // namespace

int cli_vfprintf(std::FILE* fp, const char* fmt, std::va_list args) {
	// 必要バッファサイズを算出 (vsnprintf は終端 NUL を含まないバイト数を返す)。
	std::va_list args_copy;
	va_copy(args_copy, args);
	const int needed = std::vsnprintf(nullptr, 0, fmt, args_copy);
	va_end(args_copy);
	if (needed < 0) return needed;

	std::vector<char> buf(static_cast<size_t>(needed) + 1);
	std::vsnprintf(buf.data(), buf.size(), fmt, args);

	const std::string cp932 = utf8_to_cp932(buf.data(), needed);
	std::fwrite(cp932.data(), 1, cp932.size(), fp);
	return needed;
}

int cli_printf(const char* fmt, ...) {
	std::va_list args;
	va_start(args, fmt);
	const int r = cli_vfprintf(stdout, fmt, args);
	va_end(args);
	return r;
}

int cli_fprintf(std::FILE* fp, const char* fmt, ...) {
	std::va_list args;
	va_start(args, fmt);
	const int r = cli_vfprintf(fp, fmt, args);
	va_end(args);
	return r;
}

}  // namespace app
