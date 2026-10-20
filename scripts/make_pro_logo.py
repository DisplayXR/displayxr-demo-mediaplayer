"""Media Player Pro logo: a 2D icon + a side-by-side (sbs-lr) 3D icon with real per-layer depth.

Layers, back to front: graded backdrop + colour bars (behind the screen), viewfinder
frame lines (on the screen), play triangle (in front).
Usage: python scripts/make_pro_logo.py displayxr
"""
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

SS = 4                 # supersampling
N = 512                # output size of one view
S = N * SS             # working size


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(len(a)))


def backdrop():
    im = Image.new("RGBA", (S, S))
    px = im.load()
    top, bot = (24, 30, 46), (8, 10, 18)
    cx, cy = S * 0.5, S * 0.42
    for y in range(0, S, 2):
        base = lerp(top, bot, y / S)
        for x in range(0, S, 2):
            d = math.hypot(x - cx, y - cy) / (S * 0.75)
            glow = max(0.0, 1.0 - d) ** 2
            c = tuple(min(255, int(base[i] + glow * (18, 24, 44)[i])) for i in range(3)) + (255,)
            px[x, y] = c
            if x + 1 < S:
                px[x + 1, y] = c
            if y + 1 < S:
                px[x, y + 1] = c
                if x + 1 < S:
                    px[x + 1, y + 1] = c
    return im


def colour_bars():
    """SMPTE-style 75 % bars as a soft band low in the frame (behind the screen)."""
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    bars = [(191, 191, 191), (191, 191, 0), (0, 191, 191), (0, 191, 0),
            (191, 0, 191), (191, 0, 0), (0, 0, 191)]
    x0, x1 = S * 0.30, S * 0.70
    y0, y1 = S * 0.76, S * 0.795
    w = (x1 - x0) / len(bars)
    for i, c in enumerate(bars):
        d.rectangle([x0 + i * w, y0, x0 + (i + 1) * w, y1], fill=c + (150,))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([x0, y0, x1, y1], radius=int(S * 0.012), fill=255)
    im.putalpha(Image.composite(im.getchannel("A"), Image.new("L", (S, S), 0), mask))
    return im


def frame_lines():
    """Viewfinder corner brackets + a centre tick: the 'monitor' cue."""
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    m, L, t = S * 0.14, S * 0.15, int(S * 0.028)
    col = (236, 240, 248, 255)
    r = t // 2
    for (cx, sx) in ((m, 1), (S - m, -1)):
        for (cy, sy) in ((m, 1), (S - m, -1)):
            d.line([(cx, cy + sy * L), (cx, cy), (cx + sx * L, cy)], fill=col, width=t, joint="curve")
            for p in ((cx, cy + sy * L), (cx + sx * L, cy)):
                d.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=col)
    return im


def play_triangle():
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    cx, cy, h = S * 0.525, S * 0.46, S * 0.42
    w = h * math.sqrt(3) / 2
    pts = [(cx - w / 2, cy - h / 2), (cx - w / 2, cy + h / 2), (cx + w / 2, cy)]
    rr = int(S * 0.035)  # corner rounding: fill + a thick curved outline
    mask = Image.new("L", (S, S), 0)
    md = ImageDraw.Draw(mask)
    # shrink toward the centroid by rr so the rounded outline lands on the true size
    gx, gy = sum(p[0] for p in pts) / 3, sum(p[1] for p in pts) / 3
    inner = [(gx + (x - gx) * 0.86, gy + (y - gy) * 0.86) for x, y in pts]
    md.polygon(inner, fill=255)
    md.line(inner + [inner[0]], fill=255, width=rr * 2, joint="curve")
    for x, y in inner:
        md.ellipse([x - rr, y - rr, x + rr, y + rr], fill=255)
    # cyan -> violet diagonal gradient
    grad = Image.new("RGBA", (S, S))
    gp = grad.load()
    a, b = (64, 214, 255), (150, 92, 255)
    for y in range(0, S, 4):
        for x in range(0, S, 4):
            t = min(1.0, max(0.0, ((x - S * 0.35) + (y - S * 0.30)) / (S * 0.6)))
            c = lerp(a, b, t) + (255,)
            for dy in range(4):
                for dx in range(4):
                    if x + dx < S and y + dy < S:
                        gp[x + dx, y + dy] = c
    shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    shadow.putalpha(mask.point(lambda v: v * 0.55))
    shadow = shadow.filter(ImageFilter.GaussianBlur(S * 0.02))
    glow = Image.new("RGBA", (S, S), (90, 160, 255, 0))
    glow.putalpha(mask.filter(ImageFilter.GaussianBlur(S * 0.03)).point(lambda v: v * 0.45))
    tri = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    tri.paste(grad, (0, 0), mask)
    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    out.alpha_composite(glow)
    out.alpha_composite(shadow, (0, int(S * 0.012)))
    out.alpha_composite(tri)
    return out


# disparity in OUTPUT pixels (positive = in front of the screen)
LAYERS = [
    (backdrop, -3.0),
    (colour_bars, -5.0),
    (frame_lines, 0.0),
    (play_triangle, 6.0),
]


def render(eye):
    """eye: 0 = mono, -1 = left, +1 = right."""
    out = Image.new("RGBA", (S, S), (0, 0, 0, 255))
    for fn, disp in LAYERS:
        layer = CACHE[fn]
        # crossed disparity: in front -> left view shifted right, right view shifted left
        shift = int(round(-eye * disp * SS / 2))
        if fn is backdrop:
            # the backdrop is full-bleed: shift by stretching a hair so edges stay filled
            if shift:
                big = layer.resize((S + abs(shift) * 2, S), Image.LANCZOS)
                layer = big.crop((abs(shift) - shift, 0, abs(shift) - shift + S, S))
            out.alpha_composite(layer)
        else:
            out.alpha_composite(layer, (shift, 0))
    return out.convert("RGB").resize((N, N), Image.LANCZOS)


if __name__ == "__main__":
    dst = Path(sys.argv[1])
    dst.mkdir(parents=True, exist_ok=True)
    CACHE = {fn: fn() for fn, _ in LAYERS}
    mono = render(0)
    mono.save(dst / "icon_pro.png")
    sbs = Image.new("RGB", (N * 2, N))
    sbs.paste(render(-1), (0, 0))
    sbs.paste(render(1), (N, 0))
    sbs.save(dst / "icon_pro_sbs.png")
    mono.save(dst / "icon_pro.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print("wrote", dst)
