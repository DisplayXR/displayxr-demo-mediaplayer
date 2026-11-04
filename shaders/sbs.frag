// SPDX-License-Identifier: Apache-2.0
// Sample one half of a side-by-side source and emit RGB. The source is either an RGBA
// texture (images: mode 0) or planar YUV from the video decoder (mode 1 = I420 three
// planes, mode 2 = NV12 two planes). For YUV the GPU does BOTH the colour convert and
// the downscale (the texture is the decoder's native frame; the viewport is the small
// per-eye tile), so the decode thread no longer runs swscale.
//
// uvOffset/uvScale select the SBS half: left eye = offset(0,0) scale(0.5,1), right eye =
// offset(0.5,0) scale(0.5,1); a 2D source uses offset(0,0) scale(1,1).
//
// The VR180 / VR360 viewer (#155, desktop only, MP_GRADE): when `mode` carries VR flags
// (bits 8+), uvOffset/uvScale are instead the eye's TANGENT window and VrLocalUV() maps it
// through the sphere into the eye's rect - see src/media/VrView.h (SampleUV is the CPU twin
// of VrLocalUV; keep them identical) and the push-block note there.
//
// MP_GRADE (defined for the DESKTOP build by scripts/gen_shaders.sh) adds the Media
// Player Pro grade (exposure, ASC CDL, 3D LUT at binding 3) and the QC views, and
// grows the push block to 116 bytes (yuvMatrix last). The Android leg compiles this same file WITHOUT
// it, so its 3-binding layout and 28-byte push block (sbs_renderer.cpp) are unchanged.
#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D plane0;  // RGBA (mode 0) or Y (modes 1/2)
layout(binding = 1) uniform sampler2D plane1;  // U (I420) or interleaved UV (NV12)
layout(binding = 2) uniform sampler2D plane2;  // V (I420 only)
#ifdef MP_GRADE
layout(binding = 3) uniform sampler3D lut3d;   // grade LUT (Media Player Pro); 1x1x1 dummy otherwise
layout(binding = 4) uniform sampler2D planeB0; // Pro compare source B (the clip wipe), as plane0..2
layout(binding = 5) uniform sampler2D planeB1;
layout(binding = 6) uniform sampler2D planeB2;
layout(binding = 7) uniform sampler3D calLut;  // per-panel display calibration (#133); 1x1x1 dummy otherwise
#endif

layout(push_constant) uniform PushConstants {
    vec2 uvOffset;
    vec2 uvScale;
    int mode;        // bits 0-1: 0 = RGBA, 1 = I420, 2 = NV12. MP_GRADE: bits 8+ = VR flags (#155)
    float fullRange; // 1 = full/JPEG range, 0 = limited/MPEG range
    float srgbTarget;// 1 = the colour attachment is an _SRGB view (encodes on store)
#ifdef MP_GRADE
    // --- Grade + QC (Media Player Pro; all identity / 0 in the base player) ---------
    int qcMode;      // 0 = normal, 1 = difference |this eye - other eye|, 2 = anaglyph (red/cyan),
                     // 3 = false colour (exposure bands on the graded luma)
    vec2 otherDelta; // uv offset from this eye's sample to the same point in the other eye
    float exposure;  // stops, applied in linear light
    float lutOn;     // 1 = sample lut3d
    vec4 cdlSlope;   // .rgb = ASC slope, .a = ASC saturation
    vec4 cdlOffset;  // .rgb = ASC offset, .a = 1 when the CDL is on
    vec4 cdlPower;   // .rgb = ASC power; .a = VR window pitch, radians (#155)
    vec4 lutDomain;  // .x = domain min, .y = 1/(max-min), .z = LUT size N, .w = 1 for P016
    int yuvMatrix;   // bits 0-1: 0 = BT.709 (default: base content unchanged), 1 = BT.601,
                     // 2 = BT.2020 NCL. Pro HDR (#134, 0 for SDR): bits 4-5 transfer (1 HLG,
                     // 2 PQ, 3 scene-linear stored as PQ), bit 6 roll-off, bit 7 BT.2020
                     // primaries, bit 8 PQ shaper for an OCIO look, bits 16-31 source peak
    float zebraLevel;// > 0: diagonal stripes where the graded luma is at or above it (0..1)
    int viewFlags;   // bits 0-2 channel (0 RGB, 1 R, 2 G, 3 B, 4 alpha, 5 luma); bits 4-5 wipe
                     // (0 off, 1 before | after the grade, 2 left eye | right eye, 3 clip A | B);
                     // B's sampling: bits 8-9 mode, 10 full range, 11-12 matrix, 13 P016, 14 bound;
                     // bit 15: display calibration on (calLut, domain 0..1);
                     // bits 16-31: VR heading, unorm16 of a turn (#155)
    uint wipeGamma;  // low 16 bits: wipe position across the picture (unorm16); high 16 bits:
                     // display gamma x 4096 (0 = 1.0)
#endif
} pc;

