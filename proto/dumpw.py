from fontTools.ttLib import TTFont
f=TTFont('/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf')
cm=f.getBestCmap(); hm=f['hmtx']; upm=f['head'].unitsPerEm
# pair kerning from GPOS is complex; use legacy 'kern' if present
kern={}
if 'kern' in f:
    for t in f['kern'].kernTables:
        kern.update(t.kernTable)
g2c={}
for c in range(32,127):
    if c in cm: g2c[cm[c]]=c
print(upm)
print(' '.join(str(hm[cm[c]][0]) if c in cm else '0' for c in range(32,127)))
pairs=[(g2c[a],g2c[b],v) for (a,b),v in kern.items() if a in g2c and b in g2c]
print(len(pairs))
for a,b,v in pairs: print(a,b,v)
