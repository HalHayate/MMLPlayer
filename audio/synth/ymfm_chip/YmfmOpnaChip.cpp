//
// YmfmOpnaChip.cpp: ymfm::ym2608 ラッパの実装。
//
// 設計メモ:
//   - ymfm はチップ内部レート (input_clock / divisor) で生成する。
//   - 利用側 (MMLPlayer / WAV ライタ) は固定 48 kHz を期待するため、線形補間で
//     ダウンサンプル/アップサンプルする。FIR フィルタ等は Phase J2 範囲外。
//   - Fidelity::Low (input/48 = 166.4 kHz @ 7.987 MHz) を既定。48 kHz への
//     比は ~3.47 倍で、エイリアシングは実機が ~55 kHz DAC で内部
//     samples_per_output=3 を使うため等価レベル。
//

#include "YmfmOpnaChip.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// ymfm はテンプレ展開で大量の C4100/C4244 を出す (符号付き/無し、未使用引数等)。
// 自前コードの警告は維持したいので、ymfm ヘッダの include だけ警告レベルを下げる。
#ifdef _MSC_VER
#pragma warning(push, 1)
#endif
#include "audio/synth/ymfm/ymfm.h"
#include "audio/synth/ymfm/ymfm_opn.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace {

// ymfm_interface のミニマル実装。
// MMLPlayer はタイマ割り込みや IRQ を使わず、レジスタ書き込みと generate だけ。
// ADPCM-A の ROM 読み出しも不要 (Rhythm 未使用)。
class MinimalYmfmInterface : public ymfm::ymfm_interface {
public:
	// すべてデフォルト実装 (no-op) で十分。
	// ymfm_set_timer / ymfm_update_irq などはデフォルトで no-op。
};

}  // namespace

struct YmfmOpnaChip::Impl {
	MinimalYmfmInterface intf;
	std::unique_ptr<ymfm::ym2608> chip;

	// ymfm::ym2608::output_data は { int32_t data[3] } (FM L, FM R, SSG mono)。
	std::vector<ymfm::ym2608::output_data> ymfm_buf;

	// リサンプリング用の前回サンプル (補間の左端)。
	int32_t  last_fm_l = 0;
	int32_t  last_fm_r = 0;
	int32_t  last_ssg  = 0;

	// SSG DC 除去用 1-pole IIR HPF 状態 (実機 PC-9801-86 のカップリングコンデンサ相当)。
	// ymfm の SSG amplitude table は 0..16382 の unipolar (= DC バイアス込み) で出力するため、
	// 「0 ↔ amp」の片極 square 波になる。 そのまま FM ミックス + int16 化すると DC が乗り
	// ゼロクロスのない波形 → 倍音が認識されない / スペクトラム狭帯域化。
	//   y[n] = x[n] - x[n-1] + R · y[n-1]   (R = 0.999, fc ≈ 8 Hz @ 48 kHz)
	float ssg_hpf_x_prev = 0.0f;
	float ssg_hpf_y_prev = 0.0f;

	// FM 高域シェルフ補正 (NP2 opngen 寄せ) 用の 1-pole LPF 状態。
	// ymfm の FB 音色は NP2 比で高域が明るい (BOSS ch1 @11 FB=7: 8-16kHz +2.7dB)。
	// FM のみ高域シェルフ (cut) で抑える。SSG には掛けない。
	float fm_tilt_hi_l = 0.0f, fm_tilt_hi_r = 0.0f;

	// 累積位相 (ymfm レートでの位置を fixed-point で保持)。
	uint64_t resample_pos  = 0;  // ymfm レートでの「次に取りたい位置」 32.32
	uint64_t resample_step = 0;  // 1 出力サンプルあたりに進む ymfm レート位置 32.32
	                             // ★ uint32_t だと ymfm_rate=166400 / output=48000 の
	                             //   34-bit 値 14,891,520,256 がオーバーフローして
	                             //   step が下位 32 bit (= 0.467) のみになっていた

	// ---- OPNA レジスタ shadow ----
	// 1 レジスタに複数フィールドが格納されるため、setter ごとに該当 ch/op の shadow を
	// 更新してレジスタ全体を再構築する。

	// op 単位 (6 ch × 4 op = 24 op)
	struct OpState {
		uint8_t dt    = 0;  // 3-bit (bit2 が符号)
		uint8_t mul   = 1;  // 4-bit
		uint8_t tl    = 0;  // 7-bit (0..127)
		uint8_t ks    = 0;  // 2-bit
		uint8_t ar    = 31; // 5-bit
		uint8_t d1r   = 0;  // 5-bit
		uint8_t d2r   = 0;  // 5-bit
		uint8_t sl    = 0;  // 4-bit
		uint8_t rr    = 7;  // 4-bit
	};
	OpState op[6][4];

	// ch 単位
	struct ChState {
		uint8_t  alg  = 0;
		uint8_t  fb   = 0;
		uint8_t  pms  = 0;
		uint8_t  ams  = 0;
		uint8_t  pan_l = 1;  // bit (1 = enable)
		uint8_t  pan_r = 1;
		uint16_t fnum  = 0;  // 11-bit
		uint8_t  block = 0;  // 3-bit
	};
	ChState ch[6];

	// PSG (SSG)
	uint8_t  ssg_mixer    = 0x3F;  // 全 ch tone/noise disable (bit=1 で disable)
	uint16_t ssg_tone_tp[3] = { 0, 0, 0 };
	uint8_t  ssg_volume[3]  = { 0, 0, 0 };
	uint16_t ssg_env_period = 0;
	uint8_t  ssg_env_shape  = 0;

	// チップ全体 LFO (reg 0x22)
	bool    lfo_enable = false;
	uint8_t lfo_speed  = 0;

