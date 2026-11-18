"""Separation test PDFs, one topic each, on one pixel-aligned grid: cells of
30pt with 6pt gaps, every edge on whole points (at 144dpi a cell is 60x60px).
Each PDF comes with <name>.json: its cells, row and column labels.
usage: make.py <dir>"""
import json
import os
import struct
import sys
import zlib

# every edge a multiple of 3pt: whole pixels at 144dpi and at 2400 (3pt = 100px)
CELL, GAP, MARGIN = 30, 6, 21
LEFT, TOP = 111, 51  # room for the row and column labels
TINTS = [0, 1, 2, 3, 5, 10, 20, 25, 30, 40, 50, 60, 70, 75, 80, 90, 95, 97, 99, 100]
P021 = "PANTONE 021 C"  # orange, alternate M .53 Y 1
P354 = "PANTONE 354 C"  # green, alternate C .8 Y .9
ICC = "/System/Library/ColorSync/Profiles"


def name(n):
    return "/" + n.replace(" ", "#20")


class Doc:
    """One page of cells: `cell(r, c, ops)` draws `ops` with the cell's corner
    at the origin; resources are added by kind and named."""

    def __init__(self, rows, cols):
        self.rows, self.cols = rows, cols
        self.objs = []
        self.res = {"ColorSpace": {}, "ExtGState": {}, "XObject": {}, "Shading": {}, "Pattern": {}, "Font": {}}
        self.content = []
        self.cells = []
        self.meta = {}  # into the .json: dpi, plates antialiased too, no composites

    def obj(self, body):
        self.objs.append(body if isinstance(body, bytes) else body.encode())
        return f"{len(self.objs) + 2} 0 R"  # 1 catalog, 2 pages come first

    def stream(self, dict_body, data, compress=True):
        if compress:
            data = zlib.compress(data)
            dict_body += " /Filter /FlateDecode"
        return self.obj(f"<< {dict_body} /Length {len(data)} >>\nstream\n".encode() + data + b"\nendstream")

    def add(self, kind, key, value):
        self.res[kind][key] = value
        return key

    @property
    def W(self):
        return LEFT + MARGIN + len(self.cols) * CELL + (len(self.cols) - 1) * GAP

    @property
    def H(self):
        return TOP + MARGIN + len(self.rows) * CELL + (len(self.rows) - 1) * GAP

    def xy(self, r, c):
        return LEFT + c * (CELL + GAP), self.H - TOP - CELL - r * (CELL + GAP)

    def labels(self):
        """rows on the left, columns above, 5pt Helvetica in black"""
        esc = lambda t: t.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")
        out = ["BT /Fl 5 Tf 0 g"]
        for r, label in enumerate(self.rows):
            x, y = self.xy(r, 0)
            out.append(f"1 0 0 1 {MARGIN} {y + 13} Tm ({esc(label)}) Tj")
        for c, label in enumerate(self.cols):
            x, y = self.xy(0, c)
            out.append(f"0 1 -1 0 {x + 17} {y + CELL + 4} Tm ({esc(label)}) Tj")
        return "\n".join(out + ["ET\n"])

    def cell(self, r, c, ops, half=False, row=None, col=None):
        x, y = self.xy(r, c)
        self.content.append(f"q 1 0 0 1 {x} {y} cm\n{ops}\nQ\n")
        self.cells.append({"row": row or self.rows[r], "col": col or self.cols[c], "x": x, "y": y, "half": half})

    def write(self, path):
        page_res = " ".join(f"/{k} << " + " ".join(f"/{n} {v}" for n, v in d.items()) + " >>"
                            for k, d in self.res.items() if d)
        self.res["Font"]["Fl"] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"
        content = (self.labels() + "".join(self.content)).encode()
        contents = self.stream("", content)
        annots = " ".join(self.obj(a) for a in getattr(self, "annots", []))
        page = self.obj(f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {self.W} {self.H}]"
                        f" /Contents {contents} /Resources << {page_res} >>"
                        + (f" /Annots [{annots}]" if annots else "") + " >>")
        objs = [b"<< /Type /Catalog /Pages 2 0 R >>", f"<< /Type /Pages /Kids [{page}] /Count 1 >>".encode()] + self.objs
        out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
        offsets = []
        for n, body in enumerate(objs, 1):
            offsets.append(len(out))
            out += f"{n} 0 obj\n".encode() + body + b"\nendobj\n"
        xref = len(out)
        out += f"xref\n0 {len(objs) + 1}\n0000000000 65535 f \n".encode()
        out += b"".join(f"{o:010d} 00000 n \n".encode() for o in offsets)
        out += f"trailer\n<< /Size {len(objs) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode()
        open(path + ".pdf", "wb").write(out)
        json.dump({"W": self.W, "H": self.H, "cell": CELL, "cells": self.cells, **self.meta}, open(path + ".json", "w"))

    # color spaces used across the tests
    def spot(self, n, alt):
        return self.add("ColorSpace", "S" + n.split()[1],
                        self.obj(f"[/Separation {name(n)} /DeviceCMYK << /FunctionType 2 /Domain [0 1]"
                                 f" /C0 [0 0 0 0] /C1 [{alt}] /N 1 >>]"))

    def spots(self):
        return self.spot(P021, "0 0.53 1 0"), self.spot(P354, "0.8 0 0.9 0")

    def devicen(self):
        # c = .8 s354, m = .53 s021, y = s021 + .9 s354, k = 0
        fn = b"{exch dup 0.8 mul exch 0.9 mul 3 -1 roll dup 0.53 mul 3 1 roll add 0}"
        f = self.stream("/FunctionType 4 /Domain [0 1 0 1] /Range [0 1 0 1 0 1 0 1]", fn, compress=False)
        return self.add("ColorSpace", "SN", self.obj(f"[/DeviceN [{name(P021)} {name(P354)}] /DeviceCMYK {f}]"))

    def gs(self, key, body):
        return self.add("ExtGState", key, self.obj(body))

    def image(self, key, w, h, cs, bpc, data, extra=""):
        return self.add("XObject", key, self.stream(
            f"/Type /XObject /Subtype /Image /Width {w} /Height {h} /ColorSpace {cs}"
            f" /BitsPerComponent {bpc} /Interpolate false {extra}", data))


def v(t):
    return f"{t / 100:.4f}"


def box(x=0, y=0, w=CELL, h=CELL):
    return f"{x} {y} {w} {h} re f"


# ── 01 tints: every ink's ramp, flat ──────────────────────────────────────
def tints():
    rows = ["Cyan", "Magenta", "Yellow", "Black", P021, P354, "DeviceN 021+354", "CMYK all"]
    d = Doc(rows, [f"{t}%" for t in TINTS])
    s021, s354 = d.spots()
    sn = d.devicen()
    for c, t in enumerate(TINTS):
        ops = [f"{v(t)} 0 0 0 k", f"0 {v(t)} 0 0 k", f"0 0 {v(t)} 0 k", f"0 0 0 {v(t)} k",
               f"/{s021} cs {v(t)} scn", f"/{s354} cs {v(t)} scn", f"/{sn} cs {v(t)} {v(t)} scn",
               f"{v(t)} {v(t)} {v(t)} {v(t)} k"]
        for r, op in enumerate(ops):
            d.cell(r, c, f"{op} {box()}")
    return d


# how a second color goes over a first: (label, ExtGState entries or None)
MODES = [("knockout", None), ("overprint", "/OP true /op true /OPM 1"), ("ca .25", "/ca 0.25"),
         ("ca .5", "/ca 0.5"), ("ca .75", "/ca 0.75"), ("Multiply", "/BM /Multiply"),
         ("Multiply ca .5", "/BM /Multiply /ca 0.5"), ("Screen", "/BM /Screen"), ("Darken", "/BM /Darken"),
         ("Lighten", "/BM /Lighten"), ("Overlay", "/BM /Overlay"), ("Difference", "/BM /Difference"),
         ("Exclusion", "/BM /Exclusion"), ("HardLight", "/BM /HardLight"), ("ColorBurn", "/BM /ColorBurn")]


