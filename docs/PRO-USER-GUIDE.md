# Media Player Pro — User Guide

Media Player Pro plays and monitors stereo 3D pictures on a glasses-free 3D display: movie
files, still images, image sequences, and live inputs such as a capture card or a camera.
On top of the normal player it adds the tools a stereo production uses on set and in
review: a colour grade, framing overlays, scopes, rig checks and snapshots.

This guide is for people using the player. The developer reference is [`PRO.md`](PRO.md).

## The screen

- **The picture** fills the window, in 3D on the panel.
- **Top bar**: Open, Live..., the display mode and the stereo layout.
- **Station panel** (left): live inputs, the source, looks (LUT / CDL) and snapshots.
- **Inspector panel** (right): view modes, channels, wipe, overlays, monitoring, stereo
  controls, colour and the rig check.
- **Scopes panel** (bottom centre, when on): waveform, vectorscope and histogram.
- **Status bar** (bottom): what is playing, its format, the look, timecode and depth range.

The panels fade when you leave the mouse alone and come back when you move it.
**`Tab`** pins them so they stay up (pinned panels are drawn as sharp flat 2D). **`Shift+Tab`**
hides or shows all the UI. **`F`** or **`F11`** is full screen with only the picture;
**`Esc`** leaves full screen.

## Opening something

- **A file**: click **Open** (or `Ctrl+O`), or drag the file onto the window. Movies (ProRes,
  DNxHR, H.264 / H.265, MXF), stills (JPEG, PNG, TIFF, MPO, LIF) and image sequences (EXR,
  DPX, TIFF: open any frame of the sequence).
- **The next or previous file** in the same folder: **`Down`** / **`Up`**.
- **A live input**: **Live...** in the top bar, or the Station panel's inputs list (**`J`**
  switches to the next input). Capture cards are set up with **DeckLink setup...** in the
  Station panel.
- **A stream URL**: `Ctrl+U`.

The player works out whether a picture is side by side (SBS) or mono from its name and
content. **If you see two pictures next to each other instead of one 3D picture, press
`L`**: it cycles the stereo layout override (auto, side by side, mono, ...). Pressing `L` by
accident is the usual reason for a flat side-by-side picture.

## Playing

| Key | Does |
|---|---|
| `Space` | play / pause |
| `Right` / `Left` | one frame forward / back (on a still: next / previous file) |
| `Shift+Right` / `Shift+Left` | five frames |
| `]` / `[` | one frame forward / back |
| `M` | mute |
| `S` | slideshow |

The scrubber at the bottom of the picture also seeks.

## Viewing

| Key | View |
|---|---|
| `1` | 3D |
| `2` / `3` | left eye only / right eye only |
| `4` | eye difference: what differs between the eyes, brightened 4x |
| `5` | anaglyph (red / cyan) |
| `6` | false colour (see Monitoring) |

**Zoom and pan**: the mouse wheel zooms around the pointer, middle-drag (or left-drag once
zoomed) pans, **`H`** fits the picture, **`Z`** toggles 1:1 (one source pixel per panel
pixel). Both eyes zoom together, so depth stays right.

**Stereo**: **`X`** swaps the eyes, **`-`** / **`=`** shift the convergence (where the
screen plane sits in the scene), **`0`** resets it, **`Backspace`** converges once
automatically, **`A`** keeps converging automatically on a live source. **`V`** steps through
the display's modes (2D, 3D, and any others the display offers); a chip names the new mode for
about two seconds whenever it changes, and once at start-up.

### Channels and gamma

To look at one colour channel on its own, press **`9`** (or Inspector > View > Channel). It
cycles **RGB → Red → Green → Blue → Alpha → Luma**. A single channel has no colour, so it is
shown as a grey picture: bright means a lot of that channel, dark means little. Use it to
judge noise, clipping or a key in one channel.

**Gamma** (Inspector > View) brightens or darkens the midtones of what you see, for
inspection. It is not part of the grade (the scopes ignore it), but a snapshot saves the
picture as shown, gamma included. Right-click the slider to reset it to 1.0.

