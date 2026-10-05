#include "compare.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <set>

#include "elf.hpp"
#include "sysinfo.hpp"
#include "util.hpp"

namespace bd {

namespace {

std::string cap_os(std::string os) {
  if (!os.empty()) os[0] = static_cast<char>(os[0] >= 'a' && os[0] <= 'z' ? os[0] - 'a' + 'A' : os[0]);
  return os;
}

std::string os_arch(const Platform& p) { return cap_os(p.os) + " " + p.arch; }

std::string or_str(const std::string& s, const char* fallback) { return s.empty() ? std::string(fallback) : s; }

const char* update_hint(PkgMgr m) {
  switch (m) {
    case PkgMgr::Pacman: return "Update your system (sudo pacman -Syu) - on Arch always upgrade the whole system, never a single package.";
    case PkgMgr::Apt: return "Update your system (sudo apt update && sudo apt upgrade) or use a newer distribution release / the vendor's repository.";
    case PkgMgr::Dnf: return "Update your system (sudo dnf upgrade) or use a newer distribution release.";
    default: return "Install a newer version using your distribution's package manager or the vendor's installer.";
  }
}

struct Ctx {
  const Snapshot& snap;
  const std::string& root;
  Report& rep;
  const Prober& prober;
  void add(Item it) { rep.items.push_back(std::move(it)); }
};

Item make(const char* layer, Status st, const char* code, std::string name, std::string title) {
  Item it;
  it.layer = layer;
  it.status = st;
  it.code = code;
  it.name = std::move(name);
  it.title = std::move(title);
  return it;
}

// ---------------------------------------------------------------- system

void check_system(Ctx& c) {
  const Platform& dev = c.snap.platform;
  const Platform& me = c.rep.local;
  const bool has_bins = !c.snap.binaries.empty();

  if (!dev.os.empty() && dev.os != "unknown") {
    const bool mismatch = dev.os != me.os || dev.arch != me.arch;
    Item it = make("system", mismatch ? (has_bins ? Status::Fail : Status::Warn) : Status::Ok, mismatch ? "PLATFORM_MISMATCH" : "PLATFORM_OK",
                   "platform", mismatch ? "PLATFORM MISMATCH" : "Same OS and CPU architecture");
    it.dev = os_arch(dev);
    it.you = os_arch(me);
    if (mismatch) {
      it.detail = "This project was built for " + it.dev + ".\nYou are running " + it.you + ".\n";
      it.detail += has_bins ? "The existing binary cannot be executed directly on this platform; build it from source for your system."
                            : "Building from source can still work, but build scripts and dependencies were prepared for another platform.";
    }
    c.add(std::move(it));
  }

  if (!dev.distro.empty() && !me.distro.empty()) {
    const bool same = dev.distro == me.distro;
    Item it = make("system", same ? Status::Ok : Status::Info, same ? "DISTRO_OK" : "DISTRO_DIFFERS", "distribution",
                   same ? "Same Linux distribution" : "Different Linux distribution");
    it.dev = dev.distro + (dev.distro_version.empty() ? "" : " " + dev.distro_version);
    it.you = me.distro + (me.distro_version.empty() ? "" : " " + me.distro_version);
    if (!same) it.detail = "Package names differ between distributions; suggested commands below are for " + std::string(pkg_mgr_name(c.rep.pkgmgr)) + ".";
    c.add(std::move(it));
  }

  const bool dev_libc = !dev.libc.empty() && dev.libc != "unknown";
  const bool me_libc = !me.libc.empty() && me.libc != "unknown";
  if (dev_libc && me_libc) {
    if (dev.libc != me.libc) {
      Item it = make("system", has_bins ? Status::Fail : Status::Warn, "LIBC_MISMATCH", "C library", "Different C standard library");
      it.dev = dev.libc + " " + dev.libc_version;
      it.you = me.libc + " " + me.libc_version;
      it.detail = "Binaries built against " + dev.libc + " usually do not run on " + me.libc + " (and vice versa).";
      c.add(std::move(it));
    } else {
      auto d = parse_version(dev.libc_version), m = parse_version(me.libc_version);
      Item it = make("system", Status::Ok, "LIBC_OK", "C library", dev.libc + " present");
      it.dev = dev.libc + " " + dev.libc_version;
      it.you = me.libc + " " + me.libc_version;
      if (d && m && compare_versions(*m, *d) < 0) {
        it.status = Status::Info;
        it.code = "LIBC_OLDER";
        it.title = "Older C library than the developer's";
        it.detail = "Programs built on the developer's machine may need newer " + dev.libc + " symbols; the runtime check below tells for sure.";
      }
      c.add(std::move(it));
    }
  }
}

// ------------------------------------------------------------- toolchain

struct VersionVerdict {
  Status status = Status::Ok;
  const char* code = "TOOL_OK";
  std::string title;
  std::string detail;
};

VersionVerdict judge_version(const std::string& name, const std::string& dev_v, const std::string& you_v,
                             const std::string& min_v) {
  VersionVerdict v;
  auto d = parse_version(dev_v), u = parse_version(you_v), m = parse_version(min_v);
  v.title = name + (you_v.empty() ? " is installed" : " " + you_v);
  if (!u) return v;
  if (m && compare_versions(*u, *m) < 0) {
    v.status = Status::Fail;
    v.code = "TOOL_TOO_OLD";
    v.title = name + " " + you_v + " is older than the minimum " + min_v + " the project requires";
    v.detail = "The project's build files demand at least " + name + " " + min_v + ".";
    return v;
  }
  if (d && u->major() < d->major()) {
    v.status = Status::Warn;
    v.code = "TOOL_OLDER";
    v.title = name + " " + you_v + " is older than the developer's " + dev_v;
    v.detail = "The project was built with a newer major version; features it uses may not exist in yours.";
    return v;
  }
  return v;
}

void check_toolchain(Ctx& c) {
  if (c.snap.tools.empty()) return;
  std::vector<std::string> names;
  for (const auto& t : c.snap.tools) names.push_back(t.name);
  auto probes = parallel_map(names, [&](const std::string& n) { return c.prober.probe_tool(n); });
  std::map<std::string, ToolProbe> by_name;
  for (std::size_t i = 0; i < names.size(); ++i) by_name[names[i]] = probes[i];

  std::set<std::string> done_groups;
  for (const auto& t : c.snap.tools) {
    if (t.group.empty()) {
      const ToolProbe& p = by_name[t.name];
      Item it = make("toolchain", Status::Ok, "TOOL_OK", t.name, t.name);
      it.dev = or_str(t.version, "installed");
      if (!p.present) {
        it.status = t.required ? Status::Fail : Status::Info;
        it.code = "TOOL_MISSING";
        it.title = t.name + " is not installed";
        it.you = "not found";
        it.detail = "The project needs " + t.name + " (the developer used " + or_str(t.version, "an unknown version") + ").";
        it.pkgs = packages_for_tool(c.rep.pkgmgr, t.name);
      } else {
        it.you = or_str(p.version, "installed");
        auto v = judge_version(t.name, t.version, p.version, t.required_min);
        it.status = v.status;
        it.code = v.code;
        it.title = v.title;
        it.detail = v.detail;
        if (v.status != Status::Ok) it.hint = update_hint(c.rep.pkgmgr);
      }
      c.add(std::move(it));
      continue;
    }

    if (!done_groups.insert(t.group).second) continue;
    std::vector<const Tool*> members;
    for (const auto& m : c.snap.tools)
      if (m.group == t.group) members.push_back(&m);
    const Tool* primary = members.front();
    for (const Tool* m : members)
      if (m->primary) primary = m;
    const std::string label = t.group == "cxx" ? "C++ compiler" : t.group == "cc" ? "C compiler" : "build tool";

    const Tool* chosen = nullptr;
    if (by_name[primary->name].present) chosen = primary;
    else
      for (const Tool* m : members)
        if (by_name[m->name].present) { chosen = m; break; }

    if (!chosen) {
      std::string alts;
      for (const Tool* m : members) alts += (alts.empty() ? "" : ", ") + m->name;
      Item it = make("toolchain", Status::Fail, "GROUP_MISSING", label, "No " + label + " found");
      it.dev = primary->name + " " + primary->version;
      it.you = "none of: " + alts;
      it.detail = "The project is built with " + primary->name + " " + or_str(primary->version, "") + "; none of (" + alts + ") is installed.";
      it.pkgs = packages_for_tool(c.rep.pkgmgr, primary->name);
      c.add(std::move(it));
      continue;
    }

    const ToolProbe& p = by_name[chosen->name];
    auto v = judge_version(chosen->name, chosen == primary ? primary->version : std::string(), p.version, primary->required_min);
    if (chosen == primary) v = judge_version(chosen->name, primary->version, p.version, primary->required_min);
    Item it = make("toolchain", v.status, v.code, label, v.title);
    it.dev = primary->name + " " + or_str(primary->version, "?");
    it.you = chosen->name + " " + or_str(p.version, "?");
    it.detail = v.detail;
    if (chosen != primary && t.group != "generator") {
      if (it.status == Status::Ok || it.status == Status::Info) it.status = Status::Info;
      it.code = "COMPILER_DIFFERS";
      it.title = "You use " + chosen->name + ", the developer used " + primary->name;
      it.detail = "Usually fine, but warnings/errors can differ between compilers.";
    }
    if (it.status == Status::Fail || it.status == Status::Warn) it.hint = update_hint(c.rep.pkgmgr);
    c.add(std::move(it));

    // language standard heuristic for the C++ compiler
    if (t.group == "cxx" && !c.snap.build.cxx_standard.empty()) {
      auto req = cxx_std_requirement(c.snap.build.cxx_standard);
      auto lv = parse_version(p.version);
      if (req && lv) {
        const bool clang = chosen->name.find("clang") != std::string::npos;
        const int need = clang ? req->clang : req->gcc;
        Item si = make("toolchain", Status::Ok, "CXX_STANDARD_OK", "C++" + c.snap.build.cxx_standard,
                       "C++" + c.snap.build.cxx_standard + " is supported by " + chosen->name + " " + p.version);
        si.dev = "C++" + c.snap.build.cxx_standard;
        si.you = chosen->name + " " + p.version;
        if (lv->major() < static_cast<std::uint64_t>(need)) {
          si.status = Status::Fail;
          si.code = "CXX_STANDARD_UNSUPPORTED";
          si.title = "Your " + chosen->name + " " + p.version + " is too old for C++" + c.snap.build.cxx_standard;
          si.detail = "The project is compiled as C++" + c.snap.build.cxx_standard + ". A practical minimum is " + chosen->name +
                      " " + std::to_string(need) + " (heuristic). Typical symptoms: \"'std::expected' is not a member of 'std'\", unknown -std= values, missing <format>/<print>.";
          si.hint = update_hint(c.rep.pkgmgr);
        }
        c.add(std::move(si));
      }
    }
  }
}

// ----------------------------------------------------------- build files

void check_build(Ctx& c) {
  const auto& b = c.snap.build;
  for (const auto& f : b.files) {
    auto text = read_file_under(c.root, f.path, 2u << 20);
    if (!text) {
      Item it = make("build", Status::Warn, "BUILD_FILE_MISSING", f.path, f.path + " from the snapshot is not in this directory");
      it.detail = "Are you inside the project's root directory? builtdiff compares the project in " + std::string(".") + " with the snapshot.";
      c.add(std::move(it));
    } else if (sha256_hex(*text) != f.sha256) {
      Item it = make("build", Status::Info, "BUILD_FILE_CHANGED", f.path, f.path + " changed after the snapshot was taken");
      it.detail = "Results may be slightly out of date. Ask the maintainer to run 'builtdiff snapshot' again.";
      c.add(std::move(it));
    } else {
      Item it = make("build", Status::Ok, "BUILD_FILE_OK", f.path, f.path + " matches the snapshot");
      c.add(std::move(it));
    }
  }
  if (b.fetches_network) {
    Item it = make("build", Status::Info, "NEEDS_NETWORK", "network", "The build downloads dependencies (FetchContent / ExternalProject)");
    it.detail = "You need git and a working internet connection during configuration.";
    c.add(std::move(it));
  }
  if (!b.build_hint.empty()) {
    Item it = make("build", Status::Info, "BUILD_HINT", "build command", "Maintainer's suggested build command: " + b.build_hint);
    c.add(std::move(it));
  }
  if (!c.snap.fingerprint_ok) {
    Item it = make("build", Status::Warn, "SNAPSHOT_MODIFIED", "snapshot", "The snapshot's checksum does not match its content");
    it.detail = "The file was edited by hand, damaged, or contained entries builtdiff refused to accept. Treat the comparison with care.";
    c.add(std::move(it));
  }
}

// ------------------------------------------------------------- libraries

void check_libraries(Ctx& c) {
  if (c.snap.libs.empty()) return;
  auto probes = parallel_map(c.snap.libs, [&](const Library& l) { return c.prober.probe_library(l.name); });
  for (std::size_t i = 0; i < c.snap.libs.size(); ++i) {
    const Library& l = c.snap.libs[i];
    const LibProbe& p = probes[i];
    Item it = make("libraries", Status::Ok, "LIB_OK", l.name, l.name);
    it.dev = l.found ? or_str(l.version, "found") : "not verified";
    if (!p.found) {
      it.status = l.required ? (l.found ? Status::Fail : Status::Warn) : Status::Info;
      it.code = "LIB_MISSING";
      it.title = l.name + " library not found";
      it.you = "not found";
      it.detail = "Needed by the build (" + l.kind + ")" + (l.required_min.empty() ? "" : ", version >= " + l.required_min) + ".";
      if (!l.required) it.detail += " It is optional.";
      if (!l.found) it.detail += " The developer's snapshot could not verify it either.";
      it.pkgs = packages_for_library(c.rep.pkgmgr, l.name);
      if (it.pkgs.empty())
        it.hint = "Search for it: " + std::string(c.rep.pkgmgr == PkgMgr::Pacman ? "pacman -Ss " : c.rep.pkgmgr == PkgMgr::Apt ? "apt search " :
                                                  c.rep.pkgmgr == PkgMgr::Dnf ? "dnf search " : "your package manager: search ") + to_lower(l.name);
    } else {
      it.you = or_str(p.version, "found") + " (" + p.via + ")";
      auto u = parse_version(p.version), d = parse_version(l.version), m = parse_version(l.required_min);
      if (u && m && compare_versions(*u, *m) < 0) {
        it.status = Status::Fail;
        it.code = "LIB_TOO_OLD";
        it.title = l.name + " " + p.version + " is older than the required " + l.required_min;
        it.hint = update_hint(c.rep.pkgmgr);
      } else if (u && d && u->major() < d->major()) {
        it.status = Status::Warn;
        it.code = "LIB_OLDER";
        it.title = l.name + " " + p.version + " is older than the developer's " + l.version;
        it.hint = update_hint(c.rep.pkgmgr);
      } else {
        it.title = l.name + " found";
      }
    }
    c.add(std::move(it));
  }
}

// --------------------------------------------------------------- runtime

struct LocalLibCache {
  std::map<std::string, std::optional<ElfInfo>> parsed;
  const std::optional<ElfInfo>& get(const std::string& path) {
    auto it = parsed.find(path);
    if (it != parsed.end()) return it->second;
    std::optional<ElfInfo> info;
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (fd >= 0) {
      info = parse_elf_fd(fd);
      ::close(fd);
    }
    return parsed.emplace(path, std::move(info)).first->second;
  }
};

std::string family_of(const std::string& v) {
  auto p = v.find('_');
  return p == std::string::npos ? v : v.substr(0, p);
}

void check_binary(Ctx& c, const Binary& bin, LocalLibCache& cache, const SystemLibs& libs) {
  const Platform& me = c.rep.local;
  Binary eff = bin;
  bool local_copy = false;
  if (int fd = open_under(c.root, bin.path); fd >= 0) {
    if (auto info = parse_elf_fd(fd); info && bin.format == "elf") {
      local_copy = true;
      eff.arch = elf_arch_name(info->machine, info->bits, info->little);
      eff.interpreter = sanitize_text(info->interpreter, 128);
      eff.needed.clear();
      for (auto& s : info->needed) if (is_safe_token(s, 128) && eff.needed.size() < 512) eff.needed.push_back(s);
      eff.verneed.clear();
      for (auto& v : info->verneed) {
        if (!is_safe_token(v.file, 128) || eff.verneed.size() >= 512) continue;
        VerNeed n; n.file = v.file;
        for (auto& x : v.versions) if (is_safe_token(x, 64) && n.versions.size() < 256) n.versions.push_back(x);
        eff.verneed.push_back(std::move(n));
      }
      eff.runpath = info->runpath;
    }
    ::close(fd);
  }
  const std::string where = "./" + bin.path;

  // can it run here at all?
  const bool wrong_os = (bin.format == "elf" && me.os != "linux") || (bin.format == "pe" && me.os != "windows") ||
                        (bin.format == "macho" && me.os != "darwin");
  if (wrong_os || (!bin.arch.empty() && bin.arch != "unknown" && bin.arch != me.arch)) {
    Item it = make("runtime", Status::Fail, wrong_os ? "BINARY_FORMAT_MISMATCH" : "BINARY_ARCH_MISMATCH", where,
                   where + " cannot run on this machine");
    it.dev = bin.format + " " + bin.arch;
    it.you = me.os + " " + me.arch;
    it.detail = "This is a " + bin.format + " executable for " + bin.arch + "; you are running " + me.os + " " + me.arch +
                ". Build the project from source instead.";
    c.add(std::move(it));
    return;
  }
  if (bin.format != "elf") return;

  int problems = 0;
  // dynamic loader
  if (!eff.interpreter.empty()) {
    const std::string& in = eff.interpreter;
    if (in.size() > 1 && in[0] == '/' && is_safe_relpath(in.substr(1))) {
      if (::access(in.c_str(), F_OK) != 0) {
        ++problems;
        Item it = make("runtime", Status::Fail, "LOADER_MISSING", in, "Dynamic loader " + in + " is missing");
        it.dev = in;
        it.you = "not found";
        it.detail = where + " asks the kernel to start " + in + ", which does not exist here. Running it prints a confusing\n"
                    "\"No such file or directory\" even though the file exists.";
        it.pkgs = packages_for_soname(c.rep.pkgmgr, "libc.so");
        c.add(std::move(it));
      }
    }
  }

  // shared libraries
  for (const auto& so : eff.needed) {
    if (libs.has_soname(so)) continue;
    // $ORIGIN relative RUNPATH next to the binary
    bool bundled = false;
    for (const auto& rp : eff.runpath) {
      if (rp.rfind("$ORIGIN", 0) != 0) continue;
      std::string sub = rp.substr(7);
      if (!sub.empty() && sub[0] == '/') sub.erase(0, 1);
      std::string dir = bin.path.substr(0, bin.path.rfind('/') == std::string::npos ? 0 : bin.path.rfind('/'));
      std::string rel = dir.empty() ? sub : (sub.empty() ? dir : dir + "/" + sub);
      rel += (rel.empty() ? "" : "/") + so;
      if (int fd = open_under(c.root, rel); fd >= 0) { ::close(fd); bundled = true; break; }
    }
    if (bundled) continue;
    ++problems;
    Item it = make("runtime", Status::Fail, "RUNTIME_LIB_MISSING", so, so + " is missing");
    it.dev = "linked";
    it.you = "Not Found";
    it.detail = "Required by " + where + "\nStatus: Not Found";
    it.pkgs = packages_for_soname(c.rep.pkgmgr, so);
    if (it.pkgs.empty()) it.hint = "Find the package that ships it: " + find_owner_hint(c.rep.pkgmgr, so);
    c.add(std::move(it));
  }

  // symbol versions (GLIBC_2.38, GLIBCXX_3.4.32 ...)
  for (const auto& vn : eff.verneed) {
    if (!libs.has_soname(vn.file)) continue;  // already reported as missing
    const std::optional<ElfInfo>* lib = nullptr;
    int tried = 0;
    for (const auto& path : libs.paths_for(vn.file)) {
      if (++tried > 6) break;
      const auto& info = cache.get(path);
      if (info && elf_arch_name(info->machine, info->bits, info->little) == eff.arch) { lib = &info; break; }
    }
    if (!lib || (*lib)->verdef.empty()) continue;
    std::set<std::string> provided((*lib)->verdef.begin(), (*lib)->verdef.end());
    std::map<std::string, std::string> worst_missing, best_provided;
    auto newer = [](const std::string& a, const std::string& b) {
      auto va = parse_version(a.substr(a.find('_') == std::string::npos ? 0 : a.find('_') + 1));
      auto vb = parse_version(b.substr(b.find('_') == std::string::npos ? 0 : b.find('_') + 1));
      if (!va || !vb) return false;
      return compare_versions(*va, *vb) > 0;
    };
    std::size_t missing = 0;
    for (const auto& v : vn.versions) {
      if (provided.count(v)) continue;
      ++missing;
      auto& w = worst_missing[family_of(v)];
      if (w.empty() || newer(v, w)) w = v;
    }
    if (missing == 0) continue;
    for (const auto& v : provided) {
      auto fam = family_of(v);
      if (!worst_missing.count(fam)) continue;
      auto& b = best_provided[fam];
      if (b.empty() || newer(v, b)) b = v;
    }
    ++problems;
    std::string need;
    for (const auto& [fam, v] : worst_missing)
      need += (need.empty() ? "" : "; ") + std::string("needs ") + v + (best_provided[fam].empty() ? "" : ", this machine provides up to " + best_provided[fam]);
    Item it = make("runtime", Status::Fail, "SYMVER_MISSING", vn.file,
                   vn.file + " on this machine is too old for " + where);
    it.dev = worst_missing.begin()->second;
    it.you = best_provided[worst_missing.begin()->first].empty() ? "older" : best_provided[worst_missing.begin()->first];
    it.detail = where + " " + need + ".\nTypical error: \"version `" + worst_missing.begin()->second + "' not found (required by " + where + ")\".";
    it.hint = "The binary was built against a newer " + vn.file + " than your system ships. Build it from source on this machine, "
              "or " + std::string(update_hint(c.rep.pkgmgr));
    c.add(std::move(it));
  }

  if (problems == 0) {
    Item it = make("runtime", Status::Ok, "RUNTIME_OK", where,
                   where + ": all " + std::to_string(eff.needed.size()) + " shared libraries and symbol versions are available");
    it.detail = local_copy ? "(checked the local copy of the binary)" : "(checked using the data recorded in the snapshot; the binary itself is not present here)";
    c.add(std::move(it));
  }
}

void check_runtime(Ctx& c) {
  if (c.snap.binaries.empty()) {
    c.add(make("runtime", Status::Info, "NO_BINARIES", "binaries", "The snapshot records no prebuilt binaries (build-from-source project)"));
    return;
  }
  LocalLibCache cache;
  for (const auto& b : c.snap.binaries) check_binary(c, b, cache, c.prober.libs());
}

int layer_index(const std::string& l) {
  static const char* order[] = {"system", "toolchain", "build", "libraries", "runtime"};
  for (int i = 0; i < 5; ++i)
    if (l == order[i]) return i;
  return 5;
}

bool is_missing_code(const std::string& code) {
  return code == "TOOL_MISSING" || code == "GROUP_MISSING" || code == "LIB_MISSING" || code == "RUNTIME_LIB_MISSING" ||
         code == "LOADER_MISSING";
}

}  // namespace

const char* status_name(Status s) noexcept {
  switch (s) {
    case Status::Ok: return "ok";
    case Status::Info: return "info";
    case Status::Warn: return "warn";
    case Status::Fail: return "fail";
  }
  return "?";
}

const char* layer_title(const std::string& l) noexcept {
  if (l == "system") return "SYSTEM";
  if (l == "toolchain") return "TOOLCHAIN";
  if (l == "build") return "BUILD SYSTEM";
  if (l == "libraries") return "LIBRARIES";
  if (l == "runtime") return "RUNTIME";
  return "OTHER";
}

std::string platform_string(const Platform& p) {
  std::string s = os_arch(p);
  if (!p.distro.empty()) s += " (" + p.distro + (p.distro_version.empty() ? "" : " " + p.distro_version) + ")";
  return s;
}

Report compare_snapshot(const Snapshot& snap, const std::string& root) {
  Report rep;
  rep.local = collect_platform();
  OsRelease osr = read_os_release();
  rep.pkgmgr = detect_pkg_mgr(osr.id, osr.id_like);
  rep.project = snap.project;
  Prober prober;
  Ctx c{snap, root, rep, prober};
  check_system(c);
  check_toolchain(c);
  check_build(c);
  check_libraries(c);
  check_runtime(c);

  std::stable_sort(rep.items.begin(), rep.items.end(), [](const Item& a, const Item& b) {
    const int la = layer_index(a.layer), lb = layer_index(b.layer);
    if (la != lb) return la < lb;
    return static_cast<int>(a.status) > static_cast<int>(b.status);  // Fail first
  });
  for (const auto& it : rep.items) {
    switch (it.status) {
      case Status::Ok: ++rep.oks; break;
      case Status::Info: ++rep.infos; break;
      case Status::Warn: ++rep.warns; break;
      case Status::Fail: ++rep.fails; break;
    }
    if ((it.status == Status::Fail || it.status == Status::Warn || it.status == Status::Info) && is_missing_code(it.code))
      for (const auto& p : it.pkgs)
        if (std::find(rep.install_packages.begin(), rep.install_packages.end(), p) == rep.install_packages.end())
          rep.install_packages.push_back(p);
  }
  rep.install_cmd = install_command(rep.pkgmgr, rep.install_packages);
  return rep;
}

json::Value report_to_json(const Report& r) {
  using json::Value;
  Value root = Value::object();
  root.set("project", r.project);
  root.set("platform", platform_string(r.local));
  root.set("package_manager", pkg_mgr_name(r.pkgmgr));
  Value sum = Value::object();
  sum.set("fail", r.fails);
  sum.set("warn", r.warns);
  sum.set("info", r.infos);
  sum.set("ok", r.oks);
  root.set("summary", std::move(sum));
  Value items = Value::array();
  for (const auto& it : r.items) {
    Value o = Value::object();
    o.set("layer", it.layer);
    o.set("status", status_name(it.status));
    o.set("code", it.code);
    o.set("name", it.name);
    o.set("title", it.title);
    o.set("detail", it.detail);
    o.set("developer", it.dev);
    o.set("you", it.you);
    Value pk = Value::array();
    for (const auto& p : it.pkgs) pk.push(p);
    o.set("packages", std::move(pk));
    o.set("hint", it.hint);
    items.push(std::move(o));
  }
  root.set("items", std::move(items));
  root.set("install_command", r.install_cmd);
  return root;
}

}  // namespace bd
