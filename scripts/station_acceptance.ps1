<#
.SYNOPSIS
  Media Player Pro station acceptance: the automated half of docs/STATION-ACCEPTANCE.md.

.DESCRIPTION
  Runs the installed Media Player Pro through short checks and writes one folder of
  evidence (atlas dumps, logs, snapshots, the QC report, a summary) plus a zip of it to
  send back. Every check prints PASS / FAIL / SKIP / INFO with the reason.

  Without -DeckLink it needs only the PC, the 3D display and the installed software.
  With -DeckLink it also checks the capture card: the device list, one input, and the
  two-input soak (two 4K60 10-bit signals held for -SoakMinutes, the #141 "Done when").

  Keep the window on the 3D display and do not touch the mouse while it runs. Each player
  run is short (about 20 s) and closes itself.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File station_acceptance.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File station_acceptance.ps1 -DeckLink -Left 0 -Right 1 -SoakMinutes 10
#>
param(
    [string]$Exe = "",           # default: the installed Media Player Pro
    [string]$Out = "",           # default: Desktop\station_acceptance_<time>
    [switch]$DeckLink,           # also run the capture-card checks
    [string]$Left = "0",         # DeckLink device for the left eye (number or part of the name)
    [string]$Right = "1",        # ... and the right eye (two-input soak)
    [string]$Connection = "sdi", # sdi | hdmi
    [int]$SoakMinutes = 10,
    # What the feeds must be for D2 / D3 to pass, so a lower format cannot pass as the real one.
    # Change them when the rig really feeds something else.
    [int]$ExpectEyeWidth = 3840,
    [double]$ExpectFps = 60,     # within 0.1: 59.94 needs -ExpectFps 59.94
    [int]$ExpectBits = 10,
    [string]$Only = ""           # run only the checks whose id matches (e.g. "R*")
)

$ErrorActionPreference = "Continue"  # a native tool writing to stderr must not abort the run
Add-Type -AssemblyName System.Drawing

# ---------------------------------------------------------------- setup
if (-not $Exe) {
    $dir = $null
    try { $dir = (Get-ItemProperty "HKLM:\Software\DisplayXR\Apps\MediaPlayerPro" -ErrorAction Stop).InstallPath } catch {}
    if (-not $dir) { $dir = "$env:ProgramFiles\DisplayXR\Apps\MediaPlayerPro" }
    $Exe = Join-Path $dir "mediaplayer_pro_handle_vk_win.exe"
}
if (-not (Test-Path $Exe)) { Write-Host "Media Player Pro not found at $Exe (install it, or pass -Exe)." -ForegroundColor Red; exit 2 }
$ExeDir = Split-Path $Exe
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
if (-not $Out) { $Out = Join-Path ([Environment]::GetFolderPath("Desktop")) "station_acceptance_$stamp" }
New-Item -ItemType Directory -Force $Out | Out-Null
$Pics = Join-Path ([Environment]::GetFolderPath("MyPictures")) "DisplayXR"
$Status = Join-Path $Pics "station_status.json"
$Cli = "$env:ProgramFiles\DisplayXR\Runtime\displayxr-cli.exe"
$Stereo = Join-Path $ExeDir "test_LR_2x1.png"   # installed with the player: left eye green, right eye magenta
if (-not (Test-Path $Stereo)) { $Stereo = Join-Path $PSScriptRoot "..\assets\test_LR_2x1.png" }  # a build tree

$results = New-Object System.Collections.ArrayList
function Report([string]$id, [string]$what, [string]$verdict, [string]$detail) {
    $null = $results.Add([pscustomobject]@{ id = $id; check = $what; result = $verdict; detail = $detail })
    $c = @{ PASS = "Green"; FAIL = "Red"; SKIP = "DarkYellow"; INFO = "Gray" }[$verdict]
    Write-Host ("{0,-4} {1,-5} {2}  {3}" -f $id, $verdict, $what, $detail) -ForegroundColor $c
}
# Either side may be the pattern: -Only "P*" wants P1, and -Only P1 wants the block guarded by "P*".
function Want([string]$id) { return (-not $Only) -or ($id -like $Only) -or ($Only -like $id) }

# A generated look: a CDL that warms and lifts, and a 2-point .cube that inverts.
$cdl = Join-Path $Out "accept_warm.cc"
@'
<ColorCorrection id="accept_warm">
 <SOPNode><Slope>1.3 0.6 0.7</Slope><Offset>0.05 0 0</Offset><Power>1 1 1</Power></SOPNode>
 <SatNode><Saturation>1</Saturation></SatNode>
</ColorCorrection>
'@ | Set-Content $cdl -Encoding ascii
$lut = Join-Path $Out "accept_invert.cube"
$cube = @("TITLE ""accept invert""", "LUT_3D_SIZE 2")
foreach ($b in 0, 1) { foreach ($g in 0, 1) { foreach ($r in 0, 1) { $cube += "{0} {1} {2}" -f (1 - $r), (1 - $g), (1 - $b) } } }
$cube | Set-Content $lut -Encoding ascii

# Runs the player once. Returns @{ stdout; atlas (path or $null); log (runtime log copy or $null); status (last station_status as object or $null) }
# $until: a scriptblock given the latest status object; returning $true ends the run early.
function Run-Player([string]$name, [string[]]$argv, [hashtable]$envs, [int]$dumpFrame = 90, [int]$seconds = 20, [scriptblock]$until = $null) {
    Get-ChildItem $Out -Filter "$name*" -ErrorAction SilentlyContinue | Where-Object { $_.Name -notlike "accept_*" } | Remove-Item -Force -Recurse
    $t0 = Get-Date
    $psi = New-Object System.Diagnostics.ProcessStartInfo $Exe
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.WorkingDirectory = $ExeDir
    $psi.Arguments = ($argv | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $psi.EnvironmentVariables["MEDIAPLAYER_DUMP_ATLAS"] = Join-Path $Out "$name.png"
    $psi.EnvironmentVariables["MEDIAPLAYER_DUMP_FRAME"] = "$dumpFrame"
    $psi.EnvironmentVariables["MEDIAPLAYER_HUD"] = "0"
    if ($envs) { foreach ($k in $envs.Keys) { $psi.EnvironmentVariables[$k] = [string]$envs[$k] } }
    $p = [System.Diagnostics.Process]::Start($psi)
    $oT = $p.StandardOutput.ReadToEndAsync(); $eT = $p.StandardError.ReadToEndAsync()
    $last = $null
    while (((Get-Date) - $t0).TotalSeconds -lt $seconds -and -not $p.HasExited) {
        Start-Sleep -Milliseconds 500
        if ((Test-Path $Status) -and (Get-Item $Status).LastWriteTime -gt $t0) {
            try { $last = Get-Content $Status -Raw | ConvertFrom-Json } catch {}
        }
        $el = ((Get-Date) - $t0).TotalSeconds
        $haveAtlas = Get-ChildItem $Out -Filter "${name}_atlas*.png" -ErrorAction SilentlyContinue
        if ($until) { if ($last -and $el -ge 6 -and (& $until $last)) { Start-Sleep -Seconds 1; break } }
        elseif ($haveAtlas -and $el -ge 6) { Start-Sleep -Seconds 1; break }
    }
    if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null; Start-Sleep -Seconds 3 }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Start-Sleep -Milliseconds 700
    $stdout = $oT.Result + $eT.Result
    $stdout | Set-Content (Join-Path $Out "${name}_stdout.txt") -Encoding utf8
    $log = Get-ChildItem "$env:LOCALAPPDATA\DisplayXR" -Filter "DisplayXR_mediaplayer_pro*.log" -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -gt $t0 } | Sort-Object LastWriteTime | Select-Object -Last 1
    $logCopy = $null
    if ($log) { $logCopy = Join-Path $Out "${name}_runtime.log"; Copy-Item $log.FullName $logCopy }
    if ($last) { $last | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $Out "${name}_status.json") -Encoding utf8 }
    $atlas = Get-ChildItem $Out -Filter "${name}_atlas*.png" -ErrorAction SilentlyContinue | Select-Object -First 1
    Start-Sleep -Seconds 1
    return @{ stdout = $stdout; atlas = $(if ($atlas) { $atlas.FullName } else { $null }); log = $logCopy; status = $last; t0 = $t0 }
}

