//
// mml::Player.cpp: Score → OpnaChip 駆動シーケンサ。
//

#include "MmlPlayer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include "SoundDat.h"

namespace mml {

namespace {

// アルゴリズム別のキャリア op テーブル (true = キャリア = 出力に寄与)。
constexpr bool k_carrier_table[8][4] = {
	{ false, false, false, true  },  // ALG 0: op4
	{ false, false, false, true  },  // ALG 1: op4
	{ false, false, false, true  },  // ALG 2: op4
	{ false, false, false, true  },  // ALG 3: op4
	{ false, true,  false, true  },  // ALG 4: op2, op4
	{ false, true,  true,  true  },  // ALG 5: op2, op3, op4
	{ false, true,  true,  true  },  // ALG 6: op2, op3, op4
	{ true,  true,  true,  true  },  // ALG 7: op1..op4 全部
};

// V (0..16) → キャリア op に加算する TL 減衰 dB。
// MUSIC.COM ドライバ仕様: TL 加算量 = (16 - V) * 4 (TL レジスタ単位 0..127、1 step = 0.75 dB)
// musiccom_emu.py の V スイープ実験 (VTL16.MML) で全 V 値・複数 ALG にて実証:
//   V16→0, V15→4, V14→8, ... V0→64 (= 厳密に (16-V)*4)。V16 が最大音量。
// V17 以上は実機で TL=127 (無音) になるが、 通常 MML では出現しないため非対応。
// db_to_tl で / 0.75 される前提なので、ここでは × 0.75 して dB に換算してから返す。
float volume_to_extra_tl_db(int volume_0_16) {
	if (volume_0_16 < 0)  volume_0_16 = 0;
	if (volume_0_16 > 16) volume_0_16 = 16;
	const float tl_units = (16.0f - static_cast<float>(volume_0_16)) * 4.0f;
	return tl_units * 0.75f;
}

// Hz → F-Number + block 変換 (YM2608 fclk=7987200 Hz 基準)。
// block を決めて fnum が [1, 1023] に収まるよう調整。
static std::pair<int,int> hz_to_fnum_block(double freq_hz) {
	constexpr double fclk = 7987200.0;
	// block=0 相当の生 fnum
	double fnum_d = freq_hz * 144.0 * 2097152.0 / fclk;
	int block = 0;
	while (fnum_d > 1023.5 && block < 7) { fnum_d /= 2.0; ++block; }
	while (fnum_d < 0.5    && block > 0)  { fnum_d *= 2.0; --block; }
	return { std::max(1, std::min(1023, static_cast<int>(fnum_d + 0.5))), block };
}

}  // namespace

// ---- デフォルトプリセット --------------------------------------------

FmPreset default_brass_preset() {
	// Phase 5 デモで使った ALG 4 ブラス系を流用。
	FmPreset p;
	p.algorithm = 4;
	p.feedback  = 0;
	// op1: ペア1 mod
	p.ops[0] = { 0.020f, 0.30f, 0.7f, 2.0f, 0.10f, 12.0f, 1, 0.0f };
	// op2: ペア1 carrier
	p.ops[1] = { 0.020f, 0.30f, 0.8f, 3.0f, 0.10f,  0.0f, 1, 0.0f };
	// op3: ペア2 mod
	p.ops[2] = { 0.030f, 0.40f, 0.6f, 2.5f, 0.10f, 18.0f, 2, 0.0f };
	// op4: ペア2 carrier
	p.ops[3] = { 0.020f, 0.40f, 0.7f, 3.0f, 0.10f,  6.0f, 1, 0.0f };
	return p;
}

// ---- ライフサイクル ---------------------------------------------------

Player::Player(IOpnaChip& opna)
	: m_opna(opna)
	, m_default_preset(default_brass_preset()) {
}

Player::~Player() {
	stop();
}

void Player::load(const Score& score) {
	if (m_running.load()) {
		stop();
	}
	// 自然終了後の未 join スレッドを掃除。
	if (m_worker.joinable()) {
		m_worker.join();
	}
	m_score = score;
	for (int ch = 0; ch < k_channel_count; ++ch) {
		m_state[ch] = ChannelState{};
	}
	// D パートの Tempo イベントを ch1 に移植 (既存 Tempo 処理経路に乗せる)。
	// 移植後 d_part からは Tempo を削除する。
	if (!m_score.d_part.empty()) {
		auto& d = m_score.d_part;
		for (const auto& ev : d) {
			if (ev.kind == EventKind::Tempo) m_score.channels[0].push_back(ev);
		}
		d.erase(std::remove_if(d.begin(), d.end(),
			[](const Event& e) { return e.kind == EventKind::Tempo; }), d.end());
		// ch1 を時刻順に再ソート。 同 tick のイベント順序 (Volume→Timbre→Volume→
		// Vibrato→Note 等の MML 並び) は維持する必要があるため stable_sort。
		// 不安定ソートだと、 例えば「Timbre が Note より後ろに回って」 V0→op→V0 が
		// V0→Vx→KEY ON より後に処理され、 過渡音 (op 書き換え中に carrier Vx) になる。
		std::stable_sort(m_score.channels[0].begin(), m_score.channels[0].end(),
			[](const Event& a, const Event& b) { return a.time_ticks < b.time_ticks; });

		// 注: D パートがあっても SSG ch A/B の音楽イベントは削除しない。
		// musiccom_emu (BGM34) の実測で、 MUSIC.COM は ch4/5 の音楽を通常通り演奏し、
		// 効果音再生中だけ 60Hz フレーム書き込みが一時的に上書きすると判明。
		// (旧実装の「D パート使用時は ch4/5 を占有して音楽を無視」 は誤解釈で、
		//  BGM34 では SSG メロディ 2ch が丸ごと消えていた。)
	}
	m_effect_state       = EffectState{};
	m_d_part_next_event  = 0;
	m_effect_frame_accum = 0.0;
}

void Player::set_default_fm_preset(const FmPreset& preset) {
	m_default_preset = preset;
}

void Player::set_sound_dat(const SoundDat* dat) {
	m_sound_dat = dat;
}

void Player::play() {
	if (m_running.load()) return;

	// 前回のワーカーが自然終了している場合、std::thread はまだ joinable のまま。
	// ここで join しておかないと、後の m_worker = std::thread(...) 代入で
	// std::terminate() が呼ばれてしまう。
	if (m_worker.joinable()) {
		m_worker.join();
	}

	// 全 FM ch にデフォルト音色を適用 (Timbre イベントが来るまでの初期状態)。
	// パン配置は OpnaChip コンストラクタの既定 (ch1=Both/ch2=L/ch3=R) に任せる。
	for (int i = 0; i < k_fm_channel_count; ++i) {
		m_state[i].current_preset = nullptr;  // = デフォルト使用中
		apply_preset(i, m_default_preset);
		apply_fm_volume(i);
	}
	// 全 SSG ch を初期化 (MUSIC.COM 仕様: トーンは disable のまま、キーオンで enable)。
	m_opna.psg_reset();
	for (int i = 0; i < k_fm_channel_count; ++i) {
		m_opna.set_psg_mixer(i, /*tone*/false, /*noise*/false);
		m_opna.set_psg_channel_volume(i, 0);
	}

	m_current_tempo_bpm = music_com_tempo_to_bpm(k_default_tempo_value);
	m_running.store(true);
	m_worker = std::thread([this] { worker_loop(); });
}

void Player::stop() {
	if (!m_running.load() && !m_worker.joinable()) return;
	m_running.store(false);
	if (m_worker.joinable()) {
		m_worker.join();
	}
	// 念のため全 ch を消音。
	for (int i = 0; i < k_fm_channel_count; ++i) {
		m_opna.fm_key_off(i);
		m_opna.set_psg_channel_volume(i, 0);
	}
}

// ---- ch3 特殊モード F-Number 書き込み --------------------------------

// ch3 特殊モードの全 OP (MML OP1..4) に独立 F-Number を書く。
// MML OP 番号と YM2608 ch3-special レジスタの対応:
//   MML OP1 (ops[0]) → slot1 → 0xA9/0xAD
//   MML OP2 (ops[1]) → slot3 → 0xAA/0xAE
//   MML OP3 (ops[2]) → slot2 → 0xA8/0xAC  ← DT2 が設定されている OP
//   MML OP4 (ops[3]) → slot4 → 0xA2/0xA6 (メイン、通常の ch3 F-Number と共用)
// DT2=1 の OP は base の完全 5 度上 (× 3/2) = block+1, fnum×3/4 で設定する。
void Player::write_ch3_special_fnum(double base_freq_hz, int lfo_offset_fnum,
                                    const FmPreset& preset) {
	auto& st = m_state[2];
	auto [base_fnum, base_block] = hz_to_fnum_block(base_freq_hz);
	// lfo_offset を加算 (block 変化が必要な場合を考慮)。
	int fnum  = base_fnum + lfo_offset_fnum;
	int block = base_block;
	if (fnum > 1023 && block < 7) { fnum >>= 1; ++block; }
	if (fnum < 1)   fnum = 1;
	if (fnum > 1023) fnum = 1023;

	// DT2=1 の OP 用 F-Number。MUSIC.COM 実測 (musiccom_emu S98) では
	// 「fnum − 186, block + 1」の固定変換 (キック 736→550, スネア 693→507)。
	// 旧実装の ×3/4 はキック起点では +3 fnum 程度だがスネアで +14 fnum ずれる。
	// 初回のみ OP4 初期値から derive してキャッシュ。以後は st.lfo_dt2_fnum を直接使う。
	// 毎回 re-derive するとスイープ Δ が圧縮 (-60→-45) される問題を回避する。
	const auto make_dt2_fnum = [](int f, int b) -> std::pair<int,int> {
		int f2 = f - 186;
		int b2 = b + 1;
		if (f2 > 1023 && b2 < 7) { f2 >>= 1; ++b2; }
		if (f2 < 1)   f2 = 1;
		if (f2 > 1023) f2 = 1023;
		if (b2 > 7)   b2 = 7;
		return {f2, b2};
	};
	if (!st.lfo_dt2_initialized) {
		std::tie(st.lfo_dt2_fnum, st.lfo_dt2_block) = make_dt2_fnum(fnum, block);
		st.lfo_dt2_initialized = true;
	}

	// ch3-special レジスタアドレス [lo, hi] (MML OP1..4 順)。
	constexpr uint8_t k_ch3_lo[4] = {0xA9, 0xAA, 0xA8, 0xA2};
	constexpr uint8_t k_ch3_hi[4] = {0xAD, 0xAE, 0xAC, 0xA6};

	for (int i = 0; i < 4; ++i) {
		int wf = fnum, wb = block;
		if (preset.ops[i].dt2 == 1) { wf = st.lfo_dt2_fnum; wb = st.lfo_dt2_block; }
		const auto hi = static_cast<uint8_t>((wb << 3) | ((wf >> 8) & 0x07));
		const auto lo = static_cast<uint8_t>(wf & 0xFF);
		m_opna.write_reg(0, k_ch3_hi[i], hi);
		m_opna.write_reg(0, k_ch3_lo[i], lo);
	}
}

// ---- プリセット / 音量 ----------------------------------------------

void Player::apply_preset(int fm_ch, const FmPreset& p) {
	// ALG/FB は 1 回でまとめて書く (個別だと中間値が出る。 ドラム ch の高速音色変更で
	// 顕著だった。 [[battle_brass_fm_core]] とは別件の MAP_SPR ch3 調査で発覚)。
	m_opna.set_fm_algorithm_feedback(fm_ch, p.algorithm, p.feedback);
	// 注: MML SOUND ブロックに LFO 行があっても、 実機 MUSIC.COM ドライバは
	// チップ全体 LFO (R0x22) を一度も書かない (NP2 録音 Materials/MGS42.s98 で確認)。
	// LFO を ON にすると音色が一律に「動きのある力強い」 感じになり、 実機の「平坦で
	// ぼんやりした」 音とずれる。 PMS/AMS は B4 レジスタ側で ch ごとに付与する設計
	// ([[opna_chip_lfo_unused]])。 ここでは LFO は書かない。
	// ch3 かつ DT2 設定あり → ch3 特殊モード (各 OP 独立 F-Number) を有効化。
	// NP2 録音: 0x27 bit6 = 1 でch3特殊モード、他 bit は timer 関連 (0x7a が実機値)。
	if (fm_ch == 2 && p.has_dt2) {
		m_opna.write_reg(0, 0x27, 0x40);
	}
	for (int i = 0; i < 4; ++i) {
		const auto& src = p.ops[i];
		m_opna.set_fm_op_attack_rate   (fm_ch, i, src.ar_sec);
		m_opna.set_fm_op_decay1_rate   (fm_ch, i, src.d1r_sec);
		m_opna.set_fm_op_sustain_level (fm_ch, i, src.sl);
		m_opna.set_fm_op_decay2_rate   (fm_ch, i, src.d2r_sec);
		m_opna.set_fm_op_release_rate  (fm_ch, i, src.rr_sec);
		m_opna.set_fm_op_total_level_db(fm_ch, i, src.tl_db);
		m_opna.set_fm_op_multiplier    (fm_ch, i, src.mul);
		m_opna.set_fm_op_detune_cents  (fm_ch, i, src.detune);
		m_opna.set_fm_op_key_scaling   (fm_ch, i, src.ks);
	}
}

void Player::apply_fm_volume(int fm_ch) {
	apply_fm_volume_with(fm_ch, m_state[fm_ch].volume);
}

void Player::apply_fm_volume_with(int fm_ch, int volume_0_15) {
	auto& st = m_state[fm_ch];
	const FmPreset& preset = st.current_preset ? *st.current_preset : m_default_preset;
	const float    extra   = volume_to_extra_tl_db(volume_0_15);
	const uint8_t  alg     = preset.algorithm & 0x07;
	// MUSIC.COM ドライバは carrier の TL のみ書き換える (modulator は音色構造維持)。
	for (int i = 0; i < 4; ++i) {
		if (!k_carrier_table[alg][i]) continue;
		const float tl = preset.ops[i].tl_db + extra;
		m_opna.set_fm_op_total_level_db(fm_ch, i, tl);
	}
}

// ---- 周波数 ----------------------------------------------------------

double Player::semitone_to_hz(double semitone) {
	return 440.0 * std::pow(2.0, (semitone - 69.0) / 12.0);
}

// ---- イベントハンドラ -----------------------------------------------

void Player::handle_event_fm(int fm_ch, const Event& ev) {
	auto& st = m_state[fm_ch];

	switch (ev.kind) {
	case EventKind::Note: {
		// 直前ノートのピッチを記録 (ポルタメントの開始ピッチ用)。
		if (st.current_note_semitone >= 0) {
			st.prev_note_semitone = st.current_note_semitone;
		}
		st.current_note_semitone = ev.a;
		st.note_on_tick          = ev.time_ticks;
		const bool tied = (ev.flags & k_event_flag_tied_from_prev) != 0;
		if (!tied) {
			// 通常ノート: MUSIC.COM 風キーオン手順 (NP2 録音と一致):
			//   - volume/音色が変わった「初めて」のときだけ TL=V0→TL=本来値 シーケンス。
			//   - 毎ノート: KEY OFF → KEY ON → F-Number 書込み。
			if (st.needs_tl_refresh) {
				apply_fm_volume_with(fm_ch, 0);
				apply_fm_volume_with(fm_ch, st.volume);
				st.last_applied_volume = st.volume;
				st.needs_tl_refresh = false;
			}
			m_opna.fm_key_off(fm_ch);
			m_opna.fm_key_on(fm_ch);
			// ch3 特殊モード + LFO WF=1: キーオン時にピッチスイープ初期化。
			const FmPreset* pr = st.current_preset ? st.current_preset : &m_default_preset;
			if (fm_ch == 2 && pr->has_dt2 && pr->lfo_waveform == 1 && pr->lfo_depth > 0) {
				const int D = pr->lfo_depth;
				const int speed_ms = (pr->lfo_speed > 0) ? pr->lfo_speed : 100;
				const double ticks_per_ms =
					m_current_tempo_bpm * k_ticks_per_quarter_note / 60000.0;
				st.lfo_update_interval_ticks =
					std::max(1, static_cast<int32_t>(speed_ms * ticks_per_ms + 0.5));
				// step = 8*sqrt(DEPTH) - DEPTH/5 (BOSS.MML 実測から導いた近似式)。
				// sqrt を int に切り捨てると D=1200 (スネア) で 32 になってしまう
				// (実測 38、浮動小数+四捨五入で 37)。double で計算して丸める。
				const int step = std::max(0, static_cast<int>(
					8.0 * std::sqrt(static_cast<double>(D)) - static_cast<double>(D) / 5.0 + 0.5));
				st.lfo_step_fnum     = step;
				st.lfo_fnum_offset   = step * 2;  // 初期 = 2 × step
				st.lfo_dt2_initialized = false;  // 初回 write_ch3_special_fnum で計算
			} else if (fm_ch == 2) {
				st.lfo_fnum_offset     = 0;
				st.lfo_step_fnum       = 0;
				st.lfo_dt2_initialized = false;
			}
		}
		// スラー (tied) でも F-Number は新しい音程で書き直す。
		st.last_applied_freq_hz = -1.0;
		update_effects(fm_ch, ev.time_ticks);
		// pending_key_off_tick: 通常ノートは ev.c で更新、スラー時は累積分を加算
		// (前ノートの key_off タイミングを後ろにずらして音を続ける)。
		if (tied && st.pending_key_off_tick >= 0) {
			st.pending_key_off_tick = ev.time_ticks + ev.c;
		} else {
			st.pending_key_off_tick = ev.time_ticks + ev.c;
		}
		break;
	}
	case EventKind::Volume:
		// FM の volume はドライバ内部状態のみ更新。次の Note (キーオン) 時に
		// MUSIC.COM 風シーケンス (TL=V0 相当 → TL=本来値) で反映する。
		if (ev.a != st.volume) {
			st.volume = ev.a;
			st.needs_tl_refresh = true;
		}
		break;
	case EventKind::Timbre: {
		// 同じ音色番号への再指定は無視する (マクロ展開で連発される)。
		if (st.current_timbre == ev.a) break;
		st.current_timbre = ev.a;
		// MML の @番号 に対応する SOUND: プリセットを Score から探して適用する。
		const auto it = m_score.fm_presets.find(ev.a);
		if (it != m_score.fm_presets.end()) {
			st.current_preset = &it->second;
			// MUSIC.COM 風 op 書き換え手順 (NP2 録音 Materials/MGS42.s98 922ms と一致):
			//   1. carrier TL を V0 (= サイレント) で書く → 鳴ってる音を一瞬無音化
			//   2. 全 op レジスタを書き直す (preset の TL も書かれる)
			//   3. carrier TL を再度 V0 で上書き (preset TL を消す)
			//   4. needs_tl_refresh で次のノート時に V0 → Vx で本来音量に戻す
			// この手順で全 op の DT/MUL/TL/AR/D1R/SL/D2R/RR/KS を書き換えても、
			// carrier がミュートされているため過渡音 (ブツッ) は出ない。
			apply_fm_volume_with(fm_ch, 0);
			apply_preset(fm_ch, it->second);
			apply_fm_volume_with(fm_ch, 0);
		} else {
			// 未定義の @番号: デフォルトのまま (記録だけ)。
			st.current_preset = nullptr;
		}
		// 音色変更後の carrier TL は次の Note で V0 → Vx シーケンスで書く。
		st.needs_tl_refresh = true;
		break;
	}
	case EventKind::PitchOffset:
		st.pitch_offset = ev.a;
		st.last_applied_freq_hz = -1.0;
		break;
	case EventKind::Vibrato:
		st.effect_mode      = (ev.a == 0 && ev.b == 0) ? EffectMode::None : EffectMode::Vibrato;
		st.effect_amp       = ev.a;
		st.effect_period_64 = ev.b;
		st.effect_delay_64  = ev.c;
		st.last_applied_freq_hz = -1.0;
		break;
	case EventKind::Tremolo:
		st.effect_mode      = (ev.a == 0 && ev.b == 0) ? EffectMode::None : EffectMode::Tremolo;
		st.effect_amp       = ev.a;
		st.effect_period_64 = ev.b;
		st.effect_delay_64  = ev.c;
		st.last_applied_volume = -1;
		break;
	case EventKind::Portamento:
		// P0 で解除、P>0 で有効化。amp フィールドにグライド時間を入れておく。
		st.effect_mode = (ev.a == 0) ? EffectMode::None : EffectMode::Portamento;
		st.effect_amp  = ev.a;
		break;
	case EventKind::Tempo:
		// Tempo はワーカーループ側で処理する (グローバル状態)。
		break;
	case EventKind::YRegWrite:
		// 直接レジスタ書き込み (MUSIC.COM Y コマンド)。 port 0 想定。
		m_opna.write_reg(0, static_cast<uint8_t>(ev.a), static_cast<uint8_t>(ev.b));
		// Y コマンドで FM 音色パラメータ (TL 等) が直接変更されうるため、
		// 直後の @ コマンドを強制再適用させる (同音色番号スキップを防ぐ)。
		st.current_timbre = -1;
		break;
	case EventKind::GateTime:
	case EventKind::Rest:
	case EventKind::EffectTrigger:
		// EffectTrigger は D パート専用で FM ch では発生しない。
		break;
	}
}

void Player::handle_event_ssg(int psg_ch, const Event& ev) {
	const int ch_idx = psg_ch + k_fm_channel_count;
	auto& st  = m_state[ch_idx];

	switch (ev.kind) {
	case EventKind::Note: {
		// FM 同様に prev/current を追跡してポルタメント基準にする。
		if (st.current_note_semitone >= 0) {
			st.prev_note_semitone = st.current_note_semitone;
		}
		st.current_note_semitone = ev.a;
		st.note_on_tick          = ev.time_ticks;
		const bool tied = (ev.flags & k_event_flag_tied_from_prev) != 0;
		if (!tied) {
			// MUSIC.COM の SSG キーオン手順 (musiccom_emu ground truth で確認):
			//   1. mixer tone disable → enable の二段書き (リトリガ。常に両方書く)
			//   2. tone freq + volume (update_effects で)
			// 旧実装の volume=0 サイレント化書き込みは MUSIC.COM には存在しない。
			m_opna.set_psg_mixer(psg_ch, /*tone*/false, /*noise*/false);
			m_opna.set_psg_mixer(psg_ch, /*tone*/true, /*noise*/false);
			if (st.ssg_use_hw_envelope && st.ssg_hw_envelope_shape >= 0) {
				m_opna.set_psg_envelope_shape(static_cast<uint8_t>(st.ssg_hw_envelope_shape));
			}
		}
		// スラー時も F-Number は書き直す (ピッチ変更だけ実行)。
		st.last_applied_freq_hz  = -1.0;
		st.last_applied_volume   = -1;
		update_effects(ch_idx, ev.time_ticks);
		st.pending_key_off_tick = ev.time_ticks + ev.c;
		break;
	}
	case EventKind::Volume:
		st.volume = ev.a;
		st.last_applied_volume = -1;
		// 鳴っている最中なら即時反映 (Tremolo の base になる)。
		// SSG は 4bit なので V16 は 15 にクランプ (bit4 = HW env enable の誤設定回避)。
		if (st.pending_key_off_tick >= 0) {
			m_opna.set_psg_channel_volume(psg_ch, static_cast<uint8_t>(std::min(ev.a, 15)));
		}
		break;
	case EventKind::Timbre:
		st.current_timbre = ev.a;
		// MUSIC.COM 仕様: SSG ch では @0 で SW/HW 両エンベロープ解除。
		if (ev.a == 0) {
			st.current_ssg_envelope = nullptr;
			st.ssg_use_hw_envelope  = false;
		} else {
			const auto it = m_score.ssg_envelopes.find(ev.a);
			if (it != m_score.ssg_envelopes.end()) {
				// SW envelope モードへ (HW envelope は排他で OFF)。
				st.current_ssg_envelope = &it->second;
				st.ssg_use_hw_envelope  = false;
			} else {
				st.current_ssg_envelope = nullptr;
			}
		}
		st.last_applied_volume = -1;  // エンベロープ起源で次サンプル更新
		break;
	case EventKind::PitchOffset:
		st.pitch_offset = ev.a;
		st.last_applied_freq_hz = -1.0;
		break;
	case EventKind::Vibrato:
		st.effect_mode      = (ev.a == 0 && ev.b == 0) ? EffectMode::None : EffectMode::Vibrato;
		st.effect_amp       = ev.a;
		st.effect_period_64 = ev.b;
		st.effect_delay_64  = ev.c;
		st.last_applied_freq_hz = -1.0;
		break;
	case EventKind::Tremolo:
		st.effect_mode      = (ev.a == 0 && ev.b == 0) ? EffectMode::None : EffectMode::Tremolo;
		st.effect_amp       = ev.a;
		st.effect_period_64 = ev.b;
		st.effect_delay_64  = ev.c;
		st.last_applied_volume = -1;
		break;
	case EventKind::Portamento:
		st.effect_mode = (ev.a == 0) ? EffectMode::None : EffectMode::Portamento;
		st.effect_amp  = ev.a;
		break;
	case EventKind::SsgHwEnvelopeShape:
		// HW envelope モードに切り替え (SW envelope は排他で OFF)。
		st.ssg_use_hw_envelope    = true;
		st.ssg_hw_envelope_shape  = ev.a & 0x0F;
		st.current_ssg_envelope   = nullptr;
		// シェイプ書き込みでエンベロープが再起動する (PsgChip 内で処理)。
		m_opna.set_psg_envelope_shape(static_cast<uint8_t>(st.ssg_hw_envelope_shape));
		st.last_applied_volume = -1;
		break;
	case EventKind::SsgHwEnvelopePeriod:
		// 周期はチップ全体で 1 つだが、最後に書いた ch の値が適用される。
		// SW envelope モードでも書き込んでは害がないので、モード判定は省略。
		m_opna.set_psg_envelope_period(static_cast<uint16_t>(ev.a));
		break;
	case EventKind::YRegWrite:
		// 直接レジスタ書き込み (MUSIC.COM Y コマンド)。 port 0 想定。
		m_opna.write_reg(0, static_cast<uint8_t>(ev.a), static_cast<uint8_t>(ev.b));
		break;
	case EventKind::Tempo:
	case EventKind::GateTime:
	case EventKind::Rest:
	case EventKind::EffectTrigger:
		// MUSIC.COM 仕様: SSG ch の R 休符では何もしない (tone disable も音量変更もなし)。
		// 検証経緯 (2026-05-10): fmgen 一般 PSG 仕様の「R7 tone disable」を試したところ
		// AGM01 で onset 22→43、peak count 325→75 と大幅悪化。MUSIC.COM ドライバは
		// SSGENV の volumes 列が時間経過で 0 に達する設計でフェードアウトを実現しており、
		// R 休符で tone disable しないのが正解と判明。
		break;
	}
}

// ---- LFO ・ポルタメントの per-tick 更新 ----------------------------

void Player::update_effects(int ch, int32_t tick_int) {
	auto& st = m_state[ch];
	if (st.note_on_tick < 0)            return;
	if (st.current_note_semitone < 0)   return;

	const int32_t since_on = tick_int - st.note_on_tick;
	if (since_on < 0) return;

	// ch3 特殊モード LFO WF=1 スイープ更新。
	// SPEED ms ごとに lfo_fnum_offset を減らして 0 へ収束させる。
	if (ch == 2 && st.lfo_step_fnum > 0 && st.lfo_fnum_offset > 0
	    && st.lfo_update_interval_ticks > 0 && since_on > 0
	    && (since_on % st.lfo_update_interval_ticks) == 0) {
		st.lfo_fnum_offset = std::max(0, st.lfo_fnum_offset - st.lfo_step_fnum);
		// DT2 スロットも同じ Δ=-step で更新 (REF S98 実測: 全スロット同一 Δfnum)。
		// re-derive (make_dt2_fnum を毎回呼ぶ) するとΔが圧縮されて-45になるため、
		// 初回キャッシュした値に直接 step を引く。
		if (st.lfo_dt2_initialized) {
			st.lfo_dt2_fnum = std::max(0, st.lfo_dt2_fnum - st.lfo_step_fnum);
		}
		st.last_applied_freq_hz = -1.0;  // 次回 F-Number 再書き込みをトリガ
	}

	// 基準ピッチ (N コマンドの静的オフセット込み)。
	double semitone = static_cast<double>(st.current_note_semitone) +
	                  static_cast<double>(st.pitch_offset) / 255.0;
	int    volume   = st.volume;

	// 64 分音符 = 12 tick (k_ticks_per_whole_note=768 / 64)。
	constexpr int32_t k_64th_ticks = k_ticks_per_whole_note / 64;

	// SSG ch のソフトウェアエンベロープ。 MUSIC.COM 仕様 (NP2 machine.s98 解析で確定):
	//   output = clamp(env_v + V - 15, 0..15)
	// = env 値から「最大音量 V=15 からの減衰量 (15-V) を引く」 オフセット減算。
	// 旧実装は `env_v * V / 15` (乗算スケーリング) だったが、 V=8 等の中音量で
	// REF と異なる曲線になる (例: V=8, env=12 でours=6 / REF=5)。
	// V=14 等の高音量域ではほぼ一致するため AGM01/SDM12 では露見せず、
	// MACHINE.MML (V8 + SSGENV @1) で初めて顕在化した。
	if (ch >= k_fm_channel_count && st.current_ssg_envelope) {
		const auto& env = *st.current_ssg_envelope;
		if (!env.volumes.empty() && env.period_64th > 0) {
			const int32_t period_ticks = env.period_64th * k_64th_ticks;
			if (period_ticks > 0) {
				size_t step = static_cast<size_t>(since_on / period_ticks);
				if (step >= env.volumes.size()) step = env.volumes.size() - 1;
				const int env_v = env.volumes[step];
				int out_v = env_v + st.volume - 15;
				if (out_v < 0)  out_v = 0;
				if (out_v > 15) out_v = 15;
				volume = out_v;
			}
		}
	}

	switch (st.effect_mode) {
	case EffectMode::Vibrato: {
		const int32_t delay_ticks = st.effect_delay_64 * k_64th_ticks;
		if (since_on >= delay_ticks) {
			// MUSIC.COM 仕様 (NP2 DEFEAT.s98 解析で確定): num2 は「半周期」を 64th-note
			// 単位で示す。 num2 × 64分音符 経過するごとに値が反転する矩形 LFO。
			// 例: I60,2,16 + T86 → 78ms ごとに F-Number が反転 (NP2 録音と一致)。
			// period=0 は「最高速 LFO」(= ピッチノイズ風) で打楽器音色用途 (I255,0,0)。
			const int32_t half_period = (st.effect_period_64 > 0)
				? (st.effect_period_64 * k_64th_ticks)
				: 1;
			const int32_t phase_idx = (since_on - delay_ticks) / half_period;
			const bool    high      = ((phase_idx & 1) == 0);
			const double  mod_units = high ? st.effect_amp : -st.effect_amp;
			semitone += mod_units / 255.0;
		}
		break;
	}

	case EffectMode::Tremolo: {
		const int32_t delay_ticks = st.effect_delay_64 * k_64th_ticks;
		if (since_on >= delay_ticks) {
			// Tremolo (U) も num2 = 半周期 × 64分音符 と仮定 (Vibrato と同形)。
			const int32_t half_period = (st.effect_period_64 > 0)
				? (st.effect_period_64 * k_64th_ticks)
				: 1;
			const int32_t phase_idx = (since_on - delay_ticks) / half_period;
			const bool    high      = ((phase_idx & 1) == 0);
			volume += high ? st.effect_amp : -st.effect_amp;
			if (volume < 0)  volume = 0;
			if (volume > 15) volume = 15;
		}
		break;
	}

	case EffectMode::Portamento:
		if (st.prev_note_semitone >= 0 && st.effect_amp > 0) {
			// MUSIC.COM 実機 (musiccom_emu S98) 実測: ポルタメントは P 個の 64分音符で
			// 目標に到達する (= fnum を毎 64分音符 (target-start)/P ずつ線形増減)。
			// 例: COP02 ch1 P170 で fnum -3/64分音符、ch2 P140 で -2/64分音符。
			// 旧 142/100 係数は NP2 録音の誤計測由来でグライドが遅すぎた。
			const int32_t dur_ticks = st.effect_amp * k_64th_ticks;
			if (dur_ticks > 0 && since_on < dur_ticks) {
				// 64 分音符単位で離散的に進める。NP2 録音と一致。
				const int32_t step = since_on / k_64th_ticks;
				const int32_t step_ticks = step * k_64th_ticks;
				const double progress = static_cast<double>(step_ticks) /
				                        static_cast<double>(dur_ticks);
				// MUSIC.COM は F-Number を「一定ステップ」で線形に増減させる
				// (COP02 の P170 を musiccom_emu S98 で実測: fnum が毎 64分音符 -3 ずつ)。
				// fnum はブロック内で周波数に比例するため、半音(対数)空間ではなく
				// 「周波数空間」で線形補間すると実機の一定ステップ軌道に一致する。
				const double f_from = semitone_to_hz(
					static_cast<double>(st.prev_note_semitone));
				const double f_to   = semitone_to_hz(semitone);
				const double f_lin  = f_from + (f_to - f_from) * progress;
				semitone = 69.0 + 12.0 * std::log2(f_lin / 440.0);
			}
		}
		break;

	case EffectMode::None:
	default:
		break;
	}

	// 周波数の差分書き込み (頻繁な atomic 書き込みを抑制)。
	const double freq = semitone_to_hz(semitone);
	if (std::abs(freq - st.last_applied_freq_hz) > 0.01) {
		if (ch < k_fm_channel_count) {
			// ch3 特殊モード: 通常の set_fm_frequency_hz ではなく直接レジスタに書く。
			const FmPreset* pr = (ch == 2 && st.current_preset) ? st.current_preset
			                   : (ch == 2) ? &m_default_preset : nullptr;
			if (ch == 2 && pr && pr->has_dt2) {
				write_ch3_special_fnum(freq, st.lfo_fnum_offset, *pr);
			} else {
				m_opna.set_fm_frequency_hz(ch, freq);
			}
		} else {
			m_opna.set_psg_tone_frequency(ch - k_fm_channel_count, freq);
		}
		st.last_applied_freq_hz = freq;
	}

	// 音量の差分書き込み。
	if (volume != st.last_applied_volume) {
		if (ch < k_fm_channel_count) {
			apply_fm_volume_with(ch, volume);
		} else {
			// HW envelope モード時は volume_envelope_bit (0x10) を立てる。
			// これで PSG ハードウェアのエンベロープ出力がチャンネル音量となる
			// (V コマンドの値は出力に影響しなくなる)。
			// PsgChip::volume_envelope_bit と同じ値 (0x10) をリテラルで持つ。
			// SSG は 4bit なので 15 にクランプ (V16 を & 0x0F で 0 にしない)。
			uint8_t psg_vol = static_cast<uint8_t>(std::min(volume, 15));
			if (st.ssg_use_hw_envelope) {
				psg_vol = 0x10;  // = volume_envelope_bit
			}
			m_opna.set_psg_channel_volume(ch - k_fm_channel_count, psg_vol);
		}
		st.last_applied_volume = volume;
	}
}

// ---- ワーカースレッド本体 -------------------------------------------

void Player::worker_loop() {
	using clock = std::chrono::steady_clock;

	double tempo_bpm   = music_com_tempo_to_bpm(k_default_tempo_value);
	double current_tick = 0.0;
	int32_t last_processed_tick = -1;  // まだ処理していない先頭は 0。
	auto   last_time    = clock::now();
	bool   all_consumed = false;

	while (m_running.load()) {
		// 経過実時刻 → 進めるべき tick 数。 5ms 単位で 1〜2 ticks 程度進む。
		// 過去 (last_processed_tick+1) から現在 (tick_int) まで 1 tick ずつ
		// process_tick_common を呼ばないと、 同じ tick に集中したイベント
		// (key_off + 次の key_on 等) が一括処理されて音が詰まる。
		const int32_t tick_int = static_cast<int32_t>(current_tick);
		for (int32_t t = last_processed_tick + 1; t <= tick_int; ++t) {
			all_consumed = process_tick_common(t, tempo_bpm);
			if (all_consumed) break;
		}
		last_processed_tick = tick_int;

		if (all_consumed) {
			// FM の Release 余韻が消えるまで少し待ってから終了。
			std::this_thread::sleep_for(std::chrono::milliseconds(400));
			m_running.store(false);
			break;
		}

		// 5ms スリープ → 時刻進行。
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		const auto   now         = clock::now();
		const double elapsed_sec = std::chrono::duration<double>(now - last_time).count();
		last_time = now;
		const double ticks_per_sec = tempo_bpm *
			static_cast<double>(k_ticks_per_quarter_note) / 60.0;
		current_tick += elapsed_sec * ticks_per_sec;
	}
}

// ---- オフラインレンダ ----------------------------------------------

void Player::prepare_offline() {
	// 通常 worker 駆動の片付け。
	if (m_running.load()) {
		stop();
	}
	if (m_worker.joinable()) {
		m_worker.join();
	}
	// 全 ch にデフォルト音色適用 + SSG 初期化 (play() と同等)。
	for (int i = 0; i < k_fm_channel_count; ++i) {
		m_state[i].current_preset = nullptr;
		apply_preset(i, m_default_preset);
		apply_fm_volume(i);
	}
	m_opna.psg_reset();
	for (int i = 0; i < k_fm_channel_count; ++i) {
		m_opna.set_psg_mixer(i, /*tone*/false, /*noise*/false);
		m_opna.set_psg_channel_volume(i, 0);
	}
	m_offline_tempo_bpm    = music_com_tempo_to_bpm(k_default_tempo_value);
	m_current_tempo_bpm    = m_offline_tempo_bpm;
	m_offline_current_tick = 0.0;
	m_offline_last_tick    = -1;
	m_offline_finished     = false;
	m_offline_active       = true;
}

bool Player::step_offline_seconds(double seconds) {
	if (!m_offline_active) return false;
	if (m_offline_finished)  return false;

	// テンポを反映して秒 → tick 換算。テンポは process_tick_common 内の Tempo
	// イベント処理で更新されるため、ティック単位で換算しないと最新テンポが反映されない。
	// → 1 tick ずつ進めて、毎 tick 直後にテンポ更新を取り込む。
	// 進めるべき総 tick 数 (現テンポベースで概算)。テンポが途中で変わると誤差が出るが、
	// バッファ単位 (~20ms) なので無視できる。
	const double ticks_per_sec = m_offline_tempo_bpm * static_cast<double>(k_ticks_per_quarter_note) / 60.0;
	const double end_tick = m_offline_current_tick + seconds * ticks_per_sec;
	const int32_t end_int  = static_cast<int32_t>(end_tick);

	// バッファ境界 tick を二重処理しないよう、処理済み tick の次から進める
	// (worker_loop と同じ方式)。二重処理すると update_effects の
	// `since_on % interval == 0` スイープ減算が同一 tick で 2 回発火し、
	// ch3 ドラムのピッチスイープが 2 倍速で沈む (BOSS.MML キックで実測)。
	for (int32_t t = m_offline_last_tick + 1; t <= end_int; ++t) {
		const bool all_consumed = process_tick_common(t, m_offline_tempo_bpm);
		if (all_consumed && t == end_int) {
			m_offline_finished = true;
		}
	}
	m_offline_last_tick    = std::max(m_offline_last_tick, end_int);
	m_offline_current_tick = end_tick;

	// 効果音シーケンサを実時間 60Hz で進める (テンポ非依存)。
	// seconds * 60 を蓄積し、 整数フレーム数が出たらその分進める。
	m_effect_frame_accum += seconds * 60.0;
	const int whole_frames = static_cast<int>(m_effect_frame_accum);
	if (whole_frames > 0) {
		m_effect_frame_accum -= whole_frames;
		update_effect_frames(whole_frames);
	}

	return !m_offline_finished;
}

void Player::finish_offline() {
	m_offline_active = false;
	// 念のため全 ch を消音状態に。
	for (int i = 0; i < k_fm_channel_count; ++i) {
		m_opna.fm_key_off(i);
		m_opna.set_psg_channel_volume(i, 0);
	}
}

// ---- 内部共通実装 ----------------------------------------------------

bool Player::process_tick_common(int32_t tick_int, double& tempo_bpm) {
	// 1) tick 到達済みイベントを先に処理。
	// 順序メモ: 旧実装は key_off → events だったが、 タイノート (tied_from_prev)
	// が「前ノート gate 終了 tick = 次タイノート 開始 tick」 で到着する場合に、
	// key_off が先に発火してしまいサステインが切れていた (YAG03 A2 等)。
	// events を先に処理すれば、 タイノートが pending_key_off_tick を後ろへ
	// 延長するため key_off は発火しなくなる。 非タイの通常ノートは event 側で
	// 明示的に fm_key_off + fm_key_on を発行するので挙動同等。
	for (int ch = 0; ch < k_channel_count; ++ch) {
		auto& st = m_state[ch];
		const auto& events = m_score.channels[ch];
		while (st.next_event < events.size() &&
		       events[st.next_event].time_ticks <= tick_int) {
			const auto& ev = events[st.next_event];
			if (ev.kind == EventKind::Tempo && ev.a > 0) {
				// ev.a は T コマンドの「値」(BPM ではない)。実効 BPM へ変換する。
				tempo_bpm = music_com_tempo_to_bpm(static_cast<int>(ev.a));
				m_current_tempo_bpm = tempo_bpm;
			}
			if (ch < k_fm_channel_count) {
				handle_event_fm(ch, ev);
			} else {
				handle_event_ssg(ch - k_fm_channel_count, ev);
			}
			++st.next_event;
		}
	}

	// 2) 期限切れ key_off を発火 (events 処理後の最新 pending_key_off_tick で判定)。
	for (int ch = 0; ch < k_channel_count; ++ch) {
		auto& st = m_state[ch];
		if (st.pending_key_off_tick >= 0 && tick_int >= st.pending_key_off_tick) {
			if (ch < k_fm_channel_count) {
				m_opna.fm_key_off(ch);
			} else {
				// SSG ch: MUSIC.COM 仕様で Q gate 経過時に mixer bit を立てて
				// tone disable。NP2 録音と一致 (キーオン→ tone enable、gate 終→
				// tone disable、R 休符中はそのまま)。Q デフォルトを Q7 / SSGENV
				// V スケーリング / テンポ補正など他修正と組み合わせて初めて正しく
				// 動く (= 過去に試して peak count 落ちた件は前提条件不足が原因)。
				const int psg_ch = ch - k_fm_channel_count;
				m_opna.set_psg_mixer(psg_ch, /*tone*/false, /*noise*/false);
			}
			st.pending_key_off_tick = -1;
			st.note_on_tick = -1;
		}
	}

	// 3) effect 更新。
	for (int ch = 0; ch < k_channel_count; ++ch) {
		update_effects(ch, tick_int);
	}

	// 4) D パート効果音トリガ消化。
	handle_d_part_events(tick_int);

	// 5) 終了判定 (効果音再生中も「未完了」 扱い)。
	for (int ch = 0; ch < k_channel_count; ++ch) {
		const auto& st = m_state[ch];
		const auto& events = m_score.channels[ch];
		if (st.next_event < events.size() || st.pending_key_off_tick >= 0) {
			return false;
		}
	}
	if (m_d_part_next_event < m_score.d_part.size() || m_effect_state.active) {
		return false;
	}
	return true;
}

// ---- D パート効果音シーケンサ ---------------------------------------
//
// 実機 MUSIC.COM はタイマー B (~17ms = 60Hz 近似) でフレームを駆動し、 SSG ch A/B
// レジスタを SOUND.DAT の段階列に従って書き換える。 本実装も 60Hz 固定フレームで進む。
// テンポに依存しないため、 ドラムのスピード感がメロディと独立する (実機準拠)。
//
// レジスタマップ:
//   R0/R1 = ch A tone period (low/high, 12bit)
//   R2/R3 = ch B tone period
//   R6    = noise period (5bit)
//   R7    = mixer (bit 0-2 tone disable A/B/C, bit 3-5 noise disable A/B/C)
//   R8    = ch A volume (4bit、 bit 4 = HW envelope)
//   R9    = ch B volume
// ch C (= MUSIC.COM 6 番、 メロディ用) は触らない。 R7 を書く際は bit 2/5 を保護する。

void Player::handle_d_part_events(int32_t tick_int) {
	const auto& d = m_score.d_part;
	while (m_d_part_next_event < d.size() && d[m_d_part_next_event].time_ticks <= tick_int) {
		const auto& ev = d[m_d_part_next_event];
		if (ev.kind == EventKind::EffectTrigger) {
			// musiccom_emu (BGM34: D:F1) の実測で、 D パートの音符 1 つ = 効果音 1 回
			// 発火と判明。 音符長は次の D イベントまでの時間を決めるだけで、
			// 旧実装の「音符長期間中の自然終了→再発火 (リトリガー)」 は実機に存在しない
			// (NP2 ref の reg6 書込数 = 効果音 1 回分 × ループ数 と一致)。
			trigger_effect(static_cast<int>(ev.a));
		}
		++m_d_part_next_event;
	}
}

void Player::trigger_effect(int effect_id) {
	if (!m_sound_dat) return;
	const SoundDatEffect* eff = m_sound_dat->find(effect_id);
	if (!eff || eff->stages.empty()) {
		// 該当無し → 既存再生は止めず無視 (実機挙動不明だが安全側)。
		return;
	}
	m_effect_state.active      = true;
	m_effect_state.effect_id   = effect_id;
	m_effect_state.stage_index = 0;
	apply_effect_stage_initial();
}

void Player::apply_effect_stage_initial() {
	if (!m_effect_state.active || !m_sound_dat) return;
	const SoundDatEffect* eff = m_sound_dat->find(m_effect_state.effect_id);
	if (!eff || m_effect_state.stage_index >= eff->stages.size()) {
		release_effect_channels();
		return;
	}
	const SoundDatStage& st = eff->stages[m_effect_state.stage_index];
	m_effect_state.frames_left    = st.p1;
	m_effect_state.frames_elapsed = 0;
	m_effect_state.tone_f[0]    = st.tone[0].f;
	m_effect_state.tone_f[1]    = st.tone[1].f;
	m_effect_state.vol_v_8_8[0] = st.vol[0].v_8_8;
	m_effect_state.vol_v_8_8[1] = st.vol[1].v_8_8;
	m_effect_state.noise_n_8_8  = st.noise.n_8_8;

	// 初期書き込み: 段階開始時に SSG レジスタを一気に設定する。
	// ch A tone period
	m_opna.write_reg(0, 0, static_cast<uint8_t>(st.tone[0].f & 0xFF));
	m_opna.write_reg(0, 1, static_cast<uint8_t>((st.tone[0].f >> 8) & 0x0F));
	// ch B tone period
	m_opna.write_reg(0, 2, static_cast<uint8_t>(st.tone[1].f & 0xFF));
	m_opna.write_reg(0, 3, static_cast<uint8_t>((st.tone[1].f >> 8) & 0x0F));
	// noise period
	m_opna.write_reg(0, 6, static_cast<uint8_t>((st.noise.n_8_8 >> 8) & 0x1F));
	// mixer R7: ch A/B のビットだけ enable_flag から反映する。
	// musiccom_emu (BGM34) の実測で、 MUSIC.COM は効果音の A/B ビットに「音楽側の
	// ch C 状態」 を合成した値を書くと判明 (ch6 演奏中は bit2=0、 休止中は bit2=1)。
	// set_psg_mixer 経由ならチップ内シャドウと整合し、 ch C 状態も自然に保たれる。
	m_opna.set_psg_mixer(0, !(st.enable_flag & 0x01), !(st.enable_flag & 0x08));
	m_opna.set_psg_mixer(1, !(st.enable_flag & 0x02), !(st.enable_flag & 0x10));
	// ch A/B volume (上位 4bit)
	m_opna.write_reg(0, 8, static_cast<uint8_t>((st.vol[0].v_8_8 >> 8) & 0x0F));
	m_opna.write_reg(0, 9, static_cast<uint8_t>((st.vol[1].v_8_8 >> 8) & 0x0F));
}

void Player::update_effect_frames(int frames) {
	if (!m_effect_state.active || !m_sound_dat) return;
	if (frames <= 0) return;
	const SoundDatEffect* eff = m_sound_dat->find(m_effect_state.effect_id);
	if (!eff) { release_effect_channels(); return; }

	for (int f = 0; f < frames; ++f) {
		if (!m_effect_state.active) break;
		if (m_effect_state.stage_index >= eff->stages.size()) { release_effect_channels(); break; }
		const SoundDatStage& st = eff->stages[m_effect_state.stage_index];

		// 各シーケンス値を 1 フレーム分進める。
		// MUSIC.COM 仕様: 各 seq は独立に「dur 回加算したら base にリセット」 を周期 (dur+1)
		// で繰り返す。 BGM34.s98 ref で noise period が 0x14→...→0x0f→0x14 と 5 frames 周期で
		// ループする挙動と一致 (dur=4 の場合 4 回加算 + 1 リセット = 5 frames 周期)。
		// dur=0 のシーケンスはずっと base 維持 (T2 のような空 seq)。
		const int elapsed = m_effect_state.frames_elapsed + 1;  // 加算後を frame 1 とする
		auto cycle = [elapsed](int32_t& cur, int base_8_8, int step, int dur) {
			if (dur <= 0) return;
			const int period = dur + 1;
			const int sub    = elapsed % period;
			if (sub == 0)  cur = base_8_8;  // ループ先頭でリセット
			else            cur += step;
		};
		cycle(m_effect_state.tone_f[0],    st.tone[0].f,     st.tone[0].step, st.tone[0].duration);
		cycle(m_effect_state.tone_f[1],    st.tone[1].f,     st.tone[1].step, st.tone[1].duration);
		cycle(m_effect_state.vol_v_8_8[0], st.vol[0].v_8_8,  st.vol[0].step,  st.vol[0].duration);
		cycle(m_effect_state.vol_v_8_8[1], st.vol[1].v_8_8,  st.vol[1].step,  st.vol[1].duration);
		cycle(m_effect_state.noise_n_8_8,  st.noise.n_8_8,   st.noise.step,   st.noise.duration);
		++m_effect_state.frames_elapsed;

		// 値クランプ。
		auto clamp_u16 = [](int32_t v) -> uint16_t {
			if (v < 0) return 0;
			if (v > 0xFFFF) return 0xFFFF;
			return static_cast<uint16_t>(v);
		};
		const uint16_t f_a   = clamp_u16(m_effect_state.tone_f[0]);
		const uint16_t f_b   = clamp_u16(m_effect_state.tone_f[1]);
		const uint16_t v_a88 = clamp_u16(m_effect_state.vol_v_8_8[0]);
		const uint16_t v_b88 = clamp_u16(m_effect_state.vol_v_8_8[1]);
		const uint16_t n88   = clamp_u16(m_effect_state.noise_n_8_8);

		// SSG レジスタ書き込み。
		m_opna.write_reg(0, 0, static_cast<uint8_t>(f_a & 0xFF));
		m_opna.write_reg(0, 1, static_cast<uint8_t>((f_a >> 8) & 0x0F));
		m_opna.write_reg(0, 2, static_cast<uint8_t>(f_b & 0xFF));
		m_opna.write_reg(0, 3, static_cast<uint8_t>((f_b >> 8) & 0x0F));
		m_opna.write_reg(0, 6, static_cast<uint8_t>((n88 >> 8) & 0x1F));
		m_opna.write_reg(0, 8, static_cast<uint8_t>((v_a88 >> 8) & 0x0F));
		m_opna.write_reg(0, 9, static_cast<uint8_t>((v_b88 >> 8) & 0x0F));

		// 段階内フレーム消化。
		--m_effect_state.frames_left;
		if (m_effect_state.frames_left <= 0) {
			++m_effect_state.stage_index;
			if (m_effect_state.stage_index >= eff->stages.size()) {
				release_effect_channels();
				break;
			}
			apply_effect_stage_initial();
		}
	}
}

void Player::release_effect_channels() {
	if (!m_effect_state.active) return;
	m_effect_state.active = false;
	// ch A/B 音量 0、 mixer の ch A/B tone+noise を disable。
	// set_psg_mixer 経由でシャドウと整合させ、 ch C のビットは保持する。
	m_opna.write_reg(0, 8, 0);
	m_opna.write_reg(0, 9, 0);
	m_opna.set_psg_mixer(0, false, false);
	m_opna.set_psg_mixer(1, false, false);
}

}  // namespace mml
