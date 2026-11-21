# Skin protection — riset pengukuran (control-field v2, aplikasi 2)

**Tanggal:** 2026-11-16
**Diagnostik:** `tools/skin_protection_diag.cpp` (luma-only, flat, tile 1024)
**Data mentah:** `~/workspace/libwk-exp-n1/skin_protection_q70.txt`, `skin_protection_q75.txt`, `skin_protection_q80.txt`
**Status:** PENGUKURAN SELESAI — rekomendasi: **BANGUN SKIN_A** (scale 0,5); **JANGAN BANGUN SKIN_B** (scale 0,75). Ini aplikasi control-field pertama yang lolos bar pengukuran — tapi dengan satu caveat jujur di Bagian 6: gain yang terlihat ada pada region kulit terdeteksi, sementara gain SSIM pada wajah/lengan sebenarnya (kotak GT) di bawah ambang terlihat.

## 0. Ringkasan eksekutif

| Varian (q75, luma-only) | Δ file luma | Δ file penuh | Δ SkinDet PSNR | Δ SkinDet SSIM | Δ GTskin PSNR | Δ GTskin SSIM | Lagrangian (equal PSNR) |
|---|---|---|---|---|---|---|---|
| **BASE** | 689.040 B | — | 38,134 dB | 0,97301 | 42,265 dB | 0,99598 | — |
| **SKIN_A** (blok Skin, V7 scale 0,5) | **+27.032 B (+3,92% luma, +2,56% file)** | +2,56% | **+5,901 dB** | **+0,01766** (photo-mean) / **+0,00860** (block-weighted) | **+1,312 dB** | +0,00038 / **+0,00086** (weighted) | **−19.840 B (−1,88%)** |
| **SKIN_B** (blok Skin, V7 scale 0,75) | +14.098 B (+2,05% luma, +1,33% file) | +1,33% | +2,644 dB | +0,01051 / +0,00492 | +0,610 dB | +0,00018 / +0,00041 | −9.692 B (−0,92%) |

Perbandingan **equal-file** (interpolasi kurva RD q70/q75/q80 ke file BASE q75, Bagian 4):

| Varian @ file BASE q75 | Δ SSIM global | Δ SkinDet SSIM | Δ GTskin SSIM | Δ GTskin PSNR |
|---|---|---|---|---|
| SKIN_A | **−0,00123** | **+0,01688** (photo-mean) | +0,00009 | **+0,962 dB** |
| SKIN_B | −0,00073 | +0,00921 | +0,00002 | +0,409 dB |

Biaya untuk menyamai SSIM global BASE (equal-SSIM): SKIN_A **+20.268 B (+1,92% file)**, SKIN_B +11.825 B (+1,12% file).

**Hipotesis terkonfirmasi:** scale stream murah — 6.285 B, hanya **23%** dari biaya SKIN_A (vs 66% / 55.365 B pada Sky). Tapi koefisien kini mendominasi: +20.608 B (76%). Per blok, kulit jauh lebih mahal dari langit (12,95 B/blok vs 4,52 B/blok pada Sky) karena blok kulit bertekstur — totalnya kecil hanya karena bloknya sedikit (2.087 vs 18.449).

**Mengapa ini lolos sementara Sky tidak:** pada file yang sama, SKIN_A menukar −0,00123 SSIM global (jauh di bawah ambang terlihat ~0,005) demi +0,0086 SkinDet SSIM (di atas ambang terlihat) — global loss dan region gain berada di sisi berlawanan dari ambang. Sky justru sebaliknya: −0,00417 global (mendekati ambang) demi +0,00227 region (di bawah ambang), dengan biaya 3,5× lipat (+6,82% file vs +1,92%).

**Caveat utama (Bagian 6):** ~45% blok yang diperlakukan bukan kulit manusia (pantulan danau + batu noctis, krem, bunga). Gain SSIM pada wajah/lengan sebenarnya di bawah ambang terlihat — yang terlihat adalah gain pada region kulit terdeteksi secara keseluruhan.

## 1. Metode

### 1.1 Harness

Diagnostik `tools/skin_protection_diag.cpp` memakai ulang harness RDO `tools/control_field_diag.cpp` + `tools/sky_antibanding_diag.cpp` (tervalidasi), dengan klasifikasi `classify_v2` dari `tools/classifier_diag.cpp` (Run C) disalin verbatim:

