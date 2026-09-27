#pragma once
//
// MmlFileParser: MML テキストファイル全体を読んで mml::Score にまとめる。
//
// 1 行毎に行頭を見て分類:
//   ; …            コメント (無視)
//   1: 〜 6:        チャンネル本文 (本文部分を集めて MmlBodyParser に渡す)
//   STR:NAME$=…   マクロ定義 (MmlMacroTable に登録)
//   SOUND: / LFO: / OP1: / OP2: / OP3: / OP4: …   FM 音色定義 (Phase 8 で対応)
//   SSGENV: / -> …  SSG エンベロープ定義 (Phase 8 で対応)
//   D:             ドラムパート (Phase 10)
//
// 改行をまたぐ継続: STR: の右辺や SSGENV: が次行に続く場合あり。
// MUSICCOM.TXT 例:
//   STR:R1$=    $B$ C4 …
//   SSGENV:@1, 1, 15,14, …
//   ->          15,14,13, …
//
// Phase 7 では:
//   - チャンネル定義は「同じチャンネル番号で複数行ある場合は順に追記」
//   - STR: は単一行のみ対応 (改行継続は警告して無視)
//   - SOUND: 等は識別するが Score には反映せず警告だけ
//

#include <string>

#include "MmlMacroTable.h"
#include "MmlScore.h"

namespace mml {

class FileParser {
public:
	// テキスト全体をパースして score を生成する。
	// 既存の score 内容は上書きされる。
	void parse(const std::string& text, Score& score);

	// `{0 ... }` (= MUSIC.COM 無限ループ) を実際に何回展開するかの設定。
	// デフォルト 2 (= 1 回ループ後に終わる、 2 回再生)。 大きい値を指定すると
	// --record-s98 / --render で巨大な出力になるので、 オフライン用途で要注意。
	// 0 / 1 を指定すると 1 回再生のみ (ループなし) と等価。
	void set_infinite_loop_expansion(int count) {
		m_infinite_loop_count = (count < 1) ? 1 : count;
	}

private:
	// 行レベルの内部処理。
	// d_part_buffer は D: 行の本文を蓄積する別経路バッファ。
	void process_line(const std::string& raw_line, Score& score,
	                  std::string channel_buffers[k_channel_count],
	                  std::string& d_part_buffer);

	// 行頭セクション識別 (大文字小文字無視)。
	// 戻り値: マッチしたら true で、tag は識別したセクション名 (大文字化) と
	// rest は ':' の後の残り。
	static bool match_section(const std::string& line, std::string& tag,
	                          std::string& rest);

	// SOUND: 系のサブパース。パラメータ rest は ':' の後ろ部分。
	// 状態として m_current_timbre を持つことで、後続の LFO:/OP1..4: が
	// 「直近の SOUND: で指定された音色番号」のプリセットを更新できる。
	void process_sound(const std::string& rest, Score& score);
	void process_lfo(const std::string& rest, Score& score);
	void process_op(int op_index_0_based, const std::string& rest, Score& score);

	// SSGENV: 系。-> 行は m_continuation_ssg_env_number を頼りに直前の
	// SSGENV エンベロープの volumes 末尾に追記する。
	void process_ssgenv(const std::string& rest, Score& score);
	void process_ssgenv_continuation(const std::string& rest, Score& score);

	MacroTable m_macros;
	int        m_current_timbre = -1;  // -1 = 未指定 / 直近の SOUND:@N (N=0 もあり得る)
	int        m_continuation_ssg_env_number = 0;  // > 0 = -> がこのエンベロープに追記
	int        m_infinite_loop_count = 2;          // `{0` を展開する回数 (デフォルト 2 = 1 回ループ)
};

}  // namespace mml
