#include "snapshot.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <set>

#include "elf.hpp"
#include "knowledge.hpp"
#include "proc.hpp"
#include "scan.hpp"
#include "sysinfo.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

namespace bd {

namespace {

constexpr std::size_t kMaxSnapshotBytes = 4u << 20;
constexpr std::size_t kMaxTools = 64, kMaxLibs = 256, kMaxBinaries = 64, kMaxNeeded = 512, kMaxFiles = 32;

void add_unique(std::vector<std::string>& v, const std::string& s) {
  if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
}

json::Value jstrs(const std::vector<std::string>& v) {
  json::Value a = json::Value::array();
  for (const auto& s : v) a.push(s);
  return a;
}

std::string clean_ver(std::string_view s) {
  if (s.empty() || s.size() > 32) return {};
  for (char c : s)
    if (!((c >= '0' && c <= '9') || c == '.')) return {};
  return std::string(s);
}

// ------------------------------------------------------ binary discovery

bool is_exec(const fs::file_status& st) {
  auto p = st.permissions();
  return (p & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) != fs::perms::none;
}

bool skip_dir_name(const std::string& n) {
  return n == "CMakeFiles" || n == "_deps" || n == ".git" || n == "node_modules" || n == "deps" ||
         n == "incremental" || n == "examples" || n == "build";  // cargo: target/<profile>/build
}

void collect_dir(const std::string& root, const fs::path& rel_dir, int max_depth, std::vector<std::string>& out) {
  std::error_code ec;
  const fs::path abs = fs::path(root) / rel_dir;
  if (!fs::is_directory(fs::symlink_status(abs, ec))) return;
  std::size_t budget = 3000;
  fs::recursive_directory_iterator it(abs, fs::directory_options::none, ec), end;
  while (!ec && it != end && budget-- > 0 && out.size() < 32) {
    std::error_code e2;
    const auto st = it->symlink_status(e2);
    if (e2) { it.increment(ec); continue; }
    if (fs::is_directory(st)) {
      if (it.depth() + 1 >= max_depth || skip_dir_name(it->path().filename().string()))
        it.disable_recursion_pending();
    } else if (fs::is_regular_file(st) && is_exec(st)) {
      std::error_code e3;
      const auto size = fs::file_size(it->path(), e3);
      if (!e3 && size >= 64 && size <= (512ull << 20)) {
        std::string rel = fs::relative(it->path(), root, e3).generic_string();
        if (!e3 && is_safe_relpath(rel)) out.push_back(std::move(rel));
      }
    }
    it.increment(ec);
  }
}

std::optional<Binary> inspect_binary(const std::string& root, const std::string& rel, const Redactor& red,
                                     bool require_executable) {
  int fd = open_under(root, rel);
  if (fd < 0) return std::nullopt;
  struct Closer { int fd; ~Closer() { ::close(fd); } } closer{fd};
  char head[64] = {0};
  ssize_t n = ::pread(fd, head, sizeof head, 0);
  if (n < 4) return std::nullopt;
  const FileFormat fmt = detect_format(std::string_view(head, static_cast<std::size_t>(n)));
  Binary b;
  b.path = rel;
  b.format = format_name(fmt);
  if (fmt == FileFormat::Elf) {
    auto info = parse_elf_fd(fd);
    if (!info) return std::nullopt;
    const bool has_interp = !info->interpreter.empty();
    if (require_executable && !has_interp && info->type != 2) return std::nullopt;  // plain shared library
    b.arch = elf_arch_name(info->machine, info->bits, info->little);
    b.bits = info->bits;
    b.little = info->little;
    b.type = info->type == 2 ? "exec" : (has_interp ? "pie" : "dyn");
    b.interpreter = red.apply(sanitize_text(info->interpreter, 128));
    for (auto& s : info->needed)
      if (is_safe_token(s, 128) && b.needed.size() < kMaxNeeded) b.needed.push_back(s);
    for (auto& s : info->runpath) b.runpath.push_back(red.apply(sanitize_text(s, 160)));
    for (auto& vn : info->verneed) {
      if (!is_safe_token(vn.file, 128) || b.verneed.size() >= kMaxNeeded) continue;
      VerNeed v;
      v.file = vn.file;
      for (auto& x : vn.versions)
        if (is_safe_token(x, 64) && v.versions.size() < 256) v.versions.push_back(x);
      b.verneed.push_back(std::move(v));
    }
    return b;
  }
  if (fmt == FileFormat::PE || fmt == FileFormat::MachO) {
    if (fmt == FileFormat::PE) {
      unsigned char pe[8] = {0};
      std::uint32_t lfanew = 0;
      unsigned char lf[4];
      b.arch = "unknown";
      if (::pread(fd, lf, 4, 0x3c) == 4) {
        lfanew = static_cast<std::uint32_t>(lf[0]) | (static_cast<std::uint32_t>(lf[1]) << 8) |
                 (static_cast<std::uint32_t>(lf[2]) << 16) | (static_cast<std::uint32_t>(lf[3]) << 24);
        if (lfanew < (1u << 20) && ::pread(fd, pe, 6, lfanew) == 6 && pe[0] == 'P' && pe[1] == 'E') {
          const unsigned m = pe[4] | (static_cast<unsigned>(pe[5]) << 8);
          b.arch = m == 0x8664 ? "x86_64" : m == 0xAA64 ? "aarch64" : m == 0x14c ? "x86" : "unknown";
        }
      }
    } else {
      b.arch = "unknown";
      unsigned char mh[8];
      if (::pread(fd, mh, 8, 0) == 8) {
        const std::uint32_t cpu = static_cast<std::uint32_t>(mh[4]) | (static_cast<std::uint32_t>(mh[5]) << 8) |
                                  (static_cast<std::uint32_t>(mh[6]) << 16) | (static_cast<std::uint32_t>(mh[7]) << 24);
        b.arch = cpu == 0x01000007u ? "x86_64" : cpu == 0x0100000Cu ? "aarch64" : "unknown";
      }
    }
    b.type = "exec";
    return b;
  }
  return std::nullopt;
}

}  // namespace

std::vector<std::string> discover_binaries(const std::string& root) {
  std::vector<std::string> out;
  // top-level executables
  std::error_code ec;
  for (fs::directory_iterator it(root, ec), end; !ec && it != end && out.size() < 32; it.increment(ec)) {
    std::error_code e2;
    const auto st = it->symlink_status(e2);
    if (!e2 && fs::is_regular_file(st) && is_exec(st)) {
      std::string rel = it->path().filename().string();
      if (is_safe_relpath(rel) && rel.find('.') == std::string::npos) out.push_back(rel);  // skips *.sh, *.py ...
    }
  }
  for (const char* d : {"build", "bin", "out", "dist", "target/release", "target/debug", "cmake-build-release",
                        "cmake-build-debug", "builddir"})
    collect_dir(root, d, 3, out);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// --------------------------------------------------------------- config

ProjectConfig load_config(const std::string& root, std::vector<std::string>& warnings) {
  ProjectConfig cfg;
  auto text = read_file_under(root, kConfigFile, 256 * 1024);
  if (!text) return cfg;
  auto pr = json::parse(*text);
  if (!pr.value || !pr.value->is_object()) {
    warnings.push_back(std::string(kConfigFile) + " is not valid JSON (" + sanitize_text(pr.error, 80) + "), ignoring it");
    return cfg;
  }
  auto strs = [&](const char* key, std::vector<std::string>& out, auto&& valid) {
    const json::Value* v = pr.value->find(key);
    if (!v || !v->is_array()) return;
    for (const auto& e : *v->as_array()) {
      const std::string* s = e.as_string();
      if (!s || out.size() >= 64) continue;
      if (valid(*s)) out.push_back(*s);
      else warnings.push_back(std::string(kConfigFile) + ": ignoring invalid entry in \"" + key + "\": " + sanitize_text(*s, 60));
    }
  };
  strs("binaries", cfg.binaries, [](const std::string& s) { return is_safe_relpath(s); });
  strs("libraries", cfg.libraries, [](const std::string& s) { return is_safe_token(s, 64); });
  strs("tools", cfg.tools, [](const std::string& s) { return find_tool_def(s) != nullptr; });
  cfg.build_hint = sanitize_text(json::get_string(*pr.value, "build_hint"), 200);
  return cfg;
}

std::string make_starter_config(const std::string& root) {
  ScanResult scan = scan_project(root);
  json::Value v = json::Value::object();
  v.set("_note", "Edit me. builtdiff snapshot reads this file. Paths are relative to the project root.");
  v.set("binaries", jstrs(discover_binaries(root)));
  std::vector<std::string> libs;
  for (const auto& l : scan.libs) libs.push_back(l.name);
  v.set("libraries", jstrs({}));
  v.set("tools", jstrs({}));
  v.set("build_hint", "");
  json::Value detected = json::Value::object();
  detected.set("build_systems", jstrs(scan.build.systems));
  detected.set("languages", jstrs(scan.build.languages));
  detected.set("libraries_found_in_build_files", jstrs(libs));
  v.set("_detected", std::move(detected));
  return json::dump(v, 2);
}

// ------------------------------------------------------------ creating

namespace {
std::string detect_primary(const std::string& group, const std::vector<Tool>& tools) {
  const char* env = group == "cxx" ? "CXX" : "CC";
  const char* fallback_cc = group == "cxx" ? "g++" : "gcc";
  auto present = [&](const std::string& n) {
    return std::any_of(tools.begin(), tools.end(), [&](const Tool& t) { return t.name == n; });
  };
  auto classify = [&](std::string name) -> std::string {
    name = to_lower(name);
    if (name.find("clang") != std::string::npos) return group == "cxx" ? "clang++" : "clang";
    if (name.find("g++") != std::string::npos || name.find("gcc") != std::string::npos || name.find("c++") != std::string::npos)
      return group == "cxx" ? "g++" : "gcc";
    return {};
  };
  if (const char* e = std::getenv(env); e && *e) {
    std::string base = fs::path(e).filename().string();  // CXX may be "clang++" or "/usr/bin/clang++ -fsomething"
    base = base.substr(0, base.find(' '));
    if (auto c = classify(base); !c.empty() && present(c)) return c;
  }
  if (auto p = find_in_path(group == "cxx" ? "c++" : "cc")) {
    std::error_code ec;
    auto canon = fs::canonical(*p, ec);
    if (!ec)
      if (auto c = classify(canon.filename().string()); !c.empty() && present(c)) return c;
  }
  if (present(fallback_cc)) return fallback_cc;
  for (const auto& t : tools)
    if (t.group == group) return t.name;
  return {};
}
}  // namespace

CreateResult create_snapshot(const std::string& root, const SnapshotOptions& opts) {
  CreateResult cr;
  Redactor red;
  auto R = [&](std::string_view s, std::size_t n = 200) { return red.apply(sanitize_text(s, n)); };

  ScanResult scan = scan_project(root);
  if (scan.build.systems.empty())
    cr.notes.push_back("No known build files found here (CMakeLists.txt, Makefile, meson.build, Cargo.toml ...). "
                       "Run this from the project root. The snapshot will only describe the platform and any binaries.");

  Snapshot s;
  s.tool_version = kToolVersion;
  s.created = iso_utc_now();
  s.project = R(scan.project_name, 64);
  s.platform = collect_platform();
  s.build = scan.build;
  s.build.build_hint = R(opts.extra.build_hint, 200);

  auto has_sys = [&](const char* n) {
    return std::find(scan.build.systems.begin(), scan.build.systems.end(), n) != scan.build.systems.end();
  };
  auto has_lang = [&](const char* n) {
    return std::find(scan.build.languages.begin(), scan.build.languages.end(), n) != scan.build.languages.end();
  };

  // ---- which tools matter for this project?
  std::vector<std::string> want;
  auto w = [&](const char* n) { add_unique(want, n); };
  if (has_lang("c++")) { w("g++"); w("clang++"); w("ld"); }
  if (has_lang("c") || has_sys("autotools")) { w("gcc"); w("clang"); w("ld"); }
  if (has_sys("cmake")) { w("cmake"); w("make"); w("ninja"); }
  if (has_sys("make")) w("make");
  if (has_sys("meson")) { w("meson"); w("ninja"); }
  if (has_sys("autotools")) w("make");
  if (has_sys("cargo") || has_lang("rust")) { w("cargo"); w("rustc"); }
  if (has_sys("npm")) { w("node"); w("npm"); }
  if (has_sys("maven")) { w("mvn"); w("java"); w("javac"); }
  if (has_sys("gradle")) { w("gradle"); w("java"); w("javac"); }
  if (has_lang("java")) { w("java"); w("javac"); }
  if (has_sys("go") || has_lang("go")) w("go");
  if (has_sys("python")) w("python3");
  bool need_pc = scan.uses_pkgconfig;
  for (const auto& l : scan.libs)
    if (l.kind != "cmake") need_pc = true;
  if (need_pc) w("pkg-config");
  if (scan.build.fetches_network) w("git");
  for (const auto& t : opts.extra.tools)
    if (find_tool_def(t)) w(t.c_str());

  Prober prober;
  auto probes = parallel_map(want, [&](const std::string& n) { return prober.probe_tool(n); });
  for (std::size_t i = 0; i < want.size() && s.tools.size() < kMaxTools; ++i) {
    if (!probes[i].present) continue;
    const ToolDef* def = find_tool_def(want[i]);
    Tool t;
    t.name = want[i];
    t.role = def->role;
    t.group = def->group;
    t.version = probes[i].version;
    t.banner = R(probes[i].banner, 120);
    t.required = want[i] != "ld";
    if (auto it = scan.tool_min.find(t.name); it != scan.tool_min.end()) t.required_min = it->second;
    s.tools.push_back(std::move(t));
  }
  if (has_sys("cmake") && !has_sys("make")) {
    for (auto& t : s.tools)
      if (t.name == "make" || t.name == "ninja") t.group = "generator";
  }
  for (const char* g : {"cxx", "cc"}) {
    std::string prim = detect_primary(g, s.tools);
    for (auto& t : s.tools)
      if (t.group == g) t.primary = (t.name == prim);
  }
  // tools demanded by the project but missing on the dev machine
  for (const auto& [name, ver] : scan.tool_min)
    if (std::none_of(s.tools.begin(), s.tools.end(), [&](const Tool& t) { return t.name == name; }))
      cr.notes.push_back("The project asks for " + name + " >= " + ver + " but " + name + " was not found on this machine.");

  // ---- libraries
  std::vector<LibSpec> specs = scan.libs;
  for (const auto& l : opts.extra.libraries) {
    LibSpec ls;
    ls.name = l;
    ls.kind = "pkg-config";
    auto same = [&](const LibSpec& x) { return iequals(x.name, l); };
    if (std::none_of(specs.begin(), specs.end(), same)) specs.push_back(std::move(ls));
  }
  auto lib_probes = parallel_map(specs, [&](const LibSpec& l) { return prober.probe_library(l.name); });
  for (std::size_t i = 0; i < specs.size() && s.libs.size() < kMaxLibs; ++i) {
    Library l;
    l.name = specs[i].name;
    l.kind = specs[i].kind;
    l.required_min = specs[i].required_min;
    l.required = specs[i].required;
    l.found = lib_probes[i].found;
    l.version = lib_probes[i].version;
    l.via = lib_probes[i].via;
    if (!l.found)
      cr.notes.push_back("Library '" + l.name + "' (" + l.kind + ") was not found on THIS machine - it is recorded "
                         "as a requirement but without a reference version.");
    s.libs.push_back(std::move(l));
  }

  // ---- binaries
  std::vector<std::string> rels = discover_binaries(root);
  for (const auto& b : opts.extra.binaries) add_unique(rels, b);
  for (const auto& rel : rels) {
    if (s.binaries.size() >= kMaxBinaries) break;
    const bool explicit_entry = std::find(opts.extra.binaries.begin(), opts.extra.binaries.end(), rel) != opts.extra.binaries.end();
    auto b = inspect_binary(root, rel, red, !explicit_entry);
    if (b) s.binaries.push_back(std::move(*b));
    else if (explicit_entry) cr.notes.push_back("Binary '" + sanitize_text(rel, 80) + "' could not be read (missing, symlink or unsupported format).");
  }

  s.fingerprint = compute_fingerprint(s);
  cr.snap = std::move(s);
  return cr;
}

// -------------------------------------------------------- serializing

json::Value snapshot_to_json(const Snapshot& s, bool include_fingerprint) {
  using json::Value;
  Value root = Value::object();
  root.set("builtdiff_version", s.format);
  root.set("tool_version", s.tool_version);
  root.set("created", s.created);
  root.set("project", s.project);

  Value p = Value::object();
  p.set("os", s.platform.os);
  p.set("arch", s.platform.arch);
  p.set("distro", s.platform.distro);
  p.set("distro_version", s.platform.distro_version);
  p.set("libc", s.platform.libc);
  p.set("libc_version", s.platform.libc_version);
  root.set("platform", std::move(p));

  Value b = Value::object();
  b.set("systems", jstrs(s.build.systems));
  b.set("languages", jstrs(s.build.languages));
  b.set("cxx_standard", s.build.cxx_standard);
  b.set("c_standard", s.build.c_standard);
  b.set("fetches_network", s.build.fetches_network);
  b.set("build_hint", s.build.build_hint);
  Value files = Value::array();
  for (const auto& f : s.build.files) {
    Value fo = Value::object();
    fo.set("path", f.path);
    fo.set("sha256", f.sha256);
    files.push(std::move(fo));
  }
  b.set("files", std::move(files));
  root.set("build", std::move(b));

  Value tools = Value::array();
  for (const auto& t : s.tools) {
    Value o = Value::object();
    o.set("name", t.name);
    o.set("role", t.role);
    o.set("group", t.group);
    o.set("version", t.version);
    o.set("banner", t.banner);
    o.set("required_min", t.required_min);
    o.set("primary", t.primary);
    o.set("required", t.required);
    tools.push(std::move(o));
  }
  root.set("toolchain", std::move(tools));

  Value libs = Value::array();
  for (const auto& l : s.libs) {
    Value o = Value::object();
    o.set("name", l.name);
    o.set("kind", l.kind);
    o.set("required_min", l.required_min);
    o.set("version", l.version);
    o.set("via", l.via);
    o.set("required", l.required);
    o.set("found", l.found);
    libs.push(std::move(o));
  }
  root.set("libraries", std::move(libs));

  Value bins = Value::array();
  for (const auto& bi : s.binaries) {
    Value o = Value::object();
    o.set("path", bi.path);
    o.set("format", bi.format);
    o.set("arch", bi.arch);
    o.set("bits", bi.bits);
    o.set("endian", bi.little ? "little" : "big");
    o.set("type", bi.type);
    o.set("interpreter", bi.interpreter);
    o.set("needed", jstrs(bi.needed));
    o.set("runpath", jstrs(bi.runpath));
    Value vn = Value::array();
    for (const auto& v : bi.verneed) {
      Value e = Value::object();
      e.set("file", v.file);
      e.set("versions", jstrs(v.versions));
      vn.push(std::move(e));
    }
    o.set("verneed", std::move(vn));
    bins.push(std::move(o));
  }
  root.set("binaries", std::move(bins));
  if (include_fingerprint) root.set("fingerprint", s.fingerprint);
  return root;
}

std::string compute_fingerprint(const Snapshot& s) {
  return sha256_hex(json::dump(snapshot_to_json(s, false), -1));
}

// ------------------------------------------------------------- loading

namespace {

struct Ctx {
  std::vector<std::string>* warnings;
  void warn(std::string m) {
    if (warnings->size() < 50) warnings->push_back(std::move(m));
  }
};

std::string S(const json::Value& o, const char* key, std::size_t maxlen = 128) {
  return sanitize_text(json::get_string(o, key), maxlen);
}

const json::Value::Array* A(const json::Value& o, const char* key) {
  const json::Value* v = o.find(key);
  return v ? v->as_array() : nullptr;
}

std::vector<std::string> token_list(const json::Value& o, const char* key, std::size_t max_items,
                                    std::size_t max_len, Ctx& ctx) {
  std::vector<std::string> out;
  if (const auto* a = A(o, key)) {
    for (const auto& e : *a) {
      const std::string* s = e.as_string();
      if (!s) continue;
      if (out.size() >= max_items) break;
      if (is_safe_token(*s, max_len)) out.push_back(*s);
      else ctx.warn(std::string("ignored suspicious entry in \"") + key + "\": " + sanitize_text(*s, 40));
    }
  }
  return out;
}

}  // namespace

LoadResult parse_snapshot(std::string_view text) {
  LoadResult lr;
  Ctx ctx{&lr.warnings};
  json::ParseOptions po;
  po.max_depth = 16;
  po.max_nodes = 100000;
  po.max_string = 64 * 1024;
  po.max_members = 256;
  auto pr = json::parse(text, po);
  if (!pr.value) {
    lr.error = "not valid JSON: " + sanitize_text(pr.error, 80) + " (offset " + std::to_string(pr.offset) + ")";
    return lr;
  }
  const json::Value& root = *pr.value;
  if (!root.is_object()) { lr.error = "top level is not a JSON object"; return lr; }
  const int ver = static_cast<int>(json::get_number(root, "builtdiff_version", 0));
  if (ver != kSnapshotFormat) {
    lr.error = ver == 0 ? "missing \"builtdiff_version\" - this does not look like a builtdiff snapshot"
                        : "snapshot format version " + std::to_string(ver) + " is not supported by this builtdiff (" +
                              kToolVersion + "); please update builtdiff";
    return lr;
  }

  Snapshot s;
  s.format = ver;
  s.tool_version = S(root, "tool_version", 32);
  s.created = S(root, "created", 40);
  s.project = S(root, "project", 64);
  s.fingerprint = S(root, "fingerprint", 64);

  if (const auto* p = root.find("platform"); p && p->is_object()) {
    s.platform.os = to_lower(S(*p, "os", 32));
    s.platform.arch = normalize_arch(S(*p, "arch", 32));
    s.platform.distro = to_lower(S(*p, "distro", 32));
    s.platform.distro_version = S(*p, "distro_version", 32);
    s.platform.libc = S(*p, "libc", 16);
    s.platform.libc_version = clean_ver(json::get_string(*p, "libc_version"));
  } else {
    lr.error = "missing \"platform\" section";
    return lr;
  }

  if (const auto* b = root.find("build"); b && b->is_object()) {
    s.build.systems = token_list(*b, "systems", 16, 32, ctx);
    s.build.languages = token_list(*b, "languages", 16, 32, ctx);
    s.build.cxx_standard = S(*b, "cxx_standard", 8);
    s.build.c_standard = S(*b, "c_standard", 8);
    s.build.fetches_network = json::get_bool(*b, "fetches_network");
    s.build.build_hint = S(*b, "build_hint", 200);
    if (const auto* files = A(*b, "files")) {
      for (const auto& f : *files) {
        if (!f.is_object() || s.build.files.size() >= kMaxFiles) continue;
        FileHash fh;
        fh.path = json::get_string(f, "path");
        fh.sha256 = json::get_string(f, "sha256");
        const bool hex = fh.sha256.size() == 64 && fh.sha256.find_first_not_of("0123456789abcdef") == std::string::npos;
        if (!is_safe_relpath(fh.path) || !hex) { ctx.warn("ignored malformed build file entry"); continue; }
        s.build.files.push_back(std::move(fh));
      }
    }
  }

  if (const auto* tools = A(root, "toolchain")) {
    for (const auto& t : *tools) {
      if (!t.is_object()) continue;
      if (s.tools.size() >= kMaxTools) { ctx.warn("too many toolchain entries, rest ignored"); break; }
      Tool tool;
      tool.name = json::get_string(t, "name");
      if (!find_tool_def(tool.name)) {
        ctx.warn("ignored unknown tool '" + sanitize_text(tool.name, 40) + "' (builtdiff only ever runs tools from its built-in allowlist)");
        continue;
      }
      tool.role = S(t, "role", 16);
      tool.group = S(t, "group", 16);
      tool.version = clean_ver(json::get_string(t, "version"));
      tool.banner = S(t, "banner", 120);
      tool.required_min = clean_ver(json::get_string(t, "required_min"));
      tool.primary = json::get_bool(t, "primary");
      tool.required = json::get_bool(t, "required", true);
      s.tools.push_back(std::move(tool));
    }
  }

  if (const auto* libs = A(root, "libraries")) {
    for (const auto& l : *libs) {
      if (!l.is_object()) continue;
      if (s.libs.size() >= kMaxLibs) { ctx.warn("too many library entries, rest ignored"); break; }
      Library lib;
      lib.name = json::get_string(l, "name");
      if (!is_safe_token(lib.name, 64)) {
        ctx.warn("ignored library with suspicious name '" + sanitize_text(lib.name, 40) + "'");
        continue;
      }
      lib.kind = S(l, "kind", 16);
      lib.required_min = clean_ver(json::get_string(l, "required_min"));
      lib.version = clean_ver(json::get_string(l, "version"));
      lib.via = S(l, "via", 16);
      lib.required = json::get_bool(l, "required", true);
      lib.found = json::get_bool(l, "found");
      s.libs.push_back(std::move(lib));
    }
  }

  if (const auto* bins = A(root, "binaries")) {
    for (const auto& b : *bins) {
      if (!b.is_object()) continue;
      if (s.binaries.size() >= kMaxBinaries) { ctx.warn("too many binaries, rest ignored"); break; }
      Binary bin;
      bin.path = json::get_string(b, "path");
      if (!is_safe_relpath(bin.path)) {
        ctx.warn("ignored binary with unsafe path '" + sanitize_text(bin.path, 40) + "'");
        continue;
      }
      bin.format = S(b, "format", 8);
      bin.arch = normalize_arch(S(b, "arch", 32));
      bin.bits = static_cast<int>(json::get_number(b, "bits", 0));
      bin.little = json::get_string(b, "endian") != "big";
      bin.type = S(b, "type", 8);
      bin.interpreter = S(b, "interpreter", 128);
      bin.needed = token_list(b, "needed", kMaxNeeded, 128, ctx);
      if (const auto* rp = A(b, "runpath"))
        for (const auto& e : *rp)
          if (const std::string* str = e.as_string(); str && bin.runpath.size() < 64) bin.runpath.push_back(sanitize_text(*str, 160));
      if (const auto* vn = A(b, "verneed")) {
        for (const auto& e : *vn) {
          if (!e.is_object() || bin.verneed.size() >= kMaxNeeded) continue;
          VerNeed v;
          v.file = json::get_string(e, "file");
          if (!is_safe_token(v.file, 128)) continue;
          v.versions = token_list(e, "versions", 256, 64, ctx);
          bin.verneed.push_back(std::move(v));
        }
      }
      s.binaries.push_back(std::move(bin));
    }
  }

  s.fingerprint_ok = !s.fingerprint.empty() && compute_fingerprint(s) == s.fingerprint;
  if (!s.fingerprint_ok)
    ctx.warn("the snapshot's checksum does not match its content - it was edited by hand, damaged, or contained "
             "entries builtdiff refused to accept");
  lr.snap = std::move(s);
  return lr;
}

LoadResult load_snapshot_file(const std::string& path) {
  LoadResult lr;
  auto text = read_file(path, kMaxSnapshotBytes);
  if (!text) {
    lr.error = "cannot read '" + sanitize_text(path, 120) + "' (missing, not a regular file, or larger than 4 MiB)";
    return lr;
  }
  return parse_snapshot(*text);
}

std::optional<std::string> find_snapshot_file(const std::string& start) {
  fs::path dir = start;
  for (int depth = 0; depth < 12; ++depth) {
    const fs::path cand = dir / kSnapshotFile;
    struct stat st {};
    if (::stat(cand.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return cand.string();
    std::error_code ec;
    if (fs::exists(dir / ".git", ec)) return std::nullopt;
    fs::path parent = dir.parent_path();
    if (parent == dir || parent.empty()) return std::nullopt;
    dir = parent;
  }
  return std::nullopt;
}

}  // namespace bd
