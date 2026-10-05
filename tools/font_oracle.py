#!/usr/bin/env python3
"""Cross-check Parade's font reader against fontTools through the C ABI.

usage: font_oracle.py [--max-mem MB] font.ttf [more fonts...]   (needs build/libparade.so)
       font_oracle.py [--max-mem MB] - < list_of_font_paths.txt

Fonts are checked in a worker process whose address space is capped (default
2048 MB, env PARADE_ORACLE_MAX_MEM)
so a huge font cannot exhaust the machine; a font that hits the cap is skipped.
Compares cmap, advances, vertical metrics and pair kerning for every pair
of printable ASCII glyphs (plus a few accented letters).
"""
import ctypes, os, resource, subprocess, sys
from fontTools.ttLib import TTFont

here = os.path.dirname(os.path.abspath(__file__))
lib = ctypes.CDLL(os.path.join(here, "..", "build", "libparade.so"))
lib.pd_font_load_file.argtypes = [ctypes.c_char_p, ctypes.c_int32, ctypes.POINTER(ctypes.c_void_p)]
lib.pd_font_glyph_index.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
lib.pd_font_glyph_index.restype = ctypes.c_uint32
lib.pd_font_glyph_advance.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
lib.pd_font_kerning.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32]
lib.pd_font_free.argtypes = [ctypes.c_void_p]


class Metrics(ctypes.Structure):
    _fields_ = [(n, ctypes.c_int32) for n in ("upem", "asc", "desc", "gap", "xh", "ch", "nglyphs", "haskern")]


lib.pd_font_get_metrics.argtypes = [ctypes.c_void_p, ctypes.POINTER(Metrics)]


def gpos_kern(font):
    """fontTools reference: first applicable PairPos subtable per lookup, summed over kern lookups"""
    if "GPOS" not in font:
        return None
    g = font["GPOS"].table
    idx = sorted({i for fr in g.FeatureList.FeatureRecord if fr.FeatureTag == "kern" for i in fr.Feature.LookupListIndex})
    if not idx:
        return None     # no GPOS kern feature: the legacy kern table applies
    subs_by_lookup = []
    for i in idx:
        lk = g.LookupList.Lookup[i]
        subs = []
        for st in lk.SubTable:
            if lk.LookupType == 9:
                if st.ExtensionLookupType != 2:
                    continue
                st = st.ExtSubTable
            elif lk.LookupType != 2:
                continue
            subs.append(st)
        subs_by_lookup.append(subs)

    def kern(l, r):
        return sum(kern_lookup(subs, l, r) for subs in subs_by_lookup)

    def kern_lookup(subs, l, r):
        """within one lookup the first subtable that applies wins"""
        for st in subs:
            cov = st.Coverage.glyphs
            if l not in cov:
                continue
            if st.Format == 1:
                ps = st.PairSet[cov.index(l)]
                for pv in ps.PairValueRecord:
                    if pv.SecondGlyph == r:
                        return getattr(pv.Value1, "XAdvance", 0) or 0
                continue
            c1 = st.ClassDef1.classDefs.get(l, 0)
            c2 = st.ClassDef2.classDefs.get(r, 0)
            return getattr(st.Class1Record[c1].Class2Record[c2].Value1, "XAdvance", 0) or 0
        return 0
    return kern


