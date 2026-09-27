#pragma once
#if defined(SUPPORT_FMGEN)

#if !defined(win32_types_h)
#define win32_types_h

// [MMLPlayer 改変] np21w の compiler.h 依存を削除。
// fmgen は本ファイルで全型を自前定義しており、compiler.h の型 (UINT/SINT/BRESULT 等)
// は実際には使っていない (ケース感度検索で確認)。移植性のため include を外す。

#define __stdcall
#define HANDLE void *

typedef unsigned char uchar;
typedef unsigned short ushort;
typedef unsigned int uint;
typedef unsigned long ulong;

typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int  uint32;

typedef signed char sint8;
typedef signed short sint16;
typedef signed int sint32;

typedef signed char int8;
typedef signed short int16;
typedef signed int int32;

#endif // win32_types_h

#endif	/* SUPPORT_FMGEN */

