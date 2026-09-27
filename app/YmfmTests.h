#pragma once
//
// YmfmTests: ymfm 単体の動作確認用デバッグツール群。
// `--debug ymfm-direct` / `--debug ymfm-test` / `--debug ymfm-ssg-test` から呼ばれる。
//
// 共通仕様:
//   - 引数なしで起動 (= argv は使わない)
//   - 固定ファイル名 (ymfm_direct.wav / ymfm_test.wav / ymfm_ssg_test.wav) に出力
//   - 戻り値 = main の終了コード (成功 0)
//

namespace app {

int run_ymfm_direct();    // ymfm 直接駆動 (リサンプラ非経由) で FM ch1 A4 を 1 秒鳴らす
int run_ymfm_test();      // YmfmOpnaChip ラッパで FM ch1 A4 を 0.5s 鳴らす × 3 セット
int run_ymfm_ssg_test();  // YmfmOpnaChip ラッパで SSG ch A 440Hz を vol on/off × 3 セット

}  // namespace app
