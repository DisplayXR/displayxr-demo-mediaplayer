<#
.SYNOPSIS
  Media Player Pro capture-path matrix: run the DeckLink capture with each combination of the
  capture levers (docs/CAPTURE-LEVERS.md), one after another, and tabulate what each did, so
  the 4K x 2 capture-rate work can be measured on the station in one unattended sitting.

.DESCRIPTION
  Each ARM is a set of environment variables (the levers). For each arm, in each round, this
  runs soak_stress.ps1 for -Minutes with that environment, then reads what the run left:
    samples.csv           delivered fps (= pairs/s for two inputs), dropped, late, unpaired, skew,
                          CPU %, GPU copy-engine %, memory
    stdout.txt            the player's `capture-config:` line (which levers were IN EFFECT, and
                          why one asked for was not), its `perf:` lines (render fps, upload copy)
                          and `capture:` lines (callback / convert / claim wait / publish ms)
    station_status.json   the last decklink.capture block (copied as final_status.json)
  and writes, in the output folder:
    results.csv     one row per arm per round
    summary.md      arms ranked by median pairs/s, the mean over rounds, the change against
                    the first arm, and a PASS/FAIL against the 4K60 target
    system.json     what the numbers depend on: CPU, last-level-cache groups, NUMA nodes, every
                    DIMM (slot, size, speed: are all memory channels filled?), the DeckLink's and
                    GPU's PCIe link (width / speed, current vs max), drivers, versions
    matrix_<stamp>.zip   all of it, to post

  Rounds run the arms in a different order each time (forward, then reversed, ...) so a slow
  drift over the session (heat, the source) does not favour one arm.

  Every arm starts from -BaseEnv (default: the station's current config, runtime DEFER on and
  four upload threads) and pro.env is switched OFF for the runs (MEDIAPLAYER_PRO_ENV_FILE=off),
  so an arm measures exactly the variables it names.

  No card? -TestSignal 3840x2160@59.94 runs the same matrix on the synthetic rig: it proves the
  script and the switches, not the card's numbers (the DeckLink-only levers are inert there).

.EXAMPLE
  # The default ladder, 5 min per arm, two rounds (about 2 hours): station loopback inputs
  powershell -ExecutionPolicy Bypass -File capture_matrix.ps1 -Left 2 -Right 0 -Minutes 5 -Rounds 2 -Fullscreen

.EXAMPLE
  # Only some arms, quick
  powershell -ExecutionPolicy Bypass -File capture_matrix.ps1 -Left 2 -Right 0 -Arms baseline,raw,raw_alloc_hold -Minutes 3

.EXAMPLE
  # List the arms and their variables
  powershell -ExecutionPolicy Bypass -File capture_matrix.ps1 -List

.EXAMPLE
  # Re-tabulate an earlier run (other target, or after editing results by hand)
  powershell -ExecutionPolicy Bypass -File capture_matrix.ps1 -Analyze -Out "C:\...\matrix_20261114_101500"
#>
param(
    [string]$Exe,
    [string]$Left = "0",
    [string]$Right = "1",
    [string]$Connection = "sdi",
    [string]$TestSignal,                 # e.g. 3840x2160@59.94: no card needed
    [string[]]$Arms,                     # names from -List; default: all of the default set
    [ValidateSet("default", "upload", "pin", "all")][string]$Set = "default",
    [double]$Minutes = 5,
    [int]$Rounds = 1,
    [double]$WarmupSec = 60,             # samples before this are left out of the medians
    [hashtable]$BaseEnv = @{ "DXR_VK_HUB_DEFER_COMPOSE_WAIT" = "1"; "MEDIAPLAYER_UPLOAD_THREADS" = "4" },
    [switch]$Fullscreen,
    [string]$Out,
    [double]$TargetShare = 0.995,        # PASS: median pairs/s >= this x the signal rate...
    [double]$TargetRenderFps = 59.0,     # ...and the render loop at least this
    [switch]$List,
    [switch]$Analyze
)

