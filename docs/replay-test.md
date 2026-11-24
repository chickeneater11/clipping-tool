# Replay recording with background export

The synchronous replay version was verified by the user on an RTX 2060: playable
30-second clip, roughly 40-42 MiB retained payload, 137 ms save pause. This revision
moves export off the capture thread. **Its Windows build and runtime tests are
pending.** The banner is `Clipping replay: background-export-v3`.

## Build and run

Transfer the whole updated repo (including `shared_replay_buffer.hpp`), then run
from the repo root in Command Prompt or Developer PowerShell:

```text
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
build\Release\clipping_replay.exe
```

This records the entire first display on adapter 0, at 1080p60 / 12 Mbps, without
audio. No video is written to disk until F8 is pressed. Optional output folder:
`clipping_replay.exe "D:\Game Clips"`. Default is `clips` in the working directory.

Let it warm up for at least 35 seconds, press **F8**, keep visible motion going,
and wait for `Saved ...`. Press F8 again a few seconds later to create a second
clip that spans the first save. **Ctrl+C** stops capture and waits for any active
save to finish. Exiting does not automatically save otherwise-unsaved footage.
F8 registration fails explicitly if another application already owns it.

## Export and memory policy

The live ring and one export snapshot share immutable encoded packets. Creating
a snapshot copies packet references and format metadata, not the whole video.
The worker receives serialized format data and creates its own COM/Media Foundation
objects. It never calls the live encoder from another thread. Status is reported
on the capture thread when the completed result is polled.

- **64 MiB shared payload cap:** counts each retained vector capacity once, across
  the live ring and the export snapshot, until its final owner releases it.
- **48 MiB export pin limit:** trims oldest complete GOPs when needed, leaving at
  least 16 MiB of the shared budget available for newer footage.
- **One active export:** no job queue. F8 requests during an active export are
  ignored. Success or failure releases the snapshot and allows a later retry.
- **3,600 live packets / 30-second window:** retention may shorten under memory
  pressure. A snapshot also has at most 3,600 references.
- **8 MiB maximum incoming access unit**, and at most six GPU input pool samples.

If a slow export holds old packets while recording needs space, live retention
is shortened by whole GOPs. If a frame still cannot fit, it is dropped and the
live buffer waits for the next IDR. The exported snapshot remains unchanged.
This bounds retained payload rather than blocking capture for storage completion.

The 64 MiB limit is NOT a total-process RAM or VRAM guarantee. Incoming/output
packets, mux copies/queues, codec format data (bounded at 1 MiB), driver/encoder
allocations, shared-pointer metadata, and GPU surfaces are outside it. The worker
still copies one packet at a time into Media Foundation mux samples; sink-writer
internal buffering is separate. Capture can still lose frames due to CPU/GPU/disk
contention, snapshot dispatch cost, or encoder pressure. This change removes the
explicit synchronous save wait; it does not guarantee zero frame drops.

Saving uses unique filenames and never overwrites existing clips. A failed save
can leave an incomplete file, but the worker releases its snapshot and recording
continues. Shutdown waits for an active save, including if capture errors out.
A stuck driver or storage call can therefore delay process exit; there is no
unsafe forced thread termination or hard storage timeout.

## Reading diagnostics

Every five seconds:

- `MiB live`: packet payload referenced by the current ring.
- `MiB shared`: unique payload across both recording and export; must stay <=64.
- `export=active/idle`: whether an export result is still pending.
- `pressure drops`: packets rejected because pinned data prevented admission.
- `input/output`: submitted GPU frames / completed encoded access units.
- `skipped slots`: elapsed scheduling slots the capture loop missed.
- `busy slots`: input skipped while encoder/pool was unavailable.
- `waiting-IDR/rejected`: packets not retained while awaiting a decodable start.

On F8, `snapshot/dispatch (ms)` measures work remaining on the capture thread.
On success, `background export time (s)` measures muxing/finalization on the worker;
it is not a capture pause. Short saves may finish between periodic status reports.
These counters are not game FPS or total process memory measurements.

## Windows validation checklist

1. Build and run CTest, including `shared_replay_buffer` accounting/concurrency tests.
2. Warm up 35 seconds, save, then save again a few seconds later. Both clips must
   start cleanly, seek, and play to the end. The second clip should include smooth
   motion across the first save rather than the former 137 ms freeze.
3. Compare skipped/busy slots before and after saves. Some scheduling variation is
   possible; report both dispatch time and background export time.
4. Run for several minutes and inspect Task Manager separately. `MiB shared`
   must remain <=64; total process RAM and GPU usage should stabilize.
5. If a save lasts long enough, press F8 again: it should report that it is already
   active. On slower storage, retention may shorten; the shared cap must still hold.
6. Stop with Ctrl+C during a save: capture should stop, the save should complete,
   and the process should exit cleanly. Check the resulting clip.
7. Exercise a save failure using a non-writable destination in a separate run;
   verify an error is reported and subsequent recording continues.

## Encoding and compatibility

The capture path uses Desktop Duplication and D3D11 scaling/color conversion.
The replay path directly drives an asynchronous hardware H.264 MFT and exports
compressed packets to MP4 without re-encoding. It verifies IDRs from Annex B NAL
boundaries, rebases clip timestamps, and requires strictly increasing output PTS.

Baseline profile (66) is required and verified. A separate B-picture setting is
optional because the RTX 2060 driver rejects it while accepting Baseline, which
excludes B pictures. The GOP-size request remains required. No software encoder
fallback is used. One initial output-format change is supported; midstream changes
stop recording rather than mixing codec configurations.

SDR/unrotated display capture only for now. No audio, dedicated cursor overlay,
automatic game selection, cross-adapter capture, or display-change recovery.
Capture/encoder errors terminate with a stage and HRESULT. Five seconds without
encoded output triggers an error; exit attempts a three-second encoder drain.

## Validation status

The previous synchronous path was confirmed playable on the user's Windows PC.
New tests cover shared accounting without payload duplication, single-export
admission, GOP trimming, pin pressure, release after failure, snapshots outliving
the producer, and concurrent snapshot release while the live ring mutates.
These new tests could not be compiled locally: the development Mac is missing
Apple SDK headers (`sys/_types/_int8_t.h`). Windows tests remain pending. Existing
H.264 compile-time assertions previously passed with GCC C++20.
