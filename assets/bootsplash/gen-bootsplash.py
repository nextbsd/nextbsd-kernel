#!/usr/bin/env python3
"""
gen-bootsplash.py -- the NextBSD boot splash assets, from one source (E19).

The loader paints the splash from PNGs and the kernel repaints it from a
compiled-in copy; both must be pixel-identical, so both come from here:

  input   nextbsd-cube.svg            the NextBSD cube (nextbsd.org's logo)
          the 12-spoke spinner        drawn below; NextBSD's own art

  output  nextbsd-logo.png            cube, 128x144 RGBA (1x)
          nextbsd-logo@2x.png         cube, 256x288 RGBA (2x)
          nextbsd-spinner.png         spinner frame 0, 40x40, white + alpha
          nextbsd-spinner@2x.png      spinner frame 0, 80x80, white + alpha
          ../../src-overlay/sys/dev/iokit/bootsplash_data.h
                                      the same logo pixels (run-length
                                      coded) and the spinner's 3 alpha
                                      frames at 1x and 2x, for the kernel

The loader installs the logo PNGs to /boot/images/ (nextbsd-freebsd-compat
carries a verbatim copy). Python 3 standard library only, deterministic.
Run it after changing the SVG or the spinner and commit everything;
`--check` verifies the committed outputs are current.

Rasteriser: a small SVG subset (polygon, line, linearGradient in
objectBoundingBox units, fill, stroke, stroke-width, stop-opacity), 8x8
supersampling, premultiplied "over" compositing. Per the SVG spec, a
gradient in objectBoundingBox units on a zero-width or zero-height shape
paints nothing (the cube's vertical edge highlight, as in a browser).

Spinner, as in the E19 quiet-console mockups: a 40 px box (80 px at 2x),
12 capsule spokes 3 px wide from radius 8 to 18 px, one every 30 degrees,
turning clockwise once a second in 12 steps. In step f the spoke at
30*i degrees has opacity 1 - 0.88 * ((f + 11 - i) mod 12) / 12. Steps 3k+j
are step j turned k quarter turns clockwise, so only steps 0, 1 and 2 are
stored and the kernel rotates them (xnu's vc_progress trick); this script
asserts that identity.
"""

import argparse
import math
import os
import struct
import sys
import xml.etree.ElementTree as ET
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SVG = os.path.join(HERE, "nextbsd-cube.svg")
HEADER = os.path.normpath(os.path.join(
    HERE, "..", "..", "src-overlay", "sys", "dev", "iokit",
    "bootsplash_data.h"))

LOGO_SIZES = {1: (128, 144), 2: (256, 288)}
SPIN_D = {1: 40, 2: 80}
SS = 8                      # supersampling grid per pixel axis
NS = "{http://www.w3.org/2000/svg}"


# ------------------------------------------------------------------ SVG ----

def parse_color(s):
    s = s.strip()
    if s.startswith("#"):
        h = s[1:]
        if len(h) == 3:
            h = "".join(c * 2 for c in h)
        return tuple(int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4))
    raise ValueError("unsupported colour %r" % s)


def parse_frac(s):
    s = s.strip()
    if s.endswith("%"):
        return float(s[:-1]) / 100.0
    return float(s)


class Gradient:
    def __init__(self, el):
        self.x1 = parse_frac(el.get("x1", "0"))
        self.y1 = parse_frac(el.get("y1", "0"))
        self.x2 = parse_frac(el.get("x2", "1"))
        self.y2 = parse_frac(el.get("y2", "0"))
        self.stops = []
        for st in el.findall(NS + "stop"):
            off = parse_frac(st.get("offset", "0"))
            col = parse_color(st.get("stop-color", "#000"))
            op = float(st.get("stop-opacity", "1"))
            self.stops.append((off, col, op))

    def at(self, t):
        st = self.stops
        if t <= st[0][0]:
            return st[0][1], st[0][2]
        for (o0, c0, a0), (o1, c1, a1) in zip(st, st[1:]):
            if t <= o1:
                u = 0.0 if o1 == o0 else (t - o0) / (o1 - o0)
                return (tuple(c0[k] + (c1[k] - c0[k]) * u for k in range(3)),
                        a0 + (a1 - a0) * u)
        return st[-1][1], st[-1][2]


