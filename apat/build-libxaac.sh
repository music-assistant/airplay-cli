#!/bin/sh
# Builds the libxaac AAC-ELD encoder (Apache-2.0) as a static library plus the xeld.c wrapper,
# then builds cliairplay with the APAT/AAC-ELD path enabled. Linux (tested on Debian 13, x86_64).
#   sh apat/build-libxaac.sh        # needs git, gcc, make, libssl-dev
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
WORK=${WORK:-$ROOT/build-xaac}
XAAC_REPO=https://github.com/ittiam-systems/libxaac.git
XAAC_REV=${XAAC_REV:-2fbadd5}

mkdir -p "$WORK"; cd "$WORK"
[ -d libxaac ] || git clone -q "$XAAC_REPO" libxaac
cd libxaac
git checkout -q -f "$XAAC_REV"
git apply "$HERE/libxaac-noldsbr.patch"   # HomePods stay silent if libxaac turns on LD-SBR for ELD
rm -rf bx; mkdir bx
F="-D_X86_ -DX86 -O3 -fwrapv -fPIC -DLOUDNESS_LEVELING_SUPPORT -Iencoder -Itest/encoder -Iencoder/drc_src -Icommon"
SRCS=$(sed -n 's/.*"\${XAAC_ROOT}\/\(.*\.c\)".*/\1/p' encoder/libxaacenc.cmake)
for s in $SRCS common/*.c encoder/drc_src/*.c; do
  gcc $F -c "$s" -o "bx/$(echo "$s" | tr / _).o"
done
gcc $F -c "$HERE/xeld.c" -o bx/xeld.o
ar rcs bx/libxaacenc.a $(ls bx/*.o | grep -v xeld.o)

cd "$ROOT"
make -j"$(nproc)" EXTRA_CFLAGS="-DAP2_ELD -DAP2_SPF=480 -Wno-error" \
     EXTRA_LDFLAGS="$WORK/libxaac/bx/xeld.o $WORK/libxaac/bx/libxaacenc.a -lm"