- Simulasi luma-only, flat quality, tile 1024, 14 mode DCT, biaya RDO = recon_SSD + λ·est_bits, λ = 17,135 pada q75 (λ = 0,85·2^((qp−12)/3)).
- Ukuran coded **sejati** lewat mesin rANS library (mode stream, span stream, coefficient ladder `encode_lossy_plane_payload`).
- SSIM = rumus persis `wkmetric` (jendela 8×8 non-overlapping) pada bidang luma. Dua pandangan region kulit: **(a) Detected Skin** — blok yang classifier sebut Skin; **(b) GT skin** — blok yang pusatnya di dalam kotak GT wajah/lengan manual (8 kotak, 918 blok; recall gabungan 34,4% — 316/918 — jadi gain GT parsial by construction). SSIM agregat dilaporkan dua cara: photo-mean (rata-rata 6 foto, seperti riset sebelumnya) dan **block-weighted** (tertimbang jumlah blok) — yang terakhir lebih jujur karena photo-mean terdistorsi oleh horizon yang hanya punya 4 blok Skin (dDetSSIM +0,0808 pada 4 blok itu saja).
- Tiga varian per foto: **BASE** (tanpa perlakuan), **SKIN_A** (semua blok Skin scale 0,5), **SKIN_B** (semua blok Skin scale 0,75).
- Akuntansi scale stream dari `write_quant_scale_stream` (`src/lossy_tile_encode.cpp`): u16 count + 3 B per blok non-identitas, per tile. Tile BASE adalah V6 dan membayar 0.
- Kotak GT disalin verbatim dari `classifier_diag.cpp` (kGt) — termasuk 4 kotak confuser "not-Skin" (dinding beige solis ×2, krem ember, dinding abu mira) untuk menghitung blok FP yang ikut diperlakukan.

### 1.2 Validasi harness (wajib lolos sebelum angka dipercaya)

- **Self-check kuantisasi** (di awal setiap run): jalur kuantisasi saya dengan scale 1,0 / 0,5 / 0,75 harus sama persis dengan library `quantize_scaled` — **lolos** di semua run.
- **BASE q75 vs riset control-field v1 dan riset Sky:** mode 49.561 B, span 70.720 B, coeff 568.759 B, total luma 689.040 B, Y-PSNR 42,4141 dB, SSIM 0,98072 — **sama persis**. Harness valid.
- **Sanity horizon/meadow/noctis** (hampir tanpa kulit): SKIN_A/B hanya mengubah blok Skin yang ada (4/26/1.289 blok); foto tanpa kotak GT tidak berubah pada GT (PSNR 100,000).
- **Blok Skin terdeteksi = 2.087** lintas 6 foto — sama persis dengan total cross-tab Run C (ember 333, mira 106, solis 329, horizon 4, meadow 26, noctis 1.289).

### 1.3 Caveat

- **Luma-only.** Angka file adalah total luma; total file lossy q75 penuh (termasuk chroma + header) adalah 1.056.775 B. Bila perlakuan Skin juga diterapkan ke chroma (444), scale stream chroma menambah biaya — angka di sini adalah **batas bawah** biaya sebenarnya.
- **SSIM sebagai proxy perseptual.** VMAF tidak tersedia; SSIM per region adalah proxy yang sama dengan riset control-field v1 dan Sky. Tidak ada pemeriksaan visual dari sim ini (sim luma-only tidak menghasilkan gambar yang bisa di-decode) — build yang sebenarnya tetap butuh visual gate pada foto wajah sebelum merge.
- **Kotak GT kasar.** Kotak wajah/lengan mengandung rambut, mata, bibir, dan latar di tepinya — jadi gain GT adalah batas bawah dari gain pada kulit murni, dan recall gabungan hanya 34,4%.
- **Interpolasi equal-file** (Bagian 4) linear antar titik kualitas; SKIN_A/B diinterpolasi murni antar q70–q75 (bukan ekstrapolasi) — tidak ada masalah ekstrapolasi seperti pada Sky.

## 2. Hasil q75 per foto

