# MMLPlayerでのfmgenの変更

取り込み元はNeko Project 21/Wのソース配布物 `np21w-src-rev101` の `sound/fmgen/` です。
以下は2026-09-27に取り込み元との比較で確認した変更内容です（変更の実施日を示すものではありません）。
fmgenの条件は [fmgen_readme.txt](fmgen_readme.txt) を参照してください。

## MMLPlayerで変更したファイル

- `fmgen_types.h`: np21wの `compiler.h` のインクルードを削除し、
  同ヘッダーの型定義だけで使用できるようにしました。
- `fmgen_headers.h`: `MAX_PATH` と `DWORD` の定義を追加しました。
  不要な `STRICT`、`WIN32_LEAN_AND_MEAN`、`max` / `min` の再定義と、
  コメントアウトされていた `windows.h` のインクルード行を削除しました。
- `fmgen_opna.h`: 未使用の `SetLPFCutoff` 引数名をコメントにしました。
  `RebuildTimeTable()` の `prescale` への代入を `-1` から `0xFF` に変更しました。

その他の同梱ソース・ヘッダーと `fmgen_readme.txt` は、np21w rev101の対応するファイルと
バイト単位で一致します。ファイル配置を `audio/synth/fmgen/` に変更し、接続用コードは
隣接する `audio/synth/fmgen_chip/` に置いています。

## 取り込み元での変更

上流の改変記録を無改変で添付しています。

- [fmgen_readme_kai.txt](fmgen_readme_kai.txt): NP2kai / AZO氏の変更記録。
- [fmgen_readme_np21w.txt](fmgen_readme_np21w.txt): np21wの変更記録。

これらの記録には、MMLPlayerには取り込んでいないファイルについての説明も含まれます。
