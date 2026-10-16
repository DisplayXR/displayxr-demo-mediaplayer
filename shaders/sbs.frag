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
// grows the push block to 112 bytes. The Android leg compiles this same file WITHOUT
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
    int qcMode;      // 0 = normal, 1 = difference |this eye - other eye|, 2 = anaglyph (red/cyan)
    vec2 otherDelta; // uv offset from this eye's sample to the same point in the other eye
    float exposure;  // stops, applied in linear light
    float lutOn;     // 1 = sample lut3d
    vec4 cdlSlope;   // .rgb = ASC slope, .a = ASC saturation
    vec4 cdlOffset;  // .rgb = ASC offset, .a = 1 when the CDL is on
    vec4 cdlPower;   // .rgb = ASC power
    vec4 lutDomain;  // .x = domain min, .y = 1/(max-min), .z = LUT size N, .w = 1 for P016
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

    // Range-expand, then BT.709. Full range uses the samples directly; limited range
    // remaps Y [16,235]->[0,1] and chroma [16,240]->[-0.5,0.5] (8-bit).
    float yb, ub, vb;
#ifdef MP_GRADE
    if (pc.lutDomain.w > 0.5 && pc.fullRange < 0.5) {
        // P016 / P010 (Pro 10-bit capture): 10-bit limited range (Y 64..940, C 64..960),
        // MSB-aligned in 16 bits.
        yb = (y * 65535.0 / 64.0 - 64.0) / 876.0;
        ub = (u * 65535.0 / 64.0 - 512.0) / 896.0;
        vb = (v * 65535.0 / 64.0 - 512.0) / 896.0;
        return clamp(vec3(yb + 1.5748 * vb, yb - 0.1873 * ub - 0.4681 * vb, yb + 1.8556 * ub), 0.0, 1.0);
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
    vec3 rgb = vec3(yb + 1.5748 * vb,
                    yb - 0.1873 * ub - 0.4681 * vb,
                    yb + 1.8556 * ub);
    return clamp(rgb, 0.0, 1.0);
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
#else
vec3 Grade(vec3 c) { return c; }
#endif

void main() {
    vec2 uv = pc.uvOffset + vUV * pc.uvScale;
    vec3 c = Grade(SampleSource(uv));

#ifdef MP_GRADE
    if (pc.qcMode == 1) {          // eye difference: alignment / sync check
        vec3 o = Grade(SampleSource(uv + pc.otherDelta));
        c = clamp(abs(c - o) * 4.0, 0.0, 1.0);
    } else if (pc.qcMode == 2) {   // anaglyph: red = this (left) eye, cyan = the other
        vec3 o = Grade(SampleSource(uv + pc.otherDelta));
        float l = dot(c, vec3(0.299, 0.587, 0.114));
        float r = dot(o, vec3(0.299, 0.587, 0.114));
        c = vec3(l, r, r);
    }
#endif

    float a = pc.mode == 0 ? texture(plane0, uv).a : 1.0;
    outColor = EncodeForTarget(vec4(c, a));
}
