#!/usr/bin/env python3
"""tests/data/slides.pptx: a small presentation for the PowerPoint reader's tests -- a master with placeholders,
text styles and a picture on every slide, a layout taking where its placeholders are from the master, a theme;
slide 1 a title and bulleted body text in the placeholders, a filled shape with text, a connector joining it to
an ellipse, a box with a gradient and a shadow; slide 2 a cropped picture, a picture turned, flipped and cut to an
ellipse, a table.

    python3 tools/make_test_pptx.py tests/data/slides.pptx
"""
import sys
import zipfile

A = 'xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" ' \
    'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" ' \
    'xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main"'
REL = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships/'


def rels(*items):
    return '<?xml version="1.0" encoding="UTF-8"?><Relationships ' \
        'xmlns="http://schemas.openxmlformats.org/package/2006/relationships">' + \
        ''.join('<Relationship Id="%s" Type="%s%s" Target="%s"/>' % (i, REL, t, g) for i, t, g in items) + \
        '</Relationships>'


def ph_sp(i, name, ph, xfrm='', body=''):
    return ('<p:sp><p:nvSpPr><p:cNvPr id="%d" name="%s"/><p:cNvSpPr/><p:nvPr>%s</p:nvPr></p:nvSpPr>'
            '<p:spPr>%s</p:spPr>%s</p:sp>') % (i, name, ph, xfrm, body)


def xfrm(x, y, w, h):
    return '<a:xfrm><a:off x="%d" y="%d"/><a:ext cx="%d" cy="%d"/></a:xfrm>' % (x, y, w, h)


TITLE_PH = '<p:ph type="title"/>'
BODY_PH = '<p:ph idx="1"/>'
master = ('<p:sldMaster %s><p:cSld><p:bg><p:bgPr><a:solidFill><a:srgbClr val="FFFFFF"/></a:solidFill></p:bgPr></p:bg>'
          '<p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>' % A +
          ph_sp(2, 'Title', TITLE_PH, xfrm(457200, 200000, 8229600, 900000),
                '<p:txBody><a:bodyPr anchor="b"/><a:lstStyle/><a:p><a:r><a:t>Title</a:t></a:r></a:p></p:txBody>') +
          ph_sp(3, 'Body', '<p:ph type="body" idx="1"/>', xfrm(457200, 1200000, 8229600, 3200000),
                '<p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>Text</a:t></a:r></a:p></p:txBody>') +
          '<p:sp><p:nvSpPr><p:cNvPr id="4" name="Band"/><p:cNvSpPr/><p:nvPr userDrawn="1"/></p:nvSpPr><p:spPr>' +
          xfrm(0, 4943500, 9144000, 200000) + '<a:prstGeom prst="rect"><a:avLst/></a:prstGeom><a:solidFill>'
          '<a:schemeClr val="accent1"/></a:solidFill></p:spPr></p:sp>'
          '</p:spTree></p:cSld><p:clrMap bg1="lt1" tx1="dk1" bg2="lt2" tx2="dk2" accent1="accent1" accent2="accent2" '
          'accent3="accent3" accent4="accent4" accent5="accent5" accent6="accent6" hlink="hlink" folHlink="folHlink"/>'
          '<p:txStyles><p:titleStyle><a:lvl1pPr algn="ctr"><a:defRPr sz="4400" b="1"><a:solidFill>'
          '<a:schemeClr val="tx2"/></a:solidFill><a:latin typeface="+mj-lt"/></a:defRPr></a:lvl1pPr></p:titleStyle>'
          '<p:bodyStyle><a:lvl1pPr marL="342900" indent="-342900"><a:buChar char="&#8226;"/><a:defRPr sz="2800">'
          '<a:solidFill><a:schemeClr val="tx1"/></a:solidFill></a:defRPr></a:lvl1pPr><a:lvl2pPr marL="742950" '
          'indent="-285750"><a:buChar char="&#8211;"/><a:defRPr sz="2400"/></a:lvl2pPr></p:bodyStyle><p:otherStyle>'
          '<a:lvl1pPr><a:defRPr sz="1800"/></a:lvl1pPr></p:otherStyle></p:txStyles></p:sldMaster>')
layout = ('<p:sldLayout %s><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/>'
          '</p:nvGrpSpPr><p:grpSpPr/>' % A + ph_sp(2, 'Title', TITLE_PH) + ph_sp(3, 'Content', BODY_PH) +
          '</p:spTree></p:cSld></p:sldLayout>')
