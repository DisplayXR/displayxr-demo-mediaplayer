# Capture levers: measuring the 4K x 2 DeckLink path

Two 2160p59.94 DeckLink inputs reach about 50-54 pairs/s on the 8K station, below the
signal (#238, #184). Rendering is no longer the limit: since runtime v2.66.17 (deferred compose
wait) and Pro 0.9.6 (copy on the upload thread), the render loop holds about 59.5 fps. The
capture side is the limit. Every frame crosses system memory several times, and the station
showed more upload threads giving fewer pairs, so the copies compete for memory.

The fixes are **levers**. Each one is an environment switch whose default is today's
behaviour. The player reports which levers are in effect and how long each capture stage
takes, and `scripts/capture_matrix.ps1` runs the combinations on the station and tabulates
them. That way the card can be tested whenever someone is at the station, without a build per
experiment.

## Where a frame goes, and which lever removes which copy

Per eye, at 2160p v210 (22.1 MB) / P016 (24.9 MB). In the table, R+W means the CPU reads and
writes the frame.

| # | step | today (0.9.8) | lever |
|---|---|---|---|
| 1 | the card DMAs the frame into memory | into the driver's buffers | `ALLOCATOR`: into page-aligned buffers this app owns, optionally on one NUMA node (`ALLOC_NODE`) |
| 2 | the card callback copies it out (R+W 22 MB) | always | `HOLD` (needs `ALLOCATOR`): the frame is kept, nothing is copied, and the lane reads the card's buffer |
| 3 | the lane converts v210 to P016 (R 22 + W 25 MB) | in row bands, into its half of the ring frame (0.9.8 direct pair) | `RAW_V210` (needs `GPU_V210`): no convert. The rows are moved into the ring by swapping vectors, and the GPU decodes them (#240) |
| 4 | compose the side-by-side pair (R+W 50 MB) | removed by 0.9.8's direct pair | (`DIRECT_PAIR=0` brings it back, for comparison) |
| 5 | the upload copy into staging (R+W 47-50 MB) | on the upload thread (0.9.6) | `UPLOAD_THREADS`, `UPLOAD_PIN` |
| 6 | the GPU reads staging over PCIe | | |

With `RAW_V210` + `HOLD`, the CPU still copies each frame twice (the lane's copy out of the
held buffer, and the upload copy). Removing both, so that the GPU reads the card's buffer
directly, is #238 phase 3. It is **not** in this change; see *Next* below.

Where the threads run is a lever too. `CAPTURE_PIN` puts lane N and its convert threads on
last-level-cache group N; `UPLOAD_PIN` does the same for the upload thread. On a Threadripper
PRO each L3 group is one CCX, so pinning decides which cache and which fabric link a copy
uses.

## The levers

All are read when a capture opens, except the runtime's (read at the first OpenXR call). The
full table, with defaults, is `src/pro/CaptureLevers.h`.

| variable | default | what |
|---|---|---|
| `MEDIAPLAYER_GPU_V210` | 0 | GPU v210 decode pass (#240). Required by `RAW_V210` |
| `MEDIAPLAYER_DECKLINK_RAW_V210` | 0 | DeckLink lanes publish v210 rows as they are: no CPU convert, no compose. The direct pair is then off, since there is nothing to convert into a half |
| `MEDIAPLAYER_DECKLINK_ALLOCATOR` | 0 | `IDeckLinkVideoBufferAllocatorProvider`: the card writes into pooled, page-aligned memory (up to 48 buffers per size). If the card refuses it, the capture falls back to the card's own buffers and turns `HOLD` off |
| `MEDIAPLAYER_DECKLINK_ALLOC_NODE` | -1 | NUMA node for those buffers (`VirtualAllocExNuma`) |
| `MEDIAPLAYER_DECKLINK_HOLD` | 0 | the callback keeps the frame (AddRef) instead of copying it out. Only with `ALLOCATOR`: holding the driver's few buffers would starve the card |
| `MEDIAPLAYER_CAPTURE_PIN` | off | `auto` = lane 0 on group 0, lane 1 on group 1; or a list such as `0,2` |
| `MEDIAPLAYER_UPLOAD_PIN` | off | `auto` = group 2; or a number |
| `MEDIAPLAYER_DECKLINK_CONVERT_THREADS` | auto | threads per input lane (convert, or the held-buffer copy) |
| `MEDIAPLAYER_UPLOAD_THREADS` | auto | upload copy threads (the station uses 4) |
| `MEDIAPLAYER_DECKLINK_DIRECT_PAIR` | 1 | 0.9.8's direct pair; 0 = compose as in 0.9.7 |
| `MEDIAPLAYER_DECKLINK_STRICT_PAIRS` | 1 | matched pairs only (0.9.7) |
| `MEDIAPLAYER_LIVE_QUEUE` | 3 | live ring queue depth |
| `MEDIAPLAYER_ASYNC_UPLOAD` | 1 | the upload copy off the render thread (0.9.6) |
| `MEDIAPLAYER_PRO_RIG` / `_THUMBS` / `_AUTOCONV` / `_UI` | 1 | render-thread extras (`pro/PerfSwitches.h`) |
| `DXR_VK_HUB_DEFER_COMPOSE_WAIT` | 0 | runtime v2.66.17+: no CPU wait on the compose fence (the station runs with 1) |

Two things to know about `RAW_V210`:
- The rig check, thumbnails, scopes and auto-convergence do not read v210 frames yet (#240
  phase 1 rule), so they hold their last value. Phase 2's CPU consumers (sparse v210 reads)
  are the follow-up before raw can become the default.
- A non-v210 signal (8-bit, RGB) is converted on the CPU as before, and counted in
  `raw_fallback`.

## What the player reports

- **At open**, one line names every lever that is set and what is actually in effect, and
  says why a lever that was asked for is not:
  `capture-config: MEDIAPLAYER_GPU_V210=1, MEDIAPLAYER_DECKLINK_RAW_V210=1 | in effect: raw_v210 on, allocator off (node -1), hold off, direct_pair off, strict on, pin -1,-1, convert 8 thread(s)/input, gpu_v210 on, L3 groups 8`
- **Every ~10 s**, after the `perf:` line, a `capture:` line gives the last window's average
  and maximum ms for each stage. Callback, convert and claim wait are per eye; publish is per
  frame shown. It also has the raw counters (published, skipped, fallback), the held frames,
  and the allocator's buffers:
  `capture: raw_v210 allocator hold pinned | callback 0.02/0.05 ms, convert 2.10/3.40 ms, claim wait 0.01/0.20 ms, publish 0.01/0.02 ms (avg/max) | raw published 3590, skipped 0, fallback 0 | held 2 (peak 4), alloc 12 buffers 265 MB, refused 0`
- **`station_status.json` / `get_rig_status`** has the same under `decklink.capture`: each
  lever's set and effective value, the in-effect flags, counters, and stage windows.

## Running the matrix on the station

```powershell
# the default ladder (7 arms), 5 min each, 2 rounds (~75 min), station loopback inputs
powershell -ExecutionPolicy Bypass -File capture_matrix.ps1 -Left 2 -Right 0 -Minutes 5 -Rounds 2 -Fullscreen
# list the arms (-Set pin / upload / all add more)
powershell -ExecutionPolicy Bypass -File capture_matrix.ps1 -List
# a few arms only
powershell -ExecutionPolicy Bypass -File capture_matrix.ps1 -Left 2 -Right 0 -Arms baseline,raw,raw_alloc_hold
# no card: the same harness on the test signal (proves the switches, not the card)
powershell -ExecutionPolicy Bypass -File capture_matrix.ps1 -TestSignal 3840x2160@59.94 -Arms baseline,raw -Minutes 2
```

Each arm is one `soak_stress.ps1` run. Every arm uses the same base environment (by default
the station's: `DXR_VK_HUB_DEFER_COMPOSE_WAIT=1`, `MEDIAPLAYER_UPLOAD_THREADS=4`), and
`pro.env` is switched off for the runs. Rounds alternate the order of the arms. The output
folder (`Documents\DisplayXR\Test Results\CaptureMatrix\matrix_<stamp>`, plus a zip) holds:
- `summary.md`: the ranked table
- `results.csv`: one row per arm per round
- `system.json`: CPU, L3 groups, NUMA nodes, **every DIMM** (are all 8 channels of the 3975WX
  filled?), the DeckLink's and the GPU's **PCIe link** (current against max), drivers and
  versions
- each run's folder

How to read it:
- **A lever that helps** raises pairs/s and lowers its own stage (raw: convert ms; hold:
  callback ms). If pairs/s rises with no stage change, compare the spread across rounds; it may
  be noise or drift.
- **Every arm at the same ceiling** means the limit is outside the levers. Check
  `system.json` (empty memory channels; a PCIe link below x8 Gen3 for the card or x16 for the
  GPU) and the claim wait (one lane waiting on the other).
- **Pairs at the signal rate but render below it** means the render thread is the limit, not
  capture (see the `perf:` lines).

## pro.env: settings that survive a reinstall

Media Player Pro reads `KEY=VALUE` lines from a settings file at start, before anything else
reads the environment. This works for any variable: the levers, `MEDIAPLAYER_PRO_10BIT`, and
the runtime's `DXR_*`, which the runtime reads when it loads. A variable already set in the
environment wins over the file. The installer rewrites the Start-menu shortcut on every
upgrade, but it never touches this file.

- Windows: `%APPDATA%\DisplayXR\MediaPlayerPro\pro.env`
- macOS: `~/Library/Application Support/DisplayXR/MediaPlayerPro/pro.env`
- Linux: `~/.config/displayxr/mediaplayer-pro/pro.env`
- `MEDIAPLAYER_PRO_ENV_FILE` gives another path; `off` reads no file.

```
# the 8K station
DXR_VK_HUB_DEFER_COMPOSE_WAIT=1
MEDIAPLAYER_UPLOAD_THREADS=4
MEDIAPLAYER_PRO_10BIT=1
```

The player logs each variable it set, or left alone because the environment already had it.

## GPUDirect, and where it fits

**NVIDIA GPUDirect for Video** (the DVP library, `dvp.h`) is for Quadro/RTX pro cards, and the
RTX A4000 qualifies. It does **not** let the DeckLink write straight into VRAM. The card still
DMAs into a system-memory buffer. DVP pins that buffer, shares it with the GPU, and schedules
the GPU's copy from it with proper sync. So the frame crosses system memory once (the card's
write) and PCIe once (the GPU's read), with no CPU copy in between. Blackmagic's SDK samples do
this with OpenGL. DVP has no Vulkan API, only OpenGL, D3D11 and CUDA, so this renderer would
need interop to use it.

**GPUDirect RDMA** (peer-to-peer from a PCIe device straight into VRAM) is Linux-only and not
something the DeckLink SDK offers.

The same result is available in Vulkan without DVP: `VK_EXT_external_memory_host` imports the
allocator's buffers (this change's `ALLOCATOR`) as Vulkan memory, and the GPU copies from them
directly. That is #238 phase 3. `ALLOCATOR` + `HOLD` are the capture half of it. The render
half (the upload slot reads an imported card buffer instead of copying, and the buffer goes
back to the pool when its GPU frame retires) is the next change. With it, the per-frame CPU
copies at 2 x 2160p drop from about 5 to 0.

## Next

1. Run the default matrix on the station and post the zip.
2. If `raw` / `raw_alloc_hold` reach the signal rate: make the v210 CPU consumers work (#240
   phase 2), then consider making raw the default.
3. If they do not: #238 phase 3, the GPU reading the card's buffers directly
   (`VK_EXT_external_memory_host`), and check `system.json` for empty memory channels first.
