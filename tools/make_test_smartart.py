#!/usr/bin/env python3
"""tests/data/smartart.pptx: SmartArt with no drawing saved of it, for the layout of diagrams from their definitions
(src/pd_dgm.c) -- slide 1 a block list of five (snake: three in a row, two under them, centred), slide 2 a process
of three with arrows between them (lin, conn), slide 3 a hierarchy of a root and three children, one of them with
two of its own, joined by bent lines (hierRoot, hierChild, a forEach by ref); the layout definitions are small ones
written for these, in the schema PowerPoint's are in, with a colour definition and a quick style. Slide 4 has a
drawing saved of it, older than its text: the text the data model's, at the size the drawing's runs are, in the
colour the colour definition gives text (not the drawing's fontRef).

Slides 5 to 8 are a cycle of four with arrows round it (cycle), a radial of a hub and five round it joined by lines
(cycle, ctrShpMap="fNode"), a pyramid of three and an inverted pyramid of two (pyra); slide 9 an organization
chart whose branches hang: both ways, to the right, and (as at first) the last level under the top's children;
slide 10 a list of two with picture placeholders, a picture (tests/data/rgba.png) in the first; slide 11 a process
with curved arrows; slide 12 labels turned a quarter, their text reading up; slide 13 a process in a quick style
with depth (the theme's gradient and shadow, a bevel). The .docx has the pictures' list too. tests/data/smartart.docx has
a cycle with no drawing and the diagram with the older drawing, inline in a Word document's paragraphs.

    python3 tools/make_test_smartart.py tests/data/smartart.pptx [tests/data/smartart.docx]
"""
import sys
import zipfile

A = 'xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" ' \
    'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" ' \
    'xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main"'
DGM = 'xmlns:dgm="http://schemas.openxmlformats.org/drawingml/2006/diagram" ' \
      'xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" ' \
      'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"'
REL = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships/'


def rels(*items):
    return '<?xml version="1.0" encoding="UTF-8"?><Relationships ' \
        'xmlns="http://schemas.openxmlformats.org/package/2006/relationships">' + \
        ''.join('<Relationship Id="%s" Type="%s%s" Target="%s"/>' % (i, REL, t, g) for i, t, g in items) + \
        '</Relationships>'


MARGINS = ''.join('<dgm:constr type="%sMarg" refType="primFontSz" fact="0.3"/>' % m for m in 'tblr')
SHRINK = '<dgm:ruleLst><dgm:rule type="primFontSz" val="5" fact="NaN" max="NaN"/></dgm:ruleLst>'

# a block list: the nodes in rows, as many to a row as makes them biggest, the last row centred
snake = ('<dgm:layoutDef %s uniqueId="test/blocks"><dgm:layoutNode name="diagram">'
         '<dgm:varLst><dgm:dir/></dgm:varLst><dgm:alg type="snake"><dgm:param type="grDir" val="tL"/>'
         '<dgm:param type="flowDir" val="row"/><dgm:param type="off" val="ctr"/></dgm:alg><dgm:shape/><dgm:presOf/>'
         '<dgm:constrLst><dgm:constr type="w" for="ch" forName="node" refType="w"/>'
         '<dgm:constr type="h" for="ch" forName="node" refType="w" refFor="ch" refForName="node" fact="0.6"/>'
         '<dgm:constr type="w" for="ch" forName="gap" refType="w" refFor="ch" refForName="node" fact="0.1"/>'
         '<dgm:constr type="sp" refType="w" refFor="ch" refForName="gap"/>'
         '<dgm:constr type="primFontSz" for="ch" forName="node" op="equ" val="65"/></dgm:constrLst>'
         '<dgm:forEach name="nodes" axis="ch" ptType="node"><dgm:layoutNode name="node"><dgm:alg type="tx"/>'
         '<dgm:shape type="rect"/><dgm:presOf axis="desOrSelf" ptType="node"/><dgm:constrLst>' + MARGINS +
         '</dgm:constrLst>' + SHRINK + '</dgm:layoutNode><dgm:forEach name="gaps" axis="followSib" '
         'ptType="sibTrans" cnt="1"><dgm:layoutNode name="gap"><dgm:alg type="sp"/><dgm:shape/><dgm:presOf/>'
         '</dgm:layoutNode></dgm:forEach></dgm:forEach></dgm:layoutNode></dgm:layoutDef>') % DGM

# a process: the nodes in a row, arrows in the gaps between them
process = ('<dgm:layoutDef %s uniqueId="test/process"><dgm:layoutNode name="diagram"><dgm:alg type="lin"/>'
           '<dgm:shape/><dgm:presOf/><dgm:constrLst>'
           '<dgm:constr type="w" for="ch" ptType="node" refType="w"/>'
           '<dgm:constr type="h" for="ch" ptType="node" op="equ"/>'
           '<dgm:constr type="primFontSz" for="ch" ptType="node" op="equ" val="65"/>'
           '<dgm:constr type="w" for="ch" ptType="sibTrans" refType="w" refFor="ch" refPtType="node" fact="0.4"/>'
           '</dgm:constrLst><dgm:forEach name="nodes" axis="ch" ptType="node"><dgm:layoutNode name="node">'
           '<dgm:alg type="tx"/><dgm:shape type="roundRect"><dgm:adjLst><dgm:adj idx="1" val="0.1"/></dgm:adjLst>'
           '</dgm:shape><dgm:presOf axis="desOrSelf" ptType="node"/><dgm:constrLst>'
           '<dgm:constr type="h" refType="w" fact="0.6"/>' + MARGINS + '</dgm:constrLst>' + SHRINK +
           '</dgm:layoutNode><dgm:forEach name="arrows" axis="followSib" ptType="sibTrans" cnt="1">'
           '<dgm:layoutNode name="arrow"><dgm:alg type="conn"/><dgm:shape type="conn"/><dgm:presOf axis="self"/>'
           '<dgm:constrLst><dgm:constr type="h" refType="w" fact="0.62"/><dgm:constr type="connDist"/>'
           '<dgm:constr type="begPad" refType="connDist" fact="0.25"/>'
           '<dgm:constr type="endPad" refType="connDist" fact="0.22"/></dgm:constrLst></dgm:layoutNode>'
           '</dgm:forEach></dgm:forEach></dgm:layoutNode></dgm:layoutDef>') % DGM

