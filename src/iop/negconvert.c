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

#include "bauhaus/bauhaus.h"
#include "common/color_picker.h"
#include "common/darktable.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"
#include "libs/lib.h"

#include <glib.h>
#include <math.h>
#include <stdlib.h>

/** DOCUMENTATION
 *
 * One-stop negative to positive conversion, with a control set organized the way
 * film scanning workflows usually are: analysis (film base and density range),
 * tone, color balance and finishing.
 *
 * The conversion works on the density of the negative relative to the film base:
 *
 *   D = log10(base / T)
 *   x = (D - D_black) / (D_white - D_black)
 *
 * x is then mapped through a tone profile, adjusted by the tone and color
 * controls, and finished with a look. The tone profile curves were measured from
 * Negative Lab Pro's output for each of its profiles, taken against its linear
 * profile on the same frames; the remaining tone/color constants are this
 * module's own choices. Nothing here is a calibrated film model.
 *
 * This is deliberately a different module from negadoctor, which implements a
 * Cineon-style physical model. Use negadoctor when you want that.
 *
 * Pipeline setup, for a negative photographed or scanned as a raw file:
 *
 *   - white balance: leave at camera reference / as shot, or "passthrough" for
 *     scanners. The film base (orange mask) is measured by this module, so any
 *     white balance applied before it only changes where the base lands
 *   - exposure, highlight reconstruction and color calibration: leave off, or at
 *     most use exposure to avoid clipping the light source. Do not neutralize
 *     the orange mask with them
 *   - input color profile: standard matrix or the scanner profile
 *   - lens corrections, crop, dust spot removal and denoising are fine, they
 *     belong before the inversion
 *   - filmic rgb, sigmoid, AgX and the rest of the scene-referred tone mapping:
 *     leave off, this module already produces display-referred values.
 *     Anything creative placed after it (color balance rgb, rgb curve, ...)
 *     acts on the positive and is fine
 **/

#define THRESHOLD 2.3283064365386963e-10f // -32 EV

// temp and tint are a density offset (add density) or the log of a gain (neutral density), per slider unit
#define BALANCE_SCALE 0.01f

// analysis histogram of log2(transmittance), 16 bins per EV over -32..0 EV
#define ANALYSIS_BINS 512
#define ANALYSIS_BINS_PER_EV 16

DT_MODULE_INTROSPECTION(1, dt_iop_negconvert_params_t)

typedef enum dt_iop_negconvert_film_t
{
  DT_NEGCONVERT_COLOR = 0, // $DESCRIPTION: "color"
  DT_NEGCONVERT_MONO = 1   // $DESCRIPTION: "black and white"
} dt_iop_negconvert_film_t;

typedef enum dt_iop_negconvert_color_method_t
{
  DT_NEGCONVERT_SHADOW_WEIGHTED = 0, // $DESCRIPTION: "shadow weighted"
  DT_NEGCONVERT_LINEAR_DYNAMIC = 1,  // $DESCRIPTION: "linear dynamic"
  DT_NEGCONVERT_LINEAR_FIXED = 2     // $DESCRIPTION: "linear fixed"
} dt_iop_negconvert_color_method_t;

typedef enum dt_iop_negconvert_density_t
{
  DT_NEGCONVERT_ADD_DENSITY = 0,    // $DESCRIPTION: "add density"
  DT_NEGCONVERT_NEUTRAL_DENSITY = 1 // $DESCRIPTION: "neutral density"
} dt_iop_negconvert_density_t;

typedef enum dt_iop_negconvert_tone_t
{
  DT_NEGCONVERT_TONE_LINEAR = 0,    // $DESCRIPTION: "linear"
  DT_NEGCONVERT_TONE_STANDARD = 1,  // $DESCRIPTION: "standard"
  DT_NEGCONVERT_TONE_CINEMATIC = 2, // $DESCRIPTION: "cinematic"
  DT_NEGCONVERT_TONE_NEUTRAL = 3    // $DESCRIPTION: "neutral"
} dt_iop_negconvert_tone_t;

typedef enum dt_iop_negconvert_curve_t
{
  DT_NEGCONVERT_CURVE_NONE = 0,           // $DESCRIPTION: "none"
  DT_NEGCONVERT_CURVE_SOFT = 1,           // $DESCRIPTION: "soft"
  DT_NEGCONVERT_CURVE_HARD = 2,           // $DESCRIPTION: "hard"
  DT_NEGCONVERT_CURVE_HIGHLIGHT_SOFT = 3, // $DESCRIPTION: "highlight soft"
  DT_NEGCONVERT_CURVE_HIGHLIGHT_HARD = 4, // $DESCRIPTION: "highlight hard"
  DT_NEGCONVERT_CURVE_SHADOW_SOFT = 5,    // $DESCRIPTION: "shadow soft"
  DT_NEGCONVERT_CURVE_SHADOW_HARD = 6,    // $DESCRIPTION: "shadow hard"
  DT_NEGCONVERT_CURVE_GAMMA = 7,          // $DESCRIPTION: "gamma"
  DT_NEGCONVERT_CURVE_FLAT = 8,           // $DESCRIPTION: "flat"
  DT_NEGCONVERT_CURVE_DEEP = 9,           // $DESCRIPTION: "deep"
  DT_NEGCONVERT_CURVE_RICH = 10           // $DESCRIPTION: "rich"
} dt_iop_negconvert_curve_t;

// standard and cinematic have a measured base curve, neutral and linear have none; the table is indexed by
// profile, so it has an unused row for linear
#define TONE_BASES (DT_NEGCONVERT_TONE_CINEMATIC + 1)
#define LOOK_CURVES 11
#define TONE_KNOTS 17

typedef enum dt_iop_negconvert_look_t
{
  DT_NEGCONVERT_LOOK_FRONTIER = 0, // $DESCRIPTION: "frontier"
  DT_NEGCONVERT_LOOK_NATURAL = 1,  // $DESCRIPTION: "natural"
  DT_NEGCONVERT_LOOK_CRYSTAL = 2,  // $DESCRIPTION: "crystal"
  DT_NEGCONVERT_LOOK_PAKON = 3     // $DESCRIPTION: "pakon"
} dt_iop_negconvert_look_t;

typedef enum dt_iop_negconvert_toning_t
{
  DT_NEGCONVERT_TONING_STANDARD = 0, // $DESCRIPTION: "standard"
  DT_NEGCONVERT_TONING_VINTAGE = 1   // $DESCRIPTION: "vintage"
} dt_iop_negconvert_toning_t;

typedef enum dt_iop_negconvert_clip_t
{
  DT_NEGCONVERT_CLIP_PROTECTED = 0, // $DESCRIPTION: "color protected"
  DT_NEGCONVERT_CLIP_HARD = 1       // $DESCRIPTION: "per channel"
} dt_iop_negconvert_clip_t;