### The wipe

A wipe is a split-screen comparison: a white vertical line splits the picture, and each
side shows a different version of it. **Drag the line** with the left mouse button (or use
Inspector > View > Wipe at). Press **`8`** to cycle the wipe modes:

- **Before | after grade**: left of the line, the picture as it arrives; right of it, with
  your grade (exposure, CDL, LUT). Use it to check what a grade or a LUT is doing to the
  same part of the picture. A subtle grade is hard to see; push the exposure to check the
  wipe is working.
- **Left eye | right eye**: left of the line the left eye, right of it the right eye. Both
  eyes are shown the same split picture, so it looks flat on purpose. If the rig's eyes do
  not match (one higher than the other, a different colour, out of sync), the picture jumps
  at the line.
- **Clip A | B**: left of the line the open clip (A), right of it a second clip (B), at the
  same moment: version 1 against version 2 of a shot, or a graded render against the
  plate. Click **Load B...** (Inspector > View) to pick B, or start the player with
  `--compare=<file>`. B follows A: it plays and pauses with A, and frame steps and seeks
  land on the same moment. Both sides go through the same look (exposure, CDL, LUT). B can
  be a movie or a still. Use clips of the same size and stereo packing.
- **Off**.

**`B`** swaps the whole picture between the two clips at the same position (see A/B
compare); with the clip wipe on, it swaps which clip is on which side.

## Colour

- **Looks** (Station panel): **Load .cube...** for a 3D LUT, **Load .cdl / .cc...** for an
  ASC CDL, or drop either file on the window. Each has an **On** box.
- **Exposure** (Inspector > Colour): ±4 stops. Right-click to reset.
- **ASC CDL values**: slope, offset, power and saturation, editable in the Inspector.
- **`G`** bypasses the whole grade, to see the picture as it arrives.

The grade is applied in this order: exposure, then the CDL, then the LUT.

### HDR sources

HDR video (HLG or PQ, e.g. HDR10) and scene-linear EXR are shown on the panel as an SDR
picture. The player recognises them from the file and the status bar says what it does,
e.g. `PQ -> SDR 1000 nits`.

- HDR reference white (203 nits) becomes the panel's white. Shadows and mid-tones are kept
  as they are, and the highlights above them are rolled off smoothly (ITU-R BT.2390) so the
  brightest part of the shot just reaches white instead of everything above white clipping.
  BT.2020 colours are converted to the panel's BT.709.
- **Exposure** acts on the HDR light before the roll-off: pull it down to see detail in
  highlights.
- **HDR tone map** (Inspector > Colour):
  - **auto** (default): roll off HLG and PQ; clip EXR at white (EXR then looks as before,
    and exposure brings back what is above white).
  - **on**: always roll off, EXR included.
  - **off**: clip everything above white.
  - **raw**: no mapping at all. Use it with a show LUT that expects PQ or HLG input.
- **Source peak (nits)**: the brightest the source gets, which sets where the roll-off ends.
  0 takes it from the file (HDR10 metadata), else 1000. HLG always uses 1000.
- The scopes, false colour and zebras measure the mapped picture.

The CDL and the LUT apply after the mapping, to the SDR picture. One exception: with an
**OpenColorIO** look on an EXR, the look gets the scene-linear values themselves, above 1.0
included, so the OCIO view (e.g. ACES) does the tone mapping; the status bar then says
`Linear -> OCIO look (unclipped)`. The compare clip B in the clip wipe is not mapped.

### Display calibration

The grade is the show's look and follows the content. A **display calibration** corrects
the panel itself, so that it shows Rec.709 / D65 / gamma 2.4 (or a site standard)
correctly. It belongs to the unit: each panel has its own, and the player loads it by
itself whenever it runs on that panel.

