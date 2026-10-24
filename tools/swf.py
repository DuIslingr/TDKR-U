#!/usr/bin/env python3
"""Parse TDKR's Flash files (gameswf_*/*.swf) and render frames for checking.

The game's UI is Flash 8-10 (ActionScript 2 only: DoAction / DoInitAction, no DoABC),
played by Gameloft's gameswf. Its exporter moved the bitmaps out of the SWF into texture
atlases beside it (<name>_atlas<id>.tga, PVR like the other .tga): the SWF keeps a tiny
DefineBitsLossless per atlas id and the bitmap fills' matrices place the image in it.

Tags in use (all 104 files): shapes 1-4, DefineBitsLossless(2), DefineEditText with
DefineFont3 outlines, sprites, PlaceObject2/3, RemoveObject2, FrameLabel, ExportAssets,
DoAction, DoInitAction. No buttons, morph shapes, sounds or video.

Usage:
  swf.py info FILE.swf                 header, characters, exports, root frames/labels
  swf.py tree FILE.swf [ID|NAME]       a sprite's timeline (root by default)
  swf.py actions FILE.swf [ID|NAME]    disassembled ActionScript
  swf.py texts FILE.swf                every edit text: variable, font, initial text
  swf.py render FILE.swf OUT.png [--frame N] [--char ID|NAME] [--scale S] [--atlas-dir DIR]
"""

import argparse
import math
import os
import struct
import sys
import zlib
from dataclasses import dataclass, field

TAG_NAMES = {0: "End", 1: "ShowFrame", 2: "DefineShape", 4: "PlaceObject", 5: "RemoveObject",
             6: "DefineBits", 8: "JPEGTables", 9: "SetBackgroundColor", 12: "DoAction",
             20: "DefineBitsLossless", 21: "DefineBitsJPEG2", 22: "DefineShape2", 26: "PlaceObject2",
             28: "RemoveObject2", 32: "DefineShape3", 35: "DefineBitsJPEG3", 36: "DefineBitsLossless2",
             37: "DefineEditText", 39: "DefineSprite", 43: "FrameLabel", 48: "DefineFont2",
             56: "ExportAssets", 59: "DoInitAction", 63: "DebugID", 64: "EnableDebugger2",
             69: "FileAttributes", 70: "PlaceObject3", 73: "DefineFontAlignZones", 74: "CSMTextSettings",
             75: "DefineFont3", 77: "Metadata", 83: "DefineShape4", 88: "DefineFontName"}


class Bits:
    """SWF bit reader over a byte string (MSB first), with byte-aligned helpers."""

    def __init__(self, data, pos=0):
        self.d, self.p, self.bit = data, pos, 0

    def align(self):
        if self.bit:
            self.bit = 0
            self.p += 1

    def ub(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.d[self.p] >> (7 - self.bit)) & 1)
            self.bit += 1
            if self.bit == 8:
                self.bit = 0
                self.p += 1
        return v

    def sb(self, n):
        v = self.ub(n)
        return v - (1 << n) if n and v & (1 << (n - 1)) else v

    def fb(self, n):
        return self.sb(n) / 65536.0

    def u8(self):
        self.align()
        self.p += 1
        return self.d[self.p - 1]

    def u16(self):
        self.align()
        self.p += 2
        return struct.unpack_from("<H", self.d, self.p - 2)[0]

    def s16(self):
        self.align()
        self.p += 2
        return struct.unpack_from("<h", self.d, self.p - 2)[0]

    def u32(self):
        self.align()
        self.p += 4
        return struct.unpack_from("<I", self.d, self.p - 4)[0]

    def string(self):
        self.align()
        e = self.d.index(b"\0", self.p)
        s = self.d[self.p:e].decode("utf-8", "replace")
        self.p = e + 1
        return s

    def rect(self):
        self.align()
        n = self.ub(5)
        r = tuple(self.sb(n) for _ in range(4))   # xmin, xmax, ymin, ymax (twips)
        self.align()
        return r

    def matrix(self):
        """(a, b, c, d, tx, ty): x' = a*x + c*y + tx, y' = b*x + d*y + ty (twips)."""
        self.align()
        a = d = 1.0
        b = c = 0.0
        if self.ub(1):
            n = self.ub(5)
            a, d = self.fb(n), self.fb(n)
        if self.ub(1):
            n = self.ub(5)
            b, c = self.fb(n), self.fb(n)
        n = self.ub(5)
        tx, ty = self.sb(n), self.sb(n)
        self.align()
        return (a, b, c, d, float(tx), float(ty))

    def cxform(self, alpha):
        """((mr, mg, mb, ma), (ar, ag, ab, aa)): c' = c * m + a, multipliers as fractions."""
        self.align()
        has_add, has_mult = self.ub(1), self.ub(1)
        n = self.ub(4)
        k = 4 if alpha else 3
        mult = [1.0] * 4
        add = [0.0] * 4
        if has_mult:
            for i in range(k):
                mult[i] = self.sb(n) / 256.0
        if has_add:
            for i in range(k):
                add[i] = self.sb(n) / 255.0
        self.align()
        return (tuple(mult), tuple(add))

    def rgb(self, alpha):
        self.align()
        c = self.d[self.p:self.p + (4 if alpha else 3)]
        self.p += len(c)
        return (c[0] / 255, c[1] / 255, c[2] / 255, c[3] / 255 if alpha else 1.0)


IDENTITY = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)
NO_CXFORM = ((1.0, 1.0, 1.0, 1.0), (0.0, 0.0, 0.0, 0.0))


def mat_mul(m, n):
    """m after n (apply n first)."""
    a, b, c, d, tx, ty = m
    a2, b2, c2, d2, tx2, ty2 = n
    return (a * a2 + c * b2, b * a2 + d * b2, a * c2 + c * d2, b * c2 + d * d2,
            a * tx2 + c * ty2 + tx, b * tx2 + d * ty2 + ty)


def cx_mul(outer, inner):
    (m1, a1), (m2, a2) = outer, inner
    return (tuple(m1[i] * m2[i] for i in range(4)), tuple(a2[i] * m1[i] + a1[i] for i in range(4)))


# --- characters ---------------------------------------------------------------------

