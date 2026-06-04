#!/usr/bin/env python3
# SPDX-License-Identifier: BSL-1.0
# Regenerate src/rhi/Font.h — an antialiased monospace font atlas (ASCII 32..126),
# grayscale coverage. Run from the repo root: python3 scripts/gen_font.py
from PIL import Image, ImageFont, ImageDraw
cands=["/System/Library/Fonts/SFNSMono.ttf","/System/Library/Fonts/Menlo.ttc",
       "/System/Library/Fonts/Monaco.ttf","/System/Library/Fonts/Courier.ttc"]
font=fpath=None
for c in cands:
    try: font=ImageFont.truetype(c,22); fpath=c; break
    except Exception: pass
assert font, "no monospace font found"
asc,desc=font.getmetrics(); cellH=asc+desc; cellW=int(round(font.getlength("M")))
glyphs=[]
for code in range(32,127):
    img=Image.new("L",(cellW,cellH),0)
    ImageDraw.Draw(img).text((0,0),chr(code),fill=255,font=font)
    glyphs.append(list(img.getdata()))
with open("src/rhi/Font.h","w") as f:
    f.write("// SPDX-License-Identifier: BSL-1.0\n")
    f.write("// GENERATED antialiased monospace font atlas (ASCII 32..126), grayscale coverage.\n")
    f.write("// Regenerate with scripts/gen_font.py (needs Pillow).\n#pragma once\n#include <cstdint>\n\n")
    f.write("static const int g_fontCellW = %d;\n" % cellW)
    f.write("static const int g_fontCellH = %d;\n\n" % cellH)
    f.write("// g_fontAtlas[c-32][y*cellW + x] = coverage 0..255.\n")
    f.write("static const uint8_t g_fontAtlas[95][%d] = {\n" % (cellW*cellH))
    for g in glyphs:
        f.write("  {"+",".join(str(v) for v in g)+"},\n")
    f.write("};\n")
print("wrote src/rhi/Font.h from", fpath)
