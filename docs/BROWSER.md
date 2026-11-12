# Media browser

The in-app gallery (#160): pick a photo or video from the device's library without
leaving the 3D session, with 3D badges, durations and filters. Android comes first. The
browser code is platform-neutral, so a desktop browser can follow by adding a provider.

This is the developer reference.

## Scope

In: images and videos from the Android MediaStore, a sidebar of sources (All media,
Camera, Downloads, Pictures, Movies, Unsupported, "Other files..." for the system picker),
a square-tile grid, 3D and duration badges, Downloads as folders, select and delete, and
view options (Both / 3D / 2D, All / Videos / Images, newest or oldest first, show
unsupported files, off by default).

Out for now: a desktop browser; the VR badge; HEIC and MV-HEVC on Android. Files
the player cannot open are listed under Unsupported with their format, so the formats
left to do stay findable. MediaStore does not index MPO, JPS or JFIF as media, so the
gallery cannot list them; "Other files..." reaches them.

## Structure

```
src/browser/                    namespace mp::browser, platform-neutral
  MediaItem.h                   one media entry: id, name, kind, size, duration, date, folder
  MediaLibrary.{h,cpp}          the item list, sources, filter and sort
  MediaProvider.h               the interface a platform implements: permission, refresh,
                                thumbnails, open, delete
  ThumbnailAtlas.{h,cpp}        GPU: thumbnail slots packed into a few textures
  ThumbnailCache.{h,cpp}        media id -> atlas slot, least recently used reuse
  StereoClassifier.{h,cpp}      3D badge verdicts: background thread, cached on disk
  StereoGrid.{h,cpp}            the 3D grid: eye crops, per-tile convergence, per-view quads,
                                the rendering-mode borrow
  ThumbnailDepth.{h,cpp}        the 3D grid: each stereo thumbnail's depth, on a worker
  TouchScroller.{h,cpp}         drag, inertia, tap vs drag
  SlideGesture.{h,cpp}          swiping to the next or previous file in the player
  MediaBrowser.{h,cpp}          the ImGui view: sidebar, top bar, grid, view options, empty states
  Icons.{h,cpp}                 hand-drawn icons

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
  touch_scroller_test.cpp, slide_gesture_test.cpp, stereo_grid_test.cpp
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
- Every fixed UI size goes through `ui::Px()`.

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

## 3D grid

Under the "3D only" filter the panel goes to its 3D mode and every tile is stereo: its left
eye in the left-eye views, its right eye in the others. Android only.

- No weave extension: the tiles are drawn into the app's own projection atlas, view by view,
  at the rects the browser laid out that frame (`MediaBrowser::Stereo()`), by
  `StereoGridRenderer`, which replaces the atlas pass for that frame. The browser lays out
  first, so a scrolling tile is never drawn a frame behind the HUD.
- The ImGui HUD stays the flat chrome (a window-space layer at disparity 0). It is
  transparent over the grid: the bars, the sidebar, folders, badges and messages are flat,
  the tiles' pictures are in the projection layer.
- The thumbnails already hold both eyes of a side-by-side or top-bottom file. The pass
  samples the `ThumbnailAtlas` pages through their ImGui descriptor sets: one bind and one
  instanced draw per page and view. A LIF or two-track video's thumbnail is one view, so
  that tile is flat (the same crop in every view). Folders are flat.
- Depth per tile (`StereoGrid.h` `TileShift`): `ThumbnailDepth` measures each stereo
  thumbnail once with the player's auto-convergence estimator. The subject goes 0.25 % of
  the tile behind the plane, and further back (up to 1 % for the subject) until the
  nearest content is at the plane. Unmeasured tiles go 0.5 % back. The two crops move
  apart inside their eyes, zooming in a little when there is no room. Every tile fades to
  the flat background over a 3 dp border, so a border never cuts something in front of the
  plane. The gutter widens from 4 to 12 dp.
- The "3D" badge is hidden (every tile is 3D) and the duration sits on an opaque pill.
- Modes (`ModeBorrow`): the browser borrows flat, or the 3D mode under the filter, and
  hands back to the player the mode it had. Only the filter decides, never what the grid
  happens to show, so a scroll never flips modes. Back from the player into a 3D-filtered
  grid goes back to 3D. Until the panel is in 3D the grid is drawn flat as before.
- Resolution: a tile's useful size per eye is its rect times the mode's view scale (half
  the width on a side-by-side panel). The 512 px thumbnails cover that on a phone.

## Android behaviour

- Launch opens the browser, over the idle splash. Without a HUD there is no browser and a
  tap on the splash opens the system picker.
- The browser borrows the flat 2D mode (the 3D mode under "3D only", see 3D grid) and hands
  the player's mode back on play.
- Back from the player stops playback and returns to the browser at the same source and
  scroll position and folder. Back in the browser leaves select mode, then goes up a folder,
  then to the home screen.
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
  two-track video's first frame is flat; a file that fails to open leaves black until
  Back; Back returns to the grid where it was, not to the file swiped to.
- Home mid-video pauses it; the app icon resumes it.
- Permission states the browser shows: not granted, partial access (with "select more"),
  denied, and empty source.
