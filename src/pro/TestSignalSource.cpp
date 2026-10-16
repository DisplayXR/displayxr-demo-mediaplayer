// SPDX-License-Identifier: Apache-2.0
#include "pro/TestSignalSource.h"

#include "Log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "stb_image.h"  // implementation compiled in src/media/ImageDecoder.cpp

namespace mp {

// Front to back: one disc 2% in front of the screen, one on it, two behind.
const float TestSignalSource::kDisparity[TestSignalSource::kDiscs] = {-0.020f, 0.0f, 0.006f,
                                                                      0.012f};

bool TestSignalConfig::ParseSpec(const std::string& specIn) {
    std::string spec = specIn;
    const size_t img = spec.find("image=");
    if (img != std::string::npos) {
        image = spec.substr(img + 6);
        spec = spec.substr(0, img);
        if (!spec.empty() && spec.back() == ',') spec.pop_back();
        if (image.empty()) return false;
    }
    if (spec.empty()) return true;
    int w = 0, h = 0;
    double f = 0.0;
    const size_t at = spec.find('@');
    const std::string size = spec.substr(0, at);
    if (!size.empty()) {
        if (std::sscanf(size.c_str(), "%dx%d", &w, &h) != 2 || w < 64 || h < 64) return false;
        eyeW = w & ~1;
        eyeH = h & ~1;
    }
    if (at != std::string::npos) {
        f = std::atof(spec.c_str() + at + 1);
        if (f < 1.0 || f > 240.0) return false;
        fps = f;
    }
    return true;
}

std::shared_ptr<const TestSignalConfig::Plate> TestSignalConfig::BuildPlate(const TestSignalConfig& cfg,
                                                                            std::string* err) {
    int iw = 0, ih = 0, ch = 0;
    stbi_uc* px = stbi_load(cfg.image.c_str(), &iw, &ih, &ch, 4);
    if (!px || iw < 4 || ih < 2) {
        if (px) stbi_image_free(px);
        if (err) *err = "cannot read " + cfg.image;
        return nullptr;
    }
    auto plate = std::make_shared<Plate>();
    const int ew = cfg.eyeW, eh = cfg.eyeH;
    const int m = std::max(16, (ew / 16) & ~1);
    plate->margin = m;
    const int pw = ew + 2 * m, ph = eh + 2 * m;
    const int sw = iw / 2;  // one eye of the side-by-side still
    // Cover-scale each eye onto the plate (bilinear), as BGRA for BgraToNv12.
    const float scale = std::max((float)pw / sw, (float)ph / ih);
    const float ox = (sw - pw / scale) * 0.5f, oy = (ih - ph / scale) * 0.5f;
    // Right-eye faults: red gain, vertical shift (down), extra crossed parallax (the right
    // eye moves left, so everything comes forward by (depthScale - 1) x 2 % of the width).
    const float dx = (cfg.depthScale - 1.0f) * 0.02f * ew;
    std::vector<uint8_t> bgra((size_t)pw * ph * 4);
    for (int eye = 0; eye < 2; ++eye) {
        const bool right = eye == 1;
        for (int y = 0; y < ph; ++y) {
            const float sy = std::clamp(oy + (y - (right ? cfg.vShiftPx : 0.0f) + 0.5f) / scale - 0.5f, 0.0f, ih - 1.001f);
            const int y0 = (int)sy;
            const float fy = sy - y0;
            for (int x = 0; x < pw; ++x) {
                const float sx = std::clamp(ox + (x + (right ? dx : 0.0f) + 0.5f) / scale - 0.5f, 0.0f, sw - 1.001f);
                const int x0 = (int)sx;
                const float fx = sx - x0;
                float c[3];
                for (int k = 0; k < 3; ++k) {
                    auto at = [&](int xx, int yy) { return (float)px[((size_t)yy * iw + eye * sw + xx) * 4 + k]; };
                    c[k] = (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) +
                           (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
                }
                if (right) c[0] *= cfg.gainR;
                uint8_t* o = &bgra[((size_t)y * pw + x) * 4];
                o[0] = (uint8_t)std::clamp((int)(c[2] + 0.5f), 0, 255);
                o[1] = (uint8_t)std::clamp((int)(c[1] + 0.5f), 0, 255);
                o[2] = (uint8_t)std::clamp((int)(c[0] + 0.5f), 0, 255);
                o[3] = 255;
            }
        }
        livepix::Nv12& dst = right ? plate->right : plate->left;
        dst.Resize(pw, ph);
        livepix::BgraToNv12(bgra.data(), pw * 4, pw, ph, dst, 0);
    }
    stbi_image_free(px);
    return plate;
}

TestSignalSource::~TestSignalSource() { Stop(); }

bool TestSignalSource::Open(const TestSignalConfig& cfgIn) {
    Stop();
    TestSignalConfig cfg = cfgIn;
    if (!cfg.image.empty() && !cfg.plate) {
        std::string err;
        cfg.plate = TestSignalConfig::BuildPlate(cfg, &err);
        if (!cfg.plate) {
            Fail(LiveState::Failed, err);
            return false;
        }
    }
    cfg_ = cfg;
    char n[160];
    if (cfg.plate) {
        const size_t slash = cfg.image.find_last_of("/\\");
        std::snprintf(n, sizeof(n), "Test rig (%s) %dx%d@%g",
                      cfg.image.substr(slash == std::string::npos ? 0 : slash + 1).c_str(), cfg.eyeW, cfg.eyeH, cfg.fps);
    } else {
        std::snprintf(n, sizeof(n), "Test signal %dx%d@%g", cfg.eyeW, cfg.eyeH, cfg.fps);
    }
    name_ = n;
    ResetStats((int)(cfg.fps + 0.5), cfg.fps);
    SetState(LiveState::Opening);
    stop_ = false;
    worker_ = std::thread([this] { Worker(); });
    LOG_INFO("test signal: %s, vshift %.1f px, eye delay %d, right gain %.2f", name_.c_str(),
             cfg.vShiftPx, cfg.eyeDelay, cfg.gainR);
    return true;
}

void TestSignalSource::Stop() {
    if (worker_.joinable()) {
        stop_ = true;
        worker_.join();
    }
    stop_ = false;
    SetState(LiveState::Closed);
}

namespace {

struct Disc {
    float cx, cy, r;  // in eye pixels
    uint8_t y, u, v;
};

// One eye of the scene at time t (seconds) into dst at column x0.
void DrawEye(livepix::Nv12& dst, int x0, int w, int h, double t, float disparityScale,
             float vShift, float gain, bool rightEye) {
    // Background: a vertical gradient (some texture for the alignment measurement comes
    // from the discs and the moving bars below).
    for (int row = 0; row < h; ++row) {
        const int yv = std::clamp((int)((40 + (row * 50) / h) * gain), 16, 235);
        std::memset(dst.y.data() + (size_t)row * dst.width + x0, yv, (size_t)w);
    }
    for (int row = 0; row < h / 2; ++row) {
        uint8_t* uv = dst.uv.data() + (size_t)row * dst.width + x0;
        for (int i = 0; i < w / 2; ++i) {
            uv[2 * i] = 140;  // a little blue
            uv[2 * i + 1] = 122;
        }
    }
    // Thin vertical bars drifting across, on the screen plane (same in both eyes).
    const int period = std::max(32, w / 12);
    const int phase = (int)(t * 120.0) % period;
    for (int row = 0; row < h; ++row) {
        const int ry = (int)(row - vShift);
        if (ry < 0 || ry >= h || (ry % 64) > 40) continue;
        uint8_t* yo = dst.y.data() + (size_t)row * dst.width + x0;
        for (int x = phase; x < w; x += period) yo[x] = (uint8_t)std::clamp((int)(200 * gain), 16, 235);
    }
    const Disc base[TestSignalSource::kDiscs] = {
        {0.30f, 0.55f, 0.14f, 200, 110, 160},  // warm, in front
        {0.55f, 0.40f, 0.12f, 170, 150, 110},
        {0.75f, 0.62f, 0.10f, 150, 100, 120},
        {0.15f, 0.30f, 0.07f, 130, 160, 140},
    };
    for (int d = 0; d < TestSignalSource::kDiscs; ++d) {
        const Disc& b = base[d];
        const float sway = 0.04f * (float)std::sin(t * (0.6 + 0.25 * d) + d);
        const float disp = TestSignalSource::kDisparity[d] * disparityScale * (float)w;
        // Disparity = right x - left x: each eye takes half of it.
        const float cx = (b.cx + sway) * w + (rightEye ? 0.5f : -0.5f) * disp;
        const float cy = b.cy * h + (rightEye ? vShift : 0.0f);
        const float r = b.r * h;
        const int x1 = std::max(0, (int)(cx - r)), x2 = std::min(w - 1, (int)(cx + r));
        const int y1 = std::max(0, (int)(cy - r)), y2 = std::min(h - 1, (int)(cy + r));
        const uint8_t yv = (uint8_t)std::clamp((int)(b.y * gain), 16, 235);
        for (int row = y1; row <= y2; ++row) {
            const float dy = row - cy;
            uint8_t* yo = dst.y.data() + (size_t)row * dst.width + x0;
            for (int x = x1; x <= x2; ++x) {
                const float dx = x - cx;
                const float q = (dx * dx + dy * dy) / (r * r);
                if (q > 1.0f) continue;
                // shaded: brighter towards the top-left, so the disc has inner texture
                const float shade = 1.0f - 0.35f * q - 0.15f * (dx + dy) / r;
                yo[x] = (uint8_t)std::clamp((int)(yv * shade), 16, 235);
            }
            if ((row & 1) == 0) {
                uint8_t* uv = dst.uv.data() + (size_t)(row / 2) * dst.width + x0;
                for (int x = x1 & ~1; x <= x2; x += 2) {
                    const float dx = x - cx;
                    if (dx * dx + dy * dy > r * r) continue;
                    uv[x] = b.u;
                    uv[x + 1] = b.v;
                }
            }
        }
    }
}

int ParseTc(const std::string& s, int fpsInt) {
    int hh = 0, mm = 0, ss = 0, ff = 0;
    if (std::sscanf(s.c_str(), "%d:%d:%d:%d", &hh, &mm, &ss, &ff) != 4) return 0;
    return ((hh * 60 + mm) * 60 + ss) * fpsInt + ff;
}

void FormatTc(int frames, int fpsInt, char* out, size_t n) {
    const int ff = frames % fpsInt, s = frames / fpsInt;
    std::snprintf(out, n, "%02d:%02d:%02d:%02d", (s / 3600) % 24, (s / 60) % 60, s % 60, ff);
}

}  // namespace

// Photo mode: copy one eye's window out of its plate; the window drifts sideways slowly
// (the pan), so a late eye shows a different window.
void PhotoEye(const TestSignalConfig::Plate& p, const livepix::Nv12& src, livepix::Nv12& dst, int x0, int w, int h, double t) {
    const int m = p.margin;
    const int ox = (m + (int)std::lround(0.8 * m * std::sin(t * 0.7))) & ~1;
    const int oy = (m + (int)std::lround(0.3 * m * std::sin(t * 0.45))) & ~1;
    for (int row = 0; row < h; ++row)
        std::memcpy(dst.y.data() + (size_t)row * dst.width + x0, src.y.data() + (size_t)(oy + row) * src.width + ox, (size_t)w);
    for (int row = 0; row < h / 2; ++row)
        std::memcpy(dst.uv.data() + (size_t)row * dst.width + x0,
                    src.uv.data() + (size_t)(oy / 2 + row) * src.width + ox, (size_t)w);
}

void TestSignalSource::Render(const TestSignalConfig& cfg, int64_t n, livepix::Nv12& frame) {
    const int w = cfg.eyeW, h = cfg.eyeH;
    if (frame.width != 2 * w || frame.height != h) frame.Resize(2 * w, h);
    const double t = (double)n / cfg.fps;
    const double tR = (double)std::max<int64_t>(0, n - cfg.eyeDelay) / cfg.fps;
    if (cfg.plate) {
        PhotoEye(*cfg.plate, cfg.plate->left, frame, 0, w, h, t);
        PhotoEye(*cfg.plate, cfg.plate->right, frame, w, w, h, tR);
        return;
    }
    DrawEye(frame, 0, w, h, t, cfg.depthScale, 0.0f, 1.0f, false);
    DrawEye(frame, w, w, h, tR, cfg.depthScale, cfg.vShiftPx, cfg.gainR, true);
}

void TestSignalSource::Worker() {
    using clock = std::chrono::steady_clock;
    const int w = cfg_.eyeW, h = cfg_.eyeH;
    const int fpsInt = std::max(1, (int)std::lround(cfg_.fps));
    const auto period = std::chrono::duration<double>(1.0 / cfg_.fps);
    livepix::Nv12 frame;
    frame.Resize(2 * w, h);
    const int tc0 = ParseTc(cfg_.startTimecode, fpsInt);
    const auto start = clock::now();
    for (int64_t n = 0; !stop_; ++n) {
        std::this_thread::sleep_until(start + std::chrono::duration_cast<clock::duration>(period * (double)n));
        if (stop_) break;
        Render(cfg_, n, frame);
        char tc[16];
        FormatTc(tc0 + (int)n, fpsInt, tc, sizeof(tc));
        Publish(frame, /*fullRange=*/false, NowSeconds(), tc, cfg_.eyeDelay);
    }
}

}  // namespace mp
