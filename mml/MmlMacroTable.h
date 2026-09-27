#pragma once
//
// MmlMacroTable: STR: マクロの名前 → 展開文字列の表。
//
// MUSIC.COM 仕様:
//   STR:NAME$=<MML…>
//   $NAME$ で呼び出し。
//   ネストしたマクロ参照は「呼び出される側が先に定義済み」が必須
//   (= 前方参照禁止)。実装上は define() 時に既存マクロを即時展開して
//   永続化することで、参照時には単純な置換で済む。
//
// 循環参照は仕様上発生し得ないが、保険として展開深さに上限を設ける。
//

#include <string>
#include <unordered_map>
#include <vector>

namespace mml {

class MacroTable {
public:
	MacroTable() = default;

	// STR: の右辺を、現在登録済みのマクロを展開した結果として登録する。
	// 戻り値: 成功 true。失敗時 (未定義マクロ参照) は警告を error_out に積む。
	bool define(const std::string& name, const std::string& body,
	            std::vector<std::string>& error_out);

	// テキストに含まれる $NAME$ を展開した結果を返す。
	// 未定義マクロは元の $NAME$ のまま残し警告を出す。
	std::string expand(const std::string& text,
	                   std::vector<std::string>& error_out) const;

	bool contains(const std::string& name) const {
		return m_table.find(name) != m_table.end();
	}

	void clear() { m_table.clear(); }

private:
	std::unordered_map<std::string, std::string> m_table;
};

}  // namespace mml
