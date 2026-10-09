#!/usr/bin/env python3
"""SmartArt laid out from its definition (src/pd_dgm.c) against the drawing PowerPoint saved of it: each shape of
each saved drawing in a presentation, where PowerPoint put it and where the layout here puts it (matched by the
presentation point it is of, its modelId), and its text's size.

    python3 tools/dgm_compare.py deck.pptx [--pd-conv build/pd_conv] [--only LAYOUT]

It runs pd_conv with PD_SMARTART=layout (the layout here, not the saved drawing) and PD_SMARTART_DUMP (the
drawings it made). Decks with slides copied from one another have diagrams whose shapes share modelIds: those
are compared with the last laid out.
"""
import argparse
import os
import re
import subprocess
import tempfile
import zipfile


def shapes(t):
    out = {}
    for m in re.finditer(r'<dsp:sp modelId="([^"]*)">(.*?)</dsp:sp>', t, re.S):
        b = m.group(2)
        off = re.search(r'<a:off x="(-?\d+)" y="(-?\d+)"', b)
        ext = re.search(r'<a:ext cx="(\d+)" cy="(\d+)"', b)
        geom = re.search(r'<a:prstGeom prst="(\w+)"', b)
        sz = re.search(r' sz="(\d+)"', b)
        if off and ext:
            out[m.group(1)] = (int(off.group(1)), int(off.group(2)), int(ext.group(1)), int(ext.group(2)),
                               geom.group(1) if geom else 'custom', int(sz.group(1)) // 100 if sz else 0,
                               ''.join(re.findall(r'<a:t>([^<]*)', b))[:24])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pptx')
    ap.add_argument('--pd-conv', default=os.path.join(os.path.dirname(__file__), '..', 'build', 'pd_conv'))
    ap.add_argument('--only', help='only diagrams whose layout id has this in it')
    a = ap.parse_args()
    z = zipfile.ZipFile(a.pptx)
    names, draws, layouts = {}, {}, {}
    for n in z.namelist():
        s = z.read(n).decode('utf8', 'replace')
        if re.match(r'ppt/diagrams/data\d+\.xml$', n):
            for m in re.finditer(r'<dgm:pt modelId="([^"]*)"[^>]*?(?:/>|>(.*?)</dgm:pt>)', s, re.S):
                p = re.search(r'presName="([^"]*)"', m.group(2) or '')
                if p:
                    names[m.group(1)] = p.group(1)
        elif re.match(r'ppt/diagrams/drawing\d+\.xml$', n):
            draws[n] = shapes(s)
        elif re.match(r'ppt/diagrams/layout\d+\.xml$', n):
            u = re.search(r'uniqueId="([^"]*)"', s)
            layouts[n.replace('layout', 'drawing')] = (u.group(1) if u else '?').split('/')[-1]
    fd, dump = tempfile.mkstemp(suffix='.xml')
    os.close(fd)
    pdf = dump + '.pdf'
    subprocess.run([a.pd_conv, a.pptx, pdf], env=dict(os.environ, PD_SMARTART='layout', PD_SMARTART_DUMP=dump),
                   capture_output=True)
    ours = {}
    for line in open(dump, encoding='utf8'):
        ours.update(shapes(line))
    for f in (dump, pdf):
        if os.path.exists(f):
            os.unlink(f)
    for dn in sorted(draws):
        lay = layouts.get(dn, '?')
        if a.only and a.only not in lay:
            continue
        print('==', dn, lay)
        print('  %-18s %38s %5s | %38s %5s' % ('', 'PowerPoint: x y w h', 'pt', 'here: x y w h', 'pt'))
        for mid, s in draws[dn].items():
            o = ours.get(mid)
            here = '%9d %9d %9d %9d %5d' % (o[0], o[1], o[2], o[3], o[5]) if o else '%38s %5s' % ('-', '')
            print('  %-18s %9d %9d %9d %9d %5d | %s  %s' % (names.get(mid, '?')[:18], s[0], s[1], s[2], s[3], s[5],
                                                         here, s[6]))


main()
