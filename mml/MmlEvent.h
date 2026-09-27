#pragma once
//
// MmlEvent: 解析済み MML の最小単位イベント。
//
// MML の文字列を時間順のイベント列に変換した形で保持する。
// L (デフォルト音長)、O (オクターブ)、Q (ゲート率)、& (タイ) などは
// パーサ内部で状態として処理し、結果として残る Note/Rest/制御変更だけが
// イベントとして保存される。
//
// 時間単位:
//   1 全音符 = k_ticks_per_whole_note (=768) tick。
//   64 分音符 = 12 tick、付点 64 分 = 18 tick で整数表現可能。
//

#include <cstdint>

namespace mml {

constexpr int32_t k_ticks_per_whole_note   = 768;
constexpr int32_t k_ticks_per_quarter_note = k_ticks_per_whole_note / 4;  // 192

// MUSIC.COM の T コマンドから実効 BPM への変換。
// musiccom_emu (実機ドライバ) を T100..200 の純 16 分音符で実測した結果、
//   実効 BPM = 6500 / floor(5880 / T)
// が全 T で厳密一致 (T120→132.65, T100→112.07, T110→122.64 等)。
// floor による量子化のため公称 BPM に対し非線形。旧実装の線形係数 1.106 は
// この floor を無視した近似で、T120 付近のみ一致し T100 で 1.3%/T200 で 1.3% 遅かった。
// L8/L4 でも 16 分の整数倍が厳密成立するため (floor はテンポ基底に 1 回だけ適用)、
// この実効 BPM を全音符長へ一律に適用すればよい。
inline double music_com_tempo_to_bpm(int t_value) {
	if (t_value <= 0)   t_value = 120;   // 既定テンポ (T コマンド省略時)
	int div = 5880 / t_value;            // 整数除算 = floor (t_value > 0)
	if (div <= 0) div = 1;               // T > 5880 のガード
	return 6500.0 / static_cast<double>(div);
}

// 半音番号は MIDI 風: 0 = C-1、60 = C4 (中央のド)、69 = A4 (440Hz)。
// MUSIC.COM の O (オクターブ) は 1..8 で「O4 C」が C4 (= MIDI 60) とする想定。
constexpr int8_t k_semitone_invalid = -1;

enum class EventKind : uint8_t {
	Note,         // a = 半音番号 (0..127), b = 全音長 tick, c = ゲート (key-on) tick
	Rest,         // a = 全音長 tick (W も Rest として扱う、c=1 で W 区別)
	Tempo,        // a = T コマンドの値 (BPM ではない。music_com_tempo_to_bpm で変換)
	Volume,       // a = 0..15
	Timbre,       // a = 0..20
	GateTime,     // a = 1..8 (Q コマンド) ※状態だが追跡用に出力する場合のみ
	PitchOffset,  // a = -255..255 (N コマンド)
	// I/U/P 効果。I/U/P は相互排他で、後から設定したものが有効 (前を上書き)。
	Vibrato,      // I: a=振幅 (-255..255 = ±半音), b=周期 (×64分音符), c=ディレイ (×64分音符)
	Tremolo,      // U: a=振幅 (V 単位 0..15ish), b=周期, c=ディレイ
	Portamento,   // P: a=グライド時間 (×64分音符)、a=0 で解除
	// S/M (SSG ハードウェアエンベロープ)。SSGENV と相互排他、後勝ち。
	SsgHwEnvelopeShape,   // S: a=シェイプ (0..14)、書き込みでエンベロープ再起動
	SsgHwEnvelopePeriod,  // M: a=周期 (0..65535)、PSG レジスタ生値
	// Y コマンド (FM/SSG レジスタ直接書き込み)。 a = レジスタアドレス, b = データ。
	// MUSIC.COM は単一ポート (port 0) 想定。 ymfm/Legacy 共通で write_reg(0, a, b)。
	YRegWrite,
	// 効果音トリガ (D パート専用)。 a = 効果音番号 (0..63)、 b = 全音長 tick。
	// SSG ch A/B (= ch 4/5) を SOUND.DAT 由来のシーケンスで一定フレーム数占有する。
	EffectTrigger,
};

struct Event {
	int32_t   time_ticks = 0;  // スコア先頭からの絶対 tick
	EventKind kind       = EventKind::Note;
	// 各イベントごとの意味は EventKind コメント参照。
	int32_t   a = 0;
	int32_t   b = 0;
	int32_t   c = 0;
	// フラグビット。
	//   bit 0: tied_from_prev — Note 専用。'&' で前ノートから繋がる異音 (スラー)。
	//          キーオンせず、ピッチだけ変えて続行する。
	uint8_t   flags = 0;
};

constexpr uint8_t k_event_flag_tied_from_prev = 0x01;

}  // namespace mml