	// ---- S98 v1 録音状態 ----
	// 1 tick = 1 ms (np21w default の timerinfo/timerinfo2 = 1/1000)。
	FILE*    s98_fh           = nullptr;
	uint64_t s98_samples_accum = 0;   // 累積 output sample 数
	uint64_t s98_last_ms       = 0;   // 直近 sync を発行した時刻 (ms)
	uint64_t s98_intcount      = 0;   // 未発火の sync tick (ms)
};

namespace {

ymfm::opn_fidelity to_ymfm_fidelity(YmfmOpnaChip::Fidelity f) {
	switch (f) {
	case YmfmOpnaChip::Fidelity::High:   return ymfm::OPN_FIDELITY_MAX;
	case YmfmOpnaChip::Fidelity::Medium: return ymfm::OPN_FIDELITY_MED;
	default:                              return ymfm::OPN_FIDELITY_MIN;
	}
}

}  // namespace

YmfmOpnaChip::YmfmOpnaChip(uint32_t output_sample_rate,
                           double   master_clock_hz,
                           Fidelity fidelity)
	: m_impl(std::make_unique<Impl>())
	, m_output_rate(output_sample_rate)
	, m_master_clock_hz(master_clock_hz) {
	m_impl->chip = std::make_unique<ymfm::ym2608>(m_impl->intf);
	m_impl->chip->set_fidelity(to_ymfm_fidelity(fidelity));
	m_impl->chip->reset();
	// MUSIC.COM は reg 0x27 で ch3 効果音モード (bit6) を常時 ON にして運用する。
	// flush_freq が A8-AE へ同値ミラーするため、通常ノートの音は変わらない。
	write_reg(0, 0x27, 0x40);

	// ymfm が生成するサンプリングレート。fidelity と master_clock_hz から決まる。
	m_ymfm_rate = m_impl->chip->sample_rate(static_cast<uint32_t>(master_clock_hz));
	if (m_ymfm_rate == 0) {
		m_ymfm_rate = 55466;  // fallback (実機 OPNA DAC レート相当)
	}

	// 1 出力サンプルあたりの ymfm 進み量 (32.32 固定小数点)。
	// step = ymfm_rate / output_rate を 32 bit シフトで表現。
	if (m_output_rate > 0) {
		m_impl->resample_step =
			(static_cast<uint64_t>(m_ymfm_rate) << 32) / m_output_rate;
	}

	// パン既定: 全 ch Both。MUSIC.COM ドライバ (NP2 録音と一致) に合わせる。
	for (int ch = 0; ch < 6; ++ch) {
		set_fm_pan(ch, Pan::Both);
	}
}

YmfmOpnaChip::~YmfmOpnaChip() = default;

// ----- flush_* ヘルパ forward 宣言 (start_s98_recording の初期 dump で使うため) ---
namespace {
void flush_dt_mul     (YmfmOpnaChip::Impl& impl, int ch, int op);
void flush_tl         (YmfmOpnaChip::Impl& impl, int ch, int op);
void flush_ks_ar      (YmfmOpnaChip::Impl& impl, int ch, int op);
void flush_d1r        (YmfmOpnaChip::Impl& impl, int ch, int op);
void flush_d2r        (YmfmOpnaChip::Impl& impl, int ch, int op);
void flush_sl_rr      (YmfmOpnaChip::Impl& impl, int ch, int op);
void flush_fb_alg     (YmfmOpnaChip::Impl& impl, int ch);
void flush_lr_ams_pms (YmfmOpnaChip::Impl& impl, int ch);
void flush_freq       (YmfmOpnaChip::Impl& impl, int ch);
}

// ----- S98 v1 録音ヘルパ -------------------------------------------------
namespace {

// 32-bit Intel little-endian で書き込む。 S98 ヘッダは LE 固定。
void s98_put_le32(FILE* fh, uint32_t v) {
	uint8_t buf[4] = {
		static_cast<uint8_t>(v        & 0xFF),
		static_cast<uint8_t>((v >>  8) & 0xFF),
		static_cast<uint8_t>((v >> 16) & 0xFF),
		static_cast<uint8_t>((v >> 24) & 0xFF),
	};
	std::fwrite(buf, 1, 4, fh);
}

// np21w 互換 sync エンコーディング:
//   intcount=1 → 0xFF
//   intcount=2 → 0xFF 0xFF
//   intcount≥3 → 0xFE + variable-length(intcount-2)
void s98_flush_intcount(YmfmOpnaChip::Impl& impl) {
	if (impl.s98_intcount == 0 || !impl.s98_fh) return;
	if (impl.s98_intcount == 1) {
		std::fputc(0xFF, impl.s98_fh);
	} else if (impl.s98_intcount == 2) {
		std::fputc(0xFF, impl.s98_fh);
		std::fputc(0xFF, impl.s98_fh);
	} else {
		std::fputc(0xFE, impl.s98_fh);
		uint64_t n = impl.s98_intcount - 2;
		while (n > 0x7F) {
			std::fputc(static_cast<int>(0x80 | (n & 0x7F)), impl.s98_fh);
			n >>= 7;
		}
		std::fputc(static_cast<int>(n & 0x7F), impl.s98_fh);
	}
	impl.s98_intcount = 0;
}

// レジスタ書き込みを 1 回 S98 に記録 (sync を先に発行)。
// port: 0 = NORMAL2608, 1 = EXTEND2608
void s98_write_reg(YmfmOpnaChip::Impl& impl, int port, uint8_t addr, uint8_t value) {
	if (!impl.s98_fh) return;
	s98_flush_intcount(impl);
	std::fputc(port == 0 ? 0x00 : 0x01, impl.s98_fh);
	std::fputc(addr, impl.s98_fh);
	std::fputc(value, impl.s98_fh);
}

}  // namespace

void YmfmOpnaChip::write_reg(int port, uint8_t addr, uint8_t value) {
	// S98 録音中なら先に時間進行 sync を flush + opcode を書き込む。
	s98_write_reg(*m_impl, port, addr, value);

	if (port == 0) {
		m_impl->chip->write_address(addr);
		m_impl->chip->write_data(value);
	} else {
		m_impl->chip->write_address_hi(addr);
		m_impl->chip->write_data_hi(value);
	}
}