# ── 00 sheet: CMYK, the two Pantones and RGB on one page ─────────────────
# top left every ink pair blended 15 ways, top right RGB over every ink and
# over RGB the same 15 ways, below the ramps of every ink and of R, G, B
# (white to the color). RGB has no plate: it separates into the process inks
def sheet():
    inks = ["Cyan", "Magenta", "Yellow", "Black", P021, P354]
    rgbs = {"R": "1 {0} {0} rg", "G": "{0} 1 {0} rg", "B": "{0} {0} 1 rg"}
    ink_pairs = [(a, b) for i, a in enumerate(inks) for b in inks[i + 1:]]
    rgb_pairs = [(r, ink) for r in rgbs for ink in inks] + [("G", "R"), ("B", "R"), ("B", "G")]
    ramps = inks + ["DeviceN 021+354", "CMYK all", "R", "G", "B"]
    top = max(len(ink_pairs), len(rgb_pairs))
    d = Doc([""] * (top + 1 + len(ramps)), [""] * (2 * len(MODES) + 1))
    s021, s354 = d.spots()
    sn = d.devicen()
    for c, (m, g) in enumerate(MODES):
        if g:
            d.gs(f"G{c}", f"<< {g} >>")
    fill = {"Cyan": "{} 0 0 0 k", "Magenta": "0 {} 0 0 k", "Yellow": "0 0 {} 0 k", "Black": "0 0 0 {} k",
            P021: f"/{s021} cs {{}} scn", P354: f"/{s354} cs {{}} scn"}

    # an RGB color at tint t: white to the full color
    def color(name, t):
        return rgbs[name].format(f"{1 - t:.4f}") if name in rgbs else fill[name].format(f"{t:.4f}")

    def blend(r, c0, a, b):
        for c, (m, g) in enumerate(MODES):
            over = f"q {'/G%d gs ' % c if g else ''}{color(b, 0.7)} {box(15, 0, 15)} Q"
            d.cell(r, c0 + c, f"{color(a, 0.6)} {box()}\n{over}", half=True, row=f"{b} over {a}", col=m)

    for r, (a, b) in enumerate(ink_pairs):
        blend(r, 0, a, b)
    for r, (b, a) in enumerate(rgb_pairs):
        blend(r, len(MODES) + 1, a, b)
    ops = {"DeviceN 021+354": f"/{sn} cs {{0}} {{0}} scn", "CMYK all": "{0} {0} {0} {0} k"}
    for i, ramp in enumerate(ramps):
        for c, t in enumerate(TINTS):
            op = ops[ramp].format(v(t)) if ramp in ops else color(ramp, t / 100)
            d.cell(top + 1 + i, c, f"{op} {box()}", row=ramp, col=f"{t}%")
    return d


# ── 02 blends: every ink pair, second ink over the right half, 12 ways ────
def blends():
    inks = ["Cyan", "Magenta", "Yellow", "Black", P021, P354]
    pairs = [(a, b) for i, a in enumerate(inks) for b in inks[i + 1:]]
    modes = MODES
    d = Doc([f"{a} + {b}" for a, b in pairs], [m for m, _ in modes])
    s021, s354 = d.spots()
    fill = {"Cyan": "{} 0 0 0 k", "Magenta": "0 {} 0 0 k", "Yellow": "0 0 {} 0 k", "Black": "0 0 0 {} k",
            P021: f"/{s021} cs {{}} scn", P354: f"/{s354} cs {{}} scn"}
    for c, (m, g) in enumerate(modes):
        if g:
            d.gs(f"G{c}", f"<< {g} >>")
    for r, (a, b) in enumerate(pairs):
        for c, (m, g) in enumerate(modes):
            top = f"q {'/G%d gs ' % c if g else ''}{fill[b].format('0.7')} {box(15, 0, 15)} Q"
            d.cell(r, c, f"{fill[a].format('0.6')} {box()}\n{top}", half=True)
    return d


# ── 03 rgb + gray + CIE vectors ───────────────────────────────────────────
def rgb_vectors():
    rows = ["rgb R", "rgb G", "rgb B", "rgb t t t", "rgb t .5 1-t", "gray", "CalRGB t 0 0", "CalGray", "Lab L 50 a t"]
    d = Doc(rows, [f"{t}%" for t in TINTS])
    cal = d.add("ColorSpace", "CR", d.obj("[/CalRGB << /WhitePoint [0.9505 1 1.089] >>]"))
    calg = d.add("ColorSpace", "CG", d.obj("[/CalGray << /WhitePoint [0.9505 1 1.089] >>]"))
    lab = d.add("ColorSpace", "LB", d.obj("[/Lab << /WhitePoint [0.9505 1 1.089] /Range [-100 100 -100 100] >>]"))
    for c, t in enumerate(TINTS):
        x = t / 100
        ops = [f"{x:.4f} 0 0 rg", f"0 {x:.4f} 0 rg", f"0 0 {x:.4f} rg", f"{x:.4f} {x:.4f} {x:.4f} rg",
               f"{x:.4f} 0.5 {1 - x:.4f} rg", f"{x:.4f} g", f"/{cal} cs {x:.4f} 0 0 sc",
               f"/{calg} cs {x:.4f} sc", f"/{lab} cs 50 {x * 200 - 100:.2f} 0 sc"]
        for r, op in enumerate(ops):
            d.cell(r, c, f"{op} {box()}")
    return d


# one image per row, a pixel per cell: cell c is the image's pixel c
def image_rows(d, specs):
    """specs: (row, colorspace, bpc, components per pixel as a function of the
    tint 0..1). Each row one image, `len(TINTS)` px wide, laid over its cells."""
    for r, (label, cs, bpc, comps) in enumerate(specs):
        maxv = (1 << bpc) - 1
        bits = []
        for t in TINTS:
            bits += [round(x * maxv) for x in comps(t / 100)]
        if bpc == 8:
            data = bytes(bits)
        elif bpc == 16:
            data = b"".join(struct.pack(">H", b) for b in bits)
        else:  # pack bpc < 8 into the row's bytes
            acc, n, out = 0, 0, bytearray()
            for b in bits:
                acc = (acc << bpc) | b
                n += bpc
                while n >= 8:
                    n -= 8
                    out.append((acc >> n) & 0xFF)
            if n:
                out.append((acc << (8 - n)) & 0xFF)
            data = bytes(out)
        key = d.image(f"Im{r}", len(TINTS), 1, cs, bpc, data)
        # each pixel is a cell and its gap: the gap shows the image too, so
        # the cells are what is compared
        x0, y0 = d.xy(r, 0)
        width = len(TINTS) * (CELL + GAP)
        d.content.append(f"q {width} 0 0 {CELL} {x0} {y0} cm /{key} Do Q\n")
        for c in range(len(TINTS)):
            x, y = d.xy(r, c)
            d.cells.append({"row": label, "col": f"{TINTS[c]}%", "x": x, "y": y, "half": False})


# ── 04 images in process colors and gray ─────────────────────────────────
def images_process():
    specs = [("CMYK 8 C", "/DeviceCMYK", 8, lambda t: (t, 0, 0, 0)),
             ("CMYK 8 M", "/DeviceCMYK", 8, lambda t: (0, t, 0, 0)),
             ("CMYK 8 Y", "/DeviceCMYK", 8, lambda t: (0, 0, t, 0)),
             ("CMYK 8 K", "/DeviceCMYK", 8, lambda t: (0, 0, 0, t)),
             ("CMYK 8 all", "/DeviceCMYK", 8, lambda t: (t, t / 2, 1 - t, t / 3)),
             ("CMYK 16 C", "/DeviceCMYK", 16, lambda t: (t, 0.2, 0, 0)),
             ("CMYK 4 all", "/DeviceCMYK", 4, lambda t: (t, 1 - t, t, 0)),
             ("Gray 8", "/DeviceGray", 8, lambda t: (t,)),
             ("Gray 4", "/DeviceGray", 4, lambda t: (t,)),
             ("Gray 1", "/DeviceGray", 1, lambda t: (1 if t >= 0.5 else 0,))]
    d = Doc([s[0] for s in specs], [f"{t}%" for t in TINTS])
    image_rows(d, specs)
    return d


# ── 05 images in RGB, CIE and indexed RGB ────────────────────────────────
def images_rgb():
    d = Doc([], [f"{t}%" for t in TINTS])
    pal = bytes(b for i in range(256) for b in (i, 255 - i, (i * 3) % 256))
    idx = d.obj(f"[/Indexed /DeviceRGB 255 <{pal.hex()}>]")
    lab = d.obj("[/Lab << /WhitePoint [0.9505 1 1.089] /Range [-128 127 -128 127] >>]")
    specs = [("RGB 8 R", "/DeviceRGB", 8, lambda t: (t, 0, 0)),
             ("RGB 8 G", "/DeviceRGB", 8, lambda t: (0, t, 0)),
             ("RGB 8 B", "/DeviceRGB", 8, lambda t: (0, 0, t)),
             ("RGB 8 t t t", "/DeviceRGB", 8, lambda t: (t, t, t)),
             ("RGB 8 mix", "/DeviceRGB", 8, lambda t: (t, 0.5, 1 - t)),
             ("RGB 16 mix", "/DeviceRGB", 16, lambda t: (1 - t, t, 0.3)),
             ("Indexed RGB 8", idx, 8, lambda t: (t,)),
             ("Lab 8", lab, 8, lambda t: (0.5, t, 0.5))]
    d.rows = [s[0] for s in specs]
    image_rows(d, specs)
    return d


