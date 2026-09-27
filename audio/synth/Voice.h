#pragma once
//
// Voice: 1 つの「発音単位」。
//
// 内部に
//   - 1 本のジェネレータ (波形ソース)
//   - 1 本の Envelope (ADSR)
// を持ち、外部からは
//   - 周波数 (Hz)
//   - 振幅   (0..1, ベロシティ相当)
//   - note_on / note_off
// をスレッド安全に制御できる。
//
// レンダースレッドで「アクティブな間だけ」サンプルを生成する。
// アクティブとは Envelope が Idle でない期間 (= Attack..Release 中)。
//
// 設計メモ:
//   ジェネレータ・エンベロープの状態はレンダースレッドからしか触らない。
//   外部スレッド (UI / シーケンサ) は atomic 経由で「目標値」とイベント要求を
//   置くだけで、レンダー側で取り込んでジェネレータ/エンベロープに反映する。
//
// 将来:
//   パン、ピッチベンド、LFO。FM チャンネル化時は Voice をサブクラス化して
//   4 オペレータ + アルゴリズム選択を束ねる。
//

#include <atomic>
#include <cstdint>
#include <memory>

#include "Envelope.h"
#include "IWaveGenerator.h"

class Voice {
public:
	Voice(uint32_t sample_rate, std::unique_ptr<IWaveGenerator> generator);
	~Voice();

	Voice(const Voice&)            = delete;
	Voice& operator=(const Voice&) = delete;

	// ---- 任意のスレッドから呼べる API ----
	void set_frequency(double hz);
	void set_amplitude(float amp);   // ベロシティ相当 (envelope と乗算)
	void note_on();                  // Envelope を Attack へ
	void note_off();                 // Envelope を Release へ

	// 発音中か (Envelope が Idle でない)。レンダー側からのみ意味のある値。
	bool is_active() const;

	// ADSR 制御は Envelope を直接触る形に統一 (コピー先を増やさない)。
	Envelope& envelope() { return m_envelope; }

	// ---- レンダースレッド専用 ----
	float next();

private:
	uint32_t                        m_sample_rate;
	std::unique_ptr<IWaveGenerator> m_generator;
	Envelope                        m_envelope;

	std::atomic<double> m_frequency_hz{440.0};
	std::atomic<float>  m_amplitude{0.0f};
	std::atomic<bool>   m_note_on_request{false};
	std::atomic<bool>   m_note_off_request{false};

	double m_cached_frequency = -1.0;  // レンダースレッド専用
};
