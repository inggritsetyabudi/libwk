# WK Image Format — Bitstream Specification v0.2.9

**MIME type:** `image/x-wk`  
**File extension:** `.wk`  
**Byte order:** Little-endian throughout  
**Format version:** 0x0001

**v0.2 changes:** lossy layout `0x374D4843` (V7) with per-block quant scales and the PLANAR prediction mode (mode 13); quantization-aware rate-distortion mode selection; the V7 seed boost (scale `0.75`) for smooth-ramp tiles below quality 75. The container format (`0x0001`) is unchanged, and V1–V6 files decode identically.

**v0.2.1 changes:** the JPEG base quant table is capped at 40 (see Section 8.2), fixing PSNR non-monotonicity in quality (the q90 < q75 anomaly). Encoder-side only; the container format is unchanged and all older files decode identically.

**v0.2.2 changes:** lossy layout `0x384D4843` (V8) entropy-codes the prediction-mode and coefficient-span streams with rANS (see Section 3.2). The encoder writes V8 whenever the rANS-coded modes+spans are smaller than the packed/adaptive forms, otherwise it writes V6/V7 as before. V8 is identical to V7 in every respect except the mode and span streams are rANS-coded; the quant-scale streams are present (possibly empty) exactly as in V7. Decoded pixels are identical to V7 for the same quant/modes — the re-coding is lossless. The container format (`0x0001`) is unchanged and V1–V7 files decode identically.

**v0.2.3 changes:** lossless frequency tables use the adaptive coefficient-table encoding (transform-flags bit 1, see Section 3.3) instead of raw 256 x uint16. Legacy files (bit 1 clear) still decode. The container format is unchanged.

**v0.2.5 changes:** lossy layout `0x3a4d4843` (V10) adds cross-component linear model (CCLM) chroma prediction (see Section 6). V10 is V9 plus a 15th prediction mode (mode 14, CCLM, chroma-only): a chroma block may be predicted from its co-located reconstructed luma as `a*luma + b`, with `(a, b)` derived from the neighbouring reconstructed luma/chroma by integer least squares (Q8 fixed point), so nothing per block is signalled beyond the mode itself. The encoder writes V10 only when at least one chroma block's RDO picks CCLM; otherwise it writes V9/V8/V7/V6 as before. The container format (`0x0001`) is unchanged and V1–V9 files decode identically.

**v0.2.7 changes:** lossless predictor mode 13 (MED, JPEG-LS median edge detector) added to Section 3.3; the predictor mode byte is now 0..13. The encoder picks the best of SELECT / MED / CLAMP_ADD per image by true coded size (measured 2026-11-15 on the 6 benchmark photos: MED is the best single predictor on every non-palette photo, −1.83% total vs the old hardcoded SELECT; GAP was measured and rejected, +19% worse). (Per image, not per tile: the lossless path encodes the whole image as a single tile holding the full image, so there is one predictor mode byte for the entire image; the decoder requires exactly one tile on the lossless path.) The encoder also compares the palette and predictor paths and keeps the smaller: a palette is not always the better choice (a smooth 256-color gradient is 499 B via predictors vs 7,193 B via palette; a 2-color checkerboard is 14.5% smaller via predictors — measured 2026-11-15). The flags byte tells the decoder which path the payload holds, so the choice is encoder-side only and the format is unchanged. A decoder MUST reject a predictor mode ≥ 14. Files written before 0.2.7 (modes 0..12) decode unchanged.
**v0.2.8 changes:** per-tile lossless. The predictor path is split into tiles of `tile_size` (the encoder default for lossless is now 256 px, `tile_size_log2` 8, down from 512): each tile is encoded independently with its own predictor choice and rANS tables, so a tile's payload is a complete Section 3.3 payload (flags + predictor mode + per-channel data) for that tile's rectangle, and the decoder stitches the tiles into the frame. The palette path stays whole-image: the encoder compares the whole-image palette against the per-tile predictor total (including 18 B of container overhead per tile: 9 B chunk framing + 9 B tile header) and keeps the smaller, so a palette image can never regress. A lossless frame therefore has either exactly one TILE chunk at (0,0) holding the whole frame (the pre-0.2.8 format, still written when the palette wins or the image is a single tile) or exactly `tile_count()` TILE chunks covering the grid once; see Section 9.1 rule 11. Measured 2026-11-15 (exp-h1, true coded sizes): the 5 non-palette benchmark photos shrink −5.6% (tile 512) / −7.3% (tile 256) from local rANS table adaptation, not predictor selection (per-tile predictor oracle: 19 B of 4,489,297). Files written before 0.2.8 (one tile, `tile_size_log2` 9) decode unchanged.

**v0.2.6 changes:** lossy layout `0x3b4d4843` (V11) adds per-tile chroma subsampling: a tile's chroma may be YUV420 or YUV444 independently of the other tiles, signalled by `layer_flags` bit 3 (`TILE_CHROMA_420`, see Section 3.1) in the tile header. V11 is V10 in every other respect (rANS modes with the 15-symbol alphabet, superblock quant map always present, CCLM allowed). The encoder writes V11 for a tile whose chroma is subsampled; the decision is per tile from the source chroma Laplacian `c_lap` (mean |4·C − up − down − left − right| over Cb and Cr, interior pixels of the tile): `c_lap < 2.1` → YUV420, else YUV444. (2.1, not 2.0: the encoder measures c_lap on its int16 chroma planes, and integer rounding adds a small positive bias versus the float reference the 2.0 value was validated on; 2.1 reproduces the same tile selection, still far inside the robust range — the dangerous tile sits at 6.9.) The mean drowns sharp edges, so four second conditions force YUV444 even when `c_lap < 2.1`: the 99.9th percentile of |Laplacian| over the tile (`p999 ≥ 79`, catches long sharp edges — a step edge has p999 79 at mean 0.15) and the same percentile restricted to saturated pixels (`sat_p999 ≥ 100`, saturation = max(|Cb−128|, |Cr−128|) > 40; catches small saturated features — lips have sat_p999 116 at plain p999 34). The saturation threshold and mid scale with bit depth (40/128 at 8-bit, 160/512 at 10-bit, 640/2048 at 12-bit). Two further conditions catch periodic high-frequency chroma texture that the mean also drowns (a synthetic 8 px-period sinusoid at amplitude 3.0 has mean 1.71 yet loses 5.11 dB under 420): the fraction of |Laplacian| values above 2 (`frac_gt2 ≥ 0.28`, cut scales as `2 << (bit_depth − 8)`) and the fraction of the tile's detrended chroma AC energy in DCT coefficients (u,v) in 1..3 over 8x8 blocks, pooled over Cb and Cr (`dct_band ≥ 0.50`; the sinusoid ladder sits at 0.73–0.97 while safe 420 content stays at most 0.302). Measured 2026-11-21 (23 images + 7 amplitude probes, q75, tile 1024): the periodic probes flip to 444 (+1.10 to +5.49 dB) with zero false positives; photos and text byte-identical, total +0.125%. The 10/12-bit spot check holds (dct_band 0.985/0.987 at amplitude 3.0 vs the 0.50 threshold). Note: the encoder's c_lap measurement carries a larger positive bias on periodic texture (+0.3–0.5, measured on synthetic probes) than the +0.05–0.13 documented above for real photos, so a tile near the threshold on periodic content may be decided 444 where the float reference would say 420. Measured 2026-11-14 (6 benchmark photos): q75 files −13.3% at −0.59 dB whole-image PSNR, q85 −13.1% at −1.10 dB; the criterion selects exactly the oracle tile set at the default 1024px tile size. Measured 2026-11-15 (23 images, second conditions on): the three sharp-edge/saturated cases recover to pure-444 quality (d_skin +1.45 dB, e_chart +19.11 dB, c_edges +13.91 dB at q75) with zero false positives on the 6 benchmark photos (byte-identical, total still 1,056,775 B) and a +0.17% total size cost. A V11 tile is rejected by decoders that do not know layout `0x3b4d4843`; V1–V10 files decode unchanged.

