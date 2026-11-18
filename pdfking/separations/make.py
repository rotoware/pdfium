"""Separation test PDFs, one topic each, on one pixel-aligned grid: cells of
30pt with 6pt gaps, every edge on whole points (at 144dpi a cell is 60x60px).
Each PDF comes with <name>.json: its cells, row and column labels.
usage: make.py <dir>"""
import json
import os
import struct
import sys
import zlib

CELL, GAP, MARGIN = 30, 6, 20
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
        self.res = {"ColorSpace": {}, "ExtGState": {}, "XObject": {}, "Shading": {}, "Pattern": {}}
        self.content = []
        self.cells = []
        self.W = 2 * MARGIN + len(cols) * CELL + (len(cols) - 1) * GAP
        self.H = 2 * MARGIN + len(rows) * CELL + (len(rows) - 1) * GAP

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

    def xy(self, r, c):
        return MARGIN + c * (CELL + GAP), self.H - MARGIN - CELL - r * (CELL + GAP)

    def cell(self, r, c, ops, half=False, row=None, col=None):
        x, y = self.xy(r, c)
        self.content.append(f"q 1 0 0 1 {x} {y} cm\n{ops}\nQ\n")
        self.cells.append({"row": row or self.rows[r], "col": col or self.cols[c], "x": x, "y": y, "half": half})

    def write(self, path):
        page_res = " ".join(f"/{k} << " + " ".join(f"/{n} {v}" for n, v in d.items()) + " >>"
                            for k, d in self.res.items() if d)
        content = "".join(self.content).encode()
        contents = self.stream("", content)
        page = self.obj(f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {self.W} {self.H}]"
                        f" /Contents {contents} /Resources << {page_res} >> >>")
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
        json.dump({"W": self.W, "H": self.H, "cell": CELL, "cells": self.cells}, open(path + ".json", "w"))

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
    d.H = 2 * MARGIN + len(d.rows) * CELL + (len(d.rows) - 1) * GAP
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
    d.H = 2 * MARGIN + len(d.rows) * CELL + (len(d.rows) - 1) * GAP
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
    d.W = 2 * MARGIN + len(d.cols) * CELL + (len(d.cols) - 1) * GAP
    d.H = 2 * MARGIN + CELL
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
    d.W = 2 * MARGIN + 5 * CELL + 4 * GAP
    d.H = 2 * MARGIN + len(d.rows) * CELL + (len(d.rows) - 1) * GAP
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
    cases = [("plain", form("F0", inner, "")),
             ("group", form("F1", inner, "/Group << /S /Transparency >>")),
             ("isolated", form("F2", inner, "/Group << /S /Transparency /I true >>")),
             ("knockout", form("F3", inner, "/Group << /S /Transparency /K true >>")),
             ("group Multiply", form("F4", f"/Mul gs {inner}", "/Group << /S /Transparency >>")),
             ("smask luminosity", form("F5", f"0 0 0 1 k {box()}", "")),
             ("smask alpha", form("F6", f"0 0 0 1 k {box()}", ""))]
    d.rows, d.cols = ["over Y .5 + 021 .5"], [c[0] for c in cases]
    d.W = 2 * MARGIN + len(d.cols) * CELL + (len(d.cols) - 1) * GAP
    d.H = 2 * MARGIN + CELL
    base = f"0 0 0.5 0 k {box()} /{s021} cs 0.5 scn {box(0, 0, 15)}"
    for c, (label, f) in enumerate(cases):
        mask = {"smask luminosity": f"/{smask_l} gs ", "smask alpha": f"/{smask_a} gs "}.get(label, "")
        d.cell(0, c, f"{base}\nq {mask}/{f} Do Q")
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
    d.H = 2 * MARGIN + len(d.rows) * CELL + (len(d.rows) - 1) * GAP
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


TESTS = [("00-sheet", sheet), ("01-tints", tints), ("02-blends", blends), ("03-rgb-gray-cie-vectors", rgb_vectors),
         ("04-images-process-gray", images_process), ("05-images-rgb", images_rgb), ("06-images-spot", images_spot),
         ("07-overprint", overprint), ("08-shadings", shadings), ("09-groups-softmasks", groups), ("10-icc", icc)]

if __name__ == "__main__":
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for test, build in TESTS:
        doc = build()
        doc.write(f"{out}/{test}")
        print(test, doc.W, doc.H)