// sRGB EOTF: display-referred (encoded) -> scene-linear. Every value this shader
// produces is display-referred — the RGBA source is an encoded texture sampled through a
// UNORM view, and the YUV path's matrix emits R'G'B', not RGB. When the attachment is
// _SRGB the hardware encodes on store, so we decode first and the round trip is an
// identity: the same encoded bytes land in the swapchain as on the UNORM path (#78).
vec3 DisplayReferredToSceneLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec4 EncodeForTarget(vec4 c) {
    if (pc.srgbTarget > 0.5) c.rgb = DisplayReferredToSceneLinear(clamp(c.rgb, 0.0, 1.0));
    return c;
}

// Range-expanded Y'CbCr (Y 0..1, Cb/Cr -0.5..0.5) -> R'G'B'. Without MP_GRADE (Android)
// and for yuvMatrix 0 this is exactly the BT.709 expression the shader always used.
vec3 YuvToRgb(float yb, float ub, float vb) {
#ifdef MP_GRADE
    if ((pc.yuvMatrix & 3) == 1)       // BT.601 (Kr .299, Kb .114): SD video, JPEG-era sources
        return vec3(yb + 1.402 * vb, yb - 0.344136 * ub - 0.714136 * vb, yb + 1.772 * ub);
    if ((pc.yuvMatrix & 3) == 2)       // BT.2020 non-constant luminance (Kr .2627, Kb .0593)
        return vec3(yb + 1.4746 * vb, yb - 0.164553 * ub - 0.571353 * vb, yb + 1.8814 * ub);
#endif
    return vec3(yb + 1.5748 * vb,
                yb - 0.1873 * ub - 0.4681 * vb,
                yb + 1.8556 * ub);
}

// The source's pixel format (the low bits of `mode`; the VR viewer uses the rest).
int SrcMode() { return pc.mode & 3; }

// Fetch the display-referred RGB at `uv` (RGBA texture or planar YUV).
vec3 SampleSource(vec2 uv) {
    if (SrcMode() == 0) return texture(plane0, uv).rgb;

    float y = texture(plane0, uv).r;
    float u, v;
    if (SrcMode() == 2) {          // NV12: chroma interleaved in .rg
        vec2 c = texture(plane1, uv).rg;
        u = c.r;
        v = c.g;
    } else {                       // I420: separate U and V planes
        u = texture(plane1, uv).r;
        v = texture(plane2, uv).r;
    }

    // Range-expand, then the frame's matrix (YuvToRgb). Full range uses the samples directly; limited range
    // remaps Y [16,235]->[0,1] and chroma [16,240]->[-0.5,0.5] (8-bit).
    float yb, ub, vb;
#ifdef MP_GRADE
    if (pc.lutDomain.w > 0.5 && pc.fullRange < 0.5) {
        // P016 / P010 (Pro 10-bit capture): 10-bit limited range (Y 64..940, C 64..960),
        // MSB-aligned in 16 bits.
        yb = (y * 65535.0 / 64.0 - 64.0) / 876.0;
        ub = (u * 65535.0 / 64.0 - 512.0) / 896.0;
        vb = (v * 65535.0 / 64.0 - 512.0) / 896.0;
        return clamp(YuvToRgb(yb, ub, vb), 0.0, 1.0);
    }
#endif
    if (pc.fullRange > 0.5) {
        yb = y;
        ub = u - 0.5;
        vb = v - 0.5;
    } else {
        yb = (y - 16.0 / 255.0) * (255.0 / 219.0);
        ub = (u - 128.0 / 255.0) * (255.0 / 224.0);
        vb = (v - 128.0 / 255.0) * (255.0 / 224.0);
    }
    return clamp(YuvToRgb(yb, ub, vb), 0.0, 1.0);
}

