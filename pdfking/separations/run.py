"""Run every PDF in <suite>/pdf through gs (tiffsep, -dUseFastColor, as
switch separate renders) and pdfium --plate, 144dpi, no anti-aliasing; per
PDF write <suite>/out/<name>/: the PDF, both composites, per plate gs,
pdfium and a diff (orange off by 1, red more), and a SUMMARY.md over all.
usage: run.py <suite> [name...]"""
import glob
import json
import os
import shutil
import subprocess
import sys
import urllib.parse

import numpy as np
from PIL import Image

PDFIUM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../out/native/pdfium_test")
DPI, K = 144, 2
suite = sys.argv[1]
only = sys.argv[2:]


def flat(path):
    """pdfium's render on paper: what nothing painted is left transparent"""
    im = Image.open(path).convert("RGBA")
    return Image.alpha_composite(Image.new("RGBA", im.size, (255, 255, 255, 255)), im).convert("RGB")


def pdfium(pdf, folder, *args):
    os.makedirs(folder, exist_ok=True)
    shutil.copy(pdf, f"{folder}/page.pdf")
    subprocess.run([PDFIUM, *args, f"--scale={K}", "--no-smoothtext", "--no-smoothimage", "--no-smoothpath",
                    "--png", "page.pdf"], cwd=folder, check=True, capture_output=True)
    return flat(f"{folder}/page.pdf.0.png")


summary = ["| test | plate | identical px | max diff | cells off | worst cells |", "|---|---|---|---|---|---|"]
for pdf in sorted(glob.glob(f"{suite}/pdf/*.pdf")):
    test = os.path.basename(pdf)[:-4]
    if only and test not in only:
        continue
    meta = json.load(open(pdf[:-4] + ".json"))
    out, work = f"{suite}/out/{test}", f"{suite}/work/{test}"
    shutil.rmtree(out, ignore_errors=True)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(out)
    os.makedirs(work)
    shutil.copy(pdf, out)
    gsargs = ["gs", "-q", "-dBATCH", "-dNOPAUSE", "-dUseFastColor", "-dOverprint=/simulate", f"-r{DPI}"]
    subprocess.run(gsargs + ["-sDEVICE=tiffsep", f"-sOutputFile={work}/gs.tif", pdf], check=True)
    subprocess.run(gsargs + ["-sDEVICE=png16m", f"-sOutputFile={out}/composite-gs.png", pdf], check=True)
    pdfium(pdf, f"{work}/color").save(f"{out}/composite-pdfium.png")
    # what is compared: the cells, not the labels (fonts differ)
    inside = np.zeros((meta["H"] * K, meta["W"] * K), bool)
    for cell in meta["cells"]:
        top, left = (meta["H"] - cell["y"] - meta["cell"]) * K, cell["x"] * K
        inside[top:top + meta["cell"] * K, left:left + meta["cell"] * K] = True
    # the composites: what each shows of the page in RGB
    ca = np.asarray(Image.open(f"{out}/composite-gs.png").convert("RGB"), int)
    cb = np.asarray(Image.open(f"{out}/composite-pdfium.png").convert("RGB"), int)
    h, w = min(ca.shape[0], cb.shape[0]), min(ca.shape[1], cb.shape[1])
    cd = np.abs(ca[:h, :w] - cb[:h, :w]).max(-1)
    Image.fromarray(np.clip(cd * 4, 0, 255).astype(np.uint8)).save(f"{out}/composite-diff.png")
    cd = np.where(inside[:h, :w], cd, 0)
    summary.append(f"| {test} | composite RGB | {100 * (cd[inside[:h, :w]] == 0).mean():.3f}% | {cd.max()} | | mean {cd[inside[:h, :w]].mean():.1f} |")
    # the plates gs separated: the four process inks, then its spots
    plates = ["Cyan", "Magenta", "Yellow", "Black"]
    plates += sorted(urllib.parse.unquote(os.path.basename(p)[3:-5]) for p in glob.glob(f"{work}/gs(*).tif")
                     if urllib.parse.unquote(os.path.basename(p)[3:-5]) not in plates)
    print(f"== {test}: plates {plates}")
    for i, plate in enumerate(plates, 1):
        tif = [p for p in glob.glob(f"{work}/gs(*).tif") if urllib.parse.unquote(os.path.basename(p)[3:-5]) == plate][0]
        gs = Image.open(tif).convert("L")
        pd = pdfium(pdf, f"{work}/{i}", "--printing", f"--plate={plate}").convert("L")
        gs.save(f"{out}/{i}-{plate}-gs.png")
        pd.save(f"{out}/{i}-{plate}-pdfium.png")
        a, b = np.asarray(gs, int), np.asarray(pd, int)
        h, w = min(a.shape[0], b.shape[0]), min(a.shape[1], b.shape[1])
        d = np.where(inside[:h, :w], (255 - a[:h, :w]) - (255 - b[:h, :w]), 0)  # ink, gs - pdfium, in cells
        vis = np.stack([b[:h, :w]] * 3, -1).astype(np.uint8)
        vis[np.abs(d) == 1] = (255, 160, 0)
        vis[np.abs(d) > 1] = (255, 0, 0)
        Image.fromarray(vis).save(f"{out}/{i}-{plate}-diff.png")
        # per cell, inset 2px: where the edges are left out
        off = []
        for cell in meta["cells"]:
            top, left = (meta["H"] - cell["y"] - meta["cell"]) * K, cell["x"] * K
            c = d[top + 2:top + meta["cell"] * K - 2, left + 2:left + meta["cell"] * K - 2]
            m = int(np.abs(c).max())
            if m:
                ga = 255 - a[top + 2:top + meta["cell"] * K - 2, left + 2:left + meta["cell"] * K - 2]
                off.append((m, f"{cell['row']} / {cell['col']} (gs {np.unique(ga)[:4].tolist()}, d {m})"))
        off.sort(reverse=True)
        line = (f"| {test} | {plate} | {100 * (d[inside[:h, :w]] == 0).mean():.3f}% | {np.abs(d).max()} | {len(off)}/{len(meta['cells'])} | "
                + "; ".join(o for _, o in off[:3]) + " |")
        summary.append(line)
        print("  ", line)
    open(f"{out}/README.txt", "w").write(
        "composite-gs.png      ghostscript color render (png16m, -dUseFastColor, 144dpi)\n"
        "composite-pdfium.png  pdfium color render (144dpi)\n"
        "composite-diff.png    |gs - pdfium| of the composites, x4\n"
        "N-<ink>-gs.png        ghostscript tiffsep plate (black = full ink)\n"
        "N-<ink>-pdfium.png    pdfium --plate=<ink>\n"
        "N-<ink>-diff.png      pdfium plate, pixels off by 1 orange, by more red\n")
open(f"{suite}/out/SUMMARY.md", "w").write("\n".join(summary) + "\n")
