/*
 * crsetup.h — generated build config for the CREngine spike.
 *
 * Hand-written equivalent of what koreader's CMake produces from
 * crengine/include/crsetup.h.cmake, with every optional feature disabled
 * except the EPUB-to-RGB565 path:
 *
 *   USE_ZLIB=1           EPUB is a ZIP container (LVZipStream inflate).
 *   USE_FREETYPE=1       CJK-free TTF text via the pinned /opt/freetype.
 *   all image codecs=0   (png/jpeg/gif/webp/svg/stb) — no images needed.
 *   shaping=0            (harfbuzz/fribidi/unibreak/utf8proc) — Latin/Cyrillic.
 *   zstd=0, fontconfig=0, chm=0, antiword=0, srell=0, md4c=0, mathml=0.
 *
 * Other settings match crsetup.h.cmake defaults: COLOR_BACKBUFFER=1 so the
 * engine renders into an RGB buffer (we wrap our own RGB565 memory via
 * LVColorDrawBuf(600,800,buf,16)); CR_USE_THREADS=0 (single-threaded app);
 * LDOM_USE_OWN_MEM_MAN=1; LFS disabled (few-MB test files, 32-bit off_t).
 *
 * This file is copied to the crengine include dir at toolchain build time so
 * every .cpp picks it up through `#include "crsetup.h"`.
 */
#ifndef CRE_CROSSNOOK_CRSETUP_H
#define CRE_CROSSNOOK_CRSETUP_H

/// Yes, even on macOS (Windows is unsupported)…
#define LINUX 1
#define _LINUX 1

/// Compression.
#define USE_ZLIB                             1
#define USE_ZSTD                             0

/// Documents cache.
#define DOCUMENT_CACHING_MIN_SIZE            0x10000   //  64.0 KiB
#define DOCUMENT_CACHING_SIZE_THRESHOLD      0x100000  //   1.0 MiB

/// Document formats.
#define CHM_SUPPORT_ENABLED                  0
#define ENABLE_ANTIWORD                      0
#define USE_MD4C                             0

/// Images.
#define ARBITRARY_IMAGE_SCALE_ENABLED        1
#define MAX_IMAGE_SCALE_MUL                  2
#define USE_GIF                              0
#define USE_LIBJPEG                          0
#define USE_LIBPNG                           0
#define USE_LIBWEBP                          0
#define USE_LUNASVG                          0
#define USE_NANOSVG                          0
#define USE_STB_IMAGE                        0 // only used with USE_NANOSVG==1

/// Miscellaneous.
#define MATHML_SUPPORT                       0
#define USE_SRELL_REGEX                      0

/// Output buffer.
#define COLOR_BACKBUFFER                     1
#define CR_INTERNAL_PAGE_ORIENTATION         1
#define GRAY_BACKBUFFER_BITS                 2
#define GRAY_INVERSE                         0

/// Streams.
/* DISABLE_CLOEXEC undefined (musl supports cloexec) */
/* HAVE_OFF64_T undefined (DISABLE_LFS): 32-bit off_t, files << 2 GiB */
#define LVLONG_FILE_SUPPORT                  0
#define USE_ANSI_FILES                       0
#define FILE_STREAM_BUFFER_SIZE              0x20000   // 128.0 KiB
#define ZIP_STREAM_BUFFER_SIZE               0x40000   // 256.0 KiB

/// System.
#define CR_USE_THREADS                       0
#define LDOM_USE_OWN_MEM_MAN                 1

/// Text.
#define USE_LIMITED_FONT_SIZES_SET           0
#define USE_BITMAP_FONTS                     0
#define USE_WIN32_FONTS                      0
#define USE_GLYPHCACHE_HASHTABLE             0
#define GLYPH_CACHE_SIZE                     0x40000   // 256.0 KiB
#define ALLOW_KERNING                        1
#define USE_FONTCONFIG                       0
#define USE_FREETYPE                         1
#define USE_FRIBIDI                          0
#define USE_HARFBUZZ                         0
#define USE_LIBUNIBREAK                      0
#define USE_UTF8PROC                         0

/// Disable unused code.
#define CR_ENABLE_PAGE_IMAGE_CACHE           0

#endif//CRE_CROSSNOOK_CRSETUP_H