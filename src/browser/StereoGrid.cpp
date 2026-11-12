// SPDX-License-Identifier: Apache-2.0
#include "browser/StereoGrid.h"

#include <algorithm>
#include <cmath>

namespace mp::browser {

void EyeRegions(StereoLayout layout, UvRect& left, UvRect& right) {
    left = UvRect{};
    right = UvRect{};
    switch (layout) {
        case StereoLayout::SbsFull:
        case StereoLayout::SbsHalf:
            left.u1 = 0.5f;
            right.u0 = 0.5f;
            break;
        case StereoLayout::TbFull:
        case StereoLayout::TbHalf:
            left.v1 = 0.5f;
            right.v0 = 0.5f;
            break;
        default:
            break;
    }
}

bool Packed(StereoLayout layout) {
    return layout == StereoLayout::SbsFull || layout == StereoLayout::SbsHalf || layout == StereoLayout::TbFull ||
           layout == StereoLayout::TbHalf;
}

float EyeAspect(StereoLayout layout, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return 1.0f;
    const float aspect = (float)w / (float)h;
    switch (layout) {
        case StereoLayout::SbsFull: return aspect * 0.5f;
        case StereoLayout::TbFull: return aspect * 2.0f;
        default: return aspect;  // a half packing is shown at the whole frame's aspect
    }
}

UvRect CenterCrop(const UvRect& eye, float eyeAspect, float tileAspect) {
    UvRect r = eye;
    if (eyeAspect <= 0.0f || tileAspect <= 0.0f) return r;
    const float ratio = eyeAspect / tileAspect;  // > 1: the eye is wider than the tile
    if (ratio > 1.0f) {
        const float mid = (eye.u0 + eye.u1) * 0.5f, half = eye.W() * 0.5f / ratio;
        r.u0 = mid - half;
        r.u1 = mid + half;
    } else {
        const float mid = (eye.v0 + eye.v1) * 0.5f, half = eye.H() * 0.5f * ratio;
        r.v0 = mid - half;
        r.v1 = mid + half;
    }
    return r;
}

float TileShift(const DepthEstimate& e, float cropFrac, const DepthPolicy& policy) {
    if (!(cropFrac > 0.0f)) cropFrac = 1.0f;
    float s = policy.unmeasured;
    if (e.ok) {
        // In tile widths, crossed (+ = in front). A shift s leaves the subject at s - subject
        // behind the plane.
        const float subject = e.subject / cropFrac;
        const float nearest = std::max(e.nearest, e.subject) / cropFrac;
        s = subject + policy.subjectBehind;
        // The nearest content at the plane, as long as the subject stays within maxBehind.
        if (nearest > s) s = std::max(s, std::min(nearest, subject + policy.maxBehind));
    }
    return std::max(-policy.maxShift, std::min(policy.maxShift, s));
}

bool ApplyShift(UvRect& left, UvRect& right, const UvRect& leftEye, const UvRect& rightEye, float shift) {
    if (!(std::fabs(shift) < 0.5f)) return false;
    const float hw = std::min(left.W(), right.W()) * 0.5f;
    if (!(hw > 0.0f)) return false;
    // A crop of half-width h moved by shift * h (its own width times shift / 2): the left
    // crop's centre moves by +, the right's by -. Largest h that keeps both inside.
    float h = hw;
    auto fit = [&](const UvRect& crop, const UvRect& eye, float dir) {
        const float c = (crop.u0 + crop.u1) * 0.5f;
        const float s = dir * shift;
        h = std::min(h, (eye.u1 - c) / (1.0f + s));
        h = std::min(h, (c - eye.u0) / (1.0f - s));
    };
    fit(left, leftEye, 1.0f);
    fit(right, rightEye, -1.0f);
    if (!(h > hw * 0.5f)) return false;  // more than a 2x zoom: leave the tile unshifted
    const float scale = h / hw;
    auto place = [&](UvRect& crop, float dir) {
        const float cu = (crop.u0 + crop.u1) * 0.5f + dir * shift * h;
        const float cv = (crop.v0 + crop.v1) * 0.5f, hv = crop.H() * 0.5f * scale;
        crop.u0 = cu - h;
        crop.u1 = cu + h;
        crop.v0 = cv - hv;
        crop.v1 = cv + hv;
    };
    place(left, 1.0f);
    place(right, -1.0f);
    return true;
}

bool TileCrops(StereoLayout layout, uint32_t w, uint32_t h, float tileAspect, const DepthEstimate* e,
               const DepthPolicy& policy, UvRect& left, UvRect& right) {
    UvRect eyeL, eyeR;
    EyeRegions(layout, eyeL, eyeR);
    const float aspect = EyeAspect(layout, w, h);
    left = CenterCrop(eyeL, aspect, tileAspect);
    if (!Packed(layout)) {
        right = left;
        return false;
    }
    right = CenterCrop(eyeR, aspect, tileAspect);
    const float cropFrac = eyeL.W() > 0.0f ? left.W() / eyeL.W() : 1.0f;
    ApplyShift(left, right, eyeL, eyeR, TileShift(e ? *e : DepthEstimate{}, cropFrac, policy));
    return true;
}

UvRect InSlot(const UvRect& r, float slotU0, float slotV0, float slotU1, float slotV1) {
    const float su = slotU1 - slotU0, sv = slotV1 - slotV0;
    return UvRect{slotU0 + su * r.u0, slotV0 + sv * r.v0, slotU0 + su * r.u1, slotV0 + sv * r.v1};
}

bool ViewQuad(float x0, float y0, float x1, float y1, float hudW, float hudH, UvRect& out) {
    if (!(hudW > 0.0f) || !(hudH > 0.0f) || !(x1 > x0) || !(y1 > y0)) return false;
    out = UvRect{x0 / hudW, y0 / hudH, x1 / hudW, y1 / hudH};
    return true;
}

bool ViewScissor(const float clip[4], float hudW, float hudH, uint32_t tx, uint32_t ty, uint32_t renderW,
                 uint32_t renderH, int32_t& x, int32_t& y, uint32_t& w, uint32_t& h) {
    if (!(hudW > 0.0f) || !(hudH > 0.0f) || renderW == 0 || renderH == 0) return false;
    const float sx = (float)renderW / hudW, sy = (float)renderH / hudH;
    auto edge = [](float v, float scale, uint32_t size) {
        return (int32_t)std::min((float)size, std::max(0.0f, std::floor(v * scale + 0.5f)));
    };
    const int32_t a = edge(clip[0], sx, renderW), b = edge(clip[1], sy, renderH);
    const int32_t c = edge(clip[2], sx, renderW), d = edge(clip[3], sy, renderH);
    if (c <= a || d <= b) return false;
    x = (int32_t)(tx * renderW) + a;
    y = (int32_t)(ty * renderH) + b;
    w = (uint32_t)(c - a);
    h = (uint32_t)(d - b);
    return true;
}

bool RightEye(uint32_t view, uint32_t cols) { return cols > 1 ? (view % cols) != 0 : view != 0; }

int32_t BrowserMode(bool only3D, int32_t flat, int32_t stereo) {
    if (only3D && stereo >= 0) return stereo;
    return flat;
}

int32_t ModeBorrow::Borrow(int32_t want, int32_t active) {
    if (want < 0 || active < 0) return -1;
    if (active != lastActive_) {  // the runtime changed mode: what was asked is answered
        lastActive_ = active;
        requested_ = -1;
    }
    if (borrowed_ < 0) before_ = active;
    borrowed_ = want;
    if (want == active) {
        // Already in force, but an earlier request for another mode may still be on its
        // way (the splash's flat, issued the same frame): ask for this one over it.
        if (requested_ >= 0 && requested_ != want) {
            requested_ = -1;
            return want;
        }
        return -1;
    }
    if (requested_ == want) return -1;  // asked already; the switch is in flight
    requested_ = want;
    return want;
}

int32_t ModeBorrow::Return(int32_t active) {
    const int32_t back =
        (borrowed_ >= 0 && before_ >= 0 && active == borrowed_ && before_ != active) ? before_ : -1;
    before_ = borrowed_ = requested_ = lastActive_ = -1;
    return back;
}

} // namespace mp::browser
