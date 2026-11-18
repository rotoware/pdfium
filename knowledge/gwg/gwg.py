"""GWG (Ghent Workgroup) PDF Output Suite 5.0: pdfium's plates against
Ghostscript's and GWG's own reference, for checking by eye.

Downloads the suite once (knowledge/.cache/gwg), renders the six test pages
(ALL_X4) as plates with gs tiffsep and plates_bench --ink (all plates in one render), and writes to
knowledge/out/gwg/:
  review.pdf       summary table, then per page the composite and every
                   plate: GWG reference | gs | pdfium | diff (gs vs pdfium)
  blend-modes.pdf  16.0, 16.1, 16.2, 16.4 composites: GWG | gs | pdfium
usage: gwg.py   (after knowledge/bench/build.sh)"""
import glob
import os
import shutil
import subprocess
import urllib.parse
import urllib.request
import zipfile

import numpy as np
from PIL import Image, ImageDraw, ImageFont

KNOWLEDGE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = f"{KNOWLEDGE}/.cache/gwg"
OUT = f"{KNOWLEDGE}/out/gwg"
WORK = f"{OUT}/work"
BENCH = f"{KNOWLEDGE}/../out/native/plates_bench"
URLS = ["https://gwg.org/wp-content/uploads/2026/05/Ghent_PDF_Output_Suite_V50_Testpages.zip",
        "https://gwg.org/wp-content/uploads/2026/05/Ghent_PDF_Output_Suite_V50_Patches-2.zip"]
DPI = 144
PROCESS = ["Cyan", "Magenta", "Yellow", "Black"]
# the color each ink prints, full, for the composites: both sides alike
INK_RGB = {"Cyan": (0, 174, 239), "Magenta": (236, 0, 140), "Yellow": (255, 242, 0), "Black": (35, 31, 32),
           "GWG Green": (0, 155, 80), "PANTONE 265 C": (144, 99, 205)}
FONT = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 28)
BOLD = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Bold.ttf", 40)
MONO = ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", 26)


def fetch():
    """the suite unzipped in CACHE, downloaded once"""
    os.makedirs(CACHE, exist_ok=True)
    for url in URLS:
        zipped = f"{CACHE}/{os.path.basename(url)}"
        if not os.path.exists(zipped):
            print("downloading", url)
            urllib.request.urlretrieve(url, zipped + ".part")
            os.rename(zipped + ".part", zipped)
        unpacked = zipped[:-4]
        if not os.path.exists(unpacked):
            with zipfile.ZipFile(zipped) as z:
                z.extractall(unpacked, [n for n in z.namelist() if not n.startswith("__MACOSX")])
    pages = glob.glob(f"{CACHE}/*Testpages/*/Ghent_PDF-Output-Test-V50_ALL_X4.pdf")[0]
    return pages, pages.replace("_ALL_X4.pdf", "_ALL_REFERENCE.pdf")


def ink_of(path):
    return urllib.parse.unquote(path[path.index("(") + 1:path.rindex(")")])


def plates(x4):
    """gs's and pdfium's plates per (page, ink), cut to one size; gs writes
    none for a spot a page does not use (pdfium's carries its /All marks)"""
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    subprocess.run(["gs", "-q", "-dBATCH", "-dNOPAUSE", "-dUseFastColor", "-dOverprint=/simulate", f"-r{DPI}",
                    "-sDEVICE=tiffsep", f"-sOutputFile={WORK}/gs-%d.tif", x4], check=True)
    npages = len(glob.glob(f"{WORK}/gs-[0-9].tif"))
    inks = PROCESS + sorted({ink_of(p) for p in glob.glob(f"{WORK}/gs-*(*).tif")} - set(PROCESS))
    gs, pd = {}, {}
    for page in range(1, npages + 1):
        for path in glob.glob(f"{WORK}/gs-{page}(*).tif"):
            gs[page, ink_of(path)] = np.asarray(Image.open(path).convert("L"))
        # each page alone (qpdf leaves it as it is), every plate in one process
        one = f"{WORK}/page{page}.pdf"
        subprocess.run(["qpdf", x4, "--pages", ".", str(page), "--", one], check=True)
        subprocess.run([BENCH, one, str(DPI), "256", ",".join(inks), f"--dump={WORK}/pd{page}", "--ink"], check=True,
                       capture_output=True)
        for ink in inks:
            pd[page, ink] = np.asarray(Image.open(f"{WORK}/pd{page}-{ink}.pgm"))
    h = min(a.shape[0] for a in [*gs.values(), *pd.values()])
    w = min(a.shape[1] for a in [*gs.values(), *pd.values()])
    return npages, inks, {k: v[:h, :w] for k, v in gs.items()}, {k: v[:h, :w] for k, v in pd.items()}


