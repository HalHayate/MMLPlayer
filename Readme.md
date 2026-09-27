# MMLPlayer

MUSIC.COM形式のMMLを再生・解析するWindows向けのコマンドラインツールです。
FM音源とSSG音源によるリアルタイム再生、WAV出力、S98形式での録音に対応しています。
KbMedia Player用のデコーダープラグイン（`.kpi`）のソースも含みます。

## 主な機能

- FM 3チャンネル（`1:`～`3:`）とSSG 3チャンネル（`4:`～`6:`）のMML再生。
- FM音色定義、SSGエンベロープ、文字列マクロ、繰り返しの読み込み。
- `SOUND.DAT` を使うDパートと効果音の再生。
- 48 kHz・16 bit・ステレオのWAV出力。
- S98 v1録音と、OPNAのレジスター記録を含むS98のWAV変換。
- fmgen、ymfm、opngen、自作音源実装の切り替え。
- チャンネル単位のイベント表示やWAV出力。

MUSIC.COMとの互換性を目指した実装ですが、すべての楽曲・命令の完全な再現を保証するものではありません。
読み込み時の警告も確認してください。

## ビルド

Windows環境で、以下を用意してください。

- Visual Studioの「C++によるデスクトップ開発」（MSVC、Windows SDK）。
- CMake 3.20以降とNinja（下記のプリセットを使う場合はCMake 3.25以降）。
  Visual StudioのCMakeツールも使用できます。
- C++17に対応したコンパイラー。

### コマンドライン版

Visual Studioの **x64 Native Tools Command Prompt** を開き、プロジェクトのルートで実行します。

```bat
cmake -S . -B out/build/cli -G Ninja -DCMAKE_BUILD_TYPE=Release -DMMLPLAYER_BUILD_KPI=OFF
cmake --build out/build/cli
```

生成される実行ファイルは `out/build/cli/MMLPlayer.exe` です。
この手順では、別途SDKが必要なKPIプラグインのビルドを無効にしています。

### KbMedia Playerプラグイン

KbMedia Player SDKの `kmp_pi.h` は `kpi/kpisdk/` を参照します。
`MMLPLAYER_BUILD_KPI=ON` でビルドします。

```bat
cmake -S . -B out/build/kpi -G Ninja -DCMAKE_BUILD_TYPE=Release -DMMLPLAYER_BUILD_KPI=ON
cmake --build out/build/kpi
```

`out/build/kpi/MMLPlayer.kpi` と、プラグイン検証用の `KpiSelfTest.exe` が生成されます。
使用するKbMedia Player本体とビット数を合わせてください。
32 bit版を作る場合はx86用の開発者コマンドプロンプトと別のビルドフォルダーを使用します。
プラグインの音源実装はfmgenです。SDKを再配布する場合はSDK側の配布条件も確認してください。

### Visual Studioで32 bit版をビルドする

プロジェクトのフォルダーを開き、構成から `x86-Release` または `x86-Debug` を選択してビルドします。
各プリセットはNinjaを使用し、Visual Studioが対象のビット数に合うMSVC環境を準備します。
Visual Studio 2022専用のビルドツール（v143）には固定していません。

以前の設定で構成済みの場合、最初の切り替え時にCMakeの「キャッシュの削除と再構成」を実行してください。
古いVisual StudioジェネレーターのキャッシュをNinjaへそのまま引き継ぐことはできません。

コマンドラインでは、**x86 Native Tools Command Prompt** を開いて実行します。

```bat
cmake --preset x86-Release --fresh
cmake --build --preset x86-Release
```

`out/build/x86-Release/` に `MMLPlayer.exe`、`MMLPlayer.kpi`、`KpiSelfTest.exe` が生成されます。
Debug版は両方のコマンドで `x86-Debug` を指定してください。
`--fresh` は古いCMakeキャッシュを作り直すための指定で、通常の再ビルドでは省略できます。
プラグインが不要なら、構成時に `-DMMLPLAYER_BUILD_KPI=OFF` を追加してください。

コマンドラインのCMake自体はプリセットの `architecture` に従ってMSVC環境を切り替えません。
32 bit版はx86用、64 bit版（`x64-Release` / `x64-Debug`）はx64用の開発者コマンドプロンプトを使用してください。

## 使い方

以下はプロジェクトのルートからコマンドライン版を実行する例です。
`song.mml` は再生したいファイルのパスに置き換えてください。
楽曲ファイルと `SOUND.DAT` は同梱していません。

```bat
out\build\cli\MMLPlayer.exe song.mml
```

再生中に `Esc` キーを押すと停止します。
引数を省略すると `Materials/MMLSamples/AGM/AGM01.MML` を探すため、通常はファイルを明示してください。

### WAV出力

```bat
out\build\cli\MMLPlayer.exe --render song.mml --out song.wav
```

### 音源実装の切り替え

```bat
out\build\cli\MMLPlayer.exe --opna=ymfm song.mml
```

| 指定値 | 音源実装 |
| --- | --- |
| `fmgen` | cisc氏のfmgen（実装上の既定値） |
| `ymfm` | Aaron Giles氏のymfm |
| `opngen` | np21wのopngen / psggen |
| `original` | MMLPlayerの自作実装 |

### S98録音・WAV変換

```bat
out\build\cli\MMLPlayer.exe --opna=ymfm --record-s98 song.s98 song.mml
out\build\cli\MMLPlayer.exe --play-s98 song.s98 --out song_s98.wav
```

`--record-s98` には `--opna=ymfm` が必要です。録音のみを行い、WAVも同時に出力する場合は `--render` を併用してください。
`--play-s98` はymfmでWAVを生成します。任意の音源構成やすべてのS98仕様への対応を意図したものではありません。

### 効果音・Dパート

```bat
out\build\cli\MMLPlayer.exe --sound-dat path\to\SOUND.DAT song.mml
out\build\cli\MMLPlayer.exe --play-effect 0 --sound-dat path\to\SOUND.DAT
```

効果音番号は `0`～`63` です。
`--play-effect` は効果音をWAVへ出力します（既定名は `mine_effect_番号.wav`）。
Dパートを使う楽曲では、パス指定がなければMMLと同じフォルダー、次に `Materials/SOUND.DAT` を探します。
KPIプラグインではMMLと同じフォルダーを探します。

### その他のオプション

| オプション | 内容 |
| --- | --- |
| `--out PATH` | WAV出力先を指定 |
| `--max-seconds N` | MMLからのWAV・S98出力時間を制限 |
| `--loop-count N` | `{0 ... }` のループ回数を指定。`0` は繰り返しなし |
| `--debug dump-events N song.mml` | チャンネルN（1～6）のイベントを表示 |
| `--debug render-ch N song.mml` | チャンネルNだけをWAV出力 |
| `--help` | コマンド一覧を表示 |

## ライセンス

MMLPlayer独自のコード・文書は [MIT License](LICENSE) で提供します。
同梱する第三者のコードには、各ライセンスが適用されます。

| コンポーネント | ライセンス |
| --- | --- |
| ymfm | [BSD 3-Clause](audio/synth/ymfm/LICENSE.txt) |
| fmgen | [独自ライセンス](audio/synth/fmgen/fmgen_readme.txt)。フリーソフトとして配布し、商用組み込みには作者の事前合意が必要 |
| opngen / psggen | [修正BSD](audio/synth/opngen/LICENSE.txt) |

**現在の実行ファイル・KPIプラグインにはfmgenが含まれます。**
音源を実行時に切り替えても、fmgenの配布条件はなくなりません。
出典、改変記録、配布物に添付する文書は [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) を参照してください。
