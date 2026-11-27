# ZERO-FB: Zero-Latency AI Feedback & Dereverb Suppressor 単一正本設計書（SPEC.md v1.7）

> **実装エージェント（Claude / Claude Code）および仕様監査役への宣言:**
> 本書は、過去の分冊（v1.2 / 補遺04 / 補完リスト05 / v1.3）および10世代プローブ・監査指摘事項をすべて統合し、すべての数式・状態遷移・バイナリ配置・受入試験プロトコルを1択へ確定させた**唯一の単一正本（Single Source of Truth）**である。
> 本書に記載する性能値（GBF、CPU、STOI等）はすべて**受入合格基準（目標値）**であり、実測済みの達成値ではない。
> 実装は「第9章：実装マイルストーン（Step 1 〜 Step 4）」の順に実施し、各ステップの自動テストがすべて合格してから次へ進むこと。

### バージョン番号の読み方
* **設計書のバージョン**: 本書は `SPEC v1.7`。これは文書の番号であり、ソフトの番号ではない。
* **ソフトのバージョン**: 最初のリリースは **`v0.1.0`**（本書の実装対象）。狭帯域ノッチ（Stage 6B）を足すリリースは **`v0.2.0`**。
* **保存データ・重みファイルの番号**: `state_version="1"` と Magic `"SWZFB004"` はそれぞれ独立した形式番号で、ソフトのバージョンとは連動しない。

### v1.7 への変更点（Step 4 実装時に確定）
1. **第6.3節 画面**: デザインを SW AUDIO の LIVE ライン準拠（`760 × 440 px`）に差し替えた。旧マスター（`720 × 380 px`）は廃止。設計書に埋め込んでいた SVG 全文は削除し、`docs/ui_master.svg` を唯一のマスターとする。
2. **第6.2節 フォント**: `Michroma`（ロゴ）／`Barlow Condensed`（ラベル）／`Space Mono`（数値）に変更（すべて SIL OFL 1.1）。プラグインの画面に日本語は出さないため `Noto Sans JP` は埋め込まない。
3. **ライセンス・配布**: 本ソフトは **AGPLv3**。Mac の署名・公証は `kajisho5/runway` の共通の仕組みを使うため、第8章にあった `scripts/package_macos.sh` は作らない。JUCE は `9.0.6`（AGPLv3 で利用。<https://github.com/juce-framework/JUCE/releases/tag/9.0.6>）。
4. **第6.2節 プリセット**: 画面にプリセットのボタン4個を置く。第6.1節の初期値（`DEREVERB 50%`）はどのプリセットとも一致しないため、起動直後はどのボタンも点灯しない（仕様どおり。初期値は変えていない）。
5. **モデル未搭載時の表示**: 学習済みの重みを差し込まずにビルドしたプラグインは全マスク `1.0`（抑制なし）で動き、画面下の帯に `NO MODEL LOADED` と出す。
6. **第9章 Step 4**: ホストなしで `AudioProcessor` を検査する `PluginSoakTest` を追加（バス構成・Bit-Perfect バイパス・状態保存・プリセット・1時間耐久）。xrun は実機のオーディオ装置でしか判定できないため、テストでは締め切り超過の回数を参考表示にとどめ、合否はヒープ確保 0 と非有限値 0 で付ける。

### v1.6 への追記（Step 3 着手時に確定）
1. **第3.2節 `EmbeddedWeights.h`**: 学習済みの重みは公開リポジトリには入れず、手元でビルド時に差し込む（個人利用専用。非商用データで学習した重みの公開は配布に当たるため）。
2. **第4.2節 ライセンス監査**: 非商用データを含む重みは個人利用専用・公開／配布しないことを明記。

### v1.5 からの変更点（Step 2 実装時の検証で確定）
1. **第3.1節・第5章 FMA の扱い**: NN の内積（GEMV・Conv1d・LayerNorm の分散）に限り、明示的な積和融合命令を使ってよい。DSP 経路（第2章）は従来どおり使わない。
2. **第3.2節 `EmbeddedWeights.h`**: 学習済みの重みができる Step 3 までは、固定の試験用重み（seed 42 の疑似乱数。学習済みモデルではない）からビルド時に生成する。
3. **第5.2節 信号パスの SIMD 化**: 見送り。DSP 全体（推論なし）で 1 コールバックの中央値がブロック予算の `1.4〜3.5%` と小さかったため（第1.2節の表）。
4. **第7.1節 閉ループベンチ**: 仮想会場の合成式、発散限界の理論値との照合、ハンチング・モードホッピングの数え方を確定。Step 2 の刺激音声は合成の疑似発話。
5. **第7.2節 NN の誤差基準**: 実測にもとづき数値を確定。
6. **第2.5節 未解決の課題を追記**: 純音の Ring Index は周波数によって `2.7〜10.8 dB` しか出ず、`+6 dB` を超えるのは全周波数の約半分。バックオフ延長条件（`T_raw > 6 dB`）は見直しが必要（Step 3 で学習済みモデルと合わせて決める）。

### v1.4 からの変更点（Step 1 実装時の検証で確定）
1. **第2.5節 FSM**: Hold 開始時に `since_hold_end` を 0 にしていたため、1回のハウリングの中で約 1 ms のうちにバックオフがレベル2（12秒）まで上がる不具合を修正。バックオフ段階は「Hold に入る瞬間」に1回だけ判定する。
2. **第2.2節・第2.6節 両端のシェルフ**: シェルフは折れ点で設定値の半分（dB）しか効かないため、対角を `1.0` と決め打ちした v1.4 の式では均一 `-12 dB` 試験が 60 Hz / 15 kHz で `+5 dB` 以上外れた（数値検証済み）。影響率行列は対角も実測値とし、シェルフの折れ点を帯域の内側境界へ移し、帯行列の幅を `±2` から `±6` へ広げた。
3. **第7.2節 誤差基準**: float32 どうしの参照でも float64 との差は最大 `1.7e-4` あった（強いトーンの帯域外漏れが単精度の丸めに埋もれるため）。「FP64 に対して `< 1e-5`」は float 実装では原理的に満たせないので、3段の基準に置き換えた。
4. **第2.1節 クロスフェード**: `1/N_ramp` の加算を繰り返すと丸めで終端が `N_ramp + 1` サンプル目にずれるため、整数カウンタで管理する。
5. **第6.3節 SVG**: `xmlns` がリンク記法になっていた誤記を修正。
6. **その他の明文化**: Stage 1 の式（第2.9節）、2段リングバッファの窓の定義、制御経路の入力ガード、Golden Fixture をリポジトリに入れずビルド時に生成すること。

---

## 1. システム概要・スコープ・環境マトリクス

### 1.1 製品仕様・受入目標
* **プラグイン名**: `ZERO-FB`
* **プラグイン種別**: ゼロレイテンシーAIハウリング抑制・デリバーブ・デノイズプラグイン
* **対応サンプリングレート**: `48,000 Hz`（学習・ネイティブ基準）、`44,100 Hz`、`96,000 Hz`（未対応レートが渡された場合は安全バイパスで動作）
* **対応バス構成**: `Mono -> Mono`、`Stereo -> Stereo`、`Mono -> Stereo`（`isBusesLayoutSupported` で明示許可）
* **プラグイン報告遅延**: `0 samples`（`getLatencySamples() == 0`）
* **遅延・位相・制御応答の分離定義**:
  1. **信号パス整数遅延（Audio Block Latency）**: `0 samples`（厳密因果。フレーム化・先読み・オーバーラップによる遅延なし）
  2. **最小位相応答（Phase / Group Delay）**: 抑制動作中は最小位相IIRフィルタの物理的な位相回転・群遅延を伴う（プリリンギングは厳密に `0`）。これは整数遅延とは別指標として扱う。
  3. **制御応答時間（Control Latency）**: 制御ホップ $H$（約 `0.333〜0.340 ms`）＋ 帯域包絡線時定数 $\tau_{\text{env}}(b)$ ＋ アタック時定数（`1.5 ms`）で定義される。
* **リリーススコープの明確な切り分け**:
  * **ソフト v0.1.0（本書の実装対象）**: **Stage 6A（32バンド Cytomic ZDF SVF 直列EQ＋Projected Gauss-Seidel補正＋帯域別FSMヒステリシス制御）単体**で全機能を完成・検証する。
  * **ソフト v0.2.0（将来拡張・予約）**: 狭帯域適応ノッチ（Stage 6B）は v0.1.0 のコンパイル対象・マイルストーンから除外する（第2.8節に予約仕様のみ記載）。

### 1.2 対象環境・ビルドマトリクス
| OS | アーキテクチャ | 配布フォーマット | コンパイラ / SIMD | 必須検証ツール |
| :--- | :--- | :--- | :--- | :--- |
| **macOS 14+** | Universal (`arm64` + `x86_64`) | `VST3`, `AU` | Apple Clang 15+ (`-O3`, NEON / AVX2) | `ZeroLatencyTest`, `GoldenParityTest`, `ClosedLoopBench`, `pluginval` (L8), `auval` |
| **Windows 11** | `x86_64` | `VST3` | MSVC 2022 (`v143`, `/O2 /arch:AVX2`) | `ZeroLatencyTest`, `GoldenParityTest`, `ClosedLoopBench`, `pluginval` (L8) |

* **CPU / xrun 基準マシン**: Apple M4 系（macOS `arm64`, Release ビルド, `48 kHz / 16 samples / Stereo`）で単一オーディオコールバック時間の対ブロック予算比 **中央値 `<= 10%`・P99 `<= 15%`**（warm-up 10秒後、60秒間計測）。
* **Step 2 時点の CPU 実測（参考値。基準マシンではない）**: `EngineBench`（48 kHz / 16 samples / Stereo、warm-up 10秒後に計測、固定の試験用重み）の結果は下表のとおり（値はブロック予算に対する割合）。いずれも共有の仮想マシンで、他の処理の割り込みが入るため P99 は環境に強く左右される。**Apple M4 系の実機での計測は未実施**で、完了条件の合否は出していない。

| 計測環境 | DSP のみ 中央値 / P99 | DSP＋NN 推論 中央値 / P99 |
| :--- | :--- | :--- |
| 開発用 Linux 仮想マシン（Intel Xeon 2.1 GHz, AVX2） | `1.75%` / `5.0%` | `4.1%` / `8.3%` |
| GitHub の Linux 実行機（x86_64, AVX2） | `1.43%` / `1.8%` | `3.3%` / `5.5%` |
| GitHub の Windows 実行機（x86_64, AVX2） | `2.19%` / `2.9%` | `4.6%` / `6.8%` |
| GitHub の macOS 実行機（arm64, NEON。機種は未確認） | `3.50%` / `16.3%` | `7.6%` / `38.1%` |

---

## 2. リアルタイムDSPパイプライン詳細仕様

```text
Input x_L[n], x_R[n]
  │
  ├──[Steady Bypass Check: wet_mix == 0.0f]────────────────────────────────────────────► Output = Input (Bit-Perfect)
  │
  ├──► [Control Path: L/R 独立ERB解析 → パワー合成 P_b[n] = 0.5*(y_bL[n]² + y_bR[n]²)]
  │      │  ※逆相相殺を防ぐため 0.5*(L+R) モノ和は禁止
  │      ▼
  │    [Stage 2-SC] 32-Band ERB ZDF SVF Bandpass Bank (SoA SIMD, 帯域別 Q_sc[b])
  │      ▼ (n_k = (k+1)*H - 1 ごとに採取)
  │    [Stage 3] 6ch 近似レベル不変特徴量抽出 (192次元, 2段リングバッファ)
  │      ▼ (H samples = 約0.333ms ごとに推論)
  │    [Stage 4] Causal Conv1D + 2-Layer GRU 推論エンジン (64ch 2-Head出力)
  │      ▼
  │    [Stage 5A] 2ヘッド統合 + 単一FSMヒステリシス制御 (Hold/Backoff/Voice-Trigger)
  │      ▼
  │    [Stage 5B] Projected Gauss-Seidel (3反復) → 目標EQゲイン g_eq[0..31] (dB)
  │      │                                          (H samples 間で係数線形補間)
  ▼ (Signal Path: 0 samples)                        │
[Stage 1] DC Blocker (1次 HPF, 20 Hz)               │
  │                                                 ▼
[Stage 6A] 32-Band Serial Cytomic ZDF SVF EQ ◄──────┘ (Band 0: LowShelf, 1..30: Peak, 31: HighShelf / 常時稼働)
  │
[Stage 7A] NaN / Inf / Over-Range (+24 dBFS) Fail-Safe Guard (検知時: 安全ミュート＋状態リセット＋Floorから5ms復帰)
  │
[Stage 7B] Safety Soft Limiter (-0.5 dBFS ceiling, 0 samples)
  │
  ▼
[5 ms Crossfade Mixer: y = (1 - wet_mix)*x_in + wet_mix*y_wet]
  │
  ▼
Output y_L[n], y_R[n]
```