@dataclass
class FillStyle:
    kind: int                       # 0 solid, 0x10 linear, 0x12 radial, 0x13 focal, 0x40-0x43 bitmap
    color: tuple = (0, 0, 0, 1)
    matrix: tuple = IDENTITY
    stops: list = field(default_factory=list)    # [(ratio 0..1, rgba)]
    spread: int = 0
    focal: float = 0.0
    bitmap: int = 0
    smooth: bool = True             # 0x40/0x42 smoothed, 0x41/0x43 not
    repeat: bool = True             # 0x40/0x41 repeat, 0x42/0x43 clipped


@dataclass
class LineStyle:
    width: float                    # twips
    color: tuple = (0, 0, 0, 1)
    fill: FillStyle = None


@dataclass
class StyleGroup:
    """A shape's edges with the style arrays they index (NewStyles starts another group)."""
    fills: list
    lines: list
    fill_edges: dict = field(default_factory=dict)   # 1-based style -> [edge]; edge = (x0,y0,cx,cy,x1,y1) or 4-tuple
    line_edges: dict = field(default_factory=dict)   # 1-based style -> [edge], record order


@dataclass
class Shape:
    id: int
    bounds: tuple
    groups: list


@dataclass
class Bitmap:
    id: int
    width: int
    height: int
    rgba: bytes = b""               # straight alpha, width*height*4


@dataclass
class Font:
    id: int
    name: str
    bold: bool
    italic: bool
    glyphs: list                    # [Shape groups (one group each), em square 1024*20]
    codes: list
    ascent: int = 0
    descent: int = 0
    leading: int = 0
    advances: list = field(default_factory=list)

    def glyph_index(self, ch):
        try:
            return self.codes.index(ord(ch))
        except ValueError:
            return -1


@dataclass
class EditText:
    id: int
    bounds: tuple
    flags: dict
    font: int = 0
    font_class: str = ""
    height: int = 0                 # twips
    color: tuple = (0, 0, 0, 1)
    max_length: int = 0
    align: int = 0                  # 0 left 1 right 2 center 3 justify
    left_margin: int = 0
    right_margin: int = 0
    indent: int = 0
    leading: int = 0
    variable: str = ""
    text: str = ""


@dataclass
class Place:
    depth: int
    move: bool
    char: int = None
    matrix: tuple = None
    cxform: tuple = None
    ratio: int = None
    name: str = None
    clip_depth: int = None
    blend: int = None
    filters: bytes = None
    visible: int = None
    class_name: str = None
    clip_actions: list = None       # [(event flags, key code, actions bytes)]


@dataclass
class Frame:
    label: str = None
    tags: list = field(default_factory=list)       # Place / ('remove', depth) / ('action', bytes)


@dataclass
class Sprite:
    id: int
    frame_count: int
    frames: list

    def label_frame(self, label):
        for i, f in enumerate(self.frames):
            if f.label and f.label.lower() == label.lower():
                return i
        return None


