<#
.SYNOPSIS
  Media Player Pro soak / stress test: run the player for hours and catch what only time
  shows - memory or handle leaks, GPU memory creep, slow-down, hangs, crashes, and files
  (logs, snapshots) that grow without bound on a station left running on set.

.DESCRIPTION
  Launches Media Player Pro WINDOWED (a fullscreen window on the panel cannot be got
  past for hours) and samples it every -SampleSec:
    process   private bytes, working set, handles, GDI + USER objects, threads, CPU %
    GPU       the process's dedicated + shared GPU memory, 3D / copy / video-decode engine %
    player    station_status.json heartbeat (seq), file position (playback keeping real
              time), DeckLink delivered fps / dropped / late / unpaired / skew when live
    disk      the runtime log's size, the station folder's size (snapshots, alerts)

  -Churn drives the player the way a session on set does, over and over, because leaks
  live in repeated operations rather than in steady playback. Actions go through the
  same channels the Resolve panel uses (resolve_link.json: open / seek / play / pause,
  resolve_commands.jsonl: snapshot) plus a posted Tab key (UI shown / hidden):
    light   UI toggle, seek, pause + play
    heavy   light + open the next -Clip, and a snapshot every 10th action

  At the end it fits a straight line to each metric after -WarmupMinutes and reports the
  growth per hour, plus the first-vs-last-window change, against the limits below. Exit
  code 0 = PASS, 1 = FAIL, 2 = could not run.

  Output folder (Desktop\soak_<stamp> by default): samples.csv, events.csv, summary.txt,
  summary.json, the runtime log, stdout, and a zip of all of it.

.EXAMPLE
  # 8 hours of one looping clip, no churn (the "left playing overnight" case)
  powershell -ExecutionPolicy Bypass -File soak_stress.ps1 -Clip D:\clips\sbs.mov -Minutes 480

.EXAMPLE
  # 2 hours of heavy churn across three clips
  powershell -ExecutionPolicy Bypass -File soak_stress.ps1 -Clip a.mov,b.mp4,c.mxf -Churn heavy -Minutes 120

.EXAMPLE
  # 4 hours of two DeckLink inputs (station loopback: Left 2 = (3), Right 0 = (1))
  powershell -ExecutionPolicy Bypass -File soak_stress.ps1 -DeckLink -Left 2 -Right 0 -Minutes 240

.EXAMPLE
  # Re-analyse an earlier run with other limits
  powershell -ExecutionPolicy Bypass -File soak_stress.ps1 -Analyze -Out C:\Users\me\Desktop\soak_20261109_120000
#>
param(
    [string]$Exe,
    [string[]]$Clip,
    [switch]$DeckLink,
    [string]$Left = "0",
    [string]$Right = "",              # "" = one input
    [string]$Connection = "sdi",
    [double]$Minutes = 60,            # hard cap: the player is closed when it runs out
    [int]$SampleSec = 10,
    [ValidateSet("none", "light", "heavy")][string]$Churn = "none",
    [int]$ChurnSec = 30,
    [double]$WarmupMinutes = 5,       # excluded from the trend (caches, pools, first decode)
    [string]$Window = "1280x720",
    [switch]$Fullscreen,
    [string]$Out,
    [hashtable]$Env,
    [switch]$Analyze,
    # Limits (growth per hour after warm-up, by least squares)
    [double]$MaxPrivateMBph = 50,
    [double]$MaxGpuMBph = 64,
    [double]$MaxHandlesPh = 200,
    [double]$MaxGuiPh = 20,
    [double]$MaxThreadsPh = 10,
    [double]$MaxLogMBph = 50,
    [double]$MinPlaybackRate = 0.98,  # file: media seconds per wall second (median, last window)
    [double]$MinFpsShare = 0.98,      # live: delivered / signal fps (median, last window)
    [double]$MaxDropShare = 0.005,    # file: frames dropped before shown / frames shown
    [double]$MaxLateShare = 0.01,     # file: frames shown > 1.5 frame times late / frames shown
    [double]$MinTrendMinutes = 30,    # shorter runs report leak trends as INFO, not PASS/FAIL
    [double]$MaxErrorsPerMin = 1,     # runtime [ERROR] lines per minute, whole run and last window
    [int]$HangSec = 15                # no status heartbeat for this long = a hang
)

$ErrorActionPreference = "Stop"
$inv = [Globalization.CultureInfo]::InvariantCulture
function F([double]$v, [int]$d = 2) { return $v.ToString("F$d", $inv) }

# ------------------------------------------------------------------ analysis
function Fit-Slope($xs, $ys) {
    # least-squares slope of y over x; x in hours
    $n = $xs.Count
    if ($n -lt 3) { return 0.0 }
    $mx = ($xs | Measure-Object -Average).Average
    $my = ($ys | Measure-Object -Average).Average
    $sxy = 0.0; $sxx = 0.0
    for ($i = 0; $i -lt $n; $i++) { $dx = $xs[$i] - $mx; $sxy += $dx * ($ys[$i] - $my); $sxx += $dx * $dx }
    if ($sxx -le 0) { return 0.0 }
    return $sxy / $sxx
}
function Median($v) {
    $a = @($v | Sort-Object)
    if ($a.Count -eq 0) { return [double]::NaN }
    $m = [int][math]::Floor($a.Count / 2)
    if ($a.Count % 2) { return [double]$a[$m] }
    return ([double]$a[$m - 1] + [double]$a[$m]) / 2
}

