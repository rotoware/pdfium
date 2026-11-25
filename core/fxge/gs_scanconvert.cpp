// pdfking: Ghostscript's edge-buffer scan converter, ported from
// Ghostscript 10.08 base/gxscanc.c, together with what gs's
// gx_general_fill_path (base/gxfill.c) runs around it for a normal opaque
// fill: path building (gxpath.c), the accurate_curves flattening pre-pass
// (gx_path_copy_reducing, gxpcopy.c / gxpflat.c) and the default trapezoid
// filler the _tr variants call (gx_default_fill_trapezoid, gdevddrw.c /
// gxdtfill.h).
//
// Copyright (C) 2001-2025 Artifex Software, Inc.
// Licensed under the GNU Affero General Public License (AGPL) v3; see
// https://www.gnu.org/licenses/agpl-3.0.html. This file is AGPL code, not
// under the PDFium BSD license.
//
// The block between "BEGIN gxscanc.c" and "END gxscanc.c" is base/gxscanc.c
// lines 138-4520 (all four scanline / trapezoid converters), kept verbatim
// so it can be diffed against the original. The only changes in it:
//  - make_table_template allocates index/table with std::vector (owned by the
//    gx_edgebuffer, passed as an extra parameter through make_table*)
//    instead of gs_alloc_bytes/gs_free_object.
//  - mark_line_app's "goto endFalling" is "goto end" (C++ forbids the jump
//    past initialisations; the two labels were adjacent).
// The other gs functions below are copied the same way, with gs's names;
// deviations are marked "pdfking:".

#ifdef UNSAFE_BUFFERS_BUILD
// Verbatim Ghostscript C code: raw pointer arithmetic and libc calls.
#pragma allow_unsafe_buffers
#pragma allow_unsafe_libc_calls
#endif

#include "core/fxge/gs_scanconvert.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <deque>
#include <vector>