# a hierarchy: each node's box over its children's subtrees, lines from it bent down to theirs
hier = ('<dgm:layoutDef %s uniqueId="test/hierarchy"><dgm:layoutNode name="diagram"><dgm:alg type="hierChild"/>'
        '<dgm:shape/><dgm:presOf/><dgm:constrLst>'
        '<dgm:constr type="w" for="des" forName="box" refType="w"/>'
        '<dgm:constr type="h" for="des" forName="box" refType="w" refFor="des" refForName="box" fact="0.5"/>'
        '<dgm:constr type="primFontSz" for="des" forName="box" op="equ" val="65"/>'
        '<dgm:constr type="sibSp" refType="w" refFor="des" refForName="box" fact="0.2"/>'
        '<dgm:constr type="sibSp" for="des" forName="kids" refType="w" refFor="des" refForName="box" fact="0.2"/>'
        '<dgm:constr type="sp" for="des" forName="root" refType="w" refFor="des" refForName="box" fact="0.25"/>'
        '</dgm:constrLst><dgm:forEach name="each" axis="ch" ptType="node">'
        '<dgm:forEach name="lines" axis="precedSib" ptType="parTrans" st="-1" cnt="1"><dgm:layoutNode name="line">'
        '<dgm:alg type="conn"><dgm:param type="dim" val="1D"/><dgm:param type="endSty" val="noArr"/>'
        '<dgm:param type="connRout" val="bend"/><dgm:param type="begPts" val="bCtr"/>'
        '<dgm:param type="endPts" val="tCtr"/></dgm:alg><dgm:shape type="conn" zOrderOff="-99999"/>'
        '<dgm:presOf axis="self"/></dgm:layoutNode></dgm:forEach>'
        '<dgm:layoutNode name="root"><dgm:alg type="hierRoot"/><dgm:shape/><dgm:presOf/>'
        '<dgm:layoutNode name="box"><dgm:alg type="tx"/><dgm:shape type="rect"/><dgm:presOf axis="self"/>'
        '<dgm:constrLst>' + MARGINS + '</dgm:constrLst>' + SHRINK + '</dgm:layoutNode>'
        '<dgm:layoutNode name="kids"><dgm:alg type="hierChild"/><dgm:shape/><dgm:presOf/>'
        '<dgm:forEach name="more" ref="each"/></dgm:layoutNode></dgm:layoutNode></dgm:forEach></dgm:layoutNode>'
        '</dgm:layoutDef>') % DGM

# a cycle: the nodes round a circle, as far apart as their spacing asks, arrows round it between them (the last's
# back to the first)
cycle = ('<dgm:layoutDef %s uniqueId="test/cycle"><dgm:layoutNode name="diagram"><dgm:alg type="cycle">'
         '<dgm:param type="stAng" val="0"/><dgm:param type="spanAng" val="360"/></dgm:alg><dgm:shape/><dgm:presOf/>'
         '<dgm:constrLst><dgm:constr type="w" for="ch" ptType="node" refType="w"/>'
         '<dgm:constr type="h" for="ch" ptType="node" refType="w" refFor="ch" refPtType="node"/>'
         '<dgm:constr type="sibSp" refType="w" refFor="ch" refPtType="node" fact="0.4"/>'
         '<dgm:constr type="h" for="ch" ptType="sibTrans" refType="w" refFor="ch" refPtType="node" fact="0.15"/>'
         '<dgm:constr type="primFontSz" for="ch" ptType="node" op="equ" val="65"/></dgm:constrLst>'
         '<dgm:forEach name="nodes" axis="ch" ptType="node"><dgm:layoutNode name="node"><dgm:alg type="tx"/>'
         '<dgm:shape type="ellipse"/><dgm:presOf axis="desOrSelf" ptType="node"/><dgm:constrLst>' + MARGINS +
         '</dgm:constrLst>' + SHRINK + '</dgm:layoutNode><dgm:forEach name="arrows" axis="followSib" '
         'ptType="sibTrans" hideLastTrans="0" cnt="1"><dgm:layoutNode name="arrow"><dgm:alg type="conn"/>'
         '<dgm:shape type="conn"/><dgm:presOf axis="self"/><dgm:constrLst><dgm:constr type="connDist"/>'
         '<dgm:constr type="begPad" refType="connDist" fact="0.1"/><dgm:constr type="endPad" refType="connDist" '
         'fact="0.1"/></dgm:constrLst></dgm:layoutNode></dgm:forEach></dgm:forEach></dgm:layoutNode>'
         '</dgm:layoutDef>') % DGM

