# src/arch — per-target code

The codec core in `src/core/` is portable C++23 and must build on
every target unchanged. Anything that differs per target lives here,
one folder per target, behind the dispatch in `src/arch/arch.h`.

## Map

| Path | What lives here |
|---|---|
| `arch.h` | Compile-time target detection (`WK_ARCH_*`, `wk::arch_name()`). The single dispatch point; core code reads these macros, never raw compiler macros. |
| `generic/` | Fallback scalar implementation. Every target must build through this; a new target never breaks the build. Today: the `std::thread` `ThreadPool` (`threading.h` + `threading.cpp`). |
| `wasm/` | Emscripten. Today: the synchronous `ThreadPool` stub (`threading.h`, header-only — wasm has no threads). |
| `x86_64/`, `aarch64/`, `arm/` | **Not created yet — intentionally.** No per-target code exists for them today, and an empty folder would be a decoration. Create one when it gains real content (see below). |

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
   `WK_ARCH_NAME`) and the `WK_ARCH_DIR` mapping in the root
   `CMakeLists.txt`. If the target has no `.cpp` files (header-only,
   like wasm today), nothing else needs a build change.
3. If the code replaces a generic file, make the generic file harmless
   on the new target (e.g. a `#if !defined(...)` guard, as in
   `generic/threading.cpp`) and update `web/build.sh`'s explicit
   source list if the target is wasm-adjacent.
4. Extend `tests/arch_test.cpp` with the new target name so a drift
   between `arch.h` and the test fails loudly.
5. Keep the generic fallback working: `WK_ARCH_GENERIC` must always
   be a buildable path.

## What is NOT here yet

- **No SIMD kernels.** SIMD is Fase 12 ("SIMD last") and explicitly
  not this split. When it lands, kernels go in their arch folder
  (e.g. `x86_64/dct_sse.cpp`) behind the same dispatch pattern.
- **Dispatch today is only generic + wasm threading.** `arch.h`
  already detects x86_64/aarch64/arm, but no code branches on those
  macros yet — detection without use is fine; a folder without
  content is not.
