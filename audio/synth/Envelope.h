#pragma once
//
// Envelope: ADSR 線形エンベロープ。
//
// 状態機械:
//   Idle  --note_on--> Attack
//   Attack: m_value を 0→1 へ a 秒で線形上昇 → Decay
//   Decay : m_value を 1→S へ d 秒で線形減衰 → Sustain
//   Sustain: m_value = S を維持 (note_off まで)
//   *  --note_off--> Release
//   Release: m_value を 1.0/r の速度で減衰 → 0 になったら Idle
//
// Release は「ピーク 1.0 から 0 まで r 秒」の絶対速度として定義する
// (= OPNA の RR と同じ流儀)。Sustain 値が低ければ release は早く 0 に到達する。
//
// 注:
//   - パラメータ setter は任意のスレッドから呼べる (atomic)。
//   - note_on / note_off / next はレンダースレッド専用。
//   - Phase 2 では線形のみ。指数カーブが必要になったら派生 or テーブル化で対応。
//   - PSG ハードウェアエンベロープ・FM EG はここではなく別系統で実装する予定。
//

#include <atomic>
#include <cstdint>

class Envelope {
public:
	enum class State {
		Idle,
		Attack,
		Decay,
		Sustain,
		Release,
	};

	Envelope() = default;

	// ---- パラメータ設定 (任意スレッド) ----
	void set_attack(float seconds);          // 0 で即時最大
	void set_decay(float seconds);           // 0 で即時 sustain へ
	void set_sustain(float level_0_to_1);    // 持続レベル
	void set_release(float seconds);         // 0 で即時 0
	void set_adsr(float attack, float decay, float sustain, float release);

	// ---- レンダースレッド専用 ----
	void note_on();   // Attack へ遷移 (m_value はそのままで再アタック可)
	void note_off();  // Release へ遷移 (Idle は無視)

	// 1 サンプル進めて現在ゲイン [0..1] を返す。
	float next(uint32_t sample_rate);

	State state() const { return m_state; }
	bool  is_idle() const { return m_state == State::Idle; }

private:
	std::atomic<float> m_attack_sec{0.01f};
	std::atomic<float> m_decay_sec{0.10f};
	std::atomic<float> m_sustain_level{0.7f};
	std::atomic<float> m_release_sec{0.20f};

	State m_state = State::Idle;
	float m_value = 0.0f;  // 現在のゲイン
};
