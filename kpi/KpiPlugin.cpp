//
// KpiPlugin.cpp: MMLPlayer を KbMedia Player のデコーダ型プラグイン (.kpi) として提供する。
//
// SDK: Materials/kpisdk/kmp_pi.h (KMPMODULE 型)。kmp_GetTestModule をエクスポートし、
// 本体が Open/Render/Close/SetPosition を呼ぶ。MML シンセは「PCM を連続生成する
// デコーダ」として振る舞う。Render は単独 exe のオフラインレンダ経路
// (opna.render + player.step_offline_seconds) と同一ロジック。
//
// PIMPL 規約に従いプラグイン実装は .cpp のみ。windows.h は本ファイルでだけ include する。
//

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "kmp_pi.h"

#include "audio/synth/opna/IOpnaChip.h"
#include "audio/synth/fmgen_chip/FmgenOpnaChip.h"
#include "mml/MmlFileParser.h"
#include "mml/MmlPlayer.h"
#include "mml/MmlScore.h"
#include "mml/SoundDat.h"

namespace {

// プラグイン出力フォーマット (固定)。OPNA エミュは 48kHz / 16bit / ステレオ。
constexpr uint32_t k_sample_rate     = 48000;
constexpr uint32_t k_channels        = 2;
constexpr uint32_t k_bits            = 16;
constexpr uint32_t k_bytes_per_frame = k_channels * (k_bits / 8);  // 4

// fmgen backend のリアルタイム音量 (main.cpp と同値)。NP2 同等で「基準」評価。
// オフライン WAV のような曲全体ピーク正規化はストリーミングでは不可なので、
// 単独 exe のリアルタイム再生と同じ master volume で鳴らす。
constexpr float k_fm_volume  = 1.06f;
constexpr float k_psg_volume = 0.28f;

// MML の `{0` 無限ループ展開回数。2 = 1 回ループ (= 2 回再生) で曲が終わる。
// main.cpp のオフライン既定 (loop_count=1 → expansion 2) と一致させる。
constexpr int k_loop_expansion = 2;

// レジスタ書き込みと PCM レンダを同期させる粒度 (1ms @ 48kHz)。
// 連続生成型エンジンでも MML の細かいノートを取りこぼさないため (main.cpp 参照)。
constexpr uint32_t k_chunk_frames = 48;

// 1 回の Open に対応する再生インスタンス。Open で生成、Close で破棄。
struct MmlContext {
	std::unique_ptr<IOpnaChip>   opna;
	std::unique_ptr<mml::Player> player;
	mml::Score    score;
	mml::SoundDat sound_dat;
	bool          has_sound_dat = false;

