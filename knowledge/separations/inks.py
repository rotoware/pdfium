"""usage: inks.py <suite> [test...]. Per cell: the ink combinations (all plates at a pixel) each renderer makes;
a knockout/overprint difference is a combination only one has, an edge
difference only moves pixels between combinations both have"""
import json, sys, glob, os
import numpy as np
from PIL import Image
S = sys.argv[1]; only = sys.argv[2:]; K = 2
for pdf in sorted(glob.glob(f"{S}/pdf/*.pdf")):
    t = os.path.basename(pdf)[:-4]
    if only and t not in only:
        continue
    m = json.load(open(pdf[:-4] + ".json")); o = f"{S}/out/{t}"
    plates = sorted(glob.glob(f"{o}/[0-9]-*-gs.png"))
    side = {s: np.stack([255 - np.asarray(Image.open(p if s == "gs" else p[:-7] + "-pdfium.png"), np.int16) for p in plates], -1)
            for s in ("gs", "pdfium")}
    bad = []
    for c in m["cells"]:
        top, left = (m["H"] - c["y"] - 30) * K + 2, c["x"] * K + 2
        sets = {}
        for s, a in side.items():
            px = a[top:top + 56, left:left + 56].reshape(-1, a.shape[-1])
            u, n = np.unique(px, axis=0, return_counts=True)
            sets[s] = {tuple(v): k for v, k in zip(u.tolist(), n)}
        only_gs = {v: k for v, k in sets["gs"].items() if v not in sets["pdfium"]}
        only_pd = {v: k for v, k in sets["pdfium"].items() if v not in sets["gs"]}
        if only_gs or only_pd:
            bad.append(f"{c['row']} / {c['col']}: gs-only {list(only_gs.items())[:2]} pdfium-only {list(only_pd.items())[:2]}")
    print(f"== {t}: {len(bad)}/{len(m['cells'])} cells with an ink combination only one renderer makes")
    for b in bad[:6]:
        print("   ", b)
    if len(bad) > 6:
        print(f"    ... +{len(bad) - 6}")
