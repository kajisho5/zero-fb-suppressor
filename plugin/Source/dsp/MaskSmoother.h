// ZERO-FB - Stage 5A: two-head combination and the per-band hysteresis FSM.
// SPEC 2.5 (normative pseudo code). All times are counted in control frames.
#pragma once

#include "BandLayout.h"
#include "FastMath.h"

#include <algorithm>
#include <cstdint>

namespace zerofb {

class MaskSmoother
{
public:
    enum class State : std::uint8_t { normalRelease, slowRelease, hold };

    struct Inputs
    {
        const float* mFb = nullptr;    // Head A, 32 values in (0, 1)
        const float* mRev = nullptr;   // Head B, 32 values in (0, 1)
        const float* deltaE = nullptr; // dB per frame
        const float* mdRaw = nullptr;  // dB
        const float* tRaw = nullptr;   // dB
        float suppress = 0.75f;        // S in [0, 1]
        float dereverb = 0.5f;         // D in [0, 1]
        float floorDb = -24.0f;
        bool vocal = false;
    };

    void prepare(double sampleRate, int hop) noexcept
    {
        auto frames = [&](double sec) { return int(std::lround(sec * sampleRate / double(hop))); };
        nHold[0] = frames(1.5);
        nHold[1] = frames(4.0);
        nHold[2] = frames(12.0);
        nReoffWin = frames(2.0);
        nVoiceOverride = frames(0.080);
        nResetRamp = frames(0.005);
        aAttack = float(alphaH(0.0015, hop, sampleRate));
        aVoiceOverride = float(alphaH(0.008, hop, sampleRate));
        aSlowRelease = float(alphaH(0.800, hop, sampleRate));
        aReleaseSpeech = float(alphaH(0.035, hop, sampleRate));
        aReleaseVocal = float(alphaH(0.060, hop, sampleRate));
        reset();
    }

    /** Power-on state: unity gain, no hold. */
    void reset() noexcept
    {
        clearTimers();
        resetRampRem = 0;
        for (int b = 0; b < kNumBands; ++b)
            mCurr[b] = 1.0f;
    }

    /** RESET parameter (step 1 of the pseudo code): timers cleared, then a
        5 ms ramp back to unity gain. */
    void triggerReset() noexcept
    {
        clearTimers();
        resetRampRem = nResetRamp;
    }

    /** Fail-safe (SPEC 2.7): every band restarts from the floor. */
    void forceFloor(float floorDb) noexcept
    {
        clearTimers();
        resetRampRem = 0;
        const float fLin = std::pow(10.0f, floorDb / 20.0f);
        for (int b = 0; b < kNumBands; ++b)
            mCurr[b] = fLin;
    }

    void update(const Inputs& in) noexcept
    {
        // 2. RESET ramp: forced return to 1.0 in 5 ms.
        if (resetRampRem > 0)
        {
            const float step = 1.0f / float(resetRampRem);
            --resetRampRem;
            for (int b = 0; b < kNumBands; ++b)
                mCurr[b] = resetRampRem == 0 ? 1.0f : mCurr[b] + (1.0f - mCurr[b]) * step;
            return;
        }

        const float fLin = std::pow(10.0f, in.floorDb / 20.0f);
        const float aRelease = in.vocal ? aReleaseVocal : aReleaseSpeech;

        // 3. Voice-triggered release (raw dB values).
        int voiceHits = 0;
        for (int b = 5; b <= 21; ++b)
            if (in.deltaE[b] > 3.0f && in.mdRaw[b] > 10.0f)
                ++voiceHits;
        if (voiceHits >= 4)
            voiceOverrideRem = nVoiceOverride;

        // 4. Bands currently in a long hold (level >= 1).
        int activeLongHolds = 0;
        for (int b = 0; b < kNumBands; ++b)
            if (holdRem[b] > 0 && backoffLevel[b] >= 1)
                ++activeLongHolds;

        // 5. Per-band update.
        for (int b = 0; b < kNumBands; ++b)
        {
            const float mComb = in.mFb[b] * (1.0f - in.dereverb * (1.0f - in.mRev[b]));
            const float mTarget = std::max(fLin, 1.0f - in.suppress * (1.0f - mComb));
            const bool deepFb = 20.0f * math::log10(std::max(in.mFb[b], 1.0e-5f)) <= -6.0f;

            // 5a. ATTACK: gain reduction always wins.
            if (mTarget < mCurr[b])
            {
                mCurr[b] += aAttack * (mTarget - mCurr[b]);
                if (deepFb)
                {
                    // The back-off level is decided once, when a hold starts.
                    // A band that is already holding keeps its level, so one
                    // continuous feedback event can never escalate itself.
                    if (state[b] != State::hold)
                    {
                        const bool recurrence = (state[b] == State::slowRelease || sinceHoldEnd[b] <= nReoffWin)
                                                && in.tRaw[b] > 6.0f;
                        if (recurrence && backoffLevel[b] == 0 && activeLongHolds < 4)
                        {
                            backoffLevel[b] = 1;
                            ++activeLongHolds;
                        }
                        else if (recurrence && backoffLevel[b] == 1)
                        {
                            backoffLevel[b] = 2;
                        }
                        state[b] = State::hold;
                    }
                    holdRem[b] = nHold[backoffLevel[b]];
                }
                continue;
            }

            // 5b. VOICE_OVERRIDE: only bands whose Head A is back on the safe side.
            if (voiceOverrideRem > 0 && in.mFb[b] > 0.75f)
            {
                holdRem[b] = 0;
                backoffLevel[b] = 0;
                state[b] = State::normalRelease;
                mCurr[b] += aVoiceOverride * (mTarget - mCurr[b]);
                if (std::abs(mTarget - mCurr[b]) < 1.0e-4f)
                    mCurr[b] = mTarget;
                continue;
            }

            // 5c. HOLD -> SLOW_RELEASE -> NORMAL_RELEASE.
            if (state[b] == State::hold)
            {
                if (deepFb)
                    holdRem[b] = std::max(holdRem[b], nHold[0]);
                if (holdRem[b] > 0)
                {
                    --holdRem[b]; // gain rise is frozen
                }
                else
                {
                    state[b] = State::slowRelease;
                    sinceHoldEnd[b] = 0;
                }
            }
            else if (state[b] == State::slowRelease)
            {
                if (sinceHoldEnd[b] <= nReoffWin)
                    ++sinceHoldEnd[b];
                mCurr[b] += aSlowRelease * (mTarget - mCurr[b]);
                if (20.0f * math::log10(std::max(mCurr[b], 1.0e-5f)) >= -1.0f)
                {
                    state[b] = State::normalRelease;
                    backoffLevel[b] = 0;
                }
            }
            else
            {
                if (sinceHoldEnd[b] <= nReoffWin)
                    ++sinceHoldEnd[b];
                mCurr[b] += aRelease * (mTarget - mCurr[b]);
                if (std::abs(mTarget - mCurr[b]) < 1.0e-4f)
                    mCurr[b] = mTarget;
            }
        }

        if (voiceOverrideRem > 0)
            --voiceOverrideRem;
    }

    const float* current() const noexcept { return mCurr; }
    State bandState(int b) const noexcept { return state[b]; }
    int bandBackoffLevel(int b) const noexcept { return backoffLevel[b]; }
    int bandHoldRemaining(int b) const noexcept { return holdRem[b]; }
    bool resetRampActive() const noexcept { return resetRampRem > 0; }
    int holdFrames(int level) const noexcept { return nHold[level]; }
    int reoffendWindowFrames() const noexcept { return nReoffWin; }
    int resetRampFrames() const noexcept { return nResetRamp; }

private:
    void clearTimers() noexcept
    {
        voiceOverrideRem = 0;
        for (int b = 0; b < kNumBands; ++b)
        {
            holdRem[b] = 0;
            backoffLevel[b] = 0;
            sinceHoldEnd[b] = nReoffWin + 1;
            state[b] = State::normalRelease;
        }
    }

    int nHold[3] {}, nReoffWin = 0, nVoiceOverride = 0, nResetRamp = 0;
    float aAttack = 0.0f, aVoiceOverride = 0.0f, aSlowRelease = 0.0f;
    float aReleaseSpeech = 0.0f, aReleaseVocal = 0.0f;

    int resetRampRem = 0, voiceOverrideRem = 0;
    float mCurr[kNumBands] {};
    int holdRem[kNumBands] {}, backoffLevel[kNumBands] {}, sinceHoldEnd[kNumBands] {};
    State state[kNumBands] {};
};

} // namespace zerofb
