# Media Player Pro

Media Player Pro is a 3D monitoring and review player for stereo productions, built
from the same source tree as the DisplayXR Media Player. The first target user is a
stereo production that wants to see its camera rig live, graded, on a glasses-free 3D
panel, and to use the panel as the 3D monitor for footage reviewed in DaVinci Resolve.

This is the developer reference. For using the player, see the
[user guide](PRO-USER-GUIDE.md).

## One trunk, two products

| | Media Player | Media Player Pro |
|---|---|---|
| Binary | `mediaplayer_handle_vk_<plat>` | `mediaplayer_pro_handle_vk_<plat>` |
| Build | always | `MP_BUILD_PRO` (ON by default on Windows, OFF elsewhere; `scripts/build_macos.sh --pro`) |
| Compile flag | `MEDIAPLAYER_PRO=0` | `MEDIAPLAYER_PRO=1` + `src/pro/` |
| Installer | `DisplayXRMediaPlayerSetup-X.Y.Z.exe`, `DisplayXRMediaPlayer-X.Y.Z.pkg` | `DisplayXRMediaPlayerProSetup-X.Y.Z.exe`, `DisplayXRMediaPlayerPro-X.Y.Z.pkg` |
| Install dir | `Program Files\DisplayXR\Demos\MediaPlayer`, `/Applications/Stereo Media Player.app` | `Program Files\DisplayXR\Apps\MediaPlayerPro`, `/Applications/Media Player Pro.app` |
| macOS bundle / pkg id | `com.displayxr.mediaplayer` | `com.displayxr.mediaplayer.pro` (installs alongside) |
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
| Panels | Two ImGui panels, laid out as in the plan's mockup. **Station** (left): pin box, inputs + *DeckLink setup...*, source with timecode and delay, looks (LUT, CDL), snapshots. **Inspector** (right): view, stereo, colour (exposure, look, CDL values folded), rig check. Unpinned by default: they appear when the mouse moves and fade with the player's controls. `Tab` or *Pin panels* keeps them up; `Shift+Tab` (the player's own key) hides or shows all UI. |
| Status bar | Along the bottom of the picture: LIVE, input, format, look, exposure, TC, near / far, convergence, A/B side. Fades with the panels; when faded, and in full screen, only a timecode chip stays (`T` hides it). |
| QC views | `1` 3D, `2` left eye, `3` right eye, `4` eye difference (x4), `5` anaglyph, `6` false colour. Diff and anaglyph address the two SBS halves directly, so they work with no viewer tracked. |
| Channels + gamma | `9` (Inspector *View*): RGB / red / green / blue / alpha / luma, a single channel shown as grey; display gamma trim 0.5-2.5 (right-click resets). After the grade and the QC view, before the zebras; the scopes measure the grade without it. |
| Wipe | `8` (Inspector *View*): a vertical split across the picture as shown (not panned / zoomed), dragged with the left button within 12 pt of the line. *Before \| after grade*: source left, graded right. *Left eye \| right eye*: every view shows the same split (like the difference view), for rig alignment. *Clip A \| B*: right of the line the compare clip B (`src/pro/CompareClip`: its own `VideoDecoder`, or a still, following A - pause mirrored, pinned within half a frame while paused, a drift over 2 frames corrected at most every 0.5 s, `DecideSync`, `tests/compare_clip_test.cpp`), sampled from the renderer's compare planes (bindings 4-6, `UploadCompare*`; B's mode / range / matrix / P016 in `viewFlags` bits 8-14) and graded like A. In the grade shader (`viewFlags`, `wipeGamma`; the push block is now at the 128-byte floor, so position and gamma share a word, `rhi/PushPack.h`). |
| Framing overlays | `src/pro/Overlays`, Inspector *Overlays*: frame lines 2.39 / 1.85 / 16:9 / 9:16 / 4:3 / 1:1 (`Q` cycles) with a matte (0..1), action / title safe 93 / 90 % (EBU R 95), centre cross, anamorphic desqueeze 1.3 / 1.5 / 1.8 / 2.0x (the picture is fitted wider), burn-ins: timecode, frame number (files), clip name, colour space (matrix, transfer, bit depth, range). Drawn in the HUD at the screen plane; frame lines and safe areas follow pan / zoom, burn-ins stay in the picture's top corners (inside the panels while they are up). They stay up when the panels fade. |
| Scopes | `W`, `src/pro/Scopes`: waveform (luma) or RGB parade, vectorscope (BT.709 Cb/Cr, 75 % bar targets, skin-tone line), histogram (R, G, B, luma). Measured on the CPU from the left eye box-filtered to 480 px wide, **after the grade** (the shader's exposure -> CDL -> LUT, re-run on the CPU), at up to 8 Hz; a paused clip or a still re-measures when the grade changes. A panel at the bottom between the Station and Inspector, flat when pinned. |
| False colour / zebras | `6` false colour on the graded Rec.709 luma: < 2.5 % purple, < 10 % blue, 38-42 % (18 % grey) green, 52-56 % (skin) pink, 97-99 % yellow, > 99 % red, the rest grey. `7` zebras at or above a level (Inspector slider, 50-100 %), striped in source pixels so they sit at the object's depth in 3D. Both in the grade shader (`qcMode` 3, `zebraLevel`). |
| Pixel probe | `P`: the graded R G B (8-bit) and luma % under the pointer, read from the scopes' 480 px analysis image (a small-area average, not a single source pixel). |
| Pan / zoom | Mouse wheel zooms about the pointer, middle-drag (or left-drag while zoomed) pans, `H` fits, `Z` toggles 1:1. Identical for both eyes, so depth stays correct while zoomed. |
| 3D LUT | `.cube` (2..256, `DOMAIN_MIN/MAX`), from the inspector, a file drop, `--lut <file>` or `MEDIAPLAYER_LUT`. |
| ASC CDL | `.cc` / `.ccc` / `.cdl` (slope, offset, power, saturation; editable in the inspector), from the inspector, a drop, `--cdl <file>` or `MEDIAPLAYER_CDL`. |
| Exposure | ±4 stops in linear light. Right-click the slider to reset. |
| Grade bypass | `G` |
| Full screen | `F` / `F11` / double-click the picture (macOS binds `F11` to Show Desktop, so use `F` or the double-click there). Escape leaves full screen and never quits. Full screen unpins the panels so only the picture and the timecode show; leaving restores the pin. |
| Live inputs | `src/pro/Station`: named inputs, `J` or the inspector's **Inputs** switches. Each row shows a thumbnail of the feed (left eye, refreshed twice a second while live; the others keep their last frame and say how old it is - only one input captures at a time). `--input="Rig=decklink:0+1,sdi"`, `--input="Resolve=decklink:2,sbs"`, `--input="Witness=camera:Integrated"`, `--input="Test=test:1920x1080@24"`, `url:<u>`; shorthands `--decklink[=<dev>]` `--decklink-right=` `--decklink-input=sdi\|hdmi` `--decklink-stereo=auto\|mono\|sbs\|dual\|pair` `--decklink-mode=Hp25`, `--test-signal[=WxH@fps]`; `--decklink-list` prints the cards. |
| DeckLink setup dialog | *DeckLink setup...* in the Station panel: left / right device (right = "none" for one cable), connection (SDI / HDMI / as set in Desktop Video), stereo packing (auto / side by side / dual-stream 3D / mono), start format; *Open* adds or replaces the "DeckLink" input and switches to it. Says plainly when the build has no SDK or no card / driver is present. |
| DeckLink input | `src/pro/DeckLinkSource`: one cable (mono / SBS), the card's dual-stream 3D mode (switches automatically when the card detects it), or two inputs paired by SMPTE timecode (`TimecodePairer`; an HFRTC when the card has one, otherwise above 30 fps RP188 counts frame pairs and the field mark tells them apart, drop-frame counted the SMPTE way, `TimecodeFrames()`; the text is at the full rate). Without timecode, two inputs **on one card** pair by the card's hardware clock at its exact frame duration (`HwClockKey()`, genlocked inputs under half a frame apart pair exactly); two inputs on **separate cards** have two clocks and pair newest-to-newest with the offset reported, so jam-sync both. The Station panel shows the eye skew (ms, last and max), the unpaired-frame count and which pairing is in use. A dark input (no cable, or a dual-stream signal with no right eye) is reported per eye and the picture goes NO SIGNAL rather than fusing a stale eye. Unavailable reasons are specific: no SDK in the build, Desktop Video not installed, Desktop Video older than the SDK this build used (14.3+ reads frames through `IDeckLinkVideoBuffer`), no input found; the same device for both eyes is refused. A set but wrong `MP_DECKLINK_SDK_DIR` stops CMake. Format detection follows the signal (interlaced input is warned about: feed PsF or progressive). Captures 10-bit v210 and keeps 16 bits (P016) into the grade. Publishes into the same live path as the camera (`LiveProducer` / `App::EnterLive`). **Windows, macOS and Linux**: the same source file over the SDK's three COM skins (`dl` shim); macOS and Linux use the SDK's `Mac/include` / `Linux/include` headers and `DeckLinkAPIDispatch.cpp`, which loads the installed DeckLinkAPI framework / `libDeckLinkAPI.so` at run time. |
| Test rig | `src/pro/TestSignalSource`: synthetic stereo with known disparities and timecode, or a real side-by-side photo panned slowly (`--test-signal=image=<file>`); faults `MEDIAPLAYER_TEST_VSHIFT` (px), `_EYE_DELAY` (frames), `_GAIN_R`, `_DEPTH`, `_ROLL` (degrees, right eye rotated; + = its right side lower). Exercises everything below without hardware. |
| Timecode + delay | Source timecode and measured capture → screen delay (the card's or camera's arrival → the panel frame it is shown in; the camera's and panel's own delays come on top). In the inspector's Source section; `T` toggles. |
| Rig checks | `src/pro/RigCheck`: vertical error (sub-pixel 2D block matching, full-res px) + roll (in displayed px, so half-SBS is not doubled), eye sync, colour match (CIE76 ΔE between the eyes), depth range (2nd / 98th percentile parallax, % of eye width, negative = in front) against `--depth-budget=-2,1`; `--vertical-limit=1` `--colour-limit=2`. **Edge (window) violations**: a band 6 % of the eye width along the left and right frame edges is block-matched from the eye whose image of a foreground object is nearer that edge (right eye at the left edge, left eye at the right edge - the main grid matches left into right only, and at the left edge that search would have to leave the frame), on a half-block column step; the edge reads its 2nd most in-front match (one stray block is not a violation), alerting below `--edge-limit=-0.3` (% of eye width; `edge_pct` in `set_depth_budget`, `edge_left_pct` / `edge_right_pct` in `get_rig_status` and the snapshot note, null when the band has too little texture). Adds ~2-4 ms per measurement on the worker (640 px analysis), none on the render thread. Everything is measured on the eyes **as shown**: the `X` swap (and an R\|L container) is applied first, so near / far and the vertical sign stay right. Needs ≥ 20 matches after a median ± MAD outlier cut, else "Measuring...". Measured on a box-filtered copy of the frame at 640 px per eye (every source pixel counts, so sub-pixel motion survives the downscale). The vertical error is the offset at the picture centre with the roll taken out. When a late eye is measured, the geometry is re-measured on the time-aligned pair, so moving content seen through the delay does not shift it. Eye sync is **measured from the pixels** (a burst of 4 consecutive frames; each eye's moving blocks block-matched into the other eye now and 1..3 frames back, with the rig's vertical-offset line (offset + roll, fitted on the static blocks) warped out of the searched eye first, scored by how far their vertical offsets sit from zero; shown once it is the clear majority of the recent readings (≥ 3 of the last ≤ 7 within 5 s, and twice any other) and held between bursts that read nothing, so the panel, the snapshot note and `get_rig_status` all see it: sideways motion alone looks like depth, so it needs some vertical motion, else "not measured"; says which eye is late; `MEDIAPLAYER_RIG_DEBUG=1` prints the candidates' scores to stderr). **The source's timecode figure leads when there is one** (`N frames (TC)`; RP188 is per frame and exact) and the picture reading is its cross-check ("picture reads M"): at 48p a frame of motion is near the picture reading's resolution, so it can sit a frame off. Without timecode the picture reading leads (`(picture)`); `get_rig_status` returns both (`eye_sync_measured_frames`, `eye_sync_late_eye`, `eye_sync_frames`). Overruns ≥ 0.5 s become alerts with timecode, also appended to `Pictures/DisplayXR/station_alerts.jsonl` (with `end_timecode`); an open alert closes at the last good sample after 2 s with nothing measurable (a cut to black, a lost signal); an episode still open when the station quits is written with `"open": true` (a kill or a crash loses it). 60-second history strip (depth band vs budget, vertical error); `R` toggles. |
| Snapshots | `N` or the inspector: both eyes as shown (graded) via the runtime's atlas capture, plus `<name>_note.json` (note, timecode, rig numbers, budget); notes are editable in the inspector. |
| QC report | *Export QC report* (Station panel, next to *Snapshot*) or the `export_qc_report` agent tool: a new `Pictures/DisplayXR/qc_<time>/` folder with `report.html` (this session's rig alerts with start/end timecode, clock and duration; each snapshot with both eyes embedded as a thumbnail, its note and rig numbers, over-budget values flagged; the depth budget), `markers.edl` (DaVinci Resolve: *Timeline > Import > Timeline Markers from EDL*; alerts red with their duration, snapshots blue, drop-frame counted the SMPTE way) and `markers.csv` (every row with the rig numbers). The session = since the station started: closed alerts come from `station_alerts.jsonl` past the size it had at start, open ones from the monitor. Rows without a source timecode stay in the HTML / CSV and are left out of the EDL (the tool reports how many). `src/pro/QcReport.{h,cpp}`, `tests/qc_report_test.cpp`. |
| Depth overlay | `D`: the rig check's matches drawn on the picture, coloured by parallax (warm in front, white on the screen, blue behind), ringed red outside the budget; follows pan / zoom. |
| Review formats | MXF; EXR / DPX / TIFF **sequences** (open any frame; `--seq-fps=24`); EXR's scene-linear float encoded for display (sRGB transfer, BT.709 limited, **16-bit P016**, row-parallel; values above 1.0 clip - for HDR / wide-gamut EXR use an OpenColorIO view, below); 10/12-bit sources (ProRes, DNxHR HQX, HEVC 10, DPX, 16-bit TIFF) kept at 16 bits (P016) into the grade. Trunk code, switched on only by Pro (`VideoDecoder::SetProFormats` / `MediaSource::SetProFormats`). |
| YUV matrix + transfer | Each frame carries a colour tag (`FrameRing::Frame::matrix` / `transfer`). Matrix: BT.709, BT.601 or BT.2020 non-constant-luminance, from the stream's `colorspace` (unspecified: BT.601 at 576 lines and below, else BT.709); live inputs tag BT.601 for SD (`livepix` sets `Nv12/P016::matrix`, a source may overwrite it from the card's colorimetry). The MP_GRADE shader picks the matrix from the push block (`yuvMatrix`, 0 = BT.709, so BT.709 content is unchanged; Android's shader is untouched). Transfer (SDR / HLG / PQ) is a **tag only**: HLG and PQ are shown without tone mapping. 8/10-bit level maths unchanged. |
| A/B compare | `B` swaps to the other clip at the same position (`--compare=<file>` or `set_compare`). |
| OpenColorIO | Optional (`-DMP_OCIO_ROOT=<OCIO 2.x install>`, e.g. Open RV's `_build/RV_DEPS_OCIO/install`, plus `MP_OCIO_DLL_DIR` for its DLLs on Windows; on macOS the Homebrew keg, `$(brew --prefix opencolorio)`, which `build_macos.sh --ocio` installs and passes; the macOS CI build and `.pkg` include it): `--ocio=<config\|ocio://default> --ocio-src=<space>` and `--ocio-display= --ocio-view=` or `--ocio-dst=`, `--ocio-looks=`; baked to a 65³ LUT in the grade's LUT slot. |
| 10-bit panel picture | `--10bit` (or `MEDIAPLAYER_PRO_10BIT=1`) asks the runtime for an R10G10B10A2 atlas (`DXR_ATLAS_FORMAT=rgb10a2`, set in the player's environment before the runtime loads; an explicit `DXR_ATLAS_FORMAT` wins, `--8bit` forces the default). The grade already runs at float precision from 10-bit sources (P016); this keeps the last step, the atlas the display processor weaves, at 10 bits, which is intended to avoid banding in gradients after grades and LUTs (no gradient / banding test has been run yet; that is part of #142). Needs DisplayXR runtime **v2.53+** (the Vulkan weave-on-scanout split carries the deep atlas; the player warns at start-up when the runtime is older) and the Leia plug-in **v2.17+** (one sRGB encode, matching colours). Verified on Windows on a Leia panel (2026-10-29: R10G10B10A2 ring, no format mismatches, picture correct by eye). Do not use it with an older runtime or plug-in: below runtime v2.53 a deep atlas retires the Vulkan split, and with a Leia plug-in older than v2.17 that retire path can crash (fixed in displayxr-leia-plugin#438). Under the DisplayXR Shell or another workspace controller the service composes the picture, so `--10bit` on the player has no effect there. The atlas capture skips the pre-weave dump for a deep format and writes only `.woven.png`. Off by default until the hardware validation pass (#142). |
| Agent tools | `set_grade`, `get_grade`, `set_view`, `set_depth_overlay`, `get_rig_status`, `set_depth_budget`, `snapshot`, `list_inputs`, `select_input`, `open_decklink`, `set_ocio`, `describe_ocio`, `set_compare`, `get_resolve_link`, `set_resolve_follow` (next to the player's own). |
| Resolve panel | `tools/resolve/DisplayXR_Station_Panel.py`, a window inside Resolve (Workspace > Workflow Integrations in Resolve Studio - the Pro installer puts it there when Resolve is installed; or Workspace > Scripts). **Markers live**: snapshot notes become blue markers and rig alerts red markers on the current timeline as they happen, at their timecodes (SMPTE drop-frame counted properly), never twice. **Timecode follow**: Resolve's playhead and the clip under it reach the station; when the station can open the same media file it follows - plays while Resolve plays (correcting drift past 0.5 s), lands on the exact frame when Resolve parks (Resolve's API has no play state, so it is inferred from the playhead). The inspector shows "Resolve <timeline> TC" and a *Follow Resolve* box. **Station in Resolve**: input, timecode, rig numbers and the open alert; *Go to station TC* moves Resolve's playhead there; *Snapshot* takes a station snapshot with a note typed in Resolve; double-click a listed note / alert to jump to it. The two talk through files in the station folder (`station_status.json`, `resolve_link.json`, `resolve_commands.jsonl`; `src/pro/ResolveLink.h`), so no port and either side can start first. `--once` (or no UI) adds the markers once and exits. |

Grade order is exposure → CDL → LUT, the order an on-set chain applies a CDL under a
show LUT. The grade is skipped on the idle screen.

Test-only env vars (headless atlas checks): `MEDIAPLAYER_PRO_VIEW=0..4`,
`MEDIAPLAYER_PRO_ZOOM=<x>`, `MEDIAPLAYER_PRO_PIN`, `_DEPTH_OVERLAY`, `_EXPOSURE=<ev>`,
`_BYPASS`, `_COMPARE_FRAME=<n>`, `_SNAPSHOT_FRAME=<n>` + `_SNAPSHOT_NOTE`,
`_DECKLINK_DIALOG`, with the trunk's `MEDIAPLAYER_DUMP_ATLAS=<path>` and
`MEDIAPLAYER_DUMP_FRAME=<n>`. Do not use `MEDIAPLAYER_DUMP_HUD` on runtime v2.31: its
blocking readback deadlocks the app (the atlas capture already contains the HUD).

### Keys

| Key | Does |
|---|---|
| `Tab` / `Shift+Tab` | pin the panels / hide or show all UI |
| `1`..`6` | 3D, left eye, right eye, difference, anaglyph, false colour |
| `7`, `8`, `9` | zebras, wipe, channel |
| `W`, `P`, `Q` | scopes, pixel probe, cycle the frame line |
| wheel, middle-drag, `H`, `Z` | zoom, pan, fit, 1:1 |
| `G` | grade bypass |
| `D` | depth overlay |
| `T`, `R` | timecode, 60 s history strip |
| `N` | snapshot (note editable in the Station panel) |
| `J` | next input |
| `B` | A/B compare |
| `Space`, `Left` / `Right`, `Shift+Left` / `Shift+Right`, `Up` / `Down` | play/pause, one frame back/forward, 5-frame jump, previous/next asset (the player's own, editor convention; `[` / `]` also step) |
| `X`, `-` / `=`, `0` | swap eyes, convergence, reset convergence (the player's own) |
| `F` / `F11`, `Esc` | full screen, leave full screen |

## Build, run, release

```bat
:: same as the Media Player; Pro is built alongside it on Windows
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFFMPEG_ROOT=<ffmpeg dev tree>
cmake --build build
build\mediaplayer_pro_handle_vk_win.exe --camera="SpatialLabs Eyes" --lut show.cube
set VERSION=0.1.0 && installer\build-installer.bat pro
```

macOS (Apple Silicon; Homebrew `cmake ninja ffmpeg pkg-config vulkan-headers vulkan-loader
molten-vk`; the script builds the OpenXR loader):

```bash
./scripts/build_macos.sh --pro --ocio                 # build/: both apps, Pro with OpenColorIO
DISPLAYXR_VERSION=1.14.2 MP_PRO_VERSION=0.1.0 \
    ./scripts/build_macos.sh --pro --ocio --installer  # + _package/DisplayXRMediaPlayer-1.14.2.pkg
                                                      #   + _package/DisplayXRMediaPlayerPro-0.1.0.pkg
MP_BUILD_DIR=build-pro ./scripts/build_macos.sh --pro # another build dir
# the Pro .pkg alone, from an existing binary:
MP_PRODUCT=pro DISPLAYXR_VERSION=0.1.0 installer/macos/build_installer.sh build/mediaplayer_pro_handle_vk_macos
```

`--ocio` runs `brew install opencolorio` when it is missing and configures
`-DMP_OCIO_ROOT=$(brew --prefix opencolorio)`. The bundler copies OpenColorIO and its
dylibs (Imath, yaml-cpp, pystring, minizip-ng, zlib-ng) into
`Media Player Pro.app/Contents/Resources/lib` and relinks them to `@rpath`, like FFmpeg.
The Pro app (`installer/macos/create_app_bundle.sh` with `MP_PRODUCT=pro`) is
`Media Player Pro.app`, bundle id and package id `com.displayxr.mediaplayer.pro`
(`installer/macos/pro/Distribution.xml`), so it installs next to the Stereo Media Player
instead of over it. Its launcher opens no sample image (Pro starts on the idle screen).
Run a dev build with
`XR_RUNTIME_JSON=<runtime>/build/openxr_displayxr-dev.json build/mediaplayer_pro_handle_vk_macos`.

CI (`build-windows.yml`, `build-macos.yml`) builds and validates both apps and both
installers on every PR and runs the unit tests on both (Windows skips `decode_formats`,
whose fixtures need FFmpeg encoders that the slim decode-only FFmpeg lacks). A
`pro-vX.Y.Z` tag publishes only the Pro installers (the `.exe` from Windows, the `.pkg`
from macOS) on one release named "Media Player Pro X.Y.Z" that is never marked latest and
never dispatches the `versions.json` bump. Both jobs pass the same name / prerelease /
`make_latest: false` to `softprops/action-gh-release`, so whichever finishes first creates
the release and the other adds its asset. Android's version stamps match `v[0-9]*` only, so `pro-v*` tags
cannot disturb them. `/dxr-release` does not know about Pro; tag it by hand
(`git tag pro-v0.1.0 && git push origin pro-v0.1.0`) until it does.

The installer reuses the Media Player icons. Generate Pro's own with `/make-app-logos`
once the UI settles.

## Next (from the plan)

Ordered by what the customer needs first.

Items 1-6 below are built (see the table above) and unit-tested; **none of the DeckLink,
rig or Resolve parts has run with a card, a stereo rig or Resolve yet** - that needs the
hardware, and the Resolve panel has run against a fake Resolve (`tests/resolve_panel_test.py`)
but not yet inside Resolve (to confirm there: that `GetSourceStartTime` counts from the
file's start, not from the media's start timecode - if it does the latter, follow is off by
that start). RP188 at 50/60p is handled (an HFRTC when the card has one,
otherwise the frame pairs unfolded with the field mark, drop-frame counted the SMPTE way;
`TimecodeFrames`, tested) but not yet seen on a card. Kept for the record:

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

Convert to 3D (the trunk's Shift+L toggle, `XR_DXR_lift`) is inherited unchanged. The
station, timecode and rig checks keep reading the SOURCE frames, never the converted
pair.

## DeckLink: first checks on a card

None of the DeckLink code has run against a signal yet. The hardware-free half (v210 /
UYVY unpack, the timecode and clock keys, the pairer) is unit-tested; everything below
needs a card. Build with `-DMP_DECKLINK_SDK_DIR=<SDK>` on the box that has it, confirm the
log line `DeckLink input (Pro): ON (SDK x.y)`, then:

1. `--decklink-list` names the inputs; Desktop Video's version is at least the SDK's
   (`UnavailableReason()` says so otherwise). A pulled cable shows NO SIGNAL within 2 s and
   LIVE again on reconnect, without a restart.
2. Single input: 1080p and 2160p, 8- and 10-bit, limited-range colours right, at every
   rate 23.98 / 24 / 25 / 29.97 / 47.95 / 48 / 50 / 59.94 / 60; 2K and 4K DCI (2048 /
   4096 wide); 3G Level A and Level B; PsF shown progressive, interlaced warned about.
3. RP188 timecode reads right at 24p and 25p **and at 48p / 50p / 60p** (frame pairs +
   field mark: the TC chip must advance every frame, never two frames per step); LTC vs
   VITC1/2; drop-frame shows `;`.
4. Two inputs on one card (8K Pro G2 in a 2- or 4-sub-device profile; Duo 2 / Quad 2),
   genlocked: skew about 0 ms, unpaired not climbing, eye sync 0 frames. Unplug one eye:
   NO SIGNAL, never a frozen eye next to a live one; replug: LIVE.
5. Two inputs on two cards (two Mini Recorder 4K): with jam-synced timecode the pairing
   is frame-accurate; without timecode the panel says so and reports the offset.
6. Dual-stream 3D on one cable (a card that supports it; the Mini Recorder 4K does
   **not**, and the 8K Pro's support is per mode: `bmdSupportedVideoModeDualStream3D`):
   Auto switches to it, both eyes show, a signal that goes back to 2D leaves it.
7. 4K60 two-input: frame rate held and no drops (conversion runs on the SDK callback
   thread under one lock; if it drops, move the v210 unpack off that thread or onto the
   GPU).
8. RGB 4:4:4 and 12-bit, Rec.2020 / HLG / PQ from the VPID: shown with a warning (the
   grade is BT.709 unless the colour tag says otherwise); audio and ancillary data are
   ignored on purpose.
9. Resolve Studio's stereo output modes (side by side, dual-stream) into the same card;
   Desktop Video Setup / Media Express running at the same time (they claim the input).

Card facts that shape the plan: the **Mini Recorder 4K** is one input (SDI *or* HDMI, no
dual-stream 3D, 2160p30 max), so it covers the single-cable Resolve case only; two-cable
stereo needs the **8K Pro G2** with its profile set to expose two or four sub-devices
(`--decklink-list` shows one input in the 1-sub-device profile), or two cards with
timecode. On macOS the usual box is an UltraStudio over Thunderbolt; Desktop Video's
system extension must be approved once.

Known limits: 8-bit to the panel by default; `--10bit` keeps the panel atlas at 10 bits (runtime v2.53+, Leia plug-in v2.17+, see *10-bit panel picture* above). Panels: **pinned** panels (and
the history strip) are shown flat - each also goes out as an `XR_DXR_local_3d_zone` Local2D
layer over the same HUD image, so inside it the runtime shows crisp 2D instead of the weave
(the panel becomes an opaque card; the picture under it is not shown). Unpinned panels stay
in the woven HUD layer while they fade, so a half-faded card never cuts a hole in the
picture. `MEDIAPLAYER_PRO_PANELS_3D=1` keeps pinned panels woven too.
