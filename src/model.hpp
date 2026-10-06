// model.hpp - typed snapshot model. Everything read from a .builtdiff file is
// validated and sanitized into these structs once (see snapshot.cpp); the rest
// of the program never touches raw JSON from disk.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "elf.hpp"

namespace bd {

inline constexpr int kSnapshotFormat = 1;
inline constexpr const char* kSnapshotFile = ".builtdiff";
inline constexpr const char* kConfigFile = "builtdiff.json";

struct Platform {
  std::string os;              // linux, darwin, ...
  std::string arch;            // x86_64, aarch64, ...
  std::string distro;          // arch, ubuntu, fedora ...
  std::string distro_version;  // 24.04 (empty on rolling releases)
  std::string libc;            // glibc, musl, unknown
  std::string libc_version;
};

struct Tool {
  std::string name;      // must be in the allowlist (knowledge.hpp)
  std::string role;      // compiler | build | interpreter | linker | vcs | pkgconfig
  std::string group;     // alternatives group ("cxx", "cc", "generator") or empty
  std::string version;   // dotted version or empty
  std::string banner;    // first line of --version, sanitized + redacted
  std::string required_min;  // minimum demanded by the project files (e.g. cmake_minimum_required)
  bool primary = false;  // the compiler the developer actually used
  bool required = true;
};

struct Library {
  std::string name;          // as written in the build files (SDL2, openssl, gtk+-3.0 ...)
  // cmake | pkg-config | make | meson for native linkage; pip | npm | cargo |
  // maven | go for language packages. The ecosystem parsers in ecosystems.cpp
  // own the second form.
  std::string kind;
  std::string required_min;  // version constraint from the build files
  // The constraint as written ("^4.18.0", ">=5.9,<7", "[1.0,2.0)"). Optional
  // and absent from older snapshots; required_min is the derived lower bound
  // that everything downstream actually compares against.
  std::string spec;
  // The exact version a lock file pinned, when the project ships one.
  std::string version_locked;
  // The declared version is something we cannot evaluate (a git URL, a maven
  // ${property}, an npm workspace protocol). Checked for presence only, and
  // the report must say so rather than imply the constraint was verified.
  bool unverifiable = false;
  std::string version;       // version found on the developer's machine
  std::string via;           // pkg-config | cmake-config | header | ldconfig | site-packages | node_modules | ...
  bool required = true;
  bool found = false;        // found on the developer's machine at snapshot time
};

struct Binary {
  std::string path;       // relative to the project root
  std::string format;     // elf | pe | macho
  std::string arch;       // canonical arch name
  int bits = 0;
  bool little = true;
  std::string type;       // exec | dyn
  std::string interpreter;
  std::vector<std::string> needed;
  std::vector<std::string> runpath;
  std::vector<VerNeed> verneed;
};

struct FileHash {
  std::string path;
  std::string sha256;
};

struct BuildInfo {
  std::vector<std::string> systems;    // cmake, make, meson, cargo, npm ...
  std::vector<std::string> languages;  // c++, c, rust, python ...
  // Ecosystems whose packages this snapshot carries: pip, npm, cargo, maven,
  // go. Empty means a native-only project, so an old snapshot is still valid.
  std::vector<std::string> package_managers;
  std::string cxx_standard;            // "23"
  std::string c_standard;
  bool fetches_network = false;        // FetchContent / ExternalProject
  std::string build_hint;              // optional, from builtdiff.json
  std::vector<FileHash> files;
};

struct Snapshot {
  int format = kSnapshotFormat;
  std::string tool_version;
  std::string created;
  std::string project;
  std::string fingerprint;  // sha256 of the canonical content (integrity, not authenticity)
  bool fingerprint_ok = true;
  Platform platform;
  std::vector<Tool> tools;
  std::vector<Library> libs;
  std::vector<Binary> binaries;
  BuildInfo build;
};

}  // namespace bd
