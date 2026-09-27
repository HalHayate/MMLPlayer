#include "SoundDat.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace mml {

namespace {

// 行頭の空白を飛ばす。
size_t skip_ws(const std::string& s, size_t i) {
	while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
	return i;
}

// 区切り文字 (空白 or ',') を飛ばす。
size_t skip_sep(const std::string& s, size_t i) {
	while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == ',')) ++i;
	return i;
}

// 行末コメント (";") 以降を切り捨て、 末尾空白も除去。
// DOS の EOF マーカ (0x1A) など印字不可制御文字も無視する。
std::string strip_comment(const std::string& line) {
	size_t end = line.find(';');
	std::string s = (end == std::string::npos) ? line : line.substr(0, end);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
	                       s.back() == '\r' || s.back() == '\n' ||
	                       static_cast<unsigned char>(s.back()) < 0x20)) {
		s.pop_back();
	}
	return s;
}

// 符号付き整数を読み取って次の位置を返す。 失敗時 ok=false で 0 を返し、 i は変更なし。
int read_signed_int(const std::string& s, size_t& i, bool& ok) {
	ok = false;
	const size_t start = i;
	bool neg = false;
	if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
		neg = (s[i] == '-');
		++i;
	}
	if (i >= s.size() || s[i] < '0' || s[i] > '9') {
		i = start;
		return 0;
	}
	int value = 0;
	while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
		value = value * 10 + (s[i] - '0');
		++i;
	}
	ok = true;
	return neg ? -value : value;
}

// "Sound:" 等のラベルを大文字小文字無視で照合し、 ':' 後の位置を返す。
// 一致しなければ string::npos。
size_t match_label(const std::string& s, const char* label) {
	const size_t n = std::strlen(label);
	if (s.size() < n + 1) return std::string::npos;
	for (size_t k = 0; k < n; ++k) {
		char a = s[k];
		char b = label[k];
		if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
		if (a != b) return std::string::npos;
	}
	if (s[n] != ':') return std::string::npos;
	return n + 1;
}

// 各種パラメータ列をカンマ区切りで読み出す (最大 max 個)。
// 失敗した位置以降は 0 で埋める。 ok_count に成功個数を返す。
void read_param_list(const std::string& s, size_t i, int* out, int max, int& ok_count) {
	ok_count = 0;
	for (int k = 0; k < max; ++k) {
		i = skip_sep(s, i);
		bool ok = false;
		int v = read_signed_int(s, i, ok);
		if (!ok) {
			out[k] = 0;
		} else {
			out[k] = v;
			++ok_count;
		}
	}
}

// 整数除算 (cdq + idiv 相当)。 cx == 0 のときの安全側ガードあり。
int16_t safe_div(int32_t dividend, int32_t divisor) {
	if (divisor == 0) return 0;
	const int32_t q = dividend / divisor;
	if (q >  0x7FFF) return  0x7FFF;
	if (q < -0x8000) return -0x8000;
	return static_cast<int16_t>(q);
}

}  // namespace

const SoundDatEffect* SoundDat::find(int effect_id) const {
	auto it = m_effects.find(effect_id);
	return (it == m_effects.end()) ? nullptr : &it->second;
}

std::vector<int> SoundDat::ids() const {
	std::vector<int> out;
	out.reserve(m_effects.size());
	for (const auto& kv : m_effects) out.push_back(kv.first);
	return out;
}

bool SoundDat::load(const std::string& path, std::vector<std::string>& warn_out) {
	std::ifstream ifs(path, std::ios::binary);
	if (!ifs) {
		warn_out.push_back("SOUND.DAT: cannot open " + path);
		return false;
	}
	std::stringstream ss;
	ss << ifs.rdbuf();
	const std::string text = ss.str();

	m_effects.clear();

	int             cur_id     = -1;        // 直近で見た @N (-1 = 未設定)
	SoundDatEffect* cur_effect = nullptr;

	std::string line;
	line.reserve(256);
	for (size_t i = 0; i <= text.size(); ++i) {
		const char c = (i < text.size()) ? text[i] : '\n';
		if (c == '\n') {
			parse_line(strip_comment(line), cur_id, cur_effect, warn_out);
			line.clear();
		} else if (c != '\r') {
			line.push_back(c);
		}
	}
	return true;
}

