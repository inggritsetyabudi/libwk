# src/arch/generic — fallback scalar target. Always compiled, on
# every target: the generic sources are the baseline every other
# target extends. Declares this target's compilable sources (read
# from the single source list, cmake/wk_sources.txt) and its headers.
# The cpu_features.h here reports no feature; a target with its own
# folder overrides it.
wk_read_sources(arch_generic WK_ARCH_TARGET_SOURCES)
set(WK_ARCH_TARGET_HEADERS
    "${CMAKE_CURRENT_LIST_DIR}/threading.h"
    "${CMAKE_CURRENT_LIST_DIR}/cpu_features.h")
