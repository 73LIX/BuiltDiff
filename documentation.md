> Written in C++20, no third-party libraries (only the standard library and POSIX).

## Comparison Layers (What is compared?)
System _(OS, arch, distro, libc)_ -> Toolchain _(gcc/clang/cmake/make/ninja/python/node/rust/go..., C++ standard vs compiler)_
-> Build system _(build files changed since the snapshot)_ -> Libraries _(pkg-config / CMake config / ldconfig)_
-> Language packages -> Runtime <br>
Missing native packages are mapped to `pacman`, `apt` or `dnf` install commands.

## Language packages
- Python, npm, Cargo, Maven and Go dependencies are read from the manifests (`requirements.txt`, `pyproject.toml`,
`setup.py`, `package.json`, `Cargo.toml`, `pom.xml`, `build.gradle`/`build.gradle.kts`, `go.mod`) and compared
against what is actually installed.<br>
- Gradle dependencies resolve through the same Maven probe, so a `group:artifact` declared in either Gradle DSL is checked against `~/.m2` exactly like a `pom.xml` entry; a version the file computes elsewhere (BOM, version catalog, a mere `project(':x')`) is reported as unverifiable rather than guessed.<br>
- Lock files (`Cargo.lock`, `go.sum`) pin exact versions, and only names the project declares directly are reported.<br>
- A `go.sum` version is compared as a version, not text, so `v1.10.0` wins over `v1.9.1`.

Installed versions are found by reading the filesystem: `site-packages` (respecting
`include-system-site-packages = false` in `pyvenv.cfg`), `node_modules`, the Cargo registry, the Go module cache and
`~/.m2`. **No subprocess is used for this** - `pip list` and friends are not in the tool allowlist and never will be,
so the probes cannot execute project-controlled input.

Because these packages belong to the project rather than to the system, their install hints are kept out of the
distro `install_command` and reported separately, and in JSON each such item carries `"hint_scope": "project"`.

Version constraints are evaluated with each ecosystem's own grammar (`^`, `~`, x-ranges, `>=1,<2`, `~=`, Maven
`[1.0,2.0)`). A constraint that cannot be parsed - a git URL, a `${property}`, a `workspace:*` protocol - is reported
as *unverifiable* rather than assumed satisfied, and a version above a range's ceiling is reported as newer-than-allowed
instead of being silently called a pass.