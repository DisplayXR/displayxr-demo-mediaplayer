// SPDX-License-Identifier: Apache-2.0
#include "pro/Scopes.h"

#include "pro/Thumbnail.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace mp::pro::scopes {

namespace {

float SrgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float LinearToSrgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }
float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

// Trilinear lookup on the LUT grid: what a linear-filtered sampler3D does with the
// shader's texel-centre mapping (t * (N-1)/N + 0.5/N).
void SampleLut(const color::CubeLut& lut, float rgb[3]) {
    const int n = lut.size;
    float t[3];
    int i0[3], i1[3];
    float f[3];
    for (int k = 0; k < 3; ++k) {
        const float span = std::max(1e-6f, lut.domainMax[0] - lut.domainMin[0]);
        t[k] = Clamp01((rgb[k] - lut.domainMin[0]) / span) * (float)(n - 1);
        i0[k] = std::min(n - 1, (int)t[k]);
        i1[k] = std::min(n - 1, i0[k] + 1);
        f[k] = t[k] - (float)i0[k];
    }
    auto at = [&](int r, int g, int b, int c) { return lut.rgb[(((size_t)b * n + g) * n + r) * 3 + c]; };
    for (int c = 0; c < 3; ++c) {
        const float c00 = at(i0[0], i0[1], i0[2], c) * (1 - f[0]) + at(i1[0], i0[1], i0[2], c) * f[0];
        const float c10 = at(i0[0], i1[1], i0[2], c) * (1 - f[0]) + at(i1[0], i1[1], i0[2], c) * f[0];
        const float c01 = at(i0[0], i0[1], i1[2], c) * (1 - f[0]) + at(i1[0], i0[1], i1[2], c) * f[0];
        const float c11 = at(i0[0], i1[1], i1[2], c) * (1 - f[0]) + at(i1[0], i1[1], i1[2], c) * f[0];
        const float c0 = c00 * (1 - f[1]) + c10 * f[1], c1 = c01 * (1 - f[1]) + c11 * f[1];
        rgb[c] = c0 * (1 - f[2]) + c1 * f[2];
    }
}

// Display aspect of one eye (a half-SBS eye is shown unsqueezed), as MakeThumbnail.
double EyeAspect(StereoLayout layout, int w, int h) {
    const bool sbs = layout == StereoLayout::SbsFull || layout == StereoLayout::SbsHalf;
    const int eyeW = sbs ? w / 2 : w;
    return (layout == StereoLayout::SbsHalf ? (double)w : (double)eyeW) / std::max(1, h);
}

// The analysis size: `analysisW` wide, the eye's display aspect, even height (the gathered
// frame's chroma is half-size, so the gather and the box filter agree exactly).
void AnalysisSize(double aspect, int analysisW, int& w, int& h) {
    w = std::max(16, analysisW) & ~1;
    h = std::max(10, (int)std::lround(w / std::max(0.1, aspect))) & ~1;
}

void Analyze(const std::vector<uint8_t>& rgba, int w, int h, const Grade& g, Result& out) {
    out.w = w;
    out.h = h;
    out.graded.assign((size_t)w * h * 3, 0);
    out.waveY.assign((size_t)w * kLevels, 0);
    out.waveR.assign((size_t)w * kLevels, 0);
    out.waveG.assign((size_t)w * kLevels, 0);
    out.waveB.assign((size_t)w * kLevels, 0);
    out.vector.assign((size_t)kVecSize * kVecSize, 0);
    std::fill(std::begin(out.histR), std::end(out.histR), 0u);
    std::fill(std::begin(out.histG), std::end(out.histG), 0u);
    std::fill(std::begin(out.histB), std::end(out.histB), 0u);
    std::fill(std::begin(out.histY), std::end(out.histY), 0u);
    float minY = 1.0f, maxY = 0.0f;
    double sumY = 0.0;
    auto lvl = [](float v) { return std::clamp((int)std::lround(v * (kLevels - 1)), 0, kLevels - 1); };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = &rgba[((size_t)y * w + x) * 4];
            float c[3] = {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f};
            ApplyGrade(g, c);
            const float Y = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
            uint8_t* o = &out.graded[((size_t)y * w + x) * 3];
            for (int k = 0; k < 3; ++k) o[k] = (uint8_t)std::lround(Clamp01(c[k]) * 255.0f);
            const size_t col = (size_t)x * kLevels;
            ++out.waveY[col + lvl(Y)];
            ++out.waveR[col + lvl(c[0])];
            ++out.waveG[col + lvl(c[1])];
            ++out.waveB[col + lvl(c[2])];
            ++out.histY[lvl(Y)];
            ++out.histR[lvl(c[0])];
            ++out.histG[lvl(c[1])];
            ++out.histB[lvl(c[2])];
            // BT.709 colour difference, -0.5..0.5; Cr up, Cb right (the broadcast layout).
            const float cb = (c[2] - Y) / 1.8556f, cr = (c[0] - Y) / 1.5748f;
            const int vx = std::clamp((int)((cb + 0.5f) * kVecSize), 0, kVecSize - 1);
            const int vy = std::clamp((int)((0.5f - cr) * kVecSize), 0, kVecSize - 1);
            ++out.vector[(size_t)vy * kVecSize + vx];
            minY = std::min(minY, Y);
            maxY = std::max(maxY, Y);
            sumY += Y;
        }
    }
    out.minY = minY;
    out.maxY = maxY;
    out.meanY = (float)(sumY / std::max(1, w * h));
    out.ok = true;
}

