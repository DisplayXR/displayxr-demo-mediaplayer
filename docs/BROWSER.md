# Media browser

The in-app gallery (#160): pick a photo or video from the device's library without
leaving the 3D session, with 3D badges, durations and filters. Android comes first. The
browser code is platform-neutral, so a desktop browser can follow by adding a provider.

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
  TouchScroller.{h,cpp}         drag, inertia, tap vs drag
  SlideGesture.{h,cpp}          swiping to the next or previous file in the player
  BrowserLayout.{h,cpp}         pure layout: columns by width, rail or drawer, date sections,
                                the sectioned grid's positions
  MediaBrowser.{h,cpp}          the ImGui view: top bar, chips, rail / drawer, grid, view
                                options, empty states

android/src/main/cpp/browser/
  AndroidMediaProvider.{h,cpp}  MediaProvider over JNI, plus the swipe's stills; its JNI
                                exports live here

android/src/main/cpp/           the player side of the swipe
  neighbour_stills.{h,cpp}      the files either side of the one playing, ready on the GPU
  still_image.{h,cpp}           an image decoded as the player shows it, for opening and stills

android/src/main/java/.../
  MediaStoreProvider.kt         MediaStore query, permissions, thumbnails, stills, opening a file

tests/
  media_library_test.cpp, stereo_classifier_test.cpp, thumbnail_cache_test.cpp,
  tile_crop_test.cpp, letterbox_test.cpp (src/media/Letterbox),
  touch_scroller_test.cpp, slide_gesture_test.cpp, browser_layout_test.cpp,
  zoom_gesture_test.cpp
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

The "3D" badge sits at the tile's top left (right of the box in select mode). The top
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

## Android behaviour

- Launch opens the browser, over the idle splash. Without a HUD there is no browser and a
  tap on the splash opens the system picker.
- The browser borrows the flat 2D mode and hands it back on play.
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
