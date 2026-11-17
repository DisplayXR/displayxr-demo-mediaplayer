// SPDX-License-Identifier: Apache-2.0
// The 3D grid's tiles (#180, stereo_grid.h): one instanced quad per tile, no vertex
// buffer for the corners. Per instance: the tile's rect in this view (normalised over the
// view's tile of the atlas), the page uv of the eye this view shows, and the border fade.
#version 450

layout(location = 0) in vec4 inRect;  // x0, y0, x1, y1 in [0, 1] of the view
layout(location = 1) in vec4 inUv;    // u0, v0, u1, v1 in the thumbnail page
layout(location = 2) in vec2 inFade;  // the border fade, as a fraction of the tile's w, h

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec2 vLocal;  // [0, 1] across the tile
layout(location = 2) flat out vec2 vFade;

void main() {
    // A triangle strip: (0,0) (1,0) (0,1) (1,1).
    vec2 c = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    gl_Position = vec4(mix(inRect.xy, inRect.zw, c) * 2.0 - 1.0, 0.0, 1.0);
    vUV = mix(inUv.xy, inUv.zw, c);
    vLocal = c;
    vFade = inFade;
}
