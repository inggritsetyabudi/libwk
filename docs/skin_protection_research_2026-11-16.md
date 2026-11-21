# Skin Protection — Measurement Study (Control-Field v2, Application 2)

**Date:** 2026-11-16  
**Diagnostic Tool:** `tools/skin_protection_diag.cpp` (luma-only, flat quality, tile size 1024)  
**Raw Data:** `~/workspace/libwk-exp-n1/skin_protection_q70.txt`, `skin_protection_q75.txt`, `skin_protection_q80.txt`  
**Status:** MEASUREMENT COMPLETE — Findings indicate **SKIN_A** (scale 0.5) is a viable candidate, whereas **SKIN_B** (scale 0.75) is ruled out due to RDO regressions. SKIN_A is the first control-field application to clear the quantitative measurement bar—subject to one candid caveat in Section 6: perceptual quality gains occur across the detected skin region as a whole, whereas SSIM gains within ground-truth face/arm bounding boxes remain below the perceptual visibility threshold.

---

## 0. Executive Summary

| Variant (q75, luma-only) | Δ Luma Size | Δ Total File Size | Δ SkinDet PSNR | Δ SkinDet SSIM | Δ GTskin PSNR | Δ GTskin SSIM | Lagrangian (Equal PSNR) |
|---|---|---|---|---|---|---|---|
| **BASE** | 689,040 B | — | 38.134 dB | 0.97301 | 42.265 dB | 0.99598 | — |
| **SKIN_A** (Skin blocks, V7 scale 0.5) | **+27,032 B (+3.92% luma, +2.56% total)** | +2.56% | **+5.901 dB** | **+0.01766** (photo-mean) / **+0.00860** (block-weighted) | **+1.312 dB** | +0.00038 / **+0.00086** (weighted) | **−19,840 B (−1.88%)** |
| **SKIN_B** (Skin blocks, V7 scale 0.75) | +14,098 B (+2.05% luma, +1.33% total) | +1.33% | +2.644 dB | +0.01051 / +0.00492 | +0.610 dB | +0.00018 / +0.00041 | −9,692 B (−0.92%) |

**Equal-File-Size Comparison** (linear interpolation of the q70/q75/q80 RD curves to match BASE q75 file size; see Section 4):

| Variant @ BASE q75 File Size | Δ Global SSIM | Δ SkinDet SSIM | Δ GTskin SSIM | Δ GTskin PSNR |
|---|---|---|---|---|
| **SKIN_A** | **−0.00123** | **+0.01688** (photo-mean) | +0.00009 | **+0.962 dB** |
| **SKIN_B** | −0.00073 | +0.00921 | +0.00002 | +0.409 dB |

**Equal-SSIM Cost** (overhead required to match BASE global SSIM): **SKIN_A** requires **+20,268 B (+1.92% total file)**; **SKIN_B** requires **+11,825 B (+1.12% total file)**.

**Hypothesis Confirmed:** The scale stream is inexpensive—6,285 B, accounting for only **23%** of SKIN_A's overhead (compared to 66% / 55,365 B in the Sky study). However, coefficient bits now dominate at +20,608 B (76%). On a per-block basis, skin is significantly more expensive than sky (12.95 B/block vs. 4.52 B/block for Sky) because skin blocks are textured; total overhead remains small solely due to the low block count (2,087 vs. 18,449).

**Why This Passed While Sky Failed:** At equal file size, SKIN_A trades a −0.00123 global SSIM drop (well below the ~0.005 visibility threshold) for a +0.0086 SkinDet SSIM gain (above the visibility threshold)—placing the global loss and regional gain on opposite sides of the perceptual threshold. Sky exhibited the exact inverse: −0.00417 global loss (approaching the threshold) for a +0.00227 regional gain (below the threshold), at 3.5× the file size cost (+6.82% vs. +1.92%).

**Primary Caveat (Section 6):** Approximately 45% of treated blocks are not human skin (lake reflections and rocks in *noctis*, cream surfaces, and flowers). The SSIM gain on true faces/arms falls below the perceptual visibility threshold; the visible gain occurs across the detected skin region as a whole.

---

## 1. Methodology

### 1.1 Test Harness