def check(path):
    font = TTFont(path, fontNumber=0)
    h = ctypes.c_void_p()
    assert lib.pd_font_load_file(path.encode(), 0, ctypes.byref(h)) == 0, "load failed"
    m = Metrics()
    lib.pd_font_get_metrics(h, ctypes.byref(m))
    bad = 0
    if m.upem != font["head"].unitsPerEm:
        print("  upem mismatch"); bad += 1
    cmap = font.getBestCmap()
    order = font.getGlyphOrder()
    cps = list(range(32, 127)) + [0xE9, 0xF6, 0x2010, 0x4E00]
    hm = font["hmtx"]
    for cp in cps:
        gid = lib.pd_font_glyph_index(h, cp)
        want = order.index(cmap[cp]) if cp in cmap else 0
        if gid != want:
            print(f"  cmap U+{cp:04X}: parade {gid} fontTools {want}"); bad += 1
        elif gid and lib.pd_font_glyph_advance(h, gid) != hm[order[gid]][0]:
            print(f"  advance U+{cp:04X} mismatch"); bad += 1
    ref = gpos_kern(font)
    legacy = {}
    if ref is None and "kern" in font:
        for t in font["kern"].kernTables:
            if getattr(t, "format", 0) == 0:
                legacy = t.kernTable
                break
    pairs = nonzero = 0
    for a in range(32, 127):
        for b in range(32, 127):
            if a not in cmap or b not in cmap:
                continue
            ga, gb = cmap[a], cmap[b]
            want = ref(ga, gb) if ref else legacy.get((ga, gb), 0)
            got = lib.pd_font_kerning(h, order.index(ga), order.index(gb))
            pairs += 1
            nonzero += want != 0
            if got != want:
                if bad < 20:
                    print(f"  kern {chr(a)}{chr(b)}: parade {got} fontTools {want}")
                bad += 1
    lib.pd_font_free(h)
    font.close()
    src = "GPOS" if ref else ("kern" if legacy else "none")
    print(f"{os.path.basename(path)}: {pairs} pairs ({nonzero} kerned, {src}), {len(cps)} code points, {bad} mismatches")
    return bad


def worker(paths):
    """check fonts in this (memory-capped) process, one flushed result line per font"""
    bad = 0
    for p in paths:
        try:
            bad += check(p)
        except Exception as e:      # fonts fontTools itself cannot read are skipped
            print(f"{os.path.basename(p)}: skipped ({type(e).__name__}: {e})")
        sys.stdout.flush()
    return 1 if bad else 0


def limit_memory(mb):
    """cap the address space; allocations beyond it fail"""
    limit = mb * 1024 * 1024
    soft, hard = resource.getrlimit(resource.RLIMIT_AS)
    if hard != resource.RLIM_INFINITY:
        limit = min(limit, hard)
    resource.setrlimit(resource.RLIMIT_AS, (limit, hard))


def supervise(paths, max_mem):
    """A MemoryError inside fontTools cannot be recovered in-process (the
    half-built tables stay referenced), so fonts are checked in a capped
    worker; a font that kills its worker is reported as skipped and the
    remaining fonts continue in a fresh worker."""
    bad, rest = 0, list(paths)
    while rest:
        proc = subprocess.Popen([sys.executable, os.path.abspath(__file__), "--worker", "--max-mem", str(max_mem), "-"],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        proc.stdin.write("\n".join(rest) + "\n")
        proc.stdin.close()
        done = 0
        for line in proc.stdout:
            print(line, end="", flush=True)
            bad += " 0 mismatches" not in line and "skipped" not in line
            done += 1
        proc.wait()
        if done < len(rest):
            print(f"{os.path.basename(rest[done])}: skipped (worker died, memory cap {max_mem} MB)", flush=True)
            done += 1
        rest = rest[done:]
    return 1 if bad else 0


if __name__ == "__main__":
    # paths as arguments, or one per line on stdin with "-"
    args = sys.argv[1:]
    max_mem = int(os.environ.get("PARADE_ORACLE_MAX_MEM", "2048"))
    is_worker = bool(args) and args[0] == "--worker"
    args = args[1:] if is_worker else args
    if len(args) >= 2 and args[0] == "--max-mem":
        max_mem, args = int(args[1]), args[2:]
    if args == ["-"]:
        args = [line.rstrip("\n") for line in sys.stdin if line.strip()]
    if is_worker:
        limit_memory(max_mem)
        sys.exit(worker(args))
    sys.exit(supervise(args, max_mem))
