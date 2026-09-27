#pragma once
//
// mml::Player: パース済み Score を OpnaChip に再生する。
//
// 動作モデル:
//   - load() で Score をコピー保持 (短いので OK)。
//   - play() でワーカースレッド開始。
//   - ワーカーは ~5ms 周期で起き、現在 tick を計算しイベントを消化する。
//   - 各 Note は key_on を即発行し、ev.time + ev.gate に key_off をスケジュール。
//   - 全イベント消化 + key_off 全消化で終了。
//
// チャンネル割当:
//   MML 1..3 → OpnaChip FM 0..2
//   MML 4..6 → OpnaChip PSG 0..2
//
// デフォルト音色:
//   Phase 8 では SOUND: ブロックを未パースのため、
//   set_default_fm_preset() で渡される FmPreset を全 FM ch に適用する。
//   @ コマンドは状態として記録するが音色変更は行わない (Phase 9)。
//

#include <atomic>
#include <cstdint>
#include <thread>

#include "audio/synth/opna/IOpnaChip.h"
#include "MmlScore.h"
#include "MmlTimbre.h"

namespace mml {

class SoundDat;  // 前方宣言 (mml/SoundDat.h)

// 簡易ブラス系プリセットを返す。
FmPreset default_brass_preset();

class Player {
public:
	explicit Player(IOpnaChip& opna);
	~Player();

	Player(const Player&)            = delete;
	Player& operator=(const Player&) = delete;

	// スコアを内部にコピー。再生中なら自動 stop。
	void load(const Score& score);

	// デフォルト音色を設定。Phase 8 では全 FM ch に同じものを適用する。
	void set_default_fm_preset(const FmPreset& preset);

	// SOUND.DAT (効果音テーブル) を Player に渡す。 D パート再生時に参照される。
	// nullptr で解除。 ポインタ所有はしないので、 呼び出し側が Player より長生きさせる。
	void set_sound_dat(const SoundDat* dat);

	// 再生開始 (非同期)。
	void play();

	// 再生停止 (worker join)。多重呼び出し可。
	void stop();

	// 現在再生中か。
	bool is_playing() const { return m_running.load(); }

	// オフライン (決定論的) レンダリング用 API。
	// worker スレッドを起動せず、呼び出し側 (主に main の render_to_wav) が
	// `prepare_offline()` → ループで `OpnaChip::render(N frames)` →
	// `step_offline_seconds(N / sample_rate)` を反復する。CPU・wall-clock に
	// 依存しないため、同じ MML から常に同じ WAV が出る。
	// テンポ変動 (T コマンド) は内部で追跡され、tick 進行に反映される。
	void  prepare_offline();
	// 指定秒数だけ Player 内部時刻を進め、その間のイベント・key_off・effect 更新を行う。
	// 終了済みなら false を返す。
	bool  step_offline_seconds(double seconds);
	// オフライン状態を解除し、リソースを通常状態に戻す。
	void  finish_offline();
	// オフライン処理が全イベント消化済みか (FillCallback からのポーリング用)。
	bool  is_offline_finished() const { return m_offline_finished; }

private:
	void worker_loop();

	// 与えた tick で「key_off → イベント処理 → effect 更新 → 終了判定」を行う。
	// tempo_bpm は Tempo イベントで更新された場合に書き戻す (出力引数)。
	// 戻り値: 全 ch 消化 + key_off 完了で true。
	bool process_tick_common(int32_t tick_int, double& tempo_bpm);

	void apply_preset(int fm_ch, const FmPreset& preset);
	// 現在のプリセットを基準に V (0..15) を TL に反映する。
	void apply_fm_volume(int fm_ch);
	// ch3 特殊モード: 全 OP の独立 F-Number をレジスタに書く。
	// base_freq_hz: OP1 (= ノート周波数 + lfo_offset 相当)
	// lfo_offset_fnum: ベース fnum への加算量 (スイープ現在値)
	// ch3 専用: m_state[2] の lfo_dt2_* フィールドを内部で参照する。
	void write_ch3_special_fnum(double base_freq_hz, int lfo_offset_fnum, const FmPreset& preset);
	// 任意のボリューム値で TL を反映 (Tremolo の一時的な値を当てる用)。
	void apply_fm_volume_with(int fm_ch, int volume_0_15);

	void handle_event_fm(int fm_ch, const Event& ev);
	void handle_event_ssg(int psg_ch, const Event& ev);

	// LFO・ポルタメントの per-tick 更新。ch は 0..5 (0..2=FM, 3..5=SSG)。
	void update_effects(int ch, int32_t tick_int);

	static double semitone_to_hz(double semitone);

	IOpnaChip&      m_opna;
	Score           m_score;
	FmPreset        m_default_preset;
	const SoundDat* m_sound_dat = nullptr;

	// 現在再生中のテンポ (BPM × tempo_factor)。LFO tick 計算に使う。
	double m_current_tempo_bpm = 0.0;

