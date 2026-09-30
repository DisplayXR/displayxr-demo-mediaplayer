# DisplayXR Stereo Media Player

A lightweight, cross-platform **stereo media player** for the
[DisplayXR runtime](https://github.com/DisplayXR/displayxr-runtime). Plays
already-stereo **images** (side-by-side JPG/PNG) and **video** (side-by-side
H.264/H.265/AV1) on a 3D display, with a subtle 3D UI.

It's an OpenXR **client app**: the DisplayXR runtime + display processor do the
weaving. The player just decodes a stereo pair and submits left/right views — no
vendor SR SDK, no weaving, no CUDA dependency.

> The minimal, un-bloated counterpart to Leia's `LeiaPlayerWin` — built on
> OpenXR + FFmpeg + SDL3 + Dear ImGui + stb. One Vulkan codebase for Windows,
> Linux, and macOS (Android planned).

## Status

✅ **M0–M6 complete — v1.0.0.** The full milestone arc is in: SDL3 window → OpenXR stereo
session → runtime weave (M0); SBS L/R routing (M1); FFmpeg video decode through a
triple-buffered `FrameRing` with aspect-correct letterboxing (M2); per-OS hardware decode
(VideoToolbox / D3D11VA+NVDEC / VAAPI) with software fallback (M3); a subtle-3D Dear ImGui
transport bar with per-eye parallax, convergence/L-R-swap controls, fullscreen, open-file,
and low-latency frame-exact scrubbing (M4); the macOS Vulkan path (M5); and Windows + macOS
installers with CI release-on-tag (M6).

Stereo images (JPG/PNG via stb_image, LIF and MPO containers) and SBS video share the same
decode → atlas → submit path; the runtime + display processor do all weaving.

**Stereo layout resolution**, in priority order:

1. **Filename** — the `*_2x1` / `*_half_2x1` convention.
2. **Container metadata** — a LIF or MPO container, or `AVStereo3D` side data from MKV
   `StereoMode`, MP4 `st3d`, or an H.264 frame-packing SEI (which also says whether the
   eyes are packed R|L).
3. **Content** — cross-correlation of the two halves, which may conclude **mono only on
   unambiguous evidence**.
4. **Otherwise the file is assumed to be stereo**, with the frame aspect settling
   full-SBS vs half-SBS.

Step 4 is a policy, not a measurement: this is a player for a 3D display, so mono is not a
use case, and *when in doubt, stereo*. That is what finally gets the common case right — a
half-SBS frame is dimensionally identical to a mono one (1920×1080 either way), so no
aspect threshold can tell them apart, and the old `>= 1.9 → full SBS, else mono` rule
therefore showed real half-SBS files flat.

Step 3 exists to rescue genuinely 2D files, and its thresholds are deliberately asymmetric:
mono has to prove itself, stereo is the fallback. They were calibrated on 2,380 real
7680×2160 stereo photographs plus the two variants each yields for free — its left eye
alone (real mono) and its halves squeezed (real half-SBS), 7,140 analyses in all — and
measured at **zero** real stereo images misclassified as 2D, with 2,367/2,380 mono
correctly identified. Correlation runs on a Sobel high-pass so the
match is illumination-invariant, and a mono verdict is vetoed by a systematic discontinuity
at the frame midpoint, which a real SBS frame has and a mono one does not.

The HUD names whichever signal decided, and **L** pins the layout by hand.
`MEDIAPLAYER_STEREO_DETECT=meta` skips step 3 if you want the pure filename/metadata/assume
ladder.

**Verified on macOS (Apple Silicon, MoltenVK)** against a local `displayxr-runtime` dev
build — correct per-eye L/R routing in 2-view (Squeezed SBS) and 4-view (Quad) modes, HUD
compositing, 114 FPS on M1 Pro. See [`docs/M5-NOTES.md`](docs/M5-NOTES.md). Windows builds
in CI; the macOS Vulkan path uses `VK_KHR_portability_enumeration` + `VK_KHR_portability_subset`
over an `XR_DXR_cocoa_window_binding` NSView.

Also in: live (continuous) window resize, an FPS counter in the title bar, and a
window-space stats **HUD** (toggle with **SHIFT+TAB**) showing fps / mode / source /
canvas / per-view tile.

```bash
# play a side-by-side stereo video or image
scripts/run_mediaplayer_handle_vk_macos.sh /path/to/clip_2x1.mp4
scripts/run_mediaplayer_handle_vk_macos.sh assets/test_LR_2x1.png
```

Keys: **V** cycles display modes, **SHIFT+TAB** toggles the HUD, **L** cycles the stereo
layout override (auto / mono / SBS-full / SBS-half), **Esc** quits. With no file argument it
falls back to a RED|BLUE left/right test pattern. See `PRD.md` §11 for the milestone map.

You can also **drag files or a folder onto the window**. Dropping several files at once
loads the first and makes the dropped set the list the arrow keys and the slideshow walk.
Dropping a **folder** opens its first asset and starts the slideshow, with the folder as
that list — the same thing as passing the folder on the command line, plus playback.

## Live camera (#90)

A live **stereo camera** can stand in for a file: a side-by-side UVC webcam such as the
Acer **SpatialLabs Eyes**, or an HDMI/SDI rig behind a UVC capture box. Pick it from the
**Camera** menu next to **Open**, press **C**, or launch with `--camera`. The frame is
shown raw, with the left half going to the left eye. The layout is guessed from the frame
aspect (16:9 means SBS-half, so the Eyes' 3840x2160 shows as two 16:9 eyes), and **L**
cycles it as usual. **X** swaps the eyes, **Space** (or a click on the LIVE pill) freezes
the picture, and opening or dropping a file replaces the camera. While live, the top bar
shows the device, the camera and panel frame rates, and the frame age.

```bash
mediaplayer_handle_vk_win.exe --camera-list        # devices + formats; no runtime needed
mediaplayer_handle_vk_win.exe --camera             # default pick (SpatialLabs Eyes first)
mediaplayer_handle_vk_win.exe --camera=webcam      # index or name substring
mediaplayer_handle_vk_win.exe --camera --camera-fps=30
```

| flag / env | meaning |
|---|---|
| `--camera[=<sel>]`, `MEDIAPLAYER_CAMERA=<sel>` | start live. `<sel>` is empty or `auto` for the default pick, a number for an index from `--camera-list`, or a case-insensitive name substring. The CLI wins over the env var. |
| `--camera-fps=<N>`, `MEDIAPLAYER_CAMERA_FPS` | requested rate. The default is the highest rate at or below 60. |
| `MEDIAPLAYER_CAMERA_DENY="a;b"` | extra name substrings that are never opened |

A 3D panel's own **eye-tracking camera** (e.g. "SpatialLabs Tracking Camera") is never
opened: opening it would stop eye tracking. The same applies to IR and depth cameras.
Such devices show as `[blocked]` in `--camera-list` and are absent from the picker.
Capture uses SDL3's camera API and requests NV12. On Windows the Camera Frame Server
decodes the Eyes' MJPG, giving 60 fps at 3840x2160. A device that offers **only** MJPG is
not supported yet (follow-up). The same code builds on macOS; Linux compiles it with SDL's
camera backend off, so there no camera is found. SDL's Windows driver has no hot-plug,
so after an unplug use **Rescan** in the Camera menu (or press `C`): it re-lists the
devices and re-opens the same camera as soon as it is back.

## Auto-convergence (live, #92)

**Off by default**: the live picture is shown raw unless you turn this on. When on, the
player measures how far apart the subject sits in the two eyes and shifts the eyes in
opposite directions so the subject lands at the display plane. It measures the pixels
(NCC block matching on a downsampled luma plane, ~5 Hz, on a side thread). There is no
face detection and no calibration. Manual `-` / `=` convergence still applies on top.

| control | effect |
|---|---|
| `A` / the **Auto-conv** button in the live top bar | on / off (turning it off glides the shift back to 0) |
| `Shift+A` | subject policy: nearest -> sharp -> centre |
| `Ctrl+click` on the picture | pin the subject under the cursor (policy `focus`, template-tracked) |
| `--auto-conv[=nearest\|sharp\|centre]`, `MEDIAPLAYER_AUTO_CONV=<policy>` | start with it on. The CLI wins over the env var. `1`/`on` = the default policy, `0`/`off` = off. |
| `MEDIAPLAYER_AUTO_CONV_CLAMP=1`, `MEDIAPLAYER_AUTO_CONV_FRONT` / `_REAR` (percent) | opt-in comfort clamp + its budgets. Off by default: with a deep scene the clamp parks the nearest content at the plane and pushes the subject far behind it; the default keeps the subject at the plane. |
| MCP `set_auto_convergence {enabled, policy, focus_x, focus_y}` | same, for agents; `get_status` reports the `auto_conv_*` fields |

Subject policies:
- **nearest** (default): the nearest strong depth plane. This is right for a person in
  front of the camera, and it is the 3D call's rule.
- **sharp**: the sharpest quarter of the matched blocks. With a shallow depth of field
  the sharp region is the subject. If sharpness is flat across the frame it falls back to
  centre.
- **centre**: blocks weighted towards the middle of the frame.
- **focus**: a point you pinned with Ctrl+click.

On top of that measurement:
- A **comfort clamp** limits the nearest content (95th percentile of the block
  disparities) to 0.5 % of eye width in front of the plane, and the farthest content
  (5th percentile) to 1.5 % behind it. When both limits cannot hold, the front limit wins.
- A median-of-3 hold rides through dropouts such as a hand over the lens.
- A low-pass and a 0.2 %-of-eye-width-per-second rate limit smooth the shift. A scene cut
  is the exception: the shift snaps to the new scene.

The HUD shows `autoconv ON nearest  d=…  conv=…  ncc …  … ms`.

The measurement is a port of displayxr-web's `js/call/disparity.js` (the 3D video call's
auto-convergence), and it keeps that file's thresholds so web and native converge the
same way. See `src/media/AutoConvergence.h`.

## Streaming URL (live) (#93)

A **network stream** can stand in for a file or the camera, for example a studio's stereo
camera put online by an encoder. Open it from **Live... > Stream URL...** in the top bar
(or **Ctrl+U**), type or paste the URL and press **Enter**. The field remembers the last
URL. You can also launch with `--url <u>`, pass the URL as the media path, or set
`MEDIAPLAYER_URL`. The stream is played **live**, like the camera: there is no seek, loop
or duration, and every frame is shown as soon as it is decoded, with late ones dropped
rather than queued. The layout is guessed from the frame aspect (16:9 means SBS-half);
**L** cycles it and **X** swaps the eyes. **Space** (or a click on the pill) freezes the
picture while the stream keeps decoding. Opening a file or the camera replaces the stream,
and **Stop live** in the same menu ends it. Audio in a stream is ignored for now.

| scheme | notes |
|---|---|
| `rtsp://` / `rtsps://` | RTSP is forced over **TCP** (no UDP loss or NAT trouble). Put credentials in the URL: `rtsp://user:pass@cam/stream1`. |
| `rtmp://` / `rtmps://` | RTMP pull, for example from a local nginx-rtmp or a media server. |
| `udp://` / `rtp://` | MPEG-TS over UDP (unicast or multicast, `udp://@239.0.0.1:1234`). |
| `http://` / `https://` | progressive MPEG-TS / FLV / fragmented MP4. |
| `*.m3u8` | HLS, local or remote. Its latency is the playlist's (seconds), not the player's. |
| `srt://` | **not supported** (needs libsrt); refused with a message. |

**Latency.** Demuxing runs with `fflags=nobuffer`, a 0.5 s `max_delay`, a small probe
(1 MB / 1.5 s, enough for a 4K SPS/PPS + IDR) and a low-delay decoder. The player adds
about one decoded frame of latency. Most of what remains is the sender's encoder and GOP:
use a zerolatency tune and a short keyframe interval (`-g 30`). The first picture shows at
the next keyframe.

**Reconnect.** A stream that stops (the sender is killed or the network drops) is detected
after 5 s without data (15 s for HLS). The pill turns amber (RECONNECTING) and the player
retries with a 1, 2, 4, 8 s backoff (capped) until the stream is back. The last picture
stays up in the meantime. A URL that never connects gives up after three attempts
(connect budget 8 s each) and returns to the idle screen with the error.

While streaming, the top bar shows `STREAM host · fps · Mbit/s`: a blue dot while streaming,
amber while connecting, reconnecting or frozen, grey once the stream has failed. The
SHIFT+TAB text HUD has the same line. Agents drive it with the MCP tool
`open_url {url}`. `get_status` reports `stream`, `stream_state`, `stream_url`
(password redacted), `stream_fps`, `stream_kbps` and `reconnects`, and `close_camera`
stops a stream too.

**FFmpeg must have network input.** The installer's slim FFmpeg (`scripts/build-ffmpeg-slim.sh`,
`scripts/build_ffmpeg_linux.sh`) enables it. An older slim tree used as a local
`-DFFMPEG_ROOT` does not, and every URL then fails with "Protocol not found". For local
work, point `FFMPEG_ROOT` at any full shared FFmpeg 8.x dev build instead; the gitignored
`third_party/ffmpeg-win64-full/` is the conventional place for it.