# a radial: the first node in the middle, the others round it, sp from it, lines out to them
radial = ('<dgm:layoutDef %s uniqueId="test/radial"><dgm:layoutNode name="diagram"><dgm:alg type="cycle">'
          '<dgm:param type="stAng" val="0"/><dgm:param type="spanAng" val="360"/>'
          '<dgm:param type="ctrShpMap" val="fNode"/></dgm:alg><dgm:shape/><dgm:presOf/><dgm:constrLst>'
          '<dgm:constr type="w" for="ch" forName="centre" refType="w"/>'
          '<dgm:constr type="w" for="ch" forName="node" refType="w" refFor="ch" refForName="centre" fact="0.6"/>'
          '<dgm:constr type="sp" refType="w" refFor="ch" refForName="centre" fact="0.25"/>'
          '<dgm:constr type="sibSp" refType="w" refFor="ch" refForName="node" fact="0.1"/>'
          '<dgm:constr type="primFontSz" for="ch" forName="node" op="equ" val="65"/></dgm:constrLst>'
          '<dgm:forEach name="first" axis="ch" ptType="node" cnt="1"><dgm:layoutNode name="centre">'
          '<dgm:alg type="tx"/><dgm:shape type="ellipse"/><dgm:presOf axis="self"/><dgm:constrLst>'
          '<dgm:constr type="h" refType="w"/><dgm:constr type="primFontSz" val="65"/>' + MARGINS +
          '</dgm:constrLst>' + SHRINK + '</dgm:layoutNode><dgm:forEach name="kids" axis="ch">'
          '<dgm:forEach name="lines" axis="self" ptType="parTrans"><dgm:layoutNode name="line">'
          '<dgm:alg type="conn"><dgm:param type="dim" val="1D"/><dgm:param type="endSty" val="noArr"/></dgm:alg>'
          '<dgm:shape type="conn" zOrderOff="-99"/><dgm:presOf axis="self"/></dgm:layoutNode></dgm:forEach>'
          '<dgm:forEach name="each" axis="self" ptType="node"><dgm:layoutNode name="node"><dgm:alg type="tx"/>'
          '<dgm:shape type="ellipse"/><dgm:presOf axis="desOrSelf" ptType="node"/><dgm:constrLst>'
          '<dgm:constr type="h" refType="w"/>' + MARGINS + '</dgm:constrLst>' + SHRINK + '</dgm:layoutNode>'
          '</dgm:forEach></dgm:forEach></dgm:forEach></dgm:layoutNode></dgm:layoutDef>') % DGM


def branch(cases, other):
    """a choose on the hierBranch variable: [(value, inside)], else OTHER"""
    return '<dgm:choose name="b">' + ''.join('<dgm:if name="b%s" func="var" arg="hierBranch" op="equ" val="%s">%s'
                                             '</dgm:if>' % (v, v, x) for v, x in cases) + \
        '<dgm:else name="bx">%s</dgm:else></dgm:choose>' % other


BENT = ('<dgm:param type="connRout" val="bend"/><dgm:param type="dim" val="1D"/><dgm:param type="endSty" '
        'val="noArr"/><dgm:param type="begPts" val="bCtr"/>')
STD_LINE = '<dgm:alg type="conn">' + BENT + '<dgm:param type="endPts" val="tCtr"/></dgm:alg>'
HANG_LINE = '<dgm:alg type="conn">' + BENT + '<dgm:param type="endPts" val="midL midR"/>%s</dgm:alg>'

# an organization chart, its branches standard, hanging both ways (hang), to the right (r), or as at first (init:
# standard, a last level under the top's children hanging to the right)
org = ('<dgm:layoutDef %s uniqueId="test/org"><dgm:layoutNode name="diagram"><dgm:alg type="hierChild"/>'
       '<dgm:shape/><dgm:presOf/><dgm:constrLst>'
       '<dgm:constr type="w" for="des" forName="comp" refType="w"/>'
       '<dgm:constr type="h" for="des" forName="comp" refType="w" refFor="des" refForName="comp" fact="0.5"/>'
       '<dgm:constr type="primFontSz" for="des" ptType="node" op="equ" val="65"/>'
       '<dgm:constr type="sp" for="des" forName="root" refType="w" refFor="des" refForName="comp" fact="0.2"/>'
       '<dgm:constr type="sibSp" refType="w" refFor="des" refForName="comp" fact="0.2"/>'
       '<dgm:constr type="sibSp" for="des" forName="kids" refType="sibSp"/>'
       '<dgm:constr type="secSibSp" refType="w" refFor="des" refForName="comp" fact="0.2"/>'
       '<dgm:constr type="secSibSp" for="des" forName="kids" refType="secSibSp"/></dgm:constrLst>'
       '<dgm:forEach name="each" axis="ch" ptType="node">'
       '<dgm:forEach name="lines" axis="precedSib" ptType="parTrans" st="-1" cnt="1"><dgm:layoutNode name="line">' +
       branch([('std', STD_LINE), ('init', '<dgm:choose name="d"><dgm:if name="d1" axis="self" func="depth" op="lte" '
                'val="2">' + STD_LINE + '</dgm:if><dgm:else name="d2">' +
                HANG_LINE % '<dgm:param type="srcNode" val="conn"/>' + '</dgm:else></dgm:choose>'),
               ('r', HANG_LINE % '<dgm:param type="srcNode" val="conn"/>')], HANG_LINE % '') +
       '<dgm:shape type="conn" zOrderOff="-99999"/><dgm:presOf axis="self"/></dgm:layoutNode></dgm:forEach>'
       '<dgm:layoutNode name="root"><dgm:varLst><dgm:hierBranch val="init"/></dgm:varLst>' +
       branch([('r', '<dgm:alg type="hierRoot"><dgm:param type="hierAlign" val="tL"/></dgm:alg>'
                '<dgm:constrLst><dgm:constr type="alignOff" val="0.25"/></dgm:constrLst>')],
              '<dgm:alg type="hierRoot"/>') +
       '<dgm:shape/><dgm:presOf/><dgm:layoutNode name="comp"><dgm:alg type="composite"/><dgm:shape/>'
       '<dgm:presOf axis="self" ptType="node" cnt="1"/>' +
       branch([(v, '<dgm:constrLst><dgm:constr type="l" for="ch" forName="box"/><dgm:constr type="t" for="ch" '
                'forName="box"/><dgm:constr type="w" for="ch" forName="box" refType="w"/><dgm:constr type="h" '
                'for="ch" forName="box" refType="h"/><dgm:constr type="%s" for="ch" forName="conn"%s/>'
                '<dgm:constr type="t" for="ch" forName="conn"/><dgm:constr type="w" for="ch" forName="conn" '
                'refType="w" fact="0.2"/><dgm:constr type="h" for="ch" forName="conn" refType="h"/>'
                '</dgm:constrLst>' % (('l', '') if v in ('init', 'r') else ('r', ' refType="w"')))
               for v in ('init', 'r')], '<dgm:constrLst><dgm:constr type="l" for="ch" forName="box"/><dgm:constr '
              'type="t" for="ch" forName="box"/><dgm:constr type="w" for="ch" forName="box" refType="w"/>'
              '<dgm:constr type="h" for="ch" forName="box" refType="h"/><dgm:constr type="r" for="ch" '
              'forName="conn" refType="w"/><dgm:constr type="t" for="ch" forName="conn"/><dgm:constr type="w" '
              'for="ch" forName="conn" refType="w" fact="0.2"/><dgm:constr type="h" for="ch" forName="conn" '
              'refType="h"/></dgm:constrLst>') +
       '<dgm:layoutNode name="box"><dgm:alg type="tx"/><dgm:shape type="rect"/><dgm:presOf axis="self"/>'
       '<dgm:constrLst>' + MARGINS + '</dgm:constrLst>' + SHRINK + '</dgm:layoutNode><dgm:layoutNode '
       'name="conn"><dgm:alg type="sp"/><dgm:shape type="rect" hideGeom="1"/><dgm:presOf/></dgm:layoutNode>'
       '</dgm:layoutNode><dgm:layoutNode name="kids">' +
       branch([('hang', '<dgm:alg type="hierChild"><dgm:param type="chAlign" val="l"/><dgm:param type="linDir" '
                'val="fromL"/><dgm:param type="secChAlign" val="t"/><dgm:param type="secLinDir" val="fromT"/>'
                '</dgm:alg>'),
               ('r', '<dgm:alg type="hierChild"><dgm:param type="chAlign" val="l"/><dgm:param type="linDir" '
                'val="fromT"/></dgm:alg>')], '<dgm:alg type="hierChild"/>') +
       '<dgm:shape/><dgm:presOf/><dgm:forEach name="more" ref="each"/></dgm:layoutNode></dgm:layoutNode>'
       '</dgm:forEach></dgm:layoutNode></dgm:layoutDef>') % DGM