- **Calibrate panel...** (Inspector > Colour) loads a `.cube` 3D LUT made for this panel by
  a calibration tool (DisplayCAL, Calman, ColourSpace: export a 3D LUT for your target).
  The player keeps a copy for this panel, so from then on it loads at start-up.
- The Inspector shows **Display: <name>**, or **Display: not calibrated** in amber when the
  panel has none; the status line shows the same. Hover it for the panel's key and file.
- **`Shift+G`** (or **Bypass calibration**) turns the calibration off and on. **`G`** does
  not touch it: bypassing the grade still shows the picture through the calibration.
- **Remove** deletes this panel's calibration.

It is applied after the grade and the QC views, and before the gamma trim. The LUT must
cover 0..1 (the usual `DOMAIN` for a display calibration); one with another domain is
refused with a message.

Each panel is recognised by the key the DisplayXR runtime gives it (the one
`displayxr-cli displays --claims` prints), on the display the player's window is on; drag
the window to another 3D panel and its calibration follows. What the key is depends on
what the panel reports:

- The panel's plug-in reports the panel's own serial: the key uses it, and moving the
  cable to another port keeps the calibration.
- Otherwise, the panel's EDID has a serial number: the same, unless the panel reports a
  different product code on each input.
- Neither: the key includes the port (`...@DISPLAY1`), so the calibration stays with the
  port, not the panel.

Two connected monitors that would otherwise share a key also get the port added. With a
DisplayXR runtime that does not
report the key yet (`XR_DXR_display_info` before v24), the OS display name (`DISPLAY1` on
Windows) stands in, which follows the port.
The Inspector shows the key when you hover the calibration line. The files live in
`%APPDATA%\DisplayXR\MediaPlayerPro\calibration\<panel>.cube` (macOS:
`~/Library/Application Support/DisplayXR/MediaPlayerPro/calibration`). Set
`MEDIAPLAYER_PRO_CAL_DIR` to use another folder, for example one shared by a site;
`MEDIAPLAYER_PRO_CAL=<file.cube>` loads a calibration for one run without keeping it.
An agent can do the same with `set_display_calibration`.

### 10-bit to the panel

By default the picture reaches the display at 8 bits per channel, which can show banding
in smooth gradients (skies, vignettes) after a strong grade or LUT. Start the player with
**`--10bit`** to keep it at 10 bits all the way to the panel:

```
mediaplayer_pro_handle_vk_win.exe --10bit clip.mov
```

It requires DisplayXR runtime 2.53 or newer and the Leia plug-in 2.17 or newer; do not
use it with older versions (the player logs a warning at start-up when the runtime is
older). It only applies when the player runs on its own: under the DisplayXR Shell the
Shell composes the picture and `--10bit` has no effect. On Windows the installer
adds a **Media Player Pro (10-bit)** shortcut that does the same. `--8bit` (or setting
`MEDIAPLAYER_PRO_10BIT=0`) goes back to the default. Setting `MEDIAPLAYER_PRO_10BIT=1` in
the environment turns it on without the flag.

## Overlays (framing)

Inspector > Overlays. They stay on the picture when the panels fade.

- **Frame line** (**`Q`** cycles): 2.39, 1.85, 16:9, 9:16, 4:3, 1:1, with a **Matte** that
  darkens the area outside the line.
- **Safe areas**: action safe (93 %, grey) and title safe (90 %, blue).
- **Centre cross**.
- **Desqueeze** 1.3x to 2.0x: stretches an anamorphic picture to its real width.
- **Burn-ins**: timecode, frame number, clip name, colour space, in the picture's top corners.

On the 3D panel the overlays sit flat at the screen surface.

## Monitoring

Inspector > Monitoring.

- **Scopes** (**`W`**): a panel with the **waveform** (brightness of every column of the
  picture, 0 to 100 %; tick **RGB parade** to see red, green and blue separately), the
  **vectorscope** (colour: hue around the circle, saturation outwards, with the 75 % colour
  bar targets and the skin-tone line) and the **histogram**. The scopes measure the left eye
  **after the grade**, so they show what the panel shows.
