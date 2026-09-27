#pragma once
//
// [MMLPlayer 追加] np21w の compiler.h 最小シム。
// opngen / psggen (audio/synth/opngen の np21w ソース) が必要とする型・マクロだけを
// 提供する。本物の np21w compiler.h はプラットフォーム依存で重いため、移植に必要な
// 最小限 (型、REG8、LOADINTELWORD、min/max、ZeroMemory) のみを定義する。
//
#include <string.h>   /* memset (C/C++ 両対応) */

typedef signed int     SINT;
typedef unsigned int   UINT;
typedef signed char    SINT8;
typedef unsigned char  UINT8;
typedef signed short   SINT16;
typedef unsigned short UINT16;
typedef signed int     SINT32;
typedef unsigned int   UINT32;

typedef UINT8 REG8;

#ifndef BRESULT
#define BRESULT UINT
#endif

typedef signed char BOOL;
#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif

// 2 バイト配列をリトルエンディアン 16bit として読む (psggenc.c の reg.tune 等)。
#ifndef LOADINTELWORD
#define LOADINTELWORD(a) ((UINT16)((a)[0] | ((a)[1] << 8)))
#endif

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef ZeroMemory
#define ZeroMemory(d, n) memset((d), 0, (n))
#endif

// np21w のデバッグトレース。リリースでは空。TRACEOUT((fmt, ...)) 形式で呼ばれる。
#ifndef TRACEOUT
#define TRACEOUT(arg) ((void)0)
#endif