#ifdef MP_GRADE
// The compare source B (the clip wipe): SampleSource's maths with B's planes and B's own
// mode / range / matrix / bit depth from viewFlags.
vec3 YuvToRgbMatrix(int m, float yb, float ub, float vb) {
    if (m == 1) return vec3(yb + 1.402 * vb, yb - 0.344136 * ub - 0.714136 * vb, yb + 1.772 * ub);
    if (m == 2) return vec3(yb + 1.4746 * vb, yb - 0.164553 * ub - 0.571353 * vb, yb + 1.8814 * ub);
    return vec3(yb + 1.5748 * vb, yb - 0.1873 * ub - 0.4681 * vb, yb + 1.8556 * ub);
}

vec3 SampleSourceB(vec2 uv) {
    const int mode = (pc.viewFlags >> 8) & 3;
    const bool full = ((pc.viewFlags >> 10) & 1) != 0;
    const int matrix = (pc.viewFlags >> 11) & 3;
    const bool p16 = ((pc.viewFlags >> 13) & 1) != 0;
    if (mode == 0) return texture(planeB0, uv).rgb;
    float y = texture(planeB0, uv).r;
    float u, v;
    if (mode == 2) {
        vec2 c = texture(planeB1, uv).rg;
        u = c.r;
        v = c.g;
    } else {
        u = texture(planeB1, uv).r;
        v = texture(planeB2, uv).r;
    }
    float yb, ub, vb;
    if (p16 && !full) {
        yb = (y * 65535.0 / 64.0 - 64.0) / 876.0;
        ub = (u * 65535.0 / 64.0 - 512.0) / 896.0;
        vb = (v * 65535.0 / 64.0 - 512.0) / 896.0;
    } else if (full) {
        yb = y;
        ub = u - 0.5;
        vb = v - 0.5;
    } else {
        yb = (y - 16.0 / 255.0) * (255.0 / 219.0);
        ub = (u - 128.0 / 255.0) * (255.0 / 224.0);
        vb = (v - 128.0 / 255.0) * (255.0 / 224.0);
    }
    return clamp(YuvToRgbMatrix(matrix, yb, ub, vb), 0.0, 1.0);
}