bool YmfmOpnaChip::start_s98_recording(const char* path) {
	if (m_impl->s98_fh) stop_s98_recording();
	m_impl->s98_fh = std::fopen(path, "wb");
	if (!m_impl->s98_fh) return false;

	// S98 v1 ヘッダ (0x80 byte)。 全 LE で書く。 reserved/title は 0 埋め。
	std::fwrite("S98", 1, 3, m_impl->s98_fh);
	std::fputc('1', m_impl->s98_fh);
	s98_put_le32(m_impl->s98_fh, 1);     // timerinfo numerator
	s98_put_le32(m_impl->s98_fh, 0);     // timerinfo2 denominator (0 = default 1/1000)
	s98_put_le32(m_impl->s98_fh, 0);     // compressing
	s98_put_le32(m_impl->s98_fh, 0x80);  // offset (title position = ヘッダ末尾)
	s98_put_le32(m_impl->s98_fh, 0x80);  // dumpdata (= sizeof(S98HDR))
	s98_put_le32(m_impl->s98_fh, 0);     // looppoint
	// reserved (0x24 bytes) + title (0x40 bytes) = 100 bytes、 全 0 埋め
	uint8_t zeros[0x24 + 0x40] = {};
	std::fwrite(zeros, 1, sizeof(zeros), m_impl->s98_fh);

	m_impl->s98_samples_accum = 0;
	m_impl->s98_last_ms       = 0;
	m_impl->s98_intcount      = 0;

	// ---- 初期 dump: 現在の shadow state を S98 に書き出す ----
	// これがないと録音開始前に set されたレジスタ (パン等) が記録されず、 再生時に
	// ymfm reset 直後の状態 (全 0、 全 ch pan disable) で再生される → 無音 or モノ。
	// np21w の S98_open 同様、 主要 FM/PSG レジスタを 1 ms 以内にまとめて書き出す。
	for (int ch = 0; ch < k_fm_count; ++ch) {
		flush_freq      (*m_impl, ch);
		flush_fb_alg    (*m_impl, ch);
		flush_lr_ams_pms(*m_impl, ch);
		for (int op = 0; op < k_fm_op; ++op) {
			flush_dt_mul (*m_impl, ch, op);
			flush_tl     (*m_impl, ch, op);
			flush_ks_ar  (*m_impl, ch, op);
			flush_d1r    (*m_impl, ch, op);
			flush_d2r    (*m_impl, ch, op);
			flush_sl_rr  (*m_impl, ch, op);
		}
	}
	// PSG side: mixer / tone period / volume / envelope
	write_reg(0, 0x07, m_impl->ssg_mixer);
	for (int ch = 0; ch < k_psg_count; ++ch) {
		const uint16_t tp = m_impl->ssg_tone_tp[ch];
		write_reg(0, static_cast<uint8_t>(0x00 + ch * 2),     static_cast<uint8_t>(tp & 0xFF));
		write_reg(0, static_cast<uint8_t>(0x00 + ch * 2 + 1), static_cast<uint8_t>((tp >> 8) & 0x0F));
		write_reg(0, static_cast<uint8_t>(0x08 + ch), m_impl->ssg_volume[ch]);
	}
	write_reg(0, 0x0B, static_cast<uint8_t>(m_impl->ssg_env_period & 0xFF));
	write_reg(0, 0x0C, static_cast<uint8_t>((m_impl->ssg_env_period >> 8) & 0xFF));
	write_reg(0, 0x0D, m_impl->ssg_env_shape);
	// チップ全体 LFO
	write_reg(0, 0x22, static_cast<uint8_t>(
		(m_impl->lfo_enable ? 0x08 : 0) | (m_impl->lfo_speed & 0x07)));
	// ch3 効果音モード (MUSIC.COM 常時 ON 運用) も S98 再生側へ伝える。
	write_reg(0, 0x27, 0x40);
	return true;
}

void YmfmOpnaChip::stop_s98_recording() {
	if (!m_impl->s98_fh) return;
	s98_flush_intcount(*m_impl);
	std::fputc(0xFD, m_impl->s98_fh);  // ENDMARK
	std::fclose(m_impl->s98_fh);
	m_impl->s98_fh = nullptr;
}

bool YmfmOpnaChip::is_recording_s98() const {
	return m_impl->s98_fh != nullptr;
}

