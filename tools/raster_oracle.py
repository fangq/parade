#!/usr/bin/env python3
"""Check Parade's outline decoder and rasterizer against fontTools.

usage: raster_oracle.py [--max-mem MB] font [font...]
For a sample of glyphs per font:
  outline: signed area of Parade's outline == fontTools AreaPen area (font units)
  raster:  sum of coverage / 255 == |area| at the rendered size (pixels^2)
"""
import ctypes, os, resource, sys

HERE = os.path.dirname(os.path.abspath(__file__))
lib = ctypes.CDLL(os.path.join(HERE, "..", "build", "libparade.so"))
VP = ctypes.c_void_p
I32 = ctypes.c_int32
MOVE = ctypes.CFUNCTYPE(None, VP, I32, I32)
LINE = MOVE
QUAD = ctypes.CFUNCTYPE(None, VP, I32, I32, I32, I32)
CUBIC = ctypes.CFUNCTYPE(None, VP, I32, I32, I32, I32, I32, I32)
CLOSE = ctypes.CFUNCTYPE(None, VP)


class Sink(ctypes.Structure):
    _fields_ = [("move_to", MOVE), ("line_to", LINE), ("quad_to", QUAD), ("cubic_to", CUBIC), ("close", CLOSE)]


class Image(ctypes.Structure):
    _fields_ = [("width", I32), ("height", I32), ("left", I32), ("top", I32)]


lib.pd_font_load_file.argtypes = [ctypes.c_char_p, I32, ctypes.POINTER(VP)]
lib.pd_font_glyph_outline.argtypes = [VP, ctypes.c_uint32, ctypes.POINTER(Sink), VP]
lib.pd_font_glyph_render.argtypes = [VP, ctypes.c_uint32, I32, I32, ctypes.c_void_p, I32, ctypes.POINTER(Image)]
lib.pd_font_free.argtypes = [VP]


def parade_area(font, gid):
    """signed area of the outline, by the shoelace formula on the flattened-free exact curve areas"""
    st = {"area": 0.0, "pt": (0, 0), "start": (0, 0)}

    def seg_line(p0, p1):
        return (p0[0] * p1[1] - p1[0] * p0[1]) / 2.0

    def seg_quad(p0, c, p1):   # exact area contribution of a quadratic Bezier
        return (p0[0] * p1[1] - p1[0] * p0[1]) / 2.0 + (
            (p0[0] * c[1] - c[0] * p0[1]) + (c[0] * p1[1] - p1[0] * c[1]) - (p0[0] * p1[1] - p1[0] * p0[1])) / 3.0

    def seg_cubic(p0, c1, c2, p3):   # fine flattening: accurate to ~1e-9 relative
        total, prev, n = 0.0, p0, 400
        for i in range(1, n + 1):
            t = i / n
            u = 1 - t
            q = (u ** 3 * p0[0] + 3 * u * u * t * c1[0] + 3 * u * t * t * c2[0] + t ** 3 * p3[0],
                 u ** 3 * p0[1] + 3 * u * u * t * c1[1] + 3 * u * t * t * c2[1] + t ** 3 * p3[1])
            total += seg_line(prev, q)
            prev = q
        return total

    def mv(u, x, y):
        close(u)
        st["pt"] = st["start"] = (x / 64.0, y / 64.0)

    def ln(u, x, y):
        p = (x / 64.0, y / 64.0)
        st["area"] += seg_line(st["pt"], p)
        st["pt"] = p

    def qd(u, cx, cy, x, y):
        p = (x / 64.0, y / 64.0)
        st["area"] += seg_quad(st["pt"], (cx / 64.0, cy / 64.0), p)
        st["pt"] = p

    def cu(u, ax, ay, bx, by, x, y):
        p = (x / 64.0, y / 64.0)
        st["area"] += seg_cubic(st["pt"], (ax / 64.0, ay / 64.0), (bx / 64.0, by / 64.0), p)
        st["pt"] = p

    def close(u):
        if st["pt"] != st["start"]:
            st["area"] += seg_line(st["pt"], st["start"])
            st["pt"] = st["start"]

    sink = Sink(MOVE(mv), LINE(ln), QUAD(qd), CUBIC(cu), CLOSE(close))
    rc = lib.pd_font_glyph_outline(font, gid, ctypes.byref(sink), None)
    return rc, st["area"]


def check(path, max_glyphs=400):
    from fontTools.ttLib import TTFont
    from fontTools.pens.areaPen import AreaPen
    tt = TTFont(path, fontNumber=0, lazy=True)
    gs = tt.getGlyphSet()
    order = tt.getGlyphOrder()
    upem = tt["head"].unitsPerEm
    h = VP()
    assert lib.pd_font_load_file(path.encode(), 0, ctypes.byref(h)) == 0
    step = max(1, len(order) // max_glyphs)
    worst_outline = worst_raster = 0.0
    n = bad = 0
    px = 64.0    # pixels per em for the raster check
    for gid in range(0, len(order), step):
        pen = AreaPen(gs)
        gs[order[gid]].draw(pen)
        ref = pen.value
        rc, got = parade_area(h, gid)
        if rc != 0:
            bad += 1
            continue
        n += 1
        rel = abs(abs(got) - abs(ref)) / max(abs(ref), 1.0)
        worst_outline = max(worst_outline, rel)
        if abs(ref) > upem * upem * 0.01:   # glyphs with real ink
            img = Image()
            lib.pd_font_glyph_render(h, gid, int(px * 65536), 0, None, 0, ctypes.byref(img))
            buf = (ctypes.c_uint8 * max(1, img.width * img.height))()
            lib.pd_font_glyph_render(h, gid, int(px * 65536), 0, buf, len(buf), ctypes.byref(img))
            ink = sum(buf) / 255.0
            want = abs(ref) * (px / upem) ** 2
            worst_raster = max(worst_raster, abs(ink - want) / want)
    lib.pd_font_free(h)
    print(f"{os.path.basename(path)}: {n} glyphs, {bad} errors, worst outline area error {worst_outline:.2e}, "
          f"worst raster coverage error {worst_raster:.2%}")
    return bad == 0 and worst_outline < 1e-3 and worst_raster < 0.03


if __name__ == "__main__":
    args = sys.argv[1:]
    mb = 2048
    if args[:1] == ["--max-mem"]:
        mb, args = int(args[1]), args[2:]
    resource.setrlimit(resource.RLIMIT_AS, (mb << 20, resource.getrlimit(resource.RLIMIT_AS)[1]))
    sys.exit(0 if all([check(p) for p in args]) else 1)
