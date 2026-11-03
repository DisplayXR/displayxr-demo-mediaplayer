#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Synthetic VR180 / VR360 test media for the VR viewer (#155).

Renders one synthetic scene - coloured balls at 1.2 to 4 m in front of a far dome ruled
with latitude / longitude lines - from two cameras on a 63 mm horizontal baseline, in the
projections the player reads:

  vr180 equirect, side by side    (YouTube VR180; sv3d bounded equirect + st3d left-right)
  vr180 dual fisheye, side by side (a raw VR180 camera; 190 degree equidistant circles)
  vr360 equirect, top-bottom       (omni-directional stereo, as stitched 360 rigs deliver;
                                    sv3d full equirect + st3d top-bottom)

The ruled dome is the eyeball check: at every pan the latitude lines must sit on the same
row in both eyes, and the balls must float in front of the dome.

Two modes:

  --fixtures        tiny clips committed under tests/fixtures/ (a few KB each): the
                    container metadata is what is under test there, not the picture.
  --assets DIR      full-size stills (PNG) and short clips (MP4) for live runs and
                    captures. Not committed (assets/media/ is git-ignored).

Needs python3 + numpy, and ffmpeg with libx264 on PATH. The MP4 / MOV spherical metadata
(Spherical Video V2: `st3d` + `sv3d` / `proj` / `equi`, inside the visual sample entry)
is written by this script - the ffmpeg CLI cannot set it - into a file whose `moov` sits
after `mdat` (ffmpeg's default), so no chunk offset moves.
"""
import argparse
import os
import struct
import subprocess
import sys

import numpy as np

BASELINE = 0.063
FISHEYE_FOV = 190.0

# (centre x, y, z in metres, radius, rgb)
BALLS = [
    ((0.0, 0.0, 1.6), 0.18, (230, 60, 50)),
    ((-0.9, -0.25, 2.2), 0.22, (60, 200, 80)),
    ((1.1, 0.35, 2.6), 0.25, (60, 110, 230)),
    ((-2.2, 0.5, 2.0), 0.25, (240, 200, 40)),
    ((2.6, -0.4, 1.8), 0.25, (220, 80, 220)),
    ((0.4, 0.9, 3.5), 0.3, (40, 220, 220)),
    ((-0.5, -0.8, 1.2), 0.12, (250, 140, 40)),
]
DOME_R = 30.0


def shade(origin, dirs, t_offset=0.0):
    """Trace rays: origin (N,3) or (3,), dirs (N,3) unit. Returns (N,3) uint8."""
    n = dirs.shape[0]
    o = np.broadcast_to(origin, (n, 3)).astype(np.float64)
    best = np.full(n, np.inf)
    col = np.zeros((n, 3), np.float64)
    for i, (c, r, rgb) in enumerate(BALLS):
        c = np.array(c, np.float64)
        if i == 0:
            c = c + np.array([0.25 * np.sin(t_offset), 0.0, 0.0])  # the clip's moving ball
        oc = o - c
        b = np.einsum('ij,ij->i', oc, dirs)
        cc = np.einsum('ij,ij->i', oc, oc) - r * r
        disc = b * b - cc
        hit = disc > 0
        t = -b - np.sqrt(np.where(hit, disc, 0.0))
        hit &= (t > 0) & (t < best)
        if np.any(hit):
            p = o[hit] + dirs[hit] * t[hit, None]
            nrm = (p - c) / r
            lam = np.clip(nrm @ np.array([-0.4, 0.6, -0.7]) / np.linalg.norm([-0.4, 0.6, -0.7]), 0, 1)
            col[hit] = np.array(rgb) * (0.35 + 0.65 * lam[:, None])
            best[hit] = t[hit]
    miss = ~np.isfinite(best)
    if np.any(miss):
        # The far dome: origin is ~0, so its direction is the ray's. Grey with 10-degree
        # longitude lines, 10-degree latitude lines, and a brighter horizon.
        d = dirs[miss]
        lon = np.degrees(np.arctan2(d[:, 0], d[:, 2]))
        lat = np.degrees(np.arctan2(d[:, 1], np.hypot(d[:, 0], d[:, 2])))
        base = 70 + 30 * (lat / 90.0)
        g = np.stack([base, base, base * 1.1], 1)
        # Longitude lines stop short of the poles, where they would merge into one blot.
        line = (np.abs(((lat + 5) % 10) - 5) < 0.35) | (
            (np.abs(lat) < 75) & (np.abs(((lon + 5) % 10) - 5) < 0.35 / np.maximum(np.cos(np.radians(lat)), 0.25)))
        g[line] = (225, 225, 225)
        g[np.abs(lat) < 0.5] = (255, 230, 120)
        col[miss] = g
    return np.clip(col, 0, 255).astype(np.uint8)


def equirect_dirs(w, h, cover_deg):
    u = (np.arange(w) + 0.5) / w
    v = (np.arange(h) + 0.5) / h
    lon = (u - 0.5) * np.radians(cover_deg)
    lat = (0.5 - v) * np.pi
    lon, lat = np.meshgrid(lon, lat)
    d = np.stack([np.cos(lat) * np.sin(lon), np.sin(lat), np.cos(lat) * np.cos(lon)], -1)
    return d.reshape(-1, 3), lon.reshape(-1)


def render_equirect180(w, t=0.0):
    eyes = []
    d, _ = equirect_dirs(w, w, 180.0)
    for cx in (-BASELINE / 2, BASELINE / 2):
        eyes.append(shade(np.array([cx, 0, 0]), d, t).reshape(w, w, 3))
    return np.concatenate(eyes, 1)  # SBS: left | right


def render_fisheye(w, t=0.0):
    u = (np.arange(w) + 0.5) / w
    uu, vv = np.meshgrid(u, u)
    dx, dy = uu - 0.5, 0.5 - vv
    r = np.hypot(dx, dy)
    th = r / 0.5 * np.radians(FISHEYE_FOV) / 2
    cs = np.where(r > 1e-9, dx / np.maximum(r, 1e-9), 0)
    sn = np.where(r > 1e-9, dy / np.maximum(r, 1e-9), 0)
    d = np.stack([np.sin(th) * cs, np.sin(th) * sn, np.cos(th)], -1).reshape(-1, 3)
    inside = (th <= np.radians(FISHEYE_FOV) / 2).reshape(-1)
    eyes = []
    for cx in (-BASELINE / 2, BASELINE / 2):
        img = shade(np.array([cx, 0, 0]), d, t)
        img[~inside] = 0
        eyes.append(img.reshape(w, w, 3))
    return np.concatenate(eyes, 1)


def render_ods360(w, t=0.0):
    """Omni-directional stereo: column azimuth th is seen from the circle of radius b/2."""
    h = w // 2
    d, lon = equirect_dirs(w, h, 360.0)
    r = BASELINE / 2
    eyes = []
    for sg in (-1.0, 1.0):  # left, right
        o = sg * r * np.stack([np.cos(lon), np.zeros_like(lon), -np.sin(lon)], -1)
        eyes.append(shade(o, d, t).reshape(h, w, 3))
    return np.concatenate(eyes, 0)  # TB: left on top


# ---- Spherical Video V2 boxes ------------------------------------------------------

def box(typ, payload):
    return struct.pack('>I4s', 8 + len(payload), typ) + payload


def fullbox(typ, payload, version=0, flags=0):
    return box(typ, struct.pack('>I', (version << 24) | flags) + payload)


def spherical_boxes(stereo_mode, bounds):
    """stereo_mode: 0 mono, 1 top-bottom, 2 left-right. bounds: (top, bottom, left, right)
    as fractions of the full 360 x 180 frame cropped away."""
    st3d = fullbox(b'st3d', struct.pack('>B', stereo_mode))
    svhd = fullbox(b'svhd', b'displayxr-demo-mediaplayer tools/vr\x00')
    prhd = fullbox(b'prhd', struct.pack('>iii', 0, 0, 0))
    fx = [min(0xFFFFFFFF, int(round(b * 4294967296.0))) for b in bounds]
    equi = fullbox(b'equi', struct.pack('>IIII', *fx))
    sv3d = box(b'sv3d', svhd + box(b'proj', prhd + equi))
    return st3d + sv3d


CONTAINERS = {b'moov', b'trak', b'mdia', b'minf', b'stbl'}


def inject(path, extra):
    data = bytearray(open(path, 'rb').read())

    def walk(start, end, chain):
        pos = start
        while pos + 8 <= end:
            size, typ = struct.unpack('>I4s', data[pos:pos + 8])
            if size < 8:
                raise SystemExit('unsupported box size in ' + path)
            if typ in CONTAINERS:
                r = walk(pos + 8, pos + size, chain + [pos])
                if r:
                    return r
            elif typ == b'stsd':
                # FullBox + entry_count, then the first sample entry.
                entry = pos + 16
                esize, etyp = struct.unpack('>I4s', data[entry:entry + 8])
                if etyp in (b'avc1', b'avc3', b'hvc1', b'hev1', b'mp4v'):
                    return chain + [pos, entry], entry + esize
            pos += size
        return None

    found = walk(0, len(data), [])
    if not found:
        raise SystemExit('no visual sample entry in ' + path)
    parents, insert_at = found
    data[insert_at:insert_at] = extra
    for p in parents:
        size = struct.unpack('>I', data[p:p + 4])[0]
        data[p:p + 4] = struct.pack('>I', size + len(extra))
    # moov after mdat (ffmpeg's default): no chunk offset moved. Refuse the other order.
    moov = data.find(b'moov') - 4
    mdat = data.find(b'mdat') - 4
    if 0 <= moov < mdat:
        raise SystemExit('moov precedes mdat in ' + path + ' (do not use +faststart)')
    open(path, 'wb').write(bytes(data))


def ffmpeg_encode(frames, path, fps=24):
    h, w, _ = frames[0].shape
    cmd = ['ffmpeg', '-y', '-v', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{w}x{h}', '-r', str(fps),
           '-i', '-']
    if path.endswith('.png'):
        cmd += ['-frames:v', '1', path]
    else:
        # zerolatency: no B-frames / lookahead, so even a tiny fixture decodes a frame
        # before EOF (the probe reads the first decoded frame's stereo side data).
        cmd += ['-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-preset', 'veryfast', '-tune', 'zerolatency', '-crf', '20',
                path]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    for f in frames:
        p.stdin.write(np.ascontiguousarray(f).tobytes())
    p.stdin.close()
    if p.wait() != 0:
        raise SystemExit('ffmpeg failed for ' + path)


def make(kind, eye_w, path, frames=1):
    render = {'vr180': render_equirect180, 'fisheye': render_fisheye, 'vr360': render_ods360}[kind]
    imgs = [render(eye_w, 2 * np.pi * i / max(1, frames)) for i in range(frames)]
    ffmpeg_encode(imgs, path)
    if path.endswith(('.mp4', '.mov')):
        if kind == 'vr180':
            inject(path, spherical_boxes(2, (0.0, 0.0, 0.25, 0.25)))
        elif kind == 'vr360':
            inject(path, spherical_boxes(1, (0.0, 0.0, 0.0, 0.0)))
        # fisheye: Spherical Video V2 has no fisheye box; the player reads the name.
    print(f'  {path}  ({os.path.getsize(path)} bytes)')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--fixtures', action='store_true', help='write the tiny tests/fixtures/ clips')
    ap.add_argument('--assets', metavar='DIR', help='write full-size stills + clips into DIR')
    a = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    if not a.fixtures and not a.assets:
        ap.error('give --fixtures and/or --assets DIR')
    if a.fixtures:
        out = os.path.join(root, 'tests', 'fixtures')
        print('fixtures ->', out)
        make('vr180', 64, os.path.join(out, 'vr180_sbs_sv3d.mov'), frames=24)
        make('vr360', 64, os.path.join(out, 'vr360_tb_sv3d.mov'), frames=24)
    if a.assets:
        os.makedirs(a.assets, exist_ok=True)
        print('assets ->', a.assets)
        make('vr180', 1440, os.path.join(a.assets, 'synth_vr180_LR.png'))
        make('vr180', 1440, os.path.join(a.assets, 'synth_vr180_sv3d.mp4'), frames=48)
        make('fisheye', 1440, os.path.join(a.assets, 'synth_fisheye_LR.png'))
        make('fisheye', 1440, os.path.join(a.assets, 'synth_dualfisheye_LR.mp4'), frames=48)
        make('vr360', 2048, os.path.join(a.assets, 'synth_vr360_TB.png'))
        make('vr360', 2048, os.path.join(a.assets, 'synth_vr360_sv3d.mp4'), frames=48)
    return 0


if __name__ == '__main__':
    sys.exit(main())
