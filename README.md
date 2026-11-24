# Clipping Tool

A Windows gameplay clipping tool focused on predictable memory use and low
frame-time impact. Native C++20; no runtime dependencies in the current core.

## Current state

This is an early prototype. **The new replay path still needs Windows build and playback validation.**

- Encoded-video replay buffer with payload allocation, packet-count, and time limits.
- Whole-GOP eviction so retained video begins at an independent keyframe.
- Synthetic buffer probe and correctness/stress tests.
- Short Windows display-to-MP4 recording test with hardware-encoder verification.
- Windows hardware capability probe (adapter inventory, encoder negotiation, GPU sample submission).
- CMake build and GitHub Actions configuration for Windows, macOS, and Linux.

The new `clipping_record_test` implements display capture and hardware-encoded MP4
recording. Audio is not implemented. Replay-buffer integration and F8 saving are implemented
in a separate executable and await Windows validation.
The separate probes do not capture your screen or measure game FPS.

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

## First real recording test

After transferring these changes and rebuilding on Windows:

```text
build\Release\clipping_record_test.exe clip-test.mp4 10
```

This captures the **entire selected display** for ten seconds, video only, at
1080p60 / 12 Mbps. Use a new filename each time. Open the resulting MP4 and check
motion, duration, and colors. See [recording instructions and limitations](docs/recording-test.md).
The recorder and its Windows CLI smoke tests have not been run in this session.

## Replay hotkey prototype

A new `clipping_replay` executable connects a direct hardware encoder to the
30-second / 64 MiB replay buffer. F8 saves an MP4 to `clips`; Ctrl+C stops it.
**Windows validation is pending. Saving briefly pauses capture in this version.**
Transfer the whole repo, including the new shared capture header, before rebuilding.
See [replay setup and test steps](docs/replay-test.md).

```text
build\Release\clipping_replay.exe
```

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

The user built and ran the capability probe on an RTX 2060: all three presets
accepted GPU-backed input. Existing buffer tests were run by the user; their
detailed results were not captured here. The user verified a playable MP4 recording: 600 submitted frames, no missed slots
or pool drops in a ten-second RTX 2060 run. The new direct-encoder replay path
is not yet verified on Windows.