// A phosphor-like brightness for a bin count: log scale, so a few pixels still show.
uint8_t Glow(uint32_t count, float logNorm) {
    if (count == 0) return 0;
    const float v = std::log1p((float)count) * logNorm;
    return (uint8_t)std::clamp((int)(40.0f + 215.0f * v), 0, 255);
}

}  // namespace

void ApplyGrade(const Grade& g, float c[3]) {
    if (g.exposure != 0.0f) {
        const float m = std::exp2(g.exposure);
        for (int k = 0; k < 3; ++k) c[k] = LinearToSrgb(Clamp01(SrgbToLinear(Clamp01(c[k])) * m));
    }
    if (g.cdlOn) {
        for (int k = 0; k < 3; ++k) c[k] = std::pow(Clamp01(c[k] * g.slope[k] + g.offset[k]), g.power[k]);
        const float luma = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
        for (int k = 0; k < 3; ++k) c[k] = Clamp01(luma + g.saturation * (c[k] - luma));
    }
    if (g.lut && g.lut->Valid()) SampleLut(*g.lut, c);
}

bool Downsample(const FrameRing::Frame& f, StereoLayout layout, int analysisW, std::vector<uint8_t>& rgba,
                int& w, int& h) {
    const double aspect = EyeAspect(layout, f.width, f.height);
    AnalysisSize(aspect, analysisW, w, h);
    return MakeThumbnail(f, layout, w, h, rgba);
}

bool DownsampleRgba(const uint8_t* src, int sw, int sh, StereoLayout layout, int analysisW,
                    std::vector<uint8_t>& rgba, int& w, int& h) {
    if (!src || sw < 2 || sh < 2) return false;
    const bool sbs = layout == StereoLayout::SbsFull || layout == StereoLayout::SbsHalf;
    const int eyeW = sbs ? sw / 2 : sw;
    const double aspect = EyeAspect(layout, sw, sh);
    AnalysisSize(aspect, analysisW, w, h);
    rgba.assign((size_t)w * h * 4, 255);
    // Box filter, 3x3 taps per output pixel (as MakeThumbnail).
    const double stepX = (double)eyeW / w, stepY = (double)sh / h;
    for (int oy = 0; oy < h; ++oy)
        for (int ox = 0; ox < w; ++ox) {
            float acc[3] = {0, 0, 0};
            for (int ty = 0; ty < 3; ++ty) {
                const int y = std::min(sh - 1, (int)((oy + (ty + 0.5) / 3) * stepY));
                for (int tx = 0; tx < 3; ++tx) {
                    const int x = std::min(eyeW - 1, (int)((ox + (tx + 0.5) / 3) * stepX));
                    const uint8_t* p = src + ((size_t)y * sw + x) * 4;
                    for (int k = 0; k < 3; ++k) acc[k] += p[k];
                }
            }
            uint8_t* o = &rgba[((size_t)oy * w + ox) * 4];
            for (int k = 0; k < 3; ++k) o[k] = (uint8_t)std::lround(acc[k] / 9.0f);
        }
    return true;
}