class Paint:
    """A solid colour or an objectBoundingBox linear gradient on a bbox."""

    def __init__(self, spec, grads, bbox):
        self.none = spec is None or spec == "none"
        self.solid = None
        self.grad = None
        if self.none:
            return
        if spec.startswith("url(#"):
            self.grad = grads[spec[5:-1]]
            x0, y0, x1, y1 = bbox
            w, h = x1 - x0, y1 - y0
            if w <= 0 or h <= 0:
                self.none = True        # SVG: nothing is painted
                return
            g = self.grad
            self.p1 = (x0 + g.x1 * w, y0 + g.y1 * h)
            self.p2 = (x0 + g.x2 * w, y0 + g.y2 * h)
            dx, dy = self.p2[0] - self.p1[0], self.p2[1] - self.p1[1]
            self.dd = dx * dx + dy * dy
            self.dx, self.dy = dx, dy
        else:
            self.solid = (parse_color(spec), 1.0)

    def at(self, x, y):
        if self.solid is not None:
            return self.solid
        t = ((x - self.p1[0]) * self.dx + (y - self.p1[1]) * self.dy) / self.dd
        return self.grad.at(min(1.0, max(0.0, t)))


def seg_dist2(px, py, ax, ay, bx, by, caps):
    dx, dy = bx - ax, by - ay
    ll = dx * dx + dy * dy
    t = ((px - ax) * dx + (py - ay) * dy) / ll if ll else 0.0
    if t < 0.0 or t > 1.0:
        if not caps:
            return float("inf")
        t = min(1.0, max(0.0, t))
    qx, qy = ax + t * dx, ay + t * dy
    return (px - qx) ** 2 + (py - qy) ** 2


def inside(px, py, pts):
    c = False
    n = len(pts)
    for i in range(n):
        x0, y0 = pts[i]
        x1, y1 = pts[(i + 1) % n]
        if (y0 > py) != (y1 > py):
            if px < x0 + (py - y0) * (x1 - x0) / (y1 - y0):
                c = not c
    return c


class Shape:
    def __init__(self, el, grads):
        tag = el.tag[len(NS):]
        if tag == "polygon":
            nums = el.get("points").replace(",", " ").split()
            v = [float(n) for n in nums]
            self.pts = list(zip(v[0::2], v[1::2]))
            self.closed = True
        elif tag == "line":
            self.pts = [(float(el.get("x1")), float(el.get("y1"))),
                        (float(el.get("x2")), float(el.get("y2")))]
            self.closed = False
        else:
            raise ValueError("unsupported element %s" % tag)
        xs = [p[0] for p in self.pts]
        ys = [p[1] for p in self.pts]
        bbox = (min(xs), min(ys), max(xs), max(ys))
        self.fill = Paint(el.get("fill", "#000") if self.closed else "none",
                          grads, bbox)
        self.stroke = Paint(el.get("stroke", "none"), grads, bbox)
        self.hw = float(el.get("stroke-width", "1")) / 2.0
        n = len(self.pts)
        if self.closed:
            self.segs = [(self.pts[i], self.pts[(i + 1) % n]) for i in range(n)]
        else:
            self.segs = [(self.pts[0], self.pts[1])]
        m = self.hw + 1e-9
        self.box = (bbox[0] - m, bbox[1] - m, bbox[2] + m, bbox[3] + m)

    def paint(self, x, y):
        """Yield (rgb, alpha) layers this shape puts at (x, y), in order."""
        b = self.box
        if x < b[0] or x > b[2] or y < b[1] or y > b[3]:
            return
        if not self.fill.none and inside(x, y, self.pts):
            yield self.fill.at(x, y)
        if not self.stroke.none:
            hw2 = self.hw * self.hw
            for (a, c) in self.segs:
                # Polygons: joins approximated as round; lines: butt caps.
                if seg_dist2(x, y, a[0], a[1], c[0], c[1],
                             self.closed) <= hw2:
                    yield self.stroke.at(x, y)
                    break


def load_svg(path):
    root = ET.parse(path).getroot()
    vb = [float(v) for v in root.get("viewBox").split()]
    grads = {}
    for g in root.iter(NS + "linearGradient"):
        grads[g.get("id")] = Gradient(g)
    shapes = []
    for el in root:
        if el.tag in (NS + "polygon", NS + "line"):
            shapes.append(Shape(el, grads))
    return vb, shapes