$ErrorActionPreference = "Stop"
$inv = [Globalization.CultureInfo]::InvariantCulture
function F([double]$v, [int]$d = 2) { return $v.ToString("F$d", $inv) }
function Num($s) { $d = 0.0; if ([double]::TryParse([string]$s, [Globalization.NumberStyles]::Float, $inv, [ref]$d)) { return $d } return [double]::NaN }
function Median([double[]]$xs) {
    $v = @($xs | Where-Object { -not [double]::IsNaN($_) } | Sort-Object)
    if ($v.Count -eq 0) { return [double]::NaN }
    if ($v.Count % 2) { return $v[[int][math]::Floor($v.Count / 2)] }
    return ($v[$v.Count / 2 - 1] + $v[$v.Count / 2]) / 2
}
function Pct([double[]]$xs, [double]$p) {
    $v = @($xs | Where-Object { -not [double]::IsNaN($_) } | Sort-Object)
    if ($v.Count -eq 0) { return [double]::NaN }
    return $v[[int][math]::Min($v.Count - 1, [math]::Floor($p * $v.Count))]
}

# ------------------------------------------------------------------ the arms
# Each lever on its own first (against `baseline`), then the stacks. RAW needs the GPU decode,
# HOLD needs the allocator: an arm sets what its lever depends on.
$raw = @{ "MEDIAPLAYER_GPU_V210" = "1"; "MEDIAPLAYER_DECKLINK_RAW_V210" = "1" }
$alloc = @{ "MEDIAPLAYER_DECKLINK_ALLOCATOR" = "1" }
$hold = @{ "MEDIAPLAYER_DECKLINK_ALLOCATOR" = "1"; "MEDIAPLAYER_DECKLINK_HOLD" = "1" }
$pin = @{ "MEDIAPLAYER_CAPTURE_PIN" = "auto"; "MEDIAPLAYER_UPLOAD_PIN" = "auto" }
$zc = @{ "MEDIAPLAYER_ZERO_COPY_UPLOAD" = "1" }
function Merge([hashtable[]]$hs) { $r = @{}; foreach ($h in $hs) { foreach ($k in $h.Keys) { $r[$k] = $h[$k] } }; return $r }
$catalog = [ordered]@{
    # default set: the ladder
    "baseline"           = @{ set = "default"; env = @{}; why = "0.9.8 as shipped (direct pair on)" }
    "compose"            = @{ set = "default"; env = @{ "MEDIAPLAYER_DECKLINK_DIRECT_PAIR" = "0" }; why = "0.9.7's compose: what 0.9.8's direct pair bought" }
    "alloc"              = @{ set = "default"; env = $alloc; why = "card writes into our page-aligned buffers (copy still made)" }
    "alloc_hold"         = @{ set = "default"; env = $hold; why = "no callback copy: the convert reads the card's buffer" }
    "raw"                = @{ set = "default"; env = $raw; why = "no CPU convert, no compose: v210 rows to the GPU" }
    "raw_alloc_hold"     = @{ set = "default"; env = (Merge @($raw, $hold)); why = "raw + no callback copy" }
    "raw_alloc_hold_zc"  = @{ set = "default"; env = (Merge @($raw, $hold, $zc)); why = "zero-copy: the GPU reads the card's buffers in place (no CPU copy at all)" }
    "raw_alloc_hold_pin" = @{ set = "default"; env = (Merge @($raw, $hold, $pin)); why = "raw + no callback copy + pinned" }
    # pin set: where the threads run
    "pin"                = @{ set = "pin"; env = $pin; why = "lanes on L3 groups 0/1, upload on 2" }
    "raw_pin"            = @{ set = "pin"; env = (Merge @($raw, $pin)); why = "raw + pinned" }
    "alloc_node0_hold"   = @{ set = "pin"; env = (Merge @($hold, @{ "MEDIAPLAYER_DECKLINK_ALLOC_NODE" = "0" })); why = "buffers on NUMA node 0" }
    "raw_alloc_hold_zc_pin" = @{ set = "pin"; env = (Merge @($raw, $hold, $zc, $pin)); why = "everything: zero-copy + pinned" }
    # upload set: the copy's threads, on the raw path
    "raw_upload2"        = @{ set = "upload"; env = (Merge @($raw, @{ "MEDIAPLAYER_UPLOAD_THREADS" = "2" })); why = "raw, 2 upload threads" }
    "raw_upload8"        = @{ set = "upload"; env = (Merge @($raw, @{ "MEDIAPLAYER_UPLOAD_THREADS" = "8" })); why = "raw, 8 upload threads" }
    "raw_convert4"       = @{ set = "upload"; env = (Merge @($raw, @{ "MEDIAPLAYER_DECKLINK_CONVERT_THREADS" = "4" })); why = "raw, 4 lane threads (only the held-buffer copy uses them)" }
}
function EnvText([hashtable]$h) {
    if (-not $h -or $h.Count -eq 0) { return "(none)" }
    return (($h.Keys | Sort-Object | ForEach-Object { "$_=$($h[$_])" }) -join " ")
}

