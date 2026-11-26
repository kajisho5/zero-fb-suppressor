#!/usr/bin/env python3
"""Weight container for ZERO-FB (SPEC 3.2).

  .swmodel           64-byte header + 18 float32 tensors, 671,616 bytes
  EmbeddedWeights.h  the same payload as a constexpr array for the plug-in

Usage:
  export_weights.py --checkpoint best.pt --swmodel OUT.swmodel [--header EmbeddedWeights.h]
  export_weights.py --random-test      --swmodel OUT.swmodel [--header EmbeddedWeights.h]

--random-test writes the fixed pseudo-random weights (seed 42) used by the
golden tests. They are not a trained model.
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

import numpy as np

MAGIC = b"SWZFB004"
HEADER_BYTES = 64
SAMPLE_RATE_REF = 48000
NUM_BANDS = 32
NUM_FEATURES = 6
HOP_REF = 16
HIDDEN = 96
TOTAL_PARAMS = 167_888
FILE_BYTES = 671_616

# (tensor name, PyTorch key, shape, byte offset from SPEC 3.2)
TENSORS = [
    ("conv1d_weight", "conv1.weight", (16, 6, 5), 0x000040),
    ("conv1d_bias", "conv1.bias", (16,), 0x0007C0),
    ("proj_weight", "proj.weight", (96, 512), 0x000800),
    ("proj_bias", "proj.bias", (96,), 0x030800),
    ("ln_gamma", "ln.weight", (96,), 0x030980),
    ("ln_beta", "ln.bias", (96,), 0x030B00),
    ("gru1_w_ih", "gru1.weight_ih_l0", (288, 96), 0x030C80),
    ("gru1_w_hh", "gru1.weight_hh_l0", (288, 96), 0x04BC80),
    ("gru1_bias_ih", "gru1.bias_ih_l0", (288,), 0x066C80),
    ("gru1_bias_hh", "gru1.bias_hh_l0", (288,), 0x067100),
    ("gru2_w_ih", "gru2.weight_ih_l0", (288, 96), 0x067580),
    ("gru2_w_hh", "gru2.weight_hh_l0", (288, 96), 0x082580),
    ("gru2_bias_ih", "gru2.bias_ih_l0", (288,), 0x09D580),
    ("gru2_bias_hh", "gru2.bias_hh_l0", (288,), 0x09DA00),
    ("head_fb_weight", "head_fb.weight", (32, 96), 0x09DE80),
    ("head_fb_bias", "head_fb.bias", (32,), 0x0A0E80),
    ("head_rev_weight", "head_rev.weight", (32, 96), 0x0A0F00),
    ("head_rev_bias", "head_rev.bias", (32,), 0x0A3F00),
]


def _check_layout() -> None:
    offset = HEADER_BYTES
    total = 0
    for name, _, shape, spec_offset in TENSORS:
        assert offset == spec_offset, (name, hex(offset), hex(spec_offset))
        assert offset % 64 == 0, name
        count = int(np.prod(shape))
        offset += 4 * count
        total += count
    assert total == TOTAL_PARAMS and offset == FILE_BYTES, (total, offset)


_check_layout()


def random_test_weights(seed: int = 42) -> dict:
    """Deterministic weights for the golden tests (NumPy only, no PyTorch).

    Scales follow the training initialisation (SPEC 3.1), but every bias and
    the LayerNorm affine are non-zero so that each term of the forward pass is
    exercised.
    """
    rng = np.random.default_rng(seed)
    w = {}

    def uniform(shape, bound):
        return rng.uniform(-bound, bound, shape)

    w["conv1d_weight"] = uniform((16, 6, 5), 1.0 / np.sqrt(30.0))
    w["conv1d_bias"] = uniform((16,), 0.1)
    w["proj_weight"] = uniform((96, 512), 1.0 / np.sqrt(512.0))
    w["proj_bias"] = uniform((96,), 0.1)
    w["ln_gamma"] = rng.uniform(0.5, 1.5, (96,))
    w["ln_beta"] = uniform((96,), 0.2)
    for layer in ("gru1", "gru2"):
        w[f"{layer}_w_ih"] = uniform((288, 96), np.sqrt(6.0 / (96 + 288)))
        q, _ = np.linalg.qr(rng.standard_normal((288, 96)))
        w[f"{layer}_w_hh"] = q
        w[f"{layer}_bias_ih"] = uniform((288,), 0.1)
        w[f"{layer}_bias_hh"] = uniform((288,), 0.1)
    for head in ("head_fb", "head_rev"):
        # Three times the training scale so that the test masks move visibly.
        w[f"{head}_weight"] = uniform((32, 96), 3.0 / np.sqrt(96.0))
        w[f"{head}_bias"] = 2.2 + uniform((32,), 0.5)
    return {k: v.astype(np.float32) for k, v in w.items()}


def from_state_dict(state_dict) -> dict:
    """PyTorch state_dict (tensors or arrays) -> {tensor name: float32 array}."""
    out = {}
    for name, key, shape, _ in TENSORS:
        value = state_dict[key]
        value = value.detach().cpu().numpy() if hasattr(value, "detach") else np.asarray(value)
        if tuple(value.shape) != shape:
            raise ValueError(f"{key}: shape {tuple(value.shape)} != {shape}")
        out[name] = np.ascontiguousarray(value, dtype=np.float32)
    return out


def to_state_dict(weights: dict) -> dict:
    return {key: weights[name] for name, key, _, _ in TENSORS}


def pack(weights: dict) -> bytes:
    payload = b"".join(
        np.ascontiguousarray(weights[name], dtype="<f4").reshape(shape).tobytes()
        for name, _, shape, _ in TENSORS)
    header = MAGIC + struct.pack("<7I", SAMPLE_RATE_REF, NUM_BANDS, NUM_FEATURES, HOP_REF,
                                 HIDDEN, TOTAL_PARAMS, zlib.crc32(payload) & 0xFFFFFFFF)
    header += b"\x00" * (HEADER_BYTES - len(header))
    blob = header + payload
    assert len(blob) == FILE_BYTES, len(blob)
    return blob


def unpack(blob: bytes) -> dict:
    if len(blob) != FILE_BYTES or blob[:8] != MAGIC:
        raise ValueError("not a ZERO-FB .swmodel file")
    fields = struct.unpack("<7I", blob[8:36])
    expected = (SAMPLE_RATE_REF, NUM_BANDS, NUM_FEATURES, HOP_REF, HIDDEN, TOTAL_PARAMS)
    if fields[:6] != expected:
        raise ValueError(f"unexpected header fields {fields[:6]}")
    if zlib.crc32(blob[HEADER_BYTES:]) & 0xFFFFFFFF != fields[6]:
        raise ValueError("CRC32 mismatch")
    out = {}
    for name, _, shape, offset in TENSORS:
        count = int(np.prod(shape))
        out[name] = np.frombuffer(blob, dtype="<f4", count=count, offset=offset).reshape(shape).copy()
    return out


def _hex_float(value: np.float32) -> str:
    text = float(value).hex()  # exact; float32 values need at most 6 hex digits
    mantissa, exponent = text.split("p")
    if "." in mantissa:
        mantissa = mantissa.rstrip("0").rstrip(".")
    return f"{mantissa}p{exponent}f"


def write_header(path: Path, blob: bytes, description: str) -> None:
    payload = np.frombuffer(blob, dtype="<f4", offset=HEADER_BYTES)
    crc = struct.unpack("<I", blob[32:36])[0]
    lines = [
        "// ZERO-FB - embedded model weights (SPEC 3.2). Generated by training/export_weights.py.",
        f"// {description}",
        "// One read-only copy is shared by every plug-in instance in the process.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace zerofb {",
        "namespace embedded {",
        "",
        f"inline constexpr int kWeightCount = {payload.size};",
        f"inline constexpr std::uint32_t kPayloadCrc32 = 0x{crc:08X}u;",
        "",
        "alignas(64) inline constexpr float kWeights[kWeightCount] = {",
    ]
    for start in range(0, payload.size, 8):
        lines.append("    " + ", ".join(_hex_float(v) for v in payload[start:start + 8]) + ",")
    lines += ["};", "", "} // namespace embedded", "} // namespace zerofb", ""]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="ascii", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--checkpoint", type=Path, help="PyTorch checkpoint (state_dict or {'model': state_dict})")
    source.add_argument("--random-test", action="store_true", help="fixed pseudo-random test weights (seed 42)")
    parser.add_argument("--swmodel", type=Path, required=True)
    parser.add_argument("--header", type=Path)
    args = parser.parse_args()

    if args.random_test:
        weights = random_test_weights()
        description = "TEST WEIGHTS: fixed pseudo-random values (seed 42). Not a trained model."
    else:
        import torch
        state = torch.load(args.checkpoint, map_location="cpu")
        state = state.get("model", state) if isinstance(state, dict) else state
        weights = from_state_dict(state)
        description = f"Exported from {args.checkpoint.name}."

    blob = pack(weights)
    unpack(blob)  # self check: header fields and CRC32
    args.swmodel.parent.mkdir(parents=True, exist_ok=True)
    args.swmodel.write_bytes(blob)
    print(f"{args.swmodel}: {len(blob)} bytes, crc32 0x{struct.unpack('<I', blob[32:36])[0]:08X}")
    if args.header:
        write_header(args.header, blob, description)
        print(f"{args.header}: {args.header.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