# a list of pictures with text beside them: each item a picture placeholder (the user's picture in it, when there
# is one) and its text from there to the item's edge
pics = ('<dgm:layoutDef %s uniqueId="test/pictures"><dgm:layoutNode name="diagram"><dgm:alg type="lin">'
        '<dgm:param type="linDir" val="fromT"/></dgm:alg><dgm:shape/><dgm:presOf/><dgm:constrLst>'
        '<dgm:constr type="w" for="ch" forName="item" refType="w"/>'
        '<dgm:constr type="h" for="ch" forName="item" refType="h" fact="0.4"/>'
        '<dgm:constr type="sp" refType="h" refFor="ch" refForName="item" fact="0.2"/>'
        '<dgm:constr type="primFontSz" for="des" forName="txt" op="equ" val="65"/></dgm:constrLst>'
        '<dgm:forEach name="items" axis="ch" ptType="node"><dgm:layoutNode name="item"><dgm:alg type="composite"/>'
        '<dgm:shape/><dgm:presOf/><dgm:constrLst><dgm:constr type="l" for="ch" forName="pic"/>'
        '<dgm:constr type="t" for="ch" forName="pic"/><dgm:constr type="h" for="ch" forName="pic" refType="h"/>'
        '<dgm:constr type="w" for="ch" forName="pic" refType="h" refFor="ch" refForName="pic"/>'
        '<dgm:constr type="l" for="ch" forName="txt" refType="r" refFor="ch" refForName="pic"/>'
        '<dgm:constr type="ctrY" for="ch" forName="txt" refType="ctrY" refFor="ch" refForName="pic"/>'
        '<dgm:constr type="h" for="ch" forName="txt" refType="h" fact="0.5"/></dgm:constrLst>'
        '<dgm:layoutNode name="pic" styleLbl="node1"><dgm:alg type="sp"/><dgm:shape type="ellipse" '
        'blipPhldr="1"/><dgm:presOf/></dgm:layoutNode><dgm:layoutNode name="txt"><dgm:alg type="tx"/>'
        '<dgm:shape type="rect"/><dgm:presOf axis="self"/><dgm:constrLst>' + MARGINS + '</dgm:constrLst>' + SHRINK +
        '</dgm:layoutNode></dgm:layoutNode></dgm:forEach></dgm:layoutNode></dgm:layoutDef>') % DGM


# a process with curved arrows between its steps (connRout="curve")
curves = process.replace('uniqueId="test/process"', 'uniqueId="test/curves"').replace(
    '<dgm:layoutNode name="arrow"><dgm:alg type="conn"/>',
    '<dgm:layoutNode name="arrow"><dgm:alg type="conn"><dgm:param type="connRout" val="curve"/></dgm:alg>')

# labels turned a quarter (shape rot="270"), their text as gravity has it (reading up), centred across (txAnchorHorz)
turned = ('<dgm:layoutDef %s uniqueId="test/turned"><dgm:layoutNode name="diagram"><dgm:alg type="lin"/><dgm:shape/>'
          '<dgm:presOf/><dgm:constrLst><dgm:constr type="w" for="ch" forName="label" refType="h"/>'
          '<dgm:constr type="h" for="ch" forName="label" refType="h" fact="0.25"/>'
          '<dgm:constr type="w" for="ch" forName="gap" refType="h" refFor="ch" refForName="label" fact="2"/>'
          '<dgm:constr type="primFontSz" for="ch" forName="label" op="equ" val="65"/></dgm:constrLst>'
          '<dgm:forEach name="labels" axis="ch" ptType="node"><dgm:layoutNode name="label"><dgm:alg type="tx">'
          '<dgm:param type="autoTxRot" val="grav"/><dgm:param type="txAnchorHorz" val="ctr"/>'
          '<dgm:param type="parTxLTRAlign" val="l"/></dgm:alg><dgm:shape type="rect" rot="270"/>'
          '<dgm:presOf axis="desOrSelf" ptType="node"/><dgm:constrLst>' + MARGINS + '</dgm:constrLst>' + SHRINK +
          '</dgm:layoutNode><dgm:forEach name="gaps" axis="followSib" ptType="sibTrans" cnt="1">'
          '<dgm:layoutNode name="gap"><dgm:alg type="sp"/><dgm:shape/><dgm:presOf/></dgm:layoutNode></dgm:forEach>'
          '</dgm:forEach></dgm:layoutNode></dgm:layoutDef>') % DGM