def composite(planes):
    """plates printed on paper, each ink filtering the light (ideal inks)"""
    rgb = None
    for ink, plate in planes:
        if rgb is None:
            rgb = np.ones(plate.shape + (3,))
        rgb *= 1 - ((255 - plate[..., None]) / 255) * (1 - np.array(INK_RGB.get(ink, (128, 128, 128))) / 255)
    return Image.fromarray(np.round(rgb * 255).astype(np.uint8))


def diff(a, b):
    """b in gray, orange where it is off a by up to 8, red more"""
    a = np.asarray(a.convert("L") if isinstance(a, Image.Image) else a, int)
    b = np.asarray(b.convert("L") if isinstance(b, Image.Image) else b, int)
    d = np.abs(a - b)
    vis = np.stack([(b * 0.4 + 153).astype(np.uint8)] * 3, -1)
    vis[(d > 0) & (d <= 8)] = (255, 160, 0)
    vis[d > 8] = (230, 0, 0)
    return Image.fromarray(vis), d


class Reference:
    """GWG's ALL_REFERENCE.pdf (~60 dpi): page 1 the composites, then a page
    per plate, each a 3 x 2 grid of the test pages"""

    def __init__(self, pdf, size, pd, npages, inks):
        subprocess.run(["gs", "-q", "-dBATCH", "-dNOPAUSE", "-sDEVICE=png16m", "-r72",
                        f"-sOutputFile={WORK}/ref-%d.png", pdf], check=True)
        self.pages = sorted(glob.glob(f"{WORK}/ref-*.png"), key=lambda p: int(p[p.rindex("-") + 1:-4]))
        self.size = size
        # which page is which plate: the one most like pdfium's
        small = {ink: np.concatenate([np.asarray(Image.fromarray(pd[p, ink]).resize((60, 80)), float).ravel()
                                      for p in range(1, npages + 1)]) for ink in inks}
        self.plate = {}
        for path in self.pages[1:]:
            a = np.concatenate([np.asarray(self.cell(path, p).convert("L").resize((60, 80)), float).ravel()
                                for p in range(1, npages + 1)])
            self.plate[min(inks, key=lambda ink: np.abs(a - small[ink]).mean())] = path

    def cell(self, path, page):
        im = Image.open(path).convert("RGB")
        w, h = im.size[0] / 3, im.size[1] / 2
        c, r = (page - 1) % 3, (page - 1) // 3
        return im.crop((round(c * w), round(r * h), round((c + 1) * w), round((r + 1) * h))).resize(self.size,
                                                                                                    Image.LANCZOS)

    def composite(self, page):
        return self.cell(self.pages[0], page)

    def of(self, page, ink):
        return self.cell(self.plate[ink], page) if ink in self.plate else Image.new("RGB", self.size, (220,) * 3)


def sheet(title, panels, note=""):
    w, h = panels[0][1].size
    pad, head = 20, 100
    im = Image.new("RGB", (len(panels) * (w + pad) + pad, h + head + 60), "white")
    d = ImageDraw.Draw(im)
    d.text((pad, 12), title, fill="black", font=BOLD)
    for i, (label, p) in enumerate(panels):
        x = pad + i * (w + pad)
        d.text((x, 62), label, fill=(80, 80, 80), font=FONT)
        im.paste(p.convert("RGB"), (x, head))
    d.text((pad, h + head + 20), note, fill=(90, 90, 90), font=FONT)
    return im