namespace gs_scan {

namespace {

/* ---------------- std.h / stdpre.h / gx.h ---------------- */

typedef unsigned int uint;
typedef unsigned short ushort;
#define min_int INT_MIN
#define max_int INT_MAX
#define arith_rshift(x, n) ((x) >> (n))
#define arith_rshift_1(x) ((x) >> 1)
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define size_of(x) ((int)(sizeof(x)))
#define gs_restrict __restrict
#define DO_NOTHING do {} while (0)
#define BEGIN do {
#define END } while (0)
/* Debug tracing (gdebug.h) and interrupt polling (gpcheck.h) compile out. */
#define if_debug0(...) DO_NOTHING
#define if_debug1(...) DO_NOTHING
#define if_debug2(...) DO_NOTHING
#define if_debug3(...) DO_NOTHING
#define if_debug4(...) DO_NOTHING
#define if_debug5(...) DO_NOTHING
#define if_debug6(...) DO_NOTHING
#define if_debug2m(...) DO_NOTHING
#define trace_segment(...) DO_NOTHING
#define return_if_interrupt(mem) DO_NOTHING

/* ---------------- gserrors.h ---------------- */

enum {
    gs_error_unregistered = -28,
    gs_error_VMerror = -25,
    gs_error_rangecheck = -15,
    gs_error_nocurrentpoint = -14
};
#define gs_note_error(e) (e)
#define return_error(e) return gs_note_error(e)

/* ---------------- gxfixed.h (fixed is int, ARCH_SIZEOF_INT == 4) ---------------- */

typedef uint ufixed; /* only used in a very few places */
#define max_fixed max_int
#define min_fixed min_int
#define fixed_0 0L
#define fixed_epsilon 1L
#define _fixed_shift 8
#define fixed_fraction_bits _fixed_shift
#define fixed_scale (1<<_fixed_shift)
#define _fixed_rshift(x) arith_rshift(x,_fixed_shift)
#define _fixed_round_v (fixed_scale>>1)
#define _fixed_fraction_v (fixed_scale-1)
#define _fixed_pixround_v (_fixed_round_v - fixed_epsilon)
#define int2fixed(i) ((fixed)(i)<<_fixed_shift)
#define fixed_1 (fixed_epsilon << _fixed_shift)
#define fixed_half (fixed_1 >> 1)
#define fixed2int(x) ((int)_fixed_rshift(x))
#define fixed2int_var(x) fixed2int(x)
#define fixed_is_int(x) !((x)&_fixed_fraction_v)
#define float2fixed(f) ((fixed)((f)*(float)fixed_scale))
#define fixed_floor(x) ((x)&(-(1L<<_fixed_shift)))
#define fixed_ceiling(x) (((x)+_fixed_fraction_v)&(-(1L<<_fixed_shift)))
#define fixed_pixround(x) (((x)+_fixed_pixround_v)&(-(1L<<_fixed_shift)))
#define max_int_in_fixed fixed2int(max_fixed)

/* ---------------- gxarith.h ---------------- */

#define any_abs(x) ((x) < 0 ? -(x) : (x))

/* ---------------- gsmisc.c: fixed_mult_quo, double variant (selected for
 * 32-bit fixed: ROUND_BITS 11 < MAX_OTHER_FACTOR_BITS - 1 = 20) ---------------- */

#define MAX_OTHER_FACTOR_BITS\
  (53 /* ARCH_DOUBLE_MANTISSA_BITS */ - 4 /* ARCH_SIZEOF_FIXED */ * 8)
#define ROUND_BITS\
  (4 /* ARCH_SIZEOF_FIXED */ * 8 * 2 - 53 /* ARCH_DOUBLE_MANTISSA_BITS */)

fixed
fixed_mult_quo(fixed signed_A, fixed B, fixed C)
{
    /*
     * Check whether A * B will fit in the mantissa of a double.
     */
#define MAX_OTHER_FACTOR (1L << MAX_OTHER_FACTOR_BITS)
    if (B < MAX_OTHER_FACTOR || any_abs(signed_A) < MAX_OTHER_FACTOR) {
#undef MAX_OTHER_FACTOR
        /*
         * The product fits, so a straightforward double computation
         * will be exact.
         */
        return (fixed)floor((double)signed_A * B / C);
    } else {
        /*
         * The product won't fit.  However, the approximate product will
         * only be off by at most +/- 1/2 * (1 << ROUND_BITS) because of
         * rounding.  If we add 1 << ROUND_BITS to the value of the product
         * (i.e., 1 in the least significant bit of the mantissa), the
         * result is always greater than the correct product by between 1/2
         * and 3/2 * (1 << ROUND_BITS).  We know this is less than C:
         * because of the 'if' just above, we know that B >=
         * MAX_OTHER_FACTOR; since B <= C, we know C >= MAX_OTHER_FACTOR;
         * and because of the #if that chose between the two
         * implementations, we know that C >= 2 * (1 << ROUND_BITS).  Hence,
         * the quotient after dividing by C will be at most 1 too large.
         */
        fixed q =
            (fixed)floor(((double)signed_A * B + (1L << ROUND_BITS)) / C);

        /*
         * Compute the remainder R.  If the quotient was correct,
         * 0 <= R < C.  If the quotient was too high, -C <= R < 0.
         */
        if (signed_A * B - q * C < 0)
            --q;
        return q;
    }
}

#undef MAX_OTHER_FACTOR_BITS
#undef ROUND_BITS

/* ---------------- gsropt.h ---------------- */

typedef uint gs_logical_operation_t;
#define rop3_T 0xf0
#define rop3_default rop3_T
/* source_transparent_default and pattern_transparent_default are false */
#define lop_default rop3_default

/* ---------------- gxpath.h / gzpath.h ---------------- */

#define gx_rule_winding_number (-1)
#define gx_rule_even_odd 1

typedef struct gs_fixed_point_s {
    fixed x, y;
} gs_fixed_point;

typedef struct gs_fixed_rect_s {
    gs_fixed_point p, q;
} gs_fixed_rect;

typedef struct gs_fixed_edge_s {
    gs_fixed_point start;
    gs_fixed_point end;
} gs_fixed_edge;

typedef enum {
    s_start,
    s_line,
    s_line_close,
    s_curve,
    s_dash,                     /* only for internal use of the stroking algorithm */
    s_gap
} segment_type;

typedef ushort segment_notes;
#define sn_none 0
#define sn_not_first 1

/* gs has one struct per segment type sharing a common head (prev, next,
 * type, notes, pt); here one struct carries the fields of all of them:
 * subpath (last, curve_count, is_closed), line_close_segment (sub),
 * curve_segment (p1, p2). */
typedef struct segment_s segment;
struct segment_s {
    segment *prev;
    segment *next;
    segment_type type;
    segment_notes notes;
    gs_fixed_point pt;          /* initial point for starts, */
                                /* final point for others */
    /* subpath */
    segment *last;              /* last segment of subpath, */
                                /* points back to here if empty */
    int curve_count;            /* # of curves */
    bool is_closed;             /* true if subpath is closed */
    /* line_close_segment */
    segment *sub;
    /* curve_segment */
    gs_fixed_point p1, p2;
};
typedef segment subpath;
typedef segment line_segment;
typedef segment line_close_segment;
typedef segment curve_segment;

typedef enum {
    /* Individual flags.  These may be or'ed together, per above. */
    psf_position_valid = 1,
    psf_subpath_open = 2,
    psf_is_drawing = 4,
    psf_outside_range = 8,
    /* Values stored by path building operations. */
    psf_last_newpath = 0,
    psf_last_moveto = psf_position_valid | psf_subpath_open,
    psf_last_draw = psf_position_valid | psf_subpath_open | psf_is_drawing,
    psf_last_closepath = psf_position_valid
} gx_path_state_flags;

/* pdfking: gs's gx_path minus sharing, bbox caching (bbox_set,
 * bbox_accurate, box_last) and the procs vector; segments are stored in a
 * deque (stable addresses) instead of being gs-allocated. */
typedef struct gx_path_s {
    std::deque<segment> storage;
    subpath *first_subpath;
    subpath *current_subpath;
    int subpath_count;
    int curve_count;
    gs_fixed_point position;    /* current position */
    int state_flags;            /* gx_path_state_flags */
} gx_path;

#define path_position_valid(ppath)\
  (((ppath)->state_flags & psf_position_valid) != 0)
#define path_subpath_open(ppath)\
  (((ppath)->state_flags & psf_subpath_open) != 0)
#define path_is_drawing(ppath)\
  (((ppath)->state_flags & psf_is_drawing) != 0)
#define path_last_is_moveto(ppath)\
  (((ppath)->state_flags & ~psf_outside_range) == psf_last_moveto)
#define path_update_moveto(ppath)\
  ((ppath)->state_flags = psf_last_moveto)
#define path_update_draw(ppath)\
  ((ppath)->state_flags = psf_last_draw)
#define path_update_closepath(ppath)\
  ((ppath)->state_flags = psf_last_closepath)
#define gx_path_has_curves(ppath)\
  ((ppath)->curve_count != 0)

#define curve_points_to_coefficients(v0, v1, v2, v3, a, b, c, t01, t12)\
  (/*d = (v0),*/\
   t01 = (v1) - (v0), c = (t01 << 1) + t01,\
   t12 = (v2) - (v1), b = (t12 << 1) + t12 - c,\
   a = (v3) - b - c - (v0))
#define k_sample_max min((size_of(int) * 8 - 1) / 3, 10)

typedef struct gx_flattened_iterator_s gx_flattened_iterator;
struct gx_flattened_iterator_s {
    /* private : */
    fixed x0, y0, x3, y3;
    fixed cx, bx, ax, cy, by, ay;
    fixed x, y;
    uint i, k;
    uint rmask;                 /* M-1 */
    fixed idx, idy, id2x, id2y, id3x, id3y;     /* I */
    uint rx, ry, rdx, rdy, rd2x, rd2y, rd3x, rd3y;      /* R */
    /* public : */
    bool curve;
    fixed lx0, ly0, lx1, ly1;
};

/* ---------------- gxpath.c: path building ---------------- */

/* pdfking: replaces path_alloc_segment's gs_alloc_struct. */
static segment *
path_alloc(gx_path *ppath, segment_type seg_type, segment_notes notes)
{
    segment *pseg = &ppath->storage.emplace_back();

    pseg->type = seg_type;
    pseg->notes = notes;
    pseg->next = 0;
    return pseg;
}

static inline void path_alloc_link(segment *pseg, subpath *psub)
{
    segment *prev = psub->last;
    prev->next = (segment *)pseg;
    pseg->prev = prev;
    psub->last = (segment *)pseg;
}

#define path_set_point(pseg, fx, fy)\
        (pseg)->pt.x = ppath->position.x = (fx),\
        (pseg)->pt.y = ppath->position.y = (fy)

int
gx_path_new_subpath(gx_path * ppath)
{
    subpath *psub = ppath->current_subpath;
    subpath *spp = path_alloc(ppath, s_start, sn_none);

    spp->last = (segment *) spp;
    spp->curve_count = 0;
    spp->is_closed = 0;
    spp->pt = ppath->position;
    if (!psub) {		/* first subpath */
        ppath->first_subpath = spp;
        spp->prev = 0;
    } else {
        segment *prev = psub->last;

        prev->next = (segment *) spp;
        spp->prev = prev;
    }
    ppath->current_subpath = spp;
    ppath->subpath_count++;
    trace_segment("[P]", ppath->memory, (const segment *)spp);
    return 0;
}

static inline int path_open(gx_path *ppath)
{
    int code = 0;
    if ( !path_is_drawing(ppath) ) {
      if ( path_position_valid(ppath) ) {
          code = gx_path_new_subpath(ppath);
      }
      else {
          code = gs_note_error(gs_error_nocurrentpoint);
      }
    }
    return code;
}

/* gz_path_add_point */
int
gx_path_add_point(gx_path * ppath, fixed x, fixed y)
{
    ppath->position.x = x;
    ppath->position.y = y;
    path_update_moveto(ppath);
    return 0;
}

/* gz_path_add_line_notes */
int
gx_path_add_line_notes(gx_path * ppath, fixed x, fixed y, segment_notes notes)
{
    subpath *psub;
    line_segment *lp;
    int code;

    code = path_open(ppath);
    if (code < 0) return code;
    psub = ppath->current_subpath;
    lp = path_alloc(ppath, s_line, notes);
    path_alloc_link((segment *)lp, psub);
    path_set_point(lp, x, y);
    path_update_draw(ppath);
    trace_segment("[P]", ppath->memory, (segment *) lp);
    return 0;
}

int
gx_path_add_lines_notes(gx_path *ppath, const gs_fixed_point *ppts, int count,
                        segment_notes notes)
{
    subpath *psub;
    segment *prev;
    line_segment *lp = 0;
    int i;
    int code = 0;

    if (count <= 0)
        return 0;

    code = path_open(ppath);
    if (code < 0) return code;
    psub = ppath->current_subpath;
    prev = psub->last;
    /*
     * We could do better than the following, but this is a start.
     * Note that we don't make any attempt to undo partial additions
     * if we fail partway through; this is equivalent to what would
     * happen with multiple calls on gx_path_add_line.
     */
    for (i = 0; i < count; i++) {
        fixed x = ppts[i].x;
        fixed y = ppts[i].y;
        line_segment *next;

        next = path_alloc(ppath, s_line, notes);
        lp = next;
        lp->type = s_line;
        lp->notes = notes;
        prev->next = (segment *) lp;
        lp->prev = prev;
        lp->pt.x = x;
        lp->pt.y = y;
        prev = (segment *) lp;
        trace_segment("[P]", ppath->memory, (segment *) lp);
    }
    if (lp != 0)
        ppath->position.x = lp->pt.x,
            ppath->position.y = lp->pt.y,
            psub->last = (segment *) lp,
            lp->next = 0,
            path_update_draw(ppath);
    return code;
}

/* gz_path_add_curve_notes */
int
gx_path_add_curve_notes(gx_path * ppath,
                 fixed x1, fixed y1, fixed x2, fixed y2, fixed x3, fixed y3,
                        segment_notes notes)
{
    subpath *psub;
    curve_segment *lp;
    int code;

    code = path_open(ppath);
    if (code < 0) return code;
    psub = ppath->current_subpath;
    lp = path_alloc(ppath, s_curve, notes);
    path_alloc_link((segment *)lp, psub);
    lp->p1.x = x1;
    lp->p1.y = y1;
    lp->p2.x = x2;
    lp->p2.y = y2;
    path_set_point(lp, x3, y3);
    psub->curve_count++;
    ppath->curve_count++;
    path_update_draw(ppath);
    trace_segment("[P]", ppath->memory, (segment *) lp);
    return 0;
}

/* gz_path_close_subpath_notes */
int
gx_path_close_subpath_notes(gx_path * ppath, segment_notes notes)
{
    subpath *psub;
    line_close_segment *lp;
    int code;

    if (!path_subpath_open(ppath))
        return 0;
    if (path_last_is_moveto(ppath)) {
        /* The last operation was a moveto: create a subpath. */
        code = gx_path_new_subpath(ppath);
        if (code < 0)
            return code;
    }
    psub = ppath->current_subpath;
    lp = path_alloc(ppath, s_line_close, notes);
    path_alloc_link((segment *)lp, psub);
    path_set_point(lp, psub->pt.x, psub->pt.y);
    lp->sub = psub;
    psub->is_closed = 1;
    path_update_closepath(ppath);
    trace_segment("[P]", ppath->memory, (segment *) lp);
    return 0;
}
#define gx_path_close_subpath(ppath)\
  gx_path_close_subpath_notes(ppath, sn_none)

/* ---------------- gxpath2.c ---------------- */

/* gx_path_bbox, for a path without setbbox and an uninitialised cached
 * box (box_last == 0), which is what the cache always reproduces. */
int
gx_path_bbox(gx_path * ppath, gs_fixed_rect * pbox)
{
    if (ppath->first_subpath == 0) {
        /* The path is empty, use the current point if any. */
        /* pdfking: gx_path_current_point inlined. */
        if (!path_position_valid(ppath)) {
            /*
             * Don't return garbage, in case the caller doesn't
             * check the return code.
             */
            pbox->p.x = pbox->p.y = 0;
            pbox->q = pbox->p;
            return gs_note_error(gs_error_nocurrentpoint);
        }
        pbox->p = ppath->position;
        pbox->q = pbox->p;
        return 0;
    }
    {
        fixed px, py, qx, qy;
        const segment *pseg = (const segment *)ppath->first_subpath;

        px = qx = pseg->pt.x;
        py = qy = pseg->pt.y;

/* Macro for adjusting the bounding box when adding a point */
#define ADJUST_BBOX(pt)\
  if ((pt).x < px) px = (pt).x;\
  else if ((pt).x > qx) qx = (pt).x;\
  if ((pt).y < py) py = (pt).y;\
  else if ((pt).y > qy) qy = (pt).y

        while ((pseg = pseg->next) != 0) {
            switch (pseg->type) {
                case s_curve:
                    ADJUST_BBOX(((const curve_segment *)pseg)->p1);
                    ADJUST_BBOX(((const curve_segment *)pseg)->p2);
                    [[fallthrough]];
                default:
                    ADJUST_BBOX(pseg->pt);
            }
        }
#undef ADJUST_BBOX

        pbox->p.x = px, pbox->p.y = py, pbox->q.x = qx, pbox->q.y = qy;
    }
    return 0;
}

/* ---------------- gxpflat.c ---------------- */

int
gx_curve_log2_samples(fixed x0, fixed y0, const curve_segment * pc,
                      fixed fixed_flat)
{
    fixed
        x03 = pc->pt.x - x0,
        y03 = pc->pt.y - y0;
    int k;

    if (x03 < 0)
        x03 = -x03;
    if (y03 < 0)
        y03 = -y03;
    if ((x03 | y03) < int2fixed(16))
        fixed_flat >>= 1;
    if (fixed_flat == 0) {	/* Use the conservative method. */
        fixed m = max(x03, y03);

        for (k = 1; m > fixed_1;)
            k++, m >>= 1;
    } else {
        const fixed
              x12 = pc->p1.x - pc->p2.x, y12 = pc->p1.y - pc->p2.y,
              dx0 = x0 - pc->p1.x - x12, dy0 = y0 - pc->p1.y - y12,
              dx1 = x12 - pc->p2.x + pc->pt.x, dy1 = y12 - pc->p2.y + pc->pt.y,
              adx0 = any_abs(dx0), ady0 = any_abs(dy0),
              adx1 = any_abs(dx1), ady1 = any_abs(dy1);
        fixed
            d = max(adx0, adx1) + max(ady0, ady1);
        /*
         * The following statement is split up to work around a
         * bug in the gcc 2.7.2 optimizer on H-P RISC systems.
         */
        uint qtmp = d - (d >> 2) /* 3/4 * D */ +fixed_flat - 1;
        uint q = qtmp / fixed_flat;

        if_debug6('2', "[2]d01=%g,%g d12=%g,%g d23=%g,%g\n",
                  fixed2float(pc->p1.x - x0), fixed2float(pc->p1.y - y0),
                  fixed2float(-x12), fixed2float(-y12),
                  fixed2float(pc->pt.x - pc->p2.x), fixed2float(pc->pt.y - pc->p2.y));
        if_debug2('2', "     D=%f, flat=%f,",
                  fixed2float(d), fixed2float(fixed_flat));
        /* Now we want to set k = ceiling(log2(q) / 2). */
        for (k = 0; q > 1;)
            k++, q = (q + 3) >> 2;
        if_debug1('2', " k=%d\n", k);
    }
    return k;
}

/*
 * Split a curve segment into two pieces at the (parametric) midpoint.
 * Algorithm is from "The Beta2-split: A special case of the Beta-spline
 * Curve and Surface Representation," B. A. Barsky and A. D. DeRose, IEEE,
 * 1985, courtesy of Crispin Goswell.
 */
static void
split_curve_midpoint(fixed x0, fixed y0, const curve_segment * pc,
                     curve_segment * pc1, curve_segment * pc2)
{				/*
                                 * We have to define midpoint carefully to avoid overflow.
                                 * (If it overflows, something really pathological is going
                                 * on, but we could get infinite recursion that way....)
                                 */
#define midpoint(a,b)\
  (arith_rshift_1(a) + arith_rshift_1(b) + (((a) | (b)) & 1))
    fixed x12 = midpoint(pc->p1.x, pc->p2.x);
    fixed y12 = midpoint(pc->p1.y, pc->p2.y);

    /*
     * pc1 or pc2 may be the same as pc, so we must be a little careful
     * about the order in which we store the results.
     */
    pc1->p1.x = midpoint(x0, pc->p1.x);
    pc1->p1.y = midpoint(y0, pc->p1.y);
    pc2->p2.x = midpoint(pc->p2.x, pc->pt.x);
    pc2->p2.y = midpoint(pc->p2.y, pc->pt.y);
    pc1->p2.x = midpoint(pc1->p1.x, x12);
    pc1->p2.y = midpoint(pc1->p1.y, y12);
    pc2->p1.x = midpoint(x12, pc2->p2.x);
    pc2->p1.y = midpoint(y12, pc2->p2.y);
    if (pc2 != pc)
        pc2->pt.x = pc->pt.x,
            pc2->pt.y = pc->pt.y;
    pc1->pt.x = midpoint(pc1->p2.x, pc2->p1.x);
    pc1->pt.y = midpoint(pc1->p2.y, pc2->p1.y);
#undef midpoint
}

static inline void
print_points(const gs_fixed_point *points, int count)
{
#ifdef DEBUG
    int i;

    if (!gs_debug_c('3'))
        return;
    for (i = 0; i < count; i++)
      if_debug2('3', "[3]out x=%ld y=%ld\n",
                (long)points[i].x, (long)points[i].y);
#endif
}

bool
curve_coeffs_ranged(fixed x0, fixed x1, fixed x2, fixed x3,
                    fixed y0, fixed y1, fixed y2, fixed y3,
                    fixed *ax, fixed *bx, fixed *cx,
                    fixed *ay, fixed *by, fixed *cy,
                    int k)
{
    fixed x01, x12, y01, y12;

    curve_points_to_coefficients(x0, x1, x2, x3,
                                 *ax, *bx, *cx, x01, x12);
    curve_points_to_coefficients(y0, y1, y2, y3,
                                 *ay, *by, *cy, y01, y12);
#   define max_fast (max_fixed / 6)
#   define min_fast (-max_fast)
#   define in_range(v) (v < max_fast && v > min_fast)
    if (k > k_sample_max ||
        !in_range(*ax) || !in_range(*ay) ||
        !in_range(*bx) || !in_range(*by) ||
        !in_range(*cx) || !in_range(*cy)
        )
        return false;
#undef max_fast
#undef min_fast
#undef in_range
    return true;
}

/*  Initialize the iterator.
    Momotonic curves with non-zero length are only allowed.
 */
bool
gx_flattened_iterator__init(gx_flattened_iterator *self,
            fixed x0, fixed y0, const curve_segment *pc, int k)
{
    /* Note : Immediately after the ininialization it keeps an invalid (zero length) segment. */
    fixed x1, y1, x2, y2;
    const int k2 = k << 1, k3 = k2 + k;
    fixed bx2, by2, ax6, ay6;

    x1 = pc->p1.x;
    y1 = pc->p1.y;
    x2 = pc->p2.x;
    y2 = pc->p2.y;
    self->x0 = self->lx0 = self->lx1 = x0;
    self->y0 = self->ly0 = self->ly1 = y0;
    self->x3 = pc->pt.x;
    self->y3 = pc->pt.y;
    if (!curve_coeffs_ranged(self->x0, x1, x2, self->x3,
                             self->y0, y1, y2, self->y3,
                             &self->ax, &self->bx, &self->cx,
                             &self->ay, &self->by, &self->cy, k))
        return false;
    self->curve = true;
    self->k = k;
#ifdef DEBUG
        if (gs_debug_c('3')) {
            dlprintf4("[3]x0=%f y0=%f x1=%f y1=%f\n",
                      fixed2float(self->x0), fixed2float(self->y0),
                      fixed2float(x1), fixed2float(y1));
            dlprintf5("   x2=%f y2=%f x3=%f y3=%f  k=%d\n",
                      fixed2float(x2), fixed2float(y2),
                      fixed2float(self->x3), fixed2float(self->y3), self->k);
        }
#endif
    if (k == -1) {
        /* A special hook for gx_subdivide_curve_rec.
           Only checked the range.
           Returning with no initialization. */
        return true;
    }
    self->rmask = (1 << k3) - 1;
    self->i = (1 << k);
    self->rx = self->ry = 0;
    if_debug6('3', "[3]ax=%f bx=%f cx=%f\n   ay=%f by=%f cy=%f\n",
              fixed2float(self->ax), fixed2float(self->bx), fixed2float(self->cx),
              fixed2float(self->ay), fixed2float(self->by), fixed2float(self->cy));
    bx2 = self->bx << 1;
    by2 = self->by << 1;
    ax6 = ((self->ax << 1) + self->ax) << 1;
    ay6 = ((self->ay << 1) + self->ay) << 1;
    self->idx = arith_rshift(self->cx, self->k);
    self->idy = arith_rshift(self->cy, self->k);
    self->rdx = ((uint)self->cx << k2) & self->rmask;
    self->rdy = ((uint)self->cy << k2) & self->rmask;
    /* bx/y terms */
    self->id2x = arith_rshift(bx2, k2);
    self->id2y = arith_rshift(by2, k2);
    self->rd2x = ((uint)bx2 << self->k) & self->rmask;
    self->rd2y = ((uint)by2 << self->k) & self->rmask;
#   define adjust_rem(r, q, rmask) if ( r > rmask ) q ++, r &= rmask
    /* We can compute all the remainders as ints, */
    /* because we know they don't exceed M. */
    /* cx/y terms */
    self->idx += arith_rshift_1(self->id2x);
    self->idy += arith_rshift_1(self->id2y);
    self->rdx += ((uint)self->bx << self->k) & self->rmask,
    self->rdy += ((uint)self->by << self->k) & self->rmask;
    adjust_rem(self->rdx, self->idx, self->rmask);
    adjust_rem(self->rdy, self->idy, self->rmask);
    /* ax/y terms */
    self->idx += arith_rshift(self->ax, k3);
    self->idy += arith_rshift(self->ay, k3);
    self->rdx += (uint)self->ax & self->rmask;
    self->rdy += (uint)self->ay & self->rmask;
    adjust_rem(self->rdx, self->idx, self->rmask);
    adjust_rem(self->rdy, self->idy, self->rmask);
    self->id2x += self->id3x = arith_rshift(ax6, k3);
    self->id2y += self->id3y = arith_rshift(ay6, k3);
    self->rd2x += self->rd3x = (uint)ax6 & self->rmask,
    self->rd2y += self->rd3y = (uint)ay6 & self->rmask;
    adjust_rem(self->rd2x, self->id2x, self->rmask);
    adjust_rem(self->rd2y, self->id2y, self->rmask);
#   undef adjust_rem
    return true;
}

static inline bool
check_diff_overflow(fixed v0, fixed v1)
{
    if (v1 > 0)
        return (v0 < min_fixed + v1);
    else if (v1 < 0)
        return (v0 > max_fixed + v1);
    return false;
}

bool
gx_check_fixed_diff_overflow(fixed v0, fixed v1)
{
    return check_diff_overflow(v0, v1);
}
bool
gx_check_fixed_sum_overflow(fixed v0, fixed v1)
{
    /* We assume that clamp_point_aux have been applied to v1,
       thus -v alweays exists.
     */
    return check_diff_overflow(v0, -v1);
}

/* Move to the next segment and store it to self->lx0, self->ly0, self->lx1, self->ly1 .
 * Return true iff there exist more segments.
 */
int
gx_flattened_iterator__next(gx_flattened_iterator *self)
{
    /*
     * We can compute successive values by finite differences,
     * using the formulas:
     x(t) =
     a*t^3 + b*t^2 + c*t + d =>
     dx(t) = x(t+e)-x(t) =
     a*(3*t^2*e + 3*t*e^2 + e^3) + b*(2*t*e + e^2) + c*e =
     (3*a*e)*t^2 + (3*a*e^2 + 2*b*e)*t + (a*e^3 + b*e^2 + c*e) =>
     d2x(t) = dx(t+e)-dx(t) =
     (3*a*e)*(2*t*e + e^2) + (3*a*e^2 + 2*b*e)*e =
     (6*a*e^2)*t + (6*a*e^3 + 2*b*e^2) =>
     d3x(t) = d2x(t+e)-d2x(t) =
     6*a*e^3;
     x(0) = d, dx(0) = (a*e^3 + b*e^2 + c*e),
     d2x(0) = 6*a*e^3 + 2*b*e^2;
     * In these formulas, e = 1/2^k; of course, there are separate
     * computations for the x and y values.
     *
     * There is a tradeoff in doing the above computation in fixed
     * point.  If we separate out the constant term (d) and require that
     * all the other values fit in a long, then on a 32-bit machine with
     * 12 bits of fraction in a fixed, k = 4 implies a maximum curve
     * size of 128 pixels; anything larger requires subdividing the
     * curve.  On the other hand, doing the computations in explicit
     * double precision slows down the loop by a factor of 3 or so.  We

     * found to our surprise that the latter is actually faster, because
     * the additional subdivisions cost more than the slower loop.
     *
     * We represent each quantity as I+R/M, where I is an "integer" and
     * the "remainder" R lies in the range 0 <= R < M=2^(3*k).  Note
     * that R may temporarily exceed M; for this reason, we require that
     * M have at least one free high-order bit.  To reduce the number of
     * variables, we don't actually compute M, only M-1 (rmask).  */
    fixed x = self->lx1, y = self->ly1;

    if (self->i <= 0)
        return_error(gs_error_unregistered); /* Must not happen. */
    self->lx0 = self->lx1;
    self->ly0 = self->ly1;
    /* Fast check for N == 3, a common special case for small characters. */
    if (self->k <= 1) {
        --self->i;
        if (self->i == 0)
            goto last;
#	define poly2(a,b,c) arith_rshift_1(arith_rshift_1(arith_rshift_1(a) + b) + c)
        x += poly2(self->ax, self->bx, self->cx);
        y += poly2(self->ay, self->by, self->cy);
#	undef poly2
        if_debug2('3', "[3]dx=%f, dy=%f\n",
                  fixed2float(x - self->x0), fixed2float(y - self->y0));
        if_debug5('3', "[3]%s x=%g, y=%g x=%ld y=%ld\n",
                  (((x ^ self->x0) | (y ^ self->y0)) & float2fixed(-0.5) ?
                   "add" : "skip"),
                  fixed2float(x), fixed2float(y), (long)x, (long)y);
        self->lx1 = x, self->ly1 = y;
        return true;
    } else {
        --self->i;
        if (self->i == 0)
            goto last; /* don't bother with last accum */
#	ifdef DEBUG
            gx_flattened_iterator__print_state(self);
#	endif
#	define accum(i, r, di, dr, rmask)\
                        if ( (r += dr) > rmask ) r &= rmask, i += di + 1;\
                        else i += di
        accum(x, self->rx, self->idx, self->rdx, self->rmask);
        accum(y, self->ry, self->idy, self->rdy, self->rmask);
        accum(self->idx, self->rdx, self->id2x, self->rd2x, self->rmask);
        accum(self->idy, self->rdy, self->id2y, self->rd2y, self->rmask);
        accum(self->id2x, self->rd2x, self->id3x, self->rd3x, self->rmask);
        accum(self->id2y, self->rd2y, self->id3y, self->rd3y, self->rmask);
        if_debug5('3', "[3]%s x=%g, y=%g x=%ld y=%ld\n",
                  (((x ^ self->lx0) | (y ^ self->ly0)) & float2fixed(-0.5) ?
                   "add" : "skip"),
                  fixed2float(x), fixed2float(y), (long)x, (long)y);
#	undef accum
        self->lx1 = self->x = x;
        self->ly1 = self->y = y;
        return true;
    }
last:
    self->lx1 = self->x3;
    self->ly1 = self->y3;
    if_debug4('3', "[3]last x=%g, y=%g x=%ld y=%ld\n",
              fixed2float(self->lx1), fixed2float(self->ly1),
              (long)self->lx1, (long)self->ly1);
    return false;
}

#define max_points 50		/* arbitrary */

static int
generate_segments(gx_path * ppath, const gs_fixed_point *points,
                    int count, segment_notes notes)
{
    if (notes & sn_not_first) {
        print_points(points, count);
        return gx_path_add_lines_notes(ppath, points, count, notes);
    } else {
        int code;

        print_points(points, 1);
        code = gx_path_add_line_notes(ppath, points[0].x, points[0].y, notes);
        if (code < 0)
            return code;
        print_points(points + 1, count - 1);
        return gx_path_add_lines_notes(ppath, points + 1, count - 1, notes | sn_not_first);
    }
}

static int
gx_subdivide_curve_rec(gx_flattened_iterator *self,
                  gx_path * ppath, int k, curve_segment * pc,
                  segment_notes notes, gs_fixed_point *points)
{
    int code;

top :
    if (!gx_flattened_iterator__init(self,
                ppath->position.x, ppath->position.y, pc, k)) {
        /* Curve is too long.  Break into two pieces and recur. */
        curve_segment cseg;

        k--;
        split_curve_midpoint(ppath->position.x, ppath->position.y, pc, &cseg, pc);
        code = gx_subdivide_curve_rec(self, ppath, k, &cseg, notes, points);
        if (code < 0)
            return code;
        notes |= sn_not_first;
        goto top;
    } else if (k < 0) {
        /* This used to be k == -1, but... If we have a very long curve, we will first
         * go through the code above to split the long curve into 2. In fact for very
         * long curves we can go through that multiple times. This can lead to k being
         * < -1 by the time we finish subdividing the curve, and that meant we did not
         * satisfy the exit condition here, leading to a loop until VM error.
         */
        /* fixme : Don't need to init the iterator. Just wanted to check in_range. */
        return gx_path_add_curve_notes(ppath, pc->p1.x, pc->p1.y, pc->p2.x, pc->p2.y,
                        pc->pt.x, pc->pt.y, notes);
    } else {
        gs_fixed_point *ppt = points;
        bool more;

        for(;;) {
            code = gx_flattened_iterator__next(self);

            if (code < 0)
                return code;
            more = code;
            ppt->x = self->lx1;
            ppt->y = self->ly1;
            ppt++;
            if (ppt == &points[max_points] || !more) {
                gs_fixed_point *pe = (more ?  ppt - 2 : ppt);

                code = generate_segments(ppath, points, pe - points, notes);
                if (code < 0)
                    return code;
                if (!more)
                    return 0;
                notes |= sn_not_first;
                memcpy(points, pe, (char *)ppt - (char *)pe);
                ppt = points + (ppt - pe);
            }
        }
    }
}

#undef coord_near

/*
 * Flatten a segment of the path by repeated sampling.
 * 2^k is the number of lines to produce (i.e., the number of points - 1,
 * including the endpoints); we require k >= 1.
 * If k or any of the coefficient values are too large,
 * use recursive subdivision to whittle them down.
 */

int
gx_subdivide_curve(gx_path * ppath, int k, curve_segment * pc, segment_notes notes)
{
    gs_fixed_point points[max_points + 1];
    gx_flattened_iterator iter;

    return gx_subdivide_curve_rec(&iter, ppath, k, pc, notes, points);
}

#undef max_points

/* ---------------- gxpcopy.c ---------------- */

typedef enum {
    pco_none = 0,
    pco_monotonize = 1,		/* make curves monotonic */
    pco_accurate = 2,		/* flatten with accurate tangents at ends */
    pco_for_stroke = 4,		/* flatten taking line width into account */
    pco_small_curves = 8	/* make curves small */
} gx_path_copy_options;
typedef struct gs_gstate_s gs_gstate;

/* pdfking: gx_path_new: drop all segments. */
static void
gx_path_new(gx_path *ppath)
{
    ppath->storage.clear();
    ppath->first_subpath = ppath->current_subpath = 0;
    ppath->subpath_count = ppath->curve_count = 0;
    ppath->state_flags = psf_last_newpath;
}


/* Forward declarations */
static void adjust_point_to_tangent(segment *, const segment *,
                                     const gs_fixed_point *);

static inline int
break_line_if_long(gx_path *ppath, const segment *pseg)
{
    fixed x0 = ppath->position.x;
    fixed y0 = ppath->position.y;

    if (gx_check_fixed_diff_overflow(pseg->pt.x, x0) ||
        gx_check_fixed_diff_overflow(pseg->pt.y, y0)) {
        fixed x, y;

        if (gx_check_fixed_sum_overflow(pseg->pt.x, x0))
            x = (pseg->pt.x >> 1) + (x0 >> 1);
        else
            x = (pseg->pt.x + x0) >> 1;
        if (gx_check_fixed_sum_overflow(pseg->pt.y, y0))
            y = (pseg->pt.y >> 1) + (y0 >> 1);
        else
            y = (pseg->pt.y + y0) >> 1;
        return gx_path_add_line_notes(ppath, x, y, pseg->notes);
        /* WARNING: Stringly speaking, the next half segment must get
           the sn_not_first flag. We don't bother, because that flag
           has no important meaning with colinear segments.
         */
    }
    return 0;
}

/* Copy a path, optionally flattening or monotonizing it. */
/* If the copy fails, free the new path. */
int
gx_path_copy_reducing(const gx_path *ppath_old, gx_path *ppath,
                      fixed fixed_flatness, const gs_gstate *pgs,
                      gx_path_copy_options options)
{
    const segment *pseg;
    fixed flat = fixed_flatness;
    gs_fixed_point expansion;
    int code = 0; /* pdfking: gs unshares ppath here */

#ifdef DEBUG
    if (gs_debug_c('P'))
        gx_dump_path(ppath_old, "before reducing");
#endif
    /* pdfking: pco_for_stroke (needs the gstate's line width and CTM) is
     * not supported; the fill pre-pass never passes it. */
    expansion.x = expansion.y = 0; /* Quiet gcc warning. */
    pseg = (const segment *)(ppath_old->first_subpath);
    while (pseg) {
        switch (pseg->type) {
            case s_start:
                code = gx_path_add_point(ppath,
                                         pseg->pt.x, pseg->pt.y);
                break;
            case s_curve:
                {
                    const curve_segment *pc = (const curve_segment *)pseg;

                    if (fixed_flatness == max_fixed) {	/* don't flatten */
                        if (options & pco_monotonize)
                            code = gs_note_error(gs_error_unregistered); /* pdfking: gx_curve_monotonize not ported */
                        else
                            code = gx_path_add_curve_notes(ppath,
                                     pc->p1.x, pc->p1.y, pc->p2.x, pc->p2.y,
                                           pc->pt.x, pc->pt.y, pseg->notes);
                    } else {
                        fixed x0 = ppath->position.x;
                        fixed y0 = ppath->position.y;
                        segment_notes notes = pseg->notes;
                        curve_segment cseg;
                        int k;

                        if (options & pco_for_stroke) {
                            /*
                             * When flattening for stroking, the flatness
                             * must apply to the outside of the resulting
                             * stroked region.  We approximate this by
                             * dividing the flatness by the ratio of the
                             * expanded bounding box to the original
                             * bounding box.  This is crude, but pretty
                             * simple to calculate, and produces reasonably
                             * good results.
                             */
                            fixed min01, max01, min23, max23;
                            fixed ex, ey, flat_x, flat_y;

#define SET_EXTENT(r, c0, c1, c2, c3)\
    BEGIN\
        if (c0 < c1) min01 = c0, max01 = c1;\
        else         min01 = c1, max01 = c0;\
        if (c2 < c3) min23 = c2, max23 = c3;\
        else         min23 = c3, max23 = c2;\
        r = max(max01, max23) - min(min01, min23);\
    END
                            SET_EXTENT(ex, x0, pc->p1.x, pc->p2.x, pc->pt.x);
                            SET_EXTENT(ey, y0, pc->p1.y, pc->p2.y, pc->pt.y);
#undef SET_EXTENT
                            /*
                             * We check for the degenerate case specially
                             * to avoid a division by zero.
                             */
                            if (ex == 0 || ey == 0)
                                if (ex != 0) {
                                    flat = fixed_mult_quo(fixed_flatness, ex,
                                                          ex + expansion.x);
                                    k = gx_curve_log2_samples(x0,y0,pc,flat);
                                } else if (ey != 0) {
                                    flat = fixed_mult_quo(fixed_flatness, ey,
                                                          ey + expansion.y);
                                    k = gx_curve_log2_samples(x0,y0,pc,flat);
                                } else
                                    k = 0;
                            else {
                                flat_x =
                                    fixed_mult_quo(fixed_flatness, ex,
                                                   ex + expansion.x);
                                flat_y =
                                    fixed_mult_quo(fixed_flatness, ey,
                                                   ey + expansion.y);
                                flat = min(flat_x, flat_y);
                                k = gx_curve_log2_samples(x0, y0, pc, flat);
                            }
                        } else
                            k = gx_curve_log2_samples(x0, y0, pc, flat);
                        if (options & pco_accurate) {
                            segment *start;
                            segment *end;

                            /*
                             * Add an extra line, which will become
                             * the tangent segment.
                             */
                            code = gx_path_add_line_notes(ppath, x0, y0,
                                                          notes);
                            if (code < 0)
                                break;
                            start = ppath->current_subpath->last;
                            notes |= sn_not_first;
                            cseg = *pc;
                            code = gx_subdivide_curve(ppath, k, &cseg, notes);
                            if (code < 0)
                                break;
                            /*
                             * Adjust the first and last segments so that
                             * they line up with the tangents.
                             */
                            end = ppath->current_subpath->last;
                            if ((code = gx_path_add_line_notes(ppath,
                                                          ppath->position.x,
                                                          ppath->position.y,
                                            pseg->notes | sn_not_first)) < 0)
                                break;
                            if (start->next->pt.x != pc->p1.x || start->next->pt.y != pc->p1.y)
                                adjust_point_to_tangent(start, start->next, &pc->p1);
                            else if (start->next->pt.x != pc->p2.x || start->next->pt.y != pc->p2.y)
                                adjust_point_to_tangent(start, start->next, &pc->p2);
                            else
                                adjust_point_to_tangent(start, start->next, &end->prev->pt);
                            if (end->prev->pt.x != pc->p2.x || end->prev->pt.y != pc->p2.y)
                                adjust_point_to_tangent(end, end->prev, &pc->p2);
                            else if (end->prev->pt.x != pc->p1.x || end->prev->pt.y != pc->p1.y)
                                adjust_point_to_tangent(end, end->prev, &pc->p1);
                            else
                                adjust_point_to_tangent(end, end->prev, &start->pt);
                        } else {
                            cseg = *pc;
                            code = gx_subdivide_curve(ppath, k, &cseg, notes);
                        }
                    }
                    break;
                }
            case s_line:
                code = break_line_if_long(ppath, pseg);
                if (code < 0)
                    break;
                code = gx_path_add_line_notes(ppath,
                                       pseg->pt.x, pseg->pt.y, pseg->notes);
                break;
            /* pdfking: s_gap / s_dash (stroker-only segments) are never
             * built here; they fall to the default case. */
            case s_line_close:
                code = break_line_if_long(ppath, pseg);
                if (code < 0)
                    break;
                code = gx_path_close_subpath(ppath);
                break;
            default:		/* can't happen */
                code = gs_note_error(gs_error_unregistered);
        }
        if (code < 0) {
            gx_path_new(ppath);
            return code;
        }
        pseg = pseg->next;
    }
    if (path_last_is_moveto(ppath_old)) {
        code = gx_path_add_point(ppath, ppath_old->position.x,
                          ppath_old->position.y);
        if (code < 0) {
            gx_path_new(ppath);
            return code;
        }
    }
    /* pdfking: no bbox_set paths here. */
#ifdef DEBUG
    if (gs_debug_c('P'))
        gx_dump_path(ppath, "after reducing");
#endif
    return 0;
}

/*
 * Adjust one end of a line (the first or last line of a flattened curve)
 * so it falls on the curve tangent.  The closest point on the line from
 * (0,0) to (C,D) to a point (U,V) -- i.e., the point on the line at which
 * a perpendicular line from the point intersects it -- is given by
 *      T = (C*U + D*V) / (C^2 + D^2)
 *      (X,Y) = (C*T,D*T)
 * However, any smaller value of T will also work: the one we actually
 * use is 0.25 * the value we just derived.  We must check that
 * numerical instabilities don't lead to a negative value of T.
 */
static void
adjust_point_to_tangent(segment * pseg, const segment * next,
                        const gs_fixed_point * p1)
{
    const fixed x0 = pseg->pt.x, y0 = pseg->pt.y;
    const fixed fC = p1->x - x0, fD = p1->y - y0;

    /*
     * By far the commonest case is that the end of the curve is
     * horizontal or vertical.  Check for this specially, because
     * we can handle it with far less work (and no floating point).
     */
    if (fC == 0) {
        /* Vertical tangent. */
        const fixed DT = arith_rshift(next->pt.y - y0, 2);

        if (fD == 0)
            return;		/* anomalous case */
        if_debug1('2', "[2]adjusting vertical: DT = %g\n", DT);
        if ((DT ^ fD) > 0) /* lgtm [cpp/bitwise-sign-check] */
            pseg->pt.y = DT + y0;
    } else if (fD == 0) {
        /* Horizontal tangent. */
        const fixed CT = arith_rshift(next->pt.x - x0, 2);

        if_debug1('2', "[2]adjusting horizontal: CT = %g\n",
                  fixed2float(CT));
        if ((CT ^ fC) > 0) /* lgtm [cpp/bitwise-sign-check] */
            pseg->pt.x = CT + x0;
    } else {
        /* General case. */
        const double C = fC, D = fD;
        double T = (C * (next->pt.x - x0) + D * (next->pt.y - y0)) /
        (C * C + D * D);

        if_debug3('2', "[2]adjusting: C = %g, D = %g, T = %g\n",
                  C, D, T);
        if (T > 0) {
            if (T > 1) {
                /* Don't go outside the curve bounding box. */
                T = 1;
            }
            pseg->pt.x = arith_rshift((fixed) (C * T), 2) + x0;
            pseg->pt.y = arith_rshift((fixed) (D * T), 2) + y0;
        }
    }
}

/* ---------------- gxdevcli.h / gxdcolor.h (just what is used) ---------------- */

typedef int gx_color_index;
typedef struct gx_device_color_s {
    struct {
        gx_color_index pure;
    } colors;
} gx_device_color;
typedef struct gx_device_s gx_device;
#define dev_proc_fill_rectangle(proc)\
  int proc(gx_device *dev, int x, int y, int width, int height, gx_color_index color)
#define dev_proc_fill_trapezoid(proc)\
  int proc(gx_device *dev, const gs_fixed_edge *left, const gs_fixed_edge *right,\
    fixed ybot, fixed ytop, bool swap_axes, const gx_device_color *pdcolor,\
    gs_logical_operation_t lop)
struct gx_device_s {
    int width;
    int height;
    int non_strict_bounds;
    int max_fill_band;          /* limit on band size for fill, */
                                /* must be 0 or a power of 2 */
    void *memory;
    dev_proc_fill_rectangle((*fill_rectangle));
    dev_proc_fill_trapezoid((*fill_trapezoid));
    /* pdfking: where the rectangles go, and the clip box (pixels). */
    const std::function<void(int x, int y, int w, int h)> *sink;
    int clip_x0, clip_y0, clip_x1, clip_y1;
};
#define dev_proc(dev, p) ((dev)->p)

/* pdfking: the device's fill_rectangle. Clips to the clip box (what gs's
 * clip device does for a rectangular clip path, and fit_fill for the page)
 * and hands the rectangle to the caller. */
static int
sink_fill_rectangle(gx_device *dev, int x, int y, int w, int h,
                    gx_color_index color)
{
    int x1 = x + w, y1 = y + h;

    if (x < dev->clip_x0) x = dev->clip_x0;
    if (y < dev->clip_y0) y = dev->clip_y0;
    if (x1 > dev->clip_x1) x1 = dev->clip_x1;
    if (y1 > dev->clip_y1) y1 = dev->clip_y1;
    if (x1 > x && y1 > y)
        (*dev->sink)(x, y, x1 - x, y1 - y);
    return 0;
}

/* gx_fill_rectangle_device_rop: for a pure colour and an idempotent lop
 * (the only case ScanConvertAndFill produces) gs's gx_dc_pure_fill_rectangle
 * ends in the device's fill_rectangle. */
static int
gx_fill_rectangle_device_rop(int x, int y, int w, int h,
                             const gx_device_color *pdevc, gx_device *dev,
                             gs_logical_operation_t lop)
{
    return dev_proc(dev, fill_rectangle)(dev, x, y, w, h, pdevc->colors.pure);
}

/* color_writes_pure: a pure colour with the default lop writes pure. */
#define color_writes_pure(pdevc, lop) true

/* ---------------- gdevddrw.c ---------------- */

#define SWAP(a, b, t)\
  (t = a, a = b, b = t)

/* ---------------- Polygon and line drawing ---------------- */

/* Define the 'remainder' analogue of fixed_mult_quo. */
static fixed
fixed_mult_rem(fixed a, fixed b, fixed c)
{
    /* All kinds of truncation may happen here, but it's OK. */
    return a * b - fixed_mult_quo(a, b, c) * c;
}

/*
 * The trapezoid fill algorithm uses trap_line structures to keep track of
 * the left and right edges during the Bresenham loop.
 */
typedef struct trap_line_s {
        /*
         * h is the y extent of the line (edge.end.y - edge.start.y).
         * We know h > 0.
         */
    fixed h;
        /*
         * The dx/dy ratio for the line is di + df/h.
         * (The quotient refers to the l.s.b. of di, not fixed_1.)
         * We know 0 <= df < h.
         */
    int di;
    fixed df;
        /*
         * The intersection of the line with a scan line is x + xf/h + 1.
         * (The 1 refers to the least significant bit of x, not fixed_1;
         * similarly, the quotient refers to the l.s.b. of x.)
         * We know -h <= xf < 0.
         *
         * This rational value preciselly represents the mathematical line
         * (with no machine arithmetic error).
         *
         * Note that the fractional part is negative to simplify
         * some conditions in the Bresenham algorithm.
         * Due to that some expressions are inobvious.
         * We believe that it's a kind of archaic
         * for the modern hyperthreading architecture,
         * we still keep it because the code passed a huge testing
         * on various platforms.
         */
    fixed x, xf;
        /*
         * We increment (x,xf) by (ldi,ldf) after each scan line.
         * (ldi,ldf) is just (di,df) converted to fixed point.
         * We know 0 <= ldf < h.
         */
    fixed ldi, ldf;
} trap_line;


/*
 * Compute the di and df members of a trap_line structure.  The x extent
 * (edge.end.x - edge.start.x) is a parameter; the y extent (h member)
 * has already been set.  Also adjust x for the initial y.
 */
static inline void
compute_dx(trap_line *tl, fixed xd, fixed ys)
{
    fixed h = tl->h;
    int di;

    if (xd >= 0) {
        if (xd < h)
            tl->di = 0, tl->df = xd;
        else {
            tl->di = di = (int)(xd / h);
            tl->df = xd - di * h;
            tl->x += ys * di;
        }
    } else {
        if ((tl->df = xd + h) >= 0 /* xd >= -h */)
            tl->di = -1, tl->x -= ys;
        else {
            tl->di = di = (int)((xd + 1) / h - 1);
            tl->df = xd - di * h;
            tl->x += ys * di;
        }
    }
}

#define YMULT_LIMIT (max_fixed / fixed_1)

/* Compute ldi, ldf, and xf similarly. */
static inline void
compute_ldx(trap_line *tl, fixed ys)
{
    int di = tl->di;
    fixed df = tl->df;
    fixed h = tl->h;

    if ( df < YMULT_LIMIT ) {
         if ( df == 0 )		/* vertical edge, worth checking for */
             tl->ldi = int2fixed(di), tl->ldf = 0, tl->xf = -h;
         else {
             tl->ldi = int2fixed(di) + int2fixed(df) / h;
             tl->ldf = int2fixed(df) % h;
             tl->xf =
                 (ys < fixed_1 ? ys * df % h : fixed_mult_rem(ys, df, h)) - h;
         }
    }
    else {
        tl->ldi = int2fixed(di) + fixed_mult_quo(fixed_1, df, h);
        tl->ldf = fixed_mult_rem(fixed_1, df, h);
        tl->xf = fixed_mult_rem(ys, df, h) - h;
    }
}

/* ---------------- gxdtfill.h, instantiated as gdevddrw.c does for the
 * non-shading variants (LINEAR_COLOR 0, CONTIGUOUS_FILL 0). pdfking: the
 * four SWAP_AXES x FILL_DIRECT instances are a C++ template instead of four
 * #includes; "register" is dropped (removed in C++17). ---------------- */

#define register
#define LINEAR_COLOR 0 /* Common for shading variants. */
#define EDGE_TYPE gs_fixed_edge  /* Common for non-shading variants. */
#define FILL_ATTRS gs_logical_operation_t  /* Common for non-shading variants. */
#define CONTIGUOUS_FILL 0
#define GX_FILL_TRAPEZOID template <int SWAP_AXES, int FILL_DIRECT> int gx_fill_trapezoid_tmpl
GX_FILL_TRAPEZOID (gx_device * dev, const EDGE_TYPE * left,
    const EDGE_TYPE * right, fixed ybot, fixed ytop, int flags,
    const gx_device_color * pdevc, FILL_ATTRS fa)
{
    const fixed ymin = fixed_pixround(ybot) + fixed_half;
    const fixed ymax = fixed_pixround(ytop);

    if (ymin >= ymax)
        return 0;		/* no scan lines to sample */
    {
        int iy = fixed2int_var(ymin);
        const int iy1 = fixed2int_var(ymax);
        trap_line l, r;
        register int rxl, rxr;
#if !LINEAR_COLOR
        int ry;
#endif
        const fixed
            x0l = left->start.x, x1l = left->end.x, x0r = right->start.x,
            x1r = right->end.x, dxl = x1l - x0l, dxr = x1r - x0r;
        const fixed	/* partial pixel offset to first line to sample */
            ysl = ymin - left->start.y, ysr = ymin - right->start.y;
        fixed fxl;
        int code;
#	if CONTIGUOUS_FILL
            const bool peak0 = ((flags & 1) != 0);
            const bool peak1 = ((flags & 2) != 0);
            int peak_y0 = ybot + fixed_half;
            int peak_y1 = ytop - fixed_half;
#	endif
#	if LINEAR_COLOR
            int num_components = dev->color_info.num_components;
            frac31 lgc[GX_DEVICE_COLOR_MAX_COMPONENTS];
            int32_t lgf[GX_DEVICE_COLOR_MAX_COMPONENTS];
            int32_t lgnum[GX_DEVICE_COLOR_MAX_COMPONENTS];
            frac31 rgc[GX_DEVICE_COLOR_MAX_COMPONENTS];
            int32_t rgf[GX_DEVICE_COLOR_MAX_COMPONENTS];
            int32_t rgnum[GX_DEVICE_COLOR_MAX_COMPONENTS];
            frac31 xgc[GX_DEVICE_COLOR_MAX_COMPONENTS];
            int32_t xgf[GX_DEVICE_COLOR_MAX_COMPONENTS];
            int32_t xgnum[GX_DEVICE_COLOR_MAX_COMPONENTS];
            trap_gradient lg, rg, xg;
#	else
            gx_color_index cindex = pdevc->colors.pure;
            dev_proc_fill_rectangle((*fill_rect)) =
                dev_proc(dev, fill_rectangle);
#	endif

        if_debug2m('z', dev->memory, "[z]y=[%d,%d]\n", iy, iy1);

        l.h = left->end.y - left->start.y;
        if (l.h == 0)
           return 0;
        r.h = right->end.y - right->start.y;
        if (r.h == 0)
           return 0;
        l.x = x0l + (fixed_half - fixed_epsilon);
        r.x = x0r + (fixed_half - fixed_epsilon);
#if !LINEAR_COLOR
        ry = iy;
#endif

/*
 * Free variables of FILL_TRAP_RECT:
 *	SWAP_AXES, pdevc, dev, fa
 * Free variables of FILL_TRAP_RECT_DIRECT:
 *	SWAP_AXES, fill_rect, dev, cindex
 */
#define FILL_TRAP_RECT_INDIRECT(x,y,w,h)\
  (SWAP_AXES ? gx_fill_rectangle_device_rop(y, x, h, w, pdevc, dev, fa) :\
   gx_fill_rectangle_device_rop(x, y, w, h, pdevc, dev, fa))
#define FILL_TRAP_RECT_DIRECT(x,y,w,h)\
  (SWAP_AXES ? (*fill_rect)(dev, y, x, h, w, cindex) :\
   (*fill_rect)(dev, x, y, w, h, cindex))

#if LINEAR_COLOR
#   define FILL_TRAP_RECT(x,y,w,h)\
        (!(w) ? 0 : dev_proc(dev, fill_linear_color_scanline)(dev, fa, x, y, w, xg.c, xg.f, xg.num, xg.den))
#else
#   define FILL_TRAP_RECT(x,y,w,h)\
        (FILL_DIRECT ? FILL_TRAP_RECT_DIRECT(x,y,w,h) : FILL_TRAP_RECT_INDIRECT(x,y,w,h))
#endif

        /* Compute the dx/dy ratios. */

        /*
         * Compute the x offsets at the first scan line to sample.  We need
         * to be careful in computing ys# * dx#f {/,%} h# because the
         * multiplication may overflow.  We know that all the quantities
         * involved are non-negative, and that ys# is usually less than 1 (as
         * a fixed, of course); this gives us a cheap conservative check for
         * overflow in the multiplication.
         */
#define YMULT_QUO(ys, tl)\
  (ys < fixed_1 && tl.df < YMULT_LIMIT ? ys * tl.df / tl.h :\
   fixed_mult_quo(ys, tl.df, tl.h))

#if CONTIGUOUS_FILL
/*
 * If left and right boundary round to same pixel index,
 * we would not paing the scan and would get a dropout.
 * Check for this case and choose one of two pixels
 * which is closer to the "axis". We need to exclude
 * 'peak' because it would paint an excessive pixel.
 */
#define SET_MINIMAL_WIDTH(ixl, ixr, l, r) \
    if (ixl == ixr) \
        if ((!peak0 || iy >= peak_y0) && (!peak1 || iy <= peak_y1)) {\
            fixed x = int2fixed(ixl) + fixed_half;\
            if (x - l.x < r.x - x)\
                ++ixr;\
            else\
                --ixl;\
        }

#define CONNECT_RECTANGLES(ixl, ixr, rxl, rxr, iy, ry, adj1, adj2, fill)\
    if (adj1 < adj2) {\
        if (iy - ry > 1) {\
            code = fill(rxl, ry, rxr - rxl, iy - ry - 1);\
            if (code < 0)\
                goto xit;\
            ry = iy - 1;\
        }\
        adj1 = adj2 = (adj2 + adj2) / 2;\
    }

#else
#define SET_MINIMAL_WIDTH(ixl, ixr, l, r) DO_NOTHING
#define CONNECT_RECTANGLES(ixl, ixr, rxl, rxr, iy, ry, adj1, adj2, fill) DO_NOTHING
#endif
        if (fixed_floor(l.x) == fixed_pixround(x1l)) {
            /* Left edge is vertical, we don't need to increment. */
            l.di = 0, l.df = 0;
            fxl = 0;
        } else {
            compute_dx(&l, dxl, ysl);
            fxl = YMULT_QUO(ysl, l);
            l.x += fxl;
        }
        if (fixed_floor(r.x) == fixed_pixround(x1r)) {
            /* Right edge is vertical.  If both are vertical, */
            /* we have a rectangle. */
#	    if !LINEAR_COLOR
                if (l.di == 0 && l.df == 0) {
                    rxl = fixed2int_var(l.x);
                    rxr = fixed2int_var(r.x);
                    SET_MINIMAL_WIDTH(rxl, rxr, l, r);
                    code = FILL_TRAP_RECT(rxl, ry, rxr - rxl, iy1 - ry);
                    goto xit;
                }
#	    endif
            r.di = 0, r.df = 0;
        }
        /*
         * The test for fxl != 0 is required because the right edge might
         * cross some pixel centers even if the left edge doesn't.
         */
        else if (dxr == dxl && fxl != 0) {
            if (l.di == 0)
                r.di = 0, r.df = l.df;
            else
                compute_dx(&r, dxr, ysr);
            if (ysr == ysl && r.h == l.h)
                r.x += fxl;
            else
                r.x += YMULT_QUO(ysr, r);
        } else {
            compute_dx(&r, dxr, ysr);
            r.x += YMULT_QUO(ysr, r);
        }
        /* Compute one line's worth of dx/dy. */
        compute_ldx(&l, ysl);
        compute_ldx(&r, ysr);
        /* We subtracted fixed_epsilon from l.x, r.x to simplify rounding
           when the rational part is zero. Now add it back to get xl', xr' */
        l.x += fixed_epsilon;
        r.x += fixed_epsilon;
#	if LINEAR_COLOR
#	    ifdef DEBUG
                if (check_gradient_overflow(left, right)) {
                    /* The caller must care of.
                       Checking it here looses some performance with triangles. */
                    return_error(gs_error_unregistered);
                }
#	    endif
            lg.c = lgc;
            lg.f = lgf;
            lg.num = lgnum;
            rg.c = rgc;
            rg.f = rgf;
            rg.num = rgnum;
            xg.c = xgc;
            xg.f = xgf;
            xg.num = xgnum;
            code = init_gradient(&lg, fa, left, right, &l, ymin, num_components);
            if (code < 0)
                return code;
            code = init_gradient(&rg, fa, right, left, &r, ymin, num_components);
            if (code < 0)
                return code;

#	endif

#define rational_floor(tl)\
  fixed2int_var(fixed_is_int(tl.x) && tl.xf == -tl.h ? tl.x - fixed_1 : tl.x)
#define STEP_LINE(ix, tl)\
  tl.x += tl.ldi;\
  if ( (tl.xf += tl.ldf) >= 0 ) tl.xf -= tl.h, tl.x++;\
  ix = rational_floor(tl)

        rxl = rational_floor(l);
        rxr = rational_floor(r);
        SET_MINIMAL_WIDTH(rxl, rxr, l, r);
        while (LINEAR_COLOR ? 1 : ++iy != iy1) {
#	    if LINEAR_COLOR
                if (rxl != rxr) {
                    code = set_x_gradient(&xg, &lg, &rg, &l, &r, rxl, rxr, num_components);
                    if (code < 0)
                        goto xit;
                    code = FILL_TRAP_RECT(rxl, iy, rxr - rxl, 1);
                    if (code < 0)
                        goto xit;
                }
                if (++iy == iy1)
                    break;
                STEP_LINE(rxl, l);
                STEP_LINE(rxr, r);
                step_gradient(&lg, num_components);
                step_gradient(&rg, num_components);
#	    else
                register int ixl, ixr;

                STEP_LINE(ixl, l);
                STEP_LINE(ixr, r);
                SET_MINIMAL_WIDTH(ixl, ixr, l, r);
                if (ixl != rxl || ixr != rxr) {
                    CONNECT_RECTANGLES(ixl, ixr, rxl, rxr, iy, ry, rxr, ixl, FILL_TRAP_RECT);
                    CONNECT_RECTANGLES(ixl, ixr, rxl, rxr, iy, ry, ixr, rxl, FILL_TRAP_RECT);
                    code = FILL_TRAP_RECT(rxl, ry, rxr - rxl, iy - ry);
                    if (code < 0)
                        goto xit;
                    rxl = ixl, rxr = ixr, ry = iy;
                }
#	    endif
        }
#	if !LINEAR_COLOR
            code = FILL_TRAP_RECT(rxl, ry, rxr - rxl, iy - ry);
#	else
            code = 0;
#	endif
#undef STEP_LINE
#undef SET_MINIMAL_WIDTH
#undef CONNECT_RECTANGLES
#undef FILL_TRAP_RECT
#undef FILL_TRAP_RECT_DIRECT
#undef FILL_TRAP_RECT_INRECT
#undef YMULT_QUO
xit:	if (code < 0 && FILL_DIRECT)
            return_error(code);
        return_if_interrupt(dev->memory);
        return code;
    }
}

#undef GX_FILL_TRAPEZOID
#undef CONTIGUOUS_FILL
#undef EDGE_TYPE
#undef LINEAR_COLOR
#undef FILL_ATTRS
#undef register
#define gx_fill_trapezoid_as_fd gx_fill_trapezoid_tmpl<1, 1>
#define gx_fill_trapezoid_as_nd gx_fill_trapezoid_tmpl<1, 0>
#define gx_fill_trapezoid_ns_fd gx_fill_trapezoid_tmpl<0, 1>
#define gx_fill_trapezoid_ns_nd gx_fill_trapezoid_tmpl<0, 0>

int
gx_default_fill_trapezoid(gx_device * dev, const gs_fixed_edge * left,
    const gs_fixed_edge * right, fixed ybot, fixed ytop, bool swap_axes,
    const gx_device_color * pdevc, gs_logical_operation_t lop)
{
    bool fill_direct = color_writes_pure(pdevc, lop);

    if (swap_axes) {
        if (dev->width != 0 && dev->non_strict_bounds == 0)
        {
            /* Some devices init max->width to be int_max, which overflows when converted to fixed. */
            int dw = dev->width > max_int_in_fixed ? max_int_in_fixed : dev->width;
            if (ytop < 0)
                return 0;
            if (ybot < 0)
                ybot = 0;
            dw = int2fixed(dw);
            if (ybot > dw)
                return 0;
            if (ytop > dw)
                ytop = dw;
        }

        if (fill_direct)
            return gx_fill_trapezoid_as_fd(dev, left, right, ybot, ytop, 0, pdevc, lop);
        else
            return gx_fill_trapezoid_as_nd(dev, left, right, ybot, ytop, 0, pdevc, lop);
    } else {
        if (dev->height != 0 && dev->non_strict_bounds == 0)
        {
            /* Some devices init max->height to be int_max, which overflows when converted to fixed. */
            int dh = dev->height > max_int_in_fixed ? max_int_in_fixed : dev->height;
            if (ytop < 0)
                return 0;
            if (ybot < 0)
                ybot = 0;
            dh = int2fixed(dh);
            if (ybot > dh)
                return 0;
            if (ytop > dh)
                ytop = dh;
        }

        if (fill_direct)
            return gx_fill_trapezoid_ns_fd(dev, left, right, ybot, ytop, 0, pdevc, lop);
        else
            return gx_fill_trapezoid_ns_nd(dev, left, right, ybot, ytop, 0, pdevc, lop);
    }
}

/* ---------------- gxscanc.h ---------------- */

typedef struct gx_edgebuffer_s {
    int  base;
    int  height;
    int  xmin;
    int  xmax;
    int *index;
    int *table;
    /* pdfking: storage for index/table (gs uses gs_alloc_bytes). */
    std::vector<int> index_store;
    std::vector<int> table_store;
} gx_edgebuffer;

typedef int (gx_scan_convert_fn)(gx_device     * gs_restrict pdev,
                                 gx_path       * gs_restrict path,
                           const gs_fixed_rect * gs_restrict rect,
                                 gx_edgebuffer * gs_restrict edgebuffer,
                                 fixed                       flat);
typedef int (gx_filter_edgebuffer_fn)(gx_device       * gs_restrict pdev,
                                      gx_edgebuffer   * gs_restrict edgebuffer,
                                      int                        rule);
typedef int (gx_fill_edgebuffer_fn)(gx_device       * gs_restrict pdev,
                              const gx_device_color * gs_restrict pdevc,
                                    gx_edgebuffer   * gs_restrict edgebuffer,
                                    int                        log_op);
typedef struct {
    gx_scan_convert_fn      *scan_convert;
    gx_filter_edgebuffer_fn *filter;
    gx_fill_edgebuffer_fn   *fill;
} gx_scan_converter_t;

/* ================ BEGIN gxscanc.c (lines 138-4520) ================ */

/* NOTE: code in this file assumes that fixed and int can be used
 * interchangably. */

#undef DEBUG_SCAN_CONVERTER
#undef DEBUG_OUTPUT_SC_AS_PS

typedef int64_t fixed64;

enum
{
    DIRN_UNSET = -1,
    DIRN_UP = 0,
    DIRN_DOWN = 1
};

/* Centre of a pixel routines */

static int intcmp(const void *a, const void *b)
{
    return *((int*)a) - *((int *)b);
}

#if defined(DEBUG_SCAN_CONVERTER)
int debugging_scan_converter = 1;

static void
gx_edgebuffer_print(gx_edgebuffer * edgebuffer)
{
    int i;

    dlprintf1("Edgebuffer %x\n", edgebuffer);
    dlprintf4("xmin=%x xmax=%x base=%x height=%x\n",
              edgebuffer->xmin, edgebuffer->xmax, edgebuffer->base, edgebuffer->height);
    for (i=0; i < edgebuffer->height; i++) {
        int  offset = edgebuffer->index[i];
        int *row    = &edgebuffer->table[offset];
        int count   = *row++;
        dlprintf3("%d @ %d: %d =", i, offset, count);
        while (count-- > 0) {
            int v = *row++;
            dlprintf2(" %x:%d", v&~1, v&1);
        }
        dlprintf("\n");
    }
}
#endif

#ifdef DEBUG_OUTPUT_SC_AS_PS
static void coord(const char *str, fixed x, fixed y)
{
    if (x > 0)
        dlprintf1(" 16#%x ", x);
    else
        dlprintf1("0 16#%x sub ", -x);
    if (y > 0)
        dlprintf1(" 16#%x ", y);
    else
        dlprintf1("0 16#%x sub ", -y);
    dlprintf1("%s %%PS\n", str);
}
#endif

typedef void (zero_filler_fn)(int *, const fixed *);

static void mark_line_zero(fixed sx, fixed ex, fixed *zf)
{
    if (sx < zf[0])
        zf[0] = sx;
    if (ex < zf[0])
        zf[0] = ex;
    if (sx > zf[1])
        zf[1] = sx;
    if (ex > zf[1])
        zf[1] = ex;
}

static void mark_curve_zero(fixed sx, fixed c1x, fixed c2x, fixed ex, int depth, fixed *zf)
{
    fixed ax = (sx + c1x)>>1;
    fixed bx = (c1x + c2x)>>1;
    fixed cx = (c2x + ex)>>1;
    fixed dx = (ax + bx)>>1;
    fixed fx = (bx + cx)>>1;
    fixed gx = (dx + fx)>>1;

    assert(depth >= 0);
    if (depth == 0)
        mark_line_zero(sx, ex, zf);
    else {
        depth--;
        mark_curve_zero(sx, ax, dx, gx, depth, zf);
        mark_curve_zero(gx, fx, cx, ex, depth, zf);
    }
}

static void mark_curve_big_zero(fixed64 sx, fixed64 c1x, fixed64 c2x, fixed64 ex, int depth, fixed *zf)
{
    fixed64 ax = (sx + c1x)>>1;
    fixed64 bx = (c1x + c2x)>>1;
    fixed64 cx = (c2x + ex)>>1;
    fixed64 dx = (ax + bx)>>1;
    fixed64 fx = (bx + cx)>>1;
    fixed64 gx = (dx + fx)>>1;

    assert(depth >= 0);
    if (depth == 0)
        mark_line_zero((fixed)sx, (fixed)ex, zf);
    else {
        depth--;
        mark_curve_big_zero(sx, ax, dx, gx, depth, zf);
        mark_curve_big_zero(gx, fx, cx, ex, depth, zf);
    }
}

static void mark_curve_top_zero(fixed sx, fixed c1x, fixed c2x, fixed ex, int depth, fixed *zf)
{
    fixed test = (sx^(sx<<1))|(c1x^(c1x<<1))|(c2x^(c2x<<1))|(ex^(ex<<1));

    if (test < 0)
        mark_curve_big_zero(sx, c1x, c2x, ex, depth, zf);
    else
        mark_curve_zero(sx, c1x, c2x, ex, depth, zf);
}

static int
zero_case(gx_device      * gs_restrict pdev,
          gx_path        * gs_restrict path,
          gs_fixed_rect  * gs_restrict ibox,
          int            * gs_restrict index,
          int            * gs_restrict table,
          fixed                        fixed_flat,
          zero_filler_fn *             fill)
{
    const subpath *psub;
    fixed zf[2];

    /* Step 2 continued: Now we run through the path, filling in the real
     * values. */
    for (psub = path->first_subpath; psub != 0;) {
        const segment *pseg = (const segment *)psub;
        fixed ex = pseg->pt.x;
        fixed sy = pseg->pt.y;
        fixed ix = ex;
        int iy = fixed2int(pseg->pt.y);

        zf[0] = ex;
        zf[1] = ex;

        while ((pseg = pseg->next) != 0 &&
               pseg->type != s_start
            ) {
            fixed sx = ex;
            ex = pseg->pt.x;

            switch (pseg->type) {
                default:
                case s_start: /* Should never happen */
                case s_dash:  /* We should never be seeing a dash here */
                    assert("This should never happen" == NULL);
                    break;
                case s_curve: {
                    const curve_segment *const pcur = (const curve_segment *)pseg;
                    int k = gx_curve_log2_samples(sx, sy, pcur, fixed_flat);

                    mark_curve_top_zero(sx, pcur->p1.x, pcur->p2.x, ex, k, zf);
                    break;
                }
                case s_gap:
                case s_line:
                case s_line_close:
                    mark_line_zero(sx, ex, zf);
                    break;
            }
        }
        /* And close any open segments */
        mark_line_zero(ex, ix, zf);
        fill(&table[index[iy-ibox->p.y]], zf);
        psub = (const subpath *)pseg;
    }

    return 0;
}

static void mark_line(fixed sx, fixed sy, fixed ex, fixed ey, int base_y, int height, int *table, int *index)
{
    int64_t delta;
    int iy, ih;
    fixed clip_sy, clip_ey;
    int dirn = DIRN_UP;
    int *row;

#ifdef DEBUG_SCAN_CONVERTER
    if (debugging_scan_converter)
        dlprintf6("Marking line from %x,%x to %x,%x (%x,%x)\n", sx, sy, ex, ey, fixed2int(sy + fixed_half-1) - base_y, fixed2int(ey + fixed_half-1) - base_y);
#endif
#ifdef DEBUG_OUTPUT_SC_AS_PS
    dlprintf("0.001 setlinewidth 0 0 0 setrgbcolor %%PS\n");
    coord("moveto", sx, sy);
    coord("lineto", ex, ey);
    dlprintf("stroke %%PS\n");
#endif

    if (fixed2int(sy + fixed_half-1) == fixed2int(ey + fixed_half-1))
        return;
    if (sy > ey) {
        int t;
        t = sy; sy = ey; ey = t;
        t = sx; sx = ex; ex = t;
        dirn = DIRN_DOWN;
    }
    /* Lines go from sy to ey, closed at the start, open at the end. */
    /* We clip them to a region to make them closed at both ends. */
    /* Thus the first scanline marked (>= sy) is: */
    clip_sy = ((sy + fixed_half - 1) & ~(fixed_1-1)) | fixed_half;
    /* The last scanline marked (< ey) is: */
    clip_ey = ((ey - fixed_half - 1) & ~(fixed_1-1)) | fixed_half;
    /* Now allow for banding */
    if (clip_sy < int2fixed(base_y) + fixed_half)
        clip_sy = int2fixed(base_y) + fixed_half;
    if (ey <= clip_sy)
        return;
    if (clip_ey > int2fixed(base_y + height - 1) + fixed_half)
        clip_ey = int2fixed(base_y + height - 1) + fixed_half;
    if (sy > clip_ey)
        return;
    delta = (int64_t)clip_sy - (int64_t)sy;
    if (delta > 0)
    {
        int64_t dx = (int64_t)ex - (int64_t)sx;
        int64_t dy = (int64_t)ey - (int64_t)sy;
        int advance = (int)((dx * delta + (dy>>1)) / dy);
        sx += advance;
        sy += delta;
    }
    delta = (int64_t)ey - (int64_t)clip_ey;
    if (delta > 0)
    {
        int64_t dx = (int64_t)ex - (int64_t)sx;
        int64_t dy = (int64_t)ey - (int64_t)sy;
        int advance = (int)((dx * delta + (dy>>1)) / dy);
        ex -= advance;
        ey -= delta;
    }
    ex -= sx;
    ey -= sy;
    ih = fixed2int(ey);
    assert(ih >= 0);
    iy = fixed2int(sy) - base_y;
#ifdef DEBUG_SCAN_CONVERTER
    if (debugging_scan_converter)
        dlprintf2("    iy=%x ih=%x\n", iy, ih);
#endif
    assert(iy >= 0 && iy < height);
    /* We always cross at least one scanline */
    row = &table[index[iy]];
    *row = (*row)+1; /* Increment the count */
    row[*row] = (sx&~1) | dirn;
    if (ih == 0)
        return;
    if (ex >= 0) {
        int x_inc, n_inc, f;

        /* We want to change sx by ex in ih steps. So each step, we add
         * ex/ih to sx. That's x_inc + n_inc/ih.
         */
        x_inc = ex/ih;
        n_inc = ex-(x_inc*ih);
        f     = ih>>1;
        delta = ih;
        do {
            int count;
            iy++;
            sx += x_inc;
            f  -= n_inc;
            if (f < 0) {
                f += ih;
                sx++;
            }
            assert(iy >= 0 && iy < height);
            row = &table[index[iy]];
            count = *row = (*row)+1; /* Increment the count */
            row[count] = (sx&~1) | dirn;
        } while (--delta);
    } else {
        int x_dec, n_dec, f;

        ex = -ex;
        /* We want to change sx by ex in ih steps. So each step, we subtract
         * ex/ih from sx. That's x_dec + n_dec/ih.
         */
        x_dec = ex/ih;
        n_dec = ex-(x_dec*ih);
        f     = ih>>1;
        delta = ih;
        do {
            int count;
            iy++;
            sx -= x_dec;
            f  -= n_dec;
            if (f < 0) {
                f += ih;
                sx--;
            }
            assert(iy >= 0 && iy < height);
            row = &table[index[iy]];
            count = *row = (*row)+1; /* Increment the count */
            row[count] = (sx&~1) | dirn;
        } while (--delta);
    }
}

static void mark_curve(fixed sx, fixed sy, fixed c1x, fixed c1y, fixed c2x, fixed c2y, fixed ex, fixed ey, fixed base_y, fixed height, int *table, int *index, int depth)
{
    fixed ax = (sx + c1x)>>1;
    fixed ay = (sy + c1y)>>1;
    fixed bx = (c1x + c2x)>>1;
    fixed by = (c1y + c2y)>>1;
    fixed cx = (c2x + ex)>>1;
    fixed cy = (c2y + ey)>>1;
    fixed dx = (ax + bx)>>1;
    fixed dy = (ay + by)>>1;
    fixed fx = (bx + cx)>>1;
    fixed fy = (by + cy)>>1;
    fixed gx = (dx + fx)>>1;
    fixed gy = (dy + fy)>>1;

    assert(depth >= 0);
    if (depth == 0)
        mark_line(sx, sy, ex, ey, base_y, height, table, index);
    else {
        depth--;
        mark_curve(sx, sy, ax, ay, dx, dy, gx, gy, base_y, height, table, index, depth);
        mark_curve(gx, gy, fx, fy, cx, cy, ex, ey, base_y, height, table, index, depth);
    }
}

static void mark_curve_big(fixed64 sx, fixed64 sy, fixed64 c1x, fixed64 c1y, fixed64 c2x, fixed64 c2y, fixed64 ex, fixed64 ey, fixed base_y, fixed height, int *table, int *index, int depth)
{
    fixed64 ax = (sx + c1x)>>1;
    fixed64 ay = (sy + c1y)>>1;
    fixed64 bx = (c1x + c2x)>>1;
    fixed64 by = (c1y + c2y)>>1;
    fixed64 cx = (c2x + ex)>>1;
    fixed64 cy = (c2y + ey)>>1;
    fixed64 dx = (ax + bx)>>1;
    fixed64 dy = (ay + by)>>1;
    fixed64 fx = (bx + cx)>>1;
    fixed64 fy = (by + cy)>>1;
    fixed64 gx = (dx + fx)>>1;
    fixed64 gy = (dy + fy)>>1;

    assert(depth >= 0);
    if (depth == 0)
        mark_line((fixed)sx, (fixed)sy, (fixed)ex, (fixed)ey, base_y, height, table, index);
    else {
        depth--;
        mark_curve_big(sx, sy, ax, ay, dx, dy, gx, gy, base_y, height, table, index, depth);
        mark_curve_big(gx, gy, fx, fy, cx, cy, ex, ey, base_y, height, table, index, depth);
    }
}

static void mark_curve_top(fixed sx, fixed sy, fixed c1x, fixed c1y, fixed c2x, fixed c2y, fixed ex, fixed ey, fixed base_y, fixed height, int *table, int *index, int depth)
{
    fixed test = (sx^(sx<<1))|(sy^(sy<<1))|(c1x^(c1x<<1))|(c1y^(c1y<<1))|(c2x^(c2x<<1))|(c2y^(c2y<<1))|(ex^(ex<<1))|(ey^(ey<<1));

    if (test < 0)
        mark_curve_big(sx, sy, c1x, c1y, c2x, c2y, ex, ey, base_y, height, table, index, depth);
    else
        mark_curve(sx, sy, c1x, c1y, c2x, c2y, ex, ey, base_y, height, table, index, depth);
}

static int make_bbox(gx_path       * path,
               const gs_fixed_rect * clip,
                     gs_fixed_rect * bbox,
                     gs_fixed_rect * ibox,
                     fixed           adjust)
{
    int           code;
    int           ret = 0;

    /* Find the bbox - fixed */
    code = gx_path_bbox(path, bbox);
    if (code < 0)
        return code;

    if (bbox->p.y == bbox->q.y) {
        /* Zero height path */
        if (!clip ||
            (bbox->p.y >= clip->p.y && bbox->q.y <= clip->q.y)) {
            /* Either we're not clipping, or we are vertically inside the clip */
            if (clip) {
                if (bbox->p.x < clip->p.x)
                    bbox->p.x = clip->p.x;
                if (bbox->q.x > clip->q.x)
                    bbox->q.x = clip->q.x;
            }
            if (bbox->p.x <= bbox->q.x) {
                /* Zero height rectangle, not clipped completely away */
                ret = 1;
            }
        }
    }

    if (clip) {
        if (bbox->p.y < clip->p.y)
            bbox->p.y = clip->p.y;
        if (bbox->q.y > clip->q.y)
            bbox->q.y = clip->q.y;
    }

    /* Convert to bbox - int */
    ibox->p.x = fixed2int(bbox->p.x+adjust-(adjust?1:0));
    ibox->p.y = fixed2int(bbox->p.y+adjust-(adjust?1:0));
    ibox->q.x = fixed2int(bbox->q.x-adjust+fixed_1);
    ibox->q.y = fixed2int(bbox->q.y-adjust+fixed_1);

    return ret;
}

static inline int
make_table_template(gx_device     * pdev,
                    gx_path       * path,
                    gs_fixed_rect * ibox,
                    int             intersection_size,
                    int             adjust,
                    int           * scanlinesp,
                    int          ** indexp,
                    int          ** tablep,
                    gx_edgebuffer * edgebuffer) /* pdfking: owns the storage */
{
    int             scanlines;
    const subpath * gs_restrict psub;
    int           * gs_restrict index;
    int           * gs_restrict table;
    int             i;
    int64_t         offset;
    int             delta;
    fixed           base_y;

    *scanlinesp = 0;
    *indexp     = NULL;
    *tablep     = NULL;

    if (pdev->max_fill_band != 0)
        ibox->p.y &= ~(pdev->max_fill_band-1);
    base_y = ibox->p.y;

    /* Previously we took adjust as a fixed distance to add to miny/maxy
     * to allow for the expansion due to 'any part of a pixel'. This causes
     * problems with over/underflow near INT_MAX/INT_MIN, so instead we
     * take adjust as boolean telling us whether to expand y by 1 or not, and
     * then adjust the assignments into the index as appropriate. This
     * solves Bug 697970. */

    /* Step 1: Make us a table */
    scanlines = ibox->q.y-base_y;
    /* +1+adjust simplifies the loop below */
    edgebuffer->index_store.assign((size_t)(scanlines+1+adjust), 0);
    index = edgebuffer->index_store.data();
    if (index == NULL)
        return_error(gs_error_VMerror);

    /* Step 1 continued: Blank the index */
    memset(index, 0, (scanlines+1)*sizeof(int));

    /* Step 1 continued: Run through the path, filling in the index */
    for (psub = path->first_subpath; psub != 0;) {
        const segment * gs_restrict pseg = (const segment *)psub;
        fixed          ey = pseg->pt.y;
        fixed          iy = ey;
        int            iey = fixed2int(iy) - base_y;

        assert(pseg->type == s_start);

        /* Allow for 2 extra intersections on the start scanline.
         * This copes with the 'zero height rectangle' case. */
        if (iey >= 0 && iey < scanlines)
        {
            index[iey] += 2;
            if (iey+1 < scanlines)
                index[iey+1] -= 2;
        }

        while ((pseg = pseg->next) != 0 &&
               pseg->type != s_start
            ) {
            fixed sy = ey;
            ey = pseg->pt.y;

            switch (pseg->type) {
                default:
                case s_start: /* Should never happen */
                case s_dash:  /* We should never be seeing a dash here */
                    assert("This should never happen" == NULL);
                    break;
                case s_curve: {
                    const curve_segment *const gs_restrict pcur = (const curve_segment *)pseg;
                    fixed c1y = pcur->p1.y;
                    fixed c2y = pcur->p2.y;
                    fixed maxy = sy, miny = sy;
                    int imaxy, iminy;
                    if (miny > c1y)
                        miny = c1y;
                    if (miny > c2y)
                        miny = c2y;
                    if (miny > ey)
                        miny = ey;
                    if (maxy < c1y)
                        maxy = c1y;
                    if (maxy < c2y)
                        maxy = c2y;
                    if (maxy < ey)
                        maxy = ey;
#ifdef DEBUG_SCAN_CONVERTER
                    if (debugging_scan_converter)
                        dlprintf2("Curve (%x->%x) ", miny, maxy);
#endif
                    iminy = fixed2int(miny) - base_y;
                    if (iminy <= 0)
                        iminy = 0;
                    else
                        iminy -= adjust;
                    if (iminy < scanlines) {
                        imaxy = fixed2int(maxy) - base_y;
                        if (imaxy >= 0) {
#ifdef DEBUG_SCAN_CONVERTER
                            if (debugging_scan_converter)
                                dlprintf1("+%x ", iminy);
#endif
                            index[iminy]+=3;
                            if (imaxy < scanlines) {
#ifdef DEBUG_SCAN_CONVERTER
                                if (debugging_scan_converter)
                                    dlprintf1("-%x ", imaxy+1);
#endif
                                index[imaxy+1+adjust]-=3;
                            }
                        }
                    }
#ifdef DEBUG_SCAN_CONVERTER
                    if (debugging_scan_converter)
                        dlprintf("\n");
#endif
                    break;
                }
                case s_gap:
                case s_line:
                case s_line_close: {
                    fixed miny, maxy;
                    int imaxy, iminy;
                    if (sy == ey) {
#ifdef DEBUG_SCAN_CONVERTER
                        if (debugging_scan_converter)
                            dlprintf("Line (Horiz)\n");
#endif
                        break;
                    }
                    if (sy < ey)
                        miny = sy, maxy = ey;
                    else
                        miny = ey, maxy = sy;
#ifdef DEBUG_SCAN_CONVERTER
                    if (debugging_scan_converter)
                        dlprintf2("Line (%x->%x) ", miny, maxy);
#endif
                    iminy = fixed2int(miny) - base_y;
                    if (iminy <= 0)
                        iminy = 0;
                    else
                        iminy -= adjust;
                    if (iminy < scanlines) {
                        imaxy = fixed2int(maxy) - base_y;
                        if (imaxy >= 0) {
#ifdef DEBUG_SCAN_CONVERTER
                            if (debugging_scan_converter)
                                dlprintf1("+%x ", iminy);
#endif
                            index[iminy]++;
                            if (imaxy < scanlines) {
#ifdef DEBUG_SCAN_CONVERTER
                                if (debugging_scan_converter)
                                    dlprintf1("-%x ", imaxy+1);
#endif
                                index[imaxy+1+adjust]--;
                            }
                        }
                    }
#ifdef DEBUG_SCAN_CONVERTER
                    if (debugging_scan_converter)
                        dlprintf("\n");
#endif
                    break;
                }
            }
        }

        /* And close any segments that need it */
        if (ey != iy) {
            fixed miny, maxy;
            int imaxy, iminy;
            if (iy < ey)
                miny = iy, maxy = ey;
            else
                miny = ey, maxy = iy;
#ifdef DEBUG_SCAN_CONVERTER
            if (debugging_scan_converter)
                dlprintf2("Close (%x->%x) ", miny, maxy);
#endif
            iminy = fixed2int(miny) - base_y;
            if (iminy <= 0)
                iminy = 0;
            else
                iminy -= adjust;
            if (iminy < scanlines) {
                imaxy = fixed2int(maxy) - base_y;
                if (imaxy >= 0) {
#ifdef DEBUG_SCAN_CONVERTER
                    if (debugging_scan_converter)
                        dlprintf1("+%x ", iminy);
#endif
                    index[iminy]++;
                    if (imaxy < scanlines) {
#ifdef DEBUG_SCAN_CONVERTER
                        if (debugging_scan_converter)
                            dlprintf1("-%x ", imaxy+1);
#endif
                        index[imaxy+1+adjust]--;
                    }
                }
            }
#ifdef DEBUG_SCAN_CONVERTER
            if (debugging_scan_converter)
                dlprintf("\n");
#endif
        }
#ifdef DEBUG_SCAN_CONVERTER
        if (debugging_scan_converter)
            dlprintf("\n");
#endif
        psub = (const subpath *)pseg;
    }

    /* Step 1 continued: index now contains a list of deltas (how the
     * number of intersects on line x differs from the number on line x-1).
     * First convert them to be the real number of intersects on that line.
     * Sum these values to get us the total number of intersects. Then
     * convert the table to be a list of offsets into the real intersect
     * buffer. */
    offset = 0;
    delta  = 0;
    for (i=0; i < scanlines+adjust; i++) {
        delta    += intersection_size*index[i];  /* delta = Num ints on this scanline. */
        index[i]  = offset;                      /* Offset into table for this lines data. */
        offset   += delta+1;                     /* Adjust offset for next line. */
    }
    /* Ensure we always have enough room for our zero height rectangle hack. */
    if (offset < 2*intersection_size)
        offset += 2*intersection_size;
    offset *= sizeof(*table);

    /* Try to keep the size to 1Meg. This is enough for the vast majority
     * of files. Allow us to grow above this if it would mean dropping
     * the height below a suitably small number (set to be larger than
     * any max_fill_band we might meet). */
    if (scanlines > 16 && offset > 1024*1024) { /* Arbitrary */
        edgebuffer->index_store = std::vector<int>();
        return offset/(1024*1024) + 1;
    }

    /* In the case where we have let offset be large, at least make sure
     * it's not TOO large for us to malloc. */
    if (offset != (int64_t)(uint)offset)
    {
        edgebuffer->index_store = std::vector<int>();
        return_error(gs_error_VMerror);
    }

    /* End of step 1: index[i] = offset into table 2 for scanline i's
     * intersection data. offset = Total number of int entries required for
     * table. */

    /* Step 2: Collect the real intersections */
    edgebuffer->table_store.assign((size_t)offset / sizeof(*table), 0);
    table = edgebuffer->table_store.data();
    if (table == NULL) {
        edgebuffer->index_store = std::vector<int>();
        return_error(gs_error_VMerror);
    }

    /* Step 2 continued: initialise table's data; each scanlines data starts
     * with a count of the number of intersects so far, followed by a record
     * of the intersect points on this scanline. */
    for (i=0; i < scanlines; i++) {
        table[index[i]] = 0;
    }

    *scanlinesp = scanlines;
    *tablep     = table;
    *indexp     = index;

    return 0;
}

static int make_table(gx_device     * pdev,
                      gx_path       * path,
                      gs_fixed_rect * ibox,
                      int           * scanlines,
                      int          ** index,
                      int          ** table,
                      gx_edgebuffer * edgebuffer)
{
    return make_table_template(pdev, path, ibox, 1, 1, scanlines, index, table, edgebuffer);
}

static void
fill_zero(int *row, const fixed *x)
{
    int n = *row = (*row)+2; /* Increment the count */
    row[n-1] = (x[0]&~1);
    row[n  ] = (x[1]|1);
}

int gx_scan_convert(gx_device     * gs_restrict pdev,
                    gx_path       * gs_restrict path,
              const gs_fixed_rect * gs_restrict clip,
                    gx_edgebuffer * gs_restrict edgebuffer,
                    fixed                       fixed_flat)
{
    gs_fixed_rect  ibox;
    gs_fixed_rect  bbox;
    int            scanlines;
    const subpath *psub;
    int           *index;
    int           *table;
    int            i;
    int            code;
    int            zero;

    edgebuffer->index = NULL;
    edgebuffer->table = NULL;

    /* Bale out if no actual path. We see this with the clist */
    if (path->first_subpath == NULL)
        return 0;

    zero = make_bbox(path, clip, &bbox, &ibox, fixed_half);
    if (zero < 0)
        return zero;

    if (ibox.q.y <= ibox.p.y)
        return 0;

    code = make_table(pdev, path, &ibox, &scanlines, &index, &table, edgebuffer);
    if (code != 0) /* >0 means "retry with smaller height" */
        return code;

    if (scanlines == 0)
        return 0;

    if (zero) {
        code = zero_case(pdev, path, &ibox, index, table, fixed_flat, fill_zero);
    } else {

    /* Step 2 continued: Now we run through the path, filling in the real
     * values. */
    for (psub = path->first_subpath; psub != 0;) {
        const segment *pseg = (const segment *)psub;
        fixed ex = pseg->pt.x;
        fixed ey = pseg->pt.y;
        fixed ix = ex;
        fixed iy = ey;

        while ((pseg = pseg->next) != 0 &&
               pseg->type != s_start
            ) {
            fixed sx = ex;
            fixed sy = ey;
            ex = pseg->pt.x;
            ey = pseg->pt.y;

            switch (pseg->type) {
                default:
                case s_start: /* Should never happen */
                case s_dash:  /* We should never be seeing a dash here */
                    assert("This should never happen" == NULL);
                    break;
                case s_curve: {
                    const curve_segment *const pcur = (const curve_segment *)pseg;
                    int k = gx_curve_log2_samples(sx, sy, pcur, fixed_flat);

                    mark_curve_top(sx, sy, pcur->p1.x, pcur->p1.y, pcur->p2.x, pcur->p2.y, ex, ey, ibox.p.y, scanlines, table, index, k);
                    break;
                }
                case s_gap:
                case s_line:
                case s_line_close:
                    if (sy != ey)
                        mark_line(sx, sy, ex, ey, ibox.p.y, scanlines, table, index);
                    break;
            }
        }
        /* And close any open segments */
        if (iy != ey)
            mark_line(ex, ey, ix, iy, ibox.p.y, scanlines, table, index);
        psub = (const subpath *)pseg;
    }
    }

    /* Step 2 complete: We now have a complete list of intersection data in
     * table, indexed by index. */

    edgebuffer->base   = ibox.p.y;
    edgebuffer->height = scanlines;
    edgebuffer->xmin   = ibox.p.x;
    edgebuffer->xmax   = ibox.q.x;
    edgebuffer->index  = index;
    edgebuffer->table  = table;

#ifdef DEBUG_SCAN_CONVERTER
    if (debugging_scan_converter) {
        dlprintf("Before sorting:\n");
        gx_edgebuffer_print(edgebuffer);
    }
#endif

    /* Step 3: Sort the intersects on x */
    for (i=0; i < scanlines; i++) {
        int *row = &table[index[i]];
        int  rowlen = *row++;

        /* Bubblesort short runs, qsort longer ones. */
        /* FIXME: Check "6" below */
        if (rowlen <= 6) {
            int j, k;
            for (j = 0; j < rowlen-1; j++) {
                int t = row[j];
                for (k = j+1; k < rowlen; k++) {
                    int s = row[k];
                    if (t > s)
                         row[k] = t, t = row[j] = s;
                }
            }
        } else
            qsort(row, rowlen, sizeof(int), intcmp);
    }

    return 0;
}

/* Step 5: Filter the intersections according to the rules */
int
gx_filter_edgebuffer(gx_device       * gs_restrict pdev,
                     gx_edgebuffer   * gs_restrict edgebuffer,
                     int                        rule)
{
    int i;

#ifdef DEBUG_SCAN_CONVERTER
    if (debugging_scan_converter) {
        dlprintf("Before filtering:\n");
        gx_edgebuffer_print(edgebuffer);
    }
#endif

    for (i=0; i < edgebuffer->height; i++) {
        int *row      = &edgebuffer->table[edgebuffer->index[i]];
        int *rowstart = row;
        int  rowlen   = *row++;
        int *rowout   = row;

        while (rowlen > 0)
        {
            int left, right;

            if (rule == gx_rule_even_odd) {
                /* Even Odd */
                left  = (*row++)&~1;
                right = (*row++)&~1;
                rowlen -= 2;
            } else {
                /* Non-Zero */
                int w;

                left = *row++;
                w = ((left&1)-1) | (left&1);
                rowlen--;
                do {
                    right  = *row++;
                    rowlen--;
                    w += ((right&1)-1) | (right&1);
                } while (w != 0);
                left &= ~1;
                right &= ~1;
            }

            if (right > left) {
                *rowout++ = left;
                *rowout++ = right;
            }
        }
        *rowstart = (rowout-rowstart)-1;
    }
    return 0;
}

/* Step 6: Fill the edgebuffer */
int
gx_fill_edgebuffer(gx_device       * gs_restrict pdev,
             const gx_device_color * gs_restrict pdevc,
                   gx_edgebuffer   * gs_restrict edgebuffer,
                   int                        log_op)
{
    int i, code;

    for (i=0; i < edgebuffer->height; i++) {
        int *row    = &edgebuffer->table[edgebuffer->index[i]];
        int  rowlen = *row++;

        while (rowlen > 0) {
            int left, right;

            left  = *row++;
            right = *row++;
            rowlen -= 2;
            left  = fixed2int(left + fixed_half);
            right = fixed2int(right + fixed_half);
            right -= left;
            if (right > 0) {
#ifdef DEBUG_OUTPUT_SC_AS_PS
                dlprintf("0.001 setlinewidth 1 0.5 0 setrgbcolor %% orange %%PS\n");
                coord("moveto", int2fixed(left), int2fixed(edgebuffer->base+i));
                coord("lineto", int2fixed(left+right), int2fixed(edgebuffer->base+i));
                coord("lineto", int2fixed(left+right), int2fixed(edgebuffer->base+i+1));
                coord("lineto", int2fixed(left), int2fixed(edgebuffer->base+i+1));
                dlprintf("closepath stroke %%PS\n");
#endif
                if (log_op < 0)
                    code = dev_proc(pdev, fill_rectangle)(pdev, left, edgebuffer->base+i, right, 1, pdevc->colors.pure);
                else
                    code = gx_fill_rectangle_device_rop(left, edgebuffer->base+i, right, 1, pdevc, pdev, (gs_logical_operation_t)log_op);
                if (code < 0)
                    return code;
            }
        }
    }
    return 0;
}

/* Any part of a pixel routines */

static int edgecmp(const void *a, const void *b)
{
    int left  = ((int*)a)[0];
    int right = ((int*)b)[0];
    left -= right;
    if (left)
        return left;
    return ((int*)a)[1] - ((int*)b)[1];
}

#ifdef DEBUG_SCAN_CONVERTER
static void
gx_edgebuffer_print_app(gx_edgebuffer * edgebuffer)
{
    int i;
    int borked = 0;

    if (!debugging_scan_converter)
        return;

    dlprintf1("Edgebuffer %x\n", edgebuffer);
    dlprintf4("xmin=%x xmax=%x base=%x height=%x\n",
              edgebuffer->xmin, edgebuffer->xmax, edgebuffer->base, edgebuffer->height);
    for (i=0; i < edgebuffer->height; i++) {
        int  offset = edgebuffer->index[i];
        int *row    = &edgebuffer->table[offset];
        int count   = *row++;
        int c       = count;
        int wind    = 0;
        dlprintf3("%x @ %d: %d =", i, offset, count);
        while (count-- > 0) {
            int left  = *row++;
            int right = *row++;
            int w     = -(left&1) | 1;
            wind += w;
            dlprintf3(" (%x,%x)%c", left&~1, right, left&1 ? 'v' : '^');
        }
        if (wind != 0 || c & 1) {
            dlprintf(" <- BROKEN");
            borked = 1;
        }
        dlprintf("\n");
    }
    if (borked) {
        borked = borked; /* Breakpoint here */
    }
}
#endif

typedef struct
{
    fixed  left;
    fixed  right;
    fixed  y;
    signed char d; /* 0 up (or horiz), 1 down, -1 uninited */
    unsigned char first;
    unsigned char saved;

    fixed  save_left;
    fixed  save_right;
    int    save_iy;
    int    save_d;

    int    scanlines;
    int   *table;
    int   *index;
    int    base;
} cursor;

static inline void
cursor_output(cursor * gs_restrict cr, int iy)
{
    int *row;
    int count;

    if (iy >= 0 && iy < cr->scanlines) {
        if (cr->first) {
            /* Save it for later in case we join up */
            cr->save_left  = cr->left;
            cr->save_right = cr->right;
            cr->save_iy    = iy;
            cr->save_d     = cr->d;
            cr->saved      = 1;
        } else if (cr->d != DIRN_UNSET) {
            /* Enter it into the table */
            row = &cr->table[cr->index[iy]];
            *row = count = (*row)+1; /* Increment the count */
            row[2 * count - 1] = (cr->left&~1) | cr->d;
            row[2 * count    ] = cr->right;
        } else {
            assert(cr->left == max_fixed && cr->right == min_fixed);
        }
    }
    cr->first = 0;
}

static inline void
cursor_output_inrange(cursor * gs_restrict cr, int iy)
{
    int *row;
    int count;

    assert(iy >= 0 && iy < cr->scanlines);
    if (cr->first) {
        /* Save it for later in case we join up */
        cr->save_left  = cr->left;
        cr->save_right = cr->right;
        cr->save_iy    = iy;
        cr->save_d     = cr->d;
        cr->saved      = 1;
    } else {
        /* Enter it into the table */
        assert(cr->d != DIRN_UNSET);

        row = &cr->table[cr->index[iy]];
        *row = count = (*row)+1; /* Increment the count */
        row[2 * count - 1] = (cr->left&~1) | cr->d;
        row[2 * count    ] = cr->right;
    }
    cr->first = 0;
}

/* Step the cursor in y, allowing for maybe crossing a scanline */
static inline void
cursor_step(cursor * gs_restrict cr, fixed dy, fixed x, int skip)
{
    int new_iy;
    int iy = fixed2int(cr->y) - cr->base;

    cr->y += dy;
    new_iy = fixed2int(cr->y) - cr->base;
    if (new_iy != iy) {
        if (!skip)
            cursor_output(cr, iy);
        cr->left = x;
        cr->right = x;
    } else {
        if (x < cr->left)
            cr->left = x;
        if (x > cr->right)
            cr->right = x;
    }
}

/* Step the cursor in y, never by enough to cross a scanline. */
static inline void
cursor_never_step_vertical(cursor * gs_restrict cr, fixed dy, fixed x)
{
    assert(fixed2int(cr->y+dy) == fixed2int(cr->y));

    cr->y += dy;
}

/* Step the cursor in y, never by enough to cross a scanline,
 * knowing that we are moving left, and that the right edge
 * has already been accounted for. */
static inline void
cursor_never_step_left(cursor * gs_restrict cr, fixed dy, fixed x)
{
    assert(fixed2int(cr->y+dy) == fixed2int(cr->y));

    if (x < cr->left)
        cr->left = x;
    cr->y += dy;
}

/* Step the cursor in y, never by enough to cross a scanline,
 * knowing that we are moving right, and that the left edge
 * has already been accounted for. */
static inline void
cursor_never_step_right(cursor * gs_restrict cr, fixed dy, fixed x)
{
    assert(fixed2int(cr->y+dy) == fixed2int(cr->y));

    if (x > cr->right)
        cr->right = x;
    cr->y += dy;
}

/* Step the cursor in y, always by enough to cross a scanline. */
static inline void
cursor_always_step(cursor * gs_restrict cr, fixed dy, fixed x, int skip)
{
    int iy = fixed2int(cr->y) - cr->base;

    if (!skip)
        cursor_output(cr, iy);
    cr->y += dy;
    cr->left = x;
    cr->right = x;
}

/* Step the cursor in y, always by enough to cross a scanline, as
 * part of a vertical line, knowing that we are moving from a
 * position guaranteed to be in the valid y range. */
static inline void
cursor_always_step_inrange_vertical(cursor * gs_restrict cr, fixed dy, fixed x)
{
    int iy = fixed2int(cr->y) - cr->base;

    cursor_output(cr, iy);
    cr->y += dy;
}

/* Step the cursor in y, always by enough to cross a scanline, as
 * part of a left moving line, knowing that we are moving from a
 * position guaranteed to be in the valid y range. */
static inline void
cursor_always_inrange_step_left(cursor * gs_restrict cr, fixed dy, fixed x)
{
    int iy = fixed2int(cr->y) - cr->base;

    cr->y += dy;
    cursor_output_inrange(cr, iy);
    cr->right = x;
}

/* Step the cursor in y, always by enough to cross a scanline, as
 * part of a right moving line, knowing that we are moving from a
 * position guaranteed to be in the valid y range. */
static inline void
cursor_always_inrange_step_right(cursor * gs_restrict cr, fixed dy, fixed x)
{
    int iy = fixed2int(cr->y) - cr->base;

    cr->y += dy;
    cursor_output_inrange(cr, iy);
    cr->left = x;
}

static inline void cursor_left_merge(cursor * gs_restrict cr, fixed x)
{
    if (x < cr->left)
        cr->left = x;
}

static inline void cursor_left(cursor * gs_restrict cr, fixed x)
{
    cr->left = x;
}

static inline void cursor_right_merge(cursor * gs_restrict cr, fixed x)
{
    if (x > cr->right)
        cr->right = x;
}

static inline void cursor_right(cursor * gs_restrict cr, fixed x)
{
    cr->right = x;
}

static inline int cursor_down(cursor * gs_restrict cr, fixed x)
{
    int skip = 0;
    if ((cr->y & 0xff) == 0)
        skip = 1;
    if (cr->d == DIRN_UP)
    {
        if (!skip)
            cursor_output(cr, fixed2int(cr->y) - cr->base);
        cr->left = x;
        cr->right = x;
    }
    cr->d = DIRN_DOWN;
    return skip;
}

static inline void cursor_up(cursor * gs_restrict cr, fixed x)
{
    if (cr->d == DIRN_DOWN)
    {
        cursor_output(cr, fixed2int(cr->y) - cr->base);
        cr->left = x;
        cr->right = x;
    }
    cr->d = DIRN_UP;
}

static inline void
cursor_flush(cursor * gs_restrict cr, fixed x)
{
    int iy;

    /* This should only happen if we were entirely out of bounds,
     * or if everything was within a zero height horizontal
     * rectangle from the start point. */
    if (cr->first) {
        int iy = fixed2int(cr->y) - cr->base;
        /* Any zero height rectangle counts as filled, except
         * those on the baseline of a pixel. */
        if (cr->d == DIRN_UNSET && (cr->y & 0xff) == 0)
            return;
        assert(cr->left != max_fixed && cr->right != min_fixed);
        if (iy >= 0 && iy < cr->scanlines) {
            int *row = &cr->table[cr->index[iy]];
            int count = *row = (*row)+2; /* Increment the count */
            row[2 * count - 3] = (cr->left & ~1) | DIRN_UP;
            row[2 * count - 2] = (cr->right & ~1);
            row[2 * count - 1] = (cr->right & ~1) | DIRN_DOWN;
            row[2 * count    ] = cr->right;
        }
        return;
    }

    /* Merge save into current if we can */
    iy = fixed2int(cr->y) - cr->base;
    if (cr->saved && iy == cr->save_iy &&
        (cr->d == cr->save_d || cr->save_d == DIRN_UNSET)) {
        if (cr->left > cr->save_left)
            cr->left = cr->save_left;
        if (cr->right < cr->save_right)
            cr->right = cr->save_right;
        cursor_output(cr, iy);
        return;
    }

    /* Merge not possible */
    cursor_output(cr, iy);
    if (cr->saved) {
        cr->left  = cr->save_left;
        cr->right = cr->save_right;
        assert(cr->save_d != DIRN_UNSET);
        if (cr->save_d != DIRN_UNSET)
            cr->d = cr->save_d;
        cursor_output(cr, cr->save_iy);
    }
}

static inline void
cursor_null(cursor *cr)
{
    cr->right = min_fixed;
    cr->left  = max_fixed;
    cr->d     = DIRN_UNSET;
}

static void mark_line_app(cursor * gs_restrict cr, fixed sx, fixed sy, fixed ex, fixed ey)
{
    int isy, iey;
    fixed saved_sy = sy;
    fixed saved_ex = ex;
    fixed saved_ey = ey;
    int truncated;

    if (sx == ex && sy == ey)
        return;

    isy = fixed2int(sy) - cr->base;
    iey = fixed2int(ey) - cr->base;
#ifdef DEBUG_SCAN_CONVERTER
    if (debugging_scan_converter)
        dlprintf6("Marking line (app) from %x,%x to %x,%x (%x,%x)\n", sx, sy, ex, ey, isy, iey);
#endif
#ifdef DEBUG_OUTPUT_SC_AS_PS
    dlprintf("0.001 setlinewidth 0 0 0 setrgbcolor %%PS\n");
    coord("moveto", sx, sy);
    coord("lineto", ex, ey);
    dlprintf("stroke %%PS\n");
#endif

    /* Horizontal motion at the bottom of a pixel is ignored */
    if (sy == ey && (sy & 0xff) == 0)
        return;

    assert(cr->y == sy &&
           ((cr->left <= sx && cr->right >= sx) || ((sy & 0xff) == 0)) &&
           cr->d >= DIRN_UNSET && cr->d <= DIRN_DOWN);

    if (isy < iey) {
        /* Rising line */
        if (iey < 0 || isy >= cr->scanlines) {
            /* All line is outside. */
            if ((ey & 0xff) == 0)
                cursor_null(cr);
            else {
                cr->left = ex;
                cr->right = ex;
            }
            cr->y = ey;
            cr->first = 0;
            return;
        }
        if (isy < 0) {
            /* Move sy up */
            int64_t y = (int64_t)ey - (int64_t)sy;
            fixed new_sy = int2fixed(cr->base);
            int64_t dy = (int64_t)new_sy - (int64_t)sy;
            sx += (int)((((int64_t)(ex-sx))*dy + y/2)/y);
            sy = new_sy;
            cursor_null(cr);
            cr->y = sy;
            isy = 0;
        }
        truncated = iey > cr->scanlines;
        if (truncated) {
            /* Move ey down */
            int64_t y = ey - sy;
            fixed new_ey = int2fixed(cr->base + cr->scanlines);
            int64_t dy = (int64_t)ey - (int64_t)new_ey;
            saved_ex = ex;
            saved_ey = ey;
            ex -= (int)((((int64_t)(ex-sx))*dy + y/2)/y);
            ey = new_ey;
            iey = cr->scanlines;
        }
    } else {
        /* Falling line */
        if (isy < 0 || iey >= cr->scanlines) {
            /* All line is outside. */
            if ((ey & 0xff) == 0)
                cursor_null(cr);
            else {
                cr->left = ex;
                cr->right = ex;
            }
            cr->y = ey;
            cr->first = 0;
            return;
        }
        truncated = iey < 0;
        if (truncated) {
            /* Move ey up */
            int64_t y = (int64_t)ey - (int64_t)sy;
            fixed new_ey = int2fixed(cr->base);
            int64_t dy = (int64_t)ey - (int64_t)new_ey;
            ex -= (int)((((int64_t)(ex-sx))*dy + y/2)/y);
            ey = new_ey;
            iey = 0;
        }
        if (isy >= cr->scanlines) {
            /* Move sy down */
            int64_t y = (int64_t)ey - (int64_t)sy;
            fixed new_sy = int2fixed(cr->base + cr->scanlines);
            int64_t dy = (int64_t)new_sy - (int64_t)sy;
            sx += (int)((((int64_t)(ex-sx))*dy + y/2)/y);
            sy = new_sy;
            cursor_null(cr);
            cr->y = sy;
            isy = cr->scanlines;
        }
    }

    cursor_left_merge(cr, sx);
    cursor_right_merge(cr, sx);

    assert(cr->left <= sx);
    assert(cr->right >= sx);
    assert(cr->y == sy);

    /* A note: The code below used to be of the form:
     *   if (isy == iey)   ... deal with horizontal lines
     *   else if (ey > sy) {
     *     fixed y_steps = ey - sy;
     *      ... deal with rising lines ...
     *   } else {
     *     fixed y_steps = ey - sy;
     *     ... deal with falling lines
     *   }
     * but that lead to problems, for instance, an example seen
     * has sx=2aa8e, sy=8aee7, ex=7ffc1686, ey=8003e97a.
     * Thus isy=84f, iey=ff80038a. We can see that ey < sy, but
     * sy - ey < 0!
     * We therefore rejig our code so that the choice between
     * cases is done based on the sign of y_steps rather than
     * the relative size of ey and sy.
     */

    /* First, deal with lines that don't change scanline.
     * This accommodates horizontal lines. */
    if (isy == iey) {
        if (saved_sy == saved_ey) {
            /* Horizontal line. Don't change cr->d, don't flush. */
            if ((ey & 0xff) == 0)
                goto no_merge;
        } else if (saved_sy > saved_ey) {
            /* Falling line, flush if previous was rising */
            int skip = cursor_down(cr, sx);
            if ((ey & 0xff) == 0) {
                /* We are falling to the baseline of a subpixel, so output
                 * for the current pixel, and leave the cursor nulled. */
                if (sx <= ex) {
                    cursor_right_merge(cr, ex);
                } else {
                    cursor_left_merge(cr, ex);
                }
                if (!skip)
                    cursor_output(cr, fixed2int(cr->y) - cr->base);
                cursor_null(cr);
                goto no_merge;
            }
        } else {
            /* Rising line, flush if previous was falling */
            cursor_up(cr, sx);
            if ((ey & 0xff) == 0) {
                cursor_null(cr);
                goto no_merge;
            }
        }
        if (sx <= ex) {
            cursor_right_merge(cr, ex);
        } else {
            cursor_left_merge(cr, ex);
        }
no_merge:
        cr->y = ey;
        if (sy > saved_ey)
            goto end; /* pdfking: was goto endFalling (C++ forbids jumping
                       * past the initialisations below); endFalling is
                       * immediately followed by end. */
    } else if (iey > isy) {
        /* We want to change from sy to ey, which are guaranteed to be on
         * different scanlines. We do this in 3 phases.
         * Phase 1 gets us from sy to the next scanline boundary.
         * Phase 2 gets us all the way to the last scanline boundary.
         * Phase 3 gets us from the last scanline boundary to ey.
         */
        /* We want to change from sy to ey, which are guaranteed to be on
         * different scanlines. We do this in 3 phases.
         * Phase 1 gets us from sy to the next scanline boundary. (We may exit after phase 1).
         * Phase 2 gets us all the way to the last scanline boundary. (This may be a null operation)
         * Phase 3 gets us from the last scanline boundary to ey. (We are guaranteed to have output the cursor at least once before phase 3).
         */
        int phase1_y_steps = (-sy) & (fixed_1 - 1);
        int phase3_y_steps = ey & (fixed_1 - 1);
        ufixed y_steps = (ufixed)ey - (ufixed)sy;

        cursor_up(cr, sx);

        if (sx == ex) {
            /* Vertical line. (Rising) */

            /* Phase 1: */
            if (phase1_y_steps) {
                /* If phase 1 will move us into a new scanline, then we must
                 * flush it before we move. */
                cursor_step(cr, phase1_y_steps, sx, 0);
                sy += phase1_y_steps;
                y_steps -= phase1_y_steps;
                if (y_steps == 0) {
                    cursor_null(cr);
                    goto end;
                }
            }

            /* Phase 3: precalculation */
            y_steps -= phase3_y_steps;

            /* Phase 2: */
            y_steps = fixed2int(y_steps);
            assert(y_steps >= 0);
            if (y_steps > 0) {
                cursor_always_step(cr, fixed_1, sx, 0);
                y_steps--;
                while (y_steps) {
                    cursor_always_step_inrange_vertical(cr, fixed_1, sx);
                    y_steps--;
                }
            }

            /* Phase 3 */
            assert(cr->left == sx && cr->right == sx);
            if (phase3_y_steps == 0)
                cursor_null(cr);
            else
                cr->y += phase3_y_steps;
        } else if (sx < ex) {
            /* Lines increasing in x. (Rightwards, rising) */
            int phase1_x_steps, phase3_x_steps;
            fixed x_steps = ex - sx;

            /* Phase 1: */
            if (phase1_y_steps) {
                phase1_x_steps = (int)(((int64_t)x_steps * phase1_y_steps + y_steps/2) / y_steps);
                sx += phase1_x_steps;
                cursor_right_merge(cr, sx);
                x_steps -= phase1_x_steps;
                cursor_step(cr, phase1_y_steps, sx, 0);
                sy += phase1_y_steps;
                y_steps -= phase1_y_steps;
                if (y_steps == 0) {
                    cursor_null(cr);
                    goto end;
                }
            }

            /* Phase 3: precalculation */
            phase3_x_steps = (int)(((int64_t)x_steps * phase3_y_steps + y_steps/2) / y_steps);
            x_steps -= phase3_x_steps;
            y_steps -= phase3_y_steps;
            assert((y_steps & (fixed_1 - 1)) == 0);

            /* Phase 2: */
            y_steps = fixed2int(y_steps);
            assert(y_steps >= 0);
            if (y_steps) {
                /* We want to change sx by x_steps in y_steps steps.
                 * So each step, we add x_steps/y_steps to sx. That's x_inc + n_inc/y_steps. */
                int x_inc = x_steps/y_steps;
                int n_inc = x_steps - (x_inc * y_steps);
                int f = y_steps/2;
                int d = y_steps;

                /* Special casing the first iteration, allows us to simplify
                 * the following loop. */
                sx += x_inc;
                f -= n_inc;
                if (f < 0)
                    f += d, sx++;
                cursor_right_merge(cr, sx);
                cursor_always_step(cr, fixed_1, sx, 0);
                y_steps--;

                while (y_steps) {
                    sx += x_inc;
                    f -= n_inc;
                    if (f < 0)
                        f += d, sx++;
                    cursor_right(cr, sx);
                    cursor_always_inrange_step_right(cr, fixed_1, sx);
                    y_steps--;
                };
            }

            /* Phase 3 */
            assert(cr->left <= ex && cr->right >= sx);
            if (phase3_y_steps == 0)
                cursor_null(cr);
            else {
                cursor_right(cr, ex);
                cr->y += phase3_y_steps;
            }
        } else {
            /* Lines decreasing in x. (Leftwards, rising) */
            int phase1_x_steps, phase3_x_steps;
            fixed x_steps = sx - ex;

            /* Phase 1: */
            if (phase1_y_steps) {
                phase1_x_steps = (int)(((int64_t)x_steps * phase1_y_steps + y_steps/2) / y_steps);
                x_steps -= phase1_x_steps;
                sx -= phase1_x_steps;
                cursor_left_merge(cr, sx);
                cursor_step(cr, phase1_y_steps, sx, 0);
                sy += phase1_y_steps;
                y_steps -= phase1_y_steps;
                if (y_steps == 0) {
                    cursor_null(cr);
                    goto end;
                }
            }

            /* Phase 3: precalculation */
            phase3_x_steps = (int)(((int64_t)x_steps * phase3_y_steps + y_steps/2) / y_steps);
            x_steps -= phase3_x_steps;
            y_steps -= phase3_y_steps;
            assert((y_steps & (fixed_1 - 1)) == 0);

            /* Phase 2: */
            y_steps = fixed2int(y_steps);
            assert(y_steps >= 0);
            if (y_steps) {
                /* We want to change sx by x_steps in y_steps steps.
                 * So each step, we sub x_steps/y_steps from sx. That's x_inc + n_inc/ey. */
                int x_inc = x_steps/y_steps;
                int n_inc = x_steps - (x_inc * y_steps);
                int f = y_steps/2;
                int d = y_steps;

                /* Special casing the first iteration, allows us to simplify
                 * the following loop. */
                sx -= x_inc;
                f -= n_inc;
                if (f < 0)
                    f += d, sx--;
                cursor_left_merge(cr, sx);
                cursor_always_step(cr, fixed_1, sx, 0);
                y_steps--;

                while (y_steps) {
                    sx -= x_inc;
                    f -= n_inc;
                    if (f < 0)
                        f += d, sx--;
                    cursor_left(cr, sx);
                    cursor_always_inrange_step_left(cr, fixed_1, sx);
                    y_steps--;
                }
            }

            /* Phase 3 */
            assert(cr->right >= ex && cr->left <= sx);
            if (phase3_y_steps == 0)
                cursor_null(cr);
            else {
                cursor_left(cr, ex);
                cr->y += phase3_y_steps;
            }
        }
    } else {
        /* So lines decreasing in y. */
        /* We want to change from sy to ey, which are guaranteed to be on
         * different scanlines. We do this in 3 phases.
         * Phase 1 gets us from sy to the next scanline boundary. This never causes an output.
         * Phase 2 gets us all the way to the last scanline boundary. This is guaranteed to cause an output.
         * Phase 3 gets us from the last scanline boundary to ey. We are guaranteed to have outputted by now.
         */
        int phase1_y_steps = sy & (fixed_1 - 1);
        int phase3_y_steps = (-ey) & (fixed_1 - 1);
        ufixed y_steps = (ufixed)sy - (ufixed)ey;

        int skip = cursor_down(cr, sx);

        if (sx == ex) {
            /* Vertical line. (Falling) */

            /* Phase 1: */
            if (phase1_y_steps) {
                /* Phase 1 in a falling line never moves us into a new scanline. */
                cursor_never_step_vertical(cr, -phase1_y_steps, sx);
                sy -= phase1_y_steps;
                y_steps -= phase1_y_steps;
                if (y_steps == 0)
                    goto endFallingLeftOnEdgeOfPixel;
            }

            /* Phase 3: precalculation */
            y_steps -= phase3_y_steps;
            assert((y_steps & (fixed_1 - 1)) == 0);

            /* Phase 2: */
            y_steps = fixed2int(y_steps);
            assert(y_steps >= 0);
            if (y_steps) {
                cursor_always_step(cr, -fixed_1, sx, skip);
                skip = 0;
                y_steps--;
                while (y_steps) {
                    cursor_always_step_inrange_vertical(cr, -fixed_1, sx);
                    y_steps--;
                }
            }

            /* Phase 3 */
            if (phase3_y_steps == 0) {
endFallingLeftOnEdgeOfPixel:
                cursor_always_step_inrange_vertical(cr, 0, sx);
                cursor_null(cr);
            } else {
                cursor_step(cr, -phase3_y_steps, sx, skip);
                assert(cr->left == sx && cr->right == sx);
            }
        } else if (sx < ex) {
            /* Lines increasing in x. (Rightwards, falling) */
            int phase1_x_steps, phase3_x_steps;
            fixed x_steps = ex - sx;

            /* Phase 1: */
            if (phase1_y_steps) {
                phase1_x_steps = (int)(((int64_t)x_steps * phase1_y_steps + y_steps/2) / y_steps);
                x_steps -= phase1_x_steps;
                sx += phase1_x_steps;
                /* Phase 1 in a falling line never moves us into a new scanline. */
                cursor_never_step_right(cr, -phase1_y_steps, sx);
                sy -= phase1_y_steps;
                y_steps -= phase1_y_steps;
                if (y_steps == 0)
                    goto endFallingRightOnEdgeOfPixel;
            }

            /* Phase 3: precalculation */
            phase3_x_steps = (int)(((int64_t)x_steps * phase3_y_steps + y_steps/2) / y_steps);
            x_steps -= phase3_x_steps;
            y_steps -= phase3_y_steps;
            assert((y_steps & (fixed_1 - 1)) == 0);

            /* Phase 2: */
            y_steps = fixed2int(y_steps);
            assert(y_steps >= 0);
            if (y_steps) {
                /* We want to change sx by x_steps in y_steps steps.
                 * So each step, we add x_steps/y_steps to sx. That's x_inc + n_inc/ey. */
                int x_inc = x_steps/y_steps;
                int n_inc = x_steps - (x_inc * y_steps);
                int f = y_steps/2;
                int d = y_steps;

                cursor_always_step(cr, -fixed_1, sx, skip);
                skip = 0;
                sx += x_inc;
                f -= n_inc;
                if (f < 0)
                    f += d, sx++;
                cursor_right(cr, sx);
                y_steps--;

                while (y_steps) {
                    cursor_always_inrange_step_right(cr, -fixed_1, sx);
                    sx += x_inc;
                    f -= n_inc;
                    if (f < 0)
                        f += d, sx++;
                    cursor_right(cr, sx);
                    y_steps--;
                }
            }

            /* Phase 3 */
            if (phase3_y_steps == 0) {
endFallingRightOnEdgeOfPixel:
                cursor_always_step_inrange_vertical(cr, 0, sx);
                cursor_null(cr);
            } else {
                cursor_step(cr, -phase3_y_steps, sx, skip);
                cursor_right(cr, ex);
                assert(cr->left == sx && cr->right == ex);
            }
        } else {
            /* Lines decreasing in x. (Falling) */
            int phase1_x_steps, phase3_x_steps;
            fixed x_steps = sx - ex;

            /* Phase 1: */
            if (phase1_y_steps) {
                phase1_x_steps = (int)(((int64_t)x_steps * phase1_y_steps + y_steps/2) / y_steps);
                x_steps -= phase1_x_steps;
                sx -= phase1_x_steps;
                /* Phase 1 in a falling line never moves us into a new scanline. */
                cursor_never_step_left(cr, -phase1_y_steps, sx);
                sy -= phase1_y_steps;
                y_steps -= phase1_y_steps;
                if (y_steps == 0)
                    goto endFallingVerticalOnEdgeOfPixel;
            }

            /* Phase 3: precalculation */
            phase3_x_steps = (int)(((int64_t)x_steps * phase3_y_steps + y_steps/2) / y_steps);
            x_steps -= phase3_x_steps;
            y_steps -= phase3_y_steps;
            assert((y_steps & (fixed_1 - 1)) == 0);

            /* Phase 2: */
            y_steps = fixed2int(y_steps);
            assert(y_steps >= 0);
            if (y_steps) {
                /* We want to change sx by x_steps in y_steps steps.
                 * So each step, we sub x_steps/y_steps from sx. That's x_inc + n_inc/ey. */
                int x_inc = x_steps/y_steps;
                int n_inc = x_steps - (x_inc * y_steps);
                int f = y_steps/2;
                int d = y_steps;

                cursor_always_step(cr, -fixed_1, sx, skip);
                skip = 0;
                sx -= x_inc;
                f -= n_inc;
                if (f < 0)
                    f += d, sx--;
                cursor_left(cr, sx);
                y_steps--;

                while (y_steps) {
                    cursor_always_inrange_step_left(cr, -fixed_1, sx);
                    sx -= x_inc;
                    f -= n_inc;
                    if (f < 0)
                        f += d, sx--;
                    cursor_left(cr, sx);
                    y_steps--;
                }
            }

            /* Phase 3 */
            if (phase3_y_steps == 0) {
endFallingVerticalOnEdgeOfPixel:
                cursor_always_step_inrange_vertical(cr, 0, sx);
                cursor_null(cr);
            } else {
                cursor_step(cr, -phase3_y_steps, sx, skip);
                cursor_left(cr, ex);
                assert(cr->left == ex && cr->right == sx);
            }
        }
/* endFalling: */
    }

end:
    if (truncated) {
        cr->left = saved_ex;
        cr->right = saved_ex;
        cr->y = saved_ey;
    }
}

static void mark_curve_app(cursor *cr, fixed sx, fixed sy, fixed c1x, fixed c1y, fixed c2x, fixed c2y, fixed ex, fixed ey, int depth)
{
        int ax = (sx + c1x)>>1;
        int ay = (sy + c1y)>>1;
        int bx = (c1x + c2x)>>1;
        int by = (c1y + c2y)>>1;
        int cx = (c2x + ex)>>1;
        int cy = (c2y + ey)>>1;
        int dx = (ax + bx)>>1;
        int dy = (ay + by)>>1;
        int fx = (bx + cx)>>1;
        int fy = (by + cy)>>1;
        int gx = (dx + fx)>>1;
        int gy = (dy + fy)>>1;

        assert(depth >= 0);
        if (depth == 0)
            mark_line_app(cr, sx, sy, ex, ey);
        else {
            depth--;
            mark_curve_app(cr, sx, sy, ax, ay, dx, dy, gx, gy, depth);
            mark_curve_app(cr, gx, gy, fx, fy, cx, cy, ex, ey, depth);
        }
}

static void mark_curve_big_app(cursor *cr, fixed64 sx, fixed64 sy, fixed64 c1x, fixed64 c1y, fixed64 c2x, fixed64 c2y, fixed64 ex, fixed64 ey, int depth)
{
    fixed64 ax = (sx + c1x)>>1;
    fixed64 ay = (sy + c1y)>>1;
    fixed64 bx = (c1x + c2x)>>1;
    fixed64 by = (c1y + c2y)>>1;
    fixed64 cx = (c2x + ex)>>1;
    fixed64 cy = (c2y + ey)>>1;
    fixed64 dx = (ax + bx)>>1;
    fixed64 dy = (ay + by)>>1;
    fixed64 fx = (bx + cx)>>1;
    fixed64 fy = (by + cy)>>1;
    fixed64 gx = (dx + fx)>>1;
    fixed64 gy = (dy + fy)>>1;

    assert(depth >= 0);
    if (depth == 0)
        mark_line_app(cr, (fixed)sx, (fixed)sy, (fixed)ex, (fixed)ey);
    else {
        depth--;
        mark_curve_big_app(cr, sx, sy, ax, ay, dx, dy, gx, gy, depth);
        mark_curve_big_app(cr, gx, gy, fx, fy, cx, cy, ex, ey, depth);
    }
}

static void mark_curve_top_app(cursor *cr, fixed sx, fixed sy, fixed c1x, fixed c1y, fixed c2x, fixed c2y, fixed ex, fixed ey, int depth)
{
    fixed test = (sx^(sx<<1))|(sy^(sy<<1))|(c1x^(c1x<<1))|(c1y^(c1y<<1))|(c2x^(c2x<<1))|(c2y^(c2y<<1))|(ex^(ex<<1))|(ey^(ey<<1));

    if (test < 0)
        mark_curve_big_app(cr, sx, sy, c1x, c1y, c2x, c2y, ex, ey, depth);
    else
        mark_curve_app(cr, sx, sy, c1x, c1y, c2x, c2y, ex, ey, depth);
}

static int make_table_app(gx_device     * pdev,
                          gx_path       * path,
                          gs_fixed_rect * ibox,
                          int           * scanlines,
                          int          ** index,
                          int          ** table,
                          gx_edgebuffer * edgebuffer)
{
    return make_table_template(pdev, path, ibox, 2, 0, scanlines, index, table, edgebuffer);
}

static void
fill_zero_app(int *row, const fixed *x)
{
    int n = *row = (*row)+2; /* Increment the count */
    row[2*n-3] = (x[0]&~1);
    row[2*n-2] = (x[1]&~1);
    row[2*n-1] = (x[1]&~1)|1;
    row[2*n  ] = x[1];
}

int gx_scan_convert_app(gx_device     * gs_restrict pdev,
                        gx_path       * gs_restrict path,
                  const gs_fixed_rect * gs_restrict clip,
                        gx_edgebuffer * gs_restrict edgebuffer,
                        fixed                    fixed_flat)
{
    gs_fixed_rect  ibox;
    gs_fixed_rect  bbox;
    int            scanlines;
    const subpath *psub;
    int           *index;
    int           *table;
    int            i;
    cursor         cr;
    int            code;
    int            zero;

    edgebuffer->index = NULL;
    edgebuffer->table = NULL;

    /* Bale out if no actual path. We see this with the clist */
    if (path->first_subpath == NULL)
        return 0;

    zero = make_bbox(path, clip, &bbox, &ibox, 0);
    if (zero < 0)
        return zero;

    if (ibox.q.y <= ibox.p.y)
        return 0;

    code = make_table_app(pdev, path, &ibox, &scanlines, &index, &table, edgebuffer);
    if (code != 0) /* > 0 means "retry with smaller height" */
        return code;

    if (scanlines == 0)
        return 0;

    if (zero) {
        code = zero_case(pdev, path, &ibox, index, table, fixed_flat, fill_zero_app);
    } else {

    /* Step 2 continued: Now we run through the path, filling in the real
     * values. */
    cr.scanlines = scanlines;
    cr.index     = index;
    cr.table     = table;
    cr.base      = ibox.p.y;
    for (psub = path->first_subpath; psub != 0;) {
        const segment *pseg = (const segment *)psub;
        fixed ex = pseg->pt.x;
        fixed ey = pseg->pt.y;
        fixed ix = ex;
        fixed iy = ey;
        fixed sx, sy;

        if ((ey & 0xff) == 0) {
            cr.left  = max_fixed;
            cr.right = min_fixed;
        } else {
            cr.left = cr.right = ex;
        }
        cr.y = ey;
        cr.d = DIRN_UNSET;
        cr.first = 1;
        cr.saved = 0;

        while ((pseg = pseg->next) != 0 &&
               pseg->type != s_start
            ) {
            sx = ex;
            sy = ey;
            ex = pseg->pt.x;
            ey = pseg->pt.y;

            switch (pseg->type) {
                default:
                case s_start: /* Should never happen */
                case s_dash:  /* We should never be seeing a dash here */
                    assert("This should never happen" == NULL);
                    break;
                case s_curve: {
                    const curve_segment *const pcur = (const curve_segment *)pseg;
                    int k = gx_curve_log2_samples(sx, sy, pcur, fixed_flat);

                    mark_curve_top_app(&cr, sx, sy, pcur->p1.x, pcur->p1.y, pcur->p2.x, pcur->p2.y, ex, ey, k);
                    break;
                }
                case s_gap:
                case s_line:
                case s_line_close:
                    mark_line_app(&cr, sx, sy, ex, ey);
                    break;
            }
        }
        /* And close any open segments */
        mark_line_app(&cr, ex, ey, ix, iy);
        cursor_flush(&cr, ex);
        psub = (const subpath *)pseg;
    }
    }

    /* Step 2 complete: We now have a complete list of intersection data in
     * table, indexed by index. */

    edgebuffer->base   = ibox.p.y;
    edgebuffer->height = scanlines;
    edgebuffer->xmin   = ibox.p.x;
    edgebuffer->xmax   = ibox.q.x;
    edgebuffer->index  = index;
    edgebuffer->table  = table;

#ifdef DEBUG_SCAN_CONVERTER
    if (debugging_scan_converter) {
        dlprintf("Before sorting:\n");
        gx_edgebuffer_print_app(edgebuffer);
    }
#endif

    /* Step 3: Sort the intersects on x */
    for (i=0; i < scanlines; i++) {
        int *row = &table[index[i]];
        int  rowlen = *row++;

        /* Bubblesort short runs, qsort longer ones. */
        /* FIXME: Verify the figure 6 below */
        if (rowlen <= 6) {
            int j, k;
            for (j = 0; j < rowlen-1; j++) {
                int * gs_restrict t = &row[j<<1];
                for (k = j+1; k < rowlen; k++) {
                    int * gs_restrict s = &row[k<<1];
                    int tmp;
                    if (t[0] < s[0])
                        continue;
                    if (t[0] > s[0])
                        goto swap01;
                    if (t[1] <= s[1])
                        continue;
                    if (0) {
swap01:
                        tmp = t[0], t[0] = s[0], s[0] = tmp;
                    }
                    tmp = t[1], t[1] = s[1], s[1] = tmp;
                }
            }
        } else
            qsort(row, rowlen, 2*sizeof(int), edgecmp);
    }

    return 0;
}

/* Step 5: Filter the intersections according to the rules */
int
gx_filter_edgebuffer_app(gx_device       * gs_restrict pdev,
                         gx_edgebuffer   * gs_restrict edgebuffer,
                         int                        rule)
{
    int i;

#ifdef DEBUG_SCAN_CONVERTER
    if (debugging_scan_converter) {
        dlprintf("Before filtering:\n");
        gx_edgebuffer_print_app(edgebuffer);
    }
#endif

    for (i=0; i < edgebuffer->height; i++) {
        int *row      = &edgebuffer->table[edgebuffer->index[i]];
        int  rowlen   = *row++;
        int *rowstart = row;
        int *rowout   = row;
        int  ll, lr, rl, rr, wind, marked_to;

        /* Avoid double setting pixels, by keeping where we have marked to. */
        marked_to = INT_MIN;
        while (rowlen > 0) {
            if (rule == gx_rule_even_odd) {
                /* Even Odd */
                ll = (*row++)&~1;
                lr = *row;
                row += 2;
                rowlen-=2;

                /* We will fill solidly from ll to at least lr, possibly further */
                assert(rowlen >= 0);
                rr = (*row++);
                if (rr > lr)
                    lr = rr;
            } else {
                /* Non-Zero */
                int w;

                ll = *row++;
                lr = *row++;
                wind = -(ll&1) | 1;
                ll &= ~1;
                rowlen--;

                assert(rowlen > 0);
                do {
                    rl = *row++;
                    rr = *row++;
                    w = -(rl&1) | 1;
                    rl &= ~1;
                    rowlen--;
                    if (rr > lr)
                        lr = rr;
                    wind += w;
                    if (wind == 0)
                        break;
                } while (rowlen > 0);
            }

            if (marked_to >= lr)
                continue;

            if (marked_to >= ll) {
                if (rowout == rowstart)
                    ll = marked_to;
                else {
                    rowout -= 2;
                    ll = *rowout;
                }
            }

            if (lr >= ll) {
                *rowout++ = ll;
                *rowout++ = lr;
                marked_to = lr;
            }
        }
        rowstart[-1] = rowout - rowstart;
    }
    return 0;
}

/* Step 6: Fill */
int
gx_fill_edgebuffer_app(gx_device       * gs_restrict pdev,
                 const gx_device_color * gs_restrict pdevc,
                       gx_edgebuffer   * gs_restrict edgebuffer,
                       int                        log_op)
{
    int i, code;

    for (i=0; i < edgebuffer->height; i++) {
        int *row    = &edgebuffer->table[edgebuffer->index[i]];
        int  rowlen = *row++;
        int  left, right;

        while (rowlen > 0) {
            left  = *row++;
            right = *row++;
            left  = fixed2int(left);
            right = fixed2int(right + fixed_1 - 1);
            rowlen -= 2;

            right -= left;
            if (right > 0) {
                if (log_op < 0)
                    code = dev_proc(pdev, fill_rectangle)(pdev, left, edgebuffer->base+i, right, 1, pdevc->colors.pure);
                else
                    code = gx_fill_rectangle_device_rop(left, edgebuffer->base+i, right, 1, pdevc, pdev, (gs_logical_operation_t)log_op);
                if (code < 0)
                    return code;
            }
        }
    }
    return 0;
}

/* Centre of a pixel trapezoid routines */

static int intcmp_tr(const void *a, const void *b)
{
    int left  = ((int*)a)[0];
    int right = ((int*)b)[0];
    if (left != right)
        return left - right;
    return ((int*)a)[1] - ((int*)b)[1];
}

#ifdef DEBUG_SCAN_CONVERTER
static void
gx_edgebuffer_print_tr(gx_edgebuffer * edgebuffer)
{
    int i;

    if (!debugging_scan_converter)
        return;

    dlprintf1("Edgebuffer %x\n", edgebuffer);
    dlprintf4("xmin=%x xmax=%x base=%x height=%x\n",
              edgebuffer->xmin, edgebuffer->xmax, edgebuffer->base, edgebuffer->height);
    for (i=0; i < edgebuffer->height; i++)
    {
        int  offset = edgebuffer->index[i];
        int *row    = &edgebuffer->table[offset];
        int count   = *row++;
        dlprintf3("%d @ %d: %d =", i, offset, count);
        while (count-- > 0) {
            int e  = *row++;
            int id = *row++;
            dlprintf3(" %x%c%d", e, id&1 ? 'v' : '^', id>>1);
        }
        dlprintf("\n");
    }
}
#endif

static void mark_line_tr(fixed sx, fixed sy, fixed ex, fixed ey, int base_y, int height, int *table, int *index, int id)
{
    int64_t delta;
    int iy, ih;
    fixed clip_sy, clip_ey;
    int dirn = DIRN_UP;
    int *row;

#ifdef DEBUG_SCAN_CONVERTER
    if (debugging_scan_converter)
        dlprintf6("Marking line (tr) from %x,%x to %x,%x (%x,%x)\n", sx, sy, ex, ey, fixed2int(sy + fixed_half-1) - base_y, fixed2int(ey + fixed_half-1) - base_y);
#endif
#ifdef DEBUG_OUTPUT_SC_AS_PS
    dlprintf("0.001 setlinewidth 0 0 0 setrgbcolor %%PS\n");
    coord("moveto", sx, sy);
    coord("lineto", ex, ey);
    dlprintf("stroke %%PS\n");
#endif

    if (fixed2int(sy + fixed_half-1) == fixed2int(ey + fixed_half-1))
        return;
    if (sy > ey) {
        int t;
        t = sy; sy = ey; ey = t;
        t = sx; sx = ex; ex = t;
        dirn = DIRN_DOWN;
    }
    /* Lines go from sy to ey, closed at the start, open at the end. */
    /* We clip them to a region to make them closed at both ends. */
    /* Thus the first scanline marked (>= sy) is: */
    clip_sy = ((sy + fixed_half - 1) & ~(fixed_1-1)) | fixed_half;
    /* The last scanline marked (< ey) is: */
    clip_ey = ((ey - fixed_half - 1) & ~(fixed_1-1)) | fixed_half;
    /* Now allow for banding */
    if (clip_sy < int2fixed(base_y) + fixed_half)
        clip_sy = int2fixed(base_y) + fixed_half;
    if (ey <= clip_sy)
        return;
    if (clip_ey > int2fixed(base_y + height - 1) + fixed_half)
        clip_ey = int2fixed(base_y + height - 1) + fixed_half;
    if (sy > clip_ey)
        return;
    delta = (int64_t)clip_sy - (int64_t)sy;
    if (delta > 0)
    {
        int64_t dx = (int64_t)ex - (int64_t)sx;
        int64_t dy = (int64_t)ey - (int64_t)sy;
        int advance = (int)((dx * delta + (dy>>1)) / dy);
        sx += advance;
        sy += delta;
    }
    delta = (int64_t)ey - (int64_t)clip_ey;
    if (delta > 0)
    {
        int64_t dx = (int64_t)ex - (int64_t)sx;
        int64_t dy = (int64_t)ey - (int64_t)sy;
        int advance = (int)((dx * delta + (dy>>1)) / dy);
        ex -= advance;
        ey -= delta;
    }
    ex -= sx;
    ey -= sy;
    ih = fixed2int(ey);
    assert(ih >= 0);
    iy = fixed2int(sy) - base_y;
#ifdef DEBUG_SCAN_CONVERTER
    if (debugging_scan_converter)
        dlprintf2("    iy=%x ih=%x\n", iy, ih);
#endif
    assert(iy >= 0 && iy < height);
    id = (id<<1) | dirn;
    /* We always cross at least one scanline */
    row = &table[index[iy]];
    *row = (*row)+1; /* Increment the count */
    row[*row * 2 - 1] = sx;
    row[*row * 2    ] = id;
    if (ih == 0)
        return;
    if (ex >= 0) {
        int x_inc, n_inc, f;

        /* We want to change sx by ex in ih steps. So each step, we add
         * ex/ih to sx. That's x_inc + n_inc/ih.
         */
        x_inc = ex/ih;
        n_inc = ex-(x_inc*ih);
        f     = ih>>1;
        delta = ih;
        do {
            int count;
            iy++;
            sx += x_inc;
            f  -= n_inc;
            if (f < 0) {
                f += ih;
                sx++;
            }
            assert(iy >= 0 && iy < height);
            row = &table[index[iy]];
            count = *row = (*row)+1; /* Increment the count */
            row[count * 2 - 1] = sx;
            row[count * 2    ] = id;
        }
        while (--delta);
    } else {
        int x_dec, n_dec, f;

        ex = -ex;
        /* We want to change sx by ex in ih steps. So each step, we subtract
         * ex/ih from sx. That's x_dec + n_dec/ih.
         */
        x_dec = ex/ih;
        n_dec = ex-(x_dec*ih);
        f     = ih>>1;
        delta = ih;
        do {
            int count;
            iy++;
            sx -= x_dec;
            f  -= n_dec;
            if (f < 0) {
                f += ih;
                sx--;
            }
            assert(iy >= 0 && iy < height);
            row = &table[index[iy]];
            count = *row = (*row)+1; /* Increment the count */
            row[count * 2 - 1] = sx;
            row[count * 2    ] = id;
         }
         while (--delta);
    }
}

static void mark_curve_tr(fixed sx, fixed sy, fixed c1x, fixed c1y, fixed c2x, fixed c2y, fixed ex, fixed ey, fixed base_y, fixed height, int *table, int *index, int *id, int depth)
{
    fixed ax = (sx + c1x)>>1;
    fixed ay = (sy + c1y)>>1;
    fixed bx = (c1x + c2x)>>1;
    fixed by = (c1y + c2y)>>1;
    fixed cx = (c2x + ex)>>1;
    fixed cy = (c2y + ey)>>1;
    fixed dx = (ax + bx)>>1;
    fixed dy = (ay + by)>>1;
    fixed fx = (bx + cx)>>1;
    fixed fy = (by + cy)>>1;
    fixed gx = (dx + fx)>>1;
    fixed gy = (dy + fy)>>1;

    assert(depth >= 0);
    if (depth == 0) {
        *id += 1;
        mark_line_tr(sx, sy, ex, ey, base_y, height, table, index, *id);
    } else {
        depth--;
        mark_curve_tr(sx, sy, ax, ay, dx, dy, gx, gy, base_y, height, table, index, id, depth);
        mark_curve_tr(gx, gy, fx, fy, cx, cy, ex, ey, base_y, height, table, index, id, depth);
    }
}

static void mark_curve_big_tr(fixed64 sx, fixed64 sy, fixed64 c1x, fixed64 c1y, fixed64 c2x, fixed64 c2y, fixed64 ex, fixed64 ey, fixed base_y, fixed height, int *table, int *index, int *id, int depth)
{
    fixed64 ax = (sx + c1x)>>1;
    fixed64 ay = (sy + c1y)>>1;
    fixed64 bx = (c1x + c2x)>>1;
    fixed64 by = (c1y + c2y)>>1;
    fixed64 cx = (c2x + ex)>>1;
    fixed64 cy = (c2y + ey)>>1;
    fixed64 dx = (ax + bx)>>1;
    fixed64 dy = (ay + by)>>1;
    fixed64 fx = (bx + cx)>>1;
    fixed64 fy = (by + cy)>>1;
    fixed64 gx = (dx + fx)>>1;
    fixed64 gy = (dy + fy)>>1;

    assert(depth >= 0);
    if (depth == 0) {
        *id += 1;
        mark_line_tr((fixed)sx, (fixed)sy, (fixed)ex, (fixed)ey, base_y, height, table, index, *id);
    } else {
        depth--;
        mark_curve_big_tr(sx, sy, ax, ay, dx, dy, gx, gy, base_y, height, table, index, id, depth);
        mark_curve_big_tr(gx, gy, fx, fy, cx, cy, ex, ey, base_y, height, table, index, id, depth);
    }
}

static void mark_curve_top_tr(fixed sx, fixed sy, fixed c1x, fixed c1y, fixed c2x, fixed c2y, fixed ex, fixed ey, fixed base_y, fixed height, int *table, int *index, int *id, int depth)
{
    fixed test = (sx^(sx<<1))|(sy^(sy<<1))|(c1x^(c1x<<1))|(c1y^(c1y<<1))|(c2x^(c2x<<1))|(c2y^(c2y<<1))|(ex^(ex<<1))|(ey^(ey<<1));

    if (test < 0)
        mark_curve_big_tr(sx, sy, c1x, c1y, c2x, c2y, ex, ey, base_y, height, table, index, id, depth);
    else
        mark_curve_tr(sx, sy, c1x, c1y, c2x, c2y, ex, ey, base_y, height, table, index, id, depth);
}

static int make_table_tr(gx_device     * pdev,
                         gx_path       * path,
                         gs_fixed_rect * ibox,
                         int           * scanlines,
                         int          ** index,
                         int          ** table,
                         gx_edgebuffer * edgebuffer)
{
    return make_table_template(pdev, path, ibox, 2, 1, scanlines, index, table, edgebuffer);
}

static void
fill_zero_tr(int *row, const fixed *x)
{
    int n = *row = (*row)+2; /* Increment the count */
    row[2*n-3] = x[0];
    row[2*n-2] = 0;
    row[2*n-1] = x[1];
    row[2*n  ] = 1;
}

int gx_scan_convert_tr(gx_device     * gs_restrict pdev,
                       gx_path       * gs_restrict path,
                 const gs_fixed_rect * gs_restrict clip,
                       gx_edgebuffer * gs_restrict edgebuffer,
                       fixed                    fixed_flat)
{
    gs_fixed_rect  ibox;
    gs_fixed_rect  bbox;
    int            scanlines;
    const subpath *psub;
    int           *index;
    int           *table;
    int            i;
    int            code;
    int            id = 0;
    int            zero;

    edgebuffer->index = NULL;
    edgebuffer->table = NULL;

    /* Bale out if no actual path. We see this with the clist */
    if (path->first_subpath == NULL)
        return 0;

    zero = make_bbox(path, clip, &bbox, &ibox, fixed_half);
    if (zero < 0)
        return zero;

    if (ibox.q.y <= ibox.p.y)
        return 0;

    code = make_table_tr(pdev, path, &ibox, &scanlines, &index, &table, edgebuffer);
    if (code != 0) /* > 0 means "retry with smaller height" */
        return code;

    if (scanlines == 0)
        return 0;

    if (zero) {
        code = zero_case(pdev, path, &ibox, index, table, fixed_flat, fill_zero_tr);
    } else {

    /* Step 3: Now we run through the path, filling in the real
     * values. */
    for (psub = path->first_subpath; psub != 0;) {
        const segment *pseg = (const segment *)psub;
        fixed ex = pseg->pt.x;
        fixed ey = pseg->pt.y;
        fixed ix = ex;
        fixed iy = ey;

        while ((pseg = pseg->next) != 0 &&
               pseg->type != s_start
            ) {
            fixed sx = ex;
            fixed sy = ey;
            ex = pseg->pt.x;
            ey = pseg->pt.y;

            switch (pseg->type) {
                default:
                case s_start: /* Should never happen */
                case s_dash:  /* We should never be seeing a dash here */
                    assert("This should never happen" == NULL);
                    break;
                case s_curve: {
                    const curve_segment *const pcur = (const curve_segment *)pseg;
                    int k = gx_curve_log2_samples(sx, sy, pcur, fixed_flat);

                    mark_curve_top_tr(sx, sy, pcur->p1.x, pcur->p1.y, pcur->p2.x, pcur->p2.y, ex, ey, ibox.p.y, scanlines, table, index, &id, k);
                    break;
                }
                case s_gap:
                case s_line:
                case s_line_close:
                    if (sy != ey)
                        mark_line_tr(sx, sy, ex, ey, ibox.p.y, scanlines, table, index, ++id);
                    break;
            }
        }
        /* And close any open segments */
        if (iy != ey)
            mark_line_tr(ex, ey, ix, iy, ibox.p.y, scanlines, table, index, ++id);
        psub = (const subpath *)pseg;
    }
    }

    /*if (zero) {
        if (table[0] == 0) { */
            /* Zero height rectangle fills a span */
/*          table[0] = 2;
            table[1] = int2fixed(fixed2int(bbox.p.x + fixed_half));
            table[2] = 0;
            table[3] = int2fixed(fixed2int(bbox.q.x + fixed_half));
            table[4] = 1;
        }
    }*/

    /* Step 2 complete: We now have a complete list of intersection data in
     * table, indexed by index. */

    edgebuffer->base   = ibox.p.y;
    edgebuffer->height = scanlines;
    edgebuffer->xmin   = ibox.p.x;
    edgebuffer->xmax   = ibox.q.x;
    edgebuffer->index  = index;
    edgebuffer->table  = table;

#ifdef DEBUG_SCAN_CONVERTER
    if (debugging_scan_converter) {
        dlprintf("Before sorting:\n");
        gx_edgebuffer_print_tr(edgebuffer);
    }
#endif

    /* Step 4: Sort the intersects on x */
    for (i=0; i < scanlines; i++) {
        int *row = &table[index[i]];
        int  rowlen = *row++;

        /* Bubblesort short runs, qsort longer ones. */
        /* FIXME: Verify the figure 6 below */
        if (rowlen <= 6) {
            int j, k;
            for (j = 0; j < rowlen-1; j++) {
                int * gs_restrict t = &row[j<<1];
                for (k = j+1; k < rowlen; k++) {
                    int * gs_restrict s = &row[k<<1];
                    int tmp;
                    if (t[0] < s[0])
                        continue;
                    if (t[0] == s[0]) {
                        if (t[1] <= s[1])
                            continue;
                    } else
                        tmp = t[0], t[0] = s[0], s[0] = tmp;
                    tmp = t[1], t[1] = s[1], s[1] = tmp;
                }
            }
        } else
            qsort(row, rowlen, 2*sizeof(int), intcmp_tr);
    }

    return 0;
}

/* Step 5: Filter the intersections according to the rules */
int
gx_filter_edgebuffer_tr(gx_device       * gs_restrict pdev,
                        gx_edgebuffer   * gs_restrict edgebuffer,
                        int                           rule)
{
    int i;

#ifdef DEBUG_SCAN_CONVERTER
    if (debugging_scan_converter) {
        dlprintf("Before filtering\n");
        gx_edgebuffer_print_tr(edgebuffer);
    }
#endif

    for (i=0; i < edgebuffer->height; i++) {
        int *row      = &edgebuffer->table[edgebuffer->index[i]];
        int  rowlen   = *row++;
        int *rowstart = row;
        int *rowout   = row;

        while (rowlen > 0) {
            int left, lid, right, rid;

            if (rule == gx_rule_even_odd) {
                /* Even Odd */
                left  = *row++;
                lid   = *row++;
                right = *row++;
                rid   = *row++;
                rowlen -= 2;
            } else {
                /* Non-Zero */
                int w;

                left = *row++;
                lid  = *row++;
                w = ((lid&1)-1) | 1;
                rowlen--;
                do {
                    right = *row++;
                    rid   = *row++;
                    rowlen--;
                    w += ((rid&1)-1) | 1;
                } while (w != 0);
            }

            if (right > left) {
                *rowout++ = left;
                *rowout++ = lid;
                *rowout++ = right;
                *rowout++ = rid;
            }
        }
        rowstart[-1] = (rowout-rowstart)>>1;
    }
    return 0;
}

/* Step 6: Fill the edgebuffer */
int
gx_fill_edgebuffer_tr(gx_device       * gs_restrict pdev,
                const gx_device_color * gs_restrict pdevc,
                      gx_edgebuffer   * gs_restrict edgebuffer,
                      int                        log_op)
{
    int i, j, code;
    int mfb = pdev->max_fill_band;

#ifdef DEBUG_SCAN_CONVERTER
    if (debugging_scan_converter) {
        dlprintf("Before filling\n");
        gx_edgebuffer_print_tr(edgebuffer);
    }
#endif

    for (i=0; i < edgebuffer->height; ) {
        int *row    = &edgebuffer->table[edgebuffer->index[i]];
        int  rowlen = *row++;
        int *row2;
        int *rowptr;
        int *row2ptr;
        int y_band_max;

        if (mfb) {
            y_band_max = (i & ~(mfb-1)) + mfb;
            if (y_band_max > edgebuffer->height)
                y_band_max = edgebuffer->height;
        } else {
            y_band_max = edgebuffer->height;
        }

        /* See how many scanlines match i */
        for (j = i+1; j < y_band_max; j++) {
            int row2len;

            row2    = &edgebuffer->table[edgebuffer->index[j]];
            row2len = *row2++;
            row2ptr = row2;
            rowptr  = row;

            if (rowlen != row2len)
                break;
            while (row2len > 0) {
                if ((rowptr[1]&~1) != (row2ptr[1]&~1))
                    goto rowdifferent;
                rowptr  += 2;
                row2ptr += 2;
                row2len--;
            }
        }
rowdifferent:{}

        /* So j is the first scanline that doesn't match i */

        if (j == i+1) {
            while (rowlen > 0) {
                int left, right;

                left  = row[0];
                right = row[2];
                row += 4;
                rowlen -= 2;

                left  = fixed2int(left + fixed_half);
                right = fixed2int(right + fixed_half);
                right -= left;
                if (right > 0) {
#ifdef DEBUG_OUTPUT_SC_AS_PS
                    dlprintf("0.001 setlinewidth 1 0 1 setrgbcolor %% purple %%PS\n");
                    coord("moveto", int2fixed(left), int2fixed(edgebuffer->base+i));
                    coord("lineto", int2fixed(left+right), int2fixed(edgebuffer->base+i));
                    coord("lineto", int2fixed(left+right), int2fixed(edgebuffer->base+i+1));
                    coord("lineto", int2fixed(left), int2fixed(edgebuffer->base+i+1));
                    dlprintf("closepath stroke %%PS\n");
#endif
                    if (log_op < 0)
                        code = dev_proc(pdev, fill_rectangle)(pdev, left, edgebuffer->base+i, right, 1, pdevc->colors.pure);
                    else
                        code = gx_fill_rectangle_device_rop(left, edgebuffer->base+i, right, 1, pdevc, pdev, (gs_logical_operation_t)log_op);
                    if (code < 0)
                        return code;
                }
            }
        } else {
            gs_fixed_edge le;
            gs_fixed_edge re;

#ifdef DEBUG_OUTPUT_SC_AS_PS
#ifdef DEBUG_OUTPUT_SC_AS_PS_TRAPS_AS_RECTS
            int k;
            for (k = i; k < j; k++)
            {
                int row2len;
                int left, right;
                row2    = &edgebuffer->table[edgebuffer->index[k]];
                row2len = *row2++;
                while (row2len > 0) {
                    left = row2[0];
                    right = row2[2];
                    row2 += 4;
                    row2len -= 2;

                    left  = fixed2int(left + fixed_half);
                    right = fixed2int(right + fixed_half);
                    right -= left;
                    if (right > 0) {
                        dlprintf("0.001 setlinewidth 1 0 0.5 setrgbcolor %%PS\n");
                        coord("moveto", int2fixed(left), int2fixed(edgebuffer->base+k));
                        coord("lineto", int2fixed(left+right), int2fixed(edgebuffer->base+k));
                        coord("lineto", int2fixed(left+right), int2fixed(edgebuffer->base+k+1));
                        coord("lineto", int2fixed(left), int2fixed(edgebuffer->base+k+1));
                        dlprintf("closepath stroke %%PS\n");
                    }
                }
            }
#endif
#endif

            le.start.y = re.start.y = int2fixed(edgebuffer->base+i) + fixed_half;
            le.end.y   = re.end.y   = int2fixed(edgebuffer->base+j) - (fixed_half-1);
            row2    = &edgebuffer->table[edgebuffer->index[j-1]+1];
            while (rowlen > 0) {
                le.start.x = row[0];
                re.start.x = row[2];
                le.end.x   = row2[0];
                re.end.x   = row2[2];
                row += 4;
                row2 += 4;
                rowlen -= 2;

                assert(re.start.x >= le.start.x);
                assert(re.end.x >= le.end.x);

#ifdef DEBUG_OUTPUT_SC_AS_PS
                dlprintf("0.001 setlinewidth 0 1 1 setrgbcolor %%cyan %%PS\n");
                coord("moveto", le.start.x, le.start.y);
                coord("lineto", le.end.x, le.end.y);
                coord("lineto", re.end.x, re.end.y);
                coord("lineto", re.start.x, re.start.y);
                dlprintf("closepath stroke %%PS\n");
#endif
                code = dev_proc(pdev, fill_trapezoid)(
                                pdev,
                                &le,
                                &re,
                                le.start.y,
                                le.end.y,
                                0, /* bool swap_axes */
                                pdevc, /*const gx_drawing_color *pdcolor */
                                log_op);
                if (code < 0)
                    return code;
            }
        }

        i = j;
    }
    return 0;
}

/* Any part of a pixel trapezoid routines */

static int edgecmp_tr(const void *a, const void *b)
{
    int left  = ((int*)a)[0];
    int right = ((int*)b)[0];
    if (left != right)
        return left - right;
    left = ((int*)a)[2] - ((int*)b)[2];
    if (left != 0)
        return left;
    left = ((int*)a)[1] - ((int*)b)[1];
    if (left != 0)
        return left;
    return ((int*)a)[3] - ((int*)b)[3];
}

#ifdef DEBUG_SCAN_CONVERTER
static void
gx_edgebuffer_print_filtered_tr_app(gx_edgebuffer * edgebuffer)
{
    int i;

    if (!debugging_scan_converter)
        return;

    dlprintf1("Edgebuffer %x\n", edgebuffer);
    dlprintf4("xmin=%x xmax=%x base=%x height=%x\n",
              edgebuffer->xmin, edgebuffer->xmax, edgebuffer->base, edgebuffer->height);
    for (i=0; i < edgebuffer->height; i++)
    {
        int  offset = edgebuffer->index[i];
        int *row    = &edgebuffer->table[offset];
        int count   = *row++;
        dlprintf3("%x @ %d: %d =", i, offset, count);
        while (count-- > 0) {
            int left  = *row++;
            int lid   = *row++;
            int right = *row++;
            int rid   = *row++;
            dlprintf4(" (%x:%d,%x:%d)", left, lid, right, rid);
        }
        dlprintf("\n");
    }
}

static void
gx_edgebuffer_print_tr_app(gx_edgebuffer * edgebuffer)
{
    int i;
    int borked = 0;

    if (!debugging_scan_converter)
        return;

    dlprintf1("Edgebuffer %x\n", edgebuffer);
    dlprintf4("xmin=%x xmax=%x base=%x height=%x\n",
              edgebuffer->xmin, edgebuffer->xmax, edgebuffer->base, edgebuffer->height);
    for (i=0; i < edgebuffer->height; i++)
    {
        int  offset = edgebuffer->index[i];
        int *row    = &edgebuffer->table[offset];
        int count   = *row++;
        int c       = count;
        int wind    = 0;
        dlprintf3("%x @ %d: %d =", i, offset, count);
        while (count-- > 0) {
            int left  = *row++;
            int lid   = *row++;
            int right = *row++;
            int rid   = *row++;
            int ww    = lid & 1;
            int w     = -ww | 1;
            lid >>= 1;
            wind += w;
            dlprintf5(" (%x:%d,%x:%d)%c", left, lid, right, rid, ww ? 'v' : '^');
        }
        if (wind != 0 || c & 1) {
            dlprintf(" <- BROKEN");
            borked = 1;
        }
        dlprintf("\n");
    }
    if (borked) {
        borked = borked; /* Breakpoint here */
    }
}
#endif

typedef struct
{
    fixed  left;
    int    lid;
    fixed  right;
    int    rid;
    fixed  y;
    signed char  d; /* 0 up (or horiz), 1 down, -1 uninited */
    unsigned char first;
    unsigned char saved;
    fixed  save_left;
    int    save_lid;
    fixed  save_right;
    int    save_rid;
    int    save_iy;
    int    save_d;

    int    scanlines;
    int   *table;
    int   *index;
    int    base;
} cursor_tr;

static inline void
cursor_output_tr(cursor_tr * gs_restrict cr, int iy)
{
    int *row;
    int count;

    if (iy >= 0 && iy < cr->scanlines) {
        if (cr->first) {
            /* Save it for later in case we join up */
            cr->save_left  = cr->left;
            cr->save_lid   = cr->lid;
            cr->save_right = cr->right;
            cr->save_rid   = cr->rid;
            cr->save_iy    = iy;
            cr->save_d     = cr->d;
            cr->saved      = 1;
        } else if (cr->d != DIRN_UNSET) {
            /* Enter it into the table */
            row = &cr->table[cr->index[iy]];
            *row = count = (*row)+1; /* Increment the count */
            row[4 * count - 3] = cr->left;
            row[4 * count - 2] = cr->d | (cr->lid<<1);
            row[4 * count - 1] = cr->right;
            row[4 * count    ] = cr->rid;
        } else {
            assert(cr->left == max_fixed && cr->right == min_fixed);
        }
    }
    cr->first = 0;
}

static inline void
cursor_output_inrange_tr(cursor_tr * gs_restrict cr, int iy)
{
    int *row;
    int count;

    assert(iy >= 0 && iy < cr->scanlines);
    if (cr->first) {
        /* Save it for later in case we join up */
        cr->save_left  = cr->left;
        cr->save_lid   = cr->lid;
        cr->save_right = cr->right;
        cr->save_rid   = cr->rid;
        cr->save_iy    = iy;
        cr->save_d     = cr->d;
        cr->saved      = 1;
    } else {
        /* Enter it into the table */
        assert(cr->d != DIRN_UNSET);

        row = &cr->table[cr->index[iy]];
        *row = count = (*row)+1; /* Increment the count */
        row[4 * count - 3] = cr->left;
        row[4 * count - 2] = cr->d | (cr->lid<<1);
        row[4 * count - 1] = cr->right;
        row[4 * count    ] = cr->rid;
    }
    cr->first = 0;
}

/* Step the cursor in y, allowing for maybe crossing a scanline */
static inline void
cursor_step_tr(cursor_tr * gs_restrict cr, fixed dy, fixed x, int id, int skip)
{
    int new_iy;
    int iy = fixed2int(cr->y) - cr->base;

    cr->y += dy;
    new_iy = fixed2int(cr->y) - cr->base;
    if (new_iy != iy) {
        if (!skip)
            cursor_output_tr(cr, iy);
        cr->left = x;
        cr->lid = id;
        cr->right = x;
        cr->rid = id;
    } else {
        if (x < cr->left) {
            cr->left = x;
            cr->lid = id;
        }
        if (x > cr->right) {
            cr->right = x;
            cr->rid = id;
        }
    }
}

/* Step the cursor in y, never by enough to cross a scanline. */
static inline void
cursor_never_step_vertical_tr(cursor_tr * gs_restrict cr, fixed dy, fixed x, int id)
{
    assert(fixed2int(cr->y+dy) == fixed2int(cr->y));

    cr->y += dy;
}

/* Step the cursor in y, never by enough to cross a scanline,
 * knowing that we are moving left, and that the right edge
 * has already been accounted for. */
static inline void
cursor_never_step_left_tr(cursor_tr * gs_restrict cr, fixed dy, fixed x, int id)
{
    assert(fixed2int(cr->y+dy) == fixed2int(cr->y));

    if (x < cr->left)
    {
        cr->left = x;
        cr->lid = id;
    }
    cr->y += dy;
}

/* Step the cursor in y, never by enough to cross a scanline,
 * knowing that we are moving right, and that the left edge
 * has already been accounted for. */
static inline void
cursor_never_step_right_tr(cursor_tr * gs_restrict cr, fixed dy, fixed x, int id)
{
    assert(fixed2int(cr->y+dy) == fixed2int(cr->y));

    if (x > cr->right)
    {
        cr->right = x;
        cr->rid = id;
    }
    cr->y += dy;
}

/* Step the cursor in y, always by enough to cross a scanline. */
static inline void
cursor_always_step_tr(cursor_tr * gs_restrict cr, fixed dy, fixed x, int id, int skip)
{
    int iy = fixed2int(cr->y) - cr->base;

    if (!skip)
        cursor_output_tr(cr, iy);
    cr->y += dy;
    cr->left = x;
    cr->lid = id;
    cr->right = x;
    cr->rid = id;
}

/* Step the cursor in y, always by enough to cross a scanline, as
 * part of a vertical line, knowing that we are moving from a
 * position guaranteed to be in the valid y range. */
static inline void
cursor_always_step_inrange_vertical_tr(cursor_tr * gs_restrict cr, fixed dy, fixed x, int id)
{
    int iy = fixed2int(cr->y) - cr->base;

    cursor_output_tr(cr, iy);
    cr->y += dy;
}

/* Step the cursor in y, always by enough to cross a scanline, as
 * part of a left moving line, knowing that we are moving from a
 * position guaranteed to be in the valid y range. */
static inline void
cursor_always_inrange_step_left_tr(cursor_tr * gs_restrict cr, fixed dy, fixed x, int id)
{
    int iy = fixed2int(cr->y) - cr->base;

    cr->y += dy;
    cursor_output_inrange_tr(cr, iy);
    cr->right = x;
    cr->rid = id;
}

/* Step the cursor in y, always by enough to cross a scanline, as
 * part of a right moving line, knowing that we are moving from a
 * position guaranteed to be in the valid y range. */
static inline void
cursor_always_inrange_step_right_tr(cursor_tr * gs_restrict cr, fixed dy, fixed x, int id)
{
    int iy = fixed2int(cr->y) - cr->base;

    cr->y += dy;
    cursor_output_inrange_tr(cr, iy);
    cr->left = x;
    cr->lid = id;
}

static inline void cursor_left_merge_tr(cursor_tr * gs_restrict cr, fixed x, int id)
{
    if (x < cr->left) {
        cr->left = x;
        cr->lid  = id;
    }
}

static inline void cursor_left_tr(cursor_tr * gs_restrict cr, fixed x, int id)
{
    cr->left = x;
    cr->lid  = id;
}

static inline void cursor_right_merge_tr(cursor_tr * gs_restrict cr, fixed x, int id)
{
    if (x > cr->right) {
        cr->right = x;
        cr->rid   = id;
    }
}

static inline void cursor_right_tr(cursor_tr * gs_restrict cr, fixed x, int id)
{
    cr->right = x;
    cr->rid   = id;
}

static inline int cursor_down_tr(cursor_tr * gs_restrict cr, fixed x, int id)
{
    int skip = 0;
    if ((cr->y & 0xff) == 0)
        skip = 1;
    if (cr->d == DIRN_UP)
    {
        if (!skip)
            cursor_output_tr(cr, fixed2int(cr->y) - cr->base);
        cr->left = x;
        cr->lid = id;
        cr->right = x;
        cr->rid = id;
    }
    cr->d = DIRN_DOWN;
    return skip;
}

static inline void cursor_up_tr(cursor_tr * gs_restrict cr, fixed x, int id)
{
    if (cr->d == DIRN_DOWN)
    {
        cursor_output_tr(cr, fixed2int(cr->y) - cr->base);
        cr->left = x;
        cr->lid = id;
        cr->right = x;
        cr->rid = id;
    }
    cr->d = DIRN_UP;
}

static inline void
cursor_flush_tr(cursor_tr * gs_restrict cr, fixed x, int id)
{
    int iy;

    /* This should only happen if we were entirely out of bounds,
     * or if everything was within a zero height horizontal
     * rectangle from the start point. */
    if (cr->first) {
        int iy = fixed2int(cr->y) - cr->base;
        /* Any zero height rectangle counts as filled, except
         * those on the baseline of a pixel. */
        if (cr->d == DIRN_UNSET && (cr->y & 0xff) == 0)
            return;
        assert(cr->left != max_fixed && cr->right != min_fixed);
        if (iy >= 0 && iy < cr->scanlines) {
            int *row = &cr->table[cr->index[iy]];
            int count = *row = (*row)+2; /* Increment the count */
            row[4 * count - 7] = cr->left;
            row[4 * count - 6] = DIRN_UP | (cr->lid<<1);
            row[4 * count - 5] = cr->right;
            row[4 * count - 4] = cr->rid;
            row[4 * count - 3] = cr->right;
            row[4 * count - 2] = DIRN_DOWN | (cr->rid<<1);
            row[4 * count - 1] = cr->right;
            row[4 * count    ] = cr->rid;
        }
        return;
    }

    /* Merge save into current if we can */
    iy = fixed2int(cr->y) - cr->base;
    if (cr->saved && iy == cr->save_iy &&
        (cr->d == cr->save_d || cr->save_d == DIRN_UNSET)) {
        if (cr->left > cr->save_left) {
            cr->left = cr->save_left;
            cr->lid  = cr->save_lid;
        }
        if (cr->right < cr->save_right) {
            cr->right = cr->save_right;
            cr->rid = cr->save_rid;
        }
        cursor_output_tr(cr, iy);
        return;
    }

    /* Merge not possible */
    cursor_output_tr(cr, iy);
    if (cr->saved) {
        cr->left  = cr->save_left;
        cr->lid   = cr->save_lid;
        cr->right = cr->save_right;
        cr->rid   = cr->save_rid;
        assert(cr->save_d != DIRN_UNSET);
        if (cr->save_d != DIRN_UNSET)
            cr->d = cr->save_d;
        cursor_output_tr(cr, cr->save_iy);
    }
}

static inline void
cursor_null_tr(cursor_tr *cr)
{
    cr->right = min_fixed;
    cr->left  = max_fixed;
    cr->d     = DIRN_UNSET;
}

static void mark_line_tr_app(cursor_tr * gs_restrict cr, fixed sx, fixed sy, fixed ex, fixed ey, int id)
{
    int isy, iey;
    fixed saved_sy = sy;
    fixed saved_ex = ex;
    fixed saved_ey = ey;
    int truncated;

    if (sy == ey && sx == ex)
        return;

    isy = fixed2int(sy) - cr->base;
    iey = fixed2int(ey) - cr->base;

#ifdef DEBUG_SCAN_CONVERTER
    if (debugging_scan_converter)
        dlprintf6("Marking line (tr_app) from %x,%x to %x,%x (%x,%x)\n", sx, sy, ex, ey, isy, iey);
#endif
#ifdef DEBUG_OUTPUT_SC_AS_PS
    dlprintf("0.001 setlinewidth 0 0 0 setrgbcolor %%PS\n");
    coord("moveto", sx, sy);
    coord("lineto", ex, ey);
    dlprintf("stroke %%PS\n");
#endif

    /* Horizontal motion at the bottom of a pixel is ignored */
    if (sy == ey && (sy & 0xff) == 0)
        return;

    assert(cr->y == sy &&
           ((cr->left <= sx && cr->right >= sx) || ((sy & 0xff) == 0)) &&
           cr->d >= DIRN_UNSET && cr->d <= DIRN_DOWN);

    if (isy < iey) {
        /* Rising line */
        if (iey < 0 || isy >= cr->scanlines) {
            /* All line is outside. */
            if ((ey & 0xff) == 0) {
                cursor_null_tr(cr);
            } else {
                cr->left = ex;
                cr->lid = id;
                cr->right = ex;
                cr->rid = id;
            }
            cr->y = ey;
            cr->first = 0;
            return;
        }
        if (isy < 0) {
            /* Move sy up */
            int64_t y = (int64_t)ey - (int64_t)sy;
            fixed new_sy = int2fixed(cr->base);
            int64_t dy = (int64_t)new_sy - (int64_t)sy;
            sx += (int)(((((int64_t)ex-sx))*dy + y/2)/y);
            sy = new_sy;
            cursor_null_tr(cr);
            cr->y = sy;
            isy = 0;
        }
        truncated = iey > cr->scanlines;
        if (truncated) {
            /* Move ey down */
            int64_t y = (int64_t)ey - (int64_t)sy;
            fixed new_ey = int2fixed(cr->base + cr->scanlines);
            int64_t dy = (int64_t)ey - (int64_t)new_ey;
            saved_ex = ex;
            saved_ey = ey;
            ex -= (int)(((((int64_t)ex-sx))*dy + y/2)/y);
            ey = new_ey;
            iey = cr->scanlines;
        }
    } else {
        /* Falling line */
        if (isy < 0 || iey >= cr->scanlines) {
            /* All line is outside. */
            if ((ey & 0xff) == 0) {
                cursor_null_tr(cr);
            } else {
                cr->left = ex;
                cr->lid = id;
                cr->right = ex;
                cr->rid = id;
            }
            cr->y = ey;
            cr->first = 0;
            return;
        }
        truncated = iey < 0;
        if (truncated) {
            /* Move ey up */
            int64_t y = (int64_t)ey - (int64_t)sy;
            fixed new_ey = int2fixed(cr->base);
            int64_t dy = (int64_t)ey - (int64_t)new_ey;
            ex -= (int)(((((int64_t)ex-sx))*dy + y/2)/y);
            ey = new_ey;
            iey = 0;
        }
        if (isy >= cr->scanlines) {
            /* Move sy down */
            int64_t y = (int64_t)ey - (int64_t)sy;
            fixed new_sy = int2fixed(cr->base + cr->scanlines);
            int64_t dy = (int64_t)new_sy - (int64_t)sy;
            sx += (int)(((((int64_t)ex-sx))*dy + y/2)/y);
            sy = new_sy;
            cursor_null_tr(cr);
            cr->y = sy;
            isy = cr->scanlines;
        }
    }

    cursor_left_merge_tr(cr, sx, id);
    cursor_right_merge_tr(cr, sx, id);

    assert(cr->left <= sx);
    assert(cr->right >= sx);
    assert(cr->y == sy);

    /* A note: The code below used to be of the form:
     *   if (isy == iey)   ... deal with horizontal lines
     *   else if (ey > sy) {
     *     fixed y_steps = ey - sy;
     *      ... deal with rising lines ...
     *   } else {
     *     fixed y_steps = ey - sy;
     *     ... deal with falling lines
     *   }
     * but that lead to problems, for instance, an example seen
     * has sx=2aa8e, sy=8aee7, ex=7ffc1686, ey=8003e97a.
     * Thus isy=84f, iey=ff80038a. We can see that ey < sy, but
     * sy - ey < 0!
     * We therefore rejig our code so that the choice between
     * cases is done based on the sign of y_steps rather than
     * the relative size of ey and sy.
     */

    /* First, deal with lines that don't change scanline.
     * This accommodates horizontal lines. */
    if (isy == iey) {
        if (saved_sy == saved_ey) {
            /* Horizontal line. Don't change cr->d, don't flush. */
            if ((ey & 0xff) == 0) {
                cursor_null_tr(cr);
                goto no_merge;
            }
        } else if (saved_sy > saved_ey) {
            /* Falling line, flush if previous was rising */
            int skip = cursor_down_tr(cr, sx, id);
            if ((ey & 0xff) == 0) {
                /* We are falling to the baseline of a subpixel, so output
                 * for the current pixel, and leave the cursor nulled. */
                if (sx <= ex) {
                    cursor_right_merge_tr(cr, ex, id);
                } else {
                    cursor_left_merge_tr(cr, ex, id);
                }
                if (!skip)
                    cursor_output_tr(cr, fixed2int(cr->y) - cr->base);
                cursor_null_tr(cr);
                goto no_merge;
            }
        } else {
            /* Rising line, flush if previous was falling */
            cursor_up_tr(cr, sx, id);
            if ((ey & 0xff) == 0) {
                cursor_null_tr(cr);
                goto no_merge;
            }
        }
        if (sx <= ex) {
            cursor_right_merge_tr(cr, ex, id);
        } else {
            cursor_left_merge_tr(cr, ex, id);
        }
no_merge:
        cr->y = ey;
        if (sy > saved_ey)
            goto end; /* pdfking: was goto endFalling (C++ forbids jumping
                       * past the initialisations below); endFalling is
                       * immediately followed by end. */
    } else if (iey > isy) {
        /* So lines increasing in y. */
        /* We want to change from sy to ey, which are guaranteed to be on
         * different scanlines. We do this in 3 phases.
         * Phase 1 gets us from sy to the next scanline boundary. (We may exit after phase 1).
         * Phase 2 gets us all the way to the last scanline boundary. (This may be a null operation)
         * Phase 3 gets us from the last scanline boundary to ey. (We are guaranteed to have output the cursor at least once before phase 3).
         */
        int phase1_y_steps = (-sy) & (fixed_1 - 1);
        int phase3_y_steps = ey & (fixed_1 - 1);
        ufixed y_steps = (ufixed)ey - (ufixed)sy;

        cursor_up_tr(cr, sx, id);

        if (sx == ex) {
            /* Vertical line. (Rising) */

            /* Phase 1: */
            if (phase1_y_steps) {
                /* If phase 1 will move us into a new scanline, then we must
                 * flush it before we move. */
                cursor_step_tr(cr, phase1_y_steps, sx, id, 0);
                sy += phase1_y_steps;
                y_steps -= phase1_y_steps;
                if (y_steps == 0) {
                    cursor_null_tr(cr);
                    goto end;
                }
            }

            /* Phase 3: precalculation */
            y_steps -= phase3_y_steps;

            /* Phase 2: */
            y_steps = fixed2int(y_steps);
            assert(y_steps >= 0);
            if (y_steps > 0) {
                cursor_always_step_tr(cr, fixed_1, sx, id, 0);
                y_steps--;
                while (y_steps) {
                    cursor_always_step_inrange_vertical_tr(cr, fixed_1, sx, id);
                    y_steps--;
                }
            }

            /* Phase 3 */
            assert(cr->left == sx && cr->right == sx && cr->lid == id && cr->rid == id);
            if (phase3_y_steps == 0)
                cursor_null_tr(cr);
            else
                cr->y += phase3_y_steps;
        } else if (sx < ex) {
            /* Lines increasing in x. (Rightwards, rising) */
            int phase1_x_steps, phase3_x_steps;
            /* Use unsigned int here, to allow for extreme cases like
             * ex = 0x7fffffff, sx = 0x80000000 */
            unsigned int x_steps = ex - sx;

            /* Phase 1: */
            if (phase1_y_steps) {
                phase1_x_steps = (int)(((int64_t)x_steps * phase1_y_steps + y_steps/2) / y_steps);
                sx += phase1_x_steps;
                cursor_right_merge_tr(cr, sx, id);
                x_steps -= phase1_x_steps;
                cursor_step_tr(cr, phase1_y_steps, sx, id, 0);
                sy += phase1_y_steps;
                y_steps -= phase1_y_steps;
                if (y_steps == 0) {
                    cursor_null_tr(cr);
                    goto end;
                }
            }

            /* Phase 3: precalculation */
            phase3_x_steps = (int)(((int64_t)x_steps * phase3_y_steps + y_steps/2) / y_steps);
            x_steps -= phase3_x_steps;
            y_steps -= phase3_y_steps;
            assert((y_steps & (fixed_1 - 1)) == 0);

            /* Phase 2: */
            y_steps = fixed2int(y_steps);
            assert(y_steps >= 0);
            if (y_steps) {
                /* We want to change sx by x_steps in y_steps steps.
                 * So each step, we add x_steps/y_steps to sx. That's x_inc + n_inc/y_steps. */
                int x_inc = x_steps/y_steps;
                int n_inc = x_steps - (x_inc * y_steps);
                int f = y_steps/2;
                int d = y_steps;

                /* Special casing the first iteration, allows us to simplify
                 * the following loop. */
                sx += x_inc;
                f -= n_inc;
                if (f < 0)
                    f += d, sx++;
                cursor_right_merge_tr(cr, sx, id);
                cursor_always_step_tr(cr, fixed_1, sx, id, 0);
                y_steps--;

                while (y_steps) {
                    sx += x_inc;
                    f -= n_inc;
                    if (f < 0)
                        f += d, sx++;
                    cursor_right_tr(cr, sx, id);
                    cursor_always_inrange_step_right_tr(cr, fixed_1, sx, id);
                    y_steps--;
                };
            }

            /* Phase 3 */
            assert(cr->left <= ex && cr->lid == id && cr->right >= sx);
            if (phase3_y_steps == 0)
                cursor_null_tr(cr);
            else {
                cursor_right_tr(cr, ex, id);
                cr->y += phase3_y_steps;
            }
        } else {
            /* Lines decreasing in x. (Leftwards, rising) */
            int phase1_x_steps, phase3_x_steps;
            /* Use unsigned int here, to allow for extreme cases like
             * sx = 0x7fffffff, ex = 0x80000000 */
            unsigned int x_steps = sx - ex;

            /* Phase 1: */
            if (phase1_y_steps) {
                phase1_x_steps = (int)(((int64_t)x_steps * phase1_y_steps + y_steps/2) / y_steps);
                x_steps -= phase1_x_steps;
                sx -= phase1_x_steps;
                cursor_left_merge_tr(cr, sx, id);
                cursor_step_tr(cr, phase1_y_steps, sx, id, 0);
                sy += phase1_y_steps;
                y_steps -= phase1_y_steps;
                if (y_steps == 0) {
                    cursor_null_tr(cr);
                    goto end;
                }
            }

            /* Phase 3: precalculation */
            phase3_x_steps = (int)(((int64_t)x_steps * phase3_y_steps + y_steps/2) / y_steps);
            x_steps -= phase3_x_steps;
            y_steps -= phase3_y_steps;
            assert((y_steps & (fixed_1 - 1)) == 0);

            /* Phase 2: */
            y_steps = fixed2int((unsigned int)y_steps);
            assert(y_steps >= 0);
            if (y_steps) {
                /* We want to change sx by x_steps in y_steps steps.
                 * So each step, we sub x_steps/y_steps from sx. That's x_inc + n_inc/ey. */
                int x_inc = x_steps/y_steps;
                int n_inc = x_steps - (x_inc * y_steps);
                int f = y_steps/2;
                int d = y_steps;

                /* Special casing the first iteration, allows us to simplify
                 * the following loop. */
                sx -= x_inc;
                f -= n_inc;
                if (f < 0)
                    f += d, sx--;
                cursor_left_merge_tr(cr, sx, id);
                cursor_always_step_tr(cr, fixed_1, sx, id, 0);
                y_steps--;

                while (y_steps) {
                    sx -= x_inc;
                    f -= n_inc;
                    if (f < 0)
                        f += d, sx--;
                    cursor_left_tr(cr, sx, id);
                    cursor_always_inrange_step_left_tr(cr, fixed_1, sx, id);
                    y_steps--;
                }
            }

            /* Phase 3 */
            assert(cr->right >= ex && cr->rid == id && cr->left <= sx);
            if (phase3_y_steps == 0)
                cursor_null_tr(cr);
            else {
                cursor_left_tr(cr, ex, id);
                cr->y += phase3_y_steps;
            }
        }
    } else {
        /* So lines decreasing in y. */
        /* We want to change from sy to ey, which are guaranteed to be on
         * different scanlines. We do this in 3 phases.
         * Phase 1 gets us from sy to the next scanline boundary. This never causes an output.
         * Phase 2 gets us all the way to the last scanline boundary. This is guaranteed to cause an output.
         * Phase 3 gets us from the last scanline boundary to ey. We are guaranteed to have outputted by now.
         */
        int phase1_y_steps = sy & (fixed_1 - 1);
        int phase3_y_steps = (-ey) & (fixed_1 - 1);
        ufixed y_steps = (ufixed)sy - (ufixed)ey;

        int skip = cursor_down_tr(cr, sx, id);

        if (sx == ex) {
            /* Vertical line. (Falling) */

            /* Phase 1: */
            if (phase1_y_steps) {
                /* Phase 1 in a falling line never moves us into a new scanline. */
                cursor_never_step_vertical_tr(cr, -phase1_y_steps, sx, id);
                sy -= phase1_y_steps;
                y_steps -= phase1_y_steps;
                if (y_steps == 0)
                    goto endFallingLeftOnEdgeOfPixel;
            }

            /* Phase 3: precalculation */
            y_steps -= phase3_y_steps;
            assert((y_steps & (fixed_1 - 1)) == 0);

            /* Phase 2: */
            y_steps = fixed2int(y_steps);
            assert(y_steps >= 0);
            if (y_steps) {
                cursor_always_step_tr(cr, -fixed_1, sx, id, skip);
                skip = 0;
                y_steps--;
                while (y_steps) {
                    cursor_always_step_inrange_vertical_tr(cr, -fixed_1, sx, id);
                    y_steps--;
                }
            }

            /* Phase 3 */
            if (phase3_y_steps == 0) {
endFallingLeftOnEdgeOfPixel:
                cursor_always_step_inrange_vertical_tr(cr, 0, sx, id);
                cursor_null_tr(cr);
            } else {
                cursor_step_tr(cr, -phase3_y_steps, sx, id, skip);
                assert(cr->left == sx && cr->lid == id && cr->right == sx && cr->rid == id);
            }
        } else if (sx < ex) {
            /* Lines increasing in x. (Rightwards, falling) */
            int phase1_x_steps, phase3_x_steps;
            /* Use unsigned int here, to allow for extreme cases like
             * ex = 0x7fffffff, sx = 0x80000000 */
            unsigned int x_steps = ex - sx;

            /* Phase 1: */
            if (phase1_y_steps) {
                phase1_x_steps = (int)(((int64_t)x_steps * phase1_y_steps + y_steps/2) / y_steps);
                x_steps -= phase1_x_steps;
                sx += phase1_x_steps;
                /* Phase 1 in a falling line never moves us into a new scanline. */
                cursor_never_step_right_tr(cr, -phase1_y_steps, sx, id);
                sy -= phase1_y_steps;
                y_steps -= phase1_y_steps;
                if (y_steps == 0)
                    goto endFallingRightOnEdgeOfPixel;
            }

            /* Phase 3: precalculation */
            phase3_x_steps = (int)(((int64_t)x_steps * phase3_y_steps + y_steps/2) / y_steps);
            x_steps -= phase3_x_steps;
            y_steps -= phase3_y_steps;
            assert((y_steps & (fixed_1 - 1)) == 0);

            /* Phase 2: */
            y_steps = fixed2int(y_steps);
            assert(y_steps >= 0);
            if (y_steps) {
                /* We want to change sx by x_steps in y_steps steps.
                 * So each step, we add x_steps/y_steps to sx. That's x_inc + n_inc/ey. */
                int x_inc = x_steps/y_steps;
                int n_inc = x_steps - (x_inc * y_steps);
                int f = y_steps/2;
                int d = y_steps;

                cursor_always_step_tr(cr, -fixed_1, sx, id, skip);
                skip = 0;
                sx += x_inc;
                f -= n_inc;
                if (f < 0)
                    f += d, sx++;
                cursor_right_tr(cr, sx, id);
                y_steps--;

                while (y_steps) {
                    cursor_always_inrange_step_right_tr(cr, -fixed_1, sx, id);
                    sx += x_inc;
                    f -= n_inc;
                    if (f < 0)
                        f += d, sx++;
                    cursor_right_tr(cr, sx, id);
                    y_steps--;
                }
            }

            /* Phase 3 */
            if (phase3_y_steps == 0) {
endFallingRightOnEdgeOfPixel:
                cursor_always_step_inrange_vertical_tr(cr, 0, sx, id);
                cursor_null_tr(cr);
            } else {
                cursor_step_tr(cr, -phase3_y_steps, sx, id, skip);
                cursor_right_tr(cr, ex, id);
                assert(cr->left == sx && cr->lid == id && cr->right == ex && cr->rid == id);
            }
        } else {
            /* Lines decreasing in x. (Falling) */
            int phase1_x_steps, phase3_x_steps;
            /* Use unsigned int here, to allow for extreme cases like
             * sx = 0x7fffffff, ex = 0x80000000 */
            unsigned int x_steps = sx - ex;

            /* Phase 1: */
            if (phase1_y_steps) {
                phase1_x_steps = (int)(((int64_t)x_steps * phase1_y_steps + y_steps/2) / y_steps);
                x_steps -= phase1_x_steps;
                sx -= phase1_x_steps;
                /* Phase 1 in a falling line never moves us into a new scanline. */
                cursor_never_step_left_tr(cr, -phase1_y_steps, sx, id);
                sy -= phase1_y_steps;
                y_steps -= phase1_y_steps;
                if (y_steps == 0)
                    goto endFallingVerticalOnEdgeOfPixel;
            }

            /* Phase 3: precalculation */
            phase3_x_steps = (int)(((int64_t)x_steps * phase3_y_steps + y_steps/2) / y_steps);
            x_steps -= phase3_x_steps;
            y_steps -= phase3_y_steps;
            assert((y_steps & (fixed_1 - 1)) == 0);

            /* Phase 2: */
            y_steps = fixed2int(y_steps);
            assert(y_steps >= 0);
            if (y_steps) {
                /* We want to change sx by x_steps in y_steps steps.
                 * So each step, we sub x_steps/y_steps from sx. That's x_inc + n_inc/ey. */
                int x_inc = x_steps/y_steps;
                int n_inc = x_steps - (x_inc * y_steps);
                int f = y_steps/2;
                int d = y_steps;

                cursor_always_step_tr(cr, -fixed_1, sx, id, skip);
                skip = 0;
                sx -= x_inc;
                f -= n_inc;
                if (f < 0)
                    f += d, sx--;
                cursor_left_tr(cr, sx, id);
                y_steps--;

                while (y_steps) {
                    cursor_always_inrange_step_left_tr(cr, -fixed_1, sx, id);
                    sx -= x_inc;
                    f -= n_inc;
                    if (f < 0)
                        f += d, sx--;
                    cursor_left_tr(cr, sx, id);
                    y_steps--;
                }
            }

            /* Phase 3 */
            if (phase3_y_steps == 0) {
endFallingVerticalOnEdgeOfPixel:
                cursor_always_step_inrange_vertical_tr(cr, 0, sx, id);
                cursor_null_tr(cr);
            } else {
                cursor_step_tr(cr, -phase3_y_steps, sx, id, skip);
                cursor_left_tr(cr, ex, id);
                assert(cr->left == ex && cr->lid == id && cr->right == sx && cr->rid == id);
            }
        }
/* endFalling: */
    }

end:
    if (truncated) {
        cr->left = saved_ex;
        cr->lid = id;
        cr->right = saved_ex;
        cr->rid = id;
        cr->y = saved_ey;
    }
}

static void mark_curve_tr_app(cursor_tr * gs_restrict cr, fixed sx, fixed sy, fixed c1x, fixed c1y, fixed c2x, fixed c2y, fixed ex, fixed ey, int depth, int * gs_restrict id)
{
        int ax = (sx + c1x)>>1;
        int ay = (sy + c1y)>>1;
        int bx = (c1x + c2x)>>1;
        int by = (c1y + c2y)>>1;
        int cx = (c2x + ex)>>1;
        int cy = (c2y + ey)>>1;
        int dx = (ax + bx)>>1;
        int dy = (ay + by)>>1;
        int fx = (bx + cx)>>1;
        int fy = (by + cy)>>1;
        int gx = (dx + fx)>>1;
        int gy = (dy + fy)>>1;

        assert(depth >= 0);
        if (depth == 0) {
            *id += 1;
            mark_line_tr_app(cr, sx, sy, ex, ey, *id);
        } else {
            depth--;
            mark_curve_tr_app(cr, sx, sy, ax, ay, dx, dy, gx, gy, depth, id);
            mark_curve_tr_app(cr, gx, gy, fx, fy, cx, cy, ex, ey, depth, id);
        }
}

static void mark_curve_big_tr_app(cursor_tr * gs_restrict cr, fixed64 sx, fixed64 sy, fixed64 c1x, fixed64 c1y, fixed64 c2x, fixed64 c2y, fixed64 ex, fixed64 ey, int depth, int * gs_restrict id)
{
    fixed64 ax = (sx + c1x)>>1;
    fixed64 ay = (sy + c1y)>>1;
    fixed64 bx = (c1x + c2x)>>1;
    fixed64 by = (c1y + c2y)>>1;
    fixed64 cx = (c2x + ex)>>1;
    fixed64 cy = (c2y + ey)>>1;
    fixed64 dx = (ax + bx)>>1;
    fixed64 dy = (ay + by)>>1;
    fixed64 fx = (bx + cx)>>1;
    fixed64 fy = (by + cy)>>1;
    fixed64 gx = (dx + fx)>>1;
    fixed64 gy = (dy + fy)>>1;

    assert(depth >= 0);
    if (depth == 0) {
        *id += 1;
        mark_line_tr_app(cr, (fixed)sx, (fixed)sy, (fixed)ex, (fixed)ey, *id);
    } else {
        depth--;
        mark_curve_big_tr_app(cr, sx, sy, ax, ay, dx, dy, gx, gy, depth, id);
        mark_curve_big_tr_app(cr, gx, gy, fx, fy, cx, cy, ex, ey, depth, id);
    }
}

static void mark_curve_top_tr_app(cursor_tr * gs_restrict cr, fixed sx, fixed sy, fixed c1x, fixed c1y, fixed c2x, fixed c2y, fixed ex, fixed ey, int depth, int * gs_restrict id)
{
    fixed test = (sx^(sx<<1))|(sy^(sy<<1))|(c1x^(c1x<<1))|(c1y^(c1y<<1))|(c2x^(c2x<<1))|(c2y^(c2y<<1))|(ex^(ex<<1))|(ey^(ey<<1));

    if (test < 0)
        mark_curve_big_tr_app(cr, sx, sy, c1x, c1y, c2x, c2y, ex, ey, depth, id);
    else
        mark_curve_tr_app(cr, sx, sy, c1x, c1y, c2x, c2y, ex, ey, depth, id);
}

static int make_table_tr_app(gx_device     * pdev,
                             gx_path       * path,
                             gs_fixed_rect * ibox,
                             int           * scanlines,
                             int          ** index,
                             int          ** table,
                             gx_edgebuffer * edgebuffer)
{
    return make_table_template(pdev, path, ibox, 4, 0, scanlines, index, table, edgebuffer);
}

static void
fill_zero_app_tr(int *row, const fixed *x)
{
    int n = *row = (*row)+2; /* Increment the count */
    row[4*n-7] = x[0];
    row[4*n-6] = 0;
    row[4*n-5] = x[1];
    row[4*n-4] = 0;
    row[4*n-3] = x[1];
    row[4*n-2] = (1<<1)|1;
    row[4*n-1] = x[1];
    row[4*n  ] = 1;
}

int gx_scan_convert_tr_app(gx_device     * gs_restrict pdev,
                           gx_path       * gs_restrict path,
                     const gs_fixed_rect * gs_restrict clip,
                           gx_edgebuffer * gs_restrict edgebuffer,
                           fixed                    fixed_flat)
{
    gs_fixed_rect  ibox;
    gs_fixed_rect  bbox;
    int            scanlines;
    const subpath *psub;
    int           *index;
    int           *table;
    int            i;
    cursor_tr      cr;
    int            code;
    int            id = 0;
    int            zero;

    edgebuffer->index = NULL;
    edgebuffer->table = NULL;

    /* Bale out if no actual path. We see this with the clist */
    if (path->first_subpath == NULL)
        return 0;

    zero = make_bbox(path, clip, &bbox, &ibox, 0);
    if (zero < 0)
        return zero;

    if (ibox.q.y <= ibox.p.y)
        return 0;

    code = make_table_tr_app(pdev, path, &ibox, &scanlines, &index, &table, edgebuffer);
    if (code != 0) /* > 0 means "retry with smaller height" */
        return code;

    if (scanlines == 0)
        return 0;

    if (zero) {
        code = zero_case(pdev, path, &ibox, index, table, fixed_flat, fill_zero_app_tr);
    } else {

    /* Step 2 continued: Now we run through the path, filling in the real
     * values. */
    cr.scanlines = scanlines;
    cr.index     = index;
    cr.table     = table;
    cr.base      = ibox.p.y;
    for (psub = path->first_subpath; psub != 0;) {
        const segment *pseg = (const segment *)psub;
        fixed ex = pseg->pt.x;
        fixed ey = pseg->pt.y;
        fixed ix = ex;
        fixed iy = ey;
        fixed sx, sy;

        if ((ey & 0xff) == 0) {
            cr.left  = max_fixed;
            cr.right = min_fixed;
        } else {
            cr.left = cr.right = ex;
        }
        cr.lid = cr.rid = id+1;
        cr.y = ey;
        cr.d = DIRN_UNSET;
        cr.first = 1;
        cr.saved = 0;

        while ((pseg = pseg->next) != 0 &&
               pseg->type != s_start
            ) {
            sx = ex;
            sy = ey;
            ex = pseg->pt.x;
            ey = pseg->pt.y;

            switch (pseg->type) {
                default:
                case s_start: /* Should never happen */
                case s_dash:  /* We should never be seeing a dash here */
                    assert("This should never happen" == NULL);
                    break;
                case s_curve: {
                    const curve_segment *const pcur = (const curve_segment *)pseg;
                    int k = gx_curve_log2_samples(sx, sy, pcur, fixed_flat);

                    mark_curve_top_tr_app(&cr, sx, sy, pcur->p1.x, pcur->p1.y, pcur->p2.x, pcur->p2.y, ex, ey, k, &id);
                    break;
                }
                case s_gap:
                case s_line:
                case s_line_close:
                    mark_line_tr_app(&cr, sx, sy, ex, ey, ++id);
                    break;
            }
        }
        /* And close any open segments */
        mark_line_tr_app(&cr, ex, ey, ix, iy, ++id);
        cursor_flush_tr(&cr, ex, id);
        psub = (const subpath *)pseg;
    }
    }

    /* Step 2 complete: We now have a complete list of intersection data in
     * table, indexed by index. */

    edgebuffer->base   = ibox.p.y;
    edgebuffer->height = scanlines;
    edgebuffer->xmin   = ibox.p.x;
    edgebuffer->xmax   = ibox.q.x;
    edgebuffer->index  = index;
    edgebuffer->table  = table;

#ifdef DEBUG_SCAN_CONVERTER
    if (debugging_scan_converter) {
        dlprintf("Before sorting\n");
        gx_edgebuffer_print_tr_app(edgebuffer);
    }
#endif

    /* Step 3: Sort the intersects on x */
    for (i=0; i < scanlines; i++) {
        int *row = &table[index[i]];
        int  rowlen = *row++;

        /* Bubblesort short runs, qsort longer ones. */
        /* Figure of '6' comes from testing */
        if (rowlen <= 6) {
            int j, k;
            for (j = 0; j < rowlen-1; j++) {
                int * gs_restrict t = &row[j<<2];
                for (k = j+1; k < rowlen; k++) {
                    int * gs_restrict s = &row[k<<2];
                    int tmp;
                    if (t[0] < s[0])
                        continue;
                    if (t[0] > s[0])
                        goto swap0213;
                    if (t[2] < s[2])
                        continue;
                    if (t[2] > s[2])
                        goto swap213;
                    if (t[1] < s[1])
                        continue;
                    if (t[1] > s[1])
                        goto swap13;
                    if (t[3] <= s[3])
                        continue;
                    if (0) {
swap0213:
                        tmp = t[0], t[0] = s[0], s[0] = tmp;
swap213:
                        tmp = t[2], t[2] = s[2], s[2] = tmp;
swap13:
                        tmp = t[1], t[1] = s[1], s[1] = tmp;
                    }
                    tmp = t[3], t[3] = s[3], s[3] = tmp;
                }
            }
        } else
            qsort(row, rowlen, 4*sizeof(int), edgecmp_tr);
    }

    return 0;
}

/* Step 5: Filter the intersections according to the rules */
int
gx_filter_edgebuffer_tr_app(gx_device       * gs_restrict pdev,
                            gx_edgebuffer   * gs_restrict edgebuffer,
                            int                        rule)
{
    int i;
    int marked_id = 0;

#ifdef DEBUG_SCAN_CONVERTER
    if (debugging_scan_converter) {
        dlprintf("Before filtering:\n");
        gx_edgebuffer_print_tr_app(edgebuffer);
    }
#endif

    for (i=0; i < edgebuffer->height; i++) {
        int *row      = &edgebuffer->table[edgebuffer->index[i]];
        int  rowlen   = *row++;
        int *rowstart = row;
        int *rowout   = row;
        int  ll, llid, lr, lrid, rlid, rr, rrid, wind, marked_to;

        /* Avoid double setting pixels, by keeping where we have marked to. */
        marked_to = INT_MIN;
        while (rowlen > 0) {
            if (rule == gx_rule_even_odd) {
                /* Even Odd */
                ll   = *row++;
                llid = (*row++)>>1;
                lr   = *row++;
                lrid = *row++;
                rowlen--;

                /* We will fill solidly from ll to at least lr, possibly further */
                assert(rowlen > 0);
                (void)row++; /* rl not needed here */
                (void)row++;
                rr   = *row++;
                rrid = *row++;
                rowlen--;
                if (rr > lr) {
                    lr   = rr;
                    lrid = rrid;
                }
            } else {
                /* Non-Zero */
                int w;

                ll   = *row++;
                llid = *row++;
                lr   = *row++;
                lrid = *row++;
                wind = -(llid&1) | 1;
                llid >>= 1;
                rowlen--;

                assert(rowlen > 0);
                do {
                    (void)row++; /* rl not needed */
                    rlid = *row++;
                    rr   = *row++;
                    rrid = *row++;
                    w = -(rlid&1) | 1;
                    rlid >>= 1;
                    rowlen--;
                    if (rr > lr) {
                        lr   = rr;
                        lrid = rrid;
                    }
                    wind += w;
                    if (wind == 0)
                        break;
                } while (rowlen > 0);
            }

            if (lr < marked_to)
                continue;

            if (marked_to >= ll) {
                if (rowout == rowstart) {
                    ll   = marked_to;
                    llid = --marked_id;
                } else {
                    rowout -= 4;
                    ll   = rowout[0];
                    llid = rowout[1];
                }
            }

            if (lr >= ll) {
                *rowout++ = ll;
                *rowout++ = llid;
                *rowout++ = lr;
                *rowout++ = lrid;
                marked_to = lr;
            }
        }
        rowstart[-1] = (rowout - rowstart)>>2;
    }
    return 0;
}

/* Step 6: Fill */
int
gx_fill_edgebuffer_tr_app(gx_device       * gs_restrict pdev,
                    const gx_device_color * gs_restrict pdevc,
                          gx_edgebuffer   * gs_restrict edgebuffer,
                          int                        log_op)
{
    int i, j, code;
    int mfb = pdev->max_fill_band;

#ifdef DEBUG_SCAN_CONVERTER
    if (debugging_scan_converter) {
        dlprintf("Filling:\n");
        gx_edgebuffer_print_filtered_tr_app(edgebuffer);
    }
#endif

    for (i=0; i < edgebuffer->height; ) {
        int *row    = &edgebuffer->table[edgebuffer->index[i]];
        int  rowlen = *row++;
        int *row2;
        int *rowptr;
        int *row2ptr;
        int y_band_max;

        if (mfb) {
            y_band_max = (i & ~(mfb-1)) + mfb;
            if (y_band_max > edgebuffer->height)
                y_band_max = edgebuffer->height;
        } else {
            y_band_max = edgebuffer->height;
        }

        /* See how many scanlines match i */
        for (j = i+1; j < y_band_max; j++) {
            int row2len;

            row2    = &edgebuffer->table[edgebuffer->index[j]];
            row2len = *row2++;
            row2ptr = row2;
            rowptr  = row;

            if (rowlen != row2len)
                break;
            while (row2len > 0) {
                if (rowptr[1] != row2ptr[1] || rowptr[3] != row2ptr[3])
                    goto rowdifferent;
                rowptr  += 4;
                row2ptr += 4;
                row2len--;
            }
        }
rowdifferent:{}

        /* So j is the first scanline that doesn't match i */

        /* The first scanline is always sent as rectangles */
        while (rowlen > 0) {
            int left  = row[0];
            int right = row[2];
            row += 4;
            left = fixed2int(left);
            right = fixed2int(right + fixed_1 - 1);
            rowlen--;

            right -= left;
            if (right > 0) {
#ifdef DEBUG_OUTPUT_SC_AS_PS
                dlprintf("0.001 setlinewidth 1 0 0 setrgbcolor %%red %%PS\n");
                coord("moveto", int2fixed(left), int2fixed(edgebuffer->base+i));
                coord("lineto", int2fixed(left+right), int2fixed(edgebuffer->base+i));
                coord("lineto", int2fixed(left+right), int2fixed(edgebuffer->base+i+1));
                coord("lineto", int2fixed(left), int2fixed(edgebuffer->base+i+1));
                dlprintf("closepath stroke %%PS\n");
#endif
                if (log_op < 0)
                    code = dev_proc(pdev, fill_rectangle)(pdev, left, edgebuffer->base+i, right, 1, pdevc->colors.pure);
                else
                    code = gx_fill_rectangle_device_rop(left, edgebuffer->base+i, right, 1, pdevc, pdev, (gs_logical_operation_t)log_op);
                if (code < 0)
                    return code;
            }
        }

        /* The middle section (all but the first and last
         * scanlines) can be sent as a trapezoid. */
        if (i + 2 < j) {
            gs_fixed_edge le;
            gs_fixed_edge re;
            fixed ybot = int2fixed(edgebuffer->base+i+1);
            fixed ytop = int2fixed(edgebuffer->base+j-1);
            int *row3, *row4;
            int offset = 1;
            row    = &edgebuffer->table[edgebuffer->index[i]];
            row2    = &edgebuffer->table[edgebuffer->index[i+1]];
            row3    = &edgebuffer->table[edgebuffer->index[j-2]];
            row4    = &edgebuffer->table[edgebuffer->index[j-1]];
            rowlen = *row;
            while (rowlen > 0) {
                /* The fill rules used by fill_trap state that if a
                 * pixel centre is touched by a boundary, the pixel
                 * will be filled iff the boundary is horizontal and
                 * the filled region is above it, or the boundary is
                 * not horizontal, and the filled region is to the
                 * right of it.
                 *
                 * We need to fill "any part of a pixel", not just
                 * "centre covered", so we need to adjust our edges
                 * by half a pixel in both X and Y.
                 *
                 * X is relatively easy. We move the left edge back by
                 * just less than half, so ...00 goes to ...81 and
                 * therefore does not cause an extra pixel to get filled.
                 *
                 * Similarly, we move the right edge forward by half, so
                 *  ...00 goes to ...80 and therefore does not cause an
                 * extra pixel to get filled.
                 *
                 * For y, we can adjust edges up or down as appropriate.
                 * We move up by half, so ...0 goes to ..80 and therefore
                 * does not cause an extra pixel to get filled. We move
                 * down by just less than a half so that ...0 goes to
                 * ...81 and therefore does not cause an extra pixel to
                 * get filled.
                 *
                 * We use ybot = ...80 and ytop = ...81 in the trap call
                 * so that it just covers the pixel centres.
                 */
                if (row[offset] <= row4[offset]) {
                    le.start.x = row2[offset] - (fixed_half-1);
                    le.end.x   = row4[offset] - (fixed_half-1);
                    le.start.y = ybot + fixed_half;
                    le.end.y   = ytop + fixed_half;
                } else {
                    le.start.x = row [offset] - (fixed_half-1);
                    le.end.x   = row3[offset] - (fixed_half-1);
                    le.start.y = ybot - (fixed_half-1);
                    le.end.y   = ytop - (fixed_half-1);
                }
                if (row[offset+2] <= row4[offset+2]) {
                    re.start.x = row [offset+2] + fixed_half;
                    re.end.x   = row3[offset+2] + fixed_half;
                    re.start.y = ybot - (fixed_half-1);
                    re.end.y   = ytop - (fixed_half-1);
                } else {
                    re.start.x = row2[offset+2] + fixed_half;
                    re.end.x   = row4[offset+2] + fixed_half;
                    re.start.y = ybot + fixed_half;
                    re.end.y   = ytop + fixed_half;
                }
                offset += 4;
                rowlen--;

                assert(re.start.x >= le.start.x);
                assert(re.end.x >= le.end.x);
                assert(le.start.y <= ybot + fixed_half);
                assert(re.start.y <= ybot + fixed_half);
                assert(le.end.y >= ytop - (fixed_half - 1));
                assert(re.end.y >= ytop - (fixed_half - 1));

#ifdef DEBUG_OUTPUT_SC_AS_PS
                dlprintf("0.001 setlinewidth 0 1 0 setrgbcolor %% green %%PS\n");
                coord("moveto", le.start.x, le.start.y);
                coord("lineto", le.end.x, le.end.y);
                coord("lineto", re.end.x, re.end.y);
                coord("lineto", re.start.x, re.start.y);
                dlprintf("closepath stroke %%PS\n");
#endif
                code = dev_proc(pdev, fill_trapezoid)(
                                pdev,
                                &le,
                                &re,
                                ybot + fixed_half,
                                ytop - (fixed_half - 1),
                                0, /* bool swap_axes */
                                pdevc, /*const gx_drawing_color *pdcolor */
                                log_op);
                if (code < 0)
                    return code;
            }
        }

        if (i + 1 < j)
        {
            /* The last scanline is always sent as rectangles */
            row    = &edgebuffer->table[edgebuffer->index[j-1]];
            rowlen = *row++;
            while (rowlen > 0) {
                int left  = row[0];
                int right = row[2];
                row += 4;
                left = fixed2int(left);
                right = fixed2int(right + fixed_1 - 1);
                rowlen--;

                right -= left;
                if (right > 0) {
#ifdef DEBUG_OUTPUT_SC_AS_PS
                    dlprintf("0.001 setlinewidth 0 0 1 setrgbcolor %% blue %%PS\n");
                    coord("moveto", int2fixed(left), int2fixed(edgebuffer->base+j-1));
                    coord("lineto", int2fixed(left+right), int2fixed(edgebuffer->base+j-1));
                    coord("lineto", int2fixed(left+right), int2fixed(edgebuffer->base+j));
                    coord("lineto", int2fixed(left), int2fixed(edgebuffer->base+j));
                    dlprintf("closepath stroke %%PS\n");
#endif
                    if (log_op < 0)
                        code = dev_proc(pdev, fill_rectangle)(pdev, left, edgebuffer->base+j-1, right, 1, pdevc->colors.pure);
                    else
                        code = gx_fill_rectangle_device_rop(left, edgebuffer->base+j-1, right, 1, pdevc, pdev, (gs_logical_operation_t)log_op);
                    if (code < 0)
                        return code;
                }
            }
        }
        i = j;
    }
    return 0;
}



/* ================ END gxscanc.c ================ */

/* ---------------- gxscanc.c (bottom) ---------------- */

void
gx_edgebuffer_init(gx_edgebuffer * edgebuffer)
{
    edgebuffer->base   = 0;
    edgebuffer->height = 0;
    edgebuffer->index  = NULL;
    edgebuffer->table  = NULL;
}

void
gx_edgebuffer_fin(gx_device     * pdev,
                  gx_edgebuffer * edgebuffer)
{
    edgebuffer->table_store = std::vector<int>();
    edgebuffer->index_store = std::vector<int>();
    edgebuffer->index = NULL;
    edgebuffer->table = NULL;
}

const gx_scan_converter_t gx_scan_converter =
{
    gx_scan_convert,
    gx_filter_edgebuffer,
    gx_fill_edgebuffer
};

const gx_scan_converter_t gx_scan_converter_app =
{
    gx_scan_convert_app,
    gx_filter_edgebuffer_app,
    gx_fill_edgebuffer_app
};

const gx_scan_converter_t gx_scan_converter_tr =
{
    gx_scan_convert_tr,
    gx_filter_edgebuffer_tr,
    gx_fill_edgebuffer_tr
};

const gx_scan_converter_t gx_scan_converter_tr_app =
{
    gx_scan_convert_tr_app,
    gx_filter_edgebuffer_tr_app,
    gx_fill_edgebuffer_tr_app
};

int
gx_scan_convert_and_fill(const gx_scan_converter_t *sc,
                               gx_device       *dev,
                               gx_path         *ppath,
                         const gs_fixed_rect   *ibox,
                               fixed            flat,
                               int              rule,
                         const gx_device_color *pdevc,
                               int              lop)
{
    int code;
    gx_edgebuffer eb;
    gs_fixed_rect ibox2 = *ibox;
    int height;
    int mfb = dev->max_fill_band;

    if (mfb != 0) {
        ibox2.p.y &= ~(mfb-1);
        ibox2.q.y = (ibox2.q.y+mfb-1) & ~(mfb-1);
    }
    height = ibox2.q.y - ibox2.p.y;

    do {
        gx_edgebuffer_init(&eb);
        while (1) {
            ibox2.q.y = ibox2.p.y + height;
            if (ibox2.q.y > ibox->q.y)
                ibox2.q.y = ibox->q.y;
            code = sc->scan_convert(dev,
                                    ppath,
                                    &ibox2,
                                    &eb,
                                    flat);
            if (code <= 0)
                break;
            /* Let's shrink the ibox and try again */
            if (mfb && height == mfb) {
                /* Can't shrink the height any more! */
                code = gs_error_rangecheck;
                break;
            }
            height = height/code;
            if (mfb)
                height = (height + mfb-1) & ~(mfb-1);
            if (height < (mfb ? mfb : 1)) {
                code = gs_error_VMerror;
                break;
            }
        }
        if (code >= 0)
            code = sc->filter(dev,
                              &eb,
                              rule);
        if (code >= 0)
            code = sc->fill(dev,
                            pdevc,
                            &eb,
                            lop);
        gx_edgebuffer_fin(dev,&eb);
        ibox2.p.y += height;
    }
    while (ibox2.p.y < ibox->q.y);

    return code;
}

/* ---------------- gsrect.h ---------------- */

#define rect_within(inner, outer)\
  ((inner).q.y <= (outer).q.y && (inner).q.x <= (outer).q.x &&\
   (inner).p.y >= (outer).p.y && (inner).p.x >= (outer).p.x)

#define rect_intersect(to, from)\
  BEGIN\
    if ((from).p.x > (to).p.x) (to).p.x = (from).p.x;\
    if ((from).q.x < (to).q.x) (to).q.x = (from).q.x;\
    if ((from).p.y > (to).p.y) (to).p.y = (from).p.y;\
    if ((from).q.y < (to).q.y) (to).q.y = (from).q.y;\
  END

}  // namespace

int ScanConvertAndFill(const std::vector<Segment>& in,
                       int clip_x0,
                       int clip_y0,
                       int clip_x1,
                       int clip_y1,
                       double flatness,
                       Rule rule,
                       bool any_part,
                       const std::function<void(int x, int y, int w, int h)>& fill_rect,
                       bool clip) {
    int code = 0;

    /* The current path, built with gs's path operators (gs_moveto etc.
     * end in these). */
    gx_path path_storage = {};
    gx_path *ppath = &path_storage;
    for (const Segment& s : in) {
        switch (s.type) {
            case Segment::Type::kMove:
                code = gx_path_add_point(ppath, s.x, s.y);
                break;
            case Segment::Type::kLine:
                code = gx_path_add_line_notes(ppath, s.x, s.y, sn_none);
                break;
            case Segment::Type::kCurve:
                code = gx_path_add_curve_notes(ppath, s.c1x, s.c1y, s.c2x, s.c2y,
                                               s.x, s.y, sn_none);
                break;
            case Segment::Type::kClose:
                code = gx_path_close_subpath(ppath);
                break;
        }
        if (code < 0)
            return code;
    }

    /* gx_cpath_intersect_with_params (gxcpath.c): a clip path is
     * flattened first (gx_path_add_flattened_accurate, accurate_curves). */
    gx_path clip_path = {};
    if (clip && gx_path_has_curves(ppath)) {
        code = gx_path_copy_reducing(ppath, &clip_path, float2fixed((float)flatness),
                                     NULL, pco_accurate);
        if (code < 0)
            return code;
        ppath = &clip_path;
    }

    /* gx_fill_path (gxpaint.c): params; a normal (not cached-character)
     * fill, so params.flatness = pgs->flatness (a float). */
    const float params_flatness = (float)flatness;
    const int params_rule = rule == Rule::kEvenOdd ? gx_rule_even_odd : gx_rule_winding_number;
    const gs_logical_operation_t lop = lop_default; /* pgs->log_op */
    const bool accurate_curves = true;              /* pgs->accurate_curves */

    /* gx_general_fill_path (gxfill.c), for: no alpha buffer (max_fill_band
     * 0), a rectangular clip path, a pure device colour, an idempotent lop,
     * a non-spot-analyzer device and the default (edgebuffer) scan
     * converter. */
    gs_fixed_point adjust;
    gs_fixed_rect ibox, bbox;
    gx_path ffpath = {};
    bool fill_by_trapezoids;
    bool big_path = ppath->subpath_count > 50;
    const bool fo_is_spotan = false;
    const bool fo_fill_direct = true; /* color_writes_pure(pdevc, lop) */
    fixed fo_fixed_flat;
    const gx_scan_converter_t *sc;

    gx_path_bbox(ppath, &ibox);
    adjust.x = adjust.y = any_part ? fixed_half : 0;
    bbox.p.x = int2fixed(clip_x0);
    bbox.p.y = int2fixed(clip_y0);
    bbox.q.x = int2fixed(clip_x1);
    bbox.q.y = int2fixed(clip_y1);
    if (!rect_within(ibox, bbox)) {
        /*
         * Intersect the path box and the clip bounding box.
         * If the intersection is empty, this fill is a no-op.
         */
        rect_intersect(ibox, bbox);
        if (ibox.p.x - adjust.x >= ibox.q.x + adjust.x ||
            ibox.p.y - adjust.y >= ibox.q.y + adjust.y
            ) {                 /* Intersection of boxes is empty! */
            return 0;
        }
    }
    fo_fixed_flat = float2fixed(params_flatness);

    fill_by_trapezoids = (!gx_path_has_curves(ppath) ||
                          params_flatness >= 1.0 || fo_is_spotan);
    /* (gs's rectangle / no-double-write special cases apply only to
     * non-idempotent lops.) */

    /* If we have a request for accurate curves, make sure we exactly
     * match what we'd get for stroking. */
    if (!big_path && accurate_curves && gx_path_has_curves(ppath))
    {
        code = gx_path_copy_reducing(ppath, &ffpath, fo_fixed_flat, NULL,
                                     (gx_path_copy_options)(pco_small_curves | pco_accurate));
        if (code < 0)
            return code;
        ppath = &ffpath;
    }

    if (!fill_by_trapezoids)
    {
        if (adjust.x == 0 && adjust.y == 0)
            sc = &gx_scan_converter;
        else
            sc = &gx_scan_converter_app;
    } else {
        if (adjust.x == 0 && adjust.y == 0)
            sc = &gx_scan_converter_tr;
        else
            sc = &gx_scan_converter_tr_app;
    }

    gx_device dev = {};
    /* pdfking: gx_default_fill_trapezoid clamps to [0, height) of the
     * device; rows outside the clip box are clipped anyway, so the clip
     * box's bottom stands in for the page height. */
    dev.width = clip_x1;
    dev.height = clip_y1;
    dev.non_strict_bounds = 0;
    dev.max_fill_band = 0;
    dev.fill_rectangle = sink_fill_rectangle;
    dev.fill_trapezoid = gx_default_fill_trapezoid;
    dev.sink = &fill_rect;
    dev.clip_x0 = clip_x0;
    dev.clip_y0 = clip_y0;
    dev.clip_x1 = clip_x1;
    dev.clip_y1 = clip_y1;
    gx_device_color devc = {};

    return gx_scan_convert_and_fill(sc,
                                    &dev,
                                    ppath,
                                    &ibox,
                                    fo_fixed_flat,
                                    params_rule,
                                    &devc,
                                   (!fill_by_trapezoids && fo_fill_direct) ? -1 : (int)lop);
}


/* pdfking: gx_default_fill_parallelogram (gdevddrw.c), verbatim but for
 * the device: the sink device ScanConvertAndFill fills through. */
void FillParallelogram(fixed px, fixed py, fixed ax, fixed ay, fixed bx,
                       fixed by, int clip_x0, int clip_y0, int clip_x1,
                       int clip_y1,
                       const std::function<void(int x, int y, int w, int h)>& fill_rect) {
    gx_device dev_storage = {};
    gx_device *dev = &dev_storage;
    dev->width = clip_x1;
    dev->height = clip_y1;
    dev->fill_rectangle = sink_fill_rectangle;
    dev->fill_trapezoid = gx_default_fill_trapezoid;
    dev->sink = &fill_rect;
    dev->clip_x0 = clip_x0;
    dev->clip_y0 = clip_y0;
    dev->clip_x1 = clip_x1;
    dev->clip_y1 = clip_y1;
    gx_device_color devc = {};
    const gx_device_color *pdevc = &devc;
    const gs_logical_operation_t lop = lop_default;
#define fixed2int_pixround(x) fixed2int((x) + _fixed_pixround_v)
    fixed t;
    fixed qx, qy, ym;
    dev_proc_fill_trapezoid((*fill_trapezoid));
    gs_fixed_edge left, right;

    /* Make a special fast check for rectangles. */
    if (((ax) | (by)) == 0 || ((bx) | (ay)) == 0) {
        int px_ = fixed2int_pixround(px);
        int py_ = fixed2int_pixround(py);
        int qx_ = fixed2int_pixround((px) + (ax) + (bx));
        int qy_ = fixed2int_pixround((py) + (ay) + (by));
        int rpx, rpy, rqx, rqy;

        if (qx_ < px_)
            rpx = qx_, rqx = px_;
        else
            rpx = px_, rqx = qx_;
        if (qy_ < py_)
            rpy = qy_, rqy = py_;
        else
            rpy = py_, rqy = qy_;
        gx_fill_rectangle_device_rop(rpx, rpy, rqx - rpx, rqy - rpy, pdevc,
                                     dev, lop);
        return;
    }
#undef fixed2int_pixround
    /* Ensure ay >= 0, by >= 0. */
    if (ay < 0)
        px += ax, py += ay, ax = -ax, ay = -ay;
    if (by < 0)
        px += bx, py += by, bx = -bx, by = -by;
    qx = px + ax + bx;
    if ((ax ^ bx) < 0) {	/* In this case, the test ax <= bx is sufficient. */
        if (ax > bx)
            SWAP(ax, bx, t), SWAP(ay, by, t);
    } else {
        if ((double)ay * bx < (double)by * ax)
            SWAP(ax, bx, t), SWAP(ay, by, t);
    }
    fill_trapezoid = dev_proc(dev, fill_trapezoid);
    qy = py + ay + by;
    left.start.x = right.start.x = px;
    left.start.y = right.start.y = py;
    left.end.x = px + ax;
    left.end.y = py + ay;
    right.end.x = px + bx;
    right.end.y = py + by;
#define ROUNDED_SAME(p1, p2)\
  (fixed_pixround(p1) == fixed_pixround(p2))
    if (ay < by) {
        if (!ROUNDED_SAME(py, left.end.y))
            (*fill_trapezoid) (dev, &left, &right, py, left.end.y,
                               false, pdevc, lop);
        left.start = left.end;
        left.end.x = qx, left.end.y = qy;
        ym = right.end.y;
        if (!ROUNDED_SAME(left.start.y, ym))
            (*fill_trapezoid) (dev, &left, &right, left.start.y, ym,
                               false, pdevc, lop);
        right.start = right.end;
        right.end.x = qx, right.end.y = qy;
    } else {
        if (!ROUNDED_SAME(py, right.end.y))
            (*fill_trapezoid) (dev, &left, &right, py, right.end.y,
                               false, pdevc, lop);
        right.start = right.end;
        right.end.x = qx, right.end.y = qy;
        ym = left.end.y;
        if (!ROUNDED_SAME(right.start.y, ym))
            (*fill_trapezoid) (dev, &left, &right, right.start.y, ym,
                               false, pdevc, lop);
        left.start = left.end;
        left.end.x = qx, left.end.y = qy;
    }
    if (!ROUNDED_SAME(ym, qy))
        (*fill_trapezoid) (dev, &left, &right, ym, qy,
                           false, pdevc, lop);
#undef ROUNDED_SAME
}

}  // namespace gs_scan