The `tools/skin_protection_diag.cpp` diagnostic reuses the validated RDO harness from `tools/control_field_diag.cpp` and `tools/sky_antibanding_diag.cpp`, incorporating the `classify_v2` classifier copied verbatim from `tools/classifier_diag.cpp` (Run C):

* **Encoding Simulation:** Luma-only, flat quality, 1024 tile size, 14 DCT modes, RDO cost $= \text{recon\_SSD} + \lambda \cdot \text{est\_bits}$, where $\lambda = 17.135$ at q75 ($\lambda = 0.85 \cdot 2^{(\text{qp}-12)/3}$).
* **True Coded Bitstream Size:** Measured via the library's actual rANS entropy coder (mode stream, span stream, and coefficient ladder via `encode_lossy_plane_payload`).
* **SSIM Metric:** Exact `wkmetric` formula (8×8 non-overlapping windows) evaluated on the luma plane across two skin-region definitions:
  1. **Detected Skin (`SkinDet`):** Blocks labeled as `Skin` by the classifier.
  2. **Ground-Truth Skin (`GTskin`):** Blocks whose centers fall inside manual face/arm GT bounding boxes (8 boxes, 918 blocks; combined recall is 34.4%—316/918 blocks—making GT gains partial by construction).
  * Aggregate SSIM is reported in two ways: **photo-mean** (unweighted average across the 6 test images, consistent with prior studies) and **block-weighted** (weighted by block count). Block-weighted is the more accurate metric because photo-mean is skewed by *horizon*, which contains only 4 `Skin` blocks ($\Delta\text{SkinDet SSIM} = +0.0808$ across those 4 blocks alone).
* **Variants Evaluated per Image:**
  * **BASE:** Unmodified baseline encoding.
  * **SKIN_A:** All detected `Skin` blocks scaled by `0.5`.
  * **SKIN_B:** All detected `Skin` blocks scaled by `0.75`.
* **Scale Stream Accounting:** Derived from `write_quant_scale_stream` (`src/lossy_tile_encode.cpp`): `u16` block count + 3 B per non-identity block per tile. BASE tiles use V6 and incur 0 B overhead.
* **GT Bounding Boxes:** Copied verbatim from `classifier_diag.cpp` (`kGt`), including 4 "not-Skin" confuser boxes (2 beige walls in *solis*, cream background in *ember*, gray wall in *mira*) to quantify false-positive (FP) blocks receiving treatment.

### 1.2 Harness Validation (Mandatory Pre-Checks)

* **Quantization Self-Check (executed at the start of every run):** Local quantization paths at scales `1.0`, `0.5`, and `0.75` must match the library's `quantize_scaled` bit-for-bit — **passed** across all runs.
* **BASE q75 Parity Check (vs. Control-Field v1 and Sky studies):** Mode stream 49,561 B, span stream 70,720 B, coefficient stream 568,759 B, total luma 689,040 B, Y-PSNR 42.4141 dB, SSIM 0.98072 — **exact match**. Harness is valid.
* **Sanity Check on Near-Skinless Images (*horizon* / *meadow* / *noctis*):** SKIN_A/B modify only detected `Skin` blocks (4 / 26 / 1,289 blocks, respectively); images without GT boxes show zero change on GT metrics (PSNR 100.000 dB).
* **Total Detected Skin Blocks:** 2,087 blocks across all 6 images — exact match with Run C cross-tabulation totals (*ember* 333, *mira* 106, *solis* 329, *horizon* 4, *meadow* 26, *noctis* 1,289).

### 1.3 Methodological Caveats

* **Luma-Only Scope:** Reported byte sizes reflect the luma plane only; total full-color lossy file size at q75 (including chroma and headers) is 1,056,775 B. If skin protection is also applied to chroma (4:4:4), the chroma scale stream and coefficients will add overhead—making these figures a **lower bound** on total cost.
* **SSIM as Perceptual Proxy:** VMAF is unavailable; regional SSIM serves as the perceptual proxy, consistent with Control-Field v1 and Sky studies. Because luma-only simulations do not output decodable full-color images, visual inspection was not performed in this study and remains necessary to visually verify the findings.
* **Coarse GT Bounding Boxes:** Face/arm bounding boxes include hair, eyes, lips, and background edges, meaning GT gains represent a lower bound on pure skin-tone gains, compounded by the 34.4% combined classifier recall.
* **Equal-File Interpolation (Section 4):** Linear interpolation between quality operating points; SKIN_A/B are interpolated strictly between q70 and q75 (no extrapolation), avoiding the extrapolation issues encountered in the Sky study.

