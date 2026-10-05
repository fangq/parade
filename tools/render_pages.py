#!/usr/bin/env python3
"""Draw pd_dump output: every page as Parade laid it out.

usage: render_pages.py [--max-mem MB] pages.json out.svg [--guides]

Glyphs are drawn from the font outlines (fontTools) at the exact positions
in the display list; images are shown as labelled placeholders, object
boxes as dashed frames and rules as filled rectangles. Header, footer and
float regions are tinted so placement can be checked at a glance.
"""
import json, os, resource, sys
from xml.sax.saxutils import escape

SP = 65536.0
REGION_TINT = {1: "#e8f0fb", 2: "#e8f0fb", 3: "#fdf1dc"}


def limit_memory(mb):
    soft, hard = resource.getrlimit(resource.RLIMIT_AS)
    limit = mb * 1024 * 1024
    if hard != resource.RLIM_INFINITY:
        limit = min(limit, hard)
    resource.setrlimit(resource.RLIMIT_AS, (limit, hard))


def main(argv):
    max_mem = 2048
    if len(argv) >= 2 and argv[0] == "--max-mem":
        max_mem, argv = int(argv[1]), argv[2:]
    if len(argv) < 2:
        sys.exit(__doc__)
    limit_memory(max_mem)
    from fontTools.ttLib import TTFont
    from fontTools.pens.svgPathPen import SVGPathPen

    data = json.load(open(argv[0]))
    fonts = []
    for path in data["fonts"]:
        tt = TTFont(path, lazy=True)
        fonts.append({"tt": tt, "gs": tt.getGlyphSet(), "order": tt.getGlyphOrder(),
                      "upem": tt["head"].unitsPerEm, "paths": {}})

    def path_id(fi, gid):
        f = fonts[fi]
        if gid not in f["paths"]:
            pen = SVGPathPen(f["gs"])
            f["gs"][f["order"][gid]].draw(pen)
            f["paths"][gid] = pen.getCommands()
        return "f%d_%d" % (fi, gid)

    gap, margin = 24.0, 20.0
    pages = data["pages"]
    pw = max(p["w"] for p in pages) / SP
    ph = max(p["h"] for p in pages) / SP
    per_row = min(4, len(pages))
    rows = (len(pages) + per_row - 1) // per_row
    W = margin * 2 + per_row * pw + (per_row - 1) * gap
    H = margin * 2 + rows * (ph + 22) + (rows - 1) * gap
    body = []
    for i, p in enumerate(pages):
        ox = margin + (i % per_row) * (pw + gap)
        oy = margin + (i // per_row) * (ph + 22 + gap) + 16
        body.append('<g transform="translate(%.2f,%.2f)">' % (ox, oy))
        body.append('<rect width="%.2f" height="%.2f" fill="white" stroke="#999" stroke-width="0.6"/>' %
                    (p["w"] / SP, p["h"] / SP))
        tag = "page %s%s" % (p["label"], " (float page)" if p["float_page"] else "")
        body.append('<text x="0" y="-5" class="tag">%s</text>' % escape(tag))
        # region tints first, as one box per region around its items
        boxes = {}
        for it in p["items"]:
            kind, x, y, w, h, gid, fi, size, color, region = it
            if region in REGION_TINT:
                top = y - (size * 0.8 if kind == 0 else 0)
                bot = y + (size * 0.25 if kind == 0 else h)
                b = boxes.setdefault(region, [x, top, x + w, bot])
                b[0], b[1], b[2], b[3] = min(b[0], x), min(b[1], top), max(b[2], x + w), max(b[3], bot)
        for region, (x0, y0, x1, y1) in sorted(boxes.items()):
            body.append('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" fill="%s"/>' %
                        (x0 / SP - 2, y0 / SP - 2, (x1 - x0) / SP + 4, (y1 - y0) / SP + 4, REGION_TINT[region]))
        for it in p["items"]:
            kind, x, y, w, h, gid, fi, size, color, region = it
            rgb = "#%06x" % (color & 0xFFFFFF)
            if kind == 0 and fi >= 0:
                s = size / SP / fonts[fi]["upem"]
                body.append('<use xlink:href="#%s" fill="%s" transform="translate(%.3f,%.3f) scale(%.6f,%.6f)"/>' %
                            (path_id(fi, gid), rgb, x / SP, y / SP, s, -s))
            elif kind == 1:
                body.append('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" class="img"/>'
                            '<text x="%.2f" y="%.2f" class="imgtag">image</text>' %
                            (x / SP, y / SP, w / SP, h / SP, (x + w / 2) / SP, (y + h / 2) / SP))
            elif kind == 2:
                body.append('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" class="box"/>' %
                            (x / SP, y / SP, w / SP, h / SP))
            elif kind == 3:
                body.append('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" fill="%s"/>' %
                            (x / SP, y / SP, w / SP, h / SP, rgb))
        body.append("</g>")
    defs = []
    for fi, f in enumerate(fonts):
        for gid, d in f["paths"].items():
            defs.append('<path id="f%d_%d" d="%s"/>' % (fi, gid, d))
    css = (".tag{font:9px sans-serif;fill:#555}.img{fill:#dfe9f5;stroke:#4a90d9;stroke-width:.6}"
           ".imgtag{font:10px sans-serif;fill:#4a90d9;text-anchor:middle}"
           ".box{fill:none;stroke:#d08a20;stroke-width:.5;stroke-dasharray:2 1}")
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
           'width="%.0fpt" height="%.0fpt" viewBox="0 0 %.2f %.2f"><style>%s</style>'
           '<rect width="100%%" height="100%%" fill="#eee"/><defs>%s</defs>%s</svg>' %
           (W, H, W, H, css, "".join(defs), "".join(body)))
    open(argv[1], "w").write(svg)
    print("wrote %s (%d pages, %d KB)" % (argv[1], len(pages), len(svg) // 1024))


if __name__ == "__main__":
    main(sys.argv[1:])