- **Pixel probe** (**`P`**): hover the picture to read the graded red, green, blue and luma
  under the pointer.
- **Zebras** (**`7`**): black stripes over everything brighter than a level you choose
  (50-100 %). The stripes sit on the objects at their depth in 3D.
- **False colour** (**`6`**): exposure shown as colours: purple = crushed, blue = very dark,
  green = 18 % grey (correct exposure for mid-tones), pink = skin tones, yellow = near
  white, red = clipped. Everything else is grey.

## The rig check

The rig check measures a stereo picture continuously and warns you about what makes 3D
uncomfortable or wrong. It runs on **side-by-side video and live inputs** (a capture card, a
camera, the built-in test signal); it does not run on still images.

For a live input, the **Source** section also shows the input, its timecode (**`T`** shows /
hides it) and the measured delay from capture to screen.

The **Rig check** section (Inspector) shows, each in green, amber or red:

- **Vertical error** and **roll**: one eye higher than, or tilted against, the other.
- **Eye sync**: the two eyes from different moments (frames apart).
- **Colour match**: the eyes differ in colour or brightness.
- **Depth range**: how far in front of and behind the screen the scene goes, against your
  depth budget.
- **Frame edges**: something in front of the screen that the left or right edge of the
  frame cuts off - an *edge violation*. One eye sees more of it than the other, and it
  reads as poking through a window: hard on the eyes. The line says **OK**, or which edge
  and how far in front (`left -2.5%`). To fix it in the shot, push that object back behind
  the screen (convergence) or keep it clear of the frame edge.

Problems that last half a second or more become **alerts**, shown under the numbers and
logged with their timecode to `Pictures\DisplayXR\station_alerts.jsonl`.

- **`R`** shows a 60-second history strip of depth and vertical error.
- **`D`** draws the rig check on the picture. Each **dot** is a point matched between the
  two eyes, coloured by depth: orange / red in front of the screen, white on it, blue
  behind; a **red ring** marks a dot outside the depth budget. An edge violation shows as
  a **red bar** along that edge, over the rows where it happens, with its depth
  (`EDGE -2.5%`). Press **`D`** again to turn the dots and bars off.

### Limits

Depth is in % of the picture width: negative is in front of the screen, positive behind.
The defaults, and how to change them when starting the player:

| Check | Default | Option |
|---|---|---|
| Depth budget (nearest, farthest) | -2 % to +1 % | `--depth-budget=-2,1`, or `--depth-budget=comfort` for -1 % to +2 % |
| Vertical error | 1 px | `--vertical-limit=1` |
| Colour match | dE 2 | `--colour-limit=2` |
| Frame edges (nearest allowed at the edge) | -0.3 % | `--edge-limit=-0.3` |

### Target screen

The same picture has more depth on a screen that fills more of your view (a cinema) and
less on one that fills less. **Target screen** (Inspector) judges the depth for the screen
the picture is made for: **Cinema** (10 m wide, seen from 12 m), **Home TV** (65", 1.44 m
from 2.5 m), **Monitor** (27", 0.60 m from 0.65 m), **Tablet** (11", 0.25 m from 0.40 m)
or **Custom** (your width and distance). It is **Off** by default.

- **Show at the target's size** (on when you pick a target) shows the picture on the 3D
  panel at the size it appears from a seat in front of the target. The depth then looks
  as it will there: things in front of or behind the screen by the same angles. The
  picture is cropped when the target fills more of the view than the panel does, and gets
  a black border when it fills less. The Inspector shows the scale (`Picture x1.45,
  cropped`). It uses the panel's size, how far you sit from it (followed by eye tracking,
  smoothed over about half a second; the runtime's nominal distance when nobody is
  tracked) and the player window's size: a smaller window shows less of the picture.
