# ZERO-FB 作業メモ（AIエージェント向け）

- `docs/SPEC.md` が唯一の正本。数式・定数・試験基準はここに合わせる。食い違いを見つけたら実装で勝手に吸収せず、設計書を直す提案を先に出す。
- ソフトのバージョンは `v0.1.0` から始める（`CMakeLists.txt` の `project(... VERSION ...)`）。設計書の番号（SPEC v1.5）とは別物。メジャー番号は自動で上げない。
- 進め方は SPEC 第9章の Step 1 → 4 の順。各 Step の自動テストが全部通ってから次へ進む。現在は Step 1 完了、次は Step 2。
- C++ を変えたら `training/erb_dsp.py`（NumPy 参照）も同じ式に直す。両者はコードを共有しない。
- `processBlock` 相当の経路ではヒープ確保・ロック禁止。`-ffast-math` と積和融合（FMA）は使わない。
- テストを通すためのごまかし（握りつぶし・基準の付け替え）は禁止。基準を変える必要があるときは、実測値と理由を SPEC に書く。
- テストデータは `training/gen_golden_fixtures.py` がビルド時に作る（約140MB、リポジトリには入れない）。
- 文書・画面・報告では「バージョン」と書く。
- コミットの作者メールは `info@seventh-well.com`。

## 手元での確認

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```
