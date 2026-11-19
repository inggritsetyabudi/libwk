# src/arch/wasm — Emscripten target. Header-only today (the
# synchronous ThreadPool stub and the cpu_features stub); the
# [arch_wasm] section of the source list is empty on purpose.
# No per-target compile definitions yet.
wk_read_sources(arch_wasm WK_ARCH_TARGET_SOURCES)
set(WK_ARCH_TARGET_HEADERS
    "${CMAKE_CURRENT_LIST_DIR}/threading.h"
    "${CMAKE_CURRENT_LIST_DIR}/cpu_features.h")