// ============================================================
// 単位逆変換ヘルパ
// ============================================================
namespace {

// 秒 → AR (0..31)。実機準拠: T_AR = 30 × 2^((4-AR)/2) を逆解き。
// 0 秒以下は最速 (31)、十分長い秒は 0 (= 実質無限) にクランプ。
uint8_t seconds_to_ar(float sec) {
	if (sec <= 0.0001f) return 31;
	const float ar_f = 4.0f - 2.0f * std::log2(sec / 30.0f);
	if (ar_f <= 0.0f)  return 0;
	if (ar_f >= 31.0f) return 31;
	return static_cast<uint8_t>(ar_f + 0.5f);
}

// 秒 → DR/SR (D1R/D2R, 0..31)。AR と同じ式。
uint8_t seconds_to_dr(float sec) {
	return seconds_to_ar(sec);
}

// 秒 → RR (4-bit 0..15)。eff = 4*RR、T_RR = 30 × 2^((4-eff)/2) を逆解き。
uint8_t seconds_to_rr(float sec) {
	if (sec <= 0.0f) return 15;
	const float eff_f = 4.0f - 2.0f * std::log2(sec / 30.0f);
	// eff の上限は 60 (= RR=15 で 0.11μs)。RR=11 (28.6μs) と混同しないよう
	// 閾値は sec でなく eff で判定する。
	if (eff_f >= 60.0f) return 15;
	if (eff_f <= 0.0f)  return 0;
	const float rr_f = eff_f / 4.0f;
	return static_cast<uint8_t>(rr_f + 0.5f);
}

// 線形 SL (0..1) → 4-bit SL レジスタ値 (1 step = -3 dB)。
uint8_t sl_to_4bit(float sl_0_1) {
	if (sl_0_1 >= 1.0f) return 0;
	if (sl_0_1 <= 0.0f) return 15;
	const float db = -20.0f * std::log10(sl_0_1);
	const float sl_f = db / 3.0f;
	if (sl_f >= 15.0f) return 15;
	if (sl_f <= 0.0f)  return 0;
	return static_cast<uint8_t>(sl_f + 0.5f);
}

// dB → TL (7-bit, 0..127、1 step = 0.75 dB)。
uint8_t db_to_tl(float db) {
	if (db <= 0.0f)   return 0;
	if (db >= 95.0f)  return 127;
	const float tl_f = db / 0.75f;
	return static_cast<uint8_t>(tl_f + 0.5f);
}

// cents → DT (3-bit: bit2 = sign, bits 1-0 = magnitude)。
// 実機 DT は keycode 依存だが、ここでは「絶対 cents 値 / ~5 cents」で 0..3 に量子化する近似。
uint8_t cents_to_dt(float cents) {
	const float abs_c = std::abs(cents);
	uint8_t mag = (abs_c < 2.5f) ? 0
	            : (abs_c < 7.5f) ? 1
	            : (abs_c < 15.0f) ? 2
	            : 3;
	const uint8_t sign = (cents < 0.0f) ? 0x4 : 0x0;
	return sign | mag;
}

// Hz → F-Number (11-bit) + Block (3-bit)、 ymfm/実機 OPNA 準拠式。
//   ymfm compute_phase_step は phase_step = F × 2^block / 2 を使う (per FM tick)。
//   → hz = F × 2^(block-1) × FM_rate / 2^20、 FM_rate = master / 144
//   → F × 2^block = 2 × hz × 144 × 2^20 / master_clock_hz
// 旧 OpnaChip::hz_to_fnum_block の式 (F × 2^block = hz × 144 × 2^20 / master) は
// この公式の **半分** の F-Number を返していた = 結果として ymfm が 1 オクターブ低い
// 音を出していた。 ここでは 2 倍補正して block を 1 増やすことで A4=440Hz が
// F=1039, block=4 (実機 OPNA 公式値) になるようにする。
void hz_to_fnum_block(double hz, double master_clock_hz, uint16_t& fnum, uint8_t& block) {
	if (hz <= 0.0 || master_clock_hz <= 0.0) { fnum = 0; block = 1; return; }
	constexpr double k_div = 144.0;
	const double k_inv = k_div * static_cast<double>(1 << 20) / master_clock_hz;
	// MUSIC.COM 実機 (NP2 録音) と同じ規約: F を 617-1233 域、 block を 1-7 で選ぶ。
	// hz × k_inv は block=1 時の F に相当。
	// 例: A4=440 → F=1039 block=4、A1=55 → F=617 block=1。
	double f0 = hz * k_inv;
	int blk = 1;
	while (f0 >= 1234.0 && blk < 7) { f0 *= 0.5; ++blk; }
	while (f0 < 617.0 && blk > 1)   { f0 *= 2.0; --blk; }
	if (f0 < 0.0)    f0 = 0.0;
	if (f0 > 2047.0) f0 = 2047.0;
	fnum  = static_cast<uint16_t>(f0 + 0.5);
	block = static_cast<uint8_t>(blk);
}

// 前回 block を考慮して block 切替にヒステリシスを入れた版。
// ビブラート (周波数 ±10% 程度の変動) で block 境界を跨いでしまうと、
// fmgen (NP21W) は同じ block で fnum を動かすのに対し OURS は block 切替で
// 不連続なピッチ変化を起こす。BOSS.MML の t≈12.2s 等で「block=5↔4 を行き来」
// が観測されたため、 256 ≤ fnum ≤ 2047 範囲なら前回 block を維持する。
void hz_to_fnum_block_hysteresis(double hz, double master_clock_hz,
                                 uint16_t& fnum, uint8_t& block,
                                 uint8_t prev_block) {
	if (hz <= 0.0 || master_clock_hz <= 0.0) { fnum = 0; block = 1; return; }
	constexpr double k_div = 144.0;
	const double k_inv = k_div * static_cast<double>(1 << 20) / master_clock_hz;
	const double f_base = hz * k_inv;  // block=1 時の F-Number
	// 前回 block が有効 (1-7) なら、その block での F-Number が範囲内か確認
	if (prev_block >= 1 && prev_block <= 7) {
		const double f_at_prev = f_base / static_cast<double>(1 << (prev_block - 1));
		if (f_at_prev >= 256.0 && f_at_prev <= 2047.0) {
			// ヒステリシス成立: block 維持して fnum のみ更新
			fnum  = static_cast<uint16_t>(f_at_prev + 0.5);
			block = prev_block;
			return;
		}
	}
	// 範囲外 → 通常の選定 (617-1233 域へ)
	hz_to_fnum_block(hz, master_clock_hz, fnum, block);
}

// ch (0..5) → (port, ch_low (0..2)) ペア。FM ch4-6 は port 1 でアドレス 0xA0-0xB7 等を共有。
inline void ch_to_port(int ch, int& port, int& ch_low) {
	if (ch < 3) { port = 0; ch_low = ch; }
	else         { port = 1; ch_low = ch - 3; }
}

// 実機 OPNA レジスタアドレスの op (bits 2-3) は S1/S3/S2/S4 順:
//   bits=00 → S1 (op1)
//   bits=01 → S3 (op3)
//   bits=10 → S2 (op2)
//   bits=11 → S4 (op4)
// np21w で MUSIC.COM を実行した S98 録音 (NP2_agm01.s98) と比較した結果、
// 実機 MUSIC.COM は MML の op1/2/3/4 を上記公式 slot 配置で書き込んでいる。
// 我々の MML op 0..3 (= op1..op4) → slot 0/2/1/3 にマップして同じ配置にする。
inline uint8_t op_addr_offset(int op) {
	static constexpr uint8_t k_mml_op_to_slot[4] = { 0, 2, 1, 3 };
	return static_cast<uint8_t>(k_mml_op_to_slot[op] << 2);
}

}  // namespace

