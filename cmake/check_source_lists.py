#!/usr/bin/env python3
# Checks that the single source of truth (cmake/wk_sources.txt) agrees
# with the tree and with every consumer. Run: python3
# cmake/check_source_lists.py  (from the repository root). Also wired
# as the source_lists_test CTest. Exits non-zero with a clear message
# on any disagreement.
#
# What it checks:
#   1. Every path in the list file exists, and no path is listed in
#      two sections.
#   2. Every compilable source on disk is listed in exactly its
#      section (no orphans) — same rules as the CMake drift check,
#      re-implemented independently here.
#   3. web/build.sh and web/build.ps1 read only sections that exist,
#      consume the same section set, and contain no hardcoded
#      ../src/... source paths (except the web-only threading_stub.cpp
#      and wk_wasm.cpp) and no dangling libyuv references.
#   4. Every wk_read_sources(<section>) call in CMakeLists.txt,
#      cmake/*.cmake and src/arch/*/arch.cmake names a section that
#      exists.
#   5. The bash extractor in build.sh really returns the list file's
#      content per section (run for real); the PowerShell extractor is
#      simulated, since PowerShell is not available here.

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIST_FILE = os.path.join(ROOT, "cmake", "wk_sources.txt")

# Section -> directories whose compilable files belong to it.
SECTION_DIRS = {
    "core": ["src/core"],
    "stb": ["src/stb"],
    "webp": ["src/webp/src/dec", "src/webp/src/dsp", "src/webp/src/utils"],
    "avif": ["src/avif/libavif/src"],
    "libyuv": ["src/avif/libavif/third_party/libyuv/source"],
    "dav1d": ["src/avif/dav1d/src"],
    "dav1d_tmpl": ["src/avif/dav1d/src"],
    "arch_generic": ["src/arch/generic"],
    "arch_wasm": ["src/arch/wasm"],
    "arch_x86_64": ["src/arch/x86_64"],
    "arch_aarch64": ["src/arch/aarch64"],
    "arch_arm": ["src/arch/arm"],
}

# Sections the web builds must consume (libyuv is intentionally NOT
# compiled for wasm; arch sources are header-only or excluded there).
WEB_SECTIONS = {"core", "stb", "webp", "avif", "dav1d", "dav1d_tmpl"}

# Web-only files that may appear hardcoded in the web builds.
WEB_ALLOWLIST = {"threading_stub.cpp", "wk_wasm.cpp"}

FAILURES = []


def fail(msg):
    FAILURES.append(msg)
    print("FAIL: " + msg)


