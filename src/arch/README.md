# src/arch — per-target code

The codec core in `src/core/` is portable C++23 and must build on
every target unchanged. Anything that differs per target lives here,
one folder per target, behind the dispatch in `src/arch/arch.h`.

## Map

| Path | What lives here |
|---|---|
| `arch.h` | Compile-time target detection (`WK_ARCH_*`, `wk::arch_name()`). The single dispatch point; core code reads these macros, never raw compiler macros. |
| `cpu_features.h` | Runtime CPU feature detection, one interface for every target (`wk::arch::CpuFeatures`, `wk::arch::cpu_features()`). Declares the result type and includes the per-target header for the detected target. Detection only — no SIMD kernel reads these flags yet (SIMD is Fase 12). |
| `generic/` | Fallback scalar implementation. Every target must build through this; a new target never breaks the build. Today: the `std::thread` `ThreadPool` (`threading.h` + `threading.cpp`) and the all-false `cpu_features.h`. |
| `wasm/` | Emscripten. Today: the synchronous `ThreadPool` stub (`threading.h`, header-only — wasm has no threads) and the all-false `cpu_features.h`. |
| `x86_64/` | Runtime detection via CPUID (`cpu_features.h`: SSE2/SSE4.1/AVX2/AVX512F) plus `arch.cmake`, the target's build fragment. No `.cpp` files yet — the header is the content. |
| `aarch64/` | Runtime detection via `__builtin_cpu_supports` (`cpu_features.h`: NEON/SVE/SVE2) plus `arch.cmake`. No `.cpp` files yet — the header is the content. |
| `arm/` | 32-bit ARM (armv7 and friends). Runtime detection via `__builtin_cpu_supports` on GCC, compile-time `__ARM_NEON` on Clang (`cpu_features.h`: NEON), plus `arch.cmake`. No `.cpp` files yet — the header is the content. |

Every target folder holds its own `arch.cmake` fragment, which
declares that target's compilable sources (read from the single
source list, `cmake/wk_sources.txt`) and headers. The root build
never lists per-target sources itself.

`src/stb/` (vendored stb_image/stb_image_write) is third-party and
stays outside this split; do not restructure it.

## Names: arm64 == aarch64

`arm64` (Apple/Windows name) and `aarch64` (Linux name) are the same
ISA. The canonical folder is **`aarch64`**; `arm64` is only an alias
and never gets its own folder. `arm` means ARM 32-bit (armv7 and
friends). CMake maps `aarch64|arm64|ARM64` → `aarch64`.

## How to add a new arch

1. Create `src/arch/<target>/` with the replacement code. Keep the
   interface identical to the generic one (same class/function names),
   so `src/core/` never branches on the target.
2. Extend the detection chain in `arch.h` (new `WK_ARCH_<TARGET>` +
   `WK_ARCH_NAME`) and the `WK_ARCH_DIR` mapping in
   `cmake/wk_arch.cmake`. Add the target's `arch.cmake` fragment in
   its folder and a section for its sources in
   `cmake/wk_sources.txt` (empty if the target is header-only, like
   x86_64/aarch64/arm today — the fragment still exists, so the
   target is a real build participant, not a name).
3. If the code replaces a generic file, make the generic file harmless
   on the new target (e.g. a `#if !defined(...)` guard, as in
   `generic/threading.cpp`) and update the source list if the target
   is wasm-adjacent (the web build reads the same list, so there is
   only one place to update).
4. Extend `tests/arch_test.cpp` with the new target name and its
   `cpu_features` expectations, so a drift between `arch.h` and the
   test fails loudly.
5. Keep the generic fallback working: `WK_ARCH_GENERIC` must always
   be a buildable path.

## What is NOT here yet

- **No SIMD kernels.** SIMD is Fase 12 ("SIMD last") and explicitly
  not this split. When it lands, kernels go in their arch folder
  (e.g. `x86_64/dct_sse.cpp`) behind the same dispatch pattern, and
  they will read `wk::arch::cpu_features()` to decide at runtime
  which kernel to run.
- **Detection today is runtime on x86_64/aarch64/arm, all-false on
  generic/wasm.** The one exception is 32-bit ARM under Clang, which
  has no `__builtin_cpu_supports` and falls back to the
  compile-time `__ARM_NEON` macro. The GCC spellings of the ARM
  builtin strings ("neon") are per the GCC manual; they are not
  verified on this machine (no ARM cross-compiler here) — the Clang
  spellings ("simd") are verified by syntax check.
