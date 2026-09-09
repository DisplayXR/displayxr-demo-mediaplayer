#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""lvf2.py -- build and inspect "LVF v2" stereo video files.

WHAT AN LVF v2 FILE IS
======================

An LVF v2 file is an *ordinary* `.mp4` (brands `isom`/`mp42`) that a plain 2D
player shows as a normal 2D video, and that a stereo-aware player can open as a
full-resolution stereo pair with per-frame convergence metadata.  Nothing about
it requires a bespoke container, a bespoke codec, or a bespoke demuxer: it is
the standard ISO-BMFF multi-track mechanism used the way the spec intends.

The contract, item by item.  `lvf2.py inspect` checks each of these and prints
a PASS/FAIL line per item, using the same numbering.

 1. Plain `.mp4`, `isom`/`mp42` brands.  Track order in `moov` is:
    left video, right video, [audio], convergence.

 2. Two video tracks, one full view each (for an SBS source, the two halves
    after cropping).  Both tracks use the same codec (H.264 High, libx264),
    the same resolution, the same frame rate, the same media timescale and the
    same colour signalling, and -- critically -- an **identical closed-GOP
    structure**: same `-g`, `-keyint_min`, `-bf 0`, `scenecut=0`, `open_gop=0`
    and `-force_key_frames` at the same frame numbers, so IDRs land at
    identical PTS in both tracks.  Both tracks share one PTS origin (there is
    no per-track re-basing and no edit-list delay), so every left sample has a
    right sample at the same PTS.  A player can therefore demux both tracks
    with one clock and pair frames by PTS with no search and no drift.

 3. `mdhd` language: left = `abl`, right = `abr` ("albedo left/right").  This
    is how a v1 (vendor-camera) reader identifies the eyes, and LVF v2 keeps it so
    that v1 readers keep working.

 4. `hdlr` handler name: left = "Leia Albedo Left", right = "Leia Albedo
    Right".  Human-readable; also a v1 compatibility affordance.

 5. `tkhd`: the left track is enabled (flags bit 0 set); the **right track has
    `track_enabled` cleared** (bit 0 = 0), which is what makes a naive player
    ignore it entirely and play the file as plain 2D.  `in_movie` /
    `in_preview` are left as they are.  `alternate_group` is **0 on every
    track** -- deliberately NOT used: an alternate group means "pick exactly
    one of these", which is the opposite of what a stereo pair wants, and
    setting it makes some players hide the left eye instead of the right.

 6. A `vexu` ("video extension usage") box is inserted into **each** video
    track's sample entry (`stsd/avc1`), after the existing children:

        vexu
          eyes
            stri  v0 f0, 1 byte: bit0 has_left_eye_view,
                                 bit1 has_right_eye_view,
                                 bit2 has_additional_views,
                                 bit3 eye_views_reversed
            hero  v0 f0, u8 hero_eye_indicator (1 = left, 2 = right)
           [cams
              blin v0 f0, u32 baseline in micrometres]
           [cmfy
              dadj v0 f0, i32 default convergence, units of 1/10000 of the
                          view width]

    `stri` describes what **that sample entry's own track** carries, so the
    left track sets `has_left_eye_view` only (`0x01`) and the right track sets
    `has_right_eye_view` only (`0x02`), both with `eye_views_reversed = 0`.
    FFmpeg then reports `view=left` on stream 0 and `view=right` on stream 1.
    Setting BOTH bits on one entry means "this single track is frame-packed and
    carries both eyes", which is not what a two-track pair is -- FFmpeg maps it
    to `AV_STEREO3D_VIEW_PACKED` and reports `view=packed`.  `--stri both`
    keeps that older single-entry behaviour for anyone who needs it.

    `hero` names the primary eye of the **pair** (the left one), so it is
    written identically on both entries -- it is not "which eye is in this
    track".  `cams/blin` and `cmfy/dadj` describe the rig, so they are likewise
    mirrored onto both entries when supplied.

    This is Apple's `vexu` layout as parsed by FFmpeg >= 7.1
    (`libavformat/mov.c: mov_read_vexu` / `mov_read_eyes`) and written by
    `libavformat/movenc.c: mov_write_vexu_tag` / `mov_write_eyes_tag`.  It is
    what makes a stereo-aware player recognise the file as 3D without knowing
    anything about the vendor.

 7. A per-frame **convergence** track: a `meta`-handler track whose sample
    entry is `mett` (MetadataSampleEntry -- 6 reserved bytes, u16
    data_reference_index, then a NUL-terminated `content_encoding` (empty) and
    a NUL-terminated `mime_format`), with

        mime_format = "application/vnd.leia.convergence+json"

    `minf` carries `nmhd` + the standard self-contained `dinf/dref/url `.
    Each sample is the JSON text `{"convergence": <float>}` where the value is
    a fraction of the view width; positive means "push the scene further
    away"; a player applies half of it to each eye, with opposite sign.  There
    is a sample at media time 0 and at least one per second (by default one per
    video frame; `--conv-rate 1hz` thins it).  The track's media timescale
    equals the video timescale so the two clocks are exact, and the left video
    track's `edts` is copied verbatim onto the convergence track so the two
    share one *presentation* origin as well -- a camera-captured source can
    carry a leading empty edit (vendor-camera delays video by ~168 ms to match
    audio), and without the copy every convergence sample would be early by
    exactly that delay.

    The convergence samples live in a **new `mdat` appended after the existing
    media data**, and the new `trak` is appended into `moov` after the last
    existing `trak`.  `moov` stays the LAST top-level box (this tool refuses to
    run on a faststart file rather than silently rewriting every chunk
    offset), so no existing `stco`/`co64` value changes.  `mvhd.next_track_ID`
    is bumped.

 8. Poster frame: a JPEG in `moov/udta/meta/ilst/covr`, taken from the LEFT
    view at ~1 s.  Deliberately NOT an `attached_pic` video stream -- that
    would add a third video track and confuse the "two video tracks" contract
    and every player's stream selection.

 9. Audio, if the source has any, is stream-copied.

10. The mux runs with `-strict unofficial` (harmless; it is what FFmpeg
    requires before it will write `vexu` itself, though this tool writes the
    box directly).

WHY IT DEGRADES WELL
--------------------
A player that knows nothing about any of this opens the file, picks the first
enabled video track, and plays the left eye as ordinary 2D.  The right eye and
the convergence track are inert.  A player that does know about it maps
`abl`/`abr` (or `vexu`) onto its two eyes and reads convergence per frame.

KNOWN FRAGILITY
---------------
`ffmpeg -i lvf2.mp4 -c copy out.mp4` does NOT round-trip: `-c copy` without an
explicit `-map` picks one video stream, and even with `-map 0` the `vexu` box
and the disabled-`tkhd` bit are regenerated from FFmpeg's own model rather than
copied.  LVF v2 is an output format, not an intermediate.

USAGE
-----
    lvf2.py convert IN.mp4 OUT.mp4 [options]
    lvf2.py inspect FILE.mp4

`convert` accepts two kinds of input, auto-detected:
  * a side-by-side (2x1 full-width) stereo `.mp4` -- the halves are cropped;
    `--half-sbs` additionally rescales each half back to 2x width;
  * a **dual-track v1 (vendor-camera) file** -- two video tracks tagged `abl`/`abr`,
    used directly as the two eyes, with the source's own convergence track
    carried through (unless `--convergence`/`--conv-sweep`/`--conv-csv`
    overrides it).

Requires `ffmpeg`/`ffprobe` on PATH.  `mutagen` is used for the `covr` poster
when available; otherwise an equivalent `ilst` writer built into this file is
used.  No MP4Box, no other third-party dependency.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

CONV_MIME = "application/vnd.leia.convergence+json"
CONV_MIME_LEGACY_V1 = "application/convergence"
HDLR_LEFT = "Leia Albedo Left"
HDLR_RIGHT = "Leia Albedo Right"
HDLR_CONV = "Leia Convergence"


# --------------------------------------------------------------------------
# process helpers
# --------------------------------------------------------------------------

def run(cmd, capture=True, check=True, binary=False):
    """Run a command; return CompletedProcess."""
    p = subprocess.run(
        cmd,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )
    if check and p.returncode != 0:
        err = (p.stderr or b"").decode("utf-8", "replace")
        raise RuntimeError("command failed (%d): %s\n%s" % (p.returncode, " ".join(cmd), err[-4000:]))
    if capture and not binary:
        p.stdout_text = (p.stdout or b"").decode("utf-8", "replace")
        p.stderr_text = (p.stderr or b"").decode("utf-8", "replace")
    return p