Blok Skin per foto: ember 333 (39,6%), mira 106 (12,6%), solis 329 (8,4%), horizon 4 (0,03%), meadow 26 (0,49%), noctis 1.289 (1,41%). Total 2.087 blok.

### 2.1 ember-7f3a (225×225) — foto paling banyak kulit

| Varian | Total luma | Y-PSNR | SSIM global | SkinDet PSNR | SkinDet SSIM | GTskin PSNR | GTskin SSIM |
|---|---|---|---|---|---|---|---|
| BASE | 14.094 B | 40,4608 | 0,98997 | 40,577 | 0,99359 | 42,896 | 0,99690 |
| SKIN_A | 17.478 B (**+3.384, +24,0%**) | 41,8435 | 0,99160 | 45,716 (**+5,139**) | 0,99793 (**+0,00434**) | 45,252 (**+2,356**) | 0,99785 (**+0,00095**) |
| SKIN_B | 16.313 B (+2.219) | 41,1474 | 0,99046 | 42,667 (+2,090) | 0,99577 (+0,00218) | 44,072 (+1,176) | 0,99748 (+0,00058) |

### 2.2 mira-5b8d (217×233) — kulit pucat, recall terendah

| Varian | Total luma | Y-PSNR | SSIM global | SkinDet PSNR | SkinDet SSIM | GTskin PSNR | GTskin SSIM |
|---|---|---|---|---|---|---|---|
| BASE | 7.430 B | 46,9296 | 0,99320 | 44,678 | 0,99549 | 47,114 | 0,99747 |
| SKIN_A | 8.678 B (**+1.248, +16,8%**) | 47,4986 | 0,99342 | 48,341 (**+3,663**) | 0,99750 (+0,00201) | 48,223 (**+1,109**) | 0,99767 (+0,00020) |
| SKIN_B | 8.143 B (+713) | 46,7581 | 0,99320 | 43,854 (**−0,824**) | 0,99566 (+0,00017) | 46,714 (**−0,400**) | 0,99739 (−0,00008) |

SKIN_B **regresi** pada mira di q75: SkinDet PSNR −0,824 dB dan GT PSNR −0,400 dB vs BASE, Y-PSNR global −0,17 dB. Lihat Bagian 5.1 — ini artefak RDO, bukan noise, dan menjadi alasan SKIN_B ditolak.

### 2.3 solis-2c9e (663×372)

| Varian | Total luma | Y-PSNR | SSIM global | SkinDet PSNR | SkinDet SSIM | GTskin PSNR | GTskin SSIM |
|---|---|---|---|---|---|---|---|
| BASE | 26.200 B | 41,7974 | 0,98031 | 37,837 | 0,99212 | 40,920 | 0,98153 |
| SKIN_A | 30.576 B (**+4.376, +16,7%**) | 42,4072 | 0,98072 | 42,046 (**+4,209**) | 0,99647 (+0,00435) | 42,029 (**+1,109**) | 0,98265 (+0,00112) |
| SKIN_B | 28.356 B (+2.156) | 42,1271 | 0,98054 | 39,713 (+1,876) | 0,99443 (+0,00231) | 41,524 (+0,604) | 0,98210 (+0,00057) |

### 2.4 horizon / meadow / noctis — nyaris tanpa kulit manusia

| Foto | Varian | Total luma | SkinDet PSNR | SkinDet SSIM | Keterangan |
|---|---|---|---|---|---|
| horizon | BASE → SKIN_A | 84.932 → 84.980 B (+48) | 35,675 → 40,582 (+4,907) | 0,87627 → 0,95707 (+0,0808) | 4 blok FP (bidang hangat/bunga) |
| meadow | BASE → SKIN_A | 72.986 → 73.868 B (+882) | 37,472 → 42,384 (+4,912) | 0,99504 → 0,99827 (+0,00323) | 26 blok = bunga merah muda (FP) |
| noctis | BASE → SKIN_A | 483.398 → 500.492 B (**+17.094, +3,5%**) | 37,547 → 44,175 (**+6,628**) | 0,98556 → 0,99677 (+0,01121) | 1.289 blok — mayoritas FP danau/batu, Bagian 6 |

