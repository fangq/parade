#!/usr/bin/env python3
"""Parade visual viewer: render layouts to SVG for inspection and regression review.

Positions come from build/libparade.so through the C ABI; glyph outlines come
from fontTools (Parade itself only reads metrics). Every glyph is drawn at the
exact sp position Parade computed, so what you see is the engine's output.

usage: render.py [-o out.svg] [--max-mem MB]

Scenes: greedy vs optimal justification, ragged alignments, wrap around a
figure, soft hyphens in a narrow column, mixed sizes with an inline object,
kerning on/off, CJK with kinsoku, and caret stops. Each line is annotated
with its adjustment ratio r (green r<=0.5, amber r<=1, red r>1, overfull or underfull).

The process address space is capped (default 2048 MB, env
PARADE_RENDER_MAX_MEM) so a large font cannot exhaust the machine.
"""
import ctypes, os, resource, sys
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))
SP = 65536.0

LATIN = os.environ.get("PARADE_TEST_FONT", "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf")
CJK = os.environ.get("PARADE_TEST_CJK_FONT", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc")

TEXT = ("In olden times when wishing still helped one, there lived a king whose daughters were all "
        "beautiful, but the youngest was so beautiful that the sun itself, which has seen so much, "
        "was astonished whenever it shone in her face. Close by the king's castle lay a great dark "
        "forest, and under an old lime-tree in the forest was a well, and when the day was very warm, "
        "the king's child went out into the forest and sat down by the side of the cool fountain; and "
        "when she was bored she took a golden ball, and threw it up on high and caught it; and this "
        "ball was her favorite plaything.")


# ---------------------------------------------------------------- C ABI ---

class Style(ctypes.Structure):
    _fields_ = [("font", ctypes.c_void_p), ("size", ctypes.c_int32), ("space_stretch", ctypes.c_int32),
                ("space_shrink", ctypes.c_int32), ("kerning", ctypes.c_int32), ("color", ctypes.c_uint32),
                ("user", ctypes.c_int32)]


class Params(ctypes.Structure):
    _fields_ = [(n, ctypes.c_int32) for n in (
        "mode", "align", "width", "indent", "line_penalty", "adj_demerits", "double_hyphen_demerits",
        "final_hyphen_demerits", "hyphen_penalty", "ex_hyphen_penalty", "tex_badness", "rag_stretch",
        "baseline_skip", "line_spacing")] + [("hysteresis", ctypes.c_int64), ("freeze_offset", ctypes.c_int32)]


class Info(ctypes.Structure):
    _fields_ = [("lines", ctypes.c_int32), ("demerits", ctypes.c_int64)] + \
        [(n, ctypes.c_int32) for n in ("overfull", "underfull", "reused_breakpoints", "frozen_lines", "height")]


class Line(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint32) for n in ("text_start", "text_end")] + \
        [(n, ctypes.c_int32) for n in ("x", "baseline", "width", "ascent", "descent", "ratio", "badness",
                                      "hyphenated", "overfull", "underfull")]


class Glyph(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint32) for n in ("glyph", "cluster")] + \
        [(n, ctypes.c_int32) for n in ("x", "y", "advance", "style", "kind", "user")]


