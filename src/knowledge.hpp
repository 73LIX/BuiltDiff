// knowledge.hpp - static tables: which tools we may run, how CMake package names
// map to pkg-config modules, and which distro package provides what.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ecosystems.hpp"
#include "model.hpp"

namespace bd {

// ---- tool allowlist: the ONLY programs builtdiff will ever execute.
struct ToolDef {
  const char* name;
  const char* role;
  const char* group;  // "" if none
  std::vector<const char*> args;
};
const ToolDef* find_tool_def(std::string_view name) noexcept;
const std::vector<ToolDef>& all_tool_defs() noexcept;

// ---- libraries
// CMake package / pkg-config name -> candidate pkg-config module names.
std::vector<std::string> pkgconfig_candidates(std::string_view name);
// CMake find_package() names that never mean "a system library".
bool is_ignored_cmake_package(std::string_view name) noexcept;
// -l<name> flags that are part of libc & friends.
bool is_base_link_lib(std::string_view name) noexcept;

// ---- distro packages
enum class PkgMgr { None, Pacman, Apt, Dnf };
PkgMgr detect_pkg_mgr(std::string_view distro_id, std::string_view id_like);
const char* pkg_mgr_name(PkgMgr m) noexcept;
std::string install_command(PkgMgr m, const std::vector<std::string>& pkgs);
// Package names (for the given manager) that provide a tool / library / soname.
std::vector<std::string> packages_for_tool(PkgMgr m, std::string_view tool);
std::vector<std::string> packages_for_library(PkgMgr m, std::string_view name);
std::vector<std::string> packages_for_soname(PkgMgr m, std::string_view soname);
// Command a user can run to find which package owns a file / soname.
std::string find_owner_hint(PkgMgr m, std::string_view soname);

// ---- language standard heuristics (practical minimum compiler majors)
struct StdRequirement { int gcc; int clang; };
std::optional<StdRequirement> cxx_std_requirement(std::string_view std_number) noexcept;

// ---- language packages
// The command that installs `name` for this ecosystem. Built from a fixed
// template plus a name that already passed is_eco_package_name, so there is no
// injection surface here; the caller still renders it as text, never executes
// it. `spec` is appended only when it is a plain version, since
// "pip install x>=1,<2" is not valid shell for every package manager.
// A venv is preferred over the system interpreter when the project has one, so
// the suggested command lands in the same environment the project uses.
std::string eco_install_hint(Eco eco, std::string_view name, const std::string& project_root);
std::string eco_install_command(Eco eco, const std::vector<std::string>& names, const std::string& project_root);
// True when the project has a .venv / venv / env, i.e. the hint should use it.
bool project_has_venv(const std::string& project_root);

}  // namespace bd
