#!/usr/bin/env python3
"""Check that the three descriptions of the network agree.

  model.py          PyTorch module used for training
  nn_reference.py   NumPy reference used by the golden tests
  export_weights.py .swmodel container

Loads the fixed test weights into the PyTorch module, runs both on the same
random features in float64 and compares the 64 mask outputs. Needs PyTorch.
"""

from __future__ import annotations

import sys

import numpy as np
import torch

import export_weights as X
import model as M
import nn_reference as R

TOLERANCE = 1e-12
FRAMES = 1500


def main() -> int:
    net = M.ZeroFbNet()
    assert M.count_parameters(net) == X.TOTAL_PARAMS, M.count_parameters(net)

    weights = X.unpack(X.pack(X.random_test_weights()))
    net.load_state_dict({k: torch.from_numpy(v) for k, v in X.to_state_dict(weights).items()})
    net = net.double().eval()

    rng = np.random.default_rng(7)
    features = np.clip(rng.standard_normal((FRAMES, 6, 32)) * 1.5, -3.0, 4.0)
    reference, _ = R.NumpyNet(weights, np.float64).run(features)
    with torch.no_grad():
        m_fb, m_rev, _ = net(torch.from_numpy(features)[None])
    got = torch.cat([m_fb, m_rev], dim=-1)[0].numpy()
    err = float(np.max(np.abs(got - reference)))
    print(f"parameters {M.count_parameters(net)}, NumPy reference vs PyTorch (float64): max abs error {err:.3e}")

    # A fresh module exports and reloads without loss.
    fresh = M.ZeroFbNet()
    exported = X.unpack(X.pack(X.from_state_dict(fresh.state_dict())))
    for (name, key, _, _) in X.TENSORS:
        assert np.array_equal(exported[name], fresh.state_dict()[key].numpy()), name
    head_start = float(torch.sigmoid(fresh.head_fb.bias.detach()).mean())
    print(f"export round trip ok, initial head output {head_start:.3f}")
    return 0 if err < TOLERANCE and abs(head_start - 0.9) < 0.005 else 1


if __name__ == "__main__":
    sys.exit(main())
