"""Run every PDF in <suite>/pdf through gs (tiffsep, -dUseFastColor, as
switch separate renders) and pdfium --plate, 144dpi, no anti-aliasing; per
PDF write <suite>/out/<name>/: the PDF, both composites (the plates merged), per plate gs,
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
# the color each ink prints on white, full: the composites mix them
INK_RGB = {"Cyan": (0, 174, 239), "Magenta": (236, 0, 140), "Yellow": (255, 242, 0), "Black": (35, 31, 32),
           "PANTONE 021 C": (254, 80, 0), "PANTONE 354 C": (0, 177, 64)}
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
    shutil.copy(pdf, f"{out}/0-original-{test}.pdf")  # first in the folder
    gsargs = ["gs", "-q", "-dBATCH", "-dNOPAUSE", "-dUseFastColor", "-dOverprint=/simulate", f"-r{DPI}"]
    subprocess.run(gsargs + ["-sDEVICE=tiffsep", f"-sOutputFile={work}/gs.tif", pdf], check=True)
    # each program's own color render, for reference: they turn ink into RGB
    # their own ways, so they are not compared
    subprocess.run(gsargs + ["-sDEVICE=png16m", f"-sOutputFile={out}/render-gs.png", pdf], check=True)
    pdfium(pdf, f"{work}/color").save(f"{out}/render-pdfium.png")
    # what is compared: the cells, not the labels (fonts differ)
    inside = np.zeros((meta["H"] * K, meta["W"] * K), bool)
    for cell in meta["cells"]:
        top, left = (meta["H"] - cell["y"] - meta["cell"]) * K, cell["x"] * K
        inside[top:top + meta["cell"] * K, left:left + meta["cell"] * K] = True
    # the plates gs separated: the four process inks, then its spots
    plates = ["Cyan", "Magenta", "Yellow", "Black"]
    plates += sorted(urllib.parse.unquote(os.path.basename(p)[3:-5]) for p in glob.glob(f"{work}/gs(*).tif")
                     if urllib.parse.unquote(os.path.basename(p)[3:-5]) not in plates)
    print(f"== {test}: plates {plates}")
    inks = {"gs": [], "pdfium": []}
    for i, plate in enumerate(plates, 1):
        tif = [p for p in glob.glob(f"{work}/gs(*).tif") if urllib.parse.unquote(os.path.basename(p)[3:-5]) == plate][0]
        gs = Image.open(tif).convert("L")
        pd = pdfium(pdf, f"{work}/{i}", "--printing", f"--plate={plate}").convert("L")
        gs.save(f"{out}/{i}-{plate}-gs.png")
        pd.save(f"{out}/{i}-{plate}-pdfium.png")
        a, b = np.asarray(gs, int), np.asarray(pd, int)
        inks["gs"].append((plate, 255 - a))
        inks["pdfium"].append((plate, 255 - b))
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
    # the composites: each side's plates printed on paper, one ink over the
    # other, every ink filtering the light by its color (as on press, ideal
    # inks): where the plates match the composites do
    for side, planes in inks.items():
        rgb = np.ones(planes[0][1].shape + (3,))
        for plate, ink in planes:
            color = np.array(INK_RGB.get(plate, (128, 128, 128))) / 255
            rgb *= 1 - (ink[..., None] / 255) * (1 - color)
        Image.fromarray(np.round(rgb * 255).astype(np.uint8)).save(f"{out}/composite-{side}.png")
    # the composites: what each shows of the page in RGB
    ca = np.asarray(Image.open(f"{out}/composite-gs.png").convert("RGB"), int)
    cb = np.asarray(Image.open(f"{out}/composite-pdfium.png").convert("RGB"), int)
    h, w = min(ca.shape[0], cb.shape[0]), min(ca.shape[1], cb.shape[1])
    cd = np.abs(ca[:h, :w] - cb[:h, :w]).max(-1)
    Image.fromarray(np.clip(cd * 4, 0, 255).astype(np.uint8)).save(f"{out}/composite-diff.png")
    cd = np.where(inside[:h, :w], cd, 0)
    summary.append(f"| {test} | composite RGB | {100 * (cd[inside[:h, :w]] == 0).mean():.3f}% | {cd.max()} | | mean {cd[inside[:h, :w]].mean():.1f} |")
    open(f"{out}/README.txt", "w").write(
        "0-original-<test>.pdf the test PDF\n"
        "composite-gs.png      gs plates merged: each ink printed over the others\n"
        "composite-pdfium.png  pdfium plates merged the same way\n"
        "composite-diff.png    |gs - pdfium| of the composites, x4\n"
        "render-gs.png         gs own color render (png16m), reference only\n"
        "render-pdfium.png     pdfium own color render, reference only\n"
        "N-<ink>-gs.png        ghostscript tiffsep plate (black = full ink)\n"
        "N-<ink>-pdfium.png    pdfium --plate=<ink>\n"
        "N-<ink>-diff.png      pdfium plate, pixels off by 1 orange, by more red\n")
open(f"{suite}/out/SUMMARY.md", "w").write("\n".join(summary) + "\n")