void Measure(const std::vector<uint8_t>& rgba, int w, int h, const Grade& g, Result& out) {
    const auto t0 = std::chrono::steady_clock::now();
    if (w < 1 || h < 1 || rgba.size() < (size_t)w * h * 4) {
        out.ok = false;
        return;
    }
    Analyze(rgba, w, h, g, out);
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

bool Gather(const FrameRing::Frame& f, StereoLayout layout, int analysisW, FrameRing::Frame& mini, int& w, int& h) {
    if (f.gpu || f.width < 2 || f.height < 2 || f.plane[0].empty()) return false;
    const double aspect = EyeAspect(layout, f.width, f.height);
    AnalysisSize(aspect, analysisW, w, h);
    const bool sbs = layout == StereoLayout::SbsFull || layout == StereoLayout::SbsHalf;
    const int eyeW = sbs ? f.width / 2 : f.width, H = f.height;
    const int mw = w * kGatherScale, mh = h * kGatherScale;  // even: w and h are
    const bool rgba = f.format == PixFormat::RGBA, p16 = f.format == PixFormat::P016;
    const size_t bY = rgba ? 4 : p16 ? 2 : 1;
    if (f.plane[0].size() < (size_t)f.width * H * bY) return false;
    const size_t cw = (size_t)(f.width / 2), ch = (size_t)(H / 2);
    if (f.format == PixFormat::NV12 && f.plane[1].size() < cw * ch * 2) return false;
    if (p16 && f.plane[1].size() < cw * ch * 4) return false;
    if (f.format == PixFormat::I420 && (f.plane[1].size() < cw * ch || f.plane[2].size() < cw * ch)) return false;

    mini.width = mw;
    mini.height = mh;
    mini.format = f.format;
    mini.fullRange = f.fullRange;
    mini.matrix = f.matrix;
    mini.transfer = f.transfer;
    mini.gpu = false;
    mini.serial = f.serial;
    // Source column / row of each mini sample (pixel centres).
    std::vector<int> sx(mw), sy(mh);
    for (int x = 0; x < mw; ++x) sx[x] = std::min(eyeW - 1, (int)((x + 0.5) * eyeW / mw));
    for (int y = 0; y < mh; ++y) sy[y] = std::min(H - 1, (int)((y + 0.5) * H / mh));
    mini.plane[0].resize((size_t)mw * mh * bY);
    for (int y = 0; y < mh; ++y) {
        const uint8_t* row = f.plane[0].data() + (size_t)sy[y] * f.width * bY;
        uint8_t* o = mini.plane[0].data() + (size_t)y * mw * bY;
        for (int x = 0; x < mw; ++x) std::memcpy(o + (size_t)x * bY, row + (size_t)sx[x] * bY, bY);
    }
    if (rgba) {
        mini.plane[1].clear();
        mini.plane[2].clear();
        return true;
    }
    // Chroma: one sample per 2x2 mini pixels, from the source chroma under it.
    const int mcw = mw / 2, mch = mh / 2;
    const size_t bC = f.format == PixFormat::NV12 ? 2 : p16 ? 4 : 1;
    mini.plane[1].resize((size_t)mcw * mch * bC);
    if (f.format == PixFormat::I420) mini.plane[2].resize((size_t)mcw * mch);
    else mini.plane[2].clear();
    for (int y = 0; y < mch; ++y) {
        const size_t srow = (size_t)(sy[y * 2] / 2) * cw;
        for (int x = 0; x < mcw; ++x) {
            const size_t si = srow + (size_t)(sx[x * 2] / 2);
            std::memcpy(mini.plane[1].data() + ((size_t)y * mcw + x) * bC, f.plane[1].data() + si * bC, bC);
            if (f.format == PixFormat::I420) mini.plane[2][(size_t)y * mcw + x] = f.plane[2][si];
        }
    }
    return true;
}

bool DownsampleMini(const FrameRing::Frame& mini, int w, int h, std::vector<uint8_t>& rgba) {
    return MakeThumbnail(mini, StereoLayout::Mono, w, h, rgba);  // mini has the eye's display aspect
}

// --- ScopeWorker ---------------------------------------------------------------

ScopeWorker::ScopeWorker() : thread_([this] { Run(); }) {}

ScopeWorker::~ScopeWorker() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
}

void ScopeWorker::Submit(Job&& job) {
    {
        std::lock_guard<std::mutex> lk(m_);
        job_ = std::move(job);
        pending_ = true;
    }
    cv_.notify_one();
}

