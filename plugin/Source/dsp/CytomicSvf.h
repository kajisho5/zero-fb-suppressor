// ZERO-FB - Cytomic (trapezoidal, zero-delay-feedback) state variable filter.
// SPEC 2.3. Direct Form I/II is not used anywhere in this project.
#pragma once

#include "BandLayout.h"

#include <complex>

#if !defined(ZEROFB_FORCE_SCALAR)
 #if defined(__AVX2__)
  #include <immintrin.h>
  #define ZEROFB_SIMD_AVX2 1
 #elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
  #include <arm_neon.h>
  #define ZEROFB_SIMD_NEON 1
 #endif
#endif

namespace zerofb {

/** Coefficients used by the per-sample update. */
template <typename Real>
struct SvfCoeffsT
{
    Real a1 {}, a2 {}, a3 {}, m0 {}, m1 {}, m2 {};
};

using SvfCoeffs = SvfCoeffsT<float>;

/** Design-time values, including g and k for the analytic response. */
template <typename Real>
struct SvfDesignT
{
    Real g {}, k {};
    SvfCoeffsT<Real> c;
};

namespace svf {

template <typename Real>
inline SvfDesignT<Real> finish(Real g, Real k, Real m0, Real m1, Real m2) noexcept
{
    SvfDesignT<Real> d;
    d.g = g;
    d.k = k;
    d.c.a1 = Real(1) / (Real(1) + g * (g + k));
    d.c.a2 = g * d.c.a1;
    d.c.a3 = g * d.c.a2;
    d.c.m0 = m0;
    d.c.m1 = m1;
    d.c.m2 = m2;
    return d;
}

/** Band-pass with 0 dB peak. */
template <typename Real>
inline SvfDesignT<Real> bandpass(Real gBase, Real q) noexcept
{
    const Real k = Real(1) / q;
    return finish<Real>(gBase, k, Real(0), k, Real(0));
}

/** A = 10^(gainDb / 40), sqrtA = sqrt(A). */
template <typename Real>
inline SvfDesignT<Real> lowShelf(Real gBase, Real A, Real sqrtA, Real q) noexcept
{
    const Real k = Real(1) / q;
    return finish<Real>(gBase / sqrtA, k, Real(1), k * (A - Real(1)), A * A - Real(1));
}

template <typename Real>
inline SvfDesignT<Real> peak(Real gBase, Real A, Real q) noexcept
{
    const Real k = Real(1) / (q * A);
    return finish<Real>(gBase, k, Real(1), k * (A * A - Real(1)), Real(0));
}

template <typename Real>
inline SvfDesignT<Real> highShelf(Real gBase, Real A, Real sqrtA, Real q) noexcept
{
    const Real k = Real(1) / q;
    return finish<Real>(gBase * sqrtA, k, A * A, k * (Real(1) - A) * A, Real(1) - A * A);
}

/** Complex response of a section at frequency f. Design time only. */
inline std::complex<double> response(const SvfDesignT<double>& d, double f, double fs) noexcept
{
    const std::complex<double> s(0.0, std::tan(kPi * f / fs) / d.g);
    const std::complex<double> den = s * s + d.k * s + 1.0;
    return d.c.m0 + d.c.m1 * s / den + d.c.m2 / den;
}

} // namespace svf

/** One SVF section, one channel. */
struct SvfState
{
    float ic1 = 0.0f, ic2 = 0.0f;

    inline float tick(const SvfCoeffs& c, float v0) noexcept
    {
        const float v3 = v0 - ic2;
        const float v1 = c.a1 * ic1 + c.a2 * v3;
        const float v2 = (ic2 + c.a2 * ic1) + c.a3 * v3;
        ic1 = (v1 + v1) - ic1;
        ic2 = (v2 + v2) - ic2;
        return (c.m0 * v0 + c.m1 * v1) + c.m2 * v2;
    }
};

/**
    Stage 2-SC: 32 band-pass sections per channel in structure-of-arrays form,
    fused with the band power envelope (SPEC 2.4). All bands update on every
    sample; nothing is skipped.

    The AVX2, NEON and scalar paths perform the same IEEE operations in the
    same order (no fused multiply-add), so they produce identical results.
*/
struct SidechainBank
{
    alignas(32) float a1[kNumBands] {}, a2[kNumBands] {}, a3[kNumBands] {}, m1[kNumBands] {};
    alignas(32) float aEnv[kNumBands] {}, omaEnv[kNumBands] {};
    alignas(32) float ic1[kMaxChannels][kNumBands] {}, ic2[kMaxChannels][kNumBands] {};
    alignas(32) float ps[kNumBands] {};

    void clearState() noexcept
    {
        for (int c = 0; c < kMaxChannels; ++c)
            for (int b = 0; b < kNumBands; ++b)
                ic1[c][b] = ic2[c][b] = 0.0f;
        for (int b = 0; b < kNumBands; ++b)
            ps[b] = 0.0f;
    }