	std::thread       m_worker;
	std::atomic<bool> m_running{false};

	// オフラインレンダ用状態 (worker を使わず step_offline_ticks で進む)。
	bool   m_offline_active     = false;
	double m_offline_tempo_bpm  = 0.0;
	double m_offline_current_tick = 0.0;
	int32_t m_offline_last_tick = -1;  // process_tick_common 処理済みの最終 tick
	bool   m_offline_finished   = false;

	enum class EffectMode : uint8_t {
		None,
		Vibrato,
		Tremolo,
		Portamento,
	};

	struct ChannelState {
		size_t  next_event           = 0;
		int32_t pending_key_off_tick = -1;
		int     volume               = 12;
		int     pitch_offset         = 0;
		int     current_timbre       = -1;  // -1 = 未指定 (play() のデフォルト適用状態)
		const FmPreset* current_preset = nullptr;
		// SSG ch のみ意味を持つ: ソフトウェアエンベロープ参照。
		// nullptr のとき (= @0 or 未定義) は V コマンドの音量がそのまま使われる。
		const SsgEnvelope* current_ssg_envelope = nullptr;

		// SSG ch のみ: HW (PSG) エンベロープ駆動モードか。
		// SSGENV と相互排他、後勝ちで切り替わる。
		// shape は最後に S で書かれた値 (-1 は未設定)。Note 毎にリトリガするため保持。
		bool ssg_use_hw_envelope = false;
		int  ssg_hw_envelope_shape = -1;

		// I/U/P 効果。相互排他で、後勝ちで上書き。
		EffectMode effect_mode      = EffectMode::None;
		int        effect_amp       = 0;
		int        effect_period_64 = 0;  // 64 分音符単位
		int        effect_delay_64  = 0;  // 同上

		// Note 状態 (LFO ・グライドの基準時刻と直前ピッチ)。
		int     current_note_semitone = -1;  // -1 = 未発音
		int     prev_note_semitone    = -1;  // ポルタメントの開始ピッチ
		int32_t note_on_tick          = -1;  // 現在ノートのキーオン時刻

		// 直近 OPNA に書き込んだ値 (差分検出で書き込み回数を抑える)。
		double last_applied_freq_hz = -1.0;
		int    last_applied_volume  = -1;

		// ch3 特殊モード + LFO WF=1 (のこぎり波) ピッチスイープ状態。
		// DT2 != 0 のOP を持つ音色が ch3 に割り当てられたときに使う。
		int     lfo_fnum_offset          = 0;  // OP4 の現在 F-Number オフセット (fnum 単位)
		int     lfo_step_fnum            = 0;  // 毎更新の減少量
		int32_t lfo_update_interval_ticks = 0;  // 更新間隔 (ticks)
		// DT2 スロットのキャッシュ値。キーオン時に make_dt2_fnum(op4_initial) で初期化し、
		// 以後スイープごとに同じ Δ=-step を直接加算する (re-derive しないことで Δ 誤差を防ぐ)。
		int  lfo_dt2_fnum        = 0;
		int  lfo_dt2_block       = 0;
		bool lfo_dt2_initialized = false;

		// 次の Note で TL リフレッシュ (TL=V0→TL=本来値の MUSIC.COM シーケンス) を行うか。
		// Volume/Timbre 変更時にセット、Note でクリア。
		// NP2 録音と一致: 最初のノートと音量・音色変更直後のノートのみ TL を書き直す。
		bool needs_tl_refresh = true;
	};
	ChannelState m_state[k_channel_count];

	// D パート (効果音) 再生状態。
	// SSG ch A/B (= ch 4/5) を SOUND.DAT 由来の段階列で占有する。 D パート使用時は
	// 通常メロディの ch 4/5 とは別経路で SSG レジスタを書く。
	struct EffectState {
		bool   active       = false;  // 効果音再生中フラグ
		int    effect_id    = -1;
		size_t stage_index  = 0;       // 現在 stage (= SoundDatEffect::stages のインデックス)
		int    frames_left  = 0;       // 現在 stage の残りフレーム数 (p1)
		int    frames_elapsed = 0;     // 現在 stage 開始からの経過フレーム数 (各 seq の dur 判定用)
		// 各シーケンスの累積値 (8.8 固定小数点)。 step を毎フレーム加算してから上位を書き込む。
		int32_t tone_f[2]   = {0, 0};  // 16.0 (周期そのもの)
		int32_t vol_v_8_8[2] = {0, 0};
		int32_t noise_n_8_8 = 0;
	};
	EffectState m_effect_state;
	size_t      m_d_part_next_event = 0;
	// 効果音フレームの分数部 (60Hz の整数フレームに丸める前に蓄積)。
	double      m_effect_frame_accum = 0.0;

	// D パート再生関連。
	void trigger_effect(int effect_id);
	void update_effect_frames(int frames);
	void apply_effect_stage_initial();
	void release_effect_channels();
	void handle_d_part_events(int32_t tick_int);
};

}  // namespace mml
