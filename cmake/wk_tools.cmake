# cmake/wk_tools.cmake — the command-line tools. All of them link
# the wk library defined by the root CMakeLists; the viewer pulls in
# MiniFB when WK_BUILD_VIEWER is on.

add_executable(wkenc tools/wkenc.cpp)
target_link_libraries(wkenc PRIVATE wk)
wk_copy_windows_runtime(wkenc)

add_executable(wkdec tools/wkdec.cpp)
target_link_libraries(wkdec PRIVATE wk)
wk_copy_windows_runtime(wkdec)

add_executable(wkmetric tools/wkmetric.cpp)
target_link_libraries(wkmetric PRIVATE wk)
wk_copy_windows_runtime(wkmetric)

add_executable(wkmeta-dump tools/wkmeta_dump.cpp)
target_link_libraries(wkmeta-dump PRIVATE wk)
wk_copy_windows_runtime(wkmeta-dump)

add_executable(wkmeta-edit tools/wkmeta_edit.cpp)
target_link_libraries(wkmeta-edit PRIVATE wk)
wk_copy_windows_runtime(wkmeta-edit)

option(WK_BUILD_VIEWER "Build graphical viewer" ON)
if(WK_BUILD_VIEWER)
    include(FetchContent)
    FetchContent_Declare(
        minifb
        GIT_REPOSITORY https://github.com/emoon/minifb.git
        # v0.10.1, pinned to its commit so the build does not move
        # when the tag does.
        GIT_TAG 51f8004058a99fe024328b7f0848a6e353bfac86
    )
    if(WIN32)
        set(MINIFB_USE_OPENGL_API OFF CACHE BOOL "" FORCE)
    endif()
    FetchContent_MakeAvailable(minifb)
endif()

# wkview is always built: with WK_BUILD_VIEWER it opens a window when a
# display exists (and falls back to the terminal when it cannot); without
# it the binary is terminal-only (--cli), which is what Termux needs.
add_executable(wkview tools/wkview.cpp)
target_link_libraries(wkview PRIVATE wk)
if(WK_BUILD_VIEWER)
    target_compile_definitions(wkview PRIVATE WK_VIEWER_GUI)
    target_link_libraries(wkview PRIVATE minifb)
    if(WIN32)
        target_link_libraries(wkview PRIVATE Gdi32 User32)
    elseif(APPLE)
        target_link_libraries(wkview PRIVATE "-framework Cocoa" "-framework Metal" "-framework MetalKit" "-framework QuartzCore")
    elseif(UNIX)
        target_link_libraries(wkview PRIVATE X11)
    endif()
endif()
wk_copy_windows_runtime(wkview)
