# pinned musl ARM EABI cross toolchain for Linux 2.6.29 targets
#
# toolchain: musl.cc arm-linux-musleabi-cross (gcc 11.2.1, musl ~1.2.2)
# source:    https://musl.cc/arm-linux-musleabi-cross.tgz (mutable URL;
#            the pinned artifact is the local copy in work/toolchain)
# sha256:    d70c607101fee5330083463feacf5992892a85b826d1626094500e0c37ec7d25
# Dev tool (file/readelf/objdump) and qemu-arm are installed for
# host-side validation of produced binaries.
#
# FreeType 2.13.3 is cross-compiled into a static library installed under
# /opt/freetype (no external deps: zlib/bzip2/png/harfbuzz/brotli all off).
# Source is the pinned local copy in work/freetype/.
# sha256:    0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289
#
# CREngine spike add-ons (see toolchain/crengine/pin.txt for full details):
#   zlib 1.3.1     -> /opt/zlib     (static; EPUB container inflate)
#   xxHash 0.8.2   -> /opt/xxhash   (static; lvtinydom.cpp XXH32)
#   crengine       -> /opt/crengine (static lib + headers; koreader fork)
#     pinned commit b05cf007 (master, 2026-09-10), GPL-2.0
#     compile config: toolchain/crengine/crsetup.h (copied into the include
#     tree). Feature macros mirror koreader's cmake output but with every
#     optional codec/shaping feature off: only zlib+freetype+xxhash enabled.
# A font (testapp/test-font.ttf) is copied to /opt/test-font.ttf so host
# validation can register a real TTF inside the image.
#
# BearSSL 0.6 (TLS 1.2, MIT) -> /opt/bearssl (static lib + inc/*.h).
# HTTPS transport foundation. See toolchain/bearssl/pin.txt for rationale,
# licensing and source provenance (Debian re-host of vanilla 0.6).
#     sha256:  102a625bd8f6155260867a50c508df8d8c3f796985dd744df1a68e5cd1a79004

FROM debian:bookworm-slim

ARG TOOLCHAIN_TGZ=arm-linux-musleabi-cross.tgz
ARG TOOLCHAIN_SHA256=d70c607101fee5330083463feacf5992892a85b826d1626094500e0c37ec7d25

ARG FREETYPE_TGZ=freetype-2.13.3.tar.xz
ARG FREETYPE_SHA256=0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289

ARG ZLIB_TGZ=zlib-1.3.1.tar.gz
ARG ZLIB_SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23

ARG XXHASH_TGZ=xxHash-0.8.2.tar.gz
ARG XXHASH_SHA256=baee0c6afd4f03165de7a4e67988d16f0f2b257b51d0e3cb91909302a26a79c4

ARG CRENGINE_TGZ=crengine-b05cf007.tar.gz
ARG CRENGINE_SHA256=7deeb72a1306cff1d71295d6948db6fdc784af46c1131a05684e8d2694747c14

ARG BEARSSL_TGZ=bearssl_0.6+dfsg.1.orig.tar.xz
ARG BEARSSL_SHA256=102a625bd8f6155260867a50c508df8d8c3f796985dd744df1a68e5cd1a79004