def render_svg(vb, shapes, w, h):
    sx, sy = vb[2] / w, vb[3] / h
    offs = [(k + 0.5) / SS for k in range(SS)]
    out = bytearray(w * h * 4)
    n = SS * SS
    for py in range(h):
        for px in range(w):
            ar = ag = ab = aa = 0.0
            for oy in offs:
                y = vb[1] + (py + oy) * sy
                for ox in offs:
                    x = vb[0] + (px + ox) * sx
                    r = g = b = a = 0.0        # premultiplied
                    for s in shapes:
                        for (c, op) in s.paint(x, y):
                            r = c[0] * op + r * (1 - op)
                            g = c[1] * op + g * (1 - op)
                            b = c[2] * op + b * (1 - op)
                            a = op + a * (1 - op)
                    ar += r
                    ag += g
                    ab += b
                    aa += a
            o = (py * w + px) * 4
            a = aa / n
            if a <= 0.0:
                continue
            out[o + 0] = min(255, int(round(ar / n / a * 255)))
            out[o + 1] = min(255, int(round(ag / n / a * 255)))
            out[o + 2] = min(255, int(round(ab / n / a * 255)))
            out[o + 3] = min(255, int(round(a * 255)))
    return bytes(out)


# -------------------------------------------------------------- spinner ----

def spinner_frame(f, scale):
    d = SPIN_D[scale]
    c = d / 2.0
    r = 1.5 * scale
    y_in, y_out = -(8.0 * scale + r), -(18.0 * scale - r)   # capsule axis
    offs = [(k + 0.5) / SS for k in range(SS)]
    ops = [1.0 - 0.88 * (((f + 11 - i) % 12) / 12.0) for i in range(12)]
    rot = [(math.cos(math.radians(30 * i)), math.sin(math.radians(30 * i)))
           for i in range(12)]
    out = bytearray(d * d)
    for py in range(d):
        for px in range(d):
            acc = 0.0
            for oy in offs:
                y = py + oy - c
                for ox in offs:
                    x = px + ox - c
                    a = 0.0
                    for i in range(12):
                        co, si = rot[i]
                        # Turn the point back by 30*i degrees (clockwise
                        # on screen is +angle with y down).
                        lx = x * co + y * si
                        ly = -x * si + y * co
                        t = min(y_in, max(y_out, ly))
                        if lx * lx + (ly - t) ** 2 <= r * r:
                            a = max(a, ops[i])
                    acc += a
            out[py * d + px] = int(round(acc / (SS * SS) * 255))
    return bytes(out)


def rot90cw(a, d):
    """dst(x, y) = src(y, d-1-x): the kernel blitter's quarter turn."""
    o = bytearray(d * d)
    for y in range(d):
        for x in range(d):
            o[y * d + x] = a[(d - 1 - x) * d + y]
    return bytes(o)


# ------------------------------------------------------------------ PNG ----