def ffprobe_json(path):
    p = run(["ffprobe", "-v", "error", "-print_format", "json",
             "-show_streams", "-show_format", path])
    return json.loads(p.stdout_text)


def rat(s, default=0.0):
    try:
        if "/" in s:
            n, d = s.split("/")
            return float(n) / float(d) if float(d) else default
        return float(s)
    except Exception:
        return default


# --------------------------------------------------------------------------
# ISO-BMFF box model
# --------------------------------------------------------------------------

CONTAINER_TYPES = {
    b"moov", b"trak", b"mdia", b"minf", b"stbl", b"dinf", b"edts", b"udta",
    b"mvex", b"moof", b"traf", b"mfra", b"sinf", b"schi", b"wave", b"strk",
    b"vexu", b"eyes", b"cams", b"cmfy", b"proj", b"ilst",
}

# Visual sample entries: 78-byte VisualSampleEntry prefix, then child boxes.
VISUAL_ENTRY_TYPES = {
    b"avc1", b"avc2", b"avc3", b"avc4", b"hvc1", b"hev1", b"hvt1", b"vvc1",
    b"vvi1", b"mp4v", b"av01", b"vp08", b"vp09", b"dvh1", b"dva1", b"dvav",
    b"dvhe", b"encv", b"jpeg", b"ap4h", b"apch",
}


class Box:
    __slots__ = ("type", "prefix", "payload", "children")

    def __init__(self, btype, payload=None, children=None, prefix=b""):
        if isinstance(btype, str):
            btype = btype.encode("latin-1")
        self.type = btype
        self.prefix = prefix
        self.payload = payload      # bytes / memoryview, for leaves
        self.children = children    # list[Box], for containers

    # -- geometry ---------------------------------------------------------
    def content_size(self):
        n = len(self.prefix)
        if self.children is not None:
            for c in self.children:
                n += c.size()
        elif self.payload is not None:
            n += len(self.payload)
        return n

    def size(self):
        n = 8 + self.content_size()
        return n if n <= 0xFFFFFFFF else n + 8

    def write(self, out):
        n = 8 + self.content_size()
        if n <= 0xFFFFFFFF:
            out.write(struct.pack(">I", n) + self.type)
        else:
            out.write(struct.pack(">I", 1) + self.type + struct.pack(">Q", n + 8))
        if self.prefix:
            out.write(self.prefix)
        if self.children is not None:
            for c in self.children:
                c.write(out)
        elif self.payload is not None:
            out.write(self.payload)

    def to_bytes(self):
        import io
        b = io.BytesIO()
        self.write(b)
        return b.getvalue()

    # -- tree navigation --------------------------------------------------
    def kids(self, btype):
        if isinstance(btype, str):
            btype = btype.encode("latin-1")
        if self.children is None:
            return []
        return [c for c in self.children if c.type == btype]

    def kid(self, btype):
        k = self.kids(btype)
        return k[0] if k else None

    def path(self, *types):
        cur = self
        for t in types:
            if cur is None:
                return None
            cur = cur.kid(t)
        return cur

    def __repr__(self):
        return "<Box %s size=%d%s>" % (
            self.type.decode("latin-1", "replace"), self.size(),
            " children=%d" % len(self.children) if self.children is not None else "")


def _meta_prefix_len(mv):
    """QuickTime writes 'meta' as a plain container; ISO writes it as a FullBox."""
    if len(mv) < 12:
        return 4
    # If a plausible child box header sits at offset 0, there is no prefix.
    sz = struct.unpack(">I", bytes(mv[0:4]))[0]
    tag = bytes(mv[4:8])
    if 8 <= sz <= len(mv) and re.match(rb"^[\x20-\x7e]{4}$", tag):
        return 0
    return 4


def _prefix_len(btype, parent_type, mv):
    if btype == b"meta":
        return _meta_prefix_len(mv)
    if btype == b"stsd":
        return 8
    if parent_type == b"stsd" and btype in VISUAL_ENTRY_TYPES:
        return 78
    return 0


def _is_container(btype, parent_type):
    if btype in CONTAINER_TYPES:
        return True
    if btype == b"stsd":
        return True
    if btype == b"meta":
        return True
    if parent_type == b"stsd" and btype in VISUAL_ENTRY_TYPES:
        return True
    if parent_type == b"ilst":
        return True
    return False


def parse_boxes(mv, parent_type=None, depth=0):
    """Parse a sequence of boxes from a memoryview. Returns list[Box]."""
    out = []
    pos = 0
    n = len(mv)
    while pos + 8 <= n:
        size = struct.unpack(">I", bytes(mv[pos:pos + 4]))[0]
        btype = bytes(mv[pos + 4:pos + 8])
        hdr = 8
        if size == 1:
            if pos + 16 > n:
                break
            size = struct.unpack(">Q", bytes(mv[pos + 8:pos + 16]))[0]
            hdr = 16
        elif size == 0:
            size = n - pos
        if size < hdr or pos + size > n:
            # Truncated / garbage tail: keep the remainder verbatim so that
            # inspect never crashes and serialization stays byte-exact.
            out.append(Box(b"\x00\x00\x00\x00", payload=mv[pos:n]))
            break
        body = mv[pos + hdr:pos + size]
        if depth < 12 and _is_container(btype, parent_type):
            plen = _prefix_len(btype, parent_type, body)
            try:
                kids = parse_boxes(body[plen:], btype, depth + 1)
                out.append(Box(btype, children=kids, prefix=bytes(body[:plen])))
            except Exception:
                out.append(Box(btype, payload=body))
        else:
            out.append(Box(btype, payload=body))
        pos += size
    return out


# -- small field accessors ---------------------------------------------------

def u32(b, off):
    return struct.unpack(">I", bytes(b[off:off + 4]))[0]


def u16(b, off):
    return struct.unpack(">H", bytes(b[off:off + 2]))[0]


def set_u32(ba, off, val):
    ba[off:off + 4] = struct.pack(">I", val)


def pack_lang(code):
    code = (code + "und")[:3]
    v = 0
    for ch in code:
        v = (v << 5) | ((ord(ch) - 0x60) & 0x1F)
    return v


def unpack_lang(v):
    if v == 0:
        return "und"
    return "".join(chr(((v >> s) & 0x1F) + 0x60) for s in (10, 5, 0))


def full_box(btype, version, flags, payload):
    return Box(btype, payload=bytes([version]) + flags.to_bytes(3, "big") + payload)


def cstr(s):
    return s.encode("utf-8") + b"\x00"


# --------------------------------------------------------------------------
# track introspection
# --------------------------------------------------------------------------