slide1 = ('<p:sld %s><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>'
          '<p:grpSpPr/>' % A +
          ph_sp(2, 'Title 1', TITLE_PH, '', '<p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:rPr lang="en-US"/>'
                '<a:t>Hello Slides</a:t></a:r></a:p></p:txBody>') +
          ph_sp(3, 'Content 2', BODY_PH, '', '<p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>First point</a:t>'
                '</a:r></a:p><a:p><a:pPr lvl="1"/><a:r><a:t>A detail</a:t></a:r></a:p></p:txBody>') +
          '<p:sp><p:nvSpPr><p:cNvPr id="4" name="Box"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr>' +
          xfrm(5000000, 3000000, 1500000, 800000) + '<a:prstGeom prst="roundRect"><a:avLst/></a:prstGeom><a:solidFill>'
          '<a:schemeClr val="accent2"/></a:solidFill></p:spPr><p:txBody><a:bodyPr anchor="ctr"/><a:lstStyle/>'
          '<a:p><a:pPr algn="ctr"/><a:r><a:rPr sz="2000" b="1"><a:solidFill><a:srgbClr val="FFFFFF"/></a:solidFill>'
          '</a:rPr><a:t>Box &amp; text</a:t></a:r></a:p></p:txBody></p:sp>'
          '<p:sp><p:nvSpPr><p:cNvPr id="5" name="Oval"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr>' +
          xfrm(7500000, 3000000, 800000, 800000) + '<a:prstGeom prst="ellipse"><a:avLst/></a:prstGeom></p:spPr>'
          '<p:style><a:lnRef idx="2"><a:schemeClr val="accent1"/></a:lnRef><a:fillRef idx="1"><a:schemeClr '
          'val="accent1"/></a:fillRef><a:effectRef idx="0"><a:schemeClr val="accent1"/></a:effectRef><a:fontRef '
          'idx="minor"><a:schemeClr val="lt1"/></a:fontRef></p:style></p:sp>'
          '<p:cxnSp><p:nvCxnSpPr><p:cNvPr id="6" name="Connector"/><p:cNvCxnSpPr><a:stCxn id="4" idx="3"/>'
          '<a:endCxn id="5" idx="2"/></p:cNvCxnSpPr><p:nvPr/></p:nvCxnSpPr><p:spPr>' +
          xfrm(6500000, 3400000, 1000000, 1) + '<a:prstGeom prst="straightConnector1"><a:avLst/></a:prstGeom><a:ln '
          'w="19050"><a:solidFill><a:srgbClr val="000000"/></a:solidFill><a:tailEnd type="triangle"/></a:ln>'
          '</p:spPr></p:cxnSp>'
          '<p:sp><p:nvSpPr><p:cNvPr id="8" name="Gradient"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr>' +
          xfrm(500000, 3000000, 1500000, 800000) + '<a:prstGeom prst="rect"><a:avLst/></a:prstGeom><a:gradFill>'
          '<a:gsLst><a:gs pos="0"><a:srgbClr val="FF0000"/></a:gs><a:gs pos="100000"><a:srgbClr val="0000FF"/></a:gs>'
          '</a:gsLst><a:lin ang="0" scaled="0"/></a:gradFill><a:effectLst><a:outerShdw blurRad="40000" dist="38100" '
          'dir="2700000"><a:srgbClr val="000000"><a:alpha val="40000"/></a:srgbClr></a:outerShdw></a:effectLst>'
          '</p:spPr></p:sp>'
          '<p:sp><p:nvSpPr><p:cNvPr id="7" name="Number"/><p:cNvSpPr/><p:nvPr><p:ph type="sldNum" idx="12"/></p:nvPr>'
          '</p:nvSpPr><p:spPr>' + xfrm(8500000, 4700000, 500000, 300000) + '</p:spPr><p:txBody><a:bodyPr/>'
          '<a:lstStyle/><a:p><a:fld id="{1}" type="slidenum"><a:rPr lang="en-US"/><a:t>&#8249;#&#8250;</a:t></a:fld>'
          '</a:p></p:txBody></p:sp></p:spTree></p:cSld></p:sld>')
