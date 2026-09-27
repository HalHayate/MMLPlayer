cmake_minimum_required(VERSION 3.20)
project(MMLPlayer C CXX)  # opngen/psggen は C ソースなので C も有効化

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# UTF-8 ソースを MSVC で正しく扱う
if(MSVC)
	# /wd4996 = C4996 抑制 ("fopen は安全でない" 等の MSVC 固有警告)。
	# fopen 等の標準 C 関数を fopen_s に置換すると移植性を損なうため抑制する方針。
	# _CRT_SECURE_NO_WARNINGS マクロも併用 (CRT 内部での同警告も含めて確実に抑制)。
	# /wd4828 = Shift-JIS の fmgen コメントが /utf-8 で出す文字化け警告を抑制
	# (fmgen ソースは cp932。コメントのみなので無害。ヘッダを include する側でも出る)
	add_compile_options(/utf-8 /permissive- /wd4996 /wd4828)
	add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# CORE_SRCS: チップ非依存のシンセ + MML コア。exe と .kpi プラグインで共用する。
# CLI 層 (app/) や XAudio2 (audio/MeAudio*) には非依存なので DLL からも安全に使える。
set(CORE_SRCS
	audio/synth/Envelope.cpp
	audio/synth/Generators.cpp
	audio/synth/Voice.cpp
	audio/synth/Synthesizer.cpp
	audio/synth/psg/PsgChip.cpp
	audio/synth/fm/FmOperator.cpp
	audio/synth/fm/FmChannel.cpp
	audio/synth/fm/OpnTables.cpp
	audio/synth/opna/OpnaChip.cpp
	# MmlTimbre.h はヘッダのみ
	# ymfm_fm.ipp はテンプレ実装、ymfm_opn.cpp が include する
	# ymfm ラッパ (MMLPlayer 側コード、警告レベルは /W4 維持)
	audio/synth/ymfm_chip/YmfmOpnaChip.cpp
	# fmgen ラッパ (MMLPlayer 側コード)
	audio/synth/fmgen_chip/FmgenOpnaChip.cpp
	# opngen ラッパ (MMLPlayer 側コード)
	audio/synth/opngen_chip/OpngenOpnaChip.cpp
	# mml コマンドのパーサ (MMLPlayer 側コード)
	mml/MmlMacroTable.cpp
	mml/MmlBodyParser.cpp
	mml/MmlFileParser.cpp
	mml/MmlPlayer.cpp
	mml/SoundDat.cpp
	mml/DPartParser.cpp
)

# APP_SRCS: 単独 exe 専用。CLI 補助 (引数パース / ヘルプ / WAV 出力 / オフライン
# レンダ / デバッグ群) と XAudio2 リアルタイム再生。プラグインには含めない。
set(APP_SRCS
	app/WaveWriter.cpp
	app/Render.cpp
	app/CliHelp.cpp
	app/CliPrint.cpp
	app/YmfmTests.cpp
	app/S98Player.cpp
	app/DebugDumps.cpp
	audio/MeAudio.cpp
	audio/MeAudioStream.cpp
)

set(YMFM_SRCS
	# ymfm (Aaron Giles, BSD 3-Clause) - 詳細は audio/synth/ymfm/LICENSE.txt
	audio/synth/ymfm/ymfm_opn.cpp
	audio/synth/ymfm/ymfm_ssg.cpp
	audio/synth/ymfm/ymfm_adpcm.cpp
)

set(FMGEN_SRCS
	# fmgen (cisc, np21w 同梱版) - 第3の OPNA バックエンド
	audio/synth/fmgen/fmgen_fmgen.cpp
	audio/synth/fmgen/fmgen_opna.cpp
	audio/synth/fmgen/fmgen_psg.cpp
	audio/synth/fmgen/fmgen_fmtimer.cpp
	audio/synth/fmgen/fmgen_file.cpp
)

set(OPNGEN_SRCS
	# opngen (np21w 内蔵 OPN generator + PSG) - 第4の OPNA バックエンド (NP2 参照と同一エンジン)
	audio/synth/opngen/opngenc.c
	audio/synth/opngen/opngeng.c
	audio/synth/opngen/psggenc.c
	audio/synth/opngen/psggeng.c
)