### 2.1 Bit-Perfect Bypass・5 ms クロスフェード・ホスト互換
* **5 ms ランプ長（整数丸め規則）**:
  $$N_{\text{ramp}} = \max\!\left(1,\; \text{lround}(0.005 \cdot f_s)\right)$$
  （48 kHz: `240`, 44.1 kHz: `221`, 96 kHz: `480` samples）
* **バイパス条件とクリックレス遷移**:
  * バイパス要求フラグ: `want_bypass = (bypass_param >= 0.5f) || (suppress_param <= 0.0f)`
  * `wet_mix` は 1 サンプルごとに `want_bypass ? -1.0f/N_ramp : +1.0f/N_ramp` だけ増減し、`[0.0f, 1.0f]` へクランプする（始端 `0.0` / 終端 `1.0` を厳密到達）。
    * 実装は整数位置 `ramp_pos ∈ [0, N_ramp]` を毎サンプル ±1 し、`wet_mix = (ramp_pos == N_ramp) ? 1.0f : ramp_pos / N_ramp` とする（float の加算を繰り返すと終端が1サンプルずれるため）。ちょうど `N_ramp` サンプルで遷移が終わる。
    * `prepareToPlay` 後の最初のサンプルは、要求された状態（バイパス ON なら `0.0`、OFF なら `1.0`）から始める（ランプなし）。
    * 定常バイパスへ入った時点で信号経路の状態（Stage 1・Stage 6A）をゼロクリアする。
    * `wet_mix == 1.0f` の定常状態では `y = y_wet` をそのまま出力する（ミキサーの乗算を通さない）。
  * **Bit-Perfect 条件**: `want_bypass == true` かつ `wet_mix == 0.0f` の定常状態においては、Stage 1（HPF）・Stage 6A（EQ）・Stage 7B（Limiter）を完全に迂回し、`y_out[n] = x_in[n]` をそのままコピーする（サンプル単位最大絶対誤差 `0.0`）。
  * バイパス中も Control Path（Stage 2-SC〜Stage 5）は停止せずバックグラウンドで追従させ、バイパス解除時の急変を防ぐ。
* **ホスト標準バイパス＆オフライン処理**:
  * `AudioProcessor::getBypassParameter()` をオーバーライドし、APVTS の `bypass` パラメータを返却する。
  * `isNonRealtime() == true` のオフラインバウンス時は実時間 CPU 計測を無効化し、決定論的出力を保証する。

### 2.2 32バンド ERB 周波数配置・帯域別 $Q_b$・ホップ境界処理
* **中心周波数 $f_b$（$b = 0, \dots, 31$、範囲 `60.0 Hz 〜 15,000.0 Hz`）**:
  * 44.1 kHz 時の双一次ワーピング破綻を防ぐため、上限中心周波数を `15,000.0 Hz` に設定する。
  $$\text{ERBS}(f) = 21.4 \log_{10}(0.00437\,f + 1)$$
  $$\text{ERBS}_b = \text{ERBS}(60.0) + \frac{b}{31}\big(\text{ERBS}(15000.0) - \text{ERBS}(60.0)\big),\qquad f_b = \frac{10^{\text{ERBS}_b / 21.4} - 1}{0.00437}$$
  * 代表値: $f_0 = 60.00\text{ Hz}$, $f_1 = 99.41\text{ Hz}$, $f_{15} = 1738.52\text{ Hz}$, $f_{30} = 13171.56\text{ Hz}$, $f_{31} = 15000.00\text{ Hz}$。
* **帯域別クオリティファクタ $Q$**:
  * 幾何平均境界周波数 $f_{\text{lo},b}, f_{\text{hi},b}$ を以下で定義する：
    * $b = 1 \dots 30$: $f_{\text{lo},b} = \sqrt{f_{b-1} f_b},\; f_{\text{hi},b} = \sqrt{f_b f_{b+1}}$
    * $b = 0$: $f_{\text{hi},0} = \sqrt{f_0 f_1},\; f_{\text{lo},0} = f_0^2 / f_{\text{hi},0}$
    * $b = 31$: $f_{\text{lo},31} = \sqrt{f_{30} f_{31}},\; f_{\text{hi},31} = f_{31}^2 / f_{\text{lo},31}$
  * **サイドチェイン解析用 Bandpass（Stage 2-SC）**: 全32バンドで $Q_{\text{sc},b} = \frac{f_b}{f_{\text{hi},b} - f_{\text{lo},b}}$（範囲 `1.960 〜 7.687`）。
  * **信号パス用 EQ（Stage 6A）**:
    * $b = 0$: **Low Shelf**（$f_c = f_{\text{hi},0} = 77.23\text{ Hz}$、$Q_0 = 1/\sqrt{2} \approx 0.707107$）— 60 Hz 未満の超低域ハウリングをカバー。
    * $b = 1 \dots 30$: **Peaking EQ**（$f_c = f_b$、$Q_b = Q_{\text{sc},b}$、範囲 `2.339 〜 7.679`）。
    * $b = 31$: **High Shelf**（$f_c = f_{\text{lo},31} = 14056.08\text{ Hz}$、$Q_{31} = 1/\sqrt{2} \approx 0.707107$）— 15 kHz 超の高域ヒスをカバー。
    * **シェルフの折れ点を帯域の内側境界に置く理由**: Cytomic のシェルフは折れ点 $f_c$ でちょうど設定値の半分（dB）になる。折れ点を帯域中心（60 Hz / 15 kHz）に置くと、中心で目標の半分しか効かず、目標を出すにはシェルフを2倍深く掛ける必要があり、高域側では下の5〜6バンドまで巻き込む。折れ点を内側境界（ピーク型どうしが交差するのと同じ位置）へ置くと、中心での効きは設定値の `0.71`（Low）／`0.57〜0.63`（High、レートによる）になる。以下 $f_{\text{eq},b}$ は「$b=0$ は $f_{\text{hi},0}$、$b=31$ は $f_{\text{lo},31}$、それ以外は $f_b$」を指す。
* **サンプリングレート別 制御ホップ長 $H$ と重み互換規約**:
  * `48,000 Hz`: $H = 16$（`0.3333 ms`） / `44,100 Hz`: $H = 15$（`0.3401 ms`） / `96,000 Hz`: $H = 32$（`0.3333 ms`）
  * **複数SR重み互換**: `44.1 kHz` / `96 kHz` でも `48 kHz` 学習済み重みをそのままロードする。ホップ長 $H$ により時間分解能を約 `0.333〜0.340 ms` に揃え、フィルタ・EMA 係数はすべて実 $f_s$ で計算する。
* **任意ブロック長・ホップ位相・オーバーフロー耐性**:
  * 累積サンプルカウンタは `uint64_t`（ラップアラウンド防止）、ホップ内位相カウンタは `int hop_phase ∈ [0, H-1]` で管理する。
  * ホストの `processBlock` に任意のブロック長 $N_{\text{blk}}$（`prepareToPlay` 申告値を超える変則大ブロック含む）が渡されても同じ結果になること。DSPコア（`ZeroFbEngine`）は全段を1サンプルずつ処理するため、作業バッファもチャンク分割も持たない（ヒープ確保ゼロ、ブロック長に依存する状態なし）。
  * **サンプル更新とホップ境界の厳密順序（0-based サンプル $n = 0, 1, 2, \dots$）**:
    1. サンプル $n$ について、現在の補間係数を用いて Signal Path（Stage 1 → 6A → 7A → 7B → Mixer）および Control Path のサンプル更新（Stage 2-SC SVF ＋ 包絡線 EMA）を 1 サンプル実行する。
    2. `hop_phase` が `H - 1` に到達したサンプル（すなわち $n_k = (k + 1)H - 1$、初回は $k=0, n_0 = H - 1$）の**サンプル更新直後**に、特徴量採取（Stage 3）→ NN 推論（Stage 4）→ FSM & ゲイン補正（Stage 5A/5B）→ 次ホップの目標SVF係数計算を実行する。
    3. 次のホップ区間 $m = 0, 1, \dots, H - 1$（サンプル $n_k + 1 \dots n_k + H$）では、係数 `c ∈ {a1, a2, a3, m0, m1, m2}` を始点 `c_prev` から終点 `c_next` へ毎サンプル線形補間する（`g`, `k` は係数計算の途中値で、サンプル更新式には現れないため補間しない）：
       $$c[m] = c_{\text{prev}} + \frac{m + 1}{H}\big(c_{\text{next}} - c_{\text{prev}}\big)$$
       （初回ホップ $n = 0 \dots H - 1$ は `c_prev = c_next = unity (0 dB)` で固定）
* **EMA 平滑係数の統一定義（Python / C++ 完全一致）**:
  * サンプル周期更新: $\alpha_s(\tau, f_s) = 1 - \exp\!\left(-\frac{1}{\tau \cdot f_s}\right)$
  * ホップ周期更新: $\alpha_H(\tau, H, f_s) = 1 - \exp\!\left(-\frac{H}{\tau \cdot f_s}\right)$

### 2.3 Cytomic ZDF SVF フィルタ厳密数式（Direct Form I/II 禁止・動的スキップ廃止）
クリックノイズと分岐予測ミスを根絶するため、**動的バイパス（演算スキップ）は廃止し、全32バンドを常時稼働**させる。`g_base[b] = tan(pi * f_eq_b / fs)`（サイドチェインは `f_b`）は `prepareToPlay` で事前計算テーブル化し、毎ホップの `A` 計算は SIMD 高速指数関数 `A = fast_exp2(G_dB * 0.083048202f)`（$1/40 \cdot \log_2 10$）を用いる。

* **1サンプル共通更新コア（初期状態 `ic1eq = 0.0f, ic2eq = 0.0f`）**:
  $$v_3 = v_0 - \text{ic2eq},\qquad v_1 = a_1\,\text{ic1eq} + a_2\,v_3,\qquad v_2 = \text{ic2eq} + a_2\,\text{ic1eq} + a_3\,v_3$$
  $$\text{ic1eq}' = 2 v_1 - \text{ic1eq},\qquad \text{ic2eq}' = 2 v_2 - \text{ic2eq},\qquad y = m_0 v_0 + m_1 v_1 + m_2 v_2$$