# Mean RGB of the left and right halves of an atlas dump (the two views side by side).
function Half-Means([string]$png) {
    $bmp = [System.Drawing.Bitmap]::FromFile($png)
    try {
        $res = @()
        foreach ($half in 0, 1) {
            $x0 = [int]($half * $bmp.Width / 2); $x1 = [int](($half + 1) * $bmp.Width / 2)
            $sum = @(0.0, 0.0, 0.0); $n = 0
            for ($y = 0; $y -lt $bmp.Height; $y += 16) {
                for ($x = $x0; $x -lt $x1; $x += 16) {
                    $c = $bmp.GetPixel($x, $y); $sum[0] += $c.R; $sum[1] += $c.G; $sum[2] += $c.B; $n++
                }
            }
            $r = $sum[0] / $n; $g = $sum[1] / $n; $b = $sum[2] / $n  # (a comma binds tighter than /)
            $res += , @($r, $g, $b)
        }
        return $res
    } finally { $bmp.Dispose() }
}
function Dist($a, $b) { [math]::Sqrt(($a[0] - $b[0]) * ($a[0] - $b[0]) + ($a[1] - $b[1]) * ($a[1] - $b[1]) + ($a[2] - $b[2]) * ($a[2] - $b[2])) }
function Fmt($m) { "({0:N0},{1:N0},{2:N0})" -f $m[0], $m[1], $m[2] }
function Version-AtLeast([string]$v, [int]$maj, [int]$min) {
    if ($v -notmatch '(\d+)\.(\d+)') { return $false }
    $a = [int]$Matches[1]; $b = [int]$Matches[2]
    return ($a -gt $maj) -or ($a -eq $maj -and $b -ge $min)
}