def pyramid(rot):
    """the nodes as a pyramid's levels, the first at its top (turned round: an inverted pyramid)"""
    return ('<dgm:layoutDef %s uniqueId="test/pyramid"><dgm:layoutNode name="diagram"><dgm:alg type="pyra">'
            '<dgm:param type="linDir" val="fromT"/></dgm:alg><dgm:shape/><dgm:presOf/><dgm:constrLst>'
            '<dgm:constr type="primFontSz" for="ch" ptType="node" op="equ" val="65"/></dgm:constrLst>'
            '<dgm:forEach name="levels" axis="ch" ptType="node"><dgm:layoutNode name="level"><dgm:alg type="tx"/>'
            '<dgm:shape type="trapezoid"%s/><dgm:presOf axis="desOrSelf" ptType="node"/><dgm:constrLst>' +
            MARGINS + '</dgm:constrLst>' + SHRINK + '</dgm:layoutNode></dgm:forEach></dgm:layoutNode>'
            '</dgm:layoutDef>') % (DGM, ' rot="180"' if rot else '')


colors = ('<dgm:colorsDef %s uniqueId="test/colors">'
          '<dgm:styleLbl name="node1"><dgm:fillClrLst meth="repeat"><a:schemeClr val="accent1"/></dgm:fillClrLst>'
          '<dgm:linClrLst meth="repeat"><a:schemeClr val="lt1"/></dgm:linClrLst><dgm:txFillClrLst meth="repeat">'
          '<a:schemeClr val="lt1"/></dgm:txFillClrLst></dgm:styleLbl>'
          '<dgm:styleLbl name="sibTrans2D1"><dgm:fillClrLst meth="repeat"><a:schemeClr val="accent2"/>'
          '</dgm:fillClrLst><dgm:linClrLst meth="repeat"><a:schemeClr val="accent2"/></dgm:linClrLst>'
          '</dgm:styleLbl><dgm:styleLbl name="parChTrans1D1"><dgm:linClrLst meth="repeat"><a:schemeClr '
          'val="accent6"/></dgm:linClrLst></dgm:styleLbl></dgm:colorsDef>') % DGM

style = ('<dgm:styleDef %s uniqueId="test/style">'
         '<dgm:styleLbl name="node1"><dgm:style><a:lnRef idx="2"><a:scrgbClr r="0" g="0" b="0"/></a:lnRef>'
         '<a:fillRef idx="1"><a:scrgbClr r="0" g="0" b="0"/></a:fillRef><a:effectRef idx="0"><a:scrgbClr r="0" '
         'g="0" b="0"/></a:effectRef><a:fontRef idx="minor"><a:schemeClr val="lt1"/></a:fontRef></dgm:style>'
         '</dgm:styleLbl><dgm:styleLbl name="sibTrans2D1"><dgm:style><a:lnRef idx="0"><a:scrgbClr r="0" g="0" '
         'b="0"/></a:lnRef><a:fillRef idx="1"><a:scrgbClr r="0" g="0" b="0"/></a:fillRef><a:effectRef idx="0">'
         '<a:scrgbClr r="0" g="0" b="0"/></a:effectRef><a:fontRef idx="minor"/></dgm:style></dgm:styleLbl>'
         '<dgm:styleLbl name="parChTrans1D1"><dgm:style><a:lnRef idx="2"><a:scrgbClr r="0" g="0" b="0"/>'
         '</a:lnRef><a:fillRef idx="0"><a:scrgbClr r="0" g="0" b="0"/></a:fillRef><a:effectRef idx="0">'
         '<a:scrgbClr r="0" g="0" b="0"/></a:effectRef><a:fontRef idx="minor"/></dgm:style></dgm:styleLbl>'
         '</dgm:styleDef>') % DGM


# a quick style of the kind with depth: its nodes the theme's second fill (a gradient) and second effect (a
# shadow), a scene and a bevel of its own
style3d = style.replace('uniqueId="test/style"', 'uniqueId="test/style3d"').replace(
    '<dgm:styleLbl name="node1"><dgm:style><a:lnRef idx="2"><a:scrgbClr r="0" g="0" b="0"/></a:lnRef>'
    '<a:fillRef idx="1"><a:scrgbClr r="0" g="0" b="0"/></a:fillRef><a:effectRef idx="0">',
    '<dgm:styleLbl name="node1"><dgm:scene3d><a:camera prst="orthographicFront"/><a:lightRig rig="threePt" '
    'dir="t"/></dgm:scene3d><dgm:sp3d><a:bevelT w="63500" h="25400"/></dgm:sp3d><dgm:style><a:lnRef idx="0">'
    '<a:scrgbClr r="0" g="0" b="0"/></a:lnRef><a:fillRef idx="2"><a:scrgbClr r="0" g="0" b="0"/></a:fillRef>'
    '<a:effectRef idx="2">')
assert style3d != style

