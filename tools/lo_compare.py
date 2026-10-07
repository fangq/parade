#!/usr/bin/env python3
"""Compare Parade's PDF of each .docx with LibreOffice's, over a corpus.

  python3 tools/lo_compare.py LIST OUTDIR [--pd-conv build/pd_conv] [--limit N] [--dpi 30]

LIST is a file of .docx paths, one a line. For each document, both PDFs are
made (pd_conv; soffice --convert-to pdf, in batches) and compared:

  pages     the page counts
  text      how much of the words agree, in order (difflib ratio over the whole text)
  bag       how much of the words agree, in any order (shared words over the larger count): text below
            bag is reading order -- table cells, columns, floats read in another order -- not missing words
  starts    of LibreOffice's pages, the share whose first words begin the same page in Parade's
  lines     the mean difference in lines per page, over the pages both have
  ink       the overlap of the dark pixels of the first pages, rendered small, two pixels of slack

and tagged by what the document uses (tables, pictures, charts, equations,
notes, columns, tracked changes, comments, text boxes, fields, CJK, RTL),
so that the report can say how each kind of document fares. Written:
OUTDIR/report.json (everything), OUTDIR/report.md (the summary, the
features, the worst documents). Every external program runs under an
address-space cap and a timeout.
"""
import argparse
import collections
import difflib
import json
import os
import re
import resource
import shutil
import statistics
import subprocess
import sys
import zipfile

MEM_MB = int(os.environ.get("LO_COMPARE_MEM_MB", "4096"))


def cap():
    resource.setrlimit(resource.RLIMIT_AS, (MEM_MB << 20, MEM_MB << 20))


def run(cmd, timeout=120, limit=True):
    try:
        return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout,
                              preexec_fn=cap if limit else None)
    except (subprocess.TimeoutExpired, OSError):
        return None


def pages_of(pdf):
    r = run(["pdfinfo", pdf], 60)
    if not r or r.returncode:
        return 0
    m = re.search(rb"^Pages:\s+(\d+)", r.stdout, re.M)
    return int(m.group(1)) if m else 0


def page_texts(pdf):
    """the text of each page (pdftotext puts a form feed between them)"""
    r = run(["pdftotext", "-enc", "UTF-8", pdf, "-"], 120)
    if not r or r.returncode:
        return []
    pages = r.stdout.decode("utf-8", "replace").split("\f")
    if pages and not pages[-1].strip():
        pages.pop()
    return pages


def words(s):
    return re.findall(r"\w+", s.lower())


def features(path):
    """what a document uses, from its parts"""
    f = set()
    try:
        z = zipfile.ZipFile(path)
        names = z.namelist()
        doc = z.read("word/document.xml").decode("utf-8", "replace")
    except Exception:
        return ["unreadable"]
    if "<w:tbl>" in doc or "<w:tbl " in doc:
        f.add("tables")
    if "<pic:pic" in doc or "<v:imagedata" in doc:
        f.add("pictures")
    if any(n.startswith("word/charts/") for n in names):
        f.add("charts")
    if "<m:oMath" in doc:
        f.add("equations")
    if "word/footnotes.xml" in names and "<w:footnoteReference" in doc or "<w:endnoteReference" in doc:
        f.add("notes")
    if re.search(r'<w:cols [^>]*w:num="[2-9]"', doc):
        f.add("columns")
    if "<w:ins " in doc or "<w:del " in doc:
        f.add("tracked changes")
    if "word/comments.xml" in names:
        f.add("comments")
    if "<w:txbxContent" in doc or "<wps:wsp" in doc:
        f.add("text boxes")
    if "<w:fldChar" in doc or "<w:fldSimple" in doc:
        f.add("fields")
    if "<w:sdt>" in doc:
        f.add("content controls")
    text = "".join(re.findall(r"<w:t[^>]*>([^<]*)</w:t>", doc))
    if re.search(r"[぀-ヿ㐀-鿿가-힯]", text):
        f.add("CJK")
    if re.search(r"[֐-ࣿ]", text):
        f.add("RTL")
    if not f:
        f.add("plain")
    return sorted(f)


def ink(png):
    from PIL import Image
    import numpy as np
    a = np.asarray(Image.open(png).convert("L"))
    return a < 160