**Linux: no TLS.** The `.deb` bundles a private FFmpeg whose every library must install on
Ubuntu 22.04, 24.04 and 26.04. Each Linux TLS backend adds a library whose package name
differs across those releases (`libssl3` / `libssl3t64`), so the Linux build has no TLS:
`https://`, `rtsps://`, `rtmps://` and HLS over https are **Windows and macOS only**. Plain
`http`, `rtsp`, `rtmp`, `udp` and `rtp` work everywhere. Windows uses the system SChannel
(no extra DLL). macOS uses Homebrew's full FFmpeg.

Test it with no infrastructure by serving a camera from a second `ffmpeg` on the same box:

```bash
# sender: the Eyes over UDP MPEG-TS (scale keeps a laptop's x264 real-time; check speed>=1x)
ffmpeg -f dshow -rtbufsize 200M -video_size 3840x2160 -framerate 30 -i video="SpatialLabs Eyes" \
       -vf scale=1920:-2 -c:v libx264 -preset ultrafast -tune zerolatency -g 30 -pix_fmt yuv420p \
       -f mpegts "udp://127.0.0.1:5004?pkt_size=1316"
mediaplayer_handle_vk_win.exe --url udp://127.0.0.1:5004

# or over HTTP (the sender serves one client, then exits)
ffmpeg ...same input/encode... -listen 1 -f mpegts http://127.0.0.1:8080
mediaplayer_handle_vk_win.exe --url http://127.0.0.1:8080
```