def data(tree, layout, vars=None, pictures=None):
    """a data model of a tree: [(text, [children])]; VARS: a node's text to its hierBranch (on its presentation
    point of the layout node named root); PICTURES: a node's text to the relationship of the picture in its
    placeholder (named pic)"""
    pts, cxns, n = ['<dgm:pt modelId="{doc}" type="doc"><dgm:prSet loTypeId="%s"/></dgm:pt>' % layout], [], [0]

    def add(parent, kids):
        for i, (text, sub) in enumerate(kids):
            n[0] += 1
            k = n[0]
            pts.append('<dgm:pt modelId="{n%d}"><dgm:prSet/><dgm:spPr/><dgm:t><a:bodyPr/><a:lstStyle/><a:p><a:r>'
                       '<a:rPr lang="en-US"/><a:t>%s</a:t></a:r></a:p></dgm:t></dgm:pt>' % (k, text))
            pts.append('<dgm:pt modelId="{p%d}" type="parTrans" cxnId="{c%d}"/>' % (k, k))
            pts.append('<dgm:pt modelId="{s%d}" type="sibTrans" cxnId="{c%d}"/>' % (k, k))
            cxns.append('<dgm:cxn modelId="{c%d}" srcId="{%s}" destId="{n%d}" srcOrd="%d" destOrd="0" '
                        'parTransId="{p%d}" sibTransId="{s%d}"/>' % (k, parent, k, i, k, k))

            if pictures and text in pictures:
                pts.append('<dgm:pt modelId="{q%d}" type="pres"><dgm:prSet presAssocID="{n%d}" presName="pic"/>'
                           '<dgm:spPr><a:blipFill><a:blip r:embed="%s"/><a:stretch><a:fillRect/></a:stretch>'
                           '</a:blipFill></dgm:spPr></dgm:pt>' % (k, k, pictures[text]))

            if vars and text in vars:
                pts.append('<dgm:pt modelId="{r%d}" type="pres"><dgm:prSet presAssocID="{n%d}" presName="root">'
                           '<dgm:presLayoutVars><dgm:hierBranch val="%s"/></dgm:presLayoutVars></dgm:prSet>'
                           '</dgm:pt>' % (k, k, vars[text]))
            add('n%d' % k, sub)

    add('doc', tree)
    return ('<dgm:dataModel %s><dgm:ptLst>%s</dgm:ptLst><dgm:cxnLst>%s</dgm:cxnLst><dgm:bg/><dgm:whole/>'
            '</dgm:dataModel>') % (DGM, ''.join(pts), ''.join(cxns))


# a diagram with a drawing saved of it: its one shape's text older than the model's
saved_data = ('<dgm:dataModel %s><dgm:ptLst><dgm:pt modelId="{doc}" type="doc"><dgm:prSet/></dgm:pt>'
              '<dgm:pt modelId="{n1}"><dgm:prSet/><dgm:spPr/><dgm:t><a:bodyPr/><a:lstStyle/><a:p><a:r>'
              '<a:rPr lang="en-US"/><a:t>Fresh</a:t></a:r></a:p></dgm:t></dgm:pt>'
              '<dgm:pt modelId="{p1}" type="parTrans" cxnId="{c1}"/><dgm:pt modelId="{s1}" type="sibTrans" '
              'cxnId="{c1}"/><dgm:pt modelId="{pr1}" type="pres"><dgm:prSet presAssocID="{n1}" presName="node" '
              'presStyleLbl="node1" presStyleIdx="0" presStyleCnt="1"/></dgm:pt></dgm:ptLst><dgm:cxnLst>'
              '<dgm:cxn modelId="{c1}" srcId="{doc}" destId="{n1}" srcOrd="0" destOrd="0" parTransId="{p1}" '
              'sibTransId="{s1}"/><dgm:cxn modelId="{c2}" type="presOf" srcId="{n1}" destId="{pr1}" srcOrd="0" '
              'destOrd="0"/></dgm:cxnLst><dgm:bg/><dgm:whole/><dgm:extLst><a:ext uri="http://schemas.microsoft.com/'
              'office/drawing/2008/diagram"><dsp:dataModelExt xmlns:dsp="http://schemas.microsoft.com/office/'
              'drawing/2008/diagram" relId="rId6"/></a:ext></dgm:extLst></dgm:dataModel>') % DGM
saved_drawing = ('<dsp:drawing xmlns:dsp="http://schemas.microsoft.com/office/drawing/2008/diagram" %s><dsp:spTree>'
                 '<dsp:nvGrpSpPr><dsp:cNvPr id="0" name=""/><dsp:cNvGrpSpPr/></dsp:nvGrpSpPr><dsp:grpSpPr/>'
                 '<dsp:sp modelId="{pr1}"><dsp:nvSpPr><dsp:cNvPr id="0" name=""/><dsp:cNvSpPr/></dsp:nvSpPr>'
                 '<dsp:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="2000000" cy="1000000"/></a:xfrm><a:prstGeom '
                 'prst="rect"><a:avLst/></a:prstGeom><a:solidFill><a:schemeClr val="accent1"/></a:solidFill>'
                 '</dsp:spPr><dsp:style><a:lnRef idx="0"><a:scrgbClr r="0" g="0" b="0"/></a:lnRef><a:fillRef '
                 'idx="1"><a:scrgbClr r="0" g="0" b="0"/></a:fillRef><a:effectRef idx="0"><a:scrgbClr r="0" g="0" '
                 'b="0"/></a:effectRef><a:fontRef idx="minor"><a:schemeClr val="lt1"/></a:fontRef></dsp:style>'
                 '<dsp:txBody><a:bodyPr anchor="ctr"/><a:lstStyle/><a:p><a:r><a:rPr lang="en-US" sz="1000"/>'
                 '<a:t>Stale</a:t></a:r></a:p></dsp:txBody></dsp:sp></dsp:spTree></dsp:drawing>') % DGM
saved_colors = colors.replace('<dgm:txFillClrLst meth="repeat"><a:schemeClr val="lt1"/></dgm:txFillClrLst>',
                              '<dgm:txFillClrLst meth="repeat"><a:schemeClr val="accent4"/></dgm:txFillClrLst>')

