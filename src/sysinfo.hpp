// sysinfo.hpp - look at the machine we are running on (used for BOTH the
// developer's snapshot and the user's check).
#pragma once

#include <functional>
#include <future>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "knowledge.hpp"
#include "model.hpp"

namespace bd {

struct OsRelease {
  std::string id;
  std::string id_like;
  std::string version_id;
};
OsRelease read_os_release();
Platform collect_platform();
std::string normalize_arch(std::string_view machine);

// Shared-library inventory of this machine (ldconfig cache + standard dirs).
class SystemLibs {
 public:
  SystemLibs();
  bool has_soname(std::string_view soname) const;
  std::vector<std::string> paths_for(std::string_view soname) const;
  bool ldconfig_ok() const noexcept { return ldconfig_ok_; }
  // Whether any library "lib<stem>.so*" is known.
  bool has_stem(std::string_view stem) const;

 private:
  std::map<std::string, std::vector<std::string>, std::less<>> cache_;
  bool ldconfig_ok_ = false;
};

struct ToolProbe {
  bool present = false;
  std::string version;
  std::string banner;
};

struct LibProbe {
  bool found = false;
  std::string version;
  std::string via;
};

// Thread-safe (all members are immutable after construction).
class Prober {
 public:
  Prober();
  ToolProbe probe_tool(std::string_view name) const;  // name must be in the allowlist
  LibProbe probe_library(std::string_view name) const;
  const SystemLibs& libs() const noexcept { return libs_; }
  bool have_pkg_config() const noexcept { return pkg_config_.has_value(); }

 private:
  SystemLibs libs_;
  std::optional<std::string> pkg_config_;
  std::vector<std::pair<std::string, std::string>> cmake_pkgs_;  // lowercase dir name -> full path
};

// Run f on every element on up to max_threads threads; results keep input order.
template <class T, class F>
auto parallel_map(const std::vector<T>& in, F f, std::size_t max_threads = 8)
    -> std::vector<decltype(f(in[0]))> {
  using R = decltype(f(in[0]));
  std::vector<std::future<R>> futs;
  futs.reserve(in.size());
  std::vector<R> out;
  out.reserve(in.size());
  std::size_t next_wait = 0;
  for (std::size_t i = 0; i < in.size(); ++i) {
    futs.push_back(std::async(std::launch::async, [&f, &in, i] { return f(in[i]); }));
    if (futs.size() - next_wait >= max_threads) out.push_back(futs[next_wait++].get());
  }
  while (next_wait < futs.size()) out.push_back(futs[next_wait++].get());
  return out;
}

}  // namespace bd
