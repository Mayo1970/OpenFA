#!/usr/bin/env python3
"""
fa_w01lib.py - pure-Python reader for WESTKA .W01 sprite / background sheets,
plus a tiny PNG encoder so Tkinter can show the decoded frames.

Ported from src/core/fa_w01.c. Header: 1063 bytes, magic u32 0x115 @0,
frame_count u16 @0x117, first frame-record offset u32 @0x41D. Each 22-byte
record: comp u16, w u16, h u16, next u32, pix_off u32, rect u16[4].
Pixels are little-endian RGB565; compression 0 store / 1 RLE8 / 2 RLE16.
"""
import struct
import zlib

HDR = 0x427
MAGIC = 0x115
REC = 0x16


def _u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def _u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


class W01:
    def __init__(self, data):
        self.b = data
        if len(data) < HDR or _u32(data, 0) != MAGIC:
            raise ValueError("not a .W01 sheet")
        n = _u16(data, 0x117)
        if not 0 < n <= 100000:
            raise ValueError("bad frame count")
        self.frames = []
        off = _u32(data, 0x41D)
        for _ in range(n):
            if off + REC > len(data):
                raise ValueError("frame record past EOF")
            comp = _u16(data, off)
            w = _u16(data, off + 2)
            h = _u16(data, off + 4)
            nxt = _u32(data, off + 6)
            pix = _u32(data, off + 0x0A)
            self.frames.append((comp, w, h, pix))
            off = nxt

    @classmethod
    def open(cls, path):
        return cls(open(path, "rb").read())

    def count(self):
        return len(self.frames)

    def size(self, i):
        _, w, h, _ = self.frames[i]
        return w, h

    def decode(self, i):
        """Return (w, h, [u16 rgb565] * w*h)."""
        comp, w, h, pix = self.frames[i]
        n = w * h
        b = self.b
        if comp == 0:
            return w, h, list(struct.unpack_from("<%dH" % n, b, pix))
        if comp == 1:                       # RLE8 over the byte stream
            need = n * 2
            raw = bytearray()
            p = pix
            while len(raw) < need:
                c = b[p] if b[p] < 128 else b[p] - 256
                raw.extend(bytes([b[p + 1]]) * max(0, c))
                p += 2
            return w, h, list(struct.unpack_from("<%dH" % n, bytes(raw[:need]), 0))
        # comp 2: RLE16
        out = []
        p = pix
        while len(out) < n:
            c = struct.unpack_from("<h", b, p)[0]
            p += 2
            if c >= 0:
                v = _u16(b, p)
                p += 2
                out.extend([v] * c)
            else:
                for _ in range(-c):
                    out.append(_u16(b, p))
                    p += 2
        return w, h, out[:n]


# --- RGB565 -> RGBA + PNG --------------------------------------------

def rgba_from_565(px, w, h, key=0x0000):
    """RGBA bytes; pixels equal to `key` become transparent."""
    out = bytearray(w * h * 4)
    for i, p in enumerate(px):
        if p == key:
            continue
        r = (p >> 11) & 0x1F
        g = (p >> 5) & 0x3F
        b = p & 0x1F
        o = i * 4
        out[o] = (r << 3) | (r >> 2)
        out[o + 1] = (g << 2) | (g >> 4)
        out[o + 2] = (b << 3) | (b >> 2)
        out[o + 3] = 255
    return bytes(out)


def scale_rgba(rgba, w, h, tw, th):
    """Nearest-neighbour resize of an RGBA buffer."""
    if (w, h) == (tw, th):
        return rgba
    out = bytearray(tw * th * 4)
    xm = [min(w - 1, x * w // tw) for x in range(tw)]
    for y in range(th):
        sy = min(h - 1, y * h // th)
        srow = sy * w * 4
        drow = y * tw * 4
        for x in range(tw):
            s = srow + xm[x] * 4
            out[drow + x * 4: drow + x * 4 + 4] = rgba[s:s + 4]
    return bytes(out)


def crop_rgba(rgba, w, h, x0, y0, cw, ch):
    out = bytearray(cw * ch * 4)
    for y in range(ch):
        sy = y0 + y
        if not 0 <= sy < h:
            continue
        s = (sy * w + x0) * 4
        d = y * cw * 4
        span = max(0, min(cw, w - x0)) * 4
        out[d:d + span] = rgba[s:s + span]
    return bytes(out)


def png(rgba, w, h):
    """Minimal RGBA PNG."""
    rows = bytearray()
    stride = w * 4
    for y in range(h):
        rows.append(0)                     # filter: none
        rows.extend(rgba[y * stride:(y + 1) * stride])

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(rows), 6))
            + chunk(b"IEND", b""))
