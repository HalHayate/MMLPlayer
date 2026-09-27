#pragma once
//
// IWaveGenerator: 単一波形ソースの最小インターフェース。
//
// このインターフェースは「1 サンプル単位で音を吐く」純粋な発振器を表す。
// ピッチ制御や ADSR、複数オペレータ間の変調といった上位概念は
// Voice / Synthesizer / (将来の) FmChannel 側で扱う。
//
// 後段で OPNA 互換 (FM/PSG) を載せるため、以下を意図的に分離している:
//   - 周波数 → 位相増分の計算は各実装側に委譲 (FM では cents/detune が絡む)
//   - 状態リセットを reset() に分離 (ノートオン時に位相を揃えるため)
//

#include <cstdint>

class IWaveGenerator {
public:
	virtual ~IWaveGenerator() = default;

	// 出力周波数を設定する。サンプルレートは Voice / Synthesizer から渡される。
	virtual void set_frequency(double hz, uint32_t sample_rate) = 0;

	// 1 サンプル進めて [-1.0, 1.0] の範囲で返す。
	// レンダースレッドからのみ呼ばれる前提。
	virtual float next() = 0;

	// 内部状態 (位相、LFSR など) を初期化する。ノートオン時に呼ぶ。
	virtual void reset() = 0;
};