Write-Host "Media Player Pro station acceptance -> $Out`n"

# ---------------------------------------------------------------- E: the stack
if (Want "E1") {
    if (Test-Path $Cli) {
        $st = & $Cli selftest 2>&1 | Out-String
        $st | Set-Content (Join-Path $Out "E1_selftest.txt") -Encoding utf8
        if ($LASTEXITCODE -eq 0) { Report "E1" "DisplayXR runtime selftest" "PASS" "" }
        else { Report "E1" "DisplayXR runtime selftest" "FAIL" "exit $LASTEXITCODE, see E1_selftest.txt" }
    } else { Report "E1" "DisplayXR runtime selftest" "FAIL" "displayxr-cli not found: is the runtime installed?" }
}
if ((Want "E2") -or $DeckLink) {
    if (Test-Path $Cli) {
        $info = & $Cli info 2>&1 | Out-String
        $info | Set-Content (Join-Path $Out "E2_info.txt") -Encoding utf8
        $rt = if ($info -match "git-tag:\s*'?v?([\d\.]+)") { $Matches[1] } else { "" }
        $lp = if ($info -match "DisplayXR Leia SR'\s+v?([\d\.]+)") { $Matches[1] } else { "" }
        # Runtime 2.61: display calibration keys .cube files by XrDisplayStableKeyDXR (display_info v24).
        # Desktop Video 16.0: the SDK the player is built against (third_party/decklink-sdk); older is refused.
        $what = if ($DeckLink) { "Runtime 2.61+ / Leia plug-in 2.17+ / Desktop Video 16.0+" } else { "Runtime 2.61+ / Leia plug-in 2.17+" }
        $dv = ""
        if ($DeckLink) {
            $dlList = & $Exe --decklink-list 2>&1 | Out-String
            $dv = if ($dlList -match "Desktop Video (\d+\.\d+(\.\d+)?)") { $Matches[1] } else { "" }
        }
        $fails = @()
        if (-not $rt) { $fails += "could not read the runtime version (E2_info.txt)" }
        elseif (-not (Version-AtLeast $rt 2 61)) { $fails += "runtime $rt is older than 2.61" }
        if ($lp -and -not (Version-AtLeast $lp 2 17)) { $fails += "Leia plug-in $lp is older than 2.17" }
        if ($DeckLink -and -not $dv) { $fails += "could not read the Desktop Video version (is it installed? see D1)" }
        elseif ($DeckLink -and -not (Version-AtLeast $dv 16 0)) { $fails += "Desktop Video $dv is older than 16.0" }
        $have = @("runtime $rt"); if ($lp) { $have += "Leia plug-in $lp" }; if ($dv) { $have += "Desktop Video $dv" }
        if ($fails) { Report "E2" $what "FAIL" ($fails -join "; ") }
        elseif (-not $lp) { Report "E2" $what "INFO" (($have -join ", ") + "; no Leia plug-in registered (not a Leia display?)") }
        else { Report "E2" $what "PASS" ($have -join ", ") }
    } else { Report "E2" "Runtime 2.61+ / Leia plug-in 2.17+" "SKIP" "no displayxr-cli" }
}
if (Want "E3") {
    $gpus = (Get-CimInstance Win32_VideoController | ForEach-Object { "$($_.Name) (driver $($_.DriverVersion))" }) -join "; "
    $cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
    $ram = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
    $os = (Get-CimInstance Win32_OperatingSystem).Caption
    $pv = (Get-Item $Exe).VersionInfo.ProductVersion
    "Player: $Exe ($pv)`nCPU: $cpu`nRAM: $ram GB`nGPU: $gpus`nOS: $os" | Set-Content (Join-Path $Out "E3_machine.txt") -Encoding utf8
    Report "E3" "Machine" "INFO" "$cpu, $ram GB, $gpus"
}

