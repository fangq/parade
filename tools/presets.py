#!/usr/bin/env python3
"""Office's preset shapes (ECMA-376 presetShapeDefinitions.xml) as a compact table for src/pd_preset.c.

    python3 tools/presets.py presetShapeDefinitions.xml > src/pd_presets.inc

Each shape is one string of statements separated by ';':
    a NAME FMLA                  an adjustment and its default (avLst)
    g NAME FMLA                  a guide (gdLst)
    hx GD MIN MAX X Y            a handle moving GD across (ahXY gdRefX); hy the same up and down (gdRefY);
    hxy GX MINX MAXX GY MINY MAXY X Y   one moving both
    hp GR MINR MAXR GA MINA MAXA X Y    a polar one (ahPolar); '-' for what a handle does not give
    r L T R B                    where its text goes (rect)
    x ANG X Y                    a connection site (cxnLst), where a connector's end is put, and its direction
    p W H FILL STROKE            a path: its units (0: the shape's), fill (norm, none, darken, ...), outlined (1/0)
    m X Y | l X Y | q X1 Y1 X2 Y2 | c X1 Y1 X2 Y2 X3 Y3 | A WR HR ST SW | z
"""
import sys
import xml.etree.ElementTree as ET


def tag(el):
    return el.tag.split('}')[-1]


def shape(sh):
    out = []
    for part in sh:
        t = tag(part)
        if t == 'avLst':
            out += ['a %s %s' % (g.get('name'), g.get('fmla')) for g in part]
        elif t == 'gdLst':
            out += ['g %s %s' % (g.get('name'), g.get('fmla')) for g in part]
        elif t == 'ahLst':
            for h in part:
                pos = [p for p in h if tag(p) == 'pos'][0]
                xy = '%s %s' % (pos.get('x'), pos.get('y'))
                if tag(h) == 'ahXY':
                    gx, gy = h.get('gdRefX'), h.get('gdRefY')
                    if gx and gy:
                        out.append('hxy %s %s %s %s %s %s %s' % (gx, h.get('minX', '-'), h.get('maxX', '-'), gy,
                                                                h.get('minY', '-'), h.get('maxY', '-'), xy))
                    elif gx:
                        out.append('hx %s %s %s %s' % (gx, h.get('minX', '-'), h.get('maxX', '-'), xy))
                    elif gy:
                        out.append('hy %s %s %s %s' % (gy, h.get('minY', '-'), h.get('maxY', '-'), xy))
                else:
                    out.append('hp %s %s %s %s %s %s %s' % (h.get('gdRefR', '-'), h.get('minR', '-'),
                                                            h.get('maxR', '-'), h.get('gdRefAng', '-'),
                                                            h.get('minAng', '-'), h.get('maxAng', '-'), xy))
        elif t == 'cxnLst':
            for c in part:
                pos = [p for p in c if tag(p) == 'pos'][0]
                out.append('x %s %s %s' % (c.get('ang'), pos.get('x'), pos.get('y')))
        elif t == 'rect':
            out.append('r %s %s %s %s' % (part.get('l'), part.get('t'), part.get('r'), part.get('b')))
        elif t == 'pathLst':
            for p in part:
                stroke = '0' if p.get('stroke') in ('false', '0') else '1'
                out.append('p %s %s %s %s' % (p.get('w', '0'), p.get('h', '0'), p.get('fill', 'norm'), stroke))
                for c in p:
                    ct = tag(c)
                    pts = ' '.join('%s %s' % (q.get('x'), q.get('y')) for q in c if tag(q) == 'pt')
                    if ct == 'moveTo':
                        out.append('m ' + pts)
                    elif ct == 'lnTo':
                        out.append('l ' + pts)
                    elif ct == 'quadBezTo':
                        out.append('q ' + pts)
                    elif ct == 'cubicBezTo':
                        out.append('c ' + pts)
                    elif ct == 'arcTo':
                        out.append('A %s %s %s %s' % (c.get('wR'), c.get('hR'), c.get('stAng'), c.get('swAng')))
                    elif ct == 'close':
                        out.append('z')
    return ';'.join(out)


def main():
    root = ET.parse(sys.argv[1]).getroot()
    shapes = sorted((tag(sh), shape(sh)) for sh in root)
    print('/* Office\'s preset shapes, from ECMA-376 presetShapeDefinitions.xml by tools/presets.py: do not edit */')
    print('static const struct {\n    const char* name;\n    const char* def;\n} pd_presets[] = {')
    for n, d in shapes:
        print('    { "%s", "%s" },' % (n, d.replace('\\', '\\\\').replace('"', '\\"')))
    print('};')


if __name__ == '__main__':
    main()
