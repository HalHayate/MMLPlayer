//
// MmlBodyParser.cpp: 1 チャンネル分の MML 本文 → イベント列。
//

#include "MmlBodyParser.h"

#include <cctype>

namespace mml {

namespace {

// 音名 → C を 0 とする半音オフセット。
int note_letter_offset(char c) {
	switch (c) {
	case 'C': case 'c': return 0;
	case 'D': case 'd': return 2;
	case 'E': case 'e': return 4;
	case 'F': case 'f': return 5;
	case 'G': case 'g': return 7;
	case 'A': case 'a': return 9;
	case 'B': case 'b': return 11;
	}
	return -1;
}

// O4 C を MIDI 60 (中央のド) とする。半音番号 = 12*(オクターブ+1) + 音名オフセット + 半音記号
int compute_semitone(char letter, int accidental, int octave) {
	const int base = note_letter_offset(letter);
	if (base < 0) return -1;
	return 12 * (octave + 1) + base + accidental;
}

bool is_digit(char c) {
	return c >= '0' && c <= '9';
}

}  // namespace

void BodyParser::reset() {
	m_octave         = 4;
	m_length         = 4;
	m_length_dotted  = false;
	m_gate           = 8;  // MUSIC.COM 既定 Q8 (テンポ補正後 NP2 と一致)
}

void BodyParser::skip_ws(const std::string& s, size_t& i) {
	while (i < s.size()) {
		const char c = s[i];
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			++i;
		} else {
			break;
		}
	}
}

int BodyParser::read_int(const std::string& s, size_t& i, bool& ok) {
	skip_ws(s, i);
	bool negative = false;
	if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
		// 数値の符号として扱うのは N コマンドのみ。それ以外で見たら呼び出し側責任。
		if (s[i] == '-') negative = true;
		++i;
	}
	int value = 0;
	bool any = false;
	while (i < s.size() && is_digit(s[i])) {
		value = value * 10 + (s[i] - '0');
		any = true;
		++i;
	}
	ok = any;
	return negative ? -value : value;
}

// 長さ指定 (省略可、付点可) を tick 単位に変換する。
// 省略時は L で設定された値を使う。
int32_t BodyParser::parse_length_ticks(const std::string& s, size_t& i,
                                       std::vector<std::string>& warn_out) const {
	bool ok = false;
	const size_t start = i;
	const int n = read_int(s, i, ok);

	int   length = m_length;
	bool  dotted = m_length_dotted;

	if (ok) {
		if (n <= 0) {
			warn_out.push_back("invalid note length: " + std::to_string(n));
			i = start;
			return 0;
		}
		length = n;
		dotted = false;  // 明示的長さ指定では現在の付点デフォルトは引き継がない
	}

	// 付点 (.) チェック。連続する .. は double dot (1.75x) 等もあり得るが
	// MUSIC.COM 仕様には明記がないため Phase 7 では単一付点のみ対応。
	if (i < s.size() && s[i] == '.') {
		dotted = true;
		++i;
	}

	int32_t ticks = k_ticks_per_whole_note / length;
	if (dotted) {
		ticks = ticks * 3 / 2;
	}
	return ticks;
}

void BodyParser::flush_pending(std::vector<Event>& out, int32_t& time, Pending& p,
                               bool tied_to_next) const {
	if (!p.has) return;
	Event ev;
	ev.time_ticks = time;
	ev.kind       = EventKind::Note;
	ev.a          = p.semitone;
	ev.b          = p.duration_ticks;
	// ゲート (key-on 期間) = 全体長 × m_gate / 8。
	// ただし直後にスラー `&` が続くノートは Q 短縮せず全音符長にする
	// (次タイノートへ切れ目なく繋ぐ。 MUSIC.COM 実測: スラー中は tone を落とさない)。
	ev.c          = tied_to_next ? p.duration_ticks : (p.duration_ticks * m_gate / 8);
	if (ev.c < 1) ev.c = 1;
	if (p.tied_from_prev) ev.flags |= k_event_flag_tied_from_prev;
	out.push_back(ev);
	time += p.duration_ticks;
	p.has = false;
	p.tied_from_prev = false;
}