if ($List) {
    Write-Host "Base env (every arm): $(EnvText $BaseEnv)" -ForegroundColor Cyan
    foreach ($k in $catalog.Keys) {
        $a = $catalog[$k]
        Write-Host ("  {0,-20} [{1,-7}] {2}" -f $k, $a.set, $a.why)
        Write-Host ("  {0,-20}           {1}" -f "", (EnvText $a.env)) -ForegroundColor DarkGray
    }
    exit 0
}

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
if (-not $Out) { $Out = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "DisplayXR\Test Results\CaptureMatrix\matrix_$stamp" }
New-Item -ItemType Directory -Force $Out | Out-Null
$Soak = Join-Path $PSScriptRoot "soak_stress.ps1"
$StatusFile = Join-Path (Join-Path ([Environment]::GetFolderPath("MyPictures")) "DisplayXR") "station_status.json"

# ------------------------------------------------------------------ system facts
function Pci-Link($dev) {
    $r = [ordered]@{ name = $dev.FriendlyName; instance = $dev.InstanceId }
    foreach ($k in @("DEVPKEY_PciDevice_CurrentLinkWidth", "DEVPKEY_PciDevice_MaxLinkWidth", "DEVPKEY_PciDevice_CurrentLinkSpeed", "DEVPKEY_PciDevice_MaxLinkSpeed")) {
        try { $r[$k.Replace("DEVPKEY_PciDevice_", "")] = (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName $k -ErrorAction Stop).Data } catch { $r[$k.Replace("DEVPKEY_PciDevice_", "")] = $null }
    }
    # Speed codes: 1 = 2.5 GT/s (Gen1), 2 = 5 (Gen2), 3 = 8 (Gen3), 4 = 16 (Gen4), 5 = 32 (Gen5)
    return $r
}
function System-Facts {
    $s = [ordered]@{ taken = (Get-Date).ToString("s"); computer = $env:COMPUTERNAME }
    try {
        $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
        $s.cpu = [ordered]@{ name = $cpu.Name.Trim(); cores = $cpu.NumberOfCores; logical = $cpu.NumberOfLogicalProcessors; max_mhz = $cpu.MaxClockSpeed }
    } catch {}
    try {
        $dimms = @(Get-CimInstance Win32_PhysicalMemory | ForEach-Object {
                [ordered]@{ slot = $_.DeviceLocator; bank = $_.BankLabel; gb = [math]::Round($_.Capacity / 1GB, 1); mts = $_.Speed; configured_mts = $_.ConfiguredClockSpeed; part = ("" + $_.PartNumber).Trim() } })
        $arr = Get-CimInstance Win32_PhysicalMemoryArray | Select-Object -First 1
        $s.memory = [ordered]@{ dimms = $dimms; dimm_count = $dimms.Count; slots = $arr.MemoryDevices; total_gb = [math]::Round(($dimms | Measure-Object -Property gb -Sum).Sum, 1)
            note = "A Threadripper PRO 3975WX has 8 memory channels: fewer than 8 DIMMs leaves channels empty and cuts bandwidth" }
    } catch {}
    try {
        $nodes = @(Get-CimInstance -Namespace root\cimv2 -ClassName Win32_NumaNode -ErrorAction Stop)
        $s.numa_nodes = $nodes.Count
    } catch { $s.numa_nodes = $null }
    try {
        $s.pci = [ordered]@{
            decklink = @(Get-PnpDevice -PresentOnly | Where-Object { $_.FriendlyName -match "DeckLink|Blackmagic" -and $_.InstanceId -like "PCI\*" } | ForEach-Object { Pci-Link $_ })
            gpu      = @(Get-PnpDevice -PresentOnly -Class Display | Where-Object { $_.InstanceId -like "PCI\*" } | ForEach-Object { Pci-Link $_ })
        }
    } catch {}
    try { $s.gpu_driver = @(Get-CimInstance Win32_VideoController | ForEach-Object { [ordered]@{ name = $_.Name; driver = $_.DriverVersion } }) } catch {}
    if ($Exe -and (Test-Path $Exe)) {
        $s.player = [ordered]@{ exe = $Exe; version = (Get-Item $Exe).VersionInfo.ProductVersion }
        try { $s.decklink_list = (& $Exe --decklink-list 2>&1 | Out-String).Trim() } catch {}
    }
    $cli = "C:\Program Files\DisplayXR\Runtime\displayxr-cli.exe"
    if (Test-Path $cli) { try { $s.runtime_info = (& $cli info 2>&1 | Out-String).Trim() } catch {} }
    try {
        $plan = (powercfg /getactivescheme 2>&1 | Out-String).Trim()
        $s.power_plan = $plan
    } catch {}
    return $s
}

