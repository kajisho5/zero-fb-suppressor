// ZERO-FB - SIMD kernels for the neural engine (SPEC 3.1, 5.2).
//
// One thin vector type (AVX2: 8 lanes, NEON: 4 lanes, scalar: 1 lane) and the
// kernels written once on top of it: row-major GEMV, Conv1d along the band
// axis, LayerNorm and the element-wise activations.
//
// Dot products use a fused multiply-add where the CPU has one. This is the one
// place in the project where FMA is allowed: summation order already differs
// between SIMD widths, so these kernels are compared with the reference at a
// tolerance, never bit for bit. The activations use plain multiply / add /
// divide and are bit-identical to the scalar functions in dsp/FastMath.h.
#pragma once

#include "../dsp/CytomicSvf.h" // SIMD selection macros
#include "../dsp/FastMath.h"

#include <cmath>

namespace zerofb {
namespace simd {

#if defined(ZEROFB_SIMD_AVX2)

struct Vec
{
    __m256 v;
    static constexpr int width = 8;
    static Vec load(const float* p) noexcept { return { _mm256_loadu_ps(p) }; }
    static Vec set1(float x) noexcept { return { _mm256_set1_ps(x) }; }
    static Vec zero() noexcept { return { _mm256_setzero_ps() }; }
    void store(float* p) const noexcept { _mm256_storeu_ps(p, v); }
    float sum() const noexcept
    {
        const __m128 lo = _mm256_castps256_ps128(v), hi = _mm256_extractf128_ps(v, 1);
        __m128 s = _mm_add_ps(lo, hi);
        s = _mm_add_ps(s, _mm_movehl_ps(s, s));
        s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 1));
        return _mm_cvtss_f32(s);
    }
};
inline Vec operator+(Vec a, Vec b) noexcept { return { _mm256_add_ps(a.v, b.v) }; }
inline Vec operator-(Vec a, Vec b) noexcept { return { _mm256_sub_ps(a.v, b.v) }; }
inline Vec operator*(Vec a, Vec b) noexcept { return { _mm256_mul_ps(a.v, b.v) }; }
inline Vec operator/(Vec a, Vec b) noexcept { return { _mm256_div_ps(a.v, b.v) }; }
inline Vec vmin(Vec a, Vec b) noexcept { return { _mm256_min_ps(a.v, b.v) }; }
inline Vec vmax(Vec a, Vec b) noexcept { return { _mm256_max_ps(a.v, b.v) }; }
/** a * b + c */
inline Vec mulAdd(Vec a, Vec b, Vec c) noexcept
{
 #if defined(__FMA__) || defined(_MSC_VER)
    return { _mm256_fmadd_ps(a.v, b.v, c.v) };
 #else
    return { _mm256_add_ps(_mm256_mul_ps(a.v, b.v), c.v) };
 #endif
}

#elif defined(ZEROFB_SIMD_NEON)

struct Vec
{
    float32x4_t v;
    static constexpr int width = 4;
    static Vec load(const float* p) noexcept { return { vld1q_f32(p) }; }
    static Vec set1(float x) noexcept { return { vdupq_n_f32(x) }; }
    static Vec zero() noexcept { return { vdupq_n_f32(0.0f) }; }
    void store(float* p) const noexcept { vst1q_f32(p, v); }
    float sum() const noexcept { return vaddvq_f32(v); }
};
inline Vec operator+(Vec a, Vec b) noexcept { return { vaddq_f32(a.v, b.v) }; }
inline Vec operator-(Vec a, Vec b) noexcept { return { vsubq_f32(a.v, b.v) }; }
inline Vec operator*(Vec a, Vec b) noexcept { return { vmulq_f32(a.v, b.v) }; }
inline Vec operator/(Vec a, Vec b) noexcept { return { vdivq_f32(a.v, b.v) }; }
inline Vec vmin(Vec a, Vec b) noexcept { return { vminq_f32(a.v, b.v) }; }
inline Vec vmax(Vec a, Vec b) noexcept { return { vmaxq_f32(a.v, b.v) }; }
inline Vec mulAdd(Vec a, Vec b, Vec c) noexcept { return { vfmaq_f32(c.v, a.v, b.v) }; }

#else

struct Vec
{
    float v;
    static constexpr int width = 1;
    static Vec load(const float* p) noexcept { return { *p }; }
    static Vec set1(float x) noexcept { return { x }; }
    static Vec zero() noexcept { return { 0.0f }; }
    void store(float* p) const noexcept { *p = v; }
    float sum() const noexcept { return v; }
};
inline Vec operator+(Vec a, Vec b) noexcept { return { a.v + b.v }; }
inline Vec operator-(Vec a, Vec b) noexcept { return { a.v - b.v }; }
inline Vec operator*(Vec a, Vec b) noexcept { return { a.v * b.v }; }
inline Vec operator/(Vec a, Vec b) noexcept { return { a.v / b.v }; }
inline Vec vmin(Vec a, Vec b) noexcept { return { a.v < b.v ? a.v : b.v }; }
inline Vec vmax(Vec a, Vec b) noexcept { return { a.v > b.v ? a.v : b.v }; }
inline Vec mulAdd(Vec a, Vec b, Vec c) noexcept { return { a.v * b.v + c.v }; }

#endif

// ---------------------------------------------------------------------------
// Activations. Same operations, in the same order, as fast_tanh / fast_sigmoid.
// ---------------------------------------------------------------------------