LABEL org.crossnook.crengine-revision=b05cf00791d2ba17c1f077fc69e7bd68cf780b6a
LABEL org.crossnook.bearssl-revision=0.6

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
      file binutils qemu-user ca-certificates \
      make xz-utils gcc libc6-dev; \
    rm -rf /var/lib/apt/lists/*

COPY work/toolchain/${TOOLCHAIN_TGZ} /opt/${TOOLCHAIN_TGZ}

RUN set -eux; \
    echo "${TOOLCHAIN_SHA256}  /opt/${TOOLCHAIN_TGZ}" | sha256sum -c -; \
    mkdir -p /opt/tc; \
    tar -xzf /opt/${TOOLCHAIN_TGZ} -C /opt/tc; \
    rm /opt/${TOOLCHAIN_TGZ}; \
    chmod -R ugo+rX /opt/tc

ENV PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
ENV CROSS_PREFIX=arm-linux-musleabi-

COPY work/freetype/${FREETYPE_TGZ} /opt/${FREETYPE_TGZ}

RUN set -eux; \
    echo "${FREETYPE_SHA256}  /opt/${FREETYPE_TGZ}" | sha256sum -c -; \
    tar -xJf /opt/${FREETYPE_TGZ} -C /opt; \
    rm /opt/${FREETYPE_TGZ}; \
    cd /opt/freetype-2.13.3; \
    ./configure \
      --host=arm-linux-musleabi \
      --build=x86_64-linux-gnu \
      --prefix=/opt/freetype \
      --disable-shared \
      --enable-static \
      --without-zlib --without-bzip2 --without-png \
      --without-harfbuzz --without-brotli \
      CC=arm-linux-musleabi-gcc; \
    make -j"$(nproc)" all; \
    make install; \
    rm -rf /opt/freetype-2.13.3; \
    printf '#include <ft2build.h>\n#include FT_FREETYPE_H\nint main(){FT_Library l;if(FT_Init_FreeType(&l))return 1;return FT_Done_FreeType(l);}\n' \
      > /tmp/ftchk.c; \
    arm-linux-musleabi-gcc -static -I/opt/freetype/include/freetype2 \
      -o /tmp/ftchk /tmp/ftchk.c -L/opt/freetype/lib -lfreetype; \
    file /tmp/ftchk | grep -q "ARM" && echo "freetype static link: ARM OK"; \
    rm -f /tmp/ftchk /tmp/ftchk.c

# zlib 1.3.1 (static, EABI5). EPUB is a ZIP container; LVZipStream uses inflate.
COPY work/zlib/${ZLIB_TGZ} /opt/${ZLIB_TGZ}

RUN set -eux; \
    echo "${ZLIB_SHA256}  /opt/${ZLIB_TGZ}" | sha256sum -c -; \
    tar -xzf /opt/${ZLIB_TGZ} -C /opt; \
    rm /opt/${ZLIB_TGZ}; \
    cd /opt/zlib-1.3.1; \
    CC=arm-linux-musleabi-gcc \
    AR=arm-linux-musleabi-ar \
    RANLIB=arm-linux-musleabi-ranlib \
    NM=arm-linux-musleabi-nm \
    ./configure --static --prefix=/opt/zlib; \
    make -j"$(nproc)" libz.a; \
    make install; \
    rm -rf /opt/zlib-1.3.1; \
    test -s /opt/zlib/lib/libz.a

# xxHash 0.8.2 (static). lvtinydom.cpp calls XXH32 unconditionally.
COPY work/xxhash/${XXHASH_TGZ} /opt/${XXHASH_TGZ}

RUN set -eux; \
    echo "${XXHASH_SHA256}  /opt/${XXHASH_TGZ}" | sha256sum -c -; \
    tar -xzf /opt/${XXHASH_TGZ} -C /opt; \
    rm /opt/${XXHASH_TGZ}; \
    cd /opt/xxHash-0.8.2; \
    make libxxhash.a CC=arm-linux-musleabi-gcc AR=arm-linux-musleabi-ar; \
    mkdir -p /opt/xxhash/lib /opt/xxhash/include; \
    cp libxxhash.a /opt/xxhash/lib/; \
    cp xxhash.h /opt/xxhash/include/; \
    rm -rf /opt/xxHash-0.8.2

# CREngine (koreader fork @ pinned b05cf007) as a static library.
# Source list = koreader's CRE_SRCS (their production-tested closure);
# flags use -include stdint.h because koreader relies on -D_GNU_SOURCE to
# make uint8_t visible under strict -std, which musl does not replicate.
COPY work/crengine/${CRENGINE_TGZ} /opt/${CRENGINE_TGZ}
COPY toolchain/crengine/crsetup.h /opt/crengine-cfg/crsetup.h
COPY testapp/test-font.ttf /opt/test-font.ttf

RUN set -eux; \
    echo "${CRENGINE_SHA256}  /opt/${CRENGINE_TGZ}" | sha256sum -c -; \
    tar -xzf /opt/${CRENGINE_TGZ} -C /opt; \
    rm /opt/${CRENGINE_TGZ}; \
    mv /opt/crengine /opt/crengine-src; \
    mkdir -p /opt/crengine/lib /opt/crengine/include /opt/crengine-obj; \
    FLAGS="-std=c++17 -O2 -Wall -Wextra -include stdint.h \
           -I/opt/crengine-cfg -I/opt/crengine-src -I/opt/crengine-src/include \
           -I/opt/freetype/include/freetype2 -I/opt/zlib/include -I/opt/xxhash/include"; \
    cd /opt/crengine-obj; \
    for S in \
      qimagescale/qimagescale.cpp \
      src/chmfmt.cpp src/cp_stats.cpp src/crtxtenc.cpp src/docxfmt.cpp \
      src/drawutil.cpp src/epubfmt.cpp src/fb3fmt.cpp src/hist.cpp \
      src/hyphman.cpp src/lstridmap.cpp src/lvdocview.cpp \
      src/lvdrawbuf.cpp src/lvfnt.cpp src/lvfntman.cpp src/lvimg.cpp \
      src/lvmemman.cpp src/lvopc.cpp src/lvpagesplitter.cpp src/lvrend.cpp \
      src/lvstream.cpp src/lvstring.cpp src/lvstsheet.cpp src/lvstyles.cpp \
      src/lvtextfm.cpp src/lvtinydom.cpp src/lvxml.cpp src/mathml.cpp \
      src/mdfmt.cpp src/odtfmt.cpp src/odxutil.cpp src/pdbfmt.cpp \
      src/props.cpp src/renderutil.cpp src/rtfimp.cpp src/textlang.cpp \
      src/txtselector.cpp src/wordfmt.cpp; do \
      arm-linux-musleabi-g++ $FLAGS -c "/opt/crengine-src/$S" -o "$(basename "${S%.*}").o"; \
    done; \
    n=$(ls *.o | wc -l); echo "crengine objects: $n"; test "$n" -eq 38; \
    arm-linux-musleabi-ar rcs /opt/crengine/lib/libcrengine.a *.o; \
    rm -rf /opt/crengine-obj

# Headers: public includes + generated build config, so apps (and header
# quote-includes of "crsetup.h") resolve via -I/opt/crengine/include.
RUN set -eux; \
    cp -r /opt/crengine-src/include/. /opt/crengine/include/; \
    cp /opt/crengine-cfg/crsetup.h /opt/crengine/include/crsetup.h; \
    test -f /opt/crengine/include/lvdocview.h && test -f /opt/crengine/include/crsetup.h

# In-image link+run smoke: external 16bpp RGB565 buffer + LVDocView lifespan +
# style parse + raw RGB565 pixel writes, all under qemu-arm.
RUN set -eux; \
    printf '#include <stdio.h>\n#include <stdint.h>\n#include "lvdocview.h"\n#include "lvdrawbuf.h"\nint main(){ InitFontManager(lString8("")); bool r=fontMan->RegisterFont(lString8("/opt/test-font.ttf")); LVDocView *d=new LVDocView(16); d->setViewMode(DVM_PAGES,1); d->Resize(600,800); d->setStyleSheet(lString8("body{margin:0}"),true); lUInt8 *m=new lUInt8[600*800*2]; LVColorDrawBuf *b=new LVColorDrawBuf(600,800,m,16); b->Clear(0xFFFFFF); b->FillRect(0,0,600,50,0x000000); lUInt16 *r0=(lUInt16*)b->GetScanLine(0); int ok=(r0[1]==0x0000 && r0[1+60*600]==0xFFFF && b->GetRowSize()==1200); delete b; delete[] m; delete d; if(!r) fprintf(stderr,"font register failed\\n"); printf(ok?"IMG CRE RGB565 OK\\n":"IMG CRE FAIL\\n"); return ok?0:1;}\n' > /tmp/creimg.c; \
    arm-linux-musleabi-g++ -std=c++17 -static -no-pie -fno-pie -O2 \
      -I/opt/crengine/include -I/opt/freetype/include/freetype2 \
      /tmp/creimg.c -o /tmp/creimg \
      /opt/crengine/lib/libcrengine.a /opt/freetype/lib/libfreetype.a \
      /opt/zlib/lib/libz.a /opt/xxhash/lib/libxxhash.a; \
    file /tmp/creimg | grep -q "ARM" && echo "crengine static link: ARM OK"; \
    readelf -l /tmp/creimg | grep -qE "INTERP|Dynamic" && { echo "WARN dynamic sections"; exit 1; } || echo "crengine static/non-PIE: OK"; \
    qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi /tmp/creimg; \
    rm -f /tmp/creimg /tmp/creimg.c

# BearSSL 0.6 (TLS 1.2, MIT) as a static library for the HTTPS transport.
# Every src/**/*.c is compiled (x86-specific files self-disable via
# preprocessor guards on non-x86: BR_AES_X86NI, BR_amd64, BR_SSE2, ...).
COPY work/bearssl/${BEARSSL_TGZ} /opt/${BEARSSL_TGZ}

RUN set -eux; \
    echo "${BEARSSL_SHA256}  /opt/${BEARSSL_TGZ}" | sha256sum -c -; \
    tar -xJf /opt/${BEARSSL_TGZ} -C /opt; \
    rm /opt/${BEARSSL_TGZ}; \
    mv /opt/bearssl-0.6 /opt/bearssl-src; \
    mkdir -p /opt/bearssl/lib /opt/bearssl/include /opt/bearssl-obj; \
    cd /opt/bearssl-obj; \
    for S in $(find /opt/bearssl-src/src -name '*.c' | sort); do \
      N=$(basename "${S%.*}" | tr -c 'A-Za-z0-9_' '_')_$(printf '%s' "$S" | sha256sum | cut -c1-12); \
      arm-linux-musleabi-gcc -std=c99 -O2 -Wall -Wextra \
        -I/opt/bearssl-src/inc -I/opt/bearssl-src/src -c "$S" -o "$N.o"; \
    done; \
    n=$(ls *.o | wc -l); echo "bearssl objects: $n"; test "$n" -gt 200; \
    arm-linux-musleabi-ar rcs /opt/bearssl/lib/libbearssl.a *.o; \
    rm -rf /opt/bearssl-obj; \
    cp /opt/bearssl-src/inc/bearssl.h /opt/bearssl-src/inc/bearssl_*.h /opt/bearssl/include/; \
    rm -rf /opt/bearssl-src

# In-image link+run smoke: seed HMAC-DRBG, initialise a full TLS 1.2
# client profile, inject entropy, emit a ClientHello, and run the minimal
# X.509 engine's trust-anchor/name machinery (all under qemu-arm).
RUN set -eux; \
    printf '#include <stdio.h>\n#include <string.h>\n#include <bearssl.h>\nint main(){\n  br_hmac_drbg_context rng; unsigned char seed[32], a[16], b[16], c[16]; int i, ok=1;\n  for(i=0;i<32;i++) seed[i]=(unsigned char)(i*7+1);\n  br_hmac_drbg_init(&rng,&br_sha256_vtable,seed,sizeof seed);\n  br_hmac_drbg_generate(&rng,a,sizeof a);\n  br_hmac_drbg_init(&rng,&br_sha256_vtable,seed,sizeof seed);\n  br_hmac_drbg_generate(&rng,b,sizeof b);\n  br_hmac_drbg_generate(&rng,c,sizeof c);\n  ok = ok && memcmp(a,b,16)==0 && memcmp(b,c,16)!=0;\n  br_ssl_client_context cc; br_x509_minimal_context xc;\n  br_ssl_client_init_full(&cc,&xc,0,0);\n  static unsigned char io[BR_SSL_BUFSIZE_MONO];\n  br_ssl_engine_set_buffer(&cc.eng,io,sizeof io,0);\n  br_ssl_engine_inject_entropy(&cc.eng,c,sizeof c);\n  br_ssl_client_reset(&cc,"example.invalid",0);\n  int err = br_ssl_engine_last_error(&cc.eng);\n  unsigned st = br_ssl_engine_current_state(&cc.eng);\n  ok = ok && err==BR_ERR_OK && (st & BR_SSL_SENDREC)!=0;\n  printf(ok?"BEARSSL ARM SMOKE OK\\n":"BEARSSL ARM SMOKE FAIL\\n");\n  return ok?0:1;\n}\n' > /tmp/brchk.c; \
    arm-linux-musleabi-gcc -std=c99 -static -no-pie -fno-pie -O2 \
      -I/opt/bearssl/include /tmp/brchk.c -o /tmp/brchk \
      /opt/bearssl/lib/libbearssl.a; \
    file /tmp/brchk | grep -q "ARM" && echo "bearssl static link: ARM OK"; \
    readelf -l /tmp/brchk | grep -qE "INTERP|Dynamic" && { echo "WARN dynamic sections"; exit 1; } || echo "bearssl static/non-PIE: OK"; \
    qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi /tmp/brchk; \
    rm -f /tmp/brchk /tmp/brchk.c

WORKDIR /io

CMD ["/bin/bash"]