# ---------------------------------------------------------------- P: the picture
$base = $null
if ((Want "P*") -or (Want "G*")) {
    $r = Run-Player "P1_base" @($Stereo) @{}
    if (-not $r.atlas) { Report "P1" "Player starts and renders" "FAIL" "no atlas dump (see P1_base_stdout.txt / _runtime.log)" }
    else {
        $base = Half-Means $r.atlas
        $rt = if ($r.stdout -match "Runtime: (.+)") { $Matches[1].Trim() } else { "?" }
        # With nobody tracked the eyes collapse and the player puts the left eye in every view
        # on purpose (a clean 2D picture, src/media/EyeAssign.h), so this needs a seated viewer.
        $script:tracked = (Dist $base[0] $base[1]) -gt 60
        if ($script:tracked) { Report "P1" "Player starts, both eyes reach the panel" "PASS" "left $(Fmt $base[0]) right $(Fmt $base[1]); $rt" }
        else { Report "P1" "Player starts, both eyes reach the panel" "SKIP" "rendered, but both views show the left eye: nobody was tracked (2D by design). Sit in front of the display and rerun with -Only P1" }
    }
}
if ($base -and -not $script:tracked -and (Want "P2")) { Report "P2" "QC view: left eye on both" "SKIP" "needs a tracked viewer (see P1)" }
elseif ($base -and (Want "P2")) {
    $r = Run-Player "P2_left" @($Stereo) @{ MEDIAPLAYER_PRO_VIEW = "1" }
    if ($r.atlas) {
        $m = Half-Means $r.atlas
        if ((Dist $m[0] $m[1]) -lt 25 -and (Dist $m[0] $base[0]) -lt 40) { Report "P2" "QC view: left eye on both" "PASS" "" }
        else { Report "P2" "QC view: left eye on both" "FAIL" "halves $(Fmt $m[0]) / $(Fmt $m[1])" }
    } else { Report "P2" "QC view: left eye on both" "FAIL" "no atlas dump" }
}
if ($base -and (Want "P3")) {
    $r = Run-Player "P3_difference" @($Stereo) @{ MEDIAPLAYER_PRO_VIEW = "3" }
    if ($r.atlas) { Report "P3" "QC view: difference" "INFO" "rendered (look at P3_difference_atlas*.png)" } else { Report "P3" "QC view: difference" "FAIL" "no atlas dump" }
}
if ($base -and (Want "G1")) {
    $r = Run-Player "G1_cdl" @($Stereo, "--cdl=$cdl") @{}
    if ($r.atlas) {
        $m = Half-Means $r.atlas
        if ((Dist $m[0] $base[0]) -gt 15) { Report "G1" "Grade: CDL applies" "PASS" "left $(Fmt $base[0]) -> $(Fmt $m[0])" }
        else { Report "G1" "Grade: CDL applies" "FAIL" "picture unchanged $(Fmt $m[0])" }
    } else { Report "G1" "Grade: CDL applies" "FAIL" "no atlas dump" }
}
if ($base -and (Want "G2")) {
    $r = Run-Player "G2_lut" @($Stereo, "--lut=$lut") @{} -dumpFrame 150
    if ($r.atlas) {
        $m = Half-Means $r.atlas
        if ((Dist $m[0] $base[0]) -gt 60) { Report "G2" "Grade: 3D LUT applies" "PASS" "left $(Fmt $base[0]) -> $(Fmt $m[0])" }
        else { Report "G2" "Grade: 3D LUT applies" "FAIL" "picture unchanged $(Fmt $m[0])" }
    } else { Report "G2" "Grade: 3D LUT applies" "FAIL" "no atlas dump" }
}
if (Want "P4") {
    $r = Run-Player "P4_10bit" @("--10bit", $Stereo) @{}
    $deep = $r.log -and (Select-String -Path $r.log -Pattern "R10G10B10A2|rgb10a2" -Quiet)
    $warn = $r.stdout -match "needs DisplayXR runtime 2.53"
    if ($warn) { Report "P4" "10-bit picture to the panel" "FAIL" "runtime too old for --10bit" }
    elseif (-not $r.atlas -and -not (Get-ChildItem $Out -Filter "P4_10bit*woven*.png")) { Report "P4" "10-bit picture to the panel" "FAIL" "player did not render (P4_10bit_stdout.txt)" }
    elseif ($deep) { Report "P4" "10-bit picture to the panel" "PASS" "runtime log shows the 10-bit atlas; confirm the picture by eye (manual M3)" }
    else { Report "P4" "10-bit picture to the panel" "FAIL" "no 10-bit atlas in the runtime log (P4_10bit_runtime.log)" }
}

