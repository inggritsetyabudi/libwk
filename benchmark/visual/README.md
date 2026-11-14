# Visual test results — libwk lossy decode at q75

Decoded outputs of the six benchmark photos, encoded with the current
encoder (layout V8, version 0.2.3) at quality 75 and decoded back to
images. These are the visual check for the Phase 1 syntax work: the
decode must be visually clean (no blocking, banding, or colour shift
beyond what q75 normally shows).

## How the files are stored

Snapshot pushes cannot carry binary files, so each decoded image is
stored as a base64 text file (`.b64`). To view one:

```sh
base64 -d <name>.b64 > <name>.jpg   # then open the JPG
```

The JPEGs are quality 90, capped at 1600 px on the long edge, so they
are compact visual copies (not pixel-exact references). For exact
pixel comparison use `wkmetric` on a fresh encode instead; the stored
benchmark `.wk`/`.png` files under `benchmark/{people,scenery}/` are
stale (Nov 13, V6 encoder) and must not be compared against.

## Results (q75, V8 encoder)

| file | source photo | PSNR (dB) | .wk size (B) |
|---|---|---|---|
| `ct7sea2i8f.b64` | `ember-7f3a` | 39.90 | 23137 |
| `43xtojif7u.b64` | `mira-5b8d` | 44.98 | 10457 |
| `547or10x98.b64` | `solis-2c9e` | 41.77 | 36832 |
| `mgxp6fches.b64` | `horizon-4e2a` | 36.92 | 284587 |
| `8m7f3kf03i.b64` | `meadow-6d3b` | 40.45 | 157855 |
| `cpsmv4lm12.b64` | `noctis-8c1f` | 42.93 | 729549 |

Verdict: no artefacts beyond normal q75 behaviour. The low-frequency
smooth areas stay clean; textured areas keep detail. PSNR matches the
RD baseline in `benchmark/results/rd_baseline.md` (mira 44.98 dB,
ember 39.90 dB, solis 41.77 dB, horizon 36.92 dB, meadow 40.45 dB,
noctis 42.93 dB) — the V8 re-coding is pixel-identical to V7, so the
visuals are unchanged by the syntax work; only the file sizes shrank.

Generated 2026-11-14 from the Phase 1 branch
(`work/phase1-syntax-efficiency-20261113`, v0.2.3).