# ------------------------------------------------------------------ one run's numbers
function Read-Run([string]$dir, [string]$arm, [int]$round, [double]$signalFallback) {
    $r = [ordered]@{ arm = $arm; round = $round; dir = (Split-Path $dir -Leaf); ok = $false }
    $csv = Join-Path $dir "samples.csv"
    if (Test-Path $csv) {
        $rows = @(Import-Csv $csv | Where-Object { (Num $_.t_s) -ge $WarmupSec -and (Num $_.signal_fps) -gt 0 })
        if ($rows.Count -ge 2) {
            $r.ok = $true
            $del = @($rows | ForEach-Object { Num $_.delivered_fps })
            $r.samples = $rows.Count
            $r.signal_fps = Median @($rows | ForEach-Object { Num $_.signal_fps })
            $r.pairs_med = Median $del
            $r.pairs_p10 = Pct $del 0.10
            $r.pairs_min = ($del | Measure-Object -Minimum).Minimum
            $r.share = if ($r.signal_fps -gt 0) { $r.pairs_med / $r.signal_fps } else { [double]::NaN }
            $first = $rows[0]; $last = $rows[-1]
            $r.dropped = (Num $last.dropped) - (Num $first.dropped)
            $r.late = (Num $last.late) - (Num $first.late)
            $r.unpaired = (Num $last.unpaired) - (Num $first.unpaired)
            $r.skew_ms = Num $last.skew_ms
            $r.cpu_pct = Median @($rows | ForEach-Object { Num $_.cpu_pct })
            $r.gpu_copy_pct = Median @($rows | ForEach-Object { Num $_.gpu_copy })
            $r.gpu_3d_pct = Median @($rows | ForEach-Object { Num $_.gpu_3d })
            $r.private_mb = Num $last.private_mb
        }
    }
    $so = Join-Path $dir "stdout.txt"
    if (Test-Path $so) {
        $lines = Get-Content $so
        $cfg = $lines | Where-Object { $_ -match "capture-config:" } | Select-Object -Last 1
        if ($cfg) { $r.in_effect = ($cfg -replace '^.*capture-config:\s*', '') }
        $perf = @($lines | Where-Object { $_ -match "perf: render ([0-9.]+) fps" })
        $fps = @($perf | ForEach-Object { if ($_ -match "perf: render ([0-9.]+) fps") { Num $Matches[1] } })
        $r.render_fps = Median ($fps | Select-Object -Skip ([math]::Min(2, [math]::Max(0, $fps.Count - 1))))
        $cap = @($lines | Where-Object { $_ -match "capture: " -and $_ -match "callback" } | Select-Object -Skip 2)
        foreach ($st in @("callback", "convert", "claim wait", "publish")) {
            $vals = @($cap | ForEach-Object { if ($_ -match ([regex]::Escape($st) + " ([0-9.]+)/([0-9.]+) ms")) { Num $Matches[1] } })
            $key = ($st -replace " ", "_") + "_ms"
            $r[$key] = Median $vals
        }
        $r.notes = (($lines | Where-Object { $_ -match "\[WARN" -and $_ -match "decklink|capture|allocator|v210" } | Select-Object -First 3) -join " / ")
    }
    if (-not $r.signal_fps -and $signalFallback) { $r.signal_fps = $signalFallback }
    $r.pass = $r.ok -and ($r.share -ge $TargetShare) -and ($r.render_fps -ge $TargetRenderFps)
    foreach ($k in @("samples", "signal_fps", "pairs_med", "pairs_p10", "pairs_min", "share", "dropped", "late", "unpaired", "skew_ms", "cpu_pct", "gpu_copy_pct", "gpu_3d_pct", "private_mb", "render_fps", "callback_ms", "convert_ms", "claim_wait_ms", "publish_ms", "in_effect", "notes")) {
        if (-not $r.Contains($k)) { $r[$k] = $null }   # every row has every column
    }
    return [pscustomobject]$r
}