vec3 SceneLinearToDisplayReferred(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

// Media Player Pro grade, in the order an on-set chain applies it:
// exposure (linear light) -> ASC CDL (on the encoded signal) -> 3D LUT.
// Every stage is skipped at its identity, so the base player pays three branches.
vec3 GradeStages(vec3 c, bool withExposure) {
    if (withExposure && pc.exposure != 0.0) {
        vec3 lin = DisplayReferredToSceneLinear(clamp(c, 0.0, 1.0)) * exp2(pc.exposure);
        c = SceneLinearToDisplayReferred(clamp(lin, 0.0, 1.0));
    }
    if (pc.cdlOffset.a > 0.5) {
        c = pow(clamp(c * pc.cdlSlope.rgb + pc.cdlOffset.rgb, 0.0, 1.0), pc.cdlPower.rgb);
        float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
        c = clamp(luma + pc.cdlSlope.a * (c - luma), 0.0, 1.0);
    }
    if (pc.lutOn > 0.5) {
        // Map the LUT domain onto texel centres: N samples span [0.5/N, 1 - 0.5/N].
        vec3 t = clamp((c - pc.lutDomain.x) * pc.lutDomain.y, 0.0, 1.0);
        float n = pc.lutDomain.z;
        c = texture(lut3d, t * ((n - 1.0) / n) + 0.5 / n).rgb;
    }
    return c;
}
vec3 Grade(vec3 c) { return GradeStages(c, true); }

// HDR sources on the SDR panel (#134). The CPU twin is src/color/ToneMap.h HdrToDisplay();
// change one, change the other. Signal -> linear relative to reference white (203 nits) ->
// exposure -> BT.2020 -> BT.709 -> BT.2390 roll-off on max(R,G,B) down to reference white
// -> sRGB encode, so the grade, the LUT and the QC views see an SDR picture.
const float kRefWhite = 203.0;
const float kPqM1 = 2610.0 / 16384.0;
const float kPqM2 = 2523.0 / 4096.0 * 128.0;
const float kPqC1 = 3424.0 / 4096.0;
const float kPqC2 = 2413.0 / 4096.0 * 32.0;
const float kPqC3 = 2392.0 / 4096.0 * 32.0;

int HdrTransfer() { return (pc.yuvMatrix >> 4) & 3; }

float PqToNits(float e) {
    float p = pow(clamp(e, 0.0, 1.0), 1.0 / kPqM2);
    return 10000.0 * pow(max(p - kPqC1, 0.0) / (kPqC2 - kPqC3 * p), 1.0 / kPqM1);
}
float NitsToPq(float nits) {
    float p = pow(clamp(nits / 10000.0, 0.0, 1.0), kPqM1);
    return pow((kPqC1 + kPqC2 * p) / (1.0 + kPqC3 * p), kPqM2);
}
float HlgToScene(float e) {
    e = clamp(e, 0.0, 1.0);
    return e <= 0.5 ? e * e / 3.0 : (exp((e - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
}
float Bt2390Nits(float nits, float srcPeak, float dstPeak) {
    if (srcPeak <= dstPeak) return min(nits, dstPeak);
    float sMax = NitsToPq(srcPeak);
    float e1 = NitsToPq(nits) / sMax;
    float maxLum = NitsToPq(dstPeak) / sMax;
    float ks = 1.5 * maxLum - 0.5;
    float e2 = e1;
    if (e1 >= 1.0) {
        e2 = maxLum;
    } else if (e1 > ks) {
        float t = (e1 - ks) / (1.0 - ks), t2 = t * t, t3 = t2 * t;
        e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks) + (-2.0 * t3 + 3.0 * t2) * maxLum;
    }
    return PqToNits(e2 * sMax);
}
vec3 HdrToDisplay(vec3 c, float exposure, bool lutShaper) {
    const int tr = HdrTransfer();
    vec3 l;
    if (tr == 1) {  // HLG: inverse OETF, then the OOTF for a 1000-nit display (gamma 1.2)
        vec3 s = vec3(HlgToScene(c.r), HlgToScene(c.g), HlgToScene(c.b));
        float ys = dot(s, vec3(0.2627, 0.6780, 0.0593));
        l = s * (1000.0 * pow(max(ys, 1e-6), 0.2) / kRefWhite);
    } else {        // PQ, or scene-linear stored as PQ
        l = vec3(PqToNits(c.r), PqToNits(c.g), PqToNits(c.b)) / kRefWhite;
    }
    float m = exp2(exposure);
    l *= m;
    if (((pc.yuvMatrix >> 7) & 1) != 0)
        l = vec3(1.660491 * l.r - 0.587641 * l.g - 0.072850 * l.b,
                 -0.124550 * l.r + 1.132900 * l.g - 0.008349 * l.b,
                 -0.018151 * l.r - 0.100579 * l.g + 1.118730 * l.b);
    l = max(l, vec3(0.0));
    // An OCIO look on scene-linear EXR: the LUT gets the linear light as PQ code, unclipped
    // (its bake decodes it first), instead of the SDR picture.
    if (lutShaper && ((pc.yuvMatrix >> 8) & 1) != 0)
        return vec3(NitsToPq(l.r * kRefWhite), NitsToPq(l.g * kRefWhite), NitsToPq(l.b * kRefWhite));
    float mx = max(l.r, max(l.g, l.b));
    if (((pc.yuvMatrix >> 6) & 1) != 0 && mx > 0.0) {
        float src = (tr == 1 ? 1000.0 : float((pc.yuvMatrix >> 16) & 0xFFFF)) * m;
        l *= (Bt2390Nits(mx * kRefWhite, src, kRefWhite) / kRefWhite) / mx;
    }
    return SceneLinearToDisplayReferred(clamp(l, 0.0, 1.0));
}

// The picture as decoded (the "before" side of the grade wipe): HDR made viewable, no grade.
vec3 SourceView(vec3 s) { return HdrTransfer() == 0 ? s : HdrToDisplay(s, 0.0, false); }
// This clip's grade: an HDR source takes exposure in linear light, before the roll-off.
vec3 GradeSrc(vec3 s) {
    return HdrTransfer() == 0 ? Grade(s) : GradeStages(HdrToDisplay(s, pc.exposure, true), false);
}

// False colour: exposure bands on the graded Rec.709 luma (% of full scale), the rest in
// grey, the way on-set monitors show it. Bands: crushed < 2.5 purple, 2.5-10 blue,
// 38-42 (18 % grey) green, 52-56 (skin) pink, 97-99 yellow, > 99 clipped red.
vec3 FalseColour(vec3 c) {
    float y = dot(c, vec3(0.2126, 0.7152, 0.0722)) * 100.0;
    if (y < 2.5) return vec3(0.50, 0.00, 0.60);
    if (y < 10.0) return vec3(0.10, 0.25, 0.95);
    if (y >= 38.0 && y <= 42.0) return vec3(0.15, 0.80, 0.20);
    if (y >= 52.0 && y <= 56.0) return vec3(1.00, 0.55, 0.70);
    if (y > 99.0) return vec3(1.00, 0.10, 0.05);
    if (y >= 97.0) return vec3(1.00, 0.90, 0.10);
    return vec3(y / 100.0 * 0.85);
}
#else
vec3 Grade(vec3 c) { return c; }
vec3 GradeSrc(vec3 c) { return c; }
#endif

#ifdef MP_GRADE
// ---- VR180 / VR360 viewer (#155) -------------------------------------------------------
const float kPi = 3.14159265358979;
int VrProj() { return (pc.mode >> 8) & 3; }  // 0 none, 1 equirect 180, 2 equirect 360, 3 fisheye

// This draw's (or the other eye's) rect in the frame: SBS / TB halves or the whole frame.
vec4 VrEyeRect(bool otherEye) {
    int pk = (pc.mode >> 12) & 3;
    bool second = (((pc.mode >> 14) & 1) != 0) != otherEye;
    float k = second ? 0.5 : 0.0;
    if (pk == 1) return vec4(k, 0.0, 0.5, 1.0);
    if (pk == 2) return vec4(0.0, k, 1.0, 0.5);
    return vec4(0.0, 0.0, 1.0, 1.0);
}

// Tangent coordinates -> eye-local source uv (.xy) and 1 inside the picture / 0 outside
// (.z). vr::SampleUV in src/media/VrView.h, line for line.
vec3 VrLocalUV(vec2 t, vec2 eyePx) {
    int model = (pc.mode >> 10) & 3;
    float heading = float(uint(pc.viewFlags) >> 16) / 65536.0 * 2.0 * kPi;
    vec3 d;
    if (model == 1) {              // cylinder (stereo ODS 360): row = tan(elevation)
        float a = heading + atan(t.x);
        float e = atan(t.y);
        d = vec3(cos(e) * sin(a), sin(e), cos(e) * cos(a));
    } else {                       // plane (off-axis window, pitch rotates) / free (mono)
        float cp = cos(pc.cdlPower.a), sp = sin(pc.cdlPower.a);
        vec3 c = vec3(t.x, t.y * cp + sp, -t.y * sp + cp);
        float ch = cos(heading), sh = sin(heading);
        d = vec3(c.x * ch + c.z * sh, c.y, -c.x * sh + c.z * ch);
    }
    int proj = VrProj();
    if (proj == 3) {               // equidistant fisheye, image circle inscribed in the eye
        float len = length(d);
        float th = acos(clamp(len > 0.0 ? d.z / len : 1.0, -1.0, 1.0));
        float halfFov = radians(float((pc.mode >> 16) & 511)) * 0.5;
        float r = 0.5 * th / halfFov;
        float rho = length(d.xy);
        vec2 dir = rho > 1e-8 ? d.xy / rho : vec2(0.0);
        float aspect = eyePx.y > 0.0 ? eyePx.x / eyePx.y : 1.0;
        vec2 k = aspect > 1.0 ? vec2(1.0 / aspect, 1.0) : vec2(1.0, aspect);
        return vec3(0.5 + dir.x * r * k.x, 0.5 - dir.y * r * k.y, th <= halfFov ? 1.0 : 0.0);
    }
    float lon = atan(d.x, d.z);
    float lat = atan(d.y, length(d.xz));
    float v = 0.5 - lat / kPi;
    if (proj == 1) return vec3(lon / kPi + 0.5, v, abs(lon) <= 0.5 * kPi ? 1.0 : 0.0);
    return vec3(fract(lon / (2.0 * kPi) + 0.5), v, 1.0);
}

// The frame uv a VR draw samples at tangent `t`, for this eye or the other one. .z = 0
// where the projection has no picture (drawn black).
vec3 VrFrameUV(vec2 t, bool otherEye) {
    vec4 r = VrEyeRect(otherEye);
    vec2 eyePx = r.zw * vec2(textureSize(plane0, 0));
    vec3 l = VrLocalUV(t, eyePx);
    // Stay half a texel inside the eye's rect: bilinear taps must not reach the other eye.
    vec2 h = 0.5 / max(eyePx, vec2(1.0));
    return vec3(r.xy + clamp(l.xy, h, 1.0 - h) * r.zw, l.z);
}
#endif

void main() {
    vec2 uv = pc.uvOffset + vUV * pc.uvScale;
#ifdef MP_GRADE
    // VR viewer: uv was the tangent window; the source uv comes through the sphere.
    // otherUv is where the same window point lands in the OTHER eye (the Pro QC views):
    // for a VR draw otherDelta is a tangent offset (the eyes' convergence difference).
    const bool vr = VrProj() != 0;
    vec2 tan0 = uv;
    float inPicture = 1.0;
    if (vr) {
        vec3 f = VrFrameUV(tan0, false);
        uv = f.xy;
        inPicture = f.z;
    }
    vec2 otherUv = vr ? VrFrameUV(tan0 + pc.otherDelta, true).xy : uv + pc.otherDelta;
#endif
    vec3 graded = GradeSrc(SampleSource(uv));
#ifdef MP_GRADE
    if (inPicture < 0.5) graded = vec3(0.0);
#endif
    vec3 c = graded;
#ifdef MP_GRADE
    const int channel = pc.viewFlags & 7;
    const int wipe = (pc.viewFlags >> 4) & 3;
    const float wipePos = float(pc.wipeGamma & 0xFFFFu) / 65535.0;
    // The wipe is a vertical line across the picture as shown (vUV, before pan / zoom), the
    // same in both eyes. 1: left of it the source as it arrives, right of it the grade.
    // 2: left of it the left eye, right of it the right eye (otherDelta reaches it).
    const bool pastWipe = wipe != 0 && vUV.x >= wipePos;
    if (wipe == 1 && !pastWipe) c = SourceView(SampleSource(uv));
    if (wipe == 2 && pastWipe) c = GradeSrc(SampleSource(otherUv));
    // 3: right of the line the compare clip B, at the same point of the same eye, graded
    // the same way (both versions seen through one look). Without B bound, A stays.
    if (wipe == 3 && pastWipe && ((pc.viewFlags >> 14) & 1) != 0) c = Grade(SampleSourceB(uv));
    graded = c;  // zebras judge the picture as shown on each side of the wipe
#endif

#ifdef MP_GRADE
    if (pc.qcMode == 1) {          // eye difference: alignment / sync check
        vec3 o = GradeSrc(SampleSource(otherUv));
        c = clamp(abs(c - o) * 4.0, 0.0, 1.0);
    } else if (pc.qcMode == 2) {   // anaglyph: red = this (left) eye, cyan = the other
        vec3 o = GradeSrc(SampleSource(otherUv));
        float l = dot(c, vec3(0.299, 0.587, 0.114));
        float r = dot(o, vec3(0.299, 0.587, 0.114));
        c = vec3(l, r, r);
    } else if (pc.qcMode == 3) {   // false colour
        c = FalseColour(c);
    }
    // Display calibration (#133): corrects THIS panel towards its target, so it follows the
    // show grade and the QC views (they are what the panel should show) and comes before
    // the gamma trim. Its domain is 0..1, the size is the texture's.
    if (((pc.viewFlags >> 15) & 1) != 0) {
        const float n = float(textureSize(calLut, 0).x);
        c = texture(calLut, clamp(c, 0.0, 1.0) * ((n - 1.0) / n) + 0.5 / n).rgb;
    }
    // Display gamma trim, then channel isolation (a channel or luma shown as grey).
    const float gammaTrim = float(pc.wipeGamma >> 16) / 4096.0;
    if (gammaTrim > 0.0 && abs(gammaTrim - 1.0) > 0.001) c = pow(clamp(c, 0.0, 1.0), vec3(1.0 / gammaTrim));
    if (channel == 1) c = vec3(c.r);
    else if (channel == 2) c = vec3(c.g);
    else if (channel == 3) c = vec3(c.b);
    else if (channel == 4) c = vec3(SrcMode() == 0 ? texture(plane0, uv).a : 1.0);
    else if (channel == 5) c = vec3(dot(c, vec3(0.2126, 0.7152, 0.0722)));
    if (pc.zebraLevel > 0.0 && pc.qcMode != 1 &&
        dot(graded, vec3(0.2126, 0.7152, 0.0722)) >= pc.zebraLevel) {
        // Stripes in SOURCE pixels, 45 degrees, 16 px period: both eyes stripe the same
        // content point, so the zebras sit at the object's depth, not on the screen plane
        // (a half-SBS frame's eye offset is a multiple of 16 px for the usual widths).
        vec2 sp = uv * vec2(textureSize(plane0, 0));
        if (mod(sp.x + sp.y, 16.0) < 8.0) c = vec3(0.0);
    }
#endif

    float a = SrcMode() == 0 ? texture(plane0, uv).a : 1.0;
#ifdef MP_GRADE
    if (inPicture < 0.5) { c = vec3(0.0); a = 1.0; }
#endif
    outColor = EncodeForTarget(vec4(c, a));
}