* **演算順序と精度**: 係数は `prepareToPlay` 時に double で求めて float へ丸める（Stage 6A の毎ホップ更新のみ float で計算）。積和の融合（FMA）は使わず、`v2 = (ic2eq + a2*ic1eq) + a3*v3`、`y = (m0*v0 + m1*v1) + m2*v2` の順で評価する。AVX2・NEON・スカラーの3経路と NumPy 参照が同じ IEEE 演算列になる。
* **フィルタ種別ごとの厳密係数式（共通: $a_1 = \frac{1}{1 + g(g + k)},\; a_2 = g\,a_1,\; a_3 = g\,a_2$、$A = 10^{G_{\text{dB}} / 40}$）**:
  1. **Bandpass（Stage 2-SC、0 dBピーク）**: $g = g_{\text{base},b},\; k = 1/Q_{\text{sc},b},\; m_0 = 0,\; m_1 = k,\; m_2 = 0$
  2. **Low Shelf（Stage 6A, $b=0, Q_0 = 1/\sqrt{2}$、$g_{\text{base},0} = \tan(\pi f_{\text{hi},0}/f_s)$）**: $g = g_{\text{base},0} / \sqrt{A},\; k = 1/Q_0,\; m_0 = 1,\; m_1 = k(A - 1),\; m_2 = A^2 - 1$
  3. **Peaking EQ（Stage 6A, $b=1 \dots 30, Q_b = Q_{\text{sc},b}$）**: $g = g_{\text{base},b},\; k = 1/(Q_b \cdot A),\; m_0 = 1,\; m_1 = k(A^2 - 1),\; m_2 = 0$
  4. **High Shelf（Stage 6A, $b=31, Q_{31} = 1/\sqrt{2}$、$g_{\text{base},31} = \tan(\pi f_{\text{lo},31}/f_s)$）**: $g = g_{\text{base},31} \cdot \sqrt{A},\; k = 1/Q_{31},\; m_0 = A^2,\; m_1 = k(1 - A)A,\; m_2 = 1 - A^2$

### 2.4 Stage 3：6ch 近似レベル不変特徴量（Python / C++ 厳密一致仕様）
* **レベル不変性の正確な定義**: 微小ガード定数 $\epsilon$ および無音フロアが存在するため、本特徴量は「**動作レンジ `-45 dBFS 〜 -3 dBFS` の定常状態における近似レベル不変**（入力レベル差による減衰量変動 $\le \pm 2.0\text{ dB}$）」として定義する。
* **制御経路の入力ガード**: サイドチェインへ入るサンプルは `[-16.0, +16.0]`（`±24 dBFS`）へクランプする（有限な入力なら帯域パワーが必ず有限に収まる）。`NaN` / `Inf` が入った場合は Stage 2-SC の状態と本節の統計量をすべて初期化し、そのサンプルは無音として扱う（バイパス中も同じ）。
* **ステレオパワー合成（逆相耐性）**:
  * モノラル: $p_b[n] = y_b^2[n]$ ／ ステレオ: $p_b[n] = 0.5\big(y_{b,L}^2[n] + y_{b,R}^2[n]\big)$
* **帯域別包絡線時定数（低域リップル誤認防止）**:
  $$\tau_{\text{env}}(b) = \max\!\left(0.002,\; \frac{1.5}{f_b}\right)\text{ [sec]}$$
  （60 Hz で `25.0 ms`、750 Hz 以上は `2.0 ms`。バンド外漏れ込みゼロかつ 60 Hz 純トーンのリップルを `-16 dB` 以下へ抑圧）
* **サンプル更新およびフレーム $k$（$n_k = (k+1)H - 1$）での基礎統計量**:
  * 短期バンドパワー（サンプル更新、初期値 `0.0`）: $P_s[b, n] = (1 - \alpha_s(\tau_{\text{env}}(b), f_s)) P_s[b, n-1] + \alpha_s(\tau_{\text{env}}(b), f_s) p_b[n]$
  * フレーム採取対数パワー: $E_s[b, k] = 10\log_{10}(P_s[b, n_k] + 10^{-10})$
  * フレーム包絡線振幅: $a_b[k] = \sqrt{P_s[b, n_k]}$
  * 全帯域長期レベル（ホップ更新、$\tau = 300\text{ ms}$、初期値 $P_{\text{global}}[-1] = 0.0$）:
    $$P_{\text{global}}[k] = (1 - \alpha_H(0.300, H, f_s)) P_{\text{global}}[k-1] + \alpha_H(0.300, H, f_s) \left(\frac{1}{32}\sum_{b=0}^{31} P_s[b, n_k]\right)$$
    $$E_{\text{global}}[k] = \max\!\big(-90.0,\; 10\log_{10}(P_{\text{global}}[k] + 10^{-10})\big)$$
  * バンド別因果的ノイズフロア $E_{\text{floor}}[b, k]$（実機初期値: $k=0$ のとき $E_{\text{floor}}[b, 0] = E_s[b, 0]$。学習時は第4.1節のランダム初期化）:
    $$E_{\text{floor}}[b, k] = \begin{cases} (1 - \alpha_H(0.050, H, f_s)) E_{\text{floor}}[b, k-1] + \alpha_H(0.050, H, f_s) E_s[b, k] & (E_s[b, k] < E_{\text{floor}}[b, k-1]) \\ (1 - \alpha_H(0.800, H, f_s)) E_{\text{floor}}[b, k-1] + \alpha_H(0.800, H, f_s) E_s[b, k] & (E_s[b, k] \ge E_{\text{floor}}[b, k-1]) \end{cases}$$
  * **2段リングバッファによる 80 ms 変調度（$M_d^{\text{raw}}[b, k]$ [dB]）**:
    * 16フレーム（約 `5.33 ms`）を 1 ブロックとして `blk_max`, `blk_min` を蓄積し、**15スロット**（合計 `240フレーム ≈ 80 ms`）を保持する。
    * スロット番号は `slot = (k / 16) mod 15`。`k mod 16 == 0` のフレームでそのスロットを $a_b[k]$ で上書きし、それ以外は最大・最小を更新する。したがって窓は「書き込み中の1スロット（1〜16フレーム）＋完了済み14スロット」で、長さは `225〜240 フレーム`（`75〜80 ms`）の間を動く。
    * 起動時は書き込みが始まったスロット（`min(k / 16 + 1, 15)` 個）だけで最大・最小を取る（未充填スロットは比較対象外）。
    * 生値（dB、FSM 判定と共用）:
      $$M_d^{\text{raw}}[b, k] = \begin{cases} 0.0 & (E_s[b, k] < -80.0\text{ dBFS}) \\ 20\log_{10}\!\left(\frac{\max_{j}(a_b[j]) + 10^{-5}}{\min_{j}(a_b[j]) + 10^{-5}}\right) & (\text{otherwise}) \end{cases}$$
  * **局所ピーク持続指標（Ring Index 生値 $T^{\text{raw}}[b, k]$ [dB]）**:
    $$\text{prom}[b, k] = E_s[b, k] - \begin{cases} E_s[1, k] & (b=0) \\ E_s[30, k] & (b=31) \\ 0.5(E_s[b-1, k] + E_s[b+1, k]) & (1 \le b \le 30) \end{cases}$$
    $$T^{\text{raw}}[b, k] = (1 - \alpha_H(0.050, H, f_s)) T^{\text{raw}}[b, k-1] + \alpha_H(0.050, H, f_s)\,\text{prom}[b, k]\quad (T^{\text{raw}}[b, -1] = 0.0)$$
* **6ch 特徴量テンソル $X[c, b, k]$（演算順: 生値算出 → スケーリング → クリップ）**:

| Ch | 名称 | スケーリング式 | クリップ範囲 |
| :--- | :--- | :--- | :--- |
| `0` | 相対 SNR | $(E_s[b,k] - E_{\text{floor}}[b,k]) / 30.0$ | `[-3.0, +3.0]` |
| `1` | 全帯域正規化比 | $(E_s[b,k] - E_{\text{global}}[k]) / 20.0$ | `[-3.0, +3.0]` |
| `2` | 時間変化率 $\Delta E$ | $\Delta E[b,k] / 6.0$（$\Delta E = E_s[b,k] - E_s[b,k-1]$、$k=0$ は `0.0`） | `[-3.0, +3.0]` |
| `3` | 帯域別変調度 | $(M_d^{\text{raw}}[b,k] - 12.0) / 12.0$ | `[-2.0, +4.0]` |
| `4` | Ring Index | $T^{\text{raw}}[b,k] / 15.0$ | `[-2.0, +4.0]` |
| `5` | MODE 条件フラグ | `SPEECH = 0.0` / `VOCAL = 1.0` | `[0.0, 1.0]` |

### 2.5 Stage 5A：2ヘッド統合・単一FSM（規範擬似コード）
* **2ヘッド統合式**:
  $$F_{\text{lin}} = 10^{\text{Floor}_{\text{dB}} / 20.0},\quad m_{\text{comb}}[b] = m_{\text{fb}}[b]\cdot\big(1 - D\cdot(1 - m_{\text{rev}}[b])\big)$$
  $$M_{\text{target}}[b] = \max\!\big(F_{\text{lin}},\; 1 - S\cdot(1 - m_{\text{comb}}[b])\big)$$
* **優先順位**: `FAILSAFE_MUTE > RESET > ATTACK > VOICE_OVERRIDE > HOLD > SLOW_RELEASE > NORMAL_RELEASE`
* **未解決の課題（Ring Index のしきい値）**: 定常の純音を入れたときの $T^{\text{raw}}$ の最大値（全バンド中）は、音がバンド中心にあるとき `6.9〜11.4 dB`、2つのバンド中心の間にあるときは最小 `2.7 dB` まで下がる（解析応答による計算、48 kHz）。`+6 dB` を超えるのは 70 Hz〜13 kHz の周波数の `52%`。したがって下の擬似コードの「再発かつ `T_raw > 6 dB`」という延長条件は、ハウリング周波数の約半分でしか成立しない。対策候補は (a) しきい値を下げる、(b) 隣接2バンドの大きい方を自バンドとみなして突出度を取る、(c) 延長条件から Ring を外し Head A の深さだけで判定する。どれを採るかは Step 3 で学習済みモデルの挙動と合わせて決め、それまでは v1.5 のまま動かす。
* **ホップ境界 $k$ ごとの単一状態更新アルゴリズム（単位: 時間はすべてフレーム数、1フレーム $\Delta t = H / f_s$）**:

```cpp
// 定数フレーム換算 (fn(sec) = lround(sec * fs / H))
// N_HOLD_L0 = fn(1.5), N_HOLD_L1 = fn(4.0), N_HOLD_L2 = fn(12.0)
// N_REOFF_WIN = fn(2.0), N_VOVR = fn(0.080), N_RESET_RAMP = fn(0.005)
// EMA係数: a_atk = alpha_H(0.0015), a_vovr = alpha_H(0.008),
//          a_srel = alpha_H(0.800), a_nrel = alpha_H(mode==VOCAL ? 0.060 : 0.035)

// 1. RESET エッジ検知 (atomic exchange)
if (reset_trigger.exchange(false)) {
    reset_gru_and_svf_states();
    reset_ramp_rem = N_RESET_RAMP;
    voice_ovr_rem = 0;
    for (int b = 0; b < 32; ++b) {
        hold_rem[b] = 0; backoff_lvl[b] = 0; since_hold_end[b] = N_REOFF_WIN + 1;
        state[b] = NORMAL_RELEASE;
    }
}

// 2. RESET 復帰ランプ中（5 ms で 1.0 へ強制復帰。最終フレームは厳密に 1.0f を代入）
if (reset_ramp_rem > 0) {
    float step = 1.0f / (float)reset_ramp_rem;
    --reset_ramp_rem;
    for (int b = 0; b < 32; ++b)
        M_curr[b] = (reset_ramp_rem == 0) ? 1.0f : M_curr[b] + (1.0f - M_curr[b]) * step;
    return;
}

// 3. Voice-Triggered Release 検知（生値 dB を使用）
int voice_hits = 0;
for (int b = 5; b <= 21; ++b) {
    if (delta_E[b] > 3.0f && Md_raw[b] > 10.0f) ++voice_hits;
}
if (voice_hits >= 4) voice_ovr_rem = N_VOVR; // 80 ms 再トリガー延長

// 4. ロングHold（レベル>=1）同時稼働数のカウント
int active_long_holds = 0;
for (int b = 0; b < 32; ++b) if (hold_rem[b] > 0 && backoff_lvl[b] >= 1) ++active_long_holds;

// 5. バンド別 FSM 更新
for (int b = 0; b < 32; ++b) {
    bool deep_fb = (20.0f * fast_log10(std::max(m_fb[b], 1e-5f)) <= -6.0f);

    // 5a. ATTACK（減衰方向は常に最優先）
    if (M_target[b] < M_curr[b]) {
        M_curr[b] += a_atk * (M_target[b] - M_curr[b]);
        if (deep_fb) {
            // バックオフ段階は「Hold に入る瞬間」に1回だけ判定する。すでに HOLD 中のバンドは
            // 段階を変えない（1回の発振の中で自分自身を再発と数えない）。
            if (state[b] != HOLD) {
                // 延長条件: 前回Hold終了後 2.0s以内の再発 かつ Ring > +6 dB かつ 同時4バンド未満
                bool recurrence = (state[b] == SLOW_RELEASE || since_hold_end[b] <= N_REOFF_WIN)
                                  && (T_raw[b] > 6.0f);
                if (recurrence && backoff_lvl[b] == 0 && active_long_holds < 4) {
                    backoff_lvl[b] = 1; ++active_long_holds;
                } else if (recurrence && backoff_lvl[b] == 1) {
                    backoff_lvl[b] = 2;
                }
                state[b] = HOLD;
            }
            hold_rem[b] = (backoff_lvl[b] == 0) ? N_HOLD_L0
                        : (backoff_lvl[b] == 1) ? N_HOLD_L1 : N_HOLD_L2;
            // since_hold_end は Hold 終了時（5c）にだけ 0 へ戻す
        }
        continue;
    }

    // 5b. VOICE_OVERRIDE（Head A が安全側 m_fb > 0.75 へ戻ったバンドのみ Hold 解除）
    if (voice_ovr_rem > 0 && m_fb[b] > 0.75f) {
        hold_rem[b] = 0;
        backoff_lvl[b] = 0;
        state[b] = NORMAL_RELEASE;
        M_curr[b] += a_vovr * (M_target[b] - M_curr[b]);
        if (std::abs(M_target[b] - M_curr[b]) < 1e-4f) M_curr[b] = M_target[b];
        continue;
    }

    // 5c. HOLD → SLOW_RELEASE → NORMAL_RELEASE
    if (state[b] == HOLD) {
        if (deep_fb) hold_rem[b] = std::max(hold_rem[b], N_HOLD_L0); // 継続発振中はL0を下限維持
        if (hold_rem[b] > 0) {
            --hold_rem[b]; // ゲイン上昇を凍結
        } else {
            state[b] = SLOW_RELEASE;
            since_hold_end[b] = 0;
        }
    } else if (state[b] == SLOW_RELEASE) {
        if (since_hold_end[b] <= N_REOFF_WIN) ++since_hold_end[b]; // 上限で止める（長時間稼働での桁あふれ防止）
        M_curr[b] += a_srel * (M_target[b] - M_curr[b]);
        if (20.0f * fast_log10(std::max(M_curr[b], 1e-5f)) >= -1.0f) {
            state[b] = NORMAL_RELEASE;
            backoff_lvl[b] = 0;
        }
    } else { // NORMAL_RELEASE
        if (since_hold_end[b] <= N_REOFF_WIN) ++since_hold_end[b];
        M_curr[b] += a_nrel * (M_target[b] - M_curr[b]);
        if (std::abs(M_target[b] - M_curr[b]) < 1e-4f) M_curr[b] = M_target[b];
    }
}
if (voice_ovr_rem > 0) --voice_ovr_rem;
```

### 2.6 Stage 5B：非正制約付き Projected Gauss-Seidel（Gain Coupling 補正）
* **影響率行列 $\mathbf{C} \in \mathbb{R}^{32 \times 32}$ の事前計算（`prepareToPlay`）**:
  * 各バンド $j$ の Stage 6A フィルタへ基準ゲイン `-12.0 dB` を設定し、バンド中心周波数 $f_b$ における振幅応答（dB）を複素伝達関数から解析算出して $C_{b,j} = \frac{20\log_{10}|H_j(e^{j 2\pi f_b / f_s})|}{-12.0}$ とする。**対角 $C_{b,b}$ も同じ式で実測する**（ピーク型は厳密に `1.0`、シェルフは `0.57〜0.71`）。
  * 解析応答は $s = j\tan(\pi f / f_s) / g$ として $H(s) = m_0 + \dfrac{m_1 s + m_2}{s^2 + k s + 1}$。
  * 隣接 $\pm 6$ バンド（$|b - j| \le 6$）の帯行列を使用する（それより外は `0`）。`±2` では高域シェルフの裾が届く範囲を表せず、均一 `-12 dB` で最大 `3.3 dB`（48 kHz）〜 `5.5 dB`（96 kHz）外れた。
* **毎ホップ更新（3反復・in-place、演算量 最大 1,152 MACs＋96 除算/hop）**:
  * 初期値: $g_{\text{target}}[b] = \text{clip}(20\log_{10}(\max(M_{\text{curr}}[b], 10^{-5})),\, \text{Floor}_{\text{dB}},\, 0.0)$、$g_{\text{eq}}[b] \leftarrow g_{\text{target}}[b]$
  * 反復 $t = 1, 2, 3$ について、$b = 0 \dots 31$ を順に更新：
    $$g_{\text{eq}}[b] \leftarrow \text{clip}\!\left(\frac{1}{C_{b,b}}\left(g_{\text{target}}[b] - \sum_{\substack{j = \max(0, b-6) \\ j \neq b}}^{\min(31, b+6)} C_{b,j}\, g_{\text{eq}}[j]\right),\; \text{Floor}_{\text{dB}},\; 0.0\right)$$
  * 単一バンドカット時は隣接が `0.0 dB` に留まるため過減衰しない。
  * **精度の実測値（解析応答による数値検証、3レート共通）**: 全バンド均一 `-6 dB` で最大誤差 `0.42 dB`、均一 `-12 dB` で `0.63 dB`、単一バンド `-12 dB` はピーク型で誤差 `0`、シェルフで `0.38 dB`。
  * **既知の限界**: この補正は `-12 dB` 付近での線形近似である。全バンドを同時に深く切る場合は両端の帯域で誤差が広がり、均一 `-18 dB` で最大 `約 2.6 dB`、均一 `-24 dB` で最大 `約 5.3 dB` になる（シェルフが Floor に張り付くため）。少数バンドだけを切る通常動作には影響しない。

### 2.7 Stage 7：異常値フェイルセーフガード & ソフトリミッター
1. **Stage 7A（Fail-Safe Guard — リミッター直前に配置）**:
   * Stage 6A 出力 $y$ が `!std::isfinite(y)` または `std::abs(y) > 16.0f`（`+24 dBFS` 超）となったサンプルを検知した瞬間、PA現場での爆音再発を防ぐため **安全ミュート（`0.0f`）** を出力する。
   * 同時に全 SVF 状態（信号経路・サイドチェイン）・GRU 隠れ状態・第2.4節の統計量（フレーム番号 $k$ を 0 へ戻す）をゼロクリアし、Stage 6A の係数は補間なしで Floor 相当へ即時切り替え、**全バンドの `M_curr[b]` を `F_lin`（Floor 側）へ初期化**（0 dB 即復帰による爆音再発を防止）、`failsafe_mute_rem = N_ramp`（5 ms）をセットして線形フェードインで自動復帰する。
2. **Stage 7B（瞬時ソフトリミッター、0 samples）**:
   * 天井 $L = 10^{-0.5 / 20.0} \approx 0.944061$、ニー開始点 $K = 0.85$：
     $$y_{\text{wet}} = \begin{cases} y & (|y| \le K) \\ \text{sgn}(y)\left(K + (L - K)\tanh\!\left(\frac{|y| - K}{L - K}\right)\right) & (|y| > K) \end{cases}$$

### 2.8 将来拡張予約：Stage 6B 狭帯域適応ノッチ（ソフト v0.2.0 対象・v0.1.0 非実装）
* ソフト v0.1.0 では実装・ビルド対象外とする。v0.2.0 での拡張時は 3点 Goertzel パワー放物線補間による精密周波数推定を採用し、Head A $\le -6\text{ dB}$ かつ 15 ms 連続で周波数分散 $\pm 3\text{ Hz}$ 以内に収束した上位最大3バンドへ Cytomic SVF ノッチ（$Q = 35$, 最大 `-18 dB`）をロックする設計を予約する。

### 2.9 Stage 1：DC ブロッカー（1次 HPF, 20 Hz）
* 台形積分（TPT）の1次ハイパスとする（状態 `s`、初期値 `0.0f`、$G = \dfrac{g}{1+g},\; g = \tan(\pi \cdot 20 / f_s)$）:
  $$v = (x - s)\,G,\qquad \text{lp} = v + s,\qquad s' = \text{lp} + v,\qquad y = x - \text{lp}$$
* 60 Hz での減衰は `-0.46 dB`、99 Hz で `-0.17 dB`。第7.2節の Gain Coupling 試験はこの減衰を含んだ「入力比」で合格している。

---

## 3. ニューラルネットワーク厳密仕様 & バイナリ重み規範表

汎用推論ランタイム（ONNX Runtime / LibTorch 等）は禁止とし、C++ / SIMD（NEON / AVX2）で直接実装する（`plugin/Source/nn/SimdKernels.h`, `CausalGruEngine.h`）。重みが未設定のときは全マスク `1.0`（抑制なし）を返す。

### 3.1 層構成・演算規約・厳密計算量
| 層 | 種別 | 入力 → 出力 | 厳密演算仕様（PyTorch 完全準拠） | パラメータ数 |
| :--- | :--- | :--- | :--- | ---: |
| **Input** | 特徴量 | `(6, 32)` | チャネル優先 `[c][b]`、行優先（C-contiguous） | `0` |
| **Layer 1** | `Conv1d` + `SiLU` | `(6, 32) → (16, 32)` | `kernel_size=5, stride=1, padding=2, padding_mode='zeros', bias=True`, $\text{SiLU}(x)=x\sigma(x)$ | `496` |
| **Layer 2** | `Flatten` + `Linear` + `LayerNorm` + `SiLU` | `(16, 32) → 512 → 96` | `(16, 32)` を C-order Flatten (`idx = c*32 + b`) → Linear → `LayerNorm(96, eps=1e-5, elementwise_affine=True)` → SiLU | `49,440` |
| **Layer 3** | `GRU` Layer 1（単方向） | `96 → 96` | ゲート連結順 `[r, z, n]`（各96次元、計288）、`bias=True` | `55,872` |
| **Layer 4** | `GRU` Layer 2（単方向） | `96 → 96` | ゲート連結順 `[r, z, n]`（各96次元、計288）、`bias=True` | `55,872` |
| **Layer 5A** | `Linear` + `Sigmoid`（Head A） | `96 → 32` | ハウリング抑制マスク $m_{\text{fb}} \in (0, 1)$ | `3,104` |
| **Layer 5B** | `Linear` + `Sigmoid`（Head B） | `96 → 32` | 残響・ノイズ抑制マスク $m_{\text{rev}} \in (0, 1)$ | `3,104` |
| **合計** | — | — | **FP32 重み総容量 = `671,552 bytes`（`655.8 KiB`）** | **`167,888`** |

* **標準 PyTorch GRU ゲート式（Python / C++ 厳密一致）**:
  $$r_t = \sigma(W_{ir} x_t + b_{ir} + W_{hr} h_{t-1} + b_{hr}),\qquad z_t = \sigma(W_{iz} x_t + b_{iz} + W_{hz} h_{t-1} + b_{hz})$$
  $$n_t = \tanh\big(W_{in} x_t + b_{in} + r_t \odot (W_{hn} h_{t-1} + b_{hn})\big),\qquad h_t = (1 - z_t) \odot n_t + z_t \odot h_{t-1}$$