# ------------------------------------------------------------------ tabulate
function Write-Summary([object[]]$results) {
    $cols = @("arm", "round", "ok", "pass", "samples", "signal_fps", "pairs_med", "pairs_p10", "pairs_min", "share", "render_fps", "dropped", "late", "unpaired", "skew_ms", "callback_ms", "convert_ms", "claim_wait_ms", "publish_ms", "cpu_pct", "gpu_copy_pct", "gpu_3d_pct", "private_mb", "in_effect", "notes", "dir")
    $results | ForEach-Object { $o = [ordered]@{}; foreach ($c in $cols) { $v = $_.$c; if ($v -is [double]) { $v = F $v 3 }; $o[$c] = $v }; [pscustomobject]$o } |
        Export-Csv (Join-Path $Out "results.csv") -NoTypeInformation -Encoding ascii
    $byArm = [ordered]@{}
    foreach ($r in $results) { if (-not $byArm.Contains($r.arm)) { $byArm[$r.arm] = @() }; $byArm[$r.arm] += , $r }
    $rows = @()
    foreach ($a in $byArm.Keys) {
        $rs = @($byArm[$a] | Where-Object { $_.ok })
        $rows += [pscustomobject]@{
            arm = $a; runs = $rs.Count
            pairs = Median @($rs | ForEach-Object { $_.pairs_med })
            p10 = Median @($rs | ForEach-Object { $_.pairs_p10 })
            render = Median @($rs | ForEach-Object { $_.render_fps })
            share = Median @($rs | ForEach-Object { $_.share })
            dropped = ($rs | Measure-Object -Property dropped -Sum).Sum
            cb = Median @($rs | ForEach-Object { $_.callback_ms }); cv = Median @($rs | ForEach-Object { $_.convert_ms })
            cw = Median @($rs | ForEach-Object { $_.claim_wait_ms }); pb = Median @($rs | ForEach-Object { $_.publish_ms })
            cpu = Median @($rs | ForEach-Object { $_.cpu_pct })
            pass = ($rs.Count -gt 0) -and (@($rs | Where-Object { -not $_.pass }).Count -eq 0)
            spread = if ($rs.Count -gt 1) { (($rs | Measure-Object -Property pairs_med -Maximum).Maximum - ($rs | Measure-Object -Property pairs_med -Minimum).Minimum) } else { [double]::NaN }
        }
    }
    $base = $rows | Select-Object -First 1
    $md = New-Object System.Collections.Generic.List[string]
    $md.Add("# Capture matrix $(Split-Path $Out -Leaf)")
    $md.Add("")
    $src = if ($TestSignal) { "test signal $TestSignal (DeckLink-only levers are inert)" } else { "DeckLink inputs $Left + $Right ($Connection)" }
    $md.Add("Source: $src. $Minutes min per arm, $Rounds round(s), first $WarmupSec s of each left out. Base env: $(EnvText $BaseEnv). Target: pairs >= $TargetShare x signal and render >= $TargetRenderFps fps.")
    $md.Add("")
    $md.Add("Pairs/s is the capture's delivered rate (both eyes published as one frame). Stage times are the lane's per-eye ms from the player's capture: lines (avg of the 10 s windows); 'delta' is against the first arm.")
    $md.Add("")
    $md.Add("| arm | runs | pairs/s | p10 | delta | spread | render fps | dropped | callback ms | convert ms | claim wait ms | publish ms | CPU % | target |")
    $md.Add("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    foreach ($x in ($rows | Sort-Object -Property pairs -Descending)) {
        $delta = if ($base -and -not [double]::IsNaN($base.pairs) -and -not [double]::IsNaN($x.pairs)) { F ($x.pairs - $base.pairs) 2 } else { "" }
        $md.Add("| $($x.arm) | $($x.runs) | $(F $x.pairs 2) | $(F $x.p10 2) | $delta | $(F $x.spread 2) | $(F $x.render 2) | $($x.dropped) | $(F $x.cb 2) | $(F $x.cv 2) | $(F $x.cw 2) | $(F $x.pb 2) | $(F $x.cpu 1) | $(if ($x.pass) { 'PASS' } else { 'fail' }) |")
    }
    $md.Add("")
    $md.Add("## What each arm set, and what the player says was in effect")
    $md.Add("")
    foreach ($a in $byArm.Keys) {
        $c = $catalog[$a]
        $r0 = $byArm[$a] | Where-Object { $_.in_effect } | Select-Object -First 1
        $md.Add("- **$a**: $(if ($c) { $c.why } else { '' }) - ``$(if ($c) { EnvText $c.env } else { '?' })``")
        if ($r0) { $md.Add("  - in effect: ``$($r0.in_effect)``") }
        $n = $byArm[$a] | Where-Object { $_.notes } | Select-Object -First 1
        if ($n) { $md.Add("  - warnings: $($n.notes)") }
        $bad = @($byArm[$a] | Where-Object { -not $_.ok })
        if ($bad.Count) { $md.Add("  - $($bad.Count) run(s) produced no live samples (see their folders)") }
    }
    $md.Add("")
    $md.Add("## Reading it")
    $md.Add("")
    $md.Add("- A lever that helps moves pairs/s up AND its stage down (raw: convert ms; hold: callback ms). Pairs up with no stage change is noise or drift: compare 'spread' across rounds.")
    $md.Add("- If every arm sits at the same ceiling, the limit is outside the levers: check system.json (empty memory channels, a PCIe link below x8 Gen3 for the DeckLink, x16 for the GPU) and the claim wait (one lane waiting on the other).")
    $md.Add("- render fps below the signal with pairs at the signal: the render thread, not capture (perf: lines; runtime DEFER in the base env).")
    $md -join "`r`n" | Set-Content (Join-Path $Out "summary.md") -Encoding ascii
    Write-Host ""
    $md | Select-Object -First (8 + $rows.Count) | ForEach-Object { Write-Host $_ }
}

