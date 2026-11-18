"""A page rendered in bands is the page rendered whole: every PDF in
<suite>/pdf, every plate it has (plates_bench auto: CMYK and its spots), in
one render (--ink) in bands of 7 rows against one band of the page.
usage: bands.py <suite>"""
import glob
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

BENCH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../out/native/plates_bench")
work = tempfile.mkdtemp()
bad = n = 0
for pdf in sorted(glob.glob(f"{sys.argv[1]}/pdf/*.pdf")):
    test = os.path.basename(pdf)[:-4]
    inks = subprocess.run([BENCH, pdf, "144", "7", "auto", "--ink", f"--dump={work}/bands"], check=True,
                          capture_output=True, text=True).stdout.split("\n")[:-1]
    subprocess.run([BENCH, pdf, "144", "100000", "auto", "--ink", f"--dump={work}/whole"], check=True,
                   capture_output=True)
    for ink in inks:
        a = np.asarray(Image.open(f"{work}/bands-{ink}.pgm"))
        b = np.asarray(Image.open(f"{work}/whole-{ink}.pgm"))
        n += 1
        if a.shape != b.shape or (a != b).any():
            bad += 1
            print(f"{test} {ink}: {(a != b).sum()} px differ")
print(f"{n} plates, {bad} differ banded")