* **重み初期化（学習開始時）**: Conv1d / Linear は Kaiming Uniform、GRU $W_{ih}$ は Xavier Uniform、$W_{hh}$ は Orthogonal、バイアスは `0.0`（Head A/B の出力バイアスのみ初期出力 $\approx 0.9$ となる `+2.2`）。
* **厳密計算量**: GEMV/Conv `181,248 MACs`（`0.3625 MFLOPs`）＋ 要素演算 `7,232 FLOPs` ＝ **`369,728 FLOPs/frame`（`0.370 MFLOPs/frame`）**。
  * `48 kHz / H=16` (`3,000 Hz`): **`1.109 GFLOPs/sec`** ／ `44.1 kHz / H=15` (`2,940 Hz`): **`1.087 GFLOPs/sec`** ／ `96 kHz / H=32` (`3,000 Hz`): **`1.109 GFLOPs/sec`**。

### 3.2 バイナリ重みフォーマット（`.swmodel` / `EmbeddedWeights.h`）規範レイアウト
* **共有メモリ規約**: `EmbeddedWeights` は `alignas(64) inline constexpr float[]`（名前空間 `zerofb::embedded`、`kWeights` / `kWeightCount` / `kPayloadCrc32`）としてプロセス内全インスタンスで **Read-Only 1コピー共有**し、マルチインスタンス時の L2 キャッシュ溢れを防止する。各値は16進浮動小数点リテラルで書き出し、`.swmodel` の Payload とビット単位で一致させる。
* **生成元**: Step 3 で学習済みの重みができるまでは、`training/export_weights.py --random-test` がビルド時に `<build>/generated/nn/EmbeddedWeights.h` を生成する（固定の試験用重み。seed 42 の疑似乱数で、スケールは第3.1節の初期化に合わせ、全バイアスと LayerNorm 係数を非ゼロ、ヘッドの重みを3倍にしてある。**学習済みモデルではない**）。学習済みの重みは**公開リポジトリには入れず**、手元でビルド時に `plugin/Source/nn/EmbeddedWeights.h` として差し込む（`.gitignore` 対象）。リポジトリに入れるのは生成の仕組み（`training/export_weights.py`）までとする。
* **読み込み時の検査**: `.swmodel` はサイズ `671,616 bytes`・Magic・Header の6項目・CRC32 をすべて確認してから使う（`model::checkFile`）。
* **Header（64 bytes 固定・Little Endian、`0x00 .. 0x3F`）**:
  * `0x00..0x07`: Magic `"SWZFB004"`
  * `0x08..0x0B`: `uint32_t sample_rate_ref` (`48000`)
  * `0x0C..0x0F`: `uint32_t num_bands` (`32`)
  * `0x10..0x13`: `uint32_t num_features` (`6`)
  * `0x14..0x17`: `uint32_t hop_ref` (`16`)
  * `0x18..0x1B`: `uint32_t hidden_size` (`96`)
  * `0x1C..0x1F`: `uint32_t total_params` (`167888`)
  * `0x20..0x23`: `uint32_t crc32`（`0x40` 〜末尾の IEEE 802.3 CRC32）
  * `0x24..0x3F`: Zero Reserved
* **Payload（全18テンソル・すべてパディングなしで自然に 64-byte 境界整列・総ファイルサイズ `671,616 bytes`）**:
  * 格納順はすべて **行優先（Row-Major、PyTorch `.weight.contiguous()` そのまま）** とし、C++ SIMD GEMV は行ベクトル内積（`y[o] = dot(W[o, :], x)`）で計算する（転置不要かつ全行長が SIMD 幅の倍数）。

| # | テンソル名 | PyTorch 対応キー | 形状（Row-Major） | 要素数 | バイト長 | Offset (Hex) |
| :-: | :--- | :--- | :--- | ---: | ---: | :---: |
| 1 | `conv1d_weight` | `conv1.weight` | `(16, 6, 5)` | `480` | `1,920` | `0x000040` |
| 2 | `conv1d_bias` | `conv1.bias` | `(16,)` | `16` | `64` | `0x0007C0` |
| 3 | `proj_weight` | `proj.weight` | `(96, 512)` | `49,152` | `196,608` | `0x000800` |
| 4 | `proj_bias` | `proj.bias` | `(96,)` | `96` | `384` | `0x030800` |
| 5 | `ln_gamma` | `ln.weight` | `(96,)` | `96` | `384` | `0x030980` |
| 6 | `ln_beta` | `ln.bias` | `(96,)` | `96` | `384` | `0x030B00` |
| 7 | `gru1_w_ih` | `gru1.weight_ih_l0` | `(288, 96)` `[r,z,n]` | `27,648` | `110,592` | `0x030C80` |
| 8 | `gru1_w_hh` | `gru1.weight_hh_l0` | `(288, 96)` `[r,z,n]` | `27,648` | `110,592` | `0x04BC80` |
| 9 | `gru1_bias_ih` | `gru1.bias_ih_l0` | `(288,)` `[r,z,n]` | `288` | `1,152` | `0x066C80` |
| 10 | `gru1_bias_hh` | `gru1.bias_hh_l0` | `(288,)` `[r,z,n]` | `288` | `1,152` | `0x067100` |
| 11 | `gru2_w_ih` | `gru2.weight_ih_l0` | `(288, 96)` `[r,z,n]` | `27,648` | `110,592` | `0x067580` |
| 12 | `gru2_w_hh` | `gru2.weight_hh_l0` | `(288, 96)` `[r,z,n]` | `27,648` | `110,592` | `0x082580` |
| 13 | `gru2_bias_ih` | `gru2.bias_ih_l0` | `(288,)` `[r,z,n]` | `288` | `1,152` | `0x09D580` |
| 14 | `gru2_bias_hh` | `gru2.bias_hh_l0` | `(288,)` `[r,z,n]` | `288` | `1,152` | `0x09DA00` |
| 15 | `head_fb_weight` | `head_fb.weight` | `(32, 96)` | `3,072` | `12,288` | `0x09DE80` |
| 16 | `head_fb_bias` | `head_fb.bias` | `(32,)` | `32` | `128` | `0x0A0E80` |
| 17 | `head_rev_weight` | `head_rev.weight` | `(32, 96)` | `3,072` | `12,288` | `0x0A0F00` |
| 18 | `head_rev_bias` | `head_rev.bias` | `(32,)` | `32` | `128` | `0x0A3F00` |
| **終端** | — | — | **Payload = `671,552 B`** | **`167,888`** | **`671,552`** | **`0x0A3F80`** |

---

## 4. 学習データ生成・2段階学習・厳密損失仕様

### 4.1 開ループ学習と閉ループ評価の設計分離・変数定義
* **意図的分離**: 勾配計算の安定性・速度のため、学習は**開ループ（Open-Loop）**で行い、実戦の帰還安定性・ハンチング耐性は **Stage 5A の FSM ヒステリシス制御** と **Closed-Loop 評価ベンチ（第7.1節）** で担保する。両者のギャップを縮めるため、学習サンプルの `30%` には「前段で共振帯域がすでに `-6 〜 -15 dB` 減衰された微小リンギング状態（擬似閉ループ状態）」を含める。
* **開ループ学習チャンク合成（`48,000 Hz`, 長さ `4.0 秒` = `192,000 samples` = `12,000 フレーム`）**:
  1. $s_d[n]$: ドライ直接音声（RIR の初期 `2.0 ms` のみ畳み込み）。無音・環境ノイズのみサンプルでは $s_d[n] \equiv 0$。
  2. $r[n] = (s_d * h_{\text{late\_rir}})[n] + n_{\text{env}}[n]$: 後期残響 ＋ 環境ノイズ。
  3. $u[n] = s_d[n] + r[n]$: フィードバック付加前の収録信号。
  4. **物理フィードバック合成**（遅延 $D \in [240, 1440]$ samples、共振フィルタ $h_{\text{res}}$ は 1〜4個のバイカッド共振 $Q \in [15, 80], f_c \in [120, 8000]\text{ Hz}$、天井 $C_{\text{clip}} = 0.95$）:
     $$f_{\text{lin}}[n] = G \cdot (h_{\text{res}} * y_{\text{loop}})[n - D],\qquad y_{\text{loop}}[n] = C_{\text{clip}}\tanh\!\left(\frac{u[n] + f_{\text{lin}}[n]}{C_{\text{clip}}}\right)$$
     * ループゲイン分布（チャンク単位）: `30%` 発振直前（$G \in [0.85, 0.98]$）、`40%` 成長中（$G \in [1.01, 1.06]$）、`15%` 定常発振重畳、`15%` 発振なし（$G = 0$）。
  5. **ゲイン拡張**: $y_{\text{loop}}$ のアクティブ RMS が **`-50 dBFS 〜 -3 dBFS`** の一様分布になるようスカラー $\gamma$ を決定し、全成分を $\tilde{x} = \gamma y_{\text{loop}},\; \tilde{s}_d = \gamma s_d,\; \tilde{r} = \gamma r,\; \tilde{u} = \gamma u,\; \tilde{f} = \gamma f_{\text{lin}}$ へ一括スケーリングする。
  6. **学習時ノイズフロア初期値**: 各チャンク開始時、$E_{\text{floor}}[b, 0]$ を「$\tilde{r}$ の実測平均ログパワー $\pm 10\text{ dB}$ の一様乱数」で初期化し、起動直後と長期定常状態の両分布を網羅学習させる。
* **2ヘッド階層的 IRM 正解マスクと無音帯域有効性マスク（`valid_mask`）**:
  * 各成分 $\tilde{s}_d, \tilde{r}, \tilde{u}, \tilde{f}$ を Stage 2-SC と同一の ERB SVF バンクへ通し、採取時刻 $n_k = (k+1)H - 1$ での包絡線パワー $P_b(\cdot)$ を取得する。
  * 無音判定しきい値 $\theta_{\text{sil}} = 10^{-8}$（`-80 dBFS`）とし、有効マスク $V[b, k] = \mathbb{I}\big(P_b(\tilde{u}) + P_b(\tilde{f}) \ge \theta_{\text{sil}}\big)$ を定義する。
  * 教師マスク定義（分母 $\epsilon = 10^{-10}$）:
    $$m_{\text{fb}}^*[b, k] = \begin{cases} 1.0 & (V[b,k]=0) \\ \sqrt{\dfrac{P_b(\tilde{u})}{P_b(\tilde{u}) + P_b(\tilde{f}) + \epsilon}} & (V[b,k]=1) \end{cases},\qquad m_{\text{rev}}^*[b, k] = \begin{cases} 0.0 & (V[b,k]=0 \text{ または } P_b(\tilde{s}_d) < \theta_{\text{sil}}) \\ \sqrt{\dfrac{P_b(\tilde{s}_d)}{P_b(\tilde{s}_d) + P_b(\tilde{r}) + \epsilon}} & (\text{otherwise}) \end{cases}$$

### 4.2 データ構成・分割・再現性・ライセンス監査
* **構成比率（4秒チャンク数の比率）**: スピーチ `45%`（`Ch5=0.0`）、ボーカルドライ幹音 `25%`（`Ch5=1.0`）、楽器・BGM負例 `15%`（`Ch5 ∈ {0.0, 1.0}` 50/50）、無音・環境ノイズのみ `15%`。
* **分割・リーク防止・乱数Seed**:
  * 話者ID・楽曲ID単位で重複なし **Train `80%` / Val `10%` / Test `10%`** へ分割し、`manifest.json` へ固定記録する。
  * 乱数Seedは `seed = 42` で固定（PyTorch / NumPy / Python `random` / DataLoader worker）。
  * Checkpoint 選択規則: 2,000 steps ごとに Val セット全チャンクの総損失を評価し、最小値更新時のみ `best.pt` を保存する。
* **ライセンス監査ルール**: `training/prepare_data.py` は各ソースのライセンス種別を `manifest.json` へ必須記録する。Step 3B の商用配布モデル学習時は、商用利用・派生モデル配布が許可されたデータ（CC0 / CC-BY / 許諾済みデータ）のみを `--commercial-only` フィルタで抽出する。**非商用データ（CC BY-NC 等）を含む重みは個人利用専用とし、公開・配布しない（公開リポジトリにも入れない）。**

