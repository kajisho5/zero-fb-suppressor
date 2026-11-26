# ZERO-FB

**Zero-latency feedback, reverb and noise suppressor plug-in (VST3 / AU).**
A small recurrent network watches 32 ERB bands and steers a serial minimum-phase EQ. The audio path has no framing, no look-ahead and reports 0 samples of latency.

> **Status: in development toward v0.1.0.** The DSP core is done and tested. The neural engine, the trained model and the plug-in UI are not in the repository yet, so there is nothing to install today.

## Progress

| Step | Scope | State |
| :--- | :--- | :--- |
| 1 | DSP core, reference implementation, golden tests | Done |
| 2 | SIMD inference engine, closed-loop benchmark | Next |
| 3 | Training data, model training, embedded weights | Planned |
| 4 | Plug-in UI, VST3 / AU builds, pluginval, packaging | Planned |

## What is here now

- `plugin/Source/dsp/` - the real-time pipeline as a header-only C++17 library with no JUCE dependency: 32-band sidechain analysis (AVX2 / NEON / scalar), 6-channel features, the hysteresis state machine, gain-coupling correction, the serial Cytomic SVF EQ, fail-safe guard, soft limiter and bit-perfect bypass.
- `training/erb_dsp.py` - an independent NumPy reference of the same math.
- `plugin/tests/` - `ZeroLatencyTest` and `GoldenParityTest`, run on Linux, macOS and Windows by CI.
- `docs/SPEC.md` - the single source of truth for every formula, state machine and acceptance test (Japanese).

Without a mask estimator attached, the engine runs with all masks at 1.0, which means no suppression.

## Build and test

Requirements: CMake 3.22+, a C++17 compiler, Python 3 with NumPy (used to generate the golden fixtures at build time).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Documents

- [docs/SPEC.md](docs/SPEC.md) - specification (v1.5). The document number is separate from the software version.
- [docs/SIMD.md](docs/SIMD.md) - SIMD implementation notes.
- [docs/ui_master.svg](docs/ui_master.svg) - UI design master.

## License

Not decided yet. Until a license file is added, all rights are reserved.