function Analyze-Run([string]$dir) {
    $csv = Join-Path $dir "samples.csv"
    if (-not (Test-Path $csv)) { Write-Host "No samples.csv in $dir" -ForegroundColor Red; return 2 }
    $rows = @(Import-Csv $csv)
    $events = @(); $ev = Join-Path $dir "events.csv"
    if (Test-Path $ev) { $events = @(Import-Csv $ev) }
    $results = New-Object System.Collections.ArrayList
    function Add-Result([string]$what, [string]$verdict, [string]$detail) {
        $null = $results.Add([pscustomobject]@{ check = $what; result = $verdict; detail = $detail })
    }
    $num = { param($r, $k) [double]::Parse($r.$k, $inv) }
    $total = if ($rows.Count) { (& $num $rows[-1] "t_s") / 60 } else { 0 }
    $warm = [math]::Min($WarmupMinutes, $total / 3)   # a short run still gets a trend
    $body = @($rows | Where-Object { (& $num $_ "t_s") / 60 -ge $warm })
    Add-Result "Run" "INFO" ("{0} min, {1} samples ({2} after a {3} min warm-up), churn {4}" -f (F $total 1), $rows.Count, $body.Count, (F $warm 1), $(if ($rows.Count) { $rows[0].churn } else { "?" }))

    $crash = @($events | Where-Object { $_.kind -eq "exit" })
    $hangs = @($events | Where-Object { $_.kind -eq "hang" })
    if ($crash.Count) { Add-Result "Crash" "FAIL" ("player exited at {0} min: {1}" -f (F ([double]::Parse($crash[0].t_s, $inv) / 60) 1), $crash[0].detail) }
    else { Add-Result "Crash" "PASS" "ran to the end" }
    if ($hangs.Count) { Add-Result "Hang" "FAIL" ("{0} stall(s) of the status heartbeat; first at {1} min ({2})" -f $hangs.Count, (F ([double]::Parse($hangs[0].t_s, $inv) / 60) 1), $hangs[0].detail) }
    else { Add-Result "Hang" "PASS" "status heartbeat never stopped for $HangSec s" }

    if ($body.Count -lt 6) { Add-Result "Trend" "SKIP" "too few samples after warm-up for a trend"; return Finish-Analysis $dir $results }
    $xs = @($body | ForEach-Object { (& $num $_ "t_s") / 3600 })
    $win = [math]::Max(3, [int]($body.Count / 6))
    $trend = @(
        @{ k = "private_mb";  name = "Private memory"; unit = "MB"; lim = $MaxPrivateMBph },
        @{ k = "gpu_ded_mb";  name = "GPU dedicated memory"; unit = "MB"; lim = $MaxGpuMBph },
        @{ k = "gpu_shr_mb";  name = "GPU shared memory"; unit = "MB"; lim = $MaxGpuMBph },
        @{ k = "handles";     name = "Handles"; unit = ""; lim = $MaxHandlesPh },
        @{ k = "gdi";         name = "GDI objects"; unit = ""; lim = $MaxGuiPh },
        @{ k = "user";        name = "USER objects"; unit = ""; lim = $MaxGuiPh },
        @{ k = "threads";     name = "Threads"; unit = ""; lim = $MaxThreadsPh },
        @{ k = "log_mb";      name = "Runtime log size"; unit = "MB"; lim = $MaxLogMBph },
        @{ k = "working_mb";  name = "Working set"; unit = "MB"; lim = -1 },
        @{ k = "station_mb";  name = "Station folder"; unit = "MB"; lim = -1 }
    )
    foreach ($t in $trend) {
        $ys = @($body | ForEach-Object { & $num $_ $t.k })
        $slope = Fit-Slope $xs $ys
        $first = Median ($ys | Select-Object -First $win); $last = Median ($ys | Select-Object -Last $win)
        $detail = "{0} {4}/h  (first {1} -> last {2}, peak {3})" -f (F $slope 1), (F $first 1), (F $last 1), (F (($ys | Measure-Object -Maximum).Maximum) 1), $t.unit
        if ($t.lim -lt 0) { Add-Result $t.name "INFO" $detail }
        elseif (($xs[-1] - $xs[0]) * 60 -lt $MinTrendMinutes) { Add-Result $t.name "INFO" ($detail + "; run too short to judge (< $MinTrendMinutes min after warm-up)") }
        # both the fit and the first-vs-last change must exceed the limit: a single spike
        # or a sawtooth GC pattern does not fail the run, a steady climb does
        elseif ($slope -gt $t.lim -and ($last - $first) -gt $t.lim * ($xs[-1] - $xs[0]) * 0.5) { Add-Result $t.name "FAIL" ($detail + "; limit $($t.lim)/h") }
        else { Add-Result $t.name "PASS" $detail }
    }
    $cpu = @($body | ForEach-Object { & $num $_ "cpu_pct" })
    Add-Result "CPU" "INFO" ("median {0}% first window, {1}% last window (100% = one core)" -f (F (Median ($cpu | Select-Object -First $win)) 1), (F (Median ($cpu | Select-Object -Last $win)) 1))
    foreach ($e in "gpu_3d", "gpu_copy", "gpu_vdec") {
        $ys = @($body | ForEach-Object { & $num $_ $e })
        Add-Result ("GPU engine " + $e.Substring(4)) "INFO" ("median {0}% first window, {1}% last window" -f (F (Median ($ys | Select-Object -First $win)) 1), (F (Median ($ys | Select-Object -Last $win)) 1))
    }

    # Is the picture keeping up, and did it get worse with time?
    $live = @($body | Where-Object { (& $num $_ "signal_fps") -gt 0 })
    if ($live.Count -ge 6) {
        $share = @($live | ForEach-Object { (& $num $_ "delivered_fps") / (& $num $_ "signal_fps") })
        $lw = [math]::Max(3, [int]($live.Count / 6))
        $s0 = Median ($share | Select-Object -First $lw); $s1 = Median ($share | Select-Object -Last $lw)
        $minS = ($share | Measure-Object -Minimum).Minimum
        $drop0 = & $num $live[0] "dropped"; $drop1 = & $num $live[-1] "dropped"
        $late0 = & $num $live[0] "late"; $late1 = & $num $live[-1] "late"
        $unp0 = & $num $live[0] "unpaired"; $unp1 = & $num $live[-1] "unpaired"
        $skew = ($live | ForEach-Object { & $num $_ "skew_ms" } | Measure-Object -Maximum).Maximum
        $d = "delivered/signal median {0} first, {1} last, min {2}; dropped +{3}, late +{4}, unpaired +{5}, max skew {6} ms" -f (F $s0 3), (F $s1 3), (F $minS 3), ($drop1 - $drop0), ($late1 - $late0), ($unp1 - $unp0), (F $skew 1)
        if ($s1 -lt $MinFpsShare -or $s1 -lt $s0 - 0.02) { Add-Result "Live rate" "FAIL" $d } else { Add-Result "Live rate" "PASS" $d }
    }
    $file = @($body | Where-Object { $_.f_dwall })
    if ($file.Count -ge 6) {
        $fw = [math]::Max(3, [int]($file.Count / 6))
        $sumRate = { param($rs) $m = 0.0; $w = 0.0; foreach ($r in $rs) { $m += & $num $r "f_dmedia"; $w += & $num $r "f_dwall" }; if ($w -gt 0) { $m / $w } else { 0 } }
        $r0 = & $sumRate @($file | Select-Object -First $fw); $r1 = & $sumRate @($file | Select-Object -Last $fw)
        $rAll = & $sumRate $file
        # worst one-minute stretch: consecutive valid pairs summed over ~60 s
        $k = [math]::Max(2, [int](60 / $SampleSec)); $worst = 9.9
        for ($i = 0; $i + $k -le $file.Count; $i++) { $v = & $sumRate @($file[$i..($i + $k - 1)]); if ($v -lt $worst) { $worst = $v } }
        if ($worst -gt 9) { $worst = $rAll }
        $d = "media s per wall s: {0} overall, {1} first window, {2} last window, worst minute {3}" -f (F $rAll 3), (F $r0 3), (F $r1 3), (F $worst 3)
        if ($r1 -lt $MinPlaybackRate -or $r1 -lt $r0 - 0.02) { Add-Result "Playback rate" "FAIL" $d } else { Add-Result "Playback rate" "PASS" $d }
    } elseif ($live.Count -lt 6) {
        Add-Result "Playback rate" "SKIP" "no playing file or live signal in the samples (paused the whole run?)"
    }
    # Dropped / late frames during file playback (counters from the player, cumulative).
    $fc = @($body | Where-Object { $_.f_frames })
    if ($fc.Count -ge 2) {
        $sums = { param($rs) $fr = 0.0; $dr = 0.0; $lt = 0.0
            for ($i = 1; $i -lt $rs.Count; $i++) {
                $a = & $num $rs[$i] "f_frames"; $b = & $num $rs[$i - 1] "f_frames"
                if ($a -lt $b) { continue }   # the player restarted its counters
                $fr += $a - $b
                $dr += (& $num $rs[$i] "f_dropped") - (& $num $rs[$i - 1] "f_dropped")
                $lt += (& $num $rs[$i] "f_late") - (& $num $rs[$i - 1] "f_late")
            }
            @{ fr = $fr; dr = $dr; lt = $lt } }
        $all = & $sums $fc
        $cw = [math]::Max(3, [int]($fc.Count / 6))
        $lastW = & $sums @($fc | Select-Object -Last $cw)
        $share = { param($x, $key) if ($x.fr -gt 0) { $x[$key] / $x.fr } else { 0 } }
        $dS = & $share $all "dr"; $lS = & $share $all "lt"; $dL = & $share $lastW "dr"; $lL = & $share $lastW "lt"
        $d = "{0} frames shown; dropped {1} ({2}%), late {3} ({4}%); last window dropped {5}%, late {6}%" -f $all.fr, $all.dr, (F (100 * $dS) 3), $all.lt, (F (100 * $lS) 3), (F (100 * $dL) 3), (F (100 * $lL) 3)
        if ($dS -gt $MaxDropShare -or $lS -gt $MaxLateShare -or $dL -gt $MaxDropShare -or $lL -gt $MaxLateShare) {
            Add-Result "Dropped/late frames" "FAIL" ($d + "; limits " + (F (100 * $MaxDropShare) 2) + "% / " + (F (100 * $MaxLateShare) 2) + "%")
        } else { Add-Result "Dropped/late frames" "PASS" $d }
    } elseif ($file.Count -ge 6) {
        Add-Result "Dropped/late frames" "SKIP" "this player build does not report frame counters (needs file.frames in station_status.json)"
    }
    # Runtime errors / display hotplugs (columns exist from this harness version on).
    if ($rows.Count -and ($rows[0].PSObject.Properties.Name -contains "rt_errors")) {
        $errAll = 0.0; foreach ($r in $body) { $errAll += & $num $r "rt_errors" }
        $ew = [math]::Max(3, [int]($body.Count / 6))
        $errLast = 0.0; foreach ($r in ($body | Select-Object -Last $ew)) { $errLast += & $num $r "rt_errors" }
        $spanMin = ((& $num $body[-1] "t_s") - (& $num $body[0] "t_s")) / 60
        $lastMin = ((& $num $body[-1] "t_s") - (& $num $body[-$ew] "t_s")) / 60
        $rateAll = if ($spanMin -gt 0) { $errAll / $spanMin } else { 0 }
        $rateLast = if ($lastMin -gt 0) { $errLast / $lastMin } else { 0 }
        $firstErr = @($events | Where-Object { $_.kind -eq "runtime-error" } | Select-Object -First 1)
        $d = "{0} [ERROR] lines ({1}/min), last window {2}/min" -f $errAll, (F $rateAll 2), (F $rateLast 2)
        if ($firstErr.Count) { $d += "; first at {0} min: {1}" -f (F ([double]::Parse($firstErr[0].t_s, $inv) / 60) 1), $firstErr[0].detail }
        if ($rateAll -gt $MaxErrorsPerMin -or $rateLast -gt $MaxErrorsPerMin) { Add-Result "Runtime errors" "FAIL" ($d + "; limit $MaxErrorsPerMin/min (the picture may be black or frozen)") }
        else { Add-Result "Runtime errors" "PASS" $d }
        $hp = @($events | Where-Object { $_.kind -eq "display" })
        if ($hp.Count) { Add-Result "Display hotplug" "INFO" ("{0} disconnect/reconnect burst(s), first at {1} min; check the picture recovered (Runtime errors above)" -f $hp.Count, (F ([double]::Parse($hp[0].t_s, $inv) / 60) 1)) }
        else { Add-Result "Display hotplug" "INFO" "none" }
    }
    $acts = @($events | Where-Object { $_.kind -eq "churn" })
    if ($acts.Count) {
        $by = $acts | Group-Object detail | ForEach-Object { "$($_.Name) x$($_.Count)" }
        Add-Result "Churn" "INFO" ($by -join ", ")
    }
    return Finish-Analysis $dir $results
}

