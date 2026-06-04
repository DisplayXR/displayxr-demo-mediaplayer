// SPDX-License-Identifier: BSL-1.0
// Sample one half of a side-by-side stereo texture. uvOffset/uvScale select the
// half: left eye = offset(0,0) scale(0.5,1), right eye = offset(0.5,0) scale(0.5,1).
// A 2D (non-SBS) source uses offset(0,0) scale(1,1). Half-SBS needs no special case:
// mapping a 0.5-wide UV span onto the full tile horizontally upscales it for free.
#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D uTex;

layout(push_constant) uniform PushConstants {
    vec2 uvOffset;
    vec2 uvScale;
} pc;

void main() {
    vec2 uv = pc.uvOffset + vUV * pc.uvScale;
    outColor = texture(uTex, uv);
}
