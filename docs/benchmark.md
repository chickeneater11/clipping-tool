# Game FPS and process memory measurements

The recorder's counters measure its own scheduling, not game FPS. Its encoded
payload cap also excludes textures, drivers, Media Foundation and other process
allocations. This workflow measures actual game present intervals and samples
process memory/CPU once per second. No recorder rebuild is required.

## Windows setup (Command Prompt)

Download the x64 **console executable** from the official
[PresentMon releases](https://github.com/GameTechDev/PresentMon/releases).
Use a release supporting `--v1_metrics`. The collector pins that CSV schema and
records the executable SHA256 so paired runs use the same version. CLI options
are documented in [PresentMon's console guide](https://github.com/GameTechDev/PresentMon/blob/main/README-ConsoleApplication.md).
Do not run another performance overlay/collector for just one side of a pair.

Open Command Prompt in the repository. Once the League replay/game is running:

```bat
tasklist /FI "IMAGENAME eq League of Legends.exe"
```

Use the **game PID**, not LeagueClient or RiotClient. Replace `12345` and the
PresentMon path below with your values. A restarted game may have a different PID.
The script does not start or stop your game or recorder. If ETW reports access
denied, run Command Prompt as administrator and retry; inspect the saved stderr
log for other errors. If PowerShell blocks the script, review it and follow your
machine's script-signing policy; the command does not change execution policy.

## League comparison

1. Choose a replay segment with substantial action. Keep replay file, patch,
   playback speed, camera target, resolution, graphics settings, frame cap and
   overlays identical. Record these in the scenario string and use that exact
   string in paired runs. Use the same PC, power settings and drivers.
2. Warm up the game. Stop clipping_replay for the baseline. Seek to the same
   point before each run, account for the 15-second countdown, and keep the game
   focused throughout measurement. Repeatable camera/playback matters more than
   comparing two unrelated live matches. If replay viewing isn't available,
   use a fixed practice sequence and expect more variation.
3. Run the baseline command below, return to the game during the countdown.
4. Start `build\Release\clipping_replay.exe` in another terminal; let it fill for
   at least 35 seconds. Repeat the same scene with the recording command. Do not
   save clips in this initial comparison. Ensure the game is on the display
   currently captured by replay (adapter 0 / output 0).
5. Alternate baseline/recording for **three pairs**. Keep each result separate.
   A frame cap can conceal average FPS loss: inspect 1% lows/p99 too, then test
   uncapped separately with a distinct scenario label.

```bat
powershell -NoProfile -File scripts\Measure-Run.ps1 -PresentMon "C:\Tools\PresentMon.exe" -GameProcessId 12345 -Mode baseline -Scenario "LoL replay1 10m camera-player 1080p high cap144" -Seconds 120
powershell -NoProfile -File scripts\Measure-Run.ps1 -PresentMon "C:\Tools\PresentMon.exe" -GameProcessId 12345 -Mode recording -Scenario "LoL replay1 10m camera-player 1080p high cap144" -Seconds 120
```

Do not paste both at once: follow steps 3 and 4 between commands. The scenario
above is an example; change it to accurately describe your actual test.

## Longer memory and save tests

Run recording for `-Seconds 600` after warming up. Use a representative workload
and inspect memory over the entire period. Run a separate `-Mode recording-saves`
test pressing F8 at a repeatable cadence (for example every 30 seconds); note
save times and keep the recorder console output. Saves are manual, not automated.
Verify the saved clips play properly. Stop replay with Ctrl+C after collection.

The unique `benchmarks\<timestamp-id-mode>` folders contain raw `frames.csv`,
`resources.csv`, `run.json` and PresentMon logs. Failed/interrupted runs are
marked incomplete or failed and refused by the analyzer. Keep all files together.
They are ignored by Git. Share those folders for analysis, or, with Python 3:

```bat
py scripts\analyze_benchmark.py "benchmarks\BASELINE-FOLDER" "benchmarks\RECORDING-FOLDER" > comparison.json
py scripts\analyze_benchmark.py "benchmarks\SOAK-FOLDER" > soak.json
```

## Metric definitions and limits

- Average FPS = 1000 / mean valid positive `MsBetweenPresents` interval.
- 1% low = 1000 / mean of the slowest ceil(1% of intervals). This is explicitly
  our definition; tools using a percentile FPS can report different values.
- p95/p99 frame times use linear interpolation between sorted intervals.
- Measurements are application presents, not physical display FPS. Disable
  frame generation for comparisons. Dropped presentations are included.
- A process can have multiple swapchains. The analyzer selects the largest by
  sample count, reports all counts, and blocks automatic comparisons when this
  is ambiguous. Inspect single runs with `--swapchain ADDRESS`; do not merge
  swapchains. Investigate warnings rather than treating partial captures as wins.
- Private MiB is private committed memory; working set is resident process RAM
  and can include shared pages. Neither is GPU VRAM. CPU percent is normalized
  over logical processors (100% means all logical processors busy).
- Resource peaks are **sampled**, not guaranteed instantaneous peaks. First/last
  30-second median private-memory difference describes growth, not proof of a
  leak. A longer soak and repeated runs are needed if growth continues.
- PresentMon and the sampler themselves have overhead; run identical collection
  on both sides. GPU usage/VRAM and game-specific behavior on other machines
  remain unmeasured. One RTX 2060 result cannot establish universal overhead.

Tests for analysis math and invalid captures:
`python -m unittest discover -s tests -p "test_benchmark*.py"`.