**v0.2.8 validation changes:** the decoder now rejects an FHDR whose `cicp_primaries` has no entry in ITU-T H.273 Table 2 (allowed: 0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 22) and an FHDR whose `HDR` flag disagrees with `bit_depth`/`cicp_transfer` (the flag must be set exactly when `bit_depth` > 8 or the transfer is PQ/HLG). The encoder sets the flag by the same rule, so 8-bit PQ/HLG files now carry the flag set, and the encoder also rejects `cicp_primaries` with no Table 2 entry instead of writing a file the decoder would reject. No bitstream change: the container format (`0x0001`) is unchanged, and files that already agree decode identically. Files written by earlier encoders with 8-bit PQ/HLG transfer (flag clear) are now rejected; re-encode them with the current encoder. The encoder can now carry HDR mastering metadata: `EncoderConfig.max_cll`/`max_fall` (nits, 0 = unspecified, the default) are written to FHDR as-is, and `wkenc` gains `--max-cll`/`--max-fall`. When the transfer is PQ (16) and a field is left unset (0), the encoder computes it from the source RGB instead of writing 0: per pixel max(R,G,B) in linear light via the PQ EOTF (CTA-861); MaxCLL is the maximum, MaxFALL the mean over the frame, rounded to the nearest nit. A manually set value always wins, per field. HLG/SDR files are unaffected (still 0 unless set). Note the default VALUE changes for PQ files (computed instead of 0); the FHDR fields themselves are unchanged.

**v0.2.9 changes:** conditional zlib compression of the META chunk payload (see Section 7.7): the writer compresses the serialized WKMETA payload with zlib (level 6) only when the compressed form is strictly smaller, and signals it with chunk flag bit 3 (`CHUNK_FLAG_META_COMPRESSED`); the container version stays `0x0001` and the FHDR is unchanged. A decoder that does not know the flag feeds the raw bytes to the WKMETA parser, which rejects them (a zlib stream starts with 0x78, never the 0x01 wkmeta version), so old decoders fail cleanly on compressed-META files instead of misreading them. Measured 2026-11-22 (probe, real payloads): a 16-entry block with a 1,000-character description saves 602–964 B (42.82–68.56% with realistic text), 10 varied region STRUCTs save 1,216 B (81.61%); small payloads (EXIF import 13 B, one region 18 B) stay raw, and the 15 B quality-score-only baseline stays raw because it grows to 23 B compressed. ICCP and PROV are never compressed.

---

## 1. File Structure

A WK file consists of a fixed file header followed by a sequence of typed chunks.

### 1.1 File Header

| Offset | Size | Field         | Value           |
|--------|------|---------------|-----------------|
| 0      | 5    | Magic         | `0x57 0x4B 0x49 0x4D 0x47` ("WKIMG") |
| 5      | 2    | Version       | `uint16 LE`, currently `0x0001` |

### 1.2 Chunk Format

Each chunk has the following structure:

| Offset | Size | Field   | Description |
|--------|------|---------|-------------|
| 0      | 4    | Type    | 4 ASCII characters (e.g., "FHDR") |
| 4      | 1    | Flags   | Bit field: bit0=optional, bit1=metadata-only, bit2=repeatable, bit3=compressed (META only, see Section 7.7) |
| 5      | 4    | Size    | `uint32 LE` — payload length in bytes |
| 9      | Size | Payload | Chunk-specific data |

### 1.3 Chunk Order

```
FILE HEADER (magic + version)
├── FHDR  — Frame Header (required, first after file header)
├── META  — WKMETA structured metadata (optional)
├── ICCP  — ICC color profile (optional)
├── PROV  — C2PA manifest (optional)
├── ANIM  — Animation header (present iff animated)
├── TILE  — Tile data (one or more, repeatable)
├── TILE  — ...
└── FEND  — End marker (required, last, zero-byte payload)
```

ICCP and PROV payloads are the raw profile / manifest bytes,
written verbatim. They are set through the encoder configuration
(`EncoderConfig.icc_profile` / `c2pa_manifest`) or the
`--import-icc` / `--import-prov` options of `wkenc` and
`wkmeta-edit`. A PROV chunk requires the FHDR `HAS_C2PA` flag
(see section 10 rule 12). On lossy encodes the encoder also writes
one META entry, `RATING.QUALITY_SCORE` (the mean per-tile quality
score); `EncoderConfig.write_quality_score` (default on) and
`wkenc --no-quality-score` omit it — with no other metadata this
drops the whole 24 B META chunk.

---

## 2. FHDR Chunk — Frame Header (19 bytes)

| Offset | Size | Field          | Type    | Description |
|--------|------|----------------|---------|-------------|
| 0      | 4    | width          | uint32  | Image width in pixels |
| 4      | 4    | height         | uint32  | Image height in pixels |
| 8      | 1    | bit_depth      | uint8   | 8, 10, or 12 |
| 9      | 1    | cicp_primaries | uint8   | ITU-T H.273 Table 2 |
| 10     | 1    | cicp_transfer  | uint8   | ITU-T H.273 Table 3 (1=BT.709, 13=sRGB, 16=PQ, 18=HLG) |
| 11     | 1    | cicp_matrix    | uint8   | ITU-T H.273 Table 4 |
| 12     | 2    | flags          | uint16  | See flag bits below |
| 14     | 1    | tile_size_log2 | uint8   | Tile size = 2^N, range 6..10 (64..1024) |
| 15     | 2    | max_cll        | uint16  | MaxCLL in nits (0=unspecified) |
| 17     | 2    | max_fall       | uint16  | MaxFALL in nits (0=unspecified) |

### 2.1 FHDR Flags

| Bit | Name           | Description |
|-----|----------------|-------------|
| 0   | LOSSLESS       | Image uses lossless coding |
| 1   | ANIMATED       | File contains animation |
| 2   | ALPHA          | Image has alpha channel |
| 3   | HDR            | Set exactly when bit_depth > 8 or cicp_transfer is 16 (PQ) or 18 (HLG) |
| 4   | TILED          | Image uses tile partitioning |
| 5   | HAS_WKMETA     | META chunk present |
| 6   | HAS_C2PA       | PROV chunk present |
| 7   | FULL_RANGE     | Samples use full range (vs limited) |

---

## 3. TILE Chunk

### 3.1 Tile Header (9 bytes)

| Offset | Size | Field           | Type   |
|--------|------|-----------------|--------|
| 0      | 2    | tile_x          | uint16 |
| 2      | 2    | tile_y          | uint16 |
| 4      | 1    | layer_flags     | uint8  |
| 5      | 4    | compressed_size | uint32 |