	uint64_t total_frames    = 0;  // 再生終了とみなすフレーム数 (release tail 込み)
	uint64_t frames_rendered = 0;  // これまでに生成したフレーム数
};

// パース済み score / 読み込み済み sound_dat から、チップと Player を構築して
// 先頭再生状態にする。Open と SetPosition (巻き戻し) で共用。
void build_player(MmlContext& ctx) {
	ctx.opna = std::make_unique<FmgenOpnaChip>(
		k_sample_rate, FmgenOpnaChip::k_default_master_clock_hz);
	ctx.opna->set_fm_volume(k_fm_volume);
	ctx.opna->set_psg_volume(k_psg_volume);

	ctx.player = std::make_unique<mml::Player>(*ctx.opna);
	if (ctx.has_sound_dat) {
		ctx.player->set_sound_dat(&ctx.sound_dat);
	}
	ctx.player->load(ctx.score);
	ctx.player->prepare_offline();
	ctx.frames_rendered = 0;
}

// cszFileName のディレクトリ部分 (末尾区切り込み) を返す。区切りが無ければ空。
std::string dir_of(const char* path) {
	const std::string p(path ? path : "");
	const size_t cut = p.find_last_of("/\\");
	return (cut == std::string::npos) ? std::string() : p.substr(0, cut + 1);
}

// 指定フレーム数だけ player と chip を進める。dst が非 nullptr なら PCM を書き、
// nullptr なら scratch (捨てバッファ) へ書いて状態だけ進める (シーク早送り用)。
void advance_frames(MmlContext& ctx, int16_t* dst, uint32_t frames) {
	std::vector<int16_t> scratch;
	if (!dst) scratch.resize(static_cast<size_t>(k_chunk_frames) * k_channels);

	uint32_t done = 0;
	while (done < frames) {
		const uint32_t n = std::min(frames - done, k_chunk_frames);
		int16_t* out = dst ? (dst + static_cast<size_t>(done) * k_channels)
		                   : scratch.data();
		ctx.opna->render(out, n, k_channels);
		ctx.player->step_offline_seconds(
			static_cast<double>(n) / static_cast<double>(k_sample_rate));
		done += n;
	}
	ctx.frames_rendered += frames;
}

HKMP WINAPI mml_Open(const char* cszFileName, SOUNDINFO* pInfo) {
	if (!cszFileName || !pInfo) return nullptr;

	std::ifstream ifs(cszFileName, std::ios::binary);
	if (!ifs) return nullptr;
	std::ostringstream oss;
	oss << ifs.rdbuf();

	auto ctx = std::make_unique<MmlContext>();

	mml::FileParser parser;
	parser.set_infinite_loop_expansion(k_loop_expansion);
	parser.parse(oss.str(), ctx->score);

	// SOUND.DAT は D パートを使う MML のときだけ、MML 隣を探索して読み込む。
	if (!ctx->score.d_part.empty()) {
		const std::string sd_path = dir_of(cszFileName) + "SOUND.DAT";
		std::vector<std::string> warns;
		if (ctx->sound_dat.load(sd_path, warns) && ctx->sound_dat.size() > 0) {
			ctx->has_sound_dat = true;
		}
	}

	build_player(*ctx);

	// 曲長 (秒) を total_ticks から算出する。 tick→秒 の換算係数はテンポ (T コマンド)
	// ごとに変わるため、 テンポ区間ごとに積分する。 旧実装のように k_default_tempo_value
	// (=120) 固定で換算すると、 曲の実テンポが既定と異なるとシークバー長と実再生長が
	// ずれ、 曲尾に無音が残ったり (実テンポが速い場合。 例: AGM19 は T132 で declared
	// 58s に対し実 content は ~52s)、 逆に曲が途中で切れたり (実テンポが遅い場合) する。
	// 再生側 (step_offline_seconds) と同じテンポ遷移で積分するので両者が一致する。
	auto ticks_per_sec_at = [](int t_value) {
		return mml::music_com_tempo_to_bpm(t_value) *
		       static_cast<double>(mml::k_ticks_per_quarter_note) / 60.0;
	};
	// 全チャンネルの Tempo イベントを時刻順に集める (テンポはグローバル状態)。
	std::vector<std::pair<int32_t, int>> tempo_changes;
	for (const auto& chan : ctx->score.channels) {
		for (const auto& ev : chan) {
			if (ev.kind == mml::EventKind::Tempo && ev.a > 0) {
				tempo_changes.emplace_back(ev.time_ticks, static_cast<int>(ev.a));
			}
		}
	}
	std::sort(tempo_changes.begin(), tempo_changes.end(),
		[](const auto& a, const auto& b) { return a.first < b.first; });
	// 既定テンポから開始し、 区間 [prev_tick, 次のテンポ変化) ごとに秒を積算。
	double  content_seconds = 0.0;
	int     cur_tempo = mml::k_default_tempo_value;
	int32_t prev_tick = 0;
	for (const auto& change : tempo_changes) {
		if (change.first > prev_tick) {
			content_seconds +=
				(change.first - prev_tick) / ticks_per_sec_at(cur_tempo);
			prev_tick = change.first;
		}
		cur_tempo = change.second;  // 同 tick の複数 Tempo は後勝ち
	}
	if (ctx->score.total_ticks > prev_tick) {
		content_seconds +=
			(ctx->score.total_ticks - prev_tick) / ticks_per_sec_at(cur_tempo);
	}

	const double total_seconds = content_seconds + 0.3;  // +0.3 秒 = release tail
	ctx->total_frames =
		static_cast<uint64_t>(total_seconds * static_cast<double>(k_sample_rate));

	pInfo->dwSamplesPerSec = k_sample_rate;
	pInfo->dwChannels      = k_channels;
	pInfo->dwBitsPerSample = k_bits;
	pInfo->dwLength        = static_cast<DWORD>(total_seconds * 1000.0);
	pInfo->dwSeekable      = 1;
	pInfo->dwUnitRender    = 0;
	pInfo->dwReserved1     = 0;  // 有限長 (曲が自然に終わる)
	pInfo->dwReserved2     = 0;  // 単一ファイル単一曲

	return static_cast<HKMP>(ctx.release());
}

void WINAPI mml_Close(HKMP hKMP) {
	auto* ctx = static_cast<MmlContext*>(hKMP);
	if (!ctx) return;
	if (ctx->player) ctx->player->finish_offline();
	delete ctx;
}

DWORD WINAPI mml_Render(HKMP hKMP, BYTE* Buffer, DWORD dwSize) {
	auto* ctx = static_cast<MmlContext*>(hKMP);
	if (!ctx || !Buffer) return 0;

	const uint32_t frames_req = dwSize / k_bytes_per_frame;
	// total_frames で打ち切り (offline レンダと同じ停止条件)。
	const uint64_t remain = (ctx->frames_rendered < ctx->total_frames)
		? (ctx->total_frames - ctx->frames_rendered) : 0;
	const uint32_t produced = static_cast<uint32_t>(
		std::min<uint64_t>(frames_req, remain));

	if (produced > 0) {
		advance_frames(*ctx, reinterpret_cast<int16_t*>(Buffer), produced);
	}
	// produced < frames_req → 曲終了。dwSize より小さいバイト数を返すと本体が再生終了。
	return produced * k_bytes_per_frame;
}

DWORD WINAPI mml_SetPosition(HKMP hKMP, DWORD dwPos) {
	auto* ctx = static_cast<MmlContext*>(hKMP);
	if (!ctx) return 0;

	// 先頭へ巻き戻して作り直す (曲は短いので毎回再構築で十分)。
	build_player(*ctx);

	// dwPos ms 分を捨てバッファへレンダして早送り (chip + player 状態を整合させる)。
	uint64_t target = static_cast<uint64_t>(
		static_cast<double>(dwPos) / 1000.0 * static_cast<double>(k_sample_rate));
	if (target > ctx->total_frames) target = ctx->total_frames;
	if (target > 0) {
		advance_frames(*ctx, nullptr, static_cast<uint32_t>(target));
	}
	return dwPos;
}

// 対応拡張子 (本体側で小文字化されてマッチ)。NULL 終端必須。
const char* g_support_exts[] = { ".mml", nullptr };

KMPMODULE g_module = {
	KMPMODULE_VERSION,                               // dwVersion
	100,                                             // dwPluginVersion
	"MMLPlayer",                                     // pszCopyright
	"MUSIC.COM MML player (OPNA / fmgen emulation)", // pszDescription
	g_support_exts,                                  // ppszSupportExts
	1,                                               // dwReentrant (各 HKMP 独立)
	nullptr,                                         // Init
	nullptr,                                         // Deinit
	mml_Open,                                        // Open
	nullptr,                                         // OpenFromBuffer (未使用)
	mml_Close,                                       // Close
	mml_Render,                                      // Render
	mml_SetPosition,                                 // SetPosition
};

}  // namespace

// KbMedia Player 本体が最初に呼ぶエクスポート関数。.def で未装飾名を確定させるため
// __declspec(dllexport) は付けない (x86 の stdcall 装飾 _kmp_GetTestModule@0 を回避)。
extern "C" KMPMODULE* WINAPI kmp_GetTestModule(void) {
	return &g_module;
}
