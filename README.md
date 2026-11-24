# Clipping Tool

A Windows gameplay clipping tool focused on predictable memory use and low
frame-time impact. Native C++20; no runtime dependencies in the current core.

## Current state

This is the first foundation, **not a working screen recorder yet**:

- Encoded-video replay buffer with payload allocation, packet-count, and time limits.
- Whole-GOP eviction so retained video begins at an independent keyframe.
- Synthetic buffer probe and correctness/stress tests.
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
