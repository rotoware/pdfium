"""usage: halo.py <suite>. 13-text-overprint, per renderer: paper pixels in each box (the base inks it
all over, so any is a gap round the text, but for white text), and pixels
changed on plates the text must leave (overprint)"""
import json, sys
import numpy as np
from PIL import Image
S = sys.argv[1]; t = "13-text-overprint"; K = 2
m = json.load(open(f"{S}/pdf/{t}.json"))
P = ["Cyan", "Magenta", "Yellow", "Black", "PANTONE 021 C", "PANTONE 354 C"]
BASE = [76, 76, 76, 76, 127, 0]
img = {side: np.stack([255 - np.asarray(Image.open(f"{S}/out/{t}/{i}-{p}-{side}.png"), int) for i, p in enumerate(P, 1)])
       for side in ("gs", "pdfium")}
PROC = {0, 1, 2, 3}
touch = {"OPM0 k K": PROC, "OPM1 k K": {3}, "OPM1 k C.5": {0}, "OPM1 k M Y red": {1, 2}, "OPM1 g": PROC,
         "OPM1 rgb red": PROC, "OP 354": {5}, "OP 021 same": {4}, "OP DeviceN": {4, 5}, "OP None": set(),
         "OP All .5": set(range(6)), "OPM1 K stroked .3pt": {3}, "OPM1 K fill+stroke .3pt": {3}}
rows = {}
for c in m["cells"]:
    top, left = (m["H"] - c["y"] - 30) * K + 1, c["x"] * K + 1
    for side, a in img.items():
        x = a[:, top:top + 58, left:left + 58]
        paper = int((x.max(0) == 0).sum())
        if c["row"] in touch:
            keep = [i for i in range(6) if i not in touch[c["row"]]]
            moved = int(sum((x[i] != BASE[i]).sum() for i in keep))
        else:
            moved = None
        rows.setdefault(c["row"], {}).setdefault(side, []).append((paper, moved))
print("per size 2..48pt: paper px (gap) / px changed on plates the text must leave")
for r, sides in rows.items():
    for side, v in sides.items():
        print(f"{r:26s} {side:7s} " + " ".join(f"{p}/{'-' if mv is None else mv}" for p, mv in v))
