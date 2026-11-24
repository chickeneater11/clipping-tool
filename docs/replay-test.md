# First replay-buffer integration

Status: implemented, Windows build/runtime validation pending. The previously
working `clipping_record_test` remains a separate executable. Shared capture
helpers were moved unchanged into `src/windows/capture_support.hpp`; transfer the
whole updated repository, not just a single .cpp file.

## Build and run (Command Prompt or Developer PowerShell)

```text
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
build\Release\clipping_replay.exe
```

The banner is `Clipping replay: baseline-compat-v2`. This captures the entire first
output on adapter 0, at 1080p60, 12 Mbps, without audio. Capture retains up to
30 seconds in memory. Every five seconds it prints buffered duration, encoded
payload memory, submitted/output frames, skipped/busy slots, and packets rejected
while waiting for an IDR. No recording is written to disk until F8 is pressed.

- Let it run for at least 35 seconds with visible motion, then press **F8**.
- Look for `Saved ...` and open the new MP4 in `clips` under the working directory.
- **Ctrl+C** stops the recorder. Exiting does not automatically save the buffer.
- Optionally specify an output folder: `clipping_replay.exe "D:\Game Clips"`.
- If another program owns F8, startup reports the registration error.

Clip names include a timestamp and collision suffix. Existing files are never
overwritten. MP4 files and the default clips folder are ignored by Git.

## What changed

The replay executable drives a hardware H.264 Media Foundation transform directly:

1. Enumerate hardware transforms only and negotiate D3D11 NV12 input.
2. Require baseline H.264, no B pictures, and a 60-frame GOP configuration.
3. Submit input only when the asynchronous encoder requests it.
4. Receive completed encoded access units, check Annex B NAL boundaries for IDRs,
   and move packet data into the existing capped buffer.
5. On F8, pass buffered H.264 to an MP4 mux writer with identical compressed input
   and output types. Rebase timestamps to zero and start with a real IDR.

GPU capture and scaling use the same helper code as the successful recording
test. This is a new encoding path and must be tested independently. Hardware that
cannot accept the strict codec configuration fails explicitly; no software encoder
fallback is used. One initial output-format change is supported; later format
changes stop recording instead of mixing incompatible packets in a clip.

## Memory and saving behavior

The retained encoded payload cap is 64 MiB, with a 3,600-packet cap and a 30-second
timestamp window. The first limit wins. Whole-GOP eviction can shorten the clip
by roughly a GOP; at startup it saves only footage already retained. A single
encoded access unit is limited to 8 MiB. The GPU input pool contains at most six
samples. Encoder/driver allocations, current encoded packet, mux buffers, desktop
texture, metadata, and allocator overhead are outside the retained-payload cap.
The printed MiB number is NOT total process RAM or GPU memory.

Saving is synchronous in this prototype. Capture pauses while the buffer is muxed,
and the pause duration is reported. The buffer is not cloned, concurrent saves are
not queued, and F8 messages received during a save are discarded. Already submitted
encoder frames remain bounded by input readiness and the sample pool. Encoding
resumes with wall-clock timestamps, so a later clip crossing a save pause can hold
an older frame over that gap. Save can block on slow storage; it has no hard timeout.

This tradeoff gives a bounded first integration. Continuous capture during export
will require an explicitly budgeted export worker before this is production-ready.
The sink writer can have its own buffered data; there is no total-process memory
limit yet. Do not infer FPS overhead from these counters.

## Verify on Windows

- Build and run all CTest cases, including IDR boundary checks and CLI smoke tests.
- Save once during warmup and once after 35 seconds. Verify playable beginnings,
  correct colors/aspect, seeking, end playback, and expected approximate durations.
- Save twice with time between presses; confirm both files work and capture resumes.
- Observe a few minutes of counters and Task Manager memory. Payload should stay
  below 64 MiB; check separately that process RAM and GPU memory stabilize.
- Verify Ctrl+C exits. Keep the short recording test available for comparison.
- Test F8 registration conflict and export to a non-writable folder separately.

Capture remains SDR/unrotated desktop-only, with no dedicated cursor overlay,
audio, display-switch recovery, or automatic game selection. Startup or device
errors terminate with a stage/HRESULT. A save failure is reported and capture
continues where possible; a partial MP4 may remain. Five seconds without encoded
output triggers an error. On exit, encoder draining is bounded to three seconds
before shutdown/flush (driver calls themselves can still block).

## Local validation

The H.264 NAL parser has compile-time assertions for three/four-byte start codes,
IDR vs non-IDR, SPS/PPS, truncated input, forbidden bits, and escaped payloads.
These passed GCC C++20 syntax/constant-evaluation checks on the development Mac.
Linking/running normal C++ tests remains blocked by its missing Apple SDK headers.
The new Windows targets have not been compiled or run in this development session.

## Baseline compatibility correction

The RTX 2060 accepted output/input negotiation and GOP configuration but rejected
setting CODECAPI_AVEncMPVDefaultBPictureCount to zero with E_INVALIDARG. This
property is now optional only because the program verifies that the negotiated
output profile remains Baseline (66), which excludes B pictures. It performs the
same verification after initial format renegotiation, and still rejects
non-increasing packet timestamps. It does not fall back to Main/High profiles or
a software encoder. Windows validation of this correction is pending.
