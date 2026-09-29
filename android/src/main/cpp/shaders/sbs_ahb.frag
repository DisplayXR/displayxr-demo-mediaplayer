// SPDX-License-Identifier: Apache-2.0
// Zero-copy video path (Android): the source is the decoder's AHardwareBuffer
// imported into Vulkan and sampled through an IMMUTABLE VkSamplerYcbcrConversion
// — so binding 0 already returns RGB (the conversion did YUV->RGB + range expand
// in fixed-function hardware). This frag therefore just selects the SBS half
// (uvOffset/uvScale, same as sbs.frag) and emits the sampled colour. The combined
// image sampler MUST use the immutable ycbcr sampler baked into the set layout.
//
// Separate from sbs.frag because a ycbcr-conversion sampler requires an immutable
// sampler in the descriptor layout and a single binding, whereas sbs.frag's CPU
// path declares three plane samplers and does the convert in-shader.
#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D src;  // AHB ycbcr -> RGB (immutable sampler)

// Same push-constant layout as sbs.frag so the pipeline layout is identical;
// mode/fullRange are unused here (the ycbcr conversion owns range/model).
layout(push_constant) uniform PushConstants {
    vec2 uvOffset;
    vec2 uvScale;
    int mode;
    float fullRange;
    float srgbTarget; // 1 = the colour attachment is an _SRGB view (encodes on store)
} pc;

// sRGB EOTF, same as sbs.frag. The ycbcr conversion emits R'G'B' — display-referred —
// so on an _SRGB attachment decode first and the hardware's encode on store restores
// the same bytes the UNORM path stored (runtime #1589 / #1623).
vec3 DisplayReferredToSceneLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

void main() {
    vec2 uv = pc.uvOffset + vUV * pc.uvScale;
    vec3 rgb = texture(src, uv).rgb;
    if (pc.srgbTarget > 0.5) rgb = DisplayReferredToSceneLinear(clamp(rgb, 0.0, 1.0));
    outColor = vec4(rgb, 1.0);
}