def png_bytes(w, h, rgba):
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(t, data):
        c = struct.pack(">I", len(data)) + t + data
        return c + struct.pack(">I", zlib.crc32(t + data) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def png_pixels(path):
    """(w, h, rgba) of a PNG this script wrote (8-bit RGBA, filter 0)."""
    data = open(path, "rb").read()
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        n, = struct.unpack(">I", data[pos:pos + 4])
        t = data[pos + 4:pos + 8]
        if t == b"IHDR":
            w, h = struct.unpack(">II", data[pos + 8:pos + 16])
        elif t == b"IDAT":
            idat += data[pos + 8:pos + 8 + n]
        pos += 12 + n
    raw = zlib.decompress(idat)
    rows = [raw[y * (w * 4 + 1) + 1:(y + 1) * (w * 4 + 1)] for y in range(h)]
    return w, h, b"".join(rows)


# ------------------------------------------------------------ C header ----

def rle(rgba):
    out = bytearray()
    px = [rgba[i:i + 4] for i in range(0, len(rgba), 4)]
    px = [p if p[3] else b"\0\0\0\0" for p in px]
    i = 0
    while i < len(px):
        j = i + 1
        while j < len(px) and j - i < 255 and px[j] == px[i]:
            j += 1
        out.append(j - i)
        out += px[i]
        i = j
    return bytes(out)


def c_bytes(name, data, ctype="uint8_t"):
    lines = ["static const %s %s[%d] = {" % (ctype, name, len(data))]
    for i in range(0, len(data), 12):
        lines.append("\t" + " ".join("0x%02x," % b for b in data[i:i + 12]))
    lines.append("};")
    return "\n".join(lines)


def header_text(logos, spins):
    h = []
    h.append("""/*
 * NextBSD boot splash data (E19). GENERATED by
 * assets/bootsplash/gen-bootsplash.py from assets/bootsplash/nextbsd-cube.svg
 * and the script's own 12-spoke spinner; do not edit. The loader's
 * /boot/images/nextbsd-logo{,@2x}.png hold the same logo pixels.
 *
 * Logo: run-length coded 8-bit RGBA, not premultiplied, row-major: runs
 * of (count 1..255, R, G, B, A). Composite each pixel over the background
 * as (src * a + dst * (255 - a) + 127) / 255 per channel
 * (<sys/nbsd_boot_video.h>).
 *
 * Spinner: 8-bit alpha, 3 frames per scale, VC_SPIN_FRAMES steps per
 * revolution: step s is frame s % 3 turned s / 3 quarter turns clockwise,
 * dst(x, y) = src(y, d - 1 - x).
 */

#ifndef _DEV_IOKIT_BOOTSPLASH_DATA_H_
#define\t_DEV_IOKIT_BOOTSPLASH_DATA_H_

#define\tVC_SPIN_FRAMES\t12
#define\tVC_SPIN_BASE\t3
""")
    for s in (1, 2):
        w, hh = LOGO_SIZES[s]
        h.append("#define\tVC_LOGO_%dX_W\t%d" % (s, w))
        h.append("#define\tVC_LOGO_%dX_H\t%d" % (s, hh))
        h.append("#define\tVC_SPIN_%dX_D\t%d" % (s, SPIN_D[s]))
    h.append("")
    for s in (1, 2):
        h.append(c_bytes("vc_logo_%dx_rle" % s, rle(logos[s])))
        h.append("")
    for s in (1, 2):
        d = SPIN_D[s]
        h.append("static const uint8_t vc_spin_%dx[VC_SPIN_BASE][%d] = {"
                 % (s, d * d))
        for fr in spins[s]:
            h.append("\t{")
            for i in range(0, len(fr), 12):
                h.append("\t\t" + " ".join("0x%02x," % b
                                           for b in fr[i:i + 12]))
            h.append("\t},")
        h.append("};")
        h.append("")
    h.append("#endif /* !_DEV_IOKIT_BOOTSPLASH_DATA_H_ */")
    return "\n".join(h) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="verify the committed outputs instead of writing")
    args = ap.parse_args()

    vb, shapes = load_svg(SVG)
    logos = {s: render_svg(vb, shapes, *LOGO_SIZES[s]) for s in (1, 2)}
    spins = {}
    for s in (1, 2):
        d = SPIN_D[s]
        fr = [spinner_frame(f, s) for f in range(12)]
        for f in range(12):
            want = fr[f % 3]
            for _ in range(f // 3):
                want = rot90cw(want, d)
            # Exact up to the last bit of float rounding in the sampler.
            diff = max(abs(a - b) for a, b in zip(want, fr[f]))
            if diff > 1:
                sys.exit("spinner %dx step %d is not a rotation of step %d "
                         "(max diff %d)" % (s, f, f % 3, diff))
        spins[s] = fr[:3]

    outputs = {}
    for s, name in ((1, "nextbsd-logo.png"), (2, "nextbsd-logo@2x.png")):
        outputs[name] = (LOGO_SIZES[s], logos[s])
    for s, name in ((1, "nextbsd-spinner.png"), (2, "nextbsd-spinner@2x.png")):
        d = SPIN_D[s]
        a = spins[s][0]
        rgba = b"".join(bytes((255, 255, 255, v)) if v else b"\0\0\0\0"
                        for v in a)
        outputs[name] = ((d, d), rgba)
    hdr = header_text(logos, spins)

    if args.check:
        bad = []
        for name, ((w, h), rgba) in outputs.items():
            p = os.path.join(HERE, name)
            if not os.path.exists(p) or png_pixels(p) != (w, h, rgba):
                bad.append(name)
        if not os.path.exists(HEADER) or open(HEADER).read() != hdr:
            bad.append(os.path.relpath(HEADER, HERE))
        if bad:
            sys.exit("out of date: " + ", ".join(bad))
        print("bootsplash assets are current")
        return

    for name, ((w, h), rgba) in outputs.items():
        with open(os.path.join(HERE, name), "wb") as f:
            f.write(png_bytes(w, h, rgba))
    with open(HEADER, "w") as f:
        f.write(hdr)
    print("wrote %s and %s" % (", ".join(sorted(outputs)), HEADER))


if __name__ == "__main__":
    main()