### 4.3 2段階学習プロトコル・微分可能EQプロキシ・厳密MR-STFT損失
* **Optimizer**: `AdamW`（`lr=1e-3`, `weight_decay=1e-4`, `betas=(0.9, 0.999)`）、Cosine Annealing（下限 `1e-5`）、勾配クリップ `max_norm=1.0`、Batch Size `32`。
* **TBPTT**: 4.0秒（`12,000 フレーム`）を `1.0秒`（`3,000 フレーム`）ごとに4分割し、各分割先頭で GRU 隠れ状態 $h_1, h_2$ を `.detach()` して引き継ぐ。
* **各損失項の厳密定義（バッチ平均は全サンプル等重み）**:
  1. **有効重み付き Mask-L1**:
     $$w[b, k] = 0.2 + 0.8\,V[b, k],\qquad \mathcal{L}_{\text{Mask-L1}} = \frac{\sum_{b,k} w[b,k]\big(|m_{\text{fb}} - m_{\text{fb}}^*| + |m_{\text{rev}} - m_{\text{rev}}^*|\big)}{2 \sum_{b,k} w[b,k]}$$
  2. **非対称フィードバック通過ペナルティ**:
     $$\mathcal{L}_{\text{FB-Penalty}} = \frac{\sum_{b,k} V[b,k]\cdot\mathbb{I}(m_{\text{fb}}^* < 0.7)\cdot\max(0,\, m_{\text{fb}} - m_{\text{fb}}^*)}{\max\!\big(1,\; \sum_{b,k} V[b,k]\cdot\mathbb{I}(m_{\text{fb}}^* < 0.7)\big)}$$
  3. **時間平滑化ペナルティ**:
     $$\mathcal{L}_{\text{Smooth}} = \frac{1}{2B(K-1)}\sum_{b=0}^{31}\sum_{k=1}^{K-1}\big(|m_{\text{fb}}[b,k] - m_{\text{fb}}[b,k-1]| + |m_{\text{rev}}[b,k] - m_{\text{rev}}[b,k-1]|\big)$$
  4. **Stage B 用 微分可能 EQ プロキシと厳密 MR-STFT 損失（Head 崩壊防止付き）**:
     * 毎バッチ内の各サンプルに対し、50% 確率で `D = 0.0`（正解波形 $y_{\text{ref}} = \tilde{u}$：残響は残しハウリングのみ除去）、50% 確率で `D = 1.0`（正解波形 $y_{\text{ref}} = \tilde{s}_d$：両方除去）を割り当てる。
     * 推論マスクから第2.5節の統合式（`S = 1.0, Floor = -36 dB`）および第2.6節の Projected Gauss-Seidel（3反復）で EQ ゲイン $g_{\text{eq}}[b, k]$ を算出し、解像度 $r \in \{256, 512, 1024, 2048\}$ ごとの STFT 周波数ビン $f_m$・フレーム $t$（対応時刻の $g_{\text{eq}}$ を平均プーリング）において、32段 Cytomic SVF の解析複素応答積 $H_{\text{total}}(f_m, t) = \prod_{b=0}^{31} H_b(e^{j2\pi f_m/f_s}; g_{\text{eq}}[b, t])$ を入力スペクトログラム $\mathcal{S}_r(\tilde{x})$ へ乗算して予測スペクトログラム $\hat{Y}_r = H_{\text{total}} \odot \mathcal{S}_r(\tilde{x})$ を得る。
     * STFT 仕様: `torch.stft` 準拠（`n_fft = r`, `hop_length = r // 4`, `win_length = r`, `window = hann_window(r, periodic=True)`, `center = True`, `pad_mode = 'reflect'`, `normalized = False`）。
     * 各解像度のスペクトル収束誤差（SC）および自然対数振幅誤差（LogMag、$\epsilon_{\text{stft}} = 10^{-7}$、$\|\cdot\|_F$ は時間・周波数全要素のフロベニウスノルム、$N_r$ は全ビン×フレーム数）:
       $$\mathcal{L}_{\text{SC}, r} = \frac{\big\||\mathcal{S}_r(y_{\text{ref}})| - |\hat{Y}_r|\big\|_F}{\big\||\mathcal{S}_r(y_{\text{ref}})|\big\|_F + \epsilon_{\text{stft}}},\qquad \mathcal{L}_{\text{Mag}, r} = \frac{1}{N_r}\sum_{m, t}\Big|\ln\big(|\mathcal{S}_r(y_{\text{ref}})| + \epsilon_{\text{stft}}\big) - \ln\big(|\hat{Y}_r| + \epsilon_{\text{stft}}\big)\Big|$$
       $$\mathcal{L}_{\text{MR-STFT}} = \frac{1}{4}\sum_{r \in \{256, 512, 1024, 2048\}}\big(\mathcal{L}_{\text{SC}, r} + \mathcal{L}_{\text{Mag}, r}\big)$$
* **段階的学習スケジュール**:
  * **Step 3A（ミニPoC）**: スピーチ 2時間＋物理ループ、`10,000 steps`、Stage A 損失のみ（$\mathcal{L}_A = 2.0\mathcal{L}_{\text{Mask-L1}} + 4.0\mathcal{L}_{\text{FB-Penalty}} + 0.5\mathcal{L}_{\text{Smooth}}$）。
  * **Step 3B（本番学習）**: ライセンス監査済み 200時間相当フルセット、Stage A `100,000 steps` ＋ Stage B `20,000 steps`（$\mathcal{L}_B = 1.0\mathcal{L}_{\text{MR-STFT}} + 1.5\mathcal{L}_{\text{Mask-L1}} + 4.0\mathcal{L}_{\text{FB-Penalty}} + 0.5\mathcal{L}_{\text{Smooth}}$）。

---

## 5. C++ / SIMD リアルタイム実装制約

1. **ゼロアロケーション・ロックフリー証明**: `processBlock` 内のヒープ確保・ロック・システムコールを禁止し、`juce::ScopedNoDenormals` を先頭配置。テストビルドではグローバル `operator new` フックにより `processBlock` 内の割当回数が厳密に **`0`** であることを自動検証する。
2. **SIMD 並列化（NEON / AVX2）**:
   * **サイドチェイン（32バンド × 1〜2ch）**: SoA（Structure of Arrays）メモリ配置とし、NEON（4バンド並列 `float32x4_t`）/ AVX2（8バンド並列 `__m256`）で常時更新。
   * **信号パス（直列32バンド × 1〜2ch）**: 全バンド常時稼働・スキップなし。L/R の SIMD 同時演算は行わずスカラーで実装する（DSP 全体で中央値 `1.4〜3.5%` と予算に対して小さく、複雑にする理由がないため。M4 実機で CPU が不足した場合のみ再検討する）。
   * **NN 推論**: GEMV は4行同時・行ベクトル内積、Conv1d は帯域方向に並列。内積に限り FMA（`_mm256_fmadd_ps` / `vfmaq_f32`）を使う。SIMD 幅で加算順が変わるため、NN の出力は参照とビット一致ではなく第7.2節の許容誤差で照合する。活性化関数（Sigmoid / Tanh / SiLU）の SIMD 実装は `FastMath.h` のスカラー関数と同じ演算列で、結果もビット一致させる。
3. **高速超越関数**: `fast_log10` / `fast_exp2` / Padé `fast_tanh` / `fast_sigmoid` を使用し、単体テストで `std::` 標準関数比の最大絶対誤差 **`< 1e-5`**（動作ドメイン内）を検証する。
   * 動作ドメイン: `fast_log10` は `[1e-10, 1e3]`、`fast_exp2` は `[-30, 4]`、`fast_tanh` / `fast_sigmoid` は有限の全入力。
   * `fast_exp2(0) == 1`、`fast_log10(1) == 0` は厳密に成り立つこと（0 dB のとき Stage 6A が完全に素通しになる条件）。
   * `ZEROFB_EXACT_MATH` を定義したビルドは同じ箇所で `std::` 関数を使う（第7.2節の厳しい側の照合用）。
   * コンパイラの積和融合は無効にする（GCC / Clang は `-ffp-contract=off`、MSVC は `/fp:precise`）。`-ffast-math` は使わない（`isfinite` 判定が壊れるため）。
4. **状態保存（APVTS）**: XML に `state_version="1"` 属性を付与して保存し、後方互換を維持する。
5. **ロックフリーUI通信**: `std::atomic` の値（32バンド現在GR値、Hold 中の帯域を表すビット列、IN/OUTピーク、CPU負荷率、ブロック長）による単一方向通信のみを使用。`RING` の周波数表示は画面側が GR 値と Hold ビット列から求める。`RESET` は APVTS のパラメータ経由。

---

## 6. パラメータ・プリセット・UI/UX仕様（SVGマスター準拠）

### 6.1 パラメータ一覧
| パラメータID | 表示名 | 範囲 | デフォルト | 内部動作 |
| :--- | :--- | :--- | :--- | :--- |
| `suppress` | **SUPPRESS** | `0.0 %` 〜 `100.0 %` | `75.0 %` | マスク適用深度 $S$。`0.0%` へ 5 ms フェード後 Bit-Perfect 迂回 |
| `mode` | **MODE** | `SPEECH` / `VOCAL` | `SPEECH` | `SPEECH`: Release 35ms, `Ch5=0.0` ／ `VOCAL`: Release 60ms, `Ch5=1.0` |
| `dereverb` | **DEREVERB** | `0.0 %` 〜 `100.0 %` | `50.0 %` | Head B 適用重み $D$（`0%` でハウリング抑制特化） |
| `floor` | **FLOOR** | `-36.0 dB` 〜 `-6.0 dB` | `-24.0 dB` | マスク下限ゲイン $\text{Floor}_{\text{dB}}$ |
| `reset` | **RESET** | Momentary Trigger | `OFF` | Hold・GRU状態・フィルタ状態をクリアし 5 ms 復帰 |
| `bypass` | **BYPASS** | `OFF` / `ON` | `OFF` | 5 ms クロスフェード付き Bit-Perfect ハードバイパス（ホスト標準 Bypass 同期） |

### 6.2 工場出荷プリセット & UX 要件
* **内蔵プリセット（4種）**:
  1. `01_Conference_Lavalier`: `SUPPRESS 80%`, `DEREVERB 60%`, `FLOOR -24dB`, `SPEECH`
  2. `02_Handheld_Speech`: `SUPPRESS 75%`, `DEREVERB 40%`, `FLOOR -24dB`, `SPEECH`
  3. `03_Live_Vocal`: `SUPPRESS 65%`, `DEREVERB 25%`, `FLOOR -15dB`, `VOCAL`
  4. `04_Feedback_Only`: `SUPPRESS 85%`, `DEREVERB 0%`, `FLOOR -30dB`, `SPEECH`
* **プリセットのボタン**: 画面に4個並べる（表示は `CONF LAV` / `HANDHELD` / `LIVE VOCAL` / `FB ONLY`）。4つの値がプリセットと一致している間だけ点灯する。ホストのプログラム一覧にも同じ4個を出す。
* **UX 操作**: ダブルクリックでデフォルト復帰、`Shift/Cmd(Ctrl) + ドラッグ` で微調整、数値クリックでキーボード直接入力。
* **埋め込みフォント**: `juce_add_binary_data` で埋め込み。SIL OFL 1.1 の `Michroma`（ロゴ）＋ `Barlow Condensed`（ラベル・ボタン）＋ `Space Mono`（数値）。ライセンス文は `plugin/Assets/Fonts/OFL-*.txt`。画面に日本語は出さない。