add_executable(MMLPlayer
	main.cpp
	${CORE_SRCS}
	${APP_SRCS}
	${YMFM_SRCS}
	${FMGEN_SRCS}
	${OPNGEN_SRCS}
)

target_include_directories(MMLPlayer PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
)

# ymfm はテンプレ多用 + 多数の警告 (符号比較等) を出すため、W4 strict から緩和。
# 自前コードの警告は維持したいので、ymfm のソースだけ個別に下げる。
if(MSVC)
	# MMLPlayer 側コード (core + app + kpi) は /W4 を維持。 backend ソースは /W1 に下げる。
	# kpi/*.cpp は MMLPLAYER_BUILD_KPI が OFF でも未使用ソースへの property 付与は無害。
	set_property(SOURCE ${CORE_SRCS} ${APP_SRCS} kpi/KpiPlugin.cpp kpi/KpiSelfTest.cpp
		APPEND PROPERTY COMPILE_OPTIONS "/W4")
	# ymfm ソースは /W1 に下げる。暗黙の符号変換等、警告が大量に出るため。
	set_property(SOURCE ${YMFM_SRCS} APPEND PROPERTY COMPILE_OPTIONS
		"/W1;/wd4100;/wd4244;/wd4245;/wd4267;/wd4389"
	)
	# fmgen (cisc 製、 C 寄りのスタイルで多数の警告) も警告緩和する。
	set_property(SOURCE ${FMGEN_SRCS} APPEND PROPERTY COMPILE_OPTIONS
		# /wd4828 = Shift-JIS コメントの文字化け警告抑制 (fmgen ソースは cp932)
		"/W1;/wd4100;/wd4244;/wd4245;/wd4267;/wd4389;/wd4018;/wd4828"
	)
	# opngen/psggen (np21w, C ソース。 多数の暗黙変換警告) も緩和。
	set_property(SOURCE ${OPNGEN_SRCS} APPEND PROPERTY COMPILE_OPTIONS
		# /wd4828 = Shift-JIS コメントの文字化け警告抑制 (fmgen ソースは cp932)
		"/W1;/wd4100;/wd4244;/wd4245;/wd4267;/wd4389;/wd4018;/wd4828"
	)
endif()

# fmgen は全ソースが #if defined(SUPPORT_FMGEN) で囲まれているため有効化する。
add_compile_definitions(SUPPORT_FMGEN)
# opngenc.c が M_PI を使う。MSVC は _USE_MATH_DEFINES が無いと <math.h> で定義しない。
add_compile_definitions(_USE_MATH_DEFINES)

if(WIN32)
	# XAudio2 と COM 用ライブラリ
	target_link_libraries(MMLPlayer PRIVATE xaudio2 ole32)
endif()

# ---- KbMedia Player プラグイン (.kpi) ----
# MML シンセを KMPMODULE デコーダ型プラグインとして DLL 化する (SDK: Materials/kpisdk)。
# core (CORE_SRCS + backends) を exe と共有し、kpi/KpiPlugin.cpp と .def を加える。
# 本体とビット数が一致した .kpi のみロード可能 (x86/x64 各ビルドで生成する)。
option(MMLPLAYER_BUILD_KPI "Build KbMedia Player plugin (.kpi)" ON)

if(MMLPLAYER_BUILD_KPI AND WIN32)
	add_library(MMLPlayerKpi SHARED
		kpi/KpiPlugin.cpp
		kpi/MMLPlayer.def
		${CORE_SRCS}
		${YMFM_SRCS}
		${FMGEN_SRCS}
		${OPNGEN_SRCS}
	)
	target_include_directories(MMLPlayerKpi PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		# kmp_pi.h (KbMedia SDK ヘッダ)。プラグインターゲット限定で参照する。
		${CMAKE_CURRENT_SOURCE_DIR}/kpi/kpisdk
	)
	# 出力名を <name>.kpi に (KbMedia プラグインの拡張子。PREFIX 無しで lib を付けない)。
	set_target_properties(MMLPlayerKpi PROPERTIES
		OUTPUT_NAME "MMLPlayer"
		PREFIX ""
		SUFFIX ".kpi"
	)

	# KbMedia 本体なしで Open/Render/Close を回す自動 end-to-end テスト。
	add_executable(KpiSelfTest kpi/KpiSelfTest.cpp)
	target_include_directories(KpiSelfTest PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/kpi/kpisdk
	)
endif()