inline Vec fastTanh(Vec x) noexcept
{
    x = vmin(Vec::set1(9.0f), vmax(Vec::set1(-9.0f), x));
    const Vec a = Vec::set1(0.25f) * x;
    const Vec a2 = a * a;
    const Vec num = a * (Vec::set1(135135.0f) + a2 * (Vec::set1(17325.0f) + a2 * (Vec::set1(378.0f) + a2)));
    const Vec den = Vec::set1(135135.0f) + a2 * (Vec::set1(62370.0f) + a2 * (Vec::set1(3150.0f) + a2 * Vec::set1(28.0f)));
    const Vec one = Vec::set1(1.0f);
    Vec t = num / den;
    t = (t + t) / (one + t * t);
    t = (t + t) / (one + t * t);
    return t;
}

inline Vec fastSigmoid(Vec x) noexcept
{
    return Vec::set1(0.5f) + Vec::set1(0.5f) * fastTanh(Vec::set1(0.5f) * x);
}

/** In-place element-wise activations. n must be a multiple of Vec::width. */
inline void sigmoidInPlace(float* x, int n) noexcept
{
#if defined(ZEROFB_EXACT_MATH)
    for (int i = 0; i < n; ++i) x[i] = math::sigmoid(x[i]);
#else
    for (int i = 0; i < n; i += Vec::width) fastSigmoid(Vec::load(x + i)).store(x + i);
#endif
}

inline void tanhInPlace(float* x, int n) noexcept
{
#if defined(ZEROFB_EXACT_MATH)
    for (int i = 0; i < n; ++i) x[i] = math::tanh(x[i]);
#else
    for (int i = 0; i < n; i += Vec::width) fastTanh(Vec::load(x + i)).store(x + i);
#endif
}

/** SiLU: x * sigmoid(x). */
inline void siluInPlace(float* x, int n) noexcept
{
#if defined(ZEROFB_EXACT_MATH)
    for (int i = 0; i < n; ++i) x[i] = x[i] * math::sigmoid(x[i]);
#else
    for (int i = 0; i < n; i += Vec::width)
    {
        const Vec v = Vec::load(x + i);
        (v * fastSigmoid(v)).store(x + i);
    }
#endif
}

// ---------------------------------------------------------------------------
// Linear algebra
// ---------------------------------------------------------------------------

/** y[r] = bias[r] + dot(W[r, :], x). W is row-major (rows x cols); rows must
    be a multiple of 4 and cols a multiple of Vec::width. */
inline void gemv(const float* W, int rows, int cols, const float* x, const float* bias, float* y) noexcept
{
    for (int r = 0; r < rows; r += 4)
    {
        const float* w0 = W + size_t(r) * size_t(cols);
        const float* w1 = w0 + cols;
        const float* w2 = w1 + cols;
        const float* w3 = w2 + cols;
        Vec a0 = Vec::zero(), a1 = Vec::zero(), a2 = Vec::zero(), a3 = Vec::zero();
        for (int c = 0; c < cols; c += Vec::width)
        {
            const Vec xv = Vec::load(x + c);
            a0 = mulAdd(Vec::load(w0 + c), xv, a0);
            a1 = mulAdd(Vec::load(w1 + c), xv, a1);
            a2 = mulAdd(Vec::load(w2 + c), xv, a2);
            a3 = mulAdd(Vec::load(w3 + c), xv, a3);
        }
        y[r] = bias[r] + a0.sum();
        y[r + 1] = bias[r + 1] + a1.sum();
        y[r + 2] = bias[r + 2] + a2.sum();
        y[r + 3] = bias[r + 3] + a3.sum();
    }
}

/** Conv1d along the band axis, kernel 5, zero padding 2 (PyTorch Conv1d).
    weight: (outCh, inCh, 5) row-major. padded: inCh rows of (bands + 4)
    values with two zeros on each side. out: (outCh, bands) row-major.
    bands must be a multiple of Vec::width. */
inline void conv1dK5(const float* weight, const float* bias, int outCh, int inCh, int bands,
                     const float* padded, float* out) noexcept
{
    const int stride = bands + 4;
    for (int o = 0; o < outCh; ++o)
    {
        float* dst = out + size_t(o) * size_t(bands);
        for (int b = 0; b < bands; b += Vec::width)
        {
            Vec acc = Vec::set1(bias[o]);
            const float* w = weight + size_t(o) * size_t(inCh) * 5;
            for (int c = 0; c < inCh; ++c)
            {
                const float* src = padded + size_t(c) * size_t(stride) + b;
                for (int k = 0; k < 5; ++k)
                    acc = mulAdd(Vec::set1(w[c * 5 + k]), Vec::load(src + k), acc);
            }
            acc.store(dst + b);
        }
    }
}

/** LayerNorm over n values (biased variance), in place. n multiple of Vec::width. */
inline void layerNormInPlace(float* x, int n, const float* gamma, const float* beta, float eps) noexcept
{
    Vec s = Vec::zero();
    for (int i = 0; i < n; i += Vec::width)
        s = s + Vec::load(x + i);
    const float mean = s.sum() / float(n);
    const Vec meanV = Vec::set1(mean);
    Vec q = Vec::zero();
    for (int i = 0; i < n; i += Vec::width)
    {
        const Vec d = Vec::load(x + i) - meanV;
        q = mulAdd(d, d, q);
    }
    const float var = q.sum() / float(n);
    const Vec scale = Vec::set1(1.0f / std::sqrt(var + eps));
    for (int i = 0; i < n; i += Vec::width)
        mulAdd((Vec::load(x + i) - meanV) * scale, Vec::load(gamma + i), Vec::load(beta + i)).store(x + i);
}

} // namespace simd
} // namespace zerofb
