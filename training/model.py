"""ZERO-FB network (SPEC 3.1): causal Conv1d + 2-layer GRU, two 32-band heads.

167,888 parameters. Parameter names match the "PyTorch key" column of the
weight table in SPEC 3.2, so state_dict() can be exported directly with
export_weights.py.
"""

from __future__ import annotations

import torch
from torch import nn

NUM_BANDS = 32
NUM_FEATURES = 6
CONV_CHANNELS = 16
HIDDEN = 96
TOTAL_PARAMS = 167_888
HEAD_BIAS_INIT = 2.2  # sigmoid(2.2) ~ 0.9: masks start close to "pass"


class ZeroFbNet(nn.Module):
    def __init__(self) -> None:
        super().__init__()
        self.conv1 = nn.Conv1d(NUM_FEATURES, CONV_CHANNELS, kernel_size=5, stride=1,
                               padding=2, padding_mode="zeros", bias=True)
        self.proj = nn.Linear(CONV_CHANNELS * NUM_BANDS, HIDDEN)
        self.ln = nn.LayerNorm(HIDDEN, eps=1e-5, elementwise_affine=True)
        self.gru1 = nn.GRU(HIDDEN, HIDDEN, num_layers=1, batch_first=True, bias=True)
        self.gru2 = nn.GRU(HIDDEN, HIDDEN, num_layers=1, batch_first=True, bias=True)
        self.head_fb = nn.Linear(HIDDEN, NUM_BANDS)
        self.head_rev = nn.Linear(HIDDEN, NUM_BANDS)
        self.act = nn.SiLU()
        self.reset_parameters()

    def reset_parameters(self) -> None:
        # Conv1d / Linear keep PyTorch's default Kaiming-uniform weights.
        for gru in (self.gru1, self.gru2):
            nn.init.xavier_uniform_(gru.weight_ih_l0)
            nn.init.orthogonal_(gru.weight_hh_l0)
            nn.init.zeros_(gru.bias_ih_l0)
            nn.init.zeros_(gru.bias_hh_l0)
        for module in (self.conv1, self.proj):
            nn.init.zeros_(module.bias)
        for head in (self.head_fb, self.head_rev):
            nn.init.constant_(head.bias, HEAD_BIAS_INIT)

    def forward(self, x: torch.Tensor, state=None):
        """x: (batch, frames, 6, 32). Returns (m_fb, m_rev, state), masks (batch, frames, 32)."""
        batch, frames = x.shape[0], x.shape[1]
        y = self.act(self.conv1(x.reshape(batch * frames, NUM_FEATURES, NUM_BANDS)))
        y = self.act(self.ln(self.proj(y.reshape(batch * frames, CONV_CHANNELS * NUM_BANDS))))
        y = y.reshape(batch, frames, HIDDEN)
        h1, h2 = (None, None) if state is None else state
        y, h1 = self.gru1(y, h1)
        y, h2 = self.gru2(y, h2)
        return torch.sigmoid(self.head_fb(y)), torch.sigmoid(self.head_rev(y)), (h1, h2)


def count_parameters(model: nn.Module) -> int:
    return sum(p.numel() for p in model.parameters())


if __name__ == "__main__":
    net = ZeroFbNet()
    n = count_parameters(net)
    print(f"parameters: {n}")
    assert n == TOTAL_PARAMS, n
