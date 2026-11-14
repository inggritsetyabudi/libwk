// Copyright 2026 Inggrit Setya Budi
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#pragma once

// Colored terminal logger for the wk* command-line tools.
//
// Writes to stderr (so stdout stays clean for piped primary output
// like --version, --json, or --info), with a level tag, an elapsed
// time since process start, and ANSI colors when the output is a
// terminal. Colors are OFF when stderr is not a TTY (piped/redirected),
// when NO_COLOR is set, or when TERM=dumb; --color forces them on,
// --no-color forces them off. -v/--verbose enables DEBUG, -q/--quiet
// hides INFO/DEBUG.
//
// The elapsed time on every line is deliberate: when a step is slow
// or a bug makes a run hang, the timestamps show exactly where the
// time went without a profiler.
//
// This is a CLI concern only — the library (src/) stays silent.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <io.h>
#define WK_ISATTY(fd) (_isatty(fd) != 0)
#else
#include <unistd.h>
#define WK_ISATTY(fd) (::isatty(fd) != 0)
#endif

namespace wk::tool {

enum class LogLevel : int {
    Error = 0,
    Warn = 1,
    Info = 2,
    Debug = 3,
};

namespace detail {

// Tri-state color setting: nullopt = auto-detect per stream,
// true = force colors on (--color), false = force off (--no-color).
// Auto-detection is per stream because stdout and stderr can be
// redirected independently (e.g. wkmetric piped to a file while the
// terminal still owns stderr).
inline std::optional<bool>& color_override() {
    static std::optional<bool> override;
    return override;
}

inline bool color_auto(int fd) {
    if (std::getenv("NO_COLOR") != nullptr) return false;
    const char* term = std::getenv("TERM");
    if (term != nullptr && std::string_view(term) == "dumb") return false;
    return WK_ISATTY(fd) != 0;
}

// Colors for the stderr log lines: forced by --color/--no-color,
// else auto-detected from stderr being a terminal.
inline bool stderr_color() {
    if (color_override().has_value()) return *color_override();
    return color_auto(2);
}

// Colors for a tool's primary stdout report (wkmetric text mode):
// forced by --color/--no-color, else auto-detected from stdout being
// a terminal. JSON output must never be colorized.
inline bool stdout_color() {
    if (color_override().has_value()) return *color_override();
    return color_auto(1);
}

inline LogLevel& log_level() {
    static LogLevel level = LogLevel::Info;
    return level;
}

inline std::chrono::steady_clock::time_point& log_start() {
    static std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    return start;
}

inline double elapsed_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - log_start()).count();
}

// Elapsed time as "[0.42s]" — fixed 2 decimals, never scientific.
inline std::string elapsed_tag() {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "[%.2fs]", elapsed_seconds());
    return buf;
}

inline const char* level_tag(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "ERROR";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Info: return "INFO";
        case LogLevel::Debug: return "DEBUG";
    }
    return "INFO";
}

// ANSI color for a level (empty when colors are off).
inline const char* level_color(LogLevel level) {
    if (!stderr_color()) return "";
    switch (level) {
        case LogLevel::Error: return "\033[1;31m";  // bold red
        case LogLevel::Warn: return "\033[33m";     // yellow
        case LogLevel::Info: return "\033[36m";     // cyan
        case LogLevel::Debug: return "\033[2;37m";  // dim white
    }
    return "";
}

inline const char* reset_color() { return stderr_color() ? "\033[0m" : ""; }

}  // namespace detail

inline void set_log_level(LogLevel level) { detail::log_level() = level; }
inline void set_log_color(bool enabled) { detail::color_override() = enabled; }
inline bool log_color_enabled() { return detail::stderr_color(); }
inline bool stdout_color_enabled() { return detail::stdout_color(); }

inline void log(LogLevel level, std::string_view message) {
    if (static_cast<int>(level) > static_cast<int>(detail::log_level())) return;
    std::cerr << detail::level_color(level) << '[' << detail::level_tag(level) << ']'
              << detail::reset_color() << ' ' << detail::elapsed_tag() << ' '
              << message << '\n';
}

inline void log_error(std::string_view message) { log(LogLevel::Error, message); }
inline void log_warn(std::string_view message) { log(LogLevel::Warn, message); }
inline void log_info(std::string_view message) { log(LogLevel::Info, message); }
inline void log_debug(std::string_view message) { log(LogLevel::Debug, message); }

// Success line: green OK tag, same elapsed-time format. Use for the
// final "done" summary of a tool run.
inline void log_ok(std::string_view message) {
    if (static_cast<int>(LogLevel::Info) > static_cast<int>(detail::log_level())) return;
    const char* green = log_color_enabled() ? "\033[1;32m" : "";
    std::cerr << green << "[OK]" << detail::reset_color() << ' ' << detail::elapsed_tag() << ' '
              << message << '\n';
}

// Color a value for a report by a quality threshold. `color` is the
// caller's decision (log lines: log_color_enabled(); stdout reports:
// stdout_color_enabled()). Returns the value unchanged when colors are
// off (piped output stays clean).
inline std::string colorize(std::string_view text, const char* ansi_code, bool color) {
    if (!color) return std::string(text);
    return std::string(ansi_code) + std::string(text) + "\033[0m";
}

// PSNR/SSIM coloring: green = good, yellow = acceptable, red = poor.
// Thresholds from the benchmark: q75 PSNR ~35.5 dB / SSIM ~0.95,
// q85 ~38.4 dB / ~0.97.
inline std::string color_psnr(double psnr, bool color) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", psnr);
    const char* code = psnr >= 35.0 ? "\033[1;32m" : (psnr >= 30.0 ? "\033[33m" : "\033[1;31m");
    return colorize(buf, code, color);
}

inline std::string color_ssim(double ssim, bool color) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f", ssim);
    const char* code = ssim >= 0.95 ? "\033[1;32m" : (ssim >= 0.90 ? "\033[33m" : "\033[1;31m");
    return colorize(buf, code, color);
}

}  // namespace wk::tool