bool SoundDat::parse_line(const std::string& raw, int& cur_id_inout,
                          SoundDatEffect*& cur_effect_inout,
                          std::vector<std::string>& warn_out) {
	// 行頭の空白を飛ばす。 空行はスキップ。
	size_t i = skip_ws(raw, 0);
	if (i >= raw.size()) return true;

	auto append_stage = [&](const char* warn_prefix) -> SoundDatStage* {
		if (!cur_effect_inout) {
			warn_out.push_back(std::string(warn_prefix) + ": no current effect (missing Sound:)");
			return nullptr;
		}
		if (cur_effect_inout->stages.empty()) {
			cur_effect_inout->stages.emplace_back();
		}
		return &cur_effect_inout->stages.back();
	};

	// "Sound:" 行 — 新しい段階を開始する。 @N があれば新効果音、 なければ前の効果音の次段階。
	if (size_t rest = match_label(raw.substr(i), "Sound"); rest != std::string::npos) {
		size_t p = i + rest;
		p = skip_ws(raw, p);

		bool has_at = (p < raw.size() && raw[p] == '@');
		int  effect_id = cur_id_inout;
		if (has_at) {
			++p;
			bool ok = false;
			effect_id = read_signed_int(raw, p, ok);
			if (!ok || effect_id < 0 || effect_id >= 64) {
				warn_out.push_back("SOUND.DAT: Sound: @N out of range 0..63");
				return false;
			}
			cur_id_inout = effect_id;
			cur_effect_inout = &m_effects[effect_id];
			cur_effect_inout->stages.clear();
		} else if (cur_id_inout < 0 || !cur_effect_inout) {
			warn_out.push_back("SOUND.DAT: Sound: without @N before any effect");
			return false;
		}

		// 続いて p1, p2, p3 を読む。 デフォルト: p1=60 (0x3C)、 p2=1、 p3=1。
		int params[3]; int got = 0;
		read_param_list(raw, p, params, 3, got);
		int p1 = (got >= 1 && params[0] != 0) ? params[0] : 0x3C;
		int p2 = (got >= 2 && params[1] != 0) ? params[1] : 1;
		int p3 = (got >= 3 && params[2] != 0) ? params[2] : 1;

		// 新しい段階を追加 (既存段階の続き)。
		SoundDatStage stage;
		stage.enable_flag = 0xFF;
		stage.p1          = static_cast<uint16_t>(p1);
		cur_effect_inout->stages.push_back(stage);

		// Sound: 行の p2/p3 は後段の Tone/Vol/Noise: 計算で使うのでローカル保管したいが、
		// 段階ごとに保持する設計とし、 stage 自体には埋め込まず後続行が「最後の段階」 を
		// 参照しながら p2/p3 を直接ローカルで使う方式に変える必要がある。
		// → SoundDatStage に 「parser-local」 として p2/p3 を一時保持しておく:
		//   ここでは tone[0/1].duration の代わりに p2 を Sound 行で確定させてしまうと
		//   Tone1/Vol1 行が後で来るときに p2 を参照できない。
		//   よって p2/p3 を SoundDat 自体の暫定状態として持つのが妥当。
		m_last_p2 = static_cast<uint16_t>(p2);
		m_last_p3 = static_cast<uint16_t>(p3);
		return true;
	}

	// 共通ヘルパ: Tone1/Tone2/Vol1/Vol2 で値を読む。
	auto parse_tone = [&](int slot) -> bool {
		SoundDatStage* st = append_stage(slot == 0 ? "Tone1" : "Tone2");
		if (!st) return false;
		size_t p = i;
		// "Tone1" / "Tone2" ラベルをスキップ。
		while (p < raw.size() && raw[p] != ':') ++p;
		if (p >= raw.size()) return false;
		++p;
		int params[4]; int got = 0;
		read_param_list(raw, p, params, 4, got);
		// f, df, d1, d2
		const int f  = params[0];
		const int df = params[1];
		const int d1 = (params[2] != 0) ? params[2] : 1;
		const int d2 = params[3];

		const int duration = d1 * m_last_p2;
		const int step     = safe_div(df, duration);

		st->tone[slot].f        = static_cast<uint16_t>(f);
		st->tone[slot].step     = static_cast<int16_t>(step);
		st->tone[slot].duration = static_cast<uint16_t>(duration);
		st->tone[slot].d2       = static_cast<uint8_t>(d2 & 0xFF);
		// enable_flag の Tone1/Tone2 bit を落とす (= enable)。
		st->enable_flag &= static_cast<uint8_t>(~(1 << slot));
		return true;
	};

	auto parse_vol = [&](int slot) -> bool {
		SoundDatStage* st = append_stage(slot == 0 ? "Vol1" : "Vol2");
		if (!st) return false;
		size_t p = i;
		while (p < raw.size() && raw[p] != ':') ++p;
		if (p >= raw.size()) return false;
		++p;
		int params[4]; int got = 0;
		read_param_list(raw, p, params, 4, got);
		const int v  = params[0];
		const int dv = params[1];
		const int d1 = (params[2] != 0) ? params[2] : 1;
		const int d2 = params[3];

		const int duration = d1 * m_last_p3;
		// dv は 8bit シフトしてから duration で割る (= 8.8 固定の 1 frame 加算量)
		const int step     = safe_div(dv << 8, duration);

		st->vol[slot].v_8_8    = static_cast<uint16_t>(v << 8);
		st->vol[slot].step     = static_cast<int16_t>(step);
		st->vol[slot].duration = static_cast<uint16_t>(duration);
		st->vol[slot].d2       = static_cast<uint8_t>(d2 & 0xFF);
		return true;
	};

	if (size_t rest = match_label(raw.substr(i), "Tone1"); rest != std::string::npos) return parse_tone(0);
	if (size_t rest = match_label(raw.substr(i), "Tone2"); rest != std::string::npos) return parse_tone(1);
	if (size_t rest = match_label(raw.substr(i), "Vol1");  rest != std::string::npos) return parse_vol(0);
	if (size_t rest = match_label(raw.substr(i), "Vol2");  rest != std::string::npos) return parse_vol(1);

	if (size_t rest = match_label(raw.substr(i), "Noise"); rest != std::string::npos) {
		SoundDatStage* st = append_stage("Noise");
		if (!st) return false;
		size_t p = i + rest;
		int params[5]; int got = 0;
		read_param_list(raw, p, params, 5, got);
		const int n1 = params[0];                  // enable bits 用
		const int n  = params[1];                  // 初期周期
		const int dn = params[2];                  // 変化量
		const int d1 = (params[3] != 0) ? params[3] : 1;
		const int d2 = params[4];

		const int duration = d1 * m_last_p2;
		const int step     = safe_div(dn << 8, duration);

		st->noise.n_8_8    = static_cast<uint16_t>(n << 8);
		st->noise.step     = static_cast<int16_t>(step);
		st->noise.duration = static_cast<uint16_t>(duration);
		st->noise.d2       = static_cast<uint8_t>(d2 & 0xFF);
		// enable_flag の noise bits 3-4 を更新 (n1 & 3 を bit 3 位置にシフトしてクリア)。
		const uint8_t mask = static_cast<uint8_t>(~(static_cast<uint8_t>(n1 & 3) << 3));
		st->enable_flag &= mask;
		return true;
	}

	// 未知のラベルは警告 (最初の token を抜き出す)。
	std::string token;
	size_t p = i;
	while (p < raw.size() && raw[p] != ':' && raw[p] != ' ' && raw[p] != '\t') {
		token.push_back(raw[p]); ++p;
	}
	if (!token.empty()) {
		warn_out.push_back("SOUND.DAT: unknown label '" + token + "'");
	}
	return true;
}

}  // namespace mml
