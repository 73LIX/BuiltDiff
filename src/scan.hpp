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

#include "model.hpp"

namespace bd {

struct LibSpec {
  std::string name;
  std::string kind;  // cmake | pkg-config | make | meson
  std::string required_min;
  bool required = true;
};

struct ScanResult {
  std::string project_name;
  BuildInfo build;
  std::vector<LibSpec> libs;
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
