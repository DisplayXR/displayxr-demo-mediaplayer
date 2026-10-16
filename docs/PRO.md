# Media Player Pro

Media Player Pro is a 3D monitoring and review player for stereo productions, built
from the same source tree as the DisplayXR Media Player. The first target user is a
stereo production that wants to see its camera rig live, graded, on a glasses-free 3D
panel, and to use the panel as the 3D monitor for footage reviewed in DaVinci Resolve.

## One trunk, two products

| | Media Player | Media Player Pro |
|---|---|---|
| Binary | `mediaplayer_handle_vk_<plat>` | `mediaplayer_pro_handle_vk_<plat>` |
| Build | always | `MP_BUILD_PRO` (ON by default on Windows, OFF elsewhere) |
| Compile flag | `MEDIAPLAYER_PRO=0` | `MEDIAPLAYER_PRO=1` + `src/pro/` |
| Installer | `DisplayXRMediaPlayerSetup-X.Y.Z.exe` | `DisplayXRMediaPlayerProSetup-X.Y.Z.exe` |
| Install dir | `Program Files\DisplayXR\Demos\MediaPlayer` | `Program Files\DisplayXR\Apps\MediaPlayerPro` |
| Release tag | `vX.Y.Z` | `pro-vX.Y.Z` |
| In the DisplayXR bundle / `versions.json` | yes | **no** |

Both are produced by one CMake function, `mp_add_player()` in `CMakeLists.txt`, so a
trunk change lands in both and they cannot diverge.

**The rule that keeps it that way:** Pro-only code lives in `src/pro/`. The trunk calls
into it only from the `#if MEDIAPLAYER_PRO` hooks in `App.cpp` (search for
`MEDIAPLAYER_PRO`; there are about ten, plus the glue block at the end of the file).
Do not copy a trunk file into `src/pro/` to change it. If Pro needs a new capability from
the trunk, add it to the trunk in a form the base player can ignore (the renderer's
`GradeParams` is the pattern: identity by default, so the base player behaves exactly as
before).

Shared pieces that Pro extended in the trunk:

- `shaders/sbs.frag`: the grade (exposure → ASC CDL → 3D LUT) and the QC views, all
  behind `#ifdef MP_GRADE`. The desktop build compiles with `-DMP_GRADE`
  (`scripts/gen_shaders.sh`). **The Android leg compiles the same file without it**, so
  its 3-binding layout and 28-byte push block in `android/.../sbs_renderer.cpp` stay
  valid. Keep new shader features behind `MP_GRADE` unless you also update Android.
- `VulkanRenderer`: `SetGrade()`, `UploadLut3D()` (RGBA16F 3D texture at binding 3),
  `ViewUV::otherDX/otherDY` (where the other eye is, for the QC views).
- `Window`: `SetEscapeQuits(false)` (Pro: Escape leaves full screen, never quits).
- `src/color/`: `.cube` and ASC CDL loaders, compiled into both binaries, unit-tested by
  `tests/color_test.cpp`.

## What Pro does today (Phase 1, first slice)

Everything the Media Player does: stereo files, live UVC cameras (Media Foundation,
NV12, newest frame wins, 60 fps measured on a SpatialLabs Eyes at 3840x2160), network
streams, auto-convergence, MCP tools. Plus:

| Feature | How |
|---|---|
| Inspector panel | Right-hand ImGui panel: source, view, stereo, color. `Tab` pins / unpins it. |
| QC views | `1` 3D, `2` left eye, `3` right eye, `4` eye difference (x4), `5` anaglyph. Diff and anaglyph address the two SBS halves directly, so they work with no viewer tracked. |
| Pan / zoom | Mouse wheel zooms about the pointer, middle-drag (or left-drag while zoomed) pans, `H` fits, `Z` toggles 1:1. Identical for both eyes, so depth stays correct while zoomed. |
| 3D LUT | `.cube` (2..256, `DOMAIN_MIN/MAX`), from the inspector, a file drop, `--lut <file>` or `MEDIAPLAYER_LUT`. |
| ASC CDL | `.cc` / `.ccc` / `.cdl` (slope, offset, power, saturation; editable in the inspector), from the inspector, a drop, `--cdl <file>` or `MEDIAPLAYER_CDL`. |
| Exposure | ±4 stops in linear light. Right-click the slider to reset. |
| Grade bypass | `G` |
| Full screen | `F` / `F11` / double-click the picture. Escape leaves full screen and never quits. |
| SDI / HDMI input (DeckLink) | One input carrying side by side, or two inputs (left + right camera) paired by the card's hardware timestamp. Inspector section "SDI / HDMI input", or `--decklink=0` / `--decklink=0,1` / `MEDIAPLAYER_DECKLINK`. **Written and compiled, not yet run on a card**; see below. |

Grade order is exposure → CDL → LUT, the order an on-set chain applies a CDL under a
show LUT. The grade is skipped on the idle screen.

Test-only env vars (headless atlas checks): `MEDIAPLAYER_PRO_VIEW=0..4`,
`MEDIAPLAYER_PRO_ZOOM=<x>`, with the trunk's `MEDIAPLAYER_DUMP_ATLAS=<path>`.

## Build, run, release

