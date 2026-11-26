# ZERO-FB SIMD最適化詳細

このドキュメントは、ZERO-FB（Zero-Latency AI Feedback & Dereverb Suppressor）におけるSIMD最適化の具体的な方針と実装指針をまとめたものである。  
対象アーキテクチャは仕様書通り **NEON（Apple Silicon）** および **AVX2（x86_64）** とする。

> **優先順位**: 本書は実装の手引きであり、食い違いがある場合は必ず `docs/SPEC.md`（単一正本）が優先する。
> **Step 2 時点の実装状況**: サイドチェイン（第2章）と NN 推論（第5章）は AVX2 / NEON / スカラーの3経路で実装済み。信号パスの L/R 同時処理（第3章）は見送り（DSP 全体で中央値がブロック予算の 1.4〜3.5% と小さかったため、スカラーのまま）。NN の内積だけは積和融合（FMA）を使う。

## 1. 最適化優先順位

| 優先度 | 対象 | 理由 | 期待効果 |
|--------|------|------|----------|
| 1 | サイドチェイン解析（Stage 2-SC） | 32バンド × 毎サンプル更新 | 大 |
| 2 | 信号パス直列EQ（Stage 6A） | 32バンド直列 × L/R | 大 |
| 3 | 特徴量抽出（Stage 3） | パワー合成・EMA・対数 | 中 |
| 4 | NN推論（Stage 4） | Conv1d / Linear / GRU / LayerNorm | 大 |
| 5 | Projected Gauss-Seidel | 3反復の帯行列演算 | 小〜中 |

## 2. サイドチェイン解析（Stage 2-SC）

### 演算順序（3経路で結果を一致させる）

AVX2・NEON・スカラーの3経路と NumPy 参照が同じ結果になるよう、積和の融合（FMA、`vmlaq_f32` / `_mm256_fmadd_ps`）は使わず、乗算と加算を別命令で行う。コンパイラ側も `-ffp-contract=off`（MSVC は `/fp:precise`）とする。下の NEON コード例は構造の説明用で、実装（`plugin/Source/dsp/CytomicSvf.h`）は `vmulq_f32` と `vaddq_f32` を分けて書いている。

### データレイアウト：SoA必須

```cpp
// 推奨：Structure of Arrays
alignas(32) float ic1eq[32];
alignas(32) float ic2eq[32];
alignas(32) float a1[32];
alignas(32) float a2[32];
alignas(32) float a3[32];
alignas(32) float m1[32];   // Bandpass: m1 = k, m0 = m2 = 0
```

AoS（構造体配列）は避ける。ロード/ストア効率とSIMD化のしやすさが大きく異なる。

### NEON 更新コア（4バンド並列）

```cpp
float32x4_t v0 = vdupq_n_f32(input);
float32x4_t v3 = vsubq_f32(v0, ic2eq);
float32x4_t v1 = vmlaq_f32(vmulq_f32(a1, ic1eq), a2, v3);
float32x4_t v2 = vmlaq_f32(vmlaq_f32(ic2eq, a2, ic1eq), a3, v3);

ic1eq = vsubq_f32(vaddq_f32(v1, v1), ic1eq);
ic2eq = vsubq_f32(vaddq_f32(v2, v2), ic2eq);

// Bandpass出力
float32x4_t y = vmulq_f32(m1, v1);
```

- 32バンドの場合、NEONでは8回、AVX2では4回のループで完結する。
- 32は4/8の倍数なので端数処理は不要。

### AVX2 対応

`__m256` を使用して8バンド同時処理。演算内容はNEONと同一。

## 3. 信号パス直列EQ（Stage 6A）

直列接続のためバンド間に依存関係がある。以下の2方式を検討する。

### 方法A：L/RをSIMDレーンで同時処理（推奨）

仕様書の記述「L/R 2チャンネルをSIMDレーンで同時演算」に合致する。

- `float32x2_t` または `float32x4_t` の下位2レーンを使用。
- 1バンド処理するごとにLとRを同時更新。
- 実装が単純でデバッグしやすい。
- 最初の実装として最適。

### 方法B：複数バンドの並列化（高度）

