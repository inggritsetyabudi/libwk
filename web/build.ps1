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

$exported_functions = @(
    '_wk_wasm_alloc',
    '_wk_wasm_free',
    '_wk_wasm_decode',
    '_wk_wasm_encode',
    '_wk_wasm_version',
    '_wk_wasm_get_info',
    '_malloc',
    '_free'
) -join ','

# Source list: ../cmake/wk_sources.txt is the single source of truth
# for every compilable source file. Nothing below lists a source by
# hand; Get-WkSources <section> returns that section's paths, each
# prefixed with '../'. The web build reads the [core], [stb],
# [webp], [avif], [dav1d] and [dav1d_tmpl] sections. It does NOT
# read [libyuv]: the AVIF decoder is unreachable from the wasm
# exports, so it is dropped by DCE either way, and the libyuv
# objects are not needed to link. It does NOT read the [arch_*]
# sections: src/arch/generic/threading.cpp is deliberately excluded
# — under __EMSCRIPTEN__ src/core/threading.h provides the inline
# single-threaded ThreadPool from src/arch/wasm, and the real pool
# in generic/threading.cpp would conflict with it.
function Get-WkSources($Section) {
  $inSection = $false
  foreach ($line in Get-Content ../cmake/wk_sources.txt) {
    $trimmed = $line.Trim()
    if ($trimmed -match '^\[(.+)\]$') { $inSection = ($Matches[1] -eq $Section); continue }
    if ($inSection -and $trimmed -and -not $trimmed.StartsWith('#')) { '../' + $trimmed }
  }
}

# Vendored libwebp 1.3.2 decode-only C sources (see src/webp/README.md):
# compiled with emcc (C), not em++ — em++ would compile them as C++ and
# libwebp is C. The objects are linked by em++ below, then removed.
$webpSources = @(Get-WkSources 'webp')
$webpObjDir = New-Item -ItemType Directory -Path (Join-Path ([System.IO.Path]::GetTempPath()) ("wk-webp-" + [guid]::NewGuid().ToString('N'))) -Force
$webpObjs = @()
foreach ($src in $webpSources) {
  $obj = Join-Path $webpObjDir.FullName ((Split-Path (Split-Path $src -Parent) -Leaf) + '_' + (Split-Path $src -LeafBase) + '.o')
  emcc -I../src/webp -O3 -c $src -o $obj
  if ($LASTEXITCODE -ne 0) { throw "emcc failed for $src" }
  $webpObjs += $obj
}

# Vendored libavif 1.4.2 decode-only C sources (see src/avif/README.md):
# compiled with emcc (C), not em++ — em++ would compile them as C++
# and libavif is C. AVIF_CODEC_DAV1D registers the dav1d decoder.
$avifSources = @(Get-WkSources 'avif')
$avifObjDir = New-Item -ItemType Directory -Path (Join-Path ([System.IO.Path]::GetTempPath()) ("wk-avif-" + [guid]::NewGuid().ToString('N'))) -Force
$avifObjs = @()
foreach ($src in $avifSources) {
  $obj = Join-Path $avifObjDir.FullName ((Split-Path (Split-Path $src -Parent) -Leaf) + '_' + (Split-Path $src -LeafBase) + '.o')
  emcc -I../src/avif/libavif/include -I../src/avif/libavif/third_party/libyuv/include -I../src/avif/dav1d/include -DAVIF_CODEC_DAV1D -std=c11 -O3 -c $src -o $obj
  if ($LASTEXITCODE -ne 0) { throw "emcc failed for $src" }
  $avifObjs += $obj
}

# Vendored dav1d 1.5.5 decode-only C sources (see src/avif/README.md):
# C-only build (no asm; nasm is unavailable), same defines as the
# meson build. The vendored config.h carries a __EMSCRIPTEN__
# override that turns the x86 arch flags off for wasm.
# The 13 *_tmpl.c files are compiled TWICE, once per bitdepth
# (-DBITDEPTH=8 and -DBITDEPTH=16) — meson does the same
# (libdav1d_bitdepth_8/16).
$dav1dSources = @(Get-WkSources 'dav1d')
$dav1dObjDir = New-Item -ItemType Directory -Path (Join-Path ([System.IO.Path]::GetTempPath()) ("wk-dav1d-" + [guid]::NewGuid().ToString('N'))) -Force
$dav1dObjs = @()
foreach ($src in $dav1dSources) {
  $obj = Join-Path $dav1dObjDir.FullName ((Split-Path (Split-Path $src -Parent) -Leaf) + '_' + (Split-Path $src -LeafBase) + '.o')
  emcc -I../src/avif/dav1d -I../src/avif/dav1d/include -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -ffast-math -O3 -c $src -o $obj
  if ($LASTEXITCODE -ne 0) { throw "emcc failed for $src" }
  $dav1dObjs += $obj
}
$dav1dTmplSources = @(Get-WkSources 'dav1d_tmpl')
foreach ($bitdepth in 8, 16) {
  foreach ($src in $dav1dTmplSources) {
    $obj = Join-Path $dav1dObjDir.FullName ((Split-Path (Split-Path $src -Parent) -Leaf) + '_' + (Split-Path $src -LeafBase) + '_' + $bitdepth + '.o')
    emcc -I../src/avif/dav1d -I../src/avif/dav1d/include -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -DBITDEPTH=$bitdepth -ffast-math -O3 -c $src -o $obj
    if ($LASTEXITCODE -ne 0) { throw "emcc failed for $src" }
    $dav1dObjs += $obj
  }
}

$coreSources = @(Get-WkSources 'core')
$stbSources = @(Get-WkSources 'stb')

em++ `
  -I../include `
  -I../src `
  -I../src/webp `
  -I../src/avif/libavif/include `
  -I../src/avif/dav1d `
  -I../src/avif/dav1d/include `
  $coreSources `
  $stbSources `
  threading_stub.cpp `
  $webpObjs `
  $avifObjs `
  $dav1dObjs `
  wk_wasm.cpp `
  -o wk.js `
  -s WASM=1 `
  -s "EXPORTED_FUNCTIONS=[$exported_functions]" `
  -s "EXPORTED_RUNTIME_METHODS=[`"wasmMemory`"]" `
  -s ALLOW_MEMORY_GROWTH=1 `
  -O3 `
  -std=c++2b `
  -s MODULARIZE=1 `
  -s EXPORT_NAME="WkModule"

Remove-Item -Recurse -Force $webpObjDir
Remove-Item -Recurse -Force $avifObjDir
Remove-Item -Recurse -Force $dav1dObjDir
