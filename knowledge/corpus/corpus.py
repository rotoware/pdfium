"""The public corpus (download.sh) separated by gs and pdfium, plate against
plate: what pdfium's ink render (plates_bench --ink) gets wrong on real
PDFs, not the made-up cells of separations/.

Per page: gs tiffsep (-dUseFastColor -dOverprint=/simulate -dUseCropBox),
plates_bench --ink with the plates it finds itself ("auto"), both at DPI,
no anti-aliasing. Writes knowledge/out/corpus/:
  results.json   per page: status, plates, per plate identical % and px off
  SUMMARY.md     counts by status, then the pages off, worst first
  <page>.png     gs | pdfium | diff of the worst plate, for pages off
Status: same (every plate identical), near (off by <= 8 or < 0.1% of the
page off more), edges (off only along edges: see solid()), off, group-cs
(off, and the PDF has a transparency group blending in RGB or gray: gs
blends there and converts back, pdfium has no group color space),
missing-plate (gs separates a spot pdfium does not find), extra-plate
(pdfium finds a spot gs does not, and paints it), size, pdfium-error,
pdfium-failed (a non-handle color reached the ink device), gs-bug (a
difference GS_BUGS explains: gs is wrong), gs-error,
timeout, huge (skipped), script-error.
A run of some sets or pages (--only) updates those in results.json, keeping
the rest.
usage: corpus.py [--dpi=72] [--jobs=N] [--pages=N] [--only=a,b] [set ...]
  set: altona pdf-differences pdf20examples pdfjs (default all);
  --pages: pages a PDF at most (altona: all its pages unless given);
  --only: just the pages whose name contains one of these"""
import re
import glob
import json
import multiprocessing
import os
import shutil
import subprocess
import sys
import urllib.parse

import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None  # huge pages are skipped by size, not refused by PIL

KNOWLEDGE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS = f"{KNOWLEDGE}/.cache/corpus"
OUT = f"{KNOWLEDGE}/out/corpus"
BENCH = f"{KNOWLEDGE}/../out/native/plates_bench"
# gs's default ICC profiles (gs/source.sh): RGB page groups converted as gs
PROFILES = f"{KNOWLEDGE}/.cache/gs/ghostscript-10.08.0/iccprofiles"
# gs's own share/ghostscript (fonts it substitutes): the gs on PATH
GSFONTS = os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(shutil.which("gs")))), "share/ghostscript")
PROCESS = ["Cyan", "Magenta", "Yellow", "Black"]
TIMEOUT = 120
args = [a for a in sys.argv[1:] if not a.startswith("--")]
opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
DPI = int(opts.get("dpi", 72))
SETS = args or ["altona", "pdf-differences", "pdf20examples", "pdfjs"]


# Pages gs gets wrong (checked against the file and other renderers): their
# differences are gs's, pdfium is not to follow (README.md, "gs wrong")
GS_BUGS = {
    "xobject-image": "an ASCIIHex+DCT image gs fails to decode, draws nothing",
    "freetext_no_appearance": "a FreeText with no appearance: gs prints its UTF-16 /Contents byte by byte (garbled)",
    "issue22487": "Brotli streams: gs has no /BrotliDecode",
    "type3_advance": "a show in rendering mode 3 leaves gs's text position where the show began (stringwidth's gsave / grestore): the next show draws there",
    "issue6006": "a stitching function's Encode out of its domain: gs errors, drops the pattern",
    "pattern_fill_pending_state": "gs ignores the W clip with a pattern fill, drops a tile color",
    "issue6621": "/Mask a 1-bit image without /ImageMask: gs barely draws the image",
    "issue19326": "a 16-bit 2-component JPX gs garbles",
    "issue11931": "a JPEG with R,G,B component ids gs decodes as YCbCr",
    "issue18548_reduced": "a Type 1 font with two Subrs: gs takes the empty one, draws nothing",
    "issue19634": "gs ignores a soft mask inside a Type 3 d1 glyph",
    "issue16742": "gs clips a soft mask to an axis-aligned BBox",
    "function_based_shading": "gs samples a discontinuous type 1 shading at its corners only",
    "tiling_patterns_variations": "gs drops later strokes after a pattern error",
    "bug2086012": "a PostScript function that underflows: gs drops the shading",
}
for jbig2 in ("bitmap-composite-and-xnor-refine", "bitmap-composite-or-xor-replace-refine", "bitmap-refine-customat-tpgron",
              "bitmap-refine-customat", "bitmap-refine-lossless", "bitmap-refine-refine", "bitmap-refine-template1-tpgron",
              "bitmap-refine-template1", "bitmap-refine-tpgron", "bitmap-refine", "bitmap-symbol-context-reuse",
              "bitmap-symbol-symhuffrefine-textrefine", "bitmap-symbol-symhuffrefineseveral",
              "bitmap-symbol-texthuffrefineB15", "bitmap-symbol-texthuffrefine", "bitmap-symbol-texthuffrefinecustom",
              "bitmap-symbol-texthuffrefinecustomdims", "bitmap-symbol-texthuffrefinecustompos",
              "bitmap-symbol-texthuffrefinecustomposdims", "bitmap-symbol-texthuffrefinecustomsize",
              "bitmap-trailing-7fff-stripped-harder-refine", "issue20439", "bitmap-halftone-composite"):
    GS_BUGS[jbig2] = "JBIG2 jbig2dec cannot decode (refinement / intermediate region): gs draws nothing"


