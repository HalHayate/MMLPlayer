//
// MmlMacroTable.cpp: STR: マクロの登録と展開。
//

#include "MmlMacroTable.h"

namespace mml {

namespace {
// $...$ で囲まれた識別子の文字種。英数字 + アンダースコア。
bool is_macro_name_char(char c) {
	return (c >= '0' && c <= '9') ||
	       (c >= 'A' && c <= 'Z') ||
	       (c >= 'a' && c <= 'z') ||
	       c == '_';
}
}  // namespace

bool MacroTable::define(const std::string& name, const std::string& body,
                        std::vector<std::string>& error_out) {
	// 定義時点で既存マクロを展開して固定化する (前方参照禁止仕様の実装)。
	const std::string expanded = expand(body, error_out);
	m_table[name] = expanded;
	return true;
}

std::string MacroTable::expand(const std::string& text,
                               std::vector<std::string>& error_out) const {
	std::string out;
	out.reserve(text.size());

	const size_t n = text.size();
	for (size_t i = 0; i < n; ) {
		if (text[i] != '$') {
			out.push_back(text[i]);
			++i;
			continue;
		}

		// '$' 検出: 識別子 → 終端 '$' を探す。
		size_t j = i + 1;
		while (j < n && is_macro_name_char(text[j])) {
			++j;
		}
		if (j == n || text[j] != '$' || j == i + 1) {
			// '$' が単独 or 名前なし or 終端 '$' なし → そのまま出力 (壊さない)。
			out.push_back('$');
			++i;
			continue;
		}

		const std::string name = text.substr(i + 1, j - i - 1);
		const auto it = m_table.find(name);
		if (it == m_table.end()) {
			error_out.push_back("undefined macro: $" + name + "$");
			// 解決不可: 元の表記を残す。
			out.append(text, i, j - i + 1);
		} else {
			// 既に define() 時に再帰展開済みのため、単純に挿入。
			out.append(it->second);
		}
		i = j + 1;
	}
	return out;
}

}  // namespace mml
