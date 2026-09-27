# 第三者ソフトウェアのライセンスと出典

MMLPlayer独自のコード・文書には、[LICENSE](LICENSE)に記載したMITライセンスを適用します。
第三者に由来するコードは、改変した部分を含め、それぞれのライセンス条件に従って配布します。
この文書は適用範囲と配布時の注意を説明するものであり、各ライセンス原文を置き換えるものではありません。

以下の3つの音源実装は、[Neko Project 21/W](https://simk98.github.io/np21w/)の
ソース配布物 `np21w-src-rev101` から取り込んでいます。
2026-09-27に、その配布物と本プロジェクトのソースを照合しました。

## ymfm

- 原作者・著作権表示: Copyright (c) 2021, Aaron Giles. All rights reserved.
- ライセンス: BSD 3-Clause License。
- 原プロジェクト: <https://github.com/aaronsgiles/ymfm>
- 取り込み元: np21w rev101 の `sound/mamebsd/`。
- 適用対象: `audio/synth/ymfm/` 内の `ymfm.h`、`ymfm_fm.h`、`ymfm_fm.ipp`、
  `ymfm_opn.h`、`ymfm_opn.cpp`、`ymfm_ssg.h`、`ymfm_ssg.cpp`、
  `ymfm_adpcm.h`、`ymfm_adpcm.cpp`。
- ライセンス原文: [audio/synth/ymfm/LICENSE.txt](audio/synth/ymfm/LICENSE.txt)。

これら9ファイルはnp21w rev101の対応するファイルとバイト単位で一致します。
np21w側で加えられた変更と、ソース内の `modified by SimK for np2` 等の表示を保持しています。
`audio/synth/ymfm_chip/` はMMLPlayer側の接続用コードです。

ソース配布では著作権表示・条件・免責条項を保持してください。
バイナリ配布でもこれらの全文を配布物の文書等に含めてください。
著作権者・貢献者の名前を、許可なく派生製品の推薦や宣伝に使用することはできません。

## fmgen

- 原作者・著作権表示: Copyright (C) by cisc 1998, 2003.
  各ソースファイルにある著作権表示も保持しています。
- ライセンス: fmgen独自ライセンス（MITライセンスではありません）。
- 取り込み元: np21w rev101 の `sound/fmgen/`（fmgen 008を基にした改変版）。
- 適用対象: `audio/synth/fmgen/` 内のソース・ヘッダー。
- ライセンス原文: [fmgen_readme.txt](audio/synth/fmgen/fmgen_readme.txt)。

原文には、由来・作者・著作権の明示、フリーソフトとしての配布、改変内容の明示、
ソース配布時の `fmgen_readme.txt` の無改変添付が定められています。
商用ソフト（シェアウェアを含む）への一部または全部の組み込みには、作者の事前合意が必要です。
公開時の作者への連絡は、原文ではお願いとして記載されています。

上流での改変記録を、取り込み元から変更せずに同梱しています。

- [fmgen_readme_kai.txt](audio/synth/fmgen/fmgen_readme_kai.txt): AZO氏による
  ファイル名変更、テーブル生成の修正、ステート保存・復元等の記録。
  記録内のC言語用ラッパー `fmgen_fmgwrap.cpp/.h` はMMLPlayerには含めていません。
- [fmgen_readme_np21w.txt](audio/synth/fmgen/fmgen_readme_np21w.txt):
  `SUPPORT_FMGEN` による条件付きコンパイルの記録。
- [CHANGES.md](audio/synth/fmgen/CHANGES.md): MMLPlayerで行った変更。

`audio/synth/fmgen_chip/` はMMLPlayer側の接続用コードです。
現在のCMake構成では、実行ファイルとKPIプラグインの両方にfmgenが組み込まれます。
実行時に `--opna=ymfm` 等を選択しても、fmgenの配布条件はなくなりません。

## opngen / psggen

- 著作権表示: Copyright (c) 1999-2025, NP2 developer team. All rights reserved.
- ライセンス: 修正BSDライセンス。取り込み元の原文をそのまま適用します。
- 取り込み元: np21w rev101 の `sound/`。
- 適用対象: `audio/synth/opngen/` 内の `opngen.h`、`opngenc.c`、`opngencfg.h`、
  `opngeng.c`、`psggen.h`、`psggenc.c`、`psggeng.c` と、同ディレクトリの
  互換ヘッダーに含まれるnp21w由来の定義。
- ライセンス原文: [audio/synth/opngen/LICENSE.txt](audio/synth/opngen/LICENSE.txt)。
  np21w rev101 の `LICENSES/LICENSE.TXT` をバイト単位で変更せずに複製しています。

上記7つの音源ファイルは、取り込み元の対応するファイルとバイト単位で一致します。
MMLPlayerでは同ディレクトリに `compiler.h`、`parts.h`、`pccore.h`、`sound.h` を追加し、
必要な型・マクロ・コールバック定義だけを提供する互換ヘッダーとして使用しています。
`audio/synth/opngen_chip/` はMMLPlayer側の接続用コードです。

ソース配布では著作権表示・条件・免責条項を保持し、バイナリ配布でも全文を添付してください。
原文の `<organization>` と `<COPYRIGHT HOLDER>` は配布元にある表記をそのまま保持しています。
原文冒頭の適用範囲の説明はnp21wについてのものであり、MMLPlayer全体に適用するものではありません。
原文は取り込み元と同じ文字コード（CP932 / Shift-JIS）です。

## 配布物に含める文書

現在の全音源を含む構成では、ソース・実行ファイル・KPIプラグインの配布物に次の文書を含めてください。
バイナリのみを配布する場合も、リンクだけで済ませず文書自体を添付してください。

- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
- `audio/synth/ymfm/LICENSE.txt`
- `audio/synth/fmgen/fmgen_readme.txt`
- `audio/synth/fmgen/fmgen_readme_kai.txt`
- `audio/synth/fmgen/fmgen_readme_np21w.txt`
- `audio/synth/fmgen/CHANGES.md`
- `audio/synth/opngen/LICENSE.txt`

バイナリ配布時には文書を別のフォルダーにまとめても構いませんが、出典と対応関係が分かるようにしてください。
fmgenの原文は内容・文字コードを変更せずに添付してください。

KbMedia Player SDKや楽曲・音色・効果音データを別途同梱する場合、それらには各配布元の条件が適用されます。
このプロジェクトのMITライセンスは、それらの再配布権を与えるものではありません。