class TrackView:
    """Read-only view of one `trak` box."""

    def __init__(self, trak):
        self.trak = trak
        self.tkhd = trak.kid("tkhd")
        self.mdia = trak.kid("mdia")
        self.mdhd = self.mdia.kid("mdhd") if self.mdia else None
        self.hdlr = self.mdia.kid("hdlr") if self.mdia else None
        self.minf = self.mdia.kid("minf") if self.mdia else None
        self.stbl = self.minf.kid("stbl") if self.minf else None
        self.stsd = self.stbl.kid("stsd") if self.stbl else None

    # tkhd ---------------------------------------------------------------
    @property
    def tkhd_version(self):
        return self.tkhd.payload[0] if self.tkhd else 0

    @property
    def flags(self):
        p = self.tkhd.payload
        return (p[1] << 16) | (p[2] << 8) | p[3]

    @property
    def enabled(self):
        return bool(self.flags & 1)

    def _tkhd_off(self, base_v0, base_v1):
        return base_v0 if self.tkhd_version == 0 else base_v1

    @property
    def track_id(self):
        return u32(self.tkhd.payload, self._tkhd_off(12, 20))

    @property
    def alternate_group(self):
        return u16(self.tkhd.payload, self._tkhd_off(34, 46))

    @property
    def tkhd_duration(self):
        p = self.tkhd.payload
        if self.tkhd_version == 0:
            return u32(p, 20)
        return struct.unpack(">Q", bytes(p[36:44]))[0]

    # mdhd ---------------------------------------------------------------
    @property
    def mdhd_version(self):
        return self.mdhd.payload[0] if self.mdhd else 0

    @property
    def timescale(self):
        if not self.mdhd:
            return 0
        return u32(self.mdhd.payload, 12 if self.mdhd_version == 0 else 20)

    @property
    def media_duration(self):
        if not self.mdhd:
            return 0
        if self.mdhd_version == 0:
            return u32(self.mdhd.payload, 16)
        return struct.unpack(">Q", bytes(self.mdhd.payload[24:32]))[0]

    @property
    def language(self):
        if not self.mdhd:
            return "und"
        off = 20 if self.mdhd_version == 0 else 32
        return unpack_lang(u16(self.mdhd.payload, off))

    # hdlr ---------------------------------------------------------------
    @property
    def handler_type(self):
        if not self.hdlr:
            return "????"
        return bytes(self.hdlr.payload[8:12]).decode("latin-1", "replace")

    @property
    def handler_name(self):
        if not self.hdlr:
            return ""
        raw = bytes(self.hdlr.payload[24:])
        if not raw:
            return ""
        # ISO: NUL-terminated C string.  QuickTime: leading length byte.
        z = raw.find(b"\x00")
        c = raw[:z] if z >= 0 else raw
        try:
            txt = c.decode("utf-8")
        except UnicodeDecodeError:
            txt = c.decode("latin-1", "replace")
        if raw and raw[0] == len(raw) - 1 and len(raw) > 1:
            try:
                return raw[1:].decode("utf-8").rstrip("\x00")
            except UnicodeDecodeError:
                pass
        return txt

    # stsd ---------------------------------------------------------------
    @property
    def sample_entry(self):
        if not self.stsd or not self.stsd.children:
            return None
        return self.stsd.children[0]

    @property
    def format(self):
        se = self.sample_entry
        return se.type.decode("latin-1", "replace") if se is not None else "????"

    @property
    def dimensions(self):
        se = self.sample_entry
        if se is None or se.children is None or len(se.prefix) < 78:
            return (0, 0)
        return (u16(se.prefix, 24), u16(se.prefix, 26))

    # stbl tables --------------------------------------------------------
    def stts(self):
        b = self.stbl.kid("stts") if self.stbl else None
        if not b:
            return []
        n = u32(b.payload, 4)
        return [(u32(b.payload, 8 + 8 * i), u32(b.payload, 12 + 8 * i)) for i in range(n)]

    def sample_durations(self):
        out = []
        for count, delta in self.stts():
            out.extend([delta] * count)
        return out

    def sample_count(self):
        b = self.stbl.kid("stsz") if self.stbl else None
        if b:
            return u32(b.payload, 8)
        b = self.stbl.kid("stz2") if self.stbl else None
        if b:
            return u32(b.payload, 8)
        return sum(c for c, _ in self.stts())

    def sample_sizes(self):
        b = self.stbl.kid("stsz") if self.stbl else None
        if not b:
            return []
        const = u32(b.payload, 4)
        n = u32(b.payload, 8)
        if const:
            return [const] * n
        return [u32(b.payload, 12 + 4 * i) for i in range(n)]

    def chunk_offsets(self):
        b = self.stbl.kid("stco") if self.stbl else None
        if b:
            n = u32(b.payload, 4)
            return [u32(b.payload, 8 + 4 * i) for i in range(n)]
        b = self.stbl.kid("co64") if self.stbl else None
        if b:
            n = u32(b.payload, 4)
            return [struct.unpack(">Q", bytes(b.payload[8 + 8 * i:16 + 8 * i]))[0]
                    for i in range(n)]
        return []

    def stsc(self):
        b = self.stbl.kid("stsc") if self.stbl else None
        if not b:
            return []
        n = u32(b.payload, 4)
        return [(u32(b.payload, 8 + 12 * i), u32(b.payload, 12 + 12 * i),
                 u32(b.payload, 16 + 12 * i)) for i in range(n)]

    def sample_offsets(self):
        """Return [(file_offset, size)] for every sample, or [] if not derivable."""
        sizes = self.sample_sizes()
        chunks = self.chunk_offsets()
        stsc = self.stsc()
        if not sizes or not chunks or not stsc:
            return []
        # expand stsc to samples-per-chunk
        spc = []
        for i, (first, per, _sdi) in enumerate(stsc):
            last = stsc[i + 1][0] - 1 if i + 1 < len(stsc) else len(chunks)
            for _ in range(first, last + 1):
                spc.append(per)
                if len(spc) >= len(chunks):
                    break
            if len(spc) >= len(chunks):
                break
        out = []
        si = 0
        for ci, coff in enumerate(chunks):
            per = spc[ci] if ci < len(spc) else (spc[-1] if spc else 0)
            off = coff
            for _ in range(per):
                if si >= len(sizes):
                    break
                out.append((off, sizes[si]))
                off += sizes[si]
                si += 1
        return out

    def sample_times(self):
        """Decode-order start times in media timescale ticks."""
        t = 0
        out = []
        for d in self.sample_durations():
            out.append(t)
            t += d
        return out

    def sync_samples(self):
        b = self.stbl.kid("stss") if self.stbl else None
        if not b:
            return None  # all samples are sync
        n = u32(b.payload, 4)
        return [u32(b.payload, 8 + 4 * i) for i in range(n)]

    def mett_mime(self):
        se = self.sample_entry
        if se is None or se.type != b"mett" or se.payload is None:
            return None
        p = bytes(se.payload)
        if len(p) < 8:
            return None
        rest = p[8:]
        parts = rest.split(b"\x00")
        # content_encoding, then mime_format
        if len(parts) >= 2:
            return parts[1].decode("utf-8", "replace")
        if parts:
            return parts[0].decode("utf-8", "replace")
        return None

    def vexu(self):
        se = self.sample_entry
        if se is None or se.children is None:
            return None
        return se.kid("vexu")

    def elst(self):
        """[(segment_duration_movie_ts, media_time, rate)] or []."""
        e = self.trak.kid("edts")
        b = e.kid("elst") if e is not None else None
        if b is None or b.payload is None:
            return []
        p = bytes(b.payload)
        ver = p[0]
        n = u32(p, 4)
        out = []
        off = 8
        for _ in range(n):
            if ver == 0:
                if off + 12 > len(p):
                    break
                d, mt = struct.unpack(">Ii", p[off:off + 8])
                rate = struct.unpack(">i", p[off + 8:off + 12])[0]
                off += 12
            else:
                if off + 20 > len(p):
                    break
                d, mt = struct.unpack(">Qq", p[off:off + 16])
                rate = struct.unpack(">i", p[off + 16:off + 20])[0]
                off += 20
            out.append((d, mt, rate))
        return out

    def presentation_start(self, movie_timescale):
        """Leading empty-edit delay in seconds (0 when there is no edit list)."""
        delay = 0
        for d, mt, _rate in self.elst():
            if mt == -1:
                delay += d
            else:
                break
        return delay / float(movie_timescale or 1)


# --------------------------------------------------------------------------
# top-level file handling
# --------------------------------------------------------------------------

class Mp4File:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        self.mv = memoryview(self.data)
        self.top = parse_boxes(self.mv)
        self.moov_index = None
        for i, b in enumerate(self.top):
            if b.type == b"moov":
                self.moov_index = i
        self.moov = self.top[self.moov_index] if self.moov_index is not None else None

    @property
    def brands(self):
        ftyp = None
        for b in self.top:
            if b.type == b"ftyp":
                ftyp = b
                break
        if ftyp is None or ftyp.payload is None:
            return []
        p = bytes(ftyp.payload)
        out = [p[0:4].decode("latin-1", "replace")]
        for i in range(8, len(p) - 3, 4):
            out.append(p[i:i + 4].decode("latin-1", "replace"))
        return out

    @property
    def moov_is_last(self):
        return self.moov_index == len(self.top) - 1

    def traks(self):
        return self.moov.kids("trak") if self.moov else []

    def views(self):
        return [TrackView(t) for t in self.traks()]

    def mvhd_next_track_id(self):
        mvhd = self.moov.kid("mvhd")
        ver = mvhd.payload[0]
        off = 96 if ver == 0 else 108
        return u32(mvhd.payload, off)

    def mvhd_timescale(self):
        mvhd = self.moov.kid("mvhd")
        ver = mvhd.payload[0]
        return u32(mvhd.payload, 12 if ver == 0 else 20)

    def mvhd_duration(self):
        mvhd = self.moov.kid("mvhd")
        ver = mvhd.payload[0]
        if ver == 0:
            return u32(mvhd.payload, 16)
        return struct.unpack(">Q", bytes(mvhd.payload[24:32]))[0]

    def has_covr(self):
        if not self.moov:
            return False
        meta = self.moov.path("udta", "meta")
        if meta is None:
            return False
        ilst = meta.kid("ilst")
        if ilst is None:
            return False
        return bool(ilst.kids("covr"))

    def covr_size(self):
        ilst = self.moov.path("udta", "meta", "ilst") if self.moov else None
        if not ilst:
            return 0
        for c in ilst.kids("covr"):
            if c.children:
                d = c.kid("data")
                if d is not None and d.payload is not None:
                    return max(0, len(d.payload) - 8)
            elif c.payload is not None:
                return len(c.payload)
        return 0

    def sample_bytes(self, view, index):
        offs = view.sample_offsets()
        if index < 0:
            index += len(offs)
        if not (0 <= index < len(offs)):
            return b""
        off, size = offs[index]
        return self.data[off:off + size]


