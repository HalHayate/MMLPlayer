#pragma once
//
// FmChannel: OPN/OPNA 系 4-op FM チャンネル。
//
// 4 つの FmOperator を保持し、8 種類の接続パターン (アルゴリズム) と
// op1 へのフィードバックを実装する。実機 OPNA の 1 ch 相当。
//
// アルゴリズム表 (carrier = 出力に寄与する op):
//   0: 1→2→3→4                (carrier: 4)
//   1: (1+2)→3→4              (carrier: 4)
//   2: 2→3, (1+3)→4           (carrier: 4)
//   3: 1→2, (2+3)→4           (carrier: 4)
//   4: 1→2, 3→4               (carrier: 2,4)
//   5: 1→{2,3,4}              (carrier: 2,3,4)
//   6: 1→2, 3, 4              (carrier: 2,3,4)
//   7: 1+2+3+4                (carrier: 1,2,3,4)
//
// フィードバック:
//   op1 の出力を「前 2 サンプルの平均」に FB スケールを掛けて
//   op1 自身の mod_input に再投入する (実機準拠の 1 サンプル遅延ループ安定化)。
//   FB=0 でフィードバックなし。FB=1..7 で 1/128 → 1/2 の段階的スケール。
//
// 周波数:
//   実機 OPN は 1 ch につき 1 つの F-Number/Block (= base 周波数)。
//   個別の音程差は op ごとの MUL/DT で表現する。
//   set_frequency() は全 op に同じ base_hz を伝搬する。
//

#include <atomic>
#include <cstdint>
#include <memory>

#include "FmOperator.h"

class FmChannel {
public:
	static constexpr int k_op_count       = 4;
	static constexpr int k_algorithm_count = 8;
	static constexpr int k_feedback_max   = 7;

	explicit FmChannel(uint32_t sample_rate);
	~FmChannel();

	FmChannel(const FmChannel&)            = delete;
	FmChannel& operator=(const FmChannel&) = delete;

	// ---- 任意スレッドから呼べる API ----
	void set_frequency(double base_hz);     // 全 op に伝搬
	void set_algorithm(uint8_t alg_3bit);   // 0..7
	void set_feedback(uint8_t fb_3bit);     // 0..7、op1 のみに作用

	// per-channel ソフトウェア LFO (PMS/AMS とは別系統、MUSIC.COM の SOUND の LFO: 用)。
	// WF=0..3 (矩形/鋸/三角/ワンショット)、SPEED=0..100 (0 で無効)、
	// DEPTH=-4095..4095 (0 で無効)。
	void set_lfo(uint8_t waveform, uint8_t speed, int16_t depth);

	// 実機 OPNA の PMS (Phase Modulation Sensitivity, 0..7) と AMS (Amplitude Modulation
	// Sensitivity, 0..3)。チップ全体 LFO 値に対して、ピッチ/振幅をどれだけ揺らすかを決める。
	// MUSIC.COM ドライバはチップ LFO を使わないため AGM01/COP02 では 0 のまま。
	// 実機準拠の構造として将来の拡張用に提供する。
	void set_pms(uint8_t pms_3bit);  // 0..7
	void set_ams(uint8_t ams_2bit);  // 0..3

	// レンダー直前に OpnaChip が「現在の チップ LFO 値」を渡すための setter。
	// 値は [-1, 1] の三角波相当。
	void update_chip_lfo_value(float lfo_value);

	void key_on();    // 全 op を Attack 状態へ
	void key_off();   // 全 op を Release 状態へ

	// op アクセス (個別 EG/MUL 設定用)。範囲外は assert / 未定義。
	FmOperator& op(int index);

	// ---- レンダースレッド専用 ----
	float next();

	bool is_idle() const;

private:
	std::unique_ptr<FmOperator> m_ops[k_op_count];

	std::atomic<uint8_t> m_algorithm{0};
	std::atomic<uint8_t> m_feedback{0};

	// per-channel ソフトウェア LFO 状態 (PMD 風、SOUND の LFO: コマンド由来)。
	std::atomic<uint8_t> m_lfo_waveform{2};   // 既定: 三角
	std::atomic<uint8_t> m_lfo_speed{0};      // 0 = 無効
	std::atomic<int16_t> m_lfo_depth{0};      // 0 = 無効
	uint32_t             m_sample_rate;        // LFO 周波数計算に必要
	double               m_lfo_phase = 0.0;    // [0, 1) レンダースレッド専用

	// 実機 OPNA PMS/AMS (チップ全体 LFO 用感度)。MUSIC.COM ドライバが使わないため
	// 当プレイヤーでは常時 0、将来の拡張用構造として保持。
	std::atomic<uint8_t> m_pms{0};            // 0..7
	std::atomic<uint8_t> m_ams{0};            // 0..3
	float                m_chip_lfo_value = 0.0f;  // OpnaChip が per-sample で書き込む

	// フィードバック履歴 (レンダースレッド専用)。
	// 前 2 サンプルの平均で「1 サンプル遅延」によるループ安定化を行う。
	float m_op1_prev  = 0.0f;
	float m_op1_prev2 = 0.0f;
};