# ---------------------------------------------------------------- R: rig checks on the built-in test rig
# The default test signal: moving discs at known depths over a gradient, faults dialled in.
# $stopEarly: end the run as soon as $ok holds (a fault found); otherwise run the full time.
# $detail: what to print instead of the alert list (a check judged on a number, not an alert).
function Rig-Run([string]$id, [string]$what, [hashtable]$faults, [string[]]$extra, [scriptblock]$ok, [int]$seconds = 25, [bool]$stopEarly = $true, [scriptblock]$detail = $null) {
    $envs = @{ MEDIAPLAYER_PRO_DEPTH_OVERLAY = "1" }
    foreach ($k in $faults.Keys) { $envs[$k] = $faults[$k] }
    $until = if ($stopEarly) { $ok } else { { param($s) $false } }
    $r = Run-Player "${id}_rig" (@("--test-signal") + $extra) $envs -dumpFrame 400 -seconds $seconds -until $until
    if (-not $r.status -or -not $r.status.rig) { Report $id $what "FAIL" "no station status (is Pictures\DisplayXR writable?)"; return }
    $alerts = @($r.status.rig.alerts | ForEach-Object { $_.what }) -join " | "
    $said = if ($detail) { & $detail $r.status } else { $alerts }
    if (& $ok $r.status) { Report $id $what "PASS" $said } else { Report $id $what "FAIL" $(if ($detail) { $said } else { "alerts: [$alerts]" }) }
}
# The test scene sits at about -2.0 % (foreground) / +1.2 % (background), so the other checks
# use a -3,2 budget it fits inside, and R4 tightens it to make the depth alert fire.
$roomy = "--depth-budget=-3,2"
if (Want "R1") { Rig-Run "R1" "Rig: clean rig raises no alert" @{} @($roomy) { param($s) $s.rig -and $s.rig.measured -and @($s.rig.alerts).Count -eq 0 } 20 $false }
if (Want "R2") { Rig-Run "R2" "Rig: 4 px vertical error is flagged" @{ MEDIAPLAYER_TEST_VSHIFT = "4" } @($roomy) { param($s) @($s.rig.alerts | Where-Object { $_.what -match "ertical" }).Count -gt 0 } }
if (Want "R3") { Rig-Run "R3" "Rig: colour mismatch is flagged" @{ MEDIAPLAYER_TEST_GAIN_R = "1.25" } @($roomy) { param($s) @($s.rig.alerts | Where-Object { $_.what -match "olou" }).Count -gt 0 } }
if (Want "R4") { Rig-Run "R4" "Rig: depth over budget is flagged" @{} @("--depth-budget=-0.5,0.5") { param($s) @($s.rig.alerts | Where-Object { $_.what -match "ground at" }).Count -gt 0 } }
if (Want "R5") { Rig-Run "R5" "Rig: 2-frame eye delay is measured" @{ MEDIAPLAYER_TEST_EYE_DELAY = "2" } @($roomy) { param($s) $s.rig.eye_sync_measured_frames -eq 2 } 40 $true { param($s) "measured $($s.rig.eye_sync_measured_frames) frames, $($s.rig.eye_sync_late_eye) eye late (injected 2)" } }

