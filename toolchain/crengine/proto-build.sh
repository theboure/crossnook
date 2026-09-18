#!/usr/bin/env bash
# Throwaway prototype: cross-compile CREngine (pinned b05cf007) with musl-g++ 11.
# Proves the koreader CRE_SRCS closure compiles+links static/non-PIE for ARM.
set -euo pipefail
export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
export CC=arm-linux-musleabi-gcc
export CXX=arm-linux-musleabi-g++
export AR=arm-linux-musleabi-ar
export RANLIB=arm-linux-musleabi-ranlib
export NM=arm-linux-musleabi-nm
W=/tmp/creproto
rm -rf "$W"; mkdir -p "$W"
RS=/io/work/crengine
ZS=/io/work/zlib
XS=/io/work/xxhash

echo "== extract sources =="
mkdir -p "$W/src"
tar -xzf "$RS/crengine-b05cf007.tar.gz" -C "$W/src"
mkdir -p "$W/cfg"
cp /io/toolchain/crengine/crsetup.h "$W/cfg/crsetup.h"

echo "== build zlib static =="
tar -xzf "$ZS/zlib-1.3.1.tar.gz" -C "$W"
( cd "$W/zlib-1.3.1" \
  && ./configure --static --prefix="$W/zt" >/dev/null \
  && make -j"$(nproc)" libz.a >/dev/null \
  && mkdir -p "$W/zt/include" "$W/zt/lib" \
  && cp libz.a "$W/zt/lib/" && cp zlib.h zconf.h "$W/zt/include/" )

echo "== build xxhash static =="
tar -xzf "$XS/xxHash-0.8.2.tar.gz" -C "$W"
( cd "$W/xxHash-0.8.2" \
  && make libxxhash.a >/dev/null \
  && mkdir -p "$W/xx/include" "$W/xx/lib" \
  && cp libxxhash.a "$W/xx/lib/" && cp xxhash.h "$W/xx/include/" )

echo "== create object list (koreader CRE_SRCS) =="
SRCS="$W/src/crengine/qimagescale/qimagescale.cpp
$W/src/crengine/src/chmfmt.cpp
$W/src/crengine/src/cp_stats.cpp
$W/src/crengine/src/crtxtenc.cpp
$W/src/crengine/src/docxfmt.cpp
$W/src/crengine/src/drawutil.cpp
$W/src/crengine/src/epubfmt.cpp
$W/src/crengine/src/fb3fmt.cpp
$W/src/crengine/src/hist.cpp
$W/src/crengine/src/hyphman.cpp
$W/src/crengine/src/lstridmap.cpp
$W/src/crengine/src/lvdocview.cpp
$W/src/crengine/src/lvdrawbuf.cpp
$W/src/crengine/src/lvfnt.cpp
$W/src/crengine/src/lvfntman.cpp
$W/src/crengine/src/lvimg.cpp
$W/src/crengine/src/lvmemman.cpp
$W/src/crengine/src/lvopc.cpp
$W/src/crengine/src/lvpagesplitter.cpp
$W/src/crengine/src/lvrend.cpp
$W/src/crengine/src/lvstream.cpp
$W/src/crengine/src/lvstring.cpp
$W/src/crengine/src/lvstsheet.cpp
$W/src/crengine/src/lvstyles.cpp
$W/src/crengine/src/lvtextfm.cpp
$W/src/crengine/src/lvtinydom.cpp
$W/src/crengine/src/lvxml.cpp
$W/src/crengine/src/mathml.cpp
$W/src/crengine/src/mdfmt.cpp
$W/src/crengine/src/odtfmt.cpp
$W/src/crengine/src/odxutil.cpp
$W/src/crengine/src/pdbfmt.cpp
$W/src/crengine/src/props.cpp
$W/src/crengine/src/renderutil.cpp
$W/src/crengine/src/rtfimp.cpp
$W/src/crengine/src/textlang.cpp
$W/src/crengine/src/txtselector.cpp
$W/src/crengine/src/wordfmt.cpp"

echo "== compile =="
mkdir -p "$W/obj"
CFLAGS=-include\ stdint.h
n=0
for s in $SRCS; do
  o="$W/obj/$(basename "${s%.*}.o")"
  $CXX -std=c++17 -O2 -Wall -Wextra -include stdint.h \
    -I"$W/cfg" -I"$W/src" -I"$W/src/crengine/include" \
    -I/opt/freetype/include/freetype2 \
    -I"$W/zt/include" -I"$W/xx/include" \
    -c "$s" -o "$o"
  n=$((n+1))
done
echo "compiled $n objects (expected $(echo "$SRCS" | wc -w))"

echo "== archive =="
arm-linux-musleabi-ar rcs "$W/libcrengine.a" "$W/obj"/*.o
echo "libcrengine.a: $(du -h "$W/libcrengine.a" | cut -f1)"

echo "== cache libs for iteration =="
mkdir -p /io/build/creproto/include /io/build/creproto/font
cp "$W/libcrengine.a" "$W/zt/lib/libz.a" "$W/xx/lib/libxxhash.a" /io/build/creproto/
cp -r "$W/src/crengine/include/." /io/build/creproto/include/
cp /io/toolchain/crengine/crsetup.h /io/build/creproto/include/crsetup.h
cp /io/testapp/test-font.ttf /io/build/creproto/font/ 2>/dev/null || true

echo "== link smoke =="
cat > "$W/t.c" <<'EOF'
#include <stdio.h>
#include <stdint.h>
#include "lvdocview.h"
#include "lvdrawbuf.h"
int main() {
    InitFontManager(lString8(""));
    lUInt8 *mem = new lUInt8[600*800*2];
    LVColorDrawBuf *buf = new LVColorDrawBuf(600, 800, mem, 16);
    buf->Clear(0xFFFF);
    buf->FillRect(0, 0, 600, 50, 0x0000);
    return (buf->GetWidth()==600 && buf->GetRowSize()==1200) ? 0 : 1;
}
EOF
$CXX -std=c++17 -static -no-pie -fno-pie \
  -I"$W/cfg" -I"$W/src/crengine/include" -I/opt/freetype/include/freetype2 \
  "$W/t.c" -o "$W/cresmoke" \
  "$W/libcrengine.a" /opt/freetype/lib/libfreetype.a "$W/zt/lib/libz.a" "$W/xx/lib/libxxhash.a"
file "$W/cresmoke"
readelf -h "$W/cresmoke" | grep -E "Type|Machine|Flags"
readelf -l "$W/cresmoke" | grep -E "INTERP|Dynamic" && echo "WARN dynamic" || echo "fully static: OK"

echo "== run under qemu =="
Q="qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi"
$Q "$W/cresmoke"
echo PROTO_OK