int32_t BodyParser::parse(const std::string& body, std::vector<Event>& out,
                          int32_t start_time, std::vector<std::string>& warn_out) {
	int32_t time = start_time;
	Pending pending;
	bool    tie_pending = false;
	// `&` が pending.has=false でも有効になり得るか (= 直前に少なくとも 1 つ
	// ノートが pending 化された)。 V/P/I 等のパラメータコマンドで flush 済みの
	// 状態でも、 直後の `&` でタイを継続するために必要。
	bool    had_any_pending = false;

	const size_t n = body.size();
	size_t i = 0;

	auto emit_simple = [&](EventKind k, int32_t a) {
		Event ev;
		ev.time_ticks = time;
		ev.kind = k;
		ev.a = a;
		out.push_back(ev);
	};

	while (i < n) {
		const char c = body[i];

		// ホワイトスペースは無視。
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			++i;
			continue;
		}

		// ---- 音符 (A-G) ----
		if (note_letter_offset(c) >= 0) {
			++i;
			// 半音記号 (+ / -)。複数連続する場合の挙動は仕様外。
			int accidental = 0;
			if (i < n && (body[i] == '+' || body[i] == '#')) { accidental = +1; ++i; }
			else if (i < n && body[i] == '-')                  { accidental = -1; ++i; }

			// 長さ。
			const int32_t dur = parse_length_ticks(body, i, warn_out);
			if (dur <= 0) continue;

			const int semi = compute_semitone(c, accidental, m_octave);

			if (tie_pending && pending.has && pending.semitone == semi) {
				// 同音タイ: 既存ペンディングを延長 (キーオンなし)。
				pending.duration_ticks += dur;
			} else if (tie_pending) {
				// 異音タイ (= スラー)。 タイと音符の間に P/V/@/O 等の
				// コマンドが挟まると既存ペンディングは既にフラッシュ済み
				// (pending.has=false) のことがあるため、 両ケースをここでまとめる。
				if (pending.has) {
					flush_pending(out, time, pending, tie_pending);
				}
				pending.has = true;
				pending.semitone = static_cast<int8_t>(semi);
				pending.duration_ticks = dur;
				pending.tied_from_prev = true;
			} else {
				// タイなし: 既存をフラッシュして新規ペンディングへ。
				flush_pending(out, time, pending, tie_pending);
				pending.has = true;
				pending.semitone = static_cast<int8_t>(semi);
				pending.duration_ticks = dur;
				pending.tied_from_prev = false;
			}
			had_any_pending = true;
			tie_pending = false;
			continue;
		}

		// ---- 休符 R / 持続休符 W ----
		if (c == 'R' || c == 'r' || c == 'W' || c == 'w') {
			const bool is_w = (c == 'W' || c == 'w');
			++i;
			const int32_t dur = parse_length_ticks(body, i, warn_out);
			if (dur <= 0) continue;
			flush_pending(out, time, pending, tie_pending);
			tie_pending = false;
			Event ev;
			ev.time_ticks = time;
			ev.kind = EventKind::Rest;
			ev.a = dur;
			ev.c = is_w ? 1 : 0;  // c=1 で W の意 (シーケンサ側で参照可)
			out.push_back(ev);
			time += dur;
			continue;
		}

		// ---- タイ & ----
		if (c == '&') {
			++i;
			// pending.has 中はもちろん、 V/P/I 等で pending が flush 済みでも
			// 「直前に出したノートと次ノートをタイで結ぶ」 用例 (YAG03 等) に対応。
			// 異音タイ分岐 (else if tie_pending) が pending.has=false でも
			// tied_from_prev=true で新規ペンディングを作るので安全。
			if (pending.has || had_any_pending) {
				tie_pending = true;
			} else {
				warn_out.push_back("'&' without preceding note");
			}
			continue;
		}

		// ---- L (デフォルト音長) ----
		if (c == 'L' || c == 'l') {
			++i;
			bool ok = false;
			const int v = read_int(body, i, ok);
			if (!ok || v <= 0) {
				warn_out.push_back("L: missing length");
			} else {
				m_length = v;
				m_length_dotted = false;
				if (i < n && body[i] == '.') {
					m_length_dotted = true;
					++i;
				}
			}
			continue;
		}

		// ---- O (オクターブ) ----
		if (c == 'O' || c == 'o') {
			++i;
			bool ok = false;
			const int v = read_int(body, i, ok);
			if (!ok || v < 1 || v > 8) {
				warn_out.push_back("O: octave out of range 1..8");
			} else {
				m_octave = v;
			}
			continue;
		}

		// ---- < / > (オクターブ相対) ----
		// MUSIC.COM マニュアルでは O1〜O8 だが、AGM01/COP02 では O1 から更に `<` で
		// 下げる MML が存在し、NP2 実機録音とも一致するため範囲制限は外す。
		if (c == '<') { --m_octave; ++i; continue; }
		if (c == '>') { ++m_octave; ++i; continue; }

		// ---- T (テンポ) ---- tie_pending は維持 (他 param 系と同様)。
		if (c == 'T' || c == 't') {
			++i;
			bool ok = false;
			const int v = read_int(body, i, ok);
			if (ok && v > 0) {
				flush_pending(out, time, pending, tie_pending);
				emit_simple(EventKind::Tempo, v);
			} else {
				warn_out.push_back("T: missing tempo");
			}
			continue;
		}

		// ---- V (音量) ----
		// `& V12 note` の用例 (YAG03 等) でタイを維持するため、 tie_pending は触らない。
		// 時間を消費するコマンド (R/W/note) のみ tie_pending を確定的に消費する。
		if (c == 'V' || c == 'v') {
			++i;
			bool ok = false;
			const int v = read_int(body, i, ok);
			// MUSIC.COM の V は 0..16 (musiccom_emu 実測)。V16 が FM 最大音量
			// (キャリア TL = パッチ値、offset 0)、V15 で offset 4。INT33 等で
			// V16 を多用する。SSG (4bit) では適用側で 15 にクランプする。
			if (ok && v >= 0 && v <= 16) {
				flush_pending(out, time, pending, tie_pending);
				emit_simple(EventKind::Volume, v);
			} else {
				warn_out.push_back("V: volume out of range 0..16");
			}
			continue;
		}

		// ---- @ (音色) ---- tie_pending は維持 (V と同様)。
		if (c == '@') {
			++i;
			bool ok = false;
			const int v = read_int(body, i, ok);
			if (ok && v >= 0 && v <= 20) {
				flush_pending(out, time, pending, tie_pending);
				emit_simple(EventKind::Timbre, v);
			} else {
				warn_out.push_back("@: timbre out of range 0..20");
			}
			continue;
		}

		// ---- Q (ゲート率) ---- tie_pending は維持。
		if (c == 'Q' || c == 'q') {
			++i;
			// MUSIC.COM 仕様: Q コマンドの引数は 1 桁固定 (0..8)。
			// 複数桁を貪欲に読むと "Q7O1" を意図した "Q701" のような誤記入
			// (B5_PPP.MML 等) で後続の O コマンドまで食ってしまうため、
			// ここでは 1 文字のみ取り出す。
			skip_ws(body, i);
			bool ok = false;
			int v = 0;
			if (i < body.size() && is_digit(body[i])) {
				v = body[i] - '0';
				++i;
				ok = true;
			}
			// Q0 は最強 staccato (実機ドライバも key-on 直後 key-off)。
			// flush_pending 側で gate=0 は 1 tick にクランプされるため安全。
			if (ok && v >= 0 && v <= 8) {
				// Q は「以降のノート」に適用されるべき (MUSIC.COM 仕様)。
				// flush は m_gate 変更「前」に行い、 直前 pending は古い m_gate で出力する。
				flush_pending(out, time, pending, tie_pending);
				m_gate = v;
				emit_simple(EventKind::GateTime, v);
			} else {
				warn_out.push_back("Q: gate out of range 0..8");
			}
			continue;
		}

		// ---- N (音程シフト) ---- tie_pending は維持。
		if (c == 'N' || c == 'n') {
			++i;
			bool ok = false;
			const int v = read_int(body, i, ok);
			if (ok && v >= -255 && v <= 255) {
				flush_pending(out, time, pending, tie_pending);
				emit_simple(EventKind::PitchOffset, v);
			} else {
				warn_out.push_back("N: pitch offset out of range -255..255");
			}
			continue;
		}

		// ---- I (ビブラート) U (トレモロ): 共通形 num1,num2[,num3] ----
		if (c == 'I' || c == 'i' || c == 'U' || c == 'u') {
			const EventKind kind = (c == 'I' || c == 'i') ? EventKind::Vibrato
			                                              : EventKind::Tremolo;
			++i;
			// MUSIC.COM は num1 直前のカンマを区切りとして読み飛ばす。
			// `I,50,3,16` は `I50,3,16` (振幅50/周期3/ディレイ16) と等価
			// (musiccom_emu の S98 がバイト一致で確認済)。
			skip_ws(body, i);
			if (i < n && body[i] == ',') ++i;
			bool ok1 = false;
			const int amp = read_int(body, i, ok1);
			if (!ok1) {
				warn_out.push_back(std::string(kind == EventKind::Vibrato ? "I" : "U") +
				                   ": missing first parameter");
				continue;
			}
			int period = 0;
			int delay  = 0;
			skip_ws(body, i);
			if (i < n && body[i] == ',') {
				++i;
				bool ok2 = false;
				period = read_int(body, i, ok2);
				skip_ws(body, i);
				if (i < n && body[i] == ',') {
					++i;
					bool ok3 = false;
					delay = read_int(body, i, ok3);
				}
			}
			// I/U はタイの途中で挟まれることが多い (`& I50,4,8 F4` 等)。
			// tie_pending は維持して、 次ノートでタイを継続させる。
			flush_pending(out, time, pending, tie_pending);
			Event ev;
			ev.time_ticks = time;
			ev.kind       = kind;
			ev.a = amp;
			ev.b = period;
			ev.c = delay;
			out.push_back(ev);
			continue;
		}

		// ---- S (SSG HW エンベロープ形状) ---- tie_pending は維持。
		if (c == 'S' || c == 's') {
			++i;
			bool ok = false;
			const int v = read_int(body, i, ok);
			if (!ok || v < 0 || v > 15) {
				warn_out.push_back("S: shape out of range 0..14");
				continue;
			}
			flush_pending(out, time, pending, tie_pending);
			Event ev;
			ev.time_ticks = time;
			ev.kind       = EventKind::SsgHwEnvelopeShape;
			ev.a = v;
			out.push_back(ev);
			continue;
		}

		// ---- M (SSG HW エンベロープ周期) ---- tie_pending は維持。
		if (c == 'M' || c == 'm') {
			++i;
			bool ok = false;
			const int v = read_int(body, i, ok);
			if (!ok || v < 0 || v > 65535) {
				warn_out.push_back("M: period out of range 0..65535");
				continue;
			}
			flush_pending(out, time, pending, tie_pending);
			Event ev;
			ev.time_ticks = time;
			ev.kind       = EventKind::SsgHwEnvelopePeriod;
			ev.a = v;
			out.push_back(ev);
			continue;
		}

		// ---- P (ポルタメント): num のみ ----
		// P は & (タイ) と次音符の間に挟まれることが多いので、
		// tie_pending は維持する (削除しない)。 pending は flush して
		// 次音符が「タイあり」 で発行されるようにする。
		if (c == 'P' || c == 'p') {
			++i;
			bool ok = false;
			const int dur = read_int(body, i, ok);
			if (!ok) {
				warn_out.push_back("P: missing duration");
				continue;
			}
			flush_pending(out, time, pending, tie_pending);
			Event ev;
			ev.time_ticks = time;
			ev.kind       = EventKind::Portamento;
			ev.a = dur;
			out.push_back(ev);
			continue;
		}

		// ---- Y (FM/SSG レジスタ直接書き込み): num1, num2 ----
		if (c == 'Y' || c == 'y') {
			++i;
			bool ok1 = false, ok2 = false;
			const int addr = read_int(body, i, ok1);
			int val = 0;
			if (ok1) {
				skip_ws(body, i);
				if (i < n && body[i] == ',') {
					++i;
					val = read_int(body, i, ok2);
				}
			}
			if (!ok1 || !ok2) {
				warn_out.push_back("Y: expected 'Y addr,val'");
				continue;
			}
			// Y は「現在時刻 (= 直前ノート終了 / 次ノート開始位置)」 に作用させたい。
			// 既存ペンディングをフラッシュしてから発行する (タイ継続中なら維持)。
			flush_pending(out, time, pending, tie_pending);
			Event ev;
			ev.time_ticks = time;
			ev.kind = EventKind::YRegWrite;
			ev.a = addr & 0xFF;
			ev.b = val  & 0xFF;
			out.push_back(ev);
			continue;
		}

		// ---- 未対応コマンド (S/M 等) を識別だけしてスキップ ----
		// これらはアルファベット 1 文字 + 数値 (場合によりカンマ区切り複数) を取る。
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
			warn_out.push_back(std::string("unsupported command (skipped): ") + c);
			++i;
			// 続く数値 + カンマ区切りパラメータを読み飛ばす。
			while (i < n) {
				skip_ws(body, i);
				bool ok = false;
				read_int(body, i, ok);
				if (!ok) break;
				if (i < n && body[i] == ',') { ++i; continue; }
				break;
			}
			continue;
		}

		// ---- それ以外の予期せぬ文字 ----
		warn_out.push_back(std::string("skipped unexpected character: ") + c);
		++i;
	}

	// 末尾でペンディングがあればフラッシュ。
	flush_pending(out, time, pending, tie_pending);
	return time;
}

}  // namespace mml
