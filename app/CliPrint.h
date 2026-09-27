#pragma once
//
// CliPrint: CLI (cmd / PowerShell) 向け日本語出力ラッパ。
//
// ソースは UTF-8 で書かれているが、 Windows コンソールの既定コードページは
// CP932 (Shift_JIS) のため、 std::printf にそのまま渡すと化ける。
// このヘッダ経由で出力すると UTF-8 → CP932 変換を挟んで書き出す。
//
// 利用方針: ユーザに見せる stdout / stderr メッセージは全てこちら経由にする。
// 内部ログやファイル書き込み (snprintf 等) は対象外。
//
// 公開 API は <cstdio> 由来の FILE* / 可変引数のみ。 windows.h は cpp 側に閉じる。
//

#include <cstdarg>
#include <cstdio>

namespace app {

// printf 相当 (stdout)。 UTF-8 の fmt と %s 引数を CP932 に変換して書き出す。
// 戻り値は printf と同様 (フォーマット後のバイト数 / 失敗時負値)。
int cli_printf(const char* fmt, ...);

// fprintf 相当。 fp は stdout / stderr を想定。
int cli_fprintf(std::FILE* fp, const char* fmt, ...);

// va_list 版 (上記が内部的に呼ぶ)。 外からも利用可。
int cli_vfprintf(std::FILE* fp, const char* fmt, std::va_list args);

}  // namespace app
