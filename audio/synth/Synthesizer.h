#pragma once
//
// Synthesizer: 複数の Voice をミックスして 16bit PCM フレームを書き出す。
//
// MeAudioStream::FillCallback とシグネチャを意図的に揃えてあり、
//   stream.start(audio, [&](int16_t* dst, uint32_t f, uint32_t c){
//       synth.render(dst, f, c);
//   });
// として直結できる。
//
// 将来:
//   FM/PSG チャンネルバンク、エフェクトチェイン、L/R パンを追加。
//

#include <cstdint>
#include <memory>
#include <vector>

class Voice;
class IWaveGenerator;

class Synthesizer {
public:
	explicit Synthesizer(uint32_t sample_rate);
	~Synthesizer();

	Synthesizer(const Synthesizer&)            = delete;
	Synthesizer& operator=(const Synthesizer&) = delete;

	// index のボイスをジェネレータと共に新規作成 / 置き換え。
	// 必要に応じて内部配列を拡張する。
	void set_voice(uint32_t index, std::unique_ptr<IWaveGenerator> generator);

	// index のボイスへの参照取得。範囲外は std::out_of_range。
	Voice& voice(uint32_t index);

	uint32_t voice_count() const;

	// マスターボリューム (0..1)。混合後にクリップ防止のため掛ける。
	void set_master_volume(float v);

	// frames フレーム × channels チャンネルを dst に書き込む。
	// 内部はモノラル合成し、各出力チャンネルに同値を複製する。
	void render(int16_t* dst, uint32_t frames, uint32_t channels);

private:
	uint32_t                            m_sample_rate;
	std::vector<std::unique_ptr<Voice>> m_voices;
	float                               m_master_volume = 0.5f;
};
