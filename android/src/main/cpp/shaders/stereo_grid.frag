// SPDX-License-Identifier: Apache-2.0
// The 3D grid's tiles (#180): the eye's crop, fading to the flat background over a thin
// border. The fade is the same in both views, so every tile's edge sits at the screen
// plane and a border never cuts something in front of it.
#version 450

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec2 vLocal;
layout(location = 2) flat in vec2 vFade;
layout(location = 0) out vec4 outColor;

// Set 0 binding 0: the thumbnail page's ImGui descriptor set, used as is.
layout(set = 0, binding = 0) uniform sampler2D page;

layout(push_constant) uniform PushConstants {
    vec4 background;   // display-referred
    float srgbTarget;  // 1 = the colour attachment is an _SRGB view (encodes on store)
} pc;

// sRGB EOTF, as sbs.frag: the page holds encoded bytes sampled through a UNORM view.
vec3 DisplayReferredToSceneLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

void main() {
    vec3 c = texture(page, vUV).rgb;
    vec2 edge = min(vLocal, 1.0 - vLocal) / max(vFade, vec2(1e-5));
    vec2 k = smoothstep(0.0, 1.0, clamp(edge, 0.0, 1.0));
    c = mix(pc.background.rgb, c, k.x * k.y);
    if (pc.srgbTarget > 0.5) c = DisplayReferredToSceneLinear(clamp(c, 0.0, 1.0));
    outColor = vec4(c, 1.0);
}
