#include "scan.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include "json.hpp"
#include "knowledge.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

namespace bd {

namespace {

constexpr std::size_t kMaxBuildFile = 2u << 20;  // 2 MiB
constexpr std::size_t kMaxCalls = 20000;
constexpr std::size_t kMaxArgs = 512;
constexpr std::size_t kMaxLibs = 256;

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool is_ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool is_ident(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

void add_unique(std::vector<std::string>& v, const std::string& s) {
  if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
}

void add_lib(ScanResult& r, LibSpec spec) {
  if (r.libs.size() >= kMaxLibs) return;
  if (!is_safe_token(spec.name, 64)) return;
  const std::string key = to_lower(spec.name);
  for (auto& l : r.libs) {
    if (to_lower(l.name) == key) {
      if (l.required_min.empty()) l.required_min = spec.required_min;
      l.required = l.required || spec.required;
      return;
    }
  }
  r.libs.push_back(std::move(spec));
}

std::string clean_version(std::string_view v) {
  if (auto pos = v.find("..."); pos != std::string_view::npos) v = v.substr(0, pos);
  if (auto ver = parse_version(v)) return version_string(*ver);
  return {};
}

}  // namespace

std::string normalize_cxx_standard(std::string_view s) {
  s = trim(s);
  for (const char* p : {"gnu++", "c++", "cxx_std_", "gnu", "c"}) {
    std::string_view pre(p);
    if (s.substr(0, pre.size()) == pre) { s.remove_prefix(pre.size()); break; }
  }
  if (s == "2b" || s == "23") return "23";
  if (s == "2a" || s == "20") return "20";
  if (s == "2c" || s == "26") return "26";
  if (s == "17" || s == "1z") return "17";
  if (s == "14" || s == "1y") return "14";
  if (s == "11" || s == "0x") return "11";
  if (s == "98") return "98";
  return {};
}

// ------------------------------------------------------- CMake tokenizer

std::vector<CMakeCall> parse_cmake_calls(std::string_view t) {
  std::vector<CMakeCall> calls;
  const std::size_t n = t.size();
  std::size_t i = 0;
  while (i < n && calls.size() < kMaxCalls) {
    const char c = t[i];
    if (c == '#') {
      if (t.substr(i, 3) == "#[[") {
        auto e = t.find("]]", i + 3);
        i = e == std::string_view::npos ? n : e + 2;
      } else {
        while (i < n && t[i] != '\n') ++i;
      }
      continue;
    }
    if (c == '"') {  // stray quoted string at top level
      ++i;
      while (i < n && t[i] != '"') i += (t[i] == '\\' && i + 1 < n) ? 2 : 1;
      ++i;
      continue;
    }
    if (!is_ident_start(c)) { ++i; continue; }
    std::size_t s = i;
    while (i < n && is_ident(t[i])) ++i;
    std::string name = to_lower(t.substr(s, i - s));
    std::size_t j = i;
    while (j < n && is_ws(t[j])) ++j;
    if (j >= n || t[j] != '(') continue;
    // parse arguments
    CMakeCall call;
    call.name = std::move(name);
    ++j;
    int depth = 1;
    while (j < n && depth > 0) {
      const char d = t[j];
      if (is_ws(d)) { ++j; continue; }
      if (d == '#') {
        if (t.substr(j, 3) == "#[[") {
          auto e = t.find("]]", j + 3);
          j = e == std::string_view::npos ? n : e + 2;
        } else {
          while (j < n && t[j] != '\n') ++j;
        }
        continue;
      }
      if (d == '(') { ++depth; ++j; continue; }
      if (d == ')') { --depth; ++j; continue; }
      std::string arg;
      if (d == '"') {
        ++j;
        while (j < n && t[j] != '"') {
          if (t[j] == '\\' && j + 1 < n) ++j;
          if (arg.size() < 1024) arg.push_back(t[j]);
          ++j;
        }
        ++j;
      } else {
        while (j < n && !is_ws(t[j]) && t[j] != '(' && t[j] != ')' && t[j] != '#' && t[j] != '"') {
          if (arg.size() < 1024) arg.push_back(t[j]);
          ++j;
        }
      }
      if (call.args.size() < kMaxArgs) call.args.push_back(std::move(arg));
    }
    i = j;
    calls.push_back(std::move(call));
  }
  return calls;
}

namespace {

bool arg_is(const std::string& a, const char* kw) { return iequals(a, kw); }
bool has_arg(const CMakeCall& c, const char* kw) {
  for (const auto& a : c.args)
    if (arg_is(a, kw)) return true;
  return false;
}

void scan_cmake(const std::string& text, ScanResult& r) {
  for (const auto& c : parse_cmake_calls(text)) {
    const auto& a = c.args;
    if (c.name == "cmake_minimum_required") {
      for (std::size_t k = 0; k + 1 < a.size(); ++k)
        if (arg_is(a[k], "VERSION")) {
          if (auto v = clean_version(a[k + 1]); !v.empty()) r.tool_min["cmake"] = v;
        }
    } else if (c.name == "project") {
      if (!a.empty() && a[0].find('$') == std::string::npos && r.project_name.empty())
        r.project_name = sanitize_text(a[0], 64);
    } else if (c.name == "find_package") {
      if (a.empty() || a[0].find('$') != std::string::npos) continue;
      if (iequals(a[0], "PkgConfig")) r.uses_pkgconfig = true;
      if (is_ignored_cmake_package(a[0])) continue;
      LibSpec s;
      s.name = a[0];
      s.kind = "cmake";
      if (a.size() > 1 && !a[1].empty() && std::isdigit(static_cast<unsigned char>(a[1][0])))
        s.required_min = clean_version(a[1]);
      s.required = has_arg(c, "REQUIRED");
      add_lib(r, std::move(s));
    } else if (c.name == "pkg_check_modules" || c.name == "pkg_search_module" || c.name == "pkg_search_modules") {
      r.uses_pkgconfig = true;
      const bool required = has_arg(c, "REQUIRED");
      const bool search = c.name != "pkg_check_modules";
      LibSpec* last = nullptr;
      std::vector<LibSpec> specs;
      for (std::size_t k = 1; k < a.size(); ++k) {
        const std::string& tok = a[k];
        if (tok.empty() || tok.find('$') != std::string::npos) continue;
        if (arg_is(tok, "REQUIRED") || arg_is(tok, "QUIET") || arg_is(tok, "IMPORTED_TARGET") ||
            arg_is(tok, "GLOBAL") || arg_is(tok, "NO_CMAKE_PATH") || arg_is(tok, "NO_CMAKE_ENVIRONMENT_PATH") ||
            arg_is(tok, "NO_CMAKE_SYSTEM_PATH"))
          continue;
        if (tok == ">=" || tok == "=" || tok == "<=" || tok == "<" || tok == ">") {
          if (last && k + 1 < a.size() && (tok == ">=" || tok == "=")) last->required_min = clean_version(a[k + 1]);
          ++k;
          continue;
        }
        const auto op = tok.find_first_of("<>=");
        LibSpec s;
        s.name = tok.substr(0, op);
        s.kind = "pkg-config";
        s.required = required && !search;
        if (op != std::string::npos && tok.compare(op, 2, ">=") == 0) s.required_min = clean_version(tok.substr(op + 2));
        else if (op != std::string::npos && tok[op] == '=') s.required_min = clean_version(tok.substr(op + 1));
        specs.push_back(std::move(s));
        last = &specs.back();
      }
      if (search && !specs.empty() && required) specs.front().required = true;
      for (auto& s : specs) add_lib(r, std::move(s));
    } else if (c.name == "set") {
      if (a.size() >= 2 && a[0] == "CMAKE_CXX_STANDARD") {
        if (auto s = normalize_cxx_standard(a[1]); !s.empty()) r.build.cxx_standard = s;
      } else if (a.size() >= 2 && a[0] == "CMAKE_C_STANDARD") {
        if (auto s = normalize_cxx_standard(a[1]); !s.empty()) r.build.c_standard = s;
      }
    } else if (c.name == "target_compile_features") {
      for (const auto& x : a)
        if (x.rfind("cxx_std_", 0) == 0) {
          if (auto s = normalize_cxx_standard(x); !s.empty()) r.build.cxx_standard = s;
        }
    } else if (c.name == "fetchcontent_declare" || c.name == "externalproject_add") {
      r.build.fetches_network = true;
    } else if (c.name == "include") {
      if (!a.empty() && iequals(a[0], "FindPkgConfig")) r.uses_pkgconfig = true;
    }
    // -std=... anywhere in compile options
    if (c.name == "add_compile_options" || c.name == "target_compile_options" || c.name == "set") {
      for (const auto& x : a) {
        auto pos = x.find("-std=");
        if (pos == std::string::npos) pos = x.find("/std:");
        if (pos != std::string::npos) {
          if (auto s = normalize_cxx_standard(x.substr(pos + 5)); !s.empty()) r.build.cxx_standard = s;
        }
      }
    }
  }
}

void scan_makefile(const std::string& text, ScanResult& r) {
  for (const auto& raw : split(text, '\n')) {
    std::string_view line = trim(raw);
    if (line.empty() || line.front() == '#') continue;
    if (auto pos = line.find("-std="); pos != std::string_view::npos) {
      std::size_t e = pos + 5;
      while (e < line.size() && !is_ws(line[e])) ++e;
      if (auto s = normalize_cxx_standard(line.substr(pos + 5, e - pos - 5)); !s.empty()) r.build.cxx_standard = s;
    }
    const bool pc = line.find("pkg-config") != std::string_view::npos || line.find("pkgconf") != std::string_view::npos;
    if (pc) r.uses_pkgconfig = true;
    for (const auto& tok : split(line, ' ')) {
      if (tok.rfind("-l", 0) == 0 && tok.size() > 2) {
        std::string lib = tok.substr(2);
        if (is_base_link_lib(lib)) continue;
        LibSpec s;
        s.name = lib;
        s.kind = "make";
        add_lib(r, std::move(s));
      } else if (pc) {
        if (tok.front() == '-' || tok.front() == '$' || tok.front() == '`' || tok.front() == '(' ||
            tok.front() == '@' || tok.front() == '\t' || tok.find('=') != std::string::npos)
          continue;
        if (tok == "pkg-config" || tok == "pkgconf" || tok == "shell" || tok == "$(shell") continue;
        if (!is_safe_token(tok, 64) || tok.find_first_not_of("0123456789.") == std::string::npos) continue;
        if (tok == "pkg-config" || tok.rfind("pkg-config", 0) == 0) continue;
        // Skip obvious make syntax words
        if (tok == "echo" || tok == "if" || tok == "then" || tok == "else" || tok == "fi" || tok == "shell") continue;
        LibSpec s;
        s.name = tok;
        s.kind = "pkg-config";
        add_lib(r, std::move(s));
      }
    }
  }
}

void scan_meson(const std::string& text, ScanResult& r) {
  std::size_t pos = 0;
  while ((pos = text.find("dependency(", pos)) != std::string::npos) {
    pos += 11;
    const std::size_t close = text.find(')', pos);
    const std::size_t end = close == std::string::npos ? std::min(text.size(), pos + 256) : close;
    const std::string_view call(text.data() + pos, end - pos);
    auto q = call.find_first_of("'\"");
    if (q == std::string_view::npos) continue;
    auto q2 = call.find(call[q], q + 1);
    if (q2 == std::string_view::npos) continue;
    LibSpec s;
    s.name = std::string(call.substr(q + 1, q2 - q - 1));
    s.kind = "meson";
    auto rq = call.find("required");
    if (rq != std::string_view::npos && call.find("false", rq) != std::string_view::npos) s.required = false;
    if (auto vq = call.find("version"); vq != std::string_view::npos) {
      auto ge = call.find(">=", vq);
      if (ge != std::string_view::npos) s.required_min = clean_version(call.substr(ge + 2, 24));
    }
    add_lib(r, std::move(s));
  }
  if (auto mv = text.find("meson_version"); mv != std::string::npos) {
    auto ge = text.find(">=", mv);
    if (ge != std::string::npos && ge - mv < 40)
      if (auto v = clean_version(std::string_view(text).substr(ge + 2, 24)); !v.empty()) r.tool_min["meson"] = v;
  }
  if (auto cs = text.find("cpp_std="); cs != std::string::npos) {
    std::size_t e = cs + 8;
    while (e < text.size() && (std::isalnum(static_cast<unsigned char>(text[e])) || text[e] == '+')) ++e;
    if (auto s = normalize_cxx_standard(std::string_view(text).substr(cs + 8, e - cs - 8)); !s.empty())
      r.build.cxx_standard = s;
  }
  if (auto pn = text.find("project("); pn != std::string::npos && r.project_name.empty()) {
    auto q = text.find_first_of("'\"", pn);
    if (q != std::string::npos) {
      auto q2 = text.find(text[q], q + 1);
      if (q2 != std::string::npos && q2 - q < 70) r.project_name = sanitize_text(text.substr(q + 1, q2 - q - 1), 64);
    }
  }
}

std::string toml_string_value(std::string_view line) {
  auto q = line.find('"');
  if (q == std::string_view::npos) return {};
  auto q2 = line.find('"', q + 1);
  if (q2 == std::string_view::npos) return {};
  return std::string(line.substr(q + 1, q2 - q - 1));
}

void scan_cargo(const std::string& text, ScanResult& r) {
  bool in_package = false;
  for (const auto& raw : split(text, '\n')) {
    std::string_view line = trim(raw);
    if (line.empty() || line.front() == '#') continue;
    if (line.front() == '[') { in_package = line == "[package]"; continue; }
    if (!in_package) continue;
    if (line.rfind("name", 0) == 0 && r.project_name.empty()) r.project_name = sanitize_text(toml_string_value(line), 64);
    if (line.rfind("rust-version", 0) == 0)
      if (auto v = clean_version(toml_string_value(line)); !v.empty()) r.tool_min["rustc"] = v;
  }
  // Crate names and their caret constraints. The workspace members themselves
  // live in [package] and are not dependencies, so parse_cargo_deps's
  // section scoping keeps them out.
  merge_specs(r.packages, parse_cargo_deps(text));
}

void scan_package_json(const std::string& text, ScanResult& r) {
  auto pr = json::parse(text);
  if (!pr.value || !pr.value->is_object()) return;
  if (r.project_name.empty()) r.project_name = sanitize_text(json::get_string(*pr.value, "name"), 64);
  if (const auto* eng = pr.value->find("engines"); eng && eng->is_object()) {
    std::string node = json::get_string(*eng, "node");
    std::string_view nv = trim(node);
    if (nv.rfind(">=", 0) == 0 || nv.rfind('^', 0) == 0 || nv.rfind('~', 0) == 0 || (!nv.empty() && std::isdigit(static_cast<unsigned char>(nv[0]))))
      if (auto v = clean_version(nv); !v.empty()) r.tool_min["node"] = v;
  }
  merge_specs(r.packages, parse_package_json_deps(text));
}

void scan_pyproject(const std::string& text, ScanResult& r) {
  for (const auto& raw : split(text, '\n')) {
    std::string_view line = trim(raw);
    if (line.rfind("requires-python", 0) == 0) {
      auto v = toml_string_value(line);
      if (v.rfind(">=", 0) == 0)
        if (auto cv = clean_version(v); !cv.empty()) r.tool_min["python3"] = cv;
    }
    if (line.rfind("name", 0) == 0 && r.project_name.empty() && line.find('=') != std::string_view::npos)
      r.project_name = sanitize_text(toml_string_value(line), 64);
  }
  merge_specs(r.packages, parse_pyproject_deps(text));
}

void scan_requirements(const std::string& text, ScanResult& r) {
  merge_specs(r.packages, parse_requirements(text));
}

void scan_setup_py(const std::string& text, ScanResult& r) {
  merge_specs(r.packages, parse_setup_py_deps(text));
}

void scan_pom(const std::string& text, ScanResult& r) {
  merge_specs(r.packages, parse_pom_deps(text));
}

void scan_gradle(const std::string& text, ScanResult& r) {
  merge_specs(r.packages, parse_gradle_deps(text));
}

void scan_go_mod(const std::string& text, ScanResult& r) {
  for (const auto& raw : split(text, '\n')) {
    std::string_view line = trim(raw);
    if (line.rfind("go ", 0) == 0)
      if (auto v = clean_version(line.substr(3)); !v.empty()) r.tool_min["go"] = v;
    if (line.rfind("module ", 0) == 0 && r.project_name.empty()) {
      std::string m(trim(line.substr(7)));
      auto slash = m.rfind('/');
      r.project_name = sanitize_text(slash == std::string::npos ? m : m.substr(slash + 1), 64);
    }
  }
  merge_specs(r.packages, parse_go_mod_requires(text));
}

void detect_languages(const std::string& root, ScanResult& r) {
  static const char* const skip[] = {".git", "build", "node_modules", "target", "third_party", "vendor",
                                     ".cache", "external", "extern", "deps", "out", "dist"};
  std::size_t cxx = 0, c = 0, rs = 0, go = 0, py = 0, java = 0, js = 0;
  std::error_code ec;
  std::size_t budget = 4000;
  fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end;
  while (!ec && it != end && budget-- > 0) {
    const auto& p = it->path();
    std::error_code ec2;
    if (it->is_directory(ec2)) {
      const std::string fn = p.filename().string();
      if (it.depth() >= 3 || std::find_if(std::begin(skip), std::end(skip), [&](const char* s) { return fn == s; }) != std::end(skip))
        it.disable_recursion_pending();
    } else if (it->is_regular_file(ec2)) {
      const std::string ext = to_lower(p.extension().string());
      if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".hpp" || ext == ".hh" || ext == ".hxx" || ext == ".cppm") ++cxx;
      else if (ext == ".c") ++c;
      else if (ext == ".rs") ++rs;
      else if (ext == ".go") ++go;
      else if (ext == ".py") ++py;
      else if (ext == ".java") ++java;
      else if (ext == ".js" || ext == ".ts" || ext == ".mjs") ++js;
    }
    it.increment(ec);
  }
  if (cxx) add_unique(r.build.languages, "c++");
  if (c) add_unique(r.build.languages, "c");
  if (rs) add_unique(r.build.languages, "rust");
  if (go) add_unique(r.build.languages, "go");
  if (py) add_unique(r.build.languages, "python");
  if (java) add_unique(r.build.languages, "java");
  if (js) add_unique(r.build.languages, "javascript");
}

}  // namespace

ScanResult scan_project(const std::string& root) {
  ScanResult r;
  struct Entry {
    const char* file;
    const char* system;
    void (*fn)(const std::string&, ScanResult&);
  };
  static const Entry entries[] = {
      {"CMakeLists.txt", "cmake", scan_cmake},
      {"Makefile", "make", scan_makefile},
      {"makefile", "make", scan_makefile},
      {"GNUmakefile", "make", scan_makefile},
      {"meson.build", "meson", scan_meson},
      {"Cargo.toml", "cargo", scan_cargo},
      {"package.json", "npm", scan_package_json},
      {"pom.xml", "maven", scan_pom},
      {"build.gradle", "gradle", scan_gradle},
      {"build.gradle.kts", "gradle", scan_gradle},
      {"go.mod", "go", scan_go_mod},
      {"go.sum", "go", nullptr},
      {"pyproject.toml", "python", scan_pyproject},
      {"setup.py", "python", scan_setup_py},
      {"requirements.txt", "python", scan_requirements},
      {"configure.ac", "autotools", nullptr},
  };
  for (const auto& e : entries) {
    auto text = read_file_under(root, e.file, kMaxBuildFile);
    if (!text) continue;
    add_unique(r.build.systems, e.system);
    r.build.files.push_back({e.file, sha256_hex(*text)});
    if (e.fn) e.fn(*text, r);
  }
  // A lock file pins exact versions, which is strictly better information than
  // the manifest ranges. Read it after the manifests so it wins, and only keep
  // names that are actually declared dependencies: Cargo.lock and go.sum also
  // list transitive crates and modules that the project never names directly,
  // and reporting those as requirements would bury the real ones.
  if (auto lock = read_file_under(root, "Cargo.lock", kMaxBuildFile)) {
    std::vector<PkgSpec> pinned;
    for (auto& p : parse_cargo_lock(*lock)) {
      const bool declared = std::any_of(r.packages.begin(), r.packages.end(), [&](const PkgSpec& d) {
        return d.kind == "cargo" && d.name == p.name;
      });
      if (!declared) continue;
      p.spec.clear();
      p.required_min = p.version_locked;  // the lock is exact, so use it as the bound
      pinned.push_back(std::move(p));
    }
    merge_specs(r.packages, pinned);
  }
  if (auto sum = read_file_under(root, "go.sum", kMaxBuildFile)) {
    // go.sum lists a module once per dependency edge plus its /go.mod hash,
    // and often holds several versions of one module. parse_go_sum() collapses
    // that to module -> highest version and matches the module path exactly, so
    // this no longer has to guess which line belongs to which dependency.
    const std::map<std::string, std::string> pinned_by_mod = parse_go_sum(*sum);
    for (auto& d : r.packages) {
      if (d.kind != "go") continue;
      const auto it = pinned_by_mod.find(d.name);
      if (it == pinned_by_mod.end()) continue;
      d.version_locked = it->second;
    }
  }

  // Record which ecosystems this snapshot carries so `check` can tell a
  // native-only project from one that also has packages.
  for (const auto& p : r.packages) add_unique(r.build.package_managers, p.kind);

  if (r.project_name.empty()) {
    std::error_code ec;
    r.project_name = sanitize_text(fs::path(root).filename().string(), 64);
    if (r.project_name.empty() || r.project_name == ".") r.project_name = "project";
  }
  detect_languages(root, r);
  // CMake / Make / Meson projects that never mention a language file still need a C/C++ compiler
  if (r.build.languages.empty() && (std::find(r.build.systems.begin(), r.build.systems.end(), "cmake") != r.build.systems.end()))
    r.build.languages.push_back("c++");
  return r;
}

}  // namespace bd