function Finish-Analysis([string]$dir, $results) {
    $fail = @($results | Where-Object { $_.result -eq "FAIL" }).Count
    $verdict = if ($fail) { "FAIL ($fail)" } else { "PASS" }
    $lines = @("Media Player Pro soak - $verdict", "")
    foreach ($r in $results) {
        $lines += ("{0,-22} {1,-5} {2}" -f $r.check, $r.result, $r.detail)
        $c = @{ PASS = "Green"; FAIL = "Red"; SKIP = "DarkYellow"; INFO = "Gray" }[$r.result]
        Write-Host ("{0,-22} {1,-5} {2}" -f $r.check, $r.result, $r.detail) -ForegroundColor $c
    }
    Write-Host ""; Write-Host "Soak: $verdict" -ForegroundColor $(if ($fail) { "Red" } else { "Green" })
    $lines | Set-Content (Join-Path $dir "summary.txt") -Encoding utf8
    @{ verdict = $verdict; results = $results } | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $dir "summary.json") -Encoding utf8
    if ($fail) { return 1 } else { return 0 }
}

if ($Analyze) {
    if (-not $Out) { Write-Host "-Analyze needs -Out <run folder>" -ForegroundColor Red; exit 2 }
    exit (Analyze-Run $Out)
}

# ------------------------------------------------------------------ setup
if (-not $Exe) {
    $dir = $null
    try { $dir = (Get-ItemProperty "HKLM:\Software\DisplayXR\Apps\MediaPlayerPro" -ErrorAction Stop).InstallPath } catch {}
    if (-not $dir) { $dir = "$env:ProgramFiles\DisplayXR\Apps\MediaPlayerPro" }
    $Exe = Join-Path $dir "mediaplayer_pro_handle_vk_win.exe"
}
if (-not (Test-Path $Exe)) { Write-Host "Media Player Pro not found at $Exe (install it, or pass -Exe)." -ForegroundColor Red; exit 2 }
if (-not $DeckLink -and -not $Clip) { Write-Host "Give -Clip <file>[,<file>...] or -DeckLink." -ForegroundColor Red; exit 2 }
# powershell -File passes "a,b" as one string; split it so both launch styles work.
$Clip = @($Clip | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ } | ForEach-Object { (Resolve-Path $_).Path })
if ($Churn -eq "heavy" -and -not $DeckLink -and $Clip.Count -lt 2) { Write-Host "Note: heavy churn with one clip re-opens the same file." -ForegroundColor DarkYellow }
$ExeDir = Split-Path $Exe
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
if (-not $Out) { $Out = Join-Path ([Environment]::GetFolderPath("Desktop")) "soak_$stamp" }
New-Item -ItemType Directory -Force $Out | Out-Null
$Station = Join-Path ([Environment]::GetFolderPath("MyPictures")) "DisplayXR"
$Status = Join-Path $Station "station_status.json"
$LinkFile = Join-Path $Station "resolve_link.json"
$CmdFile = Join-Path $Station "resolve_commands.jsonl"
$SamplesCsv = Join-Path $Out "samples.csv"
$EventsCsv = Join-Path $Out "events.csv"
"t_s,kind,detail" | Set-Content $EventsCsv -Encoding ascii
function Event([double]$t, [string]$kind, [string]$detail) {
    ('{0},{1},"{2}"' -f (F $t 0), $kind, ($detail -replace '"', "'")) | Add-Content $EventsCsv
    $c = if ($kind -eq "churn") { "DarkGray" } elseif ($kind -in "hang", "exit") { "Red" } else { "Gray" }
    Write-Host ("  [{0,6}s] {1}: {2}" -f (F $t 0), $kind, $detail) -ForegroundColor $c
}

