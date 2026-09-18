#!/usr/bin/env bash
# Iterate crengine smoke app: link against cached libs (build/creproto) and run
# under qemu-arm. Caches are produced by proto-build.sh.
set -euo pipefail
export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
export CC=arm-linux-musleabi-gcc
export CXX=arm-linux-musleabi-g++
OUT=/io/build/creproto
INC=/io/build/creproto/include

cat > /tmp/t.c <<'EOF'
#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>
#include "lvdocview.h"
#include "lvdrawbuf.h"

#define STEP(m) fprintf(stderr, "[crec] %s\n", m)

int main() {
    STEP("start");
    bool ok_fm = InitFontManager(lString8(""));
    STEP("InitFontManager");
    fprintf(stderr, "[crec] InitFontManager -> %d  fontMan=%p\n", ok_fm, (void*)fontMan);
    STEP("before RegisterFont");
    struct stat st;
    int r = stat("/io/build/creproto/font/test-font.ttf", &st);
    fprintf(stderr, "[crec] stat(test-font.ttf) -> %d size=%ld\n", r, r==0?(long)st.st_size:-1);
    bool ok_fr = fontMan->RegisterFont(lString8("/io/build/creproto/font/test-font.ttf"));
    STEP("after RegisterFont");
    fprintf(stderr, "[crec] RegisterFont -> %d\n", ok_fr);
    LVDocView *doc = new LVDocView(16);
    STEP("new LVDocView");
    doc->setViewMode(DVM_PAGES, 1);
    STEP("setViewMode");
    doc->Resize(600, 800);
    STEP("Resize");
    doc->setStyleSheet(lString8("body{margin:0}"), true);
    STEP("setStyleSheet");
    lUInt8 *mem = new lUInt8[600*800*2];
    LVColorDrawBuf *buf = new LVColorDrawBuf(600, 800, mem, 16);
    STEP("new LVColorDrawBuf(external 16bpp)");
    buf->Clear(0xFFFFFF);
    STEP("Clear");
    buf->FillRect(0, 0, 600, 50, 0x000000);
    STEP("FillRect");
    lUInt16 *raw = (lUInt16*)buf->GetScanLine(0);
    uint32_t p0 = raw[1];              /* raw RGB565, inside black band */
    uint32_t p1 = raw[1 + 60*600];     /* raw RGB565, below band */
    STEP("raw read");
    printf("cre smoke: W=%d H=%d bpp=%d row=%d raw(1,1)=%04X raw(1,60)=%04X\n",
        buf->GetWidth(), buf->GetHeight(), buf->GetBitsPerPixel(),
        buf->GetRowSize(), p0, p1);
    int ok = (buf->GetWidth()==600 && buf->GetHeight()==800
              && buf->GetBitsPerPixel()==16 && buf->GetRowSize()==1200
              && p0==0x0000 && p1==0xFFFF) ? 1 : 0;
    delete buf; delete[] mem; delete doc;
    STEP("done");
    printf(ok ? "RGB565 EXTERNAL BUFFER OK\n" : "RGB565 FAIL\n");
    return ok ? 0 : 1;
}
EOF

$CXX -std=c++17 -static -no-pie -fno-pie \
  -I"$INC" -I/opt/freetype/include/freetype2 \
  /tmp/t.c -o /tmp/cresmoke \
  "$OUT/libcrengine.a" /opt/freetype/lib/libfreetype.a "$OUT/libz.a" "$OUT/libxxhash.a"
echo "--- file ---"
file /tmp/cresmoke
readelf -l /tmp/cresmoke | grep -E "INTERP|Dynamic" && echo "WARN dynamic" || echo "fully static: OK"

Q="qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi"
$Q /tmp/cresmoke
echo "exit=$?"