Kill the sender to watch it reconnect, then start it again and the picture comes back.

| flag / env / key | meaning |
|---|---|
| `--url <u>`, `--url=<u>`, a URL as the path | start on the stream (`--camera` wins if both are given) |
| `MEDIAPLAYER_URL=<u>` | opens at launch when the command line names nothing else, and prefills the Stream URL field |
| **Ctrl+U** | the Stream URL field |

## Requirements

- A working DisplayXR runtime install (or dev build) — this app cannot run without it.
- Vulkan (SDK, or Homebrew `vulkan-loader` + `molten-vk` on macOS), an OpenXR loader,
  CMake ≥ 3.24, a C++17 compiler. SDL3 is fetched and built automatically.
- FFmpeg / Dear ImGui / stb arrive in M1–M4 (not needed for M0).

## Build & run (macOS — verified)

```bash
# Configure + build. The OpenXR loader is auto-found via find_package; if you built
# the Khronos OpenXR-SDK to a custom prefix, hint it with -DCMAKE_PREFIX_PATH.
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/tmp/openxr-install
cmake --build build

# Run against a local DisplayXR dev runtime (helper sets XR_RUNTIME_JSON to the
# sibling checkout's dev manifest; override XR_RUNTIME_JSON to point elsewhere).
scripts/run_mediaplayer_handle_vk_macos.sh
# equivalently:
XR_RUNTIME_JSON=/path/to/displayxr-runtime/build/openxr_displayxr-dev.json \
    ./build/mediaplayer_handle_vk_macos
```

