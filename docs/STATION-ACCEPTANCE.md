# Media Player Pro station: day-one acceptance

What to run on a new Media Player Pro station (the PC, the 3D display and the DeckLink
card) before it is used for review, and what to send back. It is the hardware validation
pass of #142 on Windows, in the order that finds problems earliest. About 30 minutes
without the card checks, plus the 10-minute capture soak.

## 1. Set up

- **DisplayXR runtime 2.61 or newer** and the **Leia plug-in 2.17 or newer** (the
  DisplayXR bundle installs both). Display calibration needs 2.61 (it keys the `.cube`
  files by the display's stable key); older versions must not be used with `--10bit`.
- **Media Player Pro** installer (`DisplayXRMediaPlayerProSetup-*.exe`).
- **Blackmagic Desktop Video 16.0 or newer** (the DeckLink driver; the player is built
  against SDK 16.0 and refuses an older driver). Restart after installing it.
- The **DeckLink 8K Pro G2 in an x8 or x16 slot**. It is a PCIe Gen 3 x8 card and is not
  supported in an x4 slot: two 4K60 inputs need about 2.7 GB/s, close to what an x4 slot
  carries. Desktop Video Setup shows the link width.
- Signals for the card checks: two 4K60 10-bit SDI feeds (left and right eye, genlocked,
  with RP188 timecode) on inputs 1 and 2. Any one feed will do for the single-input check.
- Close Desktop Video Setup and Media Express before the card checks: they claim the inputs.

## 2. Run the automated checks

The installer puts the script in `C:\Program Files\DisplayXR\Apps\MediaPlayerPro\tools\acceptance\`
(also `scripts\station_acceptance.ps1` in the repo). From that folder:

```
powershell -ExecutionPolicy Bypass -File station_acceptance.ps1
```

Sit in front of the display while it runs (several checks need a tracked viewer), and do
not touch the mouse. The player opens about a dozen times, 20 to 40 s each, and closes
itself. Then, with the feeds connected:

```
powershell -ExecutionPolicy Bypass -File station_acceptance.ps1 -DeckLink -Only "D*" -Left 0 -Right 1 -SoakMinutes 10
```

`-Left` / `-Right` take a device number or part of its name (`--decklink-list` shows
them); `-Connection hdmi` for HDMI.

| id | checks | passes when |
|---|---|---|
| E1 | runtime selftest | `displayxr-cli selftest` exits 0 |
| E2 | versions | runtime 2.61+, Leia plug-in 2.17+; with `-DeckLink` also Desktop Video 16.0+ |
| E3 | machine | (recorded: CPU, RAM, GPU, driver, OS) |
| P1 | the picture reaches the display, both eyes | the two views differ (needs a tracked viewer) |
| P2 | QC view: left eye | both views show the left eye |
| P3 | QC view: difference | (recorded) |
| G1 / G2 | a CDL / a 3D LUT change the picture | the picture changes as expected |
| P4 | `--10bit` | the runtime reports the 10-bit atlas |
| R1 | clean test rig | no alert in 20 s |
| R2 / R3 / R4 | 4 px vertical error / colour mismatch / depth over budget | each raises its alert |
| R5 | right eye 2 frames late | eye sync reads 2 frames |
| Q1 | snapshot + QC report | report.html, markers.edl, markers.csv written |
| D1 | card and driver | `--decklink-list` names the inputs |
| D2 | one input | delivered rate within 2 % of the signal's, nothing late |
| D3 | two inputs for 10 minutes (#141) | rate held, unpaired not climbing, nothing late, skew under half a frame |

It writes everything to `Desktop\station_acceptance_<time>\` (atlas dumps, logs, the QC
report, `D3_soak.csv` with a row every 2 s, `summary.md`) and a `.zip` of it.

## 3. Check by eye

These need a person in front of the display. Note each as pass / fail with a line on what
was seen.

| id | check |
|---|---|
| M1 | **3D looks right**: a real stereo clip in depth, the right way round (`X` swaps the eyes: the wrong way must look inside-out). Look away from the camera: the picture goes 2D cleanly, no ghosting. |
| M2 | **Live picture from the card**: both eyes live, the timecode chip advancing one frame per frame at 24/25p **and at 48/50/60p**, drop-frame shown with `;`. |
| M3 | **10-bit**: a smooth gradient (sky, vignette, a grey ramp) under a strong LUT, `--8bit` then `--10bit`: the 10-bit one shows less banding. |
| M4 | **Pull one cable** during the two-input run: that eye shows NO SIGNAL within 2 s (never a frozen eye next to a live one); plug it back: LIVE, without a restart. |
| M5 | **Formats** the production will use (rate, 1080p / 2160p / DCI, 8 / 10 bit, PsF): each shows, colours right. Interlaced is warned about. |
| M6 | **Resolve panel** in Resolve Studio (Workspace > Workflow Integrations > DisplayXR Station): snapshot and alert markers appear live; *Follow Resolve* tracks the playhead; *Go to station TC* jumps. Note whether the follow is offset by the clip's start timecode. |
| M7 | **Resolve EDL**: import the Q1 report's `markers.edl` (Timeline > Import > Timeline Markers from EDL): markers land at their timecodes. |
| M8 | **Resolve stereo output** (side by side, and dual-stream) into the card: Auto picks the right layout, both eyes show. |

The full list of card cases is in `docs/PRO.md`, *DeckLink: first checks on a card*.

## 4. Send back

The `.zip`, the M1-M8 notes, and the versions from `summary.md` (E2, E3). Any failure gets
its own issue in this repo with the zip attached.
