#pragma once
#if defined(SUPPORT_FMGEN)

#ifndef WIN_HEADERS_H
#define WIN_HEADERS_H

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <assert.h>

// [MMLPlayer 改変] fmgen_file.cpp が使う Windows 型を最小定義 (windows.h 非依存)。
// FileIO は標準 C stdio (fread/fwrite) ベースで、MAX_PATH (パスバッファ長) と
// DWORD (ローカル変数型) だけが Windows 由来。fmgen 各 .cpp のみが本ヘッダを include
// するため、windows.h を使う MeAudio 等とは衝突しない。
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
typedef unsigned long DWORD;

// [MMLPlayer 改変] windows.h 用の STRICT/WIN32_LEAN_AND_MEAN 定義と
// `#define max _MAX` / `#define min _MIN` を削除。
// fmgen 本体は Max/Min (misc.h の自前 inline、大文字) を使うため max/min マクロは
// 不要かつ未定義 _MAX/_MIN に展開されて危険。windows.h も include しないため不要。

#endif	// WIN_HEADERS_H

#endif	/* SUPPORT_FMGEN */