---

## 2. Per-Image Results at q75

Detected `Skin` block distribution per image: *ember* 333 (39.6%), *mira* 106 (12.6%), *solis* 329 (8.4%), *horizon* 4 (0.03%), *meadow* 26 (0.49%), *noctis* 1,289 (1.41%). Total: **2,087 blocks**.

### 2.1 `ember-7f3a` (225×225) — Highest Skin Coverage

| Variant | Total Luma | Y-PSNR | Global SSIM | SkinDet PSNR | SkinDet SSIM | GTskin PSNR | GTskin SSIM |
|---|---|---|---|---|---|---|---|
| BASE | 14,094 B | 40.4608 | 0.98997 | 40.577 | 0.99359 | 42.896 | 0.99690 |
| SKIN_A | 17,478 B (**+3,384, +24.0%**) | 41.8435 | 0.99160 | 45.716 (**+5.139**) | 0.99793 (**+0.00434**) | 45.252 (**+2.356**) | 0.99785 (**+0.00095**) |
| SKIN_B | 16,313 B (+2,219) | 41.1474 | 0.99046 | 42.667 (+2.090) | 0.99577 (+0.00218) | 44.072 (+1.176) | 0.99748 (+0.00058) |

### 2.2 `mira-5b8d` (217×233) — Fair Skin, Lowest Recall

| Variant | Total Luma | Y-PSNR | Global SSIM | SkinDet PSNR | SkinDet SSIM | GTskin PSNR | GTskin SSIM |
|---|---|---|---|---|---|---|---|
| BASE | 7,430 B | 46.9296 | 0.99320 | 44.678 | 0.99549 | 47.114 | 0.99747 |
| SKIN_A | 8,678 B (**+1,248, +16.8%**) | 47.4986 | 0.99342 | 48.341 (**+3.663**) | 0.99750 (+0.00201) | 48.223 (**+1.109**) | 0.99767 (+0.00020) |
| SKIN_B | 8,143 B (+713) | 46.7581 | 0.99320 | 43.854 (**−0.824**) | 0.99566 (+0.00017) | 46.714 (**−0.400**) | 0.99739 (−0.00008) |

**SKIN_B regresses** on *mira* at q75: SkinDet PSNR drops by −0.824 dB, GT PSNR by −0.400 dB, and global Y-PSNR by −0.17 dB relative to BASE. See Section 5.1—this is an RDO artifact rather than random noise, and is the primary reason SKIN_B is ruled out.

### 2.3 `solis-2c9e` (663×372)

| Variant | Total Luma | Y-PSNR | Global SSIM | SkinDet PSNR | SkinDet SSIM | GTskin PSNR | GTskin SSIM |
|---|---|---|---|---|---|---|---|
| BASE | 26,200 B | 41.7974 | 0.98031 | 37.837 | 0.99212 | 40.920 | 0.98153 |
| SKIN_A | 30,576 B (**+4,376, +16.7%**) | 42.4072 | 0.98072 | 42.046 (**+4.209**) | 0.99647 (+0.00435) | 42.029 (**+1.109**) | 0.98265 (+0.00112) |
| SKIN_B | 28,356 B (+2,156) | 42.1271 | 0.98054 | 39.713 (+1.876) | 0.99443 (+0.00231) | 41.524 (+0.604) | 0.98210 (+0.00057) |

### 2.4 `horizon` / `meadow` / `noctis` — Virtually No Human Skin

