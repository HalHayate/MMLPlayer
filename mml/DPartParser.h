#pragma once
//
// DPartParser: MUSIC.COM 隠しコマンド `D:` パート専用パーサ。
//
// D パートは SSG ch A/B (index 0,1 / MUSIC.COM 1-origin 4,5) を SOUND.DAT 由来
// の効果音シーケンスで占有する特殊チャンネル。 通常の音符 A-G は MIDI 半音では
// なく「効果音番号 0-6 のトリガ」として解釈される (A=@0, B=@1, ... G=@6)。
//
// 有効: L (デフォルト音長)、 R/W (休符)、 < >、 A-G、 (STR マクロは展開済み)、
//       {...} ブロック (展開済み)。
// 無効: O, S, M, SSGENV, T, V, Q, N, I, U, P, @, Y (見つけたら警告して読み飛ばす)。
//
// 行境界 (改行) は意味を持つ: MUSIC.COM は 1 ソース行が最低 1 小節 (全音符) を
// 占有する。 行の実音長が全音符未満なら小節境界へ切り上げ (max(行長, 全音符))。
// MmlFileParser が D: 行を改行で連結して渡すので、 parse() は改行で切り上げる。
//
// MmlBodyParser とコードを共有しない。 仕様が独立しているため、 共通化すると
// 条件分岐だらけになるのを避ける。
//

#include <cstdint>
#include <string>
#include <vector>

#include "MmlEvent.h"

namespace mml {

class DPartParser {
public:
	DPartParser() = default;

	// body をパースして out に EffectTrigger / Rest イベントを追記する。
	// 戻り値 = 終了時刻 tick。
	int32_t parse(const std::string& body, std::vector<Event>& out,
	              int32_t start_time, std::vector<std::string>& warn_out);

	void reset();

private:
	static void skip_ws(const std::string& s, size_t& i);
	static int  read_int(const std::string& s, size_t& i, bool& ok);
	int32_t     parse_length_ticks(const std::string& s, size_t& i,
	                               std::vector<std::string>& warn_out) const;

	int  m_length        = 4;     // L コマンドのデフォルト音長分母
	bool m_length_dotted = false;
};

}  // namespace mml
