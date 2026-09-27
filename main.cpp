//
// MMLPlayer エントリポイント。
//
// 役割は引数パースとモード分岐のみ:
//   - --help / -h        → app/CliHelp.h
//   - --debug SUBCMD     → 内部表記 --SUBCMD に正規化して既存判定にディスパッチ
//   - --debug ymfm-*     → app/YmfmTests.h
//   - --debug dump-*     → app/DebugDumps.h
//   - --play-s98         → app/S98Player.h
//   - render / record-s98 / play-effect / リアルタイム再生 → 本ファイル末尾の本処理
//
// オフラインレンダ・リアルタイム待機ループは app/Render.h に集約。
// WAV ライタは app/WaveWriter.h。
//

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "audio/MeAudio.h"
#include "audio/MeAudioStream.h"
#include "audio/synth/opna/IOpnaChip.h"
#include "audio/synth/opna/OpnaChip.h"
#include "audio/synth/ymfm_chip/YmfmOpnaChip.h"
#include "audio/synth/fmgen_chip/FmgenOpnaChip.h"
#include "audio/synth/opngen_chip/OpngenOpnaChip.h"

#include "mml/MmlFileParser.h"
#include "mml/MmlPlayer.h"
#include "mml/MmlScore.h"
#include "mml/MmlTimbre.h"
#include "mml/SoundDat.h"

#include "app/CliHelp.h"
#include "app/CliPrint.h"
#include "app/DebugDumps.h"
#include "app/Render.h"
#include "app/S98Player.h"
#include "app/YmfmTests.h"

