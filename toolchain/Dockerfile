# pinned musl ARM EABI cross toolchain for Linux 2.6.29 targets
#
# toolchain: musl.cc arm-linux-musleabi-cross (gcc 11.2.1, musl ~1.2.2)
# source:    https://musl.cc/arm-linux-musleabi-cross.tgz (mutable URL;
#            the pinned artifact is the local copy in work/toolchain)
# sha256:    d70c607101fee5330083463feacf5992892a85b826d1626094500e0c37ec7d25
# Dev tool (file/readelf/objdump) and qemu-arm are installed for
# host-side validation of produced binaries.

FROM debian:bookworm-slim

ARG TOOLCHAIN_TGZ=arm-linux-musleabi-cross.tgz
ARG TOOLCHAIN_SHA256=d70c607101fee5330083463feacf5992892a85b826d1626094500e0c37ec7d25

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
      file binutils qemu-user ca-certificates; \
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

WORKDIR /io

CMD ["/bin/bash"]