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
| Live inputs | `src/pro/Station`: named inputs, `J` or the inspector's **Inputs** switches. `--input="Rig=decklink:0+1,sdi"`, `--input="Resolve=decklink:2,sbs"`, `--input="Witness=camera:Integrated"`, `--input="Test=test:1920x1080@24"`, `url:<u>`; shorthands `--decklink[=<dev>]` `--decklink-right=` `--decklink-input=sdi\|hdmi` `--decklink-stereo=auto\|mono\|sbs\|dual\|pair` `--decklink-mode=Hp25`, `--test-signal[=WxH@fps]`; `--decklink-list` prints the cards. |
| DeckLink input | `src/pro/DeckLinkSource`: one cable (mono / SBS), the card's dual-stream 3D mode (switches automatically when the card detects it), or two inputs paired by SMPTE timecode (`TimecodePairer`). Format detection follows the signal. Captures 10-bit v210 and keeps 16 bits (P016) into the grade. Publishes into the same live path as the camera (`LiveProducer` / `App::EnterLive`). |
| Test rig | `src/pro/TestSignalSource`: synthetic stereo with known disparities and timecode; faults `MEDIAPLAYER_TEST_VSHIFT` (px), `_EYE_DELAY` (frames), `_GAIN_R`, `_DEPTH`. Exercises everything below without hardware. |
| Timecode + delay | Source timecode and measured capture → screen delay (the card's or camera's arrival → the panel frame it is shown in; the camera's and panel's own delays come on top). In the inspector's Source section; `T` toggles. |
| Rig checks | `src/pro/RigCheck`: vertical error (sub-pixel 2D block matching, full-res px) + roll, eye sync (frames between the paired inputs' timecodes), colour match (CIE76 ΔE between the eyes), depth range (2nd / 98th percentile parallax, % of eye width, negative = in front) against `--depth-budget=-2,1`; `--vertical-limit=1` `--colour-limit=2`. Overruns ≥ 0.5 s become alerts with timecode, also appended to `Pictures/DisplayXR/station_alerts.jsonl`. 60-second history strip (depth band vs budget, vertical error); `R` toggles. |
| Snapshots | `N` or the inspector: both eyes as shown (graded) via the runtime's atlas capture, plus `<name>_note.json` (note, timecode, rig numbers, budget); notes are editable in the inspector. |
| Depth overlay | `D`: the rig check's matches drawn on the picture, coloured by parallax (warm in front, white on the screen, blue behind), ringed red outside the budget; follows pan / zoom. |
| Review formats | MXF; EXR / DPX / TIFF **sequences** (open any frame; `--seq-fps=24`); EXR's scene-linear float encoded for display; 10/12-bit sources (ProRes, DNxHR HQX, HEVC 10, DPX, 16-bit TIFF) kept at 16 bits (P016) into the grade. Trunk code, switched on only by Pro (`VideoDecoder::SetProFormats` / `MediaSource::SetProFormats`). |
| A/B compare | `B` swaps to the other clip at the same position (`--compare=<file>` or `set_compare`). |
| OpenColorIO | Optional (`-DMP_OCIO_ROOT=<OCIO 2.x install>`, e.g. Open RV's `_build/RV_DEPS_OCIO/install`, plus `MP_OCIO_DLL_DIR` for its DLLs): `--ocio=<config\|ocio://default> --ocio-src=<space>` and `--ocio-display= --ocio-view=` or `--ocio-dst=`, `--ocio-looks=`; baked to a 65³ LUT in the grade's LUT slot. |
| Agent tools | `set_grade`, `get_grade`, `set_view`, `set_depth_overlay`, `get_rig_status`, `set_depth_budget`, `snapshot`, `list_inputs`, `select_input`, `open_decklink`, `set_ocio`, `describe_ocio`, `set_compare` (next to the player's own). |
| Resolve markers | `tools/resolve/DisplayXR_Station_Markers.py` (Resolve: Workspace > Scripts): snapshot notes → blue markers, rig alerts → red markers at their timecodes on the current timeline. |

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

## Next (from the plan)

Ordered by what the customer needs first.

Items 1-6 below are built (see the table above) and unit-tested; **none of the DeckLink,
rig or Resolve parts has run with a card, a stereo rig or Resolve yet** - that needs the
hardware. Still open: a Resolve *panel* inside Resolve (timecode follow + markers live;
today the marker script runs after the fact, and the station follows Resolve's timecode
over SDI); RP188 at 50/60p (frame/field counting) to confirm on a card. Kept for the record:

1. **Blackmagic DeckLink input (Phase 1).** A `src/pro/DeckLinkSource.{h,cpp}` that
   publishes into the same `FrameRing` the camera uses, so the render path is untouched.
   Two inputs, frame-synced into one SBS frame (two-cable stereo), plus single-cable
   SBS. Upload as NV12 / v210-unpacked like the camera path. Suki's Open RV port already
   has DeckLink input code to port. Build it behind `MP_DECKLINK_SDK_DIR` (the SDK is a
   registration download; never commit it). Needs a card: a DeckLink Mini Recorder 4K
   (~$235, 1x 6G-SDI + HDMI, 2160p30) to start, a DeckLink 8K Pro G2 (~$855, 4x 12G-SDI)
   for the real rig.
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
