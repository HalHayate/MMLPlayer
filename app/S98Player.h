#pragma once
//
// S98Player: S98 v1 ファイル (np21w で録音したリファレンス等) を ymfm で WAV にレンダする。
// `--play-s98 IN.s98 [--out PATH]` から呼ばれる。
//

namespace app {

// IN.s98 を読んで WAV (デフォルト mine_s98.wav、 out_path 指定で上書き) に出力。
// 戻り値 = main の終了コード (成功 0、 ファイル/フォーマットエラー 1)。
int run_play_s98(const char* in_path, const char* out_path);

}  // namespace app
