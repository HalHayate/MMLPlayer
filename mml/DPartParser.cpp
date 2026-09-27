//
// DPartParser.cpp: D: パート専用パーサ。
//

#include "DPartParser.h"

namespace mml {

namespace {

// A-G → 効果音番号 0..6。 H-Z は範囲外 (warn)。
int letter_to_effect_id(char c) {
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a';
	return -1;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

void DPartParser::reset() {
	m_length        = 4;
	m_length_dotted = false;
}

void DPartParser::skip_ws(const std::string& s, size_t& i) {
	while (i < s.size()) {
		const char c = s[i];
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++i;
		else break;
	}
}

int DPartParser::read_int(const std::string& s, size_t& i, bool& ok) {
	skip_ws(s, i);
	int value = 0;
	bool any = false;
	while (i < s.size() && is_digit(s[i])) {
		value = value * 10 + (s[i] - '0');
		any = true;
		++i;
	}
	ok = any;
	return value;
}

int32_t DPartParser::parse_length_ticks(const std::string& s, size_t& i,
                                        std::vector<std::string>& warn_out) const {
	bool ok = false;
	const size_t start = i;
	const int n = read_int(s, i, ok);

	int  length = m_length;
	bool dotted = m_length_dotted;

	if (ok) {
		if (n <= 0) {
			warn_out.push_back("D: invalid note length: " + std::to_string(n));
			i = start;
			return 0;
		}
		length = n;
		dotted = false;
	}
	if (i < s.size() && s[i] == '.') {
		dotted = true;
		++i;
	}
	int32_t ticks = k_ticks_per_whole_note / length;
	if (dotted) ticks = ticks * 3 / 2;
	return ticks;
}

int32_t DPartParser::parse(const std::string& body, std::vector<Event>& out,
                           int32_t start_time, std::vector<std::string>& warn_out) {
	int32_t time = start_time;
	// MUSIC.COM の D: パートは 1 ソース行が最低 1 小節 (全音符) を占有する。
	// 行の実音長が全音符未満なら全音符へ切り上げ、 全音符以上なら literal のまま
	// (= max(行長, 全音符)。 グリッドスナップではない。 実機エミュのプローブで確定:
	//  「C4」 1 拍→4 拍・ 「R2R1」 6 拍→6 拍)。 音符/休符の無い行 (設定行 T135 等) は
	// 時間を進めないのでパディング対象外。 line_start で行頭時刻を追う。
	int32_t line_start = start_time;
	const size_t n = body.size();
	size_t i = 0;

	while (i < n) {
		const char c = body[i];

		// 行境界 (改行): 直前行を小節境界へ切り上げる。
		if (c == '\n') {
			const int32_t advanced = time - line_start;
			if (advanced > 0 && advanced < k_ticks_per_whole_note) {
				time = line_start + k_ticks_per_whole_note;
			}
			line_start = time;
			++i;
			continue;
		}

		if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }

		// 休符 (R / W)。 通常チャンネルの R と同じく時刻だけ進める。
		if (c == 'R' || c == 'r' || c == 'W' || c == 'w') {
			++i;
			const int32_t dur = parse_length_ticks(body, i, warn_out);
			if (dur <= 0) continue;
			Event ev;
			ev.time_ticks = time;
			ev.kind       = EventKind::Rest;
			ev.a          = dur;
			out.push_back(ev);
			time += dur;
			continue;
		}

		// L (デフォルト音長)。
		if (c == 'L' || c == 'l') {
			++i;
			bool ok = false;
			const int v = read_int(body, i, ok);
			if (!ok || v <= 0) {
				warn_out.push_back("D: L: missing length");
			} else {
				m_length        = v;
				m_length_dotted = false;
				if (i < n && body[i] == '.') { m_length_dotted = true; ++i; }
			}
			continue;
		}

		// A-G: 効果音トリガ。 +/- (半音記号) はあれば読み飛ばす (仕様上意味なし)。
		const int effect_id = letter_to_effect_id(c);
		if (effect_id >= 0 && effect_id <= 6) {
			++i;
			if (i < n && (body[i] == '+' || body[i] == '-' || body[i] == '#')) ++i;
			const int32_t dur = parse_length_ticks(body, i, warn_out);
			if (dur <= 0) continue;
			Event ev;
			ev.time_ticks = time;
			ev.kind       = EventKind::EffectTrigger;
			ev.a          = effect_id;
			ev.b          = dur;
			out.push_back(ev);
			time += dur;
			continue;
		}

		// T (テンポ): D パートにも書ける (DRMGLY.MML 冒頭で T135 を D 行に書く例あり)。
		// 楽曲全体に効くので Tempo イベントを発行する。
		if (c == 'T' || c == 't') {
			++i;
			bool ok = false;
			const int v = read_int(body, i, ok);
			if (ok && v > 0) {
				Event ev;
				ev.time_ticks = time;
				ev.kind       = EventKind::Tempo;
				ev.a          = v;
				out.push_back(ev);
			}
			continue;
		}

		// 仕様で「無効」と明記されている O/S/M/SSGENV、 および効果音には影響しない
		// V/N/Q/I/U/P/@/Y 系はパラメータを食って黙って捨てる (警告も出さない)。
		// 数字付きコマンドのつもりで読みすぎても害がないように +/- も食う。
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '@') {
			++i;
			if (i < n && (body[i] == '+' || body[i] == '-')) ++i;
			bool dummy = false;
			read_int(body, i, dummy);
			// 連続パラメータ (例: I a,b,c) のカンマ区切りも食う。
			while (i < n && (body[i] == ',' || body[i] == ' ' || body[i] == '\t')) {
				if (body[i] == ',') {
					++i;
					if (i < n && (body[i] == '+' || body[i] == '-')) ++i;
					read_int(body, i, dummy);
				} else {
					++i;
				}
			}
			continue;
		}

		// 記号類 (< > その他) は単純に読み飛ばす。
		++i;
	}

	return time;
}

}  // namespace mml
