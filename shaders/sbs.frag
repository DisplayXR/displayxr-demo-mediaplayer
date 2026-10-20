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
#endif

layout(push_constant) uniform PushConstants {
    vec2 uvOffset;
    vec2 uvScale;
    int mode;        // 0 = RGBA, 1 = I420, 2 = NV12
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
    vec4 cdlPower;   // .rgb = ASC power
    vec4 lutDomain;  // .x = domain min, .y = 1/(max-min), .z = LUT size N, .w = 1 for P016
    int yuvMatrix;   // 0 = BT.709 (default: base content unchanged), 1 = BT.601, 2 = BT.2020 NCL
    float zebraLevel;// > 0: diagonal stripes where the graded luma is at or above it (0..1)
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
    if (pc.yuvMatrix == 1)         // BT.601 (Kr .299, Kb .114): SD video, JPEG-era sources
        return vec3(yb + 1.402 * vb, yb - 0.344136 * ub - 0.714136 * vb, yb + 1.772 * ub);
    if (pc.yuvMatrix == 2)         // BT.2020 non-constant luminance (Kr .2627, Kb .0593)
        return vec3(yb + 1.4746 * vb, yb - 0.164553 * ub - 0.571353 * vb, yb + 1.8814 * ub);
#endif
    return vec3(yb + 1.5748 * vb,
                yb - 0.1873 * ub - 0.4681 * vb,
                yb + 1.8556 * ub);
}

// Fetch the display-referred RGB at `uv` (RGBA texture or planar YUV).
vec3 SampleSource(vec2 uv) {
    if (pc.mode == 0) return texture(plane0, uv).rgb;

    float y = texture(plane0, uv).r;
    float u, v;
    if (pc.mode == 2) {            // NV12: chroma interleaved in .rg
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
vec3 SceneLinearToDisplayReferred(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

// Media Player Pro grade, in the order an on-set chain applies it:
// exposure (linear light) -> ASC CDL (on the encoded signal) -> 3D LUT.
// Every stage is skipped at its identity, so the base player pays three branches.
vec3 Grade(vec3 c) {
    if (pc.exposure != 0.0) {
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
#endif

void main() {
    vec2 uv = pc.uvOffset + vUV * pc.uvScale;
    vec3 graded = Grade(SampleSource(uv));
    vec3 c = graded;

#ifdef MP_GRADE
    if (pc.qcMode == 1) {          // eye difference: alignment / sync check
        vec3 o = Grade(SampleSource(uv + pc.otherDelta));
        c = clamp(abs(c - o) * 4.0, 0.0, 1.0);
    } else if (pc.qcMode == 2) {   // anaglyph: red = this (left) eye, cyan = the other
        vec3 o = Grade(SampleSource(uv + pc.otherDelta));
        float l = dot(c, vec3(0.299, 0.587, 0.114));
        float r = dot(o, vec3(0.299, 0.587, 0.114));
        c = vec3(l, r, r);
    } else if (pc.qcMode == 3) {   // false colour
        c = FalseColour(c);
    }
    if (pc.zebraLevel > 0.0 && pc.qcMode != 1 &&
        dot(graded, vec3(0.2126, 0.7152, 0.0722)) >= pc.zebraLevel) {
        // Stripes in SOURCE pixels, 45 degrees, 16 px period: both eyes stripe the same
        // content point, so the zebras sit at the object's depth, not on the screen plane
        // (a half-SBS frame's eye offset is a multiple of 16 px for the usual widths).
        vec2 sp = uv * vec2(textureSize(plane0, 0));
        if (mod(sp.x + sp.y, 16.0) < 8.0) c = vec3(0.0);
    }
#endif

    float a = pc.mode == 0 ? texture(plane0, uv).a : 1.0;
    outColor = EncodeForTarget(vec4(c, a));
}