typedef struct dt_iop_negconvert_params_t
{
  dt_iop_negconvert_film_t film_type;            // $DEFAULT: DT_NEGCONVERT_COLOR $DESCRIPTION: "film type"
  float base[4];                                 // $MIN: 0.00001 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "film base"
  float black_density[4];                        // $MIN: -1.0 $MAX: 4.0 $DEFAULT: 0.0 $DESCRIPTION: "black density"
  float white_density[4];                        // $MIN: 0.05 $MAX: 5.0 $DEFAULT: 1.5 $DESCRIPTION: "white density"
  dt_iop_negconvert_color_method_t color_method; // $DEFAULT: DT_NEGCONVERT_SHADOW_WEIGHTED $DESCRIPTION: "color method"
  dt_iop_negconvert_density_t color_density;     // $DEFAULT: DT_NEGCONVERT_ADD_DENSITY $DESCRIPTION: "color density"
  dt_iop_negconvert_tone_t tone_profile;         // $DEFAULT: DT_NEGCONVERT_TONE_LINEAR $DESCRIPTION: "tone profile"
  dt_iop_negconvert_curve_t look_curve;          // $DEFAULT: DT_NEGCONVERT_CURVE_NONE $DESCRIPTION: "look curve"
  float temp;                                    // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "temp"
  float tint;                                    // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "tint"
  float whites;                                  // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "whites"
  float lights;                                  // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "lights"
  float darks;                                   // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "darks"
  float blacks;                                  // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "blacks"
  float white_clip;                              // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "white clip"
  float black_clip;                              // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "black clip"
  float shadows_range;                           // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 5.0 $DESCRIPTION: "shadows range"
  float highs_range;                             // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 5.0 $DESCRIPTION: "highlights range"
  float red_cyan_shadows;                        // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "red/cyan"
  float green_magenta_shadows;                   // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "green/magenta"
  float blue_yellow_shadows;                     // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "blue/yellow"
  float red_cyan_mids;                           // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "red/cyan"
  float green_magenta_mids;                      // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "green/magenta"
  float blue_yellow_mids;                        // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "blue/yellow"
  float red_cyan_highs;                          // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "red/cyan"
  float green_magenta_highs;                     // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "green/magenta"
  float blue_yellow_highs;                       // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "blue/yellow"
  float saturation;                              // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 5.0 $DESCRIPTION: "saturation"
  dt_iop_negconvert_look_t look;                 // $DEFAULT: DT_NEGCONVERT_LOOK_FRONTIER $DESCRIPTION: "look"
  dt_iop_negconvert_toning_t toning;             // $DEFAULT: DT_NEGCONVERT_TONING_STANDARD $DESCRIPTION: "toning method"
  float lab_fade;                                // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "lab fade"
  float lab_glow;                                // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "lab glow"
  dt_iop_negconvert_clip_t clip_method;          // $DEFAULT: DT_NEGCONVERT_CLIP_PROTECTED $DESCRIPTION: "clip method"
  float highlight_rolloff;                       // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 25.0 $DESCRIPTION: "highlight rolloff"
  float shadow_rolloff;                          // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 25.0 $DESCRIPTION: "shadow rolloff"
  float analyze_black;                           // $MIN: 0.01 $MAX: 20.0 $DEFAULT: 1.0 $DESCRIPTION: "black percentile"
  float analyze_white;                           // $MIN: 0.01 $MAX: 20.0 $DEFAULT: 1.0 $DESCRIPTION: "white percentile"
  float contrast;                                // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "contrast"
  float auto_density;                             // $MIN: -0.5 $MAX: 0.5 $DEFAULT: 0.0 $DESCRIPTION: "auto density"
} dt_iop_negconvert_params_t;

typedef struct dt_iop_negconvert_data_t
{
  dt_aligned_pixel_t base;
  dt_aligned_pixel_t black_density;
  dt_aligned_pixel_t inv_range;
  dt_aligned_pixel_t density_shift; // temp/tint as a density offset (add density)
  dt_aligned_pixel_t gain;          // temp/tint as luminance-preserving gains (neutral density)
  dt_iop_negconvert_params_t p;
} dt_iop_negconvert_data_t;

typedef enum dt_iop_negconvert_pending_t
{
  DT_NEGCONVERT_PENDING_NONE = 0,
  DT_NEGCONVERT_PENDING_AUTO_WB
} dt_iop_negconvert_pending_t;

typedef struct dt_iop_negconvert_gui_data_t
{
  GtkNotebook *notebook;
  GtkWidget *base[3], *black_density[3], *white_density[3];
  GtkWidget *base_picker, *wb_picker, *analyze_picker;
  GtkWidget *analyze_black, *analyze_white;
  dt_gui_collapsible_section_t density_section;
  GtkWidget *temp, *tint, *auto_density, *shadow_rolloff;
  GtkWidget *auto_wb;
  // filled by the preview pipe, read by the gui thread, both under the module lock
  uint32_t analysis[3][ANALYSIS_BINS];
  gboolean analysis_valid;
  // same, limited to the area the analyze picker last covered
  uint32_t area_analysis[3][ANALYSIS_BINS];
  gboolean area_valid;
  dt_iop_negconvert_pending_t pending;
} dt_iop_negconvert_gui_data_t;

const char *name()
{
  return _("negative convert");
}

