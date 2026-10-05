#include "sysinfo.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>

#if defined(__GLIBC__) && __has_include(<gnu/libc-version.h>)
#include <gnu/libc-version.h>
#define BD_HAVE_GLIBC_VERSION 1
#endif

#include "proc.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

namespace bd {

// ------------------------------------------------------------ platform

std::string normalize_arch(std::string_view m) {
  const std::string s = to_lower(m);
  if (s == "x86_64" || s == "amd64") return "x86_64";
  if (s == "aarch64" || s == "arm64") return "aarch64";
  if (s == "i386" || s == "i486" || s == "i586" || s == "i686" || s == "x86") return "x86";
  if (s.rfind("armv", 0) == 0 || s == "arm" || s == "armhf") return "arm";
  return sanitize_text(s, 32);
}

OsRelease read_os_release() {
  OsRelease r;
  auto text = read_file("/etc/os-release", 64 * 1024);
  if (!text) text = read_file("/usr/lib/os-release", 64 * 1024);
  if (!text) return r;
  for (const auto& line : split(*text, '\n')) {
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = line.substr(0, eq);
    std::string val(trim(std::string_view(line).substr(eq + 1)));
    if (val.size() >= 2 && (val.front() == '"' || val.front() == '\'') && val.back() == val.front())
      val = val.substr(1, val.size() - 2);
    val = sanitize_text(val, 64);
    if (key == "ID") r.id = to_lower(val);
    else if (key == "ID_LIKE") r.id_like = to_lower(val);
    else if (key == "VERSION_ID") r.version_id = val;
  }
  return r;
}

Platform collect_platform() {
  Platform p;
  struct utsname u {};
  if (::uname(&u) == 0) {
    p.os = to_lower(sanitize_text(u.sysname, 32));
    p.arch = normalize_arch(u.machine);
  }
  if (p.os.empty()) p.os = "unknown";
  if (p.arch.empty()) p.arch = "unknown";
  OsRelease o = read_os_release();
  p.distro = o.id;
  p.distro_version = o.version_id;
#ifdef BD_HAVE_GLIBC_VERSION
  p.libc = "glibc";
  p.libc_version = sanitize_text(::gnu_get_libc_version(), 16);
#else
  p.libc = "unknown";
  std::error_code ec;
  for (fs::directory_iterator it("/lib", ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().filename().string().rfind("ld-musl-", 0) == 0) { p.libc = "musl"; break; }
  }
#endif
  return p;
}

// --------------------------------------------------------- SystemLibs

namespace {
const char* const kLibDirs[] = {"/lib64", "/lib", "/usr/lib", "/usr/lib64", "/usr/local/lib",
                                "/usr/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu",
                                "/lib/x86_64-linux-gnu", "/lib/aarch64-linux-gnu"};

std::optional<std::string> find_ldconfig() {
  if (auto p = find_in_path("ldconfig")) return p;
  for (const char* c : {"/sbin/ldconfig", "/usr/sbin/ldconfig", "/usr/bin/ldconfig"})
    if (::access(c, X_OK) == 0) return std::string(c);
  return std::nullopt;
}
}  // namespace

SystemLibs::SystemLibs() {
  auto exe = find_ldconfig();
  if (!exe) return;
  RunResult r = run_capture(*exe, {"-p"}, std::chrono::milliseconds(8000), 16u << 20);
  if (!r.spawned || r.exit_code != 0 || r.timed_out) return;
  ldconfig_ok_ = true;
  std::size_t pos = 0;
  while (pos < r.output.size()) {
    std::size_t nl = r.output.find('\n', pos);
    if (nl == std::string::npos) nl = r.output.size();
    std::string_view line = trim(std::string_view(r.output).substr(pos, nl - pos));
    pos = nl + 1;
    const auto arrow = line.find(" => ");
    const auto sp = line.find(' ');
    if (arrow == std::string_view::npos || sp == std::string_view::npos || sp > arrow) continue;
    std::string name(line.substr(0, sp));
    std::string path(line.substr(arrow + 4));
    if (name.empty() || path.empty() || path.front() != '/') continue;
    auto& v = cache_[name];
    if (v.size() < 8) v.push_back(std::move(path));
  }
}

std::vector<std::string> SystemLibs::paths_for(std::string_view soname) const {
  std::vector<std::string> out;
  if (!is_safe_token(soname, 128)) return out;
  if (auto it = cache_.find(soname); it != cache_.end()) out = it->second;
  for (const char* d : kLibDirs) {
    std::string p = std::string(d) + "/" + std::string(soname);
    if (::access(p.c_str(), R_OK) == 0 && std::find(out.begin(), out.end(), p) == out.end() && out.size() < 16)
      out.push_back(std::move(p));
  }
  return out;
}

bool SystemLibs::has_soname(std::string_view soname) const {
  if (!is_safe_token(soname, 128)) return false;
  if (cache_.find(soname) != cache_.end()) return true;
  for (const char* d : kLibDirs) {
    std::string p = std::string(d) + "/" + std::string(soname);
    if (::access(p.c_str(), F_OK) == 0) return true;
  }
  return false;
}

bool SystemLibs::has_stem(std::string_view stem) const {
  const std::string prefix = "lib" + std::string(stem) + ".so";
  auto it = cache_.lower_bound(prefix);
  if (it != cache_.end() && it->first.rfind(prefix, 0) == 0) return true;
  // also libfoo-2.0.so.0 style
  const std::string prefix2 = "lib" + std::string(stem) + "-";
  it = cache_.lower_bound(prefix2);
  return it != cache_.end() && it->first.rfind(prefix2, 0) == 0 && it->first.find(".so") != std::string::npos;
}

// -------------------------------------------------------------- Prober

namespace {

std::optional<std::string> first_line_with_digit(std::string_view out) {
  for (const auto& line : split(out, '\n')) {
    std::string_view t = trim(line);
    if (t.empty() || t.rfind("Picked up", 0) == 0) continue;
    if (t.find_first_of("0123456789") != std::string_view::npos) return std::string(t);
  }
  return std::nullopt;
}

std::string read_cmake_package_version(const std::string& dir) {
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::string fn = to_lower(it->path().filename().string());
    if (fn.size() > 12 && fn.find("version.cmake") != std::string::npos && fn.find("config") != std::string::npos) {
      auto txt = read_file(it->path().string(), 256 * 1024);
      if (!txt) continue;
      auto pos = txt->find("PACKAGE_VERSION \"");
      if (pos == std::string::npos) continue;
      pos += 17;
      auto end_q = txt->find('"', pos);
      if (end_q == std::string::npos) continue;
      if (auto v = parse_version(std::string_view(*txt).substr(pos, std::min<std::size_t>(end_q - pos, 32))))
        return version_string(*v);
    }
  }
  return {};
}

}  // namespace

