# ZERO-FB 作業メモ（AIエージェント向け）

- `docs/SPEC.md` が唯一の正本。数式・定数・試験基準はここに合わせる。食い違いを見つけたら実装で勝手に吸収せず、設計書を直す提案を先に出す。
- ソフトのバージョンは `v0.1.0` から始める（`CMakeLists.txt` の `project(... VERSION ...)`）。設計書の番号（SPEC v1.7）とは別物。メジャー番号は自動で上げない。
- 進め方は SPEC 第9章の Step 1 → 4 の順。各 Step の自動テストが全部通ってから次へ進む。現在は Step 2 まで実装済み（CPU 負荷の M4 実測だけ未確認）。Step 3（学習）は別の担当が進行中。Step 4 は画面と VST3 / AU のビルドまで実装済みで、実機・DAW での確認と署名・配布が残り。
- C++ を変えたら `training/erb_dsp.py` / `training/nn_reference.py`（NumPy 参照）も同じ式に直す。両者はコードを共有しない。
- `processBlock` 相当の経路ではヒープ確保・ロック禁止。`-ffast-math` は使わない。積和融合（FMA）は DSP 経路では使わず、NN の内積（`nn/SimdKernels.h`）だけ明示的に使う。
- テストを通すためのごまかし（握りつぶし・基準の付け替え）は禁止。基準を変える必要があるときは、実測値と理由を SPEC に書く。
- テストデータは `training/gen_golden_fixtures.py` がビルド時に作る（約200MB、リポジトリには入れない）。`EmbeddedWeights.h` も今はビルド時に試験用重みから生成する（学習済みモデルではない）。
- 文書・画面・報告では「バージョン」と書く。
- コミットの作者メールは `info@seventh-well.com`。

- ライセンスは AGPLv3（JUCE 9.0.6 を AGPLv3 で利用）。学習済みの重み（`plugin/Source/nn/EmbeddedWeights.h`・`*.swmodel`）は絶対にコミットしない。
- 画面は `docs/ui_master.svg`（760 × 440、SW AUDIO の LIVE ライン）が正。画面を変えたら `UiSnapshot` で PNG を書き出してマスターと見比べ、`docs/ui_render.png` を更新する。SW AUDIO 標準の Morph・Lock などは設計書に無いので足さない。
- プラグインのビルドは `-DZEROFB_BUILD_PLUGIN=ON`。Linux で画面を出す道具とテストは `xvfb-run -a` を付けて動かす。

## 手元での確認

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```