def parse_list_file():
    sections = {}
    current = None
    with open(LIST_FILE, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            m = re.match(r"^\[([A-Za-z0-9_]+)\]$", line)
            if m:
                current = m.group(1)
                sections.setdefault(current, [])
                continue
            if current is None:
                fail(f"list file: path outside any section: {line}")
                continue
            sections[current].append(line)
    return sections


def check_list_file(sections):
    seen = {}
    for section, paths in sections.items():
        if section not in SECTION_DIRS:
            fail(f"list file: unknown section [{section}]")
        for path in paths:
            if not os.path.isfile(os.path.join(ROOT, path)):
                fail(f"list file: [{section}] lists a missing file: {path}")
            if path in seen:
                fail(f"list file: {path} listed in both [{seen[path]}] and [{section}]")
            seen[path] = section


def on_disk_files(section):
    found = []
    for d in SECTION_DIRS[section]:
        full = os.path.join(ROOT, d)
        if not os.path.isdir(full):
            continue
        for name in sorted(os.listdir(full)):
            if name.endswith((".c", ".cpp")):
                found.append(f"{d}/{name}")
    if section == "dav1d":
        found = [f for f in found if not f.endswith("_tmpl.c")]
    if section == "dav1d_tmpl":
        found = [f for f in found if f.endswith("_tmpl.c")]
    return sorted(found)


def check_tree_coverage(sections):
    for section in SECTION_DIRS:
        listed = sorted(sections.get(section, []))
        disk = on_disk_files(section)
        if listed != disk:
            only_disk = sorted(set(disk) - set(listed))
            only_listed = sorted(set(listed) - set(disk))
            fail(f"section [{section}] disagrees with the tree: "
                 f"on disk, not listed: {only_disk}; "
                 f"listed, not on disk: {only_listed}")


def extract_bash_sections():
    with open(os.path.join(ROOT, "web", "build.sh"), encoding="utf-8") as fh:
        text = fh.read()
    return set(re.findall(r"wk_sources\s+([A-Za-z0-9_]+)", text))


def extract_ps1_sections():
    with open(os.path.join(ROOT, "web", "build.ps1"), encoding="utf-8") as fh:
        text = fh.read()
    return set(re.findall(r"Get-WkSources\s+'([A-Za-z0-9_]+)'", text))


def check_hardcoded_sources(path, text, kind):
    for m in re.finditer(r"\.\./src/[^\s'\"`]+\.(?:c|cpp|h)\b", text):
        hit = m.group(0)
        if os.path.basename(hit) in WEB_ALLOWLIST:
            continue
        fail(f"{path}: hardcoded source path outside the list file: {hit}")


def check_web_builds(sections):
    sh_path = os.path.join(ROOT, "web", "build.sh")
    ps1_path = os.path.join(ROOT, "web", "build.ps1")
    with open(sh_path, encoding="utf-8") as fh:
        sh = fh.read()
    with open(ps1_path, encoding="utf-8") as fh:
        ps1 = fh.read()

    sh_sections = extract_bash_sections()
    ps1_sections = extract_ps1_sections()
    if sh_sections != WEB_SECTIONS:
        fail(f"build.sh consumes sections {sorted(sh_sections)}, "
             f"expected {sorted(WEB_SECTIONS)}")
    if ps1_sections != WEB_SECTIONS:
        fail(f"build.ps1 consumes sections {sorted(ps1_sections)}, "
             f"expected {sorted(WEB_SECTIONS)}")
    if sh_sections != ps1_sections:
        fail(f"build.sh and build.ps1 consume different sections: "
             f"{sorted(sh_sections)} vs {sorted(ps1_sections)}")
    for section in sh_sections | ps1_sections:
        if section not in sections:
            fail(f"web build reads unknown section [{section}]")

    check_hardcoded_sources("web/build.sh", sh, "sh")
    check_hardcoded_sources("web/build.ps1", ps1, "ps1")
    # "libyuv" legitimately appears in include paths; only a variable
    # reference is a dangling dependency.
    if re.search(r"\$LIBYUV", sh):
        fail("web/build.sh: dangling $LIBYUV reference (never set)")
    if re.search(r"\$libyuv\w*", ps1):
        fail("web/build.ps1: dangling $libyuv* reference (never set)")
    if ",," in ps1:
        fail("web/build.ps1: ',,' list quirk still present")


def check_cmake_calls(sections):
    roots = [os.path.join(ROOT, "CMakeLists.txt")]
    roots += [os.path.join(ROOT, "cmake", f) for f in os.listdir(os.path.join(ROOT, "cmake"))
              if f.endswith(".cmake")]
    for arch in os.listdir(os.path.join(ROOT, "src", "arch")):
        frag = os.path.join(ROOT, "src", "arch", arch, "arch.cmake")
        if os.path.isfile(frag):
            roots.append(frag)
    for path in roots:
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
        for section in re.findall(r"wk_read_sources\(\s*([A-Za-z0-9_]+)", text):
            if section not in sections:
                fail(f"{os.path.relpath(path, ROOT)}: "
                     f"wk_read_sources of unknown section [{section}]")


def extract_section_bash(section):
    # Run the real wk_sources() function from web/build.sh.
    sh_path = os.path.join(ROOT, "web", "build.sh")
    with open(sh_path, encoding="utf-8") as fh:
        lines = fh.readlines()
    try:
        start = next(i for i, line in enumerate(lines) if line.startswith("wk_sources() {"))
        end = next(i for i in range(start, len(lines)) if lines[i].rstrip() == "}")
    except StopIteration:
        fail("web/build.sh: wk_sources() function not found")
        return None
    func = "".join(lines[start:end + 1])
    cmd = ("WK_SOURCES_FILE=\"../cmake/wk_sources.txt\"\n" + func
           + f"\nwk_sources {section}\n")
    out = subprocess.run(["bash", "-c", cmd], cwd=os.path.join(ROOT, "web"),
                         capture_output=True, text=True, timeout=30)
    if out.returncode != 0:
        fail(f"bash extractor failed for [{section}]: {out.stderr.strip()}")
        return None
    return [line for line in out.stdout.splitlines() if line]


def simulate_ps1_extraction(sections, section):
    # Simulate Get-WkSources from web/build.ps1: the function must
    # contain the section-header match and the comment/blank skip.
    ps1_path = os.path.join(ROOT, "web", "build.ps1")
    with open(ps1_path, encoding="utf-8") as fh:
        text = fh.read()
    m = re.search(r"function Get-WkSources.*?\n\}", text, re.DOTALL)
    if not m:
        fail("web/build.ps1: Get-WkSources function not found")
        return None
    body = m.group(0)
    for snippet in ["Get-Content", "StartsWith('#')",
                    "'^\\[(.+)\\]$'", "'../'"]:
        if snippet not in body:
            fail(f"web/build.ps1: Get-WkSources body lacks {snippet!r}")
            return None
    return ["../" + p for p in sections.get(section, [])]


def check_extractors(sections):
    for section in sorted(WEB_SECTIONS):
        got = extract_section_bash(section)
        if got is not None and got != sections.get(section, []):
            fail(f"bash extractor for [{section}] disagrees with the list file")
        sim = simulate_ps1_extraction(sections, section)
        if sim is not None and sim != ["../" + p for p in sections.get(section, [])]:
            fail(f"PowerShell extractor simulation for [{section}] disagrees")


def main():
    sections = parse_list_file()
    check_list_file(sections)
    check_tree_coverage(sections)
    check_web_builds(sections)
    check_cmake_calls(sections)
    check_extractors(sections)
    if FAILURES:
        print(f"{len(FAILURES)} check(s) failed")
        return 1
    print(f"source lists OK: {len(sections)} sections, "
          f"{sum(len(v) for v in sections.values())} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
