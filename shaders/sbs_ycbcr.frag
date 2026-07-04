// SPDX-License-Identifier: Apache-2.0
// Zero-copy video path (Windows, issue #28): the source is a D3D11VA-decoded NV12
// surface imported into Vulkan and sampled through an IMMUTABLE VkSamplerYcbcrConversion
// — so binding 0 already returns RGB (the conversion did YUV->RGB + range expand in
// fixed-function hardware). This frag just selects the SBS half (uvOffset/uvScale, same
// as sbs.frag) and emits the sampled colour. The combined image sampler MUST use the
// immutable ycbcr sampler baked into the set layout.
//
// Separate from sbs.frag because a ycbcr-conversion sampler requires an immutable sampler
// in the descriptor layout and a single binding, whereas sbs.frag's CPU path declares
// three plane samplers and does the convert in-shader. Mirrors android/.../sbs_ahb.frag
// but samples a concrete VK_FORMAT_G8_B8R8_2PLANE_420_UNORM (Android uses an opaque
// external format).
#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D src;  // imported NV12 ycbcr -> RGB (immutable sampler)

// Same push-constant layout as sbs.frag so the pipeline layout is identical;
// mode/fullRange are unused here (the ycbcr conversion owns range/model).
layout(push_constant) uniform PushConstants {
    vec2 uvOffset;
    vec2 uvScale;
    int mode;
    float fullRange;
} pc;

void main() {
    vec2 uv = pc.uvOffset + vUV * pc.uvScale;
    outColor = vec4(texture(src, uv).rgb, 1.0);
}
