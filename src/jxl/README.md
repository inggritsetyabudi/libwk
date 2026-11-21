# Vendored libjxl 0.13.0 — JXL decode only (native)

`libjxl` 0.13.0, decode-only subset, vendored 2026-11-20 for JXL input
(slice JXL decode). Research that picked this version:
the AVIF/JXL conversion research notes of 19 Nov 2026 (kept locally,
not in the repo). Vendor script:
`~/workspace/libwk-exp-n1/jxl_slice/vendor_jxl.py` (copies the file
sets from upstream `lib/jxl_lists.cmake`: `JPEGXL_INTERNAL_DEC_SOURCES`
75 `.cc` + `JPEGXL_INTERNAL_DEC_BOX_SOURCES` 1 `.cc` +
`JPEGXL_INTERNAL_CMS_SOURCES` 1 `.cc`).

- libjxl source: https://github.com/libjxl/libjxl/releases/tag/v0.13.0
  (tarball `libjxl-0.13.0.tar.gz`, 1,734,982 bytes)
- libjxl SHA256: `24d18c9af2d1f5a17730b3f8d3f70a5c16e5ac303f5d9d5fe9dc00cf7ca82af2`
  (verified 2026-11-20 against the tarball in `~/workspace/libwk-exp-n1/avif_jxl/deps/`)
- libjxl license: BSD-3-Clause, see `LICENSE`
- Upstream: https://github.com/libjxl/libjxl

## Submodules (decode needs three)

The codeload tarball does not contain the submodules; each was
fetched separately and pinned to the commit recorded for tag v0.13.0
(via the GitHub API `contents/third_party/<name>`):

| Submodule | Commit pin | Tarball | SHA256 |
|---|---|---|---|
| highway | `457c8917...` | 2,235,690 B | `5124b0501c98d9930dbb065bfa1a5bbbd59ce0f12facb7e1e33aaef01a5f1f1a` |
| brotli | `db24a98a...` | 646,758 B | `113b9685dfc14dd2f3360d077283266eec2fbdfb0fe3e5600f11be151da0d68e` |
| skcms | `96d9171c...` | 10,061,812 B | `9bb4b5bba0b7c04f6c2bce9ff713d61e23c9a20c4945161ae16290498ad74627` |

- highway license: Apache-2.0, see `third_party/highway/LICENSE`
- brotli license: MIT, see `third_party/brotli/LICENSE`
- skcms license: BSD-style (Google), see `third_party/skcms/LICENSE`
- sjpeg and lcms are NOT vendored: sjpeg is only needed for encoding
  (disabled upstream with `-DJPEGXL_ENABLE_SJPEG=OFF`), and since
  0.12 the static libjxl build uses skcms instead of lcms.

## Why vendored, not the system library

- The apt candidate (`libjxl-dev` 0.7.0) is years behind 0.13.0,
  ships no headers, and apt packages are wiped on VM restart — a
  system-lib build would break on the next restart and on any clean
  clone. Same reason as the libwebp vendoring in `src/webp/README.md`
  and the libavif vendoring in `src/avif/README.md`.
- No encoder is vendored: only the decode path (`JxlDecoder*`,
  `JxlGetDefaultCms` for the ICC fallback) is compiled in. The
  68 `*enc*` files in the upstream tree are not here.

## Layout

The vendored files keep their upstream paths verbatim under
`lib/` and `third_party/` so the internal `#include` directives
resolve unchanged with include dirs (see `cmake/wk_jxl.cmake`).
329 files total (297 originally vendored + 15 `hwy/ops/` headers +
1 `lib/jxl/enc_ans_params.h` + 15 brotli internal headers, all
added 2026-11-20 — see below):