| Image | Variant | Total Luma | SkinDet PSNR | SkinDet SSIM | Notes |
|---|---|---|---|---|---|
| `horizon` | BASE → SKIN_A | 84,932 → 84,980 B (+48) | 35.675 → 40.582 (+4.907) | 0.87627 → 0.95707 (+0.0808) | 4 FP blocks (warm terrain / flowers) |
| `meadow` | BASE → SKIN_A | 72,986 → 73,868 B (+882) | 37.472 → 42.384 (+4.912) | 0.99504 → 0.99827 (+0.00323) | 26 blocks = pink flowers (FP) |
| `noctis` | BASE → SKIN_A | 483,398 → 500,492 B (**+17,094, +3.5%**) | 37.547 → 44.175 (**+6.628**) | 0.98556 → 0.99677 (+0.01121) | 1,289 blocks — mostly lake/rock FPs (Section 6) |

*Noctis* accounts for **63% of total file overhead** (+17,094 B out of +27,032 B) despite containing zero faces; all SkinDet gains on *noctis* occur on rocks, lake reflections, and flowers misclassified as skin.

### 2.5 Aggregate Totals Across All 6 Images (q75, Luma)

| Variant | Mode | Span | Coeff | Scale | TOTAL | Y-PSNR | Global SSIM | SkinDet PSNR | SkinDet SSIM | GTskin PSNR | GTskin SSIM |
|---|---|---|---|---|---|---|---|---|---|---|---|
| BASE | 49,561 | 70,720 | 568,759 | 0 | **689,040** | 42.4141 | 0.98072 | 38.134 | 0.97301 | 42.265 | 0.99598 |
| SKIN_A | 49,547 | 70,873 | 589,367 | 6,285 | **716,072 (+27,032)** | 42.5733 | 0.98113 | 44.035 | 0.99067 | 43.578 | 0.99636 |
| SKIN_B | 49,553 | 70,831 | 576,469 | 6,285 | **703,138 (+14,098)** | 42.5110 | 0.98086 | 40.779 | 0.98352 | 42.876 | 0.99616 |

*(SSIM figures above are photo-mean; block-weighted aggregates are in Section 2.6. Non-skin regions do not regress: SKIN_A `Other` PSNR is 42.549 vs. BASE 42.548, and `Other` SSIM is 0.97960 vs. 0.97963.)*

### 2.6 Block-Weighted Aggregates (q75) — More Accurate Baseline

| Variant | Δ SkinDet SSIM (Weighted, 2,087 blocks) | Δ GT SSIM (Weighted, 918 blocks) |
|---|---|---|
| **SKIN_A** | **+0.00860** | +0.00086 |
| **SKIN_B** | +0.00492 | +0.00041 |

Photo-mean inflates the apparent gains because *horizon* (4 blocks, +0.0808) and *noctis* (1,289 FP blocks, +0.01121) skew the unweighted average. Even after block-weighting, SKIN_A's $\Delta\text{SkinDet SSIM}$ **remains above the ~0.005 perceptual threshold**, while $\Delta\text{GT SSIM}$ remains below it.

---

## 3. Cost Decomposition — Coefficients Dominate, Signaling Is Cheap

| Variant | Δ Total | Δ Scale Stream | Δ Coeff | Δ Span | Δ Mode |
|---|---|---|---|---|---|
| **SKIN_A** | +27,032 | **+6,285 (23.2%)** | **+20,608 (76.2%)** | +153 | −14 |
| **SKIN_B** | +14,098 | **+6,285 (44.6%)** | +7,710 (54.7%) | +111 | −8 |

The scale stream costs $2,087\text{ blocks} \times 3\text{ B} + 2\text{ B} \times 9\text{ tiles} = 6,285\text{ B}$—**its size is independent of the scale factor**, identical to the Sky behavior. Per block, SKIN_A costs 12.95 B/block (3.01 B scale + 9.87 B coeff), whereas Sky cost only 4.52 B/block (3.00 B scale + 1.46 B coeff). Skin blocks contain fine texture, so finer quantization generates substantial non-zero coefficients; sky blocks are smooth and produce minimal additional coefficient bits. Total Skin overhead stays low purely because fewer blocks are targeted.

**Architectural Implication:** Lower overhead cannot be achieved simply by choosing a milder scale factor (the scale stream stays fixed at 6,285 B; SKIN_B only saves coefficient bits). Meaningful cost reductions require either:
1. **Higher classifier precision:** ~45% of treated blocks are false positives (Section 6), with *noctis* alone driving 63% of total overhead.
2. **Selective subset treatment:** Applying scaling only to high-confidence facial blocks (not yet measured).

