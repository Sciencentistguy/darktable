/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

// libFuzzer harness for the Lightroom import in src/develop/lightroom.c: the
// XMP parsing into lr_data_t, and the conversions from it to crop, ashift,
// exposure, tone curve, vignette and retouch params and masks. The file is
// included so the static helpers are reachable; everything else it
// references resolves against libdarktable but is never called. Nothing
// touches the database: the parser runs with a develop, which skips the
// metadata, tag and rating branches that write to it.

// LIGHTROOM_C is src/develop/lightroom.c, or a deliberately broken copy of
// it from mutate.sh
#ifndef LIGHTROOM_C
#error "build with build.sh, which defines LIGHTROOM_C"
#endif
#include LIGHTROOM_C

#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct reader_t
{
  const uint8_t *p;
  size_t n;
} reader_t;

static uint8_t rd_u8(reader_t *r)
{
  if(!r->n) return 0;
  r->n--;
  return *r->p++;
}

static uint32_t rd_u32(reader_t *r)
{
  uint32_t v = 0;
  for(int k = 0; k < 4; k++) v = (v << 8) | rd_u8(r);
  return v;
}

static double rd_unit(reader_t *r)
{
  return rd_u32(r) / (double)UINT32_MAX;
}

static float rd_float_raw(reader_t *r)
{
  const uint32_t v = rd_u32(r);
  float f;
  memcpy(&f, &v, sizeof(f));
  return f;
}

