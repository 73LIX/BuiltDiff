#include "ecosystems.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <ranges>

#include "json.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

namespace bd {

// ------------------------------------------------------------------ basics

Eco eco_from_kind(std::string_view kind) noexcept {
  if (iequals(kind, "pip")) return Eco::Pip;
  if (iequals(kind, "npm")) return Eco::Npm;
  if (iequals(kind, "cargo")) return Eco::Cargo;
  if (iequals(kind, "maven")) return Eco::Maven;
  if (iequals(kind, "go")) return Eco::Go;
  return Eco::Native;
}

const char* eco_name(Eco e) noexcept {
  switch (e) {
    case Eco::Pip: return "pip";
    case Eco::Npm: return "npm";
    case Eco::Cargo: return "cargo";
    case Eco::Maven: return "maven";
    case Eco::Go: return "go";
    case Eco::Native: break;
  }
  return "native";
}

const char* eco_label(Eco e) noexcept {
  switch (e) {
    case Eco::Pip: return "Python package";
    case Eco::Npm: return "npm package";
    case Eco::Cargo: return "Rust crate";
    case Eco::Maven: return "Java library";
    case Eco::Go: return "Go module";
    case Eco::Native: break;
  }
  return "library";
}

bool eco_scans_home(Eco e) noexcept { return e == Eco::Cargo || e == Eco::Maven || e == Eco::Go; }

std::string normalize_pep503(std::string_view name) {
  // PEP 503: collapse every run of [-_.] into a single '-', then lowercase.
  // A leading or trailing separator is NOT stripped - it produces a name that
  // fails the spec's own validity check, so we return empty instead. Silently
  // repairing it would make "-bad-" and "bad" look like the same package.
  std::string out;
  out.reserve(name.size());
  for (std::size_t i = 0; i < name.size(); ++i) {
    const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
    if (l == '-' || l == '_' || l == '.') {
      if (!out.empty() && out.back() != '-') out.push_back('-');
      continue;
    }
    if (std::isalnum(static_cast<unsigned char>(l)) == 0) return {};
    out.push_back(l);
  }
  if (out.empty() || out.front() == '-' || out.back() == '-') return {};
  return out;
}

namespace {
bool is_npm_bare(std::string_view s) noexcept {
  if (s.empty() || s.size() > 128 || s.front() == '-' || s.front() == '.') return false;
  for (char c : s) {
    const bool ok = std::islower(static_cast<unsigned char>(c)) || std::isupper(static_cast<unsigned char>(c)) ||
                    std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}
}  // namespace

bool is_npm_package_name(std::string_view name) noexcept {
  if (name.empty() || name.size() > 128) return false;
  if (name.front() != '@') return is_npm_bare(name);
  const auto slash = name.find('/');
  if (slash == std::string_view::npos || slash == 1 || slash + 1 >= name.size()) return false;
  return is_npm_bare(name.substr(1, slash - 1)) && is_npm_bare(name.substr(slash + 1));
}

bool is_eco_package_name(Eco e, std::string_view name) noexcept {
  switch (e) {
    case Eco::Npm: return is_npm_package_name(name);
    case Eco::Pip: return !normalize_pep503(name).empty() && name.size() <= 128;
    case Eco::Maven: {
      if (name.empty() || name.size() > 128) return false;
      for (char c : name)
        if (!(std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_' || c == '-')) return false;
      return name.front() != '-' && name.back() != '.';
    }
    // "Native" is not a package manager, so this predicate is meaningless for
    // it. Native libraries are validated by is_safe_token instead. Answering
    // true here would let a caller mistake "any string" for "a safe package
    // name" - which is exactly the confusion that produced a path traversal.
    case Eco::Native: return false;
    case Eco::Cargo: {
      // Crates.io names are a single path segment: [A-Za-z0-9_-] with a leading
      // '_' or alphanumeric. No '/' and no '.' at all, so "../evil" and "a/b"
      // can never reach a filesystem path built from this.
      if (name.empty() || name.size() > 64) return false;
      const char c0 = name.front();
      if (!(std::isalnum(static_cast<unsigned char>(c0)) != 0 || c0 == '_')) return false;
      for (char c : name)
        if (!(std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-')) return false;
      return true;
    }
    case Eco::Go:
      break;
  }
  // Go module paths legitimately contain '/', so the path-shaped names have to
  // be checked rather than the charset alone: no leading '/', no empty
  // component, and no "." or ".." component that could escape a cache root.
  if (name.empty() || name.size() > 128) return false;
  for (char c : name)
    if (!(std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_' || c == '-' ||
          c == '+' || c == '/' || c == '~'))
      return false;
  if (name.front() == '/' || name.back() == '/') return false;
  std::size_t start = 0;
  while (start <= name.size()) {
    const std::size_t slash = name.find('/', start);
    const std::string_view comp = name.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
    if (comp.empty() || comp == "." || comp == "..") return false;
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return true;
}

bool is_maven_coordinate(std::string_view name) noexcept {
  const std::size_t colon = name.find(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 >= name.size()) return false;
  if (name.find(':', colon + 1) != std::string_view::npos) return false;
  return is_eco_package_name(Eco::Maven, name.substr(0, colon)) &&
         is_eco_package_name(Eco::Maven, name.substr(colon + 1));
}

// ------------------------------------------------------------- specifiers

namespace {

std::vector<std::string> split_top_level(std::string_view s, char sep) {
  // Splits on `sep` but not inside (), [], {}, "" or ''.
  std::vector<std::string> out;
  int depth = 0;
  std::string cur;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '"' || c == '\'') {
      const char q = c;
      if (cur.size() < 512) cur.push_back(c);
      while (++i < s.size()) {
        if (cur.size() < 512) cur.push_back(s[i]);
        if (s[i] == q) break;
      }
      continue;
    }
    if (c == '(' || c == '[' || c == '{') ++depth;
    else if (c == ')' || c == ']' || c == '}') --depth;
    if (c == sep && depth <= 0) {
      if (!trim(cur).empty()) out.push_back(sanitize_text(trim(cur), 128));
      cur.clear();
      continue;
    }
    if (cur.size() < 512) cur.push_back(c);
  }
  if (!trim(cur).empty()) out.push_back(sanitize_text(trim(cur), 128));
  return out;
}

// A version we can actually compare: dotted numbers, optional leading "v".
std::optional<Version> parse_pkg_version(std::string_view v) {
  v = trim(v);
  if (v.empty()) return std::nullopt;
  if (v.front() == 'v' || v.front() == 'V') v.remove_prefix(1);
  // Strip a PEP 440 local/pre/post suffix: 1.2.3rc1, 1.2.3.post1, 1.0+local
  std::size_t end = 0;
  while (end < v.size() && (std::isdigit(static_cast<unsigned char>(v[end])) != 0 || v[end] == '.')) ++end;
  if (end == 0) return std::nullopt;
  return parse_version(v.substr(0, end));
}

// A single comparator. Exclusive and inclusive bounds are distinct: '>' and
// 'g' (>=) are lower bounds, '<' and 'l' (<=) are upper bounds, and '=' means
// exact equality, which is neither. Collapsing '>=' into '=' would make every
// range reject everything above its floor, so the four cases stay separate.
struct Bound {
  char op = 0;  // '>' | 'g' | '<' | 'l' | '='
  Version ver;
};

bool bound_is_lower(const Bound& b) noexcept { return b.op == '>' || b.op == 'g'; }

bool cmp_bound(const Version& have, const Bound& b) {
  const int c = compare_versions(have, b.ver);
  switch (b.op) {
    case '>': return c > 0;
    case 'g': return c >= 0;
    case '<': return c < 0;
    case 'l': return c <= 0;
    default: return c == 0;
  }
}

// ^1.2.3 -> >=1.2.3 <2.0.0 ; ^0.2.3 -> >=0.2.3 <0.3.0
std::pair<Bound, Bound> caret_range(Version v) {
  Bound lo{'g', v};
  Bound hi{'<', v};
  const std::uint64_t major = v.major();
  std::size_t first_diff = v.parts.size();
  for (std::size_t i = 1; i < v.parts.size(); ++i)
    if (v.parts[i] != 0) { first_diff = i; break; }
  if (first_diff == v.parts.size()) first_diff = v.parts.size() == 1 ? 0 : 1;
  if (major != 0) {
    // ^1.2.3 -> >=1.2.3 <2
    hi.ver.parts.clear();
    hi.ver.parts.push_back(major + 1);
  } else {
    // A leading 0 pins that component, so the bump happens at the first
    // non-zero one: ^0.5 -> <0.6, ^0.0.3 -> <0.0.4, ^0 -> <1.
    hi.ver.parts.resize(first_diff + 1);
    hi.ver.parts[first_diff] = v.parts[first_diff] + 1;
  }
  return {lo, hi};
}

// ~1.2.3 -> >=1.2.3 <1.3.0 ; ~1.2 -> >=1.2 <1.3 ; ~1 -> >=1 <2
std::pair<Bound, Bound> tilde_range(Version v, std::size_t given) {
  Bound lo{'g', v};
  Bound hi{'<', v};
  if (given <= 1) {
    hi.ver.parts.clear();
    hi.ver.parts.push_back(v.major() + 1);
  } else {
    hi.ver.parts.resize(2);
    hi.ver.parts[0] = v.major();
    hi.ver.parts[1] = v.parts.size() > 1 ? v.parts[1] : 0;
    hi.ver.parts[1] += 1;
  }
  return {lo, hi};
}

// "1.x" / "1.2.*" -> the numbers that were given, with a wildcard tail.
std::pair<Bound, Bound> wildcard_range(std::string_view s) {
  Bound lo{'g', Version{}};
  Bound hi{'<', Version{}};
  Version given;
  for (const auto& part : split(s, '.')) {
    if (part.find_first_of("xX*") != std::string::npos) break;
    if (auto v = parse_version(part)) given.parts.push_back(v->parts.empty() ? 0 : v->parts[0]);
  }
  if (given.parts.empty()) return {lo, hi};  // "*" - anything
  lo = Bound{'g', given};
  // Bump the last stated component: "1.2.x" is >=1.2.0 <1.3.0, so the ceiling
  // is the next minor, not 1.2.1.
  Version upper = given;
  upper.parts.back() += 1;
  hi = Bound{'<', upper};
  return {lo, hi};
}

struct Range {
  std::vector<Bound> bounds;  // all must hold
  bool unbounded = true;      // true => no constraint at all (empty spec, "*")
  bool unreadable = false;    // true => a constraint was given but not understood
};

Range parse_range(Eco e, std::string_view spec) {
  Range r;
  std::string_view s = trim(spec);
  if (s.empty() || s == "*" || iequals(s, "latest") || s == "x" || s == "X") return r;
  // From here on a constraint exists. If we end up with no usable bounds the
  // answer is Unknown, not Ok: a git URL or a workspace protocol is not a
  // version range, and silently treating it as unconstrained would report a
  // dependency as satisfied when we never actually checked it.
  r.unbounded = false;

  if (e == Eco::Maven && (s.front() == '[' || s.front() == '(')) {  // [1.0,2.0)
    const bool inc_lo = s.front() == '[';
    std::string_view body = s.substr(1, s.size() >= 2 ? s.size() - 2 : 0);
    const auto parts = split_top_level(body, ',');
    if (parts.empty()) return r;
    if (auto v = parse_pkg_version(parts[0]); v && !v->empty()) r.bounds.push_back({inc_lo ? 'g' : '>', *v});
    if (parts.size() > 1) {
      const std::string& last = parts.back();
      const bool inc_hi = !last.empty() && last.back() == ']';
      std::string hi = last;
      if (!hi.empty() && (hi.back() == ']' || hi.back() == ')')) hi.pop_back();
      if (auto v = parse_pkg_version(hi); v && !v->empty()) r.bounds.push_back({inc_hi ? 'l' : '<', *v});
    }
  } else if ((e == Eco::Npm || e == Eco::Cargo) && (s.front() == '^' || s.front() == '~')) {
    const char opc = s.front();
    const std::string_view rest = trim(s.substr(1));
    std::size_t given = 0;
    for (char c : rest)
      if (c == '.') ++given;
    ++given;
    if (auto v = parse_pkg_version(rest); v && !v->empty()) {
      const auto pr = opc == '^' ? caret_range(*v) : tilde_range(*v, given);
      r.bounds.push_back(pr.first);
      r.bounds.push_back(pr.second);
    }
  } else if ((e == Eco::Npm || e == Eco::Cargo) && s.find_first_of("xX*") != std::string_view::npos) {
    const auto pr = wildcard_range(s);
    r.bounds = pr.first.ver.parts.empty() && pr.second.ver.parts.empty() ? std::vector<Bound>{} : std::vector<Bound>{pr.first, pr.second};
  } else if (e == Eco::Pip && (s.rfind("~=", 0) == 0 || s.rfind("==", 0) == 0) && s.find(',') == std::string_view::npos) {
    if (auto v = parse_pkg_version(s.substr(2)); v && !v->empty()) {
      if (s.rfind("~=", 0) == 0) {
        const auto pr = tilde_range(*v, 3);
        r.bounds.push_back(pr.first);
        r.bounds.push_back(pr.second);
      } else {
        r.bounds.push_back({'=', *v});  // == is exact equality, both ways
      }
    }
  } else {
    // Comma list of comparators (pip style) or a space separated set (npm/go).
    std::string_view body = s;
    std::vector<std::string> pieces;
    if (body.find(',') != std::string_view::npos) {
      pieces = split_top_level(body, ',');
    } else if (e == Eco::Npm || e == Eco::Go) {
      pieces = split_top_level(body, ' ');
    } else {
      pieces.push_back(std::string(body));
    }
    for (const auto& piece : pieces) {
      std::string_view p = trim(piece);
      if (p.empty()) continue;
      char op = '=';
      std::string_view rest;
      if (p.rfind(">=", 0) == 0) { op = 'g'; rest = p.substr(2); }
      else if (p.rfind("<=", 0) == 0) { op = 'l'; rest = p.substr(2); }
      else if (p.rfind("===", 0) == 0) { op = '='; rest = p.substr(3); }
      else if (p.rfind("==", 0) == 0) { op = '='; rest = p.substr(2); }
      else if (p.rfind("!=", 0) == 0) { continue; }  // negation: skip, not a failure
      else if (p.rfind("!~", 0) == 0) { continue; }
      else if (p.rfind('>', 0) == 0) { op = '>'; rest = p.substr(1); }
      else if (p.rfind('<', 0) == 0) { op = '<'; rest = p.substr(1); }
      else if (p.rfind('~', 0) == 0) { rest = p.substr(1); }
      else rest = p;
      if (auto v = parse_pkg_version(rest); v && !v->empty()) r.bounds.push_back({op, *v});
    }
  }
  if (r.bounds.empty()) {
    r.unbounded = false;
    r.unreadable = true;
  }
  return r;
}

}  // namespace

Verdict check_spec(Eco e, std::string_view version, std::string_view spec) {
  const std::string_view vs = trim(version);
  if (vs.empty()) return Verdict::Unknown;
  const auto have = parse_pkg_version(vs);
  if (!have || have->empty()) return Verdict::Unknown;
  const Range r = parse_range(e, spec);
  if (r.unreadable) return Verdict::Unknown;
  if (r.unbounded) return Verdict::Ok;  // genuinely unconstrained => not a failure
  bool all = true;
  for (const auto& b : r.bounds)
    if (!cmp_bound(*have, b)) all = false;
  if (all) return Verdict::Ok;
  // A bound that failed decides the message. Below a floor is TooOld; above a
  // ceiling is TooNew. These are genuinely different problems - "you need to
  // upgrade" versus "you have something the project did not ask for" - so they
  // must not be collapsed into one verdict, and neither may be reported as Ok.
  for (const auto& b : r.bounds) {
    if (cmp_bound(*have, b)) continue;
    // '=' is exact equality, which is neither a floor nor a ceiling, so the
    // direction has to come from the comparison itself. Without this a pinned
    // version that is simply older ("31.0.0" against "==33.0.0") was reported as
    // "newer than the project allows" - the opposite of the truth.
    if (b.op == '=') return compare_versions(*have, b.ver) < 0 ? Verdict::TooOld : Verdict::TooNew;
    return bound_is_lower(b) ? Verdict::TooOld : Verdict::TooNew;
  }
  return Verdict::Ok;
}

std::string spec_min(Eco e, std::string_view spec) {
  const Range r = parse_range(e, spec);
  Version best;
  bool found = false;
  for (const auto& b : r.bounds) {
    if (!bound_is_lower(b) && b.op != '=') continue;  // only floors, not ceilings
    if (!found || compare_versions(b.ver, best) > 0) {
      best = b.ver;
      found = true;
    }
  }
  return found ? version_string(best) : std::string();
}

bool versions_match(std::string_view a, std::string_view b) {
  const std::string_view sa = trim(a), sb = trim(b);
  if (sa == sb) return true;
  const auto va = parse_pkg_version(sa);
  const auto vb = parse_pkg_version(sb);
  if (!va || !vb || va->empty() || vb->empty()) return false;
  return compare_versions(*va, *vb) == 0;
}

// ---------------------------------------------------------------- parsers

namespace {

constexpr std::size_t kMaxSpecs = 256;
constexpr std::size_t kMaxMetaBytes = 256 * 1024;
constexpr std::size_t kMaxDirEntries = 6000;

// One PEP 508 / requirements.txt line: "rich>=13,<15", "pkg[extra]==1.0", "pkg".
bool parse_one_requirement(std::string_view line, PkgSpec& out) {
  line = trim(line);
  if (line.empty() || line.front() == '#' || line.front() == '-') return false;  // -r/-e/--index-url
  const auto semi = line.find(';');
  if (semi != std::string_view::npos) line = trim(line.substr(0, semi));           // env marker
  const auto hash = line.find("--hash");
  if (hash != std::string_view::npos) line = trim(line.substr(0, hash));
  line = trim(line);
  if (line.empty() || line.front() == '#') return false;

  // PEP 508 grammar: NAME [extras] [SP version-spec] | NAME [extras] SP @ URL.
  // Walk the pieces in order instead of scanning for the first operator, so an
  // extras list like "rich[jupyter]>=13" does not hide the real constraint.
  std::size_t i = 0;
  while (i < line.size() && (std::isalnum(static_cast<unsigned char>(line[i])) != 0 || line[i] == '-' ||
                             line[i] == '_' || line[i] == '.'))
    ++i;
  const std::string_view name = line.substr(0, i);
  if (name.empty()) return false;
  if (name.find("://") != std::string_view::npos) return false;  // a bare URL, not a name

  // Extras, which we record nowhere but must step over.
  if (i < line.size() && line[i] == '[') {
    const auto close = line.find(']', i);
    if (close == std::string_view::npos) return false;
    i = close + 1;
  }
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;

  // A direct reference pins no version we can reason about.
  const bool direct_ref = i < line.size() && line[i] == '@';
  std::string_view constraint;
  if (!direct_ref && i < line.size()) {
    const char c = line[i];
    if (c == '=' || c == '<' || c == '>' || c == '!' || c == '~' || c == '^' || c == '(')
      constraint = trim(line.substr(i));
  }

  const std::string canon = normalize_pep503(name);
  if (canon.empty() || !is_eco_package_name(Eco::Pip, canon)) return false;

  // `constraint` deliberately excludes the name: check_spec() and spec_min()
  // read a bare spec, and "psutil>=5.9,<7" would be one unparseable token.
  out.name = canon;
  out.kind = "pip";
  out.spec = sanitize_text(constraint, 64);
  out.required_min = (direct_ref || out.spec.empty()) ? std::string() : spec_min(Eco::Pip, out.spec);
  out.unverifiable = direct_ref;
  return true;
}

void push(std::vector<PkgSpec>& v, PkgSpec s) {
  if (v.size() >= kMaxSpecs) return;
  merge_specs(v, {std::move(s)});
}

// pip and TOML both treat '#' as starting a comment that runs to end of line,
// and neither allows '#' inside a name, a version or a constraint.
std::string_view strip_comment(std::string_view line) {
  const auto h = line.find('#');
  return h == std::string_view::npos ? line : trim(line.substr(0, h));
}

// Reads a TOML list value that starts on line `i`, appending following lines
// while the bracket stays open. Yields unquoted, comment-free items. Returns
// false for a list that never closes, which is a malformed file: we stop
// rather than guess where the author meant it to end.
bool collect_toml_items(const std::vector<std::string>& lines, std::size_t& i, std::string_view rhs,
                        std::vector<std::string>& out) {
  // Owns the buffer for the multi-line case. `rhs` may end up pointing into it,
  // so this has to outlive the whole function, not just the branch that fills it.
  std::string body;
  if (rhs.size() >= 2 && rhs.front() == '[' && rhs.back() == ']') {
    rhs = trim(rhs.substr(1, rhs.size() - 2));
    if (rhs.empty()) return true;  // an explicit empty list
  } else if (rhs.empty() || rhs.front() != '[') {
    const std::string_view one = strip_comment(rhs);
    if (!one.empty()) out.push_back(std::string(one));
    return true;
  } else {
    body.assign(rhs);
    bool closed = false;
    for (std::size_t n = 0; n < 512; ++n) {
      if (++i >= lines.size()) return false;  // ran off the end
      body += "\n";
      body += lines[i];
      if (body.size() > kMaxMetaBytes) return false;
      if (body.find(']') != std::string::npos) { closed = true; break; }
    }
    if (!closed) return false;
    const auto open = body.find('[');
    const auto close = body.rfind(']');
    if (open == std::string::npos || close == std::string::npos || close < open) return false;
    rhs = std::string_view(body).substr(open + 1, close - open - 1);
  }
  for (const auto& item : split_top_level(rhs, ',')) {
    std::string_view q = strip_comment(trim(item));
    if (q.size() >= 2 && (q.front() == '"' || q.front() == '\'') && q.back() == q.front())
      q = q.substr(1, q.size() - 2);
    q = trim(q);
    if (!q.empty()) out.push_back(std::string(q));
  }
  return true;
}

}  // namespace

std::vector<PkgSpec> parse_requirements(std::string_view text) {
  std::vector<PkgSpec> out;
  std::string joined;   // accumulates backslash-continued lines
  for (const auto& raw : split(text, '\n')) {
    std::string_view line = trim(raw);
    if (line.empty() || line.front() == '#') {
      // A comment cannot appear between a continuation and its target, but an
      // empty line inside a continued entry is legal, so only flush on break.
      if (joined.empty()) continue;
    }
    // pip concatenates a continued line directly: "cont\" + "inued>=1.0" is one
    // requirement, not two words.
    joined += line;
    if (line.back() == '\\') {  // continuation: keep going, drop the backslash
      joined.pop_back();
      continue;
    }
    // Parse before clearing: `full` is a view into `joined`.
    const std::string full{strip_comment(trim(joined))};
    joined.clear();
    if (full.empty()) continue;
    PkgSpec s;
    if (parse_one_requirement(full, s)) push(out, std::move(s));
  }
  // A file ending mid-continuation still yields whatever we managed to gather.
  if (!joined.empty()) {
    const std::string tail{strip_comment(trim(joined))};
    if (!tail.empty()) {
      PkgSpec s;
      if (parse_one_requirement(tail, s)) push(out, std::move(s));
    }
  }
  return out;
}

std::vector<PkgSpec> parse_pyproject_deps(std::string_view text) {
  std::vector<PkgSpec> out;
  const auto lines = split(text, '\n');
  enum class Sec { None, Project, Poetry, BuildSys };
  Sec sec = Sec::None;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string_view line = strip_comment(trim(lines[i]));
    if (line.empty()) continue;
    if (line.front() == '[') {
      if (line == "[project]" || line == "[project.optional-dependencies]") sec = Sec::Project;
      else if (line == "[tool.poetry.dependencies]") sec = Sec::Poetry;
      else if (line == "[build-system]") sec = Sec::BuildSys;
      else sec = Sec::None;
      continue;
    }
    if (sec == Sec::None) continue;

    // PEP 621 and PEP 518 both spell the key first and the list after '='.
    const char* key = nullptr;
    if (sec == Sec::Project && line.rfind("dependencies", 0) == 0) key = "dependencies";
    else if (sec == Sec::BuildSys && line.rfind("requires", 0) == 0) key = "requires";
    if (key != nullptr) {
      std::string_view rhs = trim(line.substr(std::string_view(key).size()));
      if (rhs.empty() || rhs.front() != '=') continue;
      std::vector<std::string> items;
      // Pass the value intact: a bare "[" means the list continues onto the
      // following lines, and collect_toml_items picks the inline or multi-line
      // path itself. Stripping the "=" here is all this side has to do.
      if (!collect_toml_items(lines, i, trim(rhs.substr(1)), items)) break;  // truncated file
      for (const auto& item : items) {
        PkgSpec s;
        if (parse_one_requirement(item, s)) push(out, std::move(s));
      }
      continue;
    }

    if (sec != Sec::Poetry) continue;
    // Poetry is `name = constraint`, and the constraint may be a table.
    const auto eq = line.find('=');
    if (eq == std::string_view::npos) continue;
    const std::string_view raw_name = trim(line.substr(0, eq));
    if (iequals(raw_name, "python")) continue;  // the interpreter constraint, not a package
    const std::string canon = normalize_pep503(raw_name);
    if (canon.empty() || !is_eco_package_name(Eco::Pip, canon)) continue;
    std::string_view rhs = trim(line.substr(eq + 1));
    std::string_view spec;
    if (rhs.size() >= 2 && (rhs.front() == '"' || rhs.front() == '\'') && rhs.back() == rhs.front()) {
      spec = rhs.substr(1, rhs.size() - 2);
    } else {
      // { version = "^1.2", optional = true } - pull out just the version.
      const auto v = rhs.find("version");
      if (v == std::string_view::npos) continue;
      const auto q = rhs.find_first_of("\"'", v);
      if (q == std::string_view::npos) continue;
      const auto q2 = rhs.find(rhs[q], q + 1);
      if (q2 == std::string_view::npos) continue;
      spec = rhs.substr(q + 1, q2 - q - 1);
    }
    PkgSpec s;
    s.name = canon;
    s.kind = "pip";
    s.spec = sanitize_text(spec, 64);
    s.required_min = s.spec.empty() ? std::string() : spec_min(Eco::Npm, s.spec);
    if (!s.required_min.empty()) push(out, std::move(s));
  }
  return out;
}

std::vector<PkgSpec> parse_setup_py_deps(std::string_view text) {
  std::vector<PkgSpec> out;
  static const char* const keys[] = {"install_requires", "setup_requires", "tests_require"};
  for (const char* key : keys) {
    // Find the bare key, then walk to the '=' separately. Searching for
    // "install_requires=" would miss "install_requires = [", and PEP 8 (and
    // black, and every linter) requires that space around a keyword
    // argument's '=' - so the un-spaced form was the rare one, and searching
    // for it made the whole parser miss real projects.
    const std::string_view keysv(key);
    std::size_t pos = 0;
    while ((pos = text.find(keysv, pos)) != std::string_view::npos) {
      const std::size_t after = pos + keysv.size();
      // Require a non-identifier character in front, so "requires" does not
      // match inside "setup_requires".
      if (pos > 0) {
        const char prev = text[pos - 1];
        const bool ident = (prev == '_' || (prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') ||
                            (prev >= '0' && prev <= '9'));
        if (ident) { pos = after; continue; }
      }
      // ...and an '=' after any spaces/tabs (not newlines: a line break before
      // the '=' is legal Python but far too rare to be worth the risk of
      // wandering into unrelated code).
      std::size_t eq = after;
      while (eq < text.size() && (text[eq] == ' ' || text[eq] == '\t')) ++eq;
      if (eq >= text.size() || text[eq] != '=') { pos = after; continue; }
      pos = eq + 1;
      // Skip whitespace, including the newline before a wrapped list.
      std::size_t open = pos;
      while (open < text.size() && (text[open] == ' ' || text[open] == '\t' || text[open] == '\n' ||
                                    text[open] == '\r'))
        ++open;
      if (open >= text.size() || text[open] != '[') continue;
      // Walk to the matching ']', honouring quoted strings and comments so a
      // ']' inside "pkg]==1.0" does not end the list early.
      std::size_t depth = 0;
      std::size_t end = std::string_view::npos;
      char quote = 0;
      for (std::size_t i = open; i < text.size(); ++i) {
        const char c = text[i];
        if (quote != 0) {
          if (c == '\\') { ++i; continue; }
          if (c == quote) quote = 0;
          continue;
        }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '#') {  // comment runs to end of line
          while (i < text.size() && text[i] != '\n') ++i;
          continue;
        }
        if (c == '[') ++depth;
        else if (c == ']' && --depth == 0) { end = i; break; }
        if (i - open > kMaxMetaBytes) return out;  // absurdly long list
      }
      if (end == std::string_view::npos) break;  // truncated file
      for (const auto& item : split_top_level(text.substr(open + 1, end - open - 1), ',')) {
        std::string_view q = strip_comment(trim(item));
        if (q.size() >= 2 && (q.front() == '"' || q.front() == '\'') && q.back() == q.front())
          q = q.substr(1, q.size() - 2);
        PkgSpec s;
        if (parse_one_requirement(trim(q), s)) push(out, std::move(s));
      }
      pos = end;
    }
  }
  return out;
}

std::vector<PkgSpec> parse_package_json_deps(std::string_view text) {
  std::vector<PkgSpec> out;
  auto pr = json::parse(text);
  if (!pr.value || !pr.value->is_object()) return out;
  static const char* const sections[] = {"dependencies", "devDependencies", "optionalDependencies"};
  for (const char* sec : sections) {
    const json::Value* obj = pr.value->find(sec);
    if (!obj || !obj->is_object()) continue;
    for (const auto& [name, val] : *obj->as_object()) {
      if (!is_npm_package_name(name)) continue;
      const std::string* spec_s = val.as_string();
      PkgSpec s;
      s.name = name;
      s.kind = "npm";
      s.spec = spec_s ? sanitize_text(*spec_s, 48) : "";
      s.required_min = spec_min(Eco::Npm, s.spec);
      push(out, std::move(s));
    }
  }
  return out;
}

std::vector<PkgSpec> parse_cargo_deps(std::string_view text) {
  std::vector<PkgSpec> out;
  enum class Sec { None, Package, Dependencies, DevDeps, BuildDeps };
  Sec sec = Sec::None;
  for (const auto& raw : split(text, '\n')) {
    const std::string_view line = strip_comment(trim(raw));
    if (line.empty() || line.front() == '#') continue;
    if (line.front() == '[') {
      if (line == "[package]") sec = Sec::Package;
      else if (line == "[dependencies]") sec = Sec::Dependencies;
      else if (line == "[dev-dependencies]") sec = Sec::DevDeps;
      else if (line == "[build-dependencies]") sec = Sec::BuildDeps;
      else sec = Sec::None;
      continue;
    }
    if (sec != Sec::Dependencies && sec != Sec::DevDeps && sec != Sec::BuildDeps) continue;
    const auto eq = line.find('=');
    if (eq == std::string_view::npos) continue;
    const std::string name(trim(line.substr(0, eq)));
    if (!is_eco_package_name(Eco::Cargo, name)) continue;
    std::string_view rhs = trim(line.substr(eq + 1));
    std::string spec;
    if (rhs.front() == '"' || rhs.front() == '\'') {
      const auto q2 = rhs.find(rhs.front(), 1);
      if (q2 == std::string_view::npos) continue;
      spec = sanitize_text(rhs.substr(1, q2 - 1), 48);
    } else {
      const auto v = rhs.find("version");
      if (v == std::string_view::npos) continue;  // path/git dependency: unresolvable
      const auto q = rhs.find_first_of("\"'", v);
      if (q == std::string_view::npos) continue;
      const auto q2 = rhs.find(rhs[q], q + 1);
      if (q2 == std::string_view::npos) continue;
      spec = sanitize_text(rhs.substr(q + 1, q2 - q - 1), 48);
    }
    PkgSpec s;
    s.name = name;
    s.kind = "cargo";
    s.spec = spec;
    s.required_min = spec_min(Eco::Npm, spec);
    push(out, std::move(s));
  }
  return out;
}

std::vector<PkgSpec> parse_cargo_lock(std::string_view text) {
  std::vector<PkgSpec> out;
  bool in_pkg = false;
  std::string name, version;
  for (const auto& raw : split(text, '\n')) {
    const std::string_view line = strip_comment(trim(raw));
    if (line.rfind("[[package]]", 0) == 0) {
      if (!name.empty() && !version.empty()) {
        PkgSpec s;
        s.name = name;
        s.kind = "cargo";
        s.version_locked = version;
        push(out, std::move(s));
      }
      in_pkg = true;
      name.clear();
      version.clear();
      continue;
    }
    if (line.front() == '[' && line.rfind("[[", 0) != 0) { in_pkg = false; continue; }
    if (!in_pkg) continue;
    const auto eq = line.find('=');
    if (eq == std::string_view::npos) continue;
    std::string_view key = trim(line.substr(0, eq));
    std::string_view val = trim(line.substr(eq + 1));
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
    if (key == "name") name = sanitize_text(val, 64);
    else if (key == "version") version = sanitize_text(val, 32);
  }
  if (!name.empty() && !version.empty()) {
    PkgSpec s;
    s.name = name;
    s.kind = "cargo";
    s.version_locked = version;
    push(out, std::move(s));
  }
  return out;
}

std::vector<PkgSpec> parse_go_mod_requires(std::string_view text) {
  std::vector<PkgSpec> out;
  bool in_block = false;
  for (const auto& raw : split(text, '\n')) {
    std::string_view line = trim(raw);
    if (line.empty() || line.front() == '/' ) continue;
    if (line.rfind("require (", 0) == 0) { in_block = true; continue; }
    if (in_block && line == ")") { in_block = false; continue; }
    std::string_view body;
    if (in_block) body = line;
    else if (line.rfind("require ", 0) == 0) body = trim(line.substr(8));
    else continue;
    // strip an inline "// indirect" comment
    const auto slash = body.find("//");
    if (slash != std::string_view::npos) body = trim(body.substr(0, slash));
    if (body.empty()) continue;
    std::string_view name = body;
    std::string_view ver;
    const auto sp = body.find(' ');
    if (sp != std::string_view::npos) {
      name = trim(body.substr(0, sp));
      ver = trim(body.substr(sp + 1));
    }
    if (!is_eco_package_name(Eco::Go, name)) continue;
    PkgSpec s;
    s.name = std::string(name);
    s.kind = "go";
    s.spec = sanitize_text(ver, 48);
    s.required_min = spec_min(Eco::Go, s.spec);
    // A go.mod "require" is an exact pin, exactly like a lock file: there is no
    // range operator in that grammar. Recording it as version_locked lets `check`
    // say "you have v1.2.0, the project pins v1.9.1" instead of only complaining
    // that v1.2.0 is too old, and makes it correct when a newer module is present.
    if (!s.spec.empty() && s.spec.find_first_of("^~<>=*x") == std::string::npos) {
      s.version_locked = s.spec;
      if (s.required_min.empty()) s.required_min = s.spec;
    }
    push(out, std::move(s));
  }
  return out;
}

std::map<std::string, std::string> parse_go_sum(std::string_view text) {
  std::map<std::string, std::string> out;
  for (const auto& raw : split(text, '\n')) {
    const std::string_view line = trim(raw);
    if (line.empty()) continue;
    const std::size_t sp = line.find(' ');
    if (sp == std::string_view::npos) continue;
    // The module path is the whole first field. Never prefix-match it: "y"
    // must not pick up the line written for "yz".
    const std::string mod = sanitize_text(line.substr(0, sp), 128);
    if (mod.empty() || !is_eco_package_name(Eco::Go, mod)) continue;
    std::string_view ver = trim(line.substr(sp + 1));
    const std::size_t sp2 = ver.find(' ');
    if (sp2 != std::string_view::npos) ver = trim(ver.substr(0, sp2));
    const std::string vs = sanitize_text(ver, 48);
    if (vs.empty()) continue;
    const auto cur = out.find(mod);
    if (cur == out.end()) {
      out.emplace(mod, vs);
      continue;
    }
    // Compare as versions, not as text. go.sum routinely holds several
    // versions of one module, and a text comparison ranks "v1.9.1" above
    // "v1.10.0" because "9" sorts after "1".
    const auto a = parse_pkg_version(cur->second), b = parse_pkg_version(vs);
    if (!a || !b || compare_versions(*b, *a) > 0) cur->second = vs;
  }
  return out;
}

std::vector<PkgSpec> parse_gradle_deps(std::string_view text) {
  // Can check_spec() actually evaluate this `spec`? A literal or a Maven range
  // yes; "1.+", a catalog symbol or an empty version no, and those must be
  // reported as unverifiable rather than silently satisfied.
  auto version_testable = [](std::string_view spec) {
    if (spec.empty()) return false;
    // Maven dynamic forms are not a satisfiable constraint even though the
    // tolerant version parser accepts them: "1.+", "1.2.x" are "any recent",
    // not a floor we can test. A bracketed range like "[1.0,2.0)" is exact and
    // fine.
    if (spec.find('+') != std::string_view::npos) return false;
    if (spec.find('*') != std::string_view::npos || spec.find('x') != std::string_view::npos) return false;
    if (spec.front() == '[') return true;
    const auto v = parse_pkg_version(spec);
    return v && !v->empty();
  };
  // Configuration names that introduce a dependency line in the Groovy and
  // Kotlin DSLs. A project that only ever uses a custom configuration spelling
  // is missed, which is the safe direction: we report nothing rather than
  // guessing which quoted string on an unrelated line is a dependency.
  static const char* const configs[] = {
      "api", "compile", "compileOnly", "compileOnlyApi", "implementation", "providedCompile", "providedRuntime",
      "runtimeOnly", "annotationProcessor", "kapt", "ksp",
      "testApi", "testCompile", "testCompileOnly", "testImplementation", "testRuntimeOnly", "testAnnotationProcessor",
      "androidTestApi", "androidTestCompileOnly", "androidTestImplementation", "androidTestRuntimeOnly",
      "debugApi", "debugImplementation", "debugRuntimeOnly", "releaseApi", "releaseImplementation", "releaseRuntimeOnly",
  };
  // Version tails can be literal, Maven range ("[1.0,2.0)"), dynamic ("1.+") or
  // a bare project property. Only the first two are testable, so the caller
  // marks the rest unverifiable.
  auto version_spec = [](std::string_view v) -> std::string {
    const auto s = sanitize_text(trim(v), 48);
    if (s.empty()) return {};
    if (s.find("${") != std::string::npos) return {};
    return s;
  };

  std::vector<PkgSpec> out;
  for (const auto& raw : split(text, '\n') | std::views::transform(trim)) {
    std::string_view line = raw;
    if (line.empty()) continue;
    if (line[0] == '#' || line.rfind("//", 0) == 0 || line.rfind("/*", 0) == 0) continue;
    // The line must start with a known configuration name.
    const std::size_t head = line.find_first_of(" (");
    const std::string_view fn = line.substr(0, head == std::string_view::npos ? line.size() : head);
    const bool is_config = std::any_of(std::begin(configs), std::end(configs),
                                       [&](const char* c) { return fn == c; });
    if (!is_config) continue;

    // Map form: "implementation group: 'g', name: 'a', version: 'v'" (Groovy)
    // or "implementation(group = "g", name = "a", version = "v")" (Kotlin).
    auto value_of = [&](const char* key) -> std::string {
      std::size_t pos = 0;
      while ((pos = line.find(key, pos)) != std::string_view::npos) {
        char after = pos + std::strlen(key) < line.size() ? line[pos + std::strlen(key)] : ' ';
        const bool delim = (after == ':' || after == '=' || after == ' ');
        if (!delim) { pos += std::strlen(key); continue; }
        std::size_t v = pos + std::strlen(key);
        while (v < line.size() && (line[v] == ' ' || line[v] == '\t' || line[v] == ':' || line[v] == '=')) ++v;
        if (v < line.size() && (line[v] == '\'' || line[v] == '"')) {
          const char q = line[v++];
          const auto e = line.find(q, v);
          if (e != std::string_view::npos) return sanitize_text(line.substr(v, e - v), 64);
        }
        pos = v;
      }
      return {};
    };
    const std::string mg = value_of("group");
    const std::string mn = value_of("name");
    if (!mn.empty()) {
      const std::string mv = value_of("version");
      PkgSpec s;
      s.kind = "maven";
      s.name = mg.empty() ? mn : mg + ":" + mn;
      if (is_maven_coordinate(s.name) && s.name != mn) {  // external deps carry a group
        s.spec = version_spec(mv);
        s.unverifiable = !version_testable(s.spec);
        // Only a literal/range version supports a floor. A dynamic "1.+" must
        // not claim ">= 1" as its requirement.
        if (!s.unverifiable) s.required_min = spec_min(Eco::Maven, s.spec);
        if (!s.required_min.empty() || s.unverifiable) push(out, std::move(s));
      }
      continue;  // a map form is one dependency; do not also scan quoted strings
    }

    // Coordinate string form: "implementation 'g:a:v'", with or without parens.
    // A quoted string that is not a bare group or bare version has one colon at
    // most, so plugin ids ("org.foo.bar") and version literals ("1.2.3") cannot
    // be mistaken for coordinates.
    std::size_t pos = 0;
    for (;;) {
      // Look for the next of either quote and consume it. (Searching first for
      // ' and then, only if that failed, for " is wrong: a failure sets pos to
      // npos and the second search silently never matches.)
      const std::size_t q = std::min(line.find('\'', pos), line.find('"', pos));
      if (q == std::string_view::npos) break;
      const char quote = line[q];
      const auto end = line.find(quote, q + 1);
      if (end == std::string_view::npos) break;
      const std::string content = sanitize_text(line.substr(q + 1, end - q - 1), 96);
      pos = end + 1;
      if (content.empty() || content.find(':') == std::string_view::npos) continue;
      // Split into two or three segments by ':'.
      const auto c1 = content.find(':');
      const auto c2 = c1 == std::string_view::npos ? std::string_view::npos : content.find(':', c1 + 1);
      if (c2 != std::string_view::npos && content.find(':', c2 + 1) != std::string_view::npos) continue;  // classifier form
      const std::string group = std::string(trim(content.substr(0, c1)));
      const std::string art = std::string(trim(content.substr(c1 + 1, c2 == std::string_view::npos ? std::string_view::npos : c2 - c1 - 1)));
      const std::string ver = c2 == std::string_view::npos ? std::string() : std::string(trim(content.substr(c2 + 1)));
      const std::string name = group + ":" + art;
      if (group.empty() || art.empty() || !is_maven_coordinate(name)) continue;
      PkgSpec s;
      s.kind = "maven";
      s.name = name;
      s.spec = version_spec(ver);
      s.unverifiable = !version_testable(s.spec);
      if (!s.unverifiable) s.required_min = spec_min(Eco::Maven, s.spec);
      if (!s.required_min.empty() || s.unverifiable) push(out, std::move(s));
    }
  }
  return out;
}

std::vector<PkgSpec> parse_pom_deps(std::string_view text) {
  std::vector<PkgSpec> out;
  std::size_t pos = 0;
  while ((pos = text.find("<dependency>", pos)) != std::string_view::npos) {
    const auto end = text.find("</dependency>", pos);
    const std::string_view body = text.substr(pos + 11, (end == std::string_view::npos ? text.size() : end) - pos - 11);
    auto tag = [&](const char* name) -> std::string {
      const std::string open = std::string("<") + name + ">";
      const std::string close = std::string("</") + name + ">";
      const auto a = body.find(open);
      if (a == std::string_view::npos) return {};
      const auto b = body.find(close, a);
      if (b == std::string_view::npos) return {};
      return sanitize_text(trim(body.substr(a + open.size(), b - a - open.size())), 96);
    };
    const std::string group = tag("groupId");
    const std::string artifact = tag("artifactId");
    if (artifact.empty() || !is_eco_package_name(Eco::Maven, artifact)) { pos = end == std::string_view::npos ? text.size() : end; continue; }
    const std::string version = tag("version");
    PkgSpec s;
    s.name = group.empty() ? artifact : group + ":" + artifact;
    s.kind = "maven";
    s.spec = version;
    s.required_min = spec_min(Eco::Maven, version);
    s.unverifiable = version.find("${") != std::string::npos;
    if (!s.required_min.empty() || s.unverifiable) push(out, std::move(s));
    pos = end == std::string_view::npos ? text.size() : end;
  }
  return out;
}

void merge_specs(std::vector<PkgSpec>& out, const std::vector<PkgSpec>& specs) {
  for (const auto& s : specs) {
    bool merged = false;
    for (auto& e : out) {
      if (e.kind != s.kind || e.name != s.name) continue;
      merged = true;
      // The same package is often constrained in two places (requirements.txt
      // plus pyproject). Keep the strongest lower bound, since that is the one
      // the two manifests actually agree on.
      const auto mine = parse_version(e.required_min);
      const auto theirs = parse_version(s.required_min);
      if (!mine) e.required_min = s.required_min;
      else if (theirs && compare_versions(*theirs, *mine) > 0) e.required_min = s.required_min;
      if (e.spec.empty()) e.spec = s.spec;
      if (e.version_locked.empty()) e.version_locked = s.version_locked;
      e.unverifiable = e.unverifiable && s.unverifiable;
      e.required = e.required || s.required;
      break;
    }
    if (!merged) out.push_back(s);
  }
}

// ---------------------------------------------------------------- probing
//
// Everything below READS THE FILESYSTEM ONLY. No subprocess is spawned, which
// keeps the promise in knowledge.hpp: builtdiff only ever executes tools from
// its built-in allowlist, and every one of those is a bare `--version`. Adding
// `pip list` would mean either a new non-version allowlist entry or a
// `python3 -c`, and neither belongs there.
//
// Paths are read through read_file_under when they are inside the project root
// (which refuses symlinks). Registry paths under $HOME use a separate bounded
// reader with the same symlink refusal. Only a name and a version ever leave
// these functions - an absolute path is never recorded in a snapshot.

namespace {

// Like read_file_under but for a caller-supplied directory that may be outside
// the project root. Refuses symlinks and non-regular files, caps the size.
std::optional<std::string> read_registry_file(const std::string& path) {
  auto txt = read_file(path, kMaxMetaBytes);
  if (!txt) return std::nullopt;
  std::error_code ec;
  const auto st = fs::symlink_status(path, ec);
  if (ec || fs::is_symlink(st)) return std::nullopt;
  return txt;
}

void add_dir(std::vector<std::string>& v, const std::string& d, const char* why) {
  if (d.empty() || d.size() > 512) return;
  std::error_code ec;
  if (!fs::is_directory(d, ec)) return;
  for (const auto& e : v)
    if (e == d) return;
  v.push_back(d);
  (void)why;
}

std::string home_dir() {
  const char* h = std::getenv("HOME");
  if (!h || !*h || h[0] != '/' || std::string(h).size() > 400) return {};
  return h;
}

// "<name>-<version>.dist-info" -> version. Falls back to reading METADATA.
std::string version_from_dist_info(const std::string& dir_path, const std::string& dir_name,
                                   const std::string& canonical) {
  const std::string suffix = ".dist-info";
  if (dir_name.size() > suffix.size() + canonical.size() &&
      dir_name.compare(dir_name.size() - suffix.size(), suffix.size(), suffix) == 0) {
    std::string stem = dir_name.substr(0, dir_name.size() - suffix.size());
    // The directory name is escaped, the canonical name is not: "zope.interface-5.4"
    // comes from "zope_interface-5.4". Match on the trailing version instead.
    const auto dash = stem.rfind('-');
    if (dash != std::string::npos && dash > 0) {
      if (auto v = parse_pkg_version(stem.substr(dash + 1)); v && !v->empty()) return version_string(*v);
    }
  }
  if (auto txt = read_registry_file(dir_path + "/METADATA"); txt) {
    for (const auto& raw : split(*txt, '\n')) {
      const std::string_view line = trim(raw);
      if (line.empty()) break;  // headers end at the first blank line
      if (line.rfind("Version:", 0) == 0) {
        const std::string v = sanitize_text(trim(line.substr(8)), 48);
        if (!v.empty()) return v;
      }
    }
  }
  return {};
}

}  // namespace

// A venv created with `include-system-site-packages = false` (the default) does
// NOT see the interpreter's own site-packages. Searching those anyway would
// report a dependency as installed when the project's environment cannot
// actually import it - the single most damaging wrong answer this layer could
// give, because it hides a real "pip install" the user still has to do.
bool venv_sees_system_site_packages(const std::string& venv_root) {
  auto cfg = read_registry_file(venv_root + "/pyvenv.cfg");
  if (!cfg) return false;
  for (const auto& raw : split(*cfg, '\n')) {
    const std::string_view line = trim(raw);
    if (line.rfind("include-system-site-packages", 0) != 0) continue;
    const auto eq = line.find('=');
    if (eq == std::string_view::npos) continue;
    const std::string_view v = trim(line.substr(eq + 1));
    return iequals(v, "true") || v == "1";
  }
  return false;  // absent means false, per the venv spec
}

std::vector<std::string> pip_search_dirs(const std::string& project_root) {
  std::vector<std::string> out;
  // Found a virtualenv that deliberately hides the system packages. Stop here:
  // the answer must describe that environment, not the one behind it.
  bool isolated_venv = false;
  auto add_venv = [&](const std::string& vroot) {
    std::error_code ec;
    if (!fs::exists(vroot + "/pyvenv.cfg", ec)) return;
    std::error_code ec2;
    for (fs::directory_iterator it(vroot + "/lib", ec2), end; !ec2 && it != end; it.increment(ec2)) {
      const std::string sub = it->path().string() + "/site-packages";
      std::error_code ec3;
      if (fs::is_directory(sub, ec3)) add_dir(out, sub, "venv");
    }
    if (!venv_sees_system_site_packages(vroot)) isolated_venv = true;
  };
  // An active virtualenv first: that is the environment the user will actually
  // run the program in, and it is the whole reason this layer exists.
  if (const char* ve = std::getenv("VIRTUAL_ENV"); ve && *ve == '/' && std::string(ve).size() < 400)
    add_venv(ve);
  // Then a venv in the project itself, which is the common "clone and forget"
  // case: the user never activated anything, but .venv/ is right there.
  for (const char* rel : {".venv", "venv", "env", ".env"}) {
    if (project_root.empty()) break;
    add_venv(project_root + "/" + rel);
  }
  if (isolated_venv) return out;
  // Finally the interpreters installed system-wide. Only reachable when no
  // isolated virtualenv applies, so a system copy cannot mask a venv that is
  // missing the package.
  std::error_code ec;
  for (fs::directory_iterator it("/usr/lib", ec), end; !ec && it != end; it.increment(ec)) {
    const std::string fn = it->path().filename().string();
    if (fn.rfind("python3", 0) != 0) continue;
    const std::string sub = it->path().string() + "/site-packages";
    std::error_code ec2;
    if (fs::is_directory(sub, ec2)) add_dir(out, sub, "system");
  }
  const std::string home = home_dir();
  if (!home.empty()) {
    std::error_code ec2;
    for (fs::directory_iterator it(home + "/.local/lib", ec2), end; !ec2 && it != end; it.increment(ec2)) {
      const std::string fn = it->path().filename().string();
      if (fn.rfind("python3", 0) != 0) continue;
      const std::string sub = it->path().string() + "/site-packages";
      std::error_code ec3;
      if (fs::is_directory(sub, ec3)) add_dir(out, sub, "user");
    }
  }
  return out;
}

std::string probe_pip_version(std::string_view canonical_name, const std::string& project_root) {
  const std::string canon = normalize_pep503(canonical_name);
  if (canon.empty() || canon.size() > 128) return {};
  for (const auto& dir : pip_search_dirs(project_root)) {
    std::error_code ec;
    std::size_t budget = kMaxDirEntries;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end && budget > 0; it.increment(ec), --budget) {
      const std::string fn = it->path().filename().string();
      if (fn.size() < 10) continue;
      const bool di = fn.size() > 10 && fn.compare(fn.size() - 10, 10, ".dist-info") == 0;
      const bool ei = fn.size() > 8 && fn.compare(fn.size() - 8, 8, ".egg-info") == 0;
      if (!di && !ei) continue;
      std::string stem = fn.substr(0, fn.size() - (di ? 10 : 8));
      const auto dash = stem.rfind('-');
      const std::string dir_name = dash == std::string::npos ? stem : stem.substr(0, dash);
      if (normalize_pep503(dir_name) != canon) continue;
      const std::string v = version_from_dist_info(it->path().string(), fn, canon);
      if (!v.empty()) return v;
    }
  }
  return {};
}

std::vector<std::string> npm_search_dirs(const std::string& project_root) {
  std::vector<std::string> out;
  if (project_root.empty()) return out;
  // Walk up so a monorepo package still resolves against the workspace root.
  fs::path p(project_root);
  for (int depth = 0; depth < 6; ++depth) {
    std::error_code ec;
    if (!p.empty()) add_dir(out, (p / "node_modules").string(), "project");
    p = p.parent_path();
    if (p.empty() || p == "/") break;
    (void)ec;
  }
  return out;
}

std::string probe_npm_version(std::string_view name, const std::string& project_root) {
  if (!is_npm_package_name(name)) return {};
  const std::string pkg(name);
  for (const auto& dir : npm_search_dirs(project_root)) {
    std::string pj = dir + "/" + pkg + "/package.json";
    if (auto txt = read_file(pj, kMaxMetaBytes)) {
      auto pr = json::parse(*txt);
      if (pr.value && pr.value->is_object()) {
        const std::string v = sanitize_text(json::get_string(*pr.value, "version"), 48);
        if (!v.empty()) return v;
      }
    }
  }
  return {};
}

std::string probe_cargo_version(std::string_view name, const std::string& project_root) {
  if (!is_eco_package_name(Eco::Cargo, name)) return {};
  const std::string crate(name);
  // The project's own lock file is authoritative: it is what the build will use.
  if (!project_root.empty()) {
    if (auto txt = read_file_under(project_root, "Cargo.lock", 8u << 20)) {
      for (const auto& s : parse_cargo_lock(*txt))
        if (s.name == crate && !s.version_locked.empty()) return s.version_locked;
    }
  }
  // Otherwise the local registry cache.
  const std::string home = home_dir();
  if (home.empty()) return {};
  for (const char* sub : {"/.cargo/registry/src", "/.cargo/registry/cache"}) {
    const std::string base = home + sub;
    std::error_code ec;
    for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code ec2;
      if (!it->is_directory(ec2)) continue;
      for (fs::directory_iterator jt(it->path(), ec2), jend; !ec2 && jt != jend; jt.increment(ec2)) {
        const std::string fn = jt->path().filename().string();
        if (fn.size() <= crate.size() + 2) continue;
        if (fn.compare(0, crate.size(), crate) != 0 || fn[crate.size()] != '-') continue;
        if (auto v = parse_pkg_version(std::string_view(fn).substr(crate.size() + 1)); v && !v->empty())
          return version_string(*v);
      }
    }
  }
  return {};
}

// Go writes versions with a leading "v" everywhere: go.mod, go.sum and the
// module cache filenames. Normalise to that form on every probe path so the
// recorded `version` does not depend on which source answered.
std::string go_version_text(const Version& v) { return "v" + version_string(v); }

std::string probe_go_version(std::string_view name, const std::string& project_root) {
  if (!is_eco_package_name(Eco::Go, name)) return {};
  const std::string mod(name);
  // go.sum records "<module> <version> h1:...", twice per version. Look the
  // module up as a whole key so a longer path sharing this prefix cannot be
  // mistaken for it, and take the highest version when several are listed.
  if (!project_root.empty()) {
    if (auto txt = read_file_under(project_root, "go.sum", 8u << 20)) {
      const auto sums = parse_go_sum(*txt);
      const auto it = sums.find(mod);
      if (it != sums.end()) return it->second;
    }
  }
  // A vendored tree keeps the version in modules.txt.
  if (!project_root.empty()) {
    if (auto txt = read_file_under(project_root, "vendor/modules.txt", 4u << 20)) {
      for (const auto& raw : split(*txt, '\n')) {
        const std::string_view line = trim(raw);
        if (line.rfind("# " + mod + " ", 0) != 0) continue;
        const auto vpos = line.find(' ');
        if (vpos == std::string_view::npos) continue;
        if (auto v = parse_pkg_version(line.substr(vpos + 1)); v && !v->empty()) return go_version_text(*v);
      }
    }
  }
  const std::string home = home_dir();
  if (home.empty()) return {};
  std::string cache = home + "/go/pkg/mod/cache/download";
  if (const char* gm = std::getenv("GOMODCACHE"); gm && *gm == '/' && std::string(gm).size() < 400) cache = gm;
  // Module paths are lowercase and escaped (uppercase becomes !x).
  std::string esc;
  for (char c : mod) esc.push_back(std::isupper(static_cast<unsigned char>(c)) != 0 ? static_cast<char>('!' + (c - 'A' + 'a')) : c);
  const std::string dir = cache + "/" + esc + "/@v";
  std::error_code ec;
  // Keep the winning Version alongside its text: comparing by re-parsing the
  // text would have to strip the "v" first, and one missed parse would leave
  // an empty Version here.
  Version best;
  std::size_t budget = kMaxDirEntries;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end && budget > 0; it.increment(ec), --budget) {
    const std::string fn = it->path().filename().string();
    if (fn.size() < 5 || fn.compare(fn.size() - 4, 4, ".zip") != 0) continue;
    const std::string ver = fn.substr(0, fn.size() - 4);
    if (ver.find(".mod") != std::string::npos || ver.find(".info") != std::string::npos) continue;
    if (auto v = parse_pkg_version(ver); v && !v->empty()) {
      if (best.parts.empty() || compare_versions(*v, best) > 0) best = *v;
    }
  }
  if (best.parts.empty()) return {};
  return go_version_text(best);
}

EcoProbe probe_eco_package(Eco eco, std::string_view name, const std::string& project_root) {
  EcoProbe p;
  switch (eco) {
    case Eco::Pip:
      p.version = probe_pip_version(name, project_root);
      p.via = "site-packages";
      break;
    case Eco::Npm:
      p.version = probe_npm_version(name, project_root);
      p.via = "node_modules";
      break;
    case Eco::Cargo:
      p.version = probe_cargo_version(name, project_root);
      p.via = "cargo";
      break;
    case Eco::Go:
      p.version = probe_go_version(name, project_root);
      p.via = "go";
      break;
    case Eco::Maven: {
      p.via = ".m2";
      const std::size_t colon = name.find(':');
      if (colon == std::string_view::npos) return p;
      const char* home = std::getenv("HOME");
      // An absolute $HOME is required: the .m2 path is built by concatenation,
      // and a relative or empty HOME would produce a path under the CWD.
      if (!home || *home != '/' || std::string(home).size() > 400) return p;
      p.version = probe_maven_version(name.substr(0, colon), name.substr(colon + 1), home);
      break;
    }
    case Eco::Native:
      return p;
  }
  p.version = sanitize_text(p.version, 48);
  // Only claim found when a real version was read. A package we could not look
  // up must read as missing, never as satisfied.
  p.found = !p.version.empty();
  return p;
}

std::string probe_maven_version(std::string_view group, std::string_view artifact, const std::string& home) {
  if (!is_eco_package_name(Eco::Maven, artifact) || group.empty()) return {};
  if (!is_eco_package_name(Eco::Maven, group)) return {};
  std::string path;
  for (char c : group) path += (c == '.') ? '/' : c;
  const std::string dir = home + "/.m2/repository/" + path + "/" + std::string(artifact);
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return {};
  std::string best;
  std::size_t budget = kMaxDirEntries;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end && budget > 0; it.increment(ec), --budget) {
    const std::string ver = sanitize_text(it->path().filename().string(), 64);
    if (ver.empty() || ver == "maven-metadata-local.xml") continue;
    if (!is_eco_package_name(Eco::Maven, ver)) continue;
    // Several versions installed: report the newest rather than claiming the
    // dependency is missing.
    if (best.empty()) { best = ver; continue; }
    const auto a = parse_pkg_version(best), b = parse_pkg_version(ver);
    if (b && (!a || compare_versions(*b, *a) > 0)) best = ver;
  }
  return best;
}

}  // namespace bd