lib = ctypes.CDLL(os.path.join(HERE, "..", "build", "libparade.so"))
VP = ctypes.c_void_p
for name, args, res in [
        ("pd_font_load_file", [ctypes.c_char_p, ctypes.c_int32, ctypes.POINTER(VP)], ctypes.c_int),
        ("pd_font_free", [VP], None),
        ("pd_style_init", [ctypes.POINTER(Style), VP, ctypes.c_int32], None),
        ("pd_params_init", [ctypes.POINTER(Params)], None),
        ("pd_para_new", [ctypes.POINTER(VP)], ctypes.c_int),
        ("pd_para_free", [VP], None),
        ("pd_para_add_text", [VP, ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(Style)], ctypes.c_int),
        ("pd_para_add_object", [VP, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32], ctypes.c_int),
        ("pd_para_set_shape", [VP, ctypes.c_int32, ctypes.POINTER(ctypes.c_int32), ctypes.POINTER(ctypes.c_int32)],
         ctypes.c_int),
        ("pd_para_break", [VP, ctypes.POINTER(Params), ctypes.POINTER(Info)], ctypes.c_int),
        ("pd_para_line_count", [VP], ctypes.c_int32),
        ("pd_para_get_line", [VP, ctypes.c_int32, ctypes.POINTER(Line)], ctypes.c_int),
        ("pd_para_get_glyphs", [VP, ctypes.c_int32, ctypes.POINTER(Glyph), ctypes.c_int32,
                                ctypes.POINTER(ctypes.c_int32)], ctypes.c_int),
        ("pd_para_get_style", [VP, ctypes.c_int32, ctypes.POINTER(Style)], ctypes.c_int),
        ("pd_para_caret", [VP, ctypes.c_uint32, ctypes.POINTER(ctypes.c_int32), ctypes.POINTER(ctypes.c_int32),
                           ctypes.POINTER(ctypes.c_int32)], ctypes.c_int),
        ("pd_para_text_length", [VP], ctypes.c_uint32)]:
    fn = getattr(lib, name)
    fn.argtypes, fn.restype = args, res


def pt(v):
    return int(v * SP)


# ------------------------------------------------------------- fonts ---

class Font:
    """a Parade font handle plus fontTools outlines for drawing"""

    def __init__(self, path):
        from fontTools.ttLib import TTFont
        self.handle = VP()
        if lib.pd_font_load_file(path.encode(), 0, ctypes.byref(self.handle)) != 0:
            raise RuntimeError(f"Parade cannot load {path}")
        self.tt = TTFont(path, fontNumber=0, lazy=True)
        self.upem = self.tt["head"].unitsPerEm
        self.order = self.tt.getGlyphOrder()
        self.glyphset = self.tt.getGlyphSet()
        self.paths = {}
        self.key = os.path.splitext(os.path.basename(path))[0].replace(" ", "_")

    def path_id(self, gid):
        """outline of a glyph as an SVG path in font units (y up), cached"""
        if gid not in self.paths:
            from fontTools.pens.svgPathPen import SVGPathPen
            pen = SVGPathPen(self.glyphset)
            self.glyphset[self.order[gid]].draw(pen)
            self.paths[gid] = pen.getCommands()
        return f"{self.key}_{gid}"


FONTS = {}      # Parade handle value -> Font


def load_font(path):
    f = Font(path)
    FONTS[f.handle.value] = f
    return f


# ------------------------------------------------------------ layout ---

class Para:
    def __init__(self):
        self.h = VP()
        lib.pd_para_new(ctypes.byref(self.h))

    def text(self, s, font, size_pt, kerning=1):
        st = Style()
        lib.pd_style_init(ctypes.byref(st), font.handle, pt(size_pt))
        st.kerning = kerning
        b = s.encode()
        lib.pd_para_add_text(self.h, b, len(b), ctypes.byref(st))
        return self

    def obj(self, w, h, d, user=1):
        lib.pd_para_add_object(self.h, pt(w), pt(h), pt(d), user)
        return self

    def shape(self, rows):
        n = len(rows)
        ind = (ctypes.c_int32 * n)(*[pt(r[0]) for r in rows])
        wid = (ctypes.c_int32 * n)(*[pt(r[1]) for r in rows])
        lib.pd_para_set_shape(self.h, n, ind, wid)
        return self

    def brk(self, **kw):
        prm = Params()
        lib.pd_params_init(ctypes.byref(prm))
        for k, v in kw.items():
            setattr(prm, k, pt(v) if k in ("width", "indent", "rag_stretch", "baseline_skip") else v)
        info = Info()
        lib.pd_para_break(self.h, ctypes.byref(prm), ctypes.byref(info))
        self.prm, self.info = prm, info
        return self

    def lines(self):
        out = []
        for i in range(lib.pd_para_line_count(self.h)):
            L = Line()
            lib.pd_para_get_line(self.h, i, ctypes.byref(L))
            n = ctypes.c_int32()
            lib.pd_para_get_glyphs(self.h, i, None, 0, ctypes.byref(n))
            buf = (Glyph * max(n.value, 1))()
            lib.pd_para_get_glyphs(self.h, i, buf, n.value, ctypes.byref(n))
            out.append((L, list(buf[:n.value])))
        return out

    def style(self, idx):
        st = Style()
        lib.pd_para_get_style(self.h, idx, ctypes.byref(st))
        return st

    def __del__(self):
        lib.pd_para_free(self.h)


