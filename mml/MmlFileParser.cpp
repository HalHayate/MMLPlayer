//
// MmlFileParser.cpp: MML ファイル全体パーサ。
//

#include "MmlFileParser.h"

#include <cctype>
#include <cmath>
#include <cstdio>   // std::snprintf
#include <vector>

#include "MmlBodyParser.h"
#include "DPartParser.h"

namespace mml {

namespace {

// 大文字化 (ASCII のみ)。
char to_upper(char c) {
	if (c >= 'a' && c <= 'z') return static_cast<char>(c - 'a' + 'A');
	return c;
}

std::string to_upper_copy(const std::string& s) {
	std::string r;
	r.reserve(s.size());
	for (char c : s) r.push_back(to_upper(c));
	return r;
}

// 文字列の前後の空白を除く。
std::string trim(const std::string& s) {
	size_t a = 0;
	while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
	size_t b = s.size();
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
	return s.substr(a, b - a);
}

bool starts_with_ci(const std::string& s, size_t pos, const std::string& prefix) {
	if (pos + prefix.size() > s.size()) return false;
	for (size_t i = 0; i < prefix.size(); ++i) {
		if (to_upper(s[pos + i]) != to_upper(prefix[i])) return false;
	}
	return true;
}

// "1,2, -3 ,4" のようなカンマ/空白区切りの整数列を読む。
// 区切りは ','、' '、'\t' のいずれも許容 (AGM01.MML 等で空白だけのケースがある)。
// 末尾カンマや余計な空白も許容する。
std::vector<int> parse_int_list(const std::string& s) {
	std::vector<int> out;
	const size_t n = s.size();
	size_t i = 0;
	while (i < n) {
		// 区切りや空白をスキップ。
		while (i < n && (s[i] == ',' || s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) {
			++i;
		}
		if (i >= n) break;

		// 符号 + 数値。
		int sign = 1;
		if (s[i] == '+') { ++i; }
		else if (s[i] == '-') { sign = -1; ++i; }

		if (i >= n || s[i] < '0' || s[i] > '9') {
			// 数字でないものが現れたら終了 (コメント文字 ';' 等)。
			break;
		}
		int v = 0;
		while (i < n && s[i] >= '0' && s[i] <= '9') {
			v = v * 10 + (s[i] - '0');
			++i;
		}
		out.push_back(sign * v);
	}
	return out;
}

// ---- OPN raw 値 → 物理量への換算 (実機準拠) ------------------------
//
// YM2608 データシート + fmgen 等の実測値をフィットさせた式:
//   T_AR(AR)  = 30.0 × 2^((4 - AR) / 2)  ※AR=4 で 30s、+2 ごとに 1/2
//   T_DR(DR)  = 48.0 × 2^((4 - DR) / 2)  ※DR=4 で 48s、+2 ごとに 1/2
//   T_RR(RR)  = T_DR(2×RR + 1)            ※RR は 4bit、effective rate に変換
// これにより 既知の OPN タイミング表 (AR=24→30ms, AR=20→120ms, AR=16→481ms,
// AR=12→1.93s, AR=8→7.7s, AR=4→30.7s, AR=31→0.94ms) と整合する。

float convert_ar(int ar) {
	if (ar <= 0)  return 1000.0f;  // 実質無限 (アタックしない)
	if (ar > 31)  ar = 31;
	return 30.0f * std::pow(2.0f, (4.0f - static_cast<float>(ar)) / 2.0f);
}

float convert_dr(int dr) {
	if (dr <= 0)  return 1000.0f;  // 実質無限 (減衰しない)
	if (dr > 31)  dr = 31;
	// AR と同じ 30s 基準。実機表 (DR=4→30s, DR=8→7.7s, DR=12→1.93s) に整合。
	// 旧 48s 基準だと約 1.6× 遅く、Release/Decay tail が長引いて
	// AGM01 等で「電子音が間延び」して聴こえる原因になっていた。
	return 30.0f * std::pow(2.0f, (4.0f - static_cast<float>(dr)) / 2.0f);
}

float convert_sr(int sr) {
	// Sustain Rate (= Decay 2 Rate) は DR と同じレート表に従う。
	return convert_dr(sr);
}

float convert_rr(int rr) {
	if (rr <= 0)  return 1000.0f;
	if (rr > 15)  rr = 15;
	// 実機 OPN: effective rate = RR << 2 = 4 × RR (KS=0 時)。
	// AR/DR の eff_rate と同じスケールで計算。
	// AGM01 のような short staccato 譜面で release tail が ~20ms に抑えられ
	// 「タンタン」歯切れが出るのに必要な速度。
	const int eff = 4 * rr;  // 0..60
	if (eff <= 0) return 1000.0f;
	return 30.0f * std::pow(2.0f, (4.0f - static_cast<float>(eff)) / 2.0f);
}

// SL: 4bit (0..15)。実機は 1 ステップ -3dB 減衰、15 は -93dB (= 完全無音扱い)。
// 線形ゲイン (0..1) に変換。AGM01 PIANO の SL=12 → -36dB → 0.016 のように
// 「ほぼ無音」に近い値が出るのが正しい挙動 (D2R へ早く落ちる)。
float convert_sl(int sl) {
	if (sl <= 0)  return 1.0f;
	if (sl >= 15) return 0.0f;
	const float db = static_cast<float>(sl) * 3.0f;
	return std::pow(10.0f, -db / 20.0f);
}

// TL: 0..127、1 ステップ 0.75 dB。
float convert_tl(int tl) {
	return static_cast<float>(tl) * 0.75f;
}

// DT: 3-bit。bit2 = 符号、bit0..1 = 大きさ。1 ステップ ~5 cents の近似。
float convert_dt_to_cents(int dt) {
	const int sign = (dt & 0x4) ? -1 : 1;
	const int mag  = dt & 0x3;
	return static_cast<float>(sign * mag) * 5.0f;
}

// 後続パース用に「STR:NAME$=BODY」を解析。成功時 name と body を返す。
bool parse_str_definition(const std::string& after_colon,
                          std::string& name, std::string& body) {
	// 受け入れ形式:
	//   "NAME$=BODY"  ... MUSIC.COM 正式表記
	//   "NAME =BODY"  ... DANTE98II 系で見られる '$' 省略形 (FANTASIA.MML 等)
	// どちらでもマクロ参照は "$NAME$" を使うので、名前自体に '$' は含まない。
	const size_t eq = after_colon.find('=');
	if (eq == std::string::npos) return false;
	// '=' より前の最後の非空白文字までを「名前候補」 とする。 末尾の '$' は MUSIC.COM
	// 表記なので削る。 中間に空白以外があれば不正。
	size_t name_end = eq;
	while (name_end > 0 && (after_colon[name_end - 1] == ' ' ||
	                        after_colon[name_end - 1] == '\t')) --name_end;
	if (name_end > 0 && after_colon[name_end - 1] == '$') --name_end;
	size_t name_begin = 0;
	while (name_begin < name_end && (after_colon[name_begin] == ' ' ||
	                                 after_colon[name_begin] == '\t')) ++name_begin;
	name = after_colon.substr(name_begin, name_end - name_begin);
	body = after_colon.substr(eq + 1);
	return !name.empty();
}

}  // namespace

bool FileParser::match_section(const std::string& line, std::string& tag, std::string& rest) {
	// 行頭の英数字 + ':' を識別。'1:' から '6:'、'STR:'、'SOUND:'、'OP1:' 等。
	// MUSIC.COM のチャンネル番号は 1〜6 のみだが、この関数は汎用識別。
	size_t i = 0;
	while (i < line.size()) {
		const char c = line[i];
		if ((c >= '0' && c <= '9') ||
		    (c >= 'A' && c <= 'Z') ||
		    (c >= 'a' && c <= 'z')) {
			++i;
		} else {
			break;
		}
	}
	if (i == 0 || i >= line.size() || line[i] != ':') return false;
	tag  = to_upper_copy(line.substr(0, i));
	rest = line.substr(i + 1);
	return true;
}

void FileParser::process_line(const std::string& raw_line, Score& score,
                              std::string channel_buffers[k_channel_count],
                              std::string& d_part_buffer) {
	// 空行・コメント。
	if (raw_line.empty()) return;
	if (raw_line[0] == ';') return;

	// 純粋な空白だけの行も無視。
	bool only_ws = true;
	for (char c : raw_line) {
		if (c != ' ' && c != '\t' && c != '\r') { only_ws = false; break; }
	}
	if (only_ws) return;

	// 継続行 (-> で始まる): 直前の SSGENV: の volumes に追記する。
	const std::string trimmed_lead = trim(raw_line);
	if (trimmed_lead.size() >= 2 && trimmed_lead[0] == '-' && trimmed_lead[1] == '>') {
		process_ssgenv_continuation(trimmed_lead.substr(2), score);
		return;
	}

	std::string tag;
	std::string rest;
	// 行頭の空白を許容するため、トリム済み行で識別する (BGM10.MML の " SOUND: @12" 等)。
	if (!match_section(trimmed_lead, tag, rest)) {
		// チャンネルでもセクションでもない行: 警告。
		score.warnings.push_back("unrecognized line ignored: " + trimmed_lead);
		return;
	}

	// チャンネル番号 (1..6)。
	if (tag.size() == 1 && tag[0] >= '1' && tag[0] <= '6') {
		const int ch = tag[0] - '1';  // 0-origin
		channel_buffers[ch].append(rest);
		channel_buffers[ch].push_back(' ');  // 行末区切り (空白で十分)
		// チャンネル定義行が来たら SSGENV 継続コンテキストを終了。
		m_continuation_ssg_env_number = 0;
		return;
	}

	// STR: マクロ定義。
	if (tag == "STR") {
		std::string name, body;
		if (!parse_str_definition(rest, name, body)) {
			score.warnings.push_back("malformed STR: " + trimmed_lead);
			return;
		}
		m_macros.define(name, body, score.warnings);
		m_continuation_ssg_env_number = 0;
		return;
	}

	// FM 音色定義。
	if (tag == "SOUND") {
		process_sound(rest, score);
		m_continuation_ssg_env_number = 0;
		return;
	}
	if (tag == "LFO")   { process_lfo(rest, score);   return; }
	if (tag == "OP1")   { process_op(0, rest, score); return; }
	if (tag == "OP2")   { process_op(1, rest, score); return; }
	if (tag == "OP3")   { process_op(2, rest, score); return; }
	if (tag == "OP4")   { process_op(3, rest, score); return; }

	// SSGENV: ソフトウェアエンベロープ。
	if (tag == "SSGENV") {
		process_ssgenv(rest, score);
		return;
	}

	// D パート: 行本文を専用バッファに追記 (BodyParser ではなく DPartParser で処理)。
	// 行区切りは改行で残す。 MUSIC.COM の D: パートは「1 ソース行 = 最低 1 小節
	// (全音符)」 として扱う (実機エミュ実測。 短い行は小節へ切り上げ) ため、 行境界を
	// DPartParser に伝える必要がある。 改行はマクロ展開・ループ展開とも素通りする。
	if (tag == "D") {
		d_part_buffer.append(rest);
		d_part_buffer.push_back('\n');
		m_continuation_ssg_env_number = 0;
		return;
	}

	score.warnings.push_back("unknown section: " + tag);
	m_continuation_ssg_env_number = 0;
}

void FileParser::process_sound(const std::string& rest, Score& score) {
	// 期待: "@N" (任意の前後空白あり)。'@' は任意で、 MUSIC.COM は `SOUND:3` を
	// `SOUND:@3` と同一に扱う (musiccom_emu の S98 がバイト一致で確認済)。
	size_t i = 0;
	const size_t n = rest.size();
	while (i < n && (rest[i] == ' ' || rest[i] == '\t')) ++i;
	if (i < n && rest[i] == '@') ++i;
	while (i < n && (rest[i] == ' ' || rest[i] == '\t')) ++i;
	int v = 0;
	bool any = false;
	while (i < n && rest[i] >= '0' && rest[i] <= '9') {
		v = v * 10 + (rest[i] - '0');
		any = true;
		++i;
	}
	if (!any || v < 0 || v > 31) {
		// MUSIC.COM 仕様上は 1..20 だが DANTE98II 系は @0 から始まる。
		// 上限も実装上は固定不要 (我々の Player は fm_presets を std::map で持つため)。
		score.warnings.push_back("SOUND: number out of range 0..31");
		m_current_timbre = -1;
		return;
	}
	// @N の N=0 はそのまま扱いたいが、 m_current_timbre==0 は 「未設定」 マーカーとして
	// 使っているため、 内部的に「+1 シフトされた値」 でなく「-1 = 未設定、 >=0 = N」 へ
	// 切り替える必要がある。 ここでは簡便化のため m_current_timbre は v にし、 後段の
	// LFO/OP ハンドラの「未設定」 判定を 「m_current_timbre < 0」 に統一する。
	m_current_timbre = v;
	// プリセットエントリを存在させる (デフォルト値で)。
	score.fm_presets[v];
}

void FileParser::process_lfo(const std::string& rest, Score& score) {
	if (m_current_timbre < 0) {
		score.warnings.push_back("LFO: without preceding SOUND:");
		return;
	}
	const auto vals = parse_int_list(rest);
	if (vals.size() < 5) {
		score.warnings.push_back("LFO: expects 5 values (WF,SPEED,DEPTH,ALG,FB)");
		return;
	}
	auto& p = score.fm_presets[m_current_timbre];
	p.lfo_waveform = static_cast<uint8_t>(vals[0] & 0x03);
	p.lfo_speed    = static_cast<uint8_t>(vals[1] & 0xFF);
	p.lfo_depth    = static_cast<int16_t>(vals[2]);
	p.algorithm    = static_cast<uint8_t>(vals[3] & 0x07);
	p.feedback     = static_cast<uint8_t>(vals[4] & 0x07);
}

void FileParser::process_ssgenv(const std::string& rest, Score& score) {
	// 期待: "@N , period , v1, v2, ..." (空白は許容)。
	size_t i = 0;
	const size_t n = rest.size();
	while (i < n && (rest[i] == ' ' || rest[i] == '\t')) ++i;
	if (i >= n || rest[i] != '@') {
		score.warnings.push_back("SSGENV: missing '@N'");
		m_continuation_ssg_env_number = 0;
		return;
	}
	++i;
	int env_n = 0;
	bool any = false;
	while (i < n && rest[i] >= '0' && rest[i] <= '9') {
		env_n = env_n * 10 + (rest[i] - '0');
		any = true;
		++i;
	}
	if (!any || env_n < 1 || env_n > 20) {
		score.warnings.push_back("SSGENV: number out of range 1..20");
		m_continuation_ssg_env_number = 0;
		return;
	}

	// '@N' の後ろは「,」or 空白で区切られた数値列 (period, v1, v2, ...)。
	const auto vals = parse_int_list(rest.substr(i));
	if (vals.empty()) {
		score.warnings.push_back("SSGENV: missing period and volumes");
		m_continuation_ssg_env_number = 0;
		return;
	}

	auto& env = score.ssg_envelopes[env_n];
	env.period_64th = static_cast<uint16_t>((vals[0] < 1) ? 1 : (vals[0] > 64 ? 64 : vals[0]));
	env.volumes.clear();
	for (size_t k = 1; k < vals.size(); ++k) {
		int v = vals[k];
		if (v < 0)  v = 0;
		if (v > 15) v = 15;
		env.volumes.push_back(static_cast<uint8_t>(v));
	}

	// この後の -> 行は同じ env に追記される。
	m_continuation_ssg_env_number = env_n;
}

void FileParser::process_ssgenv_continuation(const std::string& rest, Score& score) {
	if (m_continuation_ssg_env_number <= 0) {
		score.warnings.push_back("'->' continuation without preceding SSGENV:");
		return;
	}
	auto it = score.ssg_envelopes.find(m_continuation_ssg_env_number);
	if (it == score.ssg_envelopes.end()) {
		// 通常起こり得ないが念のため。
		m_continuation_ssg_env_number = 0;
		return;
	}
	const auto vals = parse_int_list(rest);
	for (int v : vals) {
		if (v < 0)  v = 0;
		if (v > 15) v = 15;
		it->second.volumes.push_back(static_cast<uint8_t>(v));
	}
}

void FileParser::process_op(int op_index, const std::string& rest, Score& score) {
	if (m_current_timbre < 0) {
		score.warnings.push_back("OP" + std::to_string(op_index + 1) +
		                         ": without preceding SOUND:");
		return;
	}
	const auto vals = parse_int_list(rest);
	if (vals.size() < 9) {
		// 末尾の DT2 は MUSIC.COM では実質無効なので 9 個あれば OK。
		score.warnings.push_back("OP" + std::to_string(op_index + 1) +
		                         ": expects at least 9 values (AR DR SR RR SL TL KS ML DT [DT2])");
		return;
	}
	const int ar = vals[0];
	const int dr = vals[1];
	// vals[2] (SR 列) は MUSIC.COM ドライバ仕様により無視。
	// D2R レジスタには SL 列の値が書かれる (NP2 録音との比較で確認済み)。
	const int rr = vals[3];
	const int sl = vals[4];
	const int tl = vals[5];
	const int ks = vals[6];
	const int ml = vals[7];
	const int dt = vals[8];
	const int dt2 = (vals.size() >= 10) ? (vals[9] & 0x3) : 0;

	auto& p = score.fm_presets[m_current_timbre];
	auto& op = p.ops[op_index];
	op.ar_sec  = convert_ar(ar);
	op.d1r_sec = convert_dr(dr);
	op.d2r_sec = convert_sr(sl);  // MUSIC.COM 仕様: SL 列を D2R にも書く
	op.rr_sec  = convert_rr(rr);
	op.sl      = convert_sl(sl);
	op.tl_db   = convert_tl(tl);
	op.mul     = static_cast<uint8_t>(ml & 0x0F);
	op.detune  = convert_dt_to_cents(dt);
	op.ks      = static_cast<uint8_t>(ks & 0x3);
	op.dt2     = static_cast<uint8_t>(dt2);
	if (dt2 != 0) p.has_dt2 = true;
}

void FileParser::parse(const std::string& text, Score& score) {
	score = Score{};
	m_macros.clear();
	m_current_timbre = -1;
	m_continuation_ssg_env_number = 0;

	std::string channel_buffers[k_channel_count];
	std::string d_part_buffer;

	// 行に分割して順次処理。
	std::string line;
	line.reserve(256);
	for (size_t i = 0; i <= text.size(); ++i) {
		const char c = (i < text.size()) ? text[i] : '\n';
		if (c == '\x1a') {
			// DOS EOF マーカー (Ctrl+Z)。 以降は終端として無視。
			// 多くの MUSIC.COM 系 MML はファイル末尾に 0x1A を残している。
			process_line(line, score, channel_buffers, d_part_buffer);
			break;
		}
		if (c == '\n') {
			process_line(line, score, channel_buffers, d_part_buffer);
			line.clear();
		} else if (c != '\r') {
			line.push_back(c);
		}
	}

	// {...} 繰り返し展開を行うヘルパ (マクロ展開は済んでいる前提)。
	// loop_count: 無限ループ (`{` 回数省略・`{0`・最外周の大回数 {99 等) を展開する回数。
	//             いずれも MUSIC.COM では「曲全体の無限ループ」 とみなし、 2 パス解析で
	//             求めたチャンネルごとのバランス回数を等しく適用する。
	// warns:      null なら警告を捨てる (Pass 1 計測用に使う)。
	auto expand_repeats = [](const std::string& expanded,
	                         const std::string& tag,
	                         int loop_count,
	                         std::vector<std::string>* warns) -> std::string {
		std::string final_body;
		final_body.reserve(expanded.size() * 2);
		struct Frame { size_t output_start; int count; };
		std::vector<Frame> stack;

		size_t i = 0;
		const size_t n = expanded.size();
		while (i < n) {
			const char c = expanded[i];
			if (c == '{') {
				size_t j = i + 1;
				while (j < n && (expanded[j] == ' ' || expanded[j] == '\t')) ++j;
				int count = 0;
				bool any = false;
				while (j < n && expanded[j] >= '0' && expanded[j] <= '9') {
					count = count * 10 + (expanded[j] - '0');
					any = true;
					++j;
				}
				// 無限ループ判定。 次のいずれかは「曲全体ループ」 とみなし、 呼び出し元の
				// 2 パス解析が決めた loop_count (= バランス済み展開回数) で展開する:
				//   - `{` 回数省略   … MUSIC.COM では {0 と同義 (無限)。 NP2 録音比較でも
				//                       「他チャンネルと同じ時間だけ独立反復」 が正しい。
				//   - `{0`           … 明示的な無限ループ (正規構文なので警告なし)。
				//   - 最外周の大回数 … {99 等。 曲全体を {99 で囲んで「実質無限ループ」 と
				//                       する書き方 (HEC1-2/20/BAT3-2)。 そのまま 99 回展開
				//                       すると曲長が数十分に膨れる。
				// 大回数判定は最外周 (stack.empty()) 限定。 ネストした大回数 (MAP_SPR の
				// {0 … {39 ドラム連打 … }} ) は正規の有限反復なので literal を保つ。
				// 実データ上、 音楽的な繰り返しは 16 回以下・無限意図は 39/99 でその間
				// (17..38) は空白なので、 閾値 17 で安全に分離できる。
				constexpr int k_song_loop_min_count = 17;
				const bool is_infinite =
					!any || count == 0 ||
					(count >= k_song_loop_min_count && stack.empty());
				if (is_infinite) {
					count = loop_count;
				}
				stack.push_back({final_body.size(), count});
				i = j;
			} else if (c == '}') {
				if (stack.empty()) {
					if (warns) warns->push_back("'}' without matching '{'");
					++i;
					continue;
				}
				const Frame f = stack.back();
				stack.pop_back();
				const std::string body = final_body.substr(f.output_start);
				for (int k = 1; k < f.count; ++k) final_body.append(body);
				++i;
			} else {
				final_body.push_back(c);
				++i;
			}
		}
		if (!stack.empty() && warns) {
			warns->push_back("unclosed '{' in " + tag);
		}
		return final_body;
	};

	// マクロ展開は 1 回だけ行って結果をキャッシュ (Pass 1/2 で再展開すると警告が二重発生する)。
	std::string ch_macro_expanded[k_channel_count];
	for (int ch = 0; ch < k_channel_count; ++ch) {
		if (!channel_buffers[ch].empty()) {
			ch_macro_expanded[ch] = m_macros.expand(channel_buffers[ch], score.warnings);
		}
	}
	std::string d_macro_expanded;
	if (!d_part_buffer.empty()) {
		d_macro_expanded = m_macros.expand(d_part_buffer, score.warnings);
	}

	// Pass 1: `{` count 省略 = 1 回 (= 展開なし)・ `{0` = 1 回として測定する。
	// 各チャンネル本来 (= setup + ループ本体 1 回分) の長さを得る。
	std::vector<std::string> pass1_dummy_warnings;
	BodyParser pass1_parser;
	std::vector<int32_t> pass1_end_time(k_channel_count, 0);
	for (int ch = 0; ch < k_channel_count; ++ch) {
		if (ch_macro_expanded[ch].empty()) continue;
		const std::string p1 = expand_repeats(ch_macro_expanded[ch], "", 1, nullptr);
		pass1_parser.reset();
		std::vector<Event> tmp_events;
		pass1_end_time[ch] = pass1_parser.parse(
			p1, tmp_events, 0, pass1_dummy_warnings);
	}
	int32_t pass1_d_end_time = 0;
	if (!d_macro_expanded.empty()) {
		const std::string p1 = expand_repeats(d_macro_expanded, "", 1, nullptr);
		DPartParser tmp_d;
		std::vector<Event> tmp_events;
		pass1_d_end_time = tmp_d.parse(p1, tmp_events, 0, pass1_dummy_warnings);
	}

	// 最長チャンネルを基準に、 全チャンネルが少なくともこの長さに到達するよう
	// 「短いチャンネルだけ余計に展開」 する。 m_infinite_loop_count 倍がベースライン
	// (= 既定 2 倍。 SHIP2/CGM16 等、 各チャンネルが同尺の曲は従来通り 2 倍展開)。
	int32_t L_max = pass1_d_end_time;
	for (auto t : pass1_end_time) if (t > L_max) L_max = t;
	const int32_t target = L_max * m_infinite_loop_count;

	// Pass 2: 各チャンネルを必要回数で再展開し、 本パースを行う。
	// 展開回数は target / pass1_t を「四捨五入」 する。 切り上げ (ceil) だと、
	// ループ本体長が L_max とごく僅かに違うだけのチャンネル (例: AGM19 の
	// ch1/2/5/6 は ch4 と数 tick 差) でも +1 回多く展開され、 total_ticks が
	// 丸ごと 1 ループ分伸びて末尾無音 (= 揃わない曲尾) を生む。 四捨五入なら
	// 「ほぼ同長」 は m_infinite_loop_count のまま、 「ちょうど半分」 のドラム ch は
	// 2×m_infinite_loop_count と、 双方が目標長へ最も近い回数に揃う。
	auto compute_count = [&](int32_t pass1_t) {
		int n = m_infinite_loop_count;
		if (pass1_t > 0) {
			const int needed = static_cast<int>(
				(static_cast<int64_t>(target) + pass1_t / 2) / pass1_t);
			if (needed > n) n = needed;
		}
		return n;
	};

	BodyParser body_parser;
	for (int ch = 0; ch < k_channel_count; ++ch) {
		if (ch_macro_expanded[ch].empty()) continue;
		const int count = compute_count(pass1_end_time[ch]);
		const std::string final_body = expand_repeats(
			ch_macro_expanded[ch], "channel " + std::to_string(ch + 1),
			count, &score.warnings);
		body_parser.reset();
		const int32_t end_time = body_parser.parse(
			final_body, score.channels[ch], 0, score.warnings);
		if (end_time > score.total_ticks) score.total_ticks = end_time;
	}

	if (!d_macro_expanded.empty()) {
		const int count = compute_count(pass1_d_end_time);
		const std::string final_body = expand_repeats(
			d_macro_expanded, "D part", count, &score.warnings);
		DPartParser d_parser;
		const int32_t end_time = d_parser.parse(
			final_body, score.d_part, 0, score.warnings);
		if (end_time > score.total_ticks) score.total_ticks = end_time;
	}
}

}  // namespace mml
