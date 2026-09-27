#pragma once
// [MMLPlayer 追加] np21w parts.h 最小シム。psggeng.c が include するが
// 使うのは呼び出し規約マクロ PARTSCALL (空) 程度。
#include "compiler.h"

#ifndef PARTSCALL
#define PARTSCALL
#endif
