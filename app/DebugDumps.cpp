#include "DebugDumps.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "CliPrint.h"
#include "mml/MmlFileParser.h"
#include "mml/MmlScore.h"
#include "mml/MmlEvent.h"
#include "mml/SoundDat.h"

namespace app {

int run_dump_d_events(const char* mml_path) {
	std::ifstream ifs(mml_path, std::ios::binary);
	if (!ifs) {
		cli_fprintf(stderr, "cannot open %s\n", mml_path);
		return 1;
	}
	std::stringstream ss;
	ss << ifs.rdbuf();
	mml::Score      score;
	mml::FileParser fp;
	fp.parse(ss.str(), score);
	const double ticks_per_sec = 120.0 * mml::k_ticks_per_quarter_note / 60.0;
	cli_printf("=== D part events (%zu) ===\n", score.d_part.size());
	for (const auto& ev : score.d_part) {
		const double t_sec = ev.time_ticks / ticks_per_sec;
		const char* k =
			(ev.kind == mml::EventKind::EffectTrigger) ? "EffectTrigger" :
			(ev.kind == mml::EventKind::Rest)          ? "Rest" :
			(ev.kind == mml::EventKind::Tempo)         ? "Tempo" :
			                                              "?";
		cli_printf("  t=%6d (%5.2fs) %-14s a=%-3d b=%-4d\n",
		            ev.time_ticks, t_sec, k, ev.a, ev.b);
	}
	cli_printf("warnings: %zu\n", score.warnings.size());
	for (size_t i = 0; i < score.warnings.size() && i < 20; ++i) {
		cli_printf("  %s\n", score.warnings[i].c_str());
	}
	return 0;
}

int run_dump_sound_dat(const char* sound_dat_path) {
	mml::SoundDat            dat;
	std::vector<std::string> warns;
	const bool ok = dat.load(sound_dat_path, warns);
	cli_printf("[SOUND.DAT] load %s -> %s (effects=%zu)\n",
	            sound_dat_path, ok ? "ok" : "fail", dat.size());
	for (const auto& w : warns) cli_printf("  warn: %s\n", w.c_str());
	for (int id : dat.ids()) {
		const auto* eff = dat.find(id);
		if (!eff) continue;
		cli_printf("  @%d: stages=%zu\n", id, eff->stages.size());
		for (size_t s = 0; s < eff->stages.size(); ++s) {
			const auto& st = eff->stages[s];
			cli_printf(
				"    [%zu] enable=0x%02x p1=%u  T1(f=%u,step=%d,dur=%u,d2=%u) "
				"T2(f=%u,step=%d,dur=%u,d2=%u)\n",
				s, st.enable_flag, st.p1,
				st.tone[0].f, st.tone[0].step, st.tone[0].duration, st.tone[0].d2,
				st.tone[1].f, st.tone[1].step, st.tone[1].duration, st.tone[1].d2);
			cli_printf(
				"         V1(v=%u,step=%d,dur=%u,d2=%u) V2(v=%u,step=%d,dur=%u,d2=%u) "
				"N(n=%u,step=%d,dur=%u,d2=%u)\n",
				st.vol[0].v_8_8, st.vol[0].step, st.vol[0].duration, st.vol[0].d2,
				st.vol[1].v_8_8, st.vol[1].step, st.vol[1].duration, st.vol[1].d2,
				st.noise.n_8_8, st.noise.step, st.noise.duration, st.noise.d2);
		}
	}
	return ok ? 0 : 1;
}

void dump_channel_events(const mml::Score& score, int ch) {
	const auto& events = score.channels[ch - 1];
	cli_printf("=== ch%d events (%zu) ===\n", ch, events.size());
	// tick → 秒換算 (T120 と仮定、 k_ticks_per_quarter_note=192 → 384 ticks/sec)。
	const double ticks_per_sec = 120.0 * mml::k_ticks_per_quarter_note / 60.0;
	const char* kind_names[] = {
		"Note", "Rest", "Tempo", "Volume", "Timbre", "GateTime",
		"PitchOffset", "Vibrato", "Tremolo", "Portamento",
		"SsgHwEnvShape", "SsgHwEnvPeriod", "YRegWrite", "EffectTrigger"
	};
	const size_t n_kinds = sizeof(kind_names) / sizeof(kind_names[0]);
	for (const auto& ev : events) {
		const double t_sec = ev.time_ticks / ticks_per_sec;
		const char*  k     = (size_t(ev.kind) < n_kinds) ? kind_names[size_t(ev.kind)] : "?";
		cli_printf("  t=%6d (%5.2fs) %-15s a=%-4d b=%-4d c=%-4d f=0x%02x\n",
		            ev.time_ticks, t_sec, k, ev.a, ev.b, ev.c, ev.flags);
	}
}

}  // namespace app
