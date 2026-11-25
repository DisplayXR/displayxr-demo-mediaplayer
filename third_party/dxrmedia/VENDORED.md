# dxrmedia (vendored)

The reference reader and writer of the **DisplayXR stereo media format, version 1**:
what DisplayXR Camera writes (an MPF stereo `.jpg` with `dxr:` XMP, a side-by-side HEVC
`*_sbs.mp4` with an XMP `uuid` box, `st3d` and a timed-metadata track). The player uses
the **reader** half (`read_still`, `xmp_parse`, `mp4_probe`, `decode_frame_meta`) through
`src/media/DxrStereo.{h,cpp}`; the writer half is used only by `tests/dxr_stereo_test.cpp`
to make test files exactly as the camera does.

| | |
|---|---|
| Source repo | `DisplayXR/displayxr-camera`, `libs/dxrmedia/` |
| Source commit | `19be8443d5436f3dc4af1d5624770ea85eacf3e5` (branch `feat/capture`; last change to `libs/dxrmedia` in `a71b495b9ca779cb375de9e8d3a4b8f7ec357146`) |
| Contract | that repo's `docs/FORMAT.md` (version 1) |
| Licence | Apache-2.0 (`LICENSE`, the source repo's) |

## What is here

- `include/`, `src/` — copied **unmodified**. C++17, no platform dependencies, no pixel
  codec. Built as the static library `dxrmedia` (root `CMakeLists.txt` and
  `android/src/main/cpp/CMakeLists.txt`).
- `testing/mp4_builder.h` — the source repo's test helper (`libs/dxrmedia/tests/`), which
  builds small MP4s with a video track and a `mett` metadata track. Test-only.

## Updating

Copy `libs/dxrmedia/include` and `libs/dxrmedia/src` from the camera repo over these
directories (and `tests/mp4_builder.h` into `testing/`), update the commit above, and run
`dxr_stereo_test`. Do not edit the files here: a fix belongs in the camera repo first.