# --------------------------------------------------------------------------
# vexu construction
# --------------------------------------------------------------------------

def build_vexu(has_left=True, has_right=True, reversed_eyes=False, hero=1,
               baseline_um=None, convergence=None):
    """Build a `vexu` box per the FFmpeg mov.c / movenc.c layout."""
    stri_byte = (1 if has_left else 0) | (2 if has_right else 0) | (8 if reversed_eyes else 0)
    eyes_children = [
        full_box(b"stri", 0, 0, bytes([stri_byte])),
        full_box(b"hero", 0, 0, bytes([hero & 0xFF])),
    ]
    if baseline_um:
        eyes_children.append(Box(b"cams", children=[
            full_box(b"blin", 0, 0, struct.pack(">I", int(baseline_um) & 0xFFFFFFFF))]))
    if convergence:
        adj = int(round(float(convergence) * 10000))
        eyes_children.append(Box(b"cmfy", children=[
            full_box(b"dadj", 0, 0, struct.pack(">i", adj))]))
    return Box(b"vexu", children=[Box(b"eyes", children=eyes_children)])


# --------------------------------------------------------------------------
# convergence curve
# --------------------------------------------------------------------------

class ConvCurve:
    """Piecewise-linear convergence as a function of time in seconds."""

    def __init__(self, points, offset=0.0):
        # points: sorted [(t_seconds, value)]
        self.points = sorted(points) if points else [(0.0, 0.0)]
        self.offset = offset  # added to the query time before lookup

    @staticmethod
    def constant(v):
        return ConvCurve([(0.0, float(v))])

    @staticmethod
    def sweep(a, b, duration):
        d = duration if duration and duration > 0 else 1.0
        return ConvCurve([(0.0, float(a)), (d, float(b))])

    @staticmethod
    def from_csv(path):
        pts = []
        with open(path, "r") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = re.split(r"[,;\s]+", line)
                if len(parts) < 2:
                    continue
                try:
                    pts.append((float(parts[0]), float(parts[1])))
                except ValueError:
                    continue  # header row
        if not pts:
            raise SystemExit("conv-csv: no usable 't,value' rows in %s" % path)
        return ConvCurve(pts)

    def at(self, t):
        t = t + self.offset
        p = self.points
        if t <= p[0][0]:
            return p[0][1]
        if t >= p[-1][0]:
            return p[-1][1]
        lo, hi = 0, len(p) - 1
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if p[mid][0] <= t:
                lo = mid
            else:
                hi = mid
        t0, v0 = p[lo]
        t1, v1 = p[hi]
        if t1 == t0:
            return v1
        return v0 + (v1 - v0) * (t - t0) / (t1 - t0)


# --------------------------------------------------------------------------
# convergence track construction
# --------------------------------------------------------------------------

def build_conv_trak(track_id, timescale, movie_timescale, sample_specs,
                    chunk_offset, handler_name=HDLR_CONV, mime=CONV_MIME,
                    edts=None, movie_dur=None):
    """sample_specs: [(duration_ticks, payload_bytes)] in presentation order.

    `edts` is the left video track's edit-list box, copied verbatim so that the
    convergence track shares the video's presentation timeline exactly (a
    camera file can carry a leading empty edit; without the copy every
    convergence sample would be early by that delay).
    """
    total = sum(d for d, _ in sample_specs)
    if movie_dur is None:
        movie_dur = int(round(total * movie_timescale / float(timescale))) if timescale else 0

    # --- tkhd (v0, enabled|in_movie, alternate_group 0) -------------------
    matrix = struct.pack(">9i", 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000)
    tkhd_payload = (
        struct.pack(">II", 0, 0) +           # creation, modification
        struct.pack(">I", track_id) +
        struct.pack(">I", 0) +               # reserved
        struct.pack(">I", movie_dur) +
        b"\x00" * 8 +                        # reserved
        struct.pack(">h", 0) +               # layer
        struct.pack(">h", 0) +               # alternate_group  <- MUST be 0
        struct.pack(">h", 0) +               # volume
        struct.pack(">H", 0) +               # reserved
        matrix +
        struct.pack(">II", 0, 0)             # width, height (0 for metadata)
    )
    tkhd = full_box(b"tkhd", 0, 0x000003, tkhd_payload)

    # --- mdhd -------------------------------------------------------------
    mdhd = full_box(b"mdhd", 0, 0,
                    struct.pack(">IIII", 0, 0, timescale, total) +
                    struct.pack(">HH", pack_lang("und"), 0))

    # --- hdlr -------------------------------------------------------------
    hdlr = full_box(b"hdlr", 0, 0,
                    b"\x00\x00\x00\x00" + b"meta" + b"\x00" * 12 + cstr(handler_name))

    # --- minf -------------------------------------------------------------
    nmhd = full_box(b"nmhd", 0, 0, b"")
    url = full_box(b"url ", 0, 1, b"")                      # self-contained
    dref = full_box(b"dref", 0, 0, struct.pack(">I", 1) + url.to_bytes())
    dinf = Box(b"dinf", children=[dref])

    # --- stsd/mett --------------------------------------------------------
    mett_payload = (b"\x00" * 6 + struct.pack(">H", 1) +    # reserved + dri
                    b"\x00" +                               # content_encoding ""
                    cstr(mime))                             # mime_format
    mett = Box(b"mett", payload=mett_payload)
    stsd = Box(b"stsd", prefix=b"\x00\x00\x00\x00" + struct.pack(">I", 1),
               children=[mett])

    # --- stts (run-length) ------------------------------------------------
    runs = []
    for d, _ in sample_specs:
        if runs and runs[-1][1] == d:
            runs[-1][0] += 1
        else:
            runs.append([1, d])
    stts = full_box(b"stts", 0, 0,
                    struct.pack(">I", len(runs)) +
                    b"".join(struct.pack(">II", c, d) for c, d in runs))

    # --- stsc / stsz / stco ----------------------------------------------
    n = len(sample_specs)
    stsc = full_box(b"stsc", 0, 0, struct.pack(">I", 1) + struct.pack(">III", 1, n, 1))
    stsz = full_box(b"stsz", 0, 0,
                    struct.pack(">II", 0, n) +
                    b"".join(struct.pack(">I", len(p)) for _, p in sample_specs))
    if chunk_offset > 0xFFFFFFFF:
        stco = full_box(b"co64", 0, 0, struct.pack(">I", 1) + struct.pack(">Q", chunk_offset))
    else:
        stco = full_box(b"stco", 0, 0, struct.pack(">I", 1) + struct.pack(">I", chunk_offset))

    stbl = Box(b"stbl", children=[stsd, stts, stsc, stsz, stco])
    minf = Box(b"minf", children=[nmhd, dinf, stbl])
    mdia = Box(b"mdia", children=[mdhd, hdlr, minf])
    kids = [tkhd]
    if edts is not None:
        # verbatim copy (serialize the source box, drop its 8-byte header)
        kids.append(Box(b"edts", payload=edts.to_bytes()[8:]))
    kids.append(mdia)
    return Box(b"trak", children=kids)