// ============================================================
// レジスタ再書き込みヘルパ (Impl の shadow を読み取り 1 レジスタ分書く)
// ============================================================

namespace {

// op レベル: addr = base + op_offset + ch_low
// S98 録音にも反映するため、 直接 ymfm を叩かず s98_write_reg + chip 両方を経由する。
void write_op_reg(YmfmOpnaChip::Impl& impl, int ch, int op, uint8_t base, uint8_t value) {
	int port, ch_low;
	ch_to_port(ch, port, ch_low);
	const uint8_t addr = base + op_addr_offset(op) + static_cast<uint8_t>(ch_low);
	s98_write_reg(impl, port, addr, value);
	if (port == 0) { impl.chip->write_address(addr);    impl.chip->write_data(value); }
	else            { impl.chip->write_address_hi(addr); impl.chip->write_data_hi(value); }
}

// ch レベル: addr = base + ch_low
void write_ch_reg(YmfmOpnaChip::Impl& impl, int ch, uint8_t base, uint8_t value) {
	int port, ch_low;
	ch_to_port(ch, port, ch_low);
	const uint8_t addr = base + static_cast<uint8_t>(ch_low);
	s98_write_reg(impl, port, addr, value);
	if (port == 0) { impl.chip->write_address(addr);    impl.chip->write_data(value); }
	else            { impl.chip->write_address_hi(addr); impl.chip->write_data_hi(value); }
}

// shadow から DT/MUL レジスタ (0x30..) を構築して書き込む。
void flush_dt_mul(YmfmOpnaChip::Impl& impl, int ch, int op) {
	const auto& o = impl.op[ch][op];
	write_op_reg(impl, ch, op, 0x30, static_cast<uint8_t>((o.dt << 4) | (o.mul & 0x0F)));
}
// TL (0x40..)
void flush_tl(YmfmOpnaChip::Impl& impl, int ch, int op) {
	write_op_reg(impl, ch, op, 0x40, impl.op[ch][op].tl & 0x7F);
}
// KS/AR (0x50..)
void flush_ks_ar(YmfmOpnaChip::Impl& impl, int ch, int op) {
	const auto& o = impl.op[ch][op];
	write_op_reg(impl, ch, op, 0x50, static_cast<uint8_t>((o.ks << 6) | (o.ar & 0x1F)));
}
// AM/D1R (0x60..)。AM bit は LFO AMS と連動するが、現状は 0 で固定。
void flush_d1r(YmfmOpnaChip::Impl& impl, int ch, int op) {
	write_op_reg(impl, ch, op, 0x60, impl.op[ch][op].d1r & 0x1F);
}
// D2R (0x70..)
void flush_d2r(YmfmOpnaChip::Impl& impl, int ch, int op) {
	write_op_reg(impl, ch, op, 0x70, impl.op[ch][op].d2r & 0x1F);
}
// SL/RR (0x80..)
void flush_sl_rr(YmfmOpnaChip::Impl& impl, int ch, int op) {
	const auto& o = impl.op[ch][op];
	write_op_reg(impl, ch, op, 0x80, static_cast<uint8_t>((o.sl << 4) | (o.rr & 0x0F)));
}
// FB/ALG (0xB0..)
void flush_fb_alg(YmfmOpnaChip::Impl& impl, int ch) {
	const auto& c = impl.ch[ch];
	write_ch_reg(impl, ch, 0xB0, static_cast<uint8_t>((c.fb << 3) | (c.alg & 0x07)));
}
// L/R/AMS/PMS (0xB4..)
void flush_lr_ams_pms(YmfmOpnaChip::Impl& impl, int ch) {
	const auto& c = impl.ch[ch];
	const uint8_t v = static_cast<uint8_t>(
		(c.pan_l ? 0x80 : 0) | (c.pan_r ? 0x40 : 0) |
		((c.ams & 0x03) << 4) | (c.pms & 0x07));
	write_ch_reg(impl, ch, 0xB4, v);
}
// F-Number low (0xA0..)、F-Number high + block (0xA4..)。
// 実機要件: A4 を先に書いてから A0 で「ラッチ」される。
void flush_freq(YmfmOpnaChip::Impl& impl, int ch) {
	const auto& c = impl.ch[ch];
	const uint8_t hi = static_cast<uint8_t>((c.block << 3) | ((c.fnum >> 8) & 0x07));
	const uint8_t lo = static_cast<uint8_t>(c.fnum & 0xFF);
	write_ch_reg(impl, ch, 0xA4, hi);
	write_ch_reg(impl, ch, 0xA0, lo);
	// FM ch3 (idx 2) は MUSIC.COM が常時「ch3 効果音モード」(reg 0x27 bit6=1) で
	// 運用し、ノートごとにオペレータ個別周波数 (A8-AE) へ同値をミラーする
	// (musiccom_emu の ch3 実験で実証。書き込み順も実機ドライバ準拠:
	//  (A6,A2) → (AE,AA) → (AC,A8) → (AD,A9)、各対 hi → lo)。
	if (ch == 2) {
		static constexpr uint8_t hi_regs[3] = { 0xAE, 0xAC, 0xAD };
		static constexpr uint8_t lo_regs[3] = { 0xAA, 0xA8, 0xA9 };
		for (int i = 0; i < 3; ++i) {
			s98_write_reg(impl, 0, hi_regs[i], hi);
			impl.chip->write_address(hi_regs[i]);
			impl.chip->write_data(hi);
			s98_write_reg(impl, 0, lo_regs[i], lo);
			impl.chip->write_address(lo_regs[i]);
			impl.chip->write_data(lo);
		}
	}
}

}  // namespace