if ($Analyze) {
    if (-not (Test-Path $Out)) { Write-Host "No folder $Out" -ForegroundColor Red; exit 2 }
    $results = @()
    foreach ($d in (Get-ChildItem $Out -Directory | Sort-Object Name)) {
        if ($d.Name -match '^r(\d+)_\d+_(.+)$') { $results += , (Read-Run $d.FullName $Matches[2] ([int]$Matches[1]) 0) }
    }
    Write-Summary $results
    exit 0
}

# ------------------------------------------------------------------ run
if (-not $Exe) {
    foreach ($c in @("C:\Program Files\DisplayXR\MediaPlayerPro\mediaplayer_pro_handle_vk_win.exe", (Join-Path $PSScriptRoot "..\build_pro\mediaplayer_pro_handle_vk_win.exe"))) {
        if (Test-Path $c) { $Exe = (Resolve-Path $c).Path; break }
    }
}
if (-not $Exe -or -not (Test-Path $Exe)) { Write-Host "Media Player Pro not found; give -Exe <path to mediaplayer_pro_handle_vk_win.exe>." -ForegroundColor Red; exit 2 }
if (-not (Test-Path $Soak)) { Write-Host "soak_stress.ps1 not found next to this script." -ForegroundColor Red; exit 2 }

$names = if ($Arms) { @($Arms | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }
         else { @($catalog.Keys | Where-Object { $Set -eq "all" -or $catalog[$_].set -eq "default" -or $catalog[$_].set -eq $Set }) }
foreach ($n in $names) { if (-not $catalog.Contains($n)) { Write-Host "Unknown arm '$n' (see -List)." -ForegroundColor Red; exit 2 } }
if ($names[0] -ne "baseline" -and $names -contains "baseline") { $names = @("baseline") + @($names | Where-Object { $_ -ne "baseline" }) }

$total = $names.Count * $Rounds * ($Minutes + 0.5)
Write-Host "Capture matrix: $($names.Count) arm(s) x $Rounds round(s) x $Minutes min = about $(F ($total / 60) 1) h. Out: $Out" -ForegroundColor Cyan
Write-Host "  arms: $($names -join ', ')" -ForegroundColor Gray
Write-Host "  base env: $(EnvText $BaseEnv); pro.env is off for these runs" -ForegroundColor Gray

$sys = System-Facts
$sys | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $Out "system.json") -Encoding ascii
if ($sys.memory) { Write-Host ("  memory: {0} DIMM(s) in {1} slot(s), {2} GB" -f $sys.memory.dimm_count, $sys.memory.slots, $sys.memory.total_gb) -ForegroundColor Gray }