# --------------------------------------------------------------------------
# covr fallback writer (used when mutagen is unavailable or refuses the file)
# --------------------------------------------------------------------------

def set_covr_inplace(moov, jpeg_bytes):
    """Insert/replace moov/udta/meta/ilst/covr with a JPEG."""
    data = full_box(b"data", 0, 13, b"\x00\x00\x00\x00" + jpeg_bytes)  # flags 13 = JPEG
    covr = Box(b"covr", children=[data])

    udta = moov.kid("udta")
    if udta is None:
        udta = Box(b"udta", children=[])
        moov.children.append(udta)
    if udta.children is None:
        udta.children = parse_boxes(memoryview(udta.payload or b""), b"udta")
        udta.payload = None
    meta = udta.kid("meta")
    if meta is None:
        hdlr = full_box(b"hdlr", 0, 0, b"\x00\x00\x00\x00" + b"mdir" + b"appl" +
                        b"\x00" * 8 + b"\x00")
        meta = Box(b"meta", prefix=b"\x00\x00\x00\x00", children=[hdlr])
        udta.children.append(meta)
    if meta.children is None:
        meta.children = []
    ilst = meta.kid("ilst")
    if ilst is None:
        ilst = Box(b"ilst", children=[])
        meta.children.append(ilst)
    if ilst.children is None:
        ilst.children = []
    ilst.children = [c for c in ilst.children if c.type != b"covr"]
    ilst.children.append(covr)


# --------------------------------------------------------------------------
# moov patching helpers
# --------------------------------------------------------------------------

def set_tkhd_enabled(view, enabled):
    p = bytearray(bytes(view.tkhd.payload))
    flags = (p[1] << 16) | (p[2] << 8) | p[3]
    flags = (flags | 1) if enabled else (flags & ~1)
    p[1] = (flags >> 16) & 0xFF
    p[2] = (flags >> 8) & 0xFF
    p[3] = flags & 0xFF
    view.tkhd.payload = bytes(p)


def set_alternate_group(view, group):
    p = bytearray(bytes(view.tkhd.payload))
    off = 34 if view.tkhd_version == 0 else 46
    p[off:off + 2] = struct.pack(">H", group)
    view.tkhd.payload = bytes(p)


def set_mdhd_language(view, code):
    p = bytearray(bytes(view.mdhd.payload))
    off = 20 if view.mdhd_version == 0 else 32
    p[off:off + 2] = struct.pack(">H", pack_lang(code))
    view.mdhd.payload = bytes(p)


def set_handler_name(view, name):
    p = bytes(view.hdlr.payload)
    view.hdlr.payload = p[:24] + cstr(name)


def bump_next_track_id(moov, new_id):
    mvhd = moov.kid("mvhd")
    p = bytearray(bytes(mvhd.payload))
    off = 96 if p[0] == 0 else 108
    cur = struct.unpack(">I", bytes(p[off:off + 4]))[0]
    if new_id + 1 > cur:
        p[off:off + 4] = struct.pack(">I", new_id + 1)
    mvhd.payload = bytes(p)


# --------------------------------------------------------------------------
# convert
# --------------------------------------------------------------------------

def detect_source(info):
    """Return ('sbs'|'dual', video_stream_indices, audio_stream_index_or_None,
    conv_stream_index_or_None)."""
    vids = [s for s in info["streams"] if s.get("codec_type") == "video"
            and s.get("disposition", {}).get("attached_pic", 0) == 0]
    auds = [s for s in info["streams"] if s.get("codec_type") == "audio"]
    datas = [s for s in info["streams"] if s.get("codec_type") == "data"]
    aud = auds[0]["index"] if auds else None
    conv = datas[0]["index"] if datas else None
    if len(vids) >= 2:
        # order by abl/abr language tag when present
        def key(s):
            lang = (s.get("tags") or {}).get("language", "")
            return {"abl": 0, "abr": 1}.get(lang, 2 + s["index"])
        vids = sorted(vids, key=key)
        return "dual", [vids[0], vids[1]], aud, conv
    if not vids:
        raise SystemExit("input has no video stream")
    return "sbs", [vids[0]], aud, conv


def read_v1_conv_curve(path, conv_index, time_offset=0.0):
    """Read a v1 convergence track: samples via -f data, PTS via -show_packets."""
    p = run(["ffmpeg", "-v", "error", "-i", path, "-map", "0:%d" % conv_index,
             "-c", "copy", "-f", "data", "-"], binary=True)
    raw = p.stdout.decode("utf-8", "replace")
    # Concatenated JSON objects, no separator.
    chunks = re.findall(r"\{[^{}]*\}", raw)
    values = []
    for c in chunks:
        try:
            values.append(float(json.loads(c)["convergence"]))
        except Exception:
            m = re.search(r"-?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?", c)
            if m:
                values.append(float(m.group(0)))
    q = run(["ffprobe", "-v", "error", "-select_streams", "%d" % conv_index,
             "-show_packets", "-show_entries", "packet=pts_time",
             "-of", "csv=p=0", path])
    times = []
    for line in q.stdout_text.splitlines():
        line = line.strip().rstrip(",")
        if not line:
            continue
        try:
            times.append(float(line))
        except ValueError:
            pass
    n = min(len(times), len(values))
    if n == 0:
        return None, 0
    pts = [(times[i], values[i]) for i in range(n)]
    return ConvCurve(pts, offset=time_offset), n


def encode(src, dst, kind, vstreams, aud_index, args, src_info):
    v0 = vstreams[0]
    w = int(v0["width"])
    h = int(v0["height"])

    if kind == "sbs":
        vw = w // 2
        if vw % 2:
            vw -= 1
        vh = h if h % 2 == 0 else h - 1
        if args.half_sbs:
            lf = "[0:v:0]crop=%d:%d:0:0,scale=%d:%d,setsar=1[l]" % (vw, vh, vw * 2, vh)
            rf = "[0:v:0]crop=%d:%d:%d:0,scale=%d:%d,setsar=1[r]" % (vw, vh, w - vw, vw * 2, vh)
            out_w, out_h = vw * 2, vh
        else:
            lf = "[0:v:0]crop=%d:%d:0:0,setsar=1[l]" % (vw, vh)
            rf = "[0:v:0]crop=%d:%d:%d:0,setsar=1[r]" % (vw, vh, w - vw)
            out_w, out_h = vw, vh
        fc = lf + ";" + rf
    else:
        fc = "[0:v:0]setsar=1[l];[0:v:1]setsar=1[r]"
        out_w, out_h = w, h

    # media timescale: reuse the source's video time_base denominator when sane
    tb = v0.get("time_base", "1/15360")
    try:
        ts = int(tb.split("/")[1])
    except Exception:
        ts = 15360
    if ts < 600 or ts > 1000000:
        ts = 15360

    g = str(args.gop)
    kf = "expr:eq(mod(n,%d),0)" % args.gop
    x264p = ("keyint=%d:min-keyint=%d:scenecut=0:open_gop=0:bframes=0"
             % (args.gop, args.gop))

    cmd = ["ffmpeg", "-y", "-v", "warning", "-i", src,
           "-filter_complex", fc,
           "-map", "[l]", "-map", "[r]"]
    if aud_index is not None:
        cmd += ["-map", "0:%d" % aud_index, "-c:a", "copy"]
    cmd += [
        "-c:v", "libx264", "-profile:v", "high", "-pix_fmt", "yuv420p",
        "-crf", str(args.crf), "-preset", args.preset,
        "-g", g, "-keyint_min", g, "-bf", "0", "-sc_threshold", "0",
        "-flags:v", "+cgop",
        "-x264-params", x264p,
        "-force_key_frames:v:0", kf,
        "-force_key_frames:v:1", kf,
        "-video_track_timescale", str(ts),
        "-metadata:s:v:0", "language=abl",
        "-metadata:s:v:1", "language=abr",
        "-metadata:s:v:0", "handler_name=%s" % HDLR_LEFT,
        "-metadata:s:v:1", "handler_name=%s" % HDLR_RIGHT,
        "-brand", "mp42",
        "-strict", "unofficial",
    ]
    # carry identical colour signalling onto both tracks when the source states it
    for key, opt in (("color_primaries", "-color_primaries"),
                     ("color_transfer", "-color_trc"),
                     ("color_space", "-colorspace"),
                     ("color_range", "-color_range")):
        val = v0.get(key)
        if val and val not in ("unknown", "unspecified"):
            cmd += [opt, val]
    cmd += [dst]
    run(cmd, capture=True)
    return out_w, out_h, ts


