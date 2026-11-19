# Media browser

The in-app gallery (#160): pick a photo or video from the device's library without
leaving the 3D session, with 3D badges, durations and filters. Android came first; the
Windows consumer player has it too (#252, see [Desktop (Windows)](#desktop-windows)).

This is the developer reference.

## Scope

In: images and videos from the Android MediaStore, the sources (All media, Camera,
Downloads, Pictures, Movies, Unsupported, "Other files..." for the system picker) as a
rail or a drawer, a square-tile grid with date sections, 3D and duration badges,
Downloads as folders, select and delete, filter chips (All / 3D / 2D, All types / Videos /
Photos) and view options (newest or oldest first, show unsupported files, off by default).

Out for now: a desktop browser; the VR badge; HEIC and MV-HEVC on Android. Files
the player cannot open are listed under Unsupported with their format, so the formats
left to do stay findable. MediaStore does not index MPO, JPS or JFIF as media, so the
gallery cannot list them; "Other files..." reaches them.

## Structure

```
src/ui/Icons.{h,cpp}            every icon, the player's and the browser's (one set)

src/browser/                    namespace mp::browser, platform-neutral
  MediaItem.h                   one media entry: id, name, kind, size, duration, date, folder
  MediaLibrary.{h,cpp}          the item list, sources, filter and sort
  MediaProvider.h               the interface a platform implements: permission, refresh,
                                thumbnails, open, delete
  ThumbnailAtlas.{h,cpp}        GPU: thumbnail slots packed into a few textures
  ThumbnailCache.{h,cpp}        media id -> atlas slot, least recently used reuse
  TileCrop.{h,cpp}              the part of a thumbnail a tile shows, and the size to ask for
  StereoClassifier.{h,cpp}      3D badge verdicts: background thread, cached on disk
  StereoGrid.{h,cpp}            the 3D grid: eye crops, per-tile convergence, per-view quads,
                                the rendering-mode borrow
  ThumbnailDepth.{h,cpp}        the 3D grid: each stereo thumbnail's depth, on a worker
  StereoPairs.{h,cpp}           the 3D grid: pairs for photos whose thumbnail is one view
  PairDiskCache.{h,cpp}         ... and those pairs kept in the app's cache dir
  TouchScroller.{h,cpp}         drag, inertia, tap vs drag
  SlideGesture.{h,cpp}          swiping to the next or previous file in the player
  BrowserLayout.{h,cpp}         pure layout: columns by width, rail or drawer, date sections,
                                the sectioned grid's positions
  MediaBrowser.{h,cpp}          the ImGui view: top bar, chips, rail / drawer, grid, view
                                options, empty states

android/src/main/cpp/browser/
  AndroidMediaProvider.{h,cpp}  MediaProvider over JNI, plus the swipe's stills; its JNI
                                exports live here

android/src/main/cpp/           the player side of the swipe, and the 3D grid's pass
  neighbour_stills.{h,cpp}      the files either side of the one playing, ready on the GPU
  still_image.{h,cpp}           an image decoded as the player shows it, for opening and stills
  stereo_grid.{h,cpp}           the 3D grid's tiles drawn into the projection atlas
  shaders/stereo_grid.{vert,frag}

android/src/main/java/.../
  MediaStoreProvider.kt         MediaStore query, permissions, thumbnails, stills, opening a file

tests/
  media_library_test.cpp, stereo_classifier_test.cpp, thumbnail_cache_test.cpp,
  tile_crop_test.cpp, letterbox_test.cpp (src/media/Letterbox),
  touch_scroller_test.cpp, slide_gesture_test.cpp, stereo_grid_test.cpp,
  browser_layout_test.cpp, zoom_gesture_test.cpp
```

## Rules

- Browser code lives in the folders above. `src/browser/` has no JNI, no MediaStore and
  no media paths: it reaches the device's media only through `MediaProvider`.
- Existing classes get wiring only (Android `main.cpp`, `MainActivity.kt`, the build
  lists). Extend one only when the capability belongs to it, like the fd variant of the
  LIF check in `LifLoader`.
- Follow the repo's C++ habits (C++17). One class per header, no globals. PascalCase
  methods, trailing `_` on members, `k` constants, C-style casts.
- A class that owns resources has a default constructor, `bool Initialize(...)` and
  `Shutdown()`, and its destructor calls `Shutdown()` (as `VulkanRenderer` and
  `XrSession` do).
- "Nothing" is a false or null return, with results through out-parameters
  (`bool Get(int slot, Thumb& out)`). No `std::optional`, no factory functions, no
  `assert`: check the inputs and return false.
- The pure logic (library, cache, scroller) has unit tests that run on desktop.
- Every fixed UI size goes through `ui::Px()`, or `ui::Dp()` for a size meant in Android dp
  (touch targets of 48 dp, the 280 dp drawer).
- Text uses the touch type scale (`ui::TextStyle`: Title, Row, Small) and icons the one set
  in `src/ui/Icons.h`.

## Threads and data flow

- The render thread owns `MediaBrowser`, `MediaLibrary`, `ThumbnailCache` and
  `ThumbnailAtlas`, like the rest of the ImGui UI.
- `StereoClassifier` is driven from the render thread and runs the content check on its
  own worker thread. `NeighbourStills` does the same for the stills: it stages and
  decodes on a low-priority worker and uploads on the render thread.
- `MediaStoreProvider` runs on the Android UI thread, a thumbnail executor and a still
  executor.
- Java to native: Java hands results (the item list, thumbnails, the permission state) to
  `AndroidMediaProvider` through JNI calls, which queue them under a mutex. The browser
  takes them once a frame.
- Native to Java: native queues requests (prompt for permission, refresh, thumbnails,
  open) and Java takes them once per Choreographer frame. Native never calls into Java:
  a JNI call into an app class from a native thread is classloader-fragile (runtime#507).
- Opening an item takes the same path as the system picker: Java opens a file descriptor
  and passes it to `nativeOpenVideoFd` with the display name, so playback is unchanged.

## 3D badges

The player's layout resolver assumes stereo unless mono is proven (`MediaSource.h`).
That is right for playback but would badge every photo, so a badge needs positive
evidence. `StereoClassifier.h` lists the evidence and its order. The file's own
evidence (a LIF, a two-track video) is read by the provider while it makes the
thumbnail, as `Thumbnail::file3D`. The verdict also picks the thumbnail
crop: the left eye for side by side, the top for top-bottom, the whole thumbnail for a
LIF or two-track video, whose thumbnail is already one view.

The "3D" badge sits at the tile's top left (right of the box in select mode; hidden in
the 3D grid, where every tile is 3D). The top
right holds a video's length and, under Unsupported, the file's format.

## Thumbnails

A tile shows a square: one eye (as above), without black bars, centre-cropped, a
half-packed eye unsqueezed (`TileCrop.h`). Only that square goes to the GPU, so an atlas
slot (512 px) holds exactly what the tile shows.

- Size: each request carries the edge to fit the thumbnail in, chosen so the square is
  about one thumbnail pixel per screen pixel (`ThumbnailEdge`). Videos only: a photo keeps
  the system's cached thumbnail at one atlas slot, as before. Photos were sharp enough, and
  decoding photo files for the grid takes CPU that opening a picture needs. The edge comes
  from the MediaStore's width and height and the verdict known at the time. A 16:9 video on a 400 px tile is made at
  712 x 400, a full side-by-side one at 1423 x 400. The square never exceeds a slot, and no
  thumbnail is made above the file's own size or 2048 px.
- A tile that still comes out soft (the content check found it side by side after it was
  asked for as 2D, bars came off, the size was unknown) asks once more, larger. The verdict
  is cached, so the next launch asks for the right size first.
- The system's cached thumbnail stops at a size of its own. When it is well short of what
  was asked, the provider decodes the file: a video's representative frame through
  `MediaMetadataRetriever.getScaledFrameAtTime`, an image subsampled. The provider logs the
  average time and size every 64 thumbnails.
- Black bars (`media/Letterbox.h`): the runtime's 2D-to-3D letterbox measurement
  (u_lift_mailbox.c), made safe for one picture: bars must be near black, come in a pair of
  about equal size, end at a hard edge and leave at least 30% of the picture. Otherwise
  nothing is cropped, so a dark scene keeps its dark. A stereo frame is measured on the eye
  the tile shows. Videos only: a still with a dark surround is more often a screenshot of a
  dialog than a letterboxed picture, and the measurement cannot tell them apart.

## 3D grid

Under the "3D only" filter the panel goes to its 3D mode and every tile is stereo: its left
eye in the left-eye views, its right eye in the others. Android only.

- No weave extension: the tiles are drawn into the app's own projection atlas, view by view,
  at the rects the browser laid out that frame (`MediaBrowser::Stereo()`), by
  `StereoGridRenderer`, which replaces the atlas pass for that frame. The browser lays out
  first, so a scrolling tile is never drawn a frame behind the HUD.
- The ImGui HUD stays the flat chrome (a window-space layer at disparity 0). It is
  transparent over the grid: the bars, the chip row, the rail or drawer, folders, section
  headers, badges and messages are flat,
  the tiles' pictures are in the projection layer.
- A tile's slot holds only the left eye's square (see Thumbnails). Under the filter the
  same square of the right eye goes in a second slot (`RightEyeKey`), so the window counts
  two slots per tile; a tile uploaded before the filter is fetched again once for it. The
  pass samples the `ThumbnailAtlas` pages through their ImGui descriptor sets: one bind and
  one instanced draw per page and eye. Folders are flat.
- Photos whose thumbnail is one view (a LIF, an MPO) get a pair (`StereoPairs.h`): the
  request asks the provider for the file too (`ThumbnailRequest::pairFile`), and one
  low-priority worker reads where the second view is (`LifLoader::FindViews`: trailer and
  JSON only; `MpoLoader::FindSecondView`: the MP index in the head) and decodes just that
  JPEG at the eye size (the thumbnail times the 3D mode's view scale) with the platform's
  scaled decoder (AImageDecoder, Android 11+). The left eye is the system's thumbnail when
  that is the whole view. Some files' is not: the NP02J camera's LIFs (since 2026-07) carry a
  512 x 512 EXIF thumbnail, a centre crop of the 16:10 view, and MediaStore returns it, so the right view decoded at its size came out squeezed.
  When the thumbnail's aspect is not the view's (`ThumbnailIsWholeView`, the view's size from
  its JPEG header), both eyes are decoded from the file at the view's aspect. The
  pair is kept in memory (24 MB, by id and modified time) and on disk (`PairDiskCache.h`):
  `Context.getCacheDir()/stereo_thumbs`, one JPEG (q85) of the whole pair per file version,
  named by a hash of the media id, modified time, size, thumbnail size, view scale and
  `kPairFormatVersion` (bump it whenever a pair would come out different). Lookup is memory,
  then disk, then made and stored, all disk I/O on the pair worker; 96 MB, least recently
  used first (a hit touches the file's mtime), written to a temp file and renamed, and a
  stored pair that does not decode is deleted and made again. The worker logs the cache's
  size once when it opens, and disk hits vs pairs made in its every-4-pairs cost line. LIFs carry no embedded thumbnail
  or preview to use instead (checked on the phone's: base JPEG + one view blob, no EXIF
  thumbnail).
- A one-view LIF (one picture and its disparity map: every 2D-to-3D conversion) gets its
  pair the way the player makes one (`LifLoader::FindDepthView`, then
  `DepthWarp::MakeSbs` at the parallax `LifLoader::Load` uses: the classic `gain`, or the
  5.x `inv_z_map` range, capped at 2 % and flipped when the map's far end is 255), on the
  same worker: the map decoded at the eye size (the platform decoder scales it while
  decoding), the picture from the system's thumbnail (or decoded small when that is a
  crop), both eyes warped on one thread. The grid then places it like any pair (Depth per
  tile below). Still flat: two-track videos.
- Leaving the grid for the player stops the thumbnail requests, so they do not compete
  with opening the file and its neighbours.
- Depth per tile (`StereoGrid.h` `TileShift`): `ThumbnailDepth` measures each stereo
  thumbnail once with the player's auto-convergence estimator. The subject goes 0.25 % of
  the tile behind the plane, and further back (up to 1 % for the subject) until the
  nearest content is at the plane. Unmeasured tiles go 0.5 % back. A slot holds only the
  square, so the two crops move apart by zooming in about as much as the shift. Every tile fades to
  the flat background over a 3 dp border, so a border never cuts something in front of the
  plane. The gutter widens from 4 to 12 dp.
- The "3D" badge is hidden (every tile is 3D) and the duration sits on an opaque pill.
- Modes (`ModeBorrow`): the browser borrows flat, or the 3D mode under the filter, and
  hands back to the player the mode it had. Only the filter decides, never what the grid
  happens to show, so a scroll never flips modes. Back from the player into a 3D-filtered
  grid goes back to 3D. Until the panel is in 3D the grid is drawn flat as before.
- Resolution: a tile's useful size per eye is its rect times the mode's view scale (0.75 on
  the phone, half the width on a side-by-side panel); a video's squares are made about 1:1
  with the tile, so they cover it. A side-by-side photo keeps the system's cached thumbnail
  (see Thumbnails), so its eyes can be softer than the tile.
- Only the browser's grid: the pass runs only while the browser shows, under the filter,
  with the panel in 3D. The player's frames (and the swipe's pages) always take the atlas
  pass as before.

## Desktop (Windows)

Phase 1 of #252: the 2D browser in the consumer player on Windows. Pro keeps its station
panels, and macOS / Linux keep the system dialog until they get a provider.

**When it shows.** Open and Ctrl+O show it when there is no workspace picker (inside a
workspace the shell's picker keeps Open). "Other files..." opens the system dialog. It
covers the HUD and pauses a playing video; the close button, or Esc at the top of a source,
goes back to the player. The basic player also starts in it when launched with no file (as
Open would: the shell's picker inside a workspace), on the place last shown
(`browser_view.txt` keeps `source=` / `folder=` beside the view); a file on the command line
opens directly. `MEDIAPLAYER_NO_BROWSER_AT_START=1` keeps the idle screen at launch.

**Sources** (`WindowsMediaProvider`, declared through `MediaLibrary::SetSources`):

| Source | From | Browsed |
|---|---|---|
| Recents | the Recent list (files only) | flat, in the order opened |
| Pictures, Videos | the Windows libraries (OneDrive Pictures and added folders included), walked | folder tree; several roots are listed first |
| Downloads | the known folder, walked | folder tree |
| Media folders | the Windows Search index: pictures with a long side of at least 1024 px, videos of at least 1 MB, nothing under AppData, Program Files, Windows or a hidden folder, nor what software keeps beside itself (below) | one tile per folder, a Newest tile (50) first |
| Pinned | folders added with "Add folder...", walked | its folders first, then a tree; the sidebar lists each pinned folder as a row of its own |

A walk reads directory entries only, at most 6 folders deep and 3 s per tree. One index
query takes about 0.15 s for ~1,400 items on the 8K dev box; the whole list about 0.2-0.3 s.
If the index cannot be queried, Media folders is empty and a strip over the grid says so.

**What software keeps beside itself is left out of Media folders** (`SoftwareFolders` in
`WinShellMedia.cpp`). On a developer's box the index holds every engine icon, test texture and
demo build's screenshots; per folder, cached:

| Rule | Leaves out |
|---|---|
| Code checkout | anything in a folder with a `.git` (folder or worktree file) on its way up, stopping at the profile and its known folders, so a dotfiles repo cannot take everything |
| Game library | anything under `Epic Games`, `steamapps`, `SteamLibrary`, `GOG Games`, ... |
| Asset folder | anything under a folder named `textures`, `tex`, `icons`, `models`, `resources`, `sprites`, `shaders`, `fonts`, `plugins`, `thirdparty`, `StreamingAssets`, `*_Data` (a Unity player) or `*.fbm` (FBX textures) |
| Program beside | pictures (not videos) in a folder that itself holds an `.exe` or `.dll`; the folder's own files only, so a stray installer in Downloads hides nothing below it |

The log line `browser: index left out software files - ...` counts each rule. On the 8K dev
box they take Media folders from 171 folders (3,467 files) to 62 (3,114): the files were mostly
the user's own, the folders mostly not. Pictures, Videos, Downloads and pinned folders are
walked, not filtered; Open file / Open folder and drop reach everything.

**Cloud files** (OneDrive / SharePoint placeholders) are never read while browsing:
- thumbnails come from the shell's thumbnail cache only; a file Windows has none for shows
  the image icon until it has been opened once;
- a cloud tile shows a cloud bottom left, and its tooltip says opening it downloads it;
- the file check for 3D (LIF, MPO) is skipped, so a cloud file the name does not settle
  shows "3D?" (`StereoVerdict::unchecked`) and is 2D for the filters.
Local stills get the file check (`LifLoader::IsLif`, `MpoLoader::IsMpo`); two-track videos
are not probed on desktop yet.

**Pinned in the sidebar.** Under PINNED, each pinned folder is a row (a pin, its name, how
many files it holds), lit while it or a folder inside it is shown; a click shows it, the
tooltip gives its full path. The path reads "Pinned > <folder>", and Pinned's top still lists
them all. Right click a row: Open, Remove from Pinned. A folder shown that is unpinned goes
back to Pinned's top (or the first source, with no pins left).

**Folder previews.** A folder card in the grid shows what it holds instead of a folder
outline: a 2 x 2 mosaic of thumbnails of its files (`MediaLibrary::FolderPreviews`: files in
it and its sub-folders, or in it only for a Media folders group; local files before cloud-only
ones, then newest), with a folder-and-count badge. The previews share the 64 thumbnail slots
with the files: the visible folder cards get four each while the slots the files leave
allow, else one each (a single picture), else none. A cell whose thumbnail is not here yet
stays the surface colour, and a card with none here shows the folder outline. Cloud-only
files are asked for the same way as their own tiles (the shell's cache only, never a
download). Not in the details view, nor in the 3D grid (two slots per file).

**Folder menu** (right click a folder tile, Shift+F10 or the Menu key on the one with the
focus ring, or a long press on a touch screen): Play slideshow (also Shift+Enter); Open (also
Enter, or a click); then Pin this folder; Remove from Pinned (top of Pinned); Hide from Media
folders (top of Media folders, for junk the size filter keeps). The Newest tile has only Play
slideshow and Open. Pinned and hidden folders are kept beside the Recent list (`pinned.txt`,
`hidden_folders.txt` in `%APPDATA%\DisplayXR\MediaPlayer\`).

**Slideshows.** A click on a folder opens it; playing one is asked for (`Action::Slideshow`,
the ids through `MediaBrowser::TakePlaylist`, `SetSlideshows` turns it on):

| From | Plays |
|---|---|
| A folder tile (menu, Shift+Enter) | the folder and its sub-folders (`MediaLibrary::QueryTree`), as its tile counts them: the files directly in a folder first, then each sub-folder by name, depth first; each folder's files in the view's sort. A Media folders group or the Newest tile: its own files |
| The header's play button, Ctrl+Enter | the place shown the same way, under the search too; a flat source (Recents) its list. Not under Unsupported |
| A file's menu: Play slideshow from here (Shift+Enter on a file) | the grid as shown, from that file |

All of them keep to the type filter and the All / 3D / 2D filter (a file whose verdict is not
known yet is left out under 3D or 2D). **Cloud-only files are left out**: a slideshow never
downloads, and the toast counts them ("Slideshow: 42 items (3 online-only skipped)"); with
nothing local, the browser stays and says so. The player takes the list as a fixed playlist
(`playlistFixed_`, as a multi-file drop): 5 s a still, a video once, a dip to black between,
and it **loops**. Up / Down step through it; S or the slideshow button pause it on the list.

- **Esc** during it (or after S, while the list is still the one walked) goes back to the
  browser at the place it started from, the focus ring on the file it was showing. Esc quits
  everywhere else, as before (`Window::SetEscapeBacksOut`).
- **2D / 3D switch:** the first file is an open, so it starts at its own default; the walk
  keeps a flip for the rest of the slideshow; any other open drops it. Under the 3D-only
  filter the panel stays in 3D for the slideshow (as for a file opened from the 3D grid).
- Opening one file from the grid does not start a slideshow; the slideshow button then walks
  the grid and **stops at its end** (it used to run on into the folder's name-sorted list).
  It steps over cloud-only files too, rather than stop at a download.
- Outside the browser: a folder on the command line, dropped on the window, or handed over by
  a second launch starts the slideshow over its own files by name; Open folder... (Ctrl+Shift+O)
  opens the first file without one.

Android: not yet (its player has no slideshow); a long press on a folder there does nothing.

**File menu** (right click a file, or Shift+F10 / the Menu key on the focused one): Open; Play
slideshow from here; Show in File Explorer; Copy path (also
Ctrl+Shift+C on the file with the focus ring, else the one under the pointer). On a file in
a selection of several, Copy path becomes "Copy N paths": every selected file's path, one a
line, in grid order (Ctrl+Shift+C does the same whenever there is a selection). Right click
outside the selection clears it, so the menu is that file's alone. No delete on desktop.

**Selection** (desktop): Ctrl+click adds or removes a file, Shift+click selects the run from
the last Ctrl+clicked (or clicked) file, Ctrl+Shift+click adds that run; Ctrl+A selects every
file shown. Folders are not selected. A plain click still opens (and drops the selection);
Esc clears it; going to another place clears it. A selected card or row has an accent fill
and a 2 px accent border (the focus ring stays a light ring outside it); the status bar
counts it. The rules are `ClickSelect` (BrowserLayout, tested).

**Views.** The toolbar's grid / details switch, at its right. The grid's cards are 120-320 px
wide (the size slider beside the switch, or Ctrl+wheel over the grid; 176 at start). The
details view is a row per file, 48 px with a 40 px picture: name (a cloud after it), depth
(3D, 3D?, 2D), type, dimensions, length, date, size. A narrow window drops type, dimensions,
size, then date. The Date heading turns the sort round. Both views draw the pictures in 3D
under the 3D filter. The view, the card size and the sort are kept between launches in
`browser_view.txt` beside `pinned.txt` (`view=grid|details`, `card=<px>`,
`sort=newest|oldest`; an unknown line or a bad value keeps the default), written when one
changes and the size slider is let go.

**Status bar** (24 px along the window's foot, under the sidebar too; none in a window under
420 px tall): what the grid holds ("3 folders · 15 items", "12 in 3D" under the 3D filter),
how many are selected, the focused file (the focus ring's, else the one under the pointer:
name · width×height · length · 3D (SBS / TB / the format, e.g. LIF) or 2D or 3D? · Online
only · size on disk; a folder: its name and count), and a hint at the right (from 760 px of
width). Parts are cut from the right as the window narrows.

**Search** (Ctrl+F, or the box in the header; none below 640 px of window width): shows the
folders and files of the place shown whose names hold the text, any case. Esc or the cross
clears it; going to another place clears it too.

**Input.** Mouse: click, wheel scrolling, a hover outline and a name tooltip; the side
buttons go back and forward. Keyboard: arrows, Page Up / Down, Home / End move a focus ring,
Enter opens, Shift+Enter plays the focused folder (or the grid from the focused file),
Ctrl+Enter plays the place shown, Shift+F10 or the Menu key opens the focused tile's menu,
Alt+Left / Alt+Right go back / forward through the places shown (any route
there: the sidebar, the path, a folder), Backspace or Alt+Up go up a folder, Esc backs out
then closes, Ctrl+O the Windows file dialog. Player hotkeys are off while it shows. A file
opened from the grid makes Up / Down in the player walk the grid as shown (no wrap).

**Sizing.** A dp is one design unit on desktop (1.5 on Android), so the browser matches the
rest of the desktop UI; the sources stay a rail from 600 dp of window width.

**Testing.** `MEDIAPLAYER_SHOW_BROWSER=1` shows it at launch (`3d`: under the 3D filter);
with `MEDIAPLAYER_DUMP_HUD` the HUD dump shows it. `MEDIAPLAYER_BROWSER_SOURCE=<name>` starts
on a source (or, with no source of that name, on the pinned folder of that name), `MEDIAPLAYER_BROWSER_VIEW=details` in the details view,
`MEDIAPLAYER_BROWSER_QUERY=<text>` with a search, `MEDIAPLAYER_BROWSER_FOCUS=<n>` with the focus
ring on tile n (0 = the first; folders count), `MEDIAPLAYER_BROWSER_SLIDESHOW=<n>` plays tile n
once the grid is listed (a folder's slideshow, or the grid's from a file; works on the browser
shown at launch, with or without `MEDIAPLAYER_SHOW_BROWSER`), `MEDIAPLAYER_BROWSER_SELECT=0,2,3` with those
files selected (counted among the files). A test run (`MEDIAPLAYER_SHOW_BROWSER`) never
reads or writes the user's `browser_view.txt`; `MEDIAPLAYER_BROWSER_PREFS=<file>` names a view
file for it to use instead. `win_shell_media_probe` (built on Windows, not a ctest test) prints
the index query and thumbnail timings on a real box; `--thumb <path> <cacheOnly>` checks one
file and whether it stayed a cloud placeholder.

Phase 2 (the 3D-only grid with per-eye thumbnails) is planned in #252.

## Android behaviour

- Launch opens the browser, over the idle splash. Without a HUD there is no browser and a
  tap on the splash opens the system picker.
- The browser borrows the flat 2D mode (the 3D mode under "3D only", see 3D grid) and hands
  the player's mode back on play.
- The player's mode follows the file: 3D for a 3D file, flat for a 2D one. The 2D/3D switch
  shows for 3D files only (no 2D->3D conversion on Android yet); a flip to 2D is kept while
  swiping through the list and dropped by a pick from the grid or the picker.
- Back from the player stops playback and returns to the browser at the same source and
  scroll position and folder. Back in the browser closes the drawer or a sheet, then leaves
  select mode, then goes up a folder, then goes to the home screen.
- Sources: on a landscape screen 840 dp wide or more, a rail beside the grid that a pick
  leaves open (the menu button hides it). Anywhere else, a 280 dp drawer over a dimmed
  grid, closed by a pick, a tap outside it or Back.
- The grid has about one column per 180 dp of its width. All media and Camera are in date
  sections ("Today", "Yesterday", "October 2026"). Past 200 items a fast-scroll thumb on the
  right shows the section at the top while it is held.
- The chip row under the top bar filters by 3D / 2D and by type. The overflow menu holds
  the sort and "Show unsupported files", as a bottom sheet in portrait and a side sheet in
  landscape, and shows an accent dot when either is not as it starts.
- Downloads shows its sub-folders first, as tiles with a count, then the files directly in
  it. The top bar names the folder ("Downloads / MEDIA / ZTEPics") beside an up arrow.
  Folders ignore the 3D / 2D filter.
- A long press on a tile starts select mode (a cross, the count and a bin in the top bar).
  The bin hands the picked files to `MediaStore.createDeleteRequest`, so Android asks the
  user; a refusal keeps the selection. Android 11+ only: on 10 a long press does nothing.
- Swiping left or right in the player opens the next or previous file in grid order
  (the source, filter and sort the grid showed). The picture follows the finger with the
  neighbour beside it. An image neighbour is decoded ahead, so it becomes the picture at
  once and the one it replaces is kept for a swipe back. A video neighbour shows its
  first frame while it opens. At either end the picture gives a little and slides back.
  No swiping after "Other files..." or under Unsupported.
- Swipe limits: one file each side is prepared; opening a video takes about 0.6 s; a
  two-track video's first frame is flat; Back returns to the grid where it was, not to the
  file swiped to.
- A file that fails to open, from the grid, a swipe or the system picker, returns to the
  grid with a toast: "Couldn't open <name>" and the format when it is known.
- A still zooms: a double tap toggles fit and 2x, a pinch goes up to 4x, one finger pans
  while zoomed. Both views get the same zoom, so the depth is kept. Swiping to the next file
  works at fit only.
- Home mid-video pauses it; the app icon resumes it.
- Permission states the browser shows: not granted, partial access (with "select more"),
  denied, and empty source.
- Text follows the system font size (Configuration.fontScale, clamped to 0.85-1.3 so the
  bars still fit a phone), and a long press that starts select mode gives a haptic tick.
- **Not accessible to TalkBack.** The browser and the player chrome are drawn with ImGui
  into the HUD layer: there are no Android views, so a screen reader sees nothing to
  focus or read. Reaching TalkBack would mean an accessibility node tree mirroring the
  ImGui widgets (`AccessibilityNodeProvider` on the activity's view); not done.