# ------------------------------------------------------------ drawing ---

def ratio_color(L):
    r = L.ratio / 1000.0
    if L.overfull or L.underfull or r > 1.0:
        return "#d62728"
    if abs(r) > 0.5:
        return "#ff9f1c"
    return "#2ca02c"


class Panel:
    """one titled drawing; coordinates in pt, origin at the panel's content corner"""

    def __init__(self, title, note=""):
        self.title, self.note, self.items, self.w, self.h = title, note, [], 0.0, 0.0

    def extend(self, w, h):
        self.w, self.h = max(self.w, w), max(self.h, h)

    def para(self, p, ox, oy, width, label=None, guides=True, ratios=True, carets=False, figure=None):
        lines = p.lines()
        info = p.info
        height = info.height / SP
        if label:
            stats = f"{info.lines} lines, demerits {info.demerits:,}"
            self.items.append(f'<text x="{ox:.2f}" y="{oy - 16:.2f}" class="label">{escape(label)}</text>'
                              f'<text x="{ox:.2f}" y="{oy - 6:.2f}" class="dim">{stats}</text>')
        if guides:
            for gx in (ox, ox + width):
                self.items.append(f'<line x1="{gx:.2f}" y1="{oy - 2:.2f}" x2="{gx:.2f}" y2="{oy + height + 2:.2f}" '
                                  f'class="margin"/>')
        if figure:
            fx, fy, fw, fh = figure
            self.items.append(f'<rect x="{ox + fx:.2f}" y="{oy + fy:.2f}" width="{fw:.2f}" height="{fh:.2f}" '
                              f'class="figure"/><text x="{ox + fx + fw / 2:.2f}" y="{oy + fy + fh / 2:.2f}" '
                              f'class="figlabel">figure</text>')
        for L, glyphs in lines:
            base = oy + L.baseline / SP
            top, bot = base - L.ascent / SP, base + L.descent / SP
            lx, lw = ox + L.x / SP, L.width / SP
            self.items.append(f'<rect x="{lx:.2f}" y="{top:.2f}" width="{lw:.2f}" height="{bot - top:.2f}" '
                              f'fill="{ratio_color(L)}" class="lineband"/>')
            self.items.append(f'<line x1="{lx:.2f}" y1="{base:.2f}" x2="{lx + lw:.2f}" y2="{base:.2f}" class="base"/>')
            for g in glyphs:
                gx, gy = ox + g.x / SP, oy + g.y / SP
                if g.kind == 1:     # inline object
                    self.items.append(f'<rect x="{gx:.2f}" y="{gy - 15:.2f}" width="{g.advance / SP:.2f}" '
                                      f'height="20" class="object"/>')
                    continue
                st = p.style(g.style)
                f = FONTS[st.font]
                s = st.size / SP / f.upem
                self.items.append(f'<use xlink:href="#{f.path_id(g.glyph)}" transform="translate({gx:.3f},{gy:.3f}) '
                                  f'scale({s:.6f},{-s:.6f})"/>')
            if ratios:
                r = L.ratio / 1000.0
                tag = "overfull" if L.overfull else "underfull" if L.underfull else f"r={r:+.2f}"
                self.items.append(f'<text x="{ox + width + 6:.2f}" y="{base:.2f}" class="ratio" '
                                  f'fill="{ratio_color(L)}">{tag}{" -" if L.hyphenated else ""}</text>')
        if carets:
            n = lib.pd_para_text_length(p.h)
            for off in range(n + 1):
                li, x, b = ctypes.c_int32(), ctypes.c_int32(), ctypes.c_int32()
                if lib.pd_para_caret(p.h, off, ctypes.byref(li), ctypes.byref(x), ctypes.byref(b)) == 0:
                    cx, cb = ox + x.value / SP, oy + b.value / SP
                    self.items.append(f'<line x1="{cx:.2f}" y1="{cb + 1.5:.2f}" x2="{cx:.2f}" y2="{cb + 4:.2f}" '
                                      f'class="caret"/>')
        self.extend(ox + width + (48 if ratios else 4), oy + height + 4)
        return height