Noctis menyumbang **63% dari total Δ file** (+17.094 dari +27.032 B) padahal tidak ada satu pun wajah di dalamnya — seluruh gain SkinDet noctis adalah pada batu/danau/bunga yang salah diklasifikasi sebagai kulit.

### 2.5 Total 6 foto (q75, luma)

| Varian | mode | span | coeff | scale | TOTAL | Y-PSNR | SSIM global | SkinDet PSNR | SkinDet SSIM | GTskin PSNR | GTskin SSIM |
|---|---|---|---|---|---|---|---|---|---|---|---|
| BASE | 49.561 | 70.720 | 568.759 | 0 | **689.040** | 42,4141 | 0,98072 | 38,134 | 0,97301 | 42,265 | 0,99598 |
| SKIN_A | 49.547 | 70.873 | 589.367 | 6.285 | **716.072 (+27.032)** | 42,5733 | 0,98113 | 44,035 | 0,99067 | 43,578 | 0,99636 |
| SKIN_B | 49.553 | 70.831 | 576.469 | 6.285 | **703.138 (+14.098)** | 42,5110 | 0,98086 | 40,779 | 0,98352 | 42,876 | 0,99616 |

(SSIM di atas adalah photo-mean; agregat block-weighted Bagian 2.6. Region Other tidak regresi: SKIN_A Other PSNR 42,549 vs BASE 42,548, Other SSIM 0,97960 vs 0,97963.)

### 2.6 Agregat block-weighted (q75) — angka yang lebih jujur

| Varian | dSkinDet SSIM (weighted, 2.087 blok) | dGT SSIM (weighted, 918 blok) |
|---|---|---|
| SKIN_A | **+0,00860** | +0,00086 |
| SKIN_B | +0,00492 | +0,00041 |

Photo-mean melebih-lebihkan karena horizon (4 blok, +0,0808) dan noctis (1.289 blok FP, +0,01121) mendominasi rata-rata. Bahkan setelah pembobotan, dSkinDet SSIM SKIN_A **tetap di atas ambang terlihat ~0,005**; dGT SSIM tetap di bawahnya.

## 3. Dekomposisi biaya — koefisien mendominasi, signalling murah

| Varian | Δ total | Δ scale stream | Δ coeff | Δ span | Δ mode |
|---|---|---|---|---|---|
| SKIN_A | +27.032 | **+6.285 (23,2%)** | **+20.608 (76,2%)** | +153 | −14 |
| SKIN_B | +14.098 | **+6.285 (44,6%)** | +7.710 (54,7%) | +111 | −8 |

Scale stream = 2.087 blok Skin × 3 B + 2 B × 9 tile = 6.285 B — **biayanya tidak tergantung nilai scale**, persis seperti pada Sky. Per blok: SKIN_A 12,95 B/blok (scale 3,01 + coeff 9,87); Sky hanya 4,52 B/blok (scale 3,00 + coeff 1,46). Blok kulit bertekstur sehingga menghaluskan kuantnya menambah koefisien nyata; blok langit halus sehingga hampir tidak ada koefisien tambahan. Total Skin tetap kecil hanya karena bloknya sedikit.

Implikasi: varian yang lebih murah tidak bisa didapat dari scale yang lebih ringan saja (scale stream tetap 6.285 B — SKIN_B hanya menghemat koefisien). Yang bisa menekan biaya: **(a)** classifier yang lebih akurat — ~45% blok yang diperlakukan adalah FP (Bagian 6), dan noctis saja menyumbang 63% biaya; **(b)** hanya memperlakukan subset kulit (mis. blok wajah dengan confidence tinggi) — belum diukur.

## 4. Perbandingan equal-file (kurva RD q70/q75/q80)

Total luma + SSIM (rata-rata foto) per kualitas:

| q | BASE bytes | BASE SSIM | BASE SkinDet SSIM | BASE GT SSIM | SKIN_A bytes | SKIN_A SSIM | SKIN_A SkinDet SSIM | SKIN_A GT SSIM | SKIN_B bytes | SKIN_B SSIM | SKIN_B SkinDet SSIM | SKIN_B GT SSIM |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 70 | 614.492 | 0,97615 | 0,96315 | 0,99512 | 640.845 | 0,97657 | 0,98849 | 0,99556 | 628.137 | 0,97624 | 0,97660 | 0,99529 |
| 75 | 689.040 | 0,98072 | 0,97301 | 0,99598 | 716.072 | 0,98113 | 0,99067 | 0,99636 | 703.138 | 0,98086 | 0,98352 | 0,99616 |
| 80 | 830.360 | 0,98554 | 0,98249 | 0,99703 | 857.892 | 0,98583 | 0,99398 | 0,99729 | 844.834 | 0,98575 | 0,98891 | 0,99721 |

