#pragma once
//
// MmlScore: パース済み MML 全体を保持する。
//
// MUSIC.COM 仕様準拠で 6 チャンネル (1..3 = FM, 4..6 = SSG)。
// 各チャンネルは時刻順のイベント列。テンポは曲頭で T コマンドが
// 出てくるまで k_default_tempo_value を使う。
//

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "MmlEvent.h"
#include "MmlTimbre.h"

namespace mml {

constexpr int  k_channel_count    = 6;
constexpr int  k_fm_channel_count = 3;
constexpr int  k_ssg_channel_first = 3;  // インデックス 3..5 が SSG (1-origin で 4..6)
// T コマンド省略時の既定テンポ「値」(BPM ではなく T 値)。
// 実効 BPM は music_com_tempo_to_bpm(k_default_tempo_value) で求める。
constexpr int  k_default_tempo_value = 120;

struct Score {
	std::vector<Event> channels[k_channel_count];

	// D パート (隠しコマンド): 効果音トリガの時刻列。 MGS42/MGS45/DRMGLY 等で使用。
	// SSG ch A/B を SOUND.DAT 由来のシーケンスで占有するため、 通常の channels[3]/[4]
	// (= ch 4/5) とは別経路で管理する。
	std::vector<Event> d_part;

	// 全チャンネル中最大の終端時刻 (tick)。曲長判定に使う。
	int32_t total_ticks = 0;

	// SOUND: ブロックから集めた FM 音色プリセット。キーは @番号 (1..20)。
	std::unordered_map<int, FmPreset> fm_presets;

	// SSGENV: ブロックから集めた SSG ソフトウェアエンベロープ。キーは @番号 (1..20)。
	std::unordered_map<int, SsgEnvelope> ssg_envelopes;

	// パーサが拾った警告 (未対応コマンド等) を蓄積。エラーにはしない。
	std::vector<std::string> warnings;
};

}  // namespace mml