`layer_flags`: bit0 = has base layer, bit1 = has refinement layers, bit2 = has alpha plane, bit3 = this tile's chroma is YUV420 (only meaningful in layout V11; see Section 3.2).

### 3.2 Lossy Tile Payload

1. **Quantization tables**: 64 x uint16 (luma) + 64 x uint16 (chroma)
2. **Block dimensions**: blocks_x (uint16), blocks_y (uint16), chroma_blocks_x (uint16), chroma_blocks_y (uint16)
3. **Layout tag**: uint32 identifying the lossy tile syntax variant
4. **Prediction mode streams**:
   - Layout tag `0x324D4843` stores a `uint16` packed-byte length followed by 4-bit prediction modes packed two per byte.
   - Layout tag `0x334D4843` keeps the packed prediction mode streams and additionally stores packed coefficient-span streams.
   - Layout tag `0x344D4843` keeps the packed prediction modes and coefficient spans, and switches coefficient-table metadata to adaptive serialization.
   - Layout tag `0x354D4843` keeps packed modes, coefficient spans, adaptive coefficient tables, and additionally stores per-plane maximum coefficient extents so fully empty high-frequency contexts can be omitted.
   - Layout tag `0x364D4843` stores one `uint8` syntax-flags field after the layout tag. Bit `0` enables adaptive coefficient-span stream encodings, bit `1` enables per-plane maximum coefficient extents, bit `2` enables split magnitude/sign coefficient coding, bit `3` enables shared chroma coefficient tables, bit `4` enables coefficient-table bank signaling, bit `5` enables single-symbol coefficient-stream elision, bit `6` enables coefficient significance maps, and bit `7` enables adaptive coefficient sign modes.
   - Layout tag `0x374D4843` (V7, adaptive quantization) is identical to `0x364D4843` in every respect, except that two per-block quant-scale streams follow immediately after the syntax-flags byte, before the prediction mode streams: one for luma blocks, one for chroma blocks. Each stream is a sparse list: `count: uint16`, then `count` entries of `(block_index: uint16, scale_index: uint8)`. A block not listed uses scale `1.0`. `block_index` counts blocks in raster order within that plane's grid (`blocks_x * blocks_y` for luma, `chroma_blocks_x * chroma_blocks_y` for chroma). `scale_index` selects a step multiplier from this table (all values exactly representable in binary floating point, so encoder and decoder agree bit-for-bit): `0: 0.5, 1: 0.625, 2: 0.75, 3: 0.875, 4: 1.0, 5: 1.25, 6: 1.5, 7: 2.0`. The effective quant step for a block is `table_step * scale` (floating point), applied at quantize time (deadzone included) and at dequantize time, for all 64 coefficients of that block. The alpha plane has no scale stream; it always uses the tile table. The encoder writes V7 only when at least one luma or chroma block has a scale other than `1.0` or uses the PLANAR prediction mode (13); otherwise it writes V6, so plain tiles pay no overhead and no behavior change. The decoder MUST reject a V7 tile when `count` exceeds the plane's block count, when any `block_index` is out of range or not strictly increasing, or when any `scale_index` exceeds `7`. A V7 tile is rejected by decoders that do not know layout `0x374D4843` (clean failure, never silent garbage); V1–V6 files decode unchanged.
   - Layout tag `0x384D4843` (V8, rANS modes/spans) is identical to `0x374D4843` (V7) in every respect, except that the prediction-mode streams and the coefficient-span streams are rANS-coded instead of packed/adaptive. The two quant-scale streams are present exactly as in V7 (possibly empty, `count: uint16 = 0`). Each rANS stream is written as: a coefficient table (`write_coefficient_table`, previous table null) for the symbol alphabet, then `size: uint32` (LE) giving the byte length of the rANS payload, then the payload (symbols encoded in reverse order, decoded forward). The mode stream alphabet is the 14 prediction modes (`0`–`13`); the span stream alphabet is `0`–`255`. A V8 tile is rejected by decoders that do not know layout `0x384D4843`; V1–V7 files decode unchanged. The encoder chooses V8 per tile when the total rANS-coded mode+span size (all planes, including alpha) is smaller than the packed/adaptive size, so small tiles with few blocks may stay on V6/V7 where the rANS table overhead outweighs the saving.
   - Layout tag `0x394D4843` (V9, superblock quant map) is identical to `0x384D4843` (V8) in every respect, except that one superblock quant map follows immediately after the two per-block quant-scale streams, before the prediction mode streams. A superblock is 64x64 pixels (8x8 blocks). The map is a sparse list: `count: uint16`, then `count` entries of `(superblock_index: uint16, scale_index: uint8)`. A superblock not listed uses scale `1.0`. `superblock_index` counts superblocks in raster order over the superblock grid (`superblocks_x = ceil(blocks_x / 8)`, `superblocks_y = ceil(blocks_y / 8)`). `scale_index` selects a step multiplier from the same table as the per-block scales (`0: 0.5, 1: 0.625, 2: 0.75, 3: 0.875, 4: 1.0, 5: 1.25, 6: 1.5, 7: 2.0`). The map is shared by luma and chroma: a chroma block uses its top-left covered luma block's superblock (in YUV420 a chroma block covers luma blocks `(2bx,2by)..(2bx+1,2by+1)`). The effective quant step for a block is `table_step * scale`, where `scale` is the block's per-block scale when it is non-`1.0` (the seed boost), else the block's superblock scale, else `1.0`. The alpha plane has no scale; it always uses the tile table. The encoder writes V9 only when the control field is enabled and at least one superblock has a scale other than `1.0`; otherwise it writes V8/V7/V6 as before, so plain tiles pay no overhead. The decoder MUST reject a V9 tile when `count` exceeds the superblock count, when any `superblock_index` is out of range or not strictly increasing, or when any `scale_index` exceeds `7`. A V9 tile is rejected by decoders that do not know layout `0x394D4843` (clean failure, never silent garbage); V1–V8 files decode unchanged.
   - Layout tag `0x3a4d4843` (V10, CCLM) is identical to `0x394D4843` (V9) in every respect, except that the chroma prediction mode stream may contain mode 14 (CCLM, see Section 6). The superblock quant map section is always present in V10 (it may be empty, `count` 0), so the decoder always reads it there, keeping the layout fixed. The encoder writes V10 only when at least one chroma block's RDO picks CCLM; otherwise it writes V9/V8/V7/V6 as before. The mode streams are rANS-coded in V10 (as in V8/V9), with a 15-symbol alphabet (modes 0–14). A V10 tile is rejected by decoders that do not know layout `0x3a4d4843` (clean failure, never silent garbage); V1–V9 files decode unchanged.
   - Layout tag `0x3b4d4843` (V11, per-tile chroma subsampling) is identical to `0x3a4d4843` (V10) in every respect, except that the tile's chroma subsampling is determined by `layer_flags` bit 3 in the tile header (Section 3.1): set means the tile's chroma is YUV420, clear means YUV444. The decoder MUST use the flag and MUST reject a V11 tile whose chroma block dimensions do not match the flagged subsampling. Each 420 tile's chroma is upsampled to the tile's full-resolution region with the same bilinear filter as `upsample_420` (Section 9.1), with sample coordinates clamped to the tile's own chroma plane (so a V11 tile decodes independently of its neighbours). The encoder writes V11 for a tile whose chroma is subsampled; a tile that is YUV444 is written as V11 only when its chroma block dimensions are ambiguous (tile width and height both ≤ 8px, where the 444 and 420 block counts coincide), so the flag — not the block counts — decides. A V11 tile is rejected by decoders that do not know layout `0x3b4d4843` (clean failure, never silent garbage); V1–V10 files decode unchanged.
   - Layout tag `0x314D4843` is the legacy layout and stores one `uint8` prediction mode per block.
   - One stream is stored for luma blocks and one stream is stored for chroma blocks.
