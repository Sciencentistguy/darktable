#!/usr/bin/env bash
# build the harness against deliberately broken copies of lightroom.c and
# check that each one is caught
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
if [ -z "${IN_NIX_SHELL:-}" ]; then
  exec nix-shell "$HERE/shell.nix" --run "$(printf '%q ' "$0" "$@")"
fi

BASE=$(cd "$HERE/../../../.." && pwd)/src/develop/lightroom.c
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

mutants=(
  "crop rotation sign|s/rangle = deg2rad(-data->pas.rotation);/rangle = deg2rad(data->pas.rotation);/"
  "crop ignores FLIP_X|s/  if(orientation \& ORIENTATION_FLIP_X)/  if(0)/"
  "crop ignores SWAP_XY|s/  if(orientation \& ORIENTATION_SWAP_XY)/  if(0)/"
  "mirror angle inversion|s/^  \/\/ adjust crop data according to the orientation/  if(orientation == ORIENTATION_FLIP_HORIZONTALLY) data->pas.rotation = -data->pas.rotation;\n  \/\/ adjust crop data according to the orientation/"
  "ashift auto-crop left on|s/ASHIFT_CROP_OFF; \/\/ the crop/ASHIFT_CROP_LARGEST; \/\/ the crop/"
  "tonecurve identity gating removed|s/  return !_tonecurve_is_identity(\&data->ptc);/  return TRUE;/"
  "parametric shadows inverted|s/const double shadows = (value\[0\] + 100.0)/const double shadows = (100.0 - value[0])/"
  "parametric highlights inverted|s/const double highlights = (value\[3\] + 100.0)/const double highlights = (100.0 - value[3])/"
  "parametric clamp removed|s/CLAMP(interpolate_val(n_pts, px, y, py, tangents, CUBIC_SPLINE), 0.0f, 1.0f)/interpolate_val(n_pts, px, y, py, tangents, CUBIC_SPLINE)/"
  "tonecurve back to Lab|s/data->ptc.tonecurve_autoscale_ab = LRDT_TONECURVE_AUTOMATIC_RGB;/data->ptc.tonecurve_autoscale_ab = 1;/"
  "exposure ignores blacks|s/  pe.black += data->pe.black;//"
  "exposure ignores defaults|s/    memcpy(\&pe, defaults, sizeof(pe));/    ;/"
  "areas: source offset from each mask|s/    if(isnan(dx))/    if(1)/"
  "areas: inverted masks kept|s/       || _rdf_prop_is(doc, mask, \"MaskInverted\", \"true\")/       || 0/"
  "dabs: repeats kept|s/       \&\& (dabs->len == 0/       \&\& (1 || dabs->len == 0/"
  "dabs: single dab kept as a stroke|s/      if(s.n_dabs == 1)/      if(0)/"
  "RetouchInfo read after RetouchAreas|s/dev != NULL \&\& !data->has_retouch_areas \&\& !xmlStrcmp/dev != NULL \&\& !xmlStrcmp/"
  "RetouchInfo radius unchecked|s/         \&\& !isnan(s->x) \&\& !isnan(s->y) \&\& s->radius > 0.0f)/         )/"
  "brush control points reversed|s/p->ctrl1\[c\] = p->corner\[c\] - (next - prev)/p->ctrl1[c] = p->corner[c] + (next - prev)/"
  "retouch ignores spot opacity|s/grpt->opacity = s->opacity;/grpt->opacity = 1.0f;/"
  "retouch radius scaled by width|s/const float rscale = MAX(cw, ch) \/ MIN(img->width, img->height);/const float rscale = cw \/ img->width;/"
  "retouch source not on the sensor|s/_lr_to_mask(img, s->xc, s->yc, form->source);/form->source[0] = s->xc; form->source[1] = s->yc;/"
  "circle feather ignored|s/      circle->border = radius \* soft;/      circle->border = 0.0f;/"
  "split toning hue over 255|s/data->pst.shadow_hue = (float)v \/ 360.0;/data->pst.shadow_hue = (float)v \/ 255.0;/"
  "split toning balance unclamped|s/return powf(CLAMP(0.5f - value \/ 200.0f, 0.0f, 1.0f), 1.5f);/return powf(0.5f - value \/ 200.0f, 1.5f);/"
  "vignette midpoint ignored|s/+ 0.576f \* (data->vignette_midpoint - 50.0f)/+ 0.0f/"
  "vignette amount unclamped|s/  const float amount = CLAMP(value, -100.0f, 100.0f);/  const float amount = value;/"
  "vignette linear in the frame ratio|s/  data->pv.whratio = frame_ratio <= 1.0f ? frame_ratio : 2.0f - 1.0f \/ frame_ratio;/  data->pv.whratio = frame_ratio;/"
  "vignette ignores orientation|s/      frame_ratio = 1.0f \/ frame_ratio;/      ;/"
  "vignette frame from crop fractions|s/  data->fratio = (cw - cx) \/ (ch - cy);/  data->fratio = (data->pcr.cw - data->pcr.cx) \/ (data->pcr.ch - data->pcr.cy);/"
  "crop frame not upright|s/  if(orientation \& ORIENTATION_SWAP_XY) data->fratio = 1.0f \/ data->fratio;//"
  "table lookups unclamped|s/  const float v = CLAMP(value, lr2dt_table\[0\].lr, lr2dt_table\[n - 1\].lr);/  const float v = value;/"
  "xpath values leaked|s/^              xmlFree(value);$//"
)

fail=0
for m in "${mutants[@]}"; do
  name=${m%%|*}
  expr=${m#*|}
  mkdir -p "$WORK/m/develop"
  sed -e "$expr" "$BASE" > "$WORK/m/develop/lightroom.c"
  if cmp -s "$BASE" "$WORK/m/develop/lightroom.c"; then
    echo "SKIPPED  $name (pattern did not apply)"; fail=1; continue
  fi
  LIGHTROOM_C="$WORK/m/develop/lightroom.c" OUT="$WORK/fuzz" "$HERE/build.sh" > "$WORK/build.log" 2>&1 \
    || { echo "NO BUILD $name"; tail -5 "$WORK/build.log"; fail=1; continue; }
  if (cd "$WORK" && ./fuzz -max_total_time=60 -max_len=32768 \
        -dict="$HERE/lightroom.dict" -artifact_prefix="$WORK/" "$HERE/corpus" > "$WORK/run.log" 2>&1); then
    echo "MISSED   $name"; fail=1
  else
    echo "caught   $name: $(grep -m1 -A1 'CHECK failed' "$WORK/run.log" | tail -1 | sed 's/^ *//' | cut -c1-90)"
  fi
done
exit $fail
