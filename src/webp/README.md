# Vendored libwebp — decode only

`libwebp` 1.3.2, decode-only subset, vendored 2026-11-17 for WebP input
(slice 2 convert).

- Source: https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.3.2.tar.gz
- SHA256: `2a499607df669e40258e53d0ade8035ba4ec0175244869d1025d460562aa09b4`
  (verified against the OpenEmbedded SPDX record and the Ubuntu Launchpad
  package page, which both publish the same value for
  `libwebp_1.3.2.orig.tar.gz`)
- License: BSD-3-Clause, see `COPYING` (preserved verbatim)
- Upstream: https://developers.google.com/speed/webp

## Why vendored, not the system library

- The VM has the runtime `libwebp.so.7` (1.3.2) but no dev headers
  (`libwebp-dev` not installed, no pkg-config file). Installing it needs
  apt, and apt packages are wiped on VM restart — a system-lib build
  would break on the next restart and on any clean clone.
- Emscripten does ship a libwebp port (`tools/ports/contrib/libwebp.py`),
  so a split (apt for native, port for wasm) is technically possible —
  but that is two dependency paths and two versions to keep in sync,
  and the port fetches from the network at wasm build time. Vendored is
  one path, pinned 1.3.2, no network at build time, same sources for
  native and wasm. It also follows the existing `src/stb/` precedent.
- Version 1.3.2 matches the system runtime lib, so decode behaviour
  matches the reference decode the corpus PPMs were made from
  (verified: PIL decode of `solis-2c9e.jpg` is pixel-identical to its
  PPM).

## Layout

The vendored files keep their upstream paths verbatim under `src/` so
the internal `#include "src/..."` directives resolve unchanged with a
single `-I src/webp` (see `CMakeLists.txt`, `web/build.sh`,
`web/build.ps1`):

```
src/webp/
  COPYING            upstream BSD-3-Clause license
  README.md          this file
  src/dec/           VP8/VP8L decoder (10 .c + 6 .h)
  src/dsp/           decode DSP: portable C + SSE2/SSE41/NEON variants
                     (28 .c + headers; MIPS/MSA files excluded — not our
                     targets; the SIMD files self-guard with
                     #if defined(WEBP_USE_*) so they compile to empty
                     translation units off-target)
  src/utils/         bit reader, Huffman, rescaler, threads, random
                     (10 .c + headers; `random_utils.c` looks encoder-only
                     but the decoder's dithering calls `VP8InitRandom`)
  src/webp/          public headers used by the decoder
                     (decode.h, types.h, format_constants.h, encode.h,
                     mux_types.h) + config.h (minimal, written for this
                     vendor — see its header comment)
  src/enc/           TWO HEADERS ONLY, no encoder code:
                     `histogram_enc.h` + `backward_references_enc.h`.
                     `src/dsp/lossless.h` includes the former for the
                     `VP8LHistogram` type the lossless decoder uses.
```

## What was excluded, and why

- `src/enc/`, `src/dsp/{cost,enc,lossless_enc,ssim}*.c`,
  `src/utils/{bit_writer,huffman_encode,random}_utils.c`: encoder-only.
- `src/{demux,mux,examples,sharpyuv}/`, `src/dsp/*{mips,msa}*`: not
  needed by the simple decode API on our targets.
- No encoder, mux, demux, or animation API is exposed — only
  `WebPGetFeatures` / `WebPDecodeRGB(A)` via `src/core/webp_decode.cpp`.

## Upgrading

Re-download the tarball, verify the SHA256 against a published source,
re-copy the same file sets, and re-run the WebP gates (`image_io_test`
`*Webp*`, solis byte-identical `.wk` vs the PPM path). Do not hand-edit
vendored files: `config.h` is the only file written for this vendor.
