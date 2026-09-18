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

FROM debian:bookworm-slim

ARG TOOLCHAIN_TGZ=arm-linux-musleabi-cross.tgz
ARG TOOLCHAIN_SHA256=d70c607101fee5330083463feacf5992892a85b826d1626094500e0c37ec7d25

ARG FREETYPE_TGZ=freetype-2.13.3.tar.xz
ARG FREETYPE_SHA256=0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289

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

WORKDIR /io

CMD ["/bin/bash"]