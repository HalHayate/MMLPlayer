#pragma once
//
// Render: オフライン WAV レンダリングとリアルタイム再生待機ループを提供。
//
// render_to_wav は決定論的レンダ (worker スレッド非依存) で WAV/S98 出力に使う。
// play_until_esc_or_finish は ESC キー (GetAsyncKeyState) で停止できる
// リアルタイム再生用ループ。
//

#include <atomic>
#include <cstdint>
#include <string>

class IOpnaChip;
namespace mml { class Player; }

namespace app {

// オフライン再生パイプラインを回す。
// wav_path が空文字列のときは WAV を書き出さず、 ymfm 内部時計を進めるためだけに
// opna.render() を呼ぶ (S98 録音時に必要)。
// normalize=true で -3 dBFS にピーク正規化 (フルミックス WAV を聞きやすい音量に補正)。
// ch isolate モード等で REF と相対音量を保ちたい場合は false。
void render_to_wav(const std::string& wav_path, double seconds,
                   uint32_t sample_rate, IOpnaChip& opna, mml::Player& player,
                   bool normalize = true);

// 曲が自然終了するか ESC が押されるまでブロック (worker スレッド再生用)。
void play_until_esc_or_finish(mml::Player& player);

// done が true になるか ESC が押されるまでブロック (オフライン API 再生用)。
void wait_until_esc_or_flag(const std::atomic<bool>& done);

// 指定 ms スリープ (リアルタイム待機ループ等の細粒度ウェイト用)。
void sleep_ms(int ms);

}  // namespace app
