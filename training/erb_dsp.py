"""ZERO-FB reference DSP (pure NumPy).

This module is the independent reference for the C++ core in plugin/Source/dsp.
Every formula and constant follows docs/SPEC.md. It is written for clarity, not
speed: the sample loops run in Python and are vectorised over bands only.

`dtype` selects the precision of the running state. Coefficients are always
derived in float64 and then rounded to `dtype`, which is what the C++ core does
(double at prepare time, float at run time).
"""

from __future__ import annotations

import numpy as np

NUM_BANDS = 32
F_LOW_HZ = 60.0
F_HIGH_HZ = 15000.0
NUM_FEATURES = 6

# Supported sample rates and their control hop length H (SPEC 2.2).
HOP_FOR_RATE = {44100: 15, 48000: 16, 96000: 32}

SHELF_Q = 1.0 / np.sqrt(2.0)
DC_BLOCK_HZ = 20.0

# Feature constants (SPEC 2.4).
POWER_EPS = 1e-10
ENV_EPS = 1e-5
GLOBAL_TAU = 0.300
GLOBAL_FLOOR_DB = -90.0
FLOOR_TAU_DOWN = 0.050
FLOOR_TAU_UP = 0.800
RING_TAU = 0.050
MOD_GATE_DB = -80.0
MOD_BLOCK_FRAMES = 16
MOD_SLOTS = 15
SIDECHAIN_INPUT_LIMIT = 16.0  # +24 dBFS clamp on the control path input

# Gain-coupling constants (SPEC 2.6).
COUPLING_REF_DB = -12.0
COUPLING_HALF_WIDTH = 6
PGS_ITERATIONS = 3


# --------------------------------------------------------------------------
# Band layout
# --------------------------------------------------------------------------

def erbs(f):
    return 21.4 * np.log10(0.00437 * np.asarray(f, dtype=np.float64) + 1.0)


def band_centers() -> np.ndarray:
    lo, hi = erbs(F_LOW_HZ), erbs(F_HIGH_HZ)
    e = lo + np.arange(NUM_BANDS, dtype=np.float64) / (NUM_BANDS - 1) * (hi - lo)
    f = (10.0 ** (e / 21.4) - 1.0) / 0.00437
    # The end points are defined exactly; remove the round trip error.
    f[0], f[-1] = F_LOW_HZ, F_HIGH_HZ
    return f


def band_edges(fc: np.ndarray):
    f_lo = np.empty(NUM_BANDS)
    f_hi = np.empty(NUM_BANDS)
    f_lo[1:] = np.sqrt(fc[:-1] * fc[1:])
    f_hi[:-1] = np.sqrt(fc[:-1] * fc[1:])
    f_lo[0] = fc[0] ** 2 / f_hi[0]
    f_hi[-1] = fc[-1] ** 2 / f_lo[-1]
    return f_lo, f_hi


def sidechain_q(fc: np.ndarray) -> np.ndarray:
    f_lo, f_hi = band_edges(fc)
    return fc / (f_hi - f_lo)


def alpha_s(tau, fs):
    return 1.0 - np.exp(-1.0 / (np.asarray(tau, dtype=np.float64) * fs))


def alpha_h(tau, hop, fs):
    return 1.0 - np.exp(-hop / (np.asarray(tau, dtype=np.float64) * fs))


def env_tau(fc: np.ndarray) -> np.ndarray:
    return np.maximum(0.002, 1.5 / fc)


def g_base(fc, fs):
    return np.tan(np.pi * np.asarray(fc, dtype=np.float64) / fs)


# --------------------------------------------------------------------------
# Cytomic SVF coefficients and analytic response
# --------------------------------------------------------------------------

def _finish(g, k, m0, m1, m2):
    a1 = 1.0 / (1.0 + g * (g + k))
    a2 = g * a1
    a3 = g * a2
    return dict(g=g, k=k, a1=a1, a2=a2, a3=a3, m0=m0, m1=m1, m2=m2)


