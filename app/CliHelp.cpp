#include "CliHelp.h"

#include "CliPrint.h"

namespace app {

void print_help() {
	cli_printf(
		"MMLPlayer - MUSIC.COM 互換 MML 再生・解析ツール\n"
		"\n"
		"使い方:\n"
		"  MMLPlayer [OPTIONS] [MML_PATH]\n"
		"\n"
		"再生・出力:\n"
		"  MML_PATH                       MML をリアルタイム再生 (引数なしは AGM01.MML)\n"
		"  --render MML_PATH              MML をオフライン WAV 出力 (既定: mine.wav)\n"
		"  --record-s98 OUT.s98 MML_PATH  S98 v1 録音のみ。 WAV も欲しいときは --render を併用\n"
		"  --play-s98 IN.s98              S98 を再生して WAV 出力 (既定: mine_s98.wav)\n"
		"  --play-effect N                SOUND.DAT 効果音 N (0..63) を単独再生\n"
		"\n"
		"オプション:\n"
		"  --opna=ymfm|original|fmgen|opngen  OPNA 実装の選択 (既定: ymfm)\n"
		"  --out PATH                     WAV 出力先を指定 (--render / --play-effect /\n"
		"                                 --play-s98 / --record-s98 併用時の WAV に共通)\n"
		"  --sound-dat PATH               SOUND.DAT のパス指定 (省略時は MML 隣 → Materials/)\n"
		"  --max-seconds N                WAV / S98 出力を N 秒で打ち切る\n"
		"  --loop-count N                 MML の `{0` 無限ループの繰り返し回数\n"
		"                                 既定: リアルタイム再生時は 99 (ESC まで実質永続)、\n"
		"                                 オフライン出力時は 1 (2 回再生)。 N=0 でループなし\n"
		"  --help, -h                     このヘルプを表示\n"
		"\n"
		"デバッグ (`--debug SUBCMD [args...]`):\n"
		"  --debug dump-events N MML_PATH   ch N (1..6) のイベント列をテキストでダンプ\n"
		"  --debug dump-d-events MML_PATH   D パートのイベント列をダンプ\n"
		"  --debug dump-sound-dat PATH      SOUND.DAT のテーブル内容をダンプ\n"
		"  --debug render-ch N MML_PATH     指定 ch だけ残してオフライン WAV (mine_chN.wav)\n"
		"  --debug ymfm-test                ymfm 単体 FM テスト音 (ymfm_test.wav)\n"
		"  --debug ymfm-ssg-test            ymfm 単体 SSG テスト音 (ymfm_ssg_test.wav)\n"
		"  --debug ymfm-direct              ymfm 直接駆動 FM テスト (ymfm_direct.wav)\n"
	);
}

}  // namespace app