5. **Coefficient span streams**:
   - Layout tag `0x334D4843` stores one packed span stream for luma blocks and one packed span stream for chroma blocks.
   - Layout tag `0x344D4843` keeps the same span signaling.
   - Layout tag `0x354D4843` keeps the same span signaling and stores one `uint8` maximum coefficient extent for luma and one `uint8` extent for chroma immediately after the span streams.
   - Layout tag `0x364D4843` uses the syntax-flags field to choose the span signaling:
     - If syntax flag bit `0` is clear, span streams use the legacy `uint16 packed-byte length + raw 7-bit packed spans` format.
     - If syntax flag bit `0` is set, each span stream starts with a tagged `uint16` header:
       - top bits `00`: raw 7-bit packed span payload, lower 14 bits = payload byte count
       - top bits `01`: single span value repeated for every block, lower 7 bits = span value
       - top bits `10`: run-length payload, lower 14 bits = payload byte count, payload = repeated `(run_length:uint16, span_value:uint8)` tuples
   - Each span is a 7-bit value in the range `0..64` and represents how many zigzag-ordered coefficients are present for the block.
   - Coefficients at positions `>= span` are implicitly zero and are not entropy-coded.
6. **Coefficient-table metadata**:
   - Layout tags `0x314D4843` to `0x334D4843` store dense symbol ranges: `first_symbol`, `last_symbol`, then one `uint16` frequency per symbol in that range.
   - Layout tag `0x344D4843` stores one-byte table encoding tags per coefficient context:
     - `0`: single-symbol table, followed by one `uint16` symbol id
     - `1`: dense range, followed by `first_symbol`, `last_symbol`, and dense `uint16` frequencies
     - `2`: sparse pairs, followed by `pair_count` and repeated `(symbol, frequency)` pairs
     - `3`: dense range, followed by `first_symbol`, `last_symbol`, and dense `uint8` frequencies
     - `4`: sparse pairs, followed by `pair_count` and repeated `(symbol:uint16, frequency:uint8)` pairs
     - `5`: reuse the previous inline adaptive table in the same payload stream; no additional table bytes follow
     - `6`: sparse delta pairs against the previous inline adaptive table, followed by `pair_count` and repeated `(symbol:uint16, delta:int8)` pairs
   - Layout tag `0x354D4843` uses the same table encodings, but only serializes coefficient contexts below the stored per-plane maximum coefficient extent.
   - Layout tag `0x364D4843` uses the same table encodings and applies the same per-plane extent omission only when syntax flag bit `1` is set.
   - If syntax flag bit `3` is set, each chroma coefficient context stores a single shared table that is reused for both the Cb and Cr entropy streams at that coefficient position.
   - If syntax flag bit `4` is set, each coefficient payload starts with a table-bank mode byte before any coefficient streams:
     - `0`: inline adaptive tables, one table per coefficient context
     - `1`: one shared adaptive table reused for every coefficient context in the payload
     - `2`: adaptive table bank + packed 4-bit per-context bank indices
     - `3`: adaptive table bank + raw 8-bit per-context bank indices
     - `4`: adaptive table bank + packed 1-bit per-context bank indices
     - `5`: adaptive table bank + packed 2-bit per-context bank indices
   - Table-bank signaling is only valid for layout tag `0x364D4843`, which already implies adaptive coefficient-table serialization.
   - Table encoding `5` is only valid for inline adaptive tables and only after a previous table has already been serialized in the same plane or shared-chroma payload.
   - Table encoding `6` is only valid for inline adaptive tables and only after a previous table has already been serialized in the same plane or shared-chroma payload.
   - If syntax flag bit `5` is set and a coefficient context resolves to an adaptive single-symbol table, the context omits the `uint32` rANS byte count and rANS payload entirely.
   - If syntax flag bit `6` is set, each coefficient context with active blocks writes a presence stream before the entropy stream:
     - `0`: all active coefficients are zero
     - `1`: all active coefficients are non-zero
     - `2`: raw packed 1-bit presence flags in block decode order
   - If syntax flag bit `7` is set and syntax flag bit `2` is also set, each coefficient payload stores packed 2-bit sign-mode flags before any tables:
     - plane payloads store one mode stream for the luma or alpha coefficient contexts
     - shared chroma payloads store one mode stream for Cb contexts and one for Cr contexts
     - `0`: raw packed sign bits follow the magnitude stream
     - `1`: all decoded non-zero magnitudes are positive, so no sign payload bytes are stored
     - `2`: all decoded non-zero magnitudes are negative, so no sign payload bytes are stored
7. **rANS-coded coefficients**:
   - The coefficient symbol range is bit-depth aware: the magnitude
     limit is `1024 << (bit_depth - 8)`, i.e. `+-1024` at 8-bit, `+-4096`
     at 10-bit, `+-16384` at 12-bit. The limit is derived from the FHDR
     bit depth, which both sides already know, so it is not stored in
     the stream. The 8-bit range is the historical one, so 8-bit files
     are unchanged.
   - Legacy signed coding stores symbols in the range `[-limit, limit]`
     mapped to `[0, 2*limit]` (at 8-bit: `[-1024, 1024]` mapped to
     `[0, 2048]`).
   - If layout tag `0x364D4843` sets syntax flag bit `2`, each coefficient context stores magnitude symbols in the range `[0, limit]` (at 8-bit: `[0, 1024]`), then stores packed sign bits for the non-zero magnitudes in block decode order.
   - The packed sign stream has no explicit byte length; its size is derived from the decoded non-zero magnitude count as `ceil(nonzero_count / 8)`.
   - If syntax flag bit `3` is set, each chroma coefficient context serializes `shared_table + cb_stream + cr_stream` instead of two independent table+stream pairs.
   - If syntax flag bit `5` is set and the coefficient table is single-symbol:
     - signed mode reconstructs that symbol for every active block with no explicit rANS stream bytes
     - split magnitude/sign mode reconstructs the shared magnitude for every active block, then reads packed sign bits only when the magnitude is non-zero
   - If syntax flag bit `6` is set, the entropy stream encodes only coefficients whose presence bit is `1`; zero-valued active blocks are reconstructed from the presence stream without consuming rANS symbols.
   - If syntax flag bit `7` is set and syntax flag bit `2` is also set, the sign-mode flags can suppress the packed sign payload entirely for contexts whose non-zero magnitudes are all positive or all negative.
8. **Optional alpha extension**: present when `layer_flags & 0x04` is set. The extension stores 64 x uint16 alpha quantization steps, then an alpha prediction mode stream using the same layout-tag-defined signaling (packed for V1–V7, rANS-coded for V8, V9, V10, and V11), then an alpha coefficient-span stream when the layout tag is `0x334D4843`, `0x344D4843`, `0x354D4843`, `0x364D4843`, `0x374D4843`, `0x384D4843`, `0x394D4843`, `0x3a4d4843`, or `0x3b4d4843` (adaptive for V3–V7, rANS-coded for V8, V9, V10, and V11), then an alpha maximum coefficient extent when the layout tag is `0x354D4843` or when layout tag `0x364D4843`, `0x374D4843`, `0x384D4843`, `0x394D4843`, `0x3a4d4843`, or `0x3b4d4843` has syntax flag bit `1` set, then one rANS-coded coefficient stream per coefficient position on the full-resolution alpha grid.