Add-Type -Namespace Soak -Name W -MemberDefinition @'
[DllImport("user32.dll")] public static extern uint GetGuiResources(IntPtr hProcess, uint uiFlags);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
'@

# The panel's channels need the station folder; the player creates it too.
New-Item -ItemType Directory -Force $Station | Out-Null

$argv = @()
if ($DeckLink) {
    $argv += "--decklink=$Left"
    if ($Right) { $argv += "--decklink-right=$Right" }
    $argv += "--decklink-input=$Connection"
} else {
    $argv += $Clip[0]
}
$psi = New-Object System.Diagnostics.ProcessStartInfo $Exe
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.WorkingDirectory = $ExeDir
$psi.Arguments = ($argv | ForEach-Object { '"' + $_ + '"' }) -join ' '
$psi.EnvironmentVariables["MEDIAPLAYER_HUD"] = "0"
$psi.EnvironmentVariables["MEDIAPLAYER_LOOP"] = "1"
if ($Fullscreen) { $psi.EnvironmentVariables["MEDIAPLAYER_FULLSCREEN"] = "1" }
elseif ($Window) { $psi.EnvironmentVariables["MEDIAPLAYER_WINDOW"] = $Window }
if ($Env) { foreach ($k in $Env.Keys) { $psi.EnvironmentVariables[$k] = [string]$Env[$k] } }