```
src/jxl/
  README.md          this file
  LICENSE            upstream BSD-3-Clause license (libjxl)
  lib/
    include/jxl/     public headers (decode.h, cms.h,
                     color_encoding.h, types.h, ...) including the
                     GENERATED version.h (0.13.0) — no generation
                     step is needed at build time.
    jxl/             77 .cc files: the 75 DEC + 1 DEC_BOX files from
                     upstream jxl_lists.cmake, plus cms/jxl_cms.cc
                     (upstream builds it as a separate jxl_cms
                     target; here it joins the same source list).
                     Subdirs: render_pipeline/ (20), modular/ (1),
                     modular/encoding/ (2), modular/transform/ (5),
                     cms/ (1). Plus 144 internal headers.
    jxl/jpeg/        headers only (dec_jpeg_data.h, jpeg_data.h,
                     ...): decode.cc includes jpeg_data.h
                     unconditionally, but every use of the JPEG
                     reconstruction types is guarded by
                     JPEGXL_ENABLE_TRANSCODE_JPEG, which is 0 here.
  third_party/
    highway/hwy/     7 .cc files (the upstream hwy target exactly),
                     plus all top-level `hwy/*.h` and the 15
                     `hwy/ops/*.h` headers: `highway.h` includes
                     the per-target ops headers (x86_128-inl.h,
                     ...), so the ops directory is required to
                     compile anything that includes
                     `<hwy/highway.h>`. The first vendor run missed
                     it; the repo build failed on `jxl_cms.cc`
                     until it was added (2026-11-20).
    brotli/c/        12 .c files: dec/ (6) + common/ (6) — the
                     brotlidec + brotlicommon targets exactly —
                     plus the 15 internal headers they include
                     relatively (`c/common/*.h` 9 + `c/dec/*.h` 6).
                     The encoder (enc/, 24 files) is not vendored.
    skcms/           skcms.h + skcms.cc + src/
                     (skcms_TransformBaseline.cc and its headers) —
                     about 117 KB of the 15 MB extract; the other
                     14 MB is profiles/ (ICC test data), not vendored.
```

## Build notes

- Native only: JXL is NOT part of the web build (`web/build.sh`,
  `web/build.ps1` do not consume the jxl sections; there is no
  official Emscripten port for libjxl). The web stays SDR 8-bit.
- No parallel runner is vendored (`lib/threads/` is absent):
  `JxlDecoderCreate(nullptr)` runs single-threaded, which is all
  the decoder here needs.
- Compile defines the vendored code needs (set in
  `cmake/wk_jxl.cmake`, mirroring the validated research build in
  `~/workspace/libwk-exp-n1/avif_jxl/deps/libjxl-0.13.0/build/`):
  `JPEGXL_ENABLE_TRANSCODE_JPEG=0` (the 4 transcode files are
  absent), `JPEGXL_ENABLE_BOXES=1` (box container support),
  `JPEGXL_ENABLE_SKCMS=1`, `JXL_STATIC_DEFINE` (static linking),
  `JXL_INTERNAL_LIBRARY_BUILD`; `HWY_STATIC_DEFINE` and
  `TOOLCHAIN_MISS_ASM_HWCAP_H` for highway; `JXL_CMS_STATIC_DEFINE`,
  `SKCMS_DISABLE_HSW`, `SKCMS_DISABLE_SKX` for the CMS/skcms
  sources. The vendored `.cc` files compile as C++17 (upstream;
  the repo default is C++23) with RTTI/exceptions on, like the
  repo's other vendored code — upstream's `-fno-rtti
  -fno-exceptions` is deliberately not mirrored (see
  `cmake/wk_jxl.cmake`: it suppresses the typeinfo the repo's own
  `jxl_decode.cpp` needs for the ICC fallback, breaking the
  Debug/ASan link). The repo's own `src/core/jxl_decode.cpp`
  stays C++23.
- Vendored sources include headers as `"lib/jxl/..."` (relative
  to `src/jxl/`), `"jxl/..."` (relative to `src/jxl/lib/include/`),
  `"hwy/..."` (relative to `src/jxl/third_party/highway/`),
  `"brotli/..."` (relative to
  `src/jxl/third_party/brotli/c/include/`), and `"skcms.h"`
  (relative to `src/jxl/third_party/skcms/`).
- Two upstream defaults are deliberately NOT mirrored:
  `HWY_DISABLED_TARGETS` (the research build disabled AVX3 etc.)
  is left unset, so highway enables what the compiler supports —
  runtime dispatch picks the right target, and the decode output
  is bit-identical either way (proven by the E2E lossless gate:
  byte-identical `.wk` through the encoder). The `__DATE__` /
  `__TIME__` redaction is also not mirrored (no other vendored
  code in this repo does it).

## What was excluded, and why

- `lib/jxl/decode_to_jpeg.cc`, `lib/jxl/jpeg/dec_jpeg_data.cc`,
  `lib/jxl/jpeg/dec_jpeg_data_writer.cc`,
  `lib/jxl/jpeg/jpeg_data.cc`: JPEG reconstruction (transcode
  JXL->JPEG without re-encoding). Decode-only slice; the uses in
  `decode.cc` are all `#if`-guarded by
  `JPEGXL_ENABLE_TRANSCODE_JPEG`, which is 0. The 5 headers in
  `lib/jxl/jpeg/` stay because `decode.cc` includes `jpeg_data.h`
  unconditionally.
- All `*enc*` files (encoder, 68 files), `lib/extras/`, `lib/threads/`,
  `tools/`, `examples/`, `doc/`, tests: not needed for decode.
- `third_party/brotli/c/enc/` (24 files): Brotli encoder.
- `third_party/skcms/profiles/` (~14 MB ICC test data): test data.
- `third_party/sjpeg/`: encode-only.
- One-character patch to vendored code: none. (The AVIF slice
  needed one for a section-symbol in `gainmap.c`; the JXL vendor
  scan found no section-symbol and no non-UTF-8 file.)
- Three vendor gaps found by the repo build (2026-11-20), all fixed
  in `vendor_jxl.py` so a re-vendor does not repeat them:
  `third_party/highway/hwy/ops/` (15 headers, `highway.h`
  includes the per-target ops headers),
  `lib/jxl/enc_ans_params.h` (encoder-named but decoder-used:
  `modular/options.h` includes it unconditionally), and the
  brotli internal headers `c/common/*.h` + `c/dec/*.h` (15 files,
  included relatively by the vendored `.c` files).

## Upgrading

Re-download the tarballs, verify the SHA256 values above, re-run
`vendor_jxl.py` against the new tree (it asserts the
`jxl_lists.cmake` set sizes: 75 + 1 + 1), and re-run the JXL gates
(`image_io_test` `*Jxl*`, the 4 research files through `wkenc`,
E2E lossless byte-identical). Do not hand-edit vendored files.
