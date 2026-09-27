#pragma once
//
// DebugDumps: パーサや SOUND.DAT の中身を確認するためのテキストダンプ群。
// `--debug dump-d-events MML` / `--debug dump-sound-dat PATH` /
// `--debug dump-events N MML` から呼ばれる。
//

namespace mml { struct Score; }

namespace app {

// MML を読んで D パートのイベント列をダンプ。 戻り値 = main 終了コード。
int run_dump_d_events(const char* mml_path);

// SOUND.DAT を読んで内容をダンプ。 戻り値 = main 終了コード。
int run_dump_sound_dat(const char* sound_dat_path);

// dump-events モード: 指定 ch (1..6) のイベント列を時刻順にダンプ。
// MML パース後の score を受け取って呼ぶ (パース自体は main の通常経路を共用)。
void dump_channel_events(const mml::Score& score, int ch);

}  // namespace app