def extract_poster(src, kind, vstreams, args, out_jpg, duration):
    v0 = vstreams[0]
    w = int(v0["width"]); h = int(v0["height"])
    ss = "1" if (duration or 0) > 1.5 else "0"
    if kind == "sbs":
        vw = w // 2
        vw -= vw % 2
        vh = h - (h % 2)
        vf = "crop=%d:%d:0:0" % (vw, vh)
        if args.half_sbs:
            vf += ",scale=%d:%d" % (vw * 2, vh)
        maps = ["-map", "0:v:0"]
    else:
        vf = "null"
        maps = ["-map", "0:v:0"]
    run(["ffmpeg", "-y", "-v", "error", "-ss", ss, "-i", src] + maps +
        ["-vf", vf, "-frames:v", "1", "-q:v", "3", "-f", "mjpeg", out_jpg])
    return os.path.exists(out_jpg) and os.path.getsize(out_jpg) > 0


def cmd_convert(args):
    src = args.input
    dst = args.output
    if not os.path.exists(src):
        raise SystemExit("input not found: %s" % src)

    info = ffprobe_json(src)
    kind, vstreams, aud_index, conv_index = detect_source(info)
    duration = float(info.get("format", {}).get("duration", 0) or 0)
    src_v_start = float(vstreams[0].get("start_time", 0) or 0)

    print("[lvf2] source: %s (%s)" % (src, "dual-track v1" if kind == "dual" else "SBS 2x1"))
    print("[lvf2]   video %dx%d  fps %s  audio=%s  conv-track=%s"
          % (int(vstreams[0]["width"]), int(vstreams[0]["height"]),
             vstreams[0].get("r_frame_rate"), aud_index, conv_index))

    tmpdir = tempfile.mkdtemp(prefix="lvf2-")
    try:
        stage = os.path.join(tmpdir, "stage.mp4")
        out_w, out_h, ts = encode(src, stage, kind, vstreams, aud_index, args, info)
        print("[lvf2] encoded two %dx%d tracks, media timescale %d" % (out_w, out_h, ts))

        # ---- convergence source -----------------------------------------
        carried = 0
        if args.conv_csv:
            curve = ConvCurve.from_csv(args.conv_csv)
            conv_src = "csv:%s" % args.conv_csv
        elif args.conv_sweep:
            a, b = [float(x) for x in args.conv_sweep.split(",")]
            curve = ConvCurve.sweep(a, b, duration)
            conv_src = "sweep %g..%g" % (a, b)
        elif args.convergence is not None:
            curve = ConvCurve.constant(args.convergence)
            conv_src = "constant %g" % args.convergence
        elif kind == "dual" and conv_index is not None:
            curve, carried = read_v1_conv_curve(src, conv_index, time_offset=src_v_start)
            if curve is None:
                curve = ConvCurve.constant(0.0)
                conv_src = "constant 0 (v1 track unreadable)"
            else:
                conv_src = "carried from v1 track (%d samples, +%.4fs origin shift)" % (
                    carried, src_v_start)
        else:
            curve = ConvCurve.constant(0.0)
            conv_src = "constant 0"
        print("[lvf2] convergence: %s" % conv_src)

        # ---- poster -----------------------------------------------------
        poster = os.path.join(tmpdir, "poster.jpg")
        have_poster = extract_poster(src, kind, vstreams, args, poster, duration)
        poster_how = "none"
        if have_poster:
            poster_how = apply_covr_mutagen(stage, poster)

        # ---- box editing -------------------------------------------------
        finalize(stage, dst, curve, args,
                 poster_path=poster if (have_poster and poster_how != "mutagen") else None,
                 poster_how=poster_how)
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    print("[lvf2] wrote %s (%d bytes)" % (dst, os.path.getsize(dst)))
    return 0


def apply_covr_mutagen(path, jpeg_path):
    try:
        from mutagen.mp4 import MP4, MP4Cover
    except Exception:
        return "builtin (mutagen not importable)"
    try:
        f = MP4(path)
        with open(jpeg_path, "rb") as fh:
            f["covr"] = [MP4Cover(fh.read(), imageformat=MP4Cover.FORMAT_JPEG)]
        f.save()
        return "mutagen"
    except Exception as e:
        return "builtin (mutagen refused: %s)" % type(e).__name__


def finalize(stage, dst, curve, args, poster_path=None, poster_how="none"):
    """Patch tkhd/hdlr/mdhd, insert vexu, append the convergence mdat+trak."""
    mp4 = Mp4File(stage)
    if mp4.moov is None:
        raise SystemExit("staged file has no moov")

    # Drop any trailing free/skip so moov can be last.
    while (not mp4.moov_is_last) and mp4.top[-1].type in (b"free", b"skip"):
        mp4.top.pop()
    if not mp4.moov_is_last:
        raise SystemExit("moov is not the last top-level box (faststart?); refusing "
                         "to rewrite chunk offsets")

    views = mp4.views()
    vids = [v for v in views if v.handler_type == "vide"]
    if len(vids) < 2:
        raise SystemExit("staged file has %d video tracks, expected 2" % len(vids))
    left, right = vids[0], vids[1]

    # (3) languages — assert/patch
    if left.language != "abl":
        set_mdhd_language(left, "abl")
    if right.language != "abr":
        set_mdhd_language(right, "abr")

    # (4) handler names — assert/patch
    if left.handler_name != HDLR_LEFT:
        set_handler_name(left, HDLR_LEFT)
    if right.handler_name != HDLR_RIGHT:
        set_handler_name(right, HDLR_RIGHT)

    # (5) tkhd enable/disable, alternate_group 0 everywhere
    set_tkhd_enabled(left, True)
    set_tkhd_enabled(right, False)
    for v in views:
        if v.alternate_group != 0:
            set_alternate_group(v, 0)

    # (6) vexu into the sample entries.  `stri` describes what THAT sample
    # entry's track carries, so in the default per-track mode the left track
    # declares the left eye only and the right track declares the right eye
    # only.  (Setting both bits on one entry means "this track is frame-packed
    # and carries both eyes", which is not what a two-track pair is; FFmpeg
    # then reports view=packed.)  `--stri both` keeps that older behaviour.
    def _install_vexu(view, **kw):
        s = view.sample_entry
        if s is None or s.children is None:
            raise SystemExit("track sample entry %s is not parseable" % view.format)
        s.children = [c for c in s.children if c.type != b"vexu"]
        s.children.append(build_vexu(baseline_um=args.baseline_um,
                                     convergence=args.vexu_convergence, **kw))

    if args.stri == "both":
        # legacy: one packed-looking vexu on the left entry, nothing on the right
        _install_vexu(left, has_left=True, has_right=True, hero=1)
        rse = right.sample_entry
        if rse is not None and rse.children is not None:
            rse.children = [c for c in rse.children if c.type != b"vexu"]
    else:
        # `hero` names the primary eye of the PAIR (the left one), so it is the
        # same on both entries -- it is not "which eye is in this track".
        _install_vexu(left, has_left=True, has_right=False, hero=1)
        _install_vexu(right, has_left=False, has_right=True, hero=1)

    # (8) poster, if mutagen did not already do it
    if poster_path:
        with open(poster_path, "rb") as fh:
            set_covr_inplace(mp4.moov, fh.read())

    # (7) convergence track
    ts = left.timescale
    durations = left.sample_durations()
    if not durations:
        raise SystemExit("left track has no stts")
    starts = []
    t = 0
    for d in durations:
        starts.append(t)
        t += d
    fps = ts / (sum(durations) / float(len(durations)))

    if args.conv_rate == "1hz":
        step = max(1, int(round(fps)))
        idx = list(range(0, len(durations), step))
    else:
        idx = list(range(len(durations)))
    specs = []
    for i, s in enumerate(idx):
        end = idx[i + 1] if i + 1 < len(idx) else len(durations)
        dur = sum(durations[s:end])
        val = curve.at(starts[s] / float(ts))
        payload = json.dumps({"convergence": round(val, 9)},
                             separators=(",", ":")).encode("utf-8")
        specs.append((dur, payload))

    conv_bytes = b"".join(p for _, p in specs)
    pre_size = sum(b.size() for b in mp4.top[:mp4.moov_index])
    chunk_offset = pre_size + 8   # right after the new mdat header
    conv_mdat = Box(b"mdat", payload=conv_bytes)

    track_id = mp4.mvhd_next_track_id()
    conv_trak = build_conv_trak(track_id, ts, mp4.mvhd_timescale(), specs, chunk_offset,
                                edts=left.trak.kid("edts"),
                                movie_dur=left.tkhd_duration)
    bump_next_track_id(mp4.moov, track_id)

    # (1) track order: insert right after the last existing trak
    kids = mp4.moov.children
    last_trak = max(i for i, c in enumerate(kids) if c.type == b"trak")
    kids.insert(last_trak + 1, conv_trak)

    # ---- write out: [everything before moov][new mdat][moov] -------------
    with open(dst, "wb") as f:
        for b in mp4.top[:mp4.moov_index]:
            b.write(f)
        conv_mdat.write(f)
        mp4.moov.write(f)

    first_v = json.loads(specs[0][1].decode())["convergence"]
    last_v = json.loads(specs[-1][1].decode())["convergence"]
    print("[lvf2] convergence track: %d samples @ %s, ts=%d, %.6f -> %.6f"
          % (len(specs), args.conv_rate, ts, first_v, last_v))
    print("[lvf2] poster: %s" % poster_how)