#define CHECK(cond, ...)                                                       \
  do                                                                           \
  {                                                                            \
    if(!(cond))                                                                \
    {                                                                          \
      fprintf(stderr, "CHECK failed: %s\n  ", #cond);                          \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
      abort();                                                                 \
    }                                                                          \
  } while(0)

// ---------------------------------------------------------------------------
// crop oracle
//
// rotation-only ashift homography, built from the same matrices as
// _homography() in src/iop/ashift.c: step 1 swaps x/y, step 2 rotates about
// the center, shear/lens shift/aspect are identity at their neutral values,
// the last steps swap back and translate to the bounding box origin.

typedef struct affine_t
{
  double a, b, c, d; // linear part, out = [a b; c d] * in + t
  double tx, ty;
} affine_t;

static affine_t ashift_rotation(const double angle_deg, const double u, const double v)
{
  const double phi = angle_deg * M_PI / 180.0;
  const double cosi = cos(phi), sini = sin(phi);

  // step 2 acts on (y : x : 1)
  const double m00 = cosi, m01 = -sini, m02 = -0.5 * v * cosi + 0.5 * u * sini + 0.5 * v;
  const double m10 = sini, m11 = cosi, m12 = -0.5 * v * sini - 0.5 * u * cosi + 0.5 * u;

  // out_y = m00 * y + m01 * x + m02, out_x = m10 * y + m11 * x + m12
  return (affine_t){ .a = m11, .b = m10, .c = m01, .d = m00, .tx = m12, .ty = m02 };
}

static void affine_apply(const affine_t *m, const double x, const double y, double *ox, double *oy)
{
  *ox = m->a * x + m->b * y + m->tx;
  *oy = m->c * x + m->d * y + m->ty;
}

static void affine_invert_apply(const affine_t *m, const double ox, const double oy, double *x, double *y)
{
  const double det = m->a * m->d - m->b * m->c;
  const double px = ox - m->tx, py = oy - m->ty;
  *x = (m->d * px - m->b * py) / det;
  *y = (-m->c * px + m->a * py) / det;
}

// where a normalized sensor point lands in the displayed image, per EXIF
// orientation, independent of darktable's flip bit encoding
static void exif_display(const int orientation, const double x, const double y, double *dx, double *dy)
{
  switch(orientation)
  {
    case 2: *dx = 1 - x; *dy = y; break;
    case 3: *dx = 1 - x; *dy = 1 - y; break;
    case 4: *dx = x; *dy = 1 - y; break;
    case 5: *dx = y; *dy = x; break;
    case 6: *dx = 1 - y; *dy = x; break;
    case 7: *dx = 1 - y; *dy = 1 - x; break;
    case 8: *dx = y; *dy = 1 - x; break;
    default: *dx = x; *dy = y; break;
  }
}

static void fuzz_crop_oracle(reader_t *r)
{
  const int W = 256 + rd_u32(r) % 16000;
  const int H = 256 + rd_u32(r) % 16000;
  const double angle = rd_unit(r) * 90.0 - 45.0;
  const int orientation = 1 + rd_u8(r) % 8;

  // the crop wanted in the rotated image, normalized to its bounding box
  double u0 = rd_unit(r), u1 = rd_unit(r), v0 = rd_unit(r), v1 = rd_unit(r);
  if(u0 > u1) { const double t = u0; u0 = u1; u1 = t; }
  if(v0 > v1) { const double t = v0; v0 = v1; v1 = t; }
  if(u1 - u0 < 1e-3 || v1 - v0 < 1e-3) return;

  const affine_t m = ashift_rotation(angle, W, H);
  double xm = INFINITY, xM = -INFINITY, ym = INFINITY, yM = -INFINITY;
  const double corners[4][2] = { { 0, 0 }, { W, 0 }, { 0, H }, { W, H } };
  for(int k = 0; k < 4; k++)
  {
    double ox, oy;
    affine_apply(&m, corners[k][0], corners[k][1], &ox, &oy);
    xm = fmin(xm, ox); xM = fmax(xM, ox);
    ym = fmin(ym, oy); yM = fmax(yM, oy);
  }

  // Lightroom stores the crop's top-left and bottom-right corners in
  // normalized sensor coordinates
  double l, t, rr, b;
  affine_invert_apply(&m, xm + u0 * (xM - xm), ym + v0 * (yM - ym), &l, &t);
  affine_invert_apply(&m, xm + u1 * (xM - xm), ym + v1 * (yM - ym), &rr, &b);

  lr_data_t data;
  memset(&data, 0, sizeof(data));
  data.pcr.cx = l / W;
  data.pcr.cy = t / H;
  data.pcr.cw = rr / W;
  data.pcr.ch = b / H;
  data.pas.rotation = angle;
  data.iwidth = W;
  data.iheight = H;
  data.orientation = orientation;

  _lr_crop_to_dt(&data);

  double ex0, ey0, ex1, ey1;
  exif_display(orientation, u0, v0, &ex0, &ey0);
  exif_display(orientation, u1, v1, &ex1, &ey1);
  const double ecx = fmin(ex0, ex1), ecw = fmax(ex0, ex1);
  const double ecy = fmin(ey0, ey1), ech = fmax(ey0, ey1);

  // float inputs, round5() and ashift's +1 pixel bounding box allow a
  // couple of pixels of slack
  const double tol = 1e-4 + 2.0 / fmin(W, H);
  CHECK(fabs(data.pcr.cx - ecx) < tol && fabs(data.pcr.cw - ecw) < tol
        && fabs(data.pcr.cy - ecy) < tol && fabs(data.pcr.ch - ech) < tol,
        "W=%d H=%d angle=%.6f orient=%d lr=(%.6f %.6f %.6f %.6f)\n"
        "  got      (%.6f %.6f %.6f %.6f)\n  expected (%.6f %.6f %.6f %.6f)",
        W, H, angle, orientation, l / W, t / H, rr / W, b / H,
        data.pcr.cx, data.pcr.cy, data.pcr.cw, data.pcr.ch, ecx, ecy, ecw, ech);

  CHECK(data.pcr.ratio_n == 0 && data.pcr.ratio_d == 0, "crop ratio not freehand");
  // the frame in pixels, upright, as the vignette sees it
  double fw = (u1 - u0) * (xM - xm), fh = (v1 - v0) * (yM - ym);
  if(orientation >= 5) { const double t = fw; fw = fh; fh = t; }
  if(fmin(fw, fh) >= 8.0)
    CHECK(fabs(data.fratio - fw / fh) <= (1e-4 + 0.01 / fmin(fw, fh)) * fw / fh,
          "W=%d H=%d angle=%.6f orient=%d: frame ratio %f, expected %f", W, H, angle, orientation, data.fratio,
          fw / fh);

  CHECK(data.pas.rotation == (float)angle, "ashift rotation %f != %f", data.pas.rotation, angle);
  if(data.pas.rotation != 0.0f)
    CHECK(data.pas.cropmode == ASHIFT_CROP_OFF && data.pas.mode == ASHIFT_MODE_GENERIC
          && data.pas.cl == 0.0f && data.pas.ct == 0.0f && data.pas.cr == 1.0f && data.pas.cb == 1.0f
          && data.pas.aspect == 1.0f && data.pas.orthocorr == 100.0f && data.pas.f_length > 0.0f
          && data.pas.lensshift_v == 0.0f && data.pas.lensshift_h == 0.0f && data.pas.shear == 0.0f,
          "ashift params not neutral apart from rotation");
}

// ---------------------------------------------------------------------------
// crop robustness: anything the XMP parser could hand over

static void fuzz_crop_raw(reader_t *r)
{
  lr_data_t data;
  memset(&data, 0, sizeof(data));
  data.pcr.cx = rd_float_raw(r);
  data.pcr.cy = rd_float_raw(r);
  data.pcr.cw = rd_float_raw(r);
  data.pcr.ch = rd_float_raw(r);
  data.pas.rotation = rd_float_raw(r);
  data.iwidth = (int32_t)rd_u32(r);
  data.iheight = (int32_t)rd_u32(r);
  data.orientation = (int32_t)rd_u32(r);

  const gboolean sane = isfinite(data.pcr.cx) && isfinite(data.pcr.cy) && isfinite(data.pcr.cw)
                        && isfinite(data.pcr.ch) && fabsf(data.pas.rotation) <= 45.0f
                        && fabsf(data.pcr.cx) < 1e6f && fabsf(data.pcr.cy) < 1e6f
                        && fabsf(data.pcr.cw) < 1e6f && fabsf(data.pcr.ch) < 1e6f
                        && data.iwidth > 0 && data.iheight > 0;

  _lr_crop_to_dt(&data);

  if(sane)
    CHECK(isfinite(data.pcr.cx) && isfinite(data.pcr.cy) && isfinite(data.pcr.cw) && isfinite(data.pcr.ch),
          "non-finite crop (%f %f %f %f) from finite input", data.pcr.cx, data.pcr.cy, data.pcr.cw,
          data.pcr.ch);
}

// ---------------------------------------------------------------------------
// tone curve

static void fuzz_tonecurve(reader_t *r)
{
  lr_data_t data;
  memset(&data, 0, sizeof(data));
  const uint8_t flags = rd_u8(r);
  data.n_pts = rd_u8(r) % (MAX_PTS + 1);
  for(int k = 0; k < MAX_PTS; k++)
    for(int c = 0; c < 2; c++)
      data.curve_pts[k][c] = (flags & 1) ? (int32_t)rd_u32(r) : rd_u8(r);
  // most inputs leave some regions at 0, as edits in Lightroom do
  const uint8_t zero = rd_u8(r);
  for(int k = 0; k < 4; k++)
    data.ptc_value[k] = (flags & 2) ? (int32_t)rd_u32(r)
                        : (zero & (1 << k)) ? 0 : (int)(rd_u8(r) % 201) - 100;
  for(int k = 0; k < 3; k++)
    // Lightroom writes whole percentages, which the parser divides by 100
    data.ptc_split[k] = (flags & 4) ? rd_float_raw(r) : (1 + rd_u8(r) % 99) / 100.0;

  const lr_data_t in = data;
  const gboolean add = _lr_tonecurve_to_dt(&data);
  const dt_iop_tonecurve_params_v5_t *p = &data.ptc;
  const int n = p->tonecurve_nodes[ch_L];

  const gboolean parametric = in.ptc_value[0] != 0 || in.ptc_value[1] != 0
                              || in.ptc_value[2] != 0 || in.ptc_value[3] != 0;
  gboolean diagonal = TRUE;
  for(int k = 0; k < in.n_pts; k++)
    if(in.curve_pts[k][0] != in.curve_pts[k][1]) diagonal = FALSE;
  if(!parametric && diagonal)
    CHECK(!add, "identity curve with %d points and no parametric adjustments was added", in.n_pts);

  // what Lightroom itself writes: point x strictly increasing within
  // 0-255, region values within +-100 and splits strictly increasing
  gboolean lr_points = !(flags & 1);
  for(int k = 1; k < in.n_pts; k++)
    if(in.curve_pts[k][0] <= in.curve_pts[k - 1][0]) lr_points = FALSE;
  const gboolean lr_parametric = !(flags & 2) && !(flags & 4) && in.ptc_split[0] < in.ptc_split[1]
                                 && in.ptc_split[1] < in.ptc_split[2];

  if(!add) return;

  CHECK(n >= 2 && n <= DT_IOP_TONECURVE_MAXNODES, "added curve has %d nodes", n);
  gboolean off_diagonal = FALSE;
  for(int k = 0; k < n; k++)
    if(fabsf(p->tonecurve[ch_L][k].x - p->tonecurve[ch_L][k].y) > 1e-4f) off_diagonal = TRUE;
  CHECK(off_diagonal, "added curve lies on the diagonal");
  CHECK(p->tonecurve_autoscale_ab == LRDT_TONECURVE_AUTOMATIC_RGB
        && p->preserve_colors == LRDT_RGB_NORM_AVERAGE && p->tonecurve_unbound_ab == 1
        && p->tonecurve_nodes[ch_a] == 7 && p->tonecurve_nodes[ch_b] == 7,
        "tonecurve params not at current defaults");

  if(!parametric)
  {
    // the point curve alone becomes the nodes as they are
    CHECK(n == in.n_pts, "point curve of %d points gave %d nodes", in.n_pts, n);
    for(int k = 0; k < n; k++)
      CHECK(p->tonecurve[ch_L][k].x == in.curve_pts[k][0] / 255.0f
            && p->tonecurve[ch_L][k].y == in.curve_pts[k][1] / 255.0f,
            "node %d (%f, %f) is not point (%d, %d)", k, p->tonecurve[ch_L][k].x,
            p->tonecurve[ch_L][k].y, in.curve_pts[k][0], in.curve_pts[k][1]);
    if(lr_points)
      for(int k = 1; k < n; k++)
        CHECK(p->tonecurve[ch_L][k].x > p->tonecurve[ch_L][k - 1].x,
              "node %d x=%f not above node %d x=%f", k, p->tonecurve[ch_L][k].x, k - 1,
              p->tonecurve[ch_L][k - 1].x);
    return;
  }

  // the parametric curve is sampled at evenly spaced x
  CHECK(n == DT_IOP_TONECURVE_MAXNODES, "parametric curve has %d nodes", n);
  for(int k = 0; k < n; k++)
    CHECK(p->tonecurve[ch_L][k].x == (float)k / (n - 1), "node %d at x=%f", k, p->tonecurve[ch_L][k].x);

  if(!lr_parametric || !(lr_points || in.n_pts < 2)) return;

  for(int k = 0; k < n; k++)
    CHECK(isfinite(p->tonecurve[ch_L][k].y) && p->tonecurve[ch_L][k].y >= 0.0f
          && p->tonecurve[ch_L][k].y <= 1.0f,
          "node %d y=%f outside [0, 1] for Lightroom-like input", k, p->tonecurve[ch_L][k].y);

  if(!diagonal) return;

  // on its own, each region slider only raises the curve when positive and
  // only lowers it when negative, and never makes it decrease
  gboolean all_up = TRUE, all_down = TRUE;
  for(int k = 0; k < 4; k++)
  {
    if(in.ptc_value[k] < 0) all_up = FALSE;
    if(in.ptc_value[k] > 0) all_down = FALSE;
  }
  for(int k = 0; k < n; k++)
  {
    const float x = p->tonecurve[ch_L][k].x, y = p->tonecurve[ch_L][k].y;
    if(all_up)
      CHECK(y >= x - 1e-5f, "values %d %d %d %d all >= 0, splits %g %g %g, %d points from x=%d to %d: lowered x=%f to %f",
            in.ptc_value[0], in.ptc_value[1], in.ptc_value[2], in.ptc_value[3], in.ptc_split[0],
            in.ptc_split[1], in.ptc_split[2], in.n_pts, in.n_pts ? in.curve_pts[0][0] : -1,
            in.n_pts ? in.curve_pts[in.n_pts - 1][0] : -1, x, y);
    if(all_down)
      CHECK(y <= x + 1e-5f, "values %d %d %d %d all <= 0, splits %g %g %g, %d points from x=%d to %d: raised x=%f to %f",
            in.ptc_value[0], in.ptc_value[1], in.ptc_value[2], in.ptc_value[3], in.ptc_split[0],
            in.ptc_split[1], in.ptc_split[2], in.n_pts, in.n_pts ? in.curve_pts[0][0] : -1,
            in.n_pts ? in.curve_pts[in.n_pts - 1][0] : -1, x, y);
    if(k > 0)
      CHECK(y >= p->tonecurve[ch_L][k - 1].y - 1e-5f, "values %d %d %d %d, splits %g %g %g: curve falls from %f to %f at x=%f",
            in.ptc_value[0], in.ptc_value[1], in.ptc_value[2], in.ptc_value[3], in.ptc_split[0],
            in.ptc_split[1], in.ptc_split[2], p->tonecurve[ch_L][k - 1].y, y, x);
  }
}

// ---------------------------------------------------------------------------
// exposure

static void fuzz_exposure(reader_t *r)
{
  lr_data_t data;
  memset(&data, 0, sizeof(data));
  const gboolean have_defaults = rd_u8(r) & 1;
  dt_iop_exposure_params_v7_t defaults;
  for(size_t k = 0; k < sizeof(defaults); k++) ((uint8_t *)&defaults)[k] = rd_u8(r);
  data.pe.exposure = rd_float_raw(r);
  data.pe.black = rd_float_raw(r);

  const dt_iop_exposure_params_v7_t pe = _lr_exposure_to_dt(have_defaults ? &defaults : NULL, &data);

  dt_iop_exposure_params_v7_t base = { .mode = EXPOSURE_MODE_MANUAL,
                                    .deflicker_percentile = 50.0f,
                                    .deflicker_target_level = -4.0f,
                                    .compensate_hilite_pres = TRUE };
  if(have_defaults) base = defaults;
  const float exposure = base.exposure + data.pe.exposure;
  const float black = base.black + data.pe.black;

  // a NaN sum's payload depends on operand order, which the compiler may swap
#define SAME(a, b) ((isnan(a) && isnan(b)) || (a) == (b))
  CHECK(SAME(pe.exposure, exposure) && SAME(pe.black, black), "exposure %g/%g, black %g/%g", pe.exposure,
        exposure, pe.black, black);
  dt_iop_exposure_params_v7_t rest = pe;
  rest.exposure = base.exposure;
  rest.black = base.black;
  CHECK(!memcmp(&rest, &base, sizeof(rest)), "exposure fields other than exposure and black changed");
#undef SAME

  // the tables hold values past their ends at the last entry
  const float v = rd_float_raw(r);
#define IN(x, lo, hi) (isnan(v) || ((x) >= (lo) && (x) <= (hi)))
  CHECK(IN(lr2dt_blacks(v), -0.010f, 0.020f), "blacks %g gave %g", v, lr2dt_blacks(v));
  CHECK(IN(lr2dt_grain_amount(v), 0.0f, 80.0f), "grain amount %g gave %g", v, lr2dt_grain_amount(v));
  CHECK(IN(lr2dt_grain_frequency(v), 100.0f / 53.3f, 800.0f / 53.3f), "grain frequency %g gave %g", v,
        lr2dt_grain_frequency(v));
  CHECK(IN(lr2dt_clarity(v), -0.65f, 0.65f), "clarity %g gave %g", v, lr2dt_clarity(v));
  if(fabsf(v) >= 100.0f)
    CHECK(lr2dt_blacks(v) == (v < 0.0f ? 0.020f : -0.010f) && lr2dt_clarity(v) == (v < 0.0f ? -0.65f : 0.65f),
          "blacks or clarity %g not held at the table's end", v);
#undef IN
}

// ---------------------------------------------------------------------------
// shared by the retouch, vignette and XMP modes

// _lrop() reads only image_storage from the develop it is given
static dt_develop_t fake_dev;

static gboolean near(const double a, const double b)
{
  return fabs(a - b) <= 1e-4 * fmax(1.0, fabs(b));
}

// Lightroom's position, normalized to the raw rawspeed hands darktable,
// in darktable's mask space, normalized to the whole sensor
static double mask_x(const dt_image_t *img, const double x)
{
  return (x * (img->width - img->crop_x - img->crop_right) + img->crop_x) / img->width;
}

static double mask_y(const dt_image_t *img, const double y)
{
  return (y * (img->height - img->crop_y - img->crop_bottom) + img->crop_y) / img->height;
}

// what the parser guarantees of every spot, whatever the XMP said
static void check_spots(const lr_data_t *data)
{
  CHECK(data->n_spots >= 0 && data->n_spots <= RETOUCH_NO_FORMS, "%d spots", data->n_spots);
  for(int k = 0; k < data->n_spots; k++)
  {
    const lr_spot_t *s = &data->spots[k];
    CHECK(!isnan(s->x) && !isnan(s->y) && s->radius > 0.0f, "spot %d at (%f, %f) radius %f", k, s->x, s->y,
          s->radius);
    CHECK((s->dabs == NULL) == (s->n_dabs == 0) && s->n_dabs != 1, "spot %d has %d dabs at %p", k, s->n_dabs,
          (void *)s->dabs);
    CHECK(s->opacity >= 0.0f && s->opacity <= 1.0f && s->feather >= 0.0f && s->feather <= 1.0f,
          "spot %d opacity %f feather %f", k, s->opacity, s->feather);
    if(s->dabs)
    {
      CHECK(s->dabs[0] == s->x && s->dabs[1] == s->y, "spot %d does not start at its first dab", k);
      for(int i = 1; i < s->n_dabs; i++)
        CHECK(s->dabs[2 * i] != s->dabs[2 * i - 2] || s->dabs[2 * i + 1] != s->dabs[2 * i - 1],
              "spot %d repeats dab %d", k, i);
    }
  }
}

// checks the shapes and params _lr_retouch_to_dt() made from data against
// the spots, then frees them. exact: the input is in Lightroom's ranges, so
// the numbers can be checked too
static void check_retouch(const dt_image_t *img, const lr_data_t *data, const dt_iop_retouch_params_v3_t *pr,
                          GList *forms, const gboolean exact)
{
  const double cw = (double)img->width - img->crop_x - img->crop_right;
  const double ch = (double)img->height - img->crop_y - img->crop_bottom;
  if((float)(img->width - img->crop_x - img->crop_right) <= 0.0f
     || (float)(img->height - img->crop_y - img->crop_bottom) <= 0.0f)
  {
    CHECK(!forms, "shapes made for a %fx%f raw", cw, ch);
    return;
  }

  const int n = data->n_spots;
  CHECK((int)g_list_length(forms) == n + 1, "%d shapes for %d spots", g_list_length(forms), n);
  const dt_masks_form_t *grp = g_list_last(forms)->data;
  CHECK(grp->type == (DT_MASKS_GROUP | DT_MASKS_CLONE), "last shape has type %d, not a clone group", grp->type);
  CHECK((int)g_list_length(grp->points) == n, "group has %d members for %d spots", g_list_length(grp->points), n);

  // spot radii are relative to the long side of the raw, darktable's to the
  // short side of the sensor
  const double rscale = fmax(cw, ch) / fmin(img->width, img->height);
  GList *f = forms, *g = grp->points;
  for(int k = 0; k < n; k++, f = g_list_next(f), g = g_list_next(g))
  {
    const lr_spot_t *s = &data->spots[k];
    const dt_masks_form_t *form = f->data;
    const dt_masks_point_group_t *gpt = g->data;

    CHECK(gpt->formid == form->formid && gpt->parentid == grp->formid, "group member %d is not shape %d", k, k);
    CHECK(gpt->state == (DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE) && gpt->opacity == s->opacity
          && gpt->group_opacity == 1.0f,
          "group member %d state %d opacity %f, spot opacity %f", k, gpt->state, gpt->opacity, s->opacity);
    for(GList *o = forms; o != f; o = g_list_next(o))
      CHECK(((dt_masks_form_t *)o->data)->formid != form->formid, "shape %d reuses id %d", k, form->formid);

    const dt_iop_retouch_form_data_v3_t *rf = &pr->rt_forms[k];
    CHECK(rf->formid == form->formid && rf->distort_mode == 2 && rf->blur_radius == pr->blur_radius
          && rf->algorithm == (s->clone ? DT_IOP_RETOUCH_CLONE : DT_IOP_RETOUCH_HEAL),
          "rt_forms[%d] formid %d algorithm %d distort %d", k, rf->formid, rf->algorithm, rf->distort_mode);
    CHECK(form->name[0], "shape %d has no name", k);

    const double radius = s->radius * rscale;
    const double soft = s->feather / 2.0;
    if(s->dabs)
    {
      CHECK(form->type == (DT_MASKS_BRUSH | DT_MASKS_CLONE), "brushed spot %d is type %d", k, form->type);
      CHECK((int)g_list_length(form->points) == s->n_dabs, "brush %d has %d points for %d dabs", k,
            g_list_length(form->points), s->n_dabs);
    }
    else
    {
      CHECK(form->type == (DT_MASKS_CIRCLE | DT_MASKS_CLONE), "circular spot %d is type %d", k, form->type);
      CHECK(g_list_length(form->points) == 1, "circle %d has %d points", k, g_list_length(form->points));
    }
    if(!exact) continue;

    CHECK(near(form->source[0], mask_x(img, s->xc)) && near(form->source[1], mask_y(img, s->yc)),
          "shape %d source (%f, %f), expected (%f, %f)", k, form->source[0], form->source[1],
          mask_x(img, s->xc), mask_y(img, s->yc));
    if(!s->dabs)
    {
      const dt_masks_point_circle_t *c = form->points->data;
      CHECK(near(c->center[0], mask_x(img, s->x)) && near(c->center[1], mask_y(img, s->y)),
            "circle %d at (%f, %f), expected (%f, %f)", k, c->center[0], c->center[1], mask_x(img, s->x),
            mask_y(img, s->y));
      // darktable's border fades outside the radius, Lightroom's feather inside
      CHECK(near(c->radius + c->border, radius) && near(c->border, radius * soft),
            "circle %d radius %f border %f, expected %f in all with %f border", k, c->radius, c->border, radius,
            radius * soft);
      continue;
    }

    // the control points of a Catmull-Rom spline through the corners, with
    // the ends extended in a straight line
    const int last = s->n_dabs - 1;
    int i = 0;
    for(GList *p = form->points; p; p = g_list_next(p), i++)
    {
      const dt_masks_point_brush_t *b = p->data;
      for(int c = 0; c < 2; c++)
      {
        const double corner = c ? mask_y(img, s->dabs[2 * i + 1]) : mask_x(img, s->dabs[2 * i]);
        CHECK(near(b->corner[c], corner), "brush %d point %d corner[%d] %f, expected %f", k, i, c, b->corner[c],
              corner);
        CHECK(near(b->ctrl1[c] + b->ctrl2[c], 2.0 * b->corner[c]), "brush %d point %d controls not mirrored", k, i);
        const double prev = i > 0 ? (c ? mask_y(img, s->dabs[2 * i - 1]) : mask_x(img, s->dabs[2 * i - 2]))
                                  : 2.0 * corner - (c ? mask_y(img, s->dabs[3]) : mask_x(img, s->dabs[2]));
        const double next = i < last ? (c ? mask_y(img, s->dabs[2 * i + 3]) : mask_x(img, s->dabs[2 * i + 2]))
                                     : 2.0 * corner - (c ? mask_y(img, s->dabs[2 * i - 1])
                                                         : mask_x(img, s->dabs[2 * i - 2]));
        CHECK(near(b->ctrl2[c] - b->ctrl1[c], (next - prev) / 3.0),
              "brush %d point %d controls span %f, expected %f", k, i, b->ctrl2[c] - b->ctrl1[c],
              (next - prev) / 3.0);
        CHECK(near(b->border[c], radius), "brush %d point %d border %f, expected %f", k, i, b->border[c], radius);
      }
      CHECK(near(b->hardness, 1.0 - soft) && b->density == 1.0f && b->state == DT_MASKS_POINT_STATE_NORMAL,
            "brush %d point %d hardness %f density %f state %d", k, i, b->hardness, b->density, b->state);
    }
  }
  for(int k = n; k < RETOUCH_NO_FORMS; k++)
    CHECK(pr->rt_forms[k].formid == 0, "rt_forms[%d] set beyond the %d spots", k, n);
}

static void run_retouch(const dt_image_t *img, const lr_data_t *data, const gboolean exact)
{
  dt_iop_retouch_params_v3_t pr = { .blur_radius = 10.0f };
  GList *forms = _lr_retouch_to_dt(img, data, &pr);
  check_retouch(img, data, &pr, forms, exact);
  g_list_free_full(forms, (GDestroyNotify)dt_masks_free_form);
}

// ---------------------------------------------------------------------------
// retouch: spots as the parser could leave them, to shapes

static float rd_lr_pos(reader_t *r)
{
  // Lightroom's spots can sit a little past the image's edges
  return (float)(rd_unit(r) * 1.4 - 0.2);
}

static void fuzz_retouch(reader_t *r)
{
  lr_data_t data;
  memset(&data, 0, sizeof(data));
  dt_image_t img;
  memset(&img, 0, sizeof(img));
  const uint8_t flags = rd_u8(r);
  const gboolean raw = flags & 1;

  img.width = raw ? (int32_t)rd_u32(r) : 1 + rd_u32(r) % 16000;
  img.height = raw ? (int32_t)rd_u32(r) : 1 + rd_u32(r) % 16000;
  img.crop_x = raw ? (int32_t)rd_u32(r) : rd_u8(r) % 80;
  img.crop_y = raw ? (int32_t)rd_u32(r) : rd_u8(r) % 80;
  img.crop_right = raw ? (int32_t)rd_u32(r) : rd_u8(r) % 80;
  img.crop_bottom = raw ? (int32_t)rd_u32(r) : rd_u8(r) % 80;

  int n = rd_u8(r) % 16;
  if(flags & 2) n = rd_u8(r) + (rd_u8(r) % 64); // up to the 300 retouch holds
  data.n_spots = MIN(n, RETOUCH_NO_FORMS);
  for(int k = 0; k < data.n_spots; k++)
  {
    lr_spot_t *s = &data.spots[k];
    const uint8_t sf = rd_u8(r);
    s->x = raw ? rd_float_raw(r) : rd_lr_pos(r);
    s->y = raw ? rd_float_raw(r) : rd_lr_pos(r);
    s->xc = raw ? rd_float_raw(r) : rd_lr_pos(r);
    s->yc = raw ? rd_float_raw(r) : rd_lr_pos(r);
    s->radius = raw ? rd_float_raw(r) : (float)(1e-4 + rd_unit(r) * 0.3);
    s->opacity = rd_unit(r);
    s->feather = rd_unit(r);
    s->clone = sf & 1;
    if(sf & 2)
    {
      // the parser never leaves a stroke of one dab
      s->n_dabs = 2 + rd_u8(r) % 40;
      s->dabs = g_new(float, 2 * s->n_dabs);
      s->dabs[0] = s->x;
      s->dabs[1] = s->y;
      for(int i = 2; i < 2 * s->n_dabs; i++) s->dabs[i] = raw ? rd_float_raw(r) : rd_lr_pos(r);
    }
  }

  run_retouch(&img, &data, !raw);
  _free_spots(&data);
}

// ---------------------------------------------------------------------------
// vignette

static void vignette_from(lr_data_t *data, const int amount, const int midpoint, const int feather,
                          const float roundness, const int iwidth, const int iheight, const int orientation,
                          const gboolean has_crop, const float fratio)
{
  memset(data, 0, sizeof(*data));
  // as the parser leaves them
  data->pv.brightness = lr2dt_vignette_gain(amount);
  data->vignette_midpoint = CLAMP(midpoint, 0, 100);
  data->vignette_feather = CLAMP(feather, 0, 100);
  data->crop_roundness = roundness;
  data->iwidth = iwidth;
  data->iheight = iheight;
  data->orientation = orientation;
  data->has_crop = has_crop;
  data->fratio = fratio;
  _lr_vignette_to_dt(data);
}

static void fuzz_vignette(reader_t *r)
{
  const uint8_t flags = rd_u8(r);
  const gboolean raw = flags & 1;
  const int amount = raw ? (int32_t)rd_u32(r) : (int)(rd_u8(r) % 201) - 100;
  const int midpoint = raw ? (int32_t)rd_u32(r) : rd_u8(r) % 101;
  const int feather = raw ? (int32_t)rd_u32(r) : rd_u8(r) % 101;
  // the parser reads roundness with atoi()
  const float roundness = raw ? (int32_t)rd_u32(r) : (int)(rd_u8(r) % 201) - 100;
  const int iwidth = raw ? (int32_t)rd_u32(r) : 1 + rd_u32(r) % 16000;
  const int iheight = raw ? (int32_t)rd_u32(r) : 1 + rd_u32(r) % 16000;
  const gboolean has_crop = flags & 2;
  // with a crop, the width/height of the cropped frame, upright
  const float fratio = raw ? rd_float_raw(r) : (float)(0.1 + rd_unit(r) * 9.9);
  const int orientation = raw ? (int32_t)rd_u32(r) : 1 + rd_u8(r) % 8;

  lr_data_t d;
  vignette_from(&d, amount, midpoint, feather, roundness, iwidth, iheight, orientation, has_crop, fratio);
  const dt_iop_vignette_params_v3_t *p = &d.pv;
  CHECK(!p->autoratio && p->shape == 1.0f && p->center.x == 0.0f && p->center.y == 0.0f,
        "vignette not centered and elliptical");

  // any amount maps within the module's range; the parser clamps midpoint
  // and feather to 0-100 itself
  const float gain = lr2dt_vignette_gain(amount);
  if(abs(amount) <= 100)
    CHECK(gain >= -1.0f && gain <= 0.74f && (amount < 0) == (gain < 0.0f) && (amount == 0) == (gain == 0.0f),
          "amount %d gave brightness %f", amount, gain);
  if(amount > -100 && amount <= 100)
    CHECK(lr2dt_vignette_gain(amount - 1) <= gain, "brightness falls from amount %d to %d", amount - 1, amount);
  // fitted to Lightroom: at -40 its corners kept about 28% of their linear
  // brightness, at -100 none
  CHECK(fabsf(lr2dt_vignette_gain(-40) + 0.72f) < 0.02f && lr2dt_vignette_gain(-100) == -1.0f,
        "brightness %f at -40, %f at -100", lr2dt_vignette_gain(-40), lr2dt_vignette_gain(-100));

  CHECK(isfinite(p->falloff_scale) && p->falloff_scale >= 5.0f && p->falloff_scale <= 82.0f,
        "feather %d gave falloff %f", feather, p->falloff_scale);
  // a crop that parsed to NaN can leave a NaN scale, which the module
  // treats as no vignette
  if(!raw) CHECK(isfinite(p->scale), "scale %f", p->scale);

  // a later midpoint pushes the falloff out
  if(!raw && d.vignette_midpoint < 100)
  {
    lr_data_t later;
    vignette_from(&later, amount, d.vignette_midpoint + 1, feather, roundness, iwidth, iheight, orientation,
                  has_crop, fratio);
    CHECK(later.pv.scale > p->scale && later.pv.falloff_scale == p->falloff_scale,
          "midpoint %d to %d moved the falloff start from %f to %f", (int)d.vignette_midpoint,
          (int)d.vignette_midpoint + 1, p->scale, later.pv.scale);
  }

  // the vignette runs after crop and flip: it follows the cropped frame or,
  // without a crop, the image as EXIF orientations 5 to 8 turn it upright
  double frame = 1.0;
  if(has_crop && isfinite(fratio) && fratio > 0.0f)
    frame = fratio;
  else if(iwidth > 0 && iheight > 0)
    frame = orientation >= 5 && orientation <= 8 ? (double)iheight / iwidth : (double)iwidth / iheight;

  // the module takes a width/height ratio up to 2, and divides by 2 - whratio
  // above 1, so every frame shape must stay inside that. The scale has no
  // such limit in process(), past the 200 its slider shows
  if(!raw && fabsf(roundness) <= 100.0f)
    CHECK(isfinite(p->whratio) && p->whratio > 0.0f && p->whratio < 2.0f,
          "frame %f roundness %f gave whratio %f", frame, roundness, p->whratio);

  // without roundness the vignette has the frame's shape, a ratio r encoded
  // as is up to 1 and as 2 - 1/r above; compared encoded, as decoding a float
  // near 2 loses precision for wide frames
  if(!raw && roundness <= 0.0f)
  {
    const double encoded = frame <= 1.0 ? frame : 2.0 - 1.0 / frame;
    CHECK(fabs(p->whratio - encoded) <= 1e-6, "frame %f gave whratio %f, expected %f", frame, p->whratio,
          encoded);
  }
}

// ---------------------------------------------------------------------------
// XMP: documents built from the input, with the spots the parser must read
// from them worked out alongside

#define XMP_HEAD                                                                                             \
  "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"                                                                 \
  " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"                                   \
  "  <rdf:Description rdf:about=\"\"\n"                                                                      \
  "    xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\"\n"                                         \
  "    xmlns:tiff=\"http://ns.adobe.com/tiff/1.0/\""
#define XMP_TAIL "  </rdf:Description>\n </rdf:RDF>\n</x:xmpmeta>\n"

// exactly representable, so it prints and parses back to the same float
static double rd_grid(reader_t *r)
{
  return (int)(rd_u32(r) % 6000) / 4096.0 - 0.2;
}

// a property as an attribute of the element just opened, or as a child
// element; attrs collects the attributes until the element's start tag is
// closed
static void xmp_prop(GString *attrs, GString *body, const gboolean as_element, const char *name,
                     const char *value)
{
  if(as_element)
    g_string_append_printf(body, "        <crs:%s>%s</crs:%s>\n", name, value, name);
  else
    g_string_append_printf(attrs, " crs:%s=\"%s\"", name, value);
}

static void xmp_propf(GString *attrs, GString *body, const gboolean as_element, const char *name,
                      const double value)
{
  char buf[64];
  g_ascii_formatd(buf, sizeof(buf), "%.17g", value);
  xmp_prop(attrs, body, as_element, name, buf);
}

// a resource: an rdf:li carrying its properties itself, or wrapping an
// rdf:Description that does
static void xmp_resource(GString *out, const gboolean wrapped, const GString *attrs, const GString *body,
                         const char *indent)
{
  if(wrapped)
    g_string_append_printf(out, "%s<rdf:li>\n%s <rdf:Description%s>\n%s%s </rdf:Description>\n%s</rdf:li>\n",
                           indent, indent, attrs->str, body->str, indent, indent);
  else
    g_string_append_printf(out, "%s<rdf:li rdf:parseType=\"Resource\"%s>\n%s%s</rdf:li>\n", indent, attrs->str,
                           body->str, indent);
}

// one RetouchAreas entry and the spots the parser must take from it
static void xmp_area(reader_t *r, GString *out, lr_data_t *expect)
{
  const uint8_t f = rd_u8(r);
  const gboolean el = f & 1;
  GString *attrs = g_string_new(NULL), *body = g_string_new(NULL);

  static const char *types[] = { "clone", "heal", "heal_patchmatch", "Clone", "" };
  const int type = (f >> 1) % 5;
  if(type < 4) xmp_prop(attrs, body, el, "SpotType", types[type]);
  const gboolean usable = type < 2;
  const gboolean has_source = !(f & 0x10) || (f & 0x20);
  // floats, as the parser does the offset arithmetic in float
  const float sx = rd_grid(r), sy = rd_grid(r);
  if(has_source)
  {
    xmp_propf(attrs, body, el, "SourceX", sx);
    xmp_propf(attrs, body, el, "OffsetY", sy);
  }
  else if(f & 0x40)
    xmp_propf(attrs, body, el, "SourceX", sx);
  float opacity = 1.0f, feather = 0.0f;
  if(f & 0x80)
  {
    const double o = (int)(rd_u8(r) % 200) / 128.0 - 0.2, fe = (int)(rd_u8(r) % 200) / 128.0 - 0.2;
    xmp_propf(attrs, body, el, "Opacity", o);
    xmp_propf(attrs, body, el, "Feather", fe);
    opacity = CLAMP((float)o, 0.0f, 1.0f);
    feather = CLAMP((float)fe, 0.0f, 1.0f);
  }

  GString *masks = g_string_new(NULL);
  const int n_masks = rd_u8(r) % 4;
  float dx = NAN, dy = NAN;
  for(int m = 0; m < n_masks; m++)
  {
    const uint8_t mf = rd_u8(r);
    const gboolean mel = mf & 1;
    GString *ma = g_string_new(NULL), *mb = g_string_new(NULL);
    gboolean keep = TRUE;
    if((mf & 0x0e) == 0x02) { xmp_prop(ma, mb, mel, "MaskActive", "false"); keep = FALSE; }
    if((mf & 0x0e) == 0x04) { xmp_prop(ma, mb, mel, "MaskInverted", "true"); keep = FALSE; }
    if((mf & 0x0e) == 0x06) { xmp_propf(ma, mb, mel, "MaskValue", -(rd_u8(r) % 2) / 2.0); keep = FALSE; }
    if((mf & 0x0e) == 0x08) xmp_prop(ma, mb, mel, "MaskActive", "true");

    lr_spot_t s = { .clone = type == 0, .opacity = opacity, .feather = feather, .x = NAN, .y = NAN };
    const int what = (mf >> 4) % 3;
    if(what == 0)
    {
      xmp_prop(ma, mb, mel, "What", "Mask/Ellipse");
      s.x = rd_grid(r);
      s.y = rd_grid(r);
      s.radius = rd_grid(r);
      xmp_propf(ma, mb, mel, "X", s.x);
      xmp_propf(ma, mb, mel, "Y", s.y);
      if(!(mf & 0x40))
        xmp_propf(ma, mb, mel, "SizeX", s.radius);
      else
        s.radius = NAN;
    }
    else if(what == 1)
    {
      xmp_prop(ma, mb, mel, "What", "Mask/Paint");
      s.radius = rd_grid(r);
      if(!(mf & 0x40))
        xmp_propf(ma, mb, mel, "Radius", s.radius);
      else
        s.radius = NAN;
      GArray *dabs = g_array_new(FALSE, FALSE, sizeof(float));
      g_string_append(mb, "        <crs:Dabs>\n         <rdf:Seq>\n");
      const int n_dabs = rd_u8(r) % 12;
      for(int i = 0; i < n_dabs; i++)
      {
        const uint8_t df = rd_u8(r);
        float xy[2] = { rd_grid(r), rd_grid(r) };
        // a dab often repeats the previous one, which the parser drops
        if((df & 3) == 0 && dabs->len) memcpy(xy, &g_array_index(dabs, float, dabs->len - 2), sizeof(xy));
        char bx[64], by[64];
        g_ascii_formatd(bx, sizeof(bx), "%.17g", xy[0]);
        g_ascii_formatd(by, sizeof(by), "%.17g", xy[1]);
        const int form = (df >> 2) % 6;
        static const char *forms[] = { "d %s %s", " %s %s", "dab %s %s", "d %s", "d", "d %s,%s" };
        char item[160];
        snprintf(item, sizeof(item), forms[form], bx, by);
        g_string_append_printf(mb, "          <rdf:li>%s</rdf:li>\n", item);
        if(form < 3
           && (dabs->len == 0 || xy[0] != g_array_index(dabs, float, dabs->len - 2)
               || xy[1] != g_array_index(dabs, float, dabs->len - 1)))
          g_array_append_vals(dabs, xy, 2);
      }
      g_string_append(mb, "         </rdf:Seq>\n        </crs:Dabs>\n");
      s.n_dabs = dabs->len / 2;
      if(s.n_dabs == 0) keep = FALSE;
      else
      {
        s.x = g_array_index(dabs, float, 0);
        s.y = g_array_index(dabs, float, 1);
      }
      if(s.n_dabs > 1) s.dabs = (float *)g_array_free(dabs, FALSE);
      else
      {
        g_array_free(dabs, TRUE);
        s.n_dabs = 0;
      }
    }
    else
    {
      xmp_prop(ma, mb, mel, "What", "Mask/Gradient");
      keep = FALSE;
    }

    if(!(s.radius > 0.0f)) keep = FALSE;
    keep = keep && usable && has_source && expect->n_spots < RETOUCH_NO_FORMS;
    if(keep)
    {
      if(isnan(dx))
      {
        dx = sx - s.x;
        dy = sy - s.y;
      }
      s.xc = s.x + dx;
      s.yc = s.y + dy;
      expect->spots[expect->n_spots++] = s;
    }
    else
      g_free(s.dabs);
    xmp_resource(masks, mf & 0x80, ma, mb, "         ");
    g_string_free(ma, TRUE);
    g_string_free(mb, TRUE);
  }
  if(n_masks)
    g_string_append_printf(body, "        <crs:Masks>\n         <rdf:Seq>\n%s         </rdf:Seq>\n        </crs:Masks>\n",
                           masks->str);
  xmp_resource(out, f & 0x08, attrs, body, "      ");
  g_string_free(masks, TRUE);
  g_string_free(attrs, TRUE);
  g_string_free(body, TRUE);
}

// one RetouchInfo entry, which lists a circle as text
static void xmp_info(reader_t *r, GString *out, lr_data_t *expect)
{
  const uint8_t f = rd_u8(r);
  const double v[5] = { rd_grid(r), rd_grid(r), rd_grid(r), rd_grid(r), rd_grid(r) };
  char b[5][64];
  for(int k = 0; k < 5; k++) g_ascii_formatd(b[k], sizeof(b[k]), "%.17g", v[k]);
  const char *sp = (f & 1) ? "  " : " ";
  const int tail = (f >> 1) % 4; // spotType clone, heal, missing, garbage
  static const char *tails[] = { ", spotType = clone", ", spotType = heal", "", ", spotType" };
  const gboolean broken = (f & 0x30) == 0x30;
  gchar *item = g_strdup_printf("centerX%s=%s%s, centerY = %s, %s = %s, sourceState = sourceSetExplicitly, "
                                "sourceX = %s, sourceY = %s%s",
                                sp, sp, b[0], b[1], broken ? "radiuss" : "radius", b[2], b[3], b[4], tails[tail]);
  g_string_append_printf(out, "      <rdf:li>%s</rdf:li>\n", item);
  g_free(item);
  // as in RetouchAreas, a circle needs a radius
  if(!broken && v[2] > 0.0 && expect->n_spots < RETOUCH_NO_FORMS)
    expect->spots[expect->n_spots++] = (lr_spot_t){ .x = v[0], .y = v[1], .radius = v[2], .xc = v[3], .yc = v[4],
                                                    .clone = tail == 0, .opacity = 1.0f };
}

static void parse_xmp(const char *xmp, const size_t len, lr_data_t *data)
{
  _lr_data_init(data);
  xmlDocPtr doc = xmlReadMemory(xmp, len, NULL, NULL, XML_PARSE_NONET);
  if(!doc) return;
  xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
  if(ctx)
  {
    _lr_parse(&fake_dev, doc, ctx, 0, data);
    xmlXPathFreeContext(ctx);
  }
  xmlFreeDoc(doc);
}

// runs every conversion the import would on what was parsed
static void convert_all(lr_data_t *data, const dt_image_t *img, const gboolean exact)
{
  check_spots(data);
  if(data->has_crop) _lr_crop_to_dt(data);
  (void)_lr_exposure_to_dt(NULL, data);
  (void)_lr_tonecurve_to_dt(data);
  if(data->has_vignette) _lr_vignette_to_dt(data);
  if(data->n_spots) run_retouch(img, data, exact);
}

static void fuzz_xmp(reader_t *r)
{
  GString *xmp = g_string_new(XMP_HEAD);
  lr_data_t expect;
  memset(&expect, 0, sizeof(expect));
  const uint8_t flags = rd_u8(r);

  // the slider settings, as attributes
  const int amount = (int)(rd_u8(r) % 241) - 120, midpoint = (int)(rd_u8(r) % 141) - 20;
  const int feather = (int)(rd_u8(r) % 141) - 20, roundness = (int)(rd_u8(r) % 201) - 100;
  const int hue = rd_u8(r) * 2 % 400, sat = rd_u8(r) % 101, balance = (int)(rd_u8(r) % 241) - 120;
  if(flags & 1)
    g_string_append_printf(xmp, "\n    crs:PostCropVignetteAmount=\"%+d\" crs:PostCropVignetteMidpoint=\"%d\""
                                " crs:PostCropVignetteFeather=\"%d\" crs:PostCropVignetteRoundness=\"%+d\"",
                           amount, midpoint, feather, roundness);
  if(flags & 2)
    g_string_append_printf(xmp, "\n    crs:SplitToningShadowHue=\"%d\" crs:SplitToningHighlightHue=\"%d\""
                                " crs:SplitToningShadowSaturation=\"%d\" crs:SplitToningBalance=\"%+d\"",
                           hue, (hue + 40) % 400, sat, balance);
  g_string_append(xmp, ">\n");

  // spots, in RetouchAreas, RetouchInfo or both, in either order
  GString *areas = g_string_new(NULL), *info = g_string_new(NULL);
  lr_data_t from_areas, from_info;
  memset(&from_areas, 0, sizeof(from_areas));
  memset(&from_info, 0, sizeof(from_info));
  const int n_areas = (flags & 4) ? rd_u8(r) % 12 : 0;
  for(int k = 0; k < n_areas; k++) xmp_area(r, areas, &from_areas);
  const int n_info = (flags & 8) ? rd_u8(r) % 12 : 0;
  for(int k = 0; k < n_info; k++) xmp_info(r, info, &from_info);
  const char *areas_xml = "    <crs:RetouchAreas>\n     <rdf:Seq>\n%s     </rdf:Seq>\n    </crs:RetouchAreas>\n";
  const char *info_xml = "    <crs:RetouchInfo>\n     <rdf:Seq>\n%s     </rdf:Seq>\n    </crs:RetouchInfo>\n";
  if((flags & 0x10) && n_info) g_string_append_printf(xmp, info_xml, info->str);
  if(n_areas) g_string_append_printf(xmp, areas_xml, areas->str);
  if(!(flags & 0x10) && n_info) g_string_append_printf(xmp, info_xml, info->str);
  g_string_append(xmp, XMP_TAIL);

  // RetouchAreas lists every spot, so where it is present RetouchInfo is
  // ignored, wherever it comes in the document
  if(n_areas)
  {
    expect = from_areas;
    _free_spots(&from_info);
  }
  else
    expect = from_info;

  lr_data_t data;
  parse_xmp(xmp->str, xmp->len, &data);

  CHECK(data.n_spots == expect.n_spots, "parsed %d spots, expected %d from\n%s", data.n_spots, expect.n_spots,
        xmp->str);
  for(int k = 0; k < data.n_spots; k++)
  {
    const lr_spot_t *s = &data.spots[k], *e = &expect.spots[k];
    CHECK(s->x == e->x && s->y == e->y && s->radius == e->radius && s->xc == e->xc && s->yc == e->yc
          && s->clone == e->clone && s->opacity == e->opacity && s->feather == e->feather && s->n_dabs == e->n_dabs,
          "spot %d parsed as (%g, %g) r %g from (%g, %g) clone %d opacity %g feather %g dabs %d,\n"
          "  expected (%g, %g) r %g from (%g, %g) clone %d opacity %g feather %g dabs %d, from\n%s",
          k, s->x, s->y, s->radius, s->xc, s->yc, s->clone, s->opacity, s->feather, s->n_dabs, e->x, e->y,
          e->radius, e->xc, e->yc, e->clone, e->opacity, e->feather, e->n_dabs, xmp->str);
    for(int i = 0; i < 2 * s->n_dabs; i++)
      CHECK(s->dabs[i] == e->dabs[i], "spot %d dab coordinate %d is %g, expected %g", k, i, s->dabs[i],
            e->dabs[i]);
  }

  if(flags & 1)
  {
    CHECK(data.has_vignette == (amount != 0) && data.vignette_midpoint == CLAMP(midpoint, 0, 100)
          && data.vignette_feather == CLAMP(feather, 0, 100) && data.crop_roundness == roundness,
          "vignette %d/%d/%d/%d parsed as %d %g %g %g", amount, midpoint, feather, roundness, data.has_vignette,
          data.vignette_midpoint, data.vignette_feather, data.crop_roundness);
    if(amount) CHECK(data.pv.brightness == lr2dt_vignette_gain(amount), "amount %d not mapped", amount);
  }
  if(flags & 2)
  {
    // hues are degrees, saturation a percentage, and balance moves the
    // crossover down from mid gray as it favors the highlights
    CHECK(data.has_splittoning == (hue || sat || (hue + 40) % 400) && near(data.pst.shadow_hue, hue / 360.0)
          && near(data.pst.highlight_hue, (hue + 40) % 400 / 360.0),
          "hues %d, %d parsed as %f, %f", hue, (hue + 40) % 400, data.pst.shadow_hue, data.pst.highlight_hue);
    CHECK(data.pst.shadow_saturation >= 0.0f && data.pst.shadow_saturation <= 0.7f
          && (sat == 0) == (data.pst.shadow_saturation == 0.0f),
          "saturation %d parsed as %f", sat, data.pst.shadow_saturation);
    CHECK(data.pst.balance >= 0.0f && data.pst.balance <= 1.0f, "balance %d gave crossover %f", balance,
          data.pst.balance);
    if(balance > -100 && balance <= 100)
      CHECK(lr2dt_splittoning_balance(balance) < lr2dt_splittoning_balance(balance - 1),
            "crossover does not fall from balance %d to %d", balance - 1, balance);
  }
  else
    CHECK(data.pst.balance == lr2dt_splittoning_balance(0.0f), "default balance %f", data.pst.balance);

  dt_image_t img = { .width = 6000, .height = 4000, .crop_x = 12, .crop_y = 8, .crop_right = 4, .crop_bottom = 2 };
  convert_all(&data, &img, TRUE);
  _free_spots(&data);
  _free_spots(&expect);
  g_string_free(xmp, TRUE);
  g_string_free(areas, TRUE);
  g_string_free(info, TRUE);
}

// arbitrary bytes as the XMP, seeded with real Lightroom sidecars
static void fuzz_xmp_raw(reader_t *r)
{
  lr_data_t data;
  parse_xmp((const char *)r->p, r->n, &data);
  dt_image_t img = { .width = 6000, .height = 4000, .crop_x = 12, .crop_y = 8, .crop_right = 4, .crop_bottom = 2 };
  convert_all(&data, &img, FALSE);
  _free_spots(&data);
}

static void silent(void *ctx, const char *msg, ...)
{
}

// libdarktable pulls in GraphicsMagick, whose signal handlers would stop
// libFuzzer from catching crashes and saving the input
int LLVMFuzzerInitialize(int *argc, char ***argv)
{
  const int sigs[] = { SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGINT, SIGTERM };
  for(size_t k = 0; k < sizeof(sigs) / sizeof(sigs[0]); k++) signal(sigs[k], SIG_DFL);

  // circles read their default size from the config when created
  darktable.conf = calloc(1, sizeof(dt_conf_t));
  darktable.conf->x_confgen = g_hash_table_new(g_str_hash, g_str_equal);
  dt_conf_init(darktable.conf, "/dev/null", FALSE, NULL);

  // malformed XMP is expected, and printing every parse error slows fuzzing
  xmlSetGenericErrorFunc(NULL, silent);
  return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *buf, size_t len)
{
  reader_t r = { buf, len };
  switch(rd_u8(&r) % 8)
  {
    case 0: fuzz_crop_oracle(&r); break;
    case 1: fuzz_crop_raw(&r); break;
    case 2: fuzz_tonecurve(&r); break;
    case 3: fuzz_exposure(&r); break;
    case 4: fuzz_vignette(&r); break;
    case 5: fuzz_retouch(&r); break;
    case 6: fuzz_xmp(&r); break;
    default: fuzz_xmp_raw(&r); break;
  }
  return 0;
}