```bat
:: same as the Media Player; Pro is built alongside it on Windows
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFFMPEG_ROOT=<ffmpeg dev tree>
cmake --build build
build\mediaplayer_pro_handle_vk_win.exe --camera="SpatialLabs Eyes" --lut show.cube
set VERSION=0.1.0 && installer\build-installer.bat pro
```

CI (`build-windows.yml`) builds and validates both apps and both installers on every PR.
A `pro-vX.Y.Z` tag publishes only the Pro installer, on a release named
"Media Player Pro X.Y.Z" that is never marked latest and never dispatches the
`versions.json` bump. Android's version stamps match `v[0-9]*` only, so `pro-v*` tags
cannot disturb them. `/dxr-release` does not know about Pro; tag it by hand
(`git tag pro-v0.1.0 && git push origin pro-v0.1.0`) until it does.

The installer reuses the Media Player icons. Generate Pro's own with `/make-app-logos`
once the UI settles.

## DeckLink input

`src/pro/DeckLinkSource.{h,cpp}` publishes into a `FrameRing` exactly like the camera,
so the render path, the grade and the QC views apply unchanged. `App` swaps its frame
source to it (`proLive_`, `StartProLive()` / `StopProLive()` in the Pro glue).

- **Single input:** the signal is already side by side (Resolve's stereo output, a rig
  processor). Published as is; the layout settles from the frame shape.
- **Two inputs:** each input converts into its own eye buffer. `StereoPairer` matches
  left and right by `GetHardwareReferenceTimestamp` within half a frame, and only a
  matched pair is published, as one full-width SBS frame (so the layout settles to
  SBS-full). Unmatched frames and the measured eye skew are counted and shown in the
  inspector: the first rig-sync check.
- **Formats:** 8-bit (`2vuy`) and 10-bit (`v210`) YUV 4:2:2, auto-detected when the card
  supports it (`VideoInputFormatChanged` re-enables the input in the detected mode). 10-bit
  is reduced to 8-bit NV12 for now (the panel path is 8-bit). RGB 4:4:4 input is refused
  with a clear message. RP188 timecode is read and shown.
- **Hardware-free half:** `src/pro/DeckLinkFrames.{h,cpp}` (UYVY / v210 -> NV12 into a
  half of an SBS frame, and the pairer) is unit-tested in `tests/decklink_frames_test.cpp`
  and builds everywhere.

**Building it.** The Blackmagic DeckLink SDK is a registration download from
blackmagicdesign.com and is **never committed**. Configure with
`-DMP_DECKLINK_SDK_DIR=<SDK>/Win/include` (the folder with `DeckLinkAPI.idl`); CMake runs
MIDL into `build/decklink/`. Without it `DeckLinkSource` is a stub that reports "this
build has no DeckLink support", so CI and the default build are unaffected. Running it
also needs Blackmagic **Desktop Video** (the driver); without the driver the inspector
and the log say so.

**Verified so far** (8K box, no card): the SDK build compiles and links against SDK
12.0 (MIDL from the IDL); `--decklink` with no driver reports "Blackmagic Desktop Video
driver not installed" and the app keeps running; the converter and pairer tests pass.
**Not verified:** anything with a real signal: format detection, frame pacing, two-input
pairing on genlocked cameras, the SDK 14.3+ `IDeckLinkVideoBuffer` branch (written, never
compiled). First things to check on a card:

1. Single input, 1080p and 2160p, 8- and 10-bit: picture correct, colours limited-range.
2. Two inputs from one genlocked rig: `eye skew` near 0, `unpaired` not climbing.
3. Unplug / replug the signal: NO SIGNAL then LIVE again, no restart needed.
4. Cost: conversion runs on the SDK callback thread under one lock. At 4K60 two-input,
   if `fps` drops below the source rate, convert outside the lock (per-eye buffers are
   already separate) or move the v210 unpack to the GPU.

## Next (from the plan)

Ordered by what the customer needs first.

1. **Validate the DeckLink input on a card (Phase 1).** Written (above); needs a card:
   a DeckLink Mini Recorder 4K (~$235, 1x 6G-SDI + HDMI, 2160p30) to start, a DeckLink
   8K Pro G2 (~$855, 4x 12G-SDI) for the real rig. Compare with Suki's Open RV DeckLink
   code for anything it handles that this does not.
2. **Timecode overlay and measured end-to-end delay (Phase 1).** SDI timecode from the
   DeckLink; show source → panel latency on screen.
3. **Rig checks (Phase 2).** Measured vertical misalignment, eye sync and colour match
   between the eyes; depth range against a budget with a 60-second history strip. The
   disparity estimator in `media/AutoConvergence.cpp` is the starting point.
4. **Snapshots with notes (Phase 2).** Both eyes with grade, timecode and a note.
5. **Resolve as the editor (Phase 3).** Confirm Resolve Studio's stereo SDI output into
   the DeckLink input; detect its formats.
6. **Review tools (Phase 4).** A Resolve panel (timecode follow, markers), ProRes / EXR /
   DPX, A/B compare, OpenColorIO.

Known limits: 8-bit to the panel (the runtime's atlas path); the inspector is an ImGui
panel in the HUD layer, so it is woven with the picture rather than drawn as a crisp 2D
zone (moving it to `XR_DXR_display_zones` is planned).
