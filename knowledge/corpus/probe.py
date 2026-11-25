"""One page of a PDF separated by gs and pdfium (as corpus.py does), and per
plate the (gs, pdfium) values most often off: what a conversion gets wrong
shows as one pair over a large area. Plates are left in --out for a look.
usage: probe.py <pdf> [page (0-based)] [dpi] [--out=dir]"""
import collections
import glob
import os
import shutil
import subprocess
import sys
import urllib.parse

import numpy as np
from PIL import Image
Image.MAX_IMAGE_PIXELS = None
args = [a for a in sys.argv[1:] if not a.startswith("--out=")]
pdf = args[0]
page = int(args[1]) if len(args) > 1 else 0
dpi = args[2] if len(args) > 2 else "72"
w = next((a[6:] for a in sys.argv[1:] if a.startswith("--out=")), "/tmp/probe")
shutil.rmtree(w, ignore_errors=True)
os.makedirs(w)
subprocess.run(["gs", "-q", "-dBATCH", "-dNOPAUSE", "-dSAFER", "-dUseFastColor", "-dOverprint=/simulate", "-dNONATIVEFONTMAP", "-dUseCropBox",
                f"-r{dpi}", f"-dFirstPage={page+1}", f"-dLastPage={page+1}", "-sDEVICE=tiffsep", f"-sOutputFile={w}/gs.tif", pdf])
found = subprocess.run([os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../out/native/plates_bench"), pdf, dpi, "256", "auto", f"--dump={w}/pd", "--ink", f"--page={page}",
                        "--profiles=" + os.path.join(os.path.dirname(os.path.abspath(__file__)), "../.cache/gs/ghostscript-10.08.0/iccprofiles"),
                        "--gsfonts=" + os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(shutil.which("gs")))), "share/ghostscript")],
                       capture_output=True, text=True).stdout.splitlines()
for p in sorted(glob.glob(f"{w}/gs(*).tif")):
    ink = urllib.parse.unquote(p[p.index("(")+1:p.rindex(")")])
    a = np.asarray(Image.open(p).convert("L"), int)
    if ink not in found: print(ink, "not in pdfium"); continue
    b = np.asarray(Image.open(f"{w}/pd-{ink}.pgm"), int)
    d = a != b
    c = collections.Counter(zip(a[d].tolist(), b[d].tolist()))
    print(f"{ink:10} same {100 - d.mean()*100:6.2f}%  top (gs,pd): {c.most_common(8)}")
