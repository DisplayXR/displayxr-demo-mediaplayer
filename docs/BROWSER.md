# Media browser

The in-app gallery (#160): pick a photo or video from the device's library without
leaving the 3D session, with 3D badges, durations and filters. Android comes first. The
browser code is platform-neutral, so a desktop browser can follow by adding a provider.

This is the developer reference.

## Scope

In: images and videos from the Android MediaStore, a sidebar of sources (All media,
Camera, Downloads, Pictures, Movies, Unsupported, "Other files..." for the system picker),
a square-tile grid, 3D and duration badges, and view options (Both / 3D / 2D, All /
Videos / Images, newest or oldest first).

Out for now: a desktop browser; the VR badge; MPO, HEIC and MV-HEVC on Android. Files
the player cannot open are listed under Unsupported with their format, so the formats
left to do stay visible. MediaStore does not index MPO, JPS or JFIF as media, so the
gallery cannot list them; "Other files..." reaches them.

## Structure

```
src/browser/                    namespace mp::browser, platform-neutral
  MediaItem.h                   one media entry: id, name, kind, size, duration, date, folder
  MediaLibrary.{h,cpp}          the item list, sources, filter and sort
  MediaProvider.h               the interface a platform implements: permission, refresh,
                                thumbnails, open
  ThumbnailAtlas.{h,cpp}        GPU: thumbnail slots packed into a few textures
  ThumbnailCache.{h,cpp}        media id -> atlas slot, least recently used reuse
  StereoClassifier.{h,cpp}      3D badge verdicts: background thread, cached on disk
  TouchScroller.{h,cpp}         drag, inertia, tap vs drag
  MediaBrowser.{h,cpp}          the ImGui view: sidebar, top bar, grid, view options, empty states
  Icons.{h,cpp}                 hand-drawn icons

android/src/main/cpp/browser/
  AndroidMediaProvider.{h,cpp}  MediaProvider over JNI; its JNI exports live here

android/src/main/java/.../
  MediaStoreProvider.kt         MediaStore query, permissions, thumbnails, opening a file

tests/
  media_library_test.cpp, stereo_classifier_test.cpp, thumbnail_cache_test.cpp,
  touch_scroller_test.cpp
```

## Rules

- Browser code lives in the folders above. `src/browser/` has no JNI, no MediaStore and
  no media paths: it reaches the device's media only through `MediaProvider`.
- Existing classes get wiring only (Android `main.cpp`, `MainActivity.kt`, the build
  lists). Extend one only when the capability belongs to it: the MPO switch in
  `MediaSource` (like its HEIC switch) and an fd variant of the LIF check in `LifLoader`.
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
  own worker thread.
- `MediaStoreProvider` runs on the Android UI thread and a thumbnail executor.
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

## Android behaviour

- Launch shows the splash. Tapping it, or the Load button, opens the browser.
- The browser borrows the flat 2D mode and hands it back on play.
- Back from the player stops playback and returns to the browser at the same source and
  scroll position. Back from the browser goes to the home screen.
- Home mid-video pauses it; the app icon resumes it.
- Permission states the browser shows: not granted, partial access (with "select more"),
  denied, and empty source.
