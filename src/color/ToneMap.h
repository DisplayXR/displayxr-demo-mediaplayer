// SPDX-License-Identifier: Apache-2.0
//
// HDR sources on an SDR panel (Media Player Pro, #134): HLG and PQ video, and scene-linear
// OpenEXR whose values go above 1.0. This is the CPU model of the shader's HdrToDisplay()
// (shaders/sbs.frag): the scopes run it, the EXR decode encodes with it, and the tests pin
// both to the published reference values. Change one, change the other.
//
// The pipeline, per pixel:
//   signal (0..1) -> linear light relative to HDR reference white (1.0 = 203 nits, ITU-R
//   BT.2408) -> exposure (stops) -> BT.2020 -> BT.709 primaries when the source is wide
//   gamut -> the highlight roll-off (ITU-R BT.2390 EETF, in the PQ domain, on max(R,G,B)
//   so hue is kept) from the source peak down to SDR white -> the sRGB encode the rest of
//   the player's pipeline expects (the grade, the LUT and the QC views run after it).
// With the roll-off off, everything above SDR white clips (the scene-linear EXR path keeps
// today's look that way, but exposure now acts before the clip, so it brings highlights back).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mp::color {

constexpr float kRefWhiteNits = 203.0f;  // BT.2408 HDR reference (graphics) white
constexpr float kPqMaxNits = 10000.0f;
constexpr float kDefaultPeakNits = 1000.0f;  // when the stream carries no light-level metadata
constexpr float kHlgDisplayNits = 1000.0f;   // the HLG OOTF's nominal display peak (gamma 1.2)
// HLG is scene-referred: its "peak" is the display it is rendered for (the OOTF's Lw), not a
// property of the stream. peakNits carries it for HLG (#169: it was fixed at 1000 nits).

// Transfer of the decoded signal (FrameRing ColorTransfer, same values).
enum class Transfer : int { SDR = 0, HLG = 1, PQ = 2, LinearPq = 3 };
// LinearPq: scene-linear (OpenEXR) stored as PQ code of (linear x kRefWhiteNits), so values
// up to ~49x reference white survive a 16-bit YUV frame. Its primaries are BT.709.

// SMPTE ST 2084.
constexpr float kPqM1 = 2610.0f / 16384.0f;
constexpr float kPqM2 = 2523.0f / 4096.0f * 128.0f;
constexpr float kPqC1 = 3424.0f / 4096.0f;
constexpr float kPqC2 = 2413.0f / 4096.0f * 32.0f;
constexpr float kPqC3 = 2392.0f / 4096.0f * 32.0f;

// PQ code (0..1) -> nits.
inline float PqToNits(float e) {
    e = std::clamp(e, 0.0f, 1.0f);
    const float p = std::pow(e, 1.0f / kPqM2);
    const float n = std::max(p - kPqC1, 0.0f) / (kPqC2 - kPqC3 * p);
    return kPqMaxNits * std::pow(n, 1.0f / kPqM1);
}
// nits -> PQ code (0..1).
inline float NitsToPq(float nits) {
    const float y = std::clamp(nits / kPqMaxNits, 0.0f, 1.0f);
    const float p = std::pow(y, kPqM1);
    return std::pow((kPqC1 + kPqC2 * p) / (1.0f + kPqC3 * p), kPqM2);
}

// The same in double: the table the EXR store uses is built with it (in float, PQ's ~79
// exponent turns rounding into ~1 16-bit code).
inline double NitsToPqD(double nits) {
    const double y = std::clamp(nits / kPqMaxNits, 0.0, 1.0);
    const double p = std::pow(y, (double)kPqM1);
    return std::pow((kPqC1 + kPqC2 * p) / (1.0 + kPqC3 * p), (double)kPqM2);
}

