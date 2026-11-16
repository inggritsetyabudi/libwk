// Copyright 2026 Inggrit Setya Budi
// SPDX-License-Identifier: Apache-2.0
//
// Third-party implementation unit: stb_image (v2.32) and
// stb_image_write (v2.32), both public domain, vendored in src/stb/.
// Kept in its own file so their warnings stay out of the codec sources
// and the implementation is compiled exactly once. The headers are
// plain C and work on Linux, Windows, Termux and Emscripten alike,
// which is what lets wkenc/wkdec read JPG/PNG everywhere instead of
// only on Windows (WIC).

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
