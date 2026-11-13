#!/usr/bin/env python3
"""RD-curve benchmark harness for libwk (Phase 1, Slice 1).

Encodes a fixed image set with .wk (and JPEG/WebP references via Pillow),
measures PSNR/SSIM/size with wkmetric, and writes a machine-readable JSON
plus a Markdown summary table. The baseline it produces is the yardstick
every Phase 1 syntax change is measured against: a change must shrink
.wk bytes at equal PSNR/SSIM, with no visual regression.

Usage:
    python3 benchmark/rd_bench.py [--bin-dir build-linux]
        [--qualities 10,25,50,75,85,90,95,100] [--codecs wk,jpeg,webp]
        [--out benchmark/results/rd_baseline] [--extra-images DIR]
        [--keep-tmp]

Image set (core, reproducible): photos/people/*.jpg and
photos/scenery/*.jpg (6 images). --extra-images adds more (jpg/png/webp)
from a directory; they are converted to RGB PPM like the core set.

Outputs (under --out):
    rd_<name>.json  full per-image records (codec, quality, psnr, ssim,
                    bytes, bpp, encode/decode seconds, wkmetric fields)
    rd_<name>.md    summary table: avg PSNR/SSIM/bpp per quality per codec

Requires: wkenc, wkdec, wkmetric in --bin-dir; Pillow + numpy.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np
from PIL import Image

DEFAULT_QUALITIES = [10, 25, 50, 75, 85, 90, 95, 100]
DEFAULT_CODECS = ["wk", "jpeg", "webp"]


def find_sources(repo_root, extra_dir=None):
    """Core set: the repo's own benchmark photos. Extra dir adds more."""
    sources = []
    for sub in ("people", "scenery"):
        d = repo_root / "photos" / sub
        if d.is_dir():
            sources += sorted(d.glob("*.jpg"))
    if extra_dir:
        d = Path(extra_dir)
        for ext in ("*.jpg", "*.jpeg", "*.png", "*.webp"):
            sources += sorted(d.glob(ext))
    # De-duplicate by name, keep order.
    seen, out = set(), []
    for p in sources:
        if p.name not in seen:
            seen.add(p.name)
            out.append(p)
    return out


def to_ppm(src_path, tmpdir):
    """Convert any image to RGB PPM (the format wkenc loads on Linux)."""
    im = Image.open(src_path).convert("RGB")
    ppm = Path(tmpdir) / (src_path.stem + ".ppm")
    im.save(ppm)
    return ppm, im.size


def run(cmd, timeout=600):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} failed: {r.stderr.strip()[:300]}")
    return r


def wkmetric_json(wkmetric, ref_ppm, cand_ppm):
    r = run([str(wkmetric), "--json", str(ref_ppm), str(cand_ppm)])
    return json.loads(r.stdout)


def encode_wk(wkenc, wkdec, wkmetric, src_ppm, q, tmpdir, stem):
    wk = Path(tmpdir) / f"{stem}_q{q}.wk"
    dec = Path(tmpdir) / f"{stem}_q{q}_dec.ppm"
    t0 = time.monotonic()
    run([str(wkenc), "--quality", str(q), str(src_ppm), str(wk)])
    t_enc = time.monotonic() - t0
    t0 = time.monotonic()
    run([str(wkdec), str(wk), str(dec)])
    t_dec = time.monotonic() - t0
    m = wkmetric_json(wkmetric, src_ppm, dec)
    nbytes = wk.stat().st_size
    # Free the intermediates immediately: a 3000x1951 PPM is ~17 MB and
    # /tmp is a 512 MB tmpfs, so keeping every decode would fill the disk.
    for p in (wk, dec):
        p.unlink(missing_ok=True)
    return {
        "bytes": nbytes,
        "psnr": m["psnr"],
        "ssim": m["ssim"],
        "mae": m["mae"],
        "y_psnr": m["ycbcr"]["Y"]["psnr"],
        "chroma_psnr": m["artifacts"]["chroma_psnr"],
        "max_abs_error": m["artifacts"]["max_abs_error"],
        "encode_s": round(t_enc, 3),
        "decode_s": round(t_dec, 3),
    }


def encode_pil(codec, src_ppm, q, tmpdir, stem, wkmetric):
    """JPEG/WebP reference via Pillow, measured the same way as .wk."""
    im = Image.open(src_ppm)
    out = Path(tmpdir) / f"{stem}_q{q}.{codec}"
    kw = {"quality": q}
    if codec == "webp":
        kw["method"] = 6  # slowest/best compression
    t0 = time.monotonic()
    im.save(out, codec.upper(), **kw)
    t_enc = time.monotonic() - t0
    # Decode back to PPM and measure against the source PPM.
    dec_ppm = Path(tmpdir) / f"{stem}_q{q}_{codec}_dec.ppm"
    t0 = time.monotonic()
    Image.open(out).convert("RGB").save(dec_ppm)
    t_dec = time.monotonic() - t0
    m = wkmetric_json(wkmetric, src_ppm, dec_ppm)
    nbytes = out.stat().st_size
    # Free intermediates immediately (see encode_wk: /tmp is a 512 MB tmpfs).
    for p in (out, dec_ppm):
        p.unlink(missing_ok=True)
    return {
        "bytes": nbytes,
        "psnr": m["psnr"],
        "ssim": m["ssim"],
        "mae": m["mae"],
        "y_psnr": m["ycbcr"]["Y"]["psnr"],
        "chroma_psnr": m["artifacts"]["chroma_psnr"],
        "max_abs_error": m["artifacts"]["max_abs_error"],
        "encode_s": round(t_enc, 3),
        "decode_s": round(t_dec, 3),
    }