// ============================================================
// IOpnaChip 実装
// ============================================================

void YmfmOpnaChip::fm_key_on(int ch) {
	if (ch < 0 || ch >= k_fm_count) return;
	// 28h: bits 7-4 = op4..op1 key、bits 2-0 = ch select。
	// ch select は ch 0..2 → 0..2、ch 3..5 → 4..6 (bit 2 set)。
	const uint8_t ch_sel = (ch < 3) ? static_cast<uint8_t>(ch)
	                                : static_cast<uint8_t>(ch - 3 + 4);
	write_reg(0, 0x28, static_cast<uint8_t>(0xF0 | ch_sel));
	// 次の set_fm_frequency_hz でヒステリシスをリセットし fresh に block を再選定させる。
	// ノート切替時は前回 block 維持より、 そのノートに最適な block を選び直す。
	// (同一ノート内のビブラート等は key_on 後の小変動なので block 維持で OK。)
	m_impl->ch[ch].block = 0;
}

void YmfmOpnaChip::fm_key_off(int ch) {
	if (ch < 0 || ch >= k_fm_count) return;
	const uint8_t ch_sel = (ch < 3) ? static_cast<uint8_t>(ch)
	                                : static_cast<uint8_t>(ch - 3 + 4);
	write_reg(0, 0x28, ch_sel);
}

void YmfmOpnaChip::set_fm_frequency_hz(int ch, double hz) {
	if (ch < 0 || ch >= k_fm_count) return;
	// 前回 block を維持するヒステリシス選定 (ノート切替は fm_key_on で block=0 リセット)。
	hz_to_fnum_block_hysteresis(hz, m_master_clock_hz,
	                            m_impl->ch[ch].fnum, m_impl->ch[ch].block,
	                            m_impl->ch[ch].block);
	flush_freq(*m_impl, ch);
}

void YmfmOpnaChip::set_fm_algorithm(int ch, uint8_t alg_3bit) {
	if (ch < 0 || ch >= k_fm_count) return;
	m_impl->ch[ch].alg = alg_3bit & 0x07;
	flush_fb_alg(*m_impl, ch);
}

void YmfmOpnaChip::set_fm_feedback(int ch, uint8_t fb_3bit) {
	if (ch < 0 || ch >= k_fm_count) return;
	m_impl->ch[ch].fb = fb_3bit & 0x07;
	flush_fb_alg(*m_impl, ch);
}

void YmfmOpnaChip::set_fm_algorithm_feedback(int ch, uint8_t alg_3bit, uint8_t fb_3bit) {
	if (ch < 0 || ch >= k_fm_count) return;
	// ALG/FB の両 shadow を更新してから 0xB0 を 1 回だけ書く (中間値を出さない)。
	m_impl->ch[ch].alg = alg_3bit & 0x07;
	m_impl->ch[ch].fb  = fb_3bit  & 0x07;
	flush_fb_alg(*m_impl, ch);
}

void YmfmOpnaChip::set_fm_lfo(int ch, uint8_t /*waveform*/, uint8_t speed, uint8_t depth) {
	if (ch < 0 || ch >= k_fm_count) return;
	// MUSIC.COM はチップ全体 LFO を使わないため、depth を PMS にマップする
	// (一般的な「ソフト LFO 代替」想定)。AMS は未使用。
	m_impl->ch[ch].pms = depth & 0x07;
	flush_lr_ams_pms(*m_impl, ch);

	// LFO speed を反映 (チップ全体だが setter 経由で更新)。speed=0 は LFO 無効と解釈。
	const bool new_enable = (speed > 0);
	const uint8_t new_speed = speed & 0x07;
	if (new_enable != m_impl->lfo_enable || new_speed != m_impl->lfo_speed) {
		m_impl->lfo_enable = new_enable;
		m_impl->lfo_speed  = new_speed;
		const uint8_t v = static_cast<uint8_t>((new_enable ? 0x08 : 0) | new_speed);
		write_reg(0, 0x22, v);
	}
}

void YmfmOpnaChip::set_fm_pan(int ch, Pan pan) {
	if (ch < 0 || ch >= k_fm_count) return;
	const uint8_t p = static_cast<uint8_t>(pan);
	m_impl->ch[ch].pan_l = (p & 0x2) ? 1 : 0;
	m_impl->ch[ch].pan_r = (p & 0x1) ? 1 : 0;
	flush_lr_ams_pms(*m_impl, ch);
}

void YmfmOpnaChip::set_fm_op_attack_rate(int ch, int op, float seconds) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_impl->op[ch][op].ar = seconds_to_ar(seconds);
	flush_ks_ar(*m_impl, ch, op);
}

void YmfmOpnaChip::set_fm_op_decay1_rate(int ch, int op, float seconds) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_impl->op[ch][op].d1r = seconds_to_dr(seconds);
	flush_d1r(*m_impl, ch, op);
}

void YmfmOpnaChip::set_fm_op_sustain_level(int ch, int op, float sl_0_to_1) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_impl->op[ch][op].sl = sl_to_4bit(sl_0_to_1);
	flush_sl_rr(*m_impl, ch, op);
}

void YmfmOpnaChip::set_fm_op_decay2_rate(int ch, int op, float seconds) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_impl->op[ch][op].d2r = seconds_to_dr(seconds);
	flush_d2r(*m_impl, ch, op);
}

void YmfmOpnaChip::set_fm_op_release_rate(int ch, int op, float seconds) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_impl->op[ch][op].rr = seconds_to_rr(seconds);
	flush_sl_rr(*m_impl, ch, op);
}

void YmfmOpnaChip::set_fm_op_total_level_db(int ch, int op, float db) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_impl->op[ch][op].tl = db_to_tl(db);
	flush_tl(*m_impl, ch, op);
}