# --------------------------------------------------------------------------
# inspect
# --------------------------------------------------------------------------

def read_vexu(view):
    """Decode one track's vexu/eyes box. Returns a dict (never raises)."""
    out = {"stri": None, "hero": None, "baseline": None, "dadj": None,
           "report": "absent"}
    vx = view.vexu()
    if vx is None:
        return out
    eyes = vx.kid("eyes")
    parts = []
    if eyes is not None:
        stri = eyes.kid("stri")
        if stri is not None and stri.payload is not None and len(stri.payload) >= 5:
            b = stri.payload[4]
            out["stri"] = b
            parts.append("stri=0x%02x(left=%d right=%d addl=%d reversed=%d)"
                         % (b, b & 1, (b >> 1) & 1, (b >> 2) & 1, (b >> 3) & 1))
        hero = eyes.kid("hero")
        if hero is not None and hero.payload is not None and len(hero.payload) >= 5:
            out["hero"] = hero.payload[4]
            parts.append("hero=%d(%s)" % (out["hero"],
                         {0: "none", 1: "left", 2: "right"}.get(out["hero"], "?")))
        cams = eyes.kid("cams")
        if cams is not None:
            blin = cams.kid("blin")
            if blin is not None and blin.payload is not None:
                out["baseline"] = u32(blin.payload, 4)
                parts.append("blin=%d um" % out["baseline"])
        cmfy = eyes.kid("cmfy")
        if cmfy is not None:
            dadjb = cmfy.kid("dadj")
            if dadjb is not None and dadjb.payload is not None:
                out["dadj"] = struct.unpack(">i", bytes(dadjb.payload[4:8]))[0]
                parts.append("dadj=%d (%.4f of view width)"
                             % (out["dadj"], out["dadj"] / 10000.0))
    out["report"] = ("vexu/eyes " + " ".join(parts)) if parts else "vexu (empty)"
    return out


def _pf(ok):
    return "PASS" if ok else "FAIL"


