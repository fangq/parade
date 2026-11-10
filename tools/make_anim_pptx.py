#!/usr/bin/env python3
"""tests/data/anim.pptx: tests/data/slides.pptx with a transition and animations of each kind a slide show plays,
written as PowerPoint writes them (a main sequence of click groups, their effects' behaviours).

slide 1: click 1 the content's first paragraph appears; click 2 its second fades in (500 ms); click 3 the box flies
         in from the bottom (500 ms) while the oval fades out; click 4 the gradient box spins once (1000 ms)
slide 2: pushed in from the left (1000 ms); as it begins, the turned box wipes in from the bottom (500 ms); click 1
         a group of two boxes comes in by a wheel (1000 ms): its children drawn for it, the group the target
"""
import sys
import zipfile

ids = [2]


def n():
    ids[0] += 1
    return ids[0]


def tgt(spid, para=None):
    tx = '' if para is None else '<p:txEl><p:pRg st="%d" end="%d"/></p:txEl>' % (para, para)
    return '<p:tgtEl><p:spTgt spid="%d">%s</p:spTgt></p:tgtEl>' % (spid, tx)


def set_vis(t, vis, delay=0):
    return ('<p:set><p:cBhvr><p:cTn id="%d" dur="1" fill="hold"><p:stCondLst><p:cond delay="%d"/></p:stCondLst>'
            '</p:cTn>%s<p:attrNameLst><p:attrName>style.visibility</p:attrName></p:attrNameLst></p:cBhvr><p:to>'
            '<p:strVal val="%s"/></p:to></p:set>' % (n(), delay, t, vis))


def anim_effect(t, trans, filt, dur):
    return ('<p:animEffect transition="%s" filter="%s"><p:cBhvr><p:cTn id="%d" dur="%d"/>%s</p:cBhvr>'
            '</p:animEffect>' % (trans, filt, n(), dur, t))


def anim(t, attr, dur, v0, v1):
    return ('<p:anim calcmode="lin" valueType="num"><p:cBhvr additive="base"><p:cTn id="%d" dur="%d" fill="hold"/>%s'
            '<p:attrNameLst><p:attrName>%s</p:attrName></p:attrNameLst></p:cBhvr><p:tavLst><p:tav tm="0"><p:val>'
            '<p:strVal val="%s"/></p:val></p:tav><p:tav tm="100000"><p:val><p:strVal val="%s"/></p:val></p:tav>'
            '</p:tavLst></p:anim>' % (n(), dur, t, attr, v0, v1))


def anim_rot(t, by, dur):
    return ('<p:animRot by="%d"><p:cBhvr><p:cTn id="%d" dur="%d" fill="hold"/>%s<p:attrNameLst><p:attrName>r'
            '</p:attrName></p:attrNameLst></p:cBhvr></p:animRot>' % (by, n(), dur, t))


def effect(cls, preset, node, behs, delay=0):
    return ('<p:par><p:cTn id="%d" presetID="%d" presetClass="%s" presetSubtype="0" fill="hold" nodeType="%s">'
            '<p:stCondLst><p:cond delay="%d"/></p:stCondLst><p:childTnLst>%s</p:childTnLst></p:cTn></p:par>'
            % (n(), preset, cls, node, delay, ''.join(behs)))


def group(effects, auto=False):
    cond = '<p:cond delay="indefinite"/>' + ('<p:cond evt="onBegin" delay="0"><p:tn val="2"/></p:cond>' if auto else '')
    return ('<p:par><p:cTn id="%d" fill="hold"><p:stCondLst>%s</p:stCondLst><p:childTnLst><p:par><p:cTn id="%d" '
            'fill="hold"><p:stCondLst><p:cond delay="0"/></p:stCondLst><p:childTnLst>%s</p:childTnLst></p:cTn>'
            '</p:par></p:childTnLst></p:cTn></p:par>' % (n(), cond, n(), ''.join(effects)))


