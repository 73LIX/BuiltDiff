// scan.hpp - reads the project's own build files (developer side) to find out
// what the project needs: build systems, languages, language standard,
// library dependencies and minimum tool versions.
//
// Uses small hand written tokenizers - no std::regex (slow, recursive, can blow
// the stack on long inputs).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "ecosystems.hpp"
#include "model.hpp"

namespace bd {

struct LibSpec {
  std::string name;
  std::string kind;  // cmake | pkg-config | make | meson | pip | npm | cargo | maven | go
  std::string required_min;
  std::string spec;           // the constraint as written, for language packages
  std::string version_locked; // exact version a lock file pinned, when present
  bool unverifiable = false;  // version came from something we cannot evaluate
  bool required = true;
};

struct ScanResult {
  std::string project_name;
  BuildInfo build;
  std::vector<LibSpec> libs;
  // Language package dependencies, already normalised by the ecosystem parsers.
  // Kept separate from `libs` only until snapshot.cpp merges them, because the
  // two sets are probed by completely different mechanisms.
  std::vector<PkgSpec> packages;
  std::map<std::string, std::string> tool_min;  // tool name -> minimum version demanded
  bool uses_pkgconfig = false;
};

// Normalises "c++23", "gnu++2b", "23" ... to "23" (empty if not understood).
std::string normalize_cxx_standard(std::string_view s);

struct CMakeCall {
  std::string name;  // lowercase command name
  std::vector<std::string> args;
};
std::vector<CMakeCall> parse_cmake_calls(std::string_view text);

ScanResult scan_project(const std::string& root);

}  // namespace bd