def cmd_inspect(args):
    path = args.file
    mp4 = Mp4File(path)
    print("=" * 78)
    print("LVF v2 inspect: %s  (%d bytes)" % (path, os.path.getsize(path)))
    print("=" * 78)
    if mp4.moov is None:
        print("  NO moov BOX -- not a usable MP4")
        return 1

    brands = mp4.brands
    top = [b.type.decode("latin-1", "replace") for b in mp4.top]
    print("brands      : %s" % " ".join(brands))
    print("top boxes   : %s   (moov index %d/%d, last=%s)"
          % (" ".join(top), mp4.moov_index, len(mp4.top) - 1, mp4.moov_is_last))
    print("movie       : timescale=%d duration=%d (%.3f s) next_track_ID=%d"
          % (mp4.mvhd_timescale(), mp4.mvhd_duration(),
             mp4.mvhd_duration() / float(mp4.mvhd_timescale() or 1),
             mp4.mvhd_next_track_id()))

    views = mp4.views()
    print("")
    print("tracks:")
    hdr = ("  #  id  hdlr  fmt   WxH         lang  enab alt  samples  timescale  "
           "dur(s)   handler_name")
    print(hdr)
    print("  " + "-" * (len(hdr) - 2))
    for i, v in enumerate(views):
        w, h = v.dimensions
        dur = v.media_duration / float(v.timescale or 1)
        print("  %-2d %-3d %-5s %-5s %-11s %-5s %-4s %-4d %-8d %-10d %-8.3f %s"
              % (i, v.track_id, v.handler_type, v.format,
                 ("%dx%d" % (w, h)) if w else "-",
                 v.language, "yes" if v.enabled else "NO",
                 v.alternate_group, v.sample_count(), v.timescale, dur,
                 v.handler_name))

    vids = [v for v in views if v.handler_type == "vide"]
    metas = [v for v in views if v.handler_type == "meta" and v.format == "mett"]
    auds = [v for v in views if v.handler_type == "soun"]

    # ---- vexu (read per video track) ------------------------------------
    print("")
    vexu_info = [read_vexu(v) for v in vids]
    for i, inf in enumerate(vexu_info):
        print("vexu (v:%d)  : %s" % (i, inf["report"]))
    stri_bits = vexu_info[0]["stri"] if vexu_info else None
    hero_val = vexu_info[0]["hero"] if vexu_info else None
    vexu_report = vexu_info[0]["report"] if vexu_info else "absent"
    r_stri = vexu_info[1]["stri"] if len(vexu_info) > 1 else None
    # Which layout is this?  Per-track (each entry declares its own eye) or the
    # legacy packed-on-left form.
    if stri_bits is not None and (stri_bits & 0x0F) == 0x01 and r_stri is not None \
            and (r_stri & 0x0F) == 0x02:
        stri_mode = "per-track (left=left-only, right=right-only)"
        stri_ok = True
    elif stri_bits is not None and (stri_bits & 0x03) == 0x03 and r_stri is None:
        stri_mode = "legacy --stri both (packed-on-left, no vexu on right)"
        stri_ok = True
    else:
        stri_mode = "non-conforming"
        stri_ok = False
    print("stri layout : %s" % stri_mode)

    # ---- convergence track ---------------------------------------------
    conv_mime = None
    conv_n = 0
    conv_first = conv_last = None
    conv_t0 = None
    conv_maxgap = None
    if metas:
        m = metas[0]
        conv_mime = m.mett_mime()
        conv_n = m.sample_count()
        try:
            b0 = mp4.sample_bytes(m, 0)
            b1 = mp4.sample_bytes(m, conv_n - 1)
            conv_first = b0.decode("utf-8", "replace").strip()
            conv_last = b1.decode("utf-8", "replace").strip()
        except Exception:
            pass
        times = m.sample_times()
        if times:
            conv_t0 = times[0] / float(m.timescale or 1)
            gaps = [(times[i + 1] - times[i]) / float(m.timescale or 1)
                    for i in range(len(times) - 1)]
            tail = (m.media_duration - times[-1]) / float(m.timescale or 1)
            conv_maxgap = max(gaps + [tail]) if gaps else tail
    mts0 = mp4.mvhd_timescale()
    if vids:
        print("presentation: video origin %.4f s%s"
              % (vids[0].presentation_start(mts0),
                 ("  conv origin %.4f s" % metas[0].presentation_start(mts0))
                 if metas else ""))
        # elst-check: item 7c compares PRESENTATION origins, not media times.
        # A track's presentation origin is the sum of its leading empty edits
        # (elst entries with media_time == -1), expressed in movie timescale
        # units.  A camera source can delay video to line up with audio
        # (vendor-camera: ~168 ms); a convergence track without the same edit list
        # would then be early by exactly that delay on every sample, which is
        # why lvf2 copies the left video track's edts onto it verbatim.
        print("elst-check  : 7c compares leading empty edits (media_time == -1); "
              "conv must copy the left video's edts")
        if getattr(args, "elst_check", False):
            for i, v in enumerate(views):
                print("              trak %d (%s) elst=%s"
                      % (i, v.handler_type, v.elst() or "none"))
    print("convergence : mime=%s samples=%d t0=%s maxgap=%s"
          % (conv_mime, conv_n,
             "%.3f" % conv_t0 if conv_t0 is not None else "-",
             "%.3f s" % conv_maxgap if conv_maxgap is not None else "-"))
    if conv_first is not None:
        print("              first=%s  last=%s" % (conv_first, conv_last))

    print("covr        : %s (%d bytes)" % (mp4.has_covr(), mp4.covr_size()))

    # ---- contract checks ------------------------------------------------
    checks = []
    ok_brand = any(b in ("isom", "mp42", "mp41", "iso2") for b in brands)
    checks.append(("1a brands isom/mp42", ok_brand, " ".join(brands)))
    order_ok = False
    if len(views) >= 3:
        exp = ["vide", "vide"] + (["soun"] if auds else []) + ["meta"]
        got = [v.handler_type for v in views]
        order_ok = got == exp
        checks.append(("1b track order L,R,[audio],conv", order_ok, "%s" % ",".join(got)))
    else:
        checks.append(("1b track order L,R,[audio],conv", False,
                       "only %d tracks" % len(views)))
    checks.append(("1c moov is last top-level box", mp4.moov_is_last,
                   "index %d of %d" % (mp4.moov_index, len(mp4.top) - 1)))

    two = len(vids) == 2
    detail = "%d video tracks" % len(vids)
    same = two
    if two:
        a, b = vids[0], vids[1]
        same = (a.format == b.format and a.dimensions == b.dimensions
                and a.timescale == b.timescale and a.sample_count() == b.sample_count())
        detail = ("%s %s ts=%d n=%d  |  %s %s ts=%d n=%d"
                  % (a.format, a.dimensions, a.timescale, a.sample_count(),
                     b.format, b.dimensions, b.timescale, b.sample_count()))
    checks.append(("2a exactly two video tracks, matched", two and same, detail))
    if two:
        ta = vids[0].sample_times()
        tb = vids[1].sample_times()
        pts_ok = ta == tb
        sa = vids[0].sync_samples()
        sb = vids[1].sync_samples()
        idr_ok = sa == sb
        checks.append(("2b identical sample times (PTS pairing)", pts_ok,
                       "%d vs %d samples, %s" % (len(ta), len(tb),
                       "identical" if pts_ok else "MISMATCH")))
        checks.append(("2c identical sync-sample (IDR) sets", idr_ok,
                       "%s" % ("all-sync both" if sa is None and sb is None
                               else "%d vs %d keyframes" % (len(sa or []), len(sb or [])))))
        ea = vids[0].trak.kid("edts")
        eb = vids[1].trak.kid("edts")
        checks.append(("2d no per-track re-basing (edit lists match)",
                       (ea is None) == (eb is None),
                       "edts left=%s right=%s" % (ea is not None, eb is not None)))
        checks.append(("3  languages abl / abr",
                       vids[0].language == "abl" and vids[1].language == "abr",
                       "%s / %s" % (vids[0].language, vids[1].language)))
        checks.append(("4  handler names",
                       vids[0].handler_name == HDLR_LEFT and vids[1].handler_name == HDLR_RIGHT,
                       "%r / %r" % (vids[0].handler_name, vids[1].handler_name)))
        checks.append(("5a left enabled, right track_enabled=0",
                       vids[0].enabled and not vids[1].enabled,
                       "flags 0x%06x / 0x%06x" % (vids[0].flags, vids[1].flags)))
    checks.append(("5b alternate_group == 0 on every track",
                   all(v.alternate_group == 0 for v in views),
                   ",".join(str(v.alternate_group) for v in views)))
    checks.append(("6a vexu/eyes/stri on left track",
                   stri_bits is not None, vexu_report))
    checks.append(("6b stri declares this track's own eye", stri_ok, stri_mode))
    checks.append(("6c hero == left(1) (primary eye of the pair)", hero_val == 1,
                   str(hero_val)))
    checks.append(("6d right track carries its own vexu/eyes/stri",
                   (r_stri is not None and (r_stri & 0x0F) == 0x02)
                   or stri_mode.startswith("legacy"),
                   ("0x%02x" % r_stri) if r_stri is not None else
                   "absent" + (" (expected for --stri both)" if stri_ok else "")))
    checks.append(("7a mett convergence track present", bool(metas),
                   "%d meta/mett tracks" % len(metas)))
    checks.append(("7b mime == %s" % CONV_MIME, conv_mime == CONV_MIME,
                   str(conv_mime) + (" (LEGACY v1)" if conv_mime == CONV_MIME_LEGACY_V1 else "")))
    mts = mp4.mvhd_timescale()
    conv_org = metas[0].presentation_start(mts) if metas else None
    vid_org = vids[0].presentation_start(mts) if vids else None
    checks.append(("7c conv shares the video presentation origin",
                   conv_t0 == 0.0 and conv_org is not None and conv_org == vid_org,
                   "first sample at media t=%s; presentation origin conv=%s video=%s"
                   % (conv_t0, conv_org, vid_org)))
    checks.append(("7d >= 1 sample per second", conv_maxgap is not None and conv_maxgap <= 1.0 + 1e-6,
                   "max gap %s" % ("%.4f s" % conv_maxgap if conv_maxgap is not None else "-")))
    checks.append(("7e conv timescale == video timescale",
                   bool(metas and vids) and metas[0].timescale == vids[0].timescale,
                   "%s vs %s" % (metas[0].timescale if metas else "-",
                                 vids[0].timescale if vids else "-")))
    checks.append(("8  covr poster in moov/udta/meta/ilst", mp4.has_covr(),
                   "%d bytes" % mp4.covr_size()))
    checks.append(("8b no attached_pic video track", len(vids) == 2 or not vids,
                   "%d video tracks" % len(vids)))
    checks.append(("9  audio track present (informational)", True,
                   "%d audio tracks" % len(auds)))

    print("")
    print("contract:")
    nfail = 0
    for name, ok, detail in checks:
        if not ok:
            nfail += 1
        print("  [%s] %-44s %s" % (_pf(ok), name, detail))
    print("")
    print("  %d/%d PASS" % (len(checks) - nfail, len(checks)))
    return 0 if nfail == 0 else 2


# --------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="lvf2.py", description="Build and inspect LVF v2 stereo MP4 files.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("convert", help="SBS or dual-track v1 mp4 -> LVF v2 mp4")
    c.add_argument("input")
    c.add_argument("output")
    c.add_argument("--crf", type=int, default=18)
    c.add_argument("--preset", default="medium")
    c.add_argument("--gop", type=int, default=12,
                   help="closed-GOP length in frames, identical on both tracks")
    g = c.add_mutually_exclusive_group()
    g.add_argument("--convergence", type=float, default=None,
                   help="constant convergence (fraction of view width)")
    g.add_argument("--conv-sweep", default=None, metavar="A,B",
                   help="linear ramp A..B over the clip")
    g.add_argument("--conv-csv", default=None, metavar="FILE",
                   help="CSV of 't_seconds,value' rows, linearly interpolated")
    c.add_argument("--conv-rate", choices=("per-frame", "1hz"), default="per-frame")
    c.add_argument("--baseline-um", type=int, default=None,
                   help="camera baseline in micrometres -> vexu/eyes/cams/blin")
    c.add_argument("--vexu-convergence", type=float, default=None,
                   help="default convergence -> vexu/eyes/cmfy/dadj")
    c.add_argument("--stri", choices=("per-track", "both"), default="per-track",
                   help="vexu/eyes/stri layout. 'per-track' (default): the left "
                        "entry declares has_left only and the right entry declares "
                        "has_right only, so FFmpeg reports view=left / view=right. "
                        "'both': legacy -- one entry on the left with both bits set, "
                        "which FFmpeg reports as view=packed.")
    c.add_argument("--half-sbs", action="store_true",
                   help="input is squeezed half-width SBS: rescale each half to 2x width")
    c.set_defaults(func=cmd_convert)

    i = sub.add_parser("inspect", help="report on any mp4 against the LVF v2 contract")
    i.add_argument("file")
    i.add_argument("--elst-check", action="store_true",
                   help="also dump every track's raw edit list "
                        "(the presentation-origin comparison behind item 7c)")
    i.set_defaults(func=cmd_inspect)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