def timing(groups):
    return ('<p:timing><p:tnLst><p:par><p:cTn id="1" dur="indefinite" restart="never" nodeType="tmRoot">'
            '<p:childTnLst><p:seq concurrent="1" nextAc="seek"><p:cTn id="2" dur="indefinite" nodeType="mainSeq">'
            '<p:childTnLst>%s</p:childTnLst></p:cTn><p:prevCondLst><p:cond evt="onPrev" delay="0"><p:tgtEl>'
            '<p:sldTgt/></p:tgtEl></p:cond></p:prevCondLst><p:nextCondLst><p:cond evt="onNext" delay="0"><p:tgtEl>'
            '<p:sldTgt/></p:tgtEl></p:cond></p:nextCondLst></p:seq></p:childTnLst></p:cTn></p:par></p:tnLst>'
            '</p:timing>' % ''.join(groups))


def slide1():
    ids[0] = 2
    return timing([
        group([effect('entr', 1, 'clickEffect', [set_vis(tgt(3, 0), 'visible')])]),
        group([effect('entr', 10, 'clickEffect', [set_vis(tgt(3, 1), 'visible'),
                                                  anim_effect(tgt(3, 1), 'in', 'fade', 500)])]),
        group([effect('entr', 2, 'clickEffect', [set_vis(tgt(4), 'visible'),
                                                 anim(tgt(4), 'ppt_x', 500, '#ppt_x', '#ppt_x'),
                                                 anim(tgt(4), 'ppt_y', 500, '1+#ppt_h/2', '#ppt_y')]),
               effect('exit', 10, 'withEffect', [anim_effect(tgt(5), 'out', 'fade', 500),
                                                 set_vis(tgt(5), 'hidden', 499)])]),
        group([effect('emph', 8, 'clickEffect', [anim_rot(tgt(8), 21600000, 1000)])]),
    ])


def box(spid, x, y, clr):
    return ('<p:sp><p:nvSpPr><p:cNvPr id="%d" name="Box %d"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr><a:xfrm>'
            '<a:off x="%d" y="%d"/><a:ext cx="1000000" cy="1000000"/></a:xfrm><a:prstGeom prst="rect"><a:avLst/>'
            '</a:prstGeom><a:solidFill><a:srgbClr val="%s"/></a:solidFill></p:spPr></p:sp>' % (spid, spid, x, y, clr))


# slide 2's group: id 9, its boxes 10 and 11 side by side
GROUP2 = ('<p:grpSp><p:nvGrpSpPr><p:cNvPr id="9" name="Group 9"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr>'
          '<a:xfrm><a:off x="4500000" y="3000000"/><a:ext cx="2000000" cy="1000000"/><a:chOff x="4500000" '
          'y="3000000"/><a:chExt cx="2000000" cy="1000000"/></a:xfrm></p:grpSpPr>' + box(10, 4500000, 3000000, 'C00000')
          + box(11, 5500000, 3000000, '0000C0') + '</p:grpSp>')


def slide2():
    ids[0] = 2
    return '<p:transition spd="slow"><p:push dir="r"/></p:transition>' + timing([
        group([effect('entr', 22, 'afterEffect', [set_vis(tgt(4), 'visible'),
                                                  anim_effect(tgt(4), 'in', 'wipe(up)', 500)])], auto=True),
        group([effect('entr', 21, 'clickEffect', [set_vis(tgt(9), 'visible'),
                                                  anim_effect(tgt(9), 'in', 'wheel(1)', 1000)])]),
    ])


def main(src, dst):
    zin = zipfile.ZipFile(src)
    zout = zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED)
    for item in zin.infolist():
        data = zin.read(item.filename)
        if item.filename in ('ppt/slides/slide1.xml', 'ppt/slides/slide2.xml'):
            x = data.decode('utf-8')
            tail = slide1() if item.filename.endswith('slide1.xml') else slide2()
            assert x.count('</p:sld>') == 1
            x = x.replace('</p:sld>', tail + '</p:sld>')
            if item.filename.endswith('slide2.xml'):
                assert x.count('</p:spTree>') == 1
                x = x.replace('</p:spTree>', GROUP2 + '</p:spTree>')
            data = x.encode('utf-8')
        zout.writestr(item, data)
    zout.close()


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'tests/data/slides.pptx',
         sys.argv[2] if len(sys.argv) > 2 else 'tests/data/anim.pptx')
