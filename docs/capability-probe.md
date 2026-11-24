# Windows hardware capability probe

## Run

Use a local Windows 10/11 desktop session with GPU drivers installed. Build from
Developer PowerShell for VS 2022:

```powershell
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\clipping_capabilities.exe | Tee-Object -FilePath capabilities.txt
$LASTEXITCODE
```

If the build command fails, stop and share that output rather than running an old
binary. The new executable is separate from the synthetic `clipping_probe`.

## What is tested

- DXGI hardware adapters, vendor/device IDs, dedicated memory capacity.
- Globally registered hardware H.264 Media Foundation transforms. Software
  encoders are deliberately excluded; there is no silent software fallback.
- For every adapter/encoder pair, each of three fixed presets is tried using a
  fresh encoder activation: 720p30 at 6 Mbps, 1080p60 at 12 Mbps, 1440p60 at 20 Mbps.
- D3D11 awareness, acceptance of the adapter's DXGI device manager, actual output
  and input type configuration, and submission of one GPU-backed NV12 sample.
- Asynchronous readiness is awaited before input, with a three-second event wait
  per candidate. Driver API calls themselves are not forcibly time-limited.

The sample contains synthetic black pixels. A CPU upload initializes that one
texture; the probe is not a benchmark of a GPU-only capture pipeline. It reads no
screen contents or audio and the executable itself writes no files. PowerShell's
`Tee-Object` saves the displayed report. The report is ignored by Git.

## Interpret the report

`GPU-backed sample: ACCEPTED` means `ProcessInput` accepted one texture sample
for that adapter/encoder/preset configuration. No output is drained or decoded.
It does not prove sustained encoding, zero-copy internals, image quality, actual
bitrate/frame rate, or support for capture on a different adapter.

`NOT VERIFIED` includes the exact stage and HRESULT. A failure applies to this
probe's particular API, NV12 texture configuration, and preset; it is not proof
that the GPU cannot record video using other APIs/formats. Global encoder
registration names do not establish physical GPU ownership. Cross-adapter pairs
can fail or introduce internal copies even when they accept a device manager.

Dedicated video memory is capacity, not current use. Integrated GPUs may report
little or no dedicated memory. Payload buffers and process RAM are not measured.

Exit 0 means at least one candidate accepted input. Exit 2 means none did. Exit 1
means the probe could not complete. On Windows N, Media Foundation may need the
Media Feature Pack; an OS missing a required DLL may fail before diagnostics run.

## Validation still required

This source has not yet been compiled or run on Windows in the development
session. Windows CI builds the target but does not run hardware checks: hosted
runners do not represent users' GPUs. Run it on the test PC and retain the report.
Verify both success and clean diagnostic exit when no hardware MFT is available.

The next milestone must encode/drain actual output and decode a short recording
before treating any candidate as a working recording backend.

## API references

- [MFTEnumEx hardware enumeration](https://learn.microsoft.com/en-us/windows/win32/api/mfapi/nf-mfapi-mftenumex)
- [D3D11 awareness](https://learn.microsoft.com/en-us/windows/win32/medfound/mf-sa-d3d11-aware)
- [Asynchronous transforms](https://learn.microsoft.com/en-us/windows/win32/medfound/asynchronous-mfts)
- [GPU surface buffers](https://learn.microsoft.com/en-us/windows/win32/api/mfapi/nf-mfapi-mfcreatedxgisurfacebuffer)
- [Device manager and threading](https://learn.microsoft.com/en-us/windows/win32/api/mfobjects/nf-mfobjects-imfdxgidevicemanager-resetdevice)
