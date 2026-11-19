# cmake/wk_webp.cmake — vendored libwebp 1.3.2 decode-only (see
# src/webp/README.md). The vendored sources include each other as
# "src/...", so src/webp is an include dir for the whole target.
wk_read_sources(webp WK_WEBP_SOURCES)