### 3.3 Lossless Tile Payload

1. **Transform flags**: uint8. Bit 0 = palette mode. Bit 1 = adaptive frequency tables (see below); only valid on the non-palette path. Bits 2–7 are reserved and must be zero; a decoder MUST reject a tile with reserved bits set, or with bit 1 set on the palette path.
2. **Predictor mode**: uint8 (0..13). Modes 0..12 are the original predictors (NONE, L, T, TR, TL, AVG_LT, AVG_LTR, AVG_LTL, AVG_TTR, AVG_TTL, AVG_LTRTL, SELECT, CLAMP_ADD); mode 13 is MED, the JPEG-LS median edge detector: if `tl >= max(l, t)` predict `min(l, t)`; else if `tl <= min(l, t)` predict `max(l, t)`; else predict `l + t - tl`. A decoder MUST reject a predictor mode ≥ 14.
3. **Per-channel data**: frequency table + rANS bitstream. When transform-flags bit 1 is clear (legacy), the table is raw: 256 x uint16 frequencies (512 B per channel). When bit 1 is set, the table is written with the adaptive coefficient-table encoding (`write_coefficient_table`, 256 symbols: single-symbol / sparse-pairs / dense-range forms, whichever is smallest). The adaptive form is never more than ~5 B larger than raw per channel and is typically far smaller: on photo residuals most tables are sparse (e.g. mira: 2048 B raw -> 834 B adaptive; the alpha channel alone drops from 512 B to 11 B). Small tiles benefit most — a 64x64 photo tile drops from 4770 B to 3178 B (-33%) because the fixed 2048 B table cost dominated it.

---

## 4. ANIM Chunk — Animation

| Offset | Size | Field           | Type   |
|--------|------|-----------------|--------|
| 0      | 4    | frame_count     | uint32 |
| 4      | 2    | loop_count      | uint16 (0=infinite) |
| 6      | 4    | background_rgba | uint32 |

Followed by `frame_count` frame entries (16 bytes each):

| Offset | Size | Field      | Type   |
|--------|------|------------|--------|
| 0      | 2    | delay_ms   | uint16 |
| 2      | 1    | blend_mode | uint8 (0=OVER, 1=SOURCE) |
| 3      | 1    | disposal   | uint8 (0=keep, 1=restore-bg) |
| 4      | 2    | rect_x     | uint16 |
| 6      | 2    | rect_y     | uint16 |
| 8      | 2    | rect_w     | uint16 |
| 10     | 2    | rect_h     | uint16 |
| 12     | 4    | tile_offset| uint32 |

### 4.1 Frame-to-tile mapping

Each frame owns a consecutive run of TILE chunks, in file order.
`tile_offset` is the index of the first TILE chunk owned by the frame
in the file's TILE chunk list (not a byte offset). The number of TILE
chunks owned by frame k is the difference between its `tile_offset`
and the next frame's `tile_offset`; the last frame owns the TILE
chunks from its `tile_offset` to the end of the TILE list. The first
frame's `tile_offset` MUST be 0, offsets MUST be strictly increasing,
and every offset MUST be smaller than the number of TILE chunks in
the file. No TILE chunk belongs to more than one frame, and no TILE
chunk belongs to no frame. A decoder MUST reject a file whose offsets
violate any of these rules.

A frame's TILE chunks are the tiles of its rectangle (Section 4.2):
the tile coordinates of a frame's TILE chunks MUST cover the tile
grid of its rectangle exactly once. In version 1 there is no
inter-frame reference: frames are independent, and a TILE chunk is
never shared between frames.

### 4.2 Frame rectangles

A frame's rectangle (`rect_x`, `rect_y`, `rect_w`, `rect_h`) MAY be
smaller than the canvas (a sub-frame); it MUST lie inside the canvas
(`rect_x + rect_w <= width`, `rect_y + rect_h <= height`) and have
non-zero width and height. The rectangle MUST be aligned to the tile
grid: `rect_x` and `rect_y` MUST be multiples of the tile size, and
`rect_w` and `rect_h` MUST be multiples of the tile size, except where
the rectangle reaches the canvas edge — there the width or height MAY
be the smaller remainder (`rect_x + rect_w == width` or
`rect_y + rect_h == height`). A decoder MUST reject a rectangle that
violates any of these rules.

### 4.3 Compositing

The canvas starts filled with `background_rgba`. For each frame in
order, the frame's rectangle is composited onto the canvas according
to its `blend_mode`: OVER (0) alpha-blends the frame's pixels over
the canvas; SOURCE (1) replaces the canvas pixels in the rectangle
with the frame's pixels. After the frame's `delay_ms` has elapsed,
its `disposal` applies: keep (0) leaves the frame's pixels on the
canvas for the next frame; restore-bg (1) restores the rectangle to
`background_rgba`.

### 4.4 Validity

An ANIM payload MUST have `frame_count >= 1`, and every frame entry
MUST have `blend_mode` and `disposal` in {0, 1}. A decoder MUST
reject an ANIM payload that violates either rule. A valid animated
file is one whose ANIM payload satisfies this section, whose
`tile_offset` values satisfy Section 4.1, and whose every frame's
TILE chunks cover its rectangle's tile grid exactly once — except for
a lossless frame, which MAY instead have exactly one TILE chunk at
(0, 0) holding the whole frame, in which case its rectangle MUST be
the full canvas.

---

## 5. rANS Entropy Coding

### 5.1 Parameters

- State: 32-bit unsigned integer
- Lower bound (L): 2^23 = 8388608
- Byte emit/consume (b): 2^8 = 256
- Upper bound: L × b = 2147483648
- Precision: 12 bits (frequency table sums to 4096)

### 5.2 Encoding

```
encode(state, freq, cum_freq):
    upper_bound = (L * b / M) * freq
    while state >= upper_bound:
        emit(state & 0xFF)
        state >>= 8
    state = (state / freq) * M + (state % freq) + cum_freq
```

### 5.3 Decoding

```
decode(state, table):
    cum_freq = state % M
    symbol = lookup(cum_freq)  // binary search or table lookup
    state = freq * (state / M) + (state % M) - sym.cum_freq
    while state < L:
        state = (state << 8) | read_byte()
    return symbol
```

### 5.4 Frequency Normalization

Frequencies are normalized to sum to M = 2^12 = 4096 using the FSE "spread" algorithm:
1. Scale all counts proportionally
2. Ensure every non-zero symbol gets freq ≥ 1
3. Adjust the most frequent symbol to make the total exact

### 5.5 Context Usage

- **Lossy mode**: 64 separate rANS tables per plane (one per DCT coefficient position in zigzag order). 3 planes (Y, Cb, Cr) = 192 total contexts.
- **Lossless mode**: Context hash from 3×3 pixel neighborhood (above, left, above-left). Up to 12 context sets via entropy image.

---

## 6. Intra Prediction Modes