DIAGRAMS = [
    (snake, data([(t, []) for t in ('Alpha', 'Beta', 'Gamma', 'Delta', 'Epsilon')], 'test/blocks')),
    (process, data([(t, []) for t in ('Plan', 'Build', 'Ship')], 'test/process')),
    (hier, data([('Root', [('Left', []), ('Middle', [('One', []), ('Two', [])]), ('Right', [])])], 'test/hierarchy')),
    (process, saved_data),
    (cycle, data([(t, []) for t in ('Plan', 'Do', 'Check', 'Act')], 'test/cycle')),
    (radial, data([('Hub', [(t, []) for t in ('North', 'East', 'South', 'West', 'More')])], 'test/radial')),
    (pyramid(0), data([(t, []) for t in ('Top', 'Middle', 'Base')], 'test/pyramid')),
    (pyramid(1), data([(t, []) for t in ('Wide', 'Narrow')], 'test/pyramid')),
    (org, data([('Head', [('Init', [('i1', []), ('i2', []), ('i3', [])]),
                          ('Both', [('b1', []), ('b2', []), ('b3', []), ('b4', [])]),
                          ('Right', [('r1', []), ('r2', [])])])], 'test/org', {'Both': 'hang', 'Right': 'r'})),
    (pics, data([('Pictured', []), ('Placeholder', [])], 'test/pictures', pictures={'Pictured': 'rId1'})),
    (curves, data([(t, []) for t in ('Plan', 'Build', 'Ship')], 'test/curves')),
    (turned, data([(t, []) for t in ('Up', 'Turned')], 'test/turned')),
    (process, data([(t, []) for t in ('Depth', 'Shadow')], 'test/process')),
]
STYLES = {len(DIAGRAMS): style3d}  # slide 13's quick style
PICTURE = open('tests/data/rgba.png', 'rb').read()


def slide(k):
    return ('<p:sld %s><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/>'
            '</p:nvGrpSpPr><p:grpSpPr/><p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id="2" name="Diagram %d"/>'
            '<p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr><p:xfrm><a:off x="1524000" y="571500"/>'
            '<a:ext cx="6096000" cy="4000500"/></p:xfrm><a:graphic><a:graphicData '
            'uri="http://schemas.openxmlformats.org/drawingml/2006/diagram"><dgm:relIds '
            'xmlns:dgm="http://schemas.openxmlformats.org/drawingml/2006/diagram" r:dm="rId2" r:lo="rId3" '
            'r:qs="rId4" r:cs="rId5"/></a:graphicData></a:graphic></p:graphicFrame></p:spTree></p:cSld>'
            '</p:sld>') % (A, k)


theme = ('<a:theme xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" name="T"><a:themeElements>'
         '<a:clrScheme name="T"><a:dk1><a:srgbClr val="000000"/></a:dk1><a:lt1><a:srgbClr val="FFFFFF"/></a:lt1>'
         '<a:dk2><a:srgbClr val="1F3864"/></a:dk2><a:lt2><a:srgbClr val="E7E6E6"/></a:lt2><a:accent1><a:srgbClr '
         'val="4472C4"/></a:accent1><a:accent2><a:srgbClr val="ED7D31"/></a:accent2><a:accent3><a:srgbClr '
         'val="A5A5A5"/></a:accent3><a:accent4><a:srgbClr val="FFC000"/></a:accent4><a:accent5><a:srgbClr '
         'val="5B9BD5"/></a:accent5><a:accent6><a:srgbClr val="70AD47"/></a:accent6><a:hlink><a:srgbClr '
         'val="0563C1"/></a:hlink><a:folHlink><a:srgbClr val="954F72"/></a:folHlink></a:clrScheme><a:fontScheme '
         'name="T"><a:majorFont><a:latin typeface="Carlito"/></a:majorFont><a:minorFont><a:latin typeface="Carlito"/>'
         '</a:minorFont></a:fontScheme><a:fmtScheme name="T"><a:fillStyleLst><a:solidFill><a:schemeClr '
         'val="phClr"/></a:solidFill><a:gradFill rotWithShape="1"><a:gsLst><a:gs pos="0"><a:schemeClr val="phClr">'
         '<a:tint val="50000"/></a:schemeClr></a:gs><a:gs pos="100000"><a:schemeClr val="phClr"><a:shade '
         'val="80000"/></a:schemeClr></a:gs></a:gsLst><a:lin ang="5400000" scaled="0"/></a:gradFill><a:solidFill>'
         '<a:schemeClr val="phClr"/></a:solidFill></a:fillStyleLst><a:lnStyleLst><a:ln w="9525"/>'
         '<a:ln w="25400"/><a:ln w="38100"/></a:lnStyleLst><a:effectStyleLst><a:effectStyle><a:effectLst/>'
         '</a:effectStyle><a:effectStyle><a:effectLst><a:outerShdw blurRad="40000" dist="38100" dir="5400000" '
         'rotWithShape="0"><a:srgbClr val="000000"><a:alpha val="40000"/></a:srgbClr></a:outerShdw></a:effectLst>'
         '</a:effectStyle><a:effectStyle><a:effectLst/></a:effectStyle></a:effectStyleLst></a:fmtScheme>'
         '</a:themeElements></a:theme>')
master = ('<p:sldMaster %s><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/>'
          '</p:nvGrpSpPr><p:grpSpPr/></p:spTree></p:cSld><p:clrMap bg1="lt1" tx1="dk1" bg2="lt2" tx2="dk2" '
          'accent1="accent1" accent2="accent2" accent3="accent3" accent4="accent4" accent5="accent5" '
          'accent6="accent6" hlink="hlink" folHlink="folHlink"/></p:sldMaster>') % A
layout = ('<p:sldLayout %s><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/>'
          '</p:nvGrpSpPr><p:grpSpPr/></p:spTree></p:cSld></p:sldLayout>') % A
pres = ('<p:presentation %s><p:sldMasterIdLst><p:sldMasterId id="2147483648" r:id="rId1"/></p:sldMasterIdLst>'
        '<p:sldIdLst>%s</p:sldIdLst><p:sldSz cx="9144000" cy="5143500"/></p:presentation>') % (
            A, ''.join('<p:sldId id="%d" r:id="rId%d"/>' % (256 + k, k + 2) for k in range(len(DIAGRAMS))))

