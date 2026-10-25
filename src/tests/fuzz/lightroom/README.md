# Lightroom import fuzzing

A libFuzzer harness for the Lightroom import in `src/develop/lightroom.c`:
the XMP parsing (`_lr_parse()`, including the RetouchAreas and RetouchInfo
spot parsers) and the conversions to crop, ashift, exposure, tone curve,
vignette and retouch params and masks (`_lr_crop_to_dt()`,
`_lr_exposure_to_dt()`, `_lr_tonecurve_to_dt()`, `_lr_vignette_to_dt()`,
`_lr_retouch_to_dt()`). The harness includes `lightroom.c` so the static
helpers are callable, and `libdarktable` resolves everything else. The
database is never touched: the parser runs with a zeroed develop, which
skips the metadata, tag and rating branches that write to it, and only the
history and masks writes are left outside the helpers.

It needs this checkout configured and built with ninja in `build/` (set
`BUILD` for another directory): it links against `build/bin/libdarktable.so`
and reuses the flags `lightroom.c` is compiled with there.

```sh
./run.sh                         # build and fuzz until stopped
./run.sh -max_total_time=300     # extra arguments go to libFuzzer
./mutate.sh                      # check that the harness catches known bugs
```

Both scripts enter `nix-shell` themselves when run outside it. `shell.nix`
takes darktable's dependencies from nixpkgs, plus clang and compiler-rt for
libFuzzer.

The first input byte selects a mode:

| mode | what is checked |
| ---- | --------------- |
| crop oracle | picks a crop in the rotated output frame and maps it back into Lightroom's sensor coordinates with ashift's own rotation matrices (copied from `_homography()` in `src/iop/ashift.c`); `_lr_crop_to_dt()` must reproduce it after an independent EXIF orientation mapping, within two pixels, and give the cropped frame's width/height upright, as the vignette sees it. The ashift params must be neutral apart from the rotation, which must equal the Lightroom angle |
| crop robustness | arbitrary floats and ints, including NaN, inf, zero and negative sizes, run under ASan/UBSan; finite, in-range input must give finite output |
| tone curve | a curve that changes nothing (no parametric adjustments and no point curve, or one on the diagonal) is never added; an added curve has 2 to 20 nodes, at least one off the diagonal, and the module's current defaults; a point curve alone gives its points as nodes, strictly increasing in x when Lightroom could have written them; with parametric adjustments there are 20 evenly spaced nodes, within [0, 1] for input Lightroom could write, and with no point curve the curve never falls, and stays on or above the diagonal when no region is negative (on or below when none is positive) |
| exposure | the result is the module defaults (or the documented fallback) plus Lightroom's exposure and blacks, with every other field untouched; the blacks, grain and clarity tables map any value within their range, holding values past their ends at the last entry |
| vignette | any amount maps within the module's brightness range, darkening for negative amounts and never falling as the amount rises, and -40 keeps the brightness Lightroom's renders were fitted to; the falloff width stays within the fitted range; for Lightroom's ranges the scale is finite, a later midpoint moves the falloff out, every image and crop shape, panoramas included, gives a width/height ratio the module can take, and without roundness the vignette has the shape of the cropped frame or, without a crop, of the image turned upright by its EXIF orientation (scale is not bounded: past the 200 its slider shows, `process()` only fades the vignette further out) |
| retouch | spots as the parser could leave them, up to the 300 retouch holds, on rawspeed-cropped images of any size: one shape per spot plus a clone group holding them all with each spot's opacity, matching `rt_forms` entries with distinct ids, circles and brushes as the spot calls for, and for Lightroom's ranges every center, corner, source, radius, border and hardness at the position and size computed independently, and brush control points those of a Catmull-Rom spline through the corners |
| XMP | documents generated with vignette and split toning settings and spots in RetouchAreas, RetouchInfo or both, in either order, using every layout RDF allows (properties as attributes or child elements, list items with or without an `rdf:Description`), with clone, heal, Remove tool and unknown spots, missing sources, inactive, inverted and zero-valued masks, ellipse, paint and other masks, and repeated or malformed dabs. The spots the parser must read are worked out while generating, and the parsed ones must match exactly; then every conversion runs on the result |
| raw XMP | arbitrary bytes as the XMP, seeded with the Lightroom sidecars of the reference set, run through the parser and every conversion under ASan, UBSan and LeakSanitizer; whatever the input, the parsed spots must be usable |

`run.sh` and `mutate.sh` pass libFuzzer `lightroom.dict`, XMP tokens for the
raw XMP mode, and allow inputs up to 32 kB so the seed sidecars fit.

The checks share two assumptions with the importer rather than testing them:
Lightroom's crop corners are in sensor orientation, and `CropAngle` reaches
`_lr_crop_to_dt()` with the sign the parser in `_lrop()` gives it. The angle's
sign against real Lightroom output is not covered; it was checked once with
darktable-cli renders against the old clipping module. Likewise the spot
coordinate spaces and the fitted vignette, split toning and feather
constants were checked against Lightroom renders, not by the harness.

GraphicsMagick, linked in through libdarktable, installs signal handlers that
stop libFuzzer from saving crashing inputs, so `LLVMFuzzerInitialize()`
resets them.