Write-Host "Media Player Pro soak: $Minutes min, sample every $SampleSec s, churn $Churn, out $Out" -ForegroundColor Cyan
Write-Host "  $Exe $($psi.Arguments)" -ForegroundColor Gray
Write-Host "  Stop early with Ctrl+C: the player is closed and the run so far is analysed." -ForegroundColor Gray
$t0 = Get-Date
$p = [System.Diagnostics.Process]::Start($psi)
$stdoutFile = Join-Path $Out "stdout.txt"
# Drain the pipes continuously (a full pipe would block the player) into a file.
$sb = New-Object System.Text.StringBuilder
$onData = { if ($EventArgs.Data) { $null = $Event.MessageData.AppendLine($EventArgs.Data) } }
$null = Register-ObjectEvent $p OutputDataReceived -Action $onData -MessageData $sb
$null = Register-ObjectEvent $p ErrorDataReceived -Action $onData -MessageData $sb
$p.BeginOutputReadLine(); $p.BeginErrorReadLine()
$procId = $p.Id

# ------------------------------------------------------------------ probes
function Gpu-Sample([int]$id) {
    $r = @{ ded = 0.0; shr = 0.0; e3d = 0.0; ecopy = 0.0; evdec = 0.0 }
    try {
        $c = Get-Counter -Counter @("\GPU Process Memory(pid_$($id)_*)\Dedicated Usage", "\GPU Process Memory(pid_$($id)_*)\Shared Usage", "\GPU Engine(pid_$($id)_*)\Utilization Percentage") -ErrorAction SilentlyContinue
        foreach ($s in $c.CounterSamples) {
            if ($s.Path -like "*dedicated usage") { $r.ded += $s.CookedValue / 1MB }
            elseif ($s.Path -like "*shared usage") { $r.shr += $s.CookedValue / 1MB }
            elseif ($s.InstanceName -like "*engtype_3d*") { $r.e3d += $s.CookedValue }
            elseif ($s.InstanceName -like "*engtype_copy*") { $r.ecopy += $s.CookedValue }
            elseif ($s.InstanceName -like "*engtype_videodecode*") { $r.evdec += $s.CookedValue }
        }
    } catch {}
    return $r
}
function Read-Status {
    try { if (Test-Path $Status) { return (Get-Content $Status -Raw | ConvertFrom-Json) } } catch {}
    return $null
}
function Find-Log {
    return Get-ChildItem "$env:LOCALAPPDATA\DisplayXR" -Filter "DisplayXR_*.$procId`_*.log" -ErrorAction SilentlyContinue | Select-Object -First 1
}
# Read what the runtime log gained since the last call. The folder listing's size goes stale
# while the runtime holds the file open (NTFS updates it on close), so the size comes from an
# open stream. Counts new [ERROR] lines and display hotplug changes (a panel blink / replug).
$script:logPos = 0L; $script:logTail = ""
function Read-LogDelta {
    $r = @{ mb = 0.0; err = 0; hot = 0; first = "" }
    $log = Find-Log
    if (-not $log) { return $r }
    try {
        $fs = [IO.File]::Open($log.FullName, 'Open', 'Read', 'ReadWrite,Delete')
        try {
            $len = $fs.Length; $r.mb = $len / 1MB
            if ($len -lt $script:logPos) { $script:logPos = 0L; $script:logTail = "" }
            $want = [math]::Min($len - $script:logPos, 64MB)
            if ($want -gt 0) {
                $null = $fs.Seek($script:logPos, 'Begin')
                $buf = New-Object byte[] $want
                $got = $fs.Read($buf, 0, $want)
                $script:logPos += $got
                $text = $script:logTail + [Text.Encoding]::UTF8.GetString($buf, 0, $got)
                $cut = $text.LastIndexOf("`n")
                if ($cut -lt 0) { $script:logTail = $text; return $r }
                $script:logTail = $text.Substring($cut + 1)
                foreach ($line in $text.Substring(0, $cut).Split("`n")) {
                    if ($line.Contains("[ERROR]")) { $r.err++; if (-not $r.first) { $r.first = $line.Trim() } }
                    elseif ($line.Contains("display hotplug: screen table generation")) { $r.hot++ }
                }
                if ($len - $script:logPos -gt 0) { $script:logPos = $len }   # skip a flood beyond 64 MB
            }
        } finally { $fs.Dispose() }
    } catch { }
    return $r
}
function Folder-MB([string]$d) {
    $s = (Get-ChildItem $d -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
    if ($s) { return $s / 1MB } else { return 0.0 }
}

# ------------------------------------------------------------------ churn
$script:linkSeq = [int64](Get-Date -UFormat %s)
$script:cmdId = [int64](Get-Date -UFormat %s) * 10
$script:clipIdx = 0
$panelClock = [Diagnostics.Stopwatch]::StartNew()
function Write-Link([string]$path, [double]$sec) {
    $script:linkSeq++
    $tc = "{0:00}:{1:00}:{2:00}:00" -f [int][math]::Floor($sec / 3600), ([int][math]::Floor($sec / 60) % 60), ([int][math]::Floor($sec) % 60)
    $j = @{ seq = $script:linkSeq; timeline = "soak"; playhead_tc = $tc; timeline_fps = 24; follow = $true;
            written = $panelClock.Elapsed.TotalSeconds; clip = @{ path = ($path -replace '\\', '/'); seconds = $sec; fps = 24 } } | ConvertTo-Json -Compress
    $tmp = "$LinkFile.soak.tmp"
    [IO.File]::WriteAllText($tmp, $j)
    Move-Item -Force $tmp $LinkFile
}
# Park on a frame: one sample. Play: samples moving at real time for $secs, then go quiet
# (the follower stops acting once samples stop; the player keeps playing).
function Link-Park([string]$path, [double]$sec) { Write-Link $path $sec; Start-Sleep -Milliseconds 400 }
function Link-Play([string]$path, [double]$sec, [double]$secs = 2.0) {
    $w = [Diagnostics.Stopwatch]::StartNew()
    while ($w.Elapsed.TotalSeconds -lt $secs) { Write-Link $path ($sec + $w.Elapsed.TotalSeconds); Start-Sleep -Milliseconds 200 }
}
function Post-Key([int]$vk) {
    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { return $false }
    $null = [Soak.W]::PostMessage($h, 0x0100, [IntPtr]$vk, [IntPtr]1)            # WM_KEYDOWN
    Start-Sleep -Milliseconds 60
    $null = [Soak.W]::PostMessage($h, 0x0101, [IntPtr]$vk, [IntPtr]0xC0000001)   # WM_KEYUP
    return $true
}
function Current-File($st) {
    if ($st -and $st.file -and $st.file.path) { return @{ path = [string]$st.file.path; sec = [double]$st.file.seconds } }
    return $null
}
$script:actionN = 0
function Do-Churn([double]$t) {
    $st = Read-Status
    $cur = Current-File $st
    $acts = @("ui", "seek", "pauseplay")
    if ($Churn -eq "heavy") { $acts += "open" }
    if ($DeckLink) { $acts = @("ui") ; if ($Churn -eq "heavy") { $acts += "snapshot" } }
    $a = $acts[$script:actionN % $acts.Count]
    if ($Churn -eq "heavy" -and ($script:actionN % 10) -eq 9) { $a = "snapshot" }
    $script:actionN++
    switch ($a) {
        "ui" {
            if (Post-Key 0x09) { Start-Sleep -Seconds 3; $null = Post-Key 0x09; Event $t "churn" "ui" } else { Event $t "churn" "ui-nowindow" }
        }
        "seek" {
            if (-not $cur) { Event $t "churn" "seek-nofile"; return }
            $to = [math]::Round((Get-Random -Minimum 0.0 -Maximum 1.0) * [math]::Max(1.0, $cur.sec + 5), 2)
            Link-Park $cur.path $to; Link-Play $cur.path $to; Event $t "churn" "seek"
        }
        "pauseplay" {
            if (-not $cur) { Event $t "churn" "pauseplay-nofile"; return }
            Link-Park $cur.path $cur.sec; Start-Sleep -Seconds 2
            $s2 = Read-Status; $c2 = Current-File $s2; $at = if ($c2) { $c2.sec } else { $cur.sec }
            Link-Play $cur.path $at; Event $t "churn" "pauseplay"
        }
        "open" {
            $script:clipIdx = ($script:clipIdx + 1) % $Clip.Count
            $next = $Clip[$script:clipIdx]
            Link-Park $next 0.0; Start-Sleep -Seconds 1; Link-Play $next 0.0; Event $t "churn" "open"
        }
        "snapshot" {
            $script:cmdId++
            ('{"cmd":"snapshot","note":"soak","id":' + $script:cmdId + '}') | Add-Content $CmdFile -Encoding ascii
            Event $t "churn" "snapshot"
        }
    }
}

# ------------------------------------------------------------------ sample loop
"t_s,churn,private_mb,working_mb,handles,gdi,user,threads,cpu_pct,gpu_ded_mb,gpu_shr_mb,gpu_3d,gpu_copy,gpu_vdec,seq,file_s,paused,rate,signal_fps,delivered_fps,dropped,late,unpaired,skew_ms,log_mb,station_mb,f_dmedia,f_dwall,f_frames,f_dropped,f_late,rt_errors,rt_hotplug" | Set-Content $SamplesCsv -Encoding ascii
$deadline = $t0.AddMinutes($Minutes)
$lastCpu = $null; $lastWall = $null
$lastSeq = -1; $lastSeqAt = Get-Date; $inHang = $false
$lastFile = $null
$nextChurn = (Get-Date).AddSeconds([math]::Max($ChurnSec, 20))
$stationMB = Folder-MB $Station; $n = 0
$stationAtStart = $stationMB
try {
    while ((Get-Date) -lt $deadline) {
        $tickStart = Get-Date
        $p.Refresh()
        if ($p.HasExited) {
            Event ((Get-Date) - $t0).TotalSeconds "exit" ("exit code 0x{0:X8}" -f $p.ExitCode)
            break
        }
        $t = ((Get-Date) - $t0).TotalSeconds
        $cpuNow = $p.TotalProcessorTime; $wallNow = Get-Date
        $cpu = 0.0
        if ($lastCpu) { $cpu = 100.0 * ($cpuNow - $lastCpu).TotalSeconds / [math]::Max(0.001, ($wallNow - $lastWall).TotalSeconds) }
        $lastCpu = $cpuNow; $lastWall = $wallNow
        $gdi = 0; $usr = 0
        try { $gdi = [Soak.W]::GetGuiResources($p.Handle, 0); $usr = [Soak.W]::GetGuiResources($p.Handle, 1) } catch {}
        $g = Gpu-Sample $procId
        $st = Read-Status
        $seq = if ($st) { [int64]$st.seq } else { -1 }
        if ($seq -ne $lastSeq) {
            if ($inHang) { Event $t "recover" ("heartbeat back after {0} s" -f [int]((Get-Date) - $lastSeqAt).TotalSeconds); $inHang = $false }
            $lastSeq = $seq; $lastSeqAt = Get-Date
        } elseif (-not $inHang -and ((Get-Date) - $lastSeqAt).TotalSeconds -ge $HangSec -and $t -gt 30) {
            $inHang = $true
            $resp = $p.Responding
            Event $t "hang" ("status seq {0} unchanged for {1} s, window responding={2}" -f $seq, [int]((Get-Date) - $lastSeqAt).TotalSeconds, $resp)
        }
        $fileS = ""; $paused = ""; $rate = ""; $fdm = ""; $fdw = ""; $ffr = ""; $fdr = ""; $flt = ""
        $cur = Current-File $st
        if ($cur) {
            $fileS = F $cur.sec 3; $paused = [string][bool]$st.file.paused
            # A pair counts only when the same file played on with no seek / loop wrap / churn
            # in between. The status file updates every 2 s, so one pair's rate is jittery;
            # the analysis sums media and wall time over a window instead.
            if ($lastFile -and $lastFile.path -eq $cur.path -and -not $st.file.paused -and $cur.sec -ge $lastFile.sec -and ($cur.sec - $lastFile.sec) -lt 3 * $SampleSec) {
                $dw = ($wallNow - $lastFile.wall).TotalSeconds
                if ($dw -gt 1) { $rate = F (($cur.sec - $lastFile.sec) / $dw) 4; $fdm = F ($cur.sec - $lastFile.sec) 3; $fdw = F $dw 3 }
            }
            $lastFile = @{ path = $cur.path; sec = $cur.sec; wall = $wallNow }
            if ($null -ne $st.file.frames) { $ffr = [string]$st.file.frames; $fdr = [string]$st.file.dropped; $flt = [string]$st.file.late }
        } else { $lastFile = $null }
        $sig = 0; $del = 0; $drop = 0; $late = 0; $unp = 0; $skew = 0
        if ($st -and $st.rig -and $st.rig.decklink) {
            $d = $st.rig.decklink
            $sig = [double]$d.signal_fps; $del = [double]$d.delivered_fps; $drop = [double]$d.dropped
            $late = [double]$d.late_frames; $unp = [double]$d.unpaired; $skew = [double]$d.max_skew_ms
        }
        $ld = Read-LogDelta
        $logMB = $ld.mb
        if ($ld.hot -gt 0) { Event $t "display" ("{0} display hotplug change(s): the panel disconnected / reconnected" -f $ld.hot) }
        if ($ld.err -gt 0 -and -not $script:errBurst) { Event $t "runtime-error" ("{0} new [ERROR] line(s); first: {1}" -f $ld.err, $ld.first.Substring(0, [math]::Min(200, $ld.first.Length))) }
        $script:errBurst = ($ld.err -gt 0)
        if (($n % 6) -eq 0) { $stationMB = Folder-MB $Station }
        $n++
        $row = @((F $t 0), $Churn, (F ($p.PrivateMemorySize64 / 1MB) 1), (F ($p.WorkingSet64 / 1MB) 1), $p.HandleCount, $gdi, $usr, $p.Threads.Count,
                 (F $cpu 1), (F $g.ded 1), (F $g.shr 1), (F $g.e3d 1), (F $g.ecopy 1), (F $g.evdec 1), $seq, $fileS, $paused, $rate,
                 (F $sig 2), (F $del 2), (F $drop 0), (F $late 0), (F $unp 0), (F $skew 2), (F $logMB 2), (F ($stationMB - $stationAtStart) 1), $fdm, $fdw, $ffr, $fdr, $flt, $ld.err, $ld.hot)
        ($row -join ",") | Add-Content $SamplesCsv
        if (($n % 6) -eq 1) {
            Write-Host ("[{0,5} min] private {1} MB, GPU {2} MB, handles {3}, gdi {4}, threads {5}, cpu {6}%, 3D {7}%, rate {8}, fps {9}/{10}, log {11} MB, frames {12} dropped {13} late {14}" -f (F ($t / 60) 1), (F ($p.PrivateMemorySize64 / 1MB) 0), (F $g.ded 0), $p.HandleCount, $gdi, $p.Threads.Count, (F $cpu 0), (F $g.e3d 0), $rate, (F $del 1), (F $sig 1), (F $logMB 1), $ffr, $fdr, $flt)
        }
        if ($Churn -ne "none" -and (Get-Date) -ge $nextChurn) {
            try { Do-Churn $t } catch { Event $t "churn" ("error: " + $_.Exception.Message) }
            $lastFile = $null   # the next pair would straddle a seek / pause / open
            $nextChurn = (Get-Date).AddSeconds($ChurnSec)
        }
        $sleepLeft = $SampleSec - ((Get-Date) - $tickStart).TotalSeconds
        if ($sleepLeft -gt 0) { Start-Sleep -Milliseconds ([int]($sleepLeft * 1000)) }
    }
} finally {
    if (-not $p.HasExited) {
        $null = $p.CloseMainWindow()
        if (-not $p.WaitForExit(8000)) { Event ((Get-Date) - $t0).TotalSeconds "close" "did not close in 8 s, killed"; Stop-Process -Id $procId -Force }
        else { Event ((Get-Date) - $t0).TotalSeconds "close" "closed cleanly" }
    }
    Start-Sleep -Milliseconds 500
    $sb.ToString() | Set-Content $stdoutFile -Encoding utf8
    Get-EventSubscriber | Where-Object { $_.SourceObject -eq $p } | Unregister-Event
    if (Test-Path $LinkFile) { Remove-Item $LinkFile -ErrorAction SilentlyContinue }  # a stale link must not steer the next session
    $log = Find-Log
    if ($log) { Copy-Item $log.FullName (Join-Path $Out "runtime.log") }
}

$code = Analyze-Run $Out
$zip = "$Out.zip"
try { Compress-Archive -Path (Join-Path $Out "*") -DestinationPath $zip -Force; Write-Host "Evidence: $zip" -ForegroundColor Cyan } catch {}
exit $code