Prober::Prober() : pkg_config_(find_in_path("pkg-config")) {
  if (!pkg_config_) pkg_config_ = find_in_path("pkgconf");
  std::vector<std::string> bases = {"/usr/lib/cmake",      "/usr/lib64/cmake",
                                    "/usr/share/cmake",    "/usr/local/lib/cmake",
                                    "/usr/local/share/cmake", "/usr/lib/x86_64-linux-gnu/cmake",
                                    "/usr/lib/aarch64-linux-gnu/cmake"};
  std::size_t budget = 6000;
  for (const auto& b : bases) {
    std::error_code ec;
    for (fs::directory_iterator it(b, ec), end; !ec && it != end && budget > 0; it.increment(ec), --budget) {
      std::error_code ec2;
      if (!it->is_directory(ec2)) continue;
      cmake_pkgs_.emplace_back(to_lower(it->path().filename().string()), it->path().string());
    }
  }
}

ToolProbe Prober::probe_tool(std::string_view name) const {
  ToolProbe t;
  const ToolDef* def = find_tool_def(name);
  if (!def) return t;
  auto exe = find_in_path(name);
  if (!exe) return t;
  t.present = true;
  std::vector<std::string> args(def->args.begin(), def->args.end());
  RunResult r = run_capture(*exe, args, std::chrono::milliseconds(6000), 8 * 1024);
  if (!r.spawned) return t;
  if (auto line = first_line_with_digit(r.output)) {
    t.banner = sanitize_text(*line, 120);
    if (auto v = parse_version(*line)) t.version = version_string(*v);
  }
  return t;
}

LibProbe Prober::probe_library(std::string_view name) const {
  LibProbe res;
  if (!is_safe_token(name, 64)) return res;
  const std::string lower = to_lower(name);

  if (lower == "boost") {
    for (const char* inc : {"/usr/include/boost/version.hpp", "/usr/local/include/boost/version.hpp"}) {
      auto txt = read_file(inc, 512 * 1024);
      if (!txt) continue;
      auto pos = txt->find("#define BOOST_LIB_VERSION \"");
      if (pos == std::string::npos) continue;
      pos += 28;
      auto q = txt->find('"', pos);
      if (q == std::string::npos || q - pos > 16) continue;
      std::string v = txt->substr(pos, q - pos);  // e.g. 1_83
      std::replace(v.begin(), v.end(), '_', '.');
      res.found = true;
      res.via = "header";
      if (auto pv = parse_version(v)) res.version = version_string(*pv);
      return res;
    }
  }

  if (pkg_config_) {
    int tries = 0;
    for (const auto& cand : pkgconfig_candidates(name)) {
      if (!is_safe_token(cand, 64) || ++tries > 4) continue;
      RunResult r = run_capture(*pkg_config_, {"--modversion", cand}, std::chrono::milliseconds(4000), 4096);
      if (r.spawned && r.exit_code == 0 && !r.timed_out) {
        res.found = true;
        res.via = "pkg-config";
        if (auto v = parse_version(r.output)) res.version = version_string(*v);
        return res;
      }
    }
  }

  for (const auto& cand : pkgconfig_candidates(name)) {
    const std::string lc = to_lower(cand);
    for (const auto& [dirname, full] : cmake_pkgs_) {
      if (dirname == lc) {
        res.found = true;
        res.via = "cmake-config";
        res.version = read_cmake_package_version(full);
        return res;
      }
    }
  }

  if (libs_.has_stem(lower)) {
    res.found = true;
    res.via = "ldconfig";
  }
  return res;
}

}  // namespace bd