def page(panels):
    gap, margin, title_h = 26.0, 24.0, 30.0
    width = max(p.w for p in panels) + 2 * margin
    y, body = margin, []
    for p in panels:
        body.append(f'<text x="{margin:.2f}" y="{y + 12:.2f}" class="title">{escape(p.title)}</text>')
        if p.note:
            body.append(f'<text x="{margin:.2f}" y="{y + 24:.2f}" class="note">{escape(p.note)}</text>')
        body.append(f'<g transform="translate({margin:.2f},{y + title_h + 22:.2f})">' + "".join(p.items) + "</g>")
        y += title_h + 22 + p.h + gap
    defs = []
    for f in {id(f): f for f in FONTS.values()}.values():
        for gid, d in f.paths.items():
            defs.append(f'<path id="{f.key}_{gid}" d="{d}"/>')
    css = """
    .title{font:bold 13px sans-serif;fill:#111}.note{font:10px sans-serif;fill:#555}
    .label{font:bold 9px sans-serif;fill:#222}.dim{font:7px sans-serif;fill:#777}
    .ratio{font:7px monospace}.margin{stroke:#4a90d9;stroke-width:.4;stroke-dasharray:2 2}
    .lineband{opacity:.10}.base{stroke:#bbb;stroke-width:.2}.caret{stroke:#e0007a;stroke-width:.35}
    .figure{fill:#dfe9f5;stroke:#4a90d9;stroke-width:.6}.figlabel{font:10px sans-serif;fill:#4a90d9;text-anchor:middle}
    .object{fill:#fbe3c0;stroke:#d08a20;stroke-width:.6}
    """
    return (f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="{width:.0f}pt" height="{y + margin:.0f}pt" '
            f'viewBox="0 0 {width:.2f} {y + margin:.2f}"><style>{css}</style>'
            f'<rect width="100%" height="100%" fill="white"/><defs>{"".join(defs)}</defs>{"".join(body)}</svg>')


# ------------------------------------------------------------- scenes ---

