# ZERO-FB

**Zero-latency feedback, reverb and noise suppressor plug-in (VST3 / AU).**
A small recurrent network watches 32 ERB bands and steers a serial minimum-phase EQ. The audio path has no framing, no look-ahead and reports 0 samples of latency.

> **Status: in development toward v0.1.0.** The DSP core and the inference engine are done and tested. There is no trained model and no plug-in UI yet, so there is nothing to install today.

## Progress

| Step | Scope | State |
| :--- | :--- | :--- |
| 1 | DSP core, reference implementation, golden tests | Done |
| 2 | SIMD inference engine, closed-loop benchmark | Done (CPU target not yet measured on Apple M4) |
| 3 | Training data, model training, embedded weights | Next |
| 4 | Plug-in UI, VST3 / AU builds, pluginval, packaging | Planned |

## What is here now

- `plugin/Source/dsp/` - the real-time pipeline as a header-only C++17 library with no JUCE dependency: 32-band sidechain analysis (AVX2 / NEON / scalar), 6-channel features, the hysteresis state machine, gain-coupling correction, the serial Cytomic SVF EQ, fail-safe guard, soft limiter and bit-perfect bypass.
- `plugin/Source/nn/` - the inference engine (Conv1d + 2-layer GRU, 167,888 parameters) on AVX2 / NEON / scalar kernels, and the `.swmodel` weight container.
- `training/erb_dsp.py`, `training/nn_reference.py` - independent NumPy references of the same math. `training/model.py` is the PyTorch definition used for training.
- `plugin/tests/` - `ZeroLatencyTest`, `GoldenParityTest`, `EngineBench` (CPU load) and `ClosedLoopBench` (simulated acoustic feedback loop), run on Linux, macOS and Windows by CI.
- `docs/SPEC.md` - the single source of truth for every formula, state machine and acceptance test (Japanese).

The tests use fixed pseudo-random weights. They are not a trained model, so the benchmark figures say nothing yet about how well feedback is suppressed.

## Build and test

Requirements: CMake 3.22+, a C++17 compiler, Python 3 with NumPy (used to generate the golden fixtures at build time).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Documents

- [docs/SPEC.md](docs/SPEC.md) - specification (v1.6). The document number is separate from the software version.
- [docs/SIMD.md](docs/SIMD.md) - SIMD implementation notes.
- [docs/ui_master.svg](docs/ui_master.svg) - UI design master.

## License

Not decided yet. Until a license file is added, all rights are reserved.