---

## 4. Equal-File Comparison (RD Curves at q70 / q75 / q80)

Total luma bytes and photo-mean SSIM across quality settings:

| q | BASE Bytes | BASE SSIM | BASE SkinDet SSIM | BASE GT SSIM | SKIN_A Bytes | SKIN_A SSIM | SKIN_A SkinDet SSIM | SKIN_A GT SSIM | SKIN_B Bytes | SKIN_B SSIM | SKIN_B SkinDet SSIM | SKIN_B GT SSIM |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 70 | 614,492 | 0.97615 | 0.96315 | 0.99512 | 640,845 | 0.97657 | 0.98849 | 0.99556 | 628,137 | 0.97624 | 0.97660 | 0.99529 |
| 75 | 689,040 | 0.98072 | 0.97301 | 0.99598 | 716,072 | 0.98113 | 0.99067 | 0.99636 | 703,138 | 0.98086 | 0.98352 | 0.99616 |
| 80 | 830,360 | 0.98554 | 0.98249 | 0.99703 | 857,892 | 0.98583 | 0.99398 | 0.99729 | 844,834 | 0.98575 | 0.98891 | 0.99721 |

**Linear Interpolation to BASE q75 File Size (689,040 B)** — strict interpolation between q70 and q75:
* **SKIN_A** ($f = 0.641$): Global SSIM `0.97949` (**−0.00123**), SkinDet SSIM `0.98989` (**+0.01688**), GT SSIM `0.99607` (**+0.00009**), GT PSNR `+0.962 dB`, SkinDet PSNR `+5.302 dB`.
* **SKIN_B** ($f = 0.812$): Global SSIM `0.97999` (**−0.00073**), SkinDet SSIM `0.98222` (**+0.00921**), GT SSIM `0.99600` (**+0.00002**), GT PSNR `+0.409 dB`.

**Equal-SSIM Cost** (overhead required to match BASE q75 global SSIM):
* **SKIN_A** requires **+20,268 B (+1.92% total file)**.
* **SKIN_B** requires **+11,825 B (+1.12% total file)**.

**Lagrangian Equal-PSNR** (per quality level, true coded bytes):
* **SKIN_A:** −2.18% / −1.88% / −1.34% (q70 / q75 / q80).
* **SKIN_B:** −1.12% / −0.92% / −0.60% (q70 / q75 / q80).

The trend is consistent across all quality levels: this is a rate-quality allocation trade-off rather than a net compression gain. The evaluation criterion is whether the perceptual quality purchased justifies the bitrate overhead—and as established in Section 0, it does for SKIN_A, subject to the caveats in Section 6.

---

## 5. Two Important Secondary Findings

### 5.1 SKIN_B Regression on *mira* at q75 — RDO Artifact, Not Noise

On *mira* at q75, SKIN_B regresses by −0.824 dB SkinDet PSNR, −0.400 dB GT PSNR, and −0.17 dB global Y-PSNR relative to BASE, while SSIM stays virtually flat (+0.00017 SkinDet). At q70, SKIN_B gains +0.755 dB SkinDet PSNR on *mira*, and at q80 it gains +4.249 dB. Thus, the regression is specific to q75 rather than inherent to the variant: at scale `0.75`, the RDO bit estimator (calibrated for scale `1.0`) misjudges mode costs on *mira*'s smooth, pale skin (where recall is only 25%), causing RDO to pick modes with worse true rate-distortion efficiency. SKIN_A exhibits zero regressions across any image or quality level. **SKIN_B is ruled out**—its gains are smaller and it is susceptible to RDO regressions that scale `0.5` avoids.

### 5.2 *Noctis*: 63% of Overhead Spent on Zero Faces

Of SKIN_A's +27,032 B overhead, +17,094 B comes from *noctis*—an image with no human faces. Its 1,289 `Skin` blocks consist predominantly of misclassified lake reflections and warm-toned rocks (Section 6). While this does not invalidate SKIN_A (the overhead is already factored into the +1.92% equal-SSIM cost), it demonstrates that improving classifier precision will substantially cut bitrate overhead without sacrificing facial quality gains.