def gs_bug(name):
    """the gs bug that explains page `name`, if one does"""
    base = name.rsplit("__p", 1)[0].split("__")[-1]
    return GS_BUGS.get(base)


def page_count(pdf):
    r = subprocess.run(["qpdf", "--show-npages", pdf], capture_output=True, text=True, errors="replace", timeout=60)
    try:
        return int(r.stdout.strip())
    except ValueError:
        return 1


def jobs():
    """(name, pdf, page) to compare"""
    out = []
    for s in SETS:
        pdfs = sorted(glob.glob(f"{CORPUS}/{s}/**/*.pdf", recursive=True))
        for pdf in pdfs:
            if "documentation" in pdf:
                continue
            limit = int(opts.get("pages", 0 if s == "altona" else 1))
            n = page_count(pdf) if limit != 1 else 1
            limit = limit or n
            name = os.path.relpath(pdf, CORPUS)[:-4].replace("/", "__").replace(" ", "_")
            out += [(f"{name}__p{p + 1}", pdf, p) for p in range(min(n, limit))]
    only = [o for o in opts.get("only", "").split(",") if o]
    return [j for j in out if not only or any(o in j[0] for o in only)]


def group_cs(pdf, page=0):
    """whether a transparency group of `page` of `pdf` blends in RGB or gray
    (or a profile of them): a dictionary with /S /Transparency and such a /CS
    among the objects the page reaches"""
    q = subprocess.run(["qpdf", "--qdf", "--object-streams=disable", "--empty", "--pages", pdf, str(page + 1), "--",
                        "-"], capture_output=True, timeout=60).stdout
    objs = {m.group(1): m.group(2).split(b"\nstream\n")[0]
            for m in re.finditer(rb"\n(\d+) 0 obj\n(.*?)\nendobj", q, re.S)}
    for body in objs.values():
        for m in re.finditer(rb"<<((?:(?!<<|>>).|<<(?:(?!<<|>>).)*>>)*?/S /Transparency(?:(?!<<|>>).|<<(?:(?!<<|>>).)*>>)*?)>>", body, re.S):
            cs = re.search(rb"/CS\s*(/\w+|\[\s*/ICCBased\s*(\d+) 0 R\s*\]|(\d+) 0 R)", m.group(0))
            if not cs:
                continue
            if cs.group(1) in (b"/DeviceRGB", b"/DeviceGray", b"/CalRGB", b"/CalGray"):
                return True
            ref = cs.group(2) or cs.group(3)
            target = objs.get(ref, b"")
            icc = re.search(rb"/ICCBased\s*(\d+) 0 R", target)
            if icc:
                target = objs.get(icc.group(1), b"")
            if re.search(rb"/N [13]\b", target) or re.search(rb"/Cal(RGB|Gray)", target):
                return True
    return False


def ink_of(path):
    return urllib.parse.unquote(path[path.index("(") + 1:path.rindex(")")])


def run(job):
    try:
        return compare(job)
    except Exception as e:  # noqa: BLE001 - one page's failure is a result, not the run's
        return {"name": job[0], "pdf": os.path.relpath(job[1], CORPUS), "page": job[2] + 1, "status": "script-error",
                "detail": repr(e)[:300]}