with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('[Content_Types].xml', '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"/>')
    z.writestr('ppt/presentation.xml', pres)
    z.writestr('ppt/_rels/presentation.xml.rels',
               rels(('rId1', 'slideMaster', 'slideMasters/slideMaster1.xml'),
                    *[('rId%d' % (k + 2), 'slide', 'slides/slide%d.xml' % (k + 1)) for k in range(len(DIAGRAMS))]))
    z.writestr('ppt/slideMasters/slideMaster1.xml', master)
    z.writestr('ppt/slideMasters/_rels/slideMaster1.xml.rels', rels(('rId1', 'theme', '../theme/theme1.xml')))
    z.writestr('ppt/slideLayouts/slideLayout1.xml', layout)
    z.writestr('ppt/slideLayouts/_rels/slideLayout1.xml.rels',
               rels(('rId1', 'slideMaster', '../slideMasters/slideMaster1.xml')))
    z.writestr('ppt/theme/theme1.xml', theme)

    for k, (lo, dm) in enumerate(DIAGRAMS, 1):
        z.writestr('ppt/slides/slide%d.xml' % k, slide(k))
        z.writestr('ppt/slides/_rels/slide%d.xml.rels' % k,
                   rels(('rId1', 'slideLayout', '../slideLayouts/slideLayout1.xml'),
                        ('rId2', 'diagramData', '../diagrams/data%d.xml' % k),
                        ('rId3', 'diagramLayout', '../diagrams/layout%d.xml' % k),
                        ('rId4', 'diagramQuickStyle', '../diagrams/quickStyle%d.xml' % k),
                        ('rId5', 'diagramColors', '../diagrams/colors%d.xml' % k),
                        ('rId6', 'diagramDrawing', '../diagrams/drawing%d.xml' % k)))
        z.writestr('ppt/diagrams/data%d.xml' % k, dm)

        if lo is pics:
            z.writestr('ppt/diagrams/_rels/data%d.xml.rels' % k, rels(('rId1', 'image', '../media/image1.png')))
            z.writestr('ppt/media/image1.png', PICTURE)
        z.writestr('ppt/diagrams/layout%d.xml' % k, lo)
        z.writestr('ppt/diagrams/quickStyle%d.xml' % k, STYLES.get(k, style))
        z.writestr('ppt/diagrams/colors%d.xml' % k, saved_colors if dm is saved_data else colors)

        if dm is saved_data:
            z.writestr('ppt/diagrams/drawing%d.xml' % k, saved_drawing)

if len(sys.argv) > 2:   # the Word document
    W = 'xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" ' \
        'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" ' \
        'xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing" ' \
        'xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"'

    def inline(k, base):
        return ('<w:p><w:r><w:drawing><wp:inline distT="0" distB="0" distL="0" distR="0"><wp:extent cx="5486400" '
                'cy="3200400"/><wp:effectExtent l="0" t="0" r="0" b="0"/><wp:docPr id="%d" name="Diagram %d"/>'
                '<wp:cNvGraphicFramePr/><a:graphic><a:graphicData '
                'uri="http://schemas.openxmlformats.org/drawingml/2006/diagram"><dgm:relIds '
                'xmlns:dgm="http://schemas.openxmlformats.org/drawingml/2006/diagram" r:dm="rId%d" r:lo="rId%d" '
                'r:qs="rId%d" r:cs="rId%d"/></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>') % (
                    k, k, base, base + 1, base + 2, base + 3)

    doc = ('<w:document %s><w:body><w:p><w:r><w:t>A cycle:</w:t></w:r></w:p>%s<w:p><w:r><w:t>A diagram with a '
           'drawing:</w:t></w:r></w:p>%s<w:p><w:r><w:t>Pictures:</w:t></w:r></w:p>%s<w:sectPr><w:pgSz w:w="12240" w:h="15840"/><w:pgMar w:top="1440" '
           'w:right="1440" w:bottom="1440" w:left="1440" w:header="720" w:footer="720" w:gutter="0"/></w:sectPr>'
           '</w:body></w:document>') % (W, inline(1, 2), inline(2, 12), inline(3, 22))
    items = [('rId1', 'theme', 'theme/theme1.xml'), ('rId6', 'diagramDrawing', 'diagrams/drawing2.xml')]

    for k, base in ((1, 2), (2, 12), (3, 22)):
        items += [('rId%d' % base, 'diagramData', 'diagrams/data%d.xml' % k),
                  ('rId%d' % (base + 1), 'diagramLayout', 'diagrams/layout%d.xml' % k),
                  ('rId%d' % (base + 2), 'diagramQuickStyle', 'diagrams/quickStyle%d.xml' % k),
                  ('rId%d' % (base + 3), 'diagramColors', 'diagrams/colors%d.xml' % k)]

    with zipfile.ZipFile(sys.argv[2], 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('[Content_Types].xml', '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
                   '<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-'
                   'officedocument.wordprocessingml.document.main+xml"/></Types>')
        z.writestr('_rels/.rels', rels(('rId1', 'officeDocument', 'word/document.xml')))
        z.writestr('word/document.xml', doc)
        z.writestr('word/_rels/document.xml.rels', rels(*items))
        z.writestr('word/theme/theme1.xml', theme)

        z.writestr('word/diagrams/_rels/data3.xml.rels', rels(('rId1', 'image', '../media/image1.png')))
        z.writestr('word/media/image1.png', PICTURE)

        for k, (lo, dm) in enumerate([DIAGRAMS[4], DIAGRAMS[3], DIAGRAMS[9]], 1):
            z.writestr('word/diagrams/data%d.xml' % k, dm)
            z.writestr('word/diagrams/layout%d.xml' % k, lo)
            z.writestr('word/diagrams/quickStyle%d.xml' % k, style)
            z.writestr('word/diagrams/colors%d.xml' % k, saved_colors if dm is saved_data else colors)

        z.writestr('word/diagrams/drawing2.xml', saved_drawing)
