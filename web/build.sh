#!/bin/bash
# Copyright 2026 Inggrit Setya Budi
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

EXPORTED_FUNCTIONS="['_wk_wasm_alloc','_wk_wasm_free','_wk_wasm_decode','_wk_wasm_encode','_wk_wasm_version','_wk_wasm_get_info','_malloc','_free']"
EXPORTED_RUNTIME="['wasmMemory']"

# Source list: ../cmake/wk_sources.txt is the single source of truth
# for every compilable source file. Nothing below lists a source by
# hand; wk_sources <section> prints that section's paths, one per
# line, relative to the repository root. The web build reads the
# [core], [stb], [webp], [avif], [dav1d] and [dav1d_tmpl] sections.
# It does NOT read [libyuv]: the AVIF decoder is unreachable from
# the wasm exports, so it is dropped by DCE either way, and the
# libyuv objects are not needed to link. It does NOT read the
# [arch_*] sections: src/arch/generic/threading.cpp is deliberately
# excluded — under __EMSCRIPTEN__ src/core/threading.h provides the
# inline single-threaded ThreadPool from src/arch/wasm, and the real
# pool in generic/threading.cpp would conflict with it.
WK_SOURCES_FILE="../cmake/wk_sources.txt"
wk_sources() {
  awk -v section="[$1]" '
    /^\[/ { current = $0 }
    current == section && NF && $1 !~ /^#/ && $1 !~ /^\[/ { print $1 }
  ' "${WK_SOURCES_FILE:-../cmake/wk_sources.txt}"
}

# Vendored libwebp 1.3.2 decode-only C sources (see src/webp/README.md):
# compiled with emcc (C), not em++ — em++ would compile them as C++ and
# libwebp is C. The objects are linked by em++ below, then removed.
WEBP_C_SOURCES="$(wk_sources webp | sed 's|^|../|')"
WEBP_OBJ_DIR="$(mktemp -d)"
trap 'rm -rf "$WEBP_OBJ_DIR"' EXIT
WEBP_OBJS=""
for webp_src in $WEBP_C_SOURCES; do
  webp_obj="$WEBP_OBJ_DIR/$(basename "$(dirname "$webp_src")")_$(basename "$webp_src" .c).o"
  emcc -I../src/webp -O3 -c "$webp_src" -o "$webp_obj" || exit 1
  WEBP_OBJS="$WEBP_OBJS $webp_obj"
done

# Vendored libavif 1.4.2 decode-only C sources (see src/avif/README.md):
# compiled with emcc (C), not em++ — em++ would compile them as C++
# and libavif is C. AVIF_CODEC_DAV1D registers the dav1d decoder.
AVIF_C_SOURCES="$(wk_sources avif | sed 's|^|../|')"
AVIF_OBJ_DIR="$(mktemp -d)"
trap 'rm -rf "$AVIF_OBJ_DIR"' EXIT
AVIF_OBJS=""
for avif_src in $AVIF_C_SOURCES; do
  avif_obj="$AVIF_OBJ_DIR/$(basename "$(dirname "$avif_src")")_$(basename "$avif_src" .c).o"
  emcc -I../src/avif/libavif/include -I../src/avif/libavif/third_party/libyuv/include -I../src/avif/dav1d/include -DAVIF_CODEC_DAV1D -std=c11 -O3 -c "$avif_src" -o "$avif_obj" || exit 1
  AVIF_OBJS="$AVIF_OBJS $avif_obj"
done

# Vendored dav1d 1.5.5 decode-only C sources (see src/avif/README.md):
# C-only build (no asm; nasm is unavailable), same defines as the
# meson build. The vendored config.h carries a __EMSCRIPTEN__
# override that turns the x86 arch flags off for wasm.
# The 13 *_tmpl.c files are compiled TWICE, once per bitdepth
# (-DBITDEPTH=8 and -DBITDEPTH=16) — meson does the same
# (libdav1d_bitdepth_8/16).
DAV1D_C_SOURCES="$(wk_sources dav1d | sed 's|^|../|')"
DAV1D_OBJ_DIR="$(mktemp -d)"
trap 'rm -rf "$DAV1D_OBJ_DIR"' EXIT
DAV1D_OBJS=""
for dav1d_src in $DAV1D_C_SOURCES; do
  dav1d_obj="$DAV1D_OBJ_DIR/$(basename "$(dirname "$dav1d_src")")_$(basename "$dav1d_src" .c).o"
  emcc -I../src/avif/dav1d -I../src/avif/dav1d/include -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -ffast-math -O3 -c "$dav1d_src" -o "$dav1d_obj" || exit 1
  DAV1D_OBJS="$DAV1D_OBJS $dav1d_obj"
done
DAV1D_TMPL_SOURCES="$(wk_sources dav1d_tmpl | sed 's|^|../|')"
for dav1d_bitdepth in 8 16; do
  for dav1d_src in $DAV1D_TMPL_SOURCES; do
    dav1d_obj="$DAV1D_OBJ_DIR/$(basename "$(dirname "$dav1d_src")")_$(basename "$dav1d_src" .c)_$dav1d_bitdepth.o"
    emcc -I../src/avif/dav1d -I../src/avif/dav1d/include -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -DBITDEPTH=$dav1d_bitdepth -ffast-math -O3 -c "$dav1d_src" -o "$dav1d_obj" || exit 1
    DAV1D_OBJS="$DAV1D_OBJS $dav1d_obj"
  done
done

# em++ (not emcc): the engine is C++ and the link needs the C++
# standard library (operator new/delete, std::string).
CORE_SOURCES="$(wk_sources core | sed 's|^|../|')"
STB_SOURCES="$(wk_sources stb | sed 's|^|../|')"
em++ \
  -I../include \
  -I../src \
  -I../src/webp \
  -I../src/avif/libavif/include \
  -I../src/avif/dav1d \
  -I../src/avif/dav1d/include \
  $CORE_SOURCES \
  $STB_SOURCES \
  threading_stub.cpp \
  $WEBP_OBJS \
  $AVIF_OBJS \
  $DAV1D_OBJS \
  wk_wasm.cpp \
  -o wk.js \
  -s WASM=1 \
  -s EXPORTED_FUNCTIONS="$EXPORTED_FUNCTIONS" \
  -s EXPORTED_RUNTIME_METHODS="$EXPORTED_RUNTIME" \
  -s ALLOW_MEMORY_GROWTH=1 \
  -O3 \
  -std=c++2b \
  -s MODULARIZE=1 \
  -s EXPORT_NAME="WkModule"
