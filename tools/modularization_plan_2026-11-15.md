# Modularisasi encoder.cpp / decoder.cpp — rencana

Dibuat 2026-11-15 ~02:20 WIB, saat Setya tidur. Alasan: encoder.cpp 2368 baris,
decoder.cpp 1150 baris — satu file memuat terlalu banyak kode, sulit di-maintain.
Prinsip: pecah per tanggung jawab, jaga output byte-identical (tidak ada perubahan
perilaku), tiap slice diverifikasi dengan gates sebelum lanjut.

## Pemetaan dependensi (diverifikasi dari kode, bukan tebakan)

### encode_lossy_tile (encoder.cpp:435-1917, ~1480 baris)
- Fungsi bebas `static`, parameter eksplisit — siap pindah apa adanya.
- Helper yang HANYA dipakai olehnya (pindah bersamanya):
  - collect_quantized_blocks (dipakai di 1151-1154, semua di dalam fungsi)
  - write_quant_scale_stream (dipakai di 1696-1735, semua di dalam fungsi)
  - analyze_tile_chroma_complexity, adaptive_luma_quality_for_tile,
    adaptive_chroma_quality_for_tile, tile_has_smooth_chroma_ramp,
    compute_quality_score
- Helper yang dipakai encode_impl (TINGGAL di encoder.cpp):
  - resolve_tile_size_log2, expand_rgb8_to_rgba8

### decode_lossy_tile (decoder.cpp:171-840, ~670 baris)
- Fungsi bebas non-static, parameter eksplisit — siap pindah.
- Helper BERSAMA (dipakai decode_lossy_tile DAN decode_impl):
  - tile_payload_view (dipakai di 898, 940, 960, 981, 1006 — decode_impl/get_info_impl)
  - infer_subsampling_from_tile (dipakai di 968 — decode_impl)
  - infer_tile_geometry (dipakai di 163 oleh infer_subsampling_from_tile,
    dan di 362 oleh decode_lossy_tile)

## Split yang direncanakan

### Encoder
- BARU `src/lossy_tile_encode.{h,cpp}`:
  encode_lossy_tile + 7 helper di atas (hapus `static`, deklarasi di header).
- TETAP `src/encoder.cpp` (~850 baris):
  encode_impl (orkestrasi: YCbCr, dispatch lossy/lossless, tile loop,
  penulisan container), encode, encode_with_stats, Image methods,
  C API (wk_encoder_config_init, wk_encode, wk_get_info, wk_free,
  wk_version), resolve_tile_size_log2, expand_rgb8_to_rgba8.
  Include lossy_tile_encode.h.

### Decoder
- BARU `src/tile_util.{h,cpp}`:
  tile_payload_view, infer_tile_geometry, infer_subsampling_from_tile.
- BARU `src/lossy_tile_decode.{h,cpp}`:
  decode_lossy_tile (include tile_util.h).
- TETAP `src/decoder.cpp` (~370 baris):
  decode_impl (orkestrasi: parsing container, dispatch tile, lossless),
  get_info_impl, get_info, decode. Include tile_util.h + lossy_tile_decode.h.

## Urutan kerja (incremental, tiap slice terverifikasi)
1. Slice 1 — encoder: pindah encode_lossy_tile + 7 helper ke
   lossy_tile_encode.{h,cpp}. Build + ctest Release. Verifikasi E2E:
   encode 6 foto q75, bandingkan byte-identical vs sebelum split.
2. Slice 2 — decoder: pindah 3 helper bersama ke tile_util.{h,cpp}.
   Build + ctest. Verifikasi decode 12/12 old .wk pixel-identical.
3. Slice 3 — decoder: pindah decode_lossy_tile ke lossy_tile_decode.{h,cpp}.
   Build + ctest. Verifikasi E2E roundtrip 6 foto pixel-identical.
4. Gates akhir: Release 28/28, ASan/UBSan 28/28, 12/12 old .wk,
   E2E 6 foto (lossy q75 + lossless) byte-identical vs pre-split.
   Commit per slice (bukan satu commit raksasa).

## Yang TIDAK dilakukan di sini
- Tidak ada perubahan perilaku/format (bukan V12, bukan ubah clamp/estimator).
- Tidak menyentuh src/lossless.{h,cpp} (sudah modular), predict/cclm
  (sudah modular), tools/ (sudah terpisah per tool).
- CMake meng-glob src/*.cpp, jadi file baru ter-pickup otomatis saat
  re-configure; tidak perlu ubah CMakeLists kecuali untuk tes baru
  (tidak ada tes baru di sini).

## Status
- Analisis selesai 2026-11-15 ~02:20 WIB. Implementasi SELESAI 2026-11-15 ~03:00 WIB
  (3 slice, masing-masing commit terpisah):
  - Slice 1 (a1e222c): encode_lossy_tile + 7 helper → lossy_tile_encode.{h,cpp};
    encoder.cpp 2368 → 538 baris. E2E q75 6 foto byte-identical.
  - Slice 2 (1683545): TileGeometry/TileDecodeResult + 3 helper bersama →
    tile_util.{h,cpp}; decoder.cpp 1150 → 1021 baris. PSNR cocok baseline.
  - Slice 3 (1747779): decode_lossy_tile → lossy_tile_decode.{h,cpp};
    decoder.cpp 1021 → 350 baris. PSNR identik, lossless roundtrip max diff 0.
  - Gates per slice: Release ctest 28/28. Final: 12/12 old .wk pixel-identical
    (terverifikasi ~03:00 WIB). ASan/UBSan ctest masih berjalan saat penulisan ini.