def compare(job):
    name, pdf, page = job
    work = f"{OUT}/work/{name}"
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    result = {"name": name, "pdf": os.path.relpath(pdf, CORPUS), "page": page + 1}
    try:
        g = subprocess.run(["gs", "-q", "-dBATCH", "-dNOPAUSE", "-dSAFER", "-dUseFastColor", "-dOverprint=/simulate", "-dNONATIVEFONTMAP",
                            "-dUseCropBox", f"-r{DPI}", f"-dFirstPage={page + 1}", f"-dLastPage={page + 1}",
                            "-sDEVICE=tiffsep", f"-sOutputFile={work}/gs.tif", pdf],
                           capture_output=True, text=True, errors="replace", timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return {**result, "status": "timeout", "detail": "gs"}
    gs = {ink_of(p): p for p in glob.glob(f"{work}/gs(*).tif")}
    if g.returncode or not gs:
        return {**result, "status": "gs-error", "detail": (g.stderr or g.stdout)[-300:]}
    w, h = Image.open(next(iter(gs.values()))).size
    if w * h > 30_000_000:
        return {**result, "status": "huge", "detail": f"{w}x{h}"}
    try:
        b = subprocess.run([BENCH, pdf, str(DPI), "256", "auto", f"--dump={work}/pd", "--ink", f"--page={page}", f"--profiles={PROFILES}", f"--gsfonts={GSFONTS}"],
                           capture_output=True, text=True, errors="replace", timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return {**result, "status": "timeout", "detail": "pdfium"}
    if b.returncode not in (0, 2):
        return {**result, "status": "pdfium-error", "detail": f"exit {b.returncode}: {b.stderr[-300:]}"}
    found = b.stdout.split("\n")[:-1]
    result["gs_plates"], result["pd_plates"] = sorted(gs), found
    plates = {}
    for ink in sorted(set(gs) | set(found)):
        a = np.asarray(Image.open(gs[ink]).convert("L"), int) if ink in gs else None
        p = np.asarray(Image.open(f"{work}/pd-{ink}.pgm"), int) if ink in found else None
        if a is not None and p is not None and a.shape != p.shape:
            return {**result, "status": "size", "detail": f"gs {a.shape} pdfium {p.shape}"}
        if a is None:
            a = np.full(p.shape, 255)
        if p is None:
            p = np.full(a.shape, 255)
        d = np.abs(a - p)
        plates[ink] = {"identical": float((d == 0).mean() * 100), "off8": int((d > 8).sum()),
                       "area8": int(solid(d > 8).sum()), "max": int(d.max()),
                       "gs_ink": int((a < 255).sum()), "pd_ink": int((p < 255).sum())}
    result["plates"] = plates
    area = next(iter(np.asarray(Image.open(f"{work}/pd-Cyan.pgm")).shape for _ in [0]))
    result["area"] = area[0] * area[1]
    missing = [i for i in gs if i not in found and plates[i]["gs_ink"]]
    extra = [i for i in found if i not in gs and plates[i]["pd_ink"]]
    worst = max(plates, key=lambda i: (plates[i]["area8"], plates[i]["off8"], -plates[i]["identical"]))
    result["worst"] = worst
    off = plates[worst]["off8"]
    if b.returncode == 2:
        result["status"] = "pdfium-failed"
    elif missing:
        result["status"], result["detail"] = "missing-plate", ", ".join(missing)
    elif extra:
        result["status"], result["detail"] = "extra-plate", ", ".join(extra)
    elif all(v["identical"] == 100 for v in plates.values()):
        result["status"] = "same"
    elif off < 0.001 * result["area"]:
        result["status"] = "near"
    elif plates[worst]["area8"] < 0.0005 * result["area"]:
        result["status"] = "edges"
    else:
        result["status"] = "group-cs" if group_cs(pdf, page) else "off"
    if result["status"] in ("off", "edges", "group-cs") and gs_bug(name):
        result["status"], result["detail"] = "gs-bug", gs_bug(name)
    if result["status"] not in ("same", "near", "edges", "gs-bug"):
        sheet(work, worst, gs.get(worst), worst in found, f"{OUT}/{name}.png")
    shutil.rmtree(work, ignore_errors=True)
    return result


def solid(mask):
    """`mask` but its pixels within a 1 px line of its edge: what is off over
    an area, not along the edges of glyphs and strokes (hinting, dropout,
    rounding of where an edge falls)"""
    m = mask.copy()
    for _ in range(2):
        e = m.copy()
        e[1:] &= m[:-1]
        e[:-1] &= m[1:]
        e[:, 1:] &= m[:, :-1]
        e[:, :-1] &= m[:, 1:]
        m = e
    return m


def sheet(work, ink, gs_path, has_pd, path):
    """gs | pdfium | diff (orange <= 8, red more) of `ink`"""
    p = np.asarray(Image.open(f"{work}/pd-{ink}.pgm"), int) if has_pd else None
    a = np.asarray(Image.open(gs_path).convert("L"), int) if gs_path else None
    if a is None:
        a = np.full(p.shape, 255)
    if p is None:
        p = np.full(a.shape, 255)
    d = np.abs(a - p)
    vis = np.stack([(p * 0.4 + 153).astype(np.uint8)] * 3, -1)
    vis[(d > 0) & (d <= 8)] = (255, 160, 0)
    vis[d > 8] = (230, 0, 0)
    h, w = a.shape
    im = Image.new("RGB", (w * 3 + 20, h), "white")
    im.paste(Image.fromarray(a.astype(np.uint8)), (0, 0))
    im.paste(Image.fromarray(p.astype(np.uint8)), (w + 10, 0))
    im.paste(Image.fromarray(vis), (2 * w + 20, 0))
    im.save(path)


def summary(results):
    order = ["same", "near", "edges", "gs-bug", "off", "group-cs", "missing-plate", "extra-plate", "size", "pdfium-failed", "pdfium-error",
             "gs-error", "timeout", "huge", "script-error"]
    lines = [f"# Corpus: gs tiffsep vs pdfium ink render, {DPI} dpi", "",
             "| status | pages |", "|---|---|"]
    for s in order:
        lines.append(f"| {s} | {sum(r['status'] == s for r in results)} |")
    lines += ["", "edges: off only along edges (glyph hinting, stroke and edge rounding); off: over an area "
                  "(solid: off > 8 more than 2 px in from any edge of what is off)", "",
              "| page | status | worst plate | identical | px off >8 | % page off solid | detail |",
              "|---|---|---|---|---|---|---|"]
    bad = [r for r in results if r["status"] not in ("same", "near", "edges", "gs-bug")]
    bad.sort(key=lambda r: (order.index(r["status"]),
                            -(r["plates"][r["worst"]]["area8"] / r["area"]) if "worst" in r else 0))
    for r in bad:
        w = r.get("worst")
        pl = r["plates"][w] if w else None
        lines.append(f"| {r['name']} | {r['status']} | {w or ''} | {pl['identical']:.2f} | {pl['off8']} | "
                     f"{pl['area8'] / r['area'] * 100:.2f} | {r.get('detail', '')[:120].replace('|', '/').replace(chr(10), ' ')} |"
                     if pl else f"| {r['name']} | {r['status']} | | | | | "
                                f"{r.get('detail', '')[:120].replace('|', '/').replace(chr(10), ' ')} |")
    open(f"{OUT}/SUMMARY.md", "w").write("\n".join(lines) + "\n")
    print("\n".join(lines[:16]))


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    todo = jobs()
    for name, _, _ in todo:
        if os.path.exists(f"{OUT}/{name}.png"):
            os.remove(f"{OUT}/{name}.png")
    print(f"{len(todo)} pages")
    results = []
    with multiprocessing.Pool(int(opts.get("jobs", os.cpu_count()))) as pool:
        for i, r in enumerate(pool.imap_unordered(run, todo)):
            results.append(r)
            if r["status"] not in ("same", "near", "edges"):
                print(f"[{i + 1}/{len(todo)}] {r['name']}: {r['status']} {r.get('detail', '')[:80]}", flush=True)
    redone = {r["name"] for r in results}
    previous = json.load(open(f"{OUT}/results.json")) if os.path.exists(f"{OUT}/results.json") else []
    results += [r for r in previous if r["name"] not in redone]
    results.sort(key=lambda r: r["name"])
    json.dump(results, open(f"{OUT}/results.json", "w"), indent=1)
    if previous:
        json.dump(previous, open(f"{OUT}/results.prev.json", "w"), indent=1)
    summary(results)
    # what this run changed: pages that moved status, worse first
    rank = ["same", "near", "edges", "off"]
    before = {r["name"]: r["status"] for r in previous}
    moved = [(r["name"], before[r["name"]], r["status"]) for r in results
             if r["name"] in before and before[r["name"]] != r["status"]]
    moved.sort(key=lambda m: (rank.index(m[2]) if m[2] in rank else 9) - (rank.index(m[1]) if m[1] in rank else 9),
               reverse=True)
    for name, a, b in moved:
        print(f"{a} -> {b}: {name}")