def review(npages, inks, gs, pd, ref):
    blank = np.full(next(iter(pd.values())).shape, 255, np.uint8)
    sheets, rows = [], []
    for page in range(1, npages + 1):
        cg = composite([(ink, gs.get((page, ink), blank)) for ink in inks])
        cp = composite([(ink, pd[page, ink]) for ink in inks])
        vis, d = diff(cg, cp)
        rows.append(f"{page:<6}{'composite':<16}{(d == 0).mean() * 100:>10.2f}%{(d > 8).sum():>12}")
        sheets.append(sheet(f"Page {page}: composite (plates merged, same inks both sides)",
                            [("GWG reference", ref.composite(page)), ("Ghostscript", cg), ("pdfium", cp),
                             ("diff: orange <=8, red >8", vis)],
                            "GWG's reference is its own color render: judge by the patches' X marks, not color"))
        for ink in inks:
            if (page, ink) not in gs:
                continue
            vis, d = diff(gs[page, ink], pd[page, ink])
            rows.append(f"{page:<6}{ink:<16}{(d == 0).mean() * 100:>10.2f}%{(d > 8).sum():>12}")
            sheets.append(sheet(f"Page {page}: plate {ink}",
                                [("GWG reference", ref.of(page, ink)), ("Ghostscript", Image.fromarray(gs[page, ink])),
                                 ("pdfium", Image.fromarray(pd[page, ink])), ("diff: orange <=8, red >8", vis)],
                                f"identical {(d == 0).mean() * 100:.2f}%, off by more than 8: {(d > 8).sum()} px"))
    front = Image.new("RGB", sheets[0].size, "white")
    d = ImageDraw.Draw(front)
    d.text((20, 12), "GWG Output Suite 5.0 (ALL_X4): Ghostscript and pdfium plates, by eye against GWG's reference",
           fill="black", font=BOLD)
    d.text((20, 70), f"{DPI} dpi, no anti-aliasing; gs -dUseFastColor -dOverprint=/simulate tiffsep; "
                     "pdfium plates_bench (8-bit gray, the page on its bottom edge as gs)", fill=(90, 90, 90), font=FONT)
    lines = [f"{'page':<6}{'plate':<16}{'identical':>11}{'px off >8':>12}"] + rows
    per_col = (front.size[1] - 160) // 34
    for i, line in enumerate(lines):
        d.text((20 + (i // per_col) * 760, 130 + (i % per_col) * 34), line, fill="black", font=MONO)
    front.save(f"{OUT}/review.pdf", save_all=True, append_images=sheets, resolution=DPI, quality=80)
    print("\n".join(lines))


def blend_modes(inks, gs, pd, ref):
    """the blend mode patches, larger: a cross marks a mode done wrong"""
    blank = np.full(next(iter(pd.values())).shape, 255, np.uint8)
    # (patch, test page, box in 144 dpi pixels)
    patches = [("16.0 DeviceCMYK, non-knockout", 2, (30, 230, 600, 500)),
               ("16.1 DeviceCMYK, knockout", 2, (600, 230, 1170, 500)),
               ("16.2 DeviceCMYK, isolated", 2, (30, 500, 600, 780)),
               ("16.4 ICCBased CMYK", 5, (590, 840, 1170, 1180))]
    sheets = []
    for title, page, box in patches:
        panels = [("GWG reference", ref.composite(page)),
                  ("Ghostscript", composite([(ink, gs.get((page, ink), blank)) for ink in inks])),
                  ("pdfium", composite([(ink, pd[page, ink]) for ink in inks]))]
        size = ((box[2] - box[0]) * 2, (box[3] - box[1]) * 2)
        sheets.append(sheet("GWG " + title, [(label, p.crop(box).resize(size, Image.LANCZOS)) for label, p in panels]))
    sheets[0].save(f"{OUT}/blend-modes.pdf", save_all=True, append_images=sheets[1:], resolution=DPI)


x4, reference = fetch()
npages, inks, gs, pd = plates(x4)
ref = Reference(reference, pd[1, "Cyan"].shape[::-1], pd, npages, inks)
review(npages, inks, gs, pd, ref)
blend_modes(inks, gs, pd, ref)
print(f"{OUT}/review.pdf\n{OUT}/blend-modes.pdf")
