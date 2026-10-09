#!/usr/bin/env python3
"""tests/data/smartart.pptx: SmartArt with no drawing saved of it, for the layout of diagrams from their definitions
(src/pd_dgm.c) -- slide 1 a block list of five (snake: three in a row, two under them, centred), slide 2 a process
of three with arrows between them (lin, conn), slide 3 a hierarchy of a root and three children, one of them with
two of its own, joined by bent lines (hierRoot, hierChild, a forEach by ref); the layout definitions are small ones
written for these, in the schema PowerPoint's are in, with a colour definition and a quick style. Slide 4 has a
drawing saved of it, older than its text: the text the data model's, at the size the drawing's runs are, in the
colour the colour definition gives text (not the drawing's fontRef).

    python3 tools/make_test_smartart.py tests/data/smartart.pptx
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


def data(tree, layout):
    """a data model of a tree: [(text, [children])]"""
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
]


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
         '</a:minorFont></a:fontScheme><a:fmtScheme name="T"><a:fillStyleLst/><a:lnStyleLst><a:ln w="9525"/>'
         '<a:ln w="25400"/><a:ln w="38100"/></a:lnStyleLst></a:fmtScheme></a:themeElements></a:theme>')
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
        z.writestr('ppt/diagrams/layout%d.xml' % k, lo)
        z.writestr('ppt/diagrams/quickStyle%d.xml' % k, style)
        z.writestr('ppt/diagrams/colors%d.xml' % k, saved_colors if dm is saved_data else colors)

        if dm is saved_data:
            z.writestr('ppt/diagrams/drawing%d.xml' % k, saved_drawing)