# ── 06 images in spots: Separation, DeviceN, indexed over a spot ──────────
def images_spot():
    d = Doc([], [f"{t}%" for t in TINTS])
    s021, s354 = d.spots()
    sn = d.devicen()
    cs = d.res["ColorSpace"]
    pal = bytes(i for i in range(256))
    idx = d.obj(f"[/Indexed {cs[s354]} 255 <{pal.hex()}>]")
    mixed = d.obj(f"[/DeviceN [/Cyan {name(P021)}] /DeviceCMYK "
                  + d.stream("/FunctionType 4 /Domain [0 1 0 1] /Range [0 1 0 1 0 1 0 1]",
                             b"{dup 0.53 mul exch 0}", compress=False) + "]")
    specs = [("Separation 021", cs[s021], 8, lambda t: (t,)),
             ("Separation 354", cs[s354], 8, lambda t: (t,)),
             ("Separation 021 16", cs[s021], 16, lambda t: (t,)),
             ("DeviceN 021+354", cs[sn], 8, lambda t: (t, 1 - t)),
             ("DeviceN Cyan+021", mixed, 8, lambda t: (t, 0.5)),
             ("Indexed over 354", idx, 8, lambda t: (t,))]
    d.rows = [s[0] for s in specs]
    image_rows(d, specs)
    return d


# ── 07 overprint: a 5-ink base, overprinted on its right half ────────────
def overprint():
    d = Doc([], [])
    s021, s354 = d.spots()
    sn = d.devicen()
    none = d.add("ColorSpace", "SNone", d.obj("[/Separation /None /DeviceCMYK << /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [0 0 0 1] /N 1 >>]"))
    allcs = d.add("ColorSpace", "SAll", d.obj("[/Separation /All /DeviceCMYK << /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [1 1 1 1] /N 1 >>]"))
    op1 = d.gs("OP1", "<< /OP true /op true /OPM 1 >>")
    op0 = d.gs("OP0", "<< /OP true /op true /OPM 0 >>")
    img = d.image("ImS", 2, 1, d.obj(f"[/Separation {name(P354)} /DeviceCMYK << /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [0.8 0 0.9 0] /N 1 >>]"), 8, bytes([128, 255]))
    sh = d.add("Shading", "Sh", d.obj(f"<< /ShadingType 2 /ColorSpace {d.res['ColorSpace'][s354]} /Coords [15 0 30 0]"
                                      " /Function << /FunctionType 2 /Domain [0 1] /C0 [1] /C1 [1] /N 1 >> >>"))
    cases = [("knockout k", None, "0 0 0 1 k " + box(15, 0, 15)),
             ("OPM0 k", op0, "0 0 0 1 k " + box(15, 0, 15)),
             ("OPM1 k K", op1, "0 0 0 1 k " + box(15, 0, 15)),
             ("OPM1 k C.5", op1, "0.5 0 0 0 k " + box(15, 0, 15)),
             ("OPM1 g", op1, "0.5 g " + box(15, 0, 15)),
             ("OPM1 rgb", op1, "1 0 0 rg " + box(15, 0, 15)),
             ("OPM1 cs CMYK", op1, "/DeviceCMYK cs 0 0 0 1 scn " + box(15, 0, 15)),
             ("OP 354", op1, f"/{s354} cs 1 scn " + box(15, 0, 15)),
             ("OP 021 same", op1, f"/{s021} cs 1 scn " + box(15, 0, 15)),
             ("OP DeviceN", op1, f"/{sn} cs 0 1 scn " + box(15, 0, 15)),
             ("OP None", op1, f"/{none} cs 1 scn " + box(15, 0, 15)),
             ("knockout None", None, f"/{none} cs 1 scn " + box(15, 0, 15)),
             ("OP All", op1, f"/{allcs} cs 0.5 scn " + box(15, 0, 15)),
             ("OP stroke K", op1, f"0 0 0 1 K 10 w 20 0 m 20 30 l S"),
             ("OP image 354", op1, f"15 0 0 30 15 0 cm /{img} Do"),
             ("OP shading 354", op1, f"15 0 15 30 re W n /{sh} sh")]
    d.rows, d.cols = ["over CMYK .3 + 021 .5"], [c[0] for c in cases]
    base = f"0.3 0.3 0.3 0.3 k {box()}\nq /{op1} gs /{s021} cs 0.5 scn {box()} Q"
    for c, (label, g, top) in enumerate(cases):
        d.cell(0, c, f"{base}\nq {'/%s gs ' % g if g else ''}{top} Q", half=True)
    return d


# ── 08 shadings: axial across the cell, per ink and space ─────────────────
def shadings():
    d = Doc([], [])
    s021, s354 = d.spots()
    sn = d.devicen()
    cs = d.res["ColorSpace"]
    specs = [("CMYK C", "/DeviceCMYK", "[0 0 0 0]", "[1 0 0 0]"),
             ("CMYK all", "/DeviceCMYK", "[0 1 0 0.2]", "[1 0 1 0]"),
             ("Gray", "/DeviceGray", "[0]", "[1]"),
             ("RGB", "/DeviceRGB", "[1 0 0]", "[0 0 1]"),
             ("Sep 021", cs[s021], "[0]", "[1]"),
             ("DeviceN", cs[sn], "[1 0]", "[0 1]")]
    d.rows, d.cols = [s[0] for s in specs], ["axial 1 cell", "axial 4 cells", "", "", "radial"]
    for r, (label, space, c0, c1) in enumerate(specs):
        fn = f"<< /FunctionType 2 /Domain [0 1] /C0 {c0} /C1 {c1} /N 1 >>"
        a1 = d.add("Shading", f"A{r}", d.obj(f"<< /ShadingType 2 /ColorSpace {space} /Coords [0 0 {CELL} 0] /Function {fn} >>"))
        a4 = d.add("Shading", f"B{r}", d.obj(f"<< /ShadingType 2 /ColorSpace {space} /Coords [0 0 {4 * CELL + 3 * GAP} 0] /Function {fn} >>"))
        rd = d.add("Shading", f"R{r}", d.obj(f"<< /ShadingType 3 /ColorSpace {space} /Coords [15 15 0 15 15 15] /Function {fn} /Extend [true true] >>"))
        x0, y0 = d.xy(r, 0)
        d.cell(r, 0, f"0 0 {CELL} {CELL} re W n /{a1} sh")
        x1, _ = d.xy(r, 1)
        d.content.append(f"q 1 0 0 1 {x1} {y0} cm 0 0 {3 * CELL + 2 * GAP} {CELL} re W n /{a4} sh Q\n")
        d.cells.append({"row": label, "col": "axial 4 cells", "x": x1, "y": y0, "half": False})
        d.cell(r, 4, f"0 0 {CELL} {CELL} re W n /{rd} sh")
    return d


# ── 09 transparency groups and soft masks ─────────────────────────────────
def groups():
    d = Doc([], [])
    s021, s354 = d.spots()
    a5 = d.gs("A5", "<< /ca 0.5 >>")
    mul = d.gs("Mul", "<< /BM /Multiply >>")

    def form(key, ops, group):
        return d.add("XObject", key, d.stream(f"/Type /XObject /Subtype /Form /BBox [0 0 {CELL} {CELL}]"
                                              f" /Resources << /ExtGState << /A5 {d.res['ExtGState'][a5]} /Mul {d.res['ExtGState'][mul]} >>"
                                              f" /ColorSpace << /S021 {d.res['ColorSpace'][s021]} /S354 {d.res['ColorSpace'][s354]} >> >>"
                                              f" {group}", ops.encode()))

    inner = f"/A5 gs 1 0 0 0 k {box(0, 0, 20)} /S354 cs 1 scn {box(10, 0, 20)}"
    lum = form("Lum", f"0.5 g {box(0, 0, 15)} 1 g {box(15, 0, 15)}", "/Group << /S /Transparency /CS /DeviceGray >>")
    smask_l = d.gs("SML", f"<< /SMask << /Type /Mask /S /Luminosity /G {d.res['XObject'][lum]} >> >>")
    alpha = form("Alp", f"/A5 gs 0 g {box(0, 0, 15)} 0 g {box(15, 0, 15)}", "/Group << /S /Transparency >>")
    smask_a = d.gs("SMA", f"<< /SMask << /Type /Mask /S /Alpha /G {d.res['XObject'][alpha]} >> >>")
    group, mul_group = form("F1", inner, "/Group << /S /Transparency >>"), form("F4", f"/Mul gs {inner}", "/Group << /S /Transparency >>")
    k = form("F5", f"0 0 0 1 k {box()}", "")
    # (label, form, the state it is drawn in): the last three a group the page
    # shows through, its objects blending with the page (an InDesign shadow)
    cases = [("plain", form("F0", inner, ""), ""), ("group", group, ""),
             ("isolated", form("F2", inner, "/Group << /S /Transparency /I true >>"), ""),
             ("knockout", form("F3", inner, "/Group << /S /Transparency /K true >>"), ""),
             ("group Multiply", mul_group, ""), ("smask luminosity", k, f"/{smask_l} gs "), ("smask alpha", k, f"/{smask_a} gs "),
             ("group ca .5", group, f"/{a5} gs "), ("group smask", group, f"/{smask_l} gs "),
             ("group Multiply smask", mul_group, f"/{smask_a} gs ")]
    d.rows, d.cols = ["over Y .5 + 021 .5"], [c[0] for c in cases]
    base = f"0 0 0.5 0 k {box()} /{s021} cs 0.5 scn {box(0, 0, 15)}"
    for c, (label, f, state) in enumerate(cases):
        d.cell(0, c, f"{base}\nq {state}/{f} Do Q")
    return d