Press **Esc** or close the window to exit. The two-plus eye views clear to distinct
colors and weave on a 3D display (a single color shows on a 2D-fallback mode).
Set `MEDIAPLAYER_LOG_DEBUG=1` for verbose logs.

> If `find_package(OpenXR)` finds nothing, the build fetches and compiles the
> pinned Khronos OpenXR-SDK loader automatically.

## Supported formats (v1 target)

| Type | Formats | Notes |
|---|---|---|
| Images | SBS JPG/PNG, LIF, MPO | layout from filename / container / content, else assumed stereo; HEIF behind `MEDIAPLAYER_WITH_HEIF` (off by default) |
| Video | SBS H.264 / H.265 / AV1 (MP4/MKV/MOV) | hardware decode via FFmpeg, CPU fallback; `AVStereo3D` side data honoured incl. eye order |

## Runtime compatibility

<!-- Covenant: which displayxr-runtime versions this demo is verified against.
     Updated on each release per docs/roadmap/demo-distribution.md. -->
| Media player | Verified against runtime |
|---|---|
| v1.0.0 | v1.10.2 |

v1.0.0 is verified against the DisplayXR runtime **v1.10.2** release line (the
current `versions.json[runtime]` pin). Coupling is **extension-wire-protocol only** —
the demo needs the runtime's window-binding + stereo-session extensions
(`XR_DXR_win32_window_binding` / `XR_DXR_cocoa_window_binding`, `XR_DXR_display_info`),
so any runtime exposing that protocol should work, but v1.10.2 is the combination this
release was validated against.

## License

Apache-2.0 — see `LICENSE`. Bundled media samples carry their own licenses.