class Swf:
    def __init__(self, path):
        data = open(path, "rb").read()
        self.path = path
        sig, self.version, size = data[:3], data[3], struct.unpack_from("<I", data, 4)[0]
        if sig == b"CWS":
            data = data[:8] + zlib.decompress(data[8:])
        elif sig != b"FWS":
            raise ValueError(f"{path}: not a SWF")
        self.data = data
        r = Bits(data, 8)
        self.frame_size = r.rect()
        self.frame_rate = r.u16() / 256.0
        frame_count = r.u16()
        self.characters = {}
        self.exports = {}
        self.init_actions = []          # [(sprite id, actions)]
        self.background = (1, 1, 1, 1)
        self.tag_counts = {}
        self.root = Sprite(0, frame_count, self._timeline(r.p, len(data)))

    @property
    def width(self):
        return (self.frame_size[1] - self.frame_size[0]) / 20

    @property
    def height(self):
        return (self.frame_size[3] - self.frame_size[2]) / 20

    def resolve(self, ref):
        """A character id or exported name -> id (0 = root)."""
        if ref is None:
            return 0
        if isinstance(ref, int) or ref.isdigit():
            return int(ref)
        for name, cid in self.exports.items():
            if name.lower() == ref.lower():
                return cid
        raise KeyError(ref)

    def sprite(self, cid):
        return self.root if cid == 0 else self.characters[cid]

    def export_name(self, cid):
        return next((n for n, c in self.exports.items() if c == cid), None)

    def _timeline(self, p, end):
        frames = [Frame()]
        d = self.data
        while p < end:
            h = struct.unpack_from("<H", d, p)[0]
            p += 2
            code, ln = h >> 6, h & 0x3F
            if ln == 0x3F:
                ln = struct.unpack_from("<I", d, p)[0]
                p += 4
            body = Bits(d, p)
            name = TAG_NAMES.get(code, str(code))
            self.tag_counts[name] = self.tag_counts.get(name, 0) + 1
            f = frames[-1]
            if code == 0:
                break
            elif code == 1:
                frames.append(Frame())
            elif code in (2, 22, 32, 83):
                sid = body.u16()
                self.characters[sid] = self._shape(body, sid, {2: 1, 22: 2, 32: 3, 83: 4}[code])
            elif code in (20, 36):
                self._bitmap(body, ln, code == 36)
            elif code == 37:
                self._edit_text(body)
            elif code == 75 or code == 48:
                self._font(body, code)
            elif code == 39:
                sid, n = body.u16(), body.u16()
                self.characters[sid] = Sprite(sid, n, self._timeline(body.p, p + ln))
            elif code == 26 or code == 70:
                f.tags.append(self._place(body, p + ln, code == 70))
            elif code == 28:
                f.tags.append(("remove", body.u16()))
            elif code == 5:
                body.u16()
                f.tags.append(("remove", body.u16()))
            elif code == 43:
                f.label = body.string()
            elif code == 12:
                f.tags.append(("action", d[p:p + ln]))
            elif code == 59:
                self.init_actions.append((body.u16(), d[p + 2:p + ln]))
            elif code == 56:
                for _ in range(body.u16()):
                    cid = body.u16()
                    self.exports[body.string()] = cid
            elif code == 9:
                self.background = body.rgb(False)
            p += ln
        if not frames[-1].tags and frames[-1].label is None:
            frames.pop()
        return frames

    # --- shapes
    def _fill_styles(self, r, ver):
        n = r.u8()
        if n == 0xFF and ver >= 2:
            n = r.u16()
        return [self._fill_style(r, ver) for _ in range(n)]

    def _fill_style(self, r, ver):
        t = r.u8()
        fs = FillStyle(t)
        if t == 0:
            fs.color = r.rgb(ver >= 3)
        elif t in (0x10, 0x12, 0x13):
            fs.matrix = r.matrix()
            r.align()
            fs.spread = r.ub(2)
            r.ub(2)
            n = r.ub(4)
            for _ in range(n):
                ratio = r.u8() / 255
                fs.stops.append((ratio, r.rgb(ver >= 3)))
            if t == 0x13:
                fs.focal = r.s16() / 256
        elif 0x40 <= t <= 0x43:
            fs.bitmap = r.u16()
            fs.matrix = r.matrix()
            fs.repeat = t in (0x40, 0x41)
            fs.smooth = t in (0x40, 0x42)
        else:
            raise ValueError(f"fill style 0x{t:x}")
        return fs

    def _line_styles(self, r, ver):
        n = r.u8()
        if n == 0xFF:
            n = r.u16()
        out = []
        for _ in range(n):
            w = r.u16()
            if ver == 4:
                r.ub(2)             # start cap
                join = r.ub(2)
                has_fill = r.ub(1)
                r.ub(3)             # no h/v scale, pixel hinting
                r.ub(5)
                r.ub(1)             # no close
                r.ub(2)             # end cap
                if join == 2:
                    r.u16()
                if has_fill:
                    fill = self._fill_style(r, ver)
                    out.append(LineStyle(w, fill.color, fill))
                else:
                    out.append(LineStyle(w, r.rgb(True)))
            else:
                out.append(LineStyle(w, r.rgb(ver >= 3)))
        return out

    def _shape(self, r, sid, ver):
        bounds = r.rect()
        if ver == 4:
            r.rect()
            r.u8()
        fills, lines = self._fill_styles(r, ver), self._line_styles(r, ver)
        r.align()
        nf, nl = r.ub(4), r.ub(4)
        groups = self._shape_records(r, ver, fills, lines, nf, nl)
        return Shape(sid, bounds, groups)

    def _shape_records(self, r, ver, fills, lines, nf, nl):
        g = StyleGroup(fills, lines)
        groups = [g]
        x = y = 0
        f0 = f1 = ln = 0
        while True:
            if r.ub(1) == 0:
                flags = r.ub(5)
                if flags == 0:
                    break
                new_styles, s_line, s_f1, s_f0, s_move = [(flags >> i) & 1 for i in (4, 3, 2, 1, 0)]
                if s_move:
                    n = r.ub(5)
                    x, y = r.sb(n), r.sb(n)
                if s_f0:
                    f0 = r.ub(nf)
                if s_f1:
                    f1 = r.ub(nf)
                if s_line:
                    ln = r.ub(nl)
                if new_styles:
                    fills, lines = self._fill_styles(r, ver), self._line_styles(r, ver)
                    r.align()
                    nf, nl = r.ub(4), r.ub(4)
                    g = StyleGroup(fills, lines)
                    groups.append(g)
                continue
            straight = r.ub(1)
            n = r.ub(4) + 2
            if straight:
                if r.ub(1):
                    dx, dy = r.sb(n), r.sb(n)
                elif r.ub(1):
                    dx, dy = 0, r.sb(n)
                else:
                    dx, dy = r.sb(n), 0
                e = (x, y, x + dx, y + dy)
                x, y = x + dx, y + dy
            else:
                cx, cy = x + r.sb(n), y + r.sb(n)
                ax, ay = cx + r.sb(n), cy + r.sb(n)
                e = (x, y, cx, cy, ax, ay)
                x, y = ax, ay
            if f1:
                g.fill_edges.setdefault(f1, []).append(e)
            if f0:
                g.fill_edges.setdefault(f0, []).append(reverse_edge(e))
            if ln:
                g.line_edges.setdefault(ln, []).append(e)
        r.align()
        return groups

    # --- bitmaps, text, fonts
    def _bitmap(self, r, ln, alpha):
        start = r.p
        cid, fmt, w, h = r.u16(), r.u8(), r.u16(), r.u16()
        ncolors = r.u8() + 1 if fmt == 3 else 0
        if r.p >= start + ln:      # an atlas placeholder: the pixels are in <name>_atlas<id>.tga
            self.characters[cid] = Bitmap(cid, w, h)
            return
        raw = zlib.decompress(self.data[r.p:start + ln])
        out = bytearray(w * h * 4)
        if fmt == 3:
            cs = 4 if alpha else 3
            pal = [tuple(raw[i * cs:i * cs + cs]) + ((255,) if not alpha else ()) for i in range(ncolors)]
            stride = (w + 3) & ~3
            base = ncolors * cs
            for yy in range(h):
                for xx in range(w):
                    c = pal[raw[base + yy * stride + xx]]
                    out[(yy * w + xx) * 4:(yy * w + xx) * 4 + 4] = bytes(unpremultiply(c) if alpha else c)
        elif fmt == 5:
            for i in range(w * h):
                a, rr, gg, bb = raw[i * 4:i * 4 + 4]
                c = (rr, gg, bb, a if alpha else 255)
                out[i * 4:i * 4 + 4] = bytes(unpremultiply(c) if alpha else c)
        elif fmt == 4:
            stride = (w * 2 + 3) & ~3
            for yy in range(h):
                for xx in range(w):
                    v = struct.unpack_from(">H", raw, yy * stride + xx * 2)[0]
                    c = ((v >> 10 & 31) * 255 // 31, (v >> 5 & 31) * 255 // 31, (v & 31) * 255 // 31, 255)
                    out[(yy * w + xx) * 4:(yy * w + xx) * 4 + 4] = bytes(c)
        self.characters[cid] = Bitmap(cid, w, h, bytes(out))

    def _edit_text(self, r):
        cid = r.u16()
        bounds = r.rect()
        b1, b2 = r.u8(), r.u8()
        names1 = ["has_font", "has_max_length", "has_color", "read_only", "password", "multiline", "word_wrap", "has_text"]
        names2 = ["use_outlines", "html", "was_static", "border", "no_select", "has_layout", "auto_size", "has_font_class"]
        flags = {n: bool(b1 >> i & 1) for i, n in enumerate(names1)}
        flags.update({n: bool(b2 >> i & 1) for i, n in enumerate(names2)})
        t = EditText(cid, bounds, flags)
        if flags["has_font"]:
            t.font = r.u16()
        if flags["has_font_class"]:
            t.font_class = r.string()
        if flags["has_font"] or flags["has_font_class"]:
            t.height = r.u16()
        if flags["has_color"]:
            t.color = r.rgb(True)
        if flags["has_max_length"]:
            t.max_length = r.u16()
        if flags["has_layout"]:
            t.align, t.left_margin, t.right_margin, t.indent, t.leading = r.u8(), r.u16(), r.u16(), r.u16(), r.s16()
        t.variable = r.string()
        if flags["has_text"]:
            t.text = r.string()
        self.characters[cid] = t

    def _font(self, r, code):
        cid = r.u16()
        flags = r.u8()
        wide_offsets, wide_codes = flags & 8, flags & 4
        r.u8()
        name = self.data[r.p + 1:r.p + 1 + self.data[r.p]].decode("latin-1").rstrip("\0")
        r.p += 1 + self.data[r.p]
        n = r.u16()
        table = r.p
        offsets = [r.u32() if wide_offsets else r.u16() for _ in range(n)]
        code_off = r.u32() if wide_offsets else r.u16()
        glyphs = []
        for i in range(n):
            g = Bits(self.data, table + offsets[i])
            nf, nl = g.ub(4), g.ub(4)
            glyphs.append(self._shape_records(g, 1, [FillStyle(0)], [], nf, nl)[0])
        r.p = table + code_off
        codes = [r.u16() if wide_codes else r.u8() for _ in range(n)]
        f = Font(cid, name, bool(flags & 1), bool(flags & 2), glyphs, codes)
        if flags & 0x80:
            f.ascent, f.descent, f.leading = r.u16(), r.u16(), r.s16()
            f.advances = [r.s16() for _ in range(n)]
        f.scale = 20.0 if code == 75 else 1.0     # DefineFont3 glyphs: 20x em units
        self.characters[cid] = f

    # --- placement
    def _place(self, r, end, v3):
        flags = r.u8()
        flags2 = r.u8() if v3 else 0
        pl = Place(r.u16(), bool(flags & 1))
        if v3 and (flags2 & 8 or (flags2 & 16 and flags & 2)):
            pl.class_name = r.string()
        if flags & 2:
            pl.char = r.u16()
        if flags & 4:
            pl.matrix = r.matrix()
        if flags & 8:
            pl.cxform = r.cxform(True)
        if flags & 16:
            pl.ratio = r.u16()
        if flags & 32:
            pl.name = r.string()
        if flags & 64:
            pl.clip_depth = r.u16()
        if v3:
            if flags2 & 1:
                fp = r.p
                skip_filters(r)
                pl.filters = self.data[fp:r.p]
            if flags2 & 2:
                pl.blend = r.u8()
            if flags2 & 4:
                r.u8()
            if flags2 & 32:
                pl.visible = r.u8()
                r.p += 4
        if flags & 128:
            r.u16()
            r.u32() if self.version >= 6 else r.u16()
            pl.clip_actions = []
            while r.p < end:
                ev = r.u32() if self.version >= 6 else r.u16()
                if ev == 0:
                    break
                size = r.u32()
                key = None
                if ev & (1 << 17):
                    key = r.u8()
                    size -= 1
                pl.clip_actions.append((ev, key, self.data[r.p:r.p + size]))
                r.p += size
        return pl


def skip_filters(r):
    for _ in range(r.u8()):
        t = r.u8()
        if t == 0:              # drop shadow
            r.p += 23
        elif t == 1:            # blur
            r.p += 9
        elif t == 2:            # glow
            r.p += 15
        elif t == 3:            # bevel
            r.p += 27
        elif t in (4, 7):       # gradient glow / bevel
            n = r.u8()
            r.p += n * 5 + 19
        elif t == 5:            # convolution
            x, y = r.u8(), r.u8()
            r.p += 8 + x * y * 4 + 5
        elif t == 6:            # color matrix
            r.p += 80


def unpremultiply(c):
    r, g, b, a = c
    if a == 0:
        return (0, 0, 0, 0)
    if a == 255:
        return (r, g, b, 255)
    return (min(255, r * 255 // a), min(255, g * 255 // a), min(255, b * 255 // a), a)


def reverse_edge(e):
    if len(e) == 4:
        return (e[2], e[3], e[0], e[1])
    return (e[4], e[5], e[2], e[3], e[0], e[1])


def chain(edges):
    """Join a fill style's edges into closed contours: [[edge...]]."""
    by_start = {}
    for i, e in enumerate(edges):
        by_start.setdefault((e[0], e[1]), []).append(i)
    used = [False] * len(edges)
    contours = []
    for i in range(len(edges)):
        if used[i]:
            continue
        used[i] = True
        c = [edges[i]]
        start = (edges[i][0], edges[i][1])
        while (c[-1][-2], c[-1][-1]) != start:
            cand = by_start.get((c[-1][-2], c[-1][-1]), [])
            nxt = next((j for j in cand if not used[j]), None)
            if nxt is None:
                break       # open contour: the fill closes it with a straight line
            used[nxt] = True
            c.append(edges[nxt])
        contours.append(c)
    return contours


# --- ActionScript 2 ----------------------------------------------------------------------

ACTIONS = {0x04: "NextFrame", 0x05: "PrevFrame", 0x06: "Play", 0x07: "Stop", 0x08: "ToggleQuality",
           0x09: "StopSounds", 0x0A: "Add", 0x0B: "Subtract", 0x0C: "Multiply", 0x0D: "Divide",
           0x0E: "Equals", 0x0F: "Less", 0x10: "And", 0x11: "Or", 0x12: "Not", 0x13: "StringEquals",
           0x14: "StringLength", 0x15: "StringExtract", 0x17: "Pop", 0x18: "ToInteger",
           0x1C: "GetVariable", 0x1D: "SetVariable", 0x20: "SetTarget2", 0x21: "StringAdd",
           0x22: "GetProperty", 0x23: "SetProperty", 0x24: "CloneSprite", 0x25: "RemoveSprite",
           0x26: "Trace", 0x27: "StartDrag", 0x28: "EndDrag", 0x29: "StringLess", 0x2A: "Throw",
           0x2B: "CastOp", 0x2C: "ImplementsOp", 0x30: "RandomNumber", 0x31: "MBStringLength",
           0x32: "CharToAscii", 0x33: "AsciiToChar", 0x34: "GetTime", 0x35: "MBStringExtract",
           0x36: "MBCharToAscii", 0x37: "MBAsciiToChar", 0x3A: "Delete", 0x3B: "Delete2",
           0x3C: "DefineLocal", 0x3D: "CallFunction", 0x3E: "Return", 0x3F: "Modulo",
           0x40: "NewObject", 0x41: "DefineLocal2", 0x42: "InitArray", 0x43: "InitObject",
           0x44: "TypeOf", 0x45: "TargetPath", 0x46: "Enumerate", 0x47: "Add2", 0x48: "Less2",
           0x49: "Equals2", 0x4A: "ToNumber", 0x4B: "ToString", 0x4C: "PushDuplicate",
           0x4D: "StackSwap", 0x4E: "GetMember", 0x4F: "SetMember", 0x50: "Increment",
           0x51: "Decrement", 0x52: "CallMethod", 0x53: "NewMethod", 0x54: "InstanceOf",
           0x55: "Enumerate2", 0x60: "BitAnd", 0x61: "BitOr", 0x62: "BitXor", 0x63: "BitLShift",
           0x64: "BitRShift", 0x65: "BitURShift", 0x66: "StrictEquals", 0x67: "Greater",
           0x68: "StringGreater", 0x69: "Extends", 0x81: "GotoFrame", 0x83: "GetURL",
           0x87: "StoreRegister", 0x88: "ConstantPool", 0x8A: "WaitForFrame", 0x8B: "SetTarget",
           0x8C: "GoToLabel", 0x8D: "WaitForFrame2", 0x8E: "DefineFunction2", 0x8F: "Try",
           0x94: "With", 0x96: "Push", 0x99: "Jump", 0x9A: "GetURL2", 0x9B: "DefineFunction",
           0x9D: "If", 0x9E: "Call", 0x9F: "GotoFrame2"}


def cstr(d, p):
    e = d.index(b"\0", p)
    return d[p:e].decode("utf-8", "replace"), e + 1


def disasm(code, indent="", pool=None, out=None):
    """Disassemble AS2 bytecode into lines (functions indented under their definition)."""
    out = [] if out is None else out
    pool = [] if pool is None else pool
    p = 0
    while p < len(code):
        op = code[p]
        at = p
        p += 1
        if op == 0:
            out.append(f"{indent}{at:04x} End")
            break
        payload = b""
        if op >= 0x80:
            n = struct.unpack_from("<H", code, p)[0]
            payload = code[p + 2:p + 2 + n]
            p += 2 + n
        name = ACTIONS.get(op, f"op_{op:02x}")
        arg = ""
        if op == 0x96:
            vals, q = [], 0
            while q < len(payload):
                t = payload[q]
                q += 1
                if t == 0:
                    s, q = cstr(payload, q)
                    vals.append(repr(s))
                elif t == 1:
                    vals.append(f"{struct.unpack_from('<f', payload, q)[0]:g}")
                    q += 4
                elif t == 2:
                    vals.append("null")
                elif t == 3:
                    vals.append("undefined")
                elif t == 4:
                    vals.append(f"r{payload[q]}")
                    q += 1
                elif t == 5:
                    vals.append("true" if payload[q] else "false")
                    q += 1
                elif t == 6:
                    raw = payload[q + 4:q + 8] + payload[q:q + 4]
                    vals.append(f"{struct.unpack('<d', raw)[0]:g}")
                    q += 8
                elif t == 7:
                    vals.append(str(struct.unpack_from("<i", payload, q)[0]))
                    q += 4
                elif t == 8 or t == 9:
                    i = payload[q] if t == 8 else struct.unpack_from("<H", payload, q)[0]
                    q += 1 if t == 8 else 2
                    vals.append(repr(pool[i]) if i < len(pool) else f"c{i}")
            arg = ", ".join(vals)
        elif op == 0x88:
            n = struct.unpack_from("<H", payload, 0)[0]
            q = 2
            pool.clear()
            for _ in range(n):
                s, q = cstr(payload, q)
                pool.append(s)
            arg = f"{n} strings"
        elif op in (0x99, 0x9D):
            off = struct.unpack_from("<h", payload, 0)[0]
            arg = f"-> {p + off:04x}"
        elif op == 0x81:
            arg = str(struct.unpack_from("<H", payload, 0)[0])
        elif op in (0x8C, 0x8B):
            arg = repr(cstr(payload, 0)[0])
        elif op == 0x87:
            arg = f"r{payload[0]}"
        elif op == 0x9F:
            arg = f"play={payload[0] & 1}"
        elif op == 0x83:
            a, q = cstr(payload, 0)
            arg = f"{a!r} {cstr(payload, q)[0]!r}"
        elif op in (0x9B, 0x8E):
            fname, q = cstr(payload, 0)
            nparams = struct.unpack_from("<H", payload, q)[0]
            q += 2
            params = []
            if op == 0x8E:
                q += 3      # register count, flags
                for _ in range(nparams):
                    reg = payload[q]
                    s, q = cstr(payload, q + 1)
                    params.append(f"{s}" + (f"@r{reg}" if reg else ""))
            else:
                for _ in range(nparams):
                    s, q = cstr(payload, q)
                    params.append(s)
            size = struct.unpack_from("<H", payload, q)[0]
            out.append(f"{indent}{at:04x} {name} {fname or '<anon>'}({', '.join(params)})")
            disasm(code[p:p + size], indent + "    ", list(pool), out)
            p += size
            continue
        elif op == 0x94:
            size = struct.unpack_from("<H", payload, 0)[0]
            out.append(f"{indent}{at:04x} With")
            disasm(code[p:p + size], indent + "    ", pool, out)
            p += size
            continue
        out.append(f"{indent}{at:04x} {name} {arg}".rstrip())
    return out


# --- rendering (cairo) -------------------------------------------------------------------

class Renderer:
    """Draw a sprite's frame the way the timeline builds it (no ActionScript runs)."""

    def __init__(self, swf, atlas_dir=None):
        self.swf = swf
        self.atlas_dir = atlas_dir if atlas_dir is not None else os.path.dirname(swf.path)
        self.surfaces = {}

    def bitmap_surface(self, bid):
        import cairo
        if bid in self.surfaces:
            return self.surfaces[bid]
        stem = os.path.splitext(os.path.basename(self.swf.path))[0]
        atlas = os.path.join(self.atlas_dir, f"{stem}_atlas{bid}.tga")
        rgba, w, h = None, 0, 0
        if os.path.exists(atlas):
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            import textures
            img = textures.load_texture(atlas)
            h, w = img.shape[:2]
            rgba = img.tobytes()
        else:
            bm = self.swf.characters.get(bid)
            if isinstance(bm, Bitmap) and bm.rgba:
                rgba, w, h = bm.rgba, bm.width, bm.height
        if rgba is None:
            self.surfaces[bid] = None
            return None
        # cairo ARGB32 is premultiplied BGRA (little-endian)
        buf = bytearray(w * h * 4)
        for i in range(w * h):
            r, g, b, a = rgba[i * 4:i * 4 + 4]
            buf[i * 4:i * 4 + 4] = bytes((b * a // 255, g * a // 255, r * a // 255, a))
        s = cairo.ImageSurface.create_for_data(buf, cairo.FORMAT_ARGB32, w, h, w * 4)
        self.surfaces[bid] = (s, buf, w, h)
        return self.surfaces[bid]

    def frame_list(self, sprite, frame):
        """The display list after `frame` (0-based): depth -> Place (merged moves)."""
        dl = {}
        for f in sprite.frames[:frame + 1]:
            for t in f.tags:
                if isinstance(t, Place):
                    old = dl.get(t.depth)
                    if t.move and old is not None and t.char is None:
                        merged = Place(**{k: (getattr(t, k) if getattr(t, k) is not None else getattr(old, k))
                                          for k in Place.__dataclass_fields__})
                        dl[t.depth] = merged
                    elif t.move and old is not None:
                        merged = Place(**{k: (getattr(t, k) if getattr(t, k) is not None else getattr(old, k))
                                          for k in Place.__dataclass_fields__})
                        dl[t.depth] = merged
                    else:
                        dl[t.depth] = t
                elif t[0] == "remove":
                    dl.pop(t[1], None)
        return dl

    def draw_char(self, cr, cid, m, cx, frame=0):
        ch = self.swf.characters.get(cid)
        if isinstance(ch, Shape):
            self.draw_shape(cr, ch.groups, m, cx)
        elif isinstance(ch, Sprite):
            self.draw_sprite(cr, ch, frame, m, cx)
        elif isinstance(ch, EditText):
            self.draw_text(cr, ch, m, cx)

    def draw_sprite(self, cr, sprite, frame, m, cx):
        import cairo
        dl = self.frame_list(sprite, min(frame, len(sprite.frames) - 1)) if sprite.frames else {}
        depths = sorted(dl)
        i = 0
        while i < len(depths):
            pl = dl[depths[i]]
            pm = mat_mul(m, pl.matrix or IDENTITY)
            pcx = cx_mul(cx, pl.cxform or NO_CXFORM)
            if pl.visible == 0 or pl.char is None:
                i += 1
                continue
            if pl.clip_depth:
                # mask: draw the clipped layers into a group, then mask it with this one
                masked = []
                j = i + 1
                while j < len(depths) and depths[j] <= pl.clip_depth:
                    masked.append(dl[depths[j]])
                    j += 1
                cr.push_group()
                for q in masked:
                    if q.char is not None and q.visible != 0:
                        self.draw_char(cr, q.char, mat_mul(m, q.matrix or IDENTITY), cx_mul(cx, q.cxform or NO_CXFORM))
                content = cr.pop_group()
                cr.push_group()
                self.draw_char(cr, pl.char, pm, (NO_CXFORM[0], NO_CXFORM[1]))
                mask = cr.pop_group()
                cr.set_source(content)
                cr.mask(mask)
                i = j
                continue
            self.draw_char(cr, pl.char, pm, pcx)
            i += 1

    def apply_matrix(self, cr, m):
        import cairo
        a, b, c, d, tx, ty = m
        cr.set_matrix(cairo.Matrix(a, b, c, d, tx, ty))

    def path(self, cr, contours):
        for c in contours:
            cr.move_to(c[0][0], c[0][1])
            for e in c:
                if len(e) == 4:
                    cr.line_to(e[2], e[3])
                else:
                    x0, y0, qx, qy, x1, y1 = e
                    cr.curve_to(x0 + 2 / 3 * (qx - x0), y0 + 2 / 3 * (qy - y0),
                                x1 + 2 / 3 * (qx - x1), y1 + 2 / 3 * (qy - y1), x1, y1)
            cr.close_path()

    def set_paint(self, cr, fs, cx):
        import cairo
        if fs.kind == 0:
            cr.set_source_rgba(*xform_color(fs.color, cx))
            return True
        if fs.kind in (0x10, 0x12, 0x13):
            if fs.kind == 0x10:
                pat = cairo.LinearGradient(-16384, 0, 16384, 0)
            else:
                pat = cairo.RadialGradient(fs.focal * 16384, 0, 0, 0, 0, 16384)
            for ratio, col in fs.stops:
                pat.add_color_stop_rgba(ratio, *xform_color(col, cx))
            pat.set_extend([cairo.EXTEND_PAD, cairo.EXTEND_REFLECT, cairo.EXTEND_REPEAT][min(fs.spread, 2)])
            a, b, c, d, tx, ty = fs.matrix
            mm = cairo.Matrix(a, b, c, d, tx, ty)
            mm.invert()
            pat.set_matrix(mm)
            cr.set_source(pat)
            return True
        s = self.bitmap_surface(fs.bitmap)
        if s is None:
            cr.set_source_rgba(1, 0, 1, 0.5 * cx[0][3])
            return True
        pat = cairo.SurfacePattern(s[0])
        a, b, c, d, tx, ty = fs.matrix
        mm = cairo.Matrix(a, b, c, d, tx, ty)
        mm.invert()
        pat.set_matrix(mm)
        pat.set_filter(cairo.FILTER_BILINEAR if fs.smooth else cairo.FILTER_NEAREST)
        pat.set_extend(cairo.EXTEND_REPEAT if fs.repeat else cairo.EXTEND_PAD)
        cr.set_source(pat)
        self._bitmap_alpha = cx[0][3]
        return True

    def draw_shape(self, cr, groups, m, cx):
        for g in groups:
            for style, edges in sorted(g.fill_edges.items()):
                if style - 1 >= len(g.fills):
                    continue
                fs = g.fills[style - 1]
                self.apply_matrix(cr, m)
                cr.new_path()
                self.path(cr, chain(edges))
                cr.set_fill_rule(1)     # even-odd
                self._bitmap_alpha = None
                self.set_paint(cr, fs, cx)
                if self._bitmap_alpha is not None and self._bitmap_alpha < 1:
                    cr.save()
                    cr.clip()
                    cr.paint_with_alpha(max(0, self._bitmap_alpha))
                    cr.restore()
                else:
                    cr.fill()
            for style, edges in sorted(g.line_edges.items()):
                if style - 1 >= len(g.lines):
                    continue
                ls = g.lines[style - 1]
                self.apply_matrix(cr, m)
                cr.new_path()
                for e in edges:
                    cr.move_to(e[0], e[1])
                    if len(e) == 4:
                        cr.line_to(e[2], e[3])
                    else:
                        x0, y0, qx, qy, x1, y1 = e
                        cr.curve_to(x0 + 2 / 3 * (qx - x0), y0 + 2 / 3 * (qy - y0),
                                    x1 + 2 / 3 * (qx - x1), y1 + 2 / 3 * (qy - y1), x1, y1)
                cr.set_source_rgba(*xform_color(ls.color, cx))
                cr.set_line_cap(1)
                cr.set_line_join(1)
                cr.set_line_width(max(ls.width, 20))
                cr.stroke()

    def draw_text(self, cr, t, m, cx):
        import re
        text = t.text
        if t.flags["html"]:
            text = re.sub(r"<br\s*/?>|</p>", "\n", text, flags=re.I)
            text = re.sub(r"<[^>]+>", "", text)
            text = text.replace("&lt;", "<").replace("&gt;", ">").replace("&amp;", "&").replace("&quot;", '"').replace("&apos;", "'")
        font = self.swf.characters.get(t.font)
        x0, x1, y0, y1 = t.bounds
        size = t.height
        lines = text.replace("\r", "\n").split("\n")
        y = y0 + 40 + size * 0.8 if not isinstance(font, Font) or not font.ascent else y0 + 40 + font.ascent / 1024 / font.scale * size
        for line in lines:
            if isinstance(font, Font) and font.glyphs and t.flags["use_outlines"]:
                scale = size / (1024 * font.scale)
                width = sum((font.advances[font.glyph_index(ch)] if font.glyph_index(ch) >= 0 and font.advances else 512 * font.scale) * scale for ch in line)
                x = self.align_x(t, width)
                for ch in line:
                    gi = font.glyph_index(ch)
                    adv = (font.advances[gi] if gi >= 0 and font.advances else 512 * font.scale) * scale
                    if gi >= 0:
                        gm = mat_mul(m, (scale, 0, 0, scale, x, y))
                        g = font.glyphs[gi]
                        g2 = StyleGroup([FillStyle(0, t.color)], [], g.fill_edges, {})
                        self.draw_shape(cr, [g2], gm, cx)
                    x += adv
            else:
                self.apply_matrix(cr, m)
                cr.select_font_face(font.name if isinstance(font, Font) else "Sans")
                cr.set_font_size(size)
                width = cr.text_extents(line).x_advance
                cr.move_to(self.align_x(t, width), y)
                cr.set_source_rgba(*xform_color(t.color, cx))
                cr.show_text(line)
            y += size + t.leading + (font.leading / 1024 / font.scale * size if isinstance(font, Font) else 0)

    @staticmethod
    def align_x(t, width):
        x0, x1 = t.bounds[0] + 40 + t.left_margin, t.bounds[1] - 40 - t.right_margin
        if t.align == 1:
            return x1 - width
        if t.align == 2:
            return (x0 + x1 - width) / 2
        return x0 + t.indent

    def render(self, out, cid=0, frame=0, scale=1.0, background=True):
        import cairo
        swf = self.swf
        if cid == 0:
            x0, x1, y0, y1 = swf.frame_size
        else:
            x0, x1, y0, y1 = self.bounds(cid, frame)
        w, h = max(1, int(math.ceil((x1 - x0) / 20 * scale))), max(1, int(math.ceil((y1 - y0) / 20 * scale)))
        surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, w, h)
        cr = cairo.Context(surf)
        if background:
            cr.set_source_rgb(*swf.background[:3]) if cid == 0 else cr.set_source_rgb(0.25, 0.25, 0.25)
            cr.paint()
        base = (scale / 20, 0, 0, scale / 20, -x0 * scale / 20, -y0 * scale / 20)
        if cid == 0:
            self.draw_sprite(cr, swf.root, frame, base, NO_CXFORM)
        else:
            self.draw_char(cr, cid, base, NO_CXFORM, frame)
        surf.write_to_png(out)
        return w, h

    def bounds(self, cid, frame=0, m=IDENTITY, depth=0):
        """Twips bounds (xmin, xmax, ymin, ymax) of a character drawn at `m`."""
        ch = self.swf.characters.get(cid)
        pts = []
        if isinstance(ch, (Shape, EditText)):
            x0, x1, y0, y1 = ch.bounds
            pts = [(x0, y0), (x1, y0), (x0, y1), (x1, y1)]
        elif isinstance(ch, Sprite) and depth < 32 and ch.frames:
            for pl in self.frame_list(ch, min(frame, len(ch.frames) - 1)).values():
                if pl.char is not None:
                    b = self.bounds(pl.char, 0, mat_mul(m, pl.matrix or IDENTITY), depth + 1)
                    if b[0] < b[1]:
                        pts += [(b[0], b[2]), (b[1], b[3])]
                        continue
            if pts:
                return (min(p[0] for p in pts), max(p[0] for p in pts), min(p[1] for p in pts), max(p[1] for p in pts))
            return (0, 0, 0, 0)
        if not pts:
            return (0, 0, 0, 0)
        a, b, c, d, tx, ty = m
        tp = [(a * x + c * y + tx, b * x + d * y + ty) for x, y in pts]
        return (min(p[0] for p in tp), max(p[0] for p in tp), min(p[1] for p in tp), max(p[1] for p in tp))


def xform_color(c, cx):
    (m, a) = cx
    return tuple(max(0.0, min(1.0, c[i] * m[i] + a[i])) for i in range(4))


# --- command line --------------------------------------------------------------------------

def describe(swf, cid):
    ch = swf.characters.get(cid)
    name = swf.export_name(cid)
    kind = type(ch).__name__ if ch is not None else "?"
    extra = ""
    if isinstance(ch, Sprite):
        extra = f"{ch.frame_count} frames"
        labels = [f.label for f in ch.frames if f.label]
        if labels:
            extra += " labels=" + ",".join(labels[:12]) + ("…" if len(labels) > 12 else "")
    elif isinstance(ch, Bitmap):
        extra = f"{ch.width}x{ch.height}"
    elif isinstance(ch, EditText):
        extra = f"var={ch.variable!r} font={ch.font} h={ch.height / 20:g} text={ch.text[:40]!r}"
    elif isinstance(ch, Font):
        extra = f"{ch.name!r} {len(ch.glyphs)} glyphs"
    return f"{cid:5} {kind:9} {name or '':28} {extra}"


def cmd_info(swf, args):
    print(f"{swf.path}: Flash {swf.version}, {swf.width:g}x{swf.height:g}, {swf.frame_rate:g} fps, "
          f"{len(swf.root.frames)} root frames, {len(swf.characters)} characters, {len(swf.exports)} exports")
    print("tags:", ", ".join(f"{k}={v}" for k, v in sorted(swf.tag_counts.items(), key=lambda kv: -kv[1])))
    print("root labels:", [f.label for f in swf.root.frames if f.label])
    print("init actions for:", [swf.export_name(s) or s for s, _ in swf.init_actions][:40])
    for cid in sorted(swf.characters):
        ch = swf.characters[cid]
        if args.all or isinstance(ch, (Sprite, Bitmap, Font)) or swf.export_name(cid):
            print(describe(swf, cid))


def cmd_tree(swf, args):
    sp = swf.sprite(swf.resolve(args.ref))

    def walk(sprite, indent, seen):
        for i, f in enumerate(sprite.frames):
            parts = []
            for t in f.tags:
                if isinstance(t, Place):
                    parts.append(t)
                elif t[0] == "remove":
                    print(f"{indent}  [{i}] remove depth {t[1]}")
                else:
                    print(f"{indent}  [{i}] action {len(t[1])} bytes")
            if f.label:
                print(f"{indent}  [{i}] label {f.label!r}")
            for t in parts:
                if t.char is None and not args.moves:
                    continue
                desc = describe(swf, t.char).strip() if t.char is not None else "(move)"
                nm = f" name={t.name!r}" if t.name else ""
                cd = f" clip->{t.clip_depth}" if t.clip_depth else ""
                print(f"{indent}  [{i}] depth {t.depth}{nm}{cd}: {desc}")
                ch = swf.characters.get(t.char)
                if isinstance(ch, Sprite) and t.char not in seen and len(indent) < args.depth * 4:
                    walk(ch, indent + "    ", seen | {t.char})
    walk(sp, "", {sp.id})


def cmd_actions(swf, args):
    if args.ref is None:
        for sid, code in swf.init_actions:
            print(f"=== init action for {swf.export_name(sid) or sid}")
            print("\n".join(disasm(code)))
    sp = swf.sprite(swf.resolve(args.ref))
    for i, f in enumerate(sp.frames):
        for t in f.tags:
            if isinstance(t, tuple) and t[0] == "action":
                print(f"=== frame {i}" + (f" ({f.label})" if f.label else ""))
                print("\n".join(disasm(t[1])))
            elif isinstance(t, Place) and t.clip_actions:
                for ev, key, code in t.clip_actions:
                    print(f"=== frame {i} depth {t.depth} clip event 0x{ev:x}")
                    print("\n".join(disasm(code)))


def cmd_texts(swf, args):
    for cid, ch in sorted(swf.characters.items()):
        if isinstance(ch, EditText):
            print(describe(swf, cid))


def cmd_render(swf, args):
    r = Renderer(swf, args.atlas_dir)
    cid = swf.resolve(args.char) if args.char else 0
    w, h = r.render(args.out, cid, args.frame, args.scale)
    print(f"{args.out}: {w}x{h}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info")
    p.add_argument("swf")
    p.add_argument("--all", action="store_true")
    p = sub.add_parser("tree")
    p.add_argument("swf")
    p.add_argument("ref", nargs="?")
    p.add_argument("--depth", type=int, default=2)
    p.add_argument("--moves", action="store_true", help="also list moves of placed characters")
    p = sub.add_parser("actions")
    p.add_argument("swf")
    p.add_argument("ref", nargs="?")
    p = sub.add_parser("texts")
    p.add_argument("swf")
    p = sub.add_parser("render")
    p.add_argument("swf")
    p.add_argument("out")
    p.add_argument("--frame", type=int, default=0)
    p.add_argument("--char")
    p.add_argument("--scale", type=float, default=1.0)
    p.add_argument("--atlas-dir")
    args = ap.parse_args()
    swf = Swf(args.swf)
    {"info": cmd_info, "tree": cmd_tree, "actions": cmd_actions, "texts": cmd_texts, "render": cmd_render}[args.cmd](swf, args)


if __name__ == "__main__":
    main()