### 6.3 UIデザインマスター（`docs/ui_master.svg` / `760 × 440 px`）
* SW AUDIO の LIVE ライン準拠。区分色は Restore（`#6aa832`、リング `#8fd14a`、耳 `#335a12`）。状態色は OK `#2bd14a`／注意 `#f0c93d`／超過 `#e0443e` で、区分色とは混ぜない。
* 画面の大きさは固定（`760 × 440 px`、リサイズなし）。座標・色・文字の大きさは `docs/ui_master.svg` に合わせる。
* 構成（上から）:
  1. **ツールバー（40 px＋区分色の帯 3 px）**: `SW AUDIO` ／ `LIVE` ／ `ZERO-FB`、レイテンシー表示（`0 smp` / `0.0 ms`。LIVE ラインでは常時表示）、`MODE`（`SPEECH` / `VOCAL`）、`RESET`（押している間だけ ON）、`BYPASS`。
  2. **GAIN REDUCTION 表示（左）**: 32バンドの現在の GR を棒で表示（縦軸 `0 〜 -24 dB`、横は ERB 配置どおり）。Hold 中の帯域は注意色にし、いちばん深い Hold 帯域の中心周波数を `RING x.xx kHz` と出す。右上に最大 GR。
  3. **プリセットのボタン4個**（高さ 44 px）。
  4. **IN / GR / OUT メーター**（各36セグメント。IN/OUT は 1 セグメント 1 dB、下限 `-36 dBFS`）。
  5. **右列**: `SUPPRESS`（大）、`DEREVERB`・`FLOOR`（小）の LED リング（各32セグメント）。数値は中央に表示。
  6. **下の帯（30 px）**: 動作の一言説明、`CPU x.x %`、`BUF n`。対応外のサンプリングレートのとき、および学習済みの重みが無いときは注意色で知らせる。
* SW AUDIO 標準機能のうち Morph・Auto gain・Δ・SW Link・Lock・Remote は本書の対象外で、実装しない。
* 自動検査: `docs/ui_master.svg` が標準 XML/SVG としてエラーなくパースできること。実際の描画は `UiSnapshot`（ホストなしで画面を PNG に書き出す道具）で作り、マスターと目で見比べる。`docs/ui_render.png` がその出力。

---

## 7. 自動テスト・ゴールデンパリティ・閉ループ評価プロトコル

### 7.1 閉ループ自動ベンチマーク（`plugin/tests/ClosedLoopBench.cpp`）
仮想音響ループ内に `ZERO-FB` を直列挿入して計測する：
$$x_{\text{mic}}[n] = s_{\text{voice}}[n] + G \cdot (h_{\text{room}} * y_{\text{plugin}})[n - D]$$
* **決定論的ベンチ条件（seed = 42、`48 kHz` モノラル、1回 10秒）**:
  * **RIR の合成式**: 長さ `RT60 × fs` サンプル。拡散成分 $h_d[n] = \mathcal{N}(0,1)\cdot 10^{-3 n / (\text{RT60}\cdot f_s)}$ に、主要共振3つ $a_i\,e^{-n/\tau_i}\sin(2\pi f_i n / f_s + \phi_i)$（$\tau_i = Q f_s / (\pi f_i)$）を加える。共振のピークは拡散成分のスペクトル RMS に対して `+14 / +12.5 / +11 dB`（第1〜第3共振）。最後に、応答の最大振幅が `0 dB` になるよう全体を正規化する。
  * **発散限界の理論値**: バイパス時のループ応答 $G\cdot H(f)e^{-j\omega D}$ の位相が $2\pi$ の整数倍になる周波数のうち実部が最大の点から $G_0$ を予測し、探索で求めた $G_0$ と照合する（合格基準: 実測が予測の `-0.5 〜 +1.5 dB` 以内。3会場とも実測と予測は一致した）。
  * **畳み込み**: ループ遅延 $D$ があるので、128 サンプル単位の分割 FFT 畳み込みで計算する（直接畳み込みとの一致を毎回検査）。
  * 仮想会場 RIR 3種（指数減衰ガウスノイズ＋主要共振3極の合成 RIR）:
    1. `Room_Small`: `RT60 = 0.6 s`, $D = 240\text{ smp}$ (`5 ms`), 主共振 `{420, 1850, 3900} Hz` ($Q = 30$)
    2. `Room_Hall`: `RT60 = 1.2 s`, $D = 576\text{ smp}$ (`12 ms`), 主共振 `{280, 1250, 2600} Hz` ($Q = 45$)
    3. `Room_Large`: `RT60 = 2.0 s`, $D = 1200\text{ smp}$ (`25 ms`), 主共振 `{190, 920, 2150} Hz` ($Q = 60$)
  * 刺激音声 $s_{\text{voice}}[n]$: Val/Test スピーチ（RMS `-24 dBFS`、発話 2秒／休止 1秒の反復 10秒）。
  * **Step 2 での刺激音声**: 学習データがまだ無いため、合成の疑似発話（基本周波数 約118 Hz の倍音列にフォルマント3つ、3.7 Hz の音節包絡、息ノイズ）を使う。Step 3 で Val/Test スピーチへ差し替える。
* **計測および採点手順**:
  1. **発散判定**: 出力 $y_{\text{plugin}}$ の 50 ms 移動 RMS が `-3.0 dBFS` を `100 ms` 連続超過、または休止区間の出力 RMS が刺激平均 RMS を `+6.0 dB` 超過した場合を発散と判定する。
  2. **$G_0$ 探索**: `BYPASS = ON` 状態で二分探索（精度 `0.25 dB`）により発散限界ゲイン $G_0$ を求める。
  3. **GBF 計測**: `ZERO-FB` 有効時（`SUPPRESS=75%, DEREVERB=50%, FLOOR=-24dB, SPEECH`）に $G_0$ から `+0.5 dB` 刻みで 10秒稼働させ、非発散最大ゲイン $G_{\max}$ から $\text{GBF} = G_{\max} - G_0$ を算出（合格基準: **3会場平均 `+6.0 dB` 以上**）。
  4. **ハンチング（バースト）判定**: $G = G_0 + 4.0\text{ dB}$ 固定で 10秒連続稼働させ、一度抑制された帯域の 50 ms 移動パワーが休止区間中に `+10 dB` 以上再上昇する事象をカウント（合格基準: **0 回**）。
  5. **モードホッピング収束**: 第1共振の抑制後に第2共振が `-20 dBFS` を超えて立ち上がってから `-30 dBFS` 以下へ沈むまでの時間を計測（合格基準: **`50 ms` 以内**）。
* **数え方の確定（実装）**:
  * 発散判定 2 の「休止区間の出力 RMS」は休止 1秒ぶん全体で評価する。出力が非有限、または絶対値 `64` 超も発散とする（バイパス時の暴走打ち切り）。
  * ハンチングは、プラグイン出力を第2.4節と同じ32バンドで観測し、「それまでに EQ ゲインが `-3 dB` 未満になったことがある帯域」について、休止区間内の 50 ms 移動パワーがその休止での最小値から `+10 dB` 以上（かつ `-60 dBFS` 超）へ上がった回数を数える。最小値 `+3 dB` 未満へ戻ったら次の1回を数えられる状態に戻す。
  * モードホッピングは、3つの共振にもっとも近い帯域を対象とし、「別の共振の帯域がすでに抑制された後」にその帯域が `-20 dBFS` を超えてから `-30 dBFS` 未満へ沈むまでの時間の最大値とする。10秒の終わりまで沈まなければ「未収束」。
* **Step 2 の位置づけ（骨格）**: 学習済みモデルが無いので GBF・ハンチング・ホッピングの値は参考表示のみ。合否にするのはベンチ自身の正しさ（畳み込みの一致、$G_0$ の理論値との一致、全マスク `1.0` のとき GBF が `±0.5 dB` 以内で EQ が動かないこと）。
* **Step 2 の参考値**: 全マスク `1.0` で GBF `0.0 dB`（3会場とも）。試験用のルールベース推定器（Ring `> 4 dB`・変調度 `< 6 dB`・長期レベル超の帯域を `0.1` にするだけのもの。製品には入らない）で3会場平均 GBF `+3.0 dB`。FSM と EQ が閉ループで働くことの確認であり、性能値ではない。

### 7.2 ゴールデンパリティ & 単体自動テスト仕様
1. **独立 NumPy 参照による Golden Fixture 生成**:
   * `training/gen_golden_fixtures.py` が固定 seed `42` の決定論的 5秒テスト信号（インパルス＋チャープ＋疑似発話＋2kHzトーン（L/R 逆相）＋無音。`44.1 / 48 / 96 kHz` のステレオ3種と `48 kHz` モノラル1種）を生成し、**C++ とコードを共有しない純 Python/NumPy 参照実装**（`training/erb_dsp.py`）を float64 と float32 の両方で実行して期待出力と SHA-256 マニフェスト（`sha256.txt`）を書き出す。
   * 生成物は約 `200 MB` あるためリポジトリには入れず、**ビルド時に毎回生成する**（CMake のカスタムコマンド。所要 約1分）。
   * Step 1 の対象は特徴量 `(T, 6, 32)` と影響率行列・Projected Gauss-Seidel。Step 2 で、固定の試験用重み `test_model.swmodel`、全フレームの出力マスク `(T, 64)`（4ケース）、先頭 2,000 フレームの全層出力（`conv1` 512＋`proj` 96＋`gru1` 96＋`gru2` 96＋マスク 64 ＝ 864 値、`48 kHz` ステレオ）を追加した。MR-STFT 損失値は Step 3 で `losses.py` と一緒に追加する。
   * NN の NumPy 参照（`training/nn_reference.py`）は PyTorch の `training/model.py` と float64 で `3.3e-16` まで一致することを確認済み（同じ重み・同じ入力 3,000 フレーム）。
   * **合格基準（$L_\infty$ ノルム最大絶対誤差。特徴量はスケーリング後の値）**:
     * 高速近似関数単体（`fast_log10 / fast_exp2 / fast_tanh / fast_sigmoid` vs `std::`）: **`< 1e-5`**
     * Python float32 参照 vs float64 参照: **`< 1e-3`**（単精度そのものの丸め限界。実測 `1.4e-4 〜 1.7e-4`）
     * C++（`ZEROFB_EXACT_MATH` ビルド、AVX2 / NEON / スカラーの各経路）vs Python float32 参照: **`< 2e-5`**（同じ演算列なので、差は C ライブラリごとの `log10` の最終桁のみ）
     * C++（高速近似ビルド）vs Python float32 参照: **`< 1e-4`**
     * C++（両ビルド）vs float64 参照: **`< 1e-3`**
     * 影響率行列 vs 参照: **`< 1e-6`**、Projected Gauss-Seidel の解 vs 参照: **`< 1e-3 dB`**
   * 特徴量の照合は「解析器単体」「エンジン経由（1ホップを不均等な2ブロックに分割）」「エンジン経由（バイパス中）」の3通りで行い、ホップ境界 $n_k = (k+1)H - 1$ とバイパス中の制御経路の継続を同時に確認する。
   * **NN の合格基準（固定の試験用重みでの値。Step 2 で実測して確定）**:
     * Python float32 参照 vs float64 参照（マスク）: **`< 1e-4`**（実測 `3.9e-6 〜 9.2e-6`）
     * C++（全ビルド・全経路）の各層出力 vs Python float32 参照: **`< 2e-5`**（実測 最大 `4.5e-6`、3つのOSを通じて）
     * C++ の各層出力 vs float64 参照: **`< 1e-4`**（実測 最大 `2.6e-5`、LayerNorm 直後の `proj` 層）
     * 音声 → C++ 特徴量 → C++ NN の通しで得たマスク vs 参照（float32・float64 とも）: **`< 1e-4`**（実測 最大 `9.2e-6`）
     * SIMD の活性化関数 vs スカラーの `fast_tanh` / `fast_sigmoid`: **ビット完全一致**
     * 試験用重みは誤差が時間方向に増幅しない性質だった（5秒・15,000 フレームで上の値）。学習済みの重みで同じとは限らないので、Step 3 で重みを差し替えたら数値を測り直す。
