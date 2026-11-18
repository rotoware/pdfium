"""One page of any PDF, gs tiffsep against plates_bench, per plate:
gs | pdfium | diff (orange off by up to 8, red more), and the stats.
usage: plates.py <pdf> <dir> [dpi]   writes <dir>/<ink>.png"""
import glob
import os
import shutil
import subprocess
import sys
import urllib.parse

import numpy as np
from PIL import Image

pdf, out = sys.argv[1], sys.argv[2]
dpi = sys.argv[3] if len(sys.argv) > 3 else "144"
BENCH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../out/native/plates_bench")
shutil.rmtree(out, ignore_errors=True)
os.makedirs(out)
subprocess.run(["gs", "-q", "-dBATCH", "-dNOPAUSE", "-dUseFastColor", "-dOverprint=/simulate", f"-r{dpi}",
                "-sDEVICE=tiffsep", f"-sOutputFile={out}/gs.tif", pdf], check=True)
inks = {urllib.parse.unquote(p[p.index("(") + 1:p.rindex(")")]): p for p in glob.glob(f"{out}/gs(*).tif")}
process = ["Cyan", "Magenta", "Yellow", "Black"]
names = [i for i in process if i in inks] + sorted(set(inks) - set(process))
subprocess.run([BENCH, pdf, dpi, "256", ",".join(names), f"--dump={out}/pd"], check=True, capture_output=True)
for ink in names:
    a = np.asarray(Image.open(inks[ink]).convert("L"), int)
    b = np.asarray(Image.open(f"{out}/pd-{ink}.pgm"), int)
    h, w = min(a.shape[0], b.shape[0]), min(a.shape[1], b.shape[1])
    a, b = a[:h, :w], b[:h, :w]
    d = np.abs(a - b)
    vis = np.stack([(b * 0.4 + 153).astype(np.uint8)] * 3, -1)
    vis[(d > 0) & (d <= 8)] = (255, 160, 0)
    vis[d > 8] = (230, 0, 0)
    gap = np.full((h, 6, 3), 255, np.uint8)
    Image.fromarray(np.concatenate([np.stack([a.astype(np.uint8)] * 3, -1), gap,
                                    np.stack([b.astype(np.uint8)] * 3, -1), gap, vis], 1)).save(f"{out}/{ink}.png")
    print(f"{ink:16} identical {(d == 0).mean() * 100:6.2f}%  off>8 {(d > 8).sum()}")