def bandpass_coeffs(gb, q):
    k = 1.0 / q
    return _finish(gb, k, np.zeros_like(gb), k, np.zeros_like(gb))


def low_shelf_coeffs(gb, gain_db, q=SHELF_Q):
    a = 10.0 ** (gain_db / 40.0)
    k = 1.0 / q
    return _finish(gb / np.sqrt(a), k, 1.0, k * (a - 1.0), a * a - 1.0)


def peak_coeffs(gb, gain_db, q):
    a = 10.0 ** (gain_db / 40.0)
    k = 1.0 / (q * a)
    return _finish(gb, k, 1.0, k * (a * a - 1.0), 0.0)


def high_shelf_coeffs(gb, gain_db, q=SHELF_Q):
    a = 10.0 ** (gain_db / 40.0)
    k = 1.0 / q
    return _finish(gb * np.sqrt(a), k, a * a, k * (1.0 - a) * a, 1.0 - a * a)


def svf_response(c, f, fs):
    """Complex response of one SVF section at frequency f (Hz)."""
    s = 1j * np.tan(np.pi * np.asarray(f, dtype=np.float64) / fs) / c["g"]
    den = s * s + c["k"] * s + 1.0
    return c["m0"] + c["m1"] * s / den + c["m2"] / den


def eq_corner_frequencies(fc: np.ndarray) -> np.ndarray:
    """Stage 6A filter frequencies.

    Peaks sit on the band centres. The two shelves put their half-gain point
    on the inner band edge (f_hi of band 0, f_lo of band 31) so that the band
    centre lies on the shelf side of the transition.
    """
    f_lo, f_hi = band_edges(fc)
    f = fc.copy()
    f[0] = f_hi[0]
    f[-1] = f_lo[-1]
    return f


def eq_band_coeffs(band: int, gain_db, fs, fc=None, q=None):
    """Stage 6A section for one band: 0 low shelf, 1..30 peak, 31 high shelf."""
    fc = band_centers() if fc is None else fc
    q = sidechain_q(fc) if q is None else q
    gb = g_base(eq_corner_frequencies(fc)[band], fs)
    if band == 0:
        return low_shelf_coeffs(gb, gain_db)
    if band == NUM_BANDS - 1:
        return high_shelf_coeffs(gb, gain_db)
    return peak_coeffs(gb, gain_db, q[band])


def eq_response_db(gains_db, f, fs):
    """Total Stage 6A magnitude (dB) at frequencies f for 32 band gains."""
    fc = band_centers()
    q = sidechain_q(fc)
    total = np.zeros(np.shape(f), dtype=np.float64)
    for b in range(NUM_BANDS):
        h = svf_response(eq_band_coeffs(b, gains_db[b], fs, fc, q), f, fs)
        total += 20.0 * np.log10(np.abs(h))
    return total


# --------------------------------------------------------------------------
# Stage 5B: gain coupling matrix and Projected Gauss-Seidel
# --------------------------------------------------------------------------

def coupling_matrix(fs) -> np.ndarray:
    """C[b, j]: dB response of band j's section at f_b per dB of its gain.

    Every entry, the diagonal included, is measured from the analytic response
    at the reference gain. Entries outside |b - j| <= COUPLING_HALF_WIDTH are
    zero.
    """
    fc = band_centers()
    q = sidechain_q(fc)
    c = np.zeros((NUM_BANDS, NUM_BANDS))
    for j in range(NUM_BANDS):
        coeffs = eq_band_coeffs(j, COUPLING_REF_DB, fs, fc, q)
        lo = max(0, j - COUPLING_HALF_WIDTH)
        hi = min(NUM_BANDS - 1, j + COUPLING_HALF_WIDTH)
        for b in range(lo, hi + 1):
            mag = np.abs(svf_response(coeffs, fc[b], fs))
            c[b, j] = 20.0 * np.log10(mag) / COUPLING_REF_DB
    return c