# ---------------------------------------------------------------- Q: snapshot + QC report
if (Want "Q1") {
    $script:qT0 = Get-Date
    $r = Run-Player "Q1_qc" @("--test-signal") @{ MEDIAPLAYER_PRO_SNAPSHOT_FRAME = "150"; MEDIAPLAYER_PRO_SNAPSHOT_NOTE = "acceptance"; MEDIAPLAYER_PRO_QC_EXPORT_FRAME = "400" } -dumpFrame 120 -seconds 30 -until {
        param($s) [bool](Get-ChildItem $Pics -Directory -Filter "qc_*" | Where-Object { $_.LastWriteTime -gt $script:qT0 -and (Test-Path (Join-Path $_.FullName "report.html")) })
    }
    $qc = Get-ChildItem $Pics -Directory -Filter "qc_*" | Where-Object { $_.LastWriteTime -gt $r.t0 } | Sort-Object LastWriteTime | Select-Object -Last 1
    $snap = Get-ChildItem $Pics -Filter "*_note.json" | Where-Object { $_.LastWriteTime -gt $r.t0 }
    if ($qc -and (Test-Path "$($qc.FullName)\report.html") -and (Test-Path "$($qc.FullName)\markers.edl") -and (Test-Path "$($qc.FullName)\markers.csv") -and $snap) {
        Copy-Item $qc.FullName (Join-Path $Out "Q1_qc_report") -Recurse
        Report "Q1" "Snapshot + QC report export" "PASS" "$($qc.Name): report.html, markers.edl, markers.csv"
    } else { Report "Q1" "Snapshot + QC report export" "FAIL" "snapshot: $([bool]$snap), report folder: $([bool]$qc)" }
}

