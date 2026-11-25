// pdfking: Ghostscript's smooth shading fill, ported from Ghostscript 10.08.
// See gs_shade.h.
//
// Copyright (C) 2001-2025 Artifex Software, Inc.
// Licensed under the GNU Affero General Public License (AGPL) v3; see
// https://www.gnu.org/licenses/agpl-3.0.html. This file is AGPL code, not
// under the PDFium BSD license.
//
// The blocks marked "BEGIN <file>" / "END <file>" are copied from the
// Ghostscript sources verbatim, so they can be diffed against the original;
// changes inside them are marked "pdfking:". What gs reaches outside them
// (its device, gstate, color space and function objects) is the small shim
// below, standing in for the tiffsep device behind gs's clip device: a
// DeviceN device of 8-bit components that takes colors as devn values.

#ifdef UNSAFE_BUFFERS_BUILD
// Verbatim Ghostscript C code: raw pointer arithmetic and libc calls.
#pragma allow_unsafe_buffers
#pragma allow_unsafe_libc_calls
#endif

#include "core/fxge/gs_shade.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

namespace gs_shade {

namespace {

/* ---------------- std.h / stdpre.h / gx.h ---------------- */

typedef unsigned char byte;
typedef unsigned char uchar;
typedef unsigned short ushort;
typedef unsigned int uint;
typedef unsigned long ulong;
#define min_int INT_MIN
#define max_int INT_MAX
#define arith_rshift(x, n) ((x) >> (n))
#define arith_rshift_1(x) ((x) >> 1)
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define any_abs(x) ((x) < 0 ? -(x) : (x))
#define count_of(a) (sizeof(a) / sizeof((a)[0]))
#define DO_NOTHING do {} while (0)
#define BEGIN do {
#define END } while (0)
#define is_fzero(f) ((f) == 0.0)
#define is_xxyy(pmat) (is_fzero((pmat)->xy) && is_fzero((pmat)->yx))
#define is_xyyx(pmat) (is_fzero((pmat)->xx) && is_fzero((pmat)->yy))
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
/* Debug tracing (gdebug.h) and interrupt polling compile out. */
#define if_debug0(...) DO_NOTHING
#define if_debug1(...) DO_NOTHING
#define if_debug2(...) DO_NOTHING
#define if_debug3(...) DO_NOTHING
#define if_debug4(...) DO_NOTHING
#define if_debug5(...) DO_NOTHING
#define if_debug6(...) DO_NOTHING
#define if_debug2m(...) DO_NOTHING
#define return_if_interrupt(mem) DO_NOTHING

/* ---------------- gserrors.h ---------------- */

enum {
    gs_error_unregistered = -28,
    gs_error_VMerror = -25,
    gs_error_undefinedresult = -23,
    gs_error_rangecheck = -15,
    gs_error_limitcheck = -13
};
#define gs_note_error(e) (e)
#define return_error(e) return gs_note_error(e)

/* ---------------- gxfixed.h (fixed is int, ARCH_SIZEOF_INT == 4) ---------------- */

typedef int32_t fixed;
#define max_fixed max_int
#define min_fixed min_int
#define fixed_0 0L
#define fixed_epsilon 1L
#define _fixed_shift 8
#define fixed_fraction_bits _fixed_shift
#define fixed_int_bits (sizeof(fixed) * 8 - _fixed_shift)
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
#define fixed2int_ceiling(x) ((int)_fixed_rshift((x)+_fixed_fraction_v))
#define fixed2int_pixround(x) fixed2int((x)+_fixed_pixround_v)
#define fixed_is_int(x) !((x)&_fixed_fraction_v)
#define float2fixed(f) ((fixed)((f)*(float)fixed_scale))
#define fixed2float(x) ((float)(x)*(1.0f/fixed_scale))
#define fixed_floor(x) ((x)&(-(1L<<_fixed_shift)))
#define fixed_ceiling(x) (((x)+_fixed_fraction_v)&(-(1L<<_fixed_shift)))
#define fixed_pixround(x) (((x)+_fixed_pixround_v)&(-(1L<<_fixed_shift)))
#define max_int_in_fixed fixed2int(max_fixed)
/* f_fits_in_bits(f, n): -2^(n-1) <= f < 2^(n-1) */
#define f_fits_in_bits(f, n)\
  ((f) >= -2.0 * (1L << ((n) - 2)) && (f) < 2.0 * (1L << ((n) - 2)))
#define f_fits_in_fixed(f) f_fits_in_bits(f, fixed_int_bits)
#define CHECK_SET_FIXED_SUM(r, a, b) \
     ((((a) ^ (b)) >= 0) && ((((a)+(b)) ^ (a)) < 0) ? \
       (((r)=(((a)<0) ? min_fixed : max_fixed)), gs_error_limitcheck) : \
       (((r) = ((a)+(b))), 0))		/* no overflow */
#define CHECK_FMUL2FIXED_VARS(vr, vfa, vfb, dtemp)\
  (dtemp = (vfa) * (vfb),\
   (f_fits_in_bits(dtemp, fixed_int_bits) ? 0 :\
    gs_note_error(gs_error_limitcheck)))
#define FINISH_FMUL2FIXED_VARS(vr, dtemp)\
  vr = float2fixed(dtemp)
#define CHECK_DFMUL2FIXED_VARS(vr, vda, vfb, dtemp)\
  CHECK_FMUL2FIXED_VARS(vr, vda, vfb, dtemp)
#define FINISH_DFMUL2FIXED_VARS(vr, dtemp)\
  FINISH_FMUL2FIXED_VARS(vr, dtemp)
/* gxfixed.h clamp_coord: a float coordinate to a fixed, clamped */
#define clamp_coord(xy)\
    (xy >= fixed2float(max_fixed) ? max_fixed : xy <= fixed2float(min_fixed) ? min_fixed : float2fixed(xy))

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
typedef struct gs_point_s {
    double x, y;
} gs_point;
typedef struct gs_rect_s {
    gs_point p, q;
} gs_rect;

/* ---------------- gsrect.h ---------------- */

#define rect_intersect(to, from)\
  BEGIN\
    if ((from).p.x > (to).p.x) (to).p.x = (from).p.x;\
    if ((from).q.x < (to).q.x) (to).q.x = (from).q.x;\
    if ((from).p.y > (to).p.y) (to).p.y = (from).p.y;\
    if ((from).q.y < (to).q.y) (to).q.y = (from).q.y;\
  END

/* ---------------- gsmatrix.h ---------------- */

typedef struct gs_matrix_s {
    float xx, xy, yx, yy, tx, ty;
} gs_matrix;
typedef struct gs_matrix_fixed_s {
    float xx, xy, yx, yy, tx, ty;
    fixed tx_fixed, ty_fixed;
    bool txy_fixed_valid;
} gs_matrix_fixed;

/* ---------------- gxfrac.h / gxcvalue.h ---------------- */

typedef int32_t frac31;
typedef unsigned short gx_color_value;
#define frac31_bits 31
#define ushort_bits 16
#define frac312ushort(fr) ((ushort)((fr) >> (frac31_bits - ushort_bits)))
#define ushort2frac31(us) ((frac31)((us) << (frac31_bits - ushort_bits)))
#define frac312cv(fr) frac312ushort(fr)
#define cv2frac31(cv) ushort2frac31(cv)
#define gx_max_color_value 0xffff

/* ---------------- gxarith.h / gsmisc.c ---------------- */

int
ilog2(int n)
{
    int t = 0, m = n;

    while (m > 1)
        m >>= 1, t++;
    return t;
}

/* ---------------- gsccolor.h / gxcspace.h / gsfunc.h ---------------- */

#define GS_CLIENT_COLOR_MAX_COMPONENTS 64
#define GX_DEVICE_COLOR_MAX_COMPONENTS 64
typedef struct gs_paint_color_s {
    float values[GS_CLIENT_COLOR_MAX_COMPONENTS];
} gs_paint_color;
typedef struct gs_client_color_s {
    gs_paint_color paint;
} gs_client_color;

typedef struct gs_gstate_s gs_gstate;
typedef struct gs_memory_s gs_memory_t;
/* gsmemory.h: pdfking: the heap */
#define gs_alloc_bytes(mem, nbytes, cname) ((byte *)malloc(nbytes))
#define gs_alloc_byte_array(mem, nelts, esize, cname) ((byte *)malloc((size_t)(nelts) * (esize)))
#define gs_free_object(mem, ptr, cname) free(ptr)
typedef struct gx_device_s gx_device;
typedef struct gx_device_color_s gx_device_color;
typedef struct gs_color_space_s gs_color_space;
/* pdfking: the ICC link: Device::remap stands in for its map_color. */
typedef struct gsicc_link_s {
    bool is_identity;
} gsicc_link_t;

/* pdfking: the shading's color space: its ranges (restrict_color) and how
 * the device takes its colors (remap_color, is_linear). */
typedef struct gs_color_space_type_s gs_color_space_type;
struct gs_color_space_s {
    const gs_color_space_type *type;
    const Colors *colors;
    const Device *device;
    /* an Indexed space's: its base (null for other spaces), its entries */
    const gs_color_space *base;
    const Shading *shading;
};
typedef struct gs_range_s {
    float rmin, rmax;
} gs_range;
#define gs_color_space_num_components(pcs)\
  ((pcs)->base ? 1 : (pcs)->colors->num_components)
typedef enum {
    gs_color_space_index_DeviceN = 0,
    gs_color_space_index_Indexed = 1
} gs_color_space_index;
#define gs_color_space_get_index(pcs)\
  ((pcs)->base ? gs_color_space_index_Indexed : gs_color_space_index_DeviceN)
#define gs_cspace_base_space(pcs) ((pcs)->base)
#define gs_cspace_indexed_num_entries(pcs) ((pcs)->shading->hival + 1)

/* pdfking: the shading's Function. */
typedef struct gs_function_params_s {
    float Domain[2];  /* a mesh's (1-input) Function's */
} gs_function_params_t;
typedef struct gs_function_s {
    const Colors *colors;
    gs_function_params_t params;
} gs_function_t;
#define gs_function_evaluate(pfn, in, out)\
  ((pfn)->colors->function((in), (out)), 0)
#define gs_function_is_monotonic(pfn, lower, upper, mask)\
  ((pfn)->colors->function_is_monotonic((lower), (upper), (mask)))

/* ---------------- gxdevcli.h / gxdcolor.h (just what is used) ---------------- */

typedef uint64_t gx_color_index;
typedef int gs_logical_operation_t;
typedef struct gx_device_color_type_s {
    int index;
} gx_device_color_type_t;
const gx_device_color_type_t gx_dc_type_data_pure = {0};
const gx_device_color_type_t gx_dc_type_data_devn = {1};
const gx_device_color_type_t *const gx_dc_type_devn = &gx_dc_type_data_devn;
struct gx_device_color_s {
    const gx_device_color_type_t *type;
    union {
        gx_color_index pure;
        struct {
            ushort values[GX_DEVICE_COLOR_MAX_COMPONENTS];
        } devn;
    } colors;
    int tag;
};
#define device_current_tag(dev) 0

typedef struct gs_linear_color_edge_s {
    gs_fixed_point start;
    gs_fixed_point end;
    const frac31 *c0, *c1;
    fixed clip_x;
} gs_linear_color_edge;

typedef struct patch_fill_state_s patch_fill_state_t;
typedef struct gs_fill_attributes_s {
      const gs_fixed_rect *clip;
      bool swap_axes;
      const void *ht; /* Reserved for possible use in future. */
      gs_logical_operation_t lop; /* Reserved for possible use in future. */
      fixed ystart, yend; /* Only for fill_linear_color_trapezoid. */
      patch_fill_state_t *pfs; /* For gx_fill_triangle_small. Clients must not change. */
} gs_fill_attributes;

typedef struct gx_device_color_info_s {
    uchar num_components;
    uint max_gray;
    uint max_color;
    uchar comp_bits[GX_DEVICE_COLOR_MAX_COMPONENTS];
    uchar comp_shift[GX_DEVICE_COLOR_MAX_COMPONENTS];
    uchar gray_index;
} gx_device_color_info;
#define GX_CINFO_COMP_NO_INDEX 0xff

#define dev_proc_fill_rectangle(proc)\
  int proc(gx_device *dev, int x, int y, int width, int height, gx_color_index color)
#define dev_proc_fill_trapezoid(proc)\
  int proc(gx_device *dev, const gs_fixed_edge *left, const gs_fixed_edge *right,\
    fixed ybot, fixed ytop, bool swap_axes, const gx_device_color *pdcolor,\
    gs_logical_operation_t lop)
#define dev_proc_fill_linear_color_scanline(proc)\
  int proc(gx_device *dev, const gs_fill_attributes *fa,\
        int i, int j, int w, /* scanline coordinates and width */\
        const frac31 *c0, /* initial color for the pixel (i,j), the integer part */\
        const int32_t *c0_f, /* initial color for the pixel (i,j), the fraction part numerator */\
        const int32_t *cg_num, /* color gradient numerator */\
        int32_t cg_den /* color gradient denominator */)
#define dev_proc_fill_linear_color_trapezoid(proc)\
  int proc(gx_device *dev, const gs_fill_attributes *fa,\
        const gs_fixed_point *p0, const gs_fixed_point *p1,\
        const gs_fixed_point *p2, const gs_fixed_point *p3,\
        const frac31 *c0, const frac31 *c1,\
        const frac31 *c2, const frac31 *c3)
#define dev_proc_fill_linear_color_triangle(proc)\
  int proc(gx_device *dev, const gs_fill_attributes *fa,\
        const gs_fixed_point *p0, const gs_fixed_point *p1,\
        const gs_fixed_point *p2,\
        const frac31 *c0, const frac31 *c1, const frac31 *c2)
#define dev_proc_fill_rectangle_hl_color(proc)\
  int proc(gx_device *dev, const gs_fixed_rect *rect,\
        const gs_gstate *pgs, const gx_device_color *pdcolor,\
        const void *pcpath)

/* pdfking: the clip device over tiffsep, as the shading paints through it:
 * its fill procs are gs's defaults (gxclip.c sets them so), its
 * fill_rectangle_hl_color hands rectangles to Device::fill. */
struct gx_device_s {
    gx_device_color_info color_info;
    int width;
    int height;
    int non_strict_bounds;
    int graphics_type_tag;
    dev_proc_fill_rectangle((*fill_rectangle));  /* pure colors: none here */
    dev_proc_fill_trapezoid((*fill_trapezoid));
    dev_proc_fill_linear_color_scanline((*fill_linear_color_scanline));
    dev_proc_fill_linear_color_trapezoid((*fill_linear_color_trapezoid));
    dev_proc_fill_linear_color_triangle((*fill_linear_color_triangle));
    dev_proc_fill_rectangle_hl_color((*fill_rectangle_hl_color));
    const Device *sink;
    const gs_color_space *space;  /* pdfking: the shading's, for gsicc_remap_fast */
};
#define dev_proc(dev, p) ((dev)->p)

/* pdfking: the gstate the shading reads. */
struct gs_gstate_s {
    gs_matrix_fixed ctm;
    float smoothness;
    float flatness;
    gs_logical_operation_t log_op;
    gs_memory_t *memory;
};
#define ctm_only(pgs) (*(const gs_matrix *)&(pgs)->ctm)

/* gx_device_color's fill_rectangle for a devn color, through the clip
 * device to the target (gx_dc_devn_fill_rectangle). */
static int
gx_fill_rectangle_device_rop(int x, int y, int w, int h,
                             const gx_device_color *pdevc, gx_device *dev,
                             gs_logical_operation_t lop)
{
    gs_fixed_rect rect;

    rect.p.x = int2fixed(x);
    rect.p.y = int2fixed(y);
    rect.q.x = int2fixed(x + w);
    rect.q.y = int2fixed(y + h);
    return dev_proc(dev, fill_rectangle_hl_color)(dev, &rect, NULL, pdevc, NULL);
}

/* A devn color never writes pure. */
#define color_writes_pure(pdevc, lop) false

static int
sink_fill_rectangle_hl_color(gx_device *dev, const gs_fixed_rect *rect,
        const gs_gstate *pgs, const gx_device_color *pdcolor,
        const void *pcpath)
{
    int x = fixed2int(rect->p.x), y = fixed2int(rect->p.y);
    int w = fixed2int(rect->q.x) - x, h = fixed2int(rect->q.y) - y;

    if (w > 0 && h > 0)
        dev->sink->fill(x, y, w, h, pdcolor->colors.devn.values);
    return 0;
}

/* ---------------- gxcspace.h: the color space type's procs used ---------------- */

typedef enum {
    gs_color_select_all = -1,
    gs_color_select_texture = 0,
    gs_color_select_source = 1
} gs_color_select_t;
#define cs_proc_is_linear(proc)\
  int proc(const gs_color_space *cs, const gs_gstate * pgs,\
                gx_device *dev,\
                const gs_client_color *c0, const gs_client_color *c1,\
                const gs_client_color *c2, const gs_client_color *c3,\
                float smoothness, gsicc_link_t *icclink)
#define cs_is_linear(pcs, pgs, dev, c0, c1, c2, c3, smoothness, icclink)\
  (*(pcs)->type->is_linear)(pcs, pgs, dev, c0, c1, c2, c3, smoothness, icclink)
struct gs_color_space_type_s {
    int (*num_components)(const gs_color_space *);
    void (*restrict_color)(gs_client_color *, const gs_color_space *);
    int (*remap_color)(const gs_client_color *, const gs_color_space *,
                       gx_device_color *, const gs_gstate *, gx_device *,
                       gs_color_select_t);
    cs_proc_is_linear((*is_linear));
};

static int
shade_num_components(const gs_color_space * pcs)
{
    return gs_color_space_num_components(pcs);
}

/* pdfking: every gs color space's restrict_color: each component clamped
 * to its range */
static void
shade_restrict_color(gs_client_color * pcc, const gs_color_space * pcs)
{
    int i;

    if (pcs->base) {
        /* gscolor2.c gx_restrict_Indexed */
        float value = pcc->paint.values[0];
        int hival = pcs->shading->hival;

        pcc->paint.values[0] =
            (value < 0 ? 0 : value >= hival ? hival : value);
        return;
    }

    for (i = 0; i < pcs->colors->num_components; ++i) {
        float v = pcc->paint.values[i];

        if (v <= pcs->colors->range_min[i])
            pcc->paint.values[i] = pcs->colors->range_min[i];
        else if (v >= pcs->colors->range_max[i])
            pcc->paint.values[i] = pcs->colors->range_max[i];
    }
}

/* pdfking: remap_color to the devn color tiffsep takes: Device::remap */
static int
shade_remap_color(const gs_client_color * pcc, const gs_color_space * pcs,
                  gx_device_color * pdc, const gs_gstate * pgs,
                  gx_device * dev, gs_color_select_t select)
{
    int i;

    for (i = 0; i < GX_DEVICE_COLOR_MAX_COMPONENTS; i++)
        pdc->colors.devn.values[i] = 0;
    pcs->device->remap(pcc->paint.values, pdc->colors.devn.values);
    pdc->type = &gx_dc_type_data_devn;
    return 0;
}

/* gxdevcli.h: tiffsep is separable and linear, contone */
#define colors_are_separable_and_linear(pci) true
#define gx_device_must_halftone(dev) false

/* ---------------- gsmisc.c: fixed_mult_quo, double variant (selected for
 * 32-bit fixed) ---------------- */

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


/* ======== BEGIN base/gdevddrw.c lines 37-320 ======== */
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
 * The linear color trapezoid fill algorithm uses trap_color structures to keep track of
 * the color change during the Bresenham loop.
 */
typedef struct trap_gradient_s {
        frac31 *c; /* integer part of the color in frac32 units. */
        int32_t *f; /* the fraction part numerator */
        int32_t *num; /* the gradient numerator */
        int32_t den; /* color gradient denominator */
} trap_gradient;

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

static inline int
init_gradient(trap_gradient *g, const gs_fill_attributes *fa,
                const gs_linear_color_edge *e, const gs_linear_color_edge *e1,
                const trap_line *l, fixed ybot, int num_components)
{
    int i;
    int64_t c;
    int32_t d;

    if (e->c1 == NULL || e->c0 == NULL)
        g->den = 0; /* A wedge - the color is axial along another edge. */
    else {
        bool ends_from_fa = (e1->c1 == NULL || e1->c0 == NULL);

        if (ends_from_fa)
            g->den = fa->yend - fa->ystart;
        else {
            g->den = e->end.y - e->start.y;
            if (g->den != l->h)
                return_error(gs_error_unregistered); /* Must not happen. */
        }
        for (i = 0; i < num_components; i++) {
            g->num[i] = e->c1[i] - e->c0[i];
            c = (int64_t)g->num[i] * (uint32_t)(ybot -
                    (ends_from_fa ? fa->ystart : e->start.y));
            d = (int32_t)(c / g->den);
            g->c[i] = e->c0[i] + d;
            c -= (int64_t)d * g->den;
            if (c < 0) {
                g->c[i]--;
                c += g->den;
            }
            g->f[i] = (int32_t)c;
        }
    }
    return 0;
}

static inline void
step_gradient(trap_gradient *g, int num_components)
{
    int i;

    if (g->den == 0)
        return;
    for (i = 0; i < num_components; i++) {
        int64_t fc = g->f[i] + (int64_t)g->num[i] * fixed_1;
        int32_t fc32;

        g->c[i] += (int32_t)(fc / g->den);
        fc32 = (int32_t)(fc -  fc / g->den * g->den);
        if (fc32 < 0) {
            fc32 += g->den;
            g->c[i]--;
        }
        g->f[i] = fc32;
    }
}

static inline bool
check_gradient_overflow(const gs_linear_color_edge *le, const gs_linear_color_edge *re)
{
    if (le->c1 == NULL || re->c1 == NULL) {
        /* A wedge doesn't use a gradient by X. */
        return false;
    } else {
        /* Check whether set_x_gradient, fill_linear_color_scanline can overflow.

           dev_proc(dev, fill_linear_color_scanline) can perform its computation in 32-bit fractions,
           so we assume it never overflows. Devices which implement it with no this
           assumption must implement the check in gx_default_fill_linear_color_trapezoid,
           gx_default_fill_linear_color_triangle with a function other than this one.

           Since set_x_gradient perform computations in int64_t, which provides 63 bits
           while multiplying a 32-bits color value to a coordinate,
           we must restrict the X span with 63 - 32 = 31 bits.
         */
        int32_t xl = min(le->start.x, le->end.x);
        int32_t xr = min(re->start.x, re->end.x);
        /* The pixel span boundaries : */
        return arith_rshift_1(xr) - arith_rshift_1(xl) >= 0x3FFFFFFE;
    }
}

static inline int
set_x_gradient_nowedge(trap_gradient *xg, const trap_gradient *lg, const trap_gradient *rg,
             const trap_line *l, const trap_line *r, int il, int ir, int num_components)
{
    /* Ignoring the ending coordinats fractions,
       so the gridient is slightly shifted to the left (in <1 'fixed' unit). */
    int32_t xl = l->x - (l->xf == -l->h ? 1 : 0) - fixed_half; /* Revert the GX_FILL_TRAPEZOID shift. */
    int32_t xr = r->x - (r->xf == -r->h ? 1 : 0) - fixed_half; /* Revert the GX_FILL_TRAPEZOID shift. */
    /* The pixel span boundaries : */
    int32_t x0 = int2fixed(il) + fixed_half; /* Shift to the pixel center. */
    int32_t x1 = int2fixed(ir) - fixed_half; /* The center of the last pixel to paint. */
    int i;

#   ifdef DEBUG
        if (arith_rshift_1(xr) - arith_rshift_1(xl) >= 0x3FFFFFFE) /* Can overflow ? */
            return_error(gs_error_unregistered); /* Must not happen. */
#   endif
    /* We cannot compute the color of the 'ir' pixel
       because it can overflow 'c1' due to the pixel ir center
       may be greater that r->x .
       Therefore we base the proportion on the pixel index ir-1 (see comment to 'x1').
       Debugged with CET 12-14O.PS SpecialTestJ02Test12.
     */
    xg->den = fixed2int(x1 - x0);
    if (xg->den <= 0) {
        /* The span contains a single pixel, will construct a degenerate gradient. */
        xg->den = 1; /* Safety (against zerodivide). */
    }
    for (i = 0; i < num_components; i++) {
        /* Ignoring the ending colors fractions,
           so the color gets a slightly smaller value
           (in <1 'frac31' unit), but it's not important due to
           the further conversion to [0, 1 << cinfo->comp_bits[j]],
           which drops the fraction anyway. */
        int32_t cl = lg->c[i];
        int32_t cr = rg->c[i];
        int32_t c0 = (int32_t)(cl + ((int64_t)cr - cl) * (x0 - xl) / (xr - xl));
        int32_t c1 = (int32_t)(cl + ((int64_t)cr - cl) * (x1 - xl) / (xr - xl));

        xg->c[i] = c0;
        xg->f[i] = 0; /* Insufficient bits to compute it better.
                         The color so the color gets a slightly smaller value
                         (in <1 'frac31' unit), but it's not important due to
                         the further conversion to [0, 1 << cinfo->comp_bits[j]],
                         which drops the fraction anyway.
                         So setting 0 appears pretty good and fast. */
        xg->num[i] = c1 - c0;
    }
    return 0;
}

static inline int
set_x_gradient(trap_gradient *xg, const trap_gradient *lg, const trap_gradient *rg,
             const trap_line *l, const trap_line *r, int il, int ir, int num_components)
{
    if (lg->den == 0 || rg->den == 0) {
        /* A wedge doesn't use a gradient by X. */
        int i;

        xg->den = 1;
        for (i = 0; i < num_components; i++) {
            xg->c[i] = (lg->den == 0 ? rg->c[i] : lg->c[i]);
            xg->f[i] = 0; /* Compatible to set_x_gradient_nowedge. */
            xg->num[i] = 0;
        }
        return 0;
    } else
        return set_x_gradient_nowedge(xg, lg, rg, l, r, il, ir, num_components);
}

/*
 * Fill a trapezoid.
 * Since we need several statically defined variants of this algorithm,
 * we stored it in gxdtfill.h and include it configuring with
 * macros defined here.
 */
/* ======== END base/gdevddrw.c ======== */

/* ---------------- gdevddrw.c: the gxdtfill.h instances the shading uses
 * (gs_shade_dtfill.inc is base/gxdtfill.h): pdfking: all but the
 * CONTIGUOUS_FILL ones; "register" is dropped (removed in C++17).
 * ---------------- */

#define register
#define LINEAR_COLOR 0 /* Common for shading variants. */
#define EDGE_TYPE gs_fixed_edge  /* Common for non-shading variants. */
#define FILL_ATTRS gs_logical_operation_t  /* Common for non-shading variants. */

#define GX_FILL_TRAPEZOID static int gx_fill_trapezoid_as_fd
#define CONTIGUOUS_FILL 0
#define SWAP_AXES 1
#define FILL_DIRECT 1
#include "core/fxge/gs_shade_dtfill.inc"
#undef GX_FILL_TRAPEZOID
#undef CONTIGUOUS_FILL
#undef SWAP_AXES
#undef FILL_DIRECT

#define GX_FILL_TRAPEZOID static int gx_fill_trapezoid_as_nd
#define CONTIGUOUS_FILL 0
#define SWAP_AXES 1
#define FILL_DIRECT 0
#include "core/fxge/gs_shade_dtfill.inc"
#undef GX_FILL_TRAPEZOID
#undef CONTIGUOUS_FILL
#undef SWAP_AXES
#undef FILL_DIRECT

#define GX_FILL_TRAPEZOID static int gx_fill_trapezoid_ns_fd
#define CONTIGUOUS_FILL 0
#define SWAP_AXES 0
#define FILL_DIRECT 1
#include "core/fxge/gs_shade_dtfill.inc"
#undef GX_FILL_TRAPEZOID
#undef CONTIGUOUS_FILL
#undef SWAP_AXES
#undef FILL_DIRECT

#define GX_FILL_TRAPEZOID static int gx_fill_trapezoid_ns_nd
#define CONTIGUOUS_FILL 0
#define SWAP_AXES 0
#define FILL_DIRECT 0
#include "core/fxge/gs_shade_dtfill.inc"
#undef GX_FILL_TRAPEZOID
#undef CONTIGUOUS_FILL
#undef SWAP_AXES
#undef FILL_DIRECT

#undef EDGE_TYPE
#undef LINEAR_COLOR
#undef FILL_ATTRS

#define LINEAR_COLOR 1 /* Common for shading variants. */
#define EDGE_TYPE gs_linear_color_edge /* Common for shading variants. */
#define FILL_ATTRS const gs_fill_attributes *  /* Common for non-shading variants. */

#define GX_FILL_TRAPEZOID static int gx_fill_trapezoid_ns_lc
#define CONTIGUOUS_FILL 0
#define SWAP_AXES 0
#define FILL_DIRECT 1
#include "core/fxge/gs_shade_dtfill.inc"
#undef GX_FILL_TRAPEZOID
#undef CONTIGUOUS_FILL
#undef SWAP_AXES
#undef FILL_DIRECT

#define GX_FILL_TRAPEZOID static int gx_fill_trapezoid_as_lc
#define CONTIGUOUS_FILL 0
#define SWAP_AXES 1
#define FILL_DIRECT 1
#include "core/fxge/gs_shade_dtfill.inc"
#undef GX_FILL_TRAPEZOID
#undef CONTIGUOUS_FILL
#undef SWAP_AXES
#undef FILL_DIRECT

#undef EDGE_TYPE
#undef LINEAR_COLOR
#undef FILL_ATTRS
#undef register

/* ======== BEGIN base/gdevddrw.c lines 416-604 ======== */

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

static inline int
fill_linear_color_trapezoid_nocheck(gx_device *dev, const gs_fill_attributes *fa,
        const gs_linear_color_edge *le, const gs_linear_color_edge *re)
{
    fixed y02 = max(le->start.y, re->start.y), ymin = max(y02, fa->clip->p.y);
    fixed y13 = min(le->end.y, re->end.y), ymax = min(y13, fa->clip->q.y);
    int code;

    code = (fa->swap_axes ? gx_fill_trapezoid_as_lc : gx_fill_trapezoid_ns_lc)(dev,
            le, re, ymin, ymax, 0, NULL, fa);
    if (code < 0)
        return code;
    return !code;
}

/*  Fill a trapezoid with a linear color.
    [p0 : p1] - left edge, from bottom to top.
    [p2 : p3] - right edge, from bottom to top.
    The filled area is within Y-spans of both edges.

    This implemetation actually handles a bilinear color,
    in which the generatrix keeps a parallelizm to the X axis.
    In general a bilinear function doesn't keep the generatrix parallelizm,
    so the caller must decompose/approximate such functions.

    Return values :
    1 - success;
    0 - Too big. The area isn't filled. The client must decompose the area.
    <0 - error.
 */
int
gx_default_fill_linear_color_trapezoid(gx_device *dev, const gs_fill_attributes *fa,
        const gs_fixed_point *p0, const gs_fixed_point *p1,
        const gs_fixed_point *p2, const gs_fixed_point *p3,
        const frac31 *c0, const frac31 *c1,
        const frac31 *c2, const frac31 *c3)
{
    gs_linear_color_edge le, re;

    le.start = *p0;
    le.end = *p1;
    le.c0 = c0;
    le.c1 = c1;
    le.clip_x = fa->clip->p.x;
    re.start = *p2;
    re.end = *p3;
    re.c0 = c2;
    re.c1 = c3;
    re.clip_x = fa->clip->q.x;
    if (check_gradient_overflow(&le, &re))
        return 0;
    return fill_linear_color_trapezoid_nocheck(dev, fa, &le, &re);
}

static inline int
fill_linear_color_triangle(gx_device *dev, const gs_fill_attributes *fa,
        const gs_fixed_point *p0, const gs_fixed_point *p1,
        const gs_fixed_point *p2,
        const frac31 *c0, const frac31 *c1, const frac31 *c2)
{   /* p0 must be the lowest vertex. */
    int code;
    gs_linear_color_edge e0, e1, e2;

    if (p0->y == p1->y)
        return gx_default_fill_linear_color_trapezoid(dev, fa, p0, p2, p1, p2, c0, c2, c1, c2);
    if (p1->y == p2->y)
        return gx_default_fill_linear_color_trapezoid(dev, fa, p0, p2, p0, p1, c0, c2, c0, c1);
    e0.start = *p0;
    e0.end = *p2;
    e0.c0 = c0;
    e0.c1 = c2;
    e0.clip_x = fa->clip->p.x;
    e1.start = *p0;
    e1.end = *p1;
    e1.c0 = c0;
    e1.c1 = c1;
    e1.clip_x = fa->clip->q.x;
    if (p0->y < p1->y && p1->y < p2->y) {
        e2.start = *p1;
        e2.end = *p2;
        e2.c0 = c1;
        e2.c1 = c2;
        e2.clip_x = fa->clip->q.x;
        if (check_gradient_overflow(&e0, &e1))
            return 0;
        if (check_gradient_overflow(&e0, &e2))
            return 0;
        code = fill_linear_color_trapezoid_nocheck(dev, fa, &e0, &e1);
        if (code <= 0) /* Sic! */
            return code;
        return fill_linear_color_trapezoid_nocheck(dev, fa, &e0, &e2);
    } else { /* p0->y < p2->y && p2->y < p1->y */
        e2.start = *p2;
        e2.end = *p1;
        e2.c0 = c2;
        e2.c1 = c1;
        e2.clip_x = fa->clip->q.x;
        if (check_gradient_overflow(&e0, &e1))
            return 0;
        if (check_gradient_overflow(&e2, &e1))
            return 0;
        code = fill_linear_color_trapezoid_nocheck(dev, fa, &e0, &e1);
        if (code <= 0) /* Sic! */
            return code;
        return fill_linear_color_trapezoid_nocheck(dev, fa, &e2, &e1);
    }
}

/*  Fill a triangle with a linear color. */
int
gx_default_fill_linear_color_triangle(gx_device *dev, const gs_fill_attributes *fa,
        const gs_fixed_point *p0, const gs_fixed_point *p1,
        const gs_fixed_point *p2,
        const frac31 *c0, const frac31 *c1, const frac31 *c2)
{
    fixed dx1 = p1->x - p0->x, dy1 = p1->y - p0->y;
    fixed dx2 = p2->x - p0->x, dy2 = p2->y - p0->y;

    if ((int64_t)dx1 * dy2 < (int64_t)dx2 * dy1) {
        const gs_fixed_point *p = p1;
        const frac31 *c = c1;

        p1 = p2;
        p2 = p;
        c1 = c2;
        c2 = c;
    }
    if (p0->y <= p1->y && p0->y <= p2->y)
        return fill_linear_color_triangle(dev, fa, p0, p1, p2, c0, c1, c2);
    if (p1->y <= p0->y && p1->y <= p2->y)
        return fill_linear_color_triangle(dev, fa, p1, p2, p0, c1, c2, c0);
    else
        return fill_linear_color_triangle(dev, fa, p2, p0, p1, c2, c0, c1);
}

/* Fill a parallelogram whose points are p, p+a, p+b, and p+a+b. */
/* We should swap axes to get best accuracy, but we don't. */
/* We must be very careful to follow the center-of-pixel rule in all cases. */
/* ======== END base/gdevddrw.c ======== */

/* ======== BEGIN base/gdevdsha.c lines 24-184 ======== */
static bool
gx_devn_diff(frac31 devn1[], frac31 devn2[], int num)
{
    int k;

    for (k = 0; k < num; k++) {
        if (devn1[k] != devn2[k]) {
            return true;
        }
    }
    return false;
}

int
gx_hl_fill_linear_color_scanline(gx_device *dev, const gs_fill_attributes *fa,
        int i0, int j, int w, const frac31 *c0, const int32_t *c0f,
        const int32_t *cg_num, int32_t cg_den)
{
    frac31 c[GX_DEVICE_COLOR_MAX_COMPONENTS];
    frac31 curr[GX_DEVICE_COLOR_MAX_COMPONENTS];
    ulong f[GX_DEVICE_COLOR_MAX_COMPONENTS];
    int i, i1 = i0 + w, bi = i0, k;
    const gx_device_color_info *cinfo = &dev->color_info;
    int n = cinfo->num_components;
    int si, ei, di, code;
    gs_fixed_rect rect;
    gx_device_color devc;

    /* Note: All the stepping math is done with frac color values */

    devc.type = gx_dc_type_devn;
    for (i = n; i < GX_DEVICE_COLOR_MAX_COMPONENTS; i++)
         devc.colors.devn.values[i] = 0;

    if (j < fixed2int(fa->clip->p.y) ||
            j > fixed2int_ceiling(fa->clip->q.y)) /* Must be compatible to the clipping logic. */
        return 0;
    for (k = 0; k < n; k++) {
        curr[k] = c[k] = c0[k];
        f[k] = c0f[k];
    }
    for (i = i0 + 1, di = 1; i < i1; i += di) {
        if (di == 1) {
            /* Advance colors by 1 pixel. */
            for (k = 0; k < n; k++) {
                if (cg_num[k]) {
                    int32_t m = f[k] + cg_num[k];

                    c[k] += m / cg_den;
                    m -= m / cg_den * cg_den;
                    if (m < 0) {
                        c[k]--;
                        m += cg_den;
                    }
                    f[k] = m;
                }
            }
        } else {
            /* Advance colors by di pixels. */
            for (k = 0; k < n; k++) {
                if (cg_num[k]) {
                    int64_t M = f[k] + (int64_t)cg_num[k] * di;
                    int32_t m;

                    c[k] += (frac31)(M / cg_den);
                    m = (int32_t)(M - M / cg_den * cg_den);
                    if (m < 0) {
                        c[k]--;
                        m += cg_den;
                    }
                    f[k] = m;
                }
            }
        }
        if (gx_devn_diff(c, curr, n)) {
            si = max(bi, fixed2int(fa->clip->p.x));	    /* Must be compatible to the clipping logic. */
            ei = min(i, fixed2int_ceiling(fa->clip->q.x));  /* Must be compatible to the clipping logic. */
            if (si < ei) {
                if (fa->swap_axes) {
                    rect.p.x = int2fixed(j);
                    rect.p.y = int2fixed(si);
                    rect.q.x = int2fixed(j + 1);
                    rect.q.y = int2fixed(ei);
                } else {
                    rect.p.x = int2fixed(si);
                    rect.p.y = int2fixed(j);
                    rect.q.x = int2fixed(ei);
                    rect.q.y = int2fixed(j + 1);
                }
                for (k = 0; k < n; k++) {
                    devc.colors.devn.values[k] = frac312cv(curr[k]);
                }
                devc.tag = device_current_tag(dev);
                code = dev_proc(dev, fill_rectangle_hl_color) (dev, &rect, NULL, &devc, NULL);
                if (code < 0)
                    return code;
            }
            bi = i;
            for (k = 0; k < n; k++) {
                curr[k] = c[k];
            }
            di = 1;
        } else if (i == i1) {
            i++;
            break;
        } else {
            /* Compute a color change pixel analytically. */
            di = i1 - i;
            for (k = 0; k < n; k++) {
                int32_t a;
                int64_t x;
                frac31 v = 1 << (31 - cinfo->comp_bits[k]); /* Color index precision in frac31. */
                frac31 u = c[k] & (v - 1);

                if (cg_num[k] == 0) {
                    /* No change. */
                    continue;
                } if (cg_num[k] > 0) {
                    /* Solve[(f[k] + cg_num[k]*x)/cg_den == v - u, x]  */
                    a = v - u;
                } else {
                    /* Solve[(f[k] + cg_num[k]*x)/cg_den == - u - 1, x]  */
                    a = -u - 1;
                }
                x = ((int64_t)a * cg_den - f[k]) / cg_num[k];
                if (i + x >= i1)
                    continue;
                else if (x < 0)
                    return_error(gs_error_unregistered); /* Must not happen. */
                else if (di > (int)x) {
                    di = (int)x;
                    if (di <= 1) {
                        di = 1;
                        break;
                    }
                }
            }
        }
    }
    si = max(bi, fixed2int(fa->clip->p.x));	    /* Must be compatible to the clipping logic. */
    ei = min(i, fixed2int_ceiling(fa->clip->q.x));  /* Must be compatible to the clipping logic. */
    if (si < ei) {
        if (fa->swap_axes) {
            rect.p.x = int2fixed(j);
            rect.p.y = int2fixed(si);
            rect.q.x = int2fixed(j + 1);
            rect.q.y = int2fixed(ei);
        } else {
            rect.p.x = int2fixed(si);
            rect.p.y = int2fixed(j);
            rect.q.x = int2fixed(ei);
            rect.q.y = int2fixed(j + 1);
        }
        for (k = 0; k < n; k++) {
            devc.colors.devn.values[k] = frac312cv(curr[k]);
        }
        devc.tag = device_current_tag(dev);
        return dev_proc(dev, fill_rectangle_hl_color) (dev, &rect, NULL, &devc, NULL);
    }
    return 0;
}
/* ======== END base/gdevdsha.c ======== */

/* ======== BEGIN base/gsmatrix.c lines 75-114 ======== */
/* ------ Matrix arithmetic ------ */

/* Multiply two matrices.  We should check for floating exceptions, */
/* but for the moment it's just too awkward. */
/* Since this is used heavily, we check for shortcuts. */
int
gs_matrix_multiply(const gs_matrix * pm1, const gs_matrix * pm2, gs_matrix * pmr)
{
    double xx1 = pm1->xx, yy1 = pm1->yy;
    double tx1 = pm1->tx, ty1 = pm1->ty;
    double xx2 = pm2->xx, yy2 = pm2->yy;
    double xy2 = pm2->xy, yx2 = pm2->yx;

    if (is_xxyy(pm1)) {
        pmr->tx = tx1 * xx2 + pm2->tx;
        pmr->ty = ty1 * yy2 + pm2->ty;
        if (is_fzero(xy2))
            pmr->xy = 0;
        else
            pmr->xy = xx1 * xy2,
                pmr->ty += tx1 * xy2;
        pmr->xx = xx1 * xx2;
        if (is_fzero(yx2))
            pmr->yx = 0;
        else
            pmr->yx = yy1 * yx2,
                pmr->tx += ty1 * yx2;
        pmr->yy = yy1 * yy2;
    } else {
        double xy1 = pm1->xy, yx1 = pm1->yx;

        pmr->xx = xx1 * xx2 + xy1 * yx2;
        pmr->xy = xx1 * xy2 + xy1 * yy2;
        pmr->yy = yx1 * xy2 + yy1 * yy2;
        pmr->yx = yx1 * xx2 + yy1 * yx2;
        pmr->tx = tx1 * xx2 + ty1 * yx2 + pm2->tx;
        pmr->ty = tx1 * xy2 + ty1 * yy2 + pm2->ty;
    }
    return 0;
}
/* ======== END base/gsmatrix.c ======== */

/* ======== BEGIN base/gsmatrix.c lines 151-190 ======== */
/* Invert a matrix.  Return gs_error_undefinedresult if not invertible. */
int
gs_matrix_invert(const gs_matrix * pm, gs_matrix * pmr)
{				/* We have to be careful about fetch/store order, */
    /* because pm might be the same as pmr. */
    if (is_xxyy(pm)) {
        if (is_fzero(pm->xx) || is_fzero(pm->yy))
            return_error(gs_error_undefinedresult);
        pmr->tx = -(pmr->xx = 1.0 / pm->xx) * pm->tx;
        pmr->xy = 0.0;
        pmr->yx = 0.0;
        pmr->ty = -(pmr->yy = 1.0 / pm->yy) * pm->ty;
    } else {
        float mxx = pm->xx, myy = pm->yy, mxy = pm->xy, myx = pm->yx;
        float mtx = pm->tx, mty = pm->ty;
        /* we declare det as double since on at least some computer (i.e. peeves)
           declaring it as a float results in different values for pmr depending
           on whether or not optimization is turned on.  I believe this is caused
           by the compiler keeping the det value in an internal register when
           optimization is enable.  As evidence of this if you add a debugging
           statement to print out det the optimized code acts the same as the
           unoptimized code.  declearing det as double does not change the CET 10-09.ps
           output. */
        double det = (float)(mxx * myy) - (float)(mxy * myx);

        /*
         * We are doing the math as floats instead of doubles to reproduce
         * the results in page 1 of CET 10-09.ps
         */
        if (det == 0)
            return_error(gs_error_undefinedresult);
        pmr->xx = myy / det;
        pmr->xy = -mxy / det;
        pmr->yx = -myx / det;
        pmr->yy = mxx / det;
        pmr->tx = (((float)(mty * myx) - (float)(mtx * myy))) / det;
        pmr->ty = (((float)(mtx * mxy) - (float)(mty * mxx))) / det;
    }
    return 0;
}
/* ======== END base/gsmatrix.c ======== */

/* ======== BEGIN base/gsmatrix.c lines 275-338 ======== */

/* Note that all the transformation routines take separate */
/* x and y arguments, but return their result in a point. */

/* Transform a point. */
int
gs_point_transform(double x, double y, const gs_matrix * pmat,
                   gs_point * ppt)
{
    /*
     * The float casts are there to reproduce results in CET 10-01.ps
     * page 4.
     */
    ppt->x = (float)(x * pmat->xx) + pmat->tx;
    ppt->y = (float)(y * pmat->yy) + pmat->ty;
    if (!is_fzero(pmat->yx))
        ppt->x += (float)(y * pmat->yx);
    if (!is_fzero(pmat->xy))
        ppt->y += (float)(x * pmat->xy);
    return 0;
}

/* Inverse-transform a point. */
/* Return gs_error_undefinedresult if the matrix is not invertible. */
int
gs_point_transform_inverse(double x, double y, const gs_matrix * pmat,
                           gs_point * ppt)
{
    if (is_xxyy(pmat)) {
        if (is_fzero(pmat->xx) || is_fzero(pmat->yy))
            return_error(gs_error_undefinedresult);
        ppt->x = (x - pmat->tx) / pmat->xx;
        ppt->y = (y - pmat->ty) / pmat->yy;
        return 0;
    } else if (is_xyyx(pmat)) {
        if (is_fzero(pmat->xy) || is_fzero(pmat->yx))
            return_error(gs_error_undefinedresult);
        ppt->x = (y - pmat->ty) / pmat->xy;
        ppt->y = (x - pmat->tx) / pmat->yx;
        return 0;
    } else {			/* There are faster ways to do this, */
        /* but we won't implement one unless we have to. */
        gs_matrix imat;
        int code = gs_matrix_invert(pmat, &imat);

        if (code < 0)
            return code;
        return gs_point_transform(x, y, &imat, ppt);
    }
}

/* Transform a distance. */
int
gs_distance_transform(double dx, double dy, const gs_matrix * pmat,
                      gs_point * pdpt)
{
    pdpt->x = dx * pmat->xx;
    pdpt->y = dy * pmat->yy;
    if (!is_fzero(pmat->yx))
        pdpt->x += dy * pmat->yx;
    if (!is_fzero(pmat->xy))
        pdpt->y += dx * pmat->xy;
    return 0;
}
/* ======== END base/gsmatrix.c ======== */

/* ======== BEGIN base/gsmatrix.c lines 366-450 ======== */

/* Compute the bounding box of 4 points. */
int
gs_points_bbox(const gs_point pts[4], gs_rect * pbox)
{
#define assign_min_max(vmin, vmax, v0, v1)\
  if ( v0 < v1 ) vmin = v0, vmax = v1; else vmin = v1, vmax = v0
#define assign_min_max_4(vmin, vmax, v0, v1, v2, v3)\
  { double min01, max01, min23, max23;\
    assign_min_max(min01, max01, v0, v1);\
    assign_min_max(min23, max23, v2, v3);\
    vmin = min(min01, min23);\
    vmax = max(max01, max23);\
  }
    assign_min_max_4(pbox->p.x, pbox->q.x,
                     pts[0].x, pts[1].x, pts[2].x, pts[3].x);
    assign_min_max_4(pbox->p.y, pbox->q.y,
                     pts[0].y, pts[1].y, pts[2].y, pts[3].y);
#undef assign_min_max
#undef assign_min_max_4
    return 0;
}

/* Transform or inverse-transform a bounding box. */
/* Return gs_error_undefinedresult if the matrix is not invertible. */
static int
bbox_transform_either_only(const gs_rect * pbox_in, const gs_matrix * pmat,
                           gs_point pts[4],
     int (*point_xform) (double, double, const gs_matrix *, gs_point *))
{
    int code;

    if ((code = (*point_xform) (pbox_in->p.x, pbox_in->p.y, pmat, &pts[0])) < 0 ||
        (code = (*point_xform) (pbox_in->p.x, pbox_in->q.y, pmat, &pts[1])) < 0 ||
        (code = (*point_xform) (pbox_in->q.x, pbox_in->p.y, pmat, &pts[2])) < 0 ||
     (code = (*point_xform) (pbox_in->q.x, pbox_in->q.y, pmat, &pts[3])) < 0
        )
        DO_NOTHING;
    return code;
}

static int
bbox_transform_either(const gs_rect * pbox_in, const gs_matrix * pmat,
                      gs_rect * pbox_out,
     int (*point_xform) (double, double, const gs_matrix *, gs_point *))
{
    int code;

    /*
     * In principle, we could transform only one point and two
     * distance vectors; however, because of rounding, we will only
     * get fully consistent results if we transform all 4 points.
     * We must compute the max and min after transforming,
     * since a rotation may be involved.
     */
    gs_point pts[4];

    if ((code = bbox_transform_either_only(pbox_in, pmat, pts, point_xform)) < 0)
        return code;
    return gs_points_bbox(pts, pbox_out);
}
int
gs_bbox_transform(const gs_rect * pbox_in, const gs_matrix * pmat,
                  gs_rect * pbox_out)
{
    return bbox_transform_either(pbox_in, pmat, pbox_out,
                                 gs_point_transform);
}
int
gs_bbox_transform_only(const gs_rect * pbox_in, const gs_matrix * pmat,
                       gs_point points[4])
{
    return bbox_transform_either_only(pbox_in, pmat, points,
                                      gs_point_transform);
}
int
gs_bbox_transform_inverse(const gs_rect * pbox_in, const gs_matrix * pmat,
                          gs_rect * pbox_out)
{
    int code = bbox_transform_either(pbox_in, pmat, pbox_out,
                                 gs_point_transform_inverse);

    return code;
}

/* ======== END base/gsmatrix.c ======== */

/* ======== BEGIN base/gsmatrix.c lines 455-531 ======== */
/* Make a gs_matrix_fixed from a gs_matrix. */
int
gs_matrix_fixed_from_matrix(gs_matrix_fixed *pfmat, const gs_matrix *pmat)
{
    *(gs_matrix *)pfmat = *pmat;
    if (f_fits_in_fixed(pmat->tx) && f_fits_in_fixed(pmat->ty)) {
        pfmat->tx = fixed2float(pfmat->tx_fixed = float2fixed(pmat->tx));
        pfmat->ty = fixed2float(pfmat->ty_fixed = float2fixed(pmat->ty));
        pfmat->txy_fixed_valid = true;
    } else {
        pfmat->txy_fixed_valid = false;
    }
    return 0;
}

/* Transform a point with a fixed-point result. */
int
gs_point_transform2fixed(const gs_matrix_fixed * pmat,
                         double x, double y, gs_fixed_point * ppt)
{
    fixed px, py, t;
    double xtemp, ytemp;
    int code;

    if (!pmat->txy_fixed_valid) {	/* The translation is out of range.  Do the */
        /* computation in floating point, and convert to */
        /* fixed at the end. */
        gs_point fpt;

        gs_point_transform(x, y, (const gs_matrix *)pmat, &fpt);
        if (!(f_fits_in_fixed(fpt.x) && f_fits_in_fixed(fpt.y)))
            return_error(gs_error_limitcheck);
        ppt->x = float2fixed(fpt.x);
        ppt->y = float2fixed(fpt.y);
        return 0;
    }
    if (!is_fzero(pmat->xy)) {	/* Hope for 90 degree rotation */
        if ((code = CHECK_DFMUL2FIXED_VARS(px, y, pmat->yx, xtemp)) < 0 ||
            (code = CHECK_DFMUL2FIXED_VARS(py, x, pmat->xy, ytemp)) < 0
            )
            return code;
        FINISH_DFMUL2FIXED_VARS(px, xtemp);
        FINISH_DFMUL2FIXED_VARS(py, ytemp);
        if (!is_fzero(pmat->xx)) {
            if ((code = CHECK_DFMUL2FIXED_VARS(t, x, pmat->xx, xtemp)) < 0)
                return code;
            FINISH_DFMUL2FIXED_VARS(t, xtemp);
            if ((code = CHECK_SET_FIXED_SUM(px, px, t)) < 0)
                return code;
        }
        if (!is_fzero(pmat->yy)) {
            if ((code = CHECK_DFMUL2FIXED_VARS(t, y, pmat->yy, ytemp)) < 0)
                return code;
            FINISH_DFMUL2FIXED_VARS(t, ytemp);
            if ((code = CHECK_SET_FIXED_SUM(py, py, t)) < 0)
                return code;
        }
    } else {
        if ((code = CHECK_DFMUL2FIXED_VARS(px, x, pmat->xx, xtemp)) < 0 ||
            (code = CHECK_DFMUL2FIXED_VARS(py, y, pmat->yy, ytemp)) < 0
            )
            return code;
        FINISH_DFMUL2FIXED_VARS(px, xtemp);
        FINISH_DFMUL2FIXED_VARS(py, ytemp);
        if (!is_fzero(pmat->yx)) {
            if ((code = CHECK_DFMUL2FIXED_VARS(t, y, pmat->yx, ytemp)) < 0)
                return code;
            FINISH_DFMUL2FIXED_VARS(t, ytemp);
            if ((code = CHECK_SET_FIXED_SUM(px, px, t)) < 0)
                return code;
        }
    }
    if (((code = CHECK_SET_FIXED_SUM(ppt->x, px, pmat->tx_fixed)) < 0) ||
        ((code = CHECK_SET_FIXED_SUM(ppt->y, py, pmat->ty_fixed)) < 0) )
        return code;
    return 0;
}
/* ======== END base/gsmatrix.c ======== */

/* pdfking: gzpath.h's curve_segment, the fields read. */
typedef struct {
    gs_fixed_point pt;
    gs_fixed_point p1, p2;
} curve_segment;

/* ======== BEGIN base/gsmisc.c lines 1029-1141 ======== */

/*
 * Define a function for finding intersection of small bars.
 * Coordinates must be so small that their cubes fit into 60 bits.
 * This function doesn't check intersections at end of bars,
 * so  the caller must care of them on necessity.
 * Returns : *ry is the Y-coordinate of the intersection
 * truncated to 'fixed'; *ey is 1 iff the precise Y coordinate of
 * the intersection is greater than *ry (used by the shading algorithm).
 */
bool
gx_intersect_small_bars(fixed q0x, fixed q0y, fixed q1x, fixed q1y, fixed q2x, fixed q2y,
                        fixed q3x, fixed q3y, fixed *ry, fixed *ey)
{
    fixed dx1 = q1x - q0x, dy1 = q1y - q0y;
    fixed dx2 = q2x - q0x, dy2 = q2y - q0y;
    fixed dx3 = q3x - q0x, dy3 = q3y - q0y;

    int64_t vp2a, vp2b, vp3a, vp3b;
    int s2, s3;

    if (dx1 == 0 && dy1 == 0)
        return false; /* Zero length bars are out of interest. */
    if (dx2 == 0 && dy2 == 0)
        return false; /* Contacting ends are out of interest. */
    if (dx3 == 0 && dy3 == 0)
        return false; /* Contacting ends are out of interest. */
    if (dx2 == dx1 && dy2 == dy1)
        return false; /* Contacting ends are out of interest. */
    if (dx3 == dx1 && dy3 == dy1)
        return false; /* Contacting ends are out of interest. */
    if (dx2 == dx3 && dy2 == dy3)
        return false; /* Zero length bars are out of interest. */
    vp2a = (int64_t)dx1 * dy2;
    vp2b = (int64_t)dy1 * dx2;
    /* vp2 = vp2a - vp2b; It can overflow int64_t, but we only need the sign. */
    if (vp2a > vp2b)
        s2 = 1;
    else if (vp2a < vp2b)
        s2 = -1;
    else
        s2 = 0;
    vp3a = (int64_t)dx1 * dy3;
    vp3b = (int64_t)dy1 * dx3;
    /* vp3 = vp3a - vp3b; It can overflow int64_t, but we only need the sign. */
    if (vp3a > vp3b)
        s3 = 1;
    else if (vp3a < vp3b)
        s3 = -1;
    else
        s3 = 0;
    if (s2 == 0) {
        if (s3 == 0)
            return false; /* Collinear bars - out of interest. */
        if (0 <= dx2 && dx2 <= dx1 && 0 <= dy2 && dy2 <= dy1) {
            /* The start of the bar 2 is in the bar 1. */
            *ry = q2y;
            *ey = 0;
            return true;
        }
    } else if (s3 == 0) {
        if (0 <= dx3 && dx3 <= dx1 && 0 <= dy3 && dy3 <= dy1) {
            /* The end of the bar 2 is in the bar 1. */
            *ry = q3y;
            *ey = 0;
            return true;
        }
    } else if (s2 * s3 < 0) {
        /* The intersection definitely exists, so the determinant isn't zero.  */
        fixed d23x = dx3 - dx2, d23y = dy3 - dy2;
        int64_t det = (int64_t)dx1 * d23y - (int64_t)dy1 * d23x;
        int64_t mul = (int64_t)dx2 * d23y - (int64_t)dy2 * d23x;
        {
            /* Assuming small bars : cubes of coordinates must fit into int64_t.
               curve_samples must provide that.  */
            int64_t num = dy1 * mul, iiy;
            fixed iy;
            fixed pry, pey;

            {   /* Likely when called form wedge_trap_decompose or constant_color_quadrangle,
                   we always have det > 0 && num >= 0, but we check here for a safety reason. */
                if (det < 0)
                    num = -num, det = -det;
                iiy = (num >= 0 ? num / det : (num - det + 1) / det);
                iy = (fixed)iiy;
                if (iy != iiy) {
                    /* If it is inside the bars, it must fit into fixed. */
                    return false;
                }
            }
            if (dy1 > 0) {
                if (iy < 0 || iy >= dy1)
                    return false; /* Outside the bar 1. */
            } else {
                if (iy > 0 || iy <= dy1)
                    return false; /* Outside the bar 1. */
            }
            if (dy2 < dy3) {
                if (iy <= dy2 || iy >= dy3)
                    return false; /* Outside the bar 2. */
            } else {
                if (iy >= dy2 || iy <= dy3)
                    return false; /* Outside the bar 2. */
            }
            pry = q0y + (fixed)iy;
            pey = (iy * det < num ? 1 : 0);
            *ry = pry;
            *ey = pey;
        }
        return true;
    }
    return false;
}
/* ======== END base/gsmisc.c ======== */

/* ======== BEGIN base/gscspace.c lines 1051-1234 ======== */
static inline int
cc2dc(const gs_color_space *cs, const gs_gstate * pgs, gx_device *dev,
            gx_device_color *dc, const gs_client_color *cc)
{
    return cs->type->remap_color(cc, cs, dc, pgs, dev, gs_color_select_texture);
}

static inline void
interpolate_cc(gs_client_color *c,
        const gs_client_color *c0, const gs_client_color *c1, double t, int n)
{
    int i;

    for (i = 0; i < n; i++)
        c->paint.values[i] = c0->paint.values[i] * t + c1->paint.values[i] * (1 - t);
}

static inline bool
is_dc_nearly_linear(const gx_device *dev, const gx_device_color *c,
        const gx_device_color *c0, const gx_device_color *c1,
        double t, uchar n, float smoothness)
{
    uchar i;

    if (c0->type == &gx_dc_type_data_pure) {
        gx_color_index pure0 = c0->colors.pure;
        gx_color_index pure1 = c1->colors.pure;
        gx_color_index pure = c->colors.pure;

        for (i = 0; i < n; i++) {
            int shift = dev->color_info.comp_shift[i];
            int mask = (1 << dev->color_info.comp_bits[i]) - 1;
            int max_color = (i == dev->color_info.gray_index ? dev->color_info.max_gray
                                                             : dev->color_info.max_color);
            float max_diff = max(1, max_color * smoothness);
            int b0 = (pure0 >> shift) & mask, b1 = (pure1 >> shift) & mask;
            int b = (pure >> shift) & mask;
            double bb = b0 * t + b1 * (1 - t);

            if (any_abs(b - bb) > max_diff)
                return false;
        }
        return true;
    } else if (c0->type == &gx_dc_type_data_devn) {
        for (i = 0; i < n; i++) {
            int max_color = (i == dev->color_info.gray_index ? dev->color_info.max_gray
                : dev->color_info.max_color);
            double max_diff = max(1, max_color * smoothness);
            /* Color values are 16 bit.  We are basing the smoothness on the
               device bit depth.  So make sure to adjust the above max diff
               based upon our device bit depth */
            double ratio = (double)max_color / (double)gx_max_color_value;
            double b0 = (c0->colors.devn.values[i]) * ratio;
            double b1 = (c1->colors.devn.values[i]) * ratio;
            double b = (c->colors.devn.values[i]) * ratio;
            double bb = b0 * t + b1 * (1 - t);
            if (any_abs(b - bb) > max_diff)
                return false;
        }
        return true;
    } else {
        /* Halftones must not paint with fill_linear_color_*. */
        return false;
    }
}

/* Default color mapping linearity check, a 2-points case. */
static int
gx_cspace_is_linear_in_line(const gs_color_space *cs, const gs_gstate * pgs,
                gx_device *dev,
                const gs_client_color *c0, const gs_client_color *c1,
                float smoothness)
{
    gs_client_color c01a, c01b;
    gx_device_color d[2], d01a, d01b;
    int n = cs->type->num_components(cs);
    uchar ndev = dev->color_info.num_components;
    int code;

    code = cc2dc(cs, pgs, dev, &d[0], c0);
    if (code < 0)
        return code;
    code = cc2dc(cs, pgs, dev, &d[1], c1);
    if (code < 0)
        return code;
    interpolate_cc(&c01a, c0, c1, 0.3, n);
    code = cc2dc(cs, pgs, dev, &d01a, &c01a);
    if (code < 0)
        return code;
    if (!is_dc_nearly_linear(dev, &d01a, &d[0], &d[1], 0.3, ndev, smoothness))
        return 0;
    interpolate_cc(&c01b, c0, c1, 0.7, n);
    code = cc2dc(cs, pgs, dev, &d01b, &c01b);
    if (code < 0)
        return code;
    if (!is_dc_nearly_linear(dev, &d01b, &d[0], &d[1], 0.7, ndev, smoothness))
        return 0;
    return 1;
}

/* Default color mapping linearity check, a triangle case. */
static int
gx_cspace_is_linear_in_triangle(const gs_color_space *cs, const gs_gstate * pgs,
                gx_device *dev,
                const gs_client_color *c0, const gs_client_color *c1,
                const gs_client_color *c2, float smoothness)
{
    /* We check 4 points - the median center, and middle points of 3 sides.
       Hopely this is enough for reasonable color spaces and color renderings.
       Note it gives 7 points for a quadrangle. */
    gs_client_color c01, c12, c20, c012;
    gx_device_color d[3], d01, d12, d20, d012;

    /* Note that the device and the client color space
       can have a different number of components */

    int n = cs->type->num_components(cs);
    uchar ndev = dev->color_info.num_components;

    int code;

    code = cc2dc(cs, pgs, dev, &d[0], c0);
    if (code < 0)
        return code;
    code = cc2dc(cs, pgs, dev, &d[1], c1);
    if (code < 0)
        return code;
    code = cc2dc(cs, pgs, dev, &d[2], c2);
    if (code < 0)
        return code;

    interpolate_cc(&c01, c0, c1, 0.5, n);
    code = cc2dc(cs, pgs, dev, &d01, &c01);
    if (code < 0)
        return code;
    if (!is_dc_nearly_linear(dev, &d01, &d[0], &d[1], 0.5, ndev, smoothness))
        return 0;

    interpolate_cc(&c012, c2, &c01, 2.0 / 3, n);
    code = cc2dc(cs, pgs, dev, &d012, &c012);
    if (code < 0)
        return code;
    if (!is_dc_nearly_linear(dev, &d012, &d[2], &d01, 2.0 / 3, ndev, smoothness))
        return 0;

    interpolate_cc(&c12, c1, c2, 0.5, n);
    code = cc2dc(cs, pgs, dev, &d12, &c12);
    if (code < 0)
        return code;
    if (!is_dc_nearly_linear(dev, &d12, &d[1], &d[2], 0.5, ndev, smoothness))
        return 0;

    interpolate_cc(&c20, c2, c0, 0.5, n);
    code = cc2dc(cs, pgs, dev, &d20, &c20);
    if (code < 0)
        return code;
    if (!is_dc_nearly_linear(dev, &d20, &d[2], &d[0], 0.5, ndev, smoothness))
        return 0;
    return 1;
}

/* Default color mapping linearity check. */
int
gx_cspace_is_linear_default(const gs_color_space *cs, const gs_gstate * pgs,
                gx_device *dev,
                const gs_client_color *c0, const gs_client_color *c1,
                const gs_client_color *c2, const gs_client_color *c3,
                float smoothness, gsicc_link_t *icclink)
{
    /* Assuming 2 <= nc <= 4. We don't need other cases. */
    /* With nc == 4 assuming a convex plain quadrangle in the client color space. */
    int code;

    if (!colors_are_separable_and_linear(&dev->color_info))
        return_error(gs_error_rangecheck);
    if (c2 == NULL)
        return gx_cspace_is_linear_in_line(cs, pgs, dev, c0, c1, smoothness);
    code = gx_cspace_is_linear_in_triangle(cs, pgs, dev, c0, c1, c2, smoothness);
    if (code <= 0)
        return code;
    if (c3 == NULL)
        return 1;
    return gx_cspace_is_linear_in_triangle(cs, pgs, dev, c1, c2, c3, smoothness);
}
/* ======== END base/gscspace.c ======== */

/* ======== BEGIN base/gsicc.c lines 83-236 ======== */
static inline void
gsicc_remap_fast(gx_device *dev, unsigned short *psrc, unsigned short *psrc_cm,
                 gsicc_link_t *icc_link)
{
    /* pdfking: Device::remap of the 16-bit source color */
    float cc[GS_CLIENT_COLOR_MAX_COMPONENTS];
    int k;

    for (k = 0; k < dev->space->colors->num_components; k++)
        cc[k] = psrc[k] / 65535.0f;
    dev->sink->remap(cc, psrc_cm);
}

/* ICC color mapping linearity check, a 2-points case. Check only the 1/2 point */
static int
gx_icc_is_linear_in_line(const gs_color_space *cs, const gs_gstate * pgs,
                        gx_device *dev,
                        const gs_client_color *c0, const gs_client_color *c1,
                        float smoothness, gsicc_link_t *icclink)
{
    int nsrc = cs->type->num_components(cs);
    int ndes;
    unsigned short src0[GS_CLIENT_COLOR_MAX_COMPONENTS];
    unsigned short src1[GS_CLIENT_COLOR_MAX_COMPONENTS];
    unsigned short src01[GS_CLIENT_COLOR_MAX_COMPONENTS];
    unsigned short des0[GS_CLIENT_COLOR_MAX_COMPONENTS];
    unsigned short des1[GS_CLIENT_COLOR_MAX_COMPONENTS];
    unsigned short des01[GS_CLIENT_COLOR_MAX_COMPONENTS];
    unsigned short interp_des;
    unsigned short max_diff = (unsigned short) max(1, 65535 * smoothness);
    int k;

    ndes = dev->color_info.num_components; /* pdfking: the device profile's */

    /* Get us to ushort and get mid point */
    for (k = 0; k < nsrc; k++) {
        src0[k] = (unsigned short) (c0->paint.values[k]*65535);
        src1[k] = (unsigned short) (c1->paint.values[k]*65535);
        src01[k] = ((unsigned int) src0[k] + (unsigned int) src1[k]) >> 1;
    }
    /* Transform the end points and the interpolated point */
    gsicc_remap_fast(dev, &(src0[0]), &(des0[0]), icclink);
    gsicc_remap_fast(dev, &(src1[0]), &(des1[0]), icclink);
    gsicc_remap_fast(dev, &(src01[0]), &(des01[0]), icclink);
    /* Interpolate 1/2 value in des space and compare */
    for (k = 0; k < ndes; k++) {
        interp_des = (des0[k] + des1[k]) >> 1;
        if (any_abs((signed int) interp_des - (signed int) des01[k]) > max_diff)
            return false;
    }
    return 1;
}

/* Default icc color mapping linearity check, a triangle case. */
static int
gx_icc_is_linear_in_triangle(const gs_color_space *cs, const gs_gstate * pgs,
                gx_device *dev,
                const gs_client_color *c0, const gs_client_color *c1,
                const gs_client_color *c2, float smoothness, gsicc_link_t *icclink)
{
    /* Check 4 points middle points of 3 sides and middle of one side with
       other point.  We avoid divisions this way. */
    unsigned short src0[GS_CLIENT_COLOR_MAX_COMPONENTS];
    unsigned short src1[GS_CLIENT_COLOR_MAX_COMPONENTS];
    unsigned short src2[GS_CLIENT_COLOR_MAX_COMPONENTS];
    unsigned short des0[GS_CLIENT_COLOR_MAX_COMPONENTS];
    unsigned short des1[GS_CLIENT_COLOR_MAX_COMPONENTS];
    unsigned short des2[GS_CLIENT_COLOR_MAX_COMPONENTS];
    unsigned short src01[GS_CLIENT_COLOR_MAX_COMPONENTS];
    unsigned short src12[GS_CLIENT_COLOR_MAX_COMPONENTS];
    unsigned short src02[GS_CLIENT_COLOR_MAX_COMPONENTS];
    unsigned short src012[GS_CLIENT_COLOR_MAX_COMPONENTS];
    unsigned short des01[GS_CLIENT_COLOR_MAX_COMPONENTS];
    unsigned short des12[GS_CLIENT_COLOR_MAX_COMPONENTS];
    unsigned short des02[GS_CLIENT_COLOR_MAX_COMPONENTS];
    unsigned short des012[GS_CLIENT_COLOR_MAX_COMPONENTS];
    int nsrc = cs->type->num_components(cs);
    int ndes;
    unsigned short max_diff = (unsigned short) max(1, 65535 * smoothness);
    unsigned int interp_des;
    int k;

    ndes = dev->color_info.num_components; /* pdfking: the device profile's */

    /* This needs to be optimized. And range corrected */
    for (k = 0; k < nsrc; k++){
        src0[k] = (unsigned short) (c0->paint.values[k]*65535);
        src1[k] = (unsigned short) (c1->paint.values[k]*65535);
        src2[k] = (unsigned short) (c2->paint.values[k]*65535);
        src01[k] = (src0[k] + src1[k]) >> 1;
        src02[k] = (src0[k] + src2[k]) >> 1;
        src12[k] = (src1[k] + src2[k]) >> 1;
        src012[k] = (src12[k] + src0[k]) >> 1;
    }
    /* Map the points */
    gsicc_remap_fast(dev, &(src0[0]), &(des0[0]), icclink);
    gsicc_remap_fast(dev, &(src1[0]), &(des1[0]), icclink);
    gsicc_remap_fast(dev, &(src2[0]), &(des2[0]), icclink);
    gsicc_remap_fast(dev, &(src01[0]), &(des01[0]), icclink);
    gsicc_remap_fast(dev, &(src12[0]), &(des12[0]), icclink);
    gsicc_remap_fast(dev, &(src02[0]), &(des02[0]), icclink);
    gsicc_remap_fast(dev, &(src012[0]), &(des012[0]), icclink);
    /* Interpolate in des space and check it */
    for (k = 0; k < ndes; k++){
        interp_des = (des0[k] + des1[k]) >> 1;
        if (any_abs((signed int) interp_des - (signed int) des01[k]) > max_diff)
            return false;
        interp_des = (des0[k] + des2[k]) >> 1;
        if (any_abs((signed int) interp_des - (signed int) des02[k]) > max_diff)
            return false;
        interp_des = (des1[k] + des2[k]) >> 1;
        if (any_abs((signed int) interp_des - (signed int) des12[k]) > max_diff)
            return false;
        /* 12 with 0 */
        interp_des = (des0[k] + interp_des) >> 1;
        if (any_abs((signed int) interp_des - (signed int) des012[k]) > max_diff)
            return false;
    }
    return 1;
}

/* ICC color mapping linearity check. */
int
gx_cspace_is_linear_ICC(const gs_color_space *cs, const gs_gstate * pgs,
                gx_device *dev,
                const gs_client_color *c0, const gs_client_color *c1,
                const gs_client_color *c2, const gs_client_color *c3,
                float smoothness, gsicc_link_t *icclink)
{
    /* Assuming 2 <= nc <= 4. We don't need other cases. */
    /* With nc == 4 assuming a convex plain quadrangle in the client color space. */
    int code;

    /* Do a quick check if we are in a halftone situation. If yes,
       then we should not be doing this linear check */
    if (gx_device_must_halftone(dev)) return 0;
    if (icclink->is_identity) return 1; /* Transform is identity, linear! */

    if (!colors_are_separable_and_linear(&dev->color_info))
        return_error(gs_error_rangecheck);
    if (c2 == NULL)
        return gx_icc_is_linear_in_line(cs, pgs, dev, c0, c1, smoothness, icclink);
    code = gx_icc_is_linear_in_triangle(cs, pgs, dev, c0, c1, c2,
                                            smoothness, icclink);
    if (code <= 0)
        return code;
    if (c3 == NULL)
        return 1;
    return gx_icc_is_linear_in_triangle(cs, pgs, dev, c1, c2, c3,
                                                smoothness, icclink);
}
/* ======== END base/gsicc.c ======== */

/* ======== BEGIN base/gxpflat.c lines 26-96 ======== */
/* ---------------- Curve flattening ---------------- */

/*
 * To calculate how many points to sample along a path in order to
 * approximate it to the desired degree of flatness, we define
 *      dist((x,y)) = abs(x) + abs(y);
 * then the number of points we need is
 *      N = 1 + sqrt(3/4 * D / flatness),
 * where
 *      D = max(dist(p0 - 2*p1 + p2), dist(p1 - 2*p2 + p3)).
 * Since we are going to use a power of 2 for the number of intervals,
 * we can avoid the square root by letting
 *      N = 1 + 2^(ceiling(log2(3/4 * D / flatness) / 2)).
 * (Reference: DEC Paris Research Laboratory report #1, May 1989.)
 *
 * We treat two cases specially.  First, if the curve is very
 * short, we halve the flatness, to avoid turning short shallow curves
 * into short straight lines.  Second, if the curve forms part of a
 * character (indicated by flatness = 0), we let
 *      N = 1 + 2 * max(abs(x3-x0), abs(y3-y0)).
 * This is probably too conservative, but it produces good results.
 */
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
/* ======== END base/gxpflat.c ======== */

/* ======== BEGIN base/gzpath.h lines 109-113 ======== */
#define curve_points_to_coefficients(v0, v1, v2, v3, a, b, c, t01, t12)\
  (/*d = (v0),*/\
   t01 = (v1) - (v0), c = (t01 << 1) + t01,\
   t12 = (v2) - (v1), b = (t12 << 1) + t12 - c,\
   a = (v3) - b - c - (v0))
/* ======== END base/gzpath.h ======== */

/* ======== BEGIN base/gxshade4.h lines 1-400 ======== */
/* Copyright (C) 2001-2025 Artifex Software, Inc.
   All Rights Reserved.

   This software is provided AS-IS with no warranty, either express or
   implied.

   This software is distributed under license and may not be copied,
   modified or distributed except as expressly authorized under the terms
   of the license contained in the file LICENSE in this distribution.

   Refer to licensing information at http://www.artifex.com or contact
   Artifex Software, Inc.,  39 Mesa Street, Suite 108A, San Francisco,
   CA 94129, USA, for further information.
*/


/* Internal definitions for triangle shading rendering */

#ifndef gxshade4_INCLUDED
#  define gxshade4_INCLUDED

/* pdfking: what gxshade4.h takes from gxshade.h and gscicach.h. */
typedef struct gs_shading_s gs_shading_mesh_t;
typedef struct gs_color_index_cache_s gs_color_index_cache_t;
typedef struct mesh_vertex_s {
    gs_fixed_point p;
    float cc[GS_CLIENT_COLOR_MAX_COMPONENTS];
} mesh_vertex_t;
typedef struct shading_vertex_s shading_vertex_t;
typedef struct patch_color_s patch_color_t;
#define shading_fill_state_common\
  gx_device *dev;\
  gs_gstate *pgs;\
  const gs_color_space *direct_space;\
  int num_components;		/* # of color components in direct_space */\
  float cc_max_error[GS_CLIENT_COLOR_MAX_COMPONENTS];\
  gx_device *trans_device;\
  gsicc_link_t *icclink;\
  bool cs_always_linear;\
  int ShadingType
typedef struct shading_fill_state_s {
    shading_fill_state_common;
} shading_fill_state_t;

/* Configuration flags for development needs only. Users should not modify them. */
#define USE_LINEAR_COLOR_PROCS 1 /* Old code = 0, new code = 1. */

#define QUADRANGLES 0 /* 0 = decompose by triangles, 1 = by quadrangles. */
/* The code QUADRANGLES 1 appears unuseful.
   We keep it because it stores a valuable code for constant_color_quadrangle,
   which decomposes a random quadrangle into 3 or 4 trapezoids.
   The color approximation looks worse than with triangles, and works some slower.
 */
#define INTERPATCH_PADDING (fixed_1 / 2) /* Emulate a trapping for poorly designed documents. */
/* When INTERPATCH_PADDING > 0, it generates paddings between patches,
   i.e. performs a patch expansion, being similar
   to the path adjustment in the filling algorithm.
   The expansion is an emulation of Adobe's trapping.
   The value specifies the width of paddings.
   We did some testing of Adobe RIP, and it looks as applying
   same logicks as for clipping - any part of pixel inside.
   Therefore the expansion should be half pixel size.
 */
#define COLOR_CONTIGUITY 1 /* A smothness divisor for triangulation. */
/* This is a coefficient used to rich
   a better color contiguity. The value 1 corresponds to PLRM,
   bigger values mean more contiguity. The speed decreases as
   a square of COLOR_CONTIGUITY.
 */
#define LAZY_WEDGES 1 /* 0 = fill immediately, 1 = fill lazy. */
/* This mode delays creating wedges for a boundary until
   both neoghbour areas are painted. At that moment we can know
   all subdivision points for both right and left areas,
   and skip wedges for common points. Therefore the number of wadges
   dramatically reduces, causing a significant speedup.
   The LAZY_WEDGES 0 mode was not systematically tested.
 */
#define VD_TRACE_DOWN 1 /* Developer's needs, not important for production. */
#define NOFILL_TEST 0 /* Developer's needs, must be off for production. */
#define SKIP_TEST 0 /* Developer's needs, must be off for production. */
/* End of configuration flags (we don't mean that users should modify the rest). */

typedef struct mesh_frame_s {	/* recursion frame */
    mesh_vertex_t va, vb, vc;	/* current vertices */
    bool check_clipping;
} mesh_frame_t;
/****** NEED GC DESCRIPTOR ******/

/*
 * Define the fill state structure for triangle shadings.  This is used
 * both for the Gouraud triangle shading types and for the Coons and
 * tensor patch types.
 *
 * The shading pointer is named pshm rather than psh in case subclasses
 * also want to store a pointer of a more specific type.
 */
#define mesh_fill_state_common\
  shading_fill_state_common;\
  const gs_shading_mesh_t *pshm;\
  gs_fixed_rect rect
typedef struct mesh_fill_state_s {
    mesh_fill_state_common;
} mesh_fill_state_t;
/****** NEED GC DESCRIPTOR ******/

typedef struct wedge_vertex_list_elem_s wedge_vertex_list_elem_t;
struct wedge_vertex_list_elem_s {
    gs_fixed_point p;
    int level;
    int32_t divide_count;
    wedge_vertex_list_elem_t *next, *prev;
};
typedef struct {
    bool last_side;
    wedge_vertex_list_elem_t *beg, *end;
} wedge_vertex_list_t;

#define LAZY_WEDGES_MAX_LEVEL 9 /* memory consumption is
    sizeof(wedge_vertex_list_elem_t) * LAZY_WEDGES_MAX_LEVEL * (1 << LAZY_WEDGES_MAX_LEVEL) */

#define SHADING_COLOR_STACK_SIZE 200; /* Should be enough for max 64 decomposition levels. */

/* Define a color to be used in curve rendering. */
/* This may be a real client color, or a parametric function argument. */
struct patch_color_s {
    float t[2];			/* parametric value */
    gs_client_color cc;
    /* NOTE : The structure gs_client_color ends with a big array, but only few elements
       are used in most cases. Therefore sometimes we allocate a shorter area,
       so that ending elements are not allocated and must not be accessed/modified.
       The number of allocated elements are known from the shading color space
       and from patch_fill_state_s::num_components. */
};

/* Define the common state for rendering Coons and tensor patches. */
struct patch_fill_state_s {
    mesh_fill_state_common;
    const gs_function_t *Function;
    int function_arg_shift;
    bool vectorization;
    int n_color_args;
    fixed max_small_coord; /* Length restriction for intersection_of_small_bars. */
    wedge_vertex_list_elem_t *wedge_vertex_list_elem_buffer;
    wedge_vertex_list_elem_t *free_wedge_vertex;
    int wedge_vertex_list_elem_count;
    int wedge_vertex_list_elem_count_max;
    gs_client_color color_domain;
    fixed decomposition_limit;
    fixed fixed_flat;
    double smoothness;
    bool maybe_self_intersecting;
    bool monotonic_color;
    bool linear_color;
    bool unlinear;
    bool inside;
    int color_stack_size;
    int color_stack_step;
    byte *color_stack_ptr;
    byte *color_stack; /* A storage for shortened patch_color_t structures. */
    byte *color_stack_limit;
    gs_memory_t *memory; /* Where color_buffer is allocated. */
    gs_color_index_cache_t *pcic;
} ;

/* Define a structure for mesh or patch vertex. */
struct shading_vertex_s {
    gs_fixed_point p;
    const patch_color_t *c;
};

/* Define one segment (vertex and next control points) of a curve. */
typedef struct patch_curve_s {
    mesh_vertex_t vertex;
    gs_fixed_point control[2];
    bool straight;
} patch_curve_t;

typedef struct {
    const shading_vertex_t *p[2][2]; /* [v][u] */
    wedge_vertex_list_t *l0001, *l0111, *l1110, *l1000;
} quadrangle_patch;

/* Initialize the fill state for triangle shading. */
int mesh_init_fill_state(mesh_fill_state_t * pfs,
                          const gs_shading_mesh_t * psh,
                          const gs_fixed_rect * rect_clip,
                          gx_device * dev, gs_gstate * pgs);

int init_patch_fill_state(patch_fill_state_t *pfs);
bool term_patch_fill_state(patch_fill_state_t *pfs);

int mesh_triangle(patch_fill_state_t *pfs,
    const shading_vertex_t *p0, const shading_vertex_t *p1, const shading_vertex_t *p2);

int mesh_padding(patch_fill_state_t *pfs, const gs_fixed_point *p0, const gs_fixed_point *p1,
            const patch_color_t *c0, const patch_color_t *c1);

int patch_fill(patch_fill_state_t * pfs, const patch_curve_t curve[4],
           const gs_fixed_point interior[4],
           void (*transform) (gs_fixed_point *, const patch_curve_t[4],
                              const gs_fixed_point[4], double, double));

int constant_color_quadrangle(patch_fill_state_t *pfs, const quadrangle_patch *p, bool self_intersecting);

int wedge_vertex_list_elem_buffer_alloc(patch_fill_state_t *pfs);
void wedge_vertex_list_elem_buffer_free(patch_fill_state_t *pfs);

void patch_resolve_color(patch_color_t * ppcr, const patch_fill_state_t *pfs);

int gx_shade_background(gx_device *pdev, const gs_fixed_rect *rect,
        const gx_device_color *pdevc, gs_logical_operation_t log_op);

int patch_color_to_device_color(const patch_fill_state_t *pfs,
        const patch_color_t *c, gx_device_color *pdevc);

byte *reserve_colors(patch_fill_state_t *pfs, patch_color_t *c0[], int n);
void release_colors(patch_fill_state_t *pfs, byte *ptr, int n);

#endif /* gxshade4_INCLUDED */

/* ======== END base/gxshade4.h ======== */

/* ======== BEGIN base/gxshade6.c lines 76-140 ======== */

enum {
    min_linear_grades = 255 /* The minimal number of device color grades,
            required to apply linear color device functions. */
};

/* ================ Utilities ================ */

static int
allocate_color_stack(patch_fill_state_t *pfs, gs_memory_t *memory)
{
    if (pfs->color_stack != NULL)
        return 0;
    pfs->color_stack_step = offsetof(patch_color_t, cc.paint.values) + sizeof(float) * pfs->num_components; /* pdfking: offset_of */
    pfs->color_stack_step = (pfs->color_stack_step + sizeof(void *) - 1) / sizeof(void *) * sizeof(void *); /* Alignment */

    pfs->color_stack_size = pfs->color_stack_step * SHADING_COLOR_STACK_SIZE;
    pfs->color_stack = gs_alloc_bytes(memory, pfs->color_stack_size, "allocate_color_stack");
    if (pfs->color_stack == NULL)
        return_error(gs_error_VMerror);
    pfs->color_stack_limit = pfs->color_stack + pfs->color_stack_size;
    pfs->color_stack_ptr = pfs->color_stack;
    pfs->memory = memory;
    return 0;
}

static inline byte *
reserve_colors_inline(patch_fill_state_t *pfs, patch_color_t *c[], int n)
{
    int i;
    byte *ptr0 = pfs->color_stack_ptr, *ptr = ptr0;

    for (i = 0; i < n; i++, ptr += pfs->color_stack_step)
        c[i] = (patch_color_t *)ptr;
    if (ptr > pfs->color_stack_limit) {
        c[0] = NULL; /* safety. */
        return NULL;
    }
    pfs->color_stack_ptr = ptr;
    return ptr0;
}

byte *
reserve_colors(patch_fill_state_t *pfs, patch_color_t *c[], int n)
{
    return reserve_colors_inline(pfs, c, n);
}

static inline void
release_colors_inline(patch_fill_state_t *pfs, byte *ptr, int n)
{
#if 0 /* Saving the invariant for records. */
    pfs->color_stack_ptr = pfs->color_stack_step * n;
    assert((byte *)pfs->color_stack_ptr == ptr);
#else
    pfs->color_stack_ptr = ptr;
#endif
}
void
release_colors(patch_fill_state_t *pfs, byte *ptr, int n)
{
    release_colors_inline(pfs, ptr, n);
}

/* Get colors for patch vertices. */
/* ======== END base/gxshade6.c ======== */

/* ======== BEGIN base/gxshade6.c lines 218-440 ======== */

static inline bool
is_linear_color_applicable(const patch_fill_state_t *pfs)
{
    if (!USE_LINEAR_COLOR_PROCS)
        return false;
    if (!colors_are_separable_and_linear(&pfs->dev->color_info))
        return false;
    /* pdfking: tiffsep is contone, never halftoned */
    return true;
}

static int
alloc_patch_fill_memory(patch_fill_state_t *pfs, gs_memory_t *memory, const gs_color_space *pcs)
{
    int code;

    pfs->memory = memory;
#   if LAZY_WEDGES
        code = wedge_vertex_list_elem_buffer_alloc(pfs);
        if (code < 0)
            return code;
#   endif
    pfs->max_small_coord = 1 << ((sizeof(int64_t) * 8 - 1/*sign*/) / 3);
    code = allocate_color_stack(pfs, memory);
    if (code < 0)
        return code;
    /* pdfking: no color index cache (it caches remap_color's results
       exactly, so this is what it returns) */
    pfs->pcic = NULL;
    return 0;
}

int
init_patch_fill_state(patch_fill_state_t *pfs)
{
    /* Warning : pfs->Function must be set in advance. */
    const gs_color_space *pcs = pfs->direct_space;
    gs_client_color fcc0, fcc1;
    int i;

    for (i = 0; i < pfs->num_components; i++) {
        fcc0.paint.values[i] = -1000000;
        fcc1.paint.values[i] = 1000000;
    }
    pcs->type->restrict_color(&fcc0, pcs);
    pcs->type->restrict_color(&fcc1, pcs);
    for (i = 0; i < pfs->num_components; i++)
        pfs->color_domain.paint.values[i] = max(fcc1.paint.values[i] - fcc0.paint.values[i], 1);
    pfs->vectorization = false; /* A stub for a while. Will use with pclwrite. */
    pfs->maybe_self_intersecting = true;
    pfs->monotonic_color = (pfs->Function == NULL);
    pfs->function_arg_shift = 0;
    pfs->linear_color = false;
    pfs->inside = false;
    pfs->n_color_args = 1;
#ifdef MAX_SHADING_RESOLUTION
    pfs->decomposition_limit = float2fixed(min(pfs->dev->HWResolution[0],
                                               pfs->dev->HWResolution[1]) / MAX_SHADING_RESOLUTION);
    pfs->decomposition_limit = max(pfs->decomposition_limit, fixed_1);
#else
    pfs->decomposition_limit = fixed_1;
#endif
    pfs->fixed_flat = float2fixed(pfs->pgs->flatness);
    /* Restrict the pfs->smoothness with 1/min_linear_grades, because cs_is_linear
       can't provide a better precision due to the color
       representation with integers.
     */
    pfs->smoothness = max(pfs->pgs->smoothness, 1.0 / min_linear_grades);
    pfs->color_stack_size = 0;
    pfs->color_stack_step = 0;
    pfs->color_stack_ptr = NULL;
    pfs->color_stack = NULL;
    pfs->color_stack_limit = NULL;
    pfs->unlinear = !is_linear_color_applicable(pfs);
    pfs->pcic = NULL;
    return alloc_patch_fill_memory(pfs, pfs->pgs->memory, pcs);
}

bool
term_patch_fill_state(patch_fill_state_t *pfs)
{
    bool b = (pfs->color_stack_ptr != pfs->color_stack);
#   if LAZY_WEDGES
        wedge_vertex_list_elem_buffer_free(pfs);
#   endif
    if (pfs->color_stack)
        gs_free_object(pfs->memory, pfs->color_stack, "term_patch_fill_state");
    return b;
}

/* Resolve a patch color using the Function if necessary. */
static inline void
patch_resolve_color_inline(patch_color_t * ppcr, const patch_fill_state_t *pfs)
{
    if (pfs->Function) {
        const gs_color_space *pcs = pfs->direct_space;

        gs_function_evaluate(pfs->Function, ppcr->t, ppcr->cc.paint.values);
        pcs->type->restrict_color(&ppcr->cc, pcs);
    }
}

void
patch_resolve_color(patch_color_t * ppcr, const patch_fill_state_t *pfs)
{
    patch_resolve_color_inline(ppcr, pfs);
}

/*
 * Calculate the interpolated color at a given point.
 * Note that we must do this twice for bilinear interpolation.
 * We use the name ppcr rather than ppc because an Apple compiler defines
 * ppc when compiling on PowerPC systems (!).
 */
static void
patch_interpolate_color(patch_color_t * ppcr, const patch_color_t * ppc0,
       const patch_color_t * ppc1, const patch_fill_state_t *pfs, double t)
{
    /* The old code gives -IND on Intel. */
    if (pfs->Function) {
        ppcr->t[0] = ppc0->t[0] * (1 - t) + t * ppc1->t[0];
        ppcr->t[1] = ppc0->t[1] * (1 - t) + t * ppc1->t[1];
        patch_resolve_color_inline(ppcr, pfs);
    } else {
        int ci;

        for (ci = pfs->num_components - 1; ci >= 0; --ci)
            ppcr->cc.paint.values[ci] =
                ppc0->cc.paint.values[ci] * (1 - t) + t * ppc1->cc.paint.values[ci];
    }
}

/* ================ Specific shadings ================ */

/*
 * The curves are stored in a clockwise or counter-clockwise order that maps
 * to the patch definition in a non-intuitive way.  The documentation
 * (pp. 277-281 of the PostScript Language Reference Manual, Third Edition)
 * says that the curves are in the order D1, C2, D2, C1.
 */
/* The starting points of the curves: */
#define D1START 0
#define C2START 1
#define D2START 3
#define C1START 0
/* The control points of the curves (x means reversed order): */
#define D1CTRL 0
#define C2CTRL 1
#define D2XCTRL 2
#define C1XCTRL 3
/* The end points of the curves: */
#define D1END 1
#define C2END 2
#define D2END 2
#define C1END 3

/* ---------------- Common code ---------------- */

/* Evaluate a curve at a given point. */
static void
curve_eval(gs_fixed_point * pt, const gs_fixed_point * p0,
           const gs_fixed_point * p1, const gs_fixed_point * p2,
           const gs_fixed_point * p3, double t)
{
    fixed a, b, c, d;
    fixed t01, t12;

    d = p0->x;
    curve_points_to_coefficients(d, p1->x, p2->x, p3->x,
                                 a, b, c, t01, t12);
    pt->x = (fixed) (((a * t + b) * t + c) * t + d);
    d = p0->y;
    curve_points_to_coefficients(d, p1->y, p2->y, p3->y,
                                 a, b, c, t01, t12);
    pt->y = (fixed) (((a * t + b) * t + c) * t + d);
    if_debug3('2', "[2]t=%g => (%g,%g)\n", t, fixed2float(pt->x),
              fixed2float(pt->y));
}

/* ---------------- Coons patch shading ---------------- */

/* Calculate the device-space coordinate corresponding to (u,v). */
static void
Cp_transform(gs_fixed_point * pt, const patch_curve_t curve[4],
             const gs_fixed_point ignore_interior[4], double u, double v)
{
    double co_u = 1.0 - u, co_v = 1.0 - v;
    gs_fixed_point c1u, d1v, c2u, d2v;

    curve_eval(&c1u, &curve[C1START].vertex.p,
               &curve[C1XCTRL].control[1], &curve[C1XCTRL].control[0],
               &curve[C1END].vertex.p, u);
    curve_eval(&d1v, &curve[D1START].vertex.p,
               &curve[D1CTRL].control[0], &curve[D1CTRL].control[1],
               &curve[D1END].vertex.p, v);
    curve_eval(&c2u, &curve[C2START].vertex.p,
               &curve[C2CTRL].control[0], &curve[C2CTRL].control[1],
               &curve[C2END].vertex.p, u);
    curve_eval(&d2v, &curve[D2START].vertex.p,
               &curve[D2XCTRL].control[1], &curve[D2XCTRL].control[0],
               &curve[D2END].vertex.p, v);
#define COMPUTE_COORD(xy)\
    pt->xy = (fixed)\
        ((co_v * c1u.xy + v * c2u.xy) + (co_u * d1v.xy + u * d2v.xy) -\
         (co_v * (co_u * curve[C1START].vertex.p.xy +\
                  u * curve[C1END].vertex.p.xy) +\
          v * (co_u * curve[C2START].vertex.p.xy +\
               u * curve[C2END].vertex.p.xy)))
    COMPUTE_COORD(x);
    COMPUTE_COORD(y);
#undef COMPUTE_COORD
    if_debug4('2', "[2](u=%g,v=%g) => (%g,%g)\n",
              u, v, fixed2float(pt->x), fixed2float(pt->y));
}

/* ======== END base/gxshade6.c ======== */

/* ======== BEGIN base/gxshade6.c lines 481-528 ======== */
/* Calculate the device-space coordinate corresponding to (u,v). */
static void
Tpp_transform(gs_fixed_point * pt, const patch_curve_t curve[4],
              const gs_fixed_point interior[4], double u, double v)
{
    double Bu[4], Bv[4];
    gs_fixed_point pts[4][4];
    int i, j;
    double x = 0, y = 0;

    /* Compute the Bernstein polynomials of u and v. */
    {
        double u2 = u * u, co_u = 1.0 - u, co_u2 = co_u * co_u;
        double v2 = v * v, co_v = 1.0 - v, co_v2 = co_v * co_v;

        Bu[0] = co_u * co_u2, Bu[1] = 3 * u * co_u2,
            Bu[2] = 3 * u2 * co_u, Bu[3] = u * u2;
        Bv[0] = co_v * co_v2, Bv[1] = 3 * v * co_v2,
            Bv[2] = 3 * v2 * co_v, Bv[3] = v * v2;
    }

    /* Arrange the points into an indexable order. */
    pts[0][0] = curve[0].vertex.p;
    pts[0][1] = curve[0].control[0];
    pts[0][2] = curve[0].control[1];
    pts[0][3] = curve[1].vertex.p;
    pts[1][3] = curve[1].control[0];
    pts[2][3] = curve[1].control[1];
    pts[3][3] = curve[2].vertex.p;
    pts[3][2] = curve[2].control[0];
    pts[3][1] = curve[2].control[1];
    pts[3][0] = curve[3].vertex.p;
    pts[2][0] = curve[3].control[0];
    pts[1][0] = curve[3].control[1];
    pts[1][1] = interior[0];
    pts[2][1] = interior[1];
    pts[2][2] = interior[2];
    pts[1][2] = interior[3];

    /* Now compute the actual point. */
    for (i = 0; i < 4; ++i)
        for (j = 0; j < 4; ++j) {
            double coeff = Bu[i] * Bv[j];

            x += pts[i][j].x * coeff, y += pts[i][j].y * coeff;
        }
    pt->x = (fixed)x, pt->y = (fixed)y;
}
/* ======== END base/gxshade6.c ======== */

/* ======== BEGIN base/gxshade6.c lines 576-3002 ======== */
/*
    This algorithm performs a decomposition of the shading area
    into a set of constant color trapezoids, some of which
    may use the transpozed coordinate system.

    The target device assumes semi-open intrvals by X to be painted
    (See PLRM3, 7.5. Scan conversion details), i.e.
    it doesn't paint pixels which falls exactly to the right side.
    Note that with raster devices the algorithm doesn't paint pixels,
    whigh are partially covered by the shading area,
    but which's centers are outside the area.

    Pixels inside a monotonic part of the shading area are painted
    at once, but some exceptions may happen :

        - While flattening boundaries of a subpatch,
        to keep the plane coverage contiguity we insert wedges
        between neighbor subpatches, which use a different
        flattening factor. With non-monotonic curves
        those wedges may overlap or be self-overlapping, and a pixel
        is painted so many times as many wedges cover it. Fortunately
        the area of most wedges is zero or extremily small.

        - Since quazi-horizontal wedges may have a non-constant color,
        they can't decompose into constant color trapezoids with
        keeping the coverage contiguity. To represent them we
        apply the XY plane transposition. But with the transposition
        a semiopen interval can met a non-transposed one,
        so that some lines are not covered. Therefore we emulate
        closed intervals with expanding the transposed trapesoids in
        fixed_epsilon, and pixels at that boundary may be painted twice.

        - A boundary of a monotonic area can't compute in XY
        preciselly due to high order polynomial equations.
        Therefore the subdivision near the monotonity boundary
        may paint some pixels twice within same monotonic part.

    Non-monotonic areas slow down due to a tinny subdivision required.

    The target device may be either raster or vector.
    Vector devices should preciselly pass trapezoids to the output.
    Note that ends of sides of a trapesoid are not necessary
    the trapezoid's vertices. Converting this thing into
    an exact quadrangle may cause an arithmetic error,
    and the rounding must be done so that the coverage
    contiguity is not lost.

    When a device passes a trapezoid to it's output,
    a regular rounding would keep the coverage contiguity,
    except for the transposed trapesoids.
    If a transposed trapezoid is being transposed back,
    it doesn't become a canonic trapezoid, and a further
    decomposition is neccessary. But rounding errors here
    would break the coverage contiguity at boundaries
    of the tansposed part of the area.

    Devices, which have no transposed trapezoids and represent
    trapezoids only with 8 coordinates of vertices of the quadrangle
    (pclwrite is an example) may apply the backward transposition,
    and a clipping instead the further decomposition.
    Note that many clip regions may appear for all wedges.
    Note that in some cases the adjustment of the right side to be
    withdrown before the backward transposition.
 */
 /* We believe that a multiplication of 32-bit integers with a
    64-bit result is performed by modern platforms performs
    in hardware level. Therefore we widely use it here,
    but we minimize the usage of a multiplication of longer integers.

    Unfortunately we do need a multiplication of long integers
    in intersection_of_small_bars, because solving the linear system
    requires tripple multiples of 'fixed'. Therefore we retain
    of it's usage in the algorithm of the main branch.
    Configuration macro QUADRANGLES prevents it.
  */

typedef struct {
    gs_fixed_point pole[4][4]; /* [v][u] */
    patch_color_t *c[2][2];     /* [v][u] */
} tensor_patch;

typedef enum {
    interpatch_padding = 1, /* A Padding between patches for poorly designed documents. */
    inpatch_wedge = 2  /* Wedges while a patch decomposition. */
} wedge_type_t;

int
wedge_vertex_list_elem_buffer_alloc(patch_fill_state_t *pfs)
{
    const int max_level = LAZY_WEDGES_MAX_LEVEL;
    gs_memory_t *memory = pfs->memory;

    /* We have 'max_level' levels, each of which divides 1 or 3 sides.
       LAZY_WEDGES stores all 2^level divisions until
       the other area of same bnoundary is processed.
       Thus the upper estimation of the buffer size is :
       max_level * (1 << max_level) * 3.
       Likely this estimation to be decreased to
       max_level * (1 << max_level) * 2.
       because 1 side of a triangle is always outside the division path.
       For now we set the smaller estimation for obtaining an experimental data
       from the wild. */
    pfs->wedge_vertex_list_elem_count_max = max_level * (1 << max_level) * 2;
    pfs->wedge_vertex_list_elem_buffer = (wedge_vertex_list_elem_t *)gs_alloc_bytes(memory,
            sizeof(wedge_vertex_list_elem_t) * (size_t)pfs->wedge_vertex_list_elem_count_max,
            "alloc_wedge_vertex_list_elem_buffer");
    if (pfs->wedge_vertex_list_elem_buffer == NULL)
        return_error(gs_error_VMerror);
    pfs->free_wedge_vertex = NULL;
    pfs->wedge_vertex_list_elem_count = 0;
    return 0;
}

void
wedge_vertex_list_elem_buffer_free(patch_fill_state_t *pfs)
{
    gs_memory_t *memory = pfs->memory;

    gs_free_object(memory, pfs->wedge_vertex_list_elem_buffer,
                "wedge_vertex_list_elem_buffer_free");
    pfs->wedge_vertex_list_elem_buffer = NULL;
    pfs->free_wedge_vertex = NULL;
}

static inline wedge_vertex_list_elem_t *
wedge_vertex_list_elem_reserve(patch_fill_state_t *pfs)
{
    wedge_vertex_list_elem_t *e = pfs->free_wedge_vertex;

    if (e != NULL) {
        pfs->free_wedge_vertex = e->next;
        return e;
    }
    if (pfs->wedge_vertex_list_elem_count < pfs->wedge_vertex_list_elem_count_max)
        return pfs->wedge_vertex_list_elem_buffer + pfs->wedge_vertex_list_elem_count++;
    return NULL;
}

static inline void
wedge_vertex_list_elem_release(patch_fill_state_t *pfs, wedge_vertex_list_elem_t *e)
{
    e->next = pfs->free_wedge_vertex;
    pfs->free_wedge_vertex = e;
}

static inline void
release_wedge_vertex_list_interval(patch_fill_state_t *pfs,
    wedge_vertex_list_elem_t *beg, wedge_vertex_list_elem_t *end)
{
    wedge_vertex_list_elem_t *e = beg->next, *ee;

    beg->next = end;
    end->prev = beg;
    for (; e != end; e = ee) {
        ee = e->next;
        wedge_vertex_list_elem_release(pfs, e);
    }
}

static inline int
release_wedge_vertex_list(patch_fill_state_t *pfs, wedge_vertex_list_t *ll, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        wedge_vertex_list_t *l = ll + i;

        if (l->beg != NULL) {
            if (l->end == NULL)
                return_error(gs_error_unregistered); /* Must not happen. */
            release_wedge_vertex_list_interval(pfs, l->beg, l->end);
            wedge_vertex_list_elem_release(pfs, l->beg);
            wedge_vertex_list_elem_release(pfs, l->end);
            l->beg = l->end = NULL;
        } else if (l->end != NULL)
            return_error(gs_error_unregistered); /* Must not happen. */
    }
    return 0;
}

static inline wedge_vertex_list_elem_t *
wedge_vertex_list_find(wedge_vertex_list_elem_t *beg, const wedge_vertex_list_elem_t *end,
            int level)
{
    wedge_vertex_list_elem_t *e = beg;

    if (beg == NULL || end == NULL)
        return NULL; /* Must not happen. */
    for (; e != end; e = e->next)
        if (e->level == level)
            return e;
    return NULL;
}

static inline void
init_wedge_vertex_list(wedge_vertex_list_t *l, int n)
{
    memset(l, 0, sizeof(*l) * n);
}

/* For a given set of poles in the tensor patch (for instance
 * [0][0], [0][1], [0][2], [0][3] or [0][2], [1][2], [2][2], [3][2])
 * return the number of subdivisions required to flatten the bezier
 * given by those poles to the required flatness.
 */
static inline int
curve_samples(patch_fill_state_t *pfs,
                const gs_fixed_point *pole, int pole_step, fixed fixed_flat)
{
    curve_segment s;
    int k;

    s.p1.x = pole[pole_step].x;
    s.p1.y = pole[pole_step].y;
    s.p2.x = pole[pole_step * 2].x;
    s.p2.y = pole[pole_step * 2].y;
    s.pt.x = pole[pole_step * 3].x;
    s.pt.y = pole[pole_step * 3].y;
    k = gx_curve_log2_samples(pole[0].x, pole[0].y, &s, fixed_flat);
    {
#       if LAZY_WEDGES || QUADRANGLES
            int k1;
            fixed L = any_abs(pole[pole_step * 1].x - pole[pole_step * 0].x) + any_abs(pole[pole_step * 1].y - pole[pole_step * 0].y) +
                      any_abs(pole[pole_step * 2].x - pole[pole_step * 1].x) + any_abs(pole[pole_step * 2].y - pole[pole_step * 1].y) +
                      any_abs(pole[pole_step * 3].x - pole[pole_step * 2].x) + any_abs(pole[pole_step * 3].y - pole[pole_step * 2].y);
#       endif

#       if LAZY_WEDGES
            /* The code here is voodoo that dates back to 2004 when Igor first committed
             * the shading code rewrite. It used to claim to "Restrict lengths for a reasonable
             * memory consumption", and just to use the formulation below based upon LAZY_WEDGES_MAX_LEVEL.
             * In bug 700989, we came across a Shading where we were "restricting the length" too much,
             * so Ken came up with an alternative formulation that makes it depend upon the smoothness
             * parameter. This is really voodoo of a different form, albeit voodoo that's based at least
             * on something plausible!
             * What we know is that for the file in that bug to render correctly, a smoothness value of
             * 0.02 (the default) needs to give a k1 value no smaller than 7.
             * It also seems likely that this only needs to be changed for Type 1 shadings (ones based
             * on functions) as the others are likely to be (at least mostly) continuous.
             * This logic may be subject to future optimisations/improvements/blood rituals.
             */
            if (pfs->ShadingType == 1) {
                /* New 'smoothness based' logic. */
                k1 = ilog2(L / fixed_1 / (1 << ((int)ceil(pfs->smoothness * 30)) ));
            } else {
                /* Igor's original logic. */
                k1 = ilog2(L / fixed_1 / (1 << (LAZY_WEDGES_MAX_LEVEL - 1)));
            }
            k = max(k, k1);
#       endif
#       if QUADRANGLES
            /* Restrict lengths for intersection_of_small_bars : */
            k = max(k, ilog2(L) - ilog2(pfs->max_small_coord));
#       endif
    }
    return 1 << k;
}

static inline bool
intersection_of_small_bars(const gs_fixed_point q[4], int i0, int i1, int i2, int i3, fixed *ry, fixed *ey)
{
    /* This function is only used with QUADRANGLES. */
    return gx_intersect_small_bars(q[i0].x, q[i0].y, q[i1].x, q[i1].y, q[i2].x, q[i2].y, q[i3].x, q[i3].y, ry, ey);
}

static inline void
adjust_swapped_boundary(fixed *b, bool swap_axes)
{
    if (swap_axes) {
        /*  Sinse the rasterizer algorithm assumes semi-open interval
            when computing pixel coverage, we should expand
            the right side of the area. Otherwise a dropout can happen :
            if the left neighbour is painted with !swap_axes,
            the left side of this area appears to be the left side
            of the neighbour area, and the side is not included
            into both areas.
         */
        *b += fixed_epsilon;
    }
}

static inline void
make_trapezoid(const gs_fixed_point q[4],
        int vi0, int vi1, int vi2, int vi3, fixed ybot, fixed ytop,
        bool swap_axes, bool orient, gs_fixed_edge *le, gs_fixed_edge *re)
{
    if (!orient) {
        le->start = q[vi0];
        le->end = q[vi1];
        re->start = q[vi2];
        re->end = q[vi3];
    } else {
        le->start = q[vi2];
        le->end = q[vi3];
        re->start = q[vi0];
        re->end = q[vi1];
    }
    adjust_swapped_boundary(&re->start.x, swap_axes);
    adjust_swapped_boundary(&re->end.x, swap_axes);
}

static inline int
gx_shade_trapezoid(patch_fill_state_t *pfs, const gs_fixed_point q[4],
        int vi0, int vi1, int vi2, int vi3, fixed ybot0, fixed ytop0,
        bool swap_axes, const gx_device_color *pdevc, bool orient)
{
    gs_fixed_edge le, re;
    int code;
    fixed ybot = max(ybot0, swap_axes ? pfs->rect.p.x : pfs->rect.p.y);
    fixed ytop = min(ytop0, swap_axes ? pfs->rect.q.x : pfs->rect.q.y);
    fixed xleft  = (swap_axes ? pfs->rect.p.y : pfs->rect.p.x);
    fixed xright = (swap_axes ? pfs->rect.q.y : pfs->rect.q.x);

    if (ybot >= ytop)
        return 0;
#   if NOFILL_TEST
        if (dbg_nofill)
            return 0;
#   endif
    make_trapezoid(q, vi0, vi1, vi2, vi3, ybot, ytop, swap_axes, orient, &le, &re);
    if (!pfs->inside) {
        bool clip = false;

        /* We are asked to clip a trapezoid to a rectangle. If the rectangle
         * is entirely contained within the rectangle, then no clipping is
         * actually required. If the left edge is entirely to the right of
         * the rectangle, or the right edge is entirely to the left, we
         * clip away to nothing. If the left edge is entirely to the left of
         * the rectangle, then we can simplify it to a vertical edge along
         * the edge of the rectangle. Likewise with the right edge if it's
         * entirely to the right of the rectangle.*/
        if (le.start.x > xright) {
            if (le.end.x > xright) {
                return 0;
            }
            clip = true;
        } else if (le.end.x > xright) {
            clip = true;
        }
        if (le.start.x < xleft) {
            if (le.end.x < xleft) {
                le.start.x = xleft;
                le.end.x   = xleft;
                le.start.y = ybot;
                le.end.y   = ytop;
            } else {
                clip = true;
            }
        } else if (le.end.x < xleft) {
            clip = true;
        }
        if (re.start.x < xleft) {
            if (re.end.x < xleft) {
                return 0;
            }
            clip = true;
        } else if (re.end.x < xleft) {
            clip = true;
        }
        if (re.start.x > xright) {
            if (re.end.x > xright) {
                re.start.x = xright;
                re.end.x   = xright;
                re.start.y = ybot;
                re.end.y   = ytop;
            } else {
                clip = true;
            }
        } else if (re.end.x > xright) {
            clip = true;
        }
        if (clip)
        {
            /* Some form of clipping seems to be required. A certain amount
             * of horridness goes on here to ensure that we round 'outwards'
             * in all cases. */
            gs_fixed_edge lenew, renew;
            fixed ybl, ybr, ytl, ytr, ymid;

            /* Reduce the clipping region horizontally if possible. */
            if (re.start.x > re.end.x) {
                if (re.start.x < xright)
                    xright = re.start.x;
            } else if (re.end.x < xright)
                xright = re.end.x;
            if (le.start.x > le.end.x) {
                if (le.end.x > xleft)
                    xleft = le.end.x;
            } else if (le.start.x > xleft)
                xleft = le.start.x;

            ybot = max(ybot, min(le.start.y, re.start.y));
            ytop = min(ytop, max(le.end.y, re.end.y));
#if 0
            /* RJW: I've disabled this code a) because it doesn't make any
             * difference in the cluster tests, and b) because I think it's wrong.
             * Taking the first case as an example; just because the le.start.x
             * is > xright, does not mean that we can simply truncate the edge at
             * xright, as this may throw away part of the trap between ybot and
             * the new le.start.y. */
            /* Reduce the edges to the left/right of the clipping region. */
            /* Only in the 4 cases which can bring ytop/ybot closer */
            if (le.start.x > xright) {
                le.start.y += (fixed)((int64_t)(le.end.y-le.start.y)*
                                      (int64_t)(le.start.x-xright)/
                                      (int64_t)(le.start.x-le.end.x));
                le.start.x = xright;
            }
            if (re.start.x < xleft) {
                re.start.y += (fixed)((int64_t)(re.end.y-re.start.y)*
                                      (int64_t)(xleft-re.start.x)/
                                      (int64_t)(re.end.x-re.start.x));
                re.start.x = xleft;
            }
            if (le.end.x > xright) {
                le.end.y -= (fixed)((int64_t)(le.end.y-le.start.y)*
                                    (int64_t)(le.end.x-xright)/
                                    (int64_t)(le.end.x-le.start.x));
                le.end.x = xright;
            }
            if (re.end.x < xleft) {
                re.end.y -= (fixed)((int64_t)(re.end.y-re.start.y)*
                                    (int64_t)(xleft-re.end.x)/
                                    (int64_t)(re.start.x-re.end.x));
                re.end.x = xleft;
            }
#endif

            if (ybot >= ytop)
                return 0;
            /* Follow the edges in, so that le.start.y == ybot etc. */
            if (le.start.y < ybot) {
                int round = ((le.end.x < le.start.x) ?
                             (le.end.y-le.start.y-1) : 0);
                le.start.x += (fixed)(((int64_t)(ybot-le.start.y)*
                                       (int64_t)(le.end.x-le.start.x)-round)/
                                      (int64_t)(le.end.y-le.start.y));
                le.start.y = ybot;
            }
            if (le.end.y > ytop) {
                int round = ((le.end.x > le.start.x) ?
                             (le.end.y-le.start.y-1) : 0);
                le.end.x += (fixed)(((int64_t)(le.end.y-ytop)*
                                     (int64_t)(le.start.x-le.end.x)-round)/
                                    (int64_t)(le.end.y-le.start.y));
                le.end.y = ytop;
            }
            if ((le.start.x < xleft) && (le.end.x < xleft)) {
                le.start.x = xleft;
                le.end.x   = xleft;
                le.start.y = ybot;
                le.end.y   = ytop;
            }
            if (re.start.y < ybot) {
                int round = ((re.end.x > re.start.x) ?
                             (re.end.y-re.start.y-1) : 0);
                re.start.x += (fixed)(((int64_t)(ybot-re.start.y)*
                                       (int64_t)(re.end.x-re.start.x)+round)/
                                      (int64_t)(re.end.y-re.start.y));
                re.start.y = ybot;
            }
            if (re.end.y > ytop) {
                int round = ((re.end.x < re.start.x) ?
                             (re.end.y-re.start.y-1) : 0);
                re.end.x += (fixed)(((int64_t)(re.end.y-ytop)*
                                     (int64_t)(re.start.x-re.end.x)+round)/
                                    (int64_t)(re.end.y-re.start.y));
                re.end.y = ytop;
            }
            if ((re.start.x > xright) && (re.end.x > xright)) {
                re.start.x = xright;
                re.end.x   = xright;
                re.start.y = ybot;
                re.end.y   = ytop;
            }
            /* Now, check whether the left and right edges cross. Previously
             * this comment said: "This can only happen (for well formed
             * input) in the case where one of the edges was completely out
             * of range and has now been pulled in to the edge of the clip
             * region." I now do not believe this to be true. */
            if (le.start.x > re.start.x) {
                if (le.start.x == le.end.x) {
                    if (re.start.x == re.end.x)
                        return 0;
                    ybot += (fixed)((int64_t)(re.end.y-re.start.y)*
                                    (int64_t)(le.start.x-re.start.x)/
                                    (int64_t)(re.end.x-re.start.x));
                    re.start.x = le.start.x;
                } else {
                    ybot += (fixed)((int64_t)(le.end.y-le.start.y)*
                                    (int64_t)(le.start.x-re.start.x)/
                                    (int64_t)(le.start.x-le.end.x));
                    le.start.x = re.start.x;
                }
                if (ybot >= ytop)
                    return 0;
                le.start.y = ybot;
                re.start.y = ybot;
            }
            if (le.end.x > re.end.x) {
                if (le.start.x == le.end.x) {
                    if (re.start.x == re.end.x)
                        return 0;
                    ytop -= (fixed)((int64_t)(re.end.y-re.start.y)*
                                    (int64_t)(le.end.x-re.end.x)/
                                    (int64_t)(re.start.x-re.end.x));
                    re.end.x = le.end.x;
                } else {
                    ytop -= (fixed)((int64_t)(le.end.y-le.start.y)*
                                    (int64_t)(le.end.x-re.end.x)/
                                    (int64_t)(le.end.x-le.start.x));
                    le.end.x = re.end.x;
                }
                if (ybot >= ytop)
                    return 0;
                le.end.y = ytop;
                re.end.y = ytop;
            }
            /* At this point we are guaranteed that le and re are constrained
             * as tightly as possible to the ybot/ytop range, and that the
             * entire ybot/ytop range will be marked at least somewhere. All
             * we need to do now is to actually fill the region.
             */
            lenew.start.x = xleft;
            lenew.start.y = ybot;
            lenew.end.x   = xleft;
            lenew.end.y   = ytop;
            renew.start.x = xright;
            renew.start.y = ybot;
            renew.end.x   = xright;
            renew.end.y   = ytop;
            /* Figure out where the left edge intersects with the left at
             * the bottom */
            ybl = ybot;
            if (le.start.x > le.end.x) {
                ybl += (fixed)((int64_t)(le.start.x-xleft) *
                               (int64_t)(le.end.y-le.start.y) /
                               (int64_t)(le.start.x-le.end.x));
                if (ybl > ytop)
                    ybl = ytop;
            }
            /* Figure out where the right edge intersects with the right at
             * the bottom */
            ybr = ybot;
            if (re.start.x < re.end.x) {
                ybr += (fixed)((int64_t)(xright-re.start.x) *
                               (int64_t)(re.end.y-re.start.y) /
                               (int64_t)(re.end.x-re.start.x));
                if (ybr > ytop)
                    ybr = ytop;
            }
            /* Figure out where the left edge intersects with the left at
             * the top */
            ytl = ytop;
            if (le.end.x > le.start.x) {
                ytl -= (fixed)((int64_t)(le.end.x-xleft) *
                               (int64_t)(le.end.y-le.start.y) /
                               (int64_t)(le.end.x-le.start.x));
                if (ytl < ybot)
                    ytl = ybot;
            }
            /* Figure out where the right edge intersects with the right at
             * the bottom */
            ytr = ytop;
            if (re.end.x < re.start.x) {
                ytr -= (fixed)((int64_t)(xright-re.end.x) *
                               (int64_t)(re.end.y-re.start.y) /
                               (int64_t)(re.start.x-re.end.x));
                if (ytr < ybot)
                    ytr = ybot;
            }
            /* Check for the 2 cases where top and bottom diagonal extents
             * overlap, and deal with them explicitly. */
            if (ytl < ybr) {
                /*     |     |
                 *  ---+-----+---
                 *     | /222|
                 *     |/111/|
                 *     |000/ |
                 *  ---+-----+---
                 *     |     |
                 */
                code = dev_proc(pfs->dev, fill_trapezoid)(pfs->dev,
                                        &lenew, &re, ybot, ytl,
                                        swap_axes, pdevc, pfs->pgs->log_op);
                if (code < 0)
                    return code;
                code = dev_proc(pfs->dev, fill_trapezoid)(pfs->dev,
                                        &le, &re, ytl, ybr,
                                        swap_axes, pdevc, pfs->pgs->log_op);
                if (code < 0)
                    return code;
                ybot = ybr;
                return dev_proc(pfs->dev, fill_trapezoid)(pfs->dev,
                                        &le, &renew, ybr, ytop,
                                        swap_axes, pdevc, pfs->pgs->log_op);
            } else if (ytr < ybl) {
                /*     |     |
                 *  ---+-----+----
                 *     |555\ |
                 *     |\444\|
                 *     | \333|
                 *  ---+-----+---
                 *     |     |
                 */
                code = dev_proc(pfs->dev, fill_trapezoid)(pfs->dev,
                                        &le, &renew, ybot, ytr,
                                        swap_axes, pdevc, pfs->pgs->log_op);
                if (code < 0)
                    return code;
                code = dev_proc(pfs->dev, fill_trapezoid)(pfs->dev,
                                        &le, &re, ytr, ybl,
                                        swap_axes, pdevc, pfs->pgs->log_op);
                if (code < 0)
                    return code;
                return dev_proc(pfs->dev, fill_trapezoid)(pfs->dev,
                                        &le, &re, ybl, ytop,
                                        swap_axes, pdevc, pfs->pgs->log_op);
            }
            /* Fill in any section where both left and right edges are
             * diagonal at the bottom */
            ymid = ybl;
            if (ymid > ybr)
                ymid = ybr;
            if (ymid > ybot) {
                /*     |\   |          |   /|
                 *     | \6/|    or    |\6/ |
                 *  ---+----+---    ---+----+---
                 *     |    |          |    |
                 */
                code = dev_proc(pfs->dev, fill_trapezoid)(pfs->dev,
                                        &le, &re, ybot, ymid,
                                        swap_axes, pdevc, pfs->pgs->log_op);
                if (code < 0)
                    return code;
                ybot = ymid;
            }
            /* Fill in any section where both left and right edges are
             * diagonal at the top */
            ymid = ytl;
            if (ymid < ytr)
                ymid = ytr;
            if (ymid < ytop) {
                /*     |    |          |    |
                 *  ---+----+---    ---+----+---
                 *     |/7\ |    or    | /7\|
                 *     |   \|          |/   |
                 */
                code = dev_proc(pfs->dev, fill_trapezoid)(pfs->dev,
                                        &le, &re, ymid, ytop,
                                        swap_axes, pdevc, pfs->pgs->log_op);
                if (code < 0)
                    return code;
                ytop = ymid;
            }
            /* Now do the single diagonal cases at the bottom */
            if (ybl > ybot) {
                /*     |    |
                 *     |\666|
                 *  ---+----+---
                 *     |    |
                 */
                code = dev_proc(pfs->dev, fill_trapezoid)(pfs->dev,
                                        &le, &renew, ybot, ybl,
                                        swap_axes, pdevc, pfs->pgs->log_op);
                if (code < 0)
                    return code;
                ybot = ybl;
            } else if (ybr > ybot) {
                /*     |    |
                 *     |777/|
                 *  ---+----+---
                 *     |    |
                 */
                code = dev_proc(pfs->dev, fill_trapezoid)(pfs->dev,
                                        &lenew, &re, ybot, ybr,
                                        swap_axes, pdevc, pfs->pgs->log_op);
                if (code < 0)
                    return code;
                ybot = ybr;
            }
            /* Now do the single diagonal cases at the top */
            if (ytl < ytop) {
                /*     |    |
                 *  ---+----+---
                 *     |/888|
                 *     |    |
                 */
                code = dev_proc(pfs->dev, fill_trapezoid)(pfs->dev,
                                        &le, &renew, ytl, ytop,
                                        swap_axes, pdevc, pfs->pgs->log_op);
                if (code < 0)
                    return code;
                ytop = ytl;
            } else if (ytr < ytop) {
                /*     |    |
                 *  ---+----+---
                 *     |999\|
                 *     |    |
                 */
                code = dev_proc(pfs->dev, fill_trapezoid)(pfs->dev,
                                        &lenew, &re, ytr, ytop,
                                        swap_axes, pdevc, pfs->pgs->log_op);
                if (code < 0)
                    return code;
                ytop = ytr;
            }
            /* And finally just whatever rectangular section is left over in
             * the middle */
            if (ybot > ytop)
                return 0;
            return dev_proc(pfs->dev, fill_trapezoid)(pfs->dev,
                                        &lenew, &renew, ybot, ytop,
                                        swap_axes, pdevc, pfs->pgs->log_op);
        }
    }
    return dev_proc(pfs->dev, fill_trapezoid)(pfs->dev,
            &le, &re, ybot, ytop, swap_axes, pdevc, pfs->pgs->log_op);
}

static inline void
dc2fc31(const patch_fill_state_t *pfs, gx_device_color *pdevc,
            frac31 fc[GX_DEVICE_COLOR_MAX_COMPONENTS])
{
    int j;
    const gx_device_color_info *cinfo = &pfs->trans_device->color_info;
    /* Note trans device is actually either the transparency parent
       device if transparency is present or the target device.  Basically
       the device from which we want to get the color information from
       for this */

    if (pdevc->type == &gx_dc_type_data_pure) {
        for (j = 0; j < cinfo->num_components; j++) {
                int shift = cinfo->comp_shift[j];
                int bits = cinfo->comp_bits[j];

                fc[j] = ((pdevc->colors.pure >> shift) & ((1 << bits) - 1)) <<
                        (sizeof(frac31) * 8 - 1 - bits);
        }
    } else {
        for (j = 0; j < cinfo->num_components; j++) {
                fc[j] = cv2frac31(pdevc->colors.devn.values[j]);
        }
    }
}

#define DEBUG_COLOR_INDEX_CACHE 0

static inline int
patch_color_to_device_color_inline(const patch_fill_state_t *pfs,
                                   const patch_color_t *c, gx_device_color *pdevc,
                                   frac31 *frac_values)
{
    /* Must return 2 if the color is not pure.
       See try_device_linear_color.
     */
    int code;
    gx_device_color devc;

#ifdef PACIFY_VALGRIND
    /* This is a hack to get us around some valgrind warnings seen
     * when transparency is in use with the clist. We run through
     * the shading code dealing with pfs->num_components components.
     * I believe this is intended to match the source space of the
     * shading, as we have to perform all shadings in the source
     * space initially, and only convert after decomposition.
     * When this reaches down to the clist writing phase, the
     * clist writes pfs->dev->color_info.num_components color
     * components to the clist. In the example I am using
     *  pfs->num_components = 1
     *  pfs->dev->color_info.num_components=3
     * So valgrind complains that 2 of the 3 color components
     * it is writing are uninitialised. Magically, it appears
     * not to actually use them when read back though, so
     * it suffices to blank them to kill the warnings now.
     * If pfs->num_components > pfs->dev->color_info.num_components
     * then we'll not be writing enough information to the clist
     * and so hopefully we'll see bad rendering!
     *
     * An example that shows why this is required:
     *  make gsdebugvg
     *  valgrind --track-origins=yes debugbin/gs -sOutputFile=test.ps
     *    -dMaxBitmap=1000 -sDEVICE=ps2write  -r300  -Z: -dNOPAUSE
     *    -dBATCH -K2000000 -dClusterJob Bug693480.pdf
     * (though ps2write is not implicated here).
     */
     if (frac_values) {
        int i;
        int n = pfs->dev->color_info.num_components;
        for (i = pfs->num_components; i < n; i++) {
            frac_values[i] = 0;
        }
    }
#endif

    if (pdevc == NULL)
        pdevc = &devc;
    pdevc->tag = pfs->dev->graphics_type_tag;
    if (DEBUG_COLOR_INDEX_CACHE || pfs->pcic == NULL) {
#       if DEBUG_COLOR_INDEX_CACHE
        gx_color_index cindex = pdevc->colors.pure;
#       endif
        gs_client_color fcc;
        const gs_color_space *pcs = pfs->direct_space;

        if (pcs != NULL) {
            memcpy(fcc.paint.values, c->cc.paint.values,
                        sizeof(fcc.paint.values[0]) * pfs->num_components);
            code = pcs->type->remap_color(&fcc, pcs, pdevc, pfs->pgs,
                                      pfs->trans_device, gs_color_select_texture);
            if (code < 0)
                return code;
            if (frac_values != NULL) {
                if (!(pdevc->type == &gx_dc_type_data_devn ||
                      pdevc->type == &gx_dc_type_data_pure))
                    return 2;
                dc2fc31(pfs, pdevc, frac_values);
            }
#           if DEBUG_COLOR_INDEX_CACHE
            if (cindex != pdevc->colors.pure)
                return_error(gs_error_unregistered);
#           endif
        } else {
            /* This is reserved for future extension,
            when a linear color triangle with frac31 colors is being decomposed
            during a clist rasterization. In this case frac31 colors are written into
            the patch color, and pcs==NULL means an identity color mapping.
            For a while we assume here pfs->pcic is also NULL. */
            int j;
            const gx_device_color_info *cinfo = &pfs->dev->color_info;

            for (j = 0; j < cinfo->num_components; j++)
                frac_values[j] = (frac31)c->cc.paint.values[j];
            pdevc->type = &gx_dc_type_data_pure;
        }
    }
    return 0;
}

int
patch_color_to_device_color(const patch_fill_state_t *pfs, const patch_color_t *c, gx_device_color *pdevc)
{
    return patch_color_to_device_color_inline(pfs, c, pdevc, NULL);
}

static inline double
color_span(const patch_fill_state_t *pfs, const patch_color_t *c0, const patch_color_t *c1)
{
    int n = pfs->num_components, i;
    double m;

    /* Dont want to copy colors, which are big things. */
    m = any_abs(c1->cc.paint.values[0] - c0->cc.paint.values[0]) / pfs->color_domain.paint.values[0];
    for (i = 1; i < n; i++)
        m = max(m, any_abs(c1->cc.paint.values[i] - c0->cc.paint.values[i]) / pfs->color_domain.paint.values[i]);
    return m;
}

static inline void
color_diff(const patch_fill_state_t *pfs, const patch_color_t *c0, const patch_color_t *c1, patch_color_t *d)
{
    int n = pfs->num_components, i;

    for (i = 0; i < n; i++)
        d->cc.paint.values[i] = c1->cc.paint.values[i] - c0->cc.paint.values[i];
}

static inline double
color_norm(const patch_fill_state_t *pfs, const patch_color_t *c)
{
    int n = pfs->num_components, i;
    double m;

    m = any_abs(c->cc.paint.values[0]) / pfs->color_domain.paint.values[0];
    for (i = 1; i < n; i++)
        m = max(m, any_abs(c->cc.paint.values[i]) / pfs->color_domain.paint.values[i]);
    return m;
}

static inline int
isnt_color_monotonic(const patch_fill_state_t *pfs, const patch_color_t *c0, const patch_color_t *c1)
{   /* checks whether the color is monotonic in the n-dimensional interval,
       where n is the number of parameters in c0->t, c1->t.
       returns : 0 = monotonic,
       bit 0 = not or don't know by t0,
       bit 1 = not or don't know by t1,
       <0 = error. */
    /* When pfs->Function is not set, the color is monotonic.
       In this case do not call this function because
       it doesn't check whether pfs->Function is set.
       Actually pfs->monotonic_color prevents that.
     */
    /* This assumes that the color space is always monotonic.
       Non-monotonic color spaces are not recommended by PLRM,
       and the result with them may be imprecise.
     */
    uint mask;
    int code = gs_function_is_monotonic(pfs->Function, c0->t, c1->t, &mask);

    if (code >= 0)
        return mask;
    return code;
}

static inline bool
covers_pixel_centers(fixed ybot, fixed ytop)
{
    return fixed_pixround(ybot) < fixed_pixround(ytop);
}

static inline int
constant_color_trapezoid(patch_fill_state_t *pfs, gs_fixed_edge *le, gs_fixed_edge *re,
        fixed ybot, fixed ytop, bool swap_axes, const patch_color_t *c)
{
    gx_device_color dc;
    int code;

#   if NOFILL_TEST
        /* if (dbg_nofill)
                return 0; */
#   endif

    code = patch_color_to_device_color_inline(pfs, c, &dc, NULL);
    if (code < 0)
        return code;

    dc.tag = device_current_tag(pfs->dev);

    return dev_proc(pfs->dev, fill_trapezoid)(pfs->dev,
        le, re, ybot, ytop, swap_axes, &dc, pfs->pgs->log_op);
}

static inline float
function_linearity(const patch_fill_state_t *pfs, const patch_color_t *c0, const patch_color_t *c1)
{
    float s = 0;

    if (pfs->Function != NULL) {
        patch_color_t c;
        /* Solaris 9 (Sun C 5.5) compiler cannot initialize a 'const' */
        /* unless it is 'static const' */
        static const float q[2] = {(float)0.3, (float)0.7};
        int i, j;

        for (j = 0; j < count_of(q); j++) {
            c.t[0] = c0->t[0] * (1 - q[j]) + c1->t[0] * q[j];
            c.t[1] = c0->t[1] * (1 - q[j]) + c1->t[1] * q[j];
            patch_resolve_color_inline(&c, pfs);
            for (i = 0; i < pfs->num_components; i++) {
                float v = c0->cc.paint.values[i] * (1 - q[j]) + c1->cc.paint.values[i] * q[j];
                float d = v - c.cc.paint.values[i];
                float s1 = any_abs(d) / pfs->color_domain.paint.values[i];

                if (s1 > pfs->smoothness)
                    return s1;
                if (s < s1)
                    s = s1;
            }
        }
    }
    return s;
}

static inline int
is_color_linear(const patch_fill_state_t *pfs, const patch_color_t *c0, const patch_color_t *c1)
{   /* returns : 1 = linear, 0 = unlinear, <0 = error. */
    if (pfs->unlinear)
        return 1; /* Disable this check. */
    else {
        const gs_color_space *cs = pfs->direct_space;
        int code;
        float s = function_linearity(pfs, c0, c1);

        if (s > pfs->smoothness)
            return 0;
        if (pfs->cs_always_linear)
            return 1;
        code = cs_is_linear(cs, pfs->pgs, pfs->trans_device,
                &c0->cc, &c1->cc, NULL, NULL, pfs->smoothness - s, pfs->icclink);
        if (code <= 0)
            return code;
        return 1;
    }
}

static int
decompose_linear_color(patch_fill_state_t *pfs, gs_fixed_edge *le, gs_fixed_edge *re,
        fixed ybot, fixed ytop, bool swap_axes, const patch_color_t *c0,
        const patch_color_t *c1)
{
    /* Assuming a very narrow trapezoid - ignore the transversal color variation. */
    /* Assuming the XY span is restricted with curve_samples.
       It is important for intersection_of_small_bars to compute faster. */
    int code;
    patch_color_t *c;
    byte *color_stack_ptr;
    bool save_inside = pfs->inside;

    if (!pfs->inside) {
        gs_fixed_rect r, r1;

        if(swap_axes) {
            r.p.y = min(le->start.x, le->end.x);
            r.p.x = min(le->start.y, le->end.y);
            r.q.y = max(re->start.x, re->end.x);
            r.q.x = max(re->start.y, re->end.y);
        } else {
            r.p.x = min(le->start.x, le->end.x);
            r.p.y = min(le->start.y, le->end.y);
            r.q.x = max(re->start.x, re->end.x);
            r.q.y = max(re->start.y, re->end.y);
        }
        r1 = r;
        rect_intersect(r, pfs->rect);
        if (r.q.x <= r.p.x || r.q.y <= r.p.y)
            return 0;
        if (r1.p.x == r.p.x && r1.p.y == r.p.y &&
            r1.q.x == r.q.x && r1.q.y == r.q.y)
            pfs->inside = true;
    }
    color_stack_ptr = reserve_colors_inline(pfs, &c, 1);
    if (color_stack_ptr == NULL)
        return_error(gs_error_unregistered); /* Must not happen. */
    /* Use the recursive decomposition due to isnt_color_monotonic
       based on fn_is_monotonic_proc_t is_monotonic,
       which applies to intervals. */
    patch_interpolate_color(c, c0, c1, pfs, 0.5);
    if (ytop - ybot < pfs->decomposition_limit) /* Prevent an infinite color decomposition. */
        code = constant_color_trapezoid(pfs, le, re, ybot, ytop, swap_axes, c);
    else {
        bool monotonic_color_save = pfs->monotonic_color;
        bool linear_color_save = pfs->linear_color;

        if (!pfs->monotonic_color) {
            code = isnt_color_monotonic(pfs, c0, c1);
            if (code < 0)
                goto out;
            if (!code)
                pfs->monotonic_color = true;
        }
        if (pfs->monotonic_color && !pfs->linear_color) {
            code = is_color_linear(pfs, c0, c1);
            if (code < 0)
                goto out;
            if (code > 0)
                pfs->linear_color =  true;
        }
        if (!pfs->unlinear && pfs->linear_color) {
            gx_device *pdev = pfs->dev;
            frac31 fc[2][GX_DEVICE_COLOR_MAX_COMPONENTS];
            gs_fill_attributes fa;
            gs_fixed_rect clip;

            memset(fc, 0x99, sizeof(fc));

            clip = pfs->rect;
            if (swap_axes) {
                fixed v;

                v = clip.p.x; clip.p.x = clip.p.y; clip.p.y = v;
                v = clip.q.x; clip.q.x = clip.q.y; clip.q.y = v;
                /* Don't need adjust_swapped_boundary here. */
            }
            clip.p.y = max(clip.p.y, ybot);
            clip.q.y = min(clip.q.y, ytop);
            fa.clip = &clip;
            fa.ht = NULL;
            fa.swap_axes = swap_axes;
            fa.lop = 0;
            fa.ystart = ybot;
            fa.yend = ytop;
            code = patch_color_to_device_color_inline(pfs, c0, NULL, fc[0]);
            if (code < 0)
                goto out;
            if (code == 2) {
                /* Must not happen. */
                code=gs_note_error(gs_error_unregistered);
                goto out;
            }
            code = patch_color_to_device_color_inline(pfs, c1, NULL, fc[1]);
            if (code < 0)
                goto out;
            code = dev_proc(pdev, fill_linear_color_trapezoid)(pdev, &fa,
                            &le->start, &le->end, &re->start, &re->end,
                            fc[0], fc[1], NULL, NULL);
            if (code == 1) {
                pfs->monotonic_color = monotonic_color_save;
                pfs->linear_color = linear_color_save;
                code = 0; /* The area is filled. */
                goto out;
            }
            if (code < 0)
                goto out;
            else {      /* code == 0, the device requested to decompose the area. */
                code = gs_note_error(gs_error_unregistered); /* Must not happen. */
                goto out;
            }
        }
        if (!pfs->unlinear || !pfs->linear_color ||
                color_span(pfs, c0, c1) > pfs->smoothness) {
            fixed y = (ybot + ytop) / 2;

            code = decompose_linear_color(pfs, le, re, ybot, y, swap_axes, c0, c);
            if (code >= 0)
                code = decompose_linear_color(pfs, le, re, y, ytop, swap_axes, c, c1);
        } else
            code = constant_color_trapezoid(pfs, le, re, ybot, ytop, swap_axes, c);
        pfs->monotonic_color = monotonic_color_save;
        pfs->linear_color = linear_color_save;
    }
out:
    pfs->inside = save_inside;
    release_colors_inline(pfs, color_stack_ptr, 1);
    return code;
}

static inline int
linear_color_trapezoid(patch_fill_state_t *pfs, gs_fixed_point q[4], int i0, int i1, int i2, int i3,
                fixed ybot, fixed ytop, bool swap_axes, const patch_color_t *c0, const patch_color_t *c1,
                bool orient)
{
    /* Assuming a very narrow trapezoid - ignore the transversal color change. */
    gs_fixed_edge le, re;

    make_trapezoid(q, i0, i1, i2, i3, ybot, ytop, swap_axes, orient, &le, &re);
    return decompose_linear_color(pfs, &le, &re, ybot, ytop, swap_axes, c0, c1);
}

static int
wedge_trap_decompose(patch_fill_state_t *pfs, gs_fixed_point q[4],
        fixed ybot, fixed ytop, const patch_color_t *c0, const patch_color_t *c1,
        bool swap_axes, bool self_intersecting)
{
    /* Assuming a very narrow trapezoid - ignore the transversal color change. */
    fixed dx1, dy1, dx2, dy2;
    bool orient;

    if (!pfs->vectorization && !covers_pixel_centers(ybot, ytop))
        return 0;
    if (ybot == ytop)
        return 0;
    dx1 = q[1].x - q[0].x, dy1 = q[1].y - q[0].y;
    dx2 = q[2].x - q[0].x, dy2 = q[2].y - q[0].y;
    if ((int64_t)dx1 * dy2 != (int64_t)dy1 * dx2) {
        orient = ((int64_t)dx1 * dy2 > (int64_t)dy1 * dx2);
        return linear_color_trapezoid(pfs, q, 0, 1, 2, 3, ybot, ytop, swap_axes, c0, c1, orient);
    } else {
        fixed dx3 = q[3].x - q[0].x, dy3 = q[3].y - q[0].y;

        orient = ((int64_t)dx1 * dy3 > (int64_t)dy1 * dx3);
        return linear_color_trapezoid(pfs, q, 0, 1, 2, 3, ybot, ytop, swap_axes, c0, c1, orient);
    }
}

static inline int
fill_wedge_trap(patch_fill_state_t *pfs, const gs_fixed_point *p0, const gs_fixed_point *p1,
            const gs_fixed_point *q0, const gs_fixed_point *q1, const patch_color_t *c0, const patch_color_t *c1,
            bool swap_axes, bool self_intersecting)
{
    /* We assume that the width of the wedge is close to zero,
       so we can ignore the slope when computing transversal distances. */
    gs_fixed_point p[4];
    const patch_color_t *cc0, *cc1;

    if (p0->y < p1->y) {
        p[2] = *p0;
        p[3] = *p1;
        cc0 = c0;
        cc1 = c1;
    } else {
        p[2] = *p1;
        p[3] = *p0;
        cc0 = c1;
        cc1 = c0;
    }
    p[0] = *q0;
    p[1] = *q1;
    return wedge_trap_decompose(pfs, p, p[2].y, p[3].y, cc0, cc1, swap_axes, self_intersecting);
}

static void
split_curve_s(const gs_fixed_point *pole, gs_fixed_point *q0, gs_fixed_point *q1, int pole_step)
{
    /*  This copies a code fragment from split_curve_midpoint,
        substituting another data type.
     */
    /*
     * We have to define midpoint carefully to avoid overflow.
     * (If it overflows, something really pathological is going
     * on, but we could get infinite recursion that way....)
     */
#define midpoint(a,b)\
  (arith_rshift_1(a) + arith_rshift_1(b) + (((a) | (b)) & 1))
    fixed x12 = midpoint(pole[1 * pole_step].x, pole[2 * pole_step].x);
    fixed y12 = midpoint(pole[1 * pole_step].y, pole[2 * pole_step].y);

    /* q[0] and q[1] must not be the same as pole. */
    q0[1 * pole_step].x = midpoint(pole[0 * pole_step].x, pole[1 * pole_step].x);
    q0[1 * pole_step].y = midpoint(pole[0 * pole_step].y, pole[1 * pole_step].y);
    q1[2 * pole_step].x = midpoint(pole[2 * pole_step].x, pole[3 * pole_step].x);
    q1[2 * pole_step].y = midpoint(pole[2 * pole_step].y, pole[3 * pole_step].y);
    q0[2 * pole_step].x = midpoint(q0[1 * pole_step].x, x12);
    q0[2 * pole_step].y = midpoint(q0[1 * pole_step].y, y12);
    q1[1 * pole_step].x = midpoint(x12, q1[2 * pole_step].x);
    q1[1 * pole_step].y = midpoint(y12, q1[2 * pole_step].y);
    q0[0 * pole_step].x = pole[0 * pole_step].x;
    q0[0 * pole_step].y = pole[0 * pole_step].y;
    q0[3 * pole_step].x = q1[0 * pole_step].x = midpoint(q0[2 * pole_step].x, q1[1 * pole_step].x);
    q0[3 * pole_step].y = q1[0 * pole_step].y = midpoint(q0[2 * pole_step].y, q1[1 * pole_step].y);
    q1[3 * pole_step].x = pole[3 * pole_step].x;
    q1[3 * pole_step].y = pole[3 * pole_step].y;
#undef midpoint
}

static void
split_curve(const gs_fixed_point pole[4], gs_fixed_point q0[4], gs_fixed_point q1[4])
{
    split_curve_s(pole, q0, q1, 1);
}

#ifdef SHADING_SWAP_AXES_FOR_PRECISION
static inline void
do_swap_axes(gs_fixed_point *p, int k)
{
    int i;

    for (i = 0; i < k; i++) {
        p[i].x ^= p[i].y; p[i].y ^= p[i].x; p[i].x ^= p[i].y;
    }
}

static inline fixed
span_x(const gs_fixed_point *p, int k)
{
    int i;
    fixed xmin = p[0].x, xmax = p[0].x;

    for (i = 1; i < k; i++) {
        xmin = min(xmin, p[i].x);
        xmax = max(xmax, p[i].x);
    }
    return xmax - xmin;
}

static inline fixed
span_y(const gs_fixed_point *p, int k)
{
    int i;
    fixed ymin = p[0].y, ymax = p[0].y;

    for (i = 1; i < k; i++) {
        ymin = min(ymin, p[i].y);
        ymax = max(ymax, p[i].y);
    }
    return ymax - ymin;
}
#endif

static inline fixed
manhattan_dist(const gs_fixed_point *p0, const gs_fixed_point *p1)
{
    fixed dx = any_abs(p1->x - p0->x), dy = any_abs(p1->y - p0->y);

    return max(dx, dy);
}

static inline int
create_wedge_vertex_list(patch_fill_state_t *pfs, wedge_vertex_list_t *l,
        const gs_fixed_point *p0, const gs_fixed_point *p1)
{
    if (l->end != NULL)
        return_error(gs_error_unregistered); /* Must not happen. */
    l->beg = wedge_vertex_list_elem_reserve(pfs);
    l->end = wedge_vertex_list_elem_reserve(pfs);
    if (l->beg == NULL)
        return_error(gs_error_unregistered); /* Must not happen. */
    if (l->end == NULL)
        return_error(gs_error_unregistered); /* Must not happen. */
    l->beg->prev = l->end->next = NULL;
    l->beg->next = l->end;
    l->end->prev = l->beg;
    l->beg->p = *p0;
    l->end->p = *p1;
    l->beg->level = l->end->level = 0;
    return 0;
}

static inline int
insert_wedge_vertex_list_elem(patch_fill_state_t *pfs, wedge_vertex_list_t *l,
                              const gs_fixed_point *p, wedge_vertex_list_elem_t **r)
{
    wedge_vertex_list_elem_t *e = wedge_vertex_list_elem_reserve(pfs);

    /* We have got enough free elements due to the preliminary decomposition
       of curves to LAZY_WEDGES_MAX_LEVEL, see curve_samples. */
    if (e == NULL)
        return_error(gs_error_unregistered); /* Must not happen. */
    if (l->beg->next != l->end)
        return_error(gs_error_unregistered); /* Must not happen. */
    if (l->end->prev != l->beg)
        return_error(gs_error_unregistered); /* Must not happen. */
    e->next = l->end;
    e->prev = l->beg;
    e->p = *p;
    e->level = max(l->beg->level, l->end->level) + 1;
    e->divide_count = 0;
    l->beg->next = l->end->prev = e;
    {   int sx = l->beg->p.x < l->end->p.x ? 1 : -1;
        int sy = l->beg->p.y < l->end->p.y ? 1 : -1;

        if ((p->x - l->beg->p.x) * sx < 0)
            return_error(gs_error_unregistered); /* Must not happen. */
        if ((p->y - l->beg->p.y) * sy < 0)
            return_error(gs_error_unregistered); /* Must not happen. */
        if ((l->end->p.x - p->x) * sx < 0)
            return_error(gs_error_unregistered); /* Must not happen. */
        if ((l->end->p.y - p->y) * sy < 0)
            return_error(gs_error_unregistered); /* Must not happen. */
    }
    *r = e;
    return 0;
}

static inline int
open_wedge_median(patch_fill_state_t *pfs, wedge_vertex_list_t *l,
        const gs_fixed_point *p0, const gs_fixed_point *p1, const gs_fixed_point *pm,
        wedge_vertex_list_elem_t **r)
{
    wedge_vertex_list_elem_t *e;
    int code;

    if (!l->last_side) {
        if (l->beg == NULL) {
            code = create_wedge_vertex_list(pfs, l, p0, p1);
            if (code < 0)
                return code;
        }
        if (l->beg->p.x != p0->x)
            return_error(gs_error_unregistered); /* Must not happen. */
        if (l->beg->p.y != p0->y)
            return_error(gs_error_unregistered); /* Must not happen. */
        if (l->end->p.x != p1->x)
            return_error(gs_error_unregistered); /* Must not happen. */
        if (l->end->p.y != p1->y)
            return_error(gs_error_unregistered); /* Must not happen. */
        code = insert_wedge_vertex_list_elem(pfs, l, pm, &e);
        if (code < 0)
            return code;
        e->divide_count++;
    } else if (l->beg == NULL) {
        code = create_wedge_vertex_list(pfs, l, p1, p0);
        if (code < 0)
            return code;
        code = insert_wedge_vertex_list_elem(pfs, l, pm, &e);
        if (code < 0)
            return code;
        e->divide_count++;
    } else {
        if (l->beg->p.x != p1->x)
            return_error(gs_error_unregistered); /* Must not happen. */
        if (l->beg->p.y != p1->y)
            return_error(gs_error_unregistered); /* Must not happen. */
        if (l->end->p.x != p0->x)
            return_error(gs_error_unregistered); /* Must not happen. */
        if (l->end->p.y != p0->y)
            return_error(gs_error_unregistered); /* Must not happen. */
        if (l->beg->next == l->end) {
            code = insert_wedge_vertex_list_elem(pfs, l, pm, &e);
            if (code < 0)
                return code;
            e->divide_count++;
        } else {
            e = wedge_vertex_list_find(l->beg, l->end,
                        max(l->beg->level, l->end->level) + 1);
            if (e == NULL)
                return_error(gs_error_unregistered); /* Must not happen. */
            if (e->p.x != pm->x || e->p.y != pm->y)
                return_error(gs_error_unregistered); /* Must not happen. */
            e->divide_count++;
        }
    }
    *r = e;
    return 0;
}

static inline int
make_wedge_median(patch_fill_state_t *pfs, wedge_vertex_list_t *l,
        wedge_vertex_list_t *l0, bool forth,
        const gs_fixed_point *p0, const gs_fixed_point *p1, const gs_fixed_point *pm)
{
    int code;

    l->last_side = l0->last_side;
    if (!l->last_side ^ !forth) {
        code = open_wedge_median(pfs, l0, p0, p1, pm, &l->end);
        l->beg = l0->beg;
    } else {
        code = open_wedge_median(pfs, l0, p0, p1, pm, &l->beg);
        l->end = l0->end;
    }
    return code;
}

static int fill_wedge_from_list(patch_fill_state_t *pfs, const wedge_vertex_list_t *l,
            const patch_color_t *c0, const patch_color_t *c1);

static inline int
close_wedge_median(patch_fill_state_t *pfs, wedge_vertex_list_t *l,
        const patch_color_t *c0, const patch_color_t *c1)
{
    int code;

    if (!l->last_side)
        return 0;
    code = fill_wedge_from_list(pfs, l, c1, c0);
    if (code < 0)
        return code;
    release_wedge_vertex_list_interval(pfs, l->beg, l->end);
    return 0;
}

static inline void
move_wedge(wedge_vertex_list_t *l, const wedge_vertex_list_t *l0, bool forth)
{
    if (!l->last_side ^ !forth) {
        l->beg = l->end;
        l->end = l0->end;
    } else {
        l->end = l->beg;
        l->beg = l0->beg;
    }
}

static inline int
fill_triangle_wedge_aux(patch_fill_state_t *pfs,
            const shading_vertex_t *q0, const shading_vertex_t *q1, const shading_vertex_t *q2)
{   int code;
    const gs_fixed_point *p0, *p1, *p2;
    gs_fixed_point qq0, qq1, qq2;
    fixed dx = any_abs(q0->p.x - q1->p.x), dy = any_abs(q0->p.y - q1->p.y);
    bool swap_axes;

#   if SKIP_TEST
        dbg_wedge_triangle_cnt++;
#   endif
    if (dx > dy) {
        swap_axes = true;
        qq0.x = q0->p.y;
        qq0.y = q0->p.x;
        qq1.x = q1->p.y;
        qq1.y = q1->p.x;
        qq2.x = q2->p.y;
        qq2.y = q2->p.x;
        p0 = &qq0;
        p1 = &qq1;
        p2 = &qq2;
    } else {
        swap_axes = false;
        p0 = &q0->p;
        p1 = &q1->p;
        p2 = &q2->p;
    }
    /* We decompose the thin triangle into 2 thin trapezoids.
       An optimization with decomposing into 2 triangles
       appears low useful, because the self_intersecting argument
       with inline expansion does that job perfectly. */
    if (p0->y < p1->y) {
        code = fill_wedge_trap(pfs, p0, p2, p0, p1, q0->c, q2->c, swap_axes, false);
        if (code < 0)
            return code;
        return fill_wedge_trap(pfs, p2, p1, p0, p1, q2->c, q1->c, swap_axes, false);
    } else {
        code = fill_wedge_trap(pfs, p0, p2, p1, p0, q0->c, q2->c, swap_axes, false);
        if (code < 0)
            return code;
        return fill_wedge_trap(pfs, p2, p1, p1, p0, q2->c, q1->c, swap_axes, false);
    }
}

static inline int
try_device_linear_color(patch_fill_state_t *pfs, bool wedge,
        const shading_vertex_t *p0, const shading_vertex_t *p1,
        const shading_vertex_t *p2)
{
    /*  Returns :
        <0 - error;
        0 - success;
        1 - decompose to linear color areas;
        2 - decompose to constant color areas;
     */
    int code;

    if (pfs->unlinear)
        return 2;
    if (!wedge) {
        const gs_color_space *cs = pfs->direct_space;

        if (cs != NULL) {
            float s0, s1, s2, s01, s012;

            s0 = function_linearity(pfs, p0->c, p1->c);
            if (s0 > pfs->smoothness)
                return 1;
            s1 = function_linearity(pfs, p1->c, p2->c);
            if (s1 > pfs->smoothness)
                return 1;
            s2 = function_linearity(pfs, p2->c, p0->c);
            if (s2 > pfs->smoothness)
                return 1;
            /* fixme: check an inner color ? */
            s01 = max(s0, s1);
            s012 = max(s01, s2);
            if (pfs->cs_always_linear)
                code = 1;
            else
                code = cs_is_linear(cs, pfs->pgs, pfs->trans_device,
                                  &p0->c->cc, &p1->c->cc, &p2->c->cc, NULL,
                                  pfs->smoothness - s012, pfs->icclink);
            if (code < 0)
                return code;
            if (code == 0)
                return 1;
        }
    }
    {   gx_device *pdev = pfs->dev;
        frac31 fc[3][GX_DEVICE_COLOR_MAX_COMPONENTS];
        gs_fill_attributes fa;
        gx_device_color dc[3];

        fa.clip = &pfs->rect;
        fa.ht = NULL;
        fa.swap_axes = false;
        fa.lop = 0;
        code = patch_color_to_device_color_inline(pfs, p0->c, &dc[0], fc[0]);
        if (code != 0)
            return code;
        if (!(dc[0].type == &gx_dc_type_data_pure ||
            dc[0].type == &gx_dc_type_data_devn))
            return 2;
        if (!wedge) {
            code = patch_color_to_device_color_inline(pfs, p1->c, &dc[1], fc[1]);
            if (code != 0)
                return code;
        }
        code = patch_color_to_device_color_inline(pfs, p2->c, &dc[2], fc[2]);
        if (code != 0)
            return code;
        code = dev_proc(pdev, fill_linear_color_triangle)(pdev, &fa,
                        &p0->p, &p1->p, &p2->p,
                        fc[0], (wedge ? NULL : fc[1]), fc[2]);
        if (code == 1)
            return 0; /* The area is filled. */
        if (code < 0)
            return code;
        else /* code == 0, the device requested to decompose the area. */
            return 1;
    }
}

static inline int
fill_triangle_wedge(patch_fill_state_t *pfs,
            const shading_vertex_t *q0, const shading_vertex_t *q1, const shading_vertex_t *q2)
{
    if ((int64_t)(q1->p.x - q0->p.x) * (q2->p.y - q0->p.y) ==
        (int64_t)(q1->p.y - q0->p.y) * (q2->p.x - q0->p.x))
        return 0; /* Zero area. */
    /*
        Can't apply try_device_linear_color here
        because didn't check is_color_linear.
        Maybe need a decomposition.
        Do same as for 'unlinear', and branch later.
     */
    return fill_triangle_wedge_aux(pfs, q0, q1, q2);
}

static inline int
fill_triangle_wedge_from_list(patch_fill_state_t *pfs,
    const wedge_vertex_list_elem_t *beg, const wedge_vertex_list_elem_t *end,
    const wedge_vertex_list_elem_t *mid,
    const patch_color_t *c0, const patch_color_t *c1)
{
    shading_vertex_t p[3];
    patch_color_t *c;
    byte *color_stack_ptr = reserve_colors_inline(pfs, &c, 1);
    int code;

    if (color_stack_ptr == NULL)
        return_error(gs_error_unregistered); /* Must not happen. */
    p[2].c = c;
    p[0].p = beg->p;
    p[0].c = c0;
    p[1].p = end->p;
    p[1].c = c1;
    p[2].p = mid->p;
    patch_interpolate_color(c, c0, c1, pfs, 0.5);
    code = fill_triangle_wedge(pfs, &p[0], &p[1], &p[2]);
    release_colors_inline(pfs, color_stack_ptr, 1);
    return code;
}

static int
fill_wedge_from_list_rec(patch_fill_state_t *pfs,
            wedge_vertex_list_elem_t *beg, const wedge_vertex_list_elem_t *end,
            int level, const patch_color_t *c0, const patch_color_t *c1)
{
    if (beg->next == end)
        return 0;
    else if (beg->next->next == end) {
        if (beg->next->divide_count != 1 && beg->next->divide_count != 2)
            return_error(gs_error_unregistered); /* Must not happen. */
        if (beg->next->divide_count != 1)
            return 0;
        return fill_triangle_wedge_from_list(pfs, beg, end, beg->next, c0, c1);
    } else {
        gs_fixed_point p;
        wedge_vertex_list_elem_t *e;
        patch_color_t *c;
        int code;
        byte *color_stack_ptr = reserve_colors_inline(pfs, &c, 1);

        if (color_stack_ptr == NULL)
            return_error(gs_error_unregistered); /* Must not happen. */
        p.x = (beg->p.x + end->p.x) / 2;
        p.y = (beg->p.y + end->p.y) / 2;
        e = wedge_vertex_list_find(beg, end, level + 1);
        if (e == NULL)
            return_error(gs_error_unregistered); /* Must not happen. */
        if (e->p.x != p.x || e->p.y != p.y)
            return_error(gs_error_unregistered); /* Must not happen. */
        patch_interpolate_color(c, c0, c1, pfs, 0.5);
        code = fill_wedge_from_list_rec(pfs, beg, e, level + 1, c0, c);
        if (code >= 0)
            code = fill_wedge_from_list_rec(pfs, e, end, level + 1, c, c1);
        if (code >= 0) {
            if (e->divide_count != 1 && e->divide_count != 2)
                return_error(gs_error_unregistered); /* Must not happen. */
            if (e->divide_count == 1)
                code = fill_triangle_wedge_from_list(pfs, beg, end, e, c0, c1);
        }
        release_colors_inline(pfs, color_stack_ptr, 1);
        return code;
    }
}

static int
fill_wedge_from_list(patch_fill_state_t *pfs, const wedge_vertex_list_t *l,
            const patch_color_t *c0, const patch_color_t *c1)
{
    return fill_wedge_from_list_rec(pfs, l->beg, l->end,
                    max(l->beg->level, l->end->level), c0, c1);
}

static inline int
terminate_wedge_vertex_list(patch_fill_state_t *pfs, wedge_vertex_list_t *l,
        const patch_color_t *c0, const patch_color_t *c1)
{
    if (l->beg != NULL) {
        int code = fill_wedge_from_list(pfs, l, c0, c1);

        if (code < 0)
            return code;
        return release_wedge_vertex_list(pfs, l, 1);
    }
    return 0;
}

static int
wedge_by_triangles(patch_fill_state_t *pfs, int ka,
        const gs_fixed_point pole[4], const patch_color_t *c0, const patch_color_t *c1)
{   /* Assuming ka >= 2, see fill_wedges. */
    gs_fixed_point q[2][4];
    patch_color_t *c;
    shading_vertex_t p[3];
    int code;
    byte *color_stack_ptr = reserve_colors_inline(pfs, &c, 1);

    if (color_stack_ptr == NULL)
        return_error(gs_error_unregistered); /* Must not happen. */
    p[2].c = c;
    split_curve(pole, q[0], q[1]);
    p[0].p = pole[0];
    p[0].c = c0;
    p[1].p = pole[3];
    p[1].c = c1;
    p[2].p = q[0][3];
    patch_interpolate_color(c, c0, c1, pfs, 0.5);
    code = fill_triangle_wedge(pfs, &p[0], &p[1], &p[2]);
    if (code >= 0) {
        if (ka == 2)
            goto out;
        code = wedge_by_triangles(pfs, ka / 2, q[0], c0, p[2].c);
    }
    if (code >= 0)
        code = wedge_by_triangles(pfs, ka / 2, q[1], p[2].c, c1);
out:
    release_colors_inline(pfs, color_stack_ptr, 1);
    return code;
}

int
mesh_padding(patch_fill_state_t *pfs, const gs_fixed_point *p0, const gs_fixed_point *p1,
            const patch_color_t *c0, const patch_color_t *c1)
{
    gs_fixed_point q0, q1;
    const patch_color_t *cc0, *cc1;
    fixed dx = p1->x - p0->x;
    fixed dy = p1->y - p0->y;
    bool swap_axes = (any_abs(dx) > any_abs(dy));
    gs_fixed_edge le, re;
    const fixed adjust = INTERPATCH_PADDING;

    if (swap_axes) {
        if (p0->x < p1->x) {
            q0.x = p0->y;
            q0.y = p0->x;
            q1.x = p1->y;
            q1.y = p1->x;
            cc0 = c0;
            cc1 = c1;
        } else {
            q0.x = p1->y;
            q0.y = p1->x;
            q1.x = p0->y;
            q1.y = p0->x;
            cc0 = c1;
            cc1 = c0;
        }
    } else if (p0->y < p1->y) {
        q0 = *p0;
        q1 = *p1;
        cc0 = c0;
        cc1 = c1;
    } else {
        q0 = *p1;
        q1 = *p0;
        cc0 = c1;
        cc1 = c0;
    }
    le.start.x = q0.x - adjust;
    re.start.x = q0.x + adjust;
    le.start.y = re.start.y = q0.y - adjust;
    le.end.x = q1.x - adjust;
    re.end.x = q1.x + adjust;
    le.end.y = re.end.y = q1.y + adjust;
    adjust_swapped_boundary(&re.start.x, swap_axes);
    adjust_swapped_boundary(&re.end.x, swap_axes);
    return decompose_linear_color(pfs, &le, &re, le.start.y, le.end.y, swap_axes, cc0, cc1);
    /* fixme : for a better performance and quality, we would like to
       consider the bar as an oriented one and to know at what side of it the spot resides.
       If we know that, we could expand only to outside the spot.
       Note that if the boundary has a self-intersection,
       we still need to expand to both directions.
     */
}

static inline void
bbox_of_points(gs_fixed_rect *r,
        const gs_fixed_point *p0, const gs_fixed_point *p1,
        const gs_fixed_point *p2, const gs_fixed_point *p3)
{
    r->p.x = r->q.x = p0->x;
    r->p.y = r->q.y = p0->y;

    if (r->p.x > p1->x)
        r->p.x = p1->x;
    if (r->q.x < p1->x)
        r->q.x = p1->x;
    if (r->p.y > p1->y)
        r->p.y = p1->y;
    if (r->q.y < p1->y)
        r->q.y = p1->y;

    if (r->p.x > p2->x)
        r->p.x = p2->x;
    if (r->q.x < p2->x)
        r->q.x = p2->x;
    if (r->p.y > p2->y)
        r->p.y = p2->y;
    if (r->q.y < p2->y)
        r->q.y = p2->y;

    if (p3 == NULL)
        return;

    if (r->p.x > p3->x)
        r->p.x = p3->x;
    if (r->q.x < p3->x)
        r->q.x = p3->x;
    if (r->p.y > p3->y)
        r->p.y = p3->y;
    if (r->q.y < p3->y)
        r->q.y = p3->y;
}

static int
fill_wedges_aux(patch_fill_state_t *pfs, int k, int ka,
        const gs_fixed_point pole[4], const patch_color_t *c0, const patch_color_t *c1,
        int wedge_type)
{
    int code;

    if (k > 1) {
        gs_fixed_point q[2][4];
        patch_color_t *c;
        bool save_inside = pfs->inside;
        byte *color_stack_ptr;

        if (!pfs->inside) {
            gs_fixed_rect r, r1;

            bbox_of_points(&r, &pole[0], &pole[1], &pole[2], &pole[3]);
            r.p.x -= INTERPATCH_PADDING;
            r.p.y -= INTERPATCH_PADDING;
            r.q.x += INTERPATCH_PADDING;
            r.q.y += INTERPATCH_PADDING;
            r1 = r;
            rect_intersect(r, pfs->rect);
            if (r.q.x <= r.p.x || r.q.y <= r.p.y)
                return 0;
            if (r1.p.x == r.p.x && r1.p.y == r.p.y &&
                r1.q.x == r.q.x && r1.q.y == r.q.y)
                pfs->inside = true;
        }
        color_stack_ptr = reserve_colors_inline(pfs, &c, 1);
        if (color_stack_ptr == NULL)
            return_error(gs_error_unregistered); /* Must not happen. */
        patch_interpolate_color(c, c0, c1, pfs, 0.5);
        split_curve(pole, q[0], q[1]);
        code = fill_wedges_aux(pfs, k / 2, ka, q[0], c0, c, wedge_type);
        if (code >= 0)
            code = fill_wedges_aux(pfs, k / 2, ka, q[1], c, c1, wedge_type);
        release_colors_inline(pfs, color_stack_ptr, 1);
        pfs->inside = save_inside;
        return code;
    } else {
        if ((INTERPATCH_PADDING != 0) && (wedge_type & interpatch_padding)) {
            code = mesh_padding(pfs, &pole[0], &pole[3], c0, c1);
            if (code < 0)
                return code;
        }
        if (ka >= 2 && (wedge_type & inpatch_wedge))
            return wedge_by_triangles(pfs, ka, pole, c0, c1);
        return 0;
    }
}

static int
fill_wedges(patch_fill_state_t *pfs, int k0, int k1,
        const gs_fixed_point *pole, int pole_step,
        const patch_color_t *c0, const patch_color_t *c1,
        int wedge_type)
{
    /* Generate wedges between 2 variants of a curve flattening. */
    /* k0, k1 is a power of 2. */
    gs_fixed_point p[4];

    if (!(wedge_type & interpatch_padding) && k0 == k1)
        return 0; /* Wedges are zero area. */
    if (k0 > k1) { /* Swap if required, so that k0 <= k1 */
        k0 ^= k1; k1 ^= k0; k0 ^= k1;
    }
    p[0] = pole[0];
    p[1] = pole[pole_step];
    p[2] = pole[pole_step * 2];
    p[3] = pole[pole_step * 3];
    return fill_wedges_aux(pfs, k0, k1 / k0, p, c0, c1, wedge_type);
}

static inline void
make_vertices(gs_fixed_point q[4], const quadrangle_patch *p)
{
    q[0] = p->p[0][0]->p;
    q[1] = p->p[0][1]->p;
    q[2] = p->p[1][1]->p;
    q[3] = p->p[1][0]->p;
}

static inline void
wrap_vertices_by_y(gs_fixed_point q[4], const gs_fixed_point s[4])
{
    fixed y = s[0].y;
    int i = 0;

    if (y > s[1].y)
        i = 1, y = s[1].y;
    if (y > s[2].y)
        i = 2, y = s[2].y;
    if (y > s[3].y)
        i = 3, y = s[3].y;
    q[0] = s[(i + 0) % 4];
    q[1] = s[(i + 1) % 4];
    q[2] = s[(i + 2) % 4];
    q[3] = s[(i + 3) % 4];
}

static int
ordered_triangle(patch_fill_state_t *pfs, gs_fixed_edge *le, gs_fixed_edge *re, patch_color_t *c)
{
    gs_fixed_edge ue;
    int code;
    gx_device_color dc;

#   if NOFILL_TEST
        if (dbg_nofill)
            return 0;
#   endif
    code = patch_color_to_device_color_inline(pfs, c, &dc, NULL);
    if (code < 0)
        return code;
    if (le->end.y < re->end.y) {
        code = dev_proc(pfs->dev, fill_trapezoid)(pfs->dev,
            le, re, le->start.y, le->end.y, false, &dc, pfs->pgs->log_op);
        if (code >= 0) {
            ue.start = le->end;
            ue.end = re->end;
            code = dev_proc(pfs->dev, fill_trapezoid)(pfs->dev,
                &ue, re, le->end.y, re->end.y, false, &dc, pfs->pgs->log_op);
        }
    } else if (le->end.y > re->end.y) {
        code = dev_proc(pfs->dev, fill_trapezoid)(pfs->dev,
            le, re, le->start.y, re->end.y, false, &dc, pfs->pgs->log_op);
        if (code >= 0) {
            ue.start = re->end;
            ue.end = le->end;
            code = dev_proc(pfs->dev, fill_trapezoid)(pfs->dev,
                le, &ue, re->end.y, le->end.y, false, &dc, pfs->pgs->log_op);
        }
    } else
        code = dev_proc(pfs->dev, fill_trapezoid)(pfs->dev,
            le, re, le->start.y, le->end.y, false, &dc, pfs->pgs->log_op);
    return code;
}

static int
constant_color_triangle(patch_fill_state_t *pfs,
        const shading_vertex_t *p0, const shading_vertex_t *p1, const shading_vertex_t *p2)
{
    patch_color_t *c[2];
    gs_fixed_edge le, re;
    fixed dx0, dy0, dx1, dy1;
    const shading_vertex_t *pp;
    int i, code = 0;
    byte *color_stack_ptr = reserve_colors_inline(pfs, c, 2);

    if (color_stack_ptr == NULL)
        return_error(gs_error_unregistered); /* Must not happen. */
    patch_interpolate_color(c[0], p0->c, p1->c, pfs, 0.5);
    patch_interpolate_color(c[1], p2->c, c[0], pfs, 0.5);
    for (i = 0; i < 3; i++) {
        /* fixme : does optimizer compiler expand this cycle ? */
        if (p0->p.y <= p1->p.y && p0->p.y <= p2->p.y) {
            le.start = re.start = p0->p;
            le.end = p1->p;
            re.end = p2->p;

            dx0 = le.end.x - le.start.x;
            dy0 = le.end.y - le.start.y;
            dx1 = re.end.x - re.start.x;
            dy1 = re.end.y - re.start.y;
            if ((int64_t)dx0 * dy1 < (int64_t)dy0 * dx1)
                code = ordered_triangle(pfs, &le, &re, c[1]);
            else
                code = ordered_triangle(pfs, &re, &le, c[1]);
            if (code < 0)
                break;
        }
        pp = p0; p0 = p1; p1 = p2; p2 = pp;
    }
    release_colors_inline(pfs, color_stack_ptr, 2);
    return code;
}

static inline int
constant_color_quadrangle_aux(patch_fill_state_t *pfs, const quadrangle_patch *p, bool self_intersecting,
        patch_color_t *c[3])
{
    /* Assuming the XY span is restricted with curve_samples.
       It is important for intersection_of_small_bars to compute faster. */
    gs_fixed_point q[4];
    fixed ry, ey;
    int code;
    bool swap_axes = false;
    gx_device_color dc;
    bool orient;

    dc.tag = device_current_tag(pfs->dev);

    patch_interpolate_color(c[1], p->p[0][0]->c, p->p[0][1]->c, pfs, 0.5);
    patch_interpolate_color(c[2], p->p[1][0]->c, p->p[1][1]->c, pfs, 0.5);
    patch_interpolate_color(c[0], c[1], c[2], pfs, 0.5);
    code = patch_color_to_device_color_inline(pfs, c[0], &dc, NULL);
    if (code < 0)
        return code;
    {   gs_fixed_point qq[4];

        make_vertices(qq, p);
#ifdef SHADING_SWAP_AXES_FOR_PRECISION
             /* Swapping axes may improve the precision,
                but slows down due to the area expansion needed
                in gx_shade_trapezoid. */
            dx = span_x(qq, 4);
            dy = span_y(qq, 4);
            if (dy < dx) {
                do_swap_axes(qq, 4);
                swap_axes = true;
            }
#endif
        wrap_vertices_by_y(q, qq);
    }
    {   fixed dx1 = q[1].x - q[0].x, dy1 = q[1].y - q[0].y;
        fixed dx3 = q[3].x - q[0].x, dy3 = q[3].y - q[0].y;
        int64_t g13 = (int64_t)dx1 * dy3, h13 = (int64_t)dy1 * dx3;

        if (g13 == h13) {
            fixed dx2 = q[2].x - q[0].x, dy2 = q[2].y - q[0].y;
            int64_t g23 = (int64_t)dx2 * dy3, h23 = (int64_t)dy2 * dx3;

            if (dx1 == 0 && dy1 == 0 && g23 == h23)
                return 0;
            if (g23 != h23) {
                orient = (g23 > h23);
                if (q[2].y <= q[3].y) {
                    if ((code = gx_shade_trapezoid(pfs, q, 1, 2, 0, 3, q[1].y, q[2].y, swap_axes, &dc, orient)) < 0)
                        return code;
                    return gx_shade_trapezoid(pfs, q, 2, 3, 0, 3, q[2].y, q[3].y, swap_axes, &dc, orient);
                } else {
                    if ((code = gx_shade_trapezoid(pfs, q, 1, 2, 0, 3, q[1].y, q[3].y, swap_axes, &dc, orient)) < 0)
                        return code;
                    return gx_shade_trapezoid(pfs, q, 1, 2, 3, 2, q[3].y, q[2].y, swap_axes, &dc, orient);
                }
            } else {
                int64_t g12 = (int64_t)dx1 * dy2, h12 = (int64_t)dy1 * dx2;

                if (dx3 == 0 && dy3 == 0 && g12 == h12)
                    return 0;
                orient = (g12 > h12);
                if (q[1].y <= q[2].y) {
                    if ((code = gx_shade_trapezoid(pfs, q, 0, 1, 3, 2, q[0].y, q[1].y, swap_axes, &dc, orient)) < 0)
                        return code;
                    return gx_shade_trapezoid(pfs, q, 1, 2, 3, 2, q[1].y, q[2].y, swap_axes, &dc, orient);
                } else {
                    if ((code = gx_shade_trapezoid(pfs, q, 0, 1, 3, 2, q[0].y, q[2].y, swap_axes, &dc, orient)) < 0)
                        return code;
                    return gx_shade_trapezoid(pfs, q, 0, 1, 2, 1, q[2].y, q[1].y, swap_axes, &dc, orient);
                }
            }
        }
        orient = ((int64_t)dx1 * dy3 > (int64_t)dy1 * dx3);
    }
    if (q[1].y <= q[2].y && q[2].y <= q[3].y) {
        if (self_intersecting && intersection_of_small_bars(q, 0, 3, 1, 2, &ry, &ey)) {
            if ((code = gx_shade_trapezoid(pfs, q, 0, 1, 0, 3, q[0].y, q[1].y, swap_axes, &dc, orient)) < 0)
                return code;
            if ((code = gx_shade_trapezoid(pfs, q, 0, 3, 1, 2, q[1].y, ry + ey, swap_axes, &dc, orient)) < 0)
                return code;
            if ((code = gx_shade_trapezoid(pfs, q, 1, 2, 0, 3, ry, q[2].y, swap_axes, &dc, orient)) < 0)
                return code;
            return gx_shade_trapezoid(pfs, q, 0, 3, 2, 3, q[2].y, q[3].y, swap_axes, &dc, orient);
        } else {
            if ((code = gx_shade_trapezoid(pfs, q, 0, 1, 0, 3, q[0].y, q[1].y, swap_axes, &dc, orient)) < 0)
                return code;
            if ((code = gx_shade_trapezoid(pfs, q, 1, 2, 0, 3, q[1].y, q[2].y, swap_axes, &dc, orient)) < 0)
                return code;
            return gx_shade_trapezoid(pfs, q, 2, 3, 0, 3, q[2].y, q[3].y, swap_axes, &dc, orient);
        }
    } else if (q[1].y <= q[3].y && q[3].y <= q[2].y) {
        if (self_intersecting && intersection_of_small_bars(q, 0, 3, 1, 2, &ry, &ey)) {
            if ((code = gx_shade_trapezoid(pfs, q, 0, 1, 0, 3, q[0].y, q[1].y, swap_axes, &dc, orient)) < 0)
                return code;
            if ((code = gx_shade_trapezoid(pfs, q, 1, 2, 0, 3, q[1].y, ry + ey, swap_axes, &dc, orient)) < 0)
                return code;
            if ((code = gx_shade_trapezoid(pfs, q, 0, 3, 1, 2, ry, q[3].y, swap_axes, &dc, orient)) < 0)
                return code;
            return gx_shade_trapezoid(pfs, q, 3, 2, 1, 2, q[3].y, q[2].y, swap_axes, &dc, orient);
        } else {
            if ((code = gx_shade_trapezoid(pfs, q, 0, 1, 0, 3, q[0].y, q[1].y, swap_axes, &dc, orient)) < 0)
                return code;
            if ((code = gx_shade_trapezoid(pfs, q, 1, 2, 0, 3, q[1].y, q[3].y, swap_axes, &dc, orient)) < 0)
                return code;
            return gx_shade_trapezoid(pfs, q, 1, 2, 3, 2, q[3].y, q[2].y, swap_axes, &dc, orient);
        }
    } else if (q[2].y <= q[1].y && q[1].y <= q[3].y) {
        if (self_intersecting && intersection_of_small_bars(q, 0, 1, 2, 3, &ry, &ey)) {
            if ((code = gx_shade_trapezoid(pfs, q, 0, 1, 0, 3, q[0].y, ry + ey, swap_axes, &dc, orient)) < 0)
                return code;
            if ((code = gx_shade_trapezoid(pfs, q, 2, 1, 2, 3, q[2].y, ry + ey, swap_axes, &dc, orient)) < 0)
                return code;
            if ((code = gx_shade_trapezoid(pfs, q, 2, 1, 0, 1, ry, q[1].y, swap_axes, &dc, orient)) < 0)
                return code;
            return gx_shade_trapezoid(pfs, q, 2, 3, 0, 3, ry, q[3].y, swap_axes, &dc, orient);
        } else if (self_intersecting && intersection_of_small_bars(q, 0, 3, 1, 2, &ry, &ey)) {
            if ((code = gx_shade_trapezoid(pfs, q, 0, 1, 0, 3, q[0].y, ry + ey, swap_axes, &dc, orient)) < 0)
                return code;
            if ((code = gx_shade_trapezoid(pfs, q, 2, 1, 2, 3, q[2].y, ry + ey, swap_axes, &dc, orient)) < 0)
                return code;
            if ((code = gx_shade_trapezoid(pfs, q, 0, 1, 2, 1, ry, q[1].y, swap_axes, &dc, orient)) < 0)
                return code;
            return gx_shade_trapezoid(pfs, q, 0, 3, 2, 3, ry, q[3].y, swap_axes, &dc, orient);
        } else {
            if ((code = gx_shade_trapezoid(pfs, q, 0, 1, 0, 3, q[0].y, q[1].y, swap_axes, &dc, orient)) < 0)
                return code;
            if ((code = gx_shade_trapezoid(pfs, q, 2, 3, 2, 1, q[2].y, q[1].y, swap_axes, &dc, orient)) < 0)
                return code;
            return gx_shade_trapezoid(pfs, q, 2, 3, 0, 3, q[1].y, q[3].y, swap_axes, &dc, orient);
        }
    } else if (q[2].y <= q[3].y && q[3].y <= q[1].y) {
        if (self_intersecting && intersection_of_small_bars(q, 0, 1, 2, 3, &ry, &ey)) {
            /* Same code as someone above. */
            if ((code = gx_shade_trapezoid(pfs, q, 0, 1, 0, 3, q[0].y, ry + ey, swap_axes, &dc, orient)) < 0)
                return code;
            if ((code = gx_shade_trapezoid(pfs, q, 2, 1, 2, 3, q[2].y, ry + ey, swap_axes, &dc, orient)) < 0)
                return code;
            if ((code = gx_shade_trapezoid(pfs, q, 2, 1, 0, 1, ry, q[1].y, swap_axes, &dc, orient)) < 0)
                return code;
            return gx_shade_trapezoid(pfs, q, 2, 3, 0, 3, ry, q[3].y, swap_axes, &dc, orient);
        } else if (self_intersecting && intersection_of_small_bars(q, 0, 3, 2, 1, &ry, &ey)) {
            /* Same code as someone above. */
            if ((code = gx_shade_trapezoid(pfs, q, 0, 1, 0, 3, q[0].y, ry + ey, swap_axes, &dc, orient)) < 0)
                return code;
            if ((code = gx_shade_trapezoid(pfs, q, 2, 1, 2, 3, q[2].y, ry + ey, swap_axes, &dc, orient)) < 0)
                return code;
            if ((code = gx_shade_trapezoid(pfs, q, 0, 1, 2, 1, ry, q[1].y, swap_axes, &dc, orient)) < 0)
                return code;
            return gx_shade_trapezoid(pfs, q, 0, 3, 2, 3, ry, q[3].y, swap_axes, &dc, orient);
        } else {
            if ((code = gx_shade_trapezoid(pfs, q, 0, 1, 0, 3, q[0].y, q[2].y, swap_axes, &dc, orient)) < 0)
                return code;
            if ((code = gx_shade_trapezoid(pfs, q, 2, 3, 0, 3, q[2].y, q[3].y, swap_axes, &dc, orient)) < 0)
                return code;
            return gx_shade_trapezoid(pfs, q, 0, 1, 2, 1, q[2].y, q[1].y, swap_axes, &dc, orient);
        }
    } else if (q[3].y <= q[1].y && q[1].y <= q[2].y) {
        if (self_intersecting && intersection_of_small_bars(q, 0, 1, 3, 2, &ry, &ey)) {
            if ((code = gx_shade_trapezoid(pfs, q, 0, 1, 0, 3, q[0].y, q[3].y, swap_axes, &dc, orient)) < 0)
                return code;
            if ((code = gx_shade_trapezoid(pfs, q, 0, 1, 3, 2, q[3].y, ry + ey, swap_axes, &dc, orient)) < 0)
                return code;
            if ((code = gx_shade_trapezoid(pfs, q, 3, 2, 0, 1, ry, q[1].y, swap_axes, &dc, orient)) < 0)
                return code;
            return gx_shade_trapezoid(pfs, q, 3, 2, 1, 2, q[1].y, q[2].y, swap_axes, &dc, orient);
        } else {
            if ((code = gx_shade_trapezoid(pfs, q, 0, 1, 0, 3, q[0].y, q[3].y, swap_axes, &dc, orient)) < 0)
                return code;
            if ((code = gx_shade_trapezoid(pfs, q, 0, 1, 3, 2, q[3].y, q[1].y, swap_axes, &dc, orient)) < 0)
                return code;
            return gx_shade_trapezoid(pfs, q, 1, 2, 3, 2, q[1].y, q[2].y, swap_axes, &dc, orient);
        }
    } else if (q[3].y <= q[2].y && q[2].y <= q[1].y) {
        if (self_intersecting && intersection_of_small_bars(q, 0, 1, 2, 3, &ry, &ey)) {
            if ((code = gx_shade_trapezoid(pfs, q, 0, 1, 0, 3, q[0].y, q[3].y, swap_axes, &dc, orient)) < 0)
                return code;
            if ((code = gx_shade_trapezoid(pfs, q, 0, 1, 3, 2, q[3].y, ry + ey, swap_axes, &dc, orient)) < 0)
                return code;
            if ((code = gx_shade_trapezoid(pfs, q, 3, 2, 0, 1, ry, q[2].y, swap_axes, &dc, orient)) < 0)
                return code;
            return gx_shade_trapezoid(pfs, q, 2, 1, 0, 1, q[2].y, q[1].y, swap_axes, &dc, orient);
        } else {
            if ((code = gx_shade_trapezoid(pfs, q, 0, 1, 0, 3, q[0].y, q[3].y, swap_axes, &dc, orient)) < 0)
                return code;
            if ((code = gx_shade_trapezoid(pfs, q, 0, 1, 3, 2, q[3].y, q[2].y, swap_axes, &dc, orient)) < 0)
                return code;
            return gx_shade_trapezoid(pfs, q, 0, 1, 2, 1, q[2].y, q[1].y, swap_axes, &dc, orient);
        }
    } else {
        /* Impossible. */
        return_error(gs_error_unregistered);
    }
}

int
constant_color_quadrangle(patch_fill_state_t *pfs, const quadrangle_patch *p, bool self_intersecting)
{
    patch_color_t *c[3];
    byte *color_stack_ptr = reserve_colors_inline(pfs, c, 3);
    int code;

    if (color_stack_ptr == NULL)
        return_error(gs_error_unregistered); /* Must not happen. */
    code = constant_color_quadrangle_aux(pfs, p, self_intersecting, c);
    release_colors_inline(pfs, color_stack_ptr, 3);
    return code;
}

static inline void
divide_quadrangle_by_v(patch_fill_state_t *pfs, quadrangle_patch *s0, quadrangle_patch *s1,
            shading_vertex_t q[2], const quadrangle_patch *p, patch_color_t *c[2])
{
    q[0].c = c[0];
    q[1].c = c[1];
    q[0].p.x = (p->p[0][0]->p.x + p->p[1][0]->p.x) / 2;
    q[1].p.x = (p->p[0][1]->p.x + p->p[1][1]->p.x) / 2;
    q[0].p.y = (p->p[0][0]->p.y + p->p[1][0]->p.y) / 2;
    q[1].p.y = (p->p[0][1]->p.y + p->p[1][1]->p.y) / 2;
    patch_interpolate_color(c[0], p->p[0][0]->c, p->p[1][0]->c, pfs, 0.5);
    patch_interpolate_color(c[1], p->p[0][1]->c, p->p[1][1]->c, pfs, 0.5);
    s0->p[0][0] = p->p[0][0];
    s0->p[0][1] = p->p[0][1];
    s0->p[1][0] = s1->p[0][0] = &q[0];
    s0->p[1][1] = s1->p[0][1] = &q[1];
    s1->p[1][0] = p->p[1][0];
    s1->p[1][1] = p->p[1][1];
}

static inline void
divide_quadrangle_by_u(patch_fill_state_t *pfs, quadrangle_patch *s0, quadrangle_patch *s1,
            shading_vertex_t q[2], const quadrangle_patch *p, patch_color_t *c[2])
{
    q[0].c = c[0];
    q[1].c = c[1];
    q[0].p.x = (p->p[0][0]->p.x + p->p[0][1]->p.x) / 2;
    q[1].p.x = (p->p[1][0]->p.x + p->p[1][1]->p.x) / 2;
    q[0].p.y = (p->p[0][0]->p.y + p->p[0][1]->p.y) / 2;
    q[1].p.y = (p->p[1][0]->p.y + p->p[1][1]->p.y) / 2;
    patch_interpolate_color(c[0], p->p[0][0]->c, p->p[0][1]->c, pfs, 0.5);
    patch_interpolate_color(c[1], p->p[1][0]->c, p->p[1][1]->c, pfs, 0.5);
    s0->p[0][0] = p->p[0][0];
    s0->p[1][0] = p->p[1][0];
    s0->p[0][1] = s1->p[0][0] = &q[0];
    s0->p[1][1] = s1->p[1][0] = &q[1];
    s1->p[0][1] = p->p[0][1];
    s1->p[1][1] = p->p[1][1];
}

static inline int
is_quadrangle_color_monotonic(const patch_fill_state_t *pfs, const quadrangle_patch *p,
                              bool *not_monotonic_by_u, bool *not_monotonic_by_v)
{   /* returns : 1 = monotonic, 0 = don't know, <0 = error. */
    int code, r;

    code = isnt_color_monotonic(pfs, p->p[0][0]->c, p->p[1][1]->c);
    if (code <= 0)
        return code;
    r = code << pfs->function_arg_shift;
    if (r & 1)
        *not_monotonic_by_u = true;
    if (r & 2)
        *not_monotonic_by_v = true;
    return !code;
}

static inline void
divide_bar(patch_fill_state_t *pfs,
        const shading_vertex_t *p0, const shading_vertex_t *p1, int radix, shading_vertex_t *p,
        patch_color_t *c)
{
    /* Assuming p.c == c for providing a non-const access. */
    p->p.x = (fixed)((int64_t)p0->p.x * (radix - 1) + p1->p.x) / radix;
    p->p.y = (fixed)((int64_t)p0->p.y * (radix - 1) + p1->p.y) / radix;
    patch_interpolate_color(c, p0->c, p1->c, pfs, (double)(radix - 1) / radix);
}

static int
triangle_by_4(patch_fill_state_t *pfs,
        const shading_vertex_t *p0, const shading_vertex_t *p1, const shading_vertex_t *p2,
        wedge_vertex_list_t *l01, wedge_vertex_list_t *l12, wedge_vertex_list_t *l20,
        double cd, fixed sd)
{
    shading_vertex_t p01, p12, p20;
    patch_color_t *c[3];
    wedge_vertex_list_t L01, L12, L20, L[3];
    bool inside_save = pfs->inside;
    gs_fixed_rect r = {{0,0},{0,0}}, r1 =  {{0,0},{0,0}};
    int code = 0;
    byte *color_stack_ptr;
    const bool inside = pfs->inside; /* 'const' should help compiler to analyze initializations. */

    if (!inside) {
        bbox_of_points(&r, &p0->p, &p1->p, &p2->p, NULL);
        r1 = r;
        rect_intersect(r, pfs->rect);
        if (r.q.x <= r.p.x || r.q.y <= r.p.y)
            return 0;
    }
    color_stack_ptr = reserve_colors_inline(pfs, c, 3);
    if(color_stack_ptr == NULL)
        return_error(gs_error_unregistered);
    p01.c = c[0];
    p12.c = c[1];
    p20.c = c[2];
    code = try_device_linear_color(pfs, false, p0, p1, p2);
    switch(code) {
        case 0: /* The area is filled. */
            goto out;
        case 2: /* decompose to constant color areas */
            /* Halftoned devices may do with some bigger areas
               due to imprecise representation of a contone color.
               So we multiply the decomposition limit by 4 for a faster rendering. */
            if (sd < pfs->decomposition_limit * 4) {
                code = constant_color_triangle(pfs, p2, p0, p1);
                goto out;
            }
            if (pfs->Function != NULL) {
                double d01 = color_span(pfs, p1->c, p0->c);
                double d12 = color_span(pfs, p2->c, p1->c);
                double d20 = color_span(pfs, p0->c, p2->c);

                if (d01 <= pfs->smoothness / COLOR_CONTIGUITY &&
                    d12 <= pfs->smoothness / COLOR_CONTIGUITY &&
                    d20 <= pfs->smoothness / COLOR_CONTIGUITY) {
                    code = constant_color_triangle(pfs, p2, p0, p1);
                    goto out;
                }
            } else if (cd <= pfs->smoothness / COLOR_CONTIGUITY) {
                code = constant_color_triangle(pfs, p2, p0, p1);
                goto out;
            }
            break;
        case 1: /* decompose to linear color areas */
            if (sd < pfs->decomposition_limit) {
                code = constant_color_triangle(pfs, p2, p0, p1);
                goto out;
            }
            break;
        default: /* Error. */
            goto out;
    }
    if (!inside) {
        if (r.p.x == r1.p.x && r.p.y == r1.p.y &&
            r.q.x == r1.q.x && r.q.y == r1.q.y)
            pfs->inside = true;
    }
    divide_bar(pfs, p0, p1, 2, &p01, c[0]);
    divide_bar(pfs, p1, p2, 2, &p12, c[1]);
    divide_bar(pfs, p2, p0, 2, &p20, c[2]);
    if (LAZY_WEDGES) {
        init_wedge_vertex_list(L, count_of(L));
        code = make_wedge_median(pfs, &L01, l01, true,  &p0->p, &p1->p, &p01.p);
        if (code >= 0)
            code = make_wedge_median(pfs, &L12, l12, true,  &p1->p, &p2->p, &p12.p);
        if (code >= 0)
            code = make_wedge_median(pfs, &L20, l20, false, &p2->p, &p0->p, &p20.p);
    } else {
        code = fill_triangle_wedge(pfs, p0, p1, &p01);
        if (code >= 0)
            code = fill_triangle_wedge(pfs, p1, p2, &p12);
        if (code >= 0)
            code = fill_triangle_wedge(pfs, p2, p0, &p20);
    }
    if (code >= 0)
        code = triangle_by_4(pfs, p0, &p01, &p20, &L01, &L[0], &L20, cd / 2, sd / 2);
    if (code >= 0) {
        if (LAZY_WEDGES) {
            move_wedge(&L01, l01, true);
            move_wedge(&L20, l20, false);
        }
        code = triangle_by_4(pfs, p1, &p12, &p01, &L12, &L[1], &L01, cd / 2, sd / 2);
    }
    if (code >= 0) {
        if (LAZY_WEDGES)
            move_wedge(&L12, l12, true);
        code = triangle_by_4(pfs, p2, &p20, &p12, &L20, &L[2], &L12, cd / 2, sd / 2);
    }
    if (code >= 0) {
        L[0].last_side = L[1].last_side = L[2].last_side = true;
        code = triangle_by_4(pfs, &p01, &p12, &p20, &L[1], &L[2], &L[0], cd / 2, sd / 2);
    }
    if (LAZY_WEDGES) {
        if (code >= 0)
            code = close_wedge_median(pfs, l01, p0->c, p1->c);
        if (code >= 0)
            code = close_wedge_median(pfs, l12, p1->c, p2->c);
        if (code >= 0)
            code = close_wedge_median(pfs, l20, p2->c, p0->c);
        if (code >= 0)
            code = terminate_wedge_vertex_list(pfs, &L[0], p01.c, p20.c);
        if (code >= 0)
            code = terminate_wedge_vertex_list(pfs, &L[1], p12.c, p01.c);
        if (code >= 0)
            code = terminate_wedge_vertex_list(pfs, &L[2], p20.c, p12.c);
    }
    pfs->inside = inside_save;
out:
    release_colors_inline(pfs, color_stack_ptr, 3);
    return code;
}

static inline int
fill_triangle(patch_fill_state_t *pfs,
        const shading_vertex_t *p0, const shading_vertex_t *p1, const shading_vertex_t *p2,
        wedge_vertex_list_t *l01, wedge_vertex_list_t *l12, wedge_vertex_list_t *l20)
{
    fixed sd01 = max(any_abs(p1->p.x - p0->p.x), any_abs(p1->p.y - p0->p.y));
    fixed sd12 = max(any_abs(p2->p.x - p1->p.x), any_abs(p2->p.y - p1->p.y));
    fixed sd20 = max(any_abs(p0->p.x - p2->p.x), any_abs(p0->p.y - p2->p.y));
    fixed sd1 = max(sd01, sd12);
    fixed sd = max(sd1, sd20);
    double cd = 0;

#   if SKIP_TEST
        dbg_triangle_cnt++;
#   endif
    if (pfs->Function == NULL) {
        double d01 = color_span(pfs, p1->c, p0->c);
        double d12 = color_span(pfs, p2->c, p1->c);
        double d20 = color_span(pfs, p0->c, p2->c);
        double cd1 = max(d01, d12);

        cd = max(cd1, d20);
    }
    return triangle_by_4(pfs, p0, p1, p2, l01, l12, l20, cd, sd);
}

static int
small_mesh_triangle(patch_fill_state_t *pfs,
        const shading_vertex_t *p0, const shading_vertex_t *p1, const shading_vertex_t *p2)
{
    int code;
    wedge_vertex_list_t l[3];

    init_wedge_vertex_list(l, count_of(l));
    code = fill_triangle(pfs, p0, p1, p2, &l[0], &l[1], &l[2]);
    if (code < 0)
        return code;
    code = terminate_wedge_vertex_list(pfs, &l[0], p0->c, p1->c);
    if (code < 0)
        return code;
    code = terminate_wedge_vertex_list(pfs, &l[1], p1->c, p2->c);
    if (code < 0)
        return code;
    return terminate_wedge_vertex_list(pfs, &l[2], p2->c, p0->c);
}

/* ======== END base/gxshade6.c ======== */

/* ======== BEGIN base/gxshade6.c lines 3095-4260 ======== */
static int
mesh_triangle_rec(patch_fill_state_t *pfs,
        const shading_vertex_t *p0, const shading_vertex_t *p1, const shading_vertex_t *p2)
{
    pfs->unlinear = !is_linear_color_applicable(pfs);
    if (manhattan_dist(&p0->p, &p1->p) < pfs->max_small_coord &&
        manhattan_dist(&p1->p, &p2->p) < pfs->max_small_coord &&
        manhattan_dist(&p2->p, &p0->p) < pfs->max_small_coord)
        return small_mesh_triangle(pfs, p0, p1, p2);
    else {
        /* Subdivide into 4 triangles with 3 triangle non-lazy wedges.
           Doing so against the wedge_vertex_list_elem_buffer overflow.
           We could apply a smarter method, dividing long sides
           with no wedges and short sides with lazy wedges.
           This needs to start wedges dynamically when
           a side becomes short. We don't do so because the
           number of checks per call significantly increases
           and the logics is complicated, but the performance
           advantage appears small due to big meshes are rare.
         */
        shading_vertex_t p01, p12, p20;
        patch_color_t *c[3];
        int code;
        byte *color_stack_ptr = reserve_colors_inline(pfs, c, 3);

        if (color_stack_ptr == NULL)
            return_error(gs_error_unregistered); /* Must not happen. */
        p01.c = c[0];
        p12.c = c[1];
        p20.c = c[2];
        divide_bar(pfs, p0, p1, 2, &p01, c[0]);
        divide_bar(pfs, p1, p2, 2, &p12, c[1]);
        divide_bar(pfs, p2, p0, 2, &p20, c[2]);
        code = fill_triangle_wedge(pfs, p0, p1, &p01);
        if (code >= 0)
            code = fill_triangle_wedge(pfs, p1, p2, &p12);
        if (code >= 0)
            code = fill_triangle_wedge(pfs, p2, p0, &p20);
        if (code >= 0)
            code = mesh_triangle_rec(pfs, p0, &p01, &p20);
        if (code >= 0)
            code = mesh_triangle_rec(pfs, p1, &p12, &p01);
        if (code >= 0)
            code = mesh_triangle_rec(pfs, p2, &p20, &p12);
        if (code >= 0)
            code = mesh_triangle_rec(pfs, &p01, &p12, &p20);
        release_colors_inline(pfs, color_stack_ptr, 3);
        return code;
    }
}

int
mesh_triangle(patch_fill_state_t *pfs,
        const shading_vertex_t *p0, const shading_vertex_t *p1, const shading_vertex_t *p2)
{
    /* pdfking: tiffsep's gxdso_pattern_shading_area is 0: no coverage path. */
    return mesh_triangle_rec(pfs, p0, p1, p2);
}

static inline int
triangles4(patch_fill_state_t *pfs, const quadrangle_patch *p, bool dummy_argument)
{
    shading_vertex_t p0001, p1011, q;
    patch_color_t *c[3];
    wedge_vertex_list_t l[4];
    int code;
    byte *color_stack_ptr = reserve_colors_inline(pfs, c, 3);

    if(color_stack_ptr == NULL)
        return_error(gs_error_unregistered); /* Must not happen. */
    p0001.c = c[0];
    p1011.c = c[1];
    q.c = c[2];
    init_wedge_vertex_list(l, count_of(l));
    divide_bar(pfs, p->p[0][0], p->p[0][1], 2, &p0001, c[0]);
    divide_bar(pfs, p->p[1][0], p->p[1][1], 2, &p1011, c[1]);
    divide_bar(pfs, &p0001, &p1011, 2, &q, c[2]);
    code = fill_triangle(pfs, p->p[0][0], p->p[0][1], &q, p->l0001, &l[0], &l[3]);
    if (code >= 0) {
        l[0].last_side = true;
        l[3].last_side = true;
        code = fill_triangle(pfs, p->p[0][1], p->p[1][1], &q, p->l0111, &l[1], &l[0]);
    }
    if (code >= 0) {
        l[1].last_side = true;
        code = fill_triangle(pfs, p->p[1][1], p->p[1][0], &q, p->l1110, &l[2], &l[1]);
    }
    if (code >= 0) {
        l[2].last_side = true;
        code = fill_triangle(pfs, p->p[1][0], p->p[0][0], &q, p->l1000, &l[3], &l[2]);
    }
    if (code >= 0)
        code = terminate_wedge_vertex_list(pfs, &l[0], p->p[0][1]->c, q.c);
    if (code >= 0)
        code = terminate_wedge_vertex_list(pfs, &l[1], p->p[1][1]->c, q.c);
    if (code >= 0)
        code = terminate_wedge_vertex_list(pfs, &l[2], p->p[1][0]->c, q.c);
    if (code >= 0)
        code = terminate_wedge_vertex_list(pfs, &l[3], q.c, p->p[0][0]->c);
    release_colors_inline(pfs, color_stack_ptr, 3);
    return code;
}

static inline int
triangles2(patch_fill_state_t *pfs, const quadrangle_patch *p, bool dummy_argument)
{
    wedge_vertex_list_t l;
    int code;

    init_wedge_vertex_list(&l, 1);
    code = fill_triangle(pfs, p->p[0][0], p->p[0][1], p->p[1][1], p->l0001, p->l0111, &l);
    if (code < 0)
        return code;
    l.last_side = true;
    code = fill_triangle(pfs, p->p[1][1], p->p[1][0], p->p[0][0], p->l1110, p->l1000, &l);
    if (code < 0)
        return code;
    code = terminate_wedge_vertex_list(pfs, &l, p->p[1][1]->c, p->p[0][0]->c);
    if (code < 0)
        return code;
    return 0;
}

static inline void
make_quadrangle(const tensor_patch *p, shading_vertex_t qq[2][2],
        wedge_vertex_list_t l[4], quadrangle_patch *q)
{
    qq[0][0].p = p->pole[0][0];
    qq[0][1].p = p->pole[0][3];
    qq[1][0].p = p->pole[3][0];
    qq[1][1].p = p->pole[3][3];
    qq[0][0].c = p->c[0][0];
    qq[0][1].c = p->c[0][1];
    qq[1][0].c = p->c[1][0];
    qq[1][1].c = p->c[1][1];
    q->p[0][0] = &qq[0][0];
    q->p[0][1] = &qq[0][1];
    q->p[1][0] = &qq[1][0];
    q->p[1][1] = &qq[1][1];
    q->l0001 = &l[0];
    q->l0111 = &l[1];
    q->l1110 = &l[2];
    q->l1000 = &l[3];
}

static inline int
is_quadrangle_color_linear_by_u(const patch_fill_state_t *pfs, const quadrangle_patch *p)
{   /* returns : 1 = linear, 0 = unlinear, <0 = error. */
    int code;

    code = is_color_linear(pfs, p->p[0][0]->c, p->p[0][1]->c);
    if (code <= 0)
        return code;
    return is_color_linear(pfs, p->p[1][0]->c, p->p[1][1]->c);
}

static inline int
is_quadrangle_color_linear_by_v(const patch_fill_state_t *pfs, const quadrangle_patch *p)
{   /* returns : 1 = linear, 0 = unlinear, <0 = error. */
    int code;

    code = is_color_linear(pfs, p->p[0][0]->c, p->p[1][0]->c);
    if (code <= 0)
        return code;
    return is_color_linear(pfs, p->p[0][1]->c, p->p[1][1]->c);
}

static inline int
is_quadrangle_color_linear_by_diagonals(const patch_fill_state_t *pfs, const quadrangle_patch *p)
{   /* returns : 1 = linear, 0 = unlinear, <0 = error. */
    int code;

    code = is_color_linear(pfs, p->p[0][0]->c, p->p[1][1]->c);
    if (code <= 0)
        return code;
    return is_color_linear(pfs, p->p[0][1]->c, p->p[1][0]->c);
}

typedef enum {
    color_change_small,
    color_change_gradient,
    color_change_linear,
    color_change_bilinear,
    color_change_general
} color_change_type_t;

static inline color_change_type_t
quadrangle_color_change(const patch_fill_state_t *pfs, const quadrangle_patch *p,
                        bool is_big_u, bool is_big_v, double size_u, double size_v,
                        bool *divide_u, bool *divide_v)
{
    patch_color_t d0001, d1011, d;
    double D, D0001, D1011, D0010, D0111, D0011, D0110;
    double Du, Dv;

    color_diff(pfs, p->p[0][0]->c, p->p[0][1]->c, &d0001);
    color_diff(pfs, p->p[1][0]->c, p->p[1][1]->c, &d1011);
    D0001 = color_norm(pfs, &d0001);
    D1011 = color_norm(pfs, &d1011);
    D0010 = color_span(pfs, p->p[0][0]->c, p->p[1][0]->c);
    D0111 = color_span(pfs, p->p[0][1]->c, p->p[1][1]->c);
    D0011 = color_span(pfs, p->p[0][0]->c, p->p[1][1]->c);
    D0110 = color_span(pfs, p->p[0][1]->c, p->p[1][0]->c);
    if (pfs->unlinear) {
        if (D0001 <= pfs->smoothness && D1011 <= pfs->smoothness &&
            D0010 <= pfs->smoothness && D0111 <= pfs->smoothness &&
            D0011 <= pfs->smoothness && D0110 <= pfs->smoothness)
            return color_change_small;
        if (D0001 <= pfs->smoothness && D1011 <= pfs->smoothness) {
            if (!is_big_v) {
                /* The color function looks uncontiguous. */
                return color_change_small;
            }
            *divide_v = true;
            return color_change_gradient;
        }
        if (D0010 <= pfs->smoothness && D0111 <= pfs->smoothness) {
            if (!is_big_u) {
                /* The color function looks uncontiguous. */
                return color_change_small;
            }
            *divide_u = true;
            return color_change_gradient;
        }
    }
    color_diff(pfs, &d0001, &d1011, &d);
    Du = max(D0001, D1011);
    Dv = max(D0010, D0111);
    if (Du <= pfs->smoothness / 8 && Dv <= pfs->smoothness / 8)
        return color_change_small;
    if (Du <= pfs->smoothness / 8)
        return color_change_linear;
    if (Dv <= pfs->smoothness / 8)
        return color_change_linear;
    D = color_norm(pfs, &d);
    if (D <= pfs->smoothness)
        return color_change_bilinear;
#if 1
    if (Du > Dv && is_big_u)
        *divide_u = true;
    else if (Du < Dv && is_big_v)
        *divide_v = true;
    else if (is_big_u && size_u > size_v)
        *divide_u = true;
    else if (is_big_v && size_v > size_u)
        *divide_v = true;
    else if (is_big_u)
        *divide_u = true;
    else if (is_big_v)
        *divide_v = true;
    else {
        /* The color function looks uncontiguous. */
        return color_change_small;
    }
#else /* Disabled due to infinite recursion with -r200 09-57.PS
         (Standard Test 6.4  - Range 6) (Test05). */
    if (Du > Dv)
        *divide_u = true;
    else
        *divide_v = true;
#endif
    return color_change_general;
}

static int
fill_quadrangle(patch_fill_state_t *pfs, const quadrangle_patch *p, bool big)
{
    /* The quadrangle is flattened enough by V and U, so ignore inner poles. */
    /* Assuming the XY span is restricted with curve_samples.
       It is important for intersection_of_small_bars to compute faster. */
    quadrangle_patch s0, s1;
    wedge_vertex_list_t l0, l1, l2;
    int code;
    bool divide_u = false, divide_v = false, big1 = big;
    shading_vertex_t q[2];
    bool monotonic_color_save = pfs->monotonic_color;
    bool linear_color_save = pfs->linear_color;
    bool inside_save = pfs->inside;
    const bool inside = pfs->inside; /* 'const' should help compiler to analyze initializations. */
    gs_fixed_rect r = {{0,0},{0,0}}, r1 = {{0,0},{0,0}};
    /* Warning : pfs->monotonic_color is not restored on error. */

    if (!inside) {
        bbox_of_points(&r, &p->p[0][0]->p, &p->p[0][1]->p, &p->p[1][0]->p, &p->p[1][1]->p);
        r1 = r;
        rect_intersect(r, pfs->rect);
        if (r.q.x <= r.p.x || r.q.y <= r.p.y)
            return 0; /* Outside. */
    }
    if (big) {
        /* Likely 'big' is an unuseful rudiment due to curve_samples
           restricts lengthes. We keep it for a while because its implementation
           isn't obvious and its time consumption is invisibly small.
         */
        fixed size_u = max(max(any_abs(p->p[0][0]->p.x - p->p[0][1]->p.x),
                               any_abs(p->p[1][0]->p.x - p->p[1][1]->p.x)),
                           max(any_abs(p->p[0][0]->p.y - p->p[0][1]->p.y),
                               any_abs(p->p[1][0]->p.y - p->p[1][1]->p.y)));
        fixed size_v = max(max(any_abs(p->p[0][0]->p.x - p->p[1][0]->p.x),
                               any_abs(p->p[0][1]->p.x - p->p[1][1]->p.x)),
                           max(any_abs(p->p[0][0]->p.y - p->p[1][0]->p.y),
                               any_abs(p->p[0][1]->p.y - p->p[1][1]->p.y)));

        if (QUADRANGLES && pfs->maybe_self_intersecting) {
            if (size_v > pfs->max_small_coord) {
                /* constant_color_quadrangle can't handle big self-intersecting areas
                   because we don't want int64_t in it. */
                divide_v = true;
            } else if (size_u > pfs->max_small_coord) {
                /* constant_color_quadrangle can't handle big self-intersecting areas,
                   because we don't want int64_t in it. */
                divide_u = true;
            } else
                big1 = false;
        } else
            big1 = false;
    }
    if (!big1) {
        bool is_big_u = false, is_big_v = false;
        double d0001x = any_abs(p->p[0][0]->p.x - p->p[0][1]->p.x);
        double d1011x = any_abs(p->p[1][0]->p.x - p->p[1][1]->p.x);
        double d0001y = any_abs(p->p[0][0]->p.y - p->p[0][1]->p.y);
        double d1011y = any_abs(p->p[1][0]->p.y - p->p[1][1]->p.y);
        double d0010x = any_abs(p->p[0][0]->p.x - p->p[1][0]->p.x);
        double d0111x = any_abs(p->p[0][1]->p.x - p->p[1][1]->p.x);
        double d0010y = any_abs(p->p[0][0]->p.y - p->p[1][0]->p.y);
        double d0111y = any_abs(p->p[0][1]->p.y - p->p[1][1]->p.y);
        double size_u = max(max(d0001x, d1011x), max(d0001y, d1011y));
        double size_v = max(max(d0010x, d0111x), max(d0010y, d0111y));

        if (size_u > pfs->decomposition_limit)
            is_big_u = true;
        if (size_v > pfs->decomposition_limit)
            is_big_v = true;
        else if (!is_big_u)
            return (QUADRANGLES || !pfs->maybe_self_intersecting ?
                        constant_color_quadrangle : triangles4)(pfs, p,
                            pfs->maybe_self_intersecting);
        if (!pfs->monotonic_color) {
            bool not_monotonic_by_u = false, not_monotonic_by_v = false;

            code = is_quadrangle_color_monotonic(pfs, p, &not_monotonic_by_u, &not_monotonic_by_v);
            if (code < 0)
                return code;
            if (is_big_u)
                divide_u = not_monotonic_by_u;
            if (is_big_v)
                divide_v = not_monotonic_by_v;
            if (!divide_u && !divide_v)
                pfs->monotonic_color = true;
        }
        if (pfs->monotonic_color && !pfs->linear_color) {
            if (divide_v && divide_u) {
                if (size_u > size_v)
                    divide_v = false;
                else
                    divide_u = false;
            } else if (!divide_u && !divide_v && !pfs->unlinear) {
                if (d0001x + d1011x + d0001y + d1011y > d0010x + d0111x + d0010y + d0111y) { /* fixme: use size_u, size_v */
                    code = is_quadrangle_color_linear_by_u(pfs, p);
                    if (code < 0)
                        return code;
                    divide_u = !code;
                }
                if (is_big_v) {
                    code = is_quadrangle_color_linear_by_v(pfs, p);
                    if (code < 0)
                        return code;
                    divide_v = !code;
                }
                if (is_big_u && is_big_v) {
                    code = is_quadrangle_color_linear_by_diagonals(pfs, p);
                    if (code < 0)
                        return code;
                    if (!code) {
                        if (d0001x + d1011x + d0001y + d1011y > d0010x + d0111x + d0010y + d0111y) { /* fixme: use size_u, size_v */
                            divide_u = true;
                            divide_v = false;
                        } else {
                            divide_v = true;
                            divide_u = false;
                        }
                    }
                }
            }
            if (!divide_u && !divide_v)
                pfs->linear_color = true;
        }
        if (!pfs->linear_color) {
            /* go to divide. */
        } else switch(quadrangle_color_change(pfs, p, is_big_u, is_big_v, size_u, size_v, &divide_u, &divide_v)) {
            case color_change_small:
                code = (QUADRANGLES || !pfs->maybe_self_intersecting ?
                            constant_color_quadrangle : triangles4)(pfs, p,
                                pfs->maybe_self_intersecting);
                pfs->monotonic_color = monotonic_color_save;
                pfs->linear_color = linear_color_save;
                return code;
            case color_change_bilinear:
                if (!QUADRANGLES) {
                    code = triangles4(pfs, p, true);
                    pfs->monotonic_color = monotonic_color_save;
                    pfs->linear_color = linear_color_save;
                    return code;
                }
            case color_change_linear:
                if (!QUADRANGLES) {
                    code = triangles2(pfs, p, true);
                    pfs->monotonic_color = monotonic_color_save;
                    pfs->linear_color = linear_color_save;
                    return code;
                }
            case color_change_gradient:
            case color_change_general:
                ; /* goto divide. */
        }
    }
    if (!inside) {
        if (r.p.x == r1.p.x && r.p.y == r1.p.y &&
            r.q.x == r1.q.x && r.q.y == r1.q.y)
            pfs->inside = true;
    }
    if (LAZY_WEDGES)
        init_wedge_vertex_list(&l0, 1);
    if (divide_v) {
        patch_color_t *c[2];
        byte *color_stack_ptr = reserve_colors_inline(pfs, c, 2);

        if(color_stack_ptr == NULL)
            return_error(gs_error_unregistered); /* Must not happen. */
        q[0].c = c[0];
        q[1].c = c[1];
        divide_quadrangle_by_v(pfs, &s0, &s1, q, p, c);
        if (LAZY_WEDGES) {
            code = make_wedge_median(pfs, &l1, p->l0111, true,  &p->p[0][1]->p, &p->p[1][1]->p, &s0.p[1][1]->p);
            if (code >= 0)
                code = make_wedge_median(pfs, &l2, p->l1000, false, &p->p[1][0]->p, &p->p[0][0]->p, &s0.p[1][0]->p);
            if (code >= 0) {
                s0.l1110 = s1.l0001 = &l0;
                s0.l0111 = s1.l0111 = &l1;
                s0.l1000 = s1.l1000 = &l2;
                s0.l0001 = p->l0001;
                s1.l1110 = p->l1110;
            }
        } else {
            code = fill_triangle_wedge(pfs, s0.p[0][0], s1.p[1][0], s0.p[1][0]);
            if (code >= 0)
                code = fill_triangle_wedge(pfs, s0.p[0][1], s1.p[1][1], s0.p[1][1]);
        }
        if (code >= 0)
            code = fill_quadrangle(pfs, &s0, big1);
        if (code >= 0) {
            if (LAZY_WEDGES) {
                l0.last_side = true;
                move_wedge(&l1, p->l0111, true);
                move_wedge(&l2, p->l1000, false);
            }
            code = fill_quadrangle(pfs, &s1, big1);
        }
        if (LAZY_WEDGES) {
            if (code >= 0)
                code = close_wedge_median(pfs, p->l0111, p->p[0][1]->c, p->p[1][1]->c);
            if (code >= 0)
                code = close_wedge_median(pfs, p->l1000, p->p[1][0]->c, p->p[0][0]->c);
            if (code >= 0)
                code = terminate_wedge_vertex_list(pfs, &l0, s0.p[1][0]->c, s0.p[1][1]->c);
            release_colors_inline(pfs, color_stack_ptr, 2);
        }
    } else if (divide_u) {
        patch_color_t *c[2];
        byte *color_stack_ptr = reserve_colors_inline(pfs, c, 2);

        if(color_stack_ptr == NULL)
            return_error(gs_error_unregistered); /* Must not happen. */
        q[0].c = c[0];
        q[1].c = c[1];
        divide_quadrangle_by_u(pfs, &s0, &s1, q, p, c);
        if (LAZY_WEDGES) {
            code = make_wedge_median(pfs, &l1, p->l0001, true,  &p->p[0][0]->p, &p->p[0][1]->p, &s0.p[0][1]->p);
            if (code >= 0)
                code = make_wedge_median(pfs, &l2, p->l1110, false, &p->p[1][1]->p, &p->p[1][0]->p, &s0.p[1][1]->p);
            if (code >= 0) {
                s0.l0111 = s1.l1000 = &l0;
                s0.l0001 = s1.l0001 = &l1;
                s0.l1110 = s1.l1110 = &l2;
                s0.l1000 = p->l1000;
                s1.l0111 = p->l0111;
            }
        } else {
            code = fill_triangle_wedge(pfs, s0.p[0][0], s1.p[0][1], s0.p[0][1]);
            if (code >= 0)
                code = fill_triangle_wedge(pfs, s0.p[1][0], s1.p[1][1], s0.p[1][1]);
        }
        if (code >= 0)
            code = fill_quadrangle(pfs, &s0, big1);
        if (code >= 0) {
            if (LAZY_WEDGES) {
                l0.last_side = true;
                move_wedge(&l1, p->l0001, true);
                move_wedge(&l2, p->l1110, false);
            }
            code = fill_quadrangle(pfs, &s1, big1);
        }
        if (LAZY_WEDGES) {
            if (code >= 0)
                code = close_wedge_median(pfs, p->l0001, p->p[0][0]->c, p->p[0][1]->c);
            if (code >= 0)
                code = close_wedge_median(pfs, p->l1110, p->p[1][1]->c, p->p[1][0]->c);
            if (code >= 0)
                code = terminate_wedge_vertex_list(pfs, &l0, s0.p[0][1]->c, s0.p[1][1]->c);
            release_colors_inline(pfs, color_stack_ptr, 2);
        }
    } else
        code = (QUADRANGLES || !pfs->maybe_self_intersecting ?
                    constant_color_quadrangle : triangles4)(pfs, p,
                        pfs->maybe_self_intersecting);
    pfs->monotonic_color = monotonic_color_save;
    pfs->linear_color = linear_color_save;
    pfs->inside = inside_save;
    return code;
}

/* This splits tensor patch p->pole[v][u] on u to give s0->pole[v][u] and s1->pole[v][u] */
static inline void
split_stripe(patch_fill_state_t *pfs, tensor_patch *s0, tensor_patch *s1, const tensor_patch *p, patch_color_t *c[2])
{
    s0->c[0][1] = c[0];
    s0->c[1][1] = c[1];
    split_curve_s(p->pole[0], s0->pole[0], s1->pole[0], 1);
    split_curve_s(p->pole[1], s0->pole[1], s1->pole[1], 1);
    split_curve_s(p->pole[2], s0->pole[2], s1->pole[2], 1);
    split_curve_s(p->pole[3], s0->pole[3], s1->pole[3], 1);
    s0->c[0][0] = p->c[0][0];
    s0->c[1][0] = p->c[1][0];
    s1->c[0][0] = s0->c[0][1];
    s1->c[1][0] = s0->c[1][1];
    patch_interpolate_color(s0->c[0][1], p->c[0][0], p->c[0][1], pfs, 0.5);
    patch_interpolate_color(s0->c[1][1], p->c[1][0], p->c[1][1], pfs, 0.5);
    s1->c[0][1] = p->c[0][1];
    s1->c[1][1] = p->c[1][1];
}

/* This splits tensor patch p->pole[v][u] on v to give s0->pole[v][u] and s1->pole[v][u] */
static inline void
split_patch(patch_fill_state_t *pfs, tensor_patch *s0, tensor_patch *s1, const tensor_patch *p, patch_color_t *c[2])
{
    s0->c[1][0] = c[0];
    s0->c[1][1] = c[1];
    split_curve_s(&p->pole[0][0], &s0->pole[0][0], &s1->pole[0][0], 4);
    split_curve_s(&p->pole[0][1], &s0->pole[0][1], &s1->pole[0][1], 4);
    split_curve_s(&p->pole[0][2], &s0->pole[0][2], &s1->pole[0][2], 4);
    split_curve_s(&p->pole[0][3], &s0->pole[0][3], &s1->pole[0][3], 4);
    s0->c[0][0] = p->c[0][0];
    s0->c[0][1] = p->c[0][1];
    s1->c[0][0] = s0->c[1][0];
    s1->c[0][1] = s0->c[1][1];
    patch_interpolate_color(s0->c[1][0], p->c[0][0], p->c[1][0], pfs, 0.5);
    patch_interpolate_color(s0->c[1][1], p->c[0][1], p->c[1][1], pfs, 0.5);
    s1->c[1][0] = p->c[1][0];
    s1->c[1][1] = p->c[1][1];
}

static inline void
tensor_patch_bbox(gs_fixed_rect *r, const tensor_patch *p)
{
    int i, j;

    r->p.x = r->q.x = p->pole[0][0].x;
    r->p.y = r->q.y = p->pole[0][0].y;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            const gs_fixed_point *q = &p->pole[i][j];

            if (r->p.x > q->x)
                r->p.x = q->x;
            if (r->p.y > q->y)
                r->p.y = q->y;
            if (r->q.x < q->x)
                r->q.x = q->x;
            if (r->q.y < q->y)
                r->q.y = q->y;
        }
    }
}

static int
decompose_stripe(patch_fill_state_t *pfs, const tensor_patch *p, int ku)
{
    if (ku > 1) {
        tensor_patch s0, s1;
        patch_color_t *c[2];
        int code;
        byte *color_stack_ptr;
        bool save_inside = pfs->inside;

        if (!pfs->inside) {
            gs_fixed_rect r, r1;

            tensor_patch_bbox(&r, p);
            r1 = r;
            rect_intersect(r, pfs->rect);
            if (r.q.x <= r.p.x || r.q.y <= r.p.y)
                return 0;
            if (r1.p.x == r.p.x && r1.p.y == r.p.y &&
                r1.q.x == r.q.x && r1.q.y == r.q.y)
                pfs->inside = true;
        }
        color_stack_ptr = reserve_colors_inline(pfs, c, 2);
        if(color_stack_ptr == NULL)
            return_error(gs_error_unregistered); /* Must not happen. */
        split_stripe(pfs, &s0, &s1, p, c);
        code = decompose_stripe(pfs, &s0, ku / 2);
        if (code >= 0)
            code = decompose_stripe(pfs, &s1, ku / 2);
        release_colors_inline(pfs, color_stack_ptr, 2);
        pfs->inside = save_inside;
        return code;
    } else {
        quadrangle_patch q;
        shading_vertex_t qq[2][2];
        wedge_vertex_list_t l[4];
        int code;

        init_wedge_vertex_list(l, count_of(l));
        make_quadrangle(p, qq, l, &q);
#       if SKIP_TEST
            dbg_quad_cnt++;
#       endif
        code = fill_quadrangle(pfs, &q, true);
        if (code < 0)
            return code;
        if (LAZY_WEDGES) {
            code = terminate_wedge_vertex_list(pfs, &l[0], q.p[0][0]->c, q.p[0][1]->c);
            if (code < 0)
                return code;
            code = terminate_wedge_vertex_list(pfs, &l[1], q.p[0][1]->c, q.p[1][1]->c);
            if (code < 0)
                return code;
            code = terminate_wedge_vertex_list(pfs, &l[2], q.p[1][1]->c, q.p[1][0]->c);
            if (code < 0)
                return code;
            code = terminate_wedge_vertex_list(pfs, &l[3], q.p[1][0]->c, q.p[0][1]->c);
            if (code < 0)
                return code;
        }
        return code;
    }
}

static int
fill_stripe(patch_fill_state_t *pfs, const tensor_patch *p)
{
    /* The stripe is flattened enough by V, so ignore inner poles. */
    int ku[4], kum, code;

    /* We would like to apply iterations for enumerating the kum curve parts,
       but the roundinmg errors would be too complicated due to
       the dependence on the direction. Note that neigbour
       patches may use the opposite direction for same bounding curve.
       We apply the recursive dichotomy, in which
       the rounding errors do not depend on the direction. */
    ku[0] = curve_samples(pfs, p->pole[0], 1, pfs->fixed_flat);
    ku[3] = curve_samples(pfs, p->pole[3], 1, pfs->fixed_flat);
    kum = max(ku[0], ku[3]);
    code = fill_wedges(pfs, ku[0], kum, p->pole[0], 1, p->c[0][0], p->c[0][1], inpatch_wedge);
    if (code < 0)
        return code;
    if (INTERPATCH_PADDING) {
        code = mesh_padding(pfs, &p->pole[0][0], &p->pole[3][0], p->c[0][0], p->c[1][0]);
        if (code < 0)
            return code;
        code = mesh_padding(pfs, &p->pole[0][3], &p->pole[3][3], p->c[0][1], p->c[1][1]);
        if (code < 0)
            return code;
    }
    code = decompose_stripe(pfs, p, kum);
    if (code < 0)
        return code;
    return fill_wedges(pfs, ku[3], kum, p->pole[3], 1, p->c[1][0], p->c[1][1], inpatch_wedge);
}

static inline bool neqs(int *a, int b)
{   /* Unequal signs. Assuming -1, 0, 1 only. */
    if (*a * b < 0)
        return true;
    if (!*a)
        *a = b;
    return false;
}

static inline int
vector_pair_orientation(const gs_fixed_point *p0, const gs_fixed_point *p1, const gs_fixed_point *p2)
{   fixed dx1 = p1->x - p0->x, dy1 = p1->y - p0->y;
    fixed dx2 = p2->x - p0->x, dy2 = p2->y - p0->y;
    int64_t vp = (int64_t)dx1 * dy2 - (int64_t)dy1 * dx2;

    return (vp > 0 ? 1 : vp < 0 ? -1 : 0);
}

static inline bool
is_x_bended(const tensor_patch *p)
{
    int sign = vector_pair_orientation(&p->pole[0][0], &p->pole[0][1], &p->pole[1][0]);

    if (neqs(&sign, vector_pair_orientation(&p->pole[0][1], &p->pole[0][2], &p->pole[1][1])))
        return true;
    if (neqs(&sign, vector_pair_orientation(&p->pole[0][2], &p->pole[0][3], &p->pole[1][2])))
        return true;
    if (neqs(&sign, -vector_pair_orientation(&p->pole[0][3], &p->pole[0][2], &p->pole[1][3])))
        return true;

    if (neqs(&sign, vector_pair_orientation(&p->pole[1][1], &p->pole[1][2], &p->pole[2][1])))
        return true;
    if (neqs(&sign, vector_pair_orientation(&p->pole[1][1], &p->pole[1][2], &p->pole[2][1])))
        return true;
    if (neqs(&sign, vector_pair_orientation(&p->pole[1][2], &p->pole[1][3], &p->pole[2][2])))
        return true;
    if (neqs(&sign, -vector_pair_orientation(&p->pole[1][3], &p->pole[1][2], &p->pole[2][3])))
        return true;

    if (neqs(&sign, vector_pair_orientation(&p->pole[2][1], &p->pole[2][2], &p->pole[3][1])))
        return true;
    if (neqs(&sign, vector_pair_orientation(&p->pole[2][1], &p->pole[2][2], &p->pole[3][1])))
        return true;
    if (neqs(&sign, vector_pair_orientation(&p->pole[2][2], &p->pole[2][3], &p->pole[3][2])))
        return true;
    if (neqs(&sign, -vector_pair_orientation(&p->pole[2][3], &p->pole[2][2], &p->pole[3][3])))
        return true;

    if (neqs(&sign, -vector_pair_orientation(&p->pole[3][1], &p->pole[3][2], &p->pole[2][1])))
        return true;
    if (neqs(&sign, -vector_pair_orientation(&p->pole[3][1], &p->pole[3][2], &p->pole[2][1])))
        return true;
    if (neqs(&sign, -vector_pair_orientation(&p->pole[3][2], &p->pole[3][3], &p->pole[2][2])))
        return true;
    if (neqs(&sign, vector_pair_orientation(&p->pole[3][3], &p->pole[3][2], &p->pole[2][3])))
        return true;
    return false;
}

static inline bool
is_y_bended(const tensor_patch *p)
{
    int sign = vector_pair_orientation(&p->pole[0][0], &p->pole[1][0], &p->pole[0][1]);

    if (neqs(&sign, vector_pair_orientation(&p->pole[1][0], &p->pole[2][0], &p->pole[1][1])))
        return true;
    if (neqs(&sign, vector_pair_orientation(&p->pole[2][0], &p->pole[3][0], &p->pole[2][1])))
        return true;
    if (neqs(&sign, -vector_pair_orientation(&p->pole[3][0], &p->pole[2][0], &p->pole[3][1])))
        return true;

    if (neqs(&sign, vector_pair_orientation(&p->pole[1][1], &p->pole[2][1], &p->pole[1][2])))
        return true;
    if (neqs(&sign, vector_pair_orientation(&p->pole[1][1], &p->pole[2][1], &p->pole[1][2])))
        return true;
    if (neqs(&sign, vector_pair_orientation(&p->pole[2][1], &p->pole[3][1], &p->pole[2][2])))
        return true;
    if (neqs(&sign, -vector_pair_orientation(&p->pole[3][1], &p->pole[2][1], &p->pole[3][2])))
        return true;

    if (neqs(&sign, vector_pair_orientation(&p->pole[1][2], &p->pole[2][2], &p->pole[1][3])))
        return true;
    if (neqs(&sign, vector_pair_orientation(&p->pole[1][2], &p->pole[2][2], &p->pole[1][3])))
        return true;
    if (neqs(&sign, vector_pair_orientation(&p->pole[2][2], &p->pole[3][2], &p->pole[2][3])))
        return true;
    if (neqs(&sign, -vector_pair_orientation(&p->pole[3][2], &p->pole[2][2], &p->pole[3][3])))
        return true;

    if (neqs(&sign, -vector_pair_orientation(&p->pole[1][3], &p->pole[2][3], &p->pole[1][2])))
        return true;
    if (neqs(&sign, -vector_pair_orientation(&p->pole[1][3], &p->pole[2][3], &p->pole[1][2])))
        return true;
    if (neqs(&sign, -vector_pair_orientation(&p->pole[2][3], &p->pole[3][3], &p->pole[2][2])))
        return true;
    if (neqs(&sign, vector_pair_orientation(&p->pole[3][3], &p->pole[2][3], &p->pole[3][2])))
        return true;
    return false;
}

static inline bool
is_curve_x_small(const patch_fill_state_t *pfs, const gs_fixed_point *pole, int pole_step, fixed fixed_flat)
{   /* Is curve within a single pixel, or smaller than half pixel ? */
    fixed xmin0 = min(pole[0 * pole_step].x, pole[1 * pole_step].x);
    fixed xmin1 = min(pole[2 * pole_step].x, pole[3 * pole_step].x);
    fixed xmin =  min(xmin0, xmin1);
    fixed xmax0 = max(pole[0 * pole_step].x, pole[1 * pole_step].x);
    fixed xmax1 = max(pole[2 * pole_step].x, pole[3 * pole_step].x);
    fixed xmax =  max(xmax0, xmax1);

    if(xmax - xmin <= pfs->decomposition_limit)
        return true;
    return false;
}

static inline bool
is_curve_y_small(const patch_fill_state_t *pfs, const gs_fixed_point *pole, int pole_step, fixed fixed_flat)
{   /* Is curve within a single pixel, or smaller than half pixel ? */
    fixed ymin0 = min(pole[0 * pole_step].y, pole[1 * pole_step].y);
    fixed ymin1 = min(pole[2 * pole_step].y, pole[3 * pole_step].y);
    fixed ymin =  min(ymin0, ymin1);
    fixed ymax0 = max(pole[0 * pole_step].y, pole[1 * pole_step].y);
    fixed ymax1 = max(pole[2 * pole_step].y, pole[3 * pole_step].y);
    fixed ymax =  max(ymax0, ymax1);

    if (ymax - ymin <= pfs->decomposition_limit)
        return true;
    return false;
}

static inline bool
is_patch_narrow(const patch_fill_state_t *pfs, const tensor_patch *p)
{
    if (!is_curve_x_small(pfs, &p->pole[0][0], 4, pfs->fixed_flat))
        return false;
    if (!is_curve_x_small(pfs, &p->pole[0][1], 4, pfs->fixed_flat))
        return false;
    if (!is_curve_x_small(pfs, &p->pole[0][2], 4, pfs->fixed_flat))
        return false;
    if (!is_curve_x_small(pfs, &p->pole[0][3], 4, pfs->fixed_flat))
        return false;
    if (!is_curve_y_small(pfs, &p->pole[0][0], 4, pfs->fixed_flat))
        return false;
    if (!is_curve_y_small(pfs, &p->pole[0][1], 4, pfs->fixed_flat))
        return false;
    if (!is_curve_y_small(pfs, &p->pole[0][2], 4, pfs->fixed_flat))
        return false;
    if (!is_curve_y_small(pfs, &p->pole[0][3], 4, pfs->fixed_flat))
        return false;
    return true;
}

static int
fill_patch(patch_fill_state_t *pfs, const tensor_patch *p, int kv, int kv0, int kv1)
{
    if (kv <= 1) {
        if (is_patch_narrow(pfs, p))
            return fill_stripe(pfs, p);
        if (!is_x_bended(p))
            return fill_stripe(pfs, p);
    }
    {   tensor_patch s0, s1;
        patch_color_t *c[2];
        shading_vertex_t q0, q1, q2;
        int code = 0;
        byte *color_stack_ptr;
        bool save_inside = pfs->inside;

        if (!pfs->inside) {
            gs_fixed_rect r, r1;

            tensor_patch_bbox(&r, p);
            r.p.x -= INTERPATCH_PADDING;
            r.p.y -= INTERPATCH_PADDING;
            r.q.x += INTERPATCH_PADDING;
            r.q.y += INTERPATCH_PADDING;
            r1 = r;
            rect_intersect(r, pfs->rect);
            if (r.q.x <= r.p.x || r.q.y <= r.p.y)
                return 0;
            if (r1.p.x == r.p.x && r1.p.y == r.p.y &&
                r1.q.x == r.q.x && r1.q.y == r.q.y)
                pfs->inside = true;
        }
        color_stack_ptr = reserve_colors_inline(pfs, c, 2);
        if(color_stack_ptr == NULL)
            return_error(gs_error_unregistered); /* Must not happen. */
        split_patch(pfs, &s0, &s1, p, c);
        if (kv0 <= 1) {
            q0.p = s0.pole[0][0];
            q0.c = s0.c[0][0];
            q1.p = s1.pole[3][0];
            q1.c = s1.c[1][0];
            q2.p = s0.pole[3][0];
            q2.c = s0.c[1][0];
            code = fill_triangle_wedge(pfs, &q0, &q1, &q2);
        }
        if (kv1 <= 1 && code >= 0) {
            q0.p = s0.pole[0][3];
            q0.c = s0.c[0][1];
            q1.p = s1.pole[3][3];
            q1.c = s1.c[1][1];
            q2.p = s0.pole[3][3];
            q2.c = s0.c[1][1];
            code = fill_triangle_wedge(pfs, &q0, &q1, &q2);
        }
        if (code >= 0)
            code = fill_patch(pfs, &s0, kv / 2, kv0 / 2, kv1 / 2);
        if (code >= 0)
            code = fill_patch(pfs, &s1, kv / 2, kv0 / 2, kv1 / 2);
        /* fixme : To provide the precise filling order, we must
           decompose left and right wedges into pieces by intersections
           with stripes, and fill each piece with its stripe.
           A lazy wedge list would be fine for storing
           the necessary information.

           If the patch is created from a radial shading,
           the wedge color appears a constant, so the filling order
           isn't important. The order is important for other
           self-overlapping patches, but the visible effect is
           just a slight narrowing of the patch (as its lower layer appears
           visible through the upper layer near the side).
           This kind of dropout isn't harmful, because
           contacting self-overlapping patches are painted
           one after one by definition, so that a side coverage break
           appears unavoidable by definition.

           Delaying this improvement because it is low importance.
         */
        release_colors_inline(pfs, color_stack_ptr, 2);
        pfs->inside = save_inside;
        return code;
    }
}

static inline int64_t
lcp1(int64_t p0, int64_t p3)
{   /* Computing the 1st pole of a 3d order besier, which appears a line. */
    return (p0 + p0 + p3);
}
static inline int64_t
lcp2(int64_t p0, int64_t p3)
{   /* Computing the 2nd pole of a 3d order besier, which appears a line. */
    return (p0 + p3 + p3);
}

static void
patch_set_color(const patch_fill_state_t *pfs, patch_color_t *c, const float *cc)
{
    if (pfs->Function) {
        c->t[0] = cc[0];
        c->t[1] = cc[1];
    } else
        memcpy(c->cc.paint.values, cc, sizeof(c->cc.paint.values[0]) * pfs->num_components);
}

static void
make_tensor_patch(const patch_fill_state_t *pfs, tensor_patch *p, const patch_curve_t curve[4],
           const gs_fixed_point interior[4])
{
    const gs_color_space *pcs = pfs->direct_space;

    p->pole[0][0] = curve[0].vertex.p;
    p->pole[1][0] = curve[0].control[0];
    p->pole[2][0] = curve[0].control[1];
    p->pole[3][0] = curve[1].vertex.p;
    p->pole[3][1] = curve[1].control[0];
    p->pole[3][2] = curve[1].control[1];
    p->pole[3][3] = curve[2].vertex.p;
    p->pole[2][3] = curve[2].control[0];
    p->pole[1][3] = curve[2].control[1];
    p->pole[0][3] = curve[3].vertex.p;
    p->pole[0][2] = curve[3].control[0];
    p->pole[0][1] = curve[3].control[1];
    if (interior != NULL) {
        p->pole[1][1] = interior[0];
        p->pole[1][2] = interior[1];
        p->pole[2][2] = interior[2];
        p->pole[2][1] = interior[3];
    } else {
        p->pole[1][1].x = (fixed)((3*(lcp1(p->pole[0][1].x, p->pole[3][1].x) +
                                      lcp1(p->pole[1][0].x, p->pole[1][3].x)) -
                                   lcp1(lcp1(p->pole[0][0].x, p->pole[0][3].x),
                                        lcp1(p->pole[3][0].x, p->pole[3][3].x)))/9);
        p->pole[1][2].x = (fixed)((3*(lcp1(p->pole[0][2].x, p->pole[3][2].x) +
                                      lcp2(p->pole[1][0].x, p->pole[1][3].x)) -
                                   lcp1(lcp2(p->pole[0][0].x, p->pole[0][3].x),
                                        lcp2(p->pole[3][0].x, p->pole[3][3].x)))/9);
        p->pole[2][1].x = (fixed)((3*(lcp2(p->pole[0][1].x, p->pole[3][1].x) +
                                      lcp1(p->pole[2][0].x, p->pole[2][3].x)) -
                                   lcp2(lcp1(p->pole[0][0].x, p->pole[0][3].x),
                                        lcp1(p->pole[3][0].x, p->pole[3][3].x)))/9);
        p->pole[2][2].x = (fixed)((3*(lcp2(p->pole[0][2].x, p->pole[3][2].x) +
                                      lcp2(p->pole[2][0].x, p->pole[2][3].x)) -
                                   lcp2(lcp2(p->pole[0][0].x, p->pole[0][3].x),
                                        lcp2(p->pole[3][0].x, p->pole[3][3].x)))/9);

        p->pole[1][1].y = (fixed)((3*(lcp1(p->pole[0][1].y, p->pole[3][1].y) +
                                      lcp1(p->pole[1][0].y, p->pole[1][3].y)) -
                                   lcp1(lcp1(p->pole[0][0].y, p->pole[0][3].y),
                                        lcp1(p->pole[3][0].y, p->pole[3][3].y)))/9);
        p->pole[1][2].y = (fixed)((3*(lcp1(p->pole[0][2].y, p->pole[3][2].y) +
                                      lcp2(p->pole[1][0].y, p->pole[1][3].y)) -
                                   lcp1(lcp2(p->pole[0][0].y, p->pole[0][3].y),
                                        lcp2(p->pole[3][0].y, p->pole[3][3].y)))/9);
        p->pole[2][1].y = (fixed)((3*(lcp2(p->pole[0][1].y, p->pole[3][1].y) +
                                      lcp1(p->pole[2][0].y, p->pole[2][3].y)) -
                                   lcp2(lcp1(p->pole[0][0].y, p->pole[0][3].y),
                                        lcp1(p->pole[3][0].y, p->pole[3][3].y)))/9);
        p->pole[2][2].y = (fixed)((3*(lcp2(p->pole[0][2].y, p->pole[3][2].y) +
                                      lcp2(p->pole[2][0].y, p->pole[2][3].y)) -
                                   lcp2(lcp2(p->pole[0][0].y, p->pole[0][3].y),
                                        lcp2(p->pole[3][0].y, p->pole[3][3].y)))/9);
    }
    patch_set_color(pfs, p->c[0][0], curve[0].vertex.cc);
    patch_set_color(pfs, p->c[1][0], curve[1].vertex.cc);
    patch_set_color(pfs, p->c[1][1], curve[2].vertex.cc);
    patch_set_color(pfs, p->c[0][1], curve[3].vertex.cc);
    patch_resolve_color_inline(p->c[0][0], pfs);
    patch_resolve_color_inline(p->c[0][1], pfs);
    patch_resolve_color_inline(p->c[1][0], pfs);
    patch_resolve_color_inline(p->c[1][1], pfs);
    if (!pfs->Function) {
        pcs->type->restrict_color(&p->c[0][0]->cc, pcs);
        pcs->type->restrict_color(&p->c[0][1]->cc, pcs);
        pcs->type->restrict_color(&p->c[1][0]->cc, pcs);
        pcs->type->restrict_color(&p->c[1][1]->cc, pcs);
    }
}

int
gx_shade_background(gx_device *pdev, const gs_fixed_rect *rect,
        const gx_device_color *pdevc, gs_logical_operation_t log_op)
{
    gs_fixed_edge le, re;

    le.start.x = rect->p.x - INTERPATCH_PADDING;
    le.start.y = rect->p.y - INTERPATCH_PADDING;
    le.end.x = rect->p.x - INTERPATCH_PADDING;
    le.end.y = rect->q.y + INTERPATCH_PADDING;
    re.start.x = rect->q.x + INTERPATCH_PADDING;
    re.start.y = rect->p.y - INTERPATCH_PADDING;
    re.end.x = rect->q.x + INTERPATCH_PADDING;
    re.end.y = rect->q.y + INTERPATCH_PADDING;
    return dev_proc(pdev, fill_trapezoid)(pdev,
            &le, &re, le.start.y, le.end.y, false, pdevc, log_op);
}

int
patch_fill(patch_fill_state_t *pfs, const patch_curve_t curve[4],
           const gs_fixed_point interior[4],
           void (*transform) (gs_fixed_point *, const patch_curve_t[4],
                              const gs_fixed_point[4], double, double))
{
    tensor_patch p;
    patch_color_t *c[4];
    int kv[4], kvm, ku[4], kum;
    int code = 0;
    byte *color_stack_ptr = reserve_colors_inline(pfs, c, 4); /* Can't fail */

    p.c[0][0] = c[0];
    p.c[0][1] = c[1];
    p.c[1][0] = c[2];
    p.c[1][1] = c[3];
#if SKIP_TEST
    dbg_patch_cnt++;
    /*if (dbg_patch_cnt != 67 && dbg_patch_cnt != 78)
        return 0;*/
#endif
    /* We decompose the patch into tiny quadrangles,
       possibly inserting wedges between them against a dropout. */
    make_tensor_patch(pfs, &p, curve, interior);
    pfs->unlinear = !is_linear_color_applicable(pfs);
    pfs->linear_color = false;
    /* pdfking: tiffsep's gxdso_pattern_shading_area is 0: no coverage path. */
    /* How many subdivisions of the patch in the u and v direction? */
    kv[0] = curve_samples(pfs, &p.pole[0][0], 4, pfs->fixed_flat);
    kv[1] = curve_samples(pfs, &p.pole[0][1], 4, pfs->fixed_flat);
    kv[2] = curve_samples(pfs, &p.pole[0][2], 4, pfs->fixed_flat);
    kv[3] = curve_samples(pfs, &p.pole[0][3], 4, pfs->fixed_flat);
    kvm = max(max(kv[0], kv[1]), max(kv[2], kv[3]));
    ku[0] = curve_samples(pfs, p.pole[0], 1, pfs->fixed_flat);
    ku[1] = curve_samples(pfs, p.pole[1], 1, pfs->fixed_flat);
    ku[2] = curve_samples(pfs, p.pole[2], 1, pfs->fixed_flat);
    ku[3] = curve_samples(pfs, p.pole[3], 1, pfs->fixed_flat);
    kum = max(max(ku[0], ku[1]), max(ku[2], ku[3]));
#   if NOFILL_TEST
    dbg_nofill = false;
#   endif
    code = fill_wedges(pfs, ku[0], kum, p.pole[0], 1, p.c[0][0], p.c[0][1],
        interpatch_padding | inpatch_wedge);
    if (code >= 0) {
        /* We would like to apply iterations for enumerating the kvm curve parts,
           but the roundinmg errors would be too complicated due to
           the dependence on the direction. Note that neigbour
           patches may use the opposite direction for same bounding curve.
           We apply the recursive dichotomy, in which
           the rounding errors do not depend on the direction. */
#       if NOFILL_TEST
            dbg_nofill = false;
            code = fill_patch(pfs, &p, kvm, kv[0], kv[3]);
            dbg_nofill = true;
#       endif
            code = fill_patch(pfs, &p, kvm, kv[0], kv[3]);
    }
    if (code >= 0)
        code = fill_wedges(pfs, ku[3], kum, p.pole[3], 1, p.c[1][0], p.c[1][1],
                interpatch_padding | inpatch_wedge);
out:
    release_colors_inline(pfs, color_stack_ptr, 4);
    return code;
}
/* ======== END base/gxshade6.c ======== */

/* ---------------- gsshade.h / gxshade.h: the shadings read ---------------- */

/* pdfking: the parameters of the shading types gxshade1.c fills. */
typedef struct gs_shading_params_s {
    const gs_color_space *ColorSpace;
    float Domain[4];
    gs_matrix Matrix;
    gs_function_t *Function;
    float Coords[6];
    bool Extend[2];
    /* pdfking: the mesh types' (gs_shading_mesh_params_t) */
    int BitsPerCoordinate;
    int BitsPerComponent;
    int BitsPerFlag;
    const float *Decode;
    int VerticesPerRow;
    const Shading *DataSource;
} gs_shading_params_t;
typedef gs_shading_params_t gs_shading_mesh_params_t;
typedef struct gs_shading_s {
    struct {
        int type;
    } head;
    gs_shading_params_t params;
} gs_shading_t;
typedef gs_shading_t gs_shading_Fb_t;
typedef gs_shading_t gs_shading_A_t;
typedef gs_shading_t gs_shading_R_t;
typedef gs_shading_t gs_shading_FfGt_t;
typedef gs_shading_t gs_shading_LfGt_t;
typedef gs_shading_t gs_shading_Cp_t;
typedef gs_shading_t gs_shading_Tpp_t;
#define gsicc_release_link(link) DO_NOTHING

/* gxshade.c shade_init_fill_state. pdfking: what the patch fill reads of
 * it; the ICC link is Device::remap, identity when gs's would be
 * (Device::Linearity::kAlways), and there is no transfer function. */
int
shade_init_fill_state(shading_fill_state_t * pfs, const gs_shading_t * psh,
                      gx_device * dev, gs_gstate * pgs)
{
    static const gsicc_link_t link = {false};
    const gs_color_space *pcs = psh->params.ColorSpace;

    pfs->cs_always_linear = false;
    pfs->dev = dev;
    pfs->pgs = pgs;
top:
    pfs->direct_space = pcs;
    pfs->num_components = gs_color_space_num_components(pcs);
    switch ( gs_color_space_get_index(pcs) )
        {
        case gs_color_space_index_Indexed:
            pcs = gs_cspace_base_space(pcs);
            goto top;
        default:
            break;
        }
    pfs->trans_device = dev;
    pfs->icclink = (gsicc_link_t *)&link;
    if (pcs->device->linearity == Device::Linearity::kAlways)
        pfs->cs_always_linear = true;
    pfs->ShadingType = psh->head.type;
    return 0;
}

/* gspath1.c make_quadrant_arc (gxpath.h quarter_arc_fraction) */
#define quarter_arc_fraction 0.55228474983079334
void
make_quadrant_arc(gs_point *p, const gs_point *c,
        const gs_point *p0, const gs_point *p1, double r)
{
    p[0].x = c->x + p0->x * r;
    p[0].y = c->y + p0->y * r;
    p[1].x = c->x + p0->x * r + p1->x * r * quarter_arc_fraction;
    p[1].y = c->y + p0->y * r + p1->y * r * quarter_arc_fraction;
    p[2].x = c->x + p0->x * r * quarter_arc_fraction + p1->x * r;
    p[2].y = c->y + p0->y * r * quarter_arc_fraction + p1->y * r;
    p[3].x = c->x + p1->x * r;
    p[3].y = c->y + p1->y * r;
}

/* ======== BEGIN base/gxshade1.c lines 36-3760 ======== */

/* ---------------- Function-based shading ---------------- */

typedef struct Fb_frame_s {	/* A rudiment of old code. */
    gs_rect region;
    gs_client_color cc[4];	/* colors at 4 corners */
    int state;
} Fb_frame_t;

typedef struct Fb_fill_state_s {
    shading_fill_state_common;
    const gs_shading_Fb_t *psh;
    gs_matrix_fixed ptm;	/* parameter space -> device space */
    Fb_frame_t frame;
} Fb_fill_state_t;
/****** NEED GC DESCRIPTOR ******/

static inline void
make_other_poles(patch_curve_t curve[4])
{
    int i, j;

    for (i = 0; i < 4; i++) {
        j = (i + 1) % 4;
        curve[i].control[0].x = (curve[i].vertex.p.x * 2 + curve[j].vertex.p.x) / 3;
        curve[i].control[0].y = (curve[i].vertex.p.y * 2 + curve[j].vertex.p.y) / 3;
        curve[i].control[1].x = (curve[i].vertex.p.x + curve[j].vertex.p.x * 2) / 3;
        curve[i].control[1].y = (curve[i].vertex.p.y + curve[j].vertex.p.y * 2) / 3;
        curve[i].straight = true;
    }
}

/* Transform a point with a fixed-point result. */
static void
gs_point_transform2fixed_clamped(const gs_matrix_fixed * pmat,
                         double x, double y, gs_fixed_point * ppt)
{
    gs_point fpt;

    gs_point_transform(x, y, (const gs_matrix *)pmat, &fpt);
    ppt->x = clamp_coord(fpt.x);
    ppt->y = clamp_coord(fpt.y);
}

static int
Fb_fill_region(Fb_fill_state_t * pfs, const gs_fixed_rect *rect)
{
    patch_fill_state_t pfs1;
    patch_curve_t curve[4];
    Fb_frame_t * fp = &pfs->frame;
    int code;

    memcpy(&pfs1, (shading_fill_state_t *)pfs, sizeof(shading_fill_state_t));
    pfs1.Function = pfs->psh->params.Function;
    code = init_patch_fill_state(&pfs1);
    if (code < 0)
        return code;
    pfs1.maybe_self_intersecting = false;
    pfs1.n_color_args = 2;
    pfs1.rect = *rect;
    gs_point_transform2fixed(&pfs->ptm, fp->region.p.x, fp->region.p.y, &curve[0].vertex.p);
    gs_point_transform2fixed(&pfs->ptm, fp->region.q.x, fp->region.p.y, &curve[1].vertex.p);
    gs_point_transform2fixed(&pfs->ptm, fp->region.q.x, fp->region.q.y, &curve[2].vertex.p);
    gs_point_transform2fixed(&pfs->ptm, fp->region.p.x, fp->region.q.y, &curve[3].vertex.p);
    make_other_poles(curve);
    curve[0].vertex.cc[0] = fp->region.p.x;   curve[0].vertex.cc[1] = fp->region.p.y;
    curve[1].vertex.cc[0] = fp->region.q.x;   curve[1].vertex.cc[1] = fp->region.p.y;
    curve[2].vertex.cc[0] = fp->region.q.x;   curve[2].vertex.cc[1] = fp->region.q.y;
    curve[3].vertex.cc[0] = fp->region.p.x;   curve[3].vertex.cc[1] = fp->region.q.y;
    code = patch_fill(&pfs1, curve, NULL, NULL);
    if (term_patch_fill_state(&pfs1))
        return_error(gs_error_unregistered); /* Must not happen. */
    return code;
}

int
gs_shading_Fb_fill_rectangle(const gs_shading_t * psh0, const gs_rect * rect,
                             const gs_fixed_rect * rect_clip,
                             gx_device * dev, gs_gstate * pgs)
{
    const gs_shading_Fb_t * const psh = (const gs_shading_Fb_t *)psh0;
    gs_matrix save_ctm;
    int xi, yi, code;
    float x[2], y[2];
    Fb_fill_state_t state;

    code = shade_init_fill_state((shading_fill_state_t *) & state, psh0, dev, pgs);
    if (code < 0)
        return code;
    state.psh = psh;
    /****** HACK FOR FIXED-POINT MATRIX MULTIPLY ******/
    /* pdfking: gs_concat (gs_matrix_multiply, then gs_setmatrix's
       update_ctm, gs_matrix_fixed_from_matrix) on a copy of the ctm */
    gs_matrix_multiply(&psh->params.Matrix, &ctm_only(pgs), &save_ctm);
    gs_matrix_fixed_from_matrix(&state.ptm, &save_ctm);
    /* Compute the parameter X and Y ranges. */
    {
        gs_rect pbox;

        code = gs_bbox_transform_inverse(rect, &psh->params.Matrix, &pbox);
        if (code < 0)
            return code;
        x[0] = max(pbox.p.x, psh->params.Domain[0]);
        x[1] = min(pbox.q.x, psh->params.Domain[1]);
        y[0] = max(pbox.p.y, psh->params.Domain[2]);
        y[1] = min(pbox.q.y, psh->params.Domain[3]);
    }
    if (x[0] > x[1] || y[0] > y[1]) {
        /* The region is outside the shading area. */
        if (state.icclink != NULL) gsicc_release_link(state.icclink);
        return 0;
    }
    for (xi = 0; xi < 2; ++xi)
        for (yi = 0; yi < 2; ++yi) {
            float v[2];

            v[0] = x[xi], v[1] = y[yi];
            gs_function_evaluate(psh->params.Function, v,
                                 state.frame.cc[yi * 2 + xi].paint.values);
        }
    state.frame.region.p.x = x[0];
    state.frame.region.p.y = y[0];
    state.frame.region.q.x = x[1];
    state.frame.region.q.y = y[1];
    code = Fb_fill_region(&state, rect_clip);
    if (state.icclink != NULL) gsicc_release_link(state.icclink);
    return code;
}

/* ---------------- Axial shading ---------------- */

typedef struct A_fill_state_s {
    const gs_shading_A_t *psh;
    gs_point delta;
    double length;
    double t0, t1;
    double v0, v1, u0, u1;
} A_fill_state_t;
/****** NEED GC DESCRIPTOR ******/

/* Note t0 and t1 vary over [0..1], not the Domain. */

typedef struct
{
    patch_curve_t curve[4];
    gs_point corners[4];
} corners_and_curves;

/* Ghostscript cannot possibly render any patch whose bounds aren't
 * representable in fixed's. In fact, this is a larger limit than
 * we need. We notionally have an area defined by coordinates
 * that can be represented in fixed point with at least 1 bit to
 * spare.
 *
 * Any patch that lies completely outside this region can be clipped
 * away. Any patch that isn't representable by fixed points can be
 * subdivided into 4.
 *
 * This avoids us subdividing patches huge numbers of times because
 * one side is just outside the region we will accept.
 */


#define MIN_CLIP_LIMIT ((int)(fixed2int(min_fixed)/2))
#define MAX_CLIP_LIMIT ((int)(fixed2int(max_fixed)/2))

static int not_clipped_away(const gs_point *p, const gs_fixed_rect *rect)
{
    if (p[0].x < rect->p.x &&
        p[1].x < rect->p.x &&
        p[2].x < rect->p.x &&
        p[3].x < rect->p.x)
        return 0; /* Clipped away! */
    if (p[0].x > rect->q.x &&
        p[1].x > rect->q.x &&
        p[2].x > rect->q.x &&
        p[3].x > rect->q.x)
        return 0; /* Clipped away! */
    if (p[0].y < rect->p.y &&
        p[1].y < rect->p.y &&
        p[2].y < rect->p.y &&
        p[3].y < rect->p.y)
        return 0; /* Clipped away! */
    if (p[0].y > rect->q.y &&
        p[1].y > rect->q.y &&
        p[2].y > rect->q.y &&
        p[3].y > rect->q.y)
        return 0; /* Clipped away! */
    return 1;
}

#define midpoint(a,b)\
  (arith_rshift_1(a) + arith_rshift_1(b) + (((a) | (b)) & 1))

#define quarterpoint(a,b)\
  (midpoint(a,midpoint(a,b)))

static int
subdivide_patch_fill(patch_fill_state_t *pfs, patch_curve_t c[4])
{
    fixed m0, m1;
    int v0, v1;
    int changed;

    if (pfs->rect.p.x >= pfs->rect.q.x || pfs->rect.p.y >= pfs->rect.q.y)
        return 0;

    /* On entry we have a patch:
     *   c[0].vertex  c[1].vertex
     *
     *   c[3].vertex  c[2].vertex
     *
     * Only the corners are set. The control points are not!
     *
     * BUT... in terms of spacial coords, it might be different...
     * They might be flipped on X, Y or both, giving:
     *  01 or 10 or 32 or 23
     *  32    23    01    10
     * or they might be rotated, and then flipped on X, Y or both, giving:
     *  03 or 30 or 12 or 21
     *  12    21    03    30
     */

    /* The +MIDPOINT_ACCURACY in the tests below is to allow for us finding the midpoint of [a] = z+1 and [b] = z, and getting z+1,
     * and updating [a] to be z+1, hence never actually shrinking the gap. Just accept not culling the patch as
     * much as we might. See bug 706378 for an example. */
#define MIDPOINT_ACCURACY 1
#define QUARTERPOINT_ACCURACY 3

    do {
        changed = 0;

        /* Is the whole of our patch outside the clipping rectangle? */
        /* Tempting to try to roll this into the cases below, but that
         * doesn't work because we want <= or >= here. Do X ones first. */
        if ((c[0].vertex.p.x <= pfs->rect.p.x &&
             c[1].vertex.p.x <= pfs->rect.p.x &&
             c[2].vertex.p.x <= pfs->rect.p.x &&
             c[3].vertex.p.x <= pfs->rect.p.x) ||
            (c[0].vertex.p.x >= pfs->rect.q.x &&
             c[1].vertex.p.x >= pfs->rect.q.x &&
             c[2].vertex.p.x >= pfs->rect.q.x &&
             c[3].vertex.p.x >= pfs->rect.q.x))
                return 0;

        /* First, let's try to see if we can cull the patch horizontally with the clipping
         * rectangle. */
        /* Non rotated cases first. Can we cull the left hand half? */
        if (c[0].vertex.p.x < pfs->rect.p.x && c[3].vertex.p.x < pfs->rect.p.x)
        {
            /* Check 0+3 off left. */
            v0 = 0;
            v1 = 3;
            goto check_left;
        }
        else if (c[1].vertex.p.x < pfs->rect.p.x && c[2].vertex.p.x < pfs->rect.p.x)
        {
            /* Check 1+2 off left. */
            v0 = 1;
            v1 = 2;
check_left:
            /* At this point we know that the condition for the following loop is true, so it
             * can be a do...while rather than a while. */
            do
            {
                /* Let's form (X coords only):
                 *
                 * c[v0].vertex  m0  c[v0^1].vertex
                 * c[v1].vertex  m1  c[v1^1].vertex
                 */
                m0 = midpoint(c[0].vertex.p.x, c[1].vertex.p.x);
                if (m0 >= pfs->rect.p.x)
                    goto check_left_quarter;
                m1 = midpoint(c[3].vertex.p.x, c[2].vertex.p.x);
                if (m1 >= pfs->rect.p.x)
                    goto check_left_quarter;
                /* So, we can completely discard the left hand half of the patch. */
                c[v0].vertex.p.x = m0;
                c[v0].vertex.p.y = midpoint(c[0].vertex.p.y, c[1].vertex.p.y);
                c[v1].vertex.p.x = m1;
                c[v1].vertex.p.y = midpoint(c[3].vertex.p.y, c[2].vertex.p.y);
                c[v0].vertex.cc[0] = (c[0].vertex.cc[0] + c[1].vertex.cc[0])/2;
                c[v1].vertex.cc[0] = (c[3].vertex.cc[0] + c[2].vertex.cc[0])/2;
                changed = 1;
            }
            while (c[v0].vertex.p.x < pfs->rect.p.x && c[v1].vertex.p.x < pfs->rect.p.x);
            if (0)
            {
check_left_quarter:
                /* At this point we know that the condition for the following loop is true, so it
                 * can be a do...while rather than a while. */
                do
                {
                    /* Let's form (X coords only):
                     *
                     * c[v0].vertex  m0  x  x  c[v0^1].vertex
                     * c[v1].vertex  m1  x  x  c[v1^1].vertex
                     */
                    m0 = quarterpoint(c[v0].vertex.p.x, c[v0^1].vertex.p.x);
                    if (m0 >= pfs->rect.p.x)
                        break;
                    m1 = quarterpoint(c[v1].vertex.p.x, c[v1^1].vertex.p.x);
                    if (m1 >= pfs->rect.p.x)
                        break;
                    /* So, we can completely discard the left hand quarter of the patch. */
                    c[v0].vertex.p.x = m0;
                    c[v0].vertex.p.y = midpoint(c[v0].vertex.p.y, c[v0^1].vertex.p.y);
                    c[v1].vertex.p.x = m1;
                    c[v1].vertex.p.y = midpoint(c[v1].vertex.p.y, c[v1^1].vertex.p.y);
                    c[v0].vertex.cc[0] = (c[v0].vertex.cc[0] + 3*c[v0^1].vertex.cc[0])/4;
                    c[v1].vertex.cc[0] = (c[v1].vertex.cc[0] + 3*c[v1^1].vertex.cc[0])/4;
                    changed = 1;
                }
                while (c[v0].vertex.p.x < pfs->rect.p.x && c[v1].vertex.p.x < pfs->rect.p.x);
            }
        }

        /* or the right hand half? */
        if (c[0].vertex.p.x > pfs->rect.q.x && c[3].vertex.p.x > pfs->rect.q.x)
        {
            /* Check 0+3 off right. */
            v0 = 0;
            v1 = 3;
            goto check_right;
        }
        else if (c[1].vertex.p.x > pfs->rect.q.x && c[2].vertex.p.x > pfs->rect.q.x)
        {
            /* Check 1+2 off right. */
            v0 = 1;
            v1 = 2;
check_right:
            /* At this point we know that the condition for the following loop is true, so it
             * can be a do...while rather than a while. */
            do
            {
                /* Let's form (X coords only):
                 *
                 * c[v0].vertex  m0  c[v0^1].vertex
                 * c[v1].vertex  m1  c[v1^1].vertex
                 */
                m0 = midpoint(c[0].vertex.p.x, c[1].vertex.p.x);
                if (m0 <= pfs->rect.q.x+MIDPOINT_ACCURACY)
                    goto check_right_quarter;
                m1 = midpoint(c[3].vertex.p.x, c[2].vertex.p.x);
                if (m1 <= pfs->rect.q.x+MIDPOINT_ACCURACY)
                    goto check_right_quarter;
                /* So, we can completely discard the left hand half of the patch. */
                c[v0].vertex.p.x = m0;
                c[v0].vertex.p.y = midpoint(c[0].vertex.p.y, c[1].vertex.p.y);
                c[v1].vertex.p.x = m1;
                c[v1].vertex.p.y = midpoint(c[3].vertex.p.y, c[2].vertex.p.y);
                c[v0].vertex.cc[0] = (c[0].vertex.cc[0] + c[1].vertex.cc[0])/2;
                c[v1].vertex.cc[0] = (c[3].vertex.cc[0] + c[2].vertex.cc[0])/2;
                changed = 1;
            }
            while (c[v0].vertex.p.x > pfs->rect.q.x+MIDPOINT_ACCURACY && c[v1].vertex.p.x > pfs->rect.q.x+MIDPOINT_ACCURACY);
            if (0)
            {
check_right_quarter:
                /* At this point we know that the condition for the following loop is true, so it
                 * can be a do...while rather than a while. */
                do
                {
                    /* Let's form (X coords only):
                     *
                     * c[v0].vertex  m0  x  x  c[v0^1].vertex
                     * c[v1].vertex  m1  x  x  c[v1^1].vertex
                     */
                    m0 = quarterpoint(c[v0].vertex.p.x, c[v0^1].vertex.p.x);
                    if (m0 <= pfs->rect.q.x+QUARTERPOINT_ACCURACY)
                        break;
                    m1 = quarterpoint(c[v1].vertex.p.x, c[v1^1].vertex.p.x);
                    if (m1 <= pfs->rect.q.x+QUARTERPOINT_ACCURACY)
                        break;
                    /* So, we can completely discard the left hand half of the patch. */
                    c[v0].vertex.p.x = m0;
                    c[v0].vertex.p.y = quarterpoint(c[v0].vertex.p.y, c[v0^1].vertex.p.y);
                    c[v1].vertex.p.x = m1;
                    c[v1].vertex.p.y = quarterpoint(c[v1].vertex.p.y, c[v1^1].vertex.p.y);
                    c[v0].vertex.cc[0] = (c[v0].vertex.cc[0] + 3*c[v0^1].vertex.cc[0])/4;
                    c[v1].vertex.cc[0] = (c[v1].vertex.cc[0] + 3*c[v1^1].vertex.cc[0])/4;
                    changed = 1;
                }
                while (c[v0].vertex.p.x > pfs->rect.q.x+QUARTERPOINT_ACCURACY && c[v1].vertex.p.x > pfs->rect.q.x+QUARTERPOINT_ACCURACY);
            }
        }

        /* Now, rotated cases: Can we cull the left hand half? */
        if (c[0].vertex.p.x < pfs->rect.p.x && c[1].vertex.p.x < pfs->rect.p.x)
        {
            /* Check 0+1 off left. */
            v0 = 0;
            v1 = 1;
            goto check_rot_left;
        }
        else if (c[3].vertex.p.x < pfs->rect.p.x && c[2].vertex.p.x < pfs->rect.p.x)
        {
            /* Check 3+2 off left. */
            v0 = 3;
            v1 = 2;
check_rot_left:
            /* At this point we know that the condition for the following loop is true, so it
             * can be a do...while rather than a while. */
            do
            {
                /* Let's form (X coords only):
                 *
                 * c[v0].vertex    m0  c[v0^3].vertex
                 * c[v1^3].vertex  m1  c[v1].vertex
                 */
                m0 = midpoint(c[0].vertex.p.x, c[3].vertex.p.x);
                if (m0 >= pfs->rect.p.x)
                    goto check_rot_left_quarter;
                m1 = midpoint(c[1].vertex.p.x, c[2].vertex.p.x);
                if (m1 >= pfs->rect.p.x)
                    goto check_rot_left_quarter;
                /* So, we can completely discard the left hand half of the patch. */
                c[v0].vertex.p.x = m0;
                c[v0].vertex.p.y = midpoint(c[0].vertex.p.y, c[3].vertex.p.y);
                c[v1].vertex.p.x = m1;
                c[v1].vertex.p.y = midpoint(c[1].vertex.p.y, c[2].vertex.p.y);
                c[v0].vertex.cc[0] = (c[0].vertex.cc[0] + c[3].vertex.cc[0])/2;
                c[v1].vertex.cc[0] = (c[1].vertex.cc[0] + c[2].vertex.cc[0])/2;
                changed = 1;
            }
            while (c[v0].vertex.p.x < pfs->rect.p.x && c[v1].vertex.p.x < pfs->rect.p.x);
            if (0)
            {
check_rot_left_quarter:
                /* At this point we know that the condition for the following loop is true, so it
                 * can be a do...while rather than a while. */
                do
                {
                    /* Let's form (X coords only):
                     *
                     * c[v0].vertex  m0  x  x  c[v0^3].vertex
                     * c[v1].vertex  m1  x  x  c[v1^3].vertex
                     */
                    m0 = quarterpoint(c[v0].vertex.p.x, c[v0^3].vertex.p.x);
                    if (m0 >= pfs->rect.p.x)
                        break;
                    m1 = quarterpoint(c[v1].vertex.p.x, c[v1^3].vertex.p.x);
                    if (m1 >= pfs->rect.p.x)
                        break;
                    /* So, we can completely discard the left hand half of the patch. */
                    c[v0].vertex.p.x = m0;
                    c[v0].vertex.p.y = quarterpoint(c[v0].vertex.p.y, c[v0^3].vertex.p.y);
                    c[v1].vertex.p.x = m1;
                    c[v1].vertex.p.y = quarterpoint(c[v1].vertex.p.y, c[v1^3].vertex.p.y);
                    c[v0].vertex.cc[0] = (c[v0].vertex.cc[0] + 3*c[v0^3].vertex.cc[0])/4;
                    c[v1].vertex.cc[0] = (c[v1].vertex.cc[0] + 3*c[v1^3].vertex.cc[0])/4;
                    changed = 1;
                }
                while (c[v0].vertex.p.x < pfs->rect.p.x && c[v1].vertex.p.x < pfs->rect.p.x);
            }
        }

        /* or the right hand half? */
        if (c[0].vertex.p.x > pfs->rect.q.x && c[1].vertex.p.x > pfs->rect.q.x)
        {
            /* Check 0+1 off right. */
            v0 = 0;
            v1 = 1;
            goto check_rot_right;
        }
        else if (c[3].vertex.p.x > pfs->rect.q.x && c[2].vertex.p.x > pfs->rect.q.x)
        {
            /* Check 3+2 off right. */
            v0 = 3;
            v1 = 2;
check_rot_right:
            /* At this point we know that the condition for the following loop is true, so it
             * can be a do...while rather than a while. */
            do
            {
                /* Let's form (X coords only):
                 *
                 * c[v0].vertex  m0  c[v0^3].vertex
                 * c[v1].vertex  m1  c[v1^3].vertex
                 */
                m0 = midpoint(c[0].vertex.p.x, c[3].vertex.p.x);
                if (m0 <= pfs->rect.q.x+MIDPOINT_ACCURACY)
                    goto check_rot_right_quarter;
                m1 = midpoint(c[1].vertex.p.x, c[2].vertex.p.x);
                if (m1 <= pfs->rect.q.x+MIDPOINT_ACCURACY)
                    goto check_rot_right_quarter;
                /* So, we can completely discard the left hand half of the patch. */
                c[v0].vertex.p.x = m0;
                c[v0].vertex.p.y = midpoint(c[0].vertex.p.y, c[3].vertex.p.y);
                c[v1].vertex.p.x = m1;
                c[v1].vertex.p.y = midpoint(c[1].vertex.p.y, c[2].vertex.p.y);
                c[v0].vertex.cc[0] = (c[0].vertex.cc[0] + c[3].vertex.cc[0])/2;
                c[v1].vertex.cc[0] = (c[1].vertex.cc[0] + c[2].vertex.cc[0])/2;
                changed = 1;
            }
            while (c[v0].vertex.p.x > pfs->rect.q.x+MIDPOINT_ACCURACY && c[v1].vertex.p.x > pfs->rect.q.x+MIDPOINT_ACCURACY);
            if (0)
            {
check_rot_right_quarter:
                /* At this point we know that the condition for the following loop is true, so it
                 * can be a do...while rather than a while. */
                do
                {
                    /* Let's form (X coords only):
                     *
                     * c[v0].vertex  m0  c[v0^3].vertex
                     * c[v1].vertex  m1  c[v1^3].vertex
                     */
                    m0 = quarterpoint(c[v0].vertex.p.x, c[v0^3].vertex.p.x);
                    if (m0 <= pfs->rect.q.x+QUARTERPOINT_ACCURACY)
                        break;
                    m1 = quarterpoint(c[v1].vertex.p.x, c[v1^3].vertex.p.x);
                    if (m1 <= pfs->rect.q.x+QUARTERPOINT_ACCURACY)
                        break;
                    /* So, we can completely discard the left hand half of the patch. */
                    c[v0].vertex.p.x = m0;
                    c[v0].vertex.p.y = quarterpoint(c[v0].vertex.p.y, c[v0^3].vertex.p.y);
                    c[v1].vertex.p.x = m1;
                    c[v1].vertex.p.y = quarterpoint(c[v1].vertex.p.y, c[v1^3].vertex.p.y);
                    c[v0].vertex.cc[0] = (c[v0].vertex.cc[0] + 3*c[v0^3].vertex.cc[0])/4;
                    c[v1].vertex.cc[0] = (c[v1].vertex.cc[0] + 3*c[v1^3].vertex.cc[0])/4;
                    changed = 1;
                }
                while (c[v0].vertex.p.x > pfs->rect.q.x+QUARTERPOINT_ACCURACY && c[v1].vertex.p.x > pfs->rect.q.x+QUARTERPOINT_ACCURACY);
            }
        }

        /* Is the whole of our patch outside the clipping rectangle? */
        /* Tempting to try to roll this into the cases below, but that
         * doesn't work because we want <= or >= here. Do Y ones. Can't have
         * done this earlier, as the previous set of tests might have reduced
         * the range here. */
        if ((c[0].vertex.p.y <= pfs->rect.p.y &&
             c[1].vertex.p.y <= pfs->rect.p.y &&
             c[2].vertex.p.y <= pfs->rect.p.y &&
             c[3].vertex.p.y <= pfs->rect.p.y) ||
            (c[0].vertex.p.y >= pfs->rect.q.y &&
             c[1].vertex.p.y >= pfs->rect.q.y &&
             c[2].vertex.p.y >= pfs->rect.q.y &&
             c[3].vertex.p.y >= pfs->rect.q.y))
            return 0;

        /* Now, let's try to see if we can cull the patch vertically with the clipping
         * rectangle. */
        /* Non rotated cases first. Can we cull the top half? */
        if (c[0].vertex.p.y < pfs->rect.p.y && c[1].vertex.p.y < pfs->rect.p.y)
        {
            /* Check 0+1 off above. */
            v0 = 0;
            v1 = 1;
            goto check_above;
        }
        else if (c[3].vertex.p.y < pfs->rect.p.y && c[2].vertex.p.y < pfs->rect.p.y)
        {
            /* Check 3+2 off above. */
            v0 = 3;
            v1 = 2;
check_above:
            /* At this point we know that the condition for the following loop is true, so it
             * can be a do...while rather than a while. */
            do
            {
                /* Let's form (Y coords only):
                 *
                 * c[v0].vertex     c[v1].vertex
                 * m0               m1
                 * c[v0^3].vertex   c[v1^3].vertex
                 */
                m0 = midpoint(c[0].vertex.p.y, c[3].vertex.p.y);
                if (m0 >= pfs->rect.p.y)
                    goto check_above_quarter;
                m1 = midpoint(c[1].vertex.p.y, c[2].vertex.p.y);
                if (m1 >= pfs->rect.p.y)
                    goto check_above_quarter;
                /* So, we can completely discard the top half of the patch. */
                c[v0].vertex.p.x = midpoint(c[0].vertex.p.x, c[3].vertex.p.x);
                c[v0].vertex.p.y = m0;
                c[v1].vertex.p.x = midpoint(c[1].vertex.p.x, c[2].vertex.p.x);
                c[v1].vertex.p.y = m1;
                c[v0].vertex.cc[0] = (c[0].vertex.cc[0] + c[3].vertex.cc[0])/2;
                c[v1].vertex.cc[0] = (c[1].vertex.cc[0] + c[2].vertex.cc[0])/2;
                changed = 1;
            }
            while (c[v0].vertex.p.y < pfs->rect.p.y && c[v1].vertex.p.y < pfs->rect.p.y);
            if (0)
            {
check_above_quarter:
                /* At this point we know that the condition for the following loop is true, so it
                 * can be a do...while rather than a while. */
                do
                {
                    /* Let's form (Y coords only):
                     *
                     * c[v0].vertex     c[v1].vertex
                     * m0               m1
                     * x                x
                     * x                x
                     * c[v0^3].vertex   c[v1^3].vertex
                     */
                    m0 = quarterpoint(c[v0].vertex.p.y, c[v0^3].vertex.p.y);
                    if (m0 >= pfs->rect.p.y)
                        break;
                    m1 = quarterpoint(c[v1].vertex.p.y, c[v1^3].vertex.p.y);
                    if (m1 >= pfs->rect.p.y)
                        break;
                    /* So, we can completely discard the top half of the patch. */
                    c[v0].vertex.p.x = quarterpoint(c[v0].vertex.p.x, c[v0^3].vertex.p.x);
                    c[v0].vertex.p.y = m0;
                    c[v1].vertex.p.x = quarterpoint(c[v1].vertex.p.x, c[v1^3].vertex.p.x);
                    c[v1].vertex.p.y = m1;
                    c[v0].vertex.cc[0] = (c[v0].vertex.cc[0] + 3*c[v0^3].vertex.cc[0])/4;
                    c[v1].vertex.cc[0] = (c[v1].vertex.cc[0] + 3*c[v1^3].vertex.cc[0])/4;
                    changed = 1;
                }
                while (c[v0].vertex.p.y < pfs->rect.p.y && c[v1].vertex.p.y < pfs->rect.p.y);
            }
        }

        /* or the bottom half? */
        if (c[0].vertex.p.y > pfs->rect.q.y && c[1].vertex.p.y > pfs->rect.q.y)
        {
            /* Check 0+1 off bottom. */
            v0 = 0;
            v1 = 1;
            goto check_bottom;
        }
        else if (c[3].vertex.p.y > pfs->rect.q.y && c[2].vertex.p.y > pfs->rect.q.y)
        {
            /* Check 3+2 off bottom. */
            v0 = 3;
            v1 = 2;
check_bottom:
            /* At this point we know that the condition for the following loop is true, so it
             * can be a do...while rather than a while. */
            do
            {
                /* Let's form (Y coords only):
                 *
                 * c[v0].vertex     c[v1].vertex
                 * m0               m1
                 * c[v0^3].vertex   c[v1^3].vertex
                 */
                m0 = midpoint(c[0].vertex.p.y, c[3].vertex.p.y);
                if (m0 <= pfs->rect.q.y+MIDPOINT_ACCURACY)
                    goto check_bottom_quarter;
                m1 = midpoint(c[1].vertex.p.y, c[2].vertex.p.y);
                if (m1 <= pfs->rect.q.y+MIDPOINT_ACCURACY)
                    goto check_bottom_quarter;
                /* So, we can completely discard the bottom half of the patch. */
                c[v0].vertex.p.x = midpoint(c[0].vertex.p.x, c[3].vertex.p.x);
                c[v0].vertex.p.y = m0;
                c[v1].vertex.p.x = midpoint(c[1].vertex.p.x, c[2].vertex.p.x);
                c[v1].vertex.p.y = m1;
                c[v0].vertex.cc[0] = (c[0].vertex.cc[0] + c[3].vertex.cc[0])/2;
                c[v1].vertex.cc[0] = (c[1].vertex.cc[0] + c[2].vertex.cc[0])/2;
                changed = 1;
            }
            while (c[v0].vertex.p.y > pfs->rect.q.y+MIDPOINT_ACCURACY && c[v1].vertex.p.y > pfs->rect.q.y+MIDPOINT_ACCURACY);
            if (0)
            {
check_bottom_quarter:
                /* At this point we know that the condition for the following loop is true, so it
                 * can be a do...while rather than a while. */
                do
                {
                    /* Let's form (Y coords only):
                     *
                     * c[v0].vertex     c[v1].vertex
                     * x                x
                     * x                x
                     * m0               m1
                     * c[v0^3].vertex   c[v1^3].vertex
                     */
                    m0 = quarterpoint(c[v0].vertex.p.y, c[v0^3].vertex.p.y);
                    if (m0 <= pfs->rect.q.y+QUARTERPOINT_ACCURACY)
                        break;
                    m1 = quarterpoint(c[v1].vertex.p.y, c[v1^3].vertex.p.y);
                    if (m1 <= pfs->rect.q.y+QUARTERPOINT_ACCURACY)
                        break;
                    /* So, we can completely discard the bottom half of the patch. */
                    c[v0].vertex.p.x = quarterpoint(c[v0].vertex.p.x, c[v0^3].vertex.p.x);
                    c[v0].vertex.p.y = m0;
                    c[v1].vertex.p.x = quarterpoint(c[v1].vertex.p.x, c[v1^3].vertex.p.x);
                    c[v1].vertex.p.y = m1;
                    c[v0].vertex.cc[0] = (c[v0].vertex.cc[0] + 3*c[v0^3].vertex.cc[0])/4;
                    c[v1].vertex.cc[0] = (c[v1].vertex.cc[0] + 3*c[v1^3].vertex.cc[0])/4;
                    changed = 1;
                }
                while (c[v0].vertex.p.y > pfs->rect.q.y+QUARTERPOINT_ACCURACY && c[v1].vertex.p.y > pfs->rect.q.y+QUARTERPOINT_ACCURACY);
            }
        }

        /* Now, rotated cases: Can we cull the top half? */
        if (c[0].vertex.p.y < pfs->rect.p.y && c[3].vertex.p.y < pfs->rect.p.y)
        {
            /* Check 0+3 off above. */
            v0 = 0;
            v1 = 3;
            goto check_rot_above;
        }
        else if (c[1].vertex.p.y < pfs->rect.p.y && c[2].vertex.p.y < pfs->rect.p.y)
        {
            /* Check 1+2 off above. */
            v0 = 1;
            v1 = 2;
check_rot_above:
            /* At this point we know that the condition for the following loop is true, so it
             * can be a do...while rather than a while. */
            do
            {
                /* Let's form (Y coords only):
                 *
                 * c[v0].vertex     c[v1].vertex
                 * m0               m1
                 * c[v0^1].vertex   c[v1^1].vertex
                 */
                m0 = midpoint(c[0].vertex.p.y, c[1].vertex.p.y);
                if (m0 >= pfs->rect.p.y)
                    goto check_rot_above_quarter;
                m1 = midpoint(c[3].vertex.p.y, c[2].vertex.p.y);
                if (m1 >= pfs->rect.p.y)
                    goto check_rot_above_quarter;
                /* So, we can completely discard the top half of the patch. */
                c[v0].vertex.p.x = midpoint(c[0].vertex.p.x, c[1].vertex.p.x);
                c[v0].vertex.p.y = m0;
                c[v1].vertex.p.x = midpoint(c[3].vertex.p.x, c[2].vertex.p.x);
                c[v1].vertex.p.y = m1;
                c[v0].vertex.cc[0] = (c[0].vertex.cc[0] + c[1].vertex.cc[0])/2;
                c[v1].vertex.cc[0] = (c[3].vertex.cc[0] + c[2].vertex.cc[0])/2;
                changed = 1;
            }
            while (c[v0].vertex.p.y < pfs->rect.p.y && c[v1].vertex.p.y < pfs->rect.p.y);
            if (0)
            {
check_rot_above_quarter:
                /* At this point we know that the condition for the following loop is true, so it
                 * can be a do...while rather than a while. */
                do
                {
                    /* Let's form (Y coords only):
                     *
                     * c[v0].vertex     c[v1].vertex
                     * m0               m1
                     * x                x
                     * x                x
                     * c[v0^1].vertex   c[v1^1].vertex
                     */
                    m0 = quarterpoint(c[v0].vertex.p.y, c[v0^1].vertex.p.y);
                    if (m0 >= pfs->rect.p.y)
                        break;
                    m1 = quarterpoint(c[v1].vertex.p.y, c[v1^1].vertex.p.y);
                    if (m1 >= pfs->rect.p.y)
                        break;
                    /* So, we can completely discard the top half of the patch. */
                    c[v0].vertex.p.x = quarterpoint(c[v0].vertex.p.x, c[v0^1].vertex.p.x);
                    c[v0].vertex.p.y = m0;
                    c[v1].vertex.p.x = quarterpoint(c[v1].vertex.p.x, c[v1^1].vertex.p.x);
                    c[v1].vertex.p.y = m1;
                    c[v0].vertex.cc[0] = (c[v0].vertex.cc[0] + 3*c[v0^1].vertex.cc[0])/4;
                    c[v1].vertex.cc[0] = (c[v1].vertex.cc[0] + 3*c[v1^1].vertex.cc[0])/4;
                    changed = 1;
                }
                while (c[v0].vertex.p.y < pfs->rect.p.y && c[v1].vertex.p.y < pfs->rect.p.y);
            }
        }

        /* or the bottom half? */
        if (c[0].vertex.p.y > pfs->rect.q.y && c[3].vertex.p.y > pfs->rect.q.y)
        {
            /* Check 0+3 off the bottom. */
            v0 = 0;
            v1 = 3;
            goto check_rot_bottom;
        }
        else if (c[1].vertex.p.y > pfs->rect.q.y && c[2].vertex.p.y > pfs->rect.q.y)
        {
            /* Check 1+2 off the bottom. */
            v0 = 1;
            v1 = 2;
check_rot_bottom:
            /* At this point we know that the condition for the following loop is true, so it
             * can be a do...while rather than a while. */
            do
            {
                /* Let's form (Y coords only):
                 *
                 * c[v0].vertex     c[v1].vertex
                 * m0               m1
                 * c[v0^1].vertex   c[v1^1].vertex
                 */
                m0 = midpoint(c[0].vertex.p.y, c[1].vertex.p.y);
                if (m0 <= pfs->rect.q.y+MIDPOINT_ACCURACY)
                    goto check_rot_bottom_quarter;
                m1 = midpoint(c[3].vertex.p.y, c[2].vertex.p.y);
                if (m1 <= pfs->rect.q.y+MIDPOINT_ACCURACY)
                    goto check_rot_bottom_quarter;
                /* So, we can completely discard the left hand half of the patch. */
                c[v0].vertex.p.x = midpoint(c[0].vertex.p.x, c[1].vertex.p.x);
                c[v0].vertex.p.y = m0;
                c[v1].vertex.p.x = midpoint(c[3].vertex.p.x, c[2].vertex.p.x);
                c[v1].vertex.p.y = m1;
                c[v0].vertex.cc[0] = (c[0].vertex.cc[0] + c[1].vertex.cc[0])/2;
                c[v1].vertex.cc[0] = (c[3].vertex.cc[0] + c[2].vertex.cc[0])/2;
                changed = 1;
            }
            while (c[v0].vertex.p.y > pfs->rect.q.y+MIDPOINT_ACCURACY && c[v1].vertex.p.y > pfs->rect.q.y+MIDPOINT_ACCURACY);
            if (0)
            {
check_rot_bottom_quarter:
                /* At this point we know that the condition for the following loop is true, so it
                 * can be a do...while rather than a while. */
                do
                {
                    /* Let's form (Y coords only):
                     *
                     * c[v0].vertex     c[v1].vertex
                     * x                x
                     * x                x
                     * m0               m1
                     * c[v0^1].vertex   c[v1^1].vertex
                     */
                    m0 = quarterpoint(c[v0].vertex.p.y, c[v0^1].vertex.p.y);
                    if (m0 <= pfs->rect.q.y+QUARTERPOINT_ACCURACY)
                        break;
                    m1 = quarterpoint(c[v1].vertex.p.y, c[v1^1].vertex.p.y);
                    if (m1 <= pfs->rect.q.y+QUARTERPOINT_ACCURACY)
                        break;
                    /* So, we can completely discard the left hand half of the patch. */
                    c[v0].vertex.p.x = quarterpoint(c[v0].vertex.p.x, c[v0^1].vertex.p.x);
                    c[v0].vertex.p.y = m0;
                    c[v1].vertex.p.x = quarterpoint(c[v1].vertex.p.x, c[v1^1].vertex.p.x);
                    c[v1].vertex.p.y = m1;
                    c[v0].vertex.cc[0] = (c[v0].vertex.cc[0] + 3*c[v0^1].vertex.cc[0])/4;
                    c[v1].vertex.cc[0] = (c[v1].vertex.cc[0] + 3*c[v1^1].vertex.cc[0])/4;
                    changed = 1;
                }
                while (c[v0].vertex.p.y > pfs->rect.q.y+QUARTERPOINT_ACCURACY && c[v1].vertex.p.y > pfs->rect.q.y+QUARTERPOINT_ACCURACY);
            }
        }
    } while (changed);

    c[0].vertex.cc[1] = c[1].vertex.cc[1] =
                        c[2].vertex.cc[1] =
                        c[3].vertex.cc[1] = 0;
    make_other_poles(c);
    return patch_fill(pfs, c, NULL, NULL);
}
#undef midpoint
#undef quarterpoint
#undef MIDPOINT_ACCURACY
#undef QUARTERPOINT_ACCURACY

#define f_fits_in_fixed(f) f_fits_in_bits(f, fixed_int_bits)

static int
A_fill_region_floats(patch_fill_state_t *pfs1, corners_and_curves *cc, int depth)
{
    corners_and_curves sub[4];
    int code;

    if (depth == 32)
        return gs_error_limitcheck;

    if (depth > 0 &&
        f_fits_in_fixed(cc->corners[0].x) &&
        f_fits_in_fixed(cc->corners[0].y) &&
        f_fits_in_fixed(cc->corners[1].x) &&
        f_fits_in_fixed(cc->corners[1].y) &&
        f_fits_in_fixed(cc->corners[2].x) &&
        f_fits_in_fixed(cc->corners[2].y) &&
        f_fits_in_fixed(cc->corners[3].x) &&
        f_fits_in_fixed(cc->corners[3].y))
    {
        cc->curve[0].vertex.p.x = float2fixed(cc->corners[0].x);
        cc->curve[0].vertex.p.y = float2fixed(cc->corners[0].y);
        cc->curve[1].vertex.p.x = float2fixed(cc->corners[1].x);
        cc->curve[1].vertex.p.y = float2fixed(cc->corners[1].y);
        cc->curve[2].vertex.p.x = float2fixed(cc->corners[2].x);
        cc->curve[2].vertex.p.y = float2fixed(cc->corners[2].y);
        cc->curve[3].vertex.p.x = float2fixed(cc->corners[3].x);
        cc->curve[3].vertex.p.y = float2fixed(cc->corners[3].y);
        return subdivide_patch_fill(pfs1, cc->curve);
    }

    /* We have patches with corners:
     *  0  1
     *  3  2
     * We subdivide these into 4 smaller patches:
     *
     *  0   10   1     Where 0123 are corners
     *   [0]  [1]      [0][1][2][3] are patches.
     *  3   23   2
     *  0   10   1
     *   [3]  [2]
     *  3   23   2
     */

    sub[0].corners[0].x = cc->corners[0].x;
    sub[0].corners[0].y = cc->corners[0].y;
    sub[1].corners[1].x = cc->corners[1].x;
    sub[1].corners[1].y = cc->corners[1].y;
    sub[2].corners[2].x = cc->corners[2].x;
    sub[2].corners[2].y = cc->corners[2].y;
    sub[3].corners[3].x = cc->corners[3].x;
    sub[3].corners[3].y = cc->corners[3].y;
    sub[1].corners[0].x = sub[0].corners[1].x = (cc->corners[0].x + cc->corners[1].x)/2;
    sub[1].corners[0].y = sub[0].corners[1].y = (cc->corners[0].y + cc->corners[1].y)/2;
    sub[3].corners[2].x = sub[2].corners[3].x = (cc->corners[2].x + cc->corners[3].x)/2;
    sub[3].corners[2].y = sub[2].corners[3].y = (cc->corners[2].y + cc->corners[3].y)/2;
    sub[3].corners[0].x = sub[0].corners[3].x = (cc->corners[0].x + cc->corners[3].x)/2;
    sub[3].corners[0].y = sub[0].corners[3].y = (cc->corners[0].y + cc->corners[3].y)/2;
    sub[2].corners[1].x = sub[1].corners[2].x = (cc->corners[1].x + cc->corners[2].x)/2;
    sub[2].corners[1].y = sub[1].corners[2].y = (cc->corners[1].y + cc->corners[2].y)/2;
    sub[0].corners[2].x = sub[1].corners[3].x =
                          sub[2].corners[0].x =
                          sub[3].corners[1].x = (sub[0].corners[3].x + sub[1].corners[2].x)/2;
    sub[0].corners[2].y = sub[1].corners[3].y =
                          sub[2].corners[0].y =
                          sub[3].corners[1].y = (sub[0].corners[3].y + sub[1].corners[2].y)/2;
    sub[0].curve[0].vertex.cc[0] = sub[0].curve[3].vertex.cc[0] =
                                   sub[3].curve[0].vertex.cc[0] =
                                   sub[3].curve[3].vertex.cc[0] = cc->curve[0].vertex.cc[0];
    sub[1].curve[1].vertex.cc[0] = sub[1].curve[2].vertex.cc[0] =
                                   sub[2].curve[1].vertex.cc[0] =
                                   sub[2].curve[2].vertex.cc[0] = cc->curve[1].vertex.cc[0];
    sub[0].curve[1].vertex.cc[0] = sub[0].curve[2].vertex.cc[0] =
                                   sub[1].curve[0].vertex.cc[0] =
                                   sub[1].curve[3].vertex.cc[0] =
                                   sub[2].curve[0].vertex.cc[0] =
                                   sub[2].curve[3].vertex.cc[0] =
                                   sub[3].curve[1].vertex.cc[0] =
                                   sub[3].curve[2].vertex.cc[0] = (cc->curve[0].vertex.cc[0] + cc->curve[1].vertex.cc[0])/2;

    depth++;
    if (not_clipped_away(sub[0].corners, &pfs1->rect)) {
        code = A_fill_region_floats(pfs1, &sub[0], depth);
        if (code < 0)
            return code;
    }
    if (not_clipped_away(sub[1].corners, &pfs1->rect)) {
        code = A_fill_region_floats(pfs1, &sub[1], depth);
        if (code < 0)
            return code;
    }
    if (not_clipped_away(sub[2].corners, &pfs1->rect)) {
        code = A_fill_region_floats(pfs1, &sub[2], depth);
        if (code < 0)
            return code;
    }
    if (not_clipped_away(sub[3].corners, &pfs1->rect)) {
        code = A_fill_region_floats(pfs1, &sub[3], depth);
        if (code < 0)
            return code;
    }

    return 0;
}

#define midpoint(a,b)      ((a+b)/2)

#define quarterpoint(a,b)  ((a+3*b)/4)

static int
subdivide_patch_fill_floats(patch_fill_state_t *pfs, corners_and_curves *cc)
{
    double m0, m1;
    int v0, v1;
    int changed;

    if (pfs->rect.p.x >= pfs->rect.q.x || pfs->rect.p.y >= pfs->rect.q.y)
        return 0;

    /* On entry we have a patch:
     *   c[0].vertex  c[1].vertex
     *
     *   c[3].vertex  c[2].vertex
     *
     * Only the corners are set. The control points are not!
     *
     * BUT... in terms of spacial coords, it might be different...
     * They might be flipped on X, Y or both, giving:
     *  01 or 10 or 32 or 23
     *  32    23    01    10
     * or they might be rotated, and then flipped on X, Y or both, giving:
     *  03 or 30 or 12 or 21
     *  12    21    03    30
     */

    /* The +MIDPOINT_ACCURACY in the tests below is to allow for us finding the midpoint of [a] = z+1 and [b] = z, and getting z+1,
     * and updating [a] to be z+1, hence never actually shrinking the gap. Just accept not culling the patch as
     * much as we might. See bug 706378 for an example. */
#define MIDPOINT_ACCURACY 0.0001
#define QUARTERPOINT_ACCURACY 0.0003

    do {
        changed = 0;

        /* Is the whole of our patch outside the clipping rectangle? */
        /* Tempting to try to roll this into the cases below, but that
         * doesn't work because we want <= or >= here. Do the X ones
         * first. */
        if ((cc->corners[0].x <= pfs->rect.p.x &&
             cc->corners[1].x <= pfs->rect.p.x &&
             cc->corners[2].x <= pfs->rect.p.x &&
             cc->corners[3].x <= pfs->rect.p.x) ||
            (cc->corners[0].x >= pfs->rect.q.x &&
             cc->corners[1].x >= pfs->rect.q.x &&
             cc->corners[2].x >= pfs->rect.q.x &&
             cc->corners[3].x >= pfs->rect.q.x))
                return 0;

        /* First, let's try to see if we can cull the patch horizontally with the clipping
         * rectangle. */
        /* Non rotated cases first. Can we cull the left hand half? */
        if (cc->corners[0].x < pfs->rect.p.x && cc->corners[3].x < pfs->rect.p.x)
        {
            /* Check 0+3 off left. */
            v0 = 0;
            v1 = 3;
            goto check_left;
        }
        else if (cc->corners[1].x < pfs->rect.p.x && cc->corners[2].x < pfs->rect.p.x)
        {
            /* Check 1+2 off left. */
            v0 = 1;
            v1 = 2;
check_left:
            /* At this point we know that the condition for the following loop is true, so it
             * can be a do...while rather than a while. */
            do
            {
                /* Let's form (X coords only):
                 *
                 * c[v0].vertex  m0  c[v0^1].vertex
                 * c[v1].vertex  m1  c[v1^1].vertex
                 */
                m0 = midpoint(cc->corners[0].x, cc->corners[1].x);
                if (m0 >= pfs->rect.p.x)
                    goto check_left_quarter;
                m1 = midpoint(cc->corners[3].x, cc->corners[2].x);
                if (m1 >= pfs->rect.p.x)
                    goto check_left_quarter;
                /* So, we can completely discard the left hand half of the patch. */
                cc->corners[v0].x = m0;
                cc->corners[v0].y = midpoint(cc->corners[0].y, cc->corners[1].y);
                cc->corners[v1].x = m1;
                cc->corners[v1].y = midpoint(cc->corners[3].y, cc->corners[2].y);
                cc->curve[v0].vertex.cc[0] = (cc->curve[0].vertex.cc[0] + cc->curve[1].vertex.cc[0])/2;
                cc->curve[v1].vertex.cc[0] = (cc->curve[3].vertex.cc[0] + cc->curve[2].vertex.cc[0])/2;
                changed = 1;
            }
            while (cc->corners[v0].x < pfs->rect.p.x && cc->corners[v1].x < pfs->rect.p.x);
            if (0)
            {
check_left_quarter:
                /* At this point we know that the condition for the following loop is true, so it
                 * can be a do...while rather than a while. */
                do
                {
                    /* Let's form (X coords only):
                     *
                     * c[v0].vertex  m0  x  x  c[v0^1].vertex
                     * c[v1].vertex  m1  x  x  c[v1^1].vertex
                     */
                    m0 = quarterpoint(cc->corners[v0].x, cc->corners[v0^1].x);
                    if (m0 >= pfs->rect.p.x)
                        break;
                    m1 = quarterpoint(cc->corners[v1].x, cc->corners[v1^1].x);
                    if (m1 >= pfs->rect.p.x)
                        break;
                    /* So, we can completely discard the left hand quarter of the patch. */
                    cc->corners[v0].x = m0;
                    cc->corners[v0].y = midpoint(cc->corners[v0].y, cc->corners[v0^1].y);
                    cc->corners[v1].x = m1;
                    cc->corners[v1].y = midpoint(cc->corners[v1].y, cc->corners[v1^1].y);
                    cc->curve[v0].vertex.cc[0] = (cc->curve[v0].vertex.cc[0] + 3*cc->curve[v0^1].vertex.cc[0])/4;
                    cc->curve[v1].vertex.cc[0] = (cc->curve[v1].vertex.cc[0] + 3*cc->curve[v1^1].vertex.cc[0])/4;
                    changed = 1;
                }
                while (cc->corners[v0].x < pfs->rect.p.x && cc->corners[v1].x < pfs->rect.p.x);
            }
        }

        /* or the right hand half? */
        if (cc->corners[0].x > pfs->rect.q.x && cc->corners[3].x > pfs->rect.q.x)
        {
            /* Check 0+3 off right. */
            v0 = 0;
            v1 = 3;
            goto check_right;
        }
        else if (cc->corners[1].x > pfs->rect.q.x && cc->corners[2].x > pfs->rect.q.x)
        {
            /* Check 1+2 off right. */
            v0 = 1;
            v1 = 2;
check_right:
            /* At this point we know that the condition for the following loop is true, so it
             * can be a do...while rather than a while. */
            do
            {
                /* Let's form (X coords only):
                 *
                 * c[v0].vertex  m0  c[v0^1].vertex
                 * c[v1].vertex  m1  c[v1^1].vertex
                 */
                m0 = midpoint(cc->corners[0].x, cc->corners[1].x);
                if (m0 <= pfs->rect.q.x+MIDPOINT_ACCURACY)
                    goto check_right_quarter;
                m1 = midpoint(cc->corners[3].x, cc->corners[2].x);
                if (m1 <= pfs->rect.q.x+MIDPOINT_ACCURACY)
                    goto check_right_quarter;
                /* So, we can completely discard the left hand half of the patch. */
                cc->corners[v0].x = m0;
                cc->corners[v0].y = midpoint(cc->corners[0].y, cc->corners[1].y);
                cc->corners[v1].x = m1;
                cc->corners[v1].y = midpoint(cc->corners[3].y, cc->corners[2].y);
                cc->curve[v0].vertex.cc[0] = (cc->curve[0].vertex.cc[0] + cc->curve[1].vertex.cc[0])/2;
                cc->curve[v1].vertex.cc[0] = (cc->curve[3].vertex.cc[0] + cc->curve[2].vertex.cc[0])/2;
                changed = 1;
            }
            while (cc->corners[v0].x > pfs->rect.q.x+MIDPOINT_ACCURACY && cc->corners[v1].x > pfs->rect.q.x+MIDPOINT_ACCURACY);
            if (0)
            {
check_right_quarter:
                /* At this point we know that the condition for the following loop is true, so it
                 * can be a do...while rather than a while. */
                do
                {
                    /* Let's form (X coords only):
                     *
                     * c[v0].vertex  m0  x  x  c[v0^1].vertex
                     * c[v1].vertex  m1  x  x  c[v1^1].vertex
                     */
                    m0 = quarterpoint(cc->corners[v0].x, cc->corners[v0^1].x);
                    if (m0 <= pfs->rect.q.x+QUARTERPOINT_ACCURACY)
                        break;
                    m1 = quarterpoint(cc->corners[v1].x, cc->corners[v1^1].x);
                    if (m1 <= pfs->rect.q.x+QUARTERPOINT_ACCURACY)
                        break;
                    /* So, we can completely discard the left hand half of the patch. */
                    cc->corners[v0].x = m0;
                    cc->corners[v0].y = quarterpoint(cc->corners[v0].y, cc->corners[v0^1].y);
                    cc->corners[v1].x = m1;
                    cc->corners[v1].y = quarterpoint(cc->corners[v1].y, cc->corners[v1^1].y);
                    cc->curve[v0].vertex.cc[0] = (cc->curve[v0].vertex.cc[0] + 3*cc->curve[v0^1].vertex.cc[0])/4;
                    cc->curve[v1].vertex.cc[0] = (cc->curve[v1].vertex.cc[0] + 3*cc->curve[v1^1].vertex.cc[0])/4;
                    changed = 1;
                }
                while (cc->corners[v0].x > pfs->rect.q.x+QUARTERPOINT_ACCURACY && cc->corners[v1].x > pfs->rect.q.x+QUARTERPOINT_ACCURACY);
            }
        }

        /* Now, rotated cases: Can we cull the left hand half? */
        if (cc->corners[0].x < pfs->rect.p.x && cc->corners[1].x < pfs->rect.p.x)
        {
            /* Check 0+1 off left. */
            v0 = 0;
            v1 = 1;
            goto check_rot_left;
        }
        else if (cc->corners[3].x < pfs->rect.p.x && cc->corners[2].x < pfs->rect.p.x)
        {
            /* Check 3+2 off left. */
            v0 = 3;
            v1 = 2;
check_rot_left:
            /* At this point we know that the condition for the following loop is true, so it
             * can be a do...while rather than a while. */
            do
            {
                /* Let's form (X coords only):
                 *
                 * c[v0].vertex    m0  c[v0^3].vertex
                 * c[v1^3].vertex  m1  c[v1].vertex
                 */
                m0 = midpoint(cc->corners[0].x, cc->corners[3].x);
                if (m0 >= pfs->rect.p.x)
                    goto check_rot_left_quarter;
                m1 = midpoint(cc->corners[1].x, cc->corners[2].x);
                if (m1 >= pfs->rect.p.x)
                    goto check_rot_left_quarter;
                /* So, we can completely discard the left hand half of the patch. */
                cc->corners[v0].x = m0;
                cc->corners[v0].y = midpoint(cc->corners[0].y, cc->corners[3].y);
                cc->corners[v1].x = m1;
                cc->corners[v1].y = midpoint(cc->corners[1].y, cc->corners[2].y);
                cc->curve[v0].vertex.cc[0] = (cc->curve[0].vertex.cc[0] + cc->curve[3].vertex.cc[0])/2;
                cc->curve[v1].vertex.cc[0] = (cc->curve[1].vertex.cc[0] + cc->curve[2].vertex.cc[0])/2;
                changed = 1;
            }
            while (cc->corners[v0].x < pfs->rect.p.x && cc->corners[v1].x < pfs->rect.p.x);
            if (0)
            {
check_rot_left_quarter:
                /* At this point we know that the condition for the following loop is true, so it
                 * can be a do...while rather than a while. */
                do
                {
                    /* Let's form (X coords only):
                     *
                     * c[v0].vertex  m0  x  x  c[v0^3].vertex
                     * c[v1].vertex  m1  x  x  c[v1^3].vertex
                     */
                    m0 = quarterpoint(cc->corners[v0].x, cc->corners[v0^3].x);
                    if (m0 >= pfs->rect.p.x)
                        break;
                    m1 = quarterpoint(cc->corners[v1].x, cc->corners[v1^3].x);
                    if (m1 >= pfs->rect.p.x)
                        break;
                    /* So, we can completely discard the left hand half of the patch. */
                    cc->corners[v0].x = m0;
                    cc->corners[v0].y = quarterpoint(cc->corners[v0].y, cc->corners[v0^3].y);
                    cc->corners[v1].x = m1;
                    cc->corners[v1].y = quarterpoint(cc->corners[v1].y, cc->corners[v1^3].y);
                    cc->curve[v0].vertex.cc[0] = (cc->curve[v0].vertex.cc[0] + 3*cc->curve[v0^3].vertex.cc[0])/4;
                    cc->curve[v1].vertex.cc[0] = (cc->curve[v1].vertex.cc[0] + 3*cc->curve[v1^3].vertex.cc[0])/4;
                    changed = 1;
                }
                while (cc->corners[v0].x < pfs->rect.p.x && cc->corners[v1].x < pfs->rect.p.x);
            }
        }

        /* or the right hand half? */
        if (cc->corners[0].x > pfs->rect.q.x && cc->corners[1].x > pfs->rect.q.x)
        {
            /* Check 0+1 off right. */
            v0 = 0;
            v1 = 1;
            goto check_rot_right;
        }
        else if (cc->corners[3].x > pfs->rect.q.x && cc->corners[2].x > pfs->rect.q.x)
        {
            /* Check 3+2 off right. */
            v0 = 3;
            v1 = 2;
check_rot_right:
            /* At this point we know that the condition for the following loop is true, so it
             * can be a do...while rather than a while. */
            do
            {
                /* Let's form (X coords only):
                 *
                 * c[v0].vertex  m0  c[v0^3].vertex
                 * c[v1].vertex  m1  c[v1^3].vertex
                 */
                m0 = midpoint(cc->corners[0].x, cc->corners[3].x);
                if (m0 <= pfs->rect.q.x+MIDPOINT_ACCURACY)
                    goto check_rot_right_quarter;
                m1 = midpoint(cc->corners[1].x, cc->corners[2].x);
                if (m1 <= pfs->rect.q.x+MIDPOINT_ACCURACY)
                    goto check_rot_right_quarter;
                /* So, we can completely discard the left hand half of the patch. */
                cc->corners[v0].x = m0;
                cc->corners[v0].y = midpoint(cc->corners[0].y, cc->corners[3].y);
                cc->corners[v1].x = m1;
                cc->corners[v1].y = midpoint(cc->corners[1].y, cc->corners[2].y);
                cc->curve[v0].vertex.cc[0] = (cc->curve[0].vertex.cc[0] + cc->curve[3].vertex.cc[0])/2;
                cc->curve[v1].vertex.cc[0] = (cc->curve[1].vertex.cc[0] + cc->curve[2].vertex.cc[0])/2;
                changed = 1;
            }
            while (cc->corners[v0].x > pfs->rect.q.x+MIDPOINT_ACCURACY && cc->corners[v1].x > pfs->rect.q.x+MIDPOINT_ACCURACY);
            if (0)
            {
check_rot_right_quarter:
                /* At this point we know that the condition for the following loop is true, so it
                 * can be a do...while rather than a while. */
                do
                {
                    /* Let's form (X coords only):
                     *
                     * c[v0].vertex  m0  c[v0^3].vertex
                     * c[v1].vertex  m1  c[v1^3].vertex
                     */
                    m0 = quarterpoint(cc->corners[v0].x, cc->corners[v0^3].x);
                    if (m0 <= pfs->rect.q.x+QUARTERPOINT_ACCURACY)
                        break;
                    m1 = quarterpoint(cc->corners[v1].x, cc->corners[v1^3].x);
                    if (m1 <= pfs->rect.q.x+QUARTERPOINT_ACCURACY)
                        break;
                    /* So, we can completely discard the left hand half of the patch. */
                    cc->corners[v0].x = m0;
                    cc->corners[v0].y = quarterpoint(cc->corners[v0].y, cc->corners[v0^3].y);
                    cc->corners[v1].x = m1;
                    cc->corners[v1].y = quarterpoint(cc->corners[v1].y, cc->corners[v1^3].y);
                    cc->curve[v0].vertex.cc[0] = (cc->curve[v0].vertex.cc[0] + 3*cc->curve[v0^3].vertex.cc[0])/4;
                    cc->curve[v1].vertex.cc[0] = (cc->curve[v1].vertex.cc[0] + 3*cc->curve[v1^3].vertex.cc[0])/4;
                    changed = 1;
                }
                while (cc->corners[v0].x > pfs->rect.q.x+QUARTERPOINT_ACCURACY && cc->corners[v1].x > pfs->rect.q.x+QUARTERPOINT_ACCURACY);
            }
        }

        /* Is the whole of our patch outside the clipping rectangle? */
        /* Tempting to try to roll this into the cases below, but that
         * doesn't work because we want <= or >= here. Do the Y ones.
         * Can't do these at the same time as the X ones, as the cases
         * above may have reduced Y by the time we get here. */
        if ((cc->corners[0].y <= pfs->rect.p.y &&
             cc->corners[1].y <= pfs->rect.p.y &&
             cc->corners[2].y <= pfs->rect.p.y &&
             cc->corners[3].y <= pfs->rect.p.y) ||
            (cc->corners[0].y >= pfs->rect.q.y &&
             cc->corners[1].y >= pfs->rect.q.y &&
             cc->corners[2].y >= pfs->rect.q.y &&
             cc->corners[3].y >= pfs->rect.q.y))
            return 0;

        /* Now, let's try to see if we can cull the patch vertically with the clipping
         * rectangle. */
        /* Non rotated cases first. Can we cull the top half? */
        if (cc->corners[0].y < pfs->rect.p.y && cc->corners[1].y < pfs->rect.p.y)
        {
            /* Check 0+1 off above. */
            v0 = 0;
            v1 = 1;
            goto check_above;
        }
        else if (cc->corners[3].y < pfs->rect.p.y && cc->corners[2].y < pfs->rect.p.y)
        {
            /* Check 3+2 off above. */
            v0 = 3;
            v1 = 2;
check_above:
            /* At this point we know that the condition for the following loop is true, so it
             * can be a do...while rather than a while. */
            do
            {
                /* Let's form (Y coords only):
                 *
                 * c[v0].vertex     c[v1].vertex
                 * m0               m1
                 * c[v0^3].vertex   c[v1^3].vertex
                 */
                m0 = midpoint(cc->corners[0].y, cc->corners[3].y);
                if (m0 >= pfs->rect.p.y)
                    goto check_above_quarter;
                m1 = midpoint(cc->corners[1].y, cc->corners[2].y);
                if (m1 >= pfs->rect.p.y)
                    goto check_above_quarter;
                /* So, we can completely discard the top half of the patch. */
                cc->corners[v0].x = midpoint(cc->corners[0].x, cc->corners[3].x);
                cc->corners[v0].y = m0;
                cc->corners[v1].x = midpoint(cc->corners[1].x, cc->corners[2].x);
                cc->corners[v1].y = m1;
                cc->curve[v0].vertex.cc[0] = (cc->curve[0].vertex.cc[0] + cc->curve[3].vertex.cc[0])/2;
                cc->curve[v1].vertex.cc[0] = (cc->curve[1].vertex.cc[0] + cc->curve[2].vertex.cc[0])/2;
                changed = 1;
            }
            while (cc->corners[v0].y < pfs->rect.p.y && cc->corners[v1].y < pfs->rect.p.y);
            if (0)
            {
check_above_quarter:
                /* At this point we know that the condition for the following loop is true, so it
                 * can be a do...while rather than a while. */
                do
                {
                    /* Let's form (Y coords only):
                     *
                     * c[v0].vertex     c[v1].vertex
                     * m0               m1
                     * x                x
                     * x                x
                     * c[v0^3].vertex   c[v1^3].vertex
                     */
                    m0 = quarterpoint(cc->corners[v0].y, cc->corners[v0^3].y);
                    if (m0 >= pfs->rect.p.y)
                        break;
                    m1 = quarterpoint(cc->corners[v1].y, cc->corners[v1^3].y);
                    if (m1 >= pfs->rect.p.y)
                        break;
                    /* So, we can completely discard the top half of the patch. */
                    cc->corners[v0].x = quarterpoint(cc->corners[v0].x, cc->corners[v0^3].x);
                    cc->corners[v0].y = m0;
                    cc->corners[v1].x = quarterpoint(cc->corners[v1].x, cc->corners[v1^3].x);
                    cc->corners[v1].y = m1;
                    cc->curve[v0].vertex.cc[0] = (cc->curve[v0].vertex.cc[0] + 3*cc->curve[v0^3].vertex.cc[0])/4;
                    cc->curve[v1].vertex.cc[0] = (cc->curve[v1].vertex.cc[0] + 3*cc->curve[v1^3].vertex.cc[0])/4;
                    changed = 1;
                }
                while (cc->corners[v0].y < pfs->rect.p.y && cc->corners[v1].y < pfs->rect.p.y);
            }
        }

        /* or the bottom half? */
        if (cc->corners[0].y > pfs->rect.q.y && cc->corners[1].y > pfs->rect.q.y)
        {
            /* Check 0+1 off bottom. */
            v0 = 0;
            v1 = 1;
            goto check_bottom;
        }
        else if (cc->corners[3].y > pfs->rect.q.y && cc->corners[2].y > pfs->rect.q.y)
        {
            /* Check 3+2 off bottom. */
            v0 = 3;
            v1 = 2;
check_bottom:
            /* At this point we know that the condition for the following loop is true, so it
             * can be a do...while rather than a while. */
            do
            {
                /* Let's form (Y coords only):
                 *
                 * c[v0].vertex     c[v1].vertex
                 * m0               m1
                 * c[v0^3].vertex   c[v1^3].vertex
                 */
                m0 = midpoint(cc->corners[0].y, cc->corners[3].y);
                if (m0 <= pfs->rect.q.y+MIDPOINT_ACCURACY)
                    goto check_bottom_quarter;
                m1 = midpoint(cc->corners[1].y, cc->corners[2].y);
                if (m1 <= pfs->rect.q.y+MIDPOINT_ACCURACY)
                    goto check_bottom_quarter;
                /* So, we can completely discard the bottom half of the patch. */
                cc->corners[v0].x = midpoint(cc->corners[0].x, cc->corners[3].x);
                cc->corners[v0].y = m0;
                cc->corners[v1].x = midpoint(cc->corners[1].x, cc->corners[2].x);
                cc->corners[v1].y = m1;
                cc->curve[v0].vertex.cc[0] = (cc->curve[0].vertex.cc[0] + cc->curve[3].vertex.cc[0])/2;
                cc->curve[v1].vertex.cc[0] = (cc->curve[1].vertex.cc[0] + cc->curve[2].vertex.cc[0])/2;
                changed = 1;
            }
            while (cc->corners[v0].y > pfs->rect.q.y+MIDPOINT_ACCURACY && cc->corners[v1].y > pfs->rect.q.y+MIDPOINT_ACCURACY);
            if (0)
            {
check_bottom_quarter:
                /* At this point we know that the condition for the following loop is true, so it
                 * can be a do...while rather than a while. */
                do
                {
                    /* Let's form (Y coords only):
                     *
                     * c[v0].vertex     c[v1].vertex
                     * x                x
                     * x                x
                     * m0               m1
                     * c[v0^3].vertex   c[v1^3].vertex
                     */
                    m0 = quarterpoint(cc->corners[v0].y, cc->corners[v0^3].y);
                    if (m0 <= pfs->rect.q.y+QUARTERPOINT_ACCURACY)
                        break;
                    m1 = quarterpoint(cc->corners[v1].y, cc->corners[v1^3].y);
                    if (m1 <= pfs->rect.q.y+QUARTERPOINT_ACCURACY)
                        break;
                    /* So, we can completely discard the bottom half of the patch. */
                    cc->corners[v0].x = quarterpoint(cc->corners[v0].x, cc->corners[v0^3].x);
                    cc->corners[v0].y = m0;
                    cc->corners[v1].x = quarterpoint(cc->corners[v1].x, cc->corners[v1^3].x);
                    cc->corners[v1].y = m1;
                    cc->curve[v0].vertex.cc[0] = (cc->curve[v0].vertex.cc[0] + 3*cc->curve[v0^3].vertex.cc[0])/4;
                    cc->curve[v1].vertex.cc[0] = (cc->curve[v1].vertex.cc[0] + 3*cc->curve[v1^3].vertex.cc[0])/4;
                    changed = 1;
                }
                while (cc->corners[v0].y > pfs->rect.q.y+QUARTERPOINT_ACCURACY && cc->corners[v1].y > pfs->rect.q.y+QUARTERPOINT_ACCURACY);
            }
        }

        /* Now, rotated cases: Can we cull the top half? */
        if (cc->corners[0].y < pfs->rect.p.y && cc->corners[3].y < pfs->rect.p.y)
        {
            /* Check 0+3 off above. */
            v0 = 0;
            v1 = 3;
            goto check_rot_above;
        }
        else if (cc->corners[1].y < pfs->rect.p.y && cc->corners[2].y < pfs->rect.p.y)
        {
            /* Check 1+2 off above. */
            v0 = 1;
            v1 = 2;
check_rot_above:
            /* At this point we know that the condition for the following loop is true, so it
             * can be a do...while rather than a while. */
            do
            {
                /* Let's form (Y coords only):
                 *
                 * c[v0].vertex     c[v1].vertex
                 * m0               m1
                 * c[v0^1].vertex   c[v1^1].vertex
                 */
                m0 = midpoint(cc->corners[0].y, cc->corners[1].y);
                if (m0 >= pfs->rect.p.y)
                    goto check_rot_above_quarter;
                m1 = midpoint(cc->corners[3].y, cc->corners[2].y);
                if (m1 >= pfs->rect.p.y)
                    goto check_rot_above_quarter;
                /* So, we can completely discard the top half of the patch. */
                cc->corners[v0].x = midpoint(cc->corners[0].x, cc->corners[1].x);
                cc->corners[v0].y = m0;
                cc->corners[v1].x = midpoint(cc->corners[3].x, cc->corners[2].x);
                cc->corners[v1].y = m1;
                cc->curve[v0].vertex.cc[0] = (cc->curve[0].vertex.cc[0] + cc->curve[1].vertex.cc[0])/2;
                cc->curve[v1].vertex.cc[0] = (cc->curve[3].vertex.cc[0] + cc->curve[2].vertex.cc[0])/2;
                changed = 1;
            }
            while (cc->corners[v0].y < pfs->rect.p.y && cc->corners[v1].y < pfs->rect.p.y);
            if (0)
            {
check_rot_above_quarter:
                /* At this point we know that the condition for the following loop is true, so it
                 * can be a do...while rather than a while. */
                do
                {
                    /* Let's form (Y coords only):
                     *
                     * c[v0].vertex     c[v1].vertex
                     * m0               m1
                     * x                x
                     * x                x
                     * c[v0^1].vertex   c[v1^1].vertex
                     */
                    m0 = quarterpoint(cc->corners[v0].y, cc->corners[v0^1].y);
                    if (m0 >= pfs->rect.p.y)
                        break;
                    m1 = quarterpoint(cc->corners[v1].y, cc->corners[v1^1].y);
                    if (m1 >= pfs->rect.p.y)
                        break;
                    /* So, we can completely discard the top half of the patch. */
                    cc->corners[v0].x = quarterpoint(cc->corners[v0].x, cc->corners[v0^1].x);
                    cc->corners[v0].y = m0;
                    cc->corners[v1].x = quarterpoint(cc->corners[v1].x, cc->corners[v1^1].x);
                    cc->corners[v1].y = m1;
                    cc->curve[v0].vertex.cc[0] = (cc->curve[v0].vertex.cc[0] + 3*cc->curve[v0^1].vertex.cc[0])/4;
                    cc->curve[v1].vertex.cc[0] = (cc->curve[v1].vertex.cc[0] + 3*cc->curve[v1^1].vertex.cc[0])/4;
                    changed = 1;
                }
                while (cc->corners[v0].y < pfs->rect.p.y && cc->corners[v1].y < pfs->rect.p.y);
            }
        }

        /* or the bottom half? */
        if (cc->corners[0].y > pfs->rect.q.y && cc->corners[3].y > pfs->rect.q.y)
        {
            /* Check 0+3 off the bottom. */
            v0 = 0;
            v1 = 3;
            goto check_rot_bottom;
        }
        else if (cc->corners[1].y > pfs->rect.q.y && cc->corners[2].y > pfs->rect.q.y)
        {
            /* Check 1+2 off the bottom. */
            v0 = 1;
            v1 = 2;
check_rot_bottom:
            /* At this point we know that the condition for the following loop is true, so it
             * can be a do...while rather than a while. */
            do
            {
                /* Let's form (Y coords only):
                 *
                 * c[v0].vertex     c[v1].vertex
                 * m0               m1
                 * c[v0^1].vertex   c[v1^1].vertex
                 */
                m0 = midpoint(cc->corners[0].y, cc->corners[1].y);
                if (m0 <= pfs->rect.q.y+MIDPOINT_ACCURACY)
                    goto check_rot_bottom_quarter;
                m1 = midpoint(cc->corners[3].y, cc->corners[2].y);
                if (m1 <= pfs->rect.q.y+MIDPOINT_ACCURACY)
                    goto check_rot_bottom_quarter;
                /* So, we can completely discard the left hand half of the patch. */
                cc->corners[v0].x = midpoint(cc->corners[0].x, cc->corners[1].x);
                cc->corners[v0].y = m0;
                cc->corners[v1].x = midpoint(cc->corners[3].x, cc->corners[2].x);
                cc->corners[v1].y = m1;
                cc->curve[v0].vertex.cc[0] = (cc->curve[0].vertex.cc[0] + cc->curve[1].vertex.cc[0])/2;
                cc->curve[v1].vertex.cc[0] = (cc->curve[3].vertex.cc[0] + cc->curve[2].vertex.cc[0])/2;
                changed = 1;
            }
            while (cc->corners[v0].y > pfs->rect.q.y+MIDPOINT_ACCURACY && cc->corners[v1].y > pfs->rect.q.y+MIDPOINT_ACCURACY);
            if (0)
            {
check_rot_bottom_quarter:
                /* At this point we know that the condition for the following loop is true, so it
                 * can be a do...while rather than a while. */
                do
                {
                    /* Let's form (Y coords only):
                     *
                     * c[v0].vertex     c[v1].vertex
                     * x                x
                     * x                x
                     * m0               m1
                     * c[v0^1].vertex   c[v1^1].vertex
                     */
                    m0 = quarterpoint(cc->corners[v0].y, cc->corners[v0^1].y);
                    if (m0 <= pfs->rect.q.y+QUARTERPOINT_ACCURACY)
                        break;
                    m1 = quarterpoint(cc->corners[v1].y, cc->corners[v1^1].y);
                    if (m1 <= pfs->rect.q.y+QUARTERPOINT_ACCURACY)
                        break;
                    /* So, we can completely discard the left hand half of the patch. */
                    cc->corners[v0].x = quarterpoint(cc->corners[v0].x, cc->corners[v0^1].x);
                    cc->corners[v0].y = m0;
                    cc->corners[v1].x = quarterpoint(cc->corners[v1].x, cc->corners[v1^1].x);
                    cc->corners[v1].y = m1;
                    cc->curve[v0].vertex.cc[0] = (cc->curve[v0].vertex.cc[0] + 3*cc->curve[v0^1].vertex.cc[0])/4;
                    cc->curve[v1].vertex.cc[0] = (cc->curve[v1].vertex.cc[0] + 3*cc->curve[v1^1].vertex.cc[0])/4;
                    changed = 1;
                }
                while (cc->corners[v0].y > pfs->rect.q.y+QUARTERPOINT_ACCURACY && cc->corners[v1].y > pfs->rect.q.y+QUARTERPOINT_ACCURACY);
            }
        }
    } while (changed);

    return A_fill_region_floats(pfs, cc, 0);
}
#undef midpoint
#undef quarterpoint
#undef MIDPOINT_ACCURACY
#undef QUARTERPOINT_ACCURACY

static int
A_fill_region(A_fill_state_t * pfs, patch_fill_state_t *pfs1)
{
    const gs_shading_A_t * const psh = pfs->psh;
    double x0 = psh->params.Coords[0] + pfs->delta.x * pfs->v0;
    double y0 = psh->params.Coords[1] + pfs->delta.y * pfs->v0;
    double x1 = psh->params.Coords[0] + pfs->delta.x * pfs->v1;
    double y1 = psh->params.Coords[1] + pfs->delta.y * pfs->v1;
    double h0 = pfs->u0, h1 = pfs->u1;
    corners_and_curves cc;
    int code;

    double dx0 = pfs->delta.x * h0;
    double dy0 = pfs->delta.y * h0;
    double dx1 = pfs->delta.x * h1;
    double dy1 = pfs->delta.y * h1;

    cc.curve[0].vertex.cc[0] = pfs->t0; /* The element cc[1] is set to a dummy value against */
    cc.curve[1].vertex.cc[0] = pfs->t1; /* interrupts while an idle processing in gxshade6.c .  */
    cc.curve[2].vertex.cc[0] = pfs->t1;
    cc.curve[3].vertex.cc[0] = pfs->t0;
    cc.corners[0].x = x0 + dy0;
    cc.corners[0].y = y0 - dx0;
    cc.corners[1].x = x1 + dy0;
    cc.corners[1].y = y1 - dx0;
    cc.corners[2].x = x1 + dy1;
    cc.corners[2].y = y1 - dx1;
    cc.corners[3].x = x0 + dy1;
    cc.corners[3].y = y0 - dx1;
    code = gs_point_transform2fixed(&pfs1->pgs->ctm, cc.corners[0].x, cc.corners[0].y, &cc.curve[0].vertex.p);
    if (code < 0)
        goto fail;
    code = gs_point_transform2fixed(&pfs1->pgs->ctm, cc.corners[1].x, cc.corners[1].y, &cc.curve[1].vertex.p);
    if (code < 0)
        goto fail;
    code = gs_point_transform2fixed(&pfs1->pgs->ctm, cc.corners[2].x, cc.corners[2].y, &cc.curve[2].vertex.p);
    if (code < 0)
        goto fail;
    code = gs_point_transform2fixed(&pfs1->pgs->ctm, cc.corners[3].x, cc.corners[3].y, &cc.curve[3].vertex.p);
    if (code < 0)
        goto fail;
    return subdivide_patch_fill(pfs1, cc.curve);
fail:
    if (code != gs_error_limitcheck)
        return code;
    code = gs_point_transform(cc.corners[0].x, cc.corners[0].y, (const gs_matrix *)&pfs1->pgs->ctm, &cc.corners[0]);
    if (code < 0)
        return code;
    code = gs_point_transform(cc.corners[1].x, cc.corners[1].y, (const gs_matrix *)&pfs1->pgs->ctm, &cc.corners[1]);
    if (code < 0)
        return code;
    code = gs_point_transform(cc.corners[2].x, cc.corners[2].y, (const gs_matrix *)&pfs1->pgs->ctm, &cc.corners[2]);
    if (code < 0)
        return code;
    code = gs_point_transform(cc.corners[3].x, cc.corners[3].y, (const gs_matrix *)&pfs1->pgs->ctm, &cc.corners[3]);
    if (code < 0)
        return code;
    return subdivide_patch_fill_floats(pfs1, &cc);
}

static inline int
gs_shading_A_fill_rectangle_aux(const gs_shading_t * psh0, const gs_rect * rect,
                            const gs_fixed_rect *clip_rect,
                            gx_device * dev, gs_gstate * pgs)
{
    const gs_shading_A_t *const psh = (const gs_shading_A_t *)psh0;
    gs_function_t * const pfn = psh->params.Function;
    gs_matrix cmat;
    gs_rect t_rect;
    A_fill_state_t state;
    patch_fill_state_t pfs1;
    float d0 = psh->params.Domain[0], d1 = psh->params.Domain[1];
    float dd = d1 - d0;
    double t0, t1;
    gs_point dist;
    int code;

    state.psh = psh;
    code = shade_init_fill_state((shading_fill_state_t *)&pfs1, psh0, dev, pgs);
    if (code < 0)
        return code;
    pfs1.Function = pfn;
    pfs1.rect = *clip_rect;
    code = init_patch_fill_state(&pfs1);
    if (code < 0)
        goto fail;
    pfs1.maybe_self_intersecting = false;
    pfs1.function_arg_shift = 1;
    /*
     * Compute the parameter range.  We construct a matrix in which
     * (0,0) corresponds to t = 0 and (0,1) corresponds to t = 1,
     * and use it to inverse-map the rectangle to be filled.
     */
    cmat.tx = psh->params.Coords[0];
    cmat.ty = psh->params.Coords[1];
    state.delta.x = psh->params.Coords[2] - psh->params.Coords[0];
    state.delta.y = psh->params.Coords[3] - psh->params.Coords[1];
    cmat.yx = state.delta.x;
    cmat.yy = state.delta.y;
    cmat.xx = cmat.yy;
    cmat.xy = -cmat.yx;
    code = gs_bbox_transform_inverse(rect, &cmat, &t_rect);
    if (code < 0) {
        code = 0; /* Swallow this silently */
        goto fail;
    }
    t0 = min(max(t_rect.p.y, 0), 1);
    t1 = max(min(t_rect.q.y, 1), 0);
    state.v0 = t0;
    state.v1 = t1;
    state.u0 = t_rect.p.x;
    state.u1 = t_rect.q.x;
    state.t0 = t0 * dd + d0;
    state.t1 = t1 * dd + d0;
    code = gs_distance_transform(state.delta.x, state.delta.y, &ctm_only(pgs),
                          &dist);
    if (code < 0)
        goto fail;
    state.length = hypot(dist.x, dist.y);	/* device space line length */
    code = A_fill_region(&state, &pfs1);
    if (psh->params.Extend[0] && t0 > t_rect.p.y) {
        if (code < 0)
            goto fail;
        /* Use the general algorithm, because we need the trapping. */
        state.v0 = t_rect.p.y;
        state.v1 = t0;
        state.t0 = state.t1 = t0 * dd + d0;
        code = A_fill_region(&state, &pfs1);
    }
    if (psh->params.Extend[1] && t1 < t_rect.q.y) {
        if (code < 0)
            goto fail;
        /* Use the general algorithm, because we need the trapping. */
        state.v0 = t1;
        state.v1 = t_rect.q.y;
        state.t0 = state.t1 = t1 * dd + d0;
        code = A_fill_region(&state, &pfs1);
    }
fail:
    gsicc_release_link(pfs1.icclink);
    if (term_patch_fill_state(&pfs1))
        return_error(gs_error_unregistered); /* Must not happen. */
    return code;
}

int
gs_shading_A_fill_rectangle(const gs_shading_t * psh0, const gs_rect * rect,
                            const gs_fixed_rect * rect_clip,
                            gx_device * dev, gs_gstate * pgs)
{
    return gs_shading_A_fill_rectangle_aux(psh0, rect, rect_clip, dev, pgs);
}

/* ---------------- Radial shading ---------------- */

/* Some notes on what I have struggled to understand about the following
 * function. This function renders the 'tube' given by interpolating one
 * circle to another.
 *
 * The first circle is at (x0, y0) with radius r0, and has 'color' t0.
 * The other circle is at (x1, y1) with radius r1, and has 'color' t1.
 *
 * We perform this rendering by approximating each quadrant of the 'tube'
 * by a tensor patch. The tensor patch is formed by taking a curve along
 * 1/4 of the circumference of the first circle, a straight line to the
 * equivalent point on the circumference of the second circle, a curve
 * back along the circumference of the second circle, and then a straight
 * line back to where we started.
 *
 * There is additional logic in this function that forms the directions of
 * the curves differently for different quadrants. This is done to ensure
 * that we always paint 'around' the tube from the back towards the front,
 * so we don't get unexpected regions showing though. This is explained more
 * below.
 *
 * The original code here examined the position change between the two
 * circles dx and dy. Based upon this vector it would pick which quadrant/
 * tensor patch to draw first. It would draw the quadrants/tensor patches
 * in anticlockwise order. Presumably this was intended to be done so that
 * the 'top' quadrant would be drawn last.
 *
 * Unfortunately this did not always work; see bug 692513. If the quadrants
 * were rendered in the order 0,1,2,3, the rendering of 1 was leaving traces
 * on top of 0, which was unexpected.
 *
 * I have therefore altered the code slightly; rather than picking a start
 * quadrant and moving anticlockwise, we now draw the 'undermost' quadrant,
 * then the two adjacent quadrants, then the topmost quadrant.
 *
 * For the purposes of explanation, we shall label the octants as below:
 *
 *     \2|1/       and Quadrants as:       |
 *     3\|/0                            Q1 | Q0
 *    ---+---                          ----+----
 *     4/|\7                            Q2 | Q3
 *     /5|6\                               |
 *
 * We find (dx,dy), the difference between the centres of the circles.
 * We look to see which octant this falls in. Firstly, this tells us which
 * quadrant of the circle we need to draw first (Octant n, starts with
 * Quadrant floor(n/2)). Secondly, it tells us which direction to form the
 * tensor patch in; we always want to draw from the side 'closest' to
 * dx/dy to the side further away. This ensures that we don't overwrite
 * pixels in the incorrect order as the patch decomposes.
 */
static int
R_tensor_annulus(patch_fill_state_t *pfs,
    double x0, double y0, double r0, double t0,
    double x1, double y1, double r1, double t1)
{
    double dx = x1 - x0, dy = y1 - y0;
    double d = hypot(dx, dy);
    gs_point p0, p1, pc0, pc1;
    int k, j, code, dirn;
    bool inside = 0;

    /* pc0 and pc1 are the centres of the respective circles. */
    pc0.x = x0, pc0.y = y0;
    pc1.x = x1, pc1.y = y1;
    /* Set p0 up so it's a unit vector giving the direction of 90 degrees
     * to the right of the major axis as we move from p0c to p1c. */
    if (r0 + d <= r1 || r1 + d <= r0) {
        /* One circle is inside another one.
           Use any subdivision,
           but don't depend on dx, dy, which may be too small. */
        p0.x = 0, p0.y = -1, dirn = 0;
        /* Align stripes along radii for faster triangulation : */
        inside = 1;
        pfs->function_arg_shift = 1;
    } else {
        /* Must generate canonic quadrangle arcs,
           because we approximate them with curves. */
        if(dx >= 0) {
            if (dy >= 0)
                p0.x = 1, p0.y = 0, dirn = (dx >= dy ? 1 : 0);
            else
                p0.x = 0, p0.y = -1, dirn = (dx >= -dy ? 0 : 1);
        } else {
            if (dy >= 0)
                p0.x = 0, p0.y = 1, dirn = (-dx >= dy ? 1 : 0);
            else
                p0.x = -1, p0.y = 0, dirn = (-dx >= -dy ? 0 : 1);
        }
        pfs->function_arg_shift = 0;
    }
    /* fixme: wish: cut invisible parts off.
       Note : when r0 != r1 the invisible part is not a half circle. */
    for (k = 0; k < 4; k++) {
        gs_point p[12];
        patch_curve_t curve[4];

        /* Set p1 to be 90 degrees anticlockwise from p0 */
        p1.x = -p0.y; p1.y = p0.x;
        if (dirn == 0) { /* Clockwise */
            make_quadrant_arc(p + 0, &pc0, &p1, &p0, r0);
            make_quadrant_arc(p + 6, &pc1, &p0, &p1, r1);
        } else { /* Anticlockwise */
            make_quadrant_arc(p + 0, &pc0, &p0, &p1, r0);
            make_quadrant_arc(p + 6, &pc1, &p1, &p0, r1);
        }
        p[4].x = (p[3].x * 2 + p[6].x) / 3;
        p[4].y = (p[3].y * 2 + p[6].y) / 3;
        p[5].x = (p[3].x + p[6].x * 2) / 3;
        p[5].y = (p[3].y + p[6].y * 2) / 3;
        p[10].x = (p[9].x * 2 + p[0].x) / 3;
        p[10].y = (p[9].y * 2 + p[0].y) / 3;
        p[11].x = (p[9].x + p[0].x * 2) / 3;
        p[11].y = (p[9].y + p[0].y * 2) / 3;
        for (j = 0; j < 4; j++) {
            int jj = (j + inside) % 4;

            if (gs_point_transform2fixed(&pfs->pgs->ctm,         p[j*3 + 0].x, p[j*3 + 0].y, &curve[jj].vertex.p) < 0)
                gs_point_transform2fixed_clamped(&pfs->pgs->ctm, p[j*3 + 0].x, p[j*3 + 0].y, &curve[jj].vertex.p);

            if (gs_point_transform2fixed(&pfs->pgs->ctm,         p[j*3 + 1].x, p[j*3 + 1].y, &curve[jj].control[0]) < 0)
                gs_point_transform2fixed_clamped(&pfs->pgs->ctm, p[j*3 + 1].x, p[j*3 + 1].y, &curve[jj].control[0]);

            if (gs_point_transform2fixed(&pfs->pgs->ctm,         p[j*3 + 2].x, p[j*3 + 2].y, &curve[jj].control[1]) < 0)
                gs_point_transform2fixed_clamped(&pfs->pgs->ctm, p[j*3 + 2].x, p[j*3 + 2].y, &curve[jj].control[1]);
            curve[j].straight = (((j + inside) & 1) != 0);
        }
        curve[(0 + inside) % 4].vertex.cc[0] = t0;
        curve[(1 + inside) % 4].vertex.cc[0] = t0;
        curve[(2 + inside) % 4].vertex.cc[0] = t1;
        curve[(3 + inside) % 4].vertex.cc[0] = t1;
        curve[0].vertex.cc[1] = curve[1].vertex.cc[1] = 0; /* Initialize against FPE. */
        curve[2].vertex.cc[1] = curve[3].vertex.cc[1] = 0; /* Initialize against FPE. */
        code = patch_fill(pfs, curve, NULL, NULL);
        if (code < 0)
            return code;
        /* Move p0 to be ready for the next position */
        if (k == 0) {
            /* p0 moves clockwise */
            p1 = p0;
            p0.x = p1.y; p0.y = -p1.x;
            dirn = 0;
        } else if (k == 1) {
            /* p0 flips sides */
            p0.x = -p0.x; p0.y = -p0.y;
            dirn = 1;
        } else if (k == 2) {
            /* p0 moves anti-clockwise */
            p1 = p0;
            p0.x = -p1.y; p0.y = p1.x;
            dirn = 0;
        }
    }
    return 0;
}

/* Find the control points for two points on the arc of a circle
 * the points must be within the same quadrant.
 */
static int find_arc_control_points(gs_point *from, gs_point *to, gs_point *from_control, gs_point *to_control, gs_point *centre)
{
    double from_tan_alpha, to_tan_alpha, from_alpha, to_alpha;
    double half_inscribed_angle, intersect_x, intersect_y, intersect_dist;
    double radius = sqrt(((from->x - centre->x) * (from->x - centre->x)) + ((from->y - centre->y) * (from->y - centre->y)));
    double tangent_intersect_dist;
    double F;
    int quadrant;

    /* Quadrant 0 is upper right, numbered anti-clockwise.
     * If the direction of the from->to is atni-clockwise, add 4
     */
    if (from->x > to->x) {
        if (from->y > to->y) {
            if (to->y >= centre->y)
                quadrant = 1 + 4;
            else
                quadrant = 3;
        } else {
            if (to->x >= centre->x)
                quadrant = 0 + 4;
            else
                quadrant = 2;
        }
    } else {
        if (from->y > to->y) {
            if (from->x >= centre->x)
                quadrant = 0;
            else
                quadrant = 2 + 4;
        } else {
            if (from->x >= centre->x)
                quadrant = 3 + 4;
            else
                quadrant = 1;
        }
    }

    switch(quadrant) {
        /* quadrant 0, arc goes clockwise */
        case 0:
            if (from->x == centre->x) {
                from_alpha = M_PI / 2;
            } else {
                from_tan_alpha = (from->y - centre->y) / (from->x - centre->x);
                from_alpha = atan(from_tan_alpha);
            }
            to_tan_alpha = (to->y - centre->y) / (to->x - centre->x);
            to_alpha = atan(to_tan_alpha);

            half_inscribed_angle = (from_alpha - to_alpha) / 2;
            intersect_dist = radius / cos(half_inscribed_angle);
            tangent_intersect_dist = tan(half_inscribed_angle) * radius;

            intersect_x = centre->x + cos(to_alpha + half_inscribed_angle) * intersect_dist;
            intersect_y = centre->y + sin(to_alpha + half_inscribed_angle) * intersect_dist;
            break;
        /* quadrant 1, arc goes clockwise */
        case 1:
            from_tan_alpha = (from->y - centre->y) / (centre->x - from->x);
            from_alpha = atan(from_tan_alpha);

            if (to->x == centre->x) {
                to_alpha = M_PI / 2;
            } else {
                to_tan_alpha = (to->y - centre->y) / (centre->x - to->x);
                to_alpha = atan(to_tan_alpha);
            }

            half_inscribed_angle = (to_alpha - from_alpha) / 2;
            intersect_dist = radius / cos(half_inscribed_angle);
            tangent_intersect_dist = tan(half_inscribed_angle) * radius;

            intersect_x = centre->x - cos(from_alpha + half_inscribed_angle) * intersect_dist;
            intersect_y = centre->y + sin(from_alpha + half_inscribed_angle) * intersect_dist;
            break;
        /* quadrant 2, arc goes clockwise */
        case 2:
            if (from->x == centre->x) {
                from_alpha = M_PI / 2;
            } else {
                from_tan_alpha = (centre->y - from->y) / (centre->x - from->x);
                from_alpha = atan(from_tan_alpha);
            }

            to_tan_alpha = (centre->y - to->y) / (centre->x - to->x);
            to_alpha = atan(to_tan_alpha);

            half_inscribed_angle = (to_alpha - from_alpha) / 2;
            intersect_dist = radius / cos(half_inscribed_angle);
            tangent_intersect_dist = tan(half_inscribed_angle) * radius;

            intersect_x = centre->x - cos(from_alpha + half_inscribed_angle) * intersect_dist;
            intersect_y = centre->y - sin(from_alpha + half_inscribed_angle) * intersect_dist;
            break;
        /* quadrant 3, arc goes clockwise */
        case 3:
            from_tan_alpha = (centre->y - from->y) / (from->x - centre->x);
            from_alpha = atan(from_tan_alpha);

            if (to->x == centre->x) {
                to_alpha = M_PI / 2;
            } else {
                to_tan_alpha = (centre->y - to->y) / (to->x - centre->x);
                to_alpha = atan(to_tan_alpha);
            }

            half_inscribed_angle = (to_alpha - from_alpha) / 2;
            intersect_dist = radius / cos(half_inscribed_angle);
            tangent_intersect_dist = tan(half_inscribed_angle) * radius;

            intersect_x = centre->x + cos(from_alpha + half_inscribed_angle) * intersect_dist;
            intersect_y = centre->y - sin(from_alpha + half_inscribed_angle) * intersect_dist;
            break;
        /* quadrant 0, arc goes anti-clockwise */
        case 4:
            from_tan_alpha = (from->y - centre->y) / (from->x - centre->x);
            from_alpha = atan(from_tan_alpha);

            if (to->y == centre->y)
                to_alpha = M_PI / 2;
            else {
                to_tan_alpha = (to->y - centre->y) / (to->x - centre->x);
                to_alpha = atan(to_tan_alpha);
            }

            half_inscribed_angle = (to_alpha - from_alpha) / 2;
            intersect_dist = radius / cos(half_inscribed_angle);
            tangent_intersect_dist = tan(half_inscribed_angle) * radius;

            intersect_x = centre->x + cos(from_alpha + half_inscribed_angle) * intersect_dist;
            intersect_y = centre->y + sin(from_alpha + half_inscribed_angle) * intersect_dist;
            break;
        /* quadrant 1, arc goes anti-clockwise */
        case 5:
            from_tan_alpha = (centre->x - from->x) / (from->y - centre->y);
            from_alpha = atan(from_tan_alpha);

            if (to->y == centre->y) {
                to_alpha = M_PI / 2;
            }
            else {
                to_tan_alpha = (centre->x - to->x) / (to->y - centre->y);
                to_alpha = atan(to_tan_alpha);
            }

            half_inscribed_angle = (to_alpha - from_alpha) / 2;
            intersect_dist = radius / cos(half_inscribed_angle);
            tangent_intersect_dist = tan(half_inscribed_angle) * radius;

            intersect_x = centre->x - sin(from_alpha + half_inscribed_angle) * intersect_dist;
            intersect_y = centre->y + cos(from_alpha + half_inscribed_angle) * intersect_dist;
            break;
        /* quadrant 2, arc goes anti-clockwise */
        case 6:
            from_tan_alpha = (from->y - centre->y) / (centre->x - from->x);
            from_alpha = atan(from_tan_alpha);

            if (to->x == centre->x) {
                to_alpha = M_PI / 2;
            } else {
                to_tan_alpha = (centre->y - to->y) / (centre->x - to->x);
                to_alpha = atan(to_tan_alpha);
            }

            half_inscribed_angle = (to_alpha - from_alpha) / 2;
            intersect_dist = radius / cos(half_inscribed_angle);
            tangent_intersect_dist = tan(half_inscribed_angle) * radius;

            intersect_x = centre->x - cos(from_alpha + half_inscribed_angle) * intersect_dist;
            intersect_y = centre->y - sin(from_alpha + half_inscribed_angle) * intersect_dist;
            break;
        /* quadrant 3, arc goes anti-clockwise */
        case 7:
            if (from->x == centre->x) {
                from_alpha = M_PI / 2;
            } else {
                from_tan_alpha = (centre->y - from->y) / (from->x - centre->x);
                from_alpha = atan(from_tan_alpha);
            }
            to_tan_alpha = (centre->y - to->y) / (to->x - centre->x);
            to_alpha = atan(to_tan_alpha);

            half_inscribed_angle = (from_alpha - to_alpha) / 2;
            intersect_dist = radius / cos(half_inscribed_angle);
            tangent_intersect_dist = tan(half_inscribed_angle) * radius;

            intersect_x = centre->x + cos(to_alpha + half_inscribed_angle) * intersect_dist;
            intersect_y = centre->y - sin(to_alpha + half_inscribed_angle) * intersect_dist;
            break;
    }

    F = (4.0 / 3.0) / (1 + sqrt(1 + ((tangent_intersect_dist / radius) * (tangent_intersect_dist / radius))));

    from_control->x = from->x - ((from->x - intersect_x) * F);
    from_control->y = from->y - ((from->y - intersect_y) * F);
    to_control->x = to->x - ((to->x - intersect_x) * F);
    to_control->y = to->y - ((to->y - intersect_y) * F);

    return 0;
}

/* Create a 'patch_curve' element whch is a straight line between two points */
static int patch_lineto(gs_matrix_fixed *ctm, gs_point *from, gs_point *to, patch_curve_t *p, float t)
{
    double x_1third, x_2third, y_1third, y_2third;

    x_1third = (to->x - from->x) / 3;
    x_2third = x_1third * 2;
    y_1third = (to->y - from->y) / 3;
    y_2third = y_1third * 2;

    gs_point_transform2fixed(ctm, from->x, from->y, &p->vertex.p);
    gs_point_transform2fixed(ctm, from->x + x_1third, from->y + y_1third, &p->control[0]);
    gs_point_transform2fixed(ctm, from->x + x_2third, from->y + y_2third, &p->control[1]);

    p->vertex.cc[0] = t;
    p->vertex.cc[1] = t;
    p->straight = 1;

    return 0;
}

static int patch_curveto(gs_matrix_fixed *ctm, gs_point *centre, gs_point *from, gs_point *to, patch_curve_t *p, float t)
{
    gs_point from_control, to_control;

    find_arc_control_points(from, to, &from_control, &to_control, centre);

    gs_point_transform2fixed(ctm, from->x, from->y, &p->vertex.p);
    gs_point_transform2fixed(ctm, from_control.x, from_control.y, &p->control[0]);
    gs_point_transform2fixed(ctm, to_control.x, to_control.y, &p->control[1]);
    p->vertex.cc[0] = t;
    p->vertex.cc[1] = t;
    p->straight = 0;

    return 0;
}

static int draw_quarter_annulus(patch_fill_state_t *pfs, gs_point *centre, double radius, gs_point *corner, float t)
{
    gs_point p0, p1, initial;
    patch_curve_t p[4];
    int code;

    if (corner->x > centre->x) {
        initial.x = centre->x + radius;
    }
    else {
        initial.x = centre->x - radius;
    }
    initial.y = centre->y;

    p1.x = initial.x;
    p1.y = corner->y;
    patch_lineto(&pfs->pgs->ctm, &initial, &p1, &p[0], t);
    p0.x = centre->x;
    p0.y = p1.y;
    patch_lineto(&pfs->pgs->ctm, &p1, &p0, &p[1], t);
    p1.x = centre->x;
    if (centre->y > corner->y) {
        p1.y = centre->y - radius;
    } else {
        p1.y = centre->y + radius;
    }
    patch_lineto(&pfs->pgs->ctm, &p0, &p1, &p[2], t);
    patch_curveto(&pfs->pgs->ctm, centre, &p1, &initial, &p[3], t);
    code = patch_fill(pfs, (const patch_curve_t *)&p, NULL, NULL);
    if (code < 0)
        return code;

    if (corner->x > centre->x)
        initial.x = corner->x - (corner->x - (centre->x + radius));
    else
        initial.x = centre->x - radius;
    initial.y = corner->y;
    patch_lineto(&pfs->pgs->ctm, corner, &initial, &p[0], t);

    p0.x = initial.x;
    p0.y = centre->y;
    patch_lineto(&pfs->pgs->ctm, &initial, &p0, &p[1], t);

    p1.y = p0.y;
    p1.x = corner->x;
    patch_lineto(&pfs->pgs->ctm, &p0, &p1, &p[2], t);
    patch_lineto(&pfs->pgs->ctm, &p1, corner, &p[3], t);

    return (patch_fill(pfs, (const patch_curve_t *)&p, NULL, NULL));
}

static int R_tensor_annulus_extend_tangent(patch_fill_state_t *pfs,
    double x0, double y0, double r0, double t0,
    double x1, double y1, double r1, double t1, double r2)
{
    patch_curve_t curve[4];
    gs_point p0, p1;
    int code = 0, q = 0;

    /* special case axis aligned circles. Its quicker to handle these specially as it
     * avoid lots of trigonometry in the general case code, and avoids us
     * having to watch out for infinity as the result of tan() operations.
     */
    if (x0 == x1 || y0 == y1) {
        if (x0 == x1 && y0 > y1) {
            /* tangent at top of circles */
            p0.x = x1, p0.y = y1;
            p1.x = x1 + r2, p1.y = y1 - r2;
            draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
            p1.x = x1 - r2, p1.y = y1 - r2;
            draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
            p1.x = x1 + r2, p1.y = y1 + r1;
            draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
            p1.x = x1 - r2, p1.y = y1 + r1;
            draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
        }
        if (x0 == x1 && y0 < y1) {
            /* tangent at bottom of circles */
            p0.x = x1, p0.y = y1;
            p1.x = x1 + r2, p1.y = y1 + r2;
            draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
            p1.x = x1 - r2, p1.y = y1 + r2;
            draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
            p1.x = x1 + r2, p1.y = y1 - r1;
            draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
            p1.x = x1 - r2, p1.y = y1 - r1;
            draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
        }
        if (y0 == y1 && x0 > x1) {
            /* tangent at right of circles */
            p0.x = x1, p0.y = y1;
            p1.x = x1 - r2, p1.y = y1 - r2;
            draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
            p1.x = x1 - r2, p1.y = y1 + r2;
            draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
            p1.x = x1 + r1, p1.y = y1 + r2;
            draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
            p1.x = x1 + r1, p1.y = y1 - r2;
            draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
        }
        if (y0 == y1 && x0 < x1) {
            /* tangent at left of circles */
            p0.x = x1, p0.y = y1;
            p1.x = x1 + r2, p1.y = y1 - r2;
            draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
            p1.x = x1 + r2, p1.y = y1 + r2;
            draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
            p1.x = x1 - r1, p1.y = y1 + r2;
            draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
            p1.x = x1 - r1, p1.y = y1 - r2;
            draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
        }
    }
    else {
        double tx, ty, endx, endy, intersectx, intersecty, alpha, sinalpha, cosalpha, tanalpha;
        gs_point centre;

        /* First lets figure out which quadrant the smaller circle is in (we always
         * get called to fill from the larger circle), x0, y0, r0 is the smaller circle.
         */
        if (x0 < x1) {
            if (y0 < y1)
                q = 2;
            else
                q = 1;
        } else {
            if (y0 < y1)
                q = 3;
            else
                q = 0;
        }
        switch(q) {
            case 0:
                /* We have two four-sided elements, from the tangent point
                 * each side, to the point where the tangent crosses an
                 * axis of the larger circle. A line back to the edge
                 * of the larger circle, a line to the point where an axis
                 * crosses the smaller circle, then an arc back to the starting point.
                 */
                /* Figure out the tangent point */
                /* sin (angle) = y1 - y0 / r1 - r0
                 * ty = ((y1 - y0) / (r1 - r0)) * r1
                 */
                ty = y1 + ((y0 - y1) / (r1 - r0)) * r1;
                tx = x1 + ((x0 - x1) / (r1 - r0)) * r1;
                /* Now actually calculating the point where the tangent crosses the axis of the larger circle
                 * So we need to know the angle the tangent makes with the axis of the smaller circle
                 * as its the same angle where it crosses the axis of the larger circle.
                 * We know the centres and the tangent are co-linear, so sin(a) = y0 - y1 / r1 - r0
                 * We know the tangent is r1 from the centre of the larger circle, so the hypotenuse
                 * is r0 / cos(a). That gives us 'x' and we already know y as its the centre of the larger
                 * circle
                 */
                sinalpha = (y0 - y1) / (r1 - r0);
                alpha = asin(sinalpha);
                cosalpha = cos(alpha);
                intersectx = x1 + (r1 / cosalpha);
                intersecty = y1;

                p0.x = tx, p0.y = ty;
                p1.x = tx + (intersectx - tx) / 2, p1.y = ty - (ty - intersecty) / 2;
                patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[0], t0);
                p0.x = intersectx, p0.y = intersecty;
                patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[1], t0);
                p1.x = x1 + r1, p1.y = y1;
                patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[2], t0);
                p0.x = tx, p0.y = ty;
                centre.x = x1, centre.y = y1;
                patch_curveto(&pfs->pgs->ctm, &centre, &p1, &p0, &curve[3], t0);
                code = patch_fill(pfs, curve, NULL, NULL);
                if (code < 0)
                    return code;

                if (intersectx < x1 + r2) {
                    /* didn't get all the way to the edge, quadrant 3 is composed of 2 quads :-(
                     * An 'annulus' where the right edge is less than the normal extent and a
                     * quad which is a rectangle with one corner chopped of at an angle.
                     */
                    p0.x = x1, p0.y = y1;
                    p1.x = intersectx, p1.y = y1 - r2;
                    draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
                    endx = x1 + r2;
                    endy = y1 - (tan ((M_PI / 2) - alpha)) * (endx - intersectx);
                    p0.x = intersectx, p0.y = y1;
                    p1.x = x1 + r2, p1.y = endy;
                    patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[0], t0);
                    p0.x = x1 + r2, p0.y = y0 - r2;
                    patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[1], t0);
                    p1.x = intersectx, p1.y = p0.y;
                    patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[2], t0);
                    p0.x = intersectx, p0.y = y1;
                    patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[3], t0);
                    code = patch_fill(pfs, curve, NULL, NULL);
                    if (code < 0)
                        return code;

                } else {
                    /* Quadrant 3 is a normal quarter annulua */
                    p0.x = x1, p0.y = y1;
                    p1.x = x1 + r2, p1.y = y1 - r2;
                    draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
                }

                /* Q2 is always a full annulus... */
                p0.x = x1, p0.y = y1;
                p1.x = x1 - r2, p1.y = y1 - r2;
                draw_quarter_annulus(pfs, &p0, r1, &p1, t0);

                /* alpha is now the angle between the x axis and the tangent to the
                 * circles.
                 */
                alpha = (M_PI / 2) - alpha;
                cosalpha = cos(alpha);
                endy = y1 + (r1 / cosalpha);
                endx = x1;

                p0.x = tx, p0.y = ty;
                p1.x = endx - ((endx - tx) / 2), p1.y = endy - ((endy - ty) / 2);
                patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[0], t0);
                p0.x = endx, p0.y = endy;
                patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[1], t0);
                p1.x = x1, p1.y = y1 + r1;
                patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[2], t0);
                p0.x = tx, p0.y = ty;
                centre.x = x1, centre.y = y1;
                patch_curveto(&pfs->pgs->ctm, &centre, &p1, &p0, &curve[3], t0);
                code = patch_fill(pfs, curve, NULL, NULL);
                if (code < 0)
                    return code;

                /* Q1 is simimlar to Q3, either a full quarter annulus
                 * or a partial one, depending on where the tangent crosses
                 * the y axis
                 */
                tanalpha = tan(alpha);
                intersecty = y1 + tanalpha * (r2 + (intersectx - x1));
                intersectx = x1 - r2;

                if (endy < y1 + r2) {
                    /* didn't get all the way to the edge, quadrant 1 is composed of 2 quads :-(
                     * An 'annulus' where the right edge is less than the normal extent and a
                     * quad which is a rectangle with one corner chopped of at an angle.
                     */
                    p0.x = x1, p0.y = y1;
                    p1.x = x1 - r2, p1.y = endy;
                    draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
                    p0.x = x1, p0.y = y1 + r1;
                    p1.x = x1, p1.y = endy;
                    patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[0], t0);
                    p0.x = x1 - r2, p0.y = intersecty;
                    patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[1], t0);
                    p1.x = p0.x, p1.y = y1 + r1;
                    patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[2], t0);
                    p0.x = x1, p0.y = y1 + r1;
                    patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[3], t0);
                    code = patch_fill(pfs, curve, NULL, NULL);
                    if (code < 0)
                        return code;
                } else {
                    /* Quadrant 1 is a normal quarter annulua */
                    p0.x = x1, p0.y = y1;
                    p1.x = x1 - r2, p1.y = y1 + r2;
                    draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
                }
                break;
            case 1:
                /* We have two four-sided elements, from the tangent point
                 * each side, to the point where the tangent crosses an
                 * axis of the larger circle. A line back to the edge
                 * of the larger circle, a line to the point where an axis
                 * crosses the smaller circle, then an arc back to the starting point.
                 */
                /* Figure out the tangent point */
                /* sin (angle) = y1 - y0 / r1 - r0
                 * ty = ((y1 - y0) / (r1 - r0)) * r1
                 */
                ty = y1 + ((y0 - y1) / (r1 - r0)) * r1;
                tx = x1 - ((x1 - x0) / (r1 - r0)) * r1;
                /* Now actually calculating the point where the tangent crosses the axis of the larger circle
                 * So we need to know the angle the tangent makes with the axis of the smaller circle
                 * as its the same angle where it crosses the axis of the larger circle.
                 * We know the centres and the tangent are co-linear, so sin(a) = y0 - y1 / r1 - r0
                 * We know the tangent is r1 from the centre of the larger circle, so the hypotenuse
                 * is r0 / cos(a). That gives us 'x' and we already know y as its the centre of the larger
                 * circle
                 */
                sinalpha = (y0 - y1) / (r1 - r0);
                alpha = asin(sinalpha);
                cosalpha = cos(alpha);
                intersectx = x1 - (r1 / cosalpha);
                intersecty = y1;

                p0.x = tx, p0.y = ty;
                p1.x = tx - (tx - intersectx) / 2, p1.y = ty - (ty - intersecty) / 2;
                patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[0], t0);
                p0.x = intersectx, p0.y = intersecty;
                patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[1], t0);
                p1.x = x1 - r1, p1.y = y1;
                patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[2], t0);
                p0.x = tx, p0.y = ty;
                centre.x = x1, centre.y = y1;
                patch_curveto(&pfs->pgs->ctm, &centre, &p1, &p0, &curve[3], t0);
                code = patch_fill(pfs, curve, NULL, NULL);
                if (code < 0)
                    return code;

                if (intersectx > x1 - r2) {
                    /* didn't get all the way to the edge, quadrant 2 is composed of 2 quads :-(
                     * An 'annulus' where the right edge is less than the normal extent and a
                     * quad which is a rectangle with one corner chopped of at an angle.
                     */
                    p0.x = x1, p0.y = y1;
                    p1.x = intersectx, p1.y = y1 - r2;
                    draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
                    endx = x1 - r2;
                    endy = y1 - (tan ((M_PI / 2) - alpha)) * (intersectx - endx);
                    p0.x = intersectx, p0.y = y1;
                    p1.x = x1 - r2, p1.y = endy;
                    patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[0], t0);
                    p0.x = x1 - r2, p0.y = y0 - r2;
                    patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[1], t0);
                    p1.x = intersectx, p1.y = p0.y;
                    patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[2], t0);
                    p0.x = intersectx, p0.y = y1;
                    patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[3], t0);
                    code = patch_fill(pfs, curve, NULL, NULL);
                    if (code < 0)
                        return code;

                } else {
                    /* Quadrant 2 is a normal quarter annulua */
                    p0.x = x1, p0.y = y1;
                    p1.x = x1 - r2, p1.y = y1 - r2;
                    draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
                }

                /* Q3 is always a full annulus... */
                p0.x = x1, p0.y = y1;
                p1.x = x1 + r2, p1.y = y1 - r2;
                draw_quarter_annulus(pfs, &p0, r1, &p1, t0);

                /* alpha is now the angle between the x axis and the tangent to the
                 * circles.
                 */
                alpha = (M_PI / 2) - alpha;
                cosalpha = cos(alpha);
                endy = y1 + (r1 / cosalpha);
                endx = x1;

                p0.x = tx, p0.y = ty;
                p1.x = endx + ((tx - endx) / 2), p1.y = endy - ((endy - ty) / 2);
                patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[0], t0);
                p0.x = endx, p0.y = endy;
                patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[1], t0);
                p1.x = x1, p1.y = y1 + r1;
                patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[2], t0);
                p0.x = tx, p0.y = ty;
                centre.x = x1, centre.y = y1;
                patch_curveto(&pfs->pgs->ctm, &centre, &p1, &p0, &curve[3], t0);
                code = patch_fill(pfs, curve, NULL, NULL);
                if (code < 0)
                    return code;

                /* Q0 is simimlar to Q2, either a full quarter annulus
                 * or a partial one, depending on where the tangent crosses
                 * the y axis
                 */
                tanalpha = tan(alpha);
                intersecty = y1 + tanalpha * (r2 + (x1 - intersectx));
                intersectx = x1 + r2;

                if (endy < y1 + r2) {
                    /* didn't get all the way to the edge, quadrant 0 is composed of 2 quads :-(
                     * An 'annulus' where the right edge is less than the normal extent and a
                     * quad which is a rectangle with one corner chopped of at an angle.
                     */
                    p0.x = x1, p0.y = y1;
                    p1.x = x1 + r2, p1.y = endy;
                    draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
                    p0.x = x1, p0.y = y1 + r1;
                    p1.x = x1, p1.y = endy;
                    patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[0], t0);
                    p0.x = x1 + r2, p0.y = intersecty;
                    patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[1], t0);
                    p1.x = p0.x, p1.y = y1 + r1;
                    patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[2], t0);
                    p0.x = x1, p0.y = y1 + r1;
                    patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[3], t0);
                    code = patch_fill(pfs, curve, NULL, NULL);
                    if (code < 0)
                        return code;
                } else {
                    /* Quadrant 0 is a normal quarter annulua */
                    p0.x = x1, p0.y = y1;
                    p1.x = x1 + r2, p1.y = y1 + r2;
                    draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
                }
                break;
            case 2:
                /* We have two four-sided elements, from the tangent point
                 * each side, to the point where the tangent crosses an
                 * axis of the larger circle. A line back to the edge
                 * of the larger circle, a line to the point where an axis
                 * crosses the smaller circle, then an arc back to the starting point.
                 */
                /* Figure out the tangent point */
                /* sin (angle) = y1 - y0 / r1 - r0
                 * ty = ((y1 - y0) / (r1 - r0)) * r1
                 */
                ty = y1 - ((y1 - y0) / (r1 - r0)) * r1;
                tx = x1 - ((x1 - x0) / (r1 - r0)) * r1;
                /* Now actually calculating the point where the tangent crosses the axis of the larger circle
                 * So we need to know the angle the tangent makes with the axis of the smaller circle
                 * as its the same angle where it crosses the axis of the larger circle.
                 * We know the centres and the tangent are co-linear, so sin(a) = y0 - y1 / r1 - r0
                 * We know the tangent is r1 from the centre of the larger circle, so the hypotenuse
                 * is r0 / cos(a). That gives us 'x' and we already know y as its the centre of the larger
                 * circle
                 */
                sinalpha = (y1 - y0) / (r1 - r0);
                alpha = asin(sinalpha);
                cosalpha = cos(alpha);
                intersectx = x1 - (r1 / cosalpha);
                intersecty = y1;

                p0.x = tx, p0.y = ty;
                p1.x = tx + (intersectx - tx) / 2, p1.y = ty - (ty - intersecty) / 2;
                patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[0], t0);
                p0.x = intersectx, p0.y = intersecty;
                patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[1], t0);
                p1.x = x1 - r1, p1.y = y1;
                patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[2], t0);
                p0.x = tx, p0.y = ty;
                centre.x = x1, centre.y = y1;
                patch_curveto(&pfs->pgs->ctm, &centre, &p1, &p0, &curve[3], t0);
                code = patch_fill(pfs, curve, NULL, NULL);
                if (code < 0)
                    return code;
                if (intersectx > x1 - r2) {
                    /* didn't get all the way to the edge, quadrant 1 is composed of 2 quads :-(
                     * An 'annulus' where the right edge is less than the normal extent and a
                     * quad which is a rectangle with one corner chopped of at an angle.
                     */
                    p0.x = x1, p0.y = y1;
                    p1.x = intersectx, p1.y = y1 + r2;
                    draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
                    endy = y1+r2;
                    endx = intersectx - ((endy - intersecty) / (tan ((M_PI / 2) - alpha)));
                    p0.x = intersectx, p0.y = y1;
                    p1.x = endx, p1.y = endy;
                    patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[0], t0);
                    p0.x = x1 - r1, p0.y = endy;
                    patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[1], t0);
                    p1.x = x1 - r1, p1.y = y1;
                    patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[2], t0);
                    p0.x = intersectx, p0.y = y1;
                    patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[3], t0);
                    code = patch_fill(pfs, curve, NULL, NULL);
                    if (code < 0)
                        return code;
                } else {
                    /* Quadrant 1 is a normal quarter annulua */
                    p0.x = x1, p0.y = y1;
                    p1.x = x1 - r2, p1.y = y1 + r2;
                    draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
                }

                /* Q0 is always a full annulus... */
                p0.x = x1, p0.y = y1;
                p1.x = x1 + r2, p1.y = y1 + r2;
                if (p1.y < 0)
                    p1.y = 0;
                draw_quarter_annulus(pfs, &p0, r1, &p1, t0);

                /* alpha is now the angle between the x axis and the tangent to the
                 * circles.
                 */
                alpha = (M_PI / 2) - alpha;
                cosalpha = cos(alpha);
                endy = y1 - (r1 / cosalpha);
                endx = x1;

                p0.x = tx, p0.y = ty;
                p1.x = endx + ((endx - tx) / 2), p1.y = endy - ((ty - endy) / 2);
                patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[0], t0);
                p0.x = endx, p0.y = endy;
                patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[1], t0);
                p1.x = x1, p1.y = y1 - r1;
                patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[2], t0);
                p0.x = tx, p0.y = ty;
                centre.x = x1, centre.y = y1;
                patch_curveto(&pfs->pgs->ctm, &centre, &p1, &p0, &curve[3], t0);
                code = patch_fill(pfs, curve, NULL, NULL);
                if (code < 0)
                    return code;

                /* Q3 is simimlar to Q1, either a full quarter annulus
                 * or a partial one, depending on where the tangent crosses
                 * the y axis
                 */
                tanalpha = tan(alpha);
                intersecty = y1 - tanalpha * (r2 + (x1 - intersectx));
                intersectx = x1 + r2;

                if (endy > y1 - r2) {
                    /* didn't get all the way to the edge, quadrant 3 is composed of 2 quads :-(
                     * An 'annulus' where the right edge is less than the normal extent and a
                     * quad which is a rectangle with one corner chopped of at an angle.
                     */
                    p0.x = x1, p0.y = y1;
                    p1.x = x1 + r2, p1.y = endy;
                    draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
                    p0.x = x1, p0.y = y1 - r1;
                    p1.x = x1, p1.y = endy;
                    patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[0], t0);
                    p0.x = x1 + r2, p0.y = intersecty;
                    patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[1], t0);
                    p1.x = p0.x, p1.y = y1 - r1;
                    patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[2], t0);
                    p0.x = x1, p0.y = y1 - r1;
                    patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[3], t0);
                    code = patch_fill(pfs, curve, NULL, NULL);
                    if (code < 0)
                        return code;
                } else {
                    /* Quadrant 1 is a normal quarter annulua */
                    p0.x = x1, p0.y = y1;
                    p1.x = x1 + r2, p1.y = y1 - r2;
                    draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
                }
                break;
            case 3:
                /* We have two four-sided elements, from the tangent point
                 * each side, to the point where the tangent crosses an
                 * axis of the larger circle. A line back to the edge
                 * of the larger circle, a line to the point where an axis
                 * crosses the smaller circle, then an arc back to the starting point.
                 */
                /* Figure out the tangent point */
                /* sin (angle) = y1 - y0 / r1 - r0
                 * ty = ((y1 - y0) / (r1 - r0)) * r1
                 */
                ty = y1 - ((y1 - y0) / (r1 - r0)) * r1;
                tx = x1 + ((x0 - x1) / (r1 - r0)) * r1;
                /* Now actually calculating the point where the tangent crosses the axis of the larger circle
                 * So we need to know the angle the tangent makes with the axis of the smaller circle
                 * as its the same angle where it crosses the axis of the larger circle.
                 * We know the centres and the tangent are co-linear, so sin(a) = y0 - y1 / r1 - r0
                 * We know the tangent is r1 from the centre of the larger circle, so the hypotenuse
                 * is r0 / cos(a). That gives us 'x' and we already know y as its the centre of the larger
                 * circle
                 */
                sinalpha = (y1 - y0) / (r1 - r0);
                alpha = asin(sinalpha);
                cosalpha = cos(alpha);
                intersectx = x1 + (r1 / cosalpha);
                intersecty = y1;

                p0.x = tx, p0.y = ty;
                p1.x = tx + (intersectx - tx) / 2, p1.y = ty + (intersecty - ty) / 2;
                patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[0], t0);
                p0.x = intersectx, p0.y = intersecty;
                patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[1], t0);
                p1.x = x1 + r1, p1.y = y1;
                patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[2], t0);
                p0.x = tx, p0.y = ty;
                centre.x = x1, centre.y = y1;
                patch_curveto(&pfs->pgs->ctm, &centre, &p1, &p0, &curve[3], t0);
                code = patch_fill(pfs, curve, NULL, NULL);
                if (code < 0)
                    return code;
                if (intersectx < x1 + r2) {
                    /* didn't get all the way to the edge, quadrant 0 is composed of 2 quads :-(
                     * An 'annulus' where the right edge is less than the normal extent and a
                     * quad which is a rectangle with one corner chopped of at an angle.
                     */
                    p0.x = x1, p0.y = y1;
                    p1.x = intersectx, p1.y = y1 + r2;
                    draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
                    endy = y1 + r2;
                    endx = intersectx + ((endy - intersecty) / (tan ((M_PI / 2) - alpha)));
                    p0.x = intersectx, p0.y = y1;
                    p1.x = endx, p1.y = endy;
                    patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[0], t0);
                    p0.x = x1 + r1, p0.y = endy;
                    patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[1], t0);
                    p1.x = x1 + r1, p1.y = y1;
                    patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[2], t0);
                    p0.x = intersectx, p0.y = y1;
                    patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[3], t0);
                    code = patch_fill(pfs, curve, NULL, NULL);
                    if (code < 0)
                        return code;

                } else {
                    /* Quadrant 0 is a normal quarter annulua */
                    p0.x = x1, p0.y = y1;
                    p1.x = x1 + r2, p1.y = y1 + r2;
                    draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
                }
                /* Q1 is always a full annulus... */
                p0.x = x1, p0.y = y1;
                p1.x = x1 - r2, p1.y = y1 + r2;
                draw_quarter_annulus(pfs, &p0, r1, &p1, t0);

                /* alpha is now the angle between the x axis and the tangent to the
                 * circles.
                 */
                alpha = (M_PI / 2) - alpha;
                cosalpha = cos(alpha);
                endy = y1 - (r1 / cosalpha);
                endx = x1;

                p0.x = tx, p0.y = ty;
                p1.x = endx + ((tx - endx) / 2), p1.y = endy + ((ty - endy) / 2);
                patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[0], t0);
                p0.x = endx, p0.y = endy;
                patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[1], t0);
                p1.x = x1, p1.y = y1 - r1;
                patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[2], t0);
                p0.x = tx, p0.y = ty;
                centre.x = x1, centre.y = y1;
                patch_curveto(&pfs->pgs->ctm, &centre, &p1, &p0, &curve[3], t0);
                code = patch_fill(pfs, curve, NULL, NULL);
                if (code < 0)
                    return code;

                /* Q3 is simimlar to Q1, either a full quarter annulus
                 * or a partial one, depending on where the tangent crosses
                 * the y axis
                 */
                tanalpha = tan(alpha);
                intersecty = y1 - tanalpha * (r2 + (intersectx - x1));
                intersectx = x1 - r2;

                if (endy > y1 - r2) {
                    /* didn't get all the way to the edge, quadrant 3 is composed of 2 quads :-(
                     * An 'annulus' where the right edge is less than the normal extent and a
                     * quad which is a rectangle with one corner chopped of at an angle.
                     */
                    p0.x = x1, p0.y = y1;
                    p1.x = x1 - r2, p1.y = endy;
                    draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
                    p0.x = x1, p0.y = y1 - r1;
                    p1.x = x1, p1.y = endy;
                    patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[0], t0);
                    p0.x = x1 - r2, p0.y = intersecty;
                    patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[1], t0);
                    p1.x = p0.x, p1.y = y1 - r1;
                    patch_lineto(&pfs->pgs->ctm, &p0, &p1, &curve[2], t0);
                    p0.x = x1, p0.y = y1 - r1;
                    patch_lineto(&pfs->pgs->ctm, &p1, &p0, &curve[3], t0);
                    code = patch_fill(pfs, curve, NULL, NULL);
                    if (code < 0)
                        return code;
                } else {
                    /* Quadrant 1 is a normal quarter annulua */
                    p0.x = x1, p0.y = y1;
                    p1.x = x1 - r2, p1.y = y1 - r2;
                    draw_quarter_annulus(pfs, &p0, r1, &p1, t0);
                }
                break;
        }
    }
    return 0;
}

static int
R_outer_circle(patch_fill_state_t *pfs, const gs_rect *rect,
        double x0, double y0, double r0,
        double x1, double y1, double r1,
        double *x2, double *y2, double *r2)
{
    double dx = x1 - x0, dy = y1 - y0;
    double sp, sq, s;

    /* Compute a cone circle, which contacts the rect externally. */
    /* Don't bother with all 4 sides of the rect,
       just do with the X or Y span only,
       so it's not an exact contact, sorry. */
    if (any_abs(dx) > any_abs(dy)) {
        /* Solving :
            x0 + (x1 - x0) * sq + r0 + (r1 - r0) * sq == bbox_px
            (x1 - x0) * sp + (r1 - r0) * sp == bbox_px - x0 - r0
            sp = (bbox_px - x0 - r0) / (x1 - x0 + r1 - r0)

            x0 + (x1 - x0) * sq - r0 - (r1 - r0) * sq == bbox_qx
            (x1 - x0) * sq - (r1 - r0) * sq == bbox_x - x0 + r0
            sq = (bbox_x - x0 + r0) / (x1 - x0 - r1 + r0)
         */
        if (x1 - x0 + r1 - r0 ==  0) /* We checked for obtuse cone. */
            return_error(gs_error_unregistered); /* Must not happen. */
        if (x1 - x0 - r1 + r0 ==  0) /* We checked for obtuse cone. */
            return_error(gs_error_unregistered); /* Must not happen. */
        sp = (rect->p.x - x0 - r0) / (x1 - x0 + r1 - r0);
        sq = (rect->q.x - x0 + r0) / (x1 - x0 - r1 + r0);
    } else {
        /* Same by Y. */
        if (y1 - y0 + r1 - r0 ==  0) /* We checked for obtuse cone. */
            return_error(gs_error_unregistered); /* Must not happen. */
        if (y1 - y0 - r1 + r0 ==  0) /* We checked for obtuse cone. */
            return_error(gs_error_unregistered); /* Must not happen. */
        sp = (rect->p.y - y0 - r0) / (y1 - y0 + r1 - r0);
        sq = (rect->q.y - y0 + r0) / (y1 - y0 - r1 + r0);
    }
    if (sp >= 1 && sq >= 1)
        s = max(sp, sq);
    else if(sp >= 1)
        s = sp;
    else if (sq >= 1)
        s = sq;
    else {
        /* The circle 1 is outside the rect, use it. */
        s = 1;
    }
    if (r0 + (r1 - r0) * s < 0) {
        /* Passed the cone apex, use the apex. */
        s = r0 / (r0 - r1);
        *r2 = 0;
    } else
        *r2 = r0 + (r1 - r0) * s;
    *x2 = x0 + (x1 - x0) * s;
    *y2 = y0 + (y1 - y0) * s;
    return 0;
}

static double
R_rect_radius(const gs_rect *rect, double x0, double y0)
{
    double d, dd;

    dd = hypot(rect->p.x - x0, rect->p.y - y0);
    d = hypot(rect->p.x - x0, rect->q.y - y0);
    dd = max(dd, d);
    d = hypot(rect->q.x - x0, rect->q.y - y0);
    dd = max(dd, d);
    d = hypot(rect->q.x - x0, rect->p.y - y0);
    dd = max(dd, d);
    return dd;
}

static int
R_fill_triangle_new(patch_fill_state_t *pfs, const gs_rect *rect,
    double x0, double y0, double x1, double y1, double x2, double y2, double t)
{
    shading_vertex_t p0, p1, p2;
    patch_color_t *c;
    int code;
    reserve_colors(pfs, &c, 1); /* Can't fail */

    p0.c = c;
    p1.c = c;
    p2.c = c;
    code = gs_point_transform2fixed(&pfs->pgs->ctm, x0, y0, &p0.p);
    if (code >= 0)
        code = gs_point_transform2fixed(&pfs->pgs->ctm, x1, y1, &p1.p);
    if (code >= 0)
        code = gs_point_transform2fixed(&pfs->pgs->ctm, x2, y2, &p2.p);
    if (code >= 0) {
        c->t[0] = c->t[1] = t;
        patch_resolve_color(c, pfs);
        code = mesh_triangle(pfs, &p0, &p1, &p2);
    }
    release_colors(pfs, pfs->color_stack, 1);
    return code;
}

static int
R_obtuse_cone(patch_fill_state_t *pfs, const gs_rect *rect,
        double x0, double y0, double r0,
        double x1, double y1, double r1, double t0, double r_rect)
{
    double dx = x1 - x0, dy = y1 - y0, dr = any_abs(r1 - r0);
    double d = hypot(dx, dy);
    /* Assuming dr > d / 3 && d > dr + 1e-7 * (d + dr), see the caller. */
    double r = r_rect * 1.4143; /* A few bigger than sqrt(2). */
    double ax, ay, as; /* Cone apex. */
    double g0; /* The distance from apex to the tangent point of the 0th circle. */
    int code;

    as = r0 / (r0 - r1);
    ax = x0 + (x1 - x0) * as;
    ay = y0 + (y1 - y0) * as;
    g0 = sqrt(dx * dx + dy * dy - dr * dr) * as;
    if (g0 < 1e-7 * r0) {
        /* Nearly degenerate, replace with half-plane. */
        /* Restrict the half plane with triangle, which covers the rect. */
        gs_point p0, p1, p2; /* Right tangent limit, apex limit, left tangent linit,
                                (right, left == when looking from the apex). */

        p0.x = ax - dy * r / d;
        p0.y = ay + dx * r / d;
        p1.x = ax - dx * r / d;
        p1.y = ay - dy * r / d;
        p2.x = ax + dy * r / d;
        p2.y = ay - dx * r / d;
        /* Split into 2 triangles at the apex,
           so that the apex is preciselly covered.
           Especially important when it is not exactly degenerate. */
        code = R_fill_triangle_new(pfs, rect, ax, ay, p0.x, p0.y, p1.x, p1.y, t0);
        if (code < 0)
            return code;
        return R_fill_triangle_new(pfs, rect, ax, ay, p1.x, p1.y, p2.x, p2.y, t0);
    } else {
        /* Compute the "limit" circle so that its
           tangent points are outside the rect. */
        /* Note: this branch is executed when the condition above is false :
           g0 >= 1e-7 * r0 .
           We believe that computing this branch with doubles
           provides enough precision after converting coordinates into 'fixed',
           and that the limit circle radius is not dramatically big.
         */
        double es, er; /* The limit circle parameter, radius. */
        double ex, ey; /* The limit circle centrum. */

        es = as - as * r / g0; /* Always negative. */
        er = r * r0 / g0 ;
        ex = x0 + dx * es;
        ey = y0 + dy * es;
        /* Fill the annulus: */
        code = R_tensor_annulus(pfs, x0, y0, r0, t0, ex, ey, er, t0);
        if (code < 0)
            return code;
        /* Fill entire ending circle to ensure entire rect is covered. */
        return R_tensor_annulus(pfs, ex, ey, er, t0, ex, ey, 0, t0);
    }
}

static int
R_tensor_cone_apex(patch_fill_state_t *pfs, const gs_rect *rect,
        double x0, double y0, double r0,
        double x1, double y1, double r1, double t)
{
    double as = r0 / (r0 - r1);
    double ax = x0 + (x1 - x0) * as;
    double ay = y0 + (y1 - y0) * as;

    return R_tensor_annulus(pfs, x1, y1, r1, t, ax, ay, 0, t);
}

/*
 * A map of this code:
 *
 * R_extensions
 * |-> (R_rect_radius)
 * |-> (R_outer_circle)
 * |-> R_obtuse_cone
 * |   |-> R_fill_triangle_new
 * |   |   '-> mesh_triangle
 * |   |       '-> mesh_triangle_rec <--.
 * |   |           |--------------------'
 * |   |           |-> small_mesh_triangle
 * |   |           |   '-> fill_triangle
 * |   |           |       '-> triangle_by_4 <--.
 * |   |           |           |----------------'
 * |   |           |           |-> constant_color_triangle
 * |   |           |           |-> make_wedge_median (etc)
 * |   |           '-----------+--------------------.
 * |   '-------------------.                        |
 * |-> R_tensor_cone_apex  |                        |
 * |   '-------------------+                        |
 * '-> R_tensor_annulus <--'                       \|/
 *     |-> (make_quadrant_arc)                      |
 *     '-> patch_fill                               |
 *         |-> fill_patch <--.                      |
 *         |   |-------------'                      |
 *         |   |------------------------------------+
 *         |   '-> fill_stripe                      |
 *         |       |-----------------------.        |
 *         |      \|/                      |        |
 *         |-> fill_wedges                 |        |
 *             '-> fill_wedges_aux <--.    |        |
 *                 |------------------'   \|/       |
 *                 |----------------> mesh_padding  '
 *                 |                  '----------------------------------.
 *                 '-> wedge_by_triangles <--.      .                    |
 *                     |---------------------'      |                    |
 *                     '-> fill_triangle_wedge <----'                    |
 *                         '-> fill_triangle_wedge_aux                   |
 *                             '-> fill_wedge_trap                       |
 *                                 '-> wedge_trap_decompose              |
 *                                     '-> linear_color_trapezoid        |
 *                                         '-> decompose_linear_color <--|
 *                                             |-------------------------'
 *                                             '-> constant_color_trapezoid
 */
static int
R_extensions(patch_fill_state_t *pfs, const gs_shading_R_t *psh, const gs_rect *rect,
        double t0, double t1, bool Extend0, bool Extend1)
{
    float x0 = psh->params.Coords[0], y0 = psh->params.Coords[1];
    double r0 = psh->params.Coords[2];
    float x1 = psh->params.Coords[3], y1 = psh->params.Coords[4];
    double r1 = psh->params.Coords[5];
    double dx = x1 - x0, dy = y1 - y0, dr = any_abs(r1 - r0);
    double d = hypot(dx, dy), r;
    int code;

    /* In order for the circles to be nested, one end circle
     * needs to be sufficiently large to cover the entirety
     * of the other end circle. i.e.
     *
     *     max(r0,r1) >= d + min(r0,r1)
     * === min(r0,r1) + dr >= d + min(r0,r1)
     * === dr >= d
     *
     * This, plus a fudge factor for FP operation is what we use below.
     *
     * An "Obtuse Cone" is defined to be one for which the "opening
     * angle" is obtuse.
     *
     * Consider two circles; one at (r0,r0) of radius r0, and one at
     * (r1,r1) of radius r1. These clearly lie on the acute/obtuse
     * boundary. The distance between the centres of these two circles
     * is d = sqr(2.(r0-r1)^2) by pythagoras. Thus d = sqr(2).dr.
     * By observation if d gets longer, we become acute, shorter, obtuse.
     * i.e. if sqr(2).dr > d we are obtuse, if d > sqr(2).dr we are acute.
     * (Thanks to Paul Gardiner for this reasoning).
     *
     * The code below tests (dr > d/3) (i.e. 3.dr > d). This
     * appears to be a factor of 2 and a bit out, so I am confused
     * by it.
     *
     * Either Igor meant something different to the standard meaning
     * of "Obtuse Cone", or he got his maths wrong. Or he was more
     * cunning than I can understand. Leave it as it until we find
     * an actual example that goes wrong.
     */

    /* Tests with Acrobat seem to indicate that it uses a fudge factor
     * of around .0001. (i.e. [1.0001 0 0 0 0 1] is accepted as a
     * non nested circle, but [1.00009 0 0 0 0 1] is a nested one.
     * Approximate the same sort of value here to appease bug 690831.
     */
    if (any_abs (dr - d) < 0.001) {
        if ((r0 > r1 && Extend0) || (r1 > r0 && Extend1)) {
            r = R_rect_radius(rect, x0, y0);
            if (r0 < r1)
                code = R_tensor_annulus_extend_tangent(pfs, x0, y0, r0, t1, x1, y1, r1, t1, r);
            else
                code = R_tensor_annulus_extend_tangent(pfs, x1, y1, r1, t0, x0, y0, r0, t0, r);
            if (code < 0)
                return code;
        } else {
            if (r0 > r1) {
                if (Extend1 && r1 > 0)
                    return R_tensor_annulus(pfs, x1, y1, r1, t1, x1, y1, 0, t1);
            }
            else {
                if (Extend0 && r0 > 0)
                    return R_tensor_annulus(pfs, x0, y0, r0, t0, x0, y0, 0, t0);
            }
        }
    } else
    if (dr > d - 1e-4 * (d + dr)) {
        /* Nested circles, or degenerate. */
        if (r0 > r1) {
            if (Extend0) {
                r = R_rect_radius(rect, x0, y0);
                if (r > r0) {
                    code = R_tensor_annulus(pfs, x0, y0, r, t0, x0, y0, r0, t0);
                    if (code < 0)
                        return code;
                }
            }
            if (Extend1 && r1 > 0)
                return R_tensor_annulus(pfs, x1, y1, r1, t1, x1, y1, 0, t1);
        } else {
            if (Extend1) {
                r = R_rect_radius(rect, x1, y1);
                if (r > r1) {
                    code = R_tensor_annulus(pfs, x1, y1, r, t1, x1, y1, r1, t1);
                    if (code < 0)
                        return code;
                }
            }
            if (Extend0 && r0 > 0)
                return R_tensor_annulus(pfs, x0, y0, r0, t0, x0, y0, 0, t0);
        }
    } else if (dr > d / 3) {
        /* Obtuse cone. */
        if (r0 > r1) {
            if (Extend0) {
                r = R_rect_radius(rect, x0, y0);
                code = R_obtuse_cone(pfs, rect, x0, y0, r0, x1, y1, r1, t0, r);
                if (code < 0)
                    return code;
            }
            if (Extend1 && r1 != 0)
                return R_tensor_cone_apex(pfs, rect, x0, y0, r0, x1, y1, r1, t1);
            return 0;
        } else {
            if (Extend1) {
                r = R_rect_radius(rect, x1, y1);
                code = R_obtuse_cone(pfs, rect, x1, y1, r1, x0, y0, r0, t1, r);
                if (code < 0)
                    return code;
            }
            if (Extend0 && r0 != 0)
                return R_tensor_cone_apex(pfs, rect, x1, y1, r1, x0, y0, r0, t0);
        }
    } else {
        /* Acute cone or cylinder. */
        double x2, y2, r2, x3, y3, r3;

        if (Extend0) {
            code = R_outer_circle(pfs, rect, x1, y1, r1, x0, y0, r0, &x3, &y3, &r3);
            if (code < 0)
                return code;
            if (x3 != x1 || y3 != y1) {
                code = R_tensor_annulus(pfs, x0, y0, r0, t0, x3, y3, r3, t0);
                if (code < 0)
                    return code;
            }
        }
        if (Extend1) {
            code = R_outer_circle(pfs, rect, x0, y0, r0, x1, y1, r1, &x2, &y2, &r2);
            if (code < 0)
                return code;
            if (x2 != x0 || y2 != y0) {
                code = R_tensor_annulus(pfs, x1, y1, r1, t1, x2, y2, r2, t1);
                if (code < 0)
                    return code;
            }
        }
    }
    return 0;
}

static int
R_fill_rect_with_const_color(patch_fill_state_t *pfs, const gs_fixed_rect *clip_rect, float t)
{
#if 0 /* Disabled because the clist writer device doesn't pass
         the clipping path with fill_recatangle. */
    patch_color_t pc;
    const gs_color_space *pcs = pfs->direct_space;
    gx_device_color dc;
    int code;

    code = gs_function_evaluate(pfs->Function, &t, pc.cc.paint.values);
    if (code < 0)
        return code;
    pcs->type->restrict_color(&pc.cc, pcs);
    code = patch_color_to_device_color(pfs, &pc, &dc);
    if (code < 0)
        return code;
    return gx_fill_rectangle_device_rop(fixed2int_pixround(clip_rect->p.x), fixed2int_pixround(clip_rect->p.y),
                                        fixed2int_pixround(clip_rect->q.x) - fixed2int_pixround(clip_rect->p.x),
                                        fixed2int_pixround(clip_rect->q.y) - fixed2int_pixround(clip_rect->p.y),
                                        &dc, pfs->dev, pfs->pgs->log_op);
#else
    /* Can't apply fill_rectangle, because the clist writer device doesn't pass
       the clipping path with fill_recatangle. Convert into trapezoids instead.
    */
    quadrangle_patch p;
    shading_vertex_t pp[2][2];
    const gs_color_space *pcs = pfs->direct_space;
    patch_color_t pc;
    int code;

    code = gs_function_evaluate(pfs->Function, &t, pc.cc.paint.values);
    if (code < 0)
        return code;
    pcs->type->restrict_color(&pc.cc, pcs);
    pc.t[0] = pc.t[1] = t;
    pp[0][0].p = clip_rect->p;
    pp[0][1].p.x = clip_rect->q.x;
    pp[0][1].p.y = clip_rect->p.y;
    pp[1][0].p.x = clip_rect->p.x;
    pp[1][0].p.y = clip_rect->q.y;
    pp[1][1].p = clip_rect->q;
    pp[0][0].c = pp[0][1].c = pp[1][0].c = pp[1][1].c = &pc;
    p.p[0][0] = &pp[0][0];
    p.p[0][1] = &pp[0][1];
    p.p[1][0] = &pp[1][0];
    p.p[1][1] = &pp[1][1];
    return constant_color_quadrangle(pfs, &p, false);
#endif
}

typedef struct radial_shading_attrs_s {
    double x0, y0;
    double x1, y1;
    double span[2][2];
    double apex;
    bool have_apex;
    bool have_root[2]; /* ongoing contact, outgoing contact. */
    bool outer_contact[2];
    gs_point p[6]; /* 4 corners of the rectangle, p[4] = p[0], p[5] = p[1] */
} radial_shading_attrs_t;

#define Pw2(a) ((a)*(a))

static void
radial_shading_external_contact(radial_shading_attrs_t *rsa, int point_index, double t, double r0, double r1, bool at_corner, int root_index)
{
    double cx = rsa->x0 + (rsa->x1 - rsa->x0) * t;
    double cy = rsa->y0 + (rsa->y1 - rsa->y0) * t;
    double rx = rsa->p[point_index].x - cx;
    double ry = rsa->p[point_index].y - cy;
    double dx = rsa->p[point_index - 1].x - rsa->p[point_index].x;
    double dy = rsa->p[point_index - 1].y - rsa->p[point_index].y;

    if (at_corner) {
        double Dx = rsa->p[point_index + 1].x - rsa->p[point_index].x;
        double Dy = rsa->p[point_index + 1].y - rsa->p[point_index].y;
        bool b1 = (dx * rx + dy * ry >= 0);
        bool b2 = (Dx * rx + Dy * ry >= 0);

        if (b1 & b2)
            rsa->outer_contact[root_index] = true;
    } else {
        if (rx * dy - ry * dx < 0)
            rsa->outer_contact[root_index] = true;
    }
}

static void
store_roots(radial_shading_attrs_t *rsa, const bool have_root[2], const double t[2], double r0, double r1, int point_index, bool at_corner)
{
    int i;

    for (i = 0; i < 2; i++) {
        bool good_root;

        if (!have_root[i])
            continue;
        good_root = (!rsa->have_apex || (rsa->apex <= 0 || r0 == 0 ? t[i] >= rsa->apex : t[i] <= rsa->apex));
        if (good_root) {
            radial_shading_external_contact(rsa, point_index, t[i], r0, r1, at_corner, i);
            if (!rsa->have_root[i]) {
                rsa->span[i][0] = rsa->span[i][1] = t[i];
                rsa->have_root[i] = true;
            } else {
                if (rsa->span[i][0] > t[i])
                    rsa->span[i][0] = t[i];
                if (rsa->span[i][1] < t[i])
                    rsa->span[i][1] = t[i];
            }
        }
    }
}

static void
compute_radial_shading_span_extended_side(radial_shading_attrs_t *rsa, double r0, double r1, int point_index)
{
    double cc, c;
    bool have_root[2] = {false, false};
    double t[2];
    bool by_x = (rsa->p[point_index].x != rsa->p[point_index + 1].x);
    int i;

    /* As t moves from 0 to 1, the circles move from r0 to r1, and from
     * from position p0 to py. For simplicity, adjust so that p0 is at
     * the origin. Consider the projection of the circle drawn at any given
     * time onto the x axis. The range of points would be:
     * p1x*t +/- (r0+(r1-r0)*t). We are interested in the first (and last)
     * moments when the range includes a point c on the x axis. So solve for:
     * p1x*t +/- (r0+(r1-r0)*t) = c. Let cc = p1x.
     * So p1x*t0 + (r1-r0)*t0 = c - r0 => t0 = (c - r0)/(p1x + r1 - r0)
     *    p1x*t1 - (r1-r0)*t1 = c + r0 => t1 = (c + r0)/(p1x - r1 + r0)
     */
    if (by_x) {
        c = rsa->p[point_index].x - rsa->x0;
        cc = rsa->x1 - rsa->x0;
    } else {
        c = rsa->p[point_index].y - rsa->y0;
        cc = rsa->y1 - rsa->y0;
    }
    t[0] = (c - r0) / (cc + r1 - r0);
    t[1] = (c + r0) / (cc - r1 + r0);
    if (t[0] > t[1]) {
        c    = t[0];
        t[0] = t[1];
        t[1] = c;
    }
    for (i = 0; i < 2; i++) {
        double d, d0, d1;

        if (by_x) {
            d = rsa->y1 - rsa->y0 + r0 + (r1 - r0) * t[i];
            d0 = rsa->p[point_index].y;
            d1 = rsa->p[point_index + 1].y;
        } else {
            d = rsa->x1 - rsa->x0 + r0 + (r1 - r0) * t[i];
            d0 = rsa->p[point_index].x;
            d1 = rsa->p[point_index + 1].x;
        }
        if (d1 > d0 ? d0 <= d && d <= d1 : d1 <= d && d <= d0)
            have_root[i] = true;
    }
    store_roots(rsa, have_root, t, r0, r1, point_index, false);
}

static int
compute_radial_shading_span_extended_point(radial_shading_attrs_t *rsa, double r0, double r1, int point_index)
{
    /* As t moves from 0 to 1, the circles move from r0 to r1, and from
     * from position p0 to py. At any given time t, therefore, we
     * paint the points that are distance r0+(r1-r0)*t from point
     * (p0x+(p1x-p0x)*t,p0y+(p1y-p0y)*t) = P(t).
     *
     * To simplify our algebra, adjust so that (p0x, p0y) is at the origin.
     * To find the time(s) t at which the a point q is painted, we therefore
     * solve for t in:
     *
     * |q-P(t)| = r0+(r1-r0)*t
     *
     *   (qx-p1x*t)^2 + (qy-p1y*t)^2 - (r0+(r1-r0)*t)^2 = 0
     * = qx^2 - 2qx.p1x.t + p1x^2.t^2 + qy^2 - 2qy.p1y.t + p1y^2.t^2 -
     *                                   (r0^2 + 2r0(r1-r0)t + (r1-r0)^2.t^2)
     * =   qx^2 + qy^2 - r0^2
     *   + -2(qx.p1x + qy.p1y + r0(r1-r0)).t
     *   + (p1x^2 + p1y^2 - (r1-r0)^2).t^2
     *
     * So solve using the usual t = (-b +/- SQRT(b^2 - 4ac)) where
     *   a = p1x^2 + p1y^2 - (r1-r0)^2
     *   b = -2(qx.p1x + qy.p1y + r0(r1-r0))
     *   c = qx^2 + qy^2 - r0^2
     */
    double p1x = rsa->x1 - rsa->x0;
    double p1y = rsa->y1 - rsa->y0;
    double qx  = rsa->p[point_index].x - rsa->x0;
    double qy  = rsa->p[point_index].y - rsa->y0;
    double a   = (Pw2(p1x) + Pw2(p1y) - Pw2(r0 - r1));
    bool have_root[2] = {false, false};
    double t[2];

    if (fabs(a) < 1e-8) {
        /* Linear equation. */
        /* This case is always the ongoing ellipse contact. */
        double cx = rsa->x0 - (rsa->x1 - rsa->x0) * r0 / (r1 - r0);
        double cy = rsa->y0 - (rsa->y1 - rsa->y0) * r0 / (r1 - r0);

        t[0] = (Pw2(qx) + Pw2(qy))/(cx*qx + cy*qy) / 2;
        have_root[0] = true;
    } else {
        /* Square equation.  No solution if b^2 - 4ac = 0. Equivalently if
         * (b^2)/4 -a.c = 0 === (b/2)^2 - a.c = 0 ===  (-b/2)^2 - a.c = 0 */
        double minushalfb = r0*(r1-r0) + p1x*qx + p1y*qy;
        double c          = Pw2(qx) + Pw2(qy) - Pw2(r0);
        double desc2      = Pw2(minushalfb) - a*c; /* desc2 = 1/4 (b^2-4ac) */

        if (desc2 < 0) {
            return -1; /* The point is outside the shading coverage.
                          Do not shorten, because we didn't observe it in practice. */
        } else {
            double desc1 = sqrt(desc2); /* desc1 = 1/2 SQRT(b^2-4ac) */

            if (a > 0) {
                t[0] = (minushalfb - desc1) / a;
                t[1] = (minushalfb + desc1) / a;
            } else {
                t[0] = (minushalfb + desc1) / a;
                t[1] = (minushalfb - desc1) / a;
            }
            have_root[0] = have_root[1] = true;
        }
    }
    store_roots(rsa, have_root, t, r0, r1, point_index, true);
    if (have_root[0] && have_root[1])
        return 15;
    if (have_root[0])
        return 15 - 4;
    if (have_root[1])
        return 15 - 2;
    return -1;
}

#undef Pw2

static int
compute_radial_shading_span_extended(radial_shading_attrs_t *rsa, double r0, double r1)
{
    int span_type0, span_type1;

    span_type0 = compute_radial_shading_span_extended_point(rsa, r0, r1, 1);
    if (span_type0 == -1)
        return -1;
    span_type1 = compute_radial_shading_span_extended_point(rsa, r0, r1, 2);
    if (span_type0 != span_type1)
        return -1;
    span_type1 = compute_radial_shading_span_extended_point(rsa, r0, r1, 3);
    if (span_type0 != span_type1)
        return -1;
    span_type1 = compute_radial_shading_span_extended_point(rsa, r0, r1, 4);
    if (span_type0 != span_type1)
        return -1;
    compute_radial_shading_span_extended_side(rsa, r0, r1, 1);
    compute_radial_shading_span_extended_side(rsa, r0, r1, 2);
    compute_radial_shading_span_extended_side(rsa, r0, r1, 3);
    compute_radial_shading_span_extended_side(rsa, r0, r1, 4);
    return span_type0;
}

static int
compute_radial_shading_span(radial_shading_attrs_t *rsa, float x0, float y0, double r0, float x1, float y1, double r1, const gs_rect * rect)
{
    /* If the shading area is much larger than the path bbox,
       we want to shorten the shading for a faster rendering.
       If any point of the path bbox falls outside the shading area,
       our math is not applicable, and we render entire shading.
       If the path bbox is inside the shading area,
       we compute 1 or 2 'spans' - the shading parameter intervals,
       which covers the bbox. For doing that we need to resolve
       a square eqation by the shading parameter
       for each corner of the bounding box,
       and for each side of the shading bbox.
       Note the equation to be solved in the user space.
       Since each equation gives 2 roots (because the points are
       strongly inside the shading area), we will get 2 parameter intervals -
       the 'lower' one corresponds to the first (ongoing) contact of
       the running circle, and the second one corresponds to the last (outgoing) contact
       (like in a sun eclipse; well our sun is rectangular).

       Here are few exceptions.

       First, the equation degenerates when the distance sqrt((x1-x0)^2 + (y1-y0)^2)
       appears equal to r0-r1. In this case the base circles do contact,
       and the running circle does contact at the same point.
       The equation degenerates to a linear one.
       Since we don't want float precision noize to affect the result,
       we compute this condition in 'fixed' coordinates.

       Second, Postscript approximates any circle with 3d order beziers.
       This approximation may give a 2% error.
       Therefore using the precise roots may cause a dropout.
       To prevetn them, we slightly modify the base radii.
       However the sign of modification smartly depends
       on the relative sizes of the base circles,
       and on the contact number. Currently we don't want to
       define and debug the smart optimal logic for that,
       so we simply try all 4 variants for each source equation,
       and use the union of intervals.

       Third, we could compute which quarter of the circle
       really covers the path bbox. Using it we could skip
       rendering of uncovering quarters. Currently we do not
       implement this optimization. The general tensor patch algorithm
       will skip uncovering parts.

       Fourth, when one base circle is (almost) inside the other,
       the parameter interval must include the shading apex.
       To know that, we determine whether the contacting circle
       is outside the rectangle (the "outer" contact),
       or it is (partially) inside the rectangle.

       At last, a small shortening of a shading won't give a
       sensible speedup, but it may replace a symmetric function domain
       with an assymmetric one, so that the rendering
       would be asymmetyric for a symmetric shading.
       Therefore we do not perform a small sortening.
       Instead we shorten only if the shading span
       is much smaller that the shading domain.
     */
    const double extent = 1.02;
    int span_type0, span_type1, span_type;

    memset(rsa, 0, sizeof(*rsa));
    rsa->x0 = x0;
    rsa->y0 = y0;
    rsa->x1 = x1;
    rsa->y1 = y1;
    rsa->p[0] = rsa->p[4] = rect->p;
    rsa->p[1].x = rsa->p[5].x = rect->p.x;
    rsa->p[1].y = rsa->p[5].y = rect->q.y;
    rsa->p[2] = rect->q;
    rsa->p[3].x = rect->q.x;
    rsa->p[3].y = rect->p.y;
    rsa->have_apex = any_abs(r1 - r0) > 1e-7 * any_abs(r1 + r0);
    rsa->apex = (rsa->have_apex ? -r0 / (r1 - r0) : 0);
    span_type0 = compute_radial_shading_span_extended(rsa, r0 / extent, r1 * extent);
    if (span_type0 == -1)
        return -1;
    span_type1 = compute_radial_shading_span_extended(rsa, r0 / extent, r1 / extent);
    if (span_type0 != span_type1)
        return -1;
    span_type1 = compute_radial_shading_span_extended(rsa, r0 * extent, r1 * extent);
    if (span_type0 != span_type1)
        return -1;
    span_type1 = compute_radial_shading_span_extended(rsa, r0 * extent, r1 / extent);
    if (span_type1 == -1)
        return -1;
    if (r0 < r1) {
        if (rsa->have_root[0] && !rsa->outer_contact[0])
            rsa->span[0][0] = rsa->apex; /* Likely never happens. Remove ? */
        if (rsa->have_root[1] && !rsa->outer_contact[1])
            rsa->span[1][0] = rsa->apex;
    } else if (r0 > r1) {
        if (rsa->have_root[0] && !rsa->outer_contact[0])
            rsa->span[0][1] = rsa->apex;
        if (rsa->have_root[1] && !rsa->outer_contact[1])
            rsa->span[1][1] = rsa->apex; /* Likely never happens. Remove ? */
    }
    span_type = 0;
    if (rsa->have_root[0] && rsa->span[0][0] < 0)
        span_type |= 1;
    if (rsa->have_root[1] && rsa->span[1][0] < 0)
        span_type |= 1;
    if (rsa->have_root[0] && rsa->span[0][1] > 0 && rsa->span[0][0] < 1)
        span_type |= 2;
    if (rsa->have_root[1] && rsa->span[1][1] > 0 && rsa->span[1][0] < 1)
        span_type |= 4;
    if (rsa->have_root[0] && rsa->span[0][1] > 1)
        span_type |= 8;
    if (rsa->have_root[1] && rsa->span[1][1] > 1)
        span_type |= 8;
    return span_type;
}

static bool
shorten_radial_shading(float *x0, float *y0, double *r0, float *d0, float *x1, float *y1, double *r1, float *d1, double span_[2])
{
    double s0 = span_[0], s1 = span_[1], w;

    if (s0 < 0)
        s0 = 0;
    if (s1 < 0)
        s1 = 0;
    if (s0 > 1)
        s0 = 1;
    if (s1 > 1)
        s1 = 1;
    w = s1 - s0;
    if (w == 0)
        return false; /* Don't pass a degenerate shading. */
    if (w > 0.3)
        return false; /* The span is big, don't shorten it. */
    {	/* Do shorten. */
        double R0 = *r0, X0 = *x0, Y0 = *y0, D0 = *d0;
        double R1 = *r1, X1 = *x1, Y1 = *y1, D1 = *d1;

        *r0 = R0 + (R1 - R0) * s0;
        *x0 = X0 + (X1 - X0) * s0;
        *y0 = Y0 + (Y1 - Y0) * s0;
        *d0 = D0 + (D1 - D0) * s0;
        *r1 = R0 + (R1 - R0) * s1;
        *x1 = X0 + (X1 - X0) * s1;
        *y1 = Y0 + (Y1 - Y0) * s1;
        *d1 = D0 + (D1 - D0) * s1;
    }
    return true;
}

static bool inline
is_radial_shading_large(double x0, double y0, double r0, double x1, double y1, double r1, const gs_rect * rect)
{
    const double d = hypot(x1 - x0, y1 - y0);
    const double area0 = M_PI * r0 * r0 / 2;
    const double area1 = M_PI * r1 * r1 / 2;
    const double area2 = (r0 + r1) / 2 * d;
    const double arbitrary = 8;
    double areaX, areaY;

    /* The shading area is not equal to area0 + area1 + area2
       when one circle is (almost) inside the other.
       We believe that the 'arbitrary' coefficient recovers that
       when it is set greater than 2. */
    /* If one dimension is large enough, the shading parameter span is wide. */
    areaX = (rect->q.x - rect->p.x) * (rect->q.x - rect->p.x);
    if (areaX * arbitrary < area0 + area1 + area2)
        return true;
    areaY = (rect->q.y - rect->p.y) * (rect->q.y - rect->p.y);
    if (areaY * arbitrary < area0 + area1 + area2)
        return true;
    return false;
}

static int
gs_shading_R_fill_rectangle_aux(const gs_shading_t * psh0, const gs_rect * rect,
                            const gs_fixed_rect *clip_rect,
                            gx_device * dev, gs_gstate * pgs)
{
    const gs_shading_R_t *const psh = (const gs_shading_R_t *)psh0;
    float d0 = psh->params.Domain[0], d1 = psh->params.Domain[1];
    float x0 = psh->params.Coords[0], y0 = psh->params.Coords[1];
    double r0 = psh->params.Coords[2];
    float x1 = psh->params.Coords[3], y1 = psh->params.Coords[4];
    double r1 = psh->params.Coords[5];
    radial_shading_attrs_t rsa;
    int span_type; /* <0 - don't shorten, 1 - extent0, 2 - first contact, 4 - last contact, 8 - extent1. */
    int code;
    patch_fill_state_t pfs1;

    if (r0 == 0 && r1 == 0)
        return 0; /* PLRM requires to paint nothing. */
    code = shade_init_fill_state((shading_fill_state_t *)&pfs1, psh0, dev, pgs);
    if (code < 0)
        return code;
    pfs1.Function = psh->params.Function;
    code = init_patch_fill_state(&pfs1);
    if (code < 0) {
        if (pfs1.icclink != NULL) gsicc_release_link(pfs1.icclink);
        return code;
    }
    pfs1.function_arg_shift = 0;
    pfs1.rect = *clip_rect;
    pfs1.maybe_self_intersecting = false;
    if (is_radial_shading_large(x0, y0, r0, x1, y1, r1, rect))
        span_type = compute_radial_shading_span(&rsa, x0, y0, r0, x1, y1, r1, rect);
    else
        span_type = -1;
    if (span_type < 0) {
        code = R_extensions(&pfs1, psh, rect, d0, d1, psh->params.Extend[0], false);
        if (code >= 0)
            code = R_tensor_annulus(&pfs1, x0, y0, r0, d0, x1, y1, r1, d1);
        if (code >= 0)
            code = R_extensions(&pfs1, psh, rect, d0, d1, false, psh->params.Extend[1]);
    } else if (span_type == 1) {
        code = R_fill_rect_with_const_color(&pfs1, clip_rect, d0);
    } else if (span_type == 8) {
        code = R_fill_rect_with_const_color(&pfs1, clip_rect, d1);
    } else {
        bool second_interval = true;

        code = 0;
        if (span_type & 1)
            code = R_extensions(&pfs1, psh, rect, d0, d1, psh->params.Extend[0], false);
        if ((code >= 0) && (span_type & 2)) {
            float X0 = x0, Y0 = y0, D0 = d0, X1 = x1, Y1 = y1, D1 = d1;
            double R0 = r0, R1 = r1;

            if ((span_type & 4) && rsa.span[0][1] >= rsa.span[1][0]) {
                double united[2];

                united[0] = rsa.span[0][0];
                united[1] = rsa.span[1][1];
                shorten_radial_shading(&X0, &Y0, &R0, &D0, &X1, &Y1, &R1, &D1, united);
                second_interval = false;
            } else {
                second_interval = shorten_radial_shading(&X0, &Y0, &R0, &D0, &X1, &Y1, &R1, &D1, rsa.span[0]);
            }
            code = R_tensor_annulus(&pfs1, X0, Y0, R0, D0, X1, Y1, R1, D1);
        }
        if (code >= 0 && second_interval) {
            if (span_type & 4) {
                float X0 = x0, Y0 = y0, D0 = d0, X1 = x1, Y1 = y1, D1 = d1;
                double R0 = r0, R1 = r1;

                shorten_radial_shading(&X0, &Y0, &R0, &D0, &X1, &Y1, &R1, &D1, rsa.span[1]);
                code = R_tensor_annulus(&pfs1, X0, Y0, R0, D0, X1, Y1, R1, D1);
            }
        }
        if (code >= 0 && (span_type & 8))
            code = R_extensions(&pfs1, psh, rect, d0, d1, false, psh->params.Extend[1]);
    }
    if (pfs1.icclink != NULL) gsicc_release_link(pfs1.icclink);
    if (term_patch_fill_state(&pfs1))
        return_error(gs_error_unregistered); /* Must not happen. */
    return code;
}

int
gs_shading_R_fill_rectangle(const gs_shading_t * psh0, const gs_rect * rect,
                            const gs_fixed_rect * rect_clip,
                            gx_device * dev, gs_gstate * pgs)
{
    return gs_shading_R_fill_rectangle_aux(psh0, rect, rect_clip, dev, pgs);
}
/* ======== END base/gxshade1.c ======== */

/* ---------------- stream.h / gxshade.h: the mesh data stream ---------------- */

/* pdfking: the shading's decoded data, read a byte at a time. */
typedef struct stream_s {
    const byte *data;
    uint pos, size;
} stream;
#define sgetc(s) ((s)->pos < (s)->size ? (int)(s)->data[(s)->pos++] : -1)
#define seofp(s) ((s)->pos >= (s)->size)
static int
sgets(stream *s, byte *buf, uint nmax, uint *pn)
{
    uint n = min(nmax, s->size - s->pos);

    memcpy(buf, s->data + s->pos, n);
    s->pos += n;
    *pn = n;
    return 0;
}
#define countof(a) count_of(a)

/* Define a stream for decoding packed coordinate values. */
typedef struct shade_coord_stream_s shade_coord_stream_t;
struct shade_coord_stream_s {
    stream ds;			/* stream if DataSource isn't one already -- */
                                /* first for GC-ability (maybe unneeded?) */
    stream *s;			/* DataSource or &ds */
    uint bits;			/* shifted bits of current byte */
    int left;			/* # of bits left in bits */
    bool ds_EOF;                /* The 'ds' stream reached EOF. */
    const gs_shading_mesh_params_t *params;
    const gs_matrix_fixed *pctm;
    int (*get_value)(shade_coord_stream_t *cs, int num_bits, uint *pvalue);
    int (*get_decoded)(shade_coord_stream_t *cs, int num_bits,
                       const float decode[2], float *pvalue);
    void (*align)(shade_coord_stream_t *cs, int radix);
    bool (*is_eod)(const shade_coord_stream_t *cs);
    int first_patch;            /* True, if we have not read a patch yet */
};

/* gscolor2.c gs_cspace_indexed_lookup: pdfking: Shading::lookup */
static int
gs_cspace_indexed_lookup(const gs_color_space *pcs, int index,
                         gs_client_color *pcc)
{
    pcs->shading->lookup(index, pcc->paint.values);
    return 0;
}

/* ======== BEGIN base/gxshade.c lines 40-310 ======== */
/* ================ Packed coordinate streams ================ */

/* Forward references */
static int cs_next_packed_value(shade_coord_stream_t *, int, uint *);
static int cs_next_array_value(shade_coord_stream_t *, int, uint *);
static int cs_next_packed_decoded(shade_coord_stream_t *, int,
                                   const float[2], float *);
static int cs_next_array_decoded(shade_coord_stream_t *, int,
                                  const float[2], float *);
static void cs_packed_align(shade_coord_stream_t *cs, int radix);
static void cs_array_align(shade_coord_stream_t *cs, int radix);
static bool cs_eod(const shade_coord_stream_t * cs);

/* Initialize a packed value stream. */
void
shade_next_init(shade_coord_stream_t * cs,
                const gs_shading_mesh_params_t * params,
                const gs_gstate * pgs)
{
    cs->params = params;
    cs->pctm = &pgs->ctm;
    /* pdfking: the stream's decoded data, from its start */
    cs->ds.data = params->DataSource->data.data();
    cs->ds.pos = 0;
    cs->ds.size = (uint)params->DataSource->data.size();
    cs->s = &cs->ds;
    if (false /* pdfking: a PDF shading's data is a stream */) {
        cs->get_value = cs_next_array_value;
        cs->get_decoded = cs_next_array_decoded;
        cs->align = cs_array_align;
    } else {
        cs->get_value = cs_next_packed_value;
        cs->get_decoded = cs_next_packed_decoded;
        cs->align = cs_packed_align;
    }
    cs->is_eod = cs_eod;
    cs->left = 0;
    cs->ds_EOF = false;
    cs->first_patch = 1;
}

/* Check for the End-Of-Data state form a stream. */
static bool
cs_eod(const shade_coord_stream_t * cs)
{
    return cs->ds_EOF;
}

/* Get the next (integer) value from a packed value stream. */
/* 1 <= num_bits <= sizeof(uint) * 8. */
static int
cs_next_packed_value(shade_coord_stream_t * cs, int num_bits, uint * pvalue)
{
    uint bits = cs->bits;
    int left = cs->left;

    if (left >= num_bits) {
        /* We can satisfy this request with the current buffered bits. */
        cs->left = left -= num_bits;
        *pvalue = (bits >> left) & ((1 << num_bits) - 1);
    } else {
        /* We need more bits. */
        int needed = num_bits - left;
        uint value = bits & ((1 << left) - 1);	/* all the remaining bits */

        for (; needed >= 8; needed -= 8) {
            int b = sgetc(cs->s);

            if (b < 0) {
                cs->ds_EOF = true;
                return_error(gs_error_rangecheck);
            }
            value = (value << 8) + b;
        }
        if (needed == 0) {
            cs->left = 0;
            *pvalue = value;
        } else {
            int b = sgetc(cs->s);

            if (b < 0) {
                cs->ds_EOF = true;
                return_error(gs_error_rangecheck);
            }
            cs->bits = b;
            cs->left = left = 8 - needed;
            *pvalue = (value << needed) + (b >> left);
        }
    }
    return 0;
}

/*
 * Get the next (integer) value from an unpacked array.  Note that
 * num_bits may be 0 if we are reading a coordinate or color value.
 */
static int
cs_next_array_value(shade_coord_stream_t * cs, int num_bits, uint * pvalue)
{
    float value;
    uint read;

    if (sgets(cs->s, (byte *)&value, sizeof(float), &read) < 0 ||
        read != sizeof(float)) {
        cs->ds_EOF = true;
        return_error(gs_error_rangecheck);
    }
    if (value < 0 || (num_bits != 0 && num_bits < sizeof(uint) * 8 &&
         value >= (1 << num_bits)) ||
        value != (uint)value
        )
        return_error(gs_error_rangecheck);
    *pvalue = (uint) value;
    return 0;
}

/* Get the next decoded floating point value. */
static int
cs_next_packed_decoded(shade_coord_stream_t * cs, int num_bits,
                       const float decode[2], float *pvalue)
{
    uint value;
    int code = cs->get_value(cs, num_bits, &value);
    double max_value = (double)(uint)
        (num_bits == sizeof(uint) * 8 ? ~0 : ((1 << num_bits) - 1));
    double dvalue = (double)value;

    if (code < 0)
        return code;
    *pvalue =
        (decode == 0 ? dvalue / max_value :
         decode[0] + dvalue * (decode[1] - decode[0]) / max_value);
    return 0;
}

/* Get the next floating point value from an array, without decoding. */
static int
cs_next_array_decoded(shade_coord_stream_t * cs, int num_bits,
                      const float decode[2], float *pvalue)
{
    float value;
    uint read;

    if (sgets(cs->s, (byte *)&value, sizeof(float), &read) < 0 ||
        read != sizeof(float)
    ) {
        cs->ds_EOF = true;
        return_error(gs_error_rangecheck);
    }
    *pvalue = value;
    return 0;
}

static void
cs_packed_align(shade_coord_stream_t *cs, int radix)
{
    cs->left = cs->left / radix * radix;
}

static void
cs_array_align(shade_coord_stream_t *cs, int radix)
{
}

/* Get the next flag value. */
/* Note that this always starts a new data byte. */
int
shade_next_flag(shade_coord_stream_t * cs, int BitsPerFlag)
{
    uint flag;
    int code;

    cs->left = 0;		/* start a new byte if packed */
    code = cs->get_value(cs, BitsPerFlag, &flag);
    return (code < 0 ? code : flag);
}

/* Get one or more coordinate pairs. */
int
shade_next_coords(shade_coord_stream_t * cs, gs_fixed_point * ppt,
                  int num_points)
{
    int num_bits = cs->params->BitsPerCoordinate;
    const float *decode = cs->params->Decode;
    int code = 0;
    int i;

    for (i = 0; i < num_points; ++i) {
        float x, y;

        if ((code = cs->get_decoded(cs, num_bits, decode, &x)) < 0 ||
            (code = cs->get_decoded(cs, num_bits, decode + 2, &y)) < 0 ||
            (code = gs_point_transform2fixed(cs->pctm, x, y, &ppt[i])) < 0
            )
            break;
    }
    return code;
}

/* Get a color.  Currently all this does is look up Indexed colors. */
int
shade_next_color(shade_coord_stream_t * cs, float *pc)
{
    const float *decode = cs->params->Decode + 4;	/* skip coord decode */
    const gs_color_space *pcs = cs->params->ColorSpace;
    gs_color_space_index index = gs_color_space_get_index(pcs);
    int num_bits = cs->params->BitsPerComponent;

    if (index == gs_color_space_index_Indexed) {
        int ncomp = gs_color_space_num_components(gs_cspace_base_space(pcs));
        int ci;
        float cf;
        int code = cs->get_decoded(cs, num_bits, decode, &cf);
        gs_client_color cc;
        int i;

        if (code < 0)
            return code;
        ci = (int)cf;
        if (ci < 0 || ci >= gs_cspace_indexed_num_entries(pcs))
            return_error(gs_error_rangecheck);
        code = gs_cspace_indexed_lookup(pcs, ci, &cc);
        if (code < 0)
            return code;
        for (i = 0; i < ncomp; ++i)
            pc[i] = cc.paint.values[i];
    } else {
        int i, code;
        int ncomp = (cs->params->Function != 0 ? 1 :
                     gs_color_space_num_components(pcs));

        for (i = 0; i < ncomp; ++i) {
            if ((code = cs->get_decoded(cs, num_bits, decode + i * 2, &pc[i])) < 0)
                return code;
            if (cs->params->Function) {
                gs_function_params_t *params = &cs->params->Function->params;

                if (pc[i] < params->Domain[i + i])
                    pc[i] = params->Domain[i + i];
                else if (pc[i] > params->Domain[i + i + 1])
                    pc[i] = params->Domain[i + i + 1];
            }
        }
    }
    return 0;
}

/* Get the next vertex for a mesh element. */
int
shade_next_vertex(shade_coord_stream_t * cs, shading_vertex_t * vertex, patch_color_t *c)
{   /* Assuming p->c == c, provides a non-const access. */
    int code = shade_next_coords(cs, &vertex->p, 1);

    if (code >= 0)
        code = shade_next_color(cs, c->cc.paint.values);
    if (code >= 0)
        cs->align(cs, 8); /* CET 09-47J.PS SpecialTestI04Test01. */
    return code;
}
/* ======== END base/gxshade.c ======== */

/* ======== BEGIN base/gxshade6.c lines 140-217 ======== */
/* Get colors for patch vertices. */
static int
shade_next_colors(shade_coord_stream_t * cs, patch_curve_t * curves,
                  int num_vertices)
{
    int i, code = 0;

    for (i = 0; i < num_vertices && code >= 0; ++i) {
        curves[i].vertex.cc[1] = 0; /* safety. (patch_fill may assume 2 arguments) */
        code = shade_next_color(cs, curves[i].vertex.cc);
    }
    return code;
}

/* Get a Bezier or tensor patch element. */
static int
shade_next_curve(shade_coord_stream_t * cs, patch_curve_t * curve)
{
    int code = shade_next_coords(cs, &curve->vertex.p, 1);

    if (code >= 0)
        code = shade_next_coords(cs, curve->control,
                                 countof(curve->control));
    return code;
}

/*
 * Parse the next patch out of the input stream.  Return 1 if done,
 * 0 if patch, <0 on error.
 */
static int
shade_next_patch(shade_coord_stream_t * cs, int BitsPerFlag,
        patch_curve_t curve[4], gs_fixed_point interior[4] /* 0 for Coons patch */)
{
    int flag = shade_next_flag(cs, BitsPerFlag);
    int num_colors, code;

    if (flag < 0) {
        if (!cs->is_eod(cs))
            return_error(gs_error_rangecheck);
        return 1;               /* no more data */
    }
    if (cs->first_patch && (flag & 3) != 0) {
        return_error(gs_error_rangecheck);
    }
    cs->first_patch = 0;
    switch (flag & 3) {
        default:
            return_error(gs_error_rangecheck);  /* not possible */
        case 0:
            if ((code = shade_next_curve(cs, &curve[0])) < 0 ||
                (code = shade_next_coords(cs, &curve[1].vertex.p, 1)) < 0
                )
                return code;
            num_colors = 4;
            goto vx;
        case 1:
            curve[0] = curve[1], curve[1].vertex = curve[2].vertex;
            goto v3;
        case 2:
            curve[0] = curve[2], curve[1].vertex = curve[3].vertex;
            goto v3;
        case 3:
            curve[1].vertex = curve[0].vertex, curve[0] = curve[3];
v3:         num_colors = 2;
vx:         if ((code = shade_next_coords(cs, curve[1].control, 2)) < 0 ||
                (code = shade_next_curve(cs, &curve[2])) < 0 ||
                (code = shade_next_curve(cs, &curve[3])) < 0 ||
                (interior != 0 &&
                 (code = shade_next_coords(cs, interior, 4)) < 0) ||
                (code = shade_next_colors(cs, &curve[4 - num_colors],
                                          num_colors)) < 0
                )
                return code;
            cs->align(cs, 8); /* See shade_next_vertex. */
    }
    return 0;
}
/* ======== END base/gxshade6.c ======== */

/* ======== BEGIN base/gxshade6.c lines 441-477 ======== */
int
gs_shading_Cp_fill_rectangle(const gs_shading_t * psh0, const gs_rect * rect,
                             const gs_fixed_rect * rect_clip,
                             gx_device * dev, gs_gstate * pgs)
{
    const gs_shading_Cp_t * const psh = (const gs_shading_Cp_t *)psh0;
    patch_fill_state_t state;
    shade_coord_stream_t cs;
    patch_curve_t curve[4];
    int code;

    code = mesh_init_fill_state((mesh_fill_state_t *) &state,
                         (const gs_shading_mesh_t *)psh0, rect_clip, dev, pgs);
    if (code < 0) {
        if (state.icclink != NULL) gsicc_release_link(state.icclink);
        return code;
    }
    state.Function = psh->params.Function;
    code = init_patch_fill_state(&state);
    if(code < 0) {
        if (state.icclink != NULL) gsicc_release_link(state.icclink);
        return code;
    }

    curve[0].straight = curve[1].straight = curve[2].straight = curve[3].straight = false;
    shade_next_init(&cs, (const gs_shading_mesh_params_t *)&psh->params, pgs);
    while ((code = shade_next_patch(&cs, psh->params.BitsPerFlag,
                                    curve, NULL)) == 0 &&
           (code = patch_fill(&state, curve, NULL, Cp_transform)) >= 0
        ) {
        DO_NOTHING;
    }
    if (term_patch_fill_state(&state))
        return_error(gs_error_unregistered); /* Must not happen. */
    if (state.icclink != NULL) gsicc_release_link(state.icclink);
    return min(code, 0);
}
/* ======== END base/gxshade6.c ======== */

/* ======== BEGIN base/gxshade6.c lines 530-574 ======== */
int
gs_shading_Tpp_fill_rectangle(const gs_shading_t * psh0, const gs_rect * rect,
                             const gs_fixed_rect * rect_clip,
                              gx_device * dev, gs_gstate * pgs)
{
    const gs_shading_Tpp_t * const psh = (const gs_shading_Tpp_t *)psh0;
    patch_fill_state_t state;
    shade_coord_stream_t cs;
    patch_curve_t curve[4];
    gs_fixed_point interior[4];
    int code;

    code = mesh_init_fill_state((mesh_fill_state_t *) & state,
                         (const gs_shading_mesh_t *)psh0, rect_clip, dev, pgs);
    if (code < 0) {
        if (state.icclink != NULL) gsicc_release_link(state.icclink);
        return code;
    }
    state.Function = psh->params.Function;
    code = init_patch_fill_state(&state);
    if(code < 0)
        return code;
    curve[0].straight = curve[1].straight = curve[2].straight = curve[3].straight = false;
    shade_next_init(&cs, (const gs_shading_mesh_params_t *)&psh->params, pgs);
    while ((code = shade_next_patch(&cs, psh->params.BitsPerFlag,
                                    curve, interior)) == 0) {
        /*
         * The order of points appears to be consistent with that for Coons
         * patches, which is different from that documented in Red Book 3.
         */
        gs_fixed_point swapped_interior[4];

        swapped_interior[0] = interior[0];
        swapped_interior[1] = interior[3];
        swapped_interior[2] = interior[2];
        swapped_interior[3] = interior[1];
        code = patch_fill(&state, curve, swapped_interior, Tpp_transform);
        if (code < 0)
            break;
    }
    if (term_patch_fill_state(&state))
        return_error(gs_error_unregistered); /* Must not happen. */
    if (state.icclink != NULL) gsicc_release_link(state.icclink);
    return min(code, 0);
}
/* ======== END base/gxshade6.c ======== */

/* ======== BEGIN base/gxshade4.c lines 34-254 ======== */
/* Initialize the fill state for triangle shading. */
int
mesh_init_fill_state(mesh_fill_state_t * pfs, const gs_shading_mesh_t * psh,
                     const gs_fixed_rect * rect_clip, gx_device * dev,
                     gs_gstate * pgs)
{
    int code;
    code = shade_init_fill_state((shading_fill_state_t *) pfs,
                                 (const gs_shading_t *)psh, dev, pgs);
    if (code < 0)
        return code;
    pfs->pshm = psh;
    pfs->rect = *rect_clip;
    return 0;
}

/* ---------------- Gouraud triangle shadings ---------------- */

static int
Gt_next_vertex(const gs_shading_mesh_t * psh, shade_coord_stream_t * cs,
               shading_vertex_t * vertex, patch_color_t *c)
{
    int code = shade_next_vertex(cs, vertex, c);
    if (code < 0)
        return code;

    if (psh->params.Function) {
        c->t[0] = c->cc.paint.values[0];
        c->t[1] = 0;
        /* Decode the color with the function. */
        code = gs_function_evaluate(psh->params.Function, c->t,
                                    c->cc.paint.values);
    } else
        psh->params.ColorSpace->type->restrict_color(&c->cc, psh->params.ColorSpace);
    return code;
}

static inline int
Gt_fill_triangle(patch_fill_state_t * pfs, const shading_vertex_t * va,
                 const shading_vertex_t * vb, const shading_vertex_t * vc)
{
    int code = 0;

    if (INTERPATCH_PADDING) {
        code = mesh_padding(pfs, &va->p, &vb->p, va->c, vb->c);
        if (code >= 0)
            code = mesh_padding(pfs, &vb->p, &vc->p, vb->c, vc->c);
        if (code >= 0)
            code = mesh_padding(pfs, &vc->p, &va->p, vc->c, va->c);
    }
    if (code >= 0)
        code = mesh_triangle(pfs, va, vb, vc);
    return code;
}

int
gs_shading_FfGt_fill_rectangle(const gs_shading_t * psh0, const gs_rect * rect,
                               const gs_fixed_rect * rect_clip,
                               gx_device * dev, gs_gstate * pgs)
{
    const gs_shading_FfGt_t * const psh = (const gs_shading_FfGt_t *)psh0;
    patch_fill_state_t pfs;
    const gs_shading_mesh_t *pshm = (const gs_shading_mesh_t *)psh;
    shade_coord_stream_t cs;
    int num_bits = psh->params.BitsPerFlag;
    int flag;
    shading_vertex_t va, vb, vc;
    patch_color_t *c, *C[3], *ca, *cb, *cc; /* va.c == ca && vb.c == cb && vc.c == cc always,
                                        provides a non-const access. */
    int code;

    code = shade_init_fill_state((shading_fill_state_t *)&pfs,
                                 (const gs_shading_t *)psh, dev, pgs);
    if (code < 0)
        return code;
    pfs.Function = pshm->params.Function;
    pfs.rect = *rect_clip;
    code = init_patch_fill_state(&pfs);
    if (code < 0) {
        if (pfs.icclink != NULL) gsicc_release_link(pfs.icclink);
        return code;
    }
    reserve_colors(&pfs, C, 3); /* Can't fail */
    va.c = ca = C[0];
    vb.c = cb = C[1];
    vc.c = cc = C[2];
    shade_next_init(&cs, (const gs_shading_mesh_params_t *)&psh->params,
                    pgs);
    /* CET 09-47J.PS SpecialTestI04Test01 does not need the color data alignment. */
    while ((flag = shade_next_flag(&cs, num_bits)) >= 0) {
        switch (flag) {
            default:
                code = gs_note_error(gs_error_rangecheck);
                goto error;
            case 0:
                if ((code = Gt_next_vertex(pshm, &cs, &va, ca)) < 0 ||
                    (code = shade_next_flag(&cs, num_bits)) < 0 ||
                    (code = Gt_next_vertex(pshm, &cs, &vb, cb)) < 0 ||
                    (code = shade_next_flag(&cs, num_bits)) < 0
                    )
                    break;
                goto v2;
            case 1:
                c = ca;
                va = vb;
                ca = cb;
                vb.c = cb = c;
                /* fall through */
            case 2:
                c = cb;
                vb = vc;
                cb = cc;
                vc.c = cc = c;
v2:		if ((code = Gt_next_vertex(pshm, &cs, &vc, cc)) < 0)
                    break;
                if ((code = Gt_fill_triangle(&pfs, &va, &vb, &vc)) < 0)
                    break;
        }
        cs.align(&cs, 8); /* Debugged with 12-14O.PS page 2. */
    }
error:
    release_colors(&pfs, pfs.color_stack, 3);
    if (pfs.icclink != NULL) gsicc_release_link(pfs.icclink);
    if (term_patch_fill_state(&pfs))
        return_error(gs_error_unregistered); /* Must not happen. */
    if (!cs.is_eod(&cs))
        return_error(gs_error_rangecheck);
    return code;
}

int
gs_shading_LfGt_fill_rectangle(const gs_shading_t * psh0, const gs_rect * rect,
                               const gs_fixed_rect * rect_clip,
                               gx_device * dev, gs_gstate * pgs)
{
    const gs_shading_LfGt_t * const psh = (const gs_shading_LfGt_t *)psh0;
    patch_fill_state_t pfs;
    const gs_shading_mesh_t *pshm = (const gs_shading_mesh_t *)psh;
    shade_coord_stream_t cs;
    shading_vertex_t *vertex = NULL;
    byte *color_buffer = NULL;
    patch_color_t **color_buffer_ptrs = NULL; /* non-const access to vertex[i].c */
    shading_vertex_t next;
    int per_row = psh->params.VerticesPerRow;
    patch_color_t *c, *cn; /* cn == next.c always, provides a non-contst access. */
    int i, code;

    code = shade_init_fill_state((shading_fill_state_t *)&pfs,
                                 (const gs_shading_t *)psh, dev, pgs);
    if (code < 0)
        return code;
    pfs.Function = pshm->params.Function;
    pfs.rect = *rect_clip;
    code = init_patch_fill_state(&pfs);
    if (code < 0)
        goto out;
    reserve_colors(&pfs, &cn, 1); /* Can't fail. */
    next.c = cn;
    shade_next_init(&cs, (const gs_shading_mesh_params_t *)&psh->params,
                    pgs);
    vertex = (shading_vertex_t *)
        gs_alloc_byte_array(pgs->memory, per_row, sizeof(*vertex),
                            "gs_shading_LfGt_render");
    if (vertex == NULL) {
        code = gs_note_error(gs_error_VMerror);
        goto out;
    }
    color_buffer = gs_alloc_bytes(pgs->memory,
                                  (size_t)pfs.color_stack_step * (size_t)per_row,
                                  "gs_shading_LfGt_fill_rectangle");
    if (color_buffer == NULL) {
        code = gs_note_error(gs_error_VMerror);
        goto out;
    }
    color_buffer_ptrs = (patch_color_t **)gs_alloc_bytes(pgs->memory,
                            sizeof(patch_color_t *) * (size_t)per_row, "gs_shading_LfGt_fill_rectangle");
    if (color_buffer_ptrs == NULL) {
        code = gs_note_error(gs_error_VMerror);
        goto out;
    }
    /* CET 09-47K.PS SpecialTestJ02Test05 needs the color data alignment. */
    for (i = 0; i < per_row; ++i) {
        color_buffer_ptrs[i] = (patch_color_t *)(color_buffer + pfs.color_stack_step * i);
        vertex[i].c = color_buffer_ptrs[i];
        if ((code = Gt_next_vertex(pshm, &cs, &vertex[i], color_buffer_ptrs[i])) < 0)
            goto out;
    }
    while (!seofp(cs.s)) {
        code = Gt_next_vertex(pshm, &cs, &next, cn);
        if (code < 0)
            goto out;
        for (i = 1; i < per_row; ++i) {
            code = Gt_fill_triangle(&pfs, &vertex[i - 1], &vertex[i], &next);
            if (code < 0)
                goto out;
            c = color_buffer_ptrs[i - 1];
            vertex[i - 1] = next;
            color_buffer_ptrs[i - 1] = cn;
            next.c = cn = c;
            code = Gt_next_vertex(pshm, &cs, &next, cn);
            if (code < 0)
                goto out;
            code = Gt_fill_triangle(&pfs, &vertex[i], &vertex[i - 1], &next);
            if (code < 0)
                goto out;
        }
        c = color_buffer_ptrs[per_row - 1];
        vertex[per_row - 1] = next;
        color_buffer_ptrs[per_row - 1] = cn;
        next.c = cn = c;
    }
out:
    gs_free_object(pgs->memory, vertex, "gs_shading_LfGt_render");
    gs_free_object(pgs->memory, color_buffer, "gs_shading_LfGt_render");
    gs_free_object(pgs->memory, color_buffer_ptrs, "gs_shading_LfGt_render");
    release_colors(&pfs, pfs.color_stack, 1);
    if (pfs.icclink != NULL) gsicc_release_link(pfs.icclink);
    if (term_patch_fill_state(&pfs))
        return_error(gs_error_unregistered); /* Must not happen. */
    return code;
}
/* ======== END base/gxshade4.c ======== */


}  // namespace

// pdfking: gs_shading_do_fill_rectangle (gsshade.c) for a clip box, with
// the device and gstate the shim stands for.
int Fill(const Shading& shading,
         const Colors& colors,
         const Device& device,
         const Matrix& ctm,
         int32_t clip_x0,
         int32_t clip_y0,
         int32_t clip_x1,
         int32_t clip_y1,
         float smoothness,
         float flatness) {
  gs_color_space_type type = {};
  type.num_components = shade_num_components;
  type.restrict_color = shade_restrict_color;
  type.remap_color = shade_remap_color;
  type.is_linear = device.linearity == Device::Linearity::kDefault
                       ? gx_cspace_is_linear_default
                       : gx_cspace_is_linear_ICC;
  gs_color_space base = {&type, &colors, &device, nullptr, &shading};
  // an Indexed space over `base`
  gs_color_space indexed = {&type, &colors, &device, &base, &shading};
  const gs_color_space& cs = shading.hival >= 0 ? indexed : base;
  gs_function_t fn = {&colors, {{shading.function_domain[0],
                                 shading.function_domain[1]}}};

  gx_device dev = {};
  dev.color_info.num_components = static_cast<uchar>(device.num_components);
  dev.color_info.max_gray = 255;
  dev.color_info.max_color = 255;
  for (int i = 0; i < device.num_components; ++i) {
    dev.color_info.comp_bits[i] = 8;
    dev.color_info.comp_shift[i] =
        static_cast<uchar>(8 * (device.num_components - 1 - i));
  }
  dev.color_info.gray_index = GX_CINFO_COMP_NO_INDEX;
  // the page's bounds gx_default_fill_trapezoid clamps to: the caller's
  // buffer clips instead
  dev.width = max_int;
  dev.height = max_int;
  dev.fill_trapezoid = gx_default_fill_trapezoid;
  // gx_default_fill_linear_color_scanline goes to this one on a device
  // that supports devn colors, as tiffsep does
  dev.fill_linear_color_scanline = gx_hl_fill_linear_color_scanline;
  dev.fill_linear_color_trapezoid = gx_default_fill_linear_color_trapezoid;
  dev.fill_linear_color_triangle = gx_default_fill_linear_color_triangle;
  dev.fill_rectangle_hl_color = sink_fill_rectangle_hl_color;
  dev.sink = &device;
  dev.space = &base;

  gs_gstate gstate = {};
  gs_matrix m = {ctm.xx, ctm.xy, ctm.yx, ctm.yy, ctm.tx, ctm.ty};
  gs_matrix_fixed_from_matrix(&gstate.ctm, &m);
  gstate.smoothness = smoothness;
  gstate.flatness = flatness;

  gs_shading_t psh = {};
  psh.head.type = shading.type;
  psh.params.ColorSpace = &cs;
  memcpy(psh.params.Domain, shading.domain, sizeof(psh.params.Domain));
  psh.params.Matrix = {shading.matrix.xx, shading.matrix.xy,
                       shading.matrix.yx, shading.matrix.yy,
                       shading.matrix.tx, shading.matrix.ty};
  psh.params.Function = colors.function ? &fn : nullptr;
  memcpy(psh.params.Coords, shading.coords, sizeof(psh.params.Coords));
  psh.params.Extend[0] = shading.extend[0];
  psh.params.Extend[1] = shading.extend[1];
  psh.params.BitsPerCoordinate = shading.bits_per_coordinate;
  psh.params.BitsPerComponent = shading.bits_per_component;
  psh.params.BitsPerFlag = shading.bits_per_flag;
  psh.params.Decode = shading.decode.data();
  psh.params.VerticesPerRow = shading.vertices_per_row;
  psh.params.DataSource = &shading;

  gs_fixed_rect path_box = {{clip_x0, clip_y0}, {clip_x1, clip_y1}};
  gs_rect path_rect = {{fixed2float(path_box.p.x), fixed2float(path_box.p.y)},
                       {fixed2float(path_box.q.x), fixed2float(path_box.q.y)}};
  gs_rect rect;
  int code = gs_bbox_transform_inverse(&path_rect, &ctm_only(&gstate), &rect);
  if (code < 0) {
    return code;
  }
  switch (shading.type) {
    case 1:
      return gs_shading_Fb_fill_rectangle(&psh, &rect, &path_box, &dev,
                                          &gstate);
    case 2:
      return gs_shading_A_fill_rectangle(&psh, &rect, &path_box, &dev,
                                         &gstate);
    case 3:
      return gs_shading_R_fill_rectangle(&psh, &rect, &path_box, &dev,
                                         &gstate);
    case 4:
      return gs_shading_FfGt_fill_rectangle(&psh, &rect, &path_box, &dev,
                                            &gstate);
    case 5:
      return gs_shading_LfGt_fill_rectangle(&psh, &rect, &path_box, &dev,
                                            &gstate);
    case 6:
      return gs_shading_Cp_fill_rectangle(&psh, &rect, &path_box, &dev,
                                          &gstate);
    case 7:
      return gs_shading_Tpp_fill_rectangle(&psh, &rect, &path_box, &dev,
                                           &gstate);
    default:
      return gs_error_rangecheck;
  }
}

}  // namespace gs_shade
