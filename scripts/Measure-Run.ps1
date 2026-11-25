# Requires Windows PowerShell 5.1 or newer. Run from Command Prompt with powershell -File.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$PresentMon,
    [Parameter(Mandatory=$true)][ValidateRange(1,2147483647)][int]$GameProcessId,
    [Parameter(Mandatory=$true)][ValidateSet('baseline','recording','recording-saves')][string]$Mode,
    [Parameter(Mandatory=$true)][string]$Scenario,
    [ValidateRange(10,3600)][int]$Seconds = 120,
    [ValidateRange(0,120)][int]$Countdown = 15,
    [string]$OutputRoot = ''
)
$ErrorActionPreference = 'Stop'
# Resolve script-relative defaults after parameter binding.
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $scriptDirectory = $PSScriptRoot
    if ([string]::IsNullOrWhiteSpace($scriptDirectory)) {
        $scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
    }
    $OutputRoot = Join-Path $scriptDirectory '..\benchmarks'
}
$inv = [Globalization.CultureInfo]::InvariantCulture
function Number($value) { if ($null -eq $value) { return '' }; return $value.ToString('G17', $inv) }
$pmPath = (Resolve-Path -LiteralPath $PresentMon).Path
$game = Get-Process -Id $GameProcessId
$recorders = @(Get-Process -Name clipping_replay -ErrorAction SilentlyContinue)
if ($Mode -eq 'baseline' -and $recorders.Count -ne 0) { throw 'Stop clipping_replay before a baseline run.' }
if ($Mode -ne 'baseline' -and $recorders.Count -ne 1) { throw 'Start exactly one clipping_replay before a recording run.' }
$targets = @(@{ Role='game'; Process=$game })
if ($recorders.Count -eq 1) { $targets += @{ Role='recorder'; Process=$recorders[0] } }
$logicalCores = [Environment]::ProcessorCount
$runId = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
$folder = New-Item -ItemType Directory -Path (Join-Path $OutputRoot "$runId-$Mode")
$metaPath = Join-Path $folder.FullName 'run.json'
$meta = [ordered]@{ schema=1; status='incomplete'; mode=$Mode; scenario=$Scenario; seconds=$Seconds;
    game_pid=$GameProcessId; game_name=$game.ProcessName; logical_processors=$logicalCores;
    presentmon_sha256=(Get-FileHash -LiteralPath $pmPath -Algorithm SHA256).Hash;
    metric='MsBetweenPresents'; started_utc=$null; error=$null }
$meta | ConvertTo-Json | Set-Content -LiteralPath $metaPath -Encoding UTF8
$pm = $null
$csv = $null
try {
    Write-Host "Measuring $($game.ProcessName) (PID $GameProcessId), $Mode. Return to the same replay segment. Starting in $Countdown seconds."
    Start-Sleep -Seconds $Countdown
    $frames = Join-Path $folder.FullName 'frames.csv'
    $arguments = '--process_id {0} --output_file "{1}" --timed {2} --terminate_after_timed --v1_metrics --no_console_stats --no_track_gpu --no_track_input --no_track_display --session_name clipping-{3}' -f $GameProcessId,$frames,$Seconds,$runId
    $meta.started_utc = [DateTime]::UtcNow.ToString('o')
    $pm = Start-Process -FilePath $pmPath -ArgumentList $arguments -PassThru -RedirectStandardOutput (Join-Path $folder.FullName 'presentmon.stdout.txt') -RedirectStandardError (Join-Path $folder.FullName 'presentmon.stderr.txt')
    # Retain a native handle while PresentMon is alive. Windows PowerShell's
    # Start-Process -PassThru can otherwise lose access to the exit code.
    $presentMonHandle = $pm.Handle
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $csv = [IO.StreamWriter]::new((Join-Path $folder.FullName 'resources.csv'), $false)
    $csv.WriteLine('elapsed_s,role,pid,private_bytes,working_set_bytes,cpu_percent')
    $previous = @{}
    while ($watch.Elapsed.TotalSeconds -lt $Seconds) {
        if ($Mode -eq 'baseline' -and @(Get-Process -Name clipping_replay -ErrorAction SilentlyContinue).Count -ne 0) { throw 'Recorder started during baseline.' }
        foreach ($target in $targets) {
            $proc = $target.Process
            $proc.Refresh()
            if ($proc.HasExited) { throw "$($target.Role) process exited during measurement." }
            $time = $watch.Elapsed.TotalSeconds
            $cpu = $proc.TotalProcessorTime.TotalSeconds
            $percent = $null
            if ($previous.ContainsKey($target.Role)) {
                $prev = $previous[$target.Role]
                $percent = 100.0 * ($cpu - $prev.Cpu) / (($time - $prev.Time) * $logicalCores)
            }
            $previous[$target.Role] = @{Cpu=$cpu; Time=$time}
            $csv.WriteLine(('{0},{1},{2},{3},{4},{5}' -f (Number $time),$target.Role,$proc.Id,$proc.PrivateMemorySize64,$proc.WorkingSet64,(Number $percent)))
        }
        $pm.Refresh()
        if ($pm.HasExited -and $watch.Elapsed.TotalSeconds -lt ($Seconds - 1)) { throw 'PresentMon exited early. Read presentmon.stderr.txt.' }
        Start-Sleep -Milliseconds 1000
    }
    if (-not $pm.WaitForExit(15000)) { throw 'PresentMon did not finish within the timeout.' }
    $exitCode = $pm.ExitCode
    if ($null -eq $exitCode) { throw 'PresentMon ended, but its exit code could not be retrieved. Keep this run folder for inspection.' }
    if ($exitCode -ne 0) { throw "PresentMon exited with code $exitCode. Read its logs." }
    if (-not (Test-Path -LiteralPath $frames) -or (Get-Item -LiteralPath $frames).Length -eq 0) { throw 'No frame CSV was produced.' }
    $meta.status = 'complete'
    Write-Host "Saved measurement: $($folder.FullName)"
} catch {
    $meta.status = 'failed'
    $meta.error = $_.Exception.Message
    throw
} finally {
    if ($null -ne $csv) { $csv.Dispose() }
    if ($null -ne $pm) {
        if (-not $pm.HasExited) { $pm.Kill(); $pm.WaitForExit() }
        $pm.Dispose()
    }
    $meta | ConvertTo-Json | Set-Content -LiteralPath $metaPath -Encoding UTF8
}
