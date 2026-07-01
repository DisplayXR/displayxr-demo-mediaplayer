#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Compose the idle screen art: the canonical DisplayXR mark with a bold
# "Media Player" label centered below it, on a transparent canvas (the app
# alpha-composites it over the dark-grey idle backdrop). The logo + text are
# centered as one group so it reads as a single, professional lockup.
#
# Output: displayxr/idle.png  (loaded by App::LoadIdleLogo, ahead of logo.png).
# Run from the repo root:  python3 scripts/make_idle_logo.py
import os
from PIL import Image, ImageFont, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LOGO = os.path.join(ROOT, "displayxr", "logo.png")   # canonical white DisplayXR mark
OUT  = os.path.join(ROOT, "displayxr", "idle.png")

LABEL      = "Media Player"
CANVAS     = 1600          # square, transparent
LOGO_W     = 720           # rendered logo width (px)
GAP        = 56            # px between logo bottom and text top
FONT_PX    = 150
TEXT_RGB   = (244, 246, 250)   # match the logo's near-white
TRACKING   = 6                 # extra px between glyphs for a calmer, premium feel


def load_bold_font(size):
    # Prefer a true bold sans; SF / Helvetica Neue read most "Apple-native".
    for path, variation in [
        ("/System/Library/Fonts/SFNS.ttf", "Bold"),
        ("/System/Library/Fonts/HelveticaNeue.ttc", "Bold"),
        ("/System/Library/Fonts/Supplemental/Arial Bold.ttf", None),
        ("/System/Library/Fonts/Helvetica.ttc", "Bold"),
    ]:
        try:
            f = ImageFont.truetype(path, size)
        except Exception:
            continue
        if variation:
            try:
                f.set_variation_by_name(variation)
            except Exception:
                pass  # static face — already bold or no named instances
        return f, path
    raise SystemExit("ERROR: no bold sans font found")


def text_size(draw, text, font, tracking):
    # Width with manual tracking; height from the font ascent/descent.
    w = sum(draw.textlength(ch, font=font) for ch in text) + tracking * (len(text) - 1)
    asc, desc = font.getmetrics()
    return int(round(w)), asc + desc


def draw_tracked(draw, x, y, text, font, fill, tracking):
    for ch in text:
        draw.text((x, y), ch, font=font, fill=fill)
        x += draw.textlength(ch, font=font) + tracking


def main():
    logo = Image.open(LOGO).convert("RGBA")
    lw = LOGO_W
    lh = int(round(LOGO_W * logo.height / logo.width))
    logo = logo.resize((lw, lh), Image.LANCZOS)

    canvas = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    draw = ImageDraw.Draw(canvas)
    font, fpath = load_bold_font(FONT_PX)

    tw, th = text_size(draw, LABEL, font, TRACKING)
    group_h = lh + GAP + th
    top = (CANVAS - group_h) // 2

    # Logo centered horizontally.
    canvas.alpha_composite(logo, ((CANVAS - lw) // 2, top))

    # Label centered horizontally, just below the logo. Draw at the ascent line.
    asc, _ = font.getmetrics()
    tx = (CANVAS - tw) // 2
    ty = top + lh + GAP
    draw_tracked(draw, tx, ty - asc + asc, LABEL, font, TEXT_RGB + (255,), TRACKING)

    canvas.save(OUT)
    print("wrote", OUT, "using", os.path.basename(fpath),
          "| logo %dx%d, text %dx%d, group_h=%d" % (lw, lh, tw, th, group_h))


if __name__ == "__main__":
    main()
