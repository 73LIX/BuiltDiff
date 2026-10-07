# BuiltDiff

> "It works on my machine" - BuiltDiff tells you why it doesn't work on yours.

<img width="1651" height="1053" alt="BuiltDiff" src="https://github.com/user-attachments/assets/754baba9-0956-44b5-b41e-175192670bdd" />
[asciinema](https://asciinema.org/a/nvp0a31ohh4kmmgy)

This is something every developer might have experienced during their development journey quite a lot. <br>
“If it works on my machine ~ it works” even though it doesn’t on the user ends - **Why? - Let BuiltDiff tell you.**

BuiltDiff snapshots the environment your project needs to run and instantly compares it against any other machine, telling you exactly what's different and what to install.

The developer runs `builtdiff snapshot` and commits a small `.builtdiff` file _(an environment
fingerprint of what the project needs: OS/arch, compilers, build tools, libraries, Python/npm/Cargo/Maven/Go
dependencies, and the shared libraries and glibc symbol versions their binaries need)_. <br> Users run `builtdiff check`
and get an exact list of what differs on their machine. With `--analyze`, a **local open-weight Gemma model (via Ollama)** turns that
list into a beginner-friendly explanation with fix steps. Nothing leaves your machine.

## Installation (Linux)

```bash
curl -fsSL https://raw.githubusercontent.com/73LIX/BuiltDiff/main/install.sh | sh
```
### Build
```bash
sudo pacman -S --needed base-devel cmake ninja git pkgconf
git clone https://github.com/73LIX/BuiltDiff.git && cd BuiltDiff
cmake -S . -B build -G Ninja && cmake --build build
```

## Usage
Developer:
```bash
builtdiff init        # writes builtdiff.json
builtdiff snapshot    # writes .builtdiff
git add .builtdiff && git commit -m "Add builtdiff snapshot"
```
User:
```bash
git clone https://github.com/userxyz/projectabc && cd projectabc
builtdiff check              # exit status 1 if something is wrong along with what is missing
builtdiff check --analyze    # better, detailed response by any ollama model (gemma, mistral, deepseek, llama)
```
BuiltDiff asks the ollama server which models it has and picks one itself, so any installed tag works **(#Tested on gemma4:e2b)**. Override with
`--model`, `--host`, `$BUILTDIFF_MODEL`, `$OLLAMA_HOST`. Without Ollama the normal report still prints.

## Privacy: What the snapshot contains
Tool names + versions, library names + versions, platform, build-file hashes, ELF metadata. It does **not** contain
user name, home path, hostname, environment variables, IP addresses, SSH keys or any file content.<br>
Home path, user and host names are redacted from the few strings that could carry them (e.g. compiler banners).

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
Tool probes run in parallel; `check` on a typical project takes ~40 ms. <br>
Fully static musl binaries (x86_64 and arm64) of ~840 KB, no heap-heavy regex.

## Linux Only
Linux is the fully supported platform for `snapshot`/`check` (macOS/Windows snapshots are *recognized* and reported as platform mismatches).<br>
Package-name tables cover common libraries only; unknown ones get a "find the owning package" hint.