---

## 6. Known Limitations (Candid Assessment)

1. **~45% of Treated Blocks Are Not Human Skin:** Out of 2,087 detected `Skin` blocks, roughly 935 are false positives: ~616 blocks in *noctis* lake reflections (4% of the 15,400-block lake box), ~240 blocks in *noctis* brown rocks (13% of the 1,850-block rock box), 26 pink flower blocks in *meadow*, and 49 wall/cream background blocks (*ember* 32, *solis* 15, *mira* 2—measured directly from "not-Skin" confuser boxes). While those pixels still receive higher fidelity (causing no visual degradation), from a skin-protection standpoint, that bitrate is wasted overhead.
2. **SSIM Gains on True Faces/Arms Fall Below the Perceptual Visibility Threshold:** Block-weighted $\Delta\text{GT SSIM}$ is +0.00086 (same-quant) and +0.00009 (equal-file)—both below 0.005. Metrics exceeding the perceptual threshold are the detected-skin regional SSIM (+0.0086 weighted) and GT PSNR (+1.31 dB same-quant / +0.96 dB equal-file). Coarse GT bounding boxes (which include hair, eyes, and background) dilute the GT numbers, but if the acceptance bar requires a visible SSIM improvement strictly within facial bounding boxes, SKIN_A has not yet cleared it.
3. **Uneven Per-Image Cost Distribution:** Portrait-heavy images incur steep local overheads—*ember* +24.0% luma, *mira* +16.8%, *solis* +16.7% (at q75, SKIN_A)—because skin makes up a large fraction of the frame. On a portrait-dominated photo library, average bitrate overhead will approach these figures rather than the +2.56% benchmark average (where half the dataset consists of landscapes).
4. **Luma-Only Measurement; Chroma Unaccounted For:** Extending skin protection to chroma planes will increase file size via additional chroma scale stream and coefficient bits. All figures here represent lower bounds.
5. **No Subjective Visual Inspection Yet:** Luma-only simulations do not produce decodable full-color images. Evaluating full-color decoded portrait test images (e.g., via `wkview` and downscaled JPEG checks) remains necessary to visually verify the measured gains.

---

## 7. Conclusions and Future Work

1. **Viability of SKIN_A (0.5 Quantization Scale):** Among the tested variants, **SKIN_A** is the first control-field candidate to meet the quantitative measurement criteria. At equal file size, it trades a negligible global SSIM decrease (−0.00123) for a perceptually meaningful gain in detected-skin SSIM (+0.0086 block-weighted), with an equal-SSIM bitrate overhead of +1.92% and zero regression in non-skin regions (`Other` SSIM −0.00003). Its rate-distortion behavior remains consistent across q70, q75, and q80.
2. **Exclusion of SKIN_B (0.75 Quantization Scale):** **SKIN_B** yields smaller regional gains and exhibits a PSNR regression on *mira* at q75 (−0.824 dB SkinDet PSNR) caused by RDO bit-estimation mismatch at the 0.75 scale factor. Consequently, it is ruled out as a viable candidate.
3. **Required Chroma and Perceptual Verification:** Because this study is limited to luma-only simulation and SSIM proxies, two empirical validation steps are required before drawing final integration conclusions:
   * Measuring the rate-distortion impact when quantization scaling is extended to chroma planes.
   * Conducting subjective visual inspections on full-color decoded portrait images (*ember*, *mira*, and *solis*) to verify that measured SSIM gains translate into visible facial quality improvements without visual artifacts.
4. **Classifier Precision as the Primary Cost-Reduction Lever:** Approximately 45% of treated blocks are false positives, with *noctis* alone generating 63% (+17,094 B) of the total bitrate overhead. Improving classifier precision to reject warm rocks and water reflections offers the largest opportunity to cut bitrate overhead without reducing facial quality gains.
5. **Next Research Direction:** Subsequent control-field measurements should evaluate **TextUI sharpness** (Application 3), which will require sourcing real text-heavy test images since the current benchmark dataset lacks text content. Foliage remains deprioritized.


