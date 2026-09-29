// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the live camera source's pure helpers (#90): the tracking-camera
// denylist, the selector grammar, the default pick and the capture-format choice.
// No SDL and no camera. The device lists below are copied from what SDL's Media
// Foundation driver reported on the win dev box (SpatialLabs Eyes + the panel's
// tracking camera + a laptop webcam), so the test pins real-world behaviour.

#include "media/CameraSelect.h"

#include <cstdio>
#include <string>
#include <vector>

using mp::CameraDevice;
using mp::CameraFormat;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

constexpr uint32_t kNV12 = 1, kMJPG = 2, kYUY2 = 3;

CameraFormat Fmt(uint32_t f, int w, int h, int fps) {
    CameraFormat c;
    c.fourccOrSdlFormat = f;
    c.w = w;
    c.h = h;
    c.fpsNum = fps;
    c.fpsDen = 1;
    c.mjpg = (f == kMJPG);
    c.nv12 = (f == kNV12);
    return c;
}

CameraDevice Dev(uint32_t id, const char* name, std::vector<CameraFormat> fmts) {
    CameraDevice d;
    d.id = id;
    d.name = name;
    d.denied = mp::IsDeniedCameraName(d.name, "");
    d.formats = std::move(fmts);
    return d;
}

// SDL enumeration order on the win box: tracker, webcam, Eyes.
std::vector<CameraDevice> BoxDevices() {
    return {
        Dev(11, "SpatialLabs Tracking Camera",
            {Fmt(kMJPG, 1280, 480, 60), Fmt(kNV12, 1280, 480, 60), Fmt(kYUY2, 640, 480, 30)}),
        Dev(12, "HD WebCam",
            {Fmt(kNV12, 1280, 720, 30), Fmt(kMJPG, 1280, 720, 30), Fmt(kYUY2, 640, 480, 30),
             Fmt(kNV12, 640, 360, 30)}),
        // The Eyes' six Media Foundation types (spike §1): the Frame Server's NV12
        // pairs plus the device's own MJPG, at 60 and (twice) 30 fps.
        Dev(13, "SpatialLabs Eyes",
            {Fmt(kNV12, 3840, 2160, 60), Fmt(kMJPG, 3840, 2160, 60), Fmt(kNV12, 3840, 2160, 30),
             Fmt(kMJPG, 3840, 2160, 30), Fmt(kNV12, 3840, 2160, 30), Fmt(kMJPG, 3840, 2160, 30)}),
    };
}

void TestDenylist() {
    std::fprintf(stderr, "denylist\n");
    CHECK(mp::IsDeniedCameraName("SpatialLabs Tracking Camera", ""), "tracker denied");
    CHECK(mp::IsDeniedCameraName("SPATIALLABS TRACKING CAMERA", ""), "upper case");
    CHECK(mp::IsDeniedCameraName("spatiallabs tracking camera", ""), "lower case");
    CHECK(mp::IsDeniedCameraName("Integrated IR Camera", ""), "IR camera denied");
    CHECK(mp::IsDeniedCameraName("Intel RealSense Depth", ""), "depth camera denied");
    CHECK(!mp::IsDeniedCameraName("SpatialLabs Eyes", ""), "Eyes allowed");
    CHECK(!mp::IsDeniedCameraName("HD WebCam", ""), "webcam allowed");
    CHECK(mp::IsDeniedCameraName("Vendor HeadCam", "foo; headcam ;"), "env extends the list");
    CHECK(mp::IsDeniedCameraName("Vendor HeadCam", "HEADCAM"), "env match is case-insensitive");
    CHECK(!mp::IsDeniedCameraName("HD WebCam", ";;  ;"), "empty env entries ignored");
}