$results = @()
$i = 0
for ($round = 1; $round -le $Rounds; $round++) {
    $order = if ($round % 2 -eq 0) { @($names[($names.Count - 1)..0]) } else { $names }
    foreach ($arm in $order) {
        $i++
        $armEnv = Merge @($BaseEnv, $catalog[$arm].env, @{ "MEDIAPLAYER_PRO_ENV_FILE" = "off" })
        $dir = Join-Path $Out ("r{0}_{1:D2}_{2}" -f $round, $i, $arm)
        Write-Host ""
        Write-Host ("[{0}/{1}] round {2}, arm {3}: {4}" -f $i, ($names.Count * $Rounds), $round, $arm, (EnvText $catalog[$arm].env)) -ForegroundColor Cyan
        $soakArgs = @{ Exe = $Exe; Minutes = $Minutes; SampleSec = 5; WarmupMinutes = [math]::Min($WarmupSec / 60, $Minutes / 3); Out = $dir; Env = $armEnv }
        if ($TestSignal) { $soakArgs.TestSignal = $TestSignal } else { $soakArgs.DeckLink = $true; $soakArgs.Left = $Left; $soakArgs.Right = $Right; $soakArgs.Connection = $Connection }
        if ($Fullscreen) { $soakArgs.Fullscreen = $true }
        try { & $Soak @soakArgs | Out-Host } catch { Write-Host "  run failed: $_" -ForegroundColor Red }
        if (Test-Path $StatusFile) { Copy-Item $StatusFile (Join-Path $dir "final_status.json") -Force -ErrorAction SilentlyContinue }
        $r = Read-Run $dir $arm $round 0
        $results += , $r
        if ($r.ok) {
            Write-Host ("  -> pairs/s {0} (p10 {1}) of {2}, render {3} fps, dropped {4}, convert {5} ms, callback {6} ms" -f (F $r.pairs_med 2), (F $r.pairs_p10 2), (F $r.signal_fps 2), (F $r.render_fps 2), $r.dropped, (F $r.convert_ms 2), (F $r.callback_ms 2)) -ForegroundColor Green
        } else {
            Write-Host "  -> no live samples: see $dir\stdout.txt" -ForegroundColor Red
        }
        Start-Sleep -Seconds 3   # let the card and the runtime settle between runs
    }
}

Write-Summary $results
$zip = Join-Path (Split-Path $Out) ("{0}.zip" -f (Split-Path $Out -Leaf))
try {
    Compress-Archive -Path (Join-Path $Out "*") -DestinationPath $zip -Force
    Write-Host "Zip: $zip" -ForegroundColor Cyan
} catch { Write-Host "Zip failed: $_" -ForegroundColor DarkYellow }
Write-Host "Summary: $(Join-Path $Out 'summary.md')" -ForegroundColor Cyan
exit 0
