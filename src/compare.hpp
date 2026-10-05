// compare.hpp - compares a developer snapshot with the machine we are running on.
//
// Layers (in this order): system -> toolchain -> build system -> libraries -> runtime.
// Every check produces an Item. Items with status Ok are kept so the report can
// show what WAS verified, not only what is wrong.
#pragma once

#include <string>
#include <vector>

#include "json.hpp"
#include "knowledge.hpp"
#include "model.hpp"

namespace bd {

enum class Status { Ok, Info, Warn, Fail };

struct Item {
  std::string layer;  // system | toolchain | build | libraries | runtime
  Status status = Status::Ok;
  std::string code;   // machine readable, e.g. TOOL_MISSING
  std::string name;   // g++, SDL2, libssl.so.3, platform ...
  std::string title;  // one line
  std::string detail; // longer explanation (may be empty)
  std::string dev;    // developer side value
  std::string you;    // this machine's value
  std::vector<std::string> pkgs;  // packages (for the local package manager) that fix it
  std::string hint;               // extra command / advice
};

struct Report {
  Platform local;
  PkgMgr pkgmgr = PkgMgr::None;
  std::string project;
  std::vector<Item> items;
  int fails = 0, warns = 0, infos = 0, oks = 0;
  std::vector<std::string> install_packages;  // deduplicated package list
  std::string install_cmd;                    // e.g. "sudo pacman -S --needed cmake sdl2"
};

Report compare_snapshot(const Snapshot& snap, const std::string& project_root);

const char* status_name(Status s) noexcept;
const char* layer_title(const std::string& layer) noexcept;
std::string platform_string(const Platform& p);
json::Value report_to_json(const Report& r);

}  // namespace bd
