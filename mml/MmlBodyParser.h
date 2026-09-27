#pragma once
//
// MmlBodyParser: 1 チャンネル分の MML 本文をイベント列へ変換する。
//
// 入力:
//   STR マクロ展開済み・繰り返しブロック展開済みの MML 文字列。
//   (本パーサ内部では $...$ や {...} は処理しない)
// 出力:
//   時刻 (tick) 付きの mml::Event 列を out に追記する。
//
// 状態:
//   現在オクターブ (O)、デフォルト音長 (L)、ゲート (Q)、現在 tick。
//   タイ (&) は内部でペンディングノートを保持して合算処理する。
//
// Phase 7 で実装するコマンド:
//   A-G (+/-、音長、&)、R、W、L、O、<、>、T、V、@、Q、N、{...} 展開済み
//
// Phase 7 では未実装 (識別だけして警告):
//   P、I、U、S、M、Y、その他
//

#include <cstdint>
#include <string>
#include <vector>

#include "MmlEvent.h"

namespace mml {

class BodyParser {
public:
	BodyParser() = default;

	// body をパースして out に追記する。
	// start_time から開始し、終了時刻 (次に書き込むべき tick) を返す。
	int32_t parse(const std::string& body, std::vector<Event>& out,
	              int32_t start_time, std::vector<std::string>& warn_out);

	// 状態を初期値に戻す (チャンネルをまたぐとき呼ぶ)。
	void reset();

private:
	// 内部で使う「保留中のノート」 (タイ処理用)。
	struct Pending {
		bool    has = false;
		int8_t  semitone = 0;
		int32_t duration_ticks = 0;
		// 異音タイ (スラー) で前ノートから繋がるとき true。 flush 時に Event.flags へ伝搬。
		bool    tied_from_prev = false;
	};

	// 文字操作ヘルパ。i は引数兼戻り値 (進める)。
	static void skip_ws(const std::string& s, size_t& i);
	static int  read_int(const std::string& s, size_t& i, bool& ok);
	int32_t     parse_length_ticks(const std::string& s, size_t& i,
	                               std::vector<std::string>& warn_out) const;

	// ペンディングノートを out に出力し、time を進める。
	// tied_to_next=true (直後にスラー `&` が続くノート) のときは Q ゲート短縮を
	// せず全音符長を key-on 期間にする。 これにより key-off tick が次タイノートの
	// 開始 tick に一致し、 シーケンサ側のタイ延長で key-off が発火しなくなる
	// (= レガートで音が途切れない。 SSG は tone disable が即無音なので必須)。
	void flush_pending(std::vector<Event>& out, int32_t& time, Pending& p,
	                   bool tied_to_next = false) const;

	// 状態。
	int  m_octave = 4;
	int  m_length = 4;  // L コマンドのデフォルト音長分母
	bool m_length_dotted = false;
	int  m_gate   = 8;  // Q コマンド (1..8)、key-on 比 n/8。MUSIC.COM 既定は Q8 (テンポ補正後 NP2 と一致)。

	// PitchOffset (N コマンド) はイベントとして発行する一方、
	// 直近の値を覚えておくと連続ノートに自動適用する流儀もある。
	// MUSIC.COM 仕様では「N コマンドが来たら以後の音程をずらす」
	// 状態的な意味合いなのでイベント化する (シーケンサ側で解釈)。
};

}  // namespace mml