| Mode | Name    | Description |
|------|---------|-------------|
| 0    | DC      | Mean of 8 above + 8 left pixels |
| 1    | V       | Copy above row |
| 2    | H       | Copy left column |
| 3    | TM      | left[r] + above[c] - above_left |
| 4    | DC_LEFT | Mean of left column only |
| 5    | DC_TOP  | Mean of above row only |
| 6    | DC_128  | Mid-value constant (128/512/2048) |
| 7    | D45     | 45° diagonal |
| 8    | D135    | 135° diagonal |
| 9    | D117    | ~117° vertical-right |
| 10   | D153    | ~153° horizontal-down |
| 11   | D207    | ~207° horizontal-up |
| 12   | D63     | ~63° vertical-left |
| 13   | PLANAR  | HEVC-style planar: `pred[r][c] = ((7-c)*left[r] + (c+1)*above[7] + (7-r)*above[c] + (r+1)*left[7] + 8) >> 4` (integer arithmetic, exact). A smooth 2D ramp is predicted as a ramp, so the slope survives even when the residual quantizes to zero. Only valid in layout `0x374D4843` (V7), `0x384D4843` (V8), `0x394D4843` (V9), `0x3a4d4843` (V10), or `0x3b4d4843` (V11); a decoder MUST reject mode 13 in any other layout. The encoder selects it through the quantization-aware RDO (section 8.2), which measures each mode's cost after quantization; under the old unquantized RDO it was disabled because it regressed photos.
| 14   | CCLM    | Cross-component linear model (chroma only): `pred[i] = clamp((a * luma_recon[i] + b) >> 8, 0, max_val)`, where `luma_recon` is the co-located reconstructed luma block (8x8; in YUV420 the 16x16 luma region downsampled to 8x8 by averaging each 2x2, rounded to nearest) and `(a, b)` are Q8 fixed-point parameters derived from the neighbouring reconstructed luma/chroma samples (the 8 above + 8 left + corner pairs) by integer least squares: `a = clamp(cov*256/var, -512, 512)`, `b = (sum_c*256 - a*sum_l)/n`, with `var = n*sum_ll - sum_l^2` and `cov = n*sum_lc - sum_l*sum_c` over the neighbour pairs; when the neighbour luma variance is ~0 the slope is 0 and `b` is the neighbour chroma mean. Neighbour luma samples are downsampled the same way in YUV420 (the above chroma row corresponds to luma rows `2*by0-2..2*by0-1` averaged; the left chroma column to luma cols `2*bx0-2..2*bx0-1` averaged). A block with no neighbours at all (top-left corner) predicts flat mid-grey (128). Only valid for chroma blocks in layout `0x3a4d4843` (V10) or `0x3b4d4843` (V11); a decoder MUST reject mode 14 in a luma mode stream or in any layout below V10. The encoder selects it through the quantization-aware RDO (section 8.2), with a ~6-bit mode-signalling charge so it only wins when its coefficient saving exceeds the mode-stream cost. Measured 2026-11-14 (6 benchmark photos): q75 files -1.87%, q85 -1.30%, PSNR within 0.01 dB everywhere; the largest win is horizon (-7.9% q75, -4.5% q85, sky/sea gradients with strong luma-chroma correlation). The chroma residual energy falls 41–59% under a per-block optimal fit (upper bound; the neighbour-derived fit achieves less), and chroma coefficient data is ~34% of a tile.

---

## 7. WKMETA Chunk — Structured Metadata

### 7.1 Envelope

| Offset | Size | Field        | Type   |
|--------|------|--------------|--------|
| 0      | 1    | wkmeta_version | uint8 (= 0x01) |
| 1      | 2    | entry_count  | uint16 |

### 7.2 Entry Format

| Offset | Size | Field        | Type   |
|--------|------|--------------|--------|
| 0      | 1    | namespace_id | uint8  |
| 1      | 2    | tag_id       | uint16 |
| 3      | 1    | type_id      | uint8  |
| 4      | 4    | value_size   | uint32 |
| 8      | N    | value        | value_size bytes |

`value_size` is authoritative for framing: an entry occupies exactly
8 header + `value_size` bytes. A decoder MUST reject a chunk whose
entry claims bytes past the chunk end, and MUST reject an entry whose
value does not consume exactly `value_size` bytes (fixed-size types
must match their section 7.4 size exactly). An ARRAY of
variable-length elements may leave padding at the end of its declared
value. An entry whose type is unknown, or whose value is malformed
for its type, MUST be preserved verbatim (header + declared value
bytes) so a later serialize writes back exactly the same bytes
(conformance rule 7).

### 7.3 Namespace Table

| ID   | Name     | Description |
|------|----------|-------------|
| 0x01 | CAPTURE  | Camera/capture device parameters |
| 0x02 | GEO      | Geographic location and motion |
| 0x03 | TIME     | Temporal metadata |
| 0x04 | RIGHTS   | Copyright, licensing, attribution |
| 0x05 | CONTENT  | Semantic content description |
| 0x06 | ANIM     | Animation-level metadata |
| 0x07 | REGION   | Spatial regions of interest |
| 0x08 | DEVICE   | Device and software environment |
| 0x09 | RATING   | Quality, audience, content ratings |
| 0x0A | CUSTOM   | Application-defined |
| 0x0B | PROV_REF | Reference into PROV chunk |

### 7.4 Type Table

| ID   | Name     | Size | Description |
|------|----------|------|-------------|
| 0x01 | UINT8    | 1    | Unsigned 8-bit |
| 0x02 | UINT16   | 2    | Unsigned 16-bit LE |
| 0x03 | UINT32   | 4    | Unsigned 32-bit LE |
| 0x04 | UINT64   | 8    | Unsigned 64-bit LE |
| 0x05 | INT32    | 4    | Signed 32-bit LE |
| 0x06 | FLOAT32  | 4    | IEEE 754 single |
| 0x07 | FLOAT64  | 8    | IEEE 754 double |
| 0x08 | RATIONAL | 8    | 2× INT32 (num, den) |
| 0x09 | LSTR     | var  | BCP-47 tag (uint8 len + bytes) + UTF-8 text |
| 0x0A | STR      | var  | UTF-8 string, no null terminator |
| 0x0B | BYTES    | var  | Raw byte array |
| 0x0C | ARRAY    | var  | uint16 count + uint8 element_type + elements |
| 0x0D | STRUCT   | var  | uint16 field_count + nested entries |
| 0x0E | TS64     | 8    | Unix timestamp, microseconds, uint64 |
| 0x0F | UUID     | 16   | RFC 4122 UUID |

### 7.5 GEO Namespace Tags

