// ecosystems.hpp - language package managers (pip, npm, cargo, maven, go).
//
// The library layer of builtdiff is native-linkage only: pkg-config, CMake
// config packages and ldconfig sonames. None of those can ever describe
// "rich" or "react". This header owns the second kind of dependency, so a
// snapshot records what a Python / Node / Rust / Java / Go project needs and
// `check` can compare it against the user's environment.
//
// Two hard rules hold everywhere in here:
//   * every name and version that reaches a terminal or a model prompt goes
//     through sanitize_text; parsers see untrusted files that arrived with a
//     cloned repository
//   * an unrecognised specifier (git URL, "latest", a version we cannot read)
//     yields Verdict::Unknown, never Verdict::Fail. A wrong "missing" is worse
//     than an admitted gap.
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace bd {

enum class Eco { Native, Pip, Npm, Cargo, Maven, Go };

// "pip" -> Eco::Pip, anything else -> Eco::Native.
Eco eco_from_kind(std::string_view kind) noexcept;
const char* eco_name(Eco e) noexcept;
// Human label for reports, e.g. "Python package".
const char* eco_label(Eco e) noexcept;
// True when the tool's own name comes from $HOME (node_modules, .m2, .cargo).
bool eco_scans_home(Eco e) noexcept;

// One declared dependency of a language package.
struct PkgSpec {
  std::string name;          // canonical form (see normalize_pep503 / npm)
  std::string kind;          // pip | npm | cargo | maven | go
  std::string required_min;  // lower bound, "" if the project did not ask for one
  std::string spec;          // raw constraint, sanitized; shown to the user and the model
  // A lock file pins the exact version the developer used. Recorded separately
  // from `spec` so `check` can report "you have 1.0, the project wants exactly 2.3".
  std::string version_locked;
  // The version came from something we cannot evaluate (a maven ${property}, a
  // git URL). Recorded so the report can say so instead of guessing.
  bool unverifiable = false;
  bool required = true;
};

// PEP 503 name normalisation: lowercase, runs of -_. collapse to a single "-".
// "Flask_SQLAlchemy" and "flask-sqlalchemy" are one PyPI project; without this
// `check` reports false mismatches.
std::string normalize_pep503(std::string_view name);

// npm allows "@scope/name", which is_safe_token rejects because of the '/'.
// Returns false for anything that is not a plain or scoped npm package name.
bool is_npm_package_name(std::string_view name) noexcept;

// Cargo crates and Go modules use '-' and '_/' in ways we accept verbatim after
// the charset check; npm additionally needs the '@' + '/' scoped form.
bool is_eco_package_name(Eco e, std::string_view name) noexcept;

// A maven coordinate is "<groupId>:<artifactId>". Both halves are validated with
// is_eco_package_name; exactly one colon is required. Validate the whole string
// through this rather than the plain predicate, which expects a bare group.
bool is_maven_coordinate(std::string_view name) noexcept;

// ------------------------------------------------------------- specifiers

enum class Verdict {
  Ok,      // version satisfies the constraint
  TooOld,  // version is below a lower bound the project asked for
  TooNew,  // version is above an upper bound: newer than the project pins
  Unknown, // constraint cannot be evaluated - never reported as a failure
};

// Does `version` satisfy `spec`? Understands the common forms of each
// ecosystem: bare "1.2.3" (==), ">=1.2", ">=1,<2", "~=1.4.2", "^1.2.3",
// "~1.2.3", "1.x", "[1.0,2.0)" and go's "v1.2.3".
Verdict check_spec(Eco e, std::string_view version, std::string_view spec);

// Lower bound implied by a specifier, or "" when there is none.
// ">=5.9,<7" -> "5.9"; "^1.2.3" -> "1.2.3"; "*" -> "";
std::string spec_min(Eco e, std::string_view spec);

// Do these two version strings denote the same release? Compared as versions,
// not as text, so "v1.9.1" and "1.9.1" match. Falls back to a plain string
// comparison when either side cannot be parsed, because two unparseable strings
// that are byte-identical really are the same thing.
bool versions_match(std::string_view a, std::string_view b);

// --------------------------------------------------------------- parsers
// All are pure functions over file text. They skip comments and unresolvable
// entries rather than guessing.

std::vector<PkgSpec> parse_requirements(std::string_view text);
std::vector<PkgSpec> parse_pyproject_deps(std::string_view text);
std::vector<PkgSpec> parse_setup_py_deps(std::string_view text);
std::vector<PkgSpec> parse_package_json_deps(std::string_view text);
std::vector<PkgSpec> parse_cargo_deps(std::string_view text);
// name -> resolved version, from [[package]] blocks of a Cargo.lock.
std::vector<PkgSpec> parse_cargo_lock(std::string_view text);
std::vector<PkgSpec> parse_go_mod_requires(std::string_view text);
// go.sum lines are "<module> <version> <hash>", and every version appears
// twice (once for the module, once for its /go.mod). Collapses that to
// module -> highest version, keeping the "v" prefix the file uses.
//
// Module paths are matched EXACTLY. A prefix match would let "github.com/x/y"
// pick up a line belonging to "github.com/x/yz" and report another module's
// version as installed, so the name is compared whole.
std::map<std::string, std::string> parse_go_sum(std::string_view text);
// "g:a" / "g:a:v" coordinates from build.gradle / build.gradle.kts DSL lines.
// Only dependency-configuration lines are read, never plugin id or version
// catalog lines, and only literal or range versions count as satisfiable; a
// version the file computes elsewhere (BOM, catalog, variable) is recorded
// as unverifiable rather than guessed.
std::vector<PkgSpec> parse_gradle_deps(std::string_view text);
std::vector<PkgSpec> parse_pom_deps(std::string_view text);

// Appends `specs` to `out`, deduplicating by canonical name and keeping the
// tightest lower bound.
void merge_specs(std::vector<PkgSpec>& out, const std::vector<PkgSpec>& specs);

// --------------------------------------------------------------- probing
// Installed-version discovery. All of these READ THE FILESYSTEM ONLY - no
// subprocess. That is deliberate: knowledge.cpp's tool allowlist is the only
// thing builtdiff will ever execute and every entry is a bare `--version`, so
// adding `pip list` would either need a new non-version allowlist entry or a
// `python3 -c`, and neither fits the security model.
//
// Absolute paths are resolved by the caller and are never recorded in a
// snapshot: only a name and a version leave these functions.
struct InstalledPkg {
  std::string name;
  std::string version;
};

// Result of looking for one installed package. Mirrors the shape of the native
// LibProbe so both kinds of dependency can flow through one comparison loop.
struct EcoProbe {
  bool found = false;
  std::string version;
  std::string via;  // site-packages | node_modules | cargo | go | .m2
};

// Locates `name` within the given ecosystem and reports what it found.
// `found` is only true when a real version was read: an ecosystem we could not
// search is reported as not found rather than optimistically satisfied.
EcoProbe probe_eco_package(Eco eco, std::string_view name, const std::string& project_root);

// python3.14 site-packages dirs, active venv first (VIRTUAL_ENV, then ./.venv,
// ./venv, ./env - each confirmed by a pyvenv.cfg), then the system
// site-packages for the interpreters in /usr/bin. Returns the version of
// `canonical_name` (PEP 503 normalised) or "" when it is absent.
std::string probe_pip_version(std::string_view canonical_name, const std::string& project_root);
std::vector<std::string> pip_search_dirs(const std::string& project_root);

// node_modules/<pkg>/package.json and node_modules/@scope/<pkg>/package.json,
// plus parent directories so a monorepo still resolves. Version of `name` or "".
std::string probe_npm_version(std::string_view name, const std::string& project_root);
std::vector<std::string> npm_search_dirs(const std::string& project_root);

// The project's own Cargo.lock is authoritative; otherwise ~/.cargo/registry/src
// and ~/.cargo/registry/cache hold "<name>-<version>" directories.
std::string probe_cargo_version(std::string_view name, const std::string& project_root);

// go.sum, then vendor/, then $GOMODCACHE or ~/go/pkg/mod.
std::string probe_go_version(std::string_view name, const std::string& project_root);

// ~/.m2/repository/<group/path>/<artifact>/<version>. Returns the highest
// version directory found, so a user with several does not look broken.
std::string probe_maven_version(std::string_view group, std::string_view artifact, const std::string& home);

}  // namespace bd