Interpolasi linear ke file BASE q75 (689.040 B) — interpolasi murni antar q70–q75:

- **SKIN_A** (f=0,641): SSIM global 0,97949 (**−0,00123**), SkinDet SSIM 0,98989 (**+0,01688**), GT SSIM 0,99607 (**+0,00009**), GT PSNR +0,962 dB, SkinDet PSNR +5,302 dB.
- **SKIN_B** (f=0,812): SSIM global 0,97999 (**−0,00073**), SkinDet SSIM 0,98222 (**+0,00921**), GT SSIM 0,99600 (**+0,00002**), GT PSNR +0,409 dB.

Equal-SSIM (biaya untuk menyamai SSIM global BASE q75):

- SKIN_A butuh **+20.268 B (+1,92% file)**.
- SKIN_B butuh **+11.825 B (+1,12% file)**.

Lagrangian equal-PSNR (per kualitas, TRUE bytes): SKIN_A −2,18% / −1,88% / −1,34% (q70/q75/q80); SKIN_B −1,12% / −0,92% / −0,60%. Arahnya konsisten di semua kualitas: ini trade kualitas-vs-file (bukan kemenangan kompresi) — yang dinilai adalah apakah kualitas yang dibeli sepadan dengan filenya, dan Bagian 0 menjawab: ya untuk SKIN_A, dengan caveat Bagian 6.

## 5. Dua temuan samping yang penting

### 5.1 SKIN_B regresi pada mira di q75 — artefak RDO, bukan noise

Pada mira, SKIN_B q75: SkinDet PSNR −0,824 dB, GT PSNR −0,400 dB, Y-PSNR global −0,17 dB vs BASE — sementara SSIM praktis datar (+0,00017 Det). Pada q70 SKIN_B mira justru gain (+0,755 dB Det PSNR) dan pada q80 gain besar (+4,249 dB). Jadi regresinya spesifik q75, bukan sifat varian: dengan scale 0,75, estimator bit RDO (yang dikalibrasi pada scale 1,0) salah menilai pilihan mode pada kulit pucat-halus mira (recall hanya 25%), dan RDO memilih mode yang secara biaya-sejati lebih buruk. SKIN_A tidak menunjukkan regresi di foto/kualitas mana pun. **SKIN_B: jangan dibangun** — gain-nya lebih kecil dan ia bisa regresi; scale 0,5 tidak punya masalah ini.

### 5.2 Noctis: 63% biaya untuk nol wajah

Dari +27.032 B SKIN_A, +17.094 B berasal dari noctis — foto tanpa wajah. Blok Skin noctis (1.289) sebagian besar adalah pantulan danau dan batu hangat yang salah diklasifikasi (Bagian 6). Ini bukan argumen menolak SKIN_A — biayanya sudah termasuk dalam +1,92% — tapi menunjukkan classifier yang lebih akurat akan memangkas biaya secara signifikan tanpa mengurangi gain pada wajah.

## 6. Keterbatasan yang diketahui (jujur)