| Tag    | Name          | Type    | Description |
|--------|---------------|---------|-------------|
| 0x0001 | GEO_LAT       | FLOAT64 | WGS-84 latitude (+N, decimal degrees) |
| 0x0002 | GEO_LON       | FLOAT64 | WGS-84 longitude (+E, decimal degrees) |
| 0x0003 | GEO_ALT       | FLOAT32 | Altitude metres above WGS-84 ellipsoid |
| 0x0004 | GEO_HPOS_ERR  | FLOAT32 | Horizontal positioning error 1σ metres |
| 0x0005 | GEO_VPOS_ERR  | FLOAT32 | Vertical positioning error 1σ metres |
| 0x0006 | GEO_SPEED     | FLOAT32 | Ground speed m/s |
| 0x0007 | GEO_HEADING   | FLOAT32 | True heading 0..360° |
| 0x0008 | GEO_PITCH     | FLOAT32 | Device pitch -90..+90° |
| 0x0009 | GEO_ROLL      | FLOAT32 | Device roll -180..+180° |
| 0x000A | GEO_COUNTRY   | STR     | ISO 3166-1 alpha-2 |
| 0x000B | GEO_REGION    | STR     | ISO 3166-2 code |
| 0x000C | GEO_CITY      | LSTR    | City name (localized) |
| 0x000D | GEO_SUBLOC    | LSTR    | Sub-location |
| 0x000E | GEO_PLACE_NAME| LSTR    | Human-readable place name |
| 0x000F | GEO_WHAT3WORDS| STR     | what3words address |
| 0x0010 | GEO_PLUS_CODE | STR     | Open Location Code |
| 0x0011 | GEO_CAPTURE_TS| TS64    | UTC capture timestamp |
| 0x0012 | GEO_DEST_LAT  | FLOAT64 | Destination latitude |
| 0x0013 | GEO_DEST_LON  | FLOAT64 | Destination longitude |

*(Complete tag listings for all namespaces follow the same pattern as documented in wkmeta.hpp)*

### 7.6 REGION Namespace Tags

Region annotations are stored as a STRUCT value (one STRUCT per
region, under namespace REGION). String fields accept STR or LSTR on
read.

| Tag    | Name          | Type             | Description |
|--------|---------------|------------------|-------------|
| 0x0001 | NAME          | LSTR             | Region name |
| 0x0002 | TYPE          | UINT8            | Application-defined region type |
| 0x0003 | ROLE          | STR              | Role of the region (e.g. `subject`) |
| 0x0004 | RECT_NORM     | ARRAY of 4 FLOAT32 | Normalized rect (x, y, w, h), 0..1 |
| 0x0005 | CONFIDENCE    | FLOAT32          | Detection confidence, 0..1 |
| 0x0006 | PERSON_NAME   | LSTR             | Name of the person in the region |
| 0x0007 | PERSON_ID     | UUID             | Identity of the person |
| 0x0008 | OBJECT_CLASS  | STR              | Object class label |
| 0x0009 | CREATOR       | STR              | Who created the annotation |

Region annotations are written as STRUCT values, never as flat
REGION entries: a flat entry (e.g. `REGION.CONFIDENCE` on its own)
is not a region and is not read by `get_regions()`. The command-line
tools build the STRUCT from individual `--set REGION.*` options,
merging each field into the first STRUCT entry (creating it, with
tag 0x0001, when the file has none) and leaving any other STRUCT
entries untouched. Value formats on the command line: `RECT_NORM`
as `x,y,w,h` (each 0..1), `CONFIDENCE` as a decimal 0..1, `TYPE` as
an integer 0..255, `PERSON_ID` as an 8-4-4-4-12 hex UUID.

### 7.7 Compressed META payloads

A META chunk whose chunk flags byte has bit 3 set (`CHUNK_FLAG_META_COMPRESSED`) holds a zlib-compressed (RFC 1950, DEFLATE) form of the Section 7.1 envelope, not the envelope itself. The writer MUST compress only when the compressed form is strictly smaller than the raw payload; otherwise it writes the raw payload with the bit clear. The bit costs no bytes of its own (it lives in the existing flags byte), so any strict saving is a net win, and small payloads stay raw.

A decoder MUST decompress the payload before parsing when the bit is set, and MUST reject the file when the payload is not a valid zlib stream, has trailing bytes after the stream, or would expand past 16 MiB (a decompression bomb bound; real META payloads are kilobytes). A decoder that does not know the bit rejects the file cleanly anyway: it feeds the compressed bytes to the Section 7.2 parser, whose first check (wkmeta_version must be 0x01) fails on any zlib stream (first byte 0x78 for the usual window sizes, never 0x01).

ICCP and PROV chunks are never compressed. The container version is unchanged (0x0001).

---

## 8. DCT and Quantization

### 8.1 Transform

8×8 block DCT-II computed directly from a precomputed 8×8 cosine
table (`cos(pi * (2n+1) * k / 16)`, orthonormal scaling
α(0) = 1/√8, α(k) = 1/2), applied as a dense 8-point dot product
per coefficient. 2D separable: rows then columns (forward), columns
then rows (inverse). This is intentionally the straightforward
direct form, not the AAN fast algorithm: the transform is not the
bottleneck, and the direct form keeps forward and inverse exact
inverses of each other.

### 8.2 Quantization