int main(int argc, char* argv[]) {
	// --help / -h: ヘルプ表示して終了。
	if (argc >= 2 && (std::strcmp(argv[1], "--help") == 0 ||
	                  std::strcmp(argv[1], "-h") == 0)) {
		app::print_help();
		return 0;
	}

	// --debug SUBCMD [args...] を内部表記 --<SUBCMD> [args...] に正規化する。
	// 既存の判定 (argv[1] == "--dump-sound-dat" 等) をそのまま流用するため、
	// argv[1] = "--debug" / argv[2] = SUBCMD を argv[1] = "--SUBCMD" に詰め替える。
	// 正規化文字列は static で寿命を main 全体に持たせる (argv に C 文字列を渡す前提)。
	static std::string debug_subcmd_normalized;
	if (argc >= 3 && std::strcmp(argv[1], "--debug") == 0) {
		debug_subcmd_normalized = std::string("--") + argv[2];
		argv[1] = const_cast<char*>(debug_subcmd_normalized.c_str());
		for (int i = 2; i < argc - 1; ++i) argv[i] = argv[i + 1];
		--argc;
	}

	// --opna=ymfm|original|fmgen: OPNA エミュレータ実装の選択 (既定 Fmgen)。
	// 内部 enum 名 (Ymfm/Legacy) は実装ファイル名 (LegacyOpnaChip) に対応するためそのまま。
	enum class OpnaBackend { Ymfm, Legacy, Fmgen, Opngen };
	OpnaBackend opna_backend = OpnaBackend::Fmgen;
	{
		int w = 1;
		for (int r = 1; r < argc; ++r) {
			if (std::strncmp(argv[r], "--opna=", 7) == 0) {
				const char* val = argv[r] + 7;
				if (std::strcmp(val, "ymfm") == 0) {
					opna_backend = OpnaBackend::Ymfm;
				} else if (std::strcmp(val, "original") == 0) {
					opna_backend = OpnaBackend::Legacy;
				} else if (std::strcmp(val, "fmgen") == 0) {
					opna_backend = OpnaBackend::Fmgen;
				} else if (std::strcmp(val, "opngen") == 0) {
					opna_backend = OpnaBackend::Opngen;
				} else {
					app::cli_fprintf(stderr, "--opna must be 'ymfm', 'original', 'fmgen' or 'opngen'\n");
					return 1;
				}
				continue;
			}
			argv[w++] = argv[r];
		}
		argc = w;
	}

	// 値付きフラグの消費パース。 早期 return パス (--play-s98 等) でも --out を
	// 参照したいので、 ここで先に処理して argv を確定させる。
	const char* s98_record_path = nullptr;
	const char* sound_dat_path  = nullptr;
	const char* output_wav_path = nullptr;
	double max_seconds = 0.0;
	int    play_effect_id   = -1;
	bool   play_effect_mode = false;
	// 無限ループ展開回数。 -1 は未指定マーカー。 既定はモード判定後に決定:
	// リアルタイム再生 → 99 / オフライン出力 → 1。
	int    loop_count = -1;
	{
		int w = 1;
		for (int r = 1; r < argc; ++r) {
			if (std::strcmp(argv[r], "--record-s98") == 0 && r + 1 < argc) {
				s98_record_path = argv[r + 1];
				++r; continue;
			}
			if (std::strcmp(argv[r], "--max-seconds") == 0 && r + 1 < argc) {
				max_seconds = std::atof(argv[r + 1]);
				++r; continue;
			}
			if (std::strcmp(argv[r], "--sound-dat") == 0 && r + 1 < argc) {
				sound_dat_path = argv[r + 1];
				++r; continue;
			}
			if (std::strcmp(argv[r], "--play-effect") == 0 && r + 1 < argc) {
				play_effect_id   = std::atoi(argv[r + 1]);
				play_effect_mode = true;
				++r; continue;
			}
			if (std::strcmp(argv[r], "--loop-count") == 0 && r + 1 < argc) {
				loop_count = std::atoi(argv[r + 1]);
				if (loop_count < 0) loop_count = 0;
				++r; continue;
			}
			if (std::strcmp(argv[r], "--out") == 0 && r + 1 < argc) {
				output_wav_path = argv[r + 1];
				++r; continue;
			}
			argv[w++] = argv[r];
		}
		argc = w;
	}

	// デバッグ早期 return パス (argv[1] フラグで分岐し、 すぐ終了するもの)。
	if (argc >= 2 && std::strcmp(argv[1], "--ymfm-direct") == 0) {
		return app::run_ymfm_direct();
	}
	if (argc >= 2 && std::strcmp(argv[1], "--ymfm-test") == 0) {
		return app::run_ymfm_test();
	}
	if (argc >= 2 && std::strcmp(argv[1], "--ymfm-ssg-test") == 0) {
		return app::run_ymfm_ssg_test();
	}
	if (argc >= 3 && std::strcmp(argv[1], "--play-s98") == 0) {
		return app::run_play_s98(argv[2], output_wav_path);
	}
	if (argc >= 3 && std::strcmp(argv[1], "--dump-d-events") == 0) {
		return app::run_dump_d_events(argv[2]);
	}
	if (argc >= 3 && std::strcmp(argv[1], "--dump-sound-dat") == 0) {
		return app::run_dump_sound_dat(argv[2]);
	}

	// 残りのフラグ判定 (位置引数 MML_PATH の前に置けるもの)。
	// argv のどこにあっても認識する (--opna=ymfm 等の前後に置けるように)。
	bool offline_mode    = false;
	int  offline_arg_idx = -1;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--render") == 0) {
			offline_mode    = true;
			offline_arg_idx = i;
			break;
		}
	}

	// --render-ch N MML_PATH: 指定 ch (1..6) のみを残して他を空にしてレンダ。
	int  isolate_ch      = 0;
	bool isolate_mode    = false;
	int  isolate_arg_idx = -1;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--render-ch") == 0) {
			if (i + 1 >= argc) {
				app::cli_fprintf(stderr, "--render-ch requires N (1..6) [MML_PATH]\n");
				return 1;
			}
			isolate_ch = std::atoi(argv[i + 1]);
			if (isolate_ch < 1 || isolate_ch > 6) {
				app::cli_fprintf(stderr, "--render-ch N must be 1..6\n");
				return 1;
			}
			isolate_mode    = true;
			isolate_arg_idx = i;
			break;
		}
	}

	// --dump-events N MML_PATH: 指定 ch のイベント列を時刻順にダンプ。
	int  dump_ch      = 0;
	bool dump_mode    = false;
	int  dump_arg_idx = -1;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--dump-events") == 0) {
			if (i + 1 >= argc) {
				app::cli_fprintf(stderr, "--dump-events requires N (1..6) [MML_PATH]\n");
				return 1;
			}
			dump_ch = std::atoi(argv[i + 1]);
			if (dump_ch < 1 || dump_ch > 6) {
				app::cli_fprintf(stderr, "--dump-events N must be 1..6\n");
				return 1;
			}
			dump_mode    = true;
			dump_arg_idx = i;
			break;
		}
	}

	MeAudio audio;

	// --opna 選択に応じて IOpnaChip インスタンスを生成。
	// Legacy: 自前 FM/PSG 実装 (秒/Hz/dB をドメインモデルで持つ)。
	// Ymfm  : Aaron Giles 氏 ymfm の YM2608 (実機準拠度高)。
	std::unique_ptr<IOpnaChip> opna_ptr;
	if (opna_backend == OpnaBackend::Ymfm) {
		// Fidelity::Low: ymfm 内部 sample rate を master/48 (= 166.4 kHz @ 7.987 MHz) に。
		// 実機 DAC が 55 kHz 相当なので 48 kHz への補間にも十分な情報量がある。
		// (High に上げてもスペクトル特性に有意差なし。出力 LPF で高域過剰を補正。)
		opna_ptr = std::make_unique<YmfmOpnaChip>(
			48000, YmfmOpnaChip::k_default_master_clock_hz, YmfmOpnaChip::Fidelity::Low);
		app::cli_printf("[OPNA] backend = ymfm (fidelity=low)\n");
	} else if (opna_backend == OpnaBackend::Fmgen) {
		opna_ptr = std::make_unique<FmgenOpnaChip>(
			48000, FmgenOpnaChip::k_default_master_clock_hz);
		app::cli_printf("[OPNA] backend = fmgen (cisc)\n");
	} else if (opna_backend == OpnaBackend::Opngen) {
		opna_ptr = std::make_unique<OpngenOpnaChip>(
			48000, OpngenOpnaChip::k_default_master_clock_hz);
		app::cli_printf("[OPNA] backend = opngen (np21w)\n");
	} else {
		opna_ptr = std::make_unique<OpnaChip>(48000);
		app::cli_printf("[OPNA] backend = original\n");
	}
	IOpnaChip& opna = *opna_ptr;

	// S98 録音は YmfmOpnaChip 経由のときだけ有効 (Legacy には write_reg 録音機構なし)。
	if (s98_record_path) {
		if (opna_backend == OpnaBackend::Ymfm) {
			auto* y = static_cast<YmfmOpnaChip*>(opna_ptr.get());
			if (y->start_s98_recording(s98_record_path)) {
				app::cli_printf("[S98] recording to %s\n", s98_record_path);
			} else {
				app::cli_fprintf(stderr, "[S98] failed to open %s\n", s98_record_path);
				return 1;
			}
		} else {
			app::cli_fprintf(stderr, "[S98] --record-s98 requires --opna=ymfm\n");
			return 1;
		}
	}

	// FM/PSG 音量バランス校正値 ([[ymfm_master_volume]] / [[ch_volume_balance]])。
	// FM:PSG 比は実聴で調整 (PSG 過大だったため ≒ 3.8:1)。生レベル (= リアルタイム音量)
	// はコアごとに異なるため backend 別に設定する。fmgen は実機 (NP2) 同等で「完璧」と
	// 評価されたため基準。ymfm は同 1.06 だと fmgen の約半分 (peak 0.27 vs 0.53) で
	// 「少し足りない」 ため、 比率を保ったまま約 2 倍に上げて fmgen の生レベルに揃える。
	// (offline は正規化されるため master volume 変更の影響を受けない = リアルタイムのみ。)
	if (opna_backend == OpnaBackend::Legacy) {
		opna.set_fm_volume(0.40f);
		opna.set_psg_volume(0.04f);
	} else if (opna_backend == OpnaBackend::Fmgen) {
		// fmgen: SetVolumePSG(dB) へ FmgenOpnaChip が換算 (基準 0.55=0dB)。
		opna.set_fm_volume(1.06f);
		opna.set_psg_volume(0.28f);
	} else if (opna_backend == OpnaBackend::Opngen) {
		// opngen: FM/SSG を別 getpcm して独立ゲイン適用。fmgen の生 peak (≒0.53) に
		// 揃うよう FM を 1.1 に。SSG 比は実聴校正前の暫定 (要フィードバック)。
		opna.set_fm_volume(1.1f);
		opna.set_psg_volume(0.55f);
	} else {
		// ymfm: fmgen の生 peak に揃えるため FM/PSG とも約 2 倍。
		opna.set_fm_volume(2.10f);
		opna.set_psg_volume(0.56f);
	}

	mml::Player     player(opna);
	mml::FileParser parser;
	// 無限ループ `{0` の展開回数。
	//   --loop-count 明示 → その値
	//   未指定 + リアルタイム再生 → 99 (ESC まで実質永続)
	//   未指定 + オフライン出力 → 1 (2 回再生でファイルサイズ抑制)
	// dump 系は早期 return 済みなので影響しない。
	const bool realtime_mode = !(offline_mode || isolate_mode
	                              || s98_record_path || play_effect_mode);
	if (loop_count < 0) {
		loop_count = realtime_mode ? 99 : 1;
	}
	parser.set_infinite_loop_expansion(loop_count + 1);

	mml::Score    score;
	mml::SoundDat sound_dat;
	double        effect_duration_seconds = 0.0;  // --play-effect モードでのみ意味あり

	if (play_effect_mode) {
		// --play-effect N: SOUND.DAT を読み、 効果音 N (0..63) のみを単独再生。
		// MML ファイルは読まず、 空 Score の d_part に EffectTrigger を 1 つ置く。
		const std::string sd_path = sound_dat_path
			? std::string(sound_dat_path)
			: std::string("Materials/SOUND.DAT");
		std::vector<std::string> sd_warns;
		if (!sound_dat.load(sd_path, sd_warns)) {
			app::cli_fprintf(stderr, "[play-effect] cannot load SOUND.DAT: %s\n", sd_path.c_str());
			return 1;
		}
		const auto* eff = sound_dat.find(play_effect_id);
		if (!eff || eff->stages.empty()) {
			app::cli_fprintf(stderr, "[play-effect] effect @%d not defined in %s\n",
			             play_effect_id, sd_path.c_str());
			return 1;
		}
		int total_frames = 0;
		for (const auto& s : eff->stages) total_frames += s.p1;
		effect_duration_seconds = (total_frames / 60.0) + 0.3;
		mml::Event ev{};
		ev.kind = mml::EventKind::EffectTrigger;
		ev.a    = play_effect_id;
		score.d_part.push_back(ev);
		app::cli_printf("[play-effect] effect @%d, stages=%zu, frames=%d (~ %.2f sec) from %s\n",
		            play_effect_id, eff->stages.size(), total_frames,
		            total_frames / 60.0, sd_path.c_str());
		for (size_t i = 0; i < std::min<size_t>(4, sd_warns.size()); ++i) {
			app::cli_printf("    sd-warn: %s\n", sd_warns[i].c_str());
		}
		player.set_sound_dat(&sound_dat);
	} else {
		// MML パスを引数から決定。
		//   --render-ch N / --dump-events N → 該当フラグ位置 + 2
		//   --render → 該当フラグ位置 + 1
		//   それ以外 → 最初の位置引数 (= 「-」 で始まらない最初の argv)
		const char* mml_arg = nullptr;
		if (isolate_mode) {
			const int idx = isolate_arg_idx + 2;
			mml_arg = (idx < argc) ? argv[idx] : nullptr;
		} else if (dump_mode) {
			const int idx = dump_arg_idx + 2;
			mml_arg = (idx < argc) ? argv[idx] : nullptr;
		} else if (offline_mode) {
			const int idx = offline_arg_idx + 1;
			mml_arg = (idx < argc) ? argv[idx] : nullptr;
		} else {
			// 位置引数 (フラグでない最初の argv) を探す
			for (int i = 1; i < argc; ++i) {
				if (argv[i][0] != '-') { mml_arg = argv[i]; break; }
			}
		}
		const std::string sample_path = mml_arg
			? std::string(mml_arg)
			: std::string("Materials/MMLSamples/AGM/AGM01.MML");
		std::ifstream ifs(sample_path, std::ios::binary);
		if (!ifs) {
			app::cli_fprintf(stderr, "cannot open %s\n", sample_path.c_str());
			return 1;
		}
		std::ostringstream oss;
		oss << ifs.rdbuf();
		parser.parse(oss.str(), score);
	}

	// ch 隔離モード: 指定 ch 以外を空にする。 Tempo は ch1 にあることが多いので
	// 他 ch の Tempo イベントだけは isolate_ch に移植してから他 ch を消す。
	if (isolate_mode) {
		for (int ch = 0; ch < mml::k_channel_count; ++ch) {
			if (ch + 1 == isolate_ch) continue;
			for (const auto& ev : score.channels[ch]) {
				if (ev.kind == mml::EventKind::Tempo) {
					score.channels[isolate_ch - 1].push_back(ev);
				}
			}
			score.channels[ch].clear();
		}
		auto& target = score.channels[isolate_ch - 1];
		std::sort(target.begin(), target.end(),
			[](const mml::Event& a, const mml::Event& b) {
				return a.time_ticks < b.time_ticks;
			});
		app::cli_printf("[isolate-mode] keeping ch%d only\n", isolate_ch);
	}

	app::cli_printf("score: total_ticks=%d, %zu FM presets, %zu SSG envs\n",
	            score.total_ticks, score.fm_presets.size(), score.ssg_envelopes.size());
	for (int ch = 0; ch < mml::k_channel_count; ++ch) {
		app::cli_printf("  ch%d: %zu events\n", ch + 1, score.channels[ch].size());
	}
	if (!score.d_part.empty()) {
		app::cli_printf("  D part: %zu events\n", score.d_part.size());
	}

	// SOUND.DAT 読み込み: D パートを使う MML のときのみ。 --play-effect では事前読み込み済み。
	if (!play_effect_mode && !score.d_part.empty()) {
		std::vector<std::string> sd_warns;
		std::string sd_path;
		if (sound_dat_path) {
			sd_path = sound_dat_path;
		} else {
			// MML パスからの自動探索: MML 隣の SOUND.DAT → Materials/SOUND.DAT。
			const char* mml_arg = (isolate_mode || dump_mode)
				? (argc >= 4 ? argv[3] : nullptr)
				: offline_mode
					? (argc >= 3 ? argv[2] : nullptr)
					: (argc >= 2 ? argv[1] : nullptr);
			const std::string p = mml_arg
				? std::string(mml_arg)
				: std::string("Materials/MMLSamples/AGM/AGM01.MML");
			const size_t cut = p.find_last_of("/\\");
			const std::string dir = (cut == std::string::npos)
				? std::string()
				: p.substr(0, cut + 1);
			sd_path = dir + "SOUND.DAT";
			std::ifstream test(sd_path, std::ios::binary);
			if (!test) sd_path = "Materials/SOUND.DAT";
		}
		if (sound_dat.load(sd_path, sd_warns)) {
			app::cli_printf("  SOUND.DAT: loaded %zu effects from %s\n",
			            sound_dat.size(), sd_path.c_str());
			player.set_sound_dat(&sound_dat);
		} else {
			app::cli_printf("  SOUND.DAT: not found (%s) - D part will be silent\n",
			            sd_path.c_str());
		}
		for (size_t i = 0; i < std::min<size_t>(4, sd_warns.size()); ++i) {
			app::cli_printf("    sd-warn: %s\n", sd_warns[i].c_str());
		}
	}
	app::cli_printf("  warnings: %zu\n", score.warnings.size());
	for (size_t i = 0; i < std::min<size_t>(8, score.warnings.size()); ++i) {
		app::cli_printf("    - %s\n", score.warnings[i].c_str());
	}

	// --dump-events モード: 指定 ch のイベント列をダンプして終了。
	if (dump_mode) {
		app::dump_channel_events(score, dump_ch);
		return 0;
	}

	player.load(score);

	// WAV 出力が必要なモード判定。 --record-s98 単独時は S98 のみ、 --render / --out
	// 併用時のみ WAV も書く。
	const bool wav_output_needed = offline_mode || isolate_mode || play_effect_mode
	                                || (s98_record_path && output_wav_path != nullptr);
	if (wav_output_needed || s98_record_path) {
		// オフラインモード: 決定論的 step_offline で進める (worker 非依存)。
		// --out 指定時はそちらを優先、 なければモード別の既定名。
		// WAV 不要時は outname を空にして render_to_wav 内で WAV 出力を skip。
		std::string outname_buf;
		const char* outname = "";
		if (wav_output_needed) {
			if (output_wav_path) {
				outname = output_wav_path;
			} else {
				char buf[64];
				if (play_effect_mode) {
					std::snprintf(buf, sizeof(buf), "mine_effect_%d.wav", play_effect_id);
				} else if (isolate_mode) {
					std::snprintf(buf, sizeof(buf), "mine_ch%d.wav", isolate_ch);
				} else if (s98_record_path) {
					std::snprintf(buf, sizeof(buf), "mine_s98_record.wav");
				} else {
					std::snprintf(buf, sizeof(buf), "mine.wav");
				}
				outname_buf = buf;
				outname = outname_buf.c_str();
			}
		}
		double song_seconds;
		if (play_effect_mode) {
			// 効果音の段階列が完走するまで (60Hz × 合計フレーム + 0.3 秒の release tail)。
			song_seconds = effect_duration_seconds;
		} else {
			// 曲の長さを total_ticks から秒換算 (+ release tail 0.3 秒)。
			// バッファ長の概算なので既定テンポで換算する (途中の T 変化は無視)。
			const double ticks_per_sec = mml::music_com_tempo_to_bpm(mml::k_default_tempo_value)
				* static_cast<double>(mml::k_ticks_per_quarter_note) / 60.0;
			song_seconds = static_cast<double>(score.total_ticks) / ticks_per_sec + 0.3;
		}
		if (max_seconds > 0.0 && max_seconds < song_seconds) {
			song_seconds = max_seconds;
		}
		// ch isolate モード (= 単独 ch レンダ) は REF と相対音量を保つため normalize 無効。
		// フルミックス時は normalize で peak を -3 dBFS に揃えて単体WAV として聞きやすく。
		const bool normalize_output = !isolate_mode;
		app::render_to_wav(outname, song_seconds, 48000, opna, player, normalize_output);
	} else {
		// 通常モード: 実時間再生。
		// FillCallback 内でオフライン API を 1ms (48 frames) 粒度で呼ぶことで、
		// レジスタ書き込みと PCM レンダを同一スレッドで同期させる。
		// これにより worker スレッドの OS sleep 精度に依存しなくなり、
		// ノイズ・テンポ崩れ・音抜けを回避する。
		if (!audio.initialize()) {
			app::cli_fprintf(stderr, "MeAudio init failed\n");
			return 1;
		}
		player.prepare_offline();
		std::atomic<bool> rt_finished{false};
		MeAudioStream stream;
		const bool started = stream.start(audio,
			[&](int16_t* dst, uint32_t frames, uint32_t channels) {
				if (rt_finished.load(std::memory_order_relaxed)) {
					std::fill_n(dst, frames * channels, int16_t{0});
					return;
				}
				constexpr uint32_t k_chunk = 48;  // 1ms @ 48kHz
				constexpr double k_sec_per_chunk = static_cast<double>(k_chunk) / 48000.0;
				uint32_t rem = frames;
				while (rem > 0) {
					const uint32_t n = std::min(rem, k_chunk);
					opna.render(dst, n, channels);
					if (!rt_finished.load(std::memory_order_relaxed)) {
						if (!player.step_offline_seconds(k_sec_per_chunk)) {
							rt_finished.store(true, std::memory_order_relaxed);
						}
					}
					dst += n * static_cast<uint32_t>(channels);
					rem -= n;
				}
			});
		if (!started) {
			app::cli_fprintf(stderr, "MeAudioStream start failed\n");
			player.finish_offline();
			return 1;
		}
		app::cli_printf("再生中... (ESC キーで停止)\n");
		app::wait_until_esc_or_flag(rt_finished);
		stream.stop();
		player.finish_offline();
	}

	// S98 録音を閉じる (ENDMARK + flush)。
	if (s98_record_path && opna_backend == OpnaBackend::Ymfm) {
		auto* y = static_cast<YmfmOpnaChip*>(opna_ptr.get());
		y->stop_s98_recording();
		app::cli_printf("[S98] closed %s\n", s98_record_path);
	}

	app::cli_printf("Done.\n");
	return 0;
}
