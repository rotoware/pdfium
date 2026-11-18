"""usage: problems.py <suite>. Into <suite>/out/problems/, every cell whose merged composite differs: gs | pdfium | diff x4, 4x zoom, one PNG per cell"""
import json, sys, glob, os, shutil
import numpy as np
from PIL import Image
S = sys.argv[1]; K = 2; Z = 4
out = f"{S}/out/problems"; shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
for pdf in sorted(glob.glob(f"{S}/pdf/*.pdf")):
    t = os.path.basename(pdf)[:-4]
    m = json.load(open(pdf[:-4] + ".json")); o = f"{S}/out/{t}"
    a = np.asarray(Image.open(f"{o}/composite-gs.png"), int); b = np.asarray(Image.open(f"{o}/composite-pdfium.png"), int)
    for c in m["cells"]:
        tp, l = (m["H"] - c["y"] - 30) * K, c["x"] * K
        ga, pb = a[tp:tp + 60, l:l + 60], b[tp:tp + 60, l:l + 60]
        d = np.abs(ga - pb)
        if d[2:58, 2:58].max() <= 1:
            continue
        gap = np.full((60, 6, 3), 255)
        strip = np.concatenate([ga, gap, pb, gap, np.clip(d * 4, 0, 255)], 1).astype(np.uint8)
        im = Image.fromarray(strip).resize((strip.shape[1] * Z, 60 * Z), Image.NEAREST)
        name = f"{t} - {c['row']} - {c['col']} (max {d.max()})".replace("/", "∕")
        im.save(f"{out}/{name}.png")
print(len(os.listdir(out)))