// ARIB STD-B67 / BT.2100 HLG inverse OETF: signal (0..1) -> scene linear (0..1).
inline float HlgToScene(float e) {
    constexpr float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
    e = std::clamp(e, 0.0f, 1.0f);
    return e <= 0.5f ? e * e / 3.0f : (std::exp((e - c) / a) + b) / 12.0f;
}
// BT.2100 HLG system gamma for a display of peak `lw` nits (1.2 at 1000).
inline float HlgSystemGamma(float lw) { return 1.2f + 0.42f * std::log10(lw / 1000.0f); }

// sRGB encode / decode (the player's display-referred convention, shaders/sbs.frag).
inline float SrgbEncode(float l) {
    l = std::clamp(l, 0.0f, 1.0f);
    return l <= 0.0031308f ? 12.92f * l : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}
// The same curve continued past 1.0 (values above SDR white, #169).
inline float SrgbEncodeOver(float l) {
    l = std::max(l, 0.0f);
    return l <= 0.0031308f ? 12.92f * l : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}
inline float SrgbDecode(float e) {
    e = std::clamp(e, 0.0f, 1.0f);
    return e <= 0.04045f ? e / 12.92f : std::pow((e + 0.055f) / 1.055f, 2.4f);
}

// BT.2390 EETF on a luminance in nits: identity up to the knee, then a Hermite spline that
// lands the source peak exactly on the target peak. Both peaks in nits; min black = 0.
inline float Bt2390Nits(float nits, float srcPeak, float dstPeak) {
    if (srcPeak <= dstPeak) return std::min(nits, dstPeak);
    const float sMax = NitsToPq(srcPeak);
    const float e1 = NitsToPq(nits) / sMax;
    const float maxLum = NitsToPq(dstPeak) / sMax;
    const float ks = 1.5f * maxLum - 0.5f;
    float e2 = e1;
    if (e1 >= 1.0f) {
        e2 = maxLum;
    } else if (e1 > ks) {
        const float t = (e1 - ks) / (1.0f - ks), t2 = t * t, t3 = t2 * t;
        e2 = (2 * t3 - 3 * t2 + 1) * ks + (t3 - 2 * t2 + t) * (1.0f - ks) + (-2 * t3 + 3 * t2) * maxLum;
    }
    return PqToNits(e2 * sMax);
}

// BT.2020 -> BT.709 primaries, linear light (D65 both).
inline void Bt2020To709(float c[3]) {
    const float r = 1.660491f * c[0] - 0.587641f * c[1] - 0.072850f * c[2];
    const float g = -0.124550f * c[0] + 1.132900f * c[1] - 0.008349f * c[2];
    const float b = -0.018151f * c[0] - 0.100579f * c[1] + 1.118730f * c[2];
    c[0] = r;
    c[1] = g;
    c[2] = b;
}

struct HdrParams {
    Transfer transfer = Transfer::SDR;
    bool bt2020 = false;       // source primaries are BT.2020 (converted to BT.709)
    bool rollOff = true;       // BT.2390 roll-off; false = clip at SDR white
    float peakNits = kDefaultPeakNits;  // the source's brightest (PQ / EXR), before exposure;
                                        // for HLG the OOTF's display peak Lw (HlgPeak)
    float exposure = 0.0f;     // stops, applied in linear light before the roll-off
    // An OpenColorIO look on scene-linear EXR: hand the LUT the linear light as PQ code of
    // (linear x 203 nits), unclipped, instead of the SDR picture; the bake decodes it
    // (ocio::Request::linearPqInput), so the OCIO view sees values above 1.0.
    bool pqShaper = false;
    // Scene-linear EXR without the roll-off (tone map auto, #169): keep values above SDR
    // white, sRGB-encoded past 1.0, for the grade; the caller clips after the grade.
    bool overWhite = false;
};