def scenes():
    latin = load_font(LATIN)
    cjk = load_font(CJK) if os.path.exists(CJK) else None
    panels = []

    p = Panel("1. Greedy vs optimal (Knuth-Plass), justified, 200pt column",
              "Same text and font. Bands and labels show each line's stretch ratio r: green |r|<=0.5, amber <=1, red >1.")
    for i, mode in enumerate(("greedy", "optimal")):
        para = Para().text(TEXT, latin, 10).brk(width=200, mode=1 if mode == "greedy" else 0)
        p.para(para, i * 290, 14, 200, label=mode)
    panels.append(p)

    p = Panel("2. Ragged alignments (optimal breaking balances the rag)", "left, right, centre; 180pt")
    for i, (name, al) in enumerate((("left", 1), ("right", 2), ("center", 3))):
        para = Para().text(TEXT, latin, 10).brk(width=180, align=al)
        p.para(para, i * 250, 14, 180, label=name, ratios=False)
    panels.append(p)

    p = Panel("3. Wrap around a figure (parshape)",
              "first 5 lines indented 130pt next to a 120x55pt figure; parshape counts lines, so the caller sizes it")
    para = Para().text(TEXT, latin, 10).shape([(130, 200)] * 5 + [(0, 330)]).brk(width=330)
    p.para(para, 0, 14, 330, label="optimal", figure=(0, 0, 120, 55))
    panels.append(p)

    soft = "­".join(["su", "per", "cal", "i", "fra", "gi", "lis", "tic"]) + " " + \
           "­".join(["ex", "pi", "ali", "do", "cious"]) + " " + \
           "and " + "­".join(["in", "com", "pre", "hen", "si", "bil", "i", "ties"]) + " with " + \
           "­".join(["anti", "dis", "es", "tab", "lish", "ment", "ar", "i", "an", "ism"]) + " today."
    p = Panel("4. Soft hyphens in a narrow column", "hyphen glyph appears only where a line breaks; 70pt")
    for i, (name, mode) in enumerate((("greedy", 1), ("optimal", 0))):
        para = Para().text(soft, latin, 10).brk(width=70, mode=mode)
        p.para(para, i * 200, 14, 70, label=name)
    panels.append(p)

    p = Panel("5. Mixed sizes and an inline object", "line heights follow the tallest content; orange box = 40x20pt object")
    para = Para().text("Small 8pt text runs into ", latin, 8).text("LARGE 18pt words", latin, 18) \
        .text(" and back to normal 10pt text with an equation ", latin, 10).obj(40, 15, 5) \
        .text(" placed inline, followed by more ordinary words to fill a few lines of the paragraph.", latin, 10) \
        .brk(width=260)
    p.para(para, 0, 14, 260, label="optimal")
    panels.append(p)

    p = Panel("6. Kerning off vs on", "GPOS pair kerning from the font; 24pt")
    for i, k in enumerate((0, 1)):
        para = Para().text("AVATAR To Wa Ty LT P. y,", latin, 24, kerning=k).brk(width=330, align=1)
        p.para(para, 0, 18 + i * 44, 330, label=f"kerning {'on' if k else 'off'}", ratios=False, guides=False)
    panels.append(p)

    if cjk:
        han = "天地玄黄，宇宙洪荒。日月盈昃，辰宿列张。寒来暑往，秋收冬藏。闰余成岁，律吕调阳。云腾致雨，露结为霜。"
        p = Panel("7. CJK with kinsoku", "no line starts with ， or 。; 130pt, Noto Sans CJK")
        para = Para().text(han, cjk, 10).brk(width=130)
        p.para(para, 0, 14, 130, label="optimal")
        panels.append(p)

    p = Panel("8. Caret stops", "magenta ticks: pd_para_caret() for every byte offset")
    para = Para().text(TEXT[:240], latin, 12).brk(width=300)
    p.para(para, 0, 14, 300, label="optimal", carets=True)
    panels.append(p)
    return panels


def limit_memory(mb):
    limit = mb * 1024 * 1024
    soft, hard = resource.getrlimit(resource.RLIMIT_AS)
    if hard != resource.RLIM_INFINITY:
        limit = min(limit, hard)
    resource.setrlimit(resource.RLIMIT_AS, (limit, hard))


def main(argv):
    out, max_mem = os.path.join(HERE, "..", "build", "parade_view.svg"), int(os.environ.get("PARADE_RENDER_MAX_MEM", "2048"))
    while argv:
        if argv[0] == "-o":
            out, argv = argv[1], argv[2:]
        elif argv[0] == "--max-mem":
            max_mem, argv = int(argv[1]), argv[2:]
        else:
            sys.exit(__doc__)
    limit_memory(max_mem)
    svg = page(scenes())
    with open(out, "w", encoding="utf-8") as fp:
        fp.write(svg)
    print(f"wrote {out} ({len(svg) // 1024} KB)")


if __name__ == "__main__":
    main(sys.argv[1:])