slide2 = ('<p:sld %s><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>'
          '<p:grpSpPr/>' % A +
          '<p:pic><p:nvPicPr><p:cNvPr id="2" name="Picture"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr><p:blipFill>'
          '<a:blip r:embed="rId2"/><a:srcRect l="25000" r="25000"/><a:stretch><a:fillRect/></a:stretch></p:blipFill>'
          '<p:spPr>' + xfrm(500000, 500000, 2000000, 2000000) + '<a:prstGeom prst="rect"><a:avLst/></a:prstGeom>'
          '</p:spPr></p:pic>'
          '<p:pic><p:nvPicPr><p:cNvPr id="4" name="Turned"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr><p:blipFill>'
          '<a:blip r:embed="rId2"/><a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr><a:xfrm rot="2700000" '
          'flipH="1"><a:off x="500000" y="3000000"/><a:ext cx="1200000" cy="900000"/></a:xfrm><a:prstGeom '
          'prst="ellipse"><a:avLst/></a:prstGeom></p:spPr></p:pic>'
          '<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id="3" name="Table"/><p:cNvGraphicFramePr/><p:nvPr/>'
          '</p:nvGraphicFramePr><p:xfrm><a:off x="3500000" y="500000"/><a:ext cx="4000000" cy="740000"/></p:xfrm>'
          '<a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/table"><a:tbl><a:tblPr '
          'firstRow="1" bandRow="1"/><a:tblGrid><a:gridCol w="2000000"/><a:gridCol w="2000000"/></a:tblGrid>'
          '<a:tr h="370000"><a:tc><a:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>Name</a:t></a:r></a:p></a:txBody>'
          '<a:tcPr/></a:tc><a:tc><a:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>Value</a:t></a:r></a:p></a:txBody>'
          '<a:tcPr/></a:tc></a:tr><a:tr h="370000"><a:tc><a:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>width</a:t>'
          '</a:r></a:p></a:txBody><a:tcPr/></a:tc><a:tc><a:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>42</a:t>'
          '</a:r></a:p></a:txBody><a:tcPr/></a:tc></a:tr></a:tbl></a:graphicData></a:graphic></p:graphicFrame>'
          '</p:spTree></p:cSld></p:sld>')
theme = ('<a:theme xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" name="T"><a:themeElements>'
         '<a:clrScheme name="T"><a:dk1><a:srgbClr val="000000"/></a:dk1><a:lt1><a:srgbClr val="FFFFFF"/></a:lt1>'
         '<a:dk2><a:srgbClr val="1F3864"/></a:dk2><a:lt2><a:srgbClr val="E7E6E6"/></a:lt2><a:accent1><a:srgbClr '
         'val="4472C4"/></a:accent1><a:accent2><a:srgbClr val="ED7D31"/></a:accent2><a:accent3><a:srgbClr '
         'val="A5A5A5"/></a:accent3><a:accent4><a:srgbClr val="FFC000"/></a:accent4><a:accent5><a:srgbClr '
         'val="5B9BD5"/></a:accent5><a:accent6><a:srgbClr val="70AD47"/></a:accent6><a:hlink><a:srgbClr '
         'val="0563C1"/></a:hlink><a:folHlink><a:srgbClr val="954F72"/></a:folHlink></a:clrScheme><a:fontScheme '
         'name="T"><a:majorFont><a:latin typeface="Carlito"/></a:majorFont><a:minorFont><a:latin typeface="Carlito"/>'
         '</a:minorFont></a:fontScheme><a:fmtScheme name="T"/></a:themeElements></a:theme>')
pres = ('<p:presentation %s><p:sldMasterIdLst><p:sldMasterId id="2147483648" r:id="rId1"/></p:sldMasterIdLst>'
        '<p:sldIdLst><p:sldId id="256" r:id="rId2"/><p:sldId id="257" r:id="rId3"/></p:sldIdLst>'
        '<p:sldSz cx="9144000" cy="5143500"/></p:presentation>' % A)

with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('[Content_Types].xml', '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"/>')
    z.writestr('ppt/presentation.xml', pres)
    z.writestr('ppt/_rels/presentation.xml.rels', rels(('rId1', 'slideMaster', 'slideMasters/slideMaster1.xml'),
                                                      ('rId2', 'slide', 'slides/slide1.xml'),
                                                      ('rId3', 'slide', 'slides/slide2.xml')))
    z.writestr('ppt/slideMasters/slideMaster1.xml', master)
    z.writestr('ppt/slideMasters/_rels/slideMaster1.xml.rels', rels(('rId1', 'theme', '../theme/theme1.xml')))
    z.writestr('ppt/slideLayouts/slideLayout1.xml', layout)
    z.writestr('ppt/slideLayouts/_rels/slideLayout1.xml.rels',
               rels(('rId1', 'slideMaster', '../slideMasters/slideMaster1.xml')))
    z.writestr('ppt/slides/slide1.xml', slide1)
    z.writestr('ppt/slides/_rels/slide1.xml.rels', rels(('rId1', 'slideLayout', '../slideLayouts/slideLayout1.xml')))
    z.writestr('ppt/slides/slide2.xml', slide2)
    z.writestr('ppt/slides/_rels/slide2.xml.rels', rels(('rId1', 'slideLayout', '../slideLayouts/slideLayout1.xml'),
                                                        ('rId2', 'image', '../media/image1.png')))
    z.writestr('ppt/theme/theme1.xml', theme)
    z.write('tests/data/rgba.png', 'ppt/media/image1.png')
