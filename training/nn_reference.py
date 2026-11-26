"""ZERO-FB network forward pass in pure NumPy (SPEC 3.1).

Independent reference for plugin/Source/nn/CausalGruEngine.h: one control
frame per call, GRU state carried between calls. `dtype` selects the
arithmetic precision. Weights are a {tensor name: array} dict as produced by
export_weights.py.
"""

from __future__ import annotations

import numpy as np

NUM_BANDS = 32
NUM_FEATURES = 6
CONV_CHANNELS = 16
HIDDEN = 96
LN_EPS = 1e-5
LAYER_SIZES = {"conv1": CONV_CHANNELS * NUM_BANDS, "proj": HIDDEN, "gru1": HIDDEN,
               "gru2": HIDDEN, "masks": 2 * NUM_BANDS}
LAYER_ORDER = ("conv1", "proj", "gru1", "gru2", "masks")
LAYER_TOTAL = sum(LAYER_SIZES.values())  # 864


def _sigmoid(x):
    return 1.0 / (1.0 + np.exp(-x))


class NumpyNet:
    def __init__(self, weights: dict, dtype=np.float64):
        self.dtype = dtype
        self.w = {k: np.asarray(v, dtype=dtype) for k, v in weights.items()}
        self.reset()

    def reset(self) -> None:
        self.h1 = np.zeros(HIDDEN, dtype=self.dtype)
        self.h2 = np.zeros(HIDDEN, dtype=self.dtype)

    def _gru(self, prefix: str, x, h):
        w = self.w
        gi = w[f"{prefix}_w_ih"] @ x + w[f"{prefix}_bias_ih"]
        gh = w[f"{prefix}_w_hh"] @ h + w[f"{prefix}_bias_hh"]
        r = _sigmoid(gi[:HIDDEN] + gh[:HIDDEN])
        z = _sigmoid(gi[HIDDEN:2 * HIDDEN] + gh[HIDDEN:2 * HIDDEN])
        n = np.tanh(gi[2 * HIDDEN:] + r * gh[2 * HIDDEN:])
        return ((1.0 - z) * n + z * h).astype(self.dtype)

    def step(self, features) -> dict:
        """features: (6, 32). Returns every layer output for this frame."""
        t = self.dtype
        w = self.w
        x = np.asarray(features, dtype=t)
        # Conv1d, kernel 5, zero padding 2, along the band axis:
        #   conv[o, b] = bias[o] + sum_{c, k} weight[o, c, k] * padded[c, b + k]
        padded = np.zeros((NUM_FEATURES, NUM_BANDS + 4), dtype=t)
        padded[:, 2:-2] = x
        windows = np.stack([padded[:, k:k + NUM_BANDS] for k in range(5)], axis=1)  # (6, 5, 32)
        conv = (w["conv1d_weight"].reshape(CONV_CHANNELS, -1) @ windows.reshape(-1, NUM_BANDS)
                + w["conv1d_bias"][:, None])
        conv = (conv * _sigmoid(conv)).astype(t)                       # SiLU

        y = w["proj_weight"] @ conv.reshape(-1) + w["proj_bias"]         # C-order flatten
        mean = y.mean(dtype=t)
        var = ((y - mean) ** 2).mean(dtype=t)
        y = (y - mean) / np.sqrt(var + t(LN_EPS)) * w["ln_gamma"] + w["ln_beta"]
        proj = (y * _sigmoid(y)).astype(t)

        self.h1 = self._gru("gru1", proj, self.h1)
        self.h2 = self._gru("gru2", self.h1, self.h2)
        m_fb = _sigmoid(w["head_fb_weight"] @ self.h2 + w["head_fb_bias"])
        m_rev = _sigmoid(w["head_rev_weight"] @ self.h2 + w["head_rev_bias"])
        masks = np.concatenate([m_fb, m_rev]).astype(t)
        return {"conv1": conv.reshape(-1), "proj": proj, "gru1": self.h1.copy(),
                "gru2": self.h2.copy(), "masks": masks}

    def run(self, features, keep_layers: int = 0):
        """features: (T, 6, 32). Returns (masks (T, 64), layers (keep_layers, 864))."""
        frames = features.shape[0]
        masks = np.empty((frames, 2 * NUM_BANDS), dtype=self.dtype)
        layers = np.empty((min(keep_layers, frames), LAYER_TOTAL), dtype=self.dtype)
        for k in range(frames):
            out = self.step(features[k])
            masks[k] = out["masks"]
            if k < keep_layers:
                layers[k] = np.concatenate([out[name] for name in LAYER_ORDER])
        return masks, layers
