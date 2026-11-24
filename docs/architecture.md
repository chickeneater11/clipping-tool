# Architecture and next milestones

## Product constraints

Windows is the runtime target. macOS/Linux can build and test the portable core.
Keep recording independent from any future settings UI. No live preview is needed
for the first recorder. Prefer bounded work and explicit failures over silent
software encoding or ever-growing queues.

## Intended product recording path

The first MP4 test uses Desktop Duplication and the Media Foundation sink writer
as a smaller integration step. Its code lives in `src/windows/record_test.cpp`.
The replay product path below is still planned; it will need direct access to
encoded packets rather than the sink writer owning the encoder.

1. Windows.Graphics.Capture acquires a selected window/display into D3D11 surfaces.
2. GPU color conversion/scaling prepares a supported encoder input format.
3. Enumerate hardware H.264 Media Foundation transforms and verify actual device,
   format, and D3D11 interoperability. Report unsupported configurations explicitly.
   Hardware enumeration alone does not prove a zero-copy pipeline.
4. Encode with no B frames and periodic independently decodable IDRs, initially
   targeting 1080p60 / 12 Mbps / 30 seconds. These are prototype settings, not
   established optimal defaults for all hardware.
5. Feed encoded video into the bounded replay buffer.
6. A hotkey requests export; a later bounded export mechanism muxes packets into
   a container without re-encoding.

Keep capture/encoder queues small and bounded. Drop raw capture frames when busy;
do not arbitrarily discard already encoded dependent frames. Request an IDR after
encoded discontinuities. Do not elevate recording to real-time process priority.

## Delivery sequence

- [x] Portable replay buffer, synthetic probe, tests, build configuration.
- [x] Windows capability probe (GPU sample acceptance verified by the user on RTX 2060): adapters, hardware H.264 encoders, supported input
      formats, D3D11 acceptance, actionable failure output.
- [x] Implement a selected-display video-only MP4 recording test using Desktop Duplication and the sink writer.
- [x] User verified playable recording on Windows / RTX 2060.
- [ ] Verify decoded output and timestamps on NVIDIA, AMD, and Intel machines.
- [x] Implement direct hardware encoding, capped replay retention, and synchronous F8 export.
- [ ] Verify the replay path on Windows, then budget an export worker for uninterrupted capture.
- [ ] Add WASAPI game/microphone audio, common clock synchronization, device changes.
- [ ] Benchmark frame times and memory before adding a tray/settings interface.

A capture frame pool and a small encoder queue consume additional GPU/system
memory. Resolution changes must drain/reset the stream and update codec metadata.
Protected content, minimized windows, capture denial, GPU resets, and unsupported
hardware need explicit behavior before release.

## Primary API references

- [Windows screen capture](https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture)
- [Hardware Media Foundation transforms](https://learn.microsoft.com/en-us/windows/win32/medfound/hardware-mfts)
- [D3D11-aware transform attribute](https://learn.microsoft.com/en-us/windows/win32/medfound/mf-sa-d3d11-aware)
