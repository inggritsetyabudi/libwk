# WK Image Format — Bitstream Specification v0.1

**MIME type:** `image/x-wk`  
**File extension:** `.wk`  
**Byte order:** Little-endian throughout  
**Format version:** 0x0001

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
| 4      | 1    | Flags   | Bit field: bit0=optional, bit1=metadata-only, bit2=repeatable |
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

---

## 2. FHDR Chunk — Frame Header (20 bytes)

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
| 3   | HDR            | Bit depth > 8 or non-sRGB transfer |
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

`layer_flags`: bit0 = has base layer, bit1 = has refinement layers, bit2 = has alpha plane.

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
8. **Optional alpha extension**: present when `layer_flags & 0x04` is set. The extension stores 64 x uint16 alpha quantization steps, then an alpha prediction mode stream using the same layout-tag-defined signaling, then an alpha coefficient-span stream when the layout tag is `0x334D4843`, `0x344D4843`, `0x354D4843`, or `0x364D4843`, then an alpha maximum coefficient extent when the layout tag is `0x354D4843` or when layout tag `0x364D4843` has syntax flag bit `1` set, then one rANS-coded coefficient stream per coefficient position on the full-resolution alpha grid.

### 3.3 Lossless Tile Payload

1. **Transform flags**: uint8 (bit0 = palette mode)
2. **Predictor mode**: uint8 (0..12)
3. **Per-channel data**: frequency table (256 × uint16) + rANS bitstream

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

---

## 8. DCT and Quantization

### 8.1 Transform

8×8 block DCT using AAN fast algorithm (5 multiplications, 29 additions per 1D pass).
2D separable: rows then columns (forward), columns then rows (inverse).

### 8.2 Quantization

64-element per-frequency tables derived from JPEG base tables with quality scaling:
- Quality < 50: scale = 5000 / quality
- Quality ≥ 50: scale = 200 - 2 × quality
- Bit depth scaling: Q10 = Q8 × 4, Q12 = Q8 × 16
- Deadzone: |coefficient| < 0.5 × step → 0

### 8.3 Zigzag Scan

Standard JPEG 8×8 zigzag order for coefficient serialization.

---

## 9. Color Space

### 9.1 RGB ↔ YCbCr

Matrix selection via CICP matrix coefficient (H.273 Table 4):
- 1: BT.709
- 5/6: BT.601
- 9: BT.2020

### 9.2 Transfer Functions

- PQ (CICP 16): exact ST 2084 EOTF/OETF
- HLG (CICP 18): exact BT.2100 HLG OETF/inverse OETF
- BT.709 (CICP 1): standard gamma
- sRGB (CICP 13): piecewise linear/power

### 9.3 HDR Pipeline

All HDR data stored as uint16_t. PQ/HLG EOTF applied to linearize before color transform; OETF applied after reconstruction.

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