def ink_overlap(pd_pdf, lo_pdf, work, dpi, npages):
    """the dark pixels of the first pages in common, over those in either; None when nothing renders"""
    import numpy as np
    both = either = 0
    for k in range(1, npages + 1):
        imgs = []
        for tag, pdf in (("p", pd_pdf), ("l", lo_pdf)):
            stem = os.path.join(work, "%s%d" % (tag, k))
            r = run(["pdftoppm", "-r", str(dpi), "-f", str(k), "-l", str(k), "-singlefile", "-png", pdf, stem], 120)
            if not r or r.returncode or not os.path.exists(stem + ".png"):
                return None
            imgs.append(ink(stem + ".png"))
        a, b = imgs
        h, w = min(a.shape[0], b.shape[0]), min(a.shape[1], b.shape[1])
        a, b = a[:h, :w], b[:h, :w]
        # two pixels of slack (a few points at this size): the same text a little apart is the same text
        a2, b2 = a.copy(), b.copy()
        for dy in range(-2, 3):
            for dx in range(-2, 3):
                a2 |= np.roll(np.roll(a, dy, 0), dx, 1)
                b2 |= np.roll(np.roll(b, dy, 0), dx, 1)
        both += int((a & b2).sum() + (b & a2).sum()) // 2
        either += int((a | b).sum())
    return both / either if either else None


def starts_same(pd_pages, lo_pages):
    """of LibreOffice's pages, the share whose first five words open the same page of Parade's"""
    hits = total = 0
    pd_first = [" ".join(words(p)[:5]) for p in pd_pages]
    for i, p in enumerate(lo_pages):
        w = words(p)[:5]
        if len(w) < 3:
            continue
        total += 1
        hits += i < len(pd_first) and pd_first[i] == " ".join(w)
    return hits / total if total else None


def line_diff(pd_pages, lo_pages):
    n = min(len(pd_pages), len(lo_pages))
    if not n:
        return None
    d = [abs(len([l for l in pd_pages[i].splitlines() if l.strip()]) -
             len([l for l in lo_pages[i].splitlines() if l.strip()])) for i in range(n)]
    return sum(d) / n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("list")
    ap.add_argument("out")
    ap.add_argument("--pd-conv", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build",
                                                       "pd_conv"))
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--dpi", type=int, default=30)
    ap.add_argument("--ink-pages", type=int, default=2)
    ap.add_argument("--batch", type=int, default=20)
    a = ap.parse_args()

    docs = [l.strip() for l in open(a.list, encoding="utf-8") if l.strip() and not l.startswith("#")]
    docs = docs[:a.limit] if a.limit else docs
    for d in ("in", "pd", "lo", "work"):
        os.makedirs(os.path.join(a.out, d), exist_ok=True)

    rows = []
    for i, src in enumerate(docs):
        name = "d%03d" % i
        dst = os.path.join(a.out, "in", name + ".docx")
        try:
            shutil.copyfile(src, dst)
        except OSError:
            continue
        rows.append({"name": name, "source": src, "features": features(dst)})

    # Parade's PDFs
    for r in rows:
        out = os.path.join(a.out, "pd", r["name"] + ".pdf")
        if not os.path.exists(out):
            run([a.pd_conv, os.path.join(a.out, "in", r["name"] + ".docx"), out], 120)

    # LibreOffice's, in batches: one start for many documents (its own profile, no memory cap: it maps a lot)
    profile = "file://" + os.path.abspath(os.path.join(a.out, "lo-profile"))
    todo = [r for r in rows if not os.path.exists(os.path.join(a.out, "lo", r["name"] + ".pdf"))]
    for k in range(0, len(todo), a.batch):
        chunk = [os.path.join(a.out, "in", r["name"] + ".docx") for r in todo[k:k + a.batch]]
        run(["soffice", "-env:UserInstallation=" + profile, "--headless", "--convert-to", "pdf", "--outdir",
             os.path.join(a.out, "lo")] + chunk, 60 * len(chunk), limit=False)
        print("  LibreOffice: %d of %d" % (min(k + a.batch, len(todo)), len(todo)), file=sys.stderr)

    for r in rows:
        pd_pdf = os.path.join(a.out, "pd", r["name"] + ".pdf")
        lo_pdf = os.path.join(a.out, "lo", r["name"] + ".pdf")
        r["pd_ok"] = os.path.exists(pd_pdf)
        r["lo_ok"] = os.path.exists(lo_pdf)
        if not (r["pd_ok"] and r["lo_ok"]):
            continue
        r["pages_pd"], r["pages_lo"] = pages_of(pd_pdf), pages_of(lo_pdf)
        pt, lt = page_texts(pd_pdf), page_texts(lo_pdf)
        wp, wl = words(" ".join(pt))[:30000], words(" ".join(lt))[:30000]
        r["words_pd"], r["words_lo"] = len(wp), len(wl)
        r["text"] = difflib.SequenceMatcher(None, wp, wl, autojunk=False).ratio() if wp or wl else 1.0
        cp, cl = collections.Counter(wp), collections.Counter(wl)
        r["bag"] = sum((cp & cl).values()) / max(len(wp), len(wl)) if wp or wl else 1.0
        r["starts"] = starts_same(pt, lt)
        r["lines"] = line_diff(pt, lt)
        work = os.path.join(a.out, "work", r["name"])
        os.makedirs(work, exist_ok=True)
        r["ink"] = ink_overlap(pd_pdf, lo_pdf, work, a.dpi, min(a.ink_pages, r["pages_pd"], r["pages_lo"]))
        shutil.rmtree(work, ignore_errors=True)

    json.dump(rows, open(os.path.join(a.out, "report.json"), "w"), indent=1)
    report(rows, os.path.join(a.out, "report.md"))


