// ZERO-FB - Stage 4 inference engine (SPEC 3.1, 3.2).
//
// Conv1d(6 -> 16, k = 5) + SiLU, Linear(512 -> 96) + LayerNorm + SiLU,
// two unidirectional GRU layers (96), two sigmoid heads (32 + 32).
// No allocation, no locks: every buffer is a member, weights are read only.
#pragma once

#include "../dsp/ZeroFbEngine.h"
#include "SimdKernels.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace zerofb {

/** Layout of the weight payload (floats, in file order after the 64-byte header). */
namespace model {

constexpr int kConvChannels = 16;
constexpr int kConvKernel = 5;
constexpr int kConvOut = kConvChannels * kNumBands; // 512
constexpr int kHidden = 96;
constexpr int kGates = 3 * kHidden;                 // 288, order [r, z, n]
constexpr float kLayerNormEps = 1.0e-5f;

constexpr int kConvWeight = 0;
constexpr int kConvBias = kConvWeight + kConvChannels * kNumFeatures * kConvKernel;
constexpr int kProjWeight = kConvBias + kConvChannels;
constexpr int kProjBias = kProjWeight + kHidden * kConvOut;
constexpr int kLnGamma = kProjBias + kHidden;
constexpr int kLnBeta = kLnGamma + kHidden;
constexpr int kGru1Wih = kLnBeta + kHidden;
constexpr int kGru1Whh = kGru1Wih + kGates * kHidden;
constexpr int kGru1Bih = kGru1Whh + kGates * kHidden;
constexpr int kGru1Bhh = kGru1Bih + kGates;
constexpr int kGru2Wih = kGru1Bhh + kGates;
constexpr int kGru2Whh = kGru2Wih + kGates * kHidden;
constexpr int kGru2Bih = kGru2Whh + kGates * kHidden;
constexpr int kGru2Bhh = kGru2Bih + kGates;
constexpr int kHeadFbWeight = kGru2Bhh + kGates;
constexpr int kHeadFbBias = kHeadFbWeight + kNumBands * kHidden;
constexpr int kHeadRevWeight = kHeadFbBias + kNumBands;
constexpr int kHeadRevBias = kHeadRevWeight + kNumBands * kHidden;
constexpr int kTotalParams = kHeadRevBias + kNumBands;

static_assert(kTotalParams == 167888, "parameter count must match SPEC 3.1");

// .swmodel container
constexpr std::size_t kHeaderBytes = 64;
constexpr std::size_t kFileBytes = kHeaderBytes + std::size_t(kTotalParams) * 4; // 671,616
static_assert(kFileBytes == 671616, "file size must match SPEC 3.2");
static_assert(kHeaderBytes + std::size_t(kHeadRevBias) * 4 == 0x0A3F00, "last tensor offset must match SPEC 3.2");

enum class FileStatus { ok, wrongSize, wrongMagic, wrongHeader, wrongCrc };

/** IEEE 802.3 CRC32 (reflected, polynomial 0xEDB88320). */
inline std::uint32_t crc32(const unsigned char* data, std::size_t size) noexcept
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

inline std::uint32_t readU32(const unsigned char* p) noexcept
{
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

/** Checks size, magic, header fields and CRC32 of a .swmodel image. */
inline FileStatus checkFile(const void* file, std::size_t size) noexcept
{
    if (size != kFileBytes)
        return FileStatus::wrongSize;
    const auto* bytes = static_cast<const unsigned char*>(file);
    if (std::memcmp(bytes, "SWZFB004", 8) != 0)
        return FileStatus::wrongMagic;
    const std::uint32_t expected[6] = { 48000u, std::uint32_t(kNumBands), std::uint32_t(kNumFeatures),
                                        16u, std::uint32_t(kHidden), std::uint32_t(kTotalParams) };
    for (int i = 0; i < 6; ++i)
        if (readU32(bytes + 8 + 4 * i) != expected[i])
            return FileStatus::wrongHeader;
    if (crc32(bytes + kHeaderBytes, kFileBytes - kHeaderBytes) != readU32(bytes + 0x20))
        return FileStatus::wrongCrc;
    return FileStatus::ok;
}

} // namespace model

class CausalGruEngine final : public MaskEstimator
{
public:
    CausalGruEngine() noexcept { reset(); }
    /** payload: model::kTotalParams floats laid out as in SPEC 3.2. The memory
        must outlive the engine; it is never written. */
    explicit CausalGruEngine(const float* payload) noexcept : weights(payload) { reset(); }

    void setWeights(const float* payload) noexcept { weights = payload; reset(); }
    bool hasWeights() const noexcept { return weights != nullptr; }

    void reset() noexcept override
    {
        for (int i = 0; i < model::kHidden; ++i)
            h1[i] = h2[i] = 0.0f;
    }

    void infer(const float* features, float* masks) noexcept override
    {
        using namespace model;
        if (weights == nullptr)
        {
            for (int i = 0; i < kMaskSize; ++i)
                masks[i] = 1.0f; // no model: pass the signal unchanged
            return;
        }
        const float* w = weights;

        // Layer 1: Conv1d + SiLU. Input rows are copied into a zero-padded buffer.
        constexpr int stride = kNumBands + 4;
        for (int c = 0; c < kNumFeatures; ++c)
            std::memcpy(padded + c * stride + 2, features + c * kNumBands, sizeof(float) * size_t(kNumBands));
        simd::conv1dK5(w + kConvWeight, w + kConvBias, kConvChannels, kNumFeatures, kNumBands, padded, conv);
        simd::siluInPlace(conv, kConvOut);

        // Layer 2: Linear + LayerNorm + SiLU (the (16, 32) block is already C-order).
        simd::gemv(w + kProjWeight, kHidden, kConvOut, conv, w + kProjBias, proj);
        simd::layerNormInPlace(proj, kHidden, w + kLnGamma, w + kLnBeta, kLayerNormEps);
        simd::siluInPlace(proj, kHidden);

        // Layers 3 and 4: GRU.
        gruStep(w + kGru1Wih, w + kGru1Whh, w + kGru1Bih, w + kGru1Bhh, proj, h1);
        gruStep(w + kGru2Wih, w + kGru2Whh, w + kGru2Bih, w + kGru2Bhh, h1, h2);

        // Heads A and B.
        simd::gemv(w + kHeadFbWeight, kNumBands, kHidden, h2, w + kHeadFbBias, masks);
        simd::gemv(w + kHeadRevWeight, kNumBands, kHidden, h2, w + kHeadRevBias, masks + kNumBands);
        simd::sigmoidInPlace(masks, kMaskSize);
    }

    // Layer outputs of the last frame (tests).
    const float* convOutput() const noexcept { return conv; }
    const float* projOutput() const noexcept { return proj; }
    const float* hidden1() const noexcept { return h1; }
    const float* hidden2() const noexcept { return h2; }

private:
    /** PyTorch GRU cell, gate order [r, z, n]. */
    void gruStep(const float* wIh, const float* wHh, const float* bIh, const float* bHh,
                 const float* x, float* h) noexcept
    {
        using namespace model;
        using simd::Vec;
        simd::gemv(wIh, kGates, kHidden, x, bIh, gi);
        simd::gemv(wHh, kGates, kHidden, h, bHh, gh);

        // r and z share one buffer of 192 values.
        for (int i = 0; i < 2 * kHidden; i += Vec::width)
            (Vec::load(gi + i) + Vec::load(gh + i)).store(rz + i);
        simd::sigmoidInPlace(rz, 2 * kHidden);

        for (int i = 0; i < kHidden; i += Vec::width)
            (Vec::load(gi + 2 * kHidden + i) + Vec::load(rz + i) * Vec::load(gh + 2 * kHidden + i)).store(nn + i);
        simd::tanhInPlace(nn, kHidden);

        const Vec one = Vec::set1(1.0f);
        for (int i = 0; i < kHidden; i += Vec::width)
        {
            const Vec z = Vec::load(rz + kHidden + i);
            ((one - z) * Vec::load(nn + i) + z * Vec::load(h + i)).store(h + i);
        }
    }

    const float* weights = nullptr;
    alignas(64) float padded[kNumFeatures * (kNumBands + 4)] {};
    alignas(64) float conv[model::kConvOut] {};
    alignas(64) float proj[model::kHidden] {};
    alignas(64) float gi[model::kGates] {}, gh[model::kGates] {};
    alignas(64) float rz[2 * model::kHidden] {}, nn[model::kHidden] {};
    alignas(64) float h1[model::kHidden] {}, h2[model::kHidden] {};
};

} // namespace zerofb
