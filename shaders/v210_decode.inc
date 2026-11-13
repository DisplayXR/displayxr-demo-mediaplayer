// SPDX-License-Identifier: Apache-2.0
//
// v210 -> P016: the work of one compute invocation (#238). ONE text, two compilers: the GPU
// decode (shaders/v210_to_p016.comp) and its C++ twin (src/media/V210.h: the unit test's
// reference and the renderer's CPU fallback) both #include this file, so the test checks the
// shader's own arithmetic, not a copy of it. Everything below is valid GLSL 450 and valid
// C++ once the includer has defined:
//   uint                 C++: an alias of uint32_t
//   V210_LOAD(eye, i)    32-bit word i of eye `eye`'s v210 rows (eye 0 = left)
//   V210_STORE(i, v)     32-bit word i of the P016 output: the Y plane's words, then (from
//                        `uvBase`) the interleaved CbCr plane's words
//
// v210 (SMPTE / Apple / Blackmagic): 10-bit 4:2:2, 6 pixels in four little-endian 32-bit
// words, three 10-bit fields each, low bits first:
//   w0 = Cb0 Y0 Cr0 | w1 = Y1 Cb2 Y2 | w2 = Cr2 Y3 Cb4 | w3 = Y4 Cr4 Y5
// A row is the width rounded up to 48 pixels (8 groups = 128 bytes), so a width that is not
// a multiple of 6 ends in a group whose missing pixels sit in the row's padding.
//
// The output is what livepix::V210ToP016 (src/pro/LivePixels.cpp) writes, bit for bit:
// 16-bit samples, MSB-aligned (code << 6); 4:2:2 -> 4:2:0 by averaging the two rows'
// chroma, rounding half up ((a + b + 1) >> 1). The colour matrix and the limited range are
// NOT applied here: the frame's tags go to the MP_GRADE shader exactly as for a P016 frame.

// Field k (0..2) of a v210 word.
uint v210Field(uint w, uint k) { return (w >> (10u * k)) & 0x3FFu; }

// Luma of pixel k (0..5) of a group.
uint v210Y(uint w0, uint w1, uint w2, uint w3, uint k) {
    return k == 0u   ? v210Field(w0, 1u)
           : k == 1u ? v210Field(w1, 0u)
           : k == 2u ? v210Field(w1, 2u)
           : k == 3u ? v210Field(w2, 1u)
           : k == 4u ? v210Field(w3, 0u)
                     : v210Field(w3, 2u);
}
// Cb / Cr of chroma pair j (0..2: pixels 2j and 2j + 1) of a group.
uint v210Cb(uint w0, uint w1, uint w2, uint j) {
    return j == 0u ? v210Field(w0, 0u) : j == 1u ? v210Field(w1, 1u) : v210Field(w2, 2u);
}
uint v210Cr(uint w0, uint w2, uint w3, uint j) {
    return j == 0u ? v210Field(w0, 2u) : j == 1u ? v210Field(w2, 0u) : v210Field(w3, 1u);
}

// Group g (pixels 6g .. 6g+5) of row pair r (rows 2r and 2r + 1) of one eye: both rows' luma
// and the pair's chroma.
//   eyeW        the eye's width in pixels (even)
//   pitchWords  one v210 row of the eye, in 32-bit words (RowBytes(eyeW) / 4)
//   dstX        the eye's first column in the output frame (even)
//   outW        the output frame's width (even); a P016 row is outW / 2 words, in both planes
//   uvBase      the CbCr plane's first word in the output
void v210DecodeGroup(uint eye, uint g, uint r, uint eyeW, uint pitchWords, uint dstX, uint outW, uint uvBase) {
    if (6u * g >= eyeW) return;
    uint a = 2u * r * pitchWords + 4u * g;  // the group in the upper row
    uint b = a + pitchWords;                // ... and in the lower one
    uint a0 = V210_LOAD(eye, a);
    uint a1 = V210_LOAD(eye, a + 1u);
    uint a2 = V210_LOAD(eye, a + 2u);
    uint a3 = V210_LOAD(eye, a + 3u);
    uint b0 = V210_LOAD(eye, b);
    uint b1 = V210_LOAD(eye, b + 1u);
    uint b2 = V210_LOAD(eye, b + 2u);
    uint b3 = V210_LOAD(eye, b + 3u);
    uint n = eyeW - 6u * g;  // pixels of this group inside the eye (even); no min(): a macro on Windows
    if (n > 6u) n = 6u;
    uint rowWords = outW / 2u;
    // Luma: two 16-bit samples per output word.
    uint ya = 2u * r * rowWords + (dstX + 6u * g) / 2u;
    uint yb = ya + rowWords;
    for (uint i = 0u; 2u * i < n; ++i) {
        V210_STORE(ya + i, (v210Y(a0, a1, a2, a3, 2u * i) << 6u) | (v210Y(a0, a1, a2, a3, 2u * i + 1u) << 22u));
        V210_STORE(yb + i, (v210Y(b0, b1, b2, b3, 2u * i) << 6u) | (v210Y(b0, b1, b2, b3, 2u * i + 1u) << 22u));
    }
    // Chroma: one Cb,Cr pair per output word, the two rows averaged.
    uint uv = uvBase + r * rowWords + dstX / 2u + 3u * g;
    for (uint j = 0u; 2u * j < n; ++j) {
        uint cb = ((v210Cb(a0, a1, a2, j) + v210Cb(b0, b1, b2, j) + 1u) >> 1u) << 6u;
        uint cr = ((v210Cr(a0, a2, a3, j) + v210Cr(b0, b2, b3, j) + 1u) >> 1u) << 6u;
        V210_STORE(uv + j, cb | (cr << 16u));
    }
}
