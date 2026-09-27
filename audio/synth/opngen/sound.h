#pragma once
// [MMLPlayer 追加] np21w sound.h 最小シム。opngen/psggen が使う SOUNDCALL と
// SOUNDCB のみ提供する (本物はサウンドストリーム管理を含むが移植では不要)。
#include "compiler.h"

#ifndef SOUNDCALL
#define SOUNDCALL
#endif

typedef void (SOUNDCALL * SOUNDCB)(void *hdl, SINT32 *pcm, UINT count);