# ── 10 ICC-based spaces, vectors and images ───────────────────────────────
def icc():
    d = Doc([], [f"{t}%" for t in TINTS])
    srgb = open(f"{ICC}/sRGB Profile.icc", "rb").read()
    cmyk = open(f"{ICC}/Generic CMYK Profile.icc", "rb").read()
    rgb_cs = d.add("ColorSpace", "IR", d.obj(f"[/ICCBased {d.stream('/N 3', srgb)}]"))
    cmyk_cs = d.add("ColorSpace", "IC", d.obj(f"[/ICCBased {d.stream('/N 4', cmyk)}]"))
    rows = ["ICC RGB vec R", "ICC RGB vec mix", "ICC CMYK vec C", "ICC CMYK vec all"]
    d.rows = rows + ["ICC RGB img mix", "ICC CMYK img all"]
    for c, t in enumerate(TINTS):
        x = t / 100
        ops = [f"/{rgb_cs} cs {x:.4f} 0 0 sc", f"/{rgb_cs} cs {x:.4f} 0.5 {1 - x:.4f} sc",
               f"/{cmyk_cs} cs {x:.4f} 0 0 0 sc", f"/{cmyk_cs} cs {x:.4f} {x / 2:.4f} {1 - x:.4f} {x / 3:.4f} sc"]
        for r, op in enumerate(ops):
            d.cell(r, c, f"{op} {box()}")
    specs = [("ICC RGB img mix", d.res["ColorSpace"][rgb_cs], 8, lambda t: (t, 0.5, 1 - t)),
             ("ICC CMYK img all", d.res["ColorSpace"][cmyk_cs], 8, lambda t: (t, t / 2, 1 - t, t / 3))]
    # images on the rows after the vectors
    cells = d.cells
    d.rows = rows + [s[0] for s in specs]
    for i, spec in enumerate(specs):
        r = len(rows) + i
        maxv = 255
        data = bytes(round(x * maxv) for t in TINTS for x in spec[3](t / 100))
        key = d.image(f"Im{r}", len(TINTS), 1, spec[1], 8, data)
        x0, y0 = d.xy(r, 0)
        d.content.append(f"q {len(TINTS) * (CELL + GAP)} 0 0 {CELL} {x0} {y0} cm /{key} Do Q\n")
        for c in range(len(TINTS)):
            x, y = d.xy(r, c)
            cells.append({"row": spec[0], "col": f"{TINTS[c]}%", "x": x, "y": y, "half": False})
    return d


# ── 11 images: every filter, space, depth and mask, drawn 6 ways ─────────
# each cell: a base of CMYK .3 with 021 .5 on the left half and 354 .5 on
# the right half overprinted, so every plate has ink under, then a 2x2
# image over the whole cell (a sample 15pt, 30px) drawn as the column says
IMAGE_MODES = [("knockout", None), ("OP OPM1", "/OP true /op true /OPM 1"), ("OP OPM0", "/OP true /op true /OPM 0"),
               ("ca .5", "/ca 0.5"), ("Multiply", "/BM /Multiply"), ("OP ca .5", "/OP true /op true /OPM 1 /ca 0.5")]
LEVELS = (0, 0.25, 0.6, 1)  # pixel i, component k: LEVELS[(i + k) % 4]


