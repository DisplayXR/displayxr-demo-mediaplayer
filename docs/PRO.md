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
| Installer | `DisplayXRMediaPlayerSetup-X.Y.Z.exe`, `DisplayXRMediaPlayer-X.Y.Z.pkg` | `DisplayXRMediaPlayerProSetup-X.Y.Z.exe`, `DisplayXRMediaPlayerPro-X.Y.Z.pkg`, `displayxr-mediaplayer-pro_X.Y.Z_amd64.deb` |
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
| Panels | Two ImGui panels, laid out as in the plan's mockup. **Station** (left): *Show panels* box, inputs + *DeckLink setup...*, source with timecode and delay, looks (LUT, CDL), snapshots. **Inspector** (right): view, stereo, colour (exposure, look, CDL values folded), rig check. Hidden by default. `Tab`, the transport's *Panels* button or *Show panels* shows or hides them together with the top bar and the transport; shown, they stay until hidden again (no timer, no click-away). |
| UI | Laid out for picture inspection, like Resolve, Nuke or RV: nothing sits on the picture unless asked for. With the panels hidden, moving the mouse (more than 8 px) shows only a slim transport strip on the bottom edge (play/pause, time and frame number, scrubber, mute, loop, *Panels*) and the pointer; both go after 2.5 s without motion (held while the pointer rests on the strip), or at once on a click on the picture or when the pointer leaves the window. A click on a strip control only does that control. Keys, the wheel and Ctrl+click never show or hide UI. `Shift+Tab` is the clean feed: nothing is drawn, not the strip, the panels, toasts or the pointer, until `Shift+Tab` (or `Tab`) again; the panels come back as they were. The pointer stays while the pixel probe is on. |
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
| Grade bypass, display calibration bypass (#133) | `G`, `Shift+G` |
| Full screen | `F` / `F11` / double-click the picture (macOS binds `F11` to Show Desktop, so use `F` or the double-click there). Escape leaves full screen and never quits. Full screen hides the panels so only the picture and the timecode show; leaving puts them back as they were. |
| Live inputs | `src/pro/Station`: named inputs, `J` or the inspector's **Inputs** switches. Each row shows a thumbnail of the feed (left eye, refreshed twice a second while live; the others keep their last frame and say how old it is - only one input captures at a time). `--input="Rig=decklink:0+1,sdi"`, `--input="Resolve=decklink:2,sbs"`, `--input="Witness=camera:Integrated"`, `--input="Test=test:1920x1080@24"`, `url:<u>`; shorthands `--decklink[=<dev>]` `--decklink-right=` `--decklink-input=sdi\|hdmi` `--decklink-stereo=auto\|mono\|sbs\|dual\|pair` `--decklink-mode=Hp25`, `--test-signal[=WxH@fps]`; `--decklink-list` prints the cards. |
| DeckLink setup dialog | *DeckLink setup...* in the Station panel: left / right device (right = "none" for one cable), connection (SDI / HDMI / as set in Desktop Video), stereo packing (auto / side by side / dual-stream 3D / mono), start format; *Open* adds or replaces the "DeckLink" input and switches to it. It opens showing the DeckLink input that is set up (the live one, else the "DeckLink" input, including one from `--decklink=` / `--decklink-right=`), with a *Live now* line naming what is capturing; *Refresh* re-lists the cards without resetting the choices. The spec grammar and the dialog mapping are `pro/DeckLinkSpec.cpp` (`tests/decklink_spec_test.cpp`). Says plainly when the build has no SDK or no card / driver is present. |
| DeckLink input | `src/pro/DeckLinkSource`: one cable (mono / SBS), the card's dual-stream 3D mode (switches automatically when the card detects it), or two inputs paired by SMPTE timecode (`TimecodePairer`; an HFRTC when the card has one, otherwise above 30 fps RP188 counts frame pairs and the field mark tells them apart, drop-frame counted the SMPTE way, `TimecodeFrames()`; the text is at the full rate). Without timecode, or when the two inputs' timecodes don't belong together, two inputs **on one card** pair by the card's hardware clock at its exact frame duration (`HwClockKey()`, genlocked inputs under half a frame apart pair exactly). That covers DaVinci Resolve's dual SDI stereo output ("Use dual outputs on SDI"): it puts its timecode on the first output only, and newest-to-newest showed the eyes a frame apart whenever one landed first. Timecode is trusted again after `kTrustTimecodeAfter` clock-paired frames whose timecodes agree; two inputs on **separate cards** have two clocks and pair newest-to-newest with the offset reported, so jam-sync both. The Station panel shows the eye skew (ms, last and max), the unpaired-frame count and which pairing is in use. A dark input (no cable, or a dual-stream signal with no right eye) is reported per eye and the picture goes NO SIGNAL rather than fusing a stale eye. Unavailable reasons are specific: no SDK in the build, Desktop Video not installed, Desktop Video older than the SDK this build used (14.3+ reads frames through `IDeckLinkVideoBuffer`), no input found; the same device for both eyes is refused. A set but wrong `MP_DECKLINK_SDK_DIR` stops CMake. Format detection follows the signal (interlaced input is warned about: feed PsF or progressive). Captures 10-bit v210 (YUV) or r210 (RGB 4:4:4) and keeps 16 bits (P016) into the grade; after a format change it asks for the signal's own depth and falls back format by format (r210 -> BGRA -> v210 -> 8-bit YUV, `src/pro/DeckLinkFormat.h`). Depth the fallback loses (12-bit RGB as r210, 10-bit as 8-bit) is a WARN line and an amber line on the Station panel (`decklink.format_warning` in `get_rig_status`). A format the card refuses in every pixel format leaves the input stopped and the source failed (status ERROR, a toast, `decklink.state` / `decklink.error`), rather than a live cable reading as NO SIGNAL; re-select the input to retry. A dual-stream 3D signal the card refuses in a new mode (already in dual-stream or switching into it) falls back to one eye. Publishes into the same live path as the camera (`LiveProducer` / `App::EnterLive`). **Conversion threads**: each input's lane thread converts its frames (v210 / r210 / UYVY / BGRA to P016) with a small pool of persistent workers (`src/pro/ConvertPool`). Each eye is split into row bands, on whole 4:2:0 row pairs, that run in parallel and write disjoint rows, so the picture is bit-identical to a one-pass convert (`tests/live_convert_test.cpp`). A dual-stream 3D input bands its left and right eyes together. On one thread per input, two 2160p59.94 10-bit inputs reached only ~40 fps (`late_frames` climbing). The default is per eye half the logical processors shared between the eyes, 1-8: on a 64-thread station, 8 per input for two inputs and 16 for one dual-stream (or Auto) input. `MEDIAPLAYER_DECKLINK_CONVERT_THREADS=<n>` sets the threads per input, its own thread included, shared by both eyes of a dual-stream input; `1` converts everything on the input's own thread, as before. The log says `decklink: converting on N thread(s) per input`. **Into the ring**: the converted frame is copied into the live `FrameRing` in row bands on the same pool (`ComposePairBanded` / `CopyFrameBanded`, bit-identical, `tests/live_convert_test.cpp`): a two-input pair is composed side by side there (~50 MB at 2160p, ~10 ms on one thread). The pair is composed **outside** the pairing lock: its two history slots are pinned (a lane never converts into a pinned slot) and a publish lock, taken while the pairing lock is still held, keeps the ring single-producer and the frames in order, so the other eye's lane converts and pairs meanwhile instead of waiting out the copy. **Direct pairing** (Pro 0.9.8, `src/pro/DirectPair`, on by default): that compose still read and wrote 47.5 MB per pair at 2x 2160p, on the memory bus the card's DMA, the converts and the upload copy share, and the 8K station published only ~45-53 pairs/s (fewer upload threads published more: the bus, not the cores). Now, when a lane knows before converting a frame which key the pairer will pair it by (`TimecodePairer::DirectKey`: the pairer is matching by key, timecode or the card's clock, and the frame is newer than the last pair shown), it converts the frame straight into its half of a ring frame reserved for that key (`FrameRing::ReserveBuffer`, at most 3 out); when the pairer pairs the two halves of that one entry, the frame is published as it is, with no compose. The pairer still decides every pair (and matched pairs only holds: an entry's two halves always carry the same key, and the card-clock guard checks every pair); any other decision (no key, newest of each, two eyes in different places) is composed as before, from wherever each eye's pixels are. A reserved frame is in no ring queue until both halves are written, so neither the renderer nor the upload thread's pin ever sees it half done, and it is taken only from the free list, so nothing they hold is ever written. The picture is byte-identical to the compose (`tests/direct_pair_test.cpp`: every raw format, odd sizes, tags, timecode, eye offset). With no ring frame to spare for a new key the frame goes the old way (`direct_no_frame`). `MEDIAPLAYER_DECKLINK_DIRECT_PAIR=0` composes every pair as before (also off with `MEDIAPLAYER_LIVE_QUEUE=0`); the log says which once per capture (`decklink: two-input pairs: ...`). Dual-stream 3D and one cable are unchanged. **Render thread**: the frame is then uploaded on the render thread, a 7680x2160 P016 frame for two 2160p eyes. That used to allocate, map, fill and free a fresh ~50 MB staging buffer every frame and held the panel to ~45-50 fps with the render thread at ~83 % of a core. The trunk renderer now keeps persistently mapped staging per plane per ring slot, reused every N frames (`VulkanRenderer::AcquireStaging`, safe because the ring waits that slot's fence before the frame records into it), and copies into it in row bands on a small pool (`src/rhi/BandCopy`, a quarter of the logical processors, 1-4; `MEDIAPLAYER_UPLOAD_THREADS=<n>` sets it, `1` is the old one-thread copy). That copy still cost the render thread ~8 ms a frame at 8 threads, which with the runtime's `xrEndFrame` (~7.7 ms) held the station at 53-55 fps (Pro 0.9.5). **Off the render thread** (`src/rhi/AsyncUpload`, trunk, on by default): an upload thread copies the frame into staging instead, in row bands on the same number of threads, and the render thread only records the buffer-to-image copy. For a live source (the ring's queue mode) the upload thread is woken by each publish and copies the next waiting frame **ahead**, while it waits in the queue, pinning it in the ring so the producer cannot reuse its buffer mid-copy (`FrameRing::PinNextQueued`); a frame that arrives just before the render thread takes it, and every frame of a latest-wins ring (file playback, the camera, streams), is copied **on demand** from the take while the render thread does the rest of its frame (the Pro hooks, `xrWaitFrame`). The commit after `xrBeginFrame` records the copy and waits only for a copy still running. The render thread stays the ring's one consumer, so the queue's one frame per refresh, its drops, trims and repeats are as before. The staging slots (frames in flight + 2, 5 by default, one frame's planes each) are refilled only after the GPU frame that read one has passed its fence, and recorded only when fully written. `MEDIAPLAYER_ASYNC_UPLOAD=0` puts the copy back on the render thread, exactly as before; the log says which once at start (`VulkanRenderer: upload copies OFF the render thread ...` / `... ON the render thread`). **Measured on site**: the renderer times itself all the time (`src/rhi/RenderPerf.h`): every ~10 s while a live source is up the log gets a `perf:` line (render fps, frame avg / max, the render thread's own time per frame next to its `xrWaitFrame` wait, the upload and staging-copy avg / max, the render loop's stages with avg / max ms per frame adding up to that own time, and the capture's delivered fps with the drops and late frames of the window), and `get_rig_status` (so `station_status.json`, under `rig`) carries the same as `render`. `MEDIAPLAYER_PRO_RIG` / `_THUMBS` / `_AUTOCONV` / `_UI` `=0` switch the optional per-frame work off for an A/B on site (first check 7). **Windows, macOS and Linux**: the same source file over the SDK's three COM skins (`dl` shim); macOS and Linux use the SDK's `Mac/include` / `Linux/include` headers and `DeckLinkAPIDispatch.cpp`, which loads the installed DeckLinkAPI framework / `libDeckLinkAPI.so` at run time. |
| Test rig | `src/pro/TestSignalSource`: synthetic stereo with known disparities and timecode, or a real side-by-side photo panned slowly (`--test-signal=image=<file>`); faults `MEDIAPLAYER_TEST_VSHIFT` (px), `_EYE_DELAY` (frames), `_GAIN_R`, `_DEPTH`, `_ROLL` (degrees, right eye rotated; + = its right side lower). Exercises everything below without hardware. |
| Timecode + delay | Source timecode and measured capture → screen delay (the card's or camera's arrival → the panel frame it is shown in; the camera's and panel's own delays come on top). In the inspector's Source section; `T` toggles. |
| Rig checks | `src/pro/RigCheck`: vertical error (sub-pixel 2D block matching, full-res px) + roll (in displayed px, so half-SBS is not doubled), eye sync, colour match (CIE76 ΔE between the eyes), depth range (2nd / 98th percentile parallax, % of eye width, negative = in front) against `--depth-budget=-2,1`; `--vertical-limit=1` `--colour-limit=2`. **Edge (window) violations**: a band 6 % of the eye width along the left and right frame edges is block-matched from the eye whose image of a foreground object is nearer that edge (right eye at the left edge, left eye at the right edge - the main grid matches left into right only, and at the left edge that search would have to leave the frame), on a half-block column step; the edge reads its 2nd most in-front match (one stray block is not a violation), alerting below `--edge-limit=-0.3` (% of eye width; `edge_pct` in `set_depth_budget`, `edge_left_pct` / `edge_right_pct` in `get_rig_status` and the snapshot note, null when the band has too little texture). Adds ~2-4 ms per measurement on the worker (640 px analysis), none on the render thread. Everything is measured on the eyes **as shown**: the `X` swap (and an R\|L container) is applied first, so near / far and the vertical sign stay right. Needs ≥ 20 matches after a median ± MAD outlier cut, else "Measuring...". Measured on a box-filtered copy of the frame at 640 px per eye (every source pixel counts, so sub-pixel motion survives the downscale). The vertical error is the offset at the picture centre with the roll taken out. When a late eye is measured, the geometry is re-measured on the time-aligned pair, so moving content seen through the delay does not shift it. Eye sync is **measured from the pixels** (a burst of 4 consecutive frames; while frames are dropping, e.g. UHD 59.94 on a busy station, the latest frame alone is measured every ~0.5 s instead, so vertical / colour / depth / edges keep updating and eye sync reads "not measured" until the frames are consecutive again; each eye's moving blocks block-matched into the other eye now and 1..3 frames back, with the rig's vertical-offset line (offset + roll, fitted on the static blocks) warped out of the searched eye first, scored by how far their vertical offsets sit from zero; shown once it is the clear majority of the recent readings (≥ 3 of the last ≤ 7 within 5 s, and twice any other) and held between bursts that read nothing, so the panel, the snapshot note and `get_rig_status` all see it: sideways motion alone looks like depth, so it needs some vertical motion, else "not measured"; says which eye is late; `MEDIAPLAYER_RIG_DEBUG=1` prints the candidates' scores to stderr). **The source's timecode figure leads when there is one** (`N frames (TC)`; RP188 is per frame and exact) and the picture reading is its cross-check ("picture reads M"): at 48p a frame of motion is near the picture reading's resolution, so it can sit a frame off. Without timecode the picture reading leads (`(picture)`); `get_rig_status` returns both (`eye_sync_measured_frames`, `eye_sync_late_eye`, `eye_sync_frames`). Overruns ≥ 0.5 s become alerts with timecode, also appended to `Pictures/DisplayXR/station_alerts.jsonl` (with `end_timecode`); an open alert closes at the last good sample after 2 s with nothing measurable (a cut to black, a lost signal); an episode still open when the station quits is written with `"open": true` (a kill or a crash loses it). 60-second history strip (depth band vs budget, vertical error); `R` toggles. |
| Snapshots | `N` or the inspector: both eyes as shown (graded) via the runtime's atlas capture, plus `<name>_note.json` (note, timecode, rig numbers, budget); notes are editable in the inspector. |
| QC report | *Export QC report* (Station panel, next to *Snapshot*) or the `export_qc_report` agent tool: a new `Pictures/DisplayXR/qc_<time>/` folder (`qc_<time>-2`, `-3`... for another export in the same second) with `report.html` (this session's rig alerts with start/end timecode, clock and duration; each snapshot with both eyes embedded as a thumbnail, its note and rig numbers, over-budget values flagged; the depth budget), `markers.edl` (DaVinci Resolve: *Timeline > Import > Timeline Markers from EDL*; alerts red with their duration, snapshots blue, drop-frame counted the SMPTE way) and `markers.csv` (every row with the rig numbers). The session = since the station started: closed alerts come from `station_alerts.jsonl` past the size it had at start, open ones from the monitor. Rows without a source timecode stay in the HTML / CSV and are left out of the EDL (the tool reports how many). The session is gathered on the render thread (~2 ms); the snapshot thumbnails (each decodes the full SBS capture) and the files are written on a worker (`src/pro/QcExporter`, every export runs, in order, and quitting finishes them), `report.html` last through a rename, so the report is complete once it exists; a snapshot taken under 5 s before the export is waited for (the runtime writes its capture a few frames later) so it keeps its thumbnail; the tool replies at once with the paths and `writing: true`. `src/pro/QcReport.{h,cpp}`, `tests/qc_report_test.cpp`, `tests/qc_exporter_test.cpp`. |
| Depth overlay | `D`: the rig check's matches drawn on the picture, coloured by parallax (warm in front, white on the screen, blue behind), ringed red outside the budget; follows pan / zoom. |
| Review formats | MXF; EXR / DPX / TIFF **sequences** (open any frame; `--seq-fps=24`); EXR's scene-linear float stored as **PQ of linear x 203 nits** (#134: 1.0 = HDR reference white, values up to ~49x survive; BT.709 limited, **16-bit P016**, row-parallel; tagged `ColorTransfer::LinearPq`, decoded and mapped by the shader below); 10/12-bit sources (ProRes, DNxHR HQX, HEVC 10, DPX, 16-bit TIFF) kept at 16 bits (P016) into the grade. Trunk code, switched on only by Pro (`VideoDecoder::SetProFormats` / `MediaSource::SetProFormats`). |
| YUV matrix + transfer | Each frame carries a colour tag (`FrameRing::Frame::matrix` / `transfer`). Matrix: BT.709, BT.601 or BT.2020 non-constant-luminance, from the stream's `colorspace` (unspecified: BT.601 at 576 lines and below, else BT.709); live inputs tag BT.601 for SD (`livepix` sets `Nv12/P016::matrix`, a source may overwrite it from the card's colorimetry). The MP_GRADE shader picks the matrix from the push block (`yuvMatrix`, 0 = BT.709, so BT.709 content is unchanged; Android's shader is untouched). Transfer (SDR / HLG / PQ / scene-linear) plus BT.2020 primaries and the stream's peak (MaxCLL, else the mastering display's) ride along. 8/10-bit level maths unchanged. |
| HDR on the SDR panel (#134) | `color/ToneMap.h` (CPU, the scopes and tests) = `HdrToDisplay()` in `sbs.frag`: signal -> linear relative to 203-nit reference white (BT.2408) -> exposure -> BT.2020 -> BT.709 -> BT.2390 EETF on max(R,G,B) from the source peak down to white -> sRGB encode; the CDL and LUT then run on that SDR picture. `tone_map` auto (roll off HLG / PQ, clip scene-linear EXR) / on / off (clip) / raw (unmapped, for a PQ-input show LUT); `peak_nits` 0 = metadata, else 1000 (for HLG: the roll-off's source peak only; the OOTF stays the 1000-nit one, system gamma 1.2, so the picture under the knee is the same at any peak, #169); `untagged` sdr / hlg / pq takes an untagged YUV source as BT.2100 (`GradeParams::forceTransfer`, BT.2020 matrix + primaries, #169). Under auto, scene-linear EXR goes into the CDL / LUT unclipped and clips after the grade (`HdrState::overWhite`, push `mode` bit 2 for A / 3 for B). HLG / PQ frames skip D3D11VA zero-copy (its convert drops the transfer); the scopes analyse a 16-bit picture on the frame's own matrix (`MakeThumbnail16`). An OCIO look on scene-linear EXR instead gets the linear light as PQ code, unclipped (`HdrParams::pqShaper`, bit 8), and its bake decodes it first (`ocio::Request::linearPqInput`; re-baked when the source changes between EXR and anything else), so the OCIO view sees values above 1.0. Packed into spare bits of the push block's `yuvMatrix` (`PackColourWord`; SDR packs to the matrix alone, so SDR frames are bit-identical). Inspector > Color, status bar, `set_grade` (`hdr` block), env `MEDIAPLAYER_PRO_TONEMAP` / `MEDIAPLAYER_PRO_PEAK_NITS` / `MEDIAPLAYER_PRO_UNTAGGED`. Not done: PQ **output** to an HDR panel (needs an app-visible runtime capability; the runtime's PQ atlas is env-only and the vendor Vulkan display processor declines it; `XR_DXR_swapchain_color_encoding` has no PQ value), EXR at 4:4:4. |
| A/B compare | `B` swaps to the other clip at the same position (`--compare=<file>` or `set_compare`). |
| OpenColorIO | Optional (`-DMP_OCIO_ROOT=<OCIO 2.x install>`, or the environment variable of the same name). The released Windows installer includes it, statically linked: `scripts/build-ocio-win.ps1 -Root <folder>` builds OCIO 2.5.1 with a pinned vcpkg and prints the folder to pass (a dynamic install, e.g. Open RV's `_build/RV_DEPS_OCIO/install`, also works, with `MP_OCIO_DLL_DIR` for its DLLs). The Linux `.deb` does not have it. On macOS the Homebrew keg, `$(brew --prefix opencolorio)`, which `build_macos.sh --ocio` installs and passes; the macOS CI build and `.pkg` include it): `--ocio=<config\|ocio://default> --ocio-src=<space>` and `--ocio-display= --ocio-view=` or `--ocio-dst=`, `--ocio-looks=`; baked to a 65³ LUT in the grade's LUT slot. In the app: Inspector > Color > **OpenColorIO (ACES)** picks the config (built-in `ocio://studio-config-latest` / `ocio://cg-config-latest`, or a `.ocio` file), input, display and view, and *Apply look* calls the same `LoadOcio` (defaults: ACEScct onto the Rec.1886 Rec.709 display; a launch-time look opens it on its settings). |
| 10-bit panel picture | `--10bit` (or `MEDIAPLAYER_PRO_10BIT=1`) asks the runtime for an R10G10B10A2 atlas (`DXR_ATLAS_FORMAT=rgb10a2`, set in the player's environment before the runtime loads; an explicit `DXR_ATLAS_FORMAT` wins, `--8bit` forces the default). The grade already runs at float precision from 10-bit sources (P016); this keeps the last step, the atlas the display processor weaves, at 10 bits, which is intended to avoid banding in gradients after grades and LUTs (no gradient / banding test has been run yet; that is part of #142). Needs DisplayXR runtime **v2.53+** (the Vulkan weave-on-scanout split carries the deep atlas; the player warns at start-up when the runtime is older) and the Leia plug-in **v2.17+** (one sRGB encode, matching colours). Verified on Windows on a Leia panel (2026-10-29: R10G10B10A2 ring, no format mismatches, picture correct by eye). Do not use it with an older runtime or plug-in: below runtime v2.53 a deep atlas retires the Vulkan split, and with a Leia plug-in older than v2.17 that retire path can crash (fixed in displayxr-leia-plugin#438). Under the DisplayXR Shell or another workspace controller the service composes the picture, so `--10bit` on the player has no effect there. The atlas capture skips the pre-weave dump for a deep format and writes only `.woven.png`. Off by default until the hardware validation pass (#142). |
| Agent tools | `set_grade`, `set_qc_view`, `set_depth_overlay`, `get_rig_status`, `set_depth_budget`, `snapshot`, `export_qc_report`, `list_inputs`, `select_input`, `open_decklink`, `set_ocio`, `describe_ocio`, `set_compare`, `set_resolve_follow`, `set_display_calibration`, `set_target_screen` (next to the player's own). The table is `pro/ProToolList.cpp`; `set_grade`, `set_qc_view` and `set_resolve_follow` with no arguments only read (they replaced `get_grade`, Pro's `set_view` and `get_resolve_link`, #220). **Budget:** the runtime takes at most 32 tools per process (`kAgentToolBudget`, `AgentTools.h`), and the player + Pro use all 32. Pro's register first, so an overrun cuts a player tool, never a station one, and the app logs it as an ERROR. `tests/agent_tools_test.cpp` fails on a duplicate name or an overrun: a new tool needs a merge or a bigger runtime budget. |
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
| `Tab` / `Shift+Tab` | show or hide the panels (full UI) / clean feed: no UI at all |
| `1`..`6` | 3D, left eye, right eye, difference, anaglyph, false colour |
| `7`, `8`, `9` | zebras, wipe, channel |
| `W`, `P`, `Q` | scopes, pixel probe, cycle the frame line |
| wheel, middle-drag, `H`, `Z` | zoom, pan, fit, 1:1 |
| `G`, `Shift+G` | grade bypass, display calibration bypass |
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

macOS (Apple Silicon; Homebrew `cmake ninja meson pkg-config sdl3 vulkan-headers vulkan-loader
molten-vk`; the script builds the OpenXR loader and the slim FFmpeg, `scripts/build_ffmpeg_macos.sh`):

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

**Linux (#139).** `scripts/package_deb_linux.sh --pro` packages Pro as
`displayxr-mediaplayer-pro_X.Y.Z_amd64.deb` (`/usr/lib/displayxr-demos/mediaplayer-pro`,
launcher `/usr/bin/displayxr-mediaplayer-pro`, DisplayXR Shell registration
`mediaplayer-pro`) from the same portable build as the Media Player's `.deb` (built on
Ubuntu 22.04, static SDL3, bundled slim FFmpeg; the package script builds Pro by default),
so one package installs on 22.04, 24.04 and 26.04 next to the Media Player. Versioned from
the `pro-v*` tags. DeckLink is built in from the vendored SDK headers (#164) and loads
`libDeckLinkAPI.so` from Blackmagic's Desktop Video at run time, which the station installs
separately; there is no OpenColorIO (stated in the package description), `.cube` LUTs and
CDLs work.
`build-linux.yml` builds Pro in every lane (`MP_BUILD_PRO=ON`) and its `DebInstall` legs
install Pro next to the Media Player in clean 22.04 / 24.04 / 26.04 containers.

CI (`build-windows.yml`, `build-macos.yml`, `build-linux.yml`) builds and validates both apps and both
installers on every PR and runs the unit tests on both (Windows skips `decode_formats`,
whose fixtures need FFmpeg encoders that the slim decode-only FFmpeg lacks). A
`pro-vX.Y.Z` tag publishes only the Pro installers (the `.exe` from Windows, the `.pkg`
from macOS, the `.deb` from Linux) on one release named "Media Player Pro X.Y.Z" that is never marked latest and
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
   has DeckLink input code to port. Build it behind `MP_DECKLINK_SDK_DIR` (the SDK's
   include folders are vendored in `third_party/decklink-sdk` and are the default). Needs a card: a DeckLink Mini Recorder 4K
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
needs a card. Any Pro build has it (the vendored `third_party/decklink-sdk` is the
default; `-DMP_DECKLINK_SDK_DIR=<SDK>` picks another, `=OFF` none); confirm the
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
7. 4K60 two-input: frame rate held and no drops, and 10-bit actually negotiated (the
   8-bit fallback is only an INFO line; `get_rig_status` `decklink.bit_depth` says).
   In `get_rig_status`, `decklink.delivered_fps` stays at the signal rate and
   `decklink.late_frames` stays flat. A late frame is one the convert did not finish in
   time. Each eye is converted in row bands over a per-input thread pool; the log line
   `decklink: converting on N thread(s) per input` gives the count. If frames still run late,
   raise `MEDIAPLAYER_DECKLINK_CONVERT_THREADS`; `1` is the old single-thread convert, for
   comparison. The same holds for dual-stream 3D at 2160p59.94 (`--decklink-stereo=dual`),
   where one input converts both eyes every frame.
   Two inputs: `decklink.published` should grow at the signal rate (~59.9/s at 59.94) with
   `decklink.unpaired` and `decklink.repeated` about flat. `decklink.pair_path` says how the
   pairs are built: `direct` (each eye converted straight into its half of the ring frame,
   the default) or `compose` (`MEDIAPLAYER_DECKLINK_DIRECT_PAIR=0`). `direct_pairs` +
   `composed_pairs` = `pairs`. With timecode or the card's clock matching, nearly all are
   `direct_pairs`; `composed_pairs` grows only while the pairer cannot match by key (start-up,
   a format change, newest of each on two unsynced cards, held timecode). `direct_no_frame`
   (frames with a key that found no spare ring frame; one lane more than ~2 frames ahead of
   the other) and `direct_skipped` (a decision whose eye already went to the screen in a whole
   pair, only at a rewind or held timecode) stay about flat. The 8K station before direct
   pairing (Resolve dual SDI 2160p59.94, async upload on, render ~60): ~45 pairs/s published
   at 8 upload threads, ~53 at 4, ~57 at 2, `repeated` +5-10/s and `unpaired` +~6.7/s - the
   memory bus, which the 47.5 MB compose per pair used too. Check with 4 upload threads
   (`MEDIAPLAYER_UPLOAD_THREADS=4`): published/s ~59.9, `repeated` and `unpaired` ~flat,
   `render.fps` ~60, `dropped` 0; then `MEDIAPLAYER_DECKLINK_DIRECT_PAIR=0` for the A/B.
   Then the other end: `decklink.dropped` (frames the renderer never took) stays flat too,
   which only happens when the render thread shows every frame (`decklink.repeated` still
   rises about once every 17 s: 59.94 into a 60 Hz panel leaves a refresh with no new
   frame). Read `render` in `get_rig_status` (or `rig.render` in `station_status.json`), or
   the `perf:` log line every ~10 s. At 2160p59.94 per eye on a 60 Hz panel, expect
   `render.fps` ~60, `frame_avg_ms` ~16.7 with `frame_max_ms` under ~25, `busy_avg_ms`
   (the render thread's own time per frame) ~10 with the rest in `xr_wait_avg_ms`,
   `upload_mb` ~47.5 (7680x2160 P016), `stages.upload` ~0.3 and `staging_allocs` flat after
   the first window (only a size change reallocates). The copy itself is the upload thread's
   now: `render.async_upload` (and the `async upload on: ...` part of the line) has `on`,
   `slots` (5), `copy_avg_ms` / `copy_max_ms` ~7-8 at 8 threads (`copy_threads`),
   `copies` and `copied_ahead` (most of them, for a live source), `taken` (staged frames
   recorded, ~60 a second), `skipped` (staged but never shown: a newer frame came first, or
   the source changed; flat while nothing drops), `no_free_slot` and `failed` (frames the
   render thread had to copy itself; 0), and `wait_avg_ms` / `wait_max_ms`, how long the
   render thread waited at the commit for a copy still running (~0). `copy_avg_ms` at the
   top level is the render thread's own copying, ~0 then. The station on 0.9.3 read
   `delivered_fps` 45-50, `dropped` +19/s and `late_frames` +10/s with the render thread at
   ~83 % of a core: the per-frame staging allocation and the one-thread copy; 0.9.5 read
   `upload` 8.2 + `xr_end` 7.7 of a busy 18.1-18.7 ms, 53-55 fps, with the copy still on
   the render thread. If `async_upload.copy_avg_ms` nears the frame period, raise
   `MEDIAPLAYER_UPLOAD_THREADS`; if `wait_avg_ms` grows, the copies are not keeping up.
   `MEDIAPLAYER_ASYNC_UPLOAD=0` puts the copy back on the render thread for an A/B
   (`stages.upload` ~8 again, `async_upload.on` false). A big `frame_open_avg_ms` means
   the GPU is behind instead. The log names the staging memory once (`VulkanRenderer:
   staging memory type N, propertyFlags ..., heap N, flags ...`): system RAM on a GPU
   without Resizable BAR.
   **Where the busy time goes**: `render.stages` in `get_rig_status`, and the `stages` part
   of the `perf:` line, time each stage of one render-loop iteration, with the average and
   the worst frame in ms. A stage that runs only now and then (the rig check a few times a
   second, the thumbnail twice) is averaged over every frame. So the averages add up to
   `busy_avg_ms` (`stages_sum_ms`, the `sum` on the line), and the max is the worst frame
   it ran in. At 59.94 a frame over 16.7 ms is a dropped frame. The stages, in loop order:
   - `pump`: window events, keys, xrPollEvent, PollLive.
   - `upload`: the ring acquire and the plane upload: with the copy off the render thread,
     the hand-over and the commit (recording the GPU copy, plus any wait for a copy still
     running); with `MEDIAPLAYER_ASYNC_UPLOAD=0`, the staging copy (`copy_avg_ms`) too.
   - `rig`: the rig check's Submit, with its Downsample.
   - `thumbs`: the live input's thumbnail.
   - `scopes`: `ProController::OnFrame`, the scopes / pixel-probe gather while they are on.
   - `autoconv`: auto-convergence sampling and its smoothed shift.
   - `pro_tick`: `ProController::Tick` (station, looks, Resolve, this log line).
   - `ui_build`: the ImGui new frame and building the UI.
   - `ui_render`: ImGui recorded into the HUD image.
   - `draw`: the view geometry and the recorded draw.
   - `submit`: the frame's one `vkQueueSubmit`.
   - `xr_begin`: xrBeginFrame, xrLocateViews and the swapchain image acquire / wait.
   - `hud_image`: the HUD swapchain image acquire and release.
   - `xr_end`: xrEndFrame, the runtime's in-process compositor and weaver.
   - `other`: the rest (UI fades, mode and depth-cursor upkeep, the alert log, the fps
     counter).

   `xr_wait_avg_ms` is xrWaitFrame alone now; the rest of BeginFrame is `xr_begin` (on
   0.9.4 the two read 0.18 ms together).
   **Station A/B**: one signed build can turn the optional per-frame work off, so a stage
   can be compared on and off without a new build. Set the variable before launch. It is
   read once, and logged once (`Pro A/B: MEDIAPLAYER_PRO_RIG=0: ... OFF`). The `perf:` line
   names the ones that are off after the stages (`| off: rig, ...`), and
   `render.switched_off` lists them. `0` / `off` / `false` / `no` turn a switch off; unset
   or anything else leaves it on. The upload and the picture's draw are never switched off.
   - `MEDIAPLAYER_PRO_RIG=0`: no rig check (no Submit / Downsample on the render thread).
     The rig checks read "Measuring..." and raise no alerts.
   - `MEDIAPLAYER_PRO_THUMBS=0`: no thumbnail for the live input in the input list.
   - `MEDIAPLAYER_PRO_AUTOCONV=0`: no auto-convergence sampling. Auto-convergence is off
     unless `A`, `--auto-conv` or `MEDIAPLAYER_AUTO_CONV` turned it on; with it on, the
     shift holds its last value.
   - `MEDIAPLAYER_PRO_UI=0`: no UI (no HUD layer, no ImGui) while a live source is up;
     the keys still work. The clean feed (`Shift+Tab`) is the in-app equivalent, but toasts
     and the on-picture overlays still draw there.
   - Scopes and the pixel probe need no switch: they are off at start and `W` / `P` toggle
     them live (`MEDIAPLAYER_PRO_SCOPES=0` keeps the scopes off at start).
8. RGB 4:4:4 at 8 / 10 / 12 bit: 10-bit arrives as r210 (`decklink.bit_depth` 10, colours
   right, the r210 byte order is unit-tested against the SDK's description only); 12-bit is
   captured as r210 too (no 12-bit converter yet) and the panel says 2 bits are lost; a card
   that refuses r210 falls back to BGRA with the same warning. Rec.2020 / HLG / PQ
   from the VPID are tone-mapped to the SDR panel. Audio and ancillary data are ignored on
   purpose.
9. Resolve Studio's stereo output modes (side by side, dual-stream) into the same card;
   Desktop Video Setup / Media Express running at the same time (they claim the input).

Also on first contact with a card: a format change during dual-stream 3D, including a mode
the card refuses; stop, reopen and input switch while streaming; pulling the card or its
Thunderbolt cable; Desktop Video older than the SDK with a card present (the card is listed,
and opening it is what reports the driver as too old).

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