直列EQでは、バンド b の入力は同じサンプルのバンド b-1 の出力である。したがって同一サンプル内でバンドをそのまま並列に計算することはできない。  
並列化するなら、段ごとに1サンプルずつ時刻をずらして複数バンドを同時に進める「斜め送り（パイプライン化）」になる。出力は変わらないが、ホップ境界の係数補間・状態リセット・フェイルセーフとの整合が複雑になるため、方法AでCPU予算を満たせない場合のみ検討する。

## 4. 特徴量抽出（Stage 3）

### パワー合成とEMA

```cpp
// ステレオパワー: 0.5 * (L² + R²)
float32x4_t p = vmulq_n_f32(
    vaddq_f32(vmulq_f32(yL, yL), vmulq_f32(yR, yR)), 0.5f);

// EMA更新
float32x4_t one_minus_alpha = vsubq_f32(vdupq_n_f32(1.0f), alpha);
Ps = vmlaq_f32(vmulq_f32(one_minus_alpha, Ps), alpha, p);
```

### 対数計算

`10 * log10` は `fast_log10` をSIMD化する。  
レンジ削減 + 多項式近似が一般的。仕様の最大絶対誤差 `< 1e-5` を満たすこと。

## 5. NN推論（Stage 4）

| 層 | SIMD方針 | 備考 |
|----|----------|------|
| Conv1d (k=5) | 入力展開後にGEMM的処理、またはチャネル方向並列 | padding=2, zeros |
| Linear (GEMV) | 行方向に並列。出力を4/8ずつ計算 | Row-Major重みを活用 |
| LayerNorm | 平均・分散をSIMD還元後、要素ごと適用 | eps=1e-5 |
| GRU | ゲート（r, z, n）ごとにGEMV | 隠れ状態96はキャッシュに乗りやすい |
| Sigmoid / SiLU | 要素ごと。Padé近似をSIMD化 | |

### 重要な実装ポイント

- 重みはRow-Majorかつ64-byte整列済み。行ベクトル内積が自然にSIMD化できる。
- 転置は不要。
- 隠れ状態サイズ（96 float）はキャッシュ効率が良い。

## 6. 実装時の注意点

### デノーマル対策
`processBlock` 先頭で `juce::ScopedNoDenormals` を必ず適用する。  
SIMD演算でもFTZ/DAZを有効にすること。

### アライメント
すべての作業配列を `alignas(32)` 以上で確保する。  
ロード/ストア命令の効率が変わる。

### プロファイリング
早期に以下を個別計測する：
- サイドチェイン更新
- 直列EQ
- GRUのGEMV

ツール例：
- macOS: Instruments (Time Profiler)
- Windows: Intel VTune / Windows Performance Analyzer

### 精度要件
`fast_log10` / `fast_exp2` / `fast_tanh` / `fast_sigmoid` は、  
動作ドメイン内で標準関数に対する最大絶対誤差 `< 1e-5` を満たすこと。  
単体テストを必須とする。

## 7. 推奨実装順序

1. サイドチェインのSoAレイアウト + NEON/AVX2更新コア
2. 信号パスのL/R同時処理（方法A）
3. 特徴量抽出のパワー合成とEMA
4. NNのLinear/GEMV
5. GRUおよびLayerNorm
6. 必要に応じてProjected Gauss-SeidelのSIMD化

この順序で進めることで、Step 2のCPU予算（Apple M4系、48 kHz / 16 samples / Stereoで中央値 ≤ 10%、P99 ≤ 15%）に到達しやすくなる。

## 8. 補足

- 32バンドは4および8の倍数であるため、SIMD幅による端数処理は原則不要。
- 動的スキップ（バンドごとの演算省略）は仕様で廃止されている。クリックノイズと処理時間のばらつきの原因になるため、「ほぼ 0 dB のバンドだけ軽くする」といった条件付きの省略も行わない。CPU が足りない場合は、全バンド常時稼働のまま SIMD 化を進める。
- 最初は可読性を優先し、プロファイリング結果を見てホットスポットのみを深掘りする。

---

*このドキュメントはZERO-FB SPEC v1.6に基づく実装指針である。*