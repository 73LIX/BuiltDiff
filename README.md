# BuiltDiff

> "It works on my machine" - BuiltDiff tells you why it doesn't work on yours.

The developer runs `builtdiff snapshot` and commits a small `.builtdiff` file (an environment
*fingerprint* of what the project needs: OS/arch, compilers, build tools, libraries, the shared
libraries and glibc symbol versions their binaries need). Users run `builtdiff check` and get an exact list of
what differs on their machine. With `--analyze`, a **local open-weight Gemma model (via Ollama)** turns that
list into a beginner-friendly explanation with fix steps. Nothing leaves your machine.

Written in C++20, no third-party libraries (only the standard library and POSIX).

## Install on Arch Linux

```bash
sudo pacman -S --needed base-devel cmake ninja git pkgconf
git clone https://github.com/73LIX/BuiltDiff.git && cd builtdiff
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build            # optional
sudo cmake --install build        # installs /usr/local/bin/builtdiff
```
Or build a package: `makepkg -si` (uses the included `PKGBUILD`, builds from the local checkout).

### AI explanations (optional)
```bash
sudo pacman -S ollama             # or ollama-cuda / ollama-rocm
sudo systemctl enable --now ollama
ollama pull gemma4:e2b                # any chat-capable model works: gemma3:4b, gemma4:e2b, llama3.2 ...
builtdiff check --analyze
```
BuiltDiff asks the server which models it has and picks one itself, so any installed tag works. Override with
`--model`, `--host`, `$BUILTDIFF_MODEL`, `$OLLAMA_HOST`. Without Ollama the normal report still prints.

## Usage
Developer:
```bash
builtdiff init        # optional builtdiff.json: binaries / extra libs / extra tools to track
builtdiff snapshot    # writes .builtdiff
git add .builtdiff && git commit -m "Add builtdiff snapshot"
```
User:
```bash
git clone https://github.com/userxyz/projectabc && cd projectabc
builtdiff check              # exit status 1 if something is wrong
builtdiff check --analyze    # + Gemma explanation
builtdiff check --json       # for CI / scripts
```

## What is compared (layers)
System (OS, arch, distro, libc) -> Toolchain (gcc/clang/cmake/make/ninja/python/node/rust/go..., C++ standard vs compiler)
-> Build system (build files changed since the snapshot) -> Libraries (pkg-config / CMake config / ldconfig)
-> Runtime (ELF `DT_NEEDED`, loader, `GLIBC_x.y` / `GLIBCXX_x.y` symbol versions; read by a bounds-checked parser, `ldd` is never run).
Missing packages are mapped to `pacman`, `apt` or `dnf` install commands.

## Privacy: What the snapshot contains
Tool names + versions, library names + versions, platform, build-file hashes, ELF metadata. It does **not** contain
user name, home path, hostname, environment variables, IP addresses, SSH keys or any file content. Home path, user and host
names are redacted from the few strings that could carry them (e.g. compiler banners).

## Security
* A `.builtdiff` arrives with a cloned repo, so it is **untrusted**: bounded JSON parser (depth, size, duplicate keys),
  every field validated and sanitized once, control characters stripped before printing, checksum tamper warning.
* Only an allowlist of tools is ever executed, via `posix_spawn` with an argv vector (no shell), absolute paths,
  empty stdin, timeout, output cap, minimal environment, and never a `.` entry in `PATH`.
* Files are read with `openat` + `O_NOFOLLOW` below the project root (no symlink escapes) and size caps.
* Model traffic: loopback only unless `--allow-remote`; the prompt marks the report as data; the reply is sanitized
  (escape sequences cannot reach your terminal) and capped.
* Hardened build flags (stack protector, FORTIFY, PIE, full RELRO). Tests include corrupted-ELF fuzzing; run them under
  ASan/UBSan with `cmake -S . -B build-asan -DBUILTDIFF_SANITIZE=ON`.

## Performance
Tool probes run in parallel; `check` on a typical project takes ~40 ms. Static binary of ~800 KB, no heap-heavy regex.

## Layout
`src/` util, json, proc, elf, knowledge, sysinfo, scan, snapshot, compare, http, analyze, render, main - `tests/tests.cpp`.

## Limitations (MVP)
Linux is the fully supported platform for `snapshot`/`check` (macOS/Windows snapshots are *recognized* and reported as platform mismatches).
Package-name tables cover common libraries only; unknown ones get a "find the owning package" hint.
