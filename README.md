# Clipping Tool

A Windows gameplay clipping tool focused on predictable memory use and low
frame-time impact. Native C++20; no runtime dependencies in the current core.

## Current state

This is the first foundation, **not a working screen recorder yet**:

- Encoded-video replay buffer with payload allocation, packet-count, and time limits.
- Whole-GOP eviction so retained video begins at an independent keyframe.
- Synthetic buffer probe and correctness/stress tests.
- Windows hardware capability probe (adapter inventory, encoder negotiation, GPU sample submission).
- CMake build and GitHub Actions configuration for Windows, macOS, and Linux.

Capture, hardware encoding, audio, hotkeys, and MP4 export are not implemented.
The probe does not capture your screen, create playable clips, or measure game FPS.

## Build and test

Install CMake 3.20+ and a C++20 compiler. On Windows, use Visual Studio 2022
Build Tools with the Desktop development with C++ workload and Windows SDK.
From a developer terminal at the repository root:

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Run `build/Release/clipping_probe.exe` on Windows with the Visual Studio generator,
or `./build/clipping_probe` with a single-configuration generator.

## Windows capability probe

After building on Windows, run from the repository root in PowerShell:

```powershell
.\build\Release\clipping_capabilities.exe | Tee-Object -FilePath capabilities.txt
$LASTEXITCODE
```

The Visual Studio generator places the executable in `build/Release`; a
single-configuration generator typically places it directly in `build`.
The report tests hardware H.264 encoders with synthetic GPU-backed NV12 input
at 720p30 / 6 Mbps, 1080p60 / 12 Mbps, and 1440p60 / 20 Mbps. It does not
capture your screen or audio. See [probe details](docs/capability-probe.md).

Exit codes: `0` = at least one combination accepted a GPU sample;
`2` = no combination verified; `1` = probe initialization or fatal error.
Acceptance is not proof of playable output or real-time performance.

## Memory contract

Defaults: 30-second maximum timestamp span, 64 MiB encoded payload capacity,
and 3,600 packets. The first limit reached wins; retained duration may be shorter.
Payload accounting uses vector capacity, so unused reserved storage is charged.
Metadata, allocator overhead, incoming packets, encoder buffers, and GPU textures
are **outside** this payload budget. This is not a total-process RAM guarantee.

The producer must bound packet allocation before calling `push`. The core takes
ownership of packets without copying their payload. It is single-threaded and
must be owned by one recording worker. The read-only packet view is invalidated
by mutations; do not use it concurrently or retain it for an asynchronous export.

Only closed-GOP video with no B frames and strictly increasing microsecond PTS is
supported. A keyframe must be an independently decodable IDR. Codec configuration
must be retained separately by the future encoder/export integration. Audio will
need its own timestamp-aligned policy. Dropping an oversized encoded frame clears
the buffer and requires a new IDR. Stream discontinuities require `reset()`.

See [architecture](docs/architecture.md) and [performance plan](docs/performance.md).

## Validation status

Tests are written but have not been executed successfully on the development Mac:
the Apple developer tools/SDK are missing. The installed Homebrew GCC also fails
because system SDK headers are absent. CI is configured but has not been run or
pushed by this setup. No FPS or total-memory performance claims have been validated.

The Windows capability probe is newly added and has not been compiled or run on
Windows in this development session. Existing buffer tests were run by the user;
their detailed results were not captured here.