// Scene-linear EXR -> PQ code, the decoder's per-sample encode (#134). A lerped table over
// u = (v / max)^(1/8), which straightens PQ's vertical toe (PQ ~ v^0.16 near black); tests/tone_map_test.cpp bounds the
// error against NitsToPq. Negative and NaN go to 0, above max (~49x white) clips.
constexpr int kLinearPqTableN = 16384;
constexpr float kLinearPqMax = kPqMaxNits / kRefWhiteNits;
inline void BuildLinearPqTable(std::vector<float>& t) {
    t.resize((size_t)kLinearPqTableN + 1);
    for (int i = 0; i <= kLinearPqTableN; ++i) {
        const double u = (double)i / kLinearPqTableN, u2 = u * u, u4 = u2 * u2;
        t[(size_t)i] = (float)NitsToPqD(u4 * u4 * kPqMaxNits);
    }
}
inline float EncodeLinearPq(const float* lut, float v) {
    v = std::fmin(std::fmax(v, 0.0f), kLinearPqMax);  // fmax sends NaN to 0
    const float t = std::sqrt(std::sqrt(std::sqrt(v / kLinearPqMax))) * (float)kLinearPqTableN;
    const int i = (std::min)((int)t, kLinearPqTableN - 1);
    return lut[i] + (lut[i + 1] - lut[i]) * (t - (float)i);
}

// The HLG OOTF's display peak Lw: peakNits, kept in the range the BT.2100 system-gamma
// formula is specified for (and that the shader's peak field carries).
constexpr float kHlgMinNits = 100.0f, kHlgMaxNits = 10000.0f;
inline float HlgPeak(const HdrParams& p) {
    return p.peakNits > 0.0f ? std::clamp(p.peakNits, kHlgMinNits, kHlgMaxNits) : kHlgDisplayNits;
}

// True when the shader takes the HDR path for this frame (SDR never does: bit-identical).
inline bool IsHdr(const HdrParams& p) { return p.transfer != Transfer::SDR; }

// One R'G'B' signal (0..1) -> display-referred sRGB-encoded R'G'B' (0..1).
inline void HdrToDisplay(const HdrParams& p, float c[3]) {
    float l[3];  // linear, relative to reference white
    if (p.transfer == Transfer::HLG) {
        float s[3];
        for (int i = 0; i < 3; ++i) s[i] = HlgToScene(c[i]);
        const float ys = 0.2627f * s[0] + 0.6780f * s[1] + 0.0593f * s[2];  // BT.2100 luma
        const float lw = HlgPeak(p);
        const float g = HlgSystemGamma(lw);
        const float k = lw * std::pow(std::max(ys, 1e-6f), g - 1.0f) / kRefWhiteNits;
        for (int i = 0; i < 3; ++i) l[i] = s[i] * k;
    } else {  // PQ or LinearPq: the code is PQ either way
        for (int i = 0; i < 3; ++i) l[i] = PqToNits(c[i]) / kRefWhiteNits;
    }
    const float m = std::exp2(p.exposure);
    for (float& v : l) v *= m;
    if (p.bt2020) Bt2020To709(l);
    for (float& v : l) v = std::max(v, 0.0f);
    if (p.pqShaper) {
        for (int i = 0; i < 3; ++i) c[i] = NitsToPq(l[i] * kRefWhiteNits);
        return;
    }
    const float mx = std::max(l[0], std::max(l[1], l[2]));
    if (p.rollOff && mx > 0.0f) {
        const float src = (p.transfer == Transfer::HLG ? HlgPeak(p) : p.peakNits) * m;
        const float out = Bt2390Nits(mx * kRefWhiteNits, src, kRefWhiteNits) / kRefWhiteNits;
        for (float& v : l) v *= out / mx;
    }
    if (p.overWhite && !p.rollOff) {
        for (int i = 0; i < 3; ++i) c[i] = SrgbEncodeOver(l[i]);
        return;
    }
    for (int i = 0; i < 3; ++i) c[i] = SrgbEncode(l[i]);
}