    inline void tick(float xL, float xR, bool stereo) noexcept
    {
#if defined(ZEROFB_SIMD_AVX2)
        const __m256 half = _mm256_set1_ps(0.5f);
        const __m256 inL = _mm256_set1_ps(xL), inR = _mm256_set1_ps(xR);
        for (int b = 0; b < kNumBands; b += 8)
        {
            const __m256 A1 = _mm256_load_ps(a1 + b), A2 = _mm256_load_ps(a2 + b);
            const __m256 A3 = _mm256_load_ps(a3 + b), M1 = _mm256_load_ps(m1 + b);
            auto section = [&](int ch, __m256 v0) noexcept
            {
                const __m256 s1 = _mm256_load_ps(ic1[ch] + b), s2 = _mm256_load_ps(ic2[ch] + b);
                const __m256 v3 = _mm256_sub_ps(v0, s2);
                const __m256 v1 = _mm256_add_ps(_mm256_mul_ps(A1, s1), _mm256_mul_ps(A2, v3));
                const __m256 v2 = _mm256_add_ps(_mm256_add_ps(s2, _mm256_mul_ps(A2, s1)), _mm256_mul_ps(A3, v3));
                _mm256_store_ps(ic1[ch] + b, _mm256_sub_ps(_mm256_add_ps(v1, v1), s1));
                _mm256_store_ps(ic2[ch] + b, _mm256_sub_ps(_mm256_add_ps(v2, v2), s2));
                return _mm256_mul_ps(M1, v1);
            };
            const __m256 yL = section(0, inL);
            __m256 p = _mm256_mul_ps(yL, yL);
            if (stereo)
            {
                const __m256 yR = section(1, inR);
                p = _mm256_mul_ps(half, _mm256_add_ps(p, _mm256_mul_ps(yR, yR)));
            }
            const __m256 env = _mm256_add_ps(_mm256_mul_ps(_mm256_load_ps(omaEnv + b), _mm256_load_ps(ps + b)),
                                             _mm256_mul_ps(_mm256_load_ps(aEnv + b), p));
            _mm256_store_ps(ps + b, env);
        }
#elif defined(ZEROFB_SIMD_NEON)
        const float32x4_t half = vdupq_n_f32(0.5f);
        const float32x4_t inL = vdupq_n_f32(xL), inR = vdupq_n_f32(xR);
        for (int b = 0; b < kNumBands; b += 4)
        {
            const float32x4_t A1 = vld1q_f32(a1 + b), A2 = vld1q_f32(a2 + b);
            const float32x4_t A3 = vld1q_f32(a3 + b), M1 = vld1q_f32(m1 + b);
            auto section = [&](int ch, float32x4_t v0) noexcept
            {
                const float32x4_t s1 = vld1q_f32(ic1[ch] + b), s2 = vld1q_f32(ic2[ch] + b);
                const float32x4_t v3 = vsubq_f32(v0, s2);
                const float32x4_t v1 = vaddq_f32(vmulq_f32(A1, s1), vmulq_f32(A2, v3));
                const float32x4_t v2 = vaddq_f32(vaddq_f32(s2, vmulq_f32(A2, s1)), vmulq_f32(A3, v3));
                vst1q_f32(ic1[ch] + b, vsubq_f32(vaddq_f32(v1, v1), s1));
                vst1q_f32(ic2[ch] + b, vsubq_f32(vaddq_f32(v2, v2), s2));
                return vmulq_f32(M1, v1);
            };
            const float32x4_t yL = section(0, inL);
            float32x4_t p = vmulq_f32(yL, yL);
            if (stereo)
            {
                const float32x4_t yR = section(1, inR);
                p = vmulq_f32(half, vaddq_f32(p, vmulq_f32(yR, yR)));
            }
            const float32x4_t env = vaddq_f32(vmulq_f32(vld1q_f32(omaEnv + b), vld1q_f32(ps + b)),
                                              vmulq_f32(vld1q_f32(aEnv + b), p));
            vst1q_f32(ps + b, env);
        }
#else
        for (int b = 0; b < kNumBands; ++b)
        {
            auto section = [&](int ch, float v0) noexcept
            {
                const float s1 = ic1[ch][b], s2 = ic2[ch][b];
                const float v3 = v0 - s2;
                const float v1 = a1[b] * s1 + a2[b] * v3;
                const float v2 = (s2 + a2[b] * s1) + a3[b] * v3;
                ic1[ch][b] = (v1 + v1) - s1;
                ic2[ch][b] = (v2 + v2) - s2;
                return m1[b] * v1;
            };
            const float yL = section(0, xL);
            float p = yL * yL;
            if (stereo)
            {
                const float yR = section(1, xR);
                p = 0.5f * (p + yR * yR);
            }
            ps[b] = omaEnv[b] * ps[b] + aEnv[b] * p;
        }
#endif
    }
};

} // namespace zerofb
