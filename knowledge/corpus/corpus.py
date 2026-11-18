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
page off more), off, missing-plate (gs separates a spot pdfium does not
find), extra-plate (pdfium finds a spot gs does not, and paints it), size,
pdfium-error, pdfium-failed (a non-handle color reached the ink device),
gs-error, timeout.
usage: corpus.py [--dpi=72] [--jobs=N] [--pages=N] [set ...]
  set: altona pdf-differences pdf20examples pdfjs (default all);
  --pages: pages a PDF at most (altona: all its pages unless given)"""
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

KNOWLEDGE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS = f"{KNOWLEDGE}/.cache/corpus"
OUT = f"{KNOWLEDGE}/out/corpus"
BENCH = f"{KNOWLEDGE}/../out/native/plates_bench"
PROCESS = ["Cyan", "Magenta", "Yellow", "Black"]
TIMEOUT = 120
args = [a for a in sys.argv[1:] if not a.startswith("--")]
opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
DPI = int(opts.get("dpi", 72))
SETS = args or ["altona", "pdf-differences", "pdf20examples", "pdfjs"]


def page_count(pdf):
    r = subprocess.run(["qpdf", "--show-npages", pdf], capture_output=True, text=True, timeout=60)
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
            n = page_count(pdf)
            limit = int(opts.get("pages", n if s == "altona" else 1))
            name = os.path.relpath(pdf, CORPUS)[:-4].replace("/", "__").replace(" ", "_")
            out += [(f"{name}__p{p + 1}", pdf, p) for p in range(min(n, limit))]
    return out


def ink_of(path):
    return urllib.parse.unquote(path[path.index("(") + 1:path.rindex(")")])


def run(job):
    name, pdf, page = job
    work = f"{OUT}/work/{name}"
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    result = {"name": name, "pdf": os.path.relpath(pdf, CORPUS), "page": page + 1}
    try:
        g = subprocess.run(["gs", "-q", "-dBATCH", "-dNOPAUSE", "-dSAFER", "-dUseFastColor", "-dOverprint=/simulate",
                            "-dUseCropBox", f"-r{DPI}", f"-dFirstPage={page + 1}", f"-dLastPage={page + 1}",
                            "-sDEVICE=tiffsep", f"-sOutputFile={work}/gs.tif", pdf],
                           capture_output=True, text=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return {**result, "status": "timeout", "detail": "gs"}
    gs = {ink_of(p): p for p in glob.glob(f"{work}/gs(*).tif")}
    if g.returncode or not gs:
        return {**result, "status": "gs-error", "detail": (g.stderr or g.stdout)[-300:]}
    try:
        b = subprocess.run([BENCH, pdf, str(DPI), "256", "auto", f"--dump={work}/pd", "--ink", f"--page={page}"],
                           capture_output=True, text=True, timeout=TIMEOUT)
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
        plates[ink] = {"identical": float((d == 0).mean() * 100), "off8": int((d > 8).sum()), "max": int(d.max()),
                       "gs_ink": int((a < 255).sum()), "pd_ink": int((p < 255).sum())}
    result["plates"] = plates
    area = next(iter(np.asarray(Image.open(f"{work}/pd-Cyan.pgm")).shape for _ in [0]))
    result["area"] = area[0] * area[1]
    missing = [i for i in gs if i not in found and plates[i]["gs_ink"]]
    extra = [i for i in found if i not in gs and plates[i]["pd_ink"]]
    worst = max(plates, key=lambda i: (plates[i]["off8"], -plates[i]["identical"]))
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
    else:
        result["status"] = "off"
    if result["status"] not in ("same", "near"):
        sheet(work, worst, gs.get(worst), worst in found, f"{OUT}/{name}.png")
    shutil.rmtree(work, ignore_errors=True)
    return result


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
    order = ["same", "near", "off", "missing-plate", "extra-plate", "size", "pdfium-failed", "pdfium-error",
             "gs-error", "timeout"]
    lines = [f"# Corpus: gs tiffsep vs pdfium ink render, {DPI} dpi", "",
             "| status | pages |", "|---|---|"]
    for s in order:
        lines.append(f"| {s} | {sum(r['status'] == s for r in results)} |")
    lines += ["", "| page | status | worst plate | identical | px off >8 | % page off | detail |",
              "|---|---|---|---|---|---|---|"]
    bad = [r for r in results if r["status"] not in ("same", "near")]
    bad.sort(key=lambda r: (order.index(r["status"]),
                            -(r["plates"][r["worst"]]["off8"] / r["area"]) if "worst" in r else 0))
    for r in bad:
        w = r.get("worst")
        pl = r["plates"][w] if w else None
        lines.append(f"| {r['name']} | {r['status']} | {w or ''} | {pl['identical']:.2f} | {pl['off8']} | "
                     f"{pl['off8'] / r['area'] * 100:.2f} | {r.get('detail', '')[:120].replace('|', '/').replace(chr(10), ' ')} |"
                     if pl else f"| {r['name']} | {r['status']} | | | | | "
                                f"{r.get('detail', '')[:120].replace('|', '/').replace(chr(10), ' ')} |")
    open(f"{OUT}/SUMMARY.md", "w").write("\n".join(lines) + "\n")
    print("\n".join(lines[:16]))


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for f in glob.glob(f"{OUT}/*.png"):
        os.remove(f)
    todo = jobs()
    print(f"{len(todo)} pages")
    results = []
    with multiprocessing.Pool(int(opts.get("jobs", os.cpu_count()))) as pool:
        for i, r in enumerate(pool.imap_unordered(run, todo)):
            results.append(r)
            if r["status"] not in ("same", "near"):
                print(f"[{i + 1}/{len(todo)}] {r['name']}: {r['status']} {r.get('detail', '')[:80]}", flush=True)
    results.sort(key=lambda r: r["name"])
    json.dump(results, open(f"{OUT}/results.json", "w"), indent=1)
    summary(results)