void YmfmOpnaChip::set_fm_op_multiplier(int ch, int op, uint8_t mul_4bit) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_impl->op[ch][op].mul = mul_4bit & 0x0F;
	flush_dt_mul(*m_impl, ch, op);
}

void YmfmOpnaChip::set_fm_op_detune_cents(int ch, int op, float cents) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_impl->op[ch][op].dt = cents_to_dt(cents);
	flush_dt_mul(*m_impl, ch, op);
}

void YmfmOpnaChip::set_fm_op_key_scaling(int ch, int op, uint8_t ks_2bit) {
	if (ch < 0 || ch >= k_fm_count || op < 0 || op >= k_fm_op) return;
	m_impl->op[ch][op].ks = ks_2bit & 0x03;
	flush_ks_ar(*m_impl, ch, op);
}

// ---- PSG ----
// OPNA SSG レジスタは port 0 の 0x00..0x0F。
//   0x00-0x05: tone period (ch A/B/C × low/high)
//   0x06     : noise period (5-bit)
//   0x07     : mixer (bit 0-2 = tone disable A/B/C、bit 3-5 = noise disable A/B/C)
//   0x08-0x0A: ch volume A/B/C (5-bit、bit 4 = envelope follow)
//   0x0B-0x0C: envelope period (low/high)
//   0x0D     : envelope shape

void YmfmOpnaChip::psg_reset() {
	// 全 ch tone/noise disable + volume 0 でクリア。
	for (uint8_t reg = 0x00; reg <= 0x0D; ++reg) {
		write_reg(0, reg, 0);
	}
	m_impl->ssg_mixer       = 0xBF;  // 全 disable + bit7 (IO port B 出力。MUSIC.COM が常時セットする)
	for (int i = 0; i < 3; ++i) {
		m_impl->ssg_tone_tp[i] = 0;
		m_impl->ssg_volume[i]  = 0;
	}
	m_impl->ssg_env_period = 0;
	m_impl->ssg_env_shape  = 0;
	// reg 0x07: 初期は全 disable。MmlPlayer が set_psg_mixer で順次 enable する。
	write_reg(0, 0x07, m_impl->ssg_mixer);
}

void YmfmOpnaChip::set_psg_tone_frequency(int ch, double hz) {
	if (ch < 0 || ch >= k_psg_count) return;
	// PSG クロック = master / 4 (OPNA 既定)。TP = clock / (16 * Hz)。
	// MUSIC.COM は整数除算 (切り捨て = floor) で TP を求める。musiccom_emu の SSG
	// 半音階実測で全 12 音が floor と一致 (C+/F/F+/G 等で四捨五入だと +1 ずれて
	// 約 6 cents 低くなっていた)。+0.5 は付けない。
	const double psg_clock = m_master_clock_hz / 4.0;
	uint16_t tp = (hz > 0.0) ? static_cast<uint16_t>(psg_clock / (16.0 * hz)) : 0;
	if (tp > 0xFFF) tp = 0xFFF;
	m_impl->ssg_tone_tp[ch] = tp;
	write_reg(0, static_cast<uint8_t>(0x00 + ch * 2),     static_cast<uint8_t>(tp & 0xFF));
	write_reg(0, static_cast<uint8_t>(0x00 + ch * 2 + 1), static_cast<uint8_t>((tp >> 8) & 0x0F));
}

void YmfmOpnaChip::set_psg_channel_volume(int ch, uint8_t volume_5bit) {
	if (ch < 0 || ch >= k_psg_count) return;
	m_impl->ssg_volume[ch] = volume_5bit & 0x1F;
	write_reg(0, static_cast<uint8_t>(0x08 + ch), m_impl->ssg_volume[ch]);
}

void YmfmOpnaChip::set_psg_mixer(int ch, bool tone, bool noise) {
	if (ch < 0 || ch >= k_psg_count) return;
	// bit が 1 で disable。tone/noise enable に対しビット反転して反映。
	uint8_t m = m_impl->ssg_mixer;
	const uint8_t tone_bit  = static_cast<uint8_t>(1 << ch);
	const uint8_t noise_bit = static_cast<uint8_t>(1 << (ch + 3));
	if (tone)  m &= ~tone_bit;  else m |= tone_bit;
	if (noise) m &= ~noise_bit; else m |= noise_bit;
	m_impl->ssg_mixer = m;
	write_reg(0, 0x07, m);
}

void YmfmOpnaChip::set_psg_envelope_shape(uint8_t shape_4bit) {
	m_impl->ssg_env_shape = shape_4bit & 0x0F;
	// 0x0D 書き込みで実機はエンベロープが再起動する。
	write_reg(0, 0x0D, m_impl->ssg_env_shape);
}

void YmfmOpnaChip::set_psg_envelope_period(uint16_t period_16bit) {
	m_impl->ssg_env_period = period_16bit;
	write_reg(0, 0x0B, static_cast<uint8_t>(period_16bit & 0xFF));
	write_reg(0, 0x0C, static_cast<uint8_t>((period_16bit >> 8) & 0xFF));
}

void YmfmOpnaChip::set_fm_volume (float linear) { m_fm_volume  = linear; }
void YmfmOpnaChip::set_psg_volume(float linear) { m_ssg_volume = linear; }

