#pragma once
//
// SoundDat: ASCII LOGIN MUSIC.COM の SOUND.DAT (効果音定義) パーサ + テーブル。
//
// MUSIC.COM 隠しコマンド `D:` パートから参照される SSG ベースのリズム音定義を読み込む。
// 仕様詳細: docs/music_com_sound_dat_disasm.md (逆アセンブル解読結果) 参照。
//
// 1 効果音は複数の「段階 (stage)」を持つ。 各段階は @N の Sound: 行から
// 始まり、 次の Sound: 行 (@N 省略 or 新 @N) で区切られる。
// 段階内の Tone1/Tone2/Vol1/Vol2/Noise が ch A / ch B の SSG レジスタを
// 時系列で動かす指示となる。
//

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mml {

// 1 段階内の Tone (ch A or B) シーケンス。
// 効果音発火後、 毎フレーム (約 17ms) 内部 cursor に step を加算し、
// duration フレーム分経過したら次段階へ移行する。
struct SoundDatToneSeq {
	uint16_t f        = 0;  // 初期周波数 (12bit、 SSG tone 周期)
	int16_t  step     = 0;  // 1 フレームあたり加算量 (= df ÷ (d1×p2))
	uint16_t duration = 0;  // 段階持続フレーム数 (= d1×p2)
	uint8_t  d2       = 0;  // 0 = 凍結 (持続音)、 非 0 = base へループ再開
};

// 1 段階内の Vol シーケンス。 値は 8.8 固定小数点 (上位 byte が SSG vol レジスタ値)。
struct SoundDatVolSeq {
	uint16_t v_8_8    = 0;  // 初期 vol << 8
	int16_t  step     = 0;  // 1 フレームあたり加算量 (= (dv<<8) ÷ (d1×p3))
	uint16_t duration = 0;  // 段階持続フレーム数 (= d1×p3)
	uint8_t  d2       = 0;
};

// 1 段階内の Noise シーケンス。 8.8 固定。
struct SoundDatNoiseSeq {
	uint16_t n_8_8    = 0;
	int16_t  step     = 0;
	uint16_t duration = 0;
	uint8_t  d2       = 0;
};

// 1 効果音の 1 段階。
struct SoundDatStage {
	uint8_t  enable_flag = 0xFF;  // mixer 反映用フラグ (bit 0/1 = Tone1/Tone2 disable、 bit 3-4 = noise mode)
	uint16_t p1          = 0;      // 段階の持続フレーム数 (= 段階終了の鳴り続き条件)

	SoundDatToneSeq  tone[2];   // [0] = Tone1 (ch A)、 [1] = Tone2 (ch B)
	SoundDatVolSeq   vol[2];    // 同上
	SoundDatNoiseSeq noise;
};

// 1 効果音 = 段階列。
struct SoundDatEffect {
	std::vector<SoundDatStage> stages;
};

// SOUND.DAT 全体。 @N (0..63) → SoundDatEffect。
class SoundDat {
public:
	SoundDat() = default;

	// 指定パスから読み込む。 失敗時は false (warn_out にメッセージ追加)。
	bool load(const std::string& path, std::vector<std::string>& warn_out);

	// 効果音番号で検索。 未定義時は nullptr。
	const SoundDatEffect* find(int effect_id) const;

	// 何個の効果音を読み込んだか。
	size_t size() const { return m_effects.size(); }

	// 全効果音番号を取得 (テスト/デバッグ用)。
	std::vector<int> ids() const;

private:
	std::map<int, SoundDatEffect> m_effects;

	// パーサ局所状態: 直近 Sound: 行の p2/p3 (後続の Tone/Vol/Noise: で参照)。
	uint16_t m_last_p2 = 1;
	uint16_t m_last_p3 = 1;

	// テキスト 1 行を解析して構造体を更新する。 戻り値 = 解析成功 (true) / 致命的エラー (false)。
	// 状態 (現在の効果音番号、 現在段階) は cur_id_inout / cur_effect_inout に保持。
	bool parse_line(const std::string& line, int& cur_id_inout,
	                SoundDatEffect*& cur_effect_inout,
	                std::vector<std::string>& warn_out);
};

}  // namespace mml
