# Performance validation plan

Do not advertise an FPS overhead percentage until the actual capture/encode path
has been measured. The synthetic probe tests retention behavior only.

## Reproducible comparison

Use the same game build, settings, resolution, scene/replay, drivers, power mode,
and frame cap. Warm up first. Alternate baseline and recording runs, at least
three runs of each; record run duration and results individually. Test both
GPU-saturated uncapped gameplay and capped gameplay with GPU headroom.

Collect average FPS, 1% lows (document the calculation), p95/p99 frame time,
process private bytes, working set, GPU memory, CPU utilization, encoder usage,
captured/encoded/dropped frames, queue depth, and actual retained clip duration.
Record clip quality and audio sync once audio exists. Track export-time spikes
separately from steady-state recording; retain raw measurements.

Exercise 720p30, 1080p60, and 1440p60 across NVIDIA, AMD, and Intel hardware,
including integrated graphics and systems with 8/16/32 GB RAM where available.
Check laptops with different rendering/encoding adapters. Coverage is a target,
not a claim of support.

## Required scenarios

- Ten-minute steady recording plus a longer soak for memory growth.
- Repeated saves and rapid hotkeys while playing.
- Full buffer, bitrate spikes, oversized keyframes, missing IDRs.
- Alt-tab, resizing, minimized target, sleep/resume, device reset.
- Disk full, slow destination, denied capture, lost audio device.

## Acceptance principles

Payload and packet limits must always hold. Process memory must stabilize during
a steady-state soak; the payload cap alone does not establish that. No unbounded
capture or export queues. A slow export must not block capture indefinitely.
Compare frame-time distributions as well as averages and report hardware-specific
results; do not interpret a synthetic buffer run as gameplay performance evidence.