void YmfmOpnaChip::render(int16_t* dst, uint32_t frames, uint32_t channels) {
	if (frames == 0 || m_ymfm_rate == 0) return;

	// S98 録音: 経過サンプル数を ms に換算し intcount に累積。
	// 次の write_reg 呼び出し時に flush される。
	if (m_impl->s98_fh) {
		m_impl->s98_samples_accum += frames;
		const uint64_t total_ms = m_impl->s98_samples_accum * 1000 / m_output_rate;
		if (total_ms > m_impl->s98_last_ms) {
			m_impl->s98_intcount += (total_ms - m_impl->s98_last_ms);
			m_impl->s98_last_ms = total_ms;
		}
	}

	// 出力 N frames を埋めるのに必要な ymfm 側サンプル数を概算 (+1 で右端補間分)。
	const uint64_t needed_pos =
		static_cast<uint64_t>(m_impl->resample_step) * frames + (1ULL << 32);
	const uint32_t needed_ymfm_samples =
		static_cast<uint32_t>(needed_pos >> 32) + 1;
	if (m_impl->ymfm_buf.size() < needed_ymfm_samples) {
		m_impl->ymfm_buf.resize(needed_ymfm_samples);
	}

	// ymfm から一括取得 (ymfm 側でレジスタ反映と内部 advance を行う)。
	m_impl->chip->generate(m_impl->ymfm_buf.data(), needed_ymfm_samples);

	// 線形補間で 48 kHz (m_output_rate) にリサンプル。
	// resample_pos は前回 render の余りを引き継ぎ、間引き境界を滑らかに保つ。
	const float fm_gain  = m_fm_volume;
	const float ssg_gain = m_ssg_volume;

	uint64_t pos = m_impl->resample_pos;
	for (uint32_t i = 0; i < frames; ++i) {
		const uint32_t idx_int  = static_cast<uint32_t>(pos >> 32);
		const uint32_t frac     = static_cast<uint32_t>(pos & 0xFFFFFFFFu);
		const float    t        = static_cast<float>(frac) * (1.0f / 4294967296.0f);

		// 補間左端: idx_int が 0 のとき前回 render の last_xxx を使う。
		int32_t a_l, a_r, a_s;
		if (idx_int == 0) {
			a_l = m_impl->last_fm_l;
			a_r = m_impl->last_fm_r;
			a_s = m_impl->last_ssg;
		} else {
			const auto& a = m_impl->ymfm_buf[idx_int - 1];
			a_l = a.data[0];
			a_r = a.data[1];
			a_s = a.data[2];
		}
		const auto& b   = m_impl->ymfm_buf[std::min(idx_int, needed_ymfm_samples - 1)];
		const int32_t b_l = b.data[0];
		const int32_t b_r = b.data[1];
		const int32_t b_s = b.data[2];

		// 線形補間 (float)。ymfm は int32 を返すが OPNA は実質 14 bit signed × 数チャンネル
		// 加算なので、float に落としても精度は十分。
		float fm_l_f            = (a_l + (b_l - a_l) * t) * fm_gain;
		float fm_r_f            = (a_r + (b_r - a_r) * t) * fm_gain;
		const float ssg_raw     = (a_s + (b_s - a_s) * t);

		// --- FM 高域シェルフ補正 (NP2 opngen 寄せ) ---
		// ymfm の FB 音色は NP2 (opngen) 比で高域が明るい (BOSS ch1 @11 FB=7 で
		// 8-16kHz +2.7dB)。FM のみ緩い高域シェルフ (fc≈6.5kHz, -3dB) で抑える。
		// 低域ブーストは曲依存 (BOSS は不足だが AGM01 は過多) のため掛けない。
		// 1-pole: lp += a*(x-lp)、out = g*x + (1-g)*lp (低域は素通し、高域のみ g 倍)。
		constexpr float k_tilt_a_hi = 0.560f;   // ≈ 1-exp(-2pi*6500/48000)
		constexpr float k_tilt_g_hi = 0.708f;   // -3.0 dB
		auto fm_tilt = [&](float x, float& lp_hi) -> float {
			lp_hi += k_tilt_a_hi * (x - lp_hi);
			return k_tilt_g_hi * x + (1.0f - k_tilt_g_hi) * lp_hi;
		};
		fm_l_f = fm_tilt(fm_l_f, m_impl->fm_tilt_hi_l);
		fm_r_f = fm_tilt(fm_r_f, m_impl->fm_tilt_hi_r);

		// SSG の DC 成分を除去 (実機カップリングコンデンサ相当)。
		// HPF: y = x - x_prev + R · y_prev、R=0.999 で fc ~ 7.6 Hz @ 48 kHz。
		constexpr float k_hpf_r = 0.999f;
		const float ssg_hpf_y = ssg_raw - m_impl->ssg_hpf_x_prev + k_hpf_r * m_impl->ssg_hpf_y_prev;
		m_impl->ssg_hpf_x_prev = ssg_raw;
		m_impl->ssg_hpf_y_prev = ssg_hpf_y;
		const float ssg_f = ssg_hpf_y * ssg_gain;

		// FM 出力は int32 で int16 範囲を超えるのが普通 (内部ヘッドルーム)。
		// ymfm.clamp16 相当の飽和を float 段で実施。
		auto clip_i16 = [](float v) -> int16_t {
			if (v >  32767.0f) return  32767;
			if (v < -32768.0f) return -32768;
			return static_cast<int16_t>(v);
		};

		const int16_t lv = clip_i16(fm_l_f + ssg_f);
		const int16_t rv = clip_i16(fm_r_f + ssg_f);

		if (channels >= 2) {
			dst[i * channels + 0] = lv;
			dst[i * channels + 1] = rv;
			for (uint32_t c = 2; c < channels; ++c) {
				dst[i * channels + c] = 0;
			}
		} else {
			dst[i] = static_cast<int16_t>((static_cast<int32_t>(lv) + static_cast<int32_t>(rv)) / 2);
		}

		pos += m_impl->resample_step;
	}

	// 次回呼び出しに引き継ぐ位置と「左端」サンプル。
	const uint32_t consumed_int = static_cast<uint32_t>(pos >> 32);
	if (consumed_int > 0 && consumed_int <= needed_ymfm_samples) {
		const auto& last = m_impl->ymfm_buf[consumed_int - 1];
		m_impl->last_fm_l = last.data[0];
		m_impl->last_fm_r = last.data[1];
		m_impl->last_ssg  = last.data[2];
	}
	// pos の整数部を切り捨て、小数部だけ次回に持ち越す (resample_pos は次回の起点)。
	m_impl->resample_pos = pos & 0xFFFFFFFFu;
}
