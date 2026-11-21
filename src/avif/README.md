# Vendored libavif + dav1d — AVIF decode only

`libavif` 1.4.2 + `dav1d` 1.5.5, decode-only subset, vendored 2026-11-19
for AVIF input (slice AVIF decode). Research that picked these versions:
the AVIF/JXL conversion research notes of 19 Nov 2026 (kept locally,
not in the repo).

- libavif source: https://github.com/AOMediaCodec/libavif/releases/tag/v1.4.2
  (tarball `libavif-1.4.2.tar.gz`, 14,665,927 bytes)
- libavif SHA256: `2b645287340ba5a631d268b551dc2d72bd73ac33335962dd36dcdb6d8366921d`
  (verified against the FreeBSD distinfo for graphics/libavif)
- libavif license: BSD-2-Clause, see `libavif/LICENSE`
- dav1d source: https://downloads.videolan.org/pub/videolan/dav1d/1.5.5/dav1d-1.5.5.tar.xz
  (tarball 1,051,484 bytes)
- dav1d SHA256: `ca96b1d0ba3f01f55de7231d775892b64994ba061aaeeaaa4f662a390b3abf9e`
  (verified against the `.sha256` file published next to the tarball)
- dav1d license: BSD-2-Clause, see `dav1d/COPYING`
- Upstream: https://github.com/AOMediaCodec/libavif, https://code.videolan.org/videolan/dav1d

## Why vendored, not the system library

- The VM has runtime `libdav1d7` 1.4.1 but no headers, and no libavif
  at all. The apt candidates (`libavif-dev` 1.0.4, `libdav1d-dev` 1.4.1)
  are years behind the current releases, and apt packages are wiped on
  VM restart — a system-lib build would break on the next restart and
  on any clean clone. Same reason as the libwebp vendoring in
  `src/webp/README.md`.
- No encoder is vendored: only the decode path (`avifDecoder*`,
  `avifImageYUVToRGB`, dav1d decode) is compiled in. libavif's own
  encoder needs a separate AV1 encoder (aom/rav1e/SVT) that is not
  here.

## Layout

The vendored files keep their upstream paths verbatim under
`libavif/` and `dav1d/` so the internal `#include` directives resolve
unchanged with include dirs (see `CMakeLists.txt`, `web/build.sh`,
`web/build.ps1`):

```
src/avif/
  README.md          this file
  libavif/
    LICENSE          upstream BSD-2-Clause license
    include/avif/    public headers (avif.h, avif_cxx.h, internal.h)
    src/             21 AVIF_SRCS .c files + codec_dav1d.c
                     (reformat_libyuv.c and reformat_libsharpyuv.c are
                     in the list but internally stubbed: AVIF_LIBYUV is
                     off, so YUV<->RGB conversion uses libavif's own
                     code, not libyuv)
    third_party/libyuv/
                     the bundled libyuv subset libavif's own CMake
                     compiles when AVIF_LIBYUV is off: 5 .c files
                     (scale, scale_common, scale_any, row_common,
                     planar_functions) + headers. It is used for image
                     scaling only (avifImageScale: gainmap rescale and
                     grid tile scaling) — never for YUV<->RGB
                     conversion. It ships inside the libavif tarball;
                     no network fetch, no system libyuv.
  dav1d/
    COPYING          upstream BSD-2-Clause license
    config.h         meson-generated config for this vendor (Linux
                     x86_64, C-only, no asm — see below), plus an
                     appended `__EMSCRIPTEN__` override block that
                     turns the x86 ARCH flags off for wasm. The
                     generated values above the override must not be
                     hand-edited; regenerate with meson if the
                     platform changes. The override is the only
                     hand-written part of the file.
    vcs_version.h    meson-generated version string (DAV1D_VERSION)
    include/         public headers (dav1d/*.h, common/*.h, compat/*)
    src/             38 .c files — exactly the set the meson C-only
                     build compiled (see research report Section 2.2)
                     + all top-level headers. 25 plain .c files are
                     compiled once; the 13 `*_tmpl.c` files are
                     compiled TWICE, once per bitdepth
                     (`-DBITDEPTH=8` and `-DBITDEPTH=16`) — meson
                     does the same (libdav1d_bitdepth_8/16). The
                     templates stamp out the 8-bit and 16-bit DSP
                     variants from the BITDEPTH macro, so compiling
                     them once without the define fails.
    src/x86/         `cpu.h` only: `src/cpu.h` includes
                     `"src/x86/cpu.h"` when ARCH_X86 is set. The other
                     x86 headers (cdef, filmgrain, ipred, ...) are
                     included only under `#if HAVE_ASM`, which is 0,
                     so they are not vendored. No .asm, no .c — the
                     asm dispatch is off (HAVE_ASM 0).
```

## Build notes

- dav1d is C-only: `nasm` is not available in this VM, so the meson
  research build used `-Denable_asm=false`. The vendored `config.h`
  has `HAVE_ASM 0`, `ARCH_X86_64 1`. The x86 dispatch headers exist
  only so the `#include "src/x86/cpu.h"` in `src/cpu.h` resolves;
  with asm off all decode runs the portable C paths.
- Compile defines the vendored code needs (set in `CMakeLists.txt`):
  `AVIF_CODEC_DAV1D` for the libavif sources (registers the dav1d
  decoder in `avif.c`), `_FILE_OFFSET_BITS=64` and `_GNU_SOURCE`
  for the dav1d sources (as in the meson build).
- dav1d sources include headers as `"src/..."` (relative to
  `dav1d/`), `"common/..."` (relative to `dav1d/include/`) and
  `"dav1d/..."` (relative to `dav1d/include/`); `config.h` and
  `vcs_version.h` are found via `-I src/avif/dav1d` and
  `-I src/avif/dav1d/include`. libavif sources include
  `"avif/..."` (relative to `libavif/include/`) and
  `"dav1d/dav1d.h"` (relative to `dav1d/include/`).
- One-character patch to vendored code: `libavif/src/gainmap.c`
  cited `C99 7.12.12` with the section-symbol character; that
  character is not used anywhere in this repo (project rule), so
  it reads `C99 Section 7.12.12` here. Re-apply on upgrade.

## What was excluded, and why

- libavif `src/codec_aom.c`, `codec_avm.c`, `codec_libgav1.c`,
  `codec_rav1e.c`, `codec_svt.c`, `src/compliance.cc`: other AV1
  codecs / encoder / compliance warden — not needed for decode
  with dav1d.
- libavif `apps/`, `examples/`, `tests/`, `ext/`: tools and tests.
- dav1d `src/{arm,loongarch,ppc,riscv,win32,x86}/` code (only the
  x86 headers are kept, see above), `tools/`, `tests/`, `doc/`,
  `examples/`: other architectures, tools, tests.
- `AVIF_LIBYUV=LOCAL` and `deps.sh` are deliberately not used:
  they fetch from the network at build time. libyuv is OFF as an
  external dependency; the scaling-only subset bundled in the
  libavif tarball (see the layout above) is what gets compiled.

## Upgrading

Re-download the tarballs, verify the SHA256 against a published
source, re-copy the same file sets, regenerate `dav1d/config.h`
with the meson C-only build (`-Denable_asm=false`), re-append the
`__EMSCRIPTEN__` override block at the end of `config.h`, and
re-run the AVIF gates (`image_io_test` `*Avif*`, the 6 research
files through `wkenc`, E2E byte-identical). Do not hand-edit
vendored files: `dav1d/config.h`'s generated part and its appended
override are the only file written for this vendor.
