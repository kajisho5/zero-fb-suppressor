#!/usr/bin/env python3
"""Generate the golden fixtures for plugin/tests/GoldenParityTest.cpp.

Everything is deterministic (seed 42). The expected values come from
training/erb_dsp.py, which shares no code with the C++ core. It is run twice:

  float64  the mathematical reference
  float32  the same arithmetic the C++ core performs

The float32 run must stay within PY_FP32_TOLERANCE of the float64 run,
otherwise generation fails. That bound is the rounding noise of single
precision itself (weak out-of-band leakage next to a strong tone), so it is the
tightest bound any float implementation can be held to against float64.

The fixtures are about 140 MB, so they are generated at build time instead of
being committed. sha256.txt lists every generated file.

Usage: gen_golden_fixtures.py OUTPUT_DIR
"""

from __future__ import annotations

import hashlib
import sys
from pathlib import Path

import numpy as np

import erb_dsp as E

SEED = 42
DURATION_SEC = 5.0
PY_FP32_TOLERANCE = 1e-3
CASES = [(44100, 2), (48000, 2), (96000, 2), (48000, 1)]


def test_signal(fs: int, rng: np.random.Generator) -> np.ndarray:
    """5 s stereo signal: impulse, chirp, pseudo speech, 2 kHz tone, silence."""
    n = int(round(DURATION_SEC * fs))
    t = np.arange(n) / fs
    left = np.zeros(n)
    right = np.zeros(n)

    left[int(0.1 * fs)] = 0.5                      # impulse
    right[int(0.1 * fs) + 3] = -0.4

    seg = (t >= 0.5) & (t < 1.5)                   # log chirp 50 Hz -> 18 kHz
    ts = t[seg] - 0.5
    k = np.log(18000.0 / 50.0)
    phase = 2.0 * np.pi * 50.0 * (np.exp(k * ts) - 1.0) / k
    left[seg] = 0.25 * np.sin(phase)
    right[seg] = 0.20 * np.sin(phase + 0.7)

    seg = (t >= 1.5) & (t < 3.0)                   # pseudo speech
    ts = t[seg] - 1.5
    syllable = np.clip(np.sin(2.0 * np.pi * 4.0 * ts), 0.0, None) ** 2
    voiced = np.zeros(ts.size)
    for h in range(1, 25):
        f = 120.0 * h
        formant = (np.exp(-((f - 700.0) / 300.0) ** 2)
                   + 0.6 * np.exp(-((f - 1800.0) / 500.0) ** 2) + 0.05)
        voiced += formant * np.sin(2.0 * np.pi * f * ts + h)
    noise = rng.standard_normal(ts.size) * 0.02
    left[seg] = 0.12 * syllable * voiced + noise * syllable
    right[seg] = 0.10 * syllable * voiced + rng.standard_normal(ts.size) * 0.02 * syllable

    seg = (t >= 3.0) & (t < 4.0)                   # 2 kHz tone, opposite polarity on R
    ts = t[seg] - 3.0
    left[seg] = 10.0 ** (-12.0 / 20.0) * np.sin(2.0 * np.pi * 2000.0 * ts)
    right[seg] = -left[seg]
    # 4.0 .. 5.0 s: digital silence

    return np.stack([left, right], axis=1).astype(np.float32)


def coupling_cases(fs: int, rng: np.random.Generator) -> np.ndarray:
    c = E.coupling_matrix(fs)
    rows = [c.reshape(-1)]
    targets = [np.full(E.NUM_BANDS, -12.0), np.zeros(E.NUM_BANDS)]
    single = np.zeros(E.NUM_BANDS)
    single[15] = -12.0
    targets.append(single)
    for _ in range(13):
        targets.append(-rng.uniform(0.0, 30.0, E.NUM_BANDS) * (rng.random(E.NUM_BANDS) < 0.5))
    floors = [-36.0, -24.0, -36.0] + list(rng.uniform(-36.0, -6.0, 13))
    c32 = c.astype(np.float32)
    for tgt, floor in zip(targets, floors):
        tgt = np.maximum(tgt, floor)
        g = E.projected_gauss_seidel(tgt, c32, floor)
        rows.append(np.concatenate([tgt, [floor], g]))
    return np.concatenate(rows)


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(SEED)
    written = []

    signals = {fs: test_signal(fs, rng) for fs in sorted({fs for fs, _ in CASES})}
    for fs, channels in CASES:
        name = f"{fs}_{'stereo' if channels == 2 else 'mono'}"
        sig = signals[fs] if channels == 2 else signals[fs][:, 0]
        ref64 = E.SidechainAnalyzer(fs, np.float64).process(sig.astype(np.float64))["features"]
        ref32 = E.SidechainAnalyzer(fs, np.float32).process(sig)["features"]
        err = float(np.max(np.abs(ref32.astype(np.float64) - ref64)))
        print(f"{name}: frames={ref64.shape[0]} python fp32 vs fp64 max abs error = {err:.3e}")
        if not err < PY_FP32_TOLERANCE:
            print(f"error: float32 reference deviates by more than {PY_FP32_TOLERANCE}")
            return 1
        sig.astype("<f4").tofile(out / f"signal_{name}.f32")
        ref64.astype("<f8").tofile(out / f"features_{name}.f64")
        ref32.astype("<f4").tofile(out / f"features_{name}.f32")
        written += [f"signal_{name}.f32", f"features_{name}.f64", f"features_{name}.f32"]

    for fs in sorted(E.HOP_FOR_RATE):
        coupling_cases(fs, rng).astype("<f8").tofile(out / f"coupling_{fs}.f64")
        written.append(f"coupling_{fs}.f64")

    with open(out / "sha256.txt", "w", encoding="ascii", newline="\n") as fh:
        for name in written:
            digest = hashlib.sha256((out / name).read_bytes()).hexdigest()
            fh.write(f"{digest}  {name}\n")
    print(f"wrote {len(written)} files to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