def pixels(n, scale=1):
    """2x2 samples of n components, 0..1, each repeated scale x scale"""
    px = [[LEVELS[(i + k) % 4] for k in range(n)] for i in range(4)]
    return [px[(y // scale) * 2 + x // scale] for y in range(2 * scale) for x in range(2 * scale)]


def pack(samples, bpc, w):
    """samples (rows of w pixels, each a list of 0..1) to bytes, rows padded"""
    maxv = (1 << bpc) - 1
    out = bytearray()
    for y in range(len(samples) // w):
        acc, n = 0, 0
        for px in samples[y * w:(y + 1) * w]:
            for x in px:
                q = round(x * maxv)
                if bpc == 16:
                    out += struct.pack(">H", q)
                    continue
                acc, n = (acc << bpc) | q, n + bpc
                while n >= 8:
                    n -= 8
                    out.append((acc >> n) & 0xFF)
        if n:
            out.append((acc << (8 - n)) & 0xFF)
    return bytes(out)


def lzw(data):
    """LZWDecode, EarlyChange 1"""
    table = {bytes([i]): i for i in range(256)}
    nxt, width, acc, nbits, out = 258, 9, 0, 0, bytearray()

    def emit(code):
        nonlocal acc, nbits
        acc, nbits = (acc << width) | code, nbits + width
        while nbits >= 8:
            nbits -= 8
            out.append((acc >> nbits) & 0xFF)

    emit(256)
    w = b""
    for c in data:
        wc = w + bytes([c])
        if wc in table:
            w = wc
            continue
        emit(table[w])
        table[wc], nxt = nxt, nxt + 1
        if nxt + 1 >= 1 << width:
            width += 1
        w = bytes([c])
    if w:
        emit(table[w])
    emit(257)
    if nbits:
        out.append((acc << (8 - nbits)) & 0xFF)
    return bytes(out)


def runlength(data):
    out = bytearray()
    for i in range(0, len(data), 128):
        chunk = data[i:i + 128]
        out += bytes([len(chunk) - 1]) + chunk
    return bytes(out + b"\x80")


def predict_png(data, row):
    """PNG Up predictor on every row: Predictor 12"""
    out, prev = bytearray(), bytes(row)
    for y in range(0, len(data), row):
        line = data[y:y + row]
        out += b"\x02" + bytes((a - b) & 0xFF for a, b in zip(line, prev))
        prev = line
    return bytes(out)


def predict_tiff(data, row, n):
    """TIFF predictor 2, 8 bits: each sample minus the one n bytes left"""
    out = bytearray()
    for y in range(0, len(data), row):
        line = data[y:y + row]
        out += bytes((line[i] - (line[i - n] if i >= n else 0)) & 0xFF for i in range(len(line)))
    return bytes(out)


def pil_image(samples, w, mode):
    from PIL import Image
    data = bytes(round(x * 255) for px in samples for x in px)
    return Image.frombytes(mode, (w, len(samples) // w), data)


def encoded(fmt, samples, w, mode):
    """samples saved by PIL as fmt: JPEG, JPEG2000 bytes, or a CCITT G4 strip"""
    import io
    buf = io.BytesIO()
    im = pil_image(samples, w, mode)
    if fmt == "JPEG":
        im.save(buf, "JPEG", quality=100, subsampling=0)
        return buf.getvalue()
    if fmt == "JPEG2000":
        im.save(buf, "JPEG2000", irreversible=False)
        return buf.getvalue()
    # G4: the TIFF's one strip
    from PIL import Image
    im.convert("1").save(buf, "TIFF", compression="group4")
    t = Image.open(io.BytesIO(buf.getvalue()))
    off, cnt = t.tag_v2[273][0], t.tag_v2[279][0]
    return buf.getvalue()[off:off + cnt]


def jbig2(mmr, w, h):
    """an embedded JBIG2 stream: page information, an immediate lossless
    generic region coded MMR (T.6, as G4), end of page"""
    def seg(num, kind, data):
        return struct.pack(">IBBBI", num, kind, 0, 1, len(data)) + data
    page = struct.pack(">IIIIBH", w, h, 0, 0, 0, 0)
    region = struct.pack(">IIIIB", w, h, 0, 0, 0) + b"\x01" + mmr
    return seg(0, 48, page) + seg(1, 39, region) + seg(2, 49, b"")


def images():
    d = Doc([], [m for m, _ in IMAGE_MODES])
    s021, s354 = d.spots()
    sn = d.devicen()
    cs = d.res["ColorSpace"]
    for c, (_, g) in enumerate(IMAGE_MODES):
        if g:
            d.gs(f"G{c}", f"<< {g} >>")
    op1 = d.gs("OPB", "<< /OP true /op true /OPM 1 >>")
    srgb = open(f"{ICC}/sRGB Profile.icc", "rb").read()
    cmyk = open(f"{ICC}/Generic CMYK Profile.icc", "rb").read()
    gray = open(f"{ICC}/Generic Gray Gamma 2.2 Profile.icc", "rb").read()
    icc_rgb = d.obj(f"[/ICCBased {d.stream('/N 3', srgb)}]")
    icc_cmyk = d.obj(f"[/ICCBased {d.stream('/N 4', cmyk)}]")
    icc_gray = d.obj(f"[/ICCBased {d.stream('/N 1', gray)}]")
    sep = lambda n, alt: d.obj(f"[/Separation {name(n)} /DeviceCMYK << /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [{alt}] /N 1 >>]")
    fn4 = lambda ps: d.stream("/FunctionType 4 /Domain [0 1 0 1] /Range [0 1 0 1 0 1 0 1]", ps, compress=False)
    pal_rgb = bytes(b for i in range(4) for b in (round(255 * LEVELS[i]), round(255 * (1 - LEVELS[i])), 128))
    pal_cmyk = bytes(b for i in range(4) for b in (round(255 * LEVELS[i]), 0, round(255 * (1 - LEVELS[i])), 40))
    pal_256 = bytes(b for i in range(256) for b in (i, 255 - i, (i * 3) % 256))
    spaces = {
        "G": "/DeviceGray", "RGB": "/DeviceRGB", "CMYK": "/DeviceCMYK",
        "IdxRGB": f"[/Indexed /DeviceRGB 3 <{pal_rgb.hex()}>]",
        "IdxRGB256": f"[/Indexed /DeviceRGB 255 <{pal_256.hex()}>]",
        "IdxCMYK": f"[/Indexed /DeviceCMYK 3 <{pal_cmyk.hex()}>]",
        "Idx021": f"[/Indexed {cs[s021]} 3 <00406699>]",
        "IdxN": f"[/Indexed {cs[sn]} 3 <0000 40ff 9940 ff99>]",
        "021": cs[s021], "354": cs[s354], "N": cs[sn],
        "NCyan021": f"[/DeviceN [/Cyan {name(P021)}] /DeviceCMYK {fn4(b'{dup 0.53 mul exch 0}')}]",
        "NNone": f"[/DeviceN [{name(P354)} /None] /DeviceCMYK {fn4(b'{pop dup 0.8 mul exch 0 exch 0.9 mul 0}')}]",
        "SepCyan": sep("Cyan", "1 0 0 0"), "SepBlack": sep("Black", "0 0 0 1"), "SepAll": sep("All", "1 1 1 1"),
        "NCMYK": f"[/DeviceN [/Cyan /Magenta /Yellow /Black] /DeviceCMYK {d.stream('/FunctionType 4 /Domain [0 1 0 1 0 1 0 1] /Range [0 1 0 1 0 1 0 1]', b'{}', compress=False)}]",
        "ICCG": icc_gray, "ICCRGB": icc_rgb, "ICCCMYK": icc_cmyk,
        "CalG": "[/CalGray << /WhitePoint [0.9505 1 1.089] /Gamma 2.2 >>]",
        "CalRGB": "[/CalRGB << /WhitePoint [0.9505 1 1.089] >>]",
        "Lab": "[/Lab << /WhitePoint [0.9505 1 1.089] /Range [-128 127 -128 127] >>]",
    }
    ncomp = {"G": 1, "RGB": 3, "CMYK": 4, "021": 1, "354": 1, "N": 2, "NCyan021": 2, "NNone": 2, "SepCyan": 1,
             "SepBlack": 1, "SepAll": 1, "NCMYK": 4, "ICCG": 1, "ICCRGB": 3, "ICCCMYK": 4, "CalG": 1,
             "CalRGB": 3, "Lab": 3}
    indexed = {"IdxRGB": 3, "IdxRGB256": 255, "IdxCMYK": 3, "Idx021": 3, "IdxN": 3}
    # rows: (label, dict entries, data), dict without /Width /Height
    rows = []

    def raw(label, space, bpc, extra="", filt=None, smask=None):
        if space in indexed:
            # indices 0 to hival, as many as the depth holds
            maxv = (1 << bpc) - 1
            samples = [[min(round(LEVELS[i] * 3) * indexed[space] // 3, maxv) / maxv] for i in range(4)]
        else:
            samples = pixels(ncomp[space])
        data = pack(samples, bpc, 2)
        rows.append((label, f"/Width 2 /Height 2 /ColorSpace {spaces[space]} /BitsPerComponent {bpc} {extra}", data, filt, smask))

    for space in ("G", "RGB", "CMYK"):
        for bpc in (1, 2, 4, 8, 16):
            raw(f"{space} {bpc}", space, bpc)
    for space, bpcs in (("IdxRGB", (1, 2, 4, 8)), ("IdxCMYK", (2, 8)), ("Idx021", (2, 8)), ("IdxN", (8,)),
                        ("IdxRGB256", (8,))):
        for bpc in bpcs:
            raw(f"{space} {bpc}", space, bpc)
    for space in ("021", "354", "N", "NCyan021", "NNone", "SepCyan", "SepBlack", "SepAll", "NCMYK",
                  "ICCG", "ICCRGB", "ICCCMYK", "CalG", "CalRGB", "Lab"):
        raw(f"{space} 8", space, 8)
    raw("021 4", "021", 4)
    raw("N 16", "N", 16)
    # Decode arrays
    raw("G 8 Decode 1 0", "G", 8, "/Decode [1 0]")
    raw("CMYK 8 Decode inv", "CMYK", 8, "/Decode [1 0 1 0 1 0 1 0]")
    raw("021 8 Decode 1 0", "021", 8, "/Decode [1 0]")
    raw("CMYK 8 Decode .2 .8", "CMYK", 8, "/Decode [0.2 0.8 0 1 0 1 0.2 0.8]")
    raw("IdxRGB 8 Decode 3 0", "IdxRGB", 8, "/Decode [3 0]")
    raw("CMYK 8 Interpolate", "CMYK", 8, "/Interpolate true")
    # masks: color key, explicit stencil, soft
    raw("RGB 8 color key", "RGB", 8, "/Mask [0 70 0 70 0 255]")
    raw("CMYK 8 color key", "CMYK", 8, "/Mask [0 70 0 255 0 255 0 255]")
    raw("IdxRGB 8 color key", "IdxRGB", 8, "/Mask [0 1]")
    stencil = d.stream("/Type /XObject /Subtype /Image /Width 2 /Height 2 /ImageMask true /BitsPerComponent 1", bytes([0x80, 0x40]))
    raw("CMYK 8 /Mask stencil", "CMYK", 8, f"/Mask {stencil}")
    soft = d.stream("/Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceGray /BitsPerComponent 8", bytes([255, 128, 64, 0]))
    raw("CMYK 8 SMask", "CMYK", 8, f"/SMask {soft}")
    raw("021 8 SMask", "021", 8, f"/SMask {soft}")
    raw("RGB 8 SMask", "RGB", 8, f"/SMask {soft}")
    soft_big = d.stream("/Type /XObject /Subtype /Image /Width 4 /Height 4 /ColorSpace /DeviceGray /BitsPerComponent 8", bytes(range(0, 256, 16)))
    raw("CMYK 8 SMask 4x4", "CMYK", 8, f"/SMask {soft_big}")
    # filters over CMYK 8
    data = pack(pixels(4), 8, 2)
    base = f"/Width 2 /Height 2 /ColorSpace /DeviceCMYK /BitsPerComponent 8"
    import base64
    rows += [("CMYK Flate", base, zlib.compress(data), "/FlateDecode", None),
             ("CMYK AHx", base, data.hex().encode() + b">", "/ASCIIHexDecode", None),
             ("CMYK A85", base, base64.a85encode(data) + b"~>", "/ASCII85Decode", None),
             ("CMYK LZW", base, lzw(data), "/LZWDecode", None),
             ("CMYK RL", base, runlength(data), "/RunLengthDecode", None),
             ("CMYK A85+Flate", base, base64.a85encode(zlib.compress(data)) + b"~>", "[/ASCII85Decode /FlateDecode]", None),
             ("CMYK Flate PNG", base + " /DecodeParms << /Predictor 12 /Colors 4 /BitsPerComponent 8 /Columns 2 >>",
              zlib.compress(predict_png(data, 8)), "/FlateDecode", None),
             ("CMYK Flate TIFF", base + " /DecodeParms << /Predictor 2 /Colors 4 /BitsPerComponent 8 /Columns 2 >>",
              zlib.compress(predict_tiff(data, 8, 4)), "/FlateDecode", None)]
    # DCT and JPX: 16x16, a sample an 8x8 block, flat so decoders agree
    for label, space, mode, n, extra in (("DCT Gray", "/DeviceGray", "L", 1, ""), ("DCT RGB", "/DeviceRGB", "RGB", 3, ""),
                                          ("DCT CMYK", "/DeviceCMYK", "CMYK", 4, ""),
                                          ("DCT CMYK Decode inv", "/DeviceCMYK", "CMYK", 4, "/Decode [1 0 1 0 1 0 1 0]"),
                                          ("DCT 021", cs[s021], "L", 1, "")):
        rows.append((label, f"/Width 16 /Height 16 /ColorSpace {space} /BitsPerComponent 8 {extra}",
                     encoded("JPEG", pixels(n, 8), 16, mode), "/DCTDecode", None))
    for label, space, mode, n in (("JPX Gray", "/DeviceGray", "L", 1), ("JPX RGB", "/DeviceRGB", "RGB", 3),
                                  ("JPX RGB own cs", None, "RGB", 3), ("JPX CMYK", "/DeviceCMYK", "CMYK", 4)):
        rows.append((label, f"/Width 16 /Height 16" + (f" /ColorSpace {space} /BitsPerComponent 8" if space else ""),
                     encoded("JPEG2000", pixels(n, 8), 16, mode), "/JPXDecode", None))
    g4 = encoded("G4", pixels(1, 8), 16, "L")
    rows += [("CCITT G4 Gray", "/Width 16 /Height 16 /ColorSpace /DeviceGray /BitsPerComponent 1"
              " /DecodeParms << /K -1 /Columns 16 /Rows 16 /BlackIs1 true >>", g4, "/CCITTFaxDecode", None),
             ("CCITT G4 021", f"/Width 16 /Height 16 /ColorSpace {cs[s021]} /BitsPerComponent 1"
              " /DecodeParms << /K -1 /Columns 16 /Rows 16 /BlackIs1 true >>", g4, "/CCITTFaxDecode", None),
             ("JBIG2 MMR Gray", "/Width 16 /Height 16 /ColorSpace /DeviceGray /BitsPerComponent 1",
              jbig2(g4, 16, 16), "/JBIG2Decode", None)]
    # image masks: painted in the fill color
    mask = bytes([0x80, 0x40])
    masks = [("ImageMask k", "0.2 0.4 0 0.6 k", ""), ("ImageMask k Decode", "0.2 0.4 0 0.6 k", "/Decode [1 0]"),
             ("ImageMask 021", f"/{s021} cs 0.8 scn", ""), ("ImageMask rgb", "1 0 0 rg", ""),
             ("ImageMask N", f"/{sn} cs 0.3 0.9 scn", ""), ("ImageMask g", "0.4 g", "")]
    ops = {}
    for label, fill, extra in masks:
        rows.append((label, f"/Width 2 /Height 2 /ImageMask true /BitsPerComponent 1 {extra}", mask, None, None))
        ops[label] = fill
    # inline images
    inline = {"inline CMYK AHx": f"BI /W 2 /H 2 /CS /CMYK /BPC 8 /F /AHx ID {data.hex()}> EI",
              "inline G 4 AHx": f"BI /W 2 /H 2 /CS /G /BPC 4 /F /AHx ID {pack(pixels(1), 4, 2).hex()}> EI",
              "inline Idx AHx": f"BI /W 2 /H 2 /CS /Ix /BPC 8 /F /AHx ID {pack([[i / 255] for i in (0, 1, 2, 3)], 8, 2).hex()}> EI",
              "inline 021 AHx": f"BI /W 2 /H 2 /CS /S021 /BPC 8 /F /AHx ID {pack(pixels(1), 8, 2).hex()}> EI",
              "inline mask k": f"0.2 0.4 0 0.6 k BI /W 2 /H 2 /IM true /F /AHx ID 8040> EI"}
    d.add("ColorSpace", "Ix", d.obj(spaces["IdxCMYK"]))
    d.rows = [r[0] for r in rows] + list(inline)
    base_ops = (f"0.3 0.3 0.3 0.3 k {box()}\nq /{op1} gs /{s021} cs 0.5 scn {box(0, 0, 15)}"
                f" /{s354} cs 0.5 scn {box(15, 0, 15)} Q")
    for r, (label, dict_body, data_, filt, smask) in enumerate(rows):
        f = f" /Filter {filt}" if filt else ""
        key = d.add("XObject", f"Im{r}", d.obj(f"<< /Type /XObject /Subtype /Image {dict_body}{f} /Length {len(data_)} >>\nstream\n".encode()
                                                + data_ + b"\nendstream"))
        for c, (_, g) in enumerate(IMAGE_MODES):
            d.cell(r, c, f"{base_ops}\nq {'/G%d gs ' % c if g else ''}{ops.get(label, '')} {CELL} 0 0 {CELL} 0 0 cm /{key} Do Q")
    for i, (label, op) in enumerate(inline.items()):
        for c, (_, g) in enumerate(IMAGE_MODES):
            d.cell(len(rows) + i, c, f"{base_ops}\nq {'/G%d gs ' % c if g else ''}{CELL} 0 0 {CELL} 0 0 cm {op} Q")
    return d


# ── 12 text: a word per box, ever bigger, clipped to its box ─────────────
# a TrueType font embedded, so both draw the same outlines
TEXT_SIZES = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 18, 24, 36, 48, 72, 120]
FONTS = {"Arial": "/System/Library/Fonts/Supplemental/Arial.ttf",
         "TimesNewRoman": "/System/Library/Fonts/Supplemental/Times New Roman.ttf"}


def font(d, key, path):
    """a simple TrueType font, WinAnsi, embedded whole"""
    from fontTools.ttLib import TTFont
    tt = TTFont(path)
    upm = tt["head"].unitsPerEm
    cmap, hmtx = tt.getBestCmap(), tt["hmtx"]
    widths = [round(hmtx[cmap[c]][0] * 1000 / upm) if c in cmap else 0 for c in range(32, 127)]
    h = tt["head"]
    data = open(path, "rb").read()
    ff = d.stream(f"/Length1 {len(data)}", data)
    desc = d.obj(f"<< /Type /FontDescriptor /FontName /{key} /Flags 32 /FontBBox [{h.xMin} {h.yMin} {h.xMax} {h.yMax}]"
                 f" /ItalicAngle 0 /Ascent 900 /Descent -200 /CapHeight 700 /StemV 80 /FontFile2 {ff} >>")
    d.add("Font", key, d.obj(f"<< /Type /Font /Subtype /TrueType /BaseFont /{key} /FirstChar 32 /LastChar 126"
                             f" /Widths [{' '.join(map(str, widths))}] /Encoding /WinAnsiEncoding /FontDescriptor {desc} >>"))


def text():
    d = Doc([], [f"{s}pt" for s in TEXT_SIZES])
    s021, _ = d.spots()
    for k, p in FONTS.items():
        font(d, f"T{k}", p)
    op1 = d.gs("OPT", "<< /OP true /op true /OPM 1 >>")
    # (label, font, how it paints, what is under it)
    rows = [("Arial K", "Arial", "0 0 0 1 k", ""),
            ("Times K", "TimesNewRoman", "0 0 0 1 k", ""),
            ("Arial C .6", "Arial", "0.6 0 0 0 k", ""),
            ("Arial 021", "Arial", f"/{s021} cs 1 scn", ""),
            ("Arial K OP over 021 .5", "Arial", f"/{op1} gs 0 0 0 1 k", f"/{s021} cs 0.5 scn {box()}"),
            ("Arial white on CMYK .3", "Arial", "0 0 0 0 k", f"0.3 0.3 0.3 0.3 k {box()}"),
            ("Arial K stroked .25pt", "Arial", "0 0 0 1 K 0.25 w 1 Tr", "")]
    d.rows = [r[0] for r in rows]
    for r, (label, f, paint, under) in enumerate(rows):
        for c, size in enumerate(TEXT_SIZES):
            # baseline a fifth up the box, 2pt in: the bigger, the more is cut
            d.cell(r, c, f"{under}\nq 0 0 {CELL} {CELL} re W n {paint} BT /T{f} {size} Tf 2 {CELL / 5} Td (Rag) Tj ET Q")
    return d


# ── 13 text overprinted: 07-overprint's cases as text, ever bigger ───────
# each box the base of 07 (CMYK .3, 021 .5 overprinted), then "Rag" painted
# as the row says: knocked out, its plates must keep exactly the glyph's
# pixels out (no paper showing round it); overprinted, the others untouched
TEXT_OP_SIZES = [2, 3, 4, 6, 8, 10, 12, 18, 24, 48]


def text_overprint():
    d = Doc([], [f"{s}pt" for s in TEXT_OP_SIZES])
    s021, s354 = d.spots()
    sn = d.devicen()
    font(d, "TArial", FONTS["Arial"])
    none = d.add("ColorSpace", "SNone", d.obj("[/Separation /None /DeviceCMYK << /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [0 0 0 1] /N 1 >>]"))
    allcs = d.add("ColorSpace", "SAll", d.obj("[/Separation /All /DeviceCMYK << /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [1 1 1 1] /N 1 >>]"))
    op1 = d.gs("OP1", "<< /OP true /op true /OPM 1 >>")
    op0 = d.gs("OP0", "<< /OP true /op true /OPM 0 >>")
    sop = d.gs("SOP", "<< /OP true /op true /OPM 1 >>")
    rows = [("knockout k K", None, "0 0 0 1 k"),
            ("knockout rgb red", None, "1 0 0 rg"),
            ("knockout white", None, "0 0 0 0 k"),
            ("knockout 354", None, f"/{s354} cs 1 scn"),
            ("OPM0 k K", op0, "0 0 0 1 k"),
            ("OPM1 k K", op1, "0 0 0 1 k"),
            ("OPM1 k C.5", op1, "0.5 0 0 0 k"),
            ("OPM1 k M Y red", op1, "0 1 1 0 k"),
            ("OPM1 g", op1, "0 g"),
            ("OPM1 rgb red", op1, "1 0 0 rg"),
            ("OP 354", op1, f"/{s354} cs 1 scn"),
            ("OP 021 same", op1, f"/{s021} cs 1 scn"),
            ("OP DeviceN", op1, f"/{sn} cs 0 1 scn"),
            ("OP None", op1, f"/{none} cs 1 scn"),
            ("OP All .5", op1, f"/{allcs} cs 0.5 scn"),
            ("OPM1 K stroked .3pt", sop, "0 0 0 1 K 0.3 w 1 Tr"),
            ("OPM1 K fill+stroke .3pt", sop, "0 0 0 1 k 0 0 0 1 K 0.3 w 2 Tr"),
            ("knockout K stroked .3pt", None, "0 0 0 1 K 0.3 w 1 Tr")]
    d.rows = [r[0] for r in rows]
    base = f"0.3 0.3 0.3 0.3 k {box()}\nq /{op1} gs /{s021} cs 0.5 scn {box()} Q"
    for r, (label, g, paint) in enumerate(rows):
        for c, size in enumerate(TEXT_OP_SIZES):
            d.cell(r, c, f"{base}\nq 0 0 {CELL} {CELL} re W n {'/%s gs ' % g if g else ''}{paint}"
                         f" BT /TArial {size} Tf 2 {CELL / 5} Td (Rag) Tj ET Q")
    return d



# ── 14 knockout groups: two objects overlapping in a form, per group kind ─
# A (cyan) on the left two thirds, B (354) on the right two thirds, over a
# base of Y .5 with 021 .5 on its left half. In a knockout group B is
# composited on the group's backdrop alone: where they overlap A is gone
KO_GROUPS = [("no group", "", ""), ("group", "/Group << /S /Transparency >>", "/A gs"),
             ("knockout", "/Group << /S /Transparency /K true >>", "/A gs"),
             ("isolated knockout", "/Group << /S /Transparency /I true /K true >>", "/A gs"),
             ("knockout opaque", "/Group << /S /Transparency /K true >>", ""),
             ("knockout Multiply", "/Group << /S /Transparency /K true >>", "/M gs"),
             ("knockout ca .5 Multiply", "/Group << /S /Transparency /K true >>", "/AM gs"),
             ("knockout overprint", "/Group << /S /Transparency /K true >>", "/O gs"),
             ("group ca .5 overprint", "/Group << /S /Transparency >>", "/AO gs")]


def knockout():
    d = Doc([], [g[0] for g in KO_GROUPS])
    s021, s354 = d.spots()
    font(d, "TArial", FONTS["Arial"])
    cs = d.res["ColorSpace"]
    gstates = {"A": "<< /ca 0.5 /CA 0.5 >>", "M": "<< /BM /Multiply >>", "AM": "<< /ca 0.5 /CA 0.5 /BM /Multiply >>",
               "O": "<< /OP true /op true /OPM 1 >>", "AO": "<< /ca 0.5 /CA 0.5 /OP true /op true /OPM 1 >>"}
    gs_refs = {k: d.obj(v) for k, v in gstates.items()}
    img_c = d.stream("/Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceCMYK /BitsPerComponent 8",
                     bytes([255, 0, 0, 0] * 4))
    img_s = d.stream(f"/Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace {cs[s354]} /BitsPerComponent 8",
                     bytes([255] * 4))
    mask = d.stream("/Type /XObject /Subtype /Image /Width 2 /Height 2 /ImageMask true /BitsPerComponent 1",
                    bytes([0x00, 0x00]))  # 0 paints: all four pixels
    res = (f"<< /ExtGState << {' '.join(f'/{k} {v}' for k, v in gs_refs.items())} >>"
           f" /ColorSpace << /S354 {cs[s354]} >> /Font << /TArial {d.res['Font']['TArial']} >>"
           f" /XObject << /IC {img_c} /IS {img_s} /IM {mask} >> >>")
    fill = {"A": "1 0 0 0 k", "B": "/S354 cs 1 scn"}
    stroke = {"A": "1 0 0 0 K", "B": "/S354 CS 1 SCN"}
    image = {"A": "IC", "B": "IS"}

    def obj(kind, who, x):
        return {"rect": f"{fill[who]} {x} 4 20 22 re f",
                "path": f"{fill[who]} {x} 4 m {x + 20} 4 l {x + 10} 27 l h f",
                "stroke": f"{stroke[who]} 5 w {x + 3} 6 m {x + 17} 24 l {x + 3} 24 l S",
                "text": f"{fill[who]} BT /TArial 30 Tf {x - 1} 5 Td (H) Tj ET",
                "image": f"q 20 0 0 22 {x} 4 cm /{image[who]} Do Q",
                "imagemask": f"{fill[who]} q 20 0 0 22 {x} 4 cm /IM Do Q"}[kind]

    pairs = [("rect", "rect"), ("path", "path"), ("stroke", "stroke"), ("text", "text"), ("image", "image"),
             ("imagemask", "imagemask"), ("rect", "text"), ("text", "rect"), ("rect", "image"), ("image", "rect"),
             ("stroke", "rect"), ("rect", "stroke"), ("image", "text")]
    d.rows = [f"{a} then {b}" for a, b in pairs]
    base = f"0 0 0.5 0 k {box()} /{s021} cs 0.5 scn {box(0, 0, 15)}"
    for r, (a, b) in enumerate(pairs):
        for c, (label, group, gstate) in enumerate(KO_GROUPS):
            ops = f"{gstate} {obj(a, 'A', 0)}\n{obj(b, 'B', 10)}"
            f = d.add("XObject", f"K{r}_{c}", d.stream(
                f"/Type /XObject /Subtype /Form /BBox [0 0 {CELL} {CELL}] /Resources {res} {group}", ops.encode()))
            d.cell(r, c, f"{base}\n/{f} Do")
    return d


# ── 15 non-separable blend modes (and the separable ones 02 lacks) ───────
# A source over the right half of a backdrop, per mode, drawn plain, at ca
# .5, as an isolated group's content, as a non-isolated group's, as a
# knockout group's, and as a group composited with the mode
NS_MODES = ["Hue", "Saturation", "Color", "Luminosity"]
NS_WAYS = ["plain", "ca .5", "in group", "in isolated", "in knockout", "group BM", "in group ca .5",
           "in isolated ca .5", "in isolated on own"]


def nonseparable():
    d = Doc([], [])
    s021, s354 = d.spots()
    backdrops = [("CMYK", "0.6 0.3 0.1 0.2 k"), ("K", "0 0 0 0.5 k"), ("RGB", "0.2 0.6 0.9 rg"),
                 ("Y + 021", f"0 0 0.5 0 k {box()} /{s021} cs 0.5 scn")]
    sources = [("CMYK", "0.1 0.7 0.4 0.1 k"), ("354", f"/{s354} cs 0.7 scn"), ("RGB", "0.9 0.3 0.2 rg"),
               ("K", "0 0 0 0.8 k")]
    cs = d.res["ColorSpace"]
    d.rows = [f"{s} over {b}" for b, _ in backdrops for s, _ in sources]
    d.cols = [f"{m} {w}" for m in NS_MODES for w in NS_WAYS] + ["SoftLight", "ColorDodge"]
    gs = {}
    for m in NS_MODES + ["SoftLight", "ColorDodge"]:
        gs[m] = d.gs(f"B{m}", f"<< /BM /{m} >>")
        gs[m + "A"] = d.gs(f"B{m}A", f"<< /BM /{m} /ca 0.5 >>")
    groups = {"in group": "/Group << /S /Transparency >>", "in isolated": "/Group << /S /Transparency /I true >>",
              "in knockout": "/Group << /S /Transparency /K true >>", "group BM": "/Group << /S /Transparency >>",
              "in group ca .5": "/Group << /S /Transparency >>",
              "in isolated ca .5": "/Group << /S /Transparency /I true >>",
              "in isolated on own": "/Group << /S /Transparency /I true >>"}
    half = d.gs("H", "<< /ca 0.5 >>")
    n = 0
    for r, ((bl, bops), (sl, sops)) in enumerate((b, s) for b in backdrops for s in sources):
        for c, col in enumerate(d.cols):
            m, way = (col.split(" ", 1) + ["plain"])[:2]
            src = f"{sops} {box(15, 0, 15)}"
            if way == "plain":
                top = f"q /{gs[m]} gs {src} Q"
            elif way == "ca .5":
                top = f"q /{gs[m + 'A']} gs {src} Q"
            else:
                inner = src if way == "group BM" else f"/G gs {src}"
                if way == "in isolated on own":
                    # the backdrop inside the group too: the blend sees it
                    inner = f"{bops.replace(f'/{s021}', '/S021')} {box()} {inner}"
                n += 1
                f = d.add("XObject", f"N{n}", d.stream(
                    f"/Type /XObject /Subtype /Form /BBox [0 0 {CELL} {CELL}] {groups[way]}"
                    f" /Resources << /ExtGState << /G {d.res['ExtGState'][gs[m]]} >>"
                    f" /ColorSpace << /{s354} {cs[s354]} /S021 {cs[s021]} >> >>", inner.encode()))
                top = (f"q /{gs[m]} gs /{f} Do Q" if way == "group BM" else
                       f"q /{half} gs /{f} Do Q" if way.endswith("ca .5") else f"/{f} Do")
            d.cell(r, c, f"{bops} {box()}\n{top}", half=True)
    return d


# ── 16 overprint in transparency groups ──────────────────────────────────
# 07-overprint's sources (OPM 1 K, C .5, a spot, DeviceN) inside a group,
# per group kind, over a base on five inks: overprint composites in the
# group (ISO 32000-2 11.7.4.3, CompatibleOverprint)
OP_GROUPS = [("group", "/Group << /S /Transparency >>", ""), ("isolated", "/Group << /S /Transparency /I true >>", ""),
             ("knockout", "/Group << /S /Transparency /K true >>", ""),
             ("group ca .5", "/Group << /S /Transparency >>", "/A gs "),
             ("group Multiply", "/Group << /S /Transparency >>", "/M gs "),
             ("ca .5 inside", "/Group << /S /Transparency >>", "")]


def overprint_groups():
    d = Doc([], [g[0] for g in OP_GROUPS])
    s021, s354 = d.spots()
    sn = d.devicen()
    cs = d.res["ColorSpace"]
    op = d.gs("OP1", "<< /OP true /op true /OPM 1 >>")
    opa = d.gs("OP1A", "<< /OP true /op true /OPM 1 /ca 0.5 >>")
    a = d.gs("A", "<< /ca 0.5 >>")
    m = d.gs("M", "<< /BM /Multiply >>")
    sources = [("OPM1 k K", "0 0 0 1 k"), ("OPM1 k C.5", "0.5 0 0 0 k"), ("OP 354", "/S354 cs 1 scn"),
               ("OP 021 same", "/S021 cs 1 scn"), ("OP DeviceN", "/SN cs 0 1 scn"), ("knockout k", None)]
    d.rows = [s[0] for s in sources]
    res = (f"<< /ExtGState << /OP {d.res['ExtGState'][op]} /OPA {d.res['ExtGState'][opa]} >>"
           f" /ColorSpace << /S021 {cs[s021]} /S354 {cs[s354]} /SN {cs[sn]} >> >>")
    base = f"0.3 0.3 0.3 0.3 k {box()}\nq /{op} gs /{s021} cs 0.5 scn {box()} Q"
    for r, (label, ops) in enumerate(sources):
        for c, (glabel, group, state) in enumerate(OP_GROUPS):
            g = "/OPA gs" if glabel == "ca .5 inside" else "/OP gs"
            inner = f"{g} {ops or '0 0 0 1 k'} {box(15, 0, 15)}" if ops else f"0 0 0 1 k {box(15, 0, 15)}"
            f = d.add("XObject", f"O{r}_{c}", d.stream(
                f"/Type /XObject /Subtype /Form /BBox [0 0 {CELL} {CELL}] /Resources {res} {group}", inner.encode()))
            d.cell(r, c, f"{base}\nq {state.replace('/A ', f'/{a} ').replace('/M ', f'/{m} ')}/{f} Do Q", half=True)
    return d


# ── 17 spot discovery: spots only found inside other resources ──────────
# each cell paints one spot that only a pattern, a shading, a Type3 glyph, a
# nested form, an indexed image, an annotation's appearance or a DeviceN
# names: the plates found (PDFKING_GetPageInks) must be gs's. gs does not
# look in a Type3 font's own resources: it drops that glyph, and its spot
def spot_discovery():
    d = Doc(["found in"], ["tiling pattern", "shading", "Type3 glyph", "form in form", "indexed image",
                           "annotation", "DeviceN"])

    def sep(n, alt):
        return d.obj(f"[/Separation {name(n)} /DeviceCMYK << /FunctionType 2 /Domain [0 1]"
                     f" /C0 [0 0 0 0] /C1 [{alt}] /N 1 >>]")

    form = lambda key, ops, res: d.add("XObject", key, d.stream(
        f"/Type /XObject /Subtype /Form /BBox [0 0 {CELL} {CELL}] /Resources << {res} >>", ops.encode()))
    tile = d.stream(f"/PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 10 10] /XStep 10 /YStep 10"
                    f" /Resources << /ColorSpace << /T {sep('Spot Tile', '1 0 0 0')} >> >>",
                    b"/T cs 1 scn 0 0 5 5 re f")
    d.add("Pattern", "PT", tile)
    sx, sy = d.xy(0, 1)  # pattern space is the page's: the cell's corner
    d.add("Pattern", "PS", d.obj(f"<< /PatternType 2 /Matrix [1 0 0 1 {sx} {sy}]"
                                 f" /Shading << /ShadingType 2 /ColorSpace {sep('Spot Shade', '0 1 0 0')}"
                                 f" /Coords [0 0 {CELL} 0] /Function << /FunctionType 2 /Domain [0 1] /C0 [0] /C1 [1] /N 1 >> >> >>"))
    glyph = d.stream("", b"0 0 d0 /G cs 1 scn 0 0 1000 1000 re f")
    t3 = d.obj(f"<< /Type /Font /Subtype /Type3 /FontBBox [0 0 1000 1000] /FontMatrix [0.001 0 0 0.001 0 0]"
               f" /CharProcs << /a {glyph} >> /Encoding << /Differences [97 /a] >> /FirstChar 97 /LastChar 97"
               f" /Widths [1000] /Resources << /ColorSpace << /G {sep('Spot Glyph', '0 0 1 0')} >> >> >>")
    d.add("Font", "T3", t3)
    inner = form("In", "/F cs 1 scn 0 0 30 30 re f", f"/ColorSpace << /F {sep('Spot Form', '0 0 0 1')} >>")
    outer = form("Out", "/Inner Do", f"/XObject << /Inner {d.res['XObject'][inner]} >>")
    img = d.image("Idx", 2, 1, f"[/Indexed {sep('Spot Index', '1 1 0 0')} 1 <00ff>]", 8, bytes([1, 1]))
    dn = d.add("ColorSpace", "DN", d.obj(
        f"[/DeviceN [{name('Spot N1')} {name('Spot N2')} /Cyan] /DeviceCMYK"
        f" {d.stream('/FunctionType 4 /Domain [0 1 0 1 0 1] /Range [0 1 0 1 0 1 0 1]', b'{add exch pop 0 0 0}', compress=False)}]"))
    ops = ["/Pattern cs /PT scn " + box(), "/Pattern cs /PS scn " + box(), "BT /T3 30 Tf 0 0 Td (a) Tj ET",
           f"/{outer} Do", f"30 0 0 30 0 0 cm /{img} Do", "", f"/{dn} cs 1 0.5 0 scn " + box()]
    for c, op in enumerate(ops):
        d.cell(0, c, op)
    # an annotation, printed: its appearance's spot
    x, y = d.xy(0, 5)
    ap = d.stream(f"/Type /XObject /Subtype /Form /BBox [0 0 {CELL} {CELL}]"
                  f" /Resources << /ColorSpace << /A {sep('Spot Annot', '1 0 1 0')} >> >>", b"/A cs 1 scn 0 0 30 30 re f")
    d.annots = [f"<< /Type /Annot /Subtype /Square /F 4 /Rect [{x} {y} {x + CELL} {y + CELL}] /AP << /N {ap} >> >>"]
    return d


TESTS = [("00-sheet", sheet), ("01-tints", tints), ("02-blends", blends), ("03-rgb-gray-cie-vectors", rgb_vectors),
         ("04-images-process-gray", images_process), ("05-images-rgb", images_rgb), ("06-images-spot", images_spot),
         ("07-overprint", overprint), ("08-shadings", shadings), ("09-groups-softmasks", groups), ("10-icc", icc), ("11-images", images), ("12-text", text), ("13-text-overprint", text_overprint), ("14-knockout", knockout),
         ("15-nonseparable", nonseparable), ("16-overprint-groups", overprint_groups),
         ("17-spot-discovery", spot_discovery)]

if __name__ == "__main__":
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for test, build in TESTS:
        doc = build()
        doc.write(f"{out}/{test}")
        print(test, doc.W, doc.H)
