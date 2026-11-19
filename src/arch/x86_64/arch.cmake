# src/arch/x86_64 — x86-64 target. Declares this target's
# compilable sources (read from the single source list,
# cmake/wk_sources.txt) and its headers. Today the target has no
# .cpp files: cpu_features.h is header-only (runtime detection, no
# SIMD kernels yet — SIMD is Fase 12). When kernels land, their
# sources and compile definitions go here, not in the root
# CMakeLists.
wk_read_sources(arch_x86_64 WK_ARCH_TARGET_SOURCES)
set(WK_ARCH_TARGET_HEADERS "${CMAKE_CURRENT_LIST_DIR}/cpu_features.h")
