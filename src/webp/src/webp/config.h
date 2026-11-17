/* Minimal config.h for the vendored libwebp 1.3.2 decode-only subset.
 *
 * Upstream generates src/webp/config.h from config.h.in (autotools) or
 * CMake, and several decode-path headers include it unconditionally
 * (utils.h, cpu.h, dsp.h). All feature macros are left undefined on
 * purpose: portable C fallbacks, no threads, no optional libraries.
 * HAVE_CONFIG_H is never defined, so cpu.h enables SSE2/NEON from the
 * compiler macros (__SSE2__, __aarch64__) exactly as upstream intends.
 * Targets are little-endian (x86_64, aarch64, arm, wasm32); WORDS_BIGENDIAN
 * is left undefined, matching the rest of the codec.
 */
#ifndef WEBP_CONFIG_H_
#define WEBP_CONFIG_H_
#endif  // WEBP_CONFIG_H_