const char *aliases()
{
  return _("film|invert|negative|scan");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("convert film negative scans to positives.\n"
                                              "leave white balance at camera reference, and exposure, "
                                              "color calibration and tone mappers (filmic, sigmoid, agx) "
                                              "off: this module outputs display-referred values"),
                                      _("corrective and creative"),
                                      _("linear, RGB, display-referred"),
                                      _("non-linear, RGB"),
                                      _("non-linear, RGB, display-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
}

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);

  dt_iop_negconvert_params_t *d = self->default_params;

  // typical orange mask, same starting point as negadoctor
  d->base[0] = 1.00f;
  d->base[1] = 0.45f;
  d->base[2] = 0.25f;
  d->base[3] = 1.00f; // keep parameter validation with -d common happy
  for(int c = 0; c < 4; c++)
  {
    d->black_density[c] = 0.0f;
    d->white_density[c] = 1.5f;
  }
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = g_malloc0(sizeof(dt_iop_negconvert_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  g_free(piece->data);
  piece->data = NULL;
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_negconvert_params_t *const p = (dt_iop_negconvert_params_t *)p1;
  dt_iop_negconvert_data_t *const d = piece->data;

  d->p = *p;

  float lo[3], hi[3];
  float lo_min = p->black_density[0];
  float hi_max = p->white_density[0];
  for(int c = 0; c < 3; c++)
  {
    // black and white film have a single channel worth of information
    const int s = (p->film_type == DT_NEGCONVERT_MONO) ? 0 : c;
    lo[c] = p->black_density[s];
    hi[c] = p->white_density[s];
    lo_min = fminf(lo_min, lo[c]);
    hi_max = fmaxf(hi_max, hi[c]);
  }

  for(int c = 0; c < 3; c++)
  {
    const int s = (p->film_type == DT_NEGCONVERT_MONO) ? 0 : c;
    d->base[c] = p->base[s];

    switch(p->color_method)
    {
      case DT_NEGCONVERT_LINEAR_FIXED:
        lo[c] = lo_min;
        hi[c] = hi_max;
        break;
      case DT_NEGCONVERT_SHADOW_WEIGHTED:
        lo[c] = 0.5f * (lo[c] + lo_min);
        break;
      case DT_NEGCONVERT_LINEAR_DYNAMIC:
      default:
        break;
    }
    // clip points move the ends of the density range, so the tone profile stays anchored to them
    const float range = hi[c] - lo[c];
    lo[c] += p->black_clip * 0.01f * range;
    hi[c] -= p->white_clip * 0.01f * range;
    // contrast steepens the inversion curve about the middle of the density range; the ends move with it
    const float squeeze = exp2f(-p->contrast * 0.01f);
    const float mid = 0.5f * (lo[c] + hi[c]);
    lo[c] = mid + (lo[c] - mid) * squeeze;
    hi[c] = mid + (hi[c] - mid) * squeeze;
    d->black_density[c] = lo[c];
    d->inv_range[c] = 1.0f / fmaxf(hi[c] - lo[c], 0.01f);
  }
  d->base[3] = d->black_density[3] = d->inv_range[3] = 1.0f;

  const float temp = p->temp * BALANCE_SCALE;
  const float tint = p->tint * BALANCE_SCALE;
  d->density_shift[0] = temp;
  d->density_shift[1] = -tint;
  d->density_shift[2] = -temp;
  d->density_shift[3] = 0.0f;

  // exponential, so the gains stay positive over the whole slider range
  const float gain_r = expf(temp);
  const float gain_g = expf(-tint);
  const float gain_b = expf(-temp);
  const float luma = 0.2126f * gain_r + 0.7152f * gain_g + 0.0722f * gain_b;
  d->gain[0] = gain_r / luma;
  d->gain[1] = gain_g / luma;
  d->gain[2] = gain_b / luma;
  d->gain[3] = 1.0f;
}

static inline float _smoothstep(const float e0, const float e1, const float x)
{
  const float t = fminf(fmaxf((x - e0) / (e1 - e0), 0.0f), 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// triangular bump of half-width w around c
static inline float _bump(const float x, const float c, const float w)
{
  return fmaxf(0.0f, 1.0f - fabsf(x - c) / w);
}

// nlp's profiles split into a base profile and a curve on top of it: every nlp lab profile is a fixed curve of lab
// standard, and cinematic rich and flat of cinematic log, where cinematic flat uses the same curve as nlp's linear
// flat. each table is the median of nlp's output against the input over the same pixels, sampled at 0, 1/16, ... 1.
// the bases are display-encoded curves of u, the output of nlp's linear profile
static const float _base_curves[TONE_BASES][TONE_KNOTS] = {
  [DT_NEGCONVERT_TONE_STANDARD]
    = { 0.009f, 0.044f, 0.082f, 0.127f, 0.179f, 0.239f, 0.307f, 0.382f, 0.461f,
        0.542f, 0.623f, 0.702f, 0.777f, 0.845f, 0.908f, 0.965f, 1.018f },
  [DT_NEGCONVERT_TONE_CINEMATIC]
    = { 0.008f, 0.041f, 0.084f, 0.133f, 0.183f, 0.233f, 0.283f, 0.333f, 0.384f,
        0.439f, 0.498f, 0.564f, 0.638f, 0.720f, 0.808f, 0.901f, 0.994f }
};

// curves on top of a base profile, of its display-encoded output
static const float _look_curves[LOOK_CURVES][TONE_KNOTS] = {
  [DT_NEGCONVERT_CURVE_SOFT]
    = { 0.042f, 0.104f, 0.167f, 0.228f, 0.287f, 0.344f, 0.397f, 0.448f, 0.499f,
        0.549f, 0.601f, 0.655f, 0.712f, 0.771f, 0.834f, 0.898f, 0.961f },
  [DT_NEGCONVERT_CURVE_HARD]
    = { 0.006f, 0.036f, 0.075f, 0.124f, 0.184f, 0.254f, 0.331f, 0.414f, 0.500f,
        0.586f, 0.670f, 0.747f, 0.816f, 0.876f, 0.924f, 0.962f, 0.991f },
  [DT_NEGCONVERT_CURVE_HIGHLIGHT_SOFT]
    = { 0.009f, 0.068f, 0.128f, 0.188f, 0.246f, 0.303f, 0.359f, 0.413f, 0.467f,
        0.522f, 0.579f, 0.637f, 0.699f, 0.763f, 0.829f, 0.896f, 0.961f },
  [DT_NEGCONVERT_CURVE_HIGHLIGHT_HARD]
    = { 0.008f, 0.064f, 0.126f, 0.191f, 0.257f, 0.325f, 0.392f, 0.460f, 0.526f,
        0.591f, 0.656f, 0.718f, 0.779f, 0.838f, 0.893f, 0.945f, 0.991f },
  [DT_NEGCONVERT_CURVE_SHADOW_SOFT]
    = { 0.038f, 0.103f, 0.170f, 0.236f, 0.300f, 0.361f, 0.419f, 0.475f, 0.530f,
        0.584f, 0.639f, 0.695f, 0.753f, 0.813f, 0.873f, 0.934f, 0.991f },
  [DT_NEGCONVERT_CURVE_SHADOW_HARD]
    = { 0.008f, 0.055f, 0.106f, 0.162f, 0.220f, 0.281f, 0.343f, 0.407f, 0.473f,
        0.539f, 0.606f, 0.673f, 0.741f, 0.807f, 0.872f, 0.934f, 0.991f },
  [DT_NEGCONVERT_CURVE_GAMMA]
    = { 0.003f, 0.017f, 0.044f, 0.081f, 0.126f, 0.176f, 0.230f, 0.290f, 0.354f,
        0.423f, 0.496f, 0.574f, 0.657f, 0.743f, 0.829f, 0.914f, 0.993f },
  [DT_NEGCONVERT_CURVE_FLAT]
    = { 0.041f, 0.094f, 0.147f, 0.200f, 0.254f, 0.309f, 0.365f, 0.421f, 0.477f,
        0.532f, 0.587f, 0.640f, 0.693f, 0.745f, 0.799f, 0.857f, 0.920f },
  [DT_NEGCONVERT_CURVE_DEEP]
    = { 0.004f, 0.050f, 0.101f, 0.156f, 0.215f, 0.276f, 0.339f, 0.404f, 0.470f,
        0.538f, 0.607f, 0.676f, 0.745f, 0.813f, 0.879f, 0.940f, 0.994f },
  [DT_NEGCONVERT_CURVE_RICH]
    = { 0.005f, 0.041f, 0.091f, 0.152f, 0.219f, 0.290f, 0.363f, 0.436f, 0.508f,
        0.578f, 0.644f, 0.707f, 0.767f, 0.825f, 0.882f, 0.938f, 0.996f }
};

static inline gboolean _has_auto_density(const dt_iop_negconvert_tone_t profile)
{
  return profile == DT_NEGCONVERT_TONE_STANDARD;
}

// slope at a knot from its neighboring secants: the harmonic mean keeps the cubic monotone
static inline float _knot_slope(const float *const k, const int i)
{
  const float h = 1.0f / (TONE_KNOTS - 1);
  const float left = (i > 0) ? (k[i] - k[i - 1]) / h : (k[1] - k[0]) / h;
  const float right = (i < TONE_KNOTS - 1) ? (k[i + 1] - k[i]) / h : left;
  return (left * right > 0.0f) ? 2.0f * left * right / (left + right) : 0.0f;
}

// display-encoded output of negative lab pro's linear profile: on a synthetic step wedge its linear light
// output is the normalized density to the power 1.93, within 0.01
static inline float _nlp_linear(const float x)
{
  const float v = powf(fmaxf(x, 0.0f), 1.93f);
  return (v <= 0.0031308f) ? 12.92f * v : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

// monotone cubic through the knots of a table; beyond the measured range the curve continues along its end
// slope, leaving the rolloffs something to work on
static inline float _table_curve(const float *const k, const float v)
{
  if(v <= 0.0f) return k[0] + v * _knot_slope(k, 0);
  if(v >= 1.0f) return k[TONE_KNOTS - 1] + (v - 1.0f) * _knot_slope(k, TONE_KNOTS - 1);
  const float pos = v * (TONE_KNOTS - 1);
  const int i = MIN((int)pos, TONE_KNOTS - 2);
  const float t = pos - i, h = 1.0f / (TONE_KNOTS - 1);
  const float t2 = t * t, t3 = t2 * t;
  return (2.0f * t3 - 3.0f * t2 + 1.0f) * k[i] + (t3 - 2.0f * t2 + t) * h * _knot_slope(k, i)
         + (-2.0f * t3 + 3.0f * t2) * k[i + 1] + (t3 - t2) * h * _knot_slope(k, i + 1);
}

// auto_density is the standard profile's brightness correction (in nlp, the lab profiles'), a gamma on the
// midtones that keeps black and white in place: on the synthetic wedge that form fits nlp to an rms of 0.002,
// where a black lift or a density offset leave 0.01
static inline float _tone(const float x, const dt_iop_negconvert_tone_t profile,
                          const dt_iop_negconvert_curve_t curve, const float auto_density)
{
  float y = x;
  if(profile != DT_NEGCONVERT_TONE_LINEAR)
  {
    y = _nlp_linear(x);
    if(_has_auto_density(profile)) y = powf(y, expf(-auto_density));
    if(profile == DT_NEGCONVERT_TONE_STANDARD || profile == DT_NEGCONVERT_TONE_CINEMATIC)
      y = _table_curve(_base_curves[profile], y);
  }
  if(curve != DT_NEGCONVERT_CURVE_NONE) y = _table_curve(_look_curves[MIN(curve, LOOK_CURVES - 1)], y);
  return y;
}

// compresses values above the knee toward 1 without a corner at the knee: unit slope there, then
// a sigmoid that approaches 1 asymptotically, like the shoulder of the agx curve (agx.c, _scaled_sigmoid)
static inline float _shoulder(const float v, const float knee)
{
  if(v <= knee) return v;
  const float u = (v - knee) / (1.0f - knee);
  return knee + (1.0f - knee) * u / sqrtf(1.0f + u * u);
}

static inline float _luma(const float *const rgb)
{
  return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
}

static void _process_pixel(const dt_iop_negconvert_data_t *const d, const float *const in, float *const out)
{
  const dt_iop_negconvert_params_t *const p = &d->p;

  dt_aligned_pixel_t y;
  for_each_channel(c)
  {
    const float density = log10f(d->base[c] / fmaxf(in[c], THRESHOLD));
    float x = (density - d->black_density[c]) * d->inv_range[c];
    if(p->color_density == DT_NEGCONVERT_ADD_DENSITY) x += d->density_shift[c];
    y[c] = _tone(x, p->tone_profile, p->look_curve, p->auto_density);
    if(p->color_density == DT_NEGCONVERT_NEUTRAL_DENSITY) y[c] *= d->gain[c];
  }

  if(p->film_type == DT_NEGCONVERT_MONO) y[0] = y[1] = y[2] = _luma(y);

  float L = _luma(y);

  // color balance per tonal range
  const float shadows_edge = 0.1f + 0.08f * p->shadows_range;
  const float highs_edge = 0.1f + 0.08f * p->highs_range;
  const float w_shadows = 1.0f - _smoothstep(0.0f, shadows_edge, L);
  const float w_highs = _smoothstep(1.0f - highs_edge, 1.0f, L);
  const float w_mids = fmaxf(0.0f, 1.0f - w_shadows - w_highs);
  const float balance_scale = 0.002f;
  const float red = balance_scale * (w_shadows * p->red_cyan_shadows + w_mids * p->red_cyan_mids
                                     + w_highs * p->red_cyan_highs);
  const float green = balance_scale * (w_shadows * p->green_magenta_shadows + w_mids * p->green_magenta_mids
                                       + w_highs * p->green_magenta_highs);
  const float blue = balance_scale * (w_shadows * p->blue_yellow_shadows + w_mids * p->blue_yellow_mids
                                      + w_highs * p->blue_yellow_highs);
  // positive values move toward the second color of the pair, as the slider reads left to right
  y[0] -= red;
  y[1] -= green;
  y[2] -= blue;

  // tonal regions, applied as a luminance-driven shift on all channels
  const float region = 0.003f * (p->blacks * (1.0f - _smoothstep(0.0f, 0.3f, L))
                                 + p->darks * _bump(L, 0.3f, 0.25f)
                                 + p->lights * _bump(L, 0.7f, 0.25f)
                                 + p->whites * _smoothstep(0.7f, 1.0f, L));
  for_each_channel(c) y[c] += region;

  // saturation, neutral at 5
  L = _luma(y);
  const float sat = p->saturation * 0.2f;
  for_each_channel(c) y[c] = L + (y[c] - L) * sat;

  // looks: contrast about mid-gray, a saturation multiplier and a small warm/cool shift
  float contrast, look_sat, warm;
  switch(p->look)
  {
    case DT_NEGCONVERT_LOOK_CRYSTAL:
      contrast = 1.15f; look_sat = 1.15f; warm = -0.01f;
      break;
    case DT_NEGCONVERT_LOOK_PAKON:
      contrast = 0.95f; look_sat = 0.95f; warm = 0.0f;
      break;
    case DT_NEGCONVERT_LOOK_NATURAL:
      contrast = 1.0f; look_sat = 1.0f; warm = 0.0f;
      break;
    case DT_NEGCONVERT_LOOK_FRONTIER:
    default:
      contrast = 1.08f; look_sat = 1.10f; warm = 0.01f;
      break;
  }
  L = _luma(y);
  for_each_channel(c)
  {
    y[c] = L + (y[c] - L) * look_sat;
    y[c] = 0.5f + (y[c] - 0.5f) * contrast;
  }
  y[0] += warm;
  y[2] -= warm;

  if(p->toning == DT_NEGCONVERT_TONING_VINTAGE)
  {
    L = _luma(y);
    const float hi = _smoothstep(0.5f, 1.0f, L);
    const float lo = 1.0f - _smoothstep(0.0f, 0.5f, L);
    y[0] += 0.02f * hi;
    y[2] -= 0.02f * hi;
    y[0] += 0.02f * lo;
    y[2] += 0.015f * lo;
  }

  // lab fade lifts the blacks, lab glow lifts and softens the highlights
  const float fade = p->lab_fade * 0.002f;
  L = _luma(y);
  const float glow = p->lab_glow * 0.002f * _smoothstep(0.6f, 1.0f, L);
  for_each_channel(c) y[c] = fade + y[c] * (1.0f - fade) + glow;

  if(p->shadow_rolloff > 0.0f)
  {
    // the toe is the shoulder mirrored about mid gray, so it approaches 0 and never goes negative
    const float knee = 0.005f * p->shadow_rolloff;
    for_each_channel(c) y[c] = 1.0f - _shoulder(1.0f - y[c], 1.0f - knee);
  }

  if(p->highlight_rolloff > 0.0f)
  {
    const float knee = 1.0f - 0.005f * p->highlight_rolloff;
    if(p->clip_method == DT_NEGCONVERT_CLIP_PROTECTED)
    {
      // roll off the brightest channel and scale the others with it, so the hue survives
      const float peak = fmaxf(y[0], fmaxf(y[1], y[2]));
      if(peak > knee)
      {
        const float scale = _shoulder(peak, knee) / peak;
        for_each_channel(c) y[c] *= scale;
      }
    }
    else
    {
      for_each_channel(c) y[c] = _shoulder(y[c], knee);
    }
  }

  if(p->clip_method == DT_NEGCONVERT_CLIP_PROTECTED)
  {
    const float peak = fmaxf(y[0], fmaxf(y[1], y[2]));
    if(peak > 1.0f)
      for_each_channel(c) y[c] /= peak;
  }
  // everything above works on display-encoded values, the pipe wants linear ones
  for_each_channel(c)
  {
    const float e = fminf(fmaxf(y[c], 0.0f), 1.0f);
    out[c] = (e <= 0.04045f) ? e / 12.92f : powf((e + 0.055f) / 1.055f, 2.4f);
  }
  out[3] = in[3];
}

static void _fill_histogram(uint32_t *const hist, const float *const in, const dt_iop_roi_t *const roi,
                            const int x0, const int y0, const int x1, const int y1)
{
  for(int y = y0; y < y1; y++)
    for(int x = x0; x < x1; x++)
    {
      const float *const px = in + 4 * ((size_t)y * roi->width + x);
      for(int c = 0; c < 3; c++)
      {
        const float ev = log2f(fmaxf(px[c], THRESHOLD));
        const int bin = CLAMP((int)((ev + 32.0f) * ANALYSIS_BINS_PER_EV), 0, ANALYSIS_BINS - 1);
        hist[c * ANALYSIS_BINS + bin]++;
      }
    }
}

// the preview pipe sees the whole (cropped) frame, so it feeds the density range analysis
static void _collect_analysis(dt_iop_module_t *self, const float *const in, const dt_iop_roi_t *const roi)
{
  dt_iop_negconvert_gui_data_t *g = self->gui_data;

  uint32_t *hist = g_malloc0_n(3 * ANALYSIS_BINS, sizeof(uint32_t));
  _fill_histogram(hist, in, roi, 0, 0, roi->width, roi->height);

  // while the analyze or white balance picker is active, also count the picked area alone; the film base
  // picker covers the border, which must not become the area the analysis works on
  const dt_iop_color_picker_t *const picker = darktable.lib->proxy.colorpicker.picker_proxy;
  uint32_t *area = NULL;
  if(self->request_color_pick == DT_REQUEST_COLORPICK_MODULE && picker
     && (picker->colorpick == g->analyze_picker || picker->colorpick == g->wb_picker)
     && darktable.lib->proxy.colorpicker.primary_sample)
  {
    int box[4];
    if(!dt_color_picker_box(self, roi, darktable.lib->proxy.colorpicker.primary_sample, PIXELPIPE_PICKER_INPUT,
                            box))
    {
      area = g_malloc0_n(3 * ANALYSIS_BINS, sizeof(uint32_t));
      _fill_histogram(area, in, roi, CLAMP(box[0], 0, roi->width), CLAMP(box[1], 0, roi->height),
                      CLAMP(box[2], 0, roi->width), CLAMP(box[3], 0, roi->height));
    }
  }

  dt_iop_gui_enter_critical_section(self);
  memcpy(g->analysis, hist, sizeof(g->analysis));
  g->analysis_valid = TRUE;
  if(area)
  {
    memcpy(g->area_analysis, area, sizeof(g->area_analysis));
    g->area_valid = TRUE;
  }
  dt_iop_gui_leave_critical_section(self);
  g_free(hist);
  g_free(area);
}

void process(dt_iop_module_t *const self, dt_dev_pixelpipe_iop_t *const piece,
             const void *const restrict ivoid, void *const restrict ovoid,
             const dt_iop_roi_t *const restrict roi_in, const dt_iop_roi_t *const restrict roi_out)
{
  const dt_iop_negconvert_data_t *const d = piece->data;
  assert(piece->colors == 4);

  const float *const restrict in = (float *)ivoid;
  float *const restrict out = (float *)ovoid;

  dt_iop_negconvert_gui_data_t *g = self->gui_data;
  if(self->dev->gui_attached && g && dt_pipe_is_preview(piece->pipe))
    _collect_analysis(self, in, roi_in);

  DT_OMP_FOR()
  for(size_t k = 0; k < (size_t)roi_out->height * roi_out->width * 4; k += 4)
    _process_pixel(d, in + k, out + k);
}

static void _set_triplet(GtkWidget *const widgets[3], const float *const values)
{
  for(int c = 0; c < 3; c++) dt_bauhaus_slider_set(widgets[c], values[c]);
}

// film base: the unexposed border of the film
static void _apply_base(dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  dt_iop_negconvert_gui_data_t *g = self->gui_data;
  dt_iop_negconvert_params_t *p = self->params;

  for(int c = 0; c < 3; c++) p->base[c] = fmaxf(self->picked_color[c], THRESHOLD);

  DT_ENTER_GUI_UPDATE();
  _set_triplet(g->base, p->base);
  DT_LEAVE_GUI_UPDATE();

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// choose temp and tint so that an area with the given normalized densities comes out neutral
static void _set_balance(dt_iop_module_t *self, const float x[3])
{
  DT_GUARD_GUI_UPDATE();
  dt_iop_negconvert_gui_data_t *g = self->gui_data;
  dt_iop_negconvert_params_t *p = self->params;

  float temp, tint;
  if(p->color_density == DT_NEGCONVERT_ADD_DENSITY)
  {
    // shifts are (t, -n, -t) added to x: the three channels can be matched exactly
    const float t = 0.5f * (x[2] - x[0]);
    const float mean = 0.5f * (x[0] + x[2]);
    temp = t;
    tint = x[1] - mean;
  }
  else
  {
    // gains are exp(t, -n, -t): make gain * tone(x) equal in the three channels
    float ly[3];
    for(int c = 0; c < 3; c++) ly[c] = logf(fmaxf(_tone(x[c], p->tone_profile, p->look_curve, p->auto_density), THRESHOLD));
    temp = 0.5f * (ly[2] - ly[0]);
    tint = ly[1] - 0.5f * (ly[0] + ly[2]);
  }
  p->temp = CLAMP(temp / BALANCE_SCALE, -100.0f, 100.0f);
  p->tint = CLAMP(tint / BALANCE_SCALE, -100.0f, 100.0f);

  DT_ENTER_GUI_UPDATE();
  dt_bauhaus_slider_set(g->temp, p->temp);
  dt_bauhaus_slider_set(g->tint, p->tint);
  DT_LEAVE_GUI_UPDATE();

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// normalized density of a transmittance, as the conversion sees it without any temp/tint
static void _unbalanced_density(dt_iop_module_t *self, const float transmittance[3], float x[3])
{
  dt_iop_negconvert_params_t neutral = *(dt_iop_negconvert_params_t *)self->params;
  neutral.temp = neutral.tint = 0.0f;
  dt_iop_negconvert_data_t d = { { 0 } };
  dt_dev_pixelpipe_iop_t piece = { 0 };
  piece.data = &d;
  commit_params(self, (dt_iop_params_t *)&neutral, NULL, &piece);

  for(int c = 0; c < 3; c++)
  {
    const float density = log10f(d.base[c] / fmaxf(transmittance[c], THRESHOLD));
    x[c] = (density - d.black_density[c]) * d.inv_range[c];
  }
}

static void _apply_analysis(dt_iop_module_t *self);
static void _apply_area_wb(dt_iop_module_t *self);

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_t *pipe)
{
  dt_iop_negconvert_gui_data_t *g = self->gui_data;

  if(picker == g->base_picker)
    _apply_base(self);
  else if(picker == g->wb_picker)
    _apply_area_wb(self);
  else if(picker == g->analyze_picker)
    _apply_analysis(self);
  else
    dt_print(DT_DEBUG_ALWAYS, "[negconvert] unknown color picker");
}

// transmittance at the given cumulative fraction, counted from the dark end, interpolated within its bin
static float _histogram_percentile(const uint32_t *const hist, const float fraction)
{
  uint64_t total = 0;
  for(int i = 0; i < ANALYSIS_BINS; i++) total += hist[i];
  const double target = fraction * (double)total;
  double sum = 0.0;
  float position = ANALYSIS_BINS;
  for(int i = 0; i < ANALYSIS_BINS; i++)
  {
    if(hist[i] && sum + hist[i] >= target)
    {
      position = i + (float)((target - sum) / hist[i]);
      break;
    }
    sum += hist[i];
  }
  return exp2f(position / ANALYSIS_BINS_PER_EV - 32.0f);
}

// density range from the picked area: the thinnest and densest parts of each channel
static void _apply_analysis(dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  dt_iop_negconvert_gui_data_t *g = self->gui_data;
  dt_iop_negconvert_params_t *p = self->params;

  dt_iop_gui_enter_critical_section(self);
  if(!g->area_valid)
  {
    dt_iop_gui_leave_critical_section(self);
    return;
  }
  float median[3];
  for(int c = 0; c < 3; c++)
  {
    // thin negative (high transmittance) is black, dense (low transmittance) is white
    const float t_black = _histogram_percentile(g->area_analysis[c], 1.0f - 0.01f * p->analyze_black);
    const float t_white = _histogram_percentile(g->area_analysis[c], 0.01f * p->analyze_white);
    p->black_density[c] = log10f(p->base[c] / t_black);
    p->white_density[c] = log10f(p->base[c] / t_white);
    median[c] = _histogram_percentile(g->area_analysis[c], 0.5f);
  }
  dt_iop_gui_leave_critical_section(self);

  // negative lab pro's lab profiles brighten dark images and slightly darken bright ones. this fit of the
  // log of their midtone gamma against the median brightness of nlp's linear output was measured over ten
  // photos and three synthetic wedges, to within 0.03
  float x[3], u[3];
  _unbalanced_density(self, median, x);
  for(int c = 0; c < 3; c++) u[c] = _nlp_linear(x[c]);
  p->auto_density = -CLAMP(1.615f * (_luma(u) - 0.543f), -0.237f, 0.069f);

  DT_ENTER_GUI_UPDATE();
  _set_triplet(g->black_density, p->black_density);
  _set_triplet(g->white_density, p->white_density);
  dt_bauhaus_slider_set(g->auto_density, p->auto_density);
  DT_LEAVE_GUI_UPDATE();

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// white balance from the picked area: the median of each channel is assumed to be neutral
static void _apply_area_wb(dt_iop_module_t *self)
{
  dt_iop_negconvert_gui_data_t *g = self->gui_data;

  float median[3];
  dt_iop_gui_enter_critical_section(self);
  const gboolean valid = g->area_valid;
  for(int c = 0; c < 3; c++) median[c] = _histogram_percentile(g->area_analysis[c], 0.5f);
  dt_iop_gui_leave_critical_section(self);
  if(!valid) return;

  float x[3];
  _unbalanced_density(self, median, x);
  _set_balance(self, x);
}

// white balance from the area last picked for analysis, or the whole frame if there is none
static void _apply_auto_wb(dt_iop_module_t *self)
{
  dt_iop_negconvert_gui_data_t *g = self->gui_data;

  float median[3];
  dt_iop_gui_enter_critical_section(self);
  const uint32_t(*const hist)[ANALYSIS_BINS] = g->area_valid ? g->area_analysis : g->analysis;
  for(int c = 0; c < 3; c++) median[c] = _histogram_percentile(hist[c], 0.5f);
  dt_iop_gui_leave_critical_section(self);

  float x[3];
  _unbalanced_density(self, median, x);
  _set_balance(self, x);
}

static void _run_pending(dt_iop_module_t *self)
{
  dt_iop_negconvert_gui_data_t *g = self->gui_data;
  const dt_iop_negconvert_pending_t action = g->pending;
  g->pending = DT_NEGCONVERT_PENDING_NONE;

  if(action == DT_NEGCONVERT_PENDING_AUTO_WB)
    _apply_auto_wb(self);
}

static void _preview_pipe_finished_callback(gpointer instance, dt_iop_module_t *self)
{
  dt_iop_negconvert_gui_data_t *g = self->gui_data;
  if(!g || g->pending == DT_NEGCONVERT_PENDING_NONE) return;

  dt_iop_gui_enter_critical_section(self);
  const gboolean valid = g->analysis_valid;
  dt_iop_gui_leave_critical_section(self);
  if(valid) _run_pending(self);
}

static void _request_analysis(dt_iop_module_t *self, const dt_iop_negconvert_pending_t action)
{
  dt_iop_negconvert_gui_data_t *g = self->gui_data;

  g->pending = action;
  if(self->enabled)
  {
    dt_iop_gui_enter_critical_section(self);
    const gboolean valid = g->analysis_valid;
    dt_iop_gui_leave_critical_section(self);
    if(valid) _run_pending(self);
  }
  else
  {
    // the preview pipe only sees this module's input once it runs
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
}

static void _auto_wb_clicked(GtkWidget *widget, dt_iop_module_t *self)
{
  _request_analysis(self, DT_NEGCONVERT_PENDING_AUTO_WB);
}

static GtkWidget *_triplet_slider(dt_iop_module_t *self, const char *const field[3], GtkWidget *out[3],
                                  const char *const label[3], const char *const tooltip)
{
  for(int c = 0; c < 3; c++)
  {
    out[c] = dt_bauhaus_slider_from_params(self, field[c]);
    dt_bauhaus_slider_set_digits(out[c], 4);
    dt_bauhaus_widget_set_label(out[c], NULL, label[c]);
    gtk_widget_set_tooltip_text(out[c], tooltip);
  }
  return out[0];
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_negconvert_gui_data_t *g = IOP_GUI_ALLOC(negconvert);

  static dt_action_def_t notebook_def = { };
  g->notebook = dt_ui_notebook_new(&notebook_def);
  dt_action_define_iop(self, NULL, N_("page"), GTK_WIDGET(g->notebook), &notebook_def);

  // page ANALYSIS
  GtkWidget *page = self->widget = dt_ui_notebook_page(g->notebook, N_("analysis"), NULL);

  g->base_picker = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, NULL);
  gtk_widget_set_tooltip_text(g->base_picker, _("pick the unexposed film border"));
  dt_action_define_iop(self, N_("pickers"), N_("film base"), g->base_picker, &dt_action_def_color_picker);

  g->analyze_picker = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, NULL);
  gtk_widget_set_tooltip_text(g->analyze_picker, _("set the density range of every channel from the picked area.\n"
                                                   "the thinnest and densest part of each channel, as set by the "
                                                   "percentiles below, become black and white, which also removes "
                                                   "most of the color cast.\n"
                                                   "pick the image content and leave out the film border, sprocket "
                                                   "holes and holder.\n"
                                                   "this sets the black and white density sliders automatically, "
                                                   "so they should usually not be modified by hand.\n"
                                                   "the film base does not need to be set for this"));
  dt_action_define_iop(self, N_("pickers"), N_("density"), g->analyze_picker, &dt_action_def_color_picker);

  dt_gui_box_add(page, dt_ui_section_label_new(C_("section", "film base")), g->base_picker);
  _triplet_slider(self, (const char *[]){ "base[0]", "base[1]", "base[2]" }, g->base,
                  (const char *[]){ N_("film base red"), N_("film base green"), N_("film base blue") },
                  _("transmittance of the unexposed film, as seen by the scanner or camera"));

  dt_gui_box_add(page, dt_ui_section_label_new(C_("section", "density")), g->analyze_picker);

  g->analyze_black = dt_bauhaus_slider_from_params(self, "analyze_black");
  GtkWidget *w = g->analyze_black;
  dt_bauhaus_slider_set_format(w, "%");
  dt_bauhaus_slider_set_soft_range(w, 0.01f, 5.0f);
  dt_bauhaus_slider_set_log_curve(w);
  dt_bauhaus_slider_set_digits(w, 3);
  gtk_widget_set_tooltip_text(w, _("share of the pixels, from the thinnest end of the negative, that analyze "
                                   "lets clip to black.\n"
                                   "raise it to ignore a bright film border or holder inside the picked area"));
  w = g->analyze_white = dt_bauhaus_slider_from_params(self, "analyze_white");
  dt_bauhaus_slider_set_format(w, "%");
  dt_bauhaus_slider_set_soft_range(w, 0.01f, 5.0f);
  dt_bauhaus_slider_set_log_curve(w);
  dt_bauhaus_slider_set_digits(w, 3);
  gtk_widget_set_tooltip_text(w, _("share of the pixels, from the densest end of the negative, that analyze "
                                   "lets clip to white.\n"
                                   "raise it to ignore dust and specular highlights"));

  GtkWidget *density_box = dt_gui_vbox();
  dt_gui_box_add(page, density_box);
  dt_gui_new_collapsible_section(&g->density_section, "plugins/darkroom/negconvert/expand_channel_densities",
                                 _("channel densities"), GTK_BOX(density_box), DT_ACTION(self));
  self->widget = GTK_WIDGET(g->density_section.container);

  _triplet_slider(self, (const char *[]){ "black_density[0]", "black_density[1]", "black_density[2]" },
                  g->black_density,
                  (const char *[]){ N_("black density red"), N_("black density green"), N_("black density blue") },
                  _("density of the negative that becomes black"));
  _triplet_slider(self, (const char *[]){ "white_density[0]", "white_density[1]", "white_density[2]" },
                  g->white_density,
                  (const char *[]){ N_("white density red"), N_("white density green"), N_("white density blue") },
                  _("density of the negative that becomes white"));
  self->widget = page;

  dt_gui_box_add(page, dt_ui_section_label_new(C_("section", "conversion")));
  w = dt_bauhaus_combobox_from_params(self, "film_type");
  gtk_widget_set_tooltip_text(w, _(
                                   "color: independent red, green and blue densities\n"
                                   "black and white: only the red channel density is used,\n"
                                   "the result is a monochrome image"));
  w = dt_bauhaus_combobox_from_params(self, "color_method");
  gtk_widget_set_tooltip_text(w, _("how the density range is applied to each channel.\n"
                                   "linear dynamic: stretch each channel on its own\n"
                                   "linear fixed: one range for all channels\n"
                                   "shadow weighted: stretch the white point per channel, "
                                   "share the black point"));
  w = dt_bauhaus_combobox_from_params(self, "color_density");
  gtk_widget_set_tooltip_text(w, _("add density: temp and tint shift the density before the tone profile\n"
                                   "neutral density: temp and tint scale the result, keeping luminance"));
  w = dt_bauhaus_combobox_from_params(self, "tone_profile");
  gtk_widget_set_tooltip_text(w, _("base response applied to the normalized density. "
                                   "all but linear follow negative lab pro's profiles.\n"
                                   "linear: the normalized density as it is, with no curve; "
                                   "the only profile that goes below black, where shadow rolloff applies\n"
                                   "standard: bold contrast like a lab scanner, with a brightness correction "
                                   "that analyze sets from the picked area (auto density). "
                                   "negative lab pro calls it lab\n"
                                   "cinematic: a logarithmic response with smooth mids and shadows "
                                   "and detail kept in the highlights (negative lab pro's cinematic log)\n"
                                   "neutral: a plain response, for flat results to grade later "
                                   "(negative lab pro calls it linear)"));
  w = dt_bauhaus_combobox_from_params(self, "look_curve");
  gtk_widget_set_tooltip_text(w, _("fixed curve on top of the tone profile. "
                                   "negative lab pro pairs them as lab soft, hard, highlight and shadow soft and hard; "
                                   "neutral gamma, flat and deep; cinematic rich and flat, "
                                   "but any curve works with any profile.\n"
                                   "soft and hard: lower and higher contrast\n"
                                   "highlight and shadow soft and hard: the same, at one end of the range\n"
                                   "gamma: darker and richer midtones\n"
                                   "flat: lifts black and lowers white\n"
                                   "deep: a touch darker\n"
                                   "rich: deeper shadows and brighter highlights"));
  g->auto_density = dt_bauhaus_slider_from_params(self, "auto_density");
  dt_bauhaus_slider_set_digits(g->auto_density, 3);
  gtk_widget_set_tooltip_text(g->auto_density, _("brightness correction of the standard tone profile: positive values "
                                                "brighten the midtones, negative values darken them, "
                                                "black and white stay in place.\n"
                                                "analyze sets it from the brightness of the picked area, "
                                                "pulling dark images up and bright ones down.\n"
                                                "has no effect on the other tone profiles"));

  // page TONE
  page = self->widget = dt_ui_notebook_page(g->notebook, N_("tone"), NULL);
  g->wb_picker = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, NULL);
  gtk_widget_set_tooltip_text(g->wb_picker, _("pick an area that should be neutral gray in the positive, "
                                              "such as a gray card, a white shirt or a concrete wall.\n"
                                              "sets temp and tint so the median of each channel in the area has no color cast, "
                                              "so dust and stray pixels in it do not matter.\n"
                                              "set the film base and density range first, "
                                              "as they define the response the balance is computed from"));
  dt_action_define_iop(self, N_("pickers"), N_("white balance"), g->wb_picker, &dt_action_def_color_picker);
  g->auto_wb = dt_iop_button_new(self, N_("auto white balance"), G_CALLBACK(_auto_wb_clicked), FALSE, 0, 0, NULL, 0, NULL);
  gtk_widget_set_tooltip_text(g->auto_wb, _("set temp and tint so the median of each channel is neutral.\n"
                                            "this assumes the scene averages out to gray, so it fails on images "
                                            "dominated by one color: use the picker on a known neutral there.\n"
                                            "it uses the area last picked with the density range or white balance picker; "
                                            "without one, the whole frame, so crop away the film border first"));
  dt_gui_box_add(page, dt_ui_section_label_new(C_("section", "white balance")), g->wb_picker, g->auto_wb);
  g->temp = w = dt_bauhaus_slider_from_params(self, "temp");
  dt_bauhaus_slider_set_soft_range(w, -30.0f, 30.0f);
  dt_bauhaus_slider_set_stop(w, 0.0, 0.3, 0.3, 1);
  dt_bauhaus_slider_set_stop(w, 0.5, 0.5, 0.5, 0.5);
  dt_bauhaus_slider_set_stop(w, 1.0, 1, 1, 0.25);
  gtk_widget_set_tooltip_text(w, _(
                                   "shifts the color balance along the blue/yellow axis\n"
                                   "positive values warm the image, negative values cool it\n"
                                   "the film base picker neutralizes the orange mask; use this for the rest"));
  g->tint = w = dt_bauhaus_slider_from_params(self, "tint");
  dt_bauhaus_slider_set_soft_range(w, -30.0f, 30.0f);
  dt_bauhaus_slider_set_stop(w, 0.0, 0.25, 1, 0.25);
  dt_bauhaus_slider_set_stop(w, 0.5, 0.5, 0.5, 0.5);
  dt_bauhaus_slider_set_stop(w, 1.0, 1, 0.25, 1);
  gtk_widget_set_tooltip_text(w, _(
                                   "shifts the color balance along the green/magenta axis\n"
                                   "positive values add magenta, negative values add green"));
  dt_gui_box_add(page, dt_ui_section_label_new(C_("section", "tone")));
  w = dt_bauhaus_slider_from_params(self, "contrast");
  gtk_widget_set_tooltip_text(w, _("steepen or flatten the inversion curve about the middle of the density range.\n"
                                   "unlike the looks, this changes the conversion itself: it scales the density "
                                   "range before the tone profile, so everything after it responds to the new "
                                   "gradient. at +100 the gradient is doubled and the outer quarter of the range "
                                   "is pushed past black and white; at -100 it is halved"));
  w = dt_bauhaus_slider_from_params(self, "whites");
  gtk_widget_set_tooltip_text(w, _(
                                   "brightness of the brightest tones\n"
                                   "positive values brighten, negative values darken"));
  w = dt_bauhaus_slider_from_params(self, "lights");
  gtk_widget_set_tooltip_text(w, _(
                                   "brightness of the upper midtones\n"
                                   "positive values brighten, negative values darken"));
  w = dt_bauhaus_slider_from_params(self, "darks");
  gtk_widget_set_tooltip_text(w, _(
                                   "brightness of the lower midtones\n"
                                   "positive values brighten, negative values darken"));
  w = dt_bauhaus_slider_from_params(self, "blacks");
  gtk_widget_set_tooltip_text(w, _(
                                   "brightness of the deepest tones\n"
                                   "positive values lift, negative values deepen"));
  dt_gui_box_add(page, dt_ui_section_label_new(C_("section", "clipping")));
  w = dt_bauhaus_slider_from_params(self, "white_clip");
  gtk_widget_set_tooltip_text(w, _("moves the white end of the density range before the tone profile:\n"
                                   "positive values clip more highlights, negative values give range back"));
  w = dt_bauhaus_slider_from_params(self, "black_clip");
  gtk_widget_set_tooltip_text(w, _("moves the black end of the density range before the tone profile:\n"
                                   "positive values clip more shadows, negative values give range back"));
  w = dt_bauhaus_slider_from_params(self, "highlight_rolloff");
  dt_bauhaus_slider_set_format(w, "%");
  gtk_widget_set_tooltip_text(w, _("compress the brightest tones smoothly toward white instead of clipping them.\n"
                                   "higher values start the rolloff earlier and recover more highlights, "
                                   "at the cost of dimming the tones near white; "
                                   "negative white clip gives that range back.\n"
                                   "0 clips hard"));
  g->shadow_rolloff = dt_bauhaus_slider_from_params(self, "shadow_rolloff");
  dt_bauhaus_slider_set_format(g->shadow_rolloff, "%");
  gtk_widget_set_tooltip_text(g->shadow_rolloff, _("compress the darkest tones smoothly toward black instead of "
                                                   "clipping them.\n"
                                                   "higher values start the rolloff earlier and recover more shadow "
                                                   "detail, at the cost of lifting the tones near black.\n"
                                                   "0 clips hard. only the linear tone profile goes below black, "
                                                   "so it has no effect on the others"));
  w = dt_bauhaus_combobox_from_params(self, "clip_method");
  gtk_widget_set_tooltip_text(w, _(
                                   "what happens to values above white after the adjustments and rolloff\n"
                                   "color protected: scale all channels together, keeping hue\n"
                                   "per channel: clip each channel on its own, which can shift hue"));

  // page COLOR
  page = self->widget = dt_ui_notebook_page(g->notebook, N_("color"), NULL);
  dt_gui_box_add(page, dt_ui_section_label_new(C_("section", "ranges")));
  w = dt_bauhaus_slider_from_params(self, "shadows_range");
  gtk_widget_set_tooltip_text(w, _(
                                   "how far into the tonal range the shadows color balance reaches\n"
                                   "higher values include more of the midtones"));
  w = dt_bauhaus_slider_from_params(self, "highs_range");
  gtk_widget_set_tooltip_text(w, _(
                                   "how far into the tonal range the highlights color balance reaches\n"
                                   "higher values include more of the midtones"));
  dt_gui_box_add(page, dt_ui_section_label_new(C_("section", "shadows")));
  w = dt_bauhaus_slider_from_params(self, "red_cyan_shadows");
  dt_bauhaus_slider_set_stop(w, 0.0, 1, 0.25, 0.25);
  dt_bauhaus_slider_set_stop(w, 0.5, 0.5, 0.5, 0.5);
  dt_bauhaus_slider_set_stop(w, 1.0, 0.25, 1, 1);
  gtk_widget_set_tooltip_text(w, _(
                                   "color balance of the shadows\n"
                                   "negative values add red, positive values add cyan"));
  w = dt_bauhaus_slider_from_params(self, "green_magenta_shadows");
  dt_bauhaus_slider_set_stop(w, 0.0, 0.25, 1, 0.25);
  dt_bauhaus_slider_set_stop(w, 0.5, 0.5, 0.5, 0.5);
  dt_bauhaus_slider_set_stop(w, 1.0, 1, 0.25, 1);
  gtk_widget_set_tooltip_text(w, _(
                                   "color balance of the shadows\n"
                                   "negative values add green, positive values add magenta"));
  w = dt_bauhaus_slider_from_params(self, "blue_yellow_shadows");
  dt_bauhaus_slider_set_stop(w, 0.0, 0.3, 0.3, 1);
  dt_bauhaus_slider_set_stop(w, 0.5, 0.5, 0.5, 0.5);
  dt_bauhaus_slider_set_stop(w, 1.0, 1, 1, 0.25);
  gtk_widget_set_tooltip_text(w, _(
                                   "color balance of the shadows\n"
                                   "negative values add blue, positive values add yellow"));
  dt_gui_box_add(page, dt_ui_section_label_new(C_("section", "midtones")));
  w = dt_bauhaus_slider_from_params(self, "red_cyan_mids");
  dt_bauhaus_slider_set_stop(w, 0.0, 1, 0.25, 0.25);
  dt_bauhaus_slider_set_stop(w, 0.5, 0.5, 0.5, 0.5);
  dt_bauhaus_slider_set_stop(w, 1.0, 0.25, 1, 1);
  gtk_widget_set_tooltip_text(w, _(
                                   "color balance of the midtones\n"
                                   "negative values add red, positive values add cyan"));
  w = dt_bauhaus_slider_from_params(self, "green_magenta_mids");
  dt_bauhaus_slider_set_stop(w, 0.0, 0.25, 1, 0.25);
  dt_bauhaus_slider_set_stop(w, 0.5, 0.5, 0.5, 0.5);
  dt_bauhaus_slider_set_stop(w, 1.0, 1, 0.25, 1);
  gtk_widget_set_tooltip_text(w, _(
                                   "color balance of the midtones\n"
                                   "negative values add green, positive values add magenta"));
  w = dt_bauhaus_slider_from_params(self, "blue_yellow_mids");
  dt_bauhaus_slider_set_stop(w, 0.0, 0.3, 0.3, 1);
  dt_bauhaus_slider_set_stop(w, 0.5, 0.5, 0.5, 0.5);
  dt_bauhaus_slider_set_stop(w, 1.0, 1, 1, 0.25);
  gtk_widget_set_tooltip_text(w, _(
                                   "color balance of the midtones\n"
                                   "negative values add blue, positive values add yellow"));
  dt_gui_box_add(page, dt_ui_section_label_new(C_("section", "highlights")));
  w = dt_bauhaus_slider_from_params(self, "red_cyan_highs");
  dt_bauhaus_slider_set_stop(w, 0.0, 1, 0.25, 0.25);
  dt_bauhaus_slider_set_stop(w, 0.5, 0.5, 0.5, 0.5);
  dt_bauhaus_slider_set_stop(w, 1.0, 0.25, 1, 1);
  gtk_widget_set_tooltip_text(w, _(
                                   "color balance of the highlights\n"
                                   "negative values add red, positive values add cyan"));
  w = dt_bauhaus_slider_from_params(self, "green_magenta_highs");
  dt_bauhaus_slider_set_stop(w, 0.0, 0.25, 1, 0.25);
  dt_bauhaus_slider_set_stop(w, 0.5, 0.5, 0.5, 0.5);
  dt_bauhaus_slider_set_stop(w, 1.0, 1, 0.25, 1);
  gtk_widget_set_tooltip_text(w, _(
                                   "color balance of the highlights\n"
                                   "negative values add green, positive values add magenta"));
  w = dt_bauhaus_slider_from_params(self, "blue_yellow_highs");
  dt_bauhaus_slider_set_stop(w, 0.0, 0.3, 0.3, 1);
  dt_bauhaus_slider_set_stop(w, 0.5, 0.5, 0.5, 0.5);
  dt_bauhaus_slider_set_stop(w, 1.0, 1, 1, 0.25);
  gtk_widget_set_tooltip_text(w, _(
                                   "color balance of the highlights\n"
                                   "negative values add blue, positive values add yellow"));

  // page FINISH
  page = self->widget = dt_ui_notebook_page(g->notebook, N_("finish"), NULL);
  w = dt_bauhaus_slider_from_params(self, "saturation");
  gtk_widget_set_tooltip_text(w, _(
                                   "color saturation of the result\n"
                                   "5 leaves the colors as converted, 0 is monochrome, 10 doubles the saturation"));
  w = dt_bauhaus_combobox_from_params(self, "look");
  gtk_widget_set_tooltip_text(w, _(
                                   "overall rendering of contrast, saturation and color cast\n"
                                   "frontier: punchy contrast, saturated, slightly warm\n"
                                   "natural: no adjustment\n"
                                   "crystal: high contrast and saturation, slightly cool\n"
                                   "pakon: soft contrast and saturation"));
  w = dt_bauhaus_combobox_from_params(self, "toning");
  gtk_widget_set_tooltip_text(w, _(
                                   "standard: neutral highlights and shadows\n"
                                   "vintage: warm highlights and slightly purple shadows, as in aged prints"));
  w = dt_bauhaus_slider_from_params(self, "lab_fade");
  gtk_widget_set_tooltip_text(w, _(
                                   "raises the black point for a faded, matte print look\n"
                                   "negative values deepen the blacks"));
  w = dt_bauhaus_slider_from_params(self, "lab_glow");
  gtk_widget_set_tooltip_text(w, _(
                                   "adds a soft glow to the highlights\n"
                                   "negative values darken them"));

  self->widget = dt_gui_vbox();
  dt_gui_box_add(self->widget, g->notebook);

  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED, _preview_pipe_finished_callback);
}

static void _update_profile_sensitivity(dt_iop_module_t *self)
{
  dt_iop_negconvert_gui_data_t *g = self->gui_data;
  const dt_iop_negconvert_params_t *p = self->params;
  gtk_widget_set_visible(g->auto_density, _has_auto_density(p->tone_profile));
  gtk_widget_set_sensitive(g->shadow_rolloff, p->tone_profile == DT_NEGCONVERT_TONE_LINEAR);
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_negconvert_gui_data_t *g = self->gui_data;

  // redo the analysis of the area already picked with the new percentiles
  if(w == g->analyze_black || w == g->analyze_white) _apply_analysis(self);
  _update_profile_sensitivity(self);
}

void gui_update(dt_iop_module_t *const self)
{
  dt_iop_negconvert_gui_data_t *g = self->gui_data;
  dt_gui_update_collapsible_section(&g->density_section);
  _update_profile_sensitivity(self);
  dt_iop_color_picker_reset(self, TRUE);
}

void gui_reset(dt_iop_module_t *self)
{
  dt_iop_color_picker_reset(self, TRUE);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