# ---------------------------------------------------------------- D: the capture card
if (-not $DeckLink) {
    if (Want "D*") { Report "D*" "DeckLink checks" "SKIP" "run again with -DeckLink once the card has signals connected" }
} else {
    if (Want "D1") {
        $list = & $Exe --decklink-list 2>&1 | Out-String
        $list | Set-Content (Join-Path $Out "D1_decklink_list.txt") -Encoding utf8
        if ($list -match "Blackmagic devices") { Report "D1" "DeckLink card + driver found" "PASS" (($list -split "`n" | Select-Object -Skip 1 | ForEach-Object { $_.Trim() } | Where-Object { $_ }) -join "; ") }
        else { Report "D1" "DeckLink card + driver found" "FAIL" $list.Trim() }
    }
    function Live-Ok($s) { $s.rig -and $s.rig.decklink -and $s.rig.decklink.signal_fps -gt 0 -and $s.rig.decklink.delivered_fps -gt 0 }
    # The format actually captured against what the feeds must be; "" when it matches.
    function Format-Miss($d) {
        $miss = @()
        if ([int]$d.eye_width -lt $ExpectEyeWidth) { $miss += "eye width $($d.eye_width), expected $ExpectEyeWidth" }
        if ([math]::Abs([double]$d.signal_fps - $ExpectFps) -gt 0.1) { $miss += "signal $($d.signal_fps) fps, expected $ExpectFps" }
        if ([int]$d.bit_depth -lt $ExpectBits) { $miss += "$($d.bit_depth)-bit, expected $ExpectBits" }
        return ($miss -join "; ")
    }
    if (Want "D2") {
        $r = Run-Player "D2_one_input" @("--decklink=$Left", "--decklink-input=$Connection") @{} -dumpFrame 300 -seconds 25
        $d = if ($r.status -and $r.status.rig) { $r.status.rig.decklink } else { $null }
        if (-not $d) { Report "D2" "One input: live picture at full rate" "FAIL" "no capture stats (no signal on input $Left?)" }
        elseif ($d.signal_fps -le 0) { Report "D2" "One input: live picture at full rate" "FAIL" "no signal detected" }
        elseif (Format-Miss $d) { Report "D2" "One input: live picture at full rate" "FAIL" (Format-Miss $d) }
        elseif ($d.delivered_fps -lt 0.98 * $d.signal_fps -or $d.late_frames -gt 0) { Report "D2" "One input: live picture at full rate" "FAIL" ("{0:N2} of {1:N2} fps, late {2}, dropped {3}" -f $d.delivered_fps, $d.signal_fps, $d.late_frames, $d.dropped) }
        else { Report "D2" "One input: live picture at full rate" "PASS" ("{0}x{1} {2}-bit, {3:N2} fps, TC {4}" -f $d.width, $d.height, $d.bit_depth, $d.delivered_fps, $r.status.timecode) }
    }
    if (Want "D3") {
        # #141 Done when: two 4K60 10-bit inputs hold 60 fps with no unpaired frames climbing over 10 minutes.
        $csv = Join-Path $Out "D3_soak.csv"
        "elapsed_s,signal_fps,delivered_fps,late_frames,dropped,published,pairs,unpaired,max_skew_ms,timecode,eye_width,bit_depth,pairing" | Set-Content $csv -Encoding ascii
        $inv = [Globalization.CultureInfo]::InvariantCulture  # no thousands separators or decimal commas in the CSV
        $secs = [int]($SoakMinutes * 60 + 20)
        Write-Host "     D3 two-input soak: $SoakMinutes min, inputs $Left + $Right ($Connection)..." -ForegroundColor Gray
        $script:soakT0 = Get-Date
        $script:soakLastRow = [datetime]::MinValue
        $r = Run-Player "D3_two_input" @("--decklink=$Left", "--decklink-right=$Right", "--decklink-input=$Connection") @{} -dumpFrame 600 -seconds $secs -until {
            param($s)
            # The status poll runs every 0.5 s; the CSV keeps one row every 2 s.
            if ($s.rig -and $s.rig.decklink -and ((Get-Date) - $script:soakLastRow).TotalSeconds -ge 2) {
                $script:soakLastRow = Get-Date
                $d = $s.rig.decklink
                [string]::Format($inv, "{0:F0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12}", ((Get-Date) - $script:soakT0).TotalSeconds, $d.signal_fps, $d.delivered_fps, $d.late_frames, $d.dropped, $d.published, $d.pairs, $d.unpaired, $d.max_skew_ms, $s.timecode, $d.eye_width, $d.bit_depth, $d.pairing) | Add-Content $csv
            }
            return $false
        }
        $rows = @(Import-Csv $csv | Where-Object { [double]$_.elapsed_s -ge 20 })
        if ($rows.Count -lt 3) { Report "D3" "Two inputs: $SoakMinutes min soak" "FAIL" "no capture stats (both inputs connected and live?)" }
        else {
            $first = $rows[0]; $lastRow = $rows[-1]
            $fps = [double]$lastRow.signal_fps
            $minFps = ($rows | ForEach-Object { [double]$_.delivered_fps } | Measure-Object -Minimum).Minimum
            $unpairedGrowth = [long]$lastRow.unpaired - [long]$first.unpaired
            $late = [long]$lastRow.late_frames; $dropped = [long]$lastRow.dropped
            $skew = [double]$lastRow.max_skew_ms; $half = if ($fps -gt 0) { 500.0 / $fps } else { 0 }
            # Frames the card delivered that never reached the panel (the ring keeps the newest).
            $droppedGrowth = [long]$lastRow.dropped - [long]$first.dropped
            $publishedGrowth = [long]$lastRow.published - [long]$first.published
            $dropShare = if ($publishedGrowth -gt 0) { $droppedGrowth / $publishedGrowth } else { 1 }
            $miss = Format-Miss ([pscustomobject]@{ eye_width = $lastRow.eye_width; signal_fps = $lastRow.signal_fps; bit_depth = $lastRow.bit_depth })
            $paired = $lastRow.pairing -in @("timecode", "hardware_clock")  # "newest" = not frame-accurate
            $detail = "{0:N2} fps signal, {1}-wide {2}-bit eyes, paired by {3}, min delivered {4:N2}, unpaired +{5}, late {6}, dropped +{7} ({8:P1}), max skew {9:N2} ms" -f $fps, $lastRow.eye_width, $lastRow.bit_depth, $lastRow.pairing, $minFps, $unpairedGrowth, $late, $droppedGrowth, $dropShare, $skew
            if ($miss) { $detail = "$miss; $detail" }
            if (-not $miss -and $paired -and $fps -gt 0 -and $minFps -ge 0.98 * $fps -and $unpairedGrowth -le 0 -and $late -eq 0 -and $dropShare -le 0.02 -and $skew -lt $half) { Report "D3" "Two inputs: $SoakMinutes min soak" "PASS" $detail }
            else { Report "D3" "Two inputs: $SoakMinutes min soak" "FAIL" $detail }
        }
    }
}

# ---------------------------------------------------------------- summary
Get-Process mediaplayer_pro_handle_vk_win -ErrorAction SilentlyContinue | Stop-Process -Force
$sum = Join-Path $Out "summary.md"
$pass = @($results | Where-Object result -eq "PASS").Count
$fail = @($results | Where-Object result -eq "FAIL").Count
$lines = @("# Media Player Pro station acceptance", "", "$(Get-Date -Format 'yyyy-MM-dd HH:mm') on ${env:COMPUTERNAME}: $pass passed, $fail failed", "",
    "| id | check | result | detail |", "|---|---|---|---|")
foreach ($x in $results) { $lines += "| $($x.id) | $($x.check) | $($x.result) | $($x.detail -replace '\|', '/') |" }
$lines += ""; $lines += "Manual checks (M1-M8 in docs/STATION-ACCEPTANCE.md) are not in this table."
$lines | Set-Content $sum -Encoding utf8
$zip = "$Out.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$Out\*" -DestinationPath $zip
Write-Host "`n$pass passed, $fail failed. Evidence: $Out" -ForegroundColor $(if ($fail) { "Red" } else { "Green" })
Write-Host "Send back: $zip"
if ($fail) { exit 1 } else { exit 0 }