void TestSelect() {
    std::fprintf(stderr, "select\n");
    const auto devs = BoxDevices();
    CHECK(devs[0].denied && !devs[1].denied && !devs[2].denied, "enumeration flags");
    CHECK(mp::SelectCamera(devs, "Tracking") == -1, "'Tracking' never selects the tracker");
    CHECK(mp::SelectCamera(devs, "SpatialLabs Tracking Camera") == -1, "exact name refused");
    CHECK(mp::SelectCamera(devs, "spatiallabs") == 2, "'spatiallabs' skips the tracker");
    CHECK(mp::SelectCamera(devs, "eyes") == 2, "'eyes' = the Eyes");
    CHECK(mp::SelectCamera(devs, "webcam") == 1, "'webcam' = HD WebCam");
    CHECK(mp::SelectCamera(devs, "") == 2, "empty = default = Eyes");
    CHECK(mp::SelectCamera(devs, "AUTO") == 2, "auto = default = Eyes");
    CHECK(mp::SelectCamera(devs, "0") == 1, "index 0 skips the denied device");
    CHECK(mp::SelectCamera(devs, "1") == 2, "index 1 = Eyes");
    CHECK(mp::SelectCamera(devs, "2") == -1, "only two selectable devices");
    CHECK(mp::SelectCamera(devs, "nonesuch") == -1, "no match");
    CHECK(mp::SelectCamera({}, "auto") == -1, "empty list");

    const std::vector<CameraDevice> onlyTracker{devs[0]};
    CHECK(mp::DefaultCamera(onlyTracker) == -1, "default never picks a denied device");
    CHECK(mp::SelectCamera(onlyTracker, "0") == -1, "index never reaches a denied device");
    const std::vector<CameraDevice> noEyes{devs[0], devs[1]};
    CHECK(mp::DefaultCamera(noEyes) == 1, "default falls back to the first allowed");
    const std::vector<CameraDevice> capBox{
        devs[1], Dev(20, "USB Capture 4K+", {Fmt(kNV12, 3840, 2160, 30)})};
    CHECK(mp::DefaultCamera(capBox) == 1, "default prefers a >= 3840 px device");
}

void TestFormat() {
    std::fprintf(stderr, "format\n");
    const auto devs = BoxDevices();
    CameraFormat f;
    CHECK(mp::ChooseFormat(devs[2], 0, 0, 0, f), "Eyes has a format");
    CHECK(f.nv12 && f.w == 3840 && f.h == 2160 && mp::FormatFps(f) == 60,
          "Eyes: NV12 4K@60 beats MJPG 4K@60");
    CHECK(mp::ChooseFormat(devs[2], 0, 0, 30, f) && f.nv12 && mp::FormatFps(f) == 30,
          "Eyes @30: NV12 4K@30");
    CHECK(mp::ChooseFormat(devs[2], 1920, 1080, 0, f) && f.w == 3840 && f.nv12,
          "an unoffered size falls back to the largest");

    const CameraDevice mjpgOnly =
        Dev(30, "MJPG cam", {Fmt(kMJPG, 3840, 2160, 30), Fmt(kMJPG, 3840, 2160, 60)});
    CHECK(mp::ChooseFormat(mjpgOnly, 0, 0, 0, f) && f.mjpg && mp::FormatFps(f) == 60,
          "MJPG-only falls back to MJPG@60");
    CHECK(!mp::ChooseFormat(mjpgOnly, 0, 0, 0, f, /*allowMjpg=*/false),
          "MJPG-only without a decoder = no format");

    CHECK(mp::ChooseFormat(devs[1], 0, 0, 0, f) && f.nv12 && f.w == 1280 && f.h == 720,
          "webcam: NV12 720p");
    const CameraDevice yuy2 =
        Dev(31, "YUY2 cam", {Fmt(kMJPG, 1920, 1080, 30), Fmt(kYUY2, 1920, 1080, 30)});
    CHECK(mp::ChooseFormat(yuy2, 0, 0, 0, f) && !f.mjpg && !f.nv12,
          "other uncompressed beats MJPG at a tie");

    const CameraDevice fast =
        Dev(32, "fast cam", {Fmt(kNV12, 1280, 720, 120), Fmt(kNV12, 1280, 720, 60)});
    CHECK(mp::ChooseFormat(fast, 0, 0, 0, f) && mp::FormatFps(f) == 60, "highest <= 60 by default");
    CHECK(mp::ChooseFormat(fast, 0, 0, 120, f) && mp::FormatFps(f) == 120, "explicit 120 honoured");

    CameraFormat ntsc = Fmt(kNV12, 1920, 1080, 30000);
    ntsc.fpsDen = 1001;
    CHECK(mp::FormatFps(ntsc) == 30, "30000/1001 rounds to 30");
}

}  // namespace

int main() {
    TestDenylist();
    TestSelect();
    TestFormat();
    if (g_failures) {
        std::fprintf(stderr, "camera_select_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "camera_select_test: all passed\n");
    return 0;
}
