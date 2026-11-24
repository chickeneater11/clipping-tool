# First display recording test

This is a Windows-only prototype, not the replay-buffer product. It records one
entire display to an H.264 MP4 with no audio. The implementation has not yet been
compiled or run on Windows in the development session.

## Build and run (Command Prompt or Developer PowerShell)

Transfer the updated repository to Windows, then run from its root:

```text
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
build\Release\clipping_record_test.exe clip-test.mp4 10
```

Stop if the build fails; do not run an older executable. Use a new filename for
each attempt. Existing files are never overwritten. Recording begins after
initialization and stops automatically after the requested recording interval;
encoder/container finalization can add time. Keep the desktop unlocked and move
a window or play a moving scene during the test. The whole selected display is
captured, including visible apps and notifications.

```text
build\Release\clipping_record_test.exe --help
build\Release\clipping_record_test.exe second-test.mp4 10 0 1
```

Arguments: output filename, duration (1-60 seconds), DXGI adapter index (default
0), and that adapter's output index (default 0). Output index is not Windows
Settings' display number. The program prints the chosen display device name.
The second example selects output 1 on adapter 0. On hybrid laptops, select the
adapter that owns the display; cross-adapter capture is not implemented.

## Playback checks

After `MP4 finalized`, open the file in a video player. Confirm:

- The correct display appears and motion progresses normally.
- Duration is approximately the requested time and seeking works.
- Output is 1920x1080; non-16:9 desktops are letterboxed, not stretched.
- Colors are plausible, text is readable, and the last part is playable.
- No audio is expected.

Send the terminal report and any playback symptoms back. Successful finalization
is not a substitute for decoding/playback validation.

## Pipeline and resource limits

Desktop Duplication -> cached D3D11 BGRA desktop texture -> D3D11 video processor
(scale/letterbox/convert to limited-range BT.709 NV12) -> Media Foundation sink
writer (hardware H.264 encoding) -> MP4.

The writer enables hardware transforms and passes the capture device manager.
Optional converter-disable and low-latency attributes are omitted while diagnosing
writer initialization compatibility. Conversion is still performed explicitly on
the GPU before submission, but additional internal processing is not ruled out. Before recording, it inspects the actual
selected encoder for the hardware identity attribute and D3D11 awareness. If
hardware cannot be verified, it stops rather than silently accepting software.
This does not prove the driver performs no internal copies.

The application owns one cached desktop texture plus a pool capped at six NV12
GPU samples. Samples are returned by the Media Foundation allocator after their
consumers release them, rather than overwriting an in-flight encoder surface.
The encoder, sink writer, capture API, and driver have their own allocations;
this pool is not a total RAM/VRAM cap. Writer throttling remains enabled. Slow
writes can block the recording worker; the next iteration skips elapsed slots
rather than queueing a catch-up burst. This is a single-thread prototype, not the
final nonblocking recording architecture. Replay buffering/hotkeys are not wired.

## Counters

- Frames submitted: successful `WriteSample` calls, not independently decoded frames.
- Repeated desktop frames: submissions using the cached image because no new
  desktop update was available. Normal for a static screen, not encoder failure.
- Missed scheduled slots: 60 Hz slots missed by scheduling/processing delays.
- Sample-pool drops: slots skipped because all pool samples were in use.
- Coalesced desktop updates: updates reported accumulated by Desktop Duplication;
  not necessarily drops relative to the 60 Hz recording target.
- Longest WriteSample call: time blocked submitting a frame, not end-to-end latency.
- Recording and finalization wall times: reported separately.

These are recorder diagnostics, not game FPS or 1% lows. A real game performance
comparison remains a separate benchmark. Timestamps retain skipped-slot gaps;
playback should hold frames over gaps rather than speeding up the video.

## Known scope and failure behavior

Use an SDR, unrotated display for this first test. Rotated/detached outputs are
rejected. HDR tone mapping, mouse-pointer composition, audio, resize recovery,
and reliable exclusive-fullscreen support are not implemented. Pointer visibility
may depend on how Windows composes it. Start with desktop or borderless-windowed
content. Protected content may appear black.

Locking the desktop, switching display modes, driver resets, and disk errors can
stop the test with a stage and HRESULT. A failed run may leave an incomplete MP4;
choose a new filename on retry. Abrupt Ctrl+C termination does not finalize MP4.
Let the short test stop itself. Driver/writer calls have no hard timeout.

## References

- [Desktop Duplication](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api)
- [Hardware MFT identity requirements](https://learn.microsoft.com/en-us/windows/win32/medfound/hardware-mfts)
- [Inspect sink writer transforms](https://learn.microsoft.com/en-us/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsinkwriterex-gettransformforstream)
- [GPU sample allocator](https://learn.microsoft.com/en-us/windows/win32/api/mfapi/nf-mfapi-mfcreatevideosampleallocatorex)

## MP4 initialization diagnostic revision

The `explicit-mp4-v2` banner identifies the revision that creates an H.264 MP4
media sink explicitly, then wraps it with a sink writer. This replaces the
automatic container-construction call that returned E_INVALIDARG on the test PC.
The earlier read/write access change alone did not resolve that failure. This
revision is an unverified fix attempt, not an established root-cause diagnosis.
Output capabilities, sink creation, and writer creation are reported separately.
The existing fixed video stream is used without AddStream, and the owned media
sink is shut down after the writer releases, including on exceptions.

## Writer settings diagnostic revision

`writer-settings-v3` removes the optional converter-disable and low-latency writer
settings. The test PC successfully created the explicit MP4 sink, but its writer
initialization still returned E_INVALIDARG. That isolates the failed API call,
not the root cause. GPU manager and hardware enablement remain required, and the
selected encoder is still checked before any recording occurs.

If minimal initialization still fails, six independent, in-memory setup checks
report HRESULTs for no attributes, GPU only, hardware only, both required settings,
and each optional setting added to the required pair. Each check uses its own
stream and sink; no encoding or recording is performed by these diagnostics.
No diagnostic-only configuration is used as a recording fallback. This revision
still requires building and running on the Windows test PC.