def projected_gauss_seidel(g_target, c, floor_db, dtype=np.float64):
    g_target = np.asarray(g_target, dtype=dtype)
    c = np.asarray(c, dtype=dtype)
    floor = dtype(floor_db)
    zero = dtype(0.0)
    g = g_target.copy()
    for _ in range(PGS_ITERATIONS):
        for b in range(NUM_BANDS):
            acc = g_target[b]
            for j in range(max(0, b - COUPLING_HALF_WIDTH),
                           min(NUM_BANDS - 1, b + COUPLING_HALF_WIDTH) + 1):
                if j != b:
                    acc = dtype(acc - c[b, j] * g[j])
            g[b] = min(zero, max(floor, dtype(acc / c[b, b])))
    return g


# --------------------------------------------------------------------------
# Stage 2-SC + Stage 3: sidechain bank and 6-channel features
# --------------------------------------------------------------------------

class SidechainAnalyzer:
    """Reference for the control path up to the feature tensor.

    process() takes samples shaped (n,) or (n, channels) and returns a dict
    with the feature tensor (T, 6, 32) and the raw per-frame quantities used
    by the FSM.
    """

    def __init__(self, fs: int, dtype=np.float64, mode: float = 0.0):
        if fs not in HOP_FOR_RATE:
            raise ValueError(f"unsupported sample rate {fs}")
        self.fs = fs
        self.hop = HOP_FOR_RATE[fs]
        self.dtype = dtype
        self.mode = mode
        t = dtype
        fc = band_centers()
        bp = bandpass_coeffs(g_base(fc, fs), sidechain_q(fc))
        self.a1 = bp["a1"].astype(t)
        self.a2 = bp["a2"].astype(t)
        self.a3 = bp["a3"].astype(t)
        self.m1 = bp["m1"].astype(t)
        a_env = alpha_s(env_tau(fc), fs)
        self.a_env = a_env.astype(t)
        self.oma_env = (1.0 - a_env).astype(t)
        self.a_global = t(alpha_h(GLOBAL_TAU, self.hop, fs))
        self.a_floor_down = t(alpha_h(FLOOR_TAU_DOWN, self.hop, fs))
        self.a_floor_up = t(alpha_h(FLOOR_TAU_UP, self.hop, fs))
        self.a_ring = t(alpha_h(RING_TAU, self.hop, fs))
        self.reset()

    def reset(self):
        t = self.dtype
        self.ic1 = None
        self.ic2 = None
        self.ps = np.zeros(NUM_BANDS, dtype=t)
        self.p_global = t(0.0)
        self.e_floor = np.zeros(NUM_BANDS, dtype=t)
        self.e_prev = np.zeros(NUM_BANDS, dtype=t)
        self.t_raw = np.zeros(NUM_BANDS, dtype=t)
        self.slot_max = np.zeros((MOD_SLOTS, NUM_BANDS), dtype=t)
        self.slot_min = np.zeros((MOD_SLOTS, NUM_BANDS), dtype=t)
        self.frame = 0
        self.hop_phase = 0

    def _frame(self):
        t = self.dtype
        k = self.frame
        ps = self.ps
        e_s = (t(10.0) * np.log10(ps + t(POWER_EPS))).astype(t)
        amp = np.sqrt(ps)

        mean_p = t(0.0)
        for b in range(NUM_BANDS):
            mean_p = t(mean_p + ps[b])
        mean_p = t(mean_p * t(1.0 / NUM_BANDS))
        self.p_global = t((t(1.0) - self.a_global) * self.p_global
                          + self.a_global * mean_p)
        e_global = max(t(GLOBAL_FLOOR_DB),
                       t(t(10.0) * np.log10(self.p_global + t(POWER_EPS))))

        if k == 0:
            self.e_floor = e_s.copy()
        else:
            a = np.where(e_s < self.e_floor, self.a_floor_down, self.a_floor_up)
            self.e_floor = ((t(1.0) - a) * self.e_floor + a * e_s).astype(t)

        slot = (k // MOD_BLOCK_FRAMES) % MOD_SLOTS
        if k % MOD_BLOCK_FRAMES == 0:
            self.slot_max[slot] = amp
            self.slot_min[slot] = amp
        else:
            self.slot_max[slot] = np.maximum(self.slot_max[slot], amp)
            self.slot_min[slot] = np.minimum(self.slot_min[slot], amp)
        filled = min(k // MOD_BLOCK_FRAMES + 1, MOD_SLOTS)
        w_max = self.slot_max[:filled].max(axis=0)
        w_min = self.slot_min[:filled].min(axis=0)
        md = (t(20.0) * np.log10((w_max + t(ENV_EPS)) / (w_min + t(ENV_EPS)))).astype(t)
        md_raw = np.where(e_s < t(MOD_GATE_DB), t(0.0), md).astype(t)

        neigh = np.empty(NUM_BANDS, dtype=t)
        neigh[0] = e_s[1]
        neigh[-1] = e_s[-2]
        neigh[1:-1] = t(0.5) * (e_s[:-2] + e_s[2:])
        prom = e_s - neigh
        self.t_raw = ((t(1.0) - self.a_ring) * self.t_raw + self.a_ring * prom).astype(t)

        delta_e = np.zeros(NUM_BANDS, dtype=t) if k == 0 else (e_s - self.e_prev)
        self.e_prev = e_s.copy()

        x = np.empty((NUM_FEATURES, NUM_BANDS), dtype=t)
        x[0] = np.clip((e_s - self.e_floor) / t(30.0), -3.0, 3.0)
        x[1] = np.clip((e_s - e_global) / t(20.0), -3.0, 3.0)
        x[2] = np.clip(delta_e / t(6.0), -3.0, 3.0)
        x[3] = np.clip((md_raw - t(12.0)) / t(12.0), -2.0, 4.0)
        x[4] = np.clip(self.t_raw / t(15.0), -2.0, 4.0)
        x[5] = t(self.mode)
        self.frame += 1
        return x, e_s, delta_e, md_raw, self.t_raw.copy()

    def process(self, samples):
        t = self.dtype
        x = np.clip(np.asarray(samples, dtype=t),
                    -SIDECHAIN_INPUT_LIMIT, SIDECHAIN_INPUT_LIMIT)
        if x.ndim == 1:
            x = x[:, None]
        n, ch = x.shape
        if self.ic1 is None:
            self.ic1 = np.zeros((ch, NUM_BANDS), dtype=t)
            self.ic2 = np.zeros((ch, NUM_BANDS), dtype=t)
        a1, a2, a3, m1 = self.a1, self.a2, self.a3, self.m1
        ic1, ic2, ps = self.ic1, self.ic2, self.ps
        a_env, oma_env = self.a_env, self.oma_env
        half = t(0.5)
        two = t(2.0)
        out = {"features": [], "e_s": [], "delta_e": [], "md_raw": [], "t_raw": []}
        for i in range(n):
            v0 = x[i][:, None]
            v3 = v0 - ic2
            v1 = a1 * ic1 + a2 * v3
            v2 = ic2 + a2 * ic1 + a3 * v3
            ic1 = two * v1 - ic1
            ic2 = two * v2 - ic2
            y = m1 * v1
            p = y[0] * y[0] if ch == 1 else half * (y[0] * y[0] + y[1] * y[1])
            ps = oma_env * ps + a_env * p
            self.hop_phase += 1
            if self.hop_phase == self.hop:
                self.hop_phase = 0
                self.ps = ps
                feat, e_s, d_e, md, tr = self._frame()
                out["features"].append(feat)
                out["e_s"].append(e_s)
                out["delta_e"].append(d_e)
                out["md_raw"].append(md)
                out["t_raw"].append(tr)
        self.ic1, self.ic2, self.ps = ic1, ic2, ps
        return {k: np.array(v, dtype=t) for k, v in out.items()}