1. **~45% blok yang diperlakukan bukan kulit manusia.** Dari 2.087 blok Skin: pantulan danau noctis ~616 blok (4% dari kotak danau 15.400 blok), batu coklat noctis ~240 blok (13% dari kotak batu 1.850 blok), bunga merah muda meadow 26 blok, dinding/krem 49 blok (ember 32, solis 15, mira 2 — dihitung langsung dari kotak confuser "not-Skin"). Total ≈ 935 blok. Piksel-piksel itu tetap dirender lebih baik (tidak ada kerusakan), tapi dari sudut pandang "perlindungan kulit" biayanya adalah pemborosan.
2. **Gain SSIM pada wajah/lengan sebenarnya di bawah ambang terlihat.** dGT SSIM block-weighted +0,00086 (same-quant) dan +0,00009 (equal-file) — keduanya < 0,005. Yang di atas ambang adalah gain pada region kulit terdeteksi (+0,0086 weighted) dan gain PSNR pada GT (+1,31 dB same-quant / +0,96 dB equal-file). Kotak GT kasar (mengandung rambut/mata/latar) ikut mengencerkan angka GT — tapi tetap: bila bar Setya adalah "perbaikan SSIM yang terlihat pada wajah", SKIN_A belum melewatinya.
3. **Biaya per foto tidak merata.** Foto potret membayar mahal: ember +24,0% luma, mira +16,8%, solis +16,7% (q75, SKIN_A) — karena sebagian besar bloknya adalah kulit. Pada koleksi foto orang, biaya totalnya akan mendekati angka ini, bukan +2,56% (angka +2,56% adalah rata-rata atas 6 foto benchmark yang separuhnya adalah lanskap).
4. **Luma-only; chroma belum dihitung.** Bila perlakuan Skin diterapkan juga ke chroma, biayanya naik (scale stream chroma + koefisien chroma). Angka di sini batas bawah.
5. **Tidak ada pemeriksaan visual.** Sim luma-only tidak menghasilkan gambar yang bisa di-decode — build yang sebenarnya tetap butuh visual gate pada foto wajah (wkview + JPEG kecil ke HP Setya) sebelum merge.

## 7. Rekomendasi

1. **BANGUN SKIN_A (scale 0,5 pada blok Skin terdeteksi).** Ini aplikasi control-field pertama yang lolos bar pengukuran: equal-SSIM cost +1,92% file (vs Sky +6,82%), equal-file global SSIM −0,00123 (jauh di bawah ambang terlihat) demi SkinDet SSIM +0,0086 block-weighted (di atas ambang terlihat), tanpa regresi di region mana pun (Other SSIM −0,00003). Mekanismenya jelas (scale stream murah karena blok sedikit; koefisien yang mendominasi) dan stabil di q70/q75/q80.
2. **JANGAN BANGUN SKIN_B.** Gain lebih kecil dan regresi PSNR pada mira@q75 (artefak RDO scale 0,75).
3. **Build-nya tetap butuh gates penuh seperti biasa:** wiring classifier yang sudah di-port ke `src/` (branch `work/control-field-v2-20261116`) ke pemilihan quant-scale di encoder + scale stream V7; TDD; Release + ASan/UBSan ctest; 12/12 old `.wk` decode; **visual gate pada foto wajah** (ember/mira/solis di wkview + JPEG kecil) sebelum merge — karena pengukuran ini luma-only dan belum pernah melihat gambarnya.
4. **Perbaikan classifier adalah penghemat biaya terbesar berikutnya:** memangkas FP danau/batu noctis akan memotong sebagian besar dari +17.094 B noctis tanpa menyentuh gain wajah. Itu pekerjaan classifier, bukan pekerjaan aplikasi — dan tidak menghalangi build SKIN_A sekarang.
5. **Arah aplikasi berikutnya setelah Skin:** TextUI sharpness (aplikasi v2 no. 3 — perlu foto berteks nyata karena benchmark tidak punya teks). Foliage tetap bukan prioritas.

## 8. Data dan reproduksi

- Diagnostik: `tools/skin_protection_diag.cpp` — link ke salinan `build-linux/libwk.a` (`/tmp/libwk_diag_skin.a`); `build-linux/` dan `build-asan/` tidak pernah di-rebuild; `src/`, `SPEC.md`, `tests/`, `web/` tidak disentuh.
- Build: `g++ -O2 -std=c++23 -I src -I include tools/skin_protection_diag.cpp /tmp/libwk_diag_skin.a -o /tmp/skin_protection_diag`
- Run: `/tmp/skin_protection_diag --quality {70,75,80} ~/workspace/libwk-exp-e/*.ppm` (6 foto benchmark; ramp_*.ppm tidak dipakai)
- Output mentah: `~/workspace/libwk-exp-n1/skin_protection_q70.txt`, `skin_protection_q75.txt`, `skin_protection_q80.txt`
- Total file lossy q75 penuh (konteks): 1.056.775 B; total luma BASE q75: 689.040 B (65,2% file).
