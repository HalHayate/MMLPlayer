//
// OpnTables.cpp: log_sin テーブル生成と提供。
//

#include "OpnTables.h"

#include <cmath>

namespace opn {

namespace {

// テーブル生成。0..255 のインデックスは 1/4 周期内のサンプル位置。
// 値は -20 × log10(|sin(...)|) を 13-bit 整数に丸めて再 dB 化したもの。
//
// なぜ 13-bit 量子化:
//   実機 OPN は 13-bit log_sin (符号なし) で値を保持する。1 step ≈ 0.0939 dB
//   実装上は 16-bit unsigned に左寄せして格納するが、ここでは float のまま
//   13-bit 量子化された値を使い、量子化アーティファクトだけ残す。
struct LogSinTable {
	float data[k_log_sin_size];

	LogSinTable() {
		// 13-bit 量子化用のステップサイズ (実機準拠の概算: max 96dB / 8192 ≈ 0.0117 dB)
		constexpr float k_step_db = 96.0f / 8192.0f;
		const double pi_half = 3.14159265358979323846 / 2.0;
		for (int i = 0; i < k_log_sin_size; ++i) {
			// (i + 0.5) で「セルの中央」サンプリング。OPN 実機もそう。
			const double angle = pi_half * (static_cast<double>(i) + 0.5) /
			                     static_cast<double>(k_log_sin_size);
			const double s = std::sin(angle);
			float db;
			if (s <= 0.0) {
				db = k_log_sin_silent_db;
			} else {
				db = -20.0f * std::log10(static_cast<float>(s));
				if (db < 0.0f) db = 0.0f;  // 念のため
				if (db > k_log_sin_silent_db) db = k_log_sin_silent_db;
				// 13-bit 量子化: 0..8192 step → 元の dB に戻す
				const int   q   = static_cast<int>(db / k_step_db + 0.5f);
				const float qd  = static_cast<float>(q) * k_step_db;
				db = qd;
			}
			data[i] = db;
		}
	}
};

const LogSinTable& table() {
	static const LogSinTable t;
	return t;
}

}  // namespace

const float* get_log_sin_table_db() {
	return table().data;
}

}  // namespace opn