- The rig check's near and far are also given **for the target**: millimetres on its
  screen and arcminutes (′) at its viewing distance, and so do the depth alerts
  (`Foreground at -2.0% for 2 s [-203 mm, -58' on Cinema]`). When the background's parallax is
  wider than the eyes are apart, the eyes have to turn outwards (diverge); past 0.5° the
  rig check raises **Background diverges on the target screen**. The Inspector shows
  where that starts (`Diverges past +1.7%` on the cinema).
- **Depth budget -1 / +2 %** sets the depth limits to 1 % in front, 2 % behind.

`MEDIAPLAYER_PRO_TARGET=cinema` (or `tv`, `monitor`, `tablet`, `<width>x<distance>` in
metres) starts with a target; `MEDIAPLAYER_PRO_TARGET_PREVIEW=0` keeps the picture at the
panel's size. An agent uses `set_target_screen`; `get_rig_status` and snapshot notes carry
a `target` block.

## Snapshots

Press **`N`**, or click **Snapshot** in the Station panel. It saves both eyes **as shown**
(with the grade, view mode, wipe and channel you have on) as a PNG in
**`Pictures\DisplayXR\`**, with a `_note.json` beside it holding the timecode, rig numbers
and depth budget. Type a note next to the snapshot in the Station panel's Snapshots list.

To take a frame from a movie: pause on it (`Space`, then `Left` / `Right` to the exact
frame) and press `N`.

A snapshot is the picture that goes to the 3D display (the two eye views side by side),
not a photo of the panel.

### QC report

Click **Export QC report** (next to **Snapshot**) to hand the session on. It makes a new
folder in `Pictures\DisplayXR\` (`qc_` and the date and time) with:

- **report.html** - every rig alert of this session (timecode, how long, what), and every
  snapshot with both eyes, its note and its rig numbers (over-budget numbers in red). One
  file: the pictures are inside it, so it can be emailed as is.
- **markers.edl** - the same as timeline markers for DaVinci Resolve (*Timeline > Import >
  Timeline Markers from EDL*): alerts red, snapshots blue, at their source timecode.
- **markers.csv** - every row with the rig numbers, for a spreadsheet or another editor.

Alerts and snapshots without a source timecode are in the report and the CSV but not in the
EDL. The report is written in the background, so the live picture keeps playing; the
button reads **Writing QC report...** until it is done, then a message gives the folder.

## A/B compare

Pick a second clip with **Load B...** (Inspector > View), `--compare=<file>` or the
`set_compare` agent tool. Then press **`B`** to swap the whole picture between the open
clip and the compare clip at the same position (the status bar shows **[A]** or **[B]**),
or use the **Clip A | B** wipe (`8`) to see both at once, split by the line.

## All keys

| Key | Does |
|---|---|
| `Tab` / `Shift+Tab` | pin the panels / hide or show all UI |
| `F` / `F11`, `Esc` | full screen, leave full screen |
| `Ctrl+O`, `Ctrl+U` | open a file, open a stream URL |
| `Up` / `Down` | previous / next file in the folder |
| `Space`, `Left` / `Right`, `Shift+Left` / `Shift+Right`, `[` / `]` | play / pause, frame step, 5 frames, frame step |
| `1`..`6` | 3D, left, right, difference, anaglyph, false colour |
| `7`, `8`, `9` | zebras, wipe, channel |
| `W`, `P`, `Q` | scopes, pixel probe, frame line |
| wheel, middle-drag, `H`, `Z` | zoom, pan, fit, 1:1 |
| `G`, `Shift+G` | grade bypass, display calibration bypass |
| `X`, `-` / `=`, `0`, `Backspace`, `A` | swap eyes, convergence, reset, converge once, auto-converge |
| `L`, `Shift+L` | stereo layout override, convert 2D to 3D |
| `V` | next display mode (2D / 3D / others the display offers; a chip names it) |
| `D`, `R`, `T` | depth overlay, rig history strip, timecode |
| `N` | snapshot |
| `J` | next live input |
| `B` | A/B compare |
| `M`, `S` | mute, slideshow |