64-element per-frequency tables derived from JPEG base tables with quality scaling:
- Quality < 50: scale = 5000 / quality
- Quality ≥ 50: scale = 200 - 2 × quality
- Bit depth scaling: Q10 = Q8 × 4, Q12 = Q8 × 16
- Base-table cap: the JPEG base value is capped at 40 before quality scaling (both luma and chroma). Without the cap, the large high-frequency base values (up to 121) resonate with the near-integer DCT coefficients of photo blocks, so the rounding error is not monotonic in the step size and PSNR falls as quality rises (measured 2026-11-13: mira q75 55.15 dB -> q80 44.77 dB; a single 8x8 block's luma SSE went 5.4 -> 834.8 because q75's steps divide its coefficients almost exactly while q80's do not). The cap keeps PSNR and file size monotonic in quality (verified q50..q100 on mira/ember/horizon). Encoder-side only: the capped tables are what get written into the file, so old files decode unchanged.
- Deadzone: |coefficient| < 0.5 × step → 0
- Chroma high-frequency deadzone: after quantization rounding, a chroma coefficient with zigzag index >= 24 and quantized value |q| <= 1 is set to 0 (8-bit only; see below). Luma is untouched, and so are chroma coefficients with zigzag index < 24 or |q| >= 2. Because the zeroing happens inside quantization, the quantization-aware RDO and the reconstruction used for prediction both see the zeroed coefficients. Encoder-side only: the stored tables and streams are unchanged, so old decoders decode new files identically. Measured 2026-11-22 (6 benchmark photos, q75, 8-bit): files -5.38% at -0.15 dB mean PSNR (worst image horizon -0.88 dB; meadow +0.05 dB). The rule does not apply at 10/12-bit: there the |q| = 1 chroma HF coefficients are signal, and zeroing them costs ~2.2 dB on gradient textures (measured on the 10/12-bit round-trip floors).
- Adaptive quantization (layout `0x374D4843` and later): a block's effective step is `table_step * scale`, where `scale` comes from that block's entry in the tile's per-block quant-scale stream (default `1.0`), or, in layout `0x394D4843` (V9), from its superblock's entry in the superblock quant map when the per-block scale is `1.0`. The scaled step applies to the deadzone test, the quantize rounding, and the dequantize multiplication alike, so encoder and decoder reconstruct the same values. The encoder's first scale policy is the seed boost: on a tile that is unambiguously a smooth chroma ramp (see section 8.2, mode selection) at quality below 75, the seed blocks -- the first row and first column, which have no above/left reference -- use scale `0.75` (index 2). The seed's block means otherwise sit on a 2-level grid (luma DC step 16 at q50) and every interior block copies the row above, extruding the seed's staircase down the tile; `0.75` puts the seed's means on a finer grid. Scale `0.5` was measured and rejected (worst step got worse, 4 -> 5) and scale `1.0` leaves the q50 ramp banded (80 distinct, worst step 4); `0.75` reaches 93 distinct (the colorspace floor) with worst step 3 at essentially the same file size. The mode decision uses the same scale as the quantize, so the two agree. The second scale policy is the Phase 2/3 control field via the V9 superblock map: a superblock whose blocks are at least 75% Textured uses scale `1.25` (index 5); all other superblocks stay at `1.0`. Measured 2026-11-14 (6 benchmark photos): q75 files -0.47%, q85 -0.25%, PSNR -0.09 dB / -0.05 dB, SSIM essentially unchanged. A lower threshold (0.5) was tried and rejected: meadow lost 1.27 dB PSNR at q75 for only 3.6% saving, because grass detail is real signal, not masking-friendly noise. The per-block V7 application of the same field was rejected earlier (files +24% to +39%) because the sparse per-block scale list costs 3 B per non-identity block; the superblock map is what makes the field cheap to signal.
- Mode selection (rate-distortion) is quantization-aware: for each candidate prediction mode, the residual is transformed, quantized (with the plane's table and the block's scale), clamped to the stream's symbol range, dequantized, and added back to the prediction. The cost is the SSD of that reconstructed block plus lambda times a bit estimate of the quantized coefficients. Measuring the cost before quantization (on the raw prediction and the unquantized residual) picks modes whose residuals look cheap but quantize badly -- it regressed photos by 5-7 dB and made quality non-monotonic in the quality setting. Chroma mode selection is joint (one mode for Cb and Cr) and, in layout V10, tries CCLM (mode 14) alongside the 14 intra modes; CCLM is charged ~6 extra bits in the RDO to account for its mode-stream signalling cost, which the coefficient-bit estimate does not include -- without the charge, marginal CCLM picks grew people photos by 0.6–0.8% at q85. Luma mode selection never tries CCLM (it is chroma-only).

### 8.3 Zigzag Scan

Standard JPEG 8×8 zigzag order for coefficient serialization.

---

## 9. Color Space

### 9.1 RGB ↔ YCbCr

Matrix selection via CICP matrix coefficient (H.273 Table 4):
- 1: BT.709
- 5/6: BT.601
- 9: BT.2020

Chroma subsampling: YUV420 stores chroma at half resolution in each dimension (a chroma sample covers a 2x2 luma block, averaged with rounding as `(sum + 2) / 4`, clamped at the image edge). Upsampling reconstructs full-resolution chroma with a bilinear filter: output pixel `(r, c)` samples the chroma plane at `(r*0.5 - 0.25, c*0.5 - 0.25)` with linear interpolation and coordinates clamped to the plane bounds, rounded to nearest. In layout V11 each 420 tile is upsampled independently to the tile's full-resolution region, with coordinates clamped to the tile's own chroma plane.

### 9.2 Transfer Functions

- PQ (CICP 16): exact ST 2084 EOTF/OETF
- HLG (CICP 18): exact BT.2100 HLG OETF/inverse OETF
- BT.709 (CICP 1): standard gamma
- sRGB (CICP 13): piecewise linear/power

### 9.3 HDR Pipeline

All HDR data stored as uint16_t. The codec works in the
nonlinear (PQ/HLG code-value) domain: YCbCr is computed from the
PQ/HLG-encoded code values directly, and the transfer function is
only signalled (CICP), never applied inside the codec — the EOTF /
OETF live at the display, not in the bitstream. This is what every
production HDR codec does (HEVC, VVC, AV1, AVIF, JPEG XL).

A linearise-first pipeline was measured and rejected (slice 0,
2026-11-17): quantising linear light uniformly at 10 bits alone
already costs PSNR 21–28 dB (max error up to 251 codes) before the
codec runs, and the full pipeline scores PSNR 14–19 dB vs 35–63 dB
for the nonlinear pipeline, with worse banding. The EOTF / OETF
functions (Section 9.2) stay in the codebase for the tone-mapping preview
and conversion tools, not for the codec core. Since slice 3 (2026-11-19)
the preview is real: wkview tone-maps PQ/HLG frames with Reinhard
(geometric-mean key value, `tools/tonemap.h`); this is viewer behaviour
only, never a bitstream change.

No bitstream change: the transfer is only signalled either way, so
files written before and after this amendment decode identically.

**Viewer-only change (slice 3, 2026-11-19):** wkview tone-maps HDR (PQ/HLG) 16-bit frames for display — PQ/HLG EOTF to linear light, Reinhard with a geometric-mean key value, sRGB OETF to 8-bit (`tools/tonemap.h`, `--linear` falls back to the old linear downscale); SDR 8-bit pixels are unchanged. Not a bitstream/format change: no file decodes differently.

---

## 10. Conformance

A conforming decoder MUST:
1. Validate the magic number and version
2. Parse FHDR before any other chunks
3. Ignore unknown optional chunks (flag bit0 set)
4. Reject unknown required chunks
5. Support all 13 prediction modes
6. Implement rANS decoding with the specified parameters
7. Preserve unknown WKMETA entries on round-trip
8. Never crash or invoke UB on malformed input
9. Reject an FHDR payload that is not exactly 19 bytes, and an ANIM
   payload whose size is not exactly `10 + frame_count * 16` bytes,
   whose `frame_count` is 0, or whose `blend_mode` or `disposal` is
   outside {0, 1}
10. Reject duplicate META, ICCP, PROV, and ANIM chunks (TILE is the only
    repeatable chunk), and reject a META chunk that fails to parse
    instead of silently dropping it
11. For a non-animated frame, reject TILE chunks that do not cover the
    frame grid exactly once: the chunk count must equal `tile_count()`
    and every `(tile_x, tile_y)` must appear exactly once; a lossless
    frame is the exception: it has either exactly one TILE chunk at
    (0,0) holding the whole frame (the pre-0.2.8 format) or exactly
    `tile_count()` TILE chunks covering the grid once (v0.2.8 per-tile
    lossless). A single tile at (0,0) on a multi-tile grid is read as
    the whole frame; a truncated per-tile payload decoded that way
    must fail in its rANS/LZ stream, never as wrong pixels
12. Reject an FHDR whose `cicp_matrix` is not one of 1, 5, 6, 9 or
    whose `cicp_transfer` is not one of 1, 13, 16, 18 (the only values
    the converter implements; anything else must not silently fall
    back to BT.709 / sRGB), whose `cicp_primaries` has no entry in
    ITU-T H.273 Table 2 (0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 22),
    or whose `HDR` flag disagrees with `bit_depth`/`cicp_transfer`
    (the flag must be set exactly when `bit_depth` > 8 or
    `cicp_transfer` is 16 or 18); and reject a file whose FHDR flags
    disagree with the chunks present: `HAS_WKMETA` must match the
    presence of a META chunk, `ANIMATED` the presence of an ANIM
    chunk, and `HAS_C2PA` the presence of a PROV chunk
13. For an animated file, reject TILE chunks that do not belong to
    exactly one frame: the first frame's `tile_offset` must be 0,
    offsets must be strictly increasing and smaller than the number
    of TILE chunks, every frame's rectangle must lie inside the canvas
    with non-zero width and height and be aligned to the tile grid
    (Section 4.2), and every frame's TILE chunks must cover its
    rectangle's tile grid exactly once — except for a lossless frame,
    which may instead have exactly one TILE chunk at (0, 0) holding
    the whole frame, in which case its rectangle must be the full
    canvas (Section 4.4)
