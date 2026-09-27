#pragma once
//
// OpnTables: 実機 OPN/OPNA 互換の log_sin LUT。
//
// 実機 OPN は sin を浮動小数点で計算するのではなく、以下のパイプラインで
// 出力を生成している:
//
//   1. Sin LUT (256 エントリ × 1/4 周期)、値は |sin(x)| の log 表現
//   2. EG/TL を log (dB) 領域で保持
//   3. 合成 = log|sin| + EG_atten + TL_atten (log 加算)
//   4. exp LUT で linear 化、sign を別管理
//
// この LUT 方式 (特に log_sin の特定の量子化) が OPN/OPNA 特有の
// 「電子音らしさ」「金属感」を生む。完全に浮動小数点で計算した sin × gain は
// 数学的に正しいが、OPN 量子化アーティファクトが出ない。
//
// 当ヘッダ:
//   - 1/4 周期 256 エントリの log_sin (dB 単位)
//   - 1/4 周期内の対称性で全 1 周期をカバー
//

#include <cstdint>

namespace opn {

// OPN 仕様の 256 エントリ。1/4 周期分。
constexpr int k_log_sin_size = 256;

// 「実質無音」とみなす dB 上限 (FmOperator の k_eg_silent_db と一致させる)。
constexpr float k_log_sin_silent_db = 96.0f;

// 1/4 周期の log_sin テーブルへのアクセサ。
// table[i] = -20 × log10(|sin(π/2 × (i + 0.5) / k_log_sin_size)|) [dB]
//   i=0 はゼロ交差近傍 (大きな dB)、i=255 はピーク近傍 (~0 dB)
//
// 量子化: 値は OPN 互換の 13-bit 相当 (1 step ≈ 0.012 dB) で丸めてある。
const float* get_log_sin_table_db();

}  // namespace opn