bool ScopeWorker::Take(Result& r, std::vector<uint8_t>& wave, std::vector<uint8_t>& vec) {
    std::lock_guard<std::mutex> lk(m_);
    if (!ready_) return false;
    std::swap(r, result_);
    std::swap(wave, wave_);
    std::swap(vec, vec_);
    ready_ = false;
    return true;
}

double ScopeWorker::LastMs() const {
    std::lock_guard<std::mutex> lk(m_);
    return lastMs_;
}

void ScopeWorker::Run() {
    std::unique_lock<std::mutex> lk(m_);
    while (true) {
        cv_.wait(lk, [this] { return stop_ || pending_; });
        if (stop_) return;
        Job job = std::move(job_);
        pending_ = false;
        lk.unlock();
        const auto t0 = std::chrono::steady_clock::now();
        if (job.hasImage) {
            image_ = std::move(job.image);
            imageW_ = job.w;
            imageH_ = job.h;
        } else if (job.hasFrame) {
            if (DownsampleMini(job.mini, job.w, job.h, image_)) {
                imageW_ = job.w;
                imageH_ = job.h;
            }
        }
        Result r;
        std::vector<uint8_t> wave, vec;
        bool ok = false;
        if (!image_.empty() && imageW_ > 0) {
            Measure(image_, imageW_, imageH_, job.grade, r);
            if (r.ok && job.pictures) {
                RenderWaveform(r, job.parade, job.pictureW, wave);
                RenderVectorscope(r, vec);
            }
            ok = r.ok;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        lk.lock();
        if (ok) {
            result_ = std::move(r);
            wave_ = std::move(wave);
            vec_ = std::move(vec);
            ready_ = true;
            lastMs_ = ms;
        }
    }
}

void RenderWaveform(const Result& r, bool parade, int outW, std::vector<uint8_t>& rgba) {
    rgba.assign((size_t)outW * kLevels * 4, 0);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    if (!r.ok || r.w < 1) return;
    // Normalise so a column of identical pixels is full brightness.
    const float logNorm = 1.0f / std::log1p((float)std::max(1, r.h));
    auto put = [&](int x, int level, const std::vector<uint32_t>& wave, int srcX, int ch) {
        const uint32_t n = wave[(size_t)srcX * kLevels + level];
        if (!n) return;
        uint8_t* p = &rgba[((size_t)(kLevels - 1 - level) * outW + x) * 4];
        const uint8_t v = Glow(n, logNorm);
        if (ch < 0) {  // luma: green phosphor
            p[0] = (uint8_t)std::max<int>(p[0], v / 4);
            p[1] = std::max(p[1], v);
            p[2] = (uint8_t)std::max<int>(p[2], v / 3);
        } else {
            p[ch] = std::max(p[ch], v);
        }
    };
    if (!parade) {
        for (int x = 0; x < outW; ++x) {
            const int sx = std::min(r.w - 1, x * r.w / outW);
            for (int l = 0; l < kLevels; ++l) put(x, l, r.waveY, sx, -1);
        }
        return;
    }
    const int third = outW / 3;
    const std::vector<uint32_t>* waves[3] = {&r.waveR, &r.waveG, &r.waveB};
    for (int c = 0; c < 3; ++c)
        for (int x = 0; x < third; ++x) {
            const int sx = std::min(r.w - 1, x * r.w / std::max(1, third));
            for (int l = 0; l < kLevels; ++l) put(c * third + x, l, *waves[c], sx, c);
        }
}

void RenderVectorscope(const Result& r, std::vector<uint8_t>& rgba) {
    rgba.assign((size_t)kVecSize * kVecSize * 4, 0);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    if (!r.ok) return;
    const float logNorm = 1.0f / std::log1p((float)std::max(1, r.w * r.h / 16));
    for (int y = 0; y < kVecSize; ++y)
        for (int x = 0; x < kVecSize; ++x) {
            const uint8_t v = Glow(r.vector[(size_t)y * kVecSize + x], logNorm);
            uint8_t* p = &rgba[((size_t)y * kVecSize + x) * 4];
            p[0] = (uint8_t)(v / 4);
            p[1] = v;
            p[2] = (uint8_t)(v / 3);
        }
}

}  // namespace mp::pro::scopes
