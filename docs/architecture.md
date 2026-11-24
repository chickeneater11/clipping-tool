# Architecture and next milestones

## Product constraints

Windows is the runtime target. macOS/Linux can build and test the portable core.
Keep recording independent from any future settings UI. No live preview is needed
for the first recorder. Prefer bounded work and explicit failures over silent
software encoding or ever-growing queues.

## Intended recording path (not implemented yet)

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
- [ ] Windows capability probe: adapters, hardware H.264 encoders, supported input
      formats, D3D11 acceptance, actionable failure output.
- [ ] Capture one selected display/window and encode a short video-only recording.
- [ ] Verify decoded output and timestamps on NVIDIA, AMD, and Intel machines.
- [ ] Integrate replay buffer and bounded hotkey export. Bound concurrent exports
      and account for retained export payloads in the global memory budget.
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