2. **`ZeroLatencyTest.cpp`**:
   * `SUPPRESS = 0%` および `BYPASS = ON`（5 ms 遷移完了後）でのサンプル単位ビット完全一致（最大絶対誤差 `0.0`）。
   * 変則ブロック長（`1, 7, 16, 31, 64, 5000 samples`）切替時に出力が 1サンプル単位処理と完全一致（誤差 `< 1e-6`）かつヒープ確保 `0`（`operator new` フック検証）。
   * 任意マスク適用時のプリリンギング `0`・インパルス開始遅延 `0 samples`。
   * **Gain Coupling 均一試験**: 全32バンド目標マスクを均一 `-12.0 dB`（`SUPPRESS=100%`, `DEREVERB=0%`, `FLOOR=-36 dB`、350 ms 整定後、整数周期ぶんの区間で測定）とした際、Band 0〜31 の全32中心周波数（`60 Hz 〜 15 kHz`）での定常サイン波入力比が **`-12.0 dB ± 1.0 dB`** 以内。また単一バンド（Band 15 のみ `-12.0 dB`）時の Band 15 減衰が **`-12.0 dB ± 0.5 dB`** 以内。
   * **NaN / Over-Range フェイルセーフ試験**: `NaN`, `+Inf`, `-Inf`, `+30 dBFS` 注入時に即座に安全ミュート（`0.0`）され、初期マスク `F_lin` から 5 ms で正常復帰すること。バイパス中に `NaN` が入った場合は出力がビット完全一致のまま、制御経路の特徴量が有限値へ戻ること。
   * **FSM 試験**: 1回の連続発振でバックオフがレベル0のまま、Hold 中はゲインが上がらない、再発でレベル 0→1→2、Ring なしの再発は延長しない、ロングHold は同時4バンドまで、Voice-Trigger 後 20 ms 以内に `-1 dB` 以上へ復帰、RESET は 5 ms で厳密に `1.0` へ戻る。
3. **音質・レベル不変性・耐久・互換試験（Step 3〜4 完了条件）**:
   * **レベル不変性試験**: `-40 dBFS`, `-20 dBFS`, `-6 dBFS` の 2 kHz 定常サイン波（2秒整定後の 1秒間 RMS 比較）の減衰量差が **`±2.0 dB` 以内**。さらに `-30 dBFS → -10 dBFS` のステップ急変後 `1.0 秒` 以内に定常減衰の `±2.0 dB` 以内へ再収束すること。
   * **STOI 音質試験**: Test スピーチ（クリーン、発話アクティブ区間のみ、10 kHz リサンプリング標準 `pystoi`）通過時の STOI 劣化が **`< 0.02`**。
   * **BGM 後発話解放時間**: 3秒間の和音BGM直後に発話アタックを投入し、音声コア帯域平均 GR が `-1.0 dB` 以上へ復帰するまでの時間（起点: 発話開始サンプル、終点: `-1.0 dB` 到達）が **`<= 20 ms`**。
   * **互換・耐久**: `pluginval --strictness-level 8`（macOS/Windows 全フォーマット）および `auval`（macOS AU）全パス。48 kHz / 16 samples バッファで 1時間連続稼働し xrun `0 回`・ヒープ増加 `0 bytes`。

---

## 8. リポジトリ構成・CI・配布仕様

```text
zero-fb-suppressor/
├── CMakeLists.txt                   # DSPコア＋テスト。`-DZEROFB_BUILD_PLUGIN=ON` で JUCE 9.0.6 を取得し VST3/AU をビルド
├── LICENSE                          # AGPLv3
├── .github/workflows/ci.yml         # Linux/macOS/Windows 自動ビルド + 全テスト、プラグインのビルド + pluginval（L8）+ auval
├── docs/
│   ├── SPEC.md                      # 本単一正本設計書（v1.7）
│   ├── SIMD.md                      # SIMD最適化の実装指針（本書が優先）
│   ├── ui_master.svg                # 760x440 SVGデザインマスター（XMLパース検査対象）
│   └── ui_render.png                # UiSnapshot が書き出した実際の画面
├── training/
│   ├── prepare_data.py              # 48kHz正規化 + 話者/楽曲分離 + ライセンス記載 manifest.json
│   ├── gen_golden_fixtures.py       # 固定seed 3SRテスト信号 + 独立FP64期待値 + SHA-256生成
│   ├── loop_simulator.py            # 開ループ物理フィードバック合成 + 2ヘッドIRM + 擬似閉ループ
│   ├── erb_dsp.py                   # C++側と式・定数完全一致のPython実装（PGS補正含む）
│   ├── model.py                     # Causal Conv(6->16) + 2-Layer GRU (167,888 params, 64ch)
│   ├── nn_reference.py              # NN の NumPy 参照実装（C++・PyTorch とコードを共有しない）
│   ├── losses.py                    # Mask-L1 / FB-Penalty / Smooth / 微分可能STFT-EQ MR-STFT
│   ├── train.py                     # Step 3A (PoC 10k) / Step 3B (Stage A 100k + Stage B 20k)
│   └── export_weights.py            # .swmodel (671,616B, CRC32) + EmbeddedWeights.h 出力
└── plugin/
    ├── Assets/Fonts/                # Michroma / Barlow Condensed / Space Mono (SIL OFL埋込)
    ├── Source/
    │   ├── PluginProcessor.h/.cpp   # AudioProcessor（Stereo Link / APVTS v1 / Bypass同期 / Fail-Safe）
    │   ├── PluginEditor.h/.cpp      # SVG準拠カスタムUI・60fpsタイマー描画・4プリセット
    │   ├── dsp/
    │   │   ├── BandLayout.h         # 32バンド配置・ホップ長・EMA係数の共通定義
    │   │   ├── FastMath.h           # fast_log10 / fast_exp2 / fast_tanh / fast_sigmoid
    │   │   ├── CytomicSvf.h         # ZDF SVF（BP / LowShelf / Peak / HighShelf）＋サイドチェイン SoA SIMD 実装
    │   │   ├── SerialPeakingEq.h    # 32バンド直列EQ + Projected Gauss-Seidel（3反復）
    │   │   ├── SidechainAnalyzer.h  # L/Rパワー合成 + 2段リングバッファ + 6ch特徴量
    │   │   ├── MaskSmoother.h       # 2ヘッド統合 + 単一FSM（Backoff Hold / Voice-Trigger）
    │   │   └── ZeroFbEngine.h       # 全段をつなぐDSPコア本体（JUCE非依存・マスク推定器は差し替え式）
    │   ├── nn/
    │   │   ├── SimdKernels.h        # NEON / AVX2 Row-Major GEMV・Conv1D・LayerNorm・GRU
    │   │   ├── CausalGruEngine.h    # ゼロアロケーション推論エンジン（192in -> 64out）
    │   │   └── EmbeddedWeights.h    # static constexpr alignas(64) 共有重み配列（学習済みは非公開。ビルド時に生成・差し込み）
    │   └── ui/
    │       ├── CustomLookAndFeel.h  # 色・フォント・LEDリングのノブ・ボタン・UX操作実装
    │       └── GrAnalyzerView.h     # 32バンドリアルタイムGRキャンバス
    ├── tools/
    │   └── UiSnapshot.cpp           # ホストなしで画面を PNG に書き出す（マスターとの見比べ用）
    └── tests/
        ├── TestUtil.h               # テスト共通の小道具
        │                            # （Golden Fixture はビルドフォルダへ生成。リポジトリには入れない）
        ├── ZeroLatencyTest.cpp      # Bit-perfect / 0smp / PGS均一・単一 / 変則BLK / NaN / Heap=0
        ├── GoldenParityTest.cpp     # 近似関数単体 + 3SR特徴量・行列・PGS vs NumPy参照（第7.2節の基準）
        ├── EngineBench.cpp          # 1コールバックのCPU負荷計測（中央値・P99。参考表示）
        ├── ClosedLoopBench.cpp      # 3仮想会場 GBF / ハンチング / ホッピング（Step 2 は骨格。第7.1節）
        └── PluginSoakTest.cpp       # ホストなしの AudioProcessor 検査＋1時間耐久（プラグインをビルドするときだけ）
```

---

## 9. 実装マイルストーン（順番に実行・各ステップの全テスト合格を必須とする）

* **Step 1（DSPコア＆Golden Fixture・数値完全一致検証）**:
  * `FastMath.h`, `CytomicSvf.h`, `SerialPeakingEq.h`（PGS 3反復）, `SidechainAnalyzer.h`（6ch・2段リングバッファ）, `MaskSmoother.h`（規範擬似コード準拠）, `training/erb_dsp.py`, `training/gen_golden_fixtures.py` を実装する。
  * **完了条件**: `ZeroLatencyTest`（Bit-Perfect / 0smp / 均一 `-12 ± 1 dB`・単一 `-12 ± 0.5 dB` / 変則ブロック / NaNフェイルセーフ / FSM / `new` 回数 0）および `GoldenParityTest`（近似関数 `< 1e-5`・3SR 特徴量・行列・PGS が第7.2節の基準内）が、Linux（AVX2）・macOS（NEON）・Windows（AVX2）の3環境で全パスすること。
  * **状態**: 完了（ソフト v0.1.0 開発中の最初の区切り）。Step 1 の時点ではマスク推定器が未接続のため、エンジンは全マスク `1.0`（抑制なし）で動く。
* **Step 2（SIMD推論エンジン＆閉ループベンチ骨格）**:
  * `SimdKernels.h`, `CausalGruEngine.h`, `EmbeddedWeights.h`（ダミー重み生成含む）, `training/model.py`, `training/export_weights.py`, `ClosedLoopBench.cpp` を実装する。
  * **完了条件**: `.swmodel` バイト長 `671,616 bytes` & CRC32 一致、中間層・64ch 出力が第7.2節の3段基準内（数値は着手時に実測して確定）、Apple M4 系 `48 kHz / 16 smp / Stereo` での CPU 負荷 中央値 `<= 10%`・P99 `<= 15%`・ヒープ確保 `0 bytes` を達成すること。
  * **状態**: 実装とテストは完了。`.swmodel` 671,616 bytes・CRC32 一致、各層と 64ch マスクが第7.2節の基準内、NN を接続した状態でヒープ確保 0 を Linux（AVX2）・macOS（NEON）・Windows（AVX2）で確認。**CPU 負荷の完了条件だけは Apple M4 系の実機が無いため未確認**（参考値は第1.2節。GitHub の macOS 実行機では中央値は基準内、P99 は基準外だった）。
* **Step 3（段階的モデル学習＆実機組み込み）**:
  * **Step 3A（ミニPoC）**: 2時間スピーチ＋開ループ合成で `10k steps` 学習 → `EmbeddedWeights.h` 出力 → `ClosedLoopBench` でエンドツーエンドの GBF 向上とハンチング抑制を確認。
  * **Step 3B（本番学習）**: ライセンス監査済み 200時間相当フルセットで Stage A（`100k steps`）＋ Stage B（微分可能 STFT-EQ プロキシ `20k steps`）を学習し、GBF（3会場平均 `+6.0 dB` 以上）・ハンチング `0 回`・ホッピング `<= 50 ms`・レベル不変性（`±2.0 dB`）・STOI 劣化 `< 0.02`・BGM後解放 `<= 20 ms` をすべて満たす重みを確定する。
* **Step 4（SVGマスター準拠UI・最終検証・パッケージ化）**:
  * `docs/ui_master.svg`（XML/SVG パース検査パス）の座標・カラー・OFL埋込フォント・UX操作・4プリセットを `PluginEditor` へ実装し、macOS Universal（VST3/AU）および Windows x64（VST3）をビルドする。
  * **完了条件**: `pluginval --strictness-level 8`・`auval` 全パスおよび 48 kHz / 16 samples 1時間耐久 xrun `0 回`・ヒープ増加 `0 bytes` を達成すること。
  * **状態**: 画面と VST3 / AU のビルドは実装済み（JUCE 9.0.6）。`PluginSoakTest` の1時間耐久でヒープ確保 0・非有限値 0 を確認。`pluginval`（L8）と `auval` は CI で実行する。**未確認**: 実機のオーディオ装置での xrun `0 回`（共有の仮想マシンでは判定できない）、実際の DAW での動作、署名・公証と配布用パッケージ（`kajisho5/runway` を使う。未着手）。学習済みの重みが無いため、配布できるリリースはまだ作らない。
