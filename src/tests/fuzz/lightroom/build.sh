#!/usr/bin/env bash
# build the libFuzzer harness against this checkout's lightroom.c and its
# configured and built tree; run inside `nix-shell`. LIGHTROOM_C swaps in
# another lightroom.c (mutate.sh uses this), BUILD another build directory
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../../../.." && pwd)
BUILD=${BUILD:-$SRC/build}
LIGHTROOM_C=${LIGHTROOM_C:-$SRC/src/develop/lightroom.c}

if [ ! -f "$BUILD/bin/libdarktable.so" ]; then
  echo "no libdarktable.so in $BUILD/bin: build darktable first, or set BUILD" >&2
  exit 1
fi

# reuse the include paths and defines darktable compiles lightroom.c with
CMD=$(ninja -C "$BUILD" -t commands bin/CMakeFiles/lib_darktable.dir/develop/lightroom.c.o | tail -1)
mapfile -t FLAGS < <(tr ' ' '\n' <<< "$CMD" | grep -E '^-(I|D)')
ISYSTEM=()
while read -r d; do ISYSTEM+=(-isystem "$d"); done < <(grep -oE -- '-isystem [^ ]+' <<< "$CMD" | cut -d' ' -f2)

clang -std=gnu99 -g -O1 -fno-omit-frame-pointer \
  -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
  -Wno-unused-function -Wno-unknown-warning-option \
  "${FLAGS[@]}" "${ISYSTEM[@]}" -I"$SRC/src" -I"$BUILD/bin" \
  -DLIGHTROOM_C="\"$LIGHTROOM_C\"" \
  "$HERE/fuzz_lightroom.c" -o "${OUT:-$HERE/fuzz_lightroom}" \
  -L"$BUILD/bin" -ldarktable -Wl,-rpath,"$BUILD/bin" \
  $(pkg-config --libs glib-2.0 gtk+-3.0 libxml-2.0 sqlite3) -lm

echo "built ${OUT:-$HERE/fuzz_lightroom} from $LIGHTROOM_C"