def bpp(nbytes, w, h):
    return round(nbytes * 8 / (w * h), 4)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bin-dir", default="build-linux",
                    help="directory with wkenc/wkdec/wkmetric")
    ap.add_argument("--qualities", default=",".join(map(str, DEFAULT_QUALITIES)))
    ap.add_argument("--codecs", default=",".join(DEFAULT_CODECS))
    ap.add_argument("--out", default="benchmark/results/rd_baseline",
                    help="output stem (no extension); writes .json and .md")
    ap.add_argument("--extra-images", default=None,
                    help="extra image directory (jpg/png/webp)")
    ap.add_argument("--keep-tmp", action="store_true")
    args = ap.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    bin_dir = Path(args.bin_dir)
    if not bin_dir.is_absolute():
        bin_dir = repo_root / bin_dir
    wkenc, wkdec, wkmetric = (bin_dir / n for n in ("wkenc", "wkdec", "wkmetric"))
    for b in (wkenc, wkdec, wkmetric):
        if not b.is_file():
            sys.exit(f"missing binary: {b} (build first or pass --bin-dir)")

    qualities = [int(x) for x in args.qualities.split(",") if x.strip()]
    codecs = [c.strip().lower() for c in args.codecs.split(",") if c.strip()]
    sources = find_sources(repo_root, args.extra_images)
    if not sources:
        sys.exit("no source images found")

    tmpdir = tempfile.mkdtemp(prefix="rd_bench_")
    records = []
    try:
        for src in sources:
            src_ppm, (w, h) = to_ppm(src, tmpdir)
            npix = w * h
            print(f"== {src.name} {w}x{h} ==", flush=True)
            for codec in codecs:
                for q in qualities:
                    stem = f"{src.stem}_{codec}"
                    try:
                        if codec == "wk":
                            r = encode_wk(wkenc, wkdec, wkmetric, src_ppm, q,
                                          tmpdir, stem)
                        else:
                            r = encode_pil(codec, src_ppm, q, tmpdir, stem,
                                           wkmetric)
                    except Exception as e:
                        print(f"   {codec} q{q}: FAIL {e}", flush=True)
                        continue
                    r.update({"image": src.name, "codec": codec, "quality": q,
                              "width": w, "height": h,
                              "bpp": bpp(r["bytes"], w, h)})
                    records.append(r)
                    print(f"   {codec} q{q:3d}: PSNR {r['psnr']:6.2f} "
                          f"SSIM {r['ssim']:.4f} {r['bytes']:7d} B "
                          f"{r['bpp']:.3f} bpp", flush=True)
            src_ppm.unlink(missing_ok=True)  # free the source PPM per image
    finally:
        if not args.keep_tmp:
            import shutil
            shutil.rmtree(tmpdir, ignore_errors=True)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    # Aggregate: avg PSNR/SSIM/bpp and total bytes per codec+quality.
    summary = {}
    for codec in codecs:
        for q in qualities:
            rows = [r for r in records if r["codec"] == codec and r["quality"] == q]
            if not rows:
                continue
            summary.setdefault(codec, {})[q] = {
                "n": len(rows),
                "avg_psnr": round(float(np.mean([r["psnr"] for r in rows])), 2),
                "avg_ssim": round(float(np.mean([r["ssim"] for r in rows])), 4),
                "avg_bpp": round(float(np.mean([r["bpp"] for r in rows])), 4),
                "total_bytes": int(sum(r["bytes"] for r in rows)),
                "avg_encode_s": round(float(np.mean([r["encode_s"] for r in rows])), 3),
            }

    payload = {
        "tool": "benchmark/rd_bench.py",
        "images": [s.name for s in sources],
        "qualities": qualities,
        "codecs": codecs,
        "summary": summary,
        "records": records,
    }
    with open(str(out) + ".json", "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=1)
        f.write("\n")

    # Markdown summary table.
    lines = ["# RD benchmark summary", "",
             "Average over %d image(s): %s" % (len(sources), ", ".join(s.name for s in sources)),
             "",
             "| codec | q | avg PSNR (dB) | avg SSIM | avg bpp | total bytes |",
             "|---|---|---|---|---|---|"]
    for codec in codecs:
        for q in qualities:
            s = summary.get(codec, {}).get(q)
            if not s:
                continue
            lines.append(f"| {codec} | {q} | {s['avg_psnr']:.2f} | "
                         f"{s['avg_ssim']:.4f} | {s['avg_bpp']:.4f} | "
                         f"{s['total_bytes']} |")
    lines += ["", f"Full records: `{out.name}.json`"]
    with open(str(out) + ".md", "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    print(f"\nwrote {out}.json and {out}.md ({len(records)} records)")


if __name__ == "__main__":
    main()