// HdrToDisplay() for a whole picture on the CPU ("Convert to 3D" hands the converter an
// 8-bit SDR picture, so an HDR frame is tone-mapped first). The same maths with the pow-heavy
// steps tabled: the signal decode, the roll-off gain over log2(max RGB) and the sRGB encode.
// tests/tone_map_test.cpp bounds it against HdrToDisplay() (under half an 8-bit code).
// pqShaper is ignored: the result is always the SDR picture.
class HdrDisplayTable {
public:
    explicit HdrDisplayTable(const HdrParams& p) : p_(p) {
        decode_.resize(kN + 1);
        for (int i = 0; i <= kN; ++i) {
            const float e = (float)i / kN;
            decode_[(size_t)i] = p.transfer == Transfer::HLG ? HlgToScene(e) : PqToNits(e) / kRefWhiteNits;
        }
        gain_.assign(kG + 1, 1.0f);
        const float m = std::exp2(p.exposure);
        src_ = (p.transfer == Transfer::HLG ? HlgPeak(p) : p.peakNits) * m;
        if (p.rollOff)
            for (int i = 0; i <= kG; ++i) {
                const float mx = std::exp2(kLogLo + (kLogHi - kLogLo) * (float)i / kG);
                gain_[(size_t)i] = Bt2390Nits(mx * kRefWhiteNits, src_, kRefWhiteNits) / kRefWhiteNits / mx;
            }
        encode_.resize(kN + 1);
        for (int i = 0; i <= kN; ++i) encode_[(size_t)i] = SrgbEncode(Unit((float)i / kN));
    }

    // One R'G'B' signal (0..1) -> display-referred sRGB-encoded R'G'B' (0..1).
    void Apply(float c[3]) const {
        float l[3];
        for (int i = 0; i < 3; ++i) l[i] = Lerp(decode_, c[i]);
        if (p_.transfer == Transfer::HLG) {
            const float ys = 0.2627f * l[0] + 0.6780f * l[1] + 0.0593f * l[2];
            const float lw = HlgPeak(p_);
            const float g = HlgSystemGamma(lw);
            const float k = lw * std::pow(std::max(ys, 1e-6f), g - 1.0f) / kRefWhiteNits;
            for (float& v : l) v *= k;
        }
        const float m = std::exp2(p_.exposure);
        for (float& v : l) v *= m;
        if (p_.bt2020) Bt2020To709(l);
        for (float& v : l) v = std::max(v, 0.0f);
        const float mx = std::max(l[0], std::max(l[1], l[2]));
        if (p_.rollOff && mx > 0.0f) {
            const float lg = std::log2(mx);
            // Below the table the curve is the identity (far under the knee); above it, the
            // source peak (and everything over it) lands on SDR white.
            const float gain = lg <= kLogLo   ? 1.0f
                               : lg >= kLogHi ? Bt2390Nits(mx * kRefWhiteNits, src_, kRefWhiteNits) / kRefWhiteNits / mx
                                              : Lerp(gain_, (lg - kLogLo) / (kLogHi - kLogLo));
            for (float& v : l) v *= gain;
        }
        for (int i = 0; i < 3; ++i) c[i] = Lerp8th(std::clamp(l[i], 0.0f, 1.0f));
    }

private:
    static constexpr int kN = 4096;   // signal decode and sRGB encode, lerped
    static constexpr int kG = 2048;   // roll-off gain over log2(max RGB)
    static constexpr float kLogLo = -16.0f, kLogHi = 7.0f;  // up to ~128x reference white
    // The sRGB encode's table is indexed by the 8th root of linear light, which straightens
    // its steep toe (the 1/2.4 power) so a lerp holds near black.
    static float Unit(float u) {
        const float u2 = u * u, u4 = u2 * u2;
        return u4 * u4;
    }
    static float Lerp(const std::vector<float>& t, float x) {
        const float f = std::clamp(x, 0.0f, 1.0f) * (float)(t.size() - 1);
        const int i = (std::min)((int)f, (int)t.size() - 2);
        return t[(size_t)i] + (t[(size_t)i + 1] - t[(size_t)i]) * (f - (float)i);
    }
    float Lerp8th(float l) const { return Lerp(encode_, std::sqrt(std::sqrt(std::sqrt(l)))); }

    HdrParams p_;
    float src_ = 0.0f;
    std::vector<float> decode_, gain_, encode_;
};

}  // namespace mp::color
