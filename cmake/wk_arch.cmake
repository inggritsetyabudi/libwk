# cmake/wk_arch.cmake — target detection and selection. The
# per-target sources come from the target's own fragment in
# src/arch/<target>/arch.cmake (included below); the generic
# fragment is always compiled, the target fragment adds its own.
# Nothing here lists per-target files.

if(EMSCRIPTEN)
    set(WK_ARCH_DIR wasm)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(WK_ARCH_DIR x86_64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(WK_ARCH_DIR aarch64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
    set(WK_ARCH_DIR arm)
else()
    set(WK_ARCH_DIR generic)
endif()
message(STATUS "wk arch: ${WK_ARCH_DIR} (processor: ${CMAKE_SYSTEM_PROCESSOR})")

# Generic first: it is the baseline every target extends.
include(${CMAKE_CURRENT_LIST_DIR}/../src/arch/generic/arch.cmake)
set(WK_ARCH_SOURCES ${WK_ARCH_TARGET_SOURCES})
set(WK_ARCH_HEADERS ${WK_ARCH_TARGET_HEADERS})
if(NOT WK_ARCH_DIR STREQUAL "generic")
    include(${CMAKE_CURRENT_LIST_DIR}/../src/arch/${WK_ARCH_DIR}/arch.cmake)
    list(APPEND WK_ARCH_SOURCES ${WK_ARCH_TARGET_SOURCES})
    list(APPEND WK_ARCH_HEADERS ${WK_ARCH_TARGET_HEADERS})
endif()