def stat(vals):
    vals = [v for v in vals if v is not None]
    return (statistics.median(vals), statistics.mean(vals), len(vals)) if vals else (None, None, 0)


def pct(v):
    return "-" if v is None else "%.0f%%" % (100 * v)


def report(rows, path):
    ok = [r for r in rows if r.get("pd_ok") and r.get("lo_ok")]
    out = []
    out.append("# Parade vs LibreOffice: %d documents\n" % len(rows))
    out.append("Compared: %d (Parade failed on %d, LibreOffice on %d).\n" % (
        len(ok), sum(not r.get("pd_ok") for r in rows), sum(not r.get("lo_ok") for r in rows)))
    same_pages = [r["pages_pd"] == r["pages_lo"] for r in ok]
    near_pages = [abs(r["pages_pd"] - r["pages_lo"]) <= max(1, r["pages_lo"] // 10) for r in ok]
    out.append("| measure | median | mean |\n|---|---|---|")
    for key, label in (("text", "words in the same order"), ("bag", "words in common, any order"), ("starts", "pages starting with the same words"),
                       ("ink", "ink overlap, first pages")):
        md, mn, n = stat(r.get(key) for r in ok)
        out.append("| %s | %s | %s |" % (label, pct(md), pct(mn)))
    md, mn, n = stat(r.get("lines") for r in ok)
    out.append("| lines per page, difference | %s | %s |" % ("-" if md is None else "%.1f" % md,
                                                             "-" if mn is None else "%.1f" % mn))
    if ok:
        out.append("| same page count | %s of documents | |" % pct(sum(same_pages) / len(ok)))
        out.append("| page count within 10%% (or 1) | %s of documents | |" % pct(sum(near_pages) / len(ok)))
    out.append("\n## By feature\n")
    out.append("| feature | documents | words | words, any order | page starts | ink | same pages |\n"
               "|---|---|---|---|---|---|---|")
    feats = sorted({f for r in ok for f in r["features"]})
    for f in feats:
        g = [r for r in ok if f in r["features"]]
        out.append("| %s | %d | %s | %s | %s | %s | %s |" % (
            f, len(g), pct(stat(r.get("text") for r in g)[0]), pct(stat(r.get("bag") for r in g)[0]),
            pct(stat(r.get("starts") for r in g)[0]),
            pct(stat(r.get("ink") for r in g)[0]), pct(sum(r["pages_pd"] == r["pages_lo"] for r in g) / len(g))))
    out.append("\n## Furthest apart (by words in any order, then in order, then page starts)\n")
    out.append("| doc | pages Parade / LO | words | any order | page starts | ink | features | source |\n"
               "|---|---|---|---|---|---|---|---|")
    for r in sorted(ok, key=lambda r: (r.get("bag") or 0, r.get("text") or 0, r.get("starts") or 0))[:25]:
        out.append("| %s | %d / %d | %s | %s | %s | %s | %s | %s |" % (
            r["name"], r["pages_pd"], r["pages_lo"], pct(r.get("text")), pct(r.get("bag")), pct(r.get("starts")),
            pct(r.get("ink")),
            ", ".join(r["features"]), os.path.basename(r["source"])))
    open(path, "w", encoding="utf-8").write("\n".join(out) + "\n")
    print("\n".join(out[:12]))


if __name__ == "__main__":
    main()
