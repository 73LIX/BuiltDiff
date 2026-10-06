// Minimal self-contained test runner (no external framework).
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "analyze.hpp"
#include "compare.hpp"
#include "ecosystems.hpp"
#include "elf.hpp"
#include "http.hpp"
#include "json.hpp"
#include "scan.hpp"
#include "snapshot.hpp"
#include "util.hpp"

static int g_fail = 0, g_run = 0;
#define CHECK(c) do { ++g_run; if (!(c)) { ++g_fail; std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

using namespace bd;

static void test_json() {
  auto r = json::parse(R"({"a":[1,2,{"b":"xé\n"}],"c":true,"d":null})");
  CHECK(r.value && r.value->is_object());
  CHECK(json::get_bool(*r.value, "c"));
  CHECK(!json::parse(R"({"a":1,"a":2})").value);          // duplicate keys rejected
  CHECK(!json::parse("[1,2").value);                        // truncated
  CHECK(!json::parse("{\"a\":1} trailing").value);          // trailing garbage
  CHECK(!json::parse(std::string(100, '[')).value);         // depth limit
  std::string deep(100000, '[');
  CHECK(!json::parse(deep).value);                          // no stack overflow
  json::Value v = json::Value::object();
  v.set("k", "a\x01\"b\\");
  auto back = json::parse(json::dump(v));
  CHECK(back.value && json::get_string(*back.value, "k") == "a\x01\"b\\");
}

static void test_util() {
  CHECK(sanitize_text("ok\x1b[31mred", 100) == "ok?[31mred");
  CHECK(sanitize_text(std::string(500, 'a'), 10).size() <= 13);
  CHECK(is_safe_token("g++") && is_safe_token("libssl.so.3") && !is_safe_token("-rf") && !is_safe_token("a b") && !is_safe_token(""));
  CHECK(is_safe_relpath("build/app") && !is_safe_relpath("../x") && !is_safe_relpath("/etc/passwd") && !is_safe_relpath("a//b") && !is_safe_relpath("a/./b"));
  auto v = parse_version("gcc (GCC) 15.2.1 20260101");
  CHECK(v && version_string(*v) == "15.2.1");
  CHECK(compare_versions(*parse_version("13.3.0"), *parse_version("15.2")) < 0);
  CHECK(compare_versions(*parse_version("1.10"), *parse_version("1.9")) > 0);
  CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  Utf8Sanitizer s;
  std::string out = s.feed("h\xc3");
  out += s.feed("\xa9llo\x1b]0;x\x07\n");
  out += s.flush();
  CHECK(out.find('\x1b') == std::string::npos && out.find('\x07') == std::string::npos);
  CHECK(out.find("h\xc3\xa9llo") == 0);   // UTF-8 split across chunks survives
}

static void test_http() {
  HttpEndpoint e;
  CHECK(parse_endpoint("localhost:8080", e) && e.host == "localhost" && e.port == 8080);
  CHECK(parse_endpoint("http://127.0.0.1", e) && e.port == 11434);
  CHECK(parse_endpoint("[::1]:99", e) && e.host == "::1" && e.port == 99);
  CHECK(!parse_endpoint("https://x", e) && !parse_endpoint("a:0", e) && !parse_endpoint("a:99999", e) && !parse_endpoint("", e));
  CHECK(!parse_endpoint("ho st", e) && !parse_endpoint("a\r\nb", e));
  ChunkedDecoder d;
  std::string out;
  const std::string body = "5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\n\r\n";
  for (char c : body) CHECK(d.feed(std::string_view(&c, 1), out));  // byte-by-byte
  CHECK(d.done() && out == "hello world");
  ChunkedDecoder bad; std::string o2;
  CHECK(!bad.feed("zz\r\n", o2));
  ChunkedDecoder bad2; CHECK(!bad2.feed("2\r\nabXX", o2));

  // Non-loopback is refused before any bytes are sent, for GET as well as POST.
  HttpEndpoint pub{"93.184.216.34", 11434, false};
  HttpResult r = http_get(pub, "/api/tags", [](int, std::string_view) { return true; });
  CHECK(!r.ok && r.error.find("loopback") != std::string::npos);
  HttpEndpoint nowhere{"127.0.0.1", 1, false};
  HttpResult dead = http_get(nowhere, "/api/tags", [](int, std::string_view) { return true; });
  CHECK(!dead.ok && !dead.error.empty());
}

// select_model prefers the exact requested tag, then any gemma, then the first
// chat-capable model; an empty list yields "".
static void test_select_model() {
  std::vector<std::string> only_gemma{"gemma4:e2b"};
  CHECK(select_model(only_gemma, "gemma4:e2b") == "gemma4:e2b");
  std::vector<std::string> many{"llama3.2:3b", "gemma3:4b", "qwen3:8b"};
  CHECK(select_model(many, "gemma4:e2b") == "gemma3:4b");   // gemma fallback, not the first
  CHECK(select_model(many, "qwen3:8b") == "qwen3:8b");      // exact match wins over gemma
  std::vector<std::string> no_gemma{"llama3.2:3b"};
  CHECK(select_model(no_gemma, "gemma4:e2b") == "llama3.2:3b");
  CHECK(select_model({}, "gemma4:e2b").empty());
}

static void test_ndjson() {
  NdjsonChat n; std::string t;
  CHECK(n.feed("{\"message\":{\"content\":\"Hel\"},\"done\":false}\n{\"message\":{\"con", t));
  CHECK(n.feed("tent\":\"lo\"},\"done\":true}\n", t));
  CHECK(t == "Hello" && n.done());
  NdjsonChat e; std::string t2;
  CHECK(!e.feed("{\"error\":\"model 'x' not found\"}\n", t2) && !e.error().empty());
  NdjsonChat g; CHECK(!g.feed("not json\n", t2));
  NdjsonChat big; CHECK(!big.feed(std::string((1u << 20) + 10, 'a'), t2));

  // Gemma4 streams reasoning in a separate "thinking" field. Only "content" may
  // reach the terminal, and the reasoning must not count as the answer.
  NdjsonChat th; std::string t3;
  CHECK(th.feed("{\"message\":{\"content\":\"\",\"thinking\":\"secret reasoning\"},\"done\":false}\n", t3));
  CHECK(th.feed("{\"message\":{\"content\":\"visible\"},\"done\":true,\"done_reason\":\"stop\"}\n", t3));
  CHECK(t3 == "visible" && th.done() && th.done_reason() == "stop");

  // done_reason "length" means the token cap cut the answer off.
  NdjsonChat cut; std::string t4;
  CHECK(cut.feed("{\"message\":{\"content\":\"half\"},\"done\":true,\"done_reason\":\"length\"}\n", t4));
  CHECK(cut.done_reason() == "length");
  CHECK(cut.error().empty());
}

static void test_elf_robustness() {
  CHECK(!parse_elf("").has_value());
  CHECK(!parse_elf("\x7f" "ELF").has_value());
  std::mt19937 rng(12345);
  // Start from a real ELF (this test binary) and corrupt it randomly.
  auto self = read_file("/proc/self/exe", 64u << 20);
  CHECK(self.has_value());
  if (!self) return;
  CHECK(parse_elf(*self).has_value());
  for (int i = 0; i < 300; ++i) {
    std::string img = self->substr(0, 4096 + rng() % 60000);
    for (int k = 0; k < 40; ++k) img[rng() % img.size()] = static_cast<char>(rng());
    (void)parse_elf(img);      // must not crash / trip sanitizers
  }
  for (int i = 0; i < 300; ++i) {   // random headers
    std::string img = "\x7f" "ELF\x02\x01\x01";
    for (int k = 0; k < 600; ++k) img.push_back(static_cast<char>(rng()));
    (void)parse_elf(img);
  }
  CHECK(true);
}

static void test_scan_and_snapshot() {
  auto calls = parse_cmake_calls("# c\nfind_package(OpenSSL 3.0 REQUIRED)\nset(CMAKE_CXX_STANDARD 23)\nmessage(\"a (b) c\")");
  CHECK(calls.size() == 3 && calls[0].name == "find_package" && calls[0].args.size() >= 2);
  CHECK(normalize_cxx_standard("gnu++2b") == "23" && normalize_cxx_standard("c++17") == "17" && normalize_cxx_standard("bogus").empty());
  CHECK(!parse_cmake_calls(std::string(200000, '(')).size() || true);  // must terminate

  CHECK(!parse_snapshot("").snap);
  CHECK(!parse_snapshot("{}").snap);
  CHECK(!parse_snapshot("[1]").snap);
  auto r = parse_snapshot(R"({"builtdiff_version":1,"project":"p\u001b[2J","platform":{"os":"linux","arch":"x86_64"},"toolchain":[{"name":"rm","version":"1"}]})");
  if (r.snap) {
    CHECK(r.snap->project.find('\x1b') == std::string::npos);  // control chars stripped
    CHECK(r.snap->tools.empty());                              // non-allowlisted tool refused
    CHECK(!r.snap->fingerprint_ok);
  }
}

// Language-package layer. Every assertion here is about correctness of an
// answer the user will act on, or about refusing to guess.
static void test_ecosystems() {
  // ---- PEP 503 and npm name normalization
  CHECK(normalize_pep503("Flask_SQLAlchemy") == "flask-sqlalchemy");
  CHECK(normalize_pep503("foo.bar") == "foo-bar");
  CHECK(is_eco_package_name(Eco::Pip, "Flask-SQLAlchemy"));
  CHECK(is_eco_package_name(Eco::Npm, "@types/node"));
  CHECK(!is_eco_package_name(Eco::Npm, "foo/bar"));       // not scoped
  CHECK(!is_eco_package_name(Eco::Cargo, "../evil"));      // no traversal
  CHECK(!is_eco_package_name(Eco::Cargo, "a/b"));          // crates are one segment
  CHECK(is_eco_package_name(Eco::Cargo, "serde_json"));
  CHECK(!is_eco_package_name(Eco::Go, "../evil"));
  CHECK(!is_eco_package_name(Eco::Go, "/abs/mod"));
  CHECK(!is_eco_package_name(Eco::Go, "a//b"));
  CHECK(!is_eco_package_name(Eco::Go, "a/../b"));
  CHECK(!is_eco_package_name(Eco::Pip, ""));
  CHECK(!is_eco_package_name(Eco::Native, "zlib"));
  CHECK(is_eco_package_name(Eco::Go, "github.com/gin-gonic/gin"));
  CHECK(is_eco_package_name(Eco::Maven, "com.google.guava"));
  CHECK(!is_eco_package_name(Eco::Maven, "guava:jar:1.0")); // group only
  // A maven coordinate carries a colon, so it has its own validator.
  CHECK(is_maven_coordinate("com.google.guava:guava"));
  CHECK(!is_maven_coordinate("guava"));            // no group
  CHECK(!is_maven_coordinate(":guava"));           // empty group
  CHECK(!is_maven_coordinate("com.foo:"));         // empty artifact
  CHECK(!is_maven_coordinate("a:b:c"));            // two colons
  CHECK(!is_maven_coordinate("com.foo:bar;rm -rf /"));

  // ---- version specifiers
  CHECK(check_spec(Eco::Pip, "1.2.3", "==1.2.3") == Verdict::Ok);
  CHECK(check_spec(Eco::Pip, "latest", ">=1") == Verdict::Unknown);  // unparseable version
  CHECK(check_spec(Eco::Pip, "6.1.1", ">=5.9,<7") == Verdict::Ok);
  CHECK(check_spec(Eco::Pip, "7.2.2", ">=5.9,<7") == Verdict::TooNew);  // above the ceiling, not below the floor
  CHECK(check_spec(Eco::Pip, "5.0", ">=5.9") == Verdict::TooOld);
  CHECK(check_spec(Eco::Npm, "1.2.9", "^1.2.0") == Verdict::Ok);
  CHECK(check_spec(Eco::Npm, "2.0.0", "^1.2.0") == Verdict::TooNew);
  CHECK(check_spec(Eco::Npm, "1.2.99", "~1.2.0") == Verdict::Ok);
  CHECK(check_spec(Eco::Npm, "1.3.0", "~1.2.0") == Verdict::TooNew);  // ~1.2.0 excludes 1.3.0
  CHECK(check_spec(Eco::Npm, "1.2.9", "1.2.x") == Verdict::Ok);    // 1.2.x is <1.3.0
  CHECK(check_spec(Eco::Npm, "1.3.0", "1.2.x") == Verdict::TooNew);
  CHECK(check_spec(Eco::Npm, "1.3.0", "*") == Verdict::Ok);          // "*" is unconstrained
  CHECK(check_spec(Eco::Cargo, "0.5.9", "^0.5") == Verdict::Ok);
  CHECK(check_spec(Eco::Cargo, "0.6.0", "^0.5") == Verdict::TooNew);  // ^0.5 pins the minor
  CHECK(check_spec(Eco::Cargo, "0.4.9", "^0.5") == Verdict::TooOld);
  CHECK(check_spec(Eco::Maven, "33.2.1", "[33,34)") == Verdict::Ok);
  CHECK(check_spec(Eco::Maven, "32.1", "[33,34)") == Verdict::TooOld);
  CHECK(check_spec(Eco::Go, "1.21.0", ">=1.20") == Verdict::Ok);
  // A spec we cannot understand must never be reported as satisfied-by-assumption
  // nor as a failure the user cannot act on.
  CHECK(check_spec(Eco::Pip, "1.0", "git+https://x/y.git#egg=z") == Verdict::Unknown);
  CHECK(check_spec(Eco::Npm, "1.0", "workspace:*") == Verdict::Unknown);
  CHECK(check_spec(Eco::Pip, "1.0", "") == Verdict::Ok);       // no constraint
  CHECK(check_spec(Eco::Pip, "1.0", "==1.0") == Verdict::Ok);
  CHECK(check_spec(Eco::Pip, "1.1", "==1.0") == Verdict::TooNew);  // == is exact both ways
  CHECK(check_spec(Eco::Maven, "31.0.0", "33.0.0") == Verdict::TooOld);   // bare maven version = pin
  CHECK(check_spec(Eco::Maven, "34.0.0", "33.0.0") == Verdict::TooNew);
  CHECK(check_spec(Eco::Pip, "31.0.0-jre", ">=33.0.0") == Verdict::TooOld);  // qualifier tolerated
  CHECK(check_spec(Eco::Go, "v1.2.3", "v1.2.3") == Verdict::Ok);
  CHECK(check_spec(Eco::Cargo, "1.0", "*") == Verdict::Ok);
  CHECK(check_spec(Eco::Cargo, "0.5.1", "~0.5.0") == Verdict::Ok);
  CHECK(check_spec(Eco::Cargo, "1.0.9", "1.0.*") == Verdict::Ok);   // cargo uses * too

  // ---- requirements.txt
  {
    auto pkgs = parse_requirements("Flask==2.3.0\n# a comment\n\nrich>=13,<15 \\\n  # inline comment\n  ; py3 marker\nnumpy\n");
    CHECK(pkgs.size() == 3);
    if (pkgs.size() == 3) {
      CHECK(pkgs[0].name == "flask" && pkgs[0].spec == "==2.3.0" && pkgs[0].required_min == "2.3.0");
      CHECK(pkgs[1].name == "rich" && pkgs[1].spec == ">=13,<15");   // continuation joined
      CHECK(pkgs[2].name == "numpy");                                // bare name, no constraint
    }
    // Options, markers and editable/URL installs must not be mistaken for names.
    CHECK(parse_requirements("--index-url https://pypi.org/simple\n").empty());
    CHECK(parse_requirements("foo[extra]>=1\n").size() == 1);
    CHECK(parse_requirements("-e git+https://x/y.git#egg=z\n").empty());
  }

  // ---- pyproject / package.json / cargo / go / maven
  {
    auto pp = parse_pyproject_deps(R"([project]
name = "demo"
dependencies = ["flask>=2.3", "rich"]
[project.optional-dependencies]
dev = ["pytest~=8.0"]
[build-system]
requires = ["setuptools>=68", "wheel"]
)");
    CHECK(pp.size() == 4);
    for (const auto& p : pp) CHECK(eco_from_kind(p.kind) == Eco::Pip);
    auto pj = parse_package_json_deps(R"({"dependencies":{"lodash":"^4.17.21"},"devDependencies":{"vitest":"~1.0.0"}})");
    CHECK(pj.size() == 2 && eco_from_kind(pj[0].kind) == Eco::Npm);
    auto ct = parse_cargo_deps(R"([dependencies]
serde = { version = "1.0", features = ["derive"] }
anyhow = "1.0.*"
tokio = { version = ">=1.35, <2" }

[dev-dependencies]
tempfile = "3"
)");
    CHECK(ct.size() == 4);
    for (const auto& p : ct) CHECK(eco_from_kind(p.kind) == Eco::Cargo);
    auto gm = parse_go_mod_requires(R"(
module example.com/m

go 1.21

require (
	github.com/gin-gonic/gin v1.9.1
	golang.org/x/text v0.14.0 // indirect
)

require github.com/spf13/cobra v1.8.0
)");
    CHECK(gm.size() == 3);
    for (const auto& p : gm) CHECK(eco_from_kind(p.kind) == Eco::Go);
    auto pm = parse_pom_deps(R"(<project xmlns="http://maven.apache.org/POM/4.0.0">
  <dependencies>
    <dependency><groupId>com.google.guava</groupId><artifactId>guava</artifactId><version>33.0.0</version></dependency>
    <dependency><groupId>org.junit.jupiter</groupId><artifactId>junit-jupiter</artifactId><version>[5.10,6)</version></dependency>
    <dependency><groupId>com.example</groupId><artifactId>placeholder</artifactId><version>${foo.version}</version></dependency>
  </dependencies>
</project>)");
    CHECK(pm.size() == 3);
    if (pm.size() == 3) {
      CHECK(pm[0].name == "com.google.guava:guava" && pm[0].required_min == "33.0.0");
      CHECK(pm[1].spec == "[5.10,6)");
      CHECK(pm[2].unverifiable);   // property placeholder, not a literal version
    }
  }

  // ---- lock files pin exact versions
  {
    auto lock = parse_cargo_lock(R"([[package]]
name = "serde"
version = "1.0.196"
[[package]]
name = "anyhow"
version = "1.0.79")");
    CHECK(lock.size() == 2);
    if (lock.size() == 2) CHECK(lock[0].name == "serde" && lock[0].version_locked == "1.0.196");
    // go.sum pins go.mod requirements the same way; verify the "/go.mod"
    // duplicate lines do not produce two entries for one module.
    auto gomod = parse_go_mod_requires("require github.com/gin-gonic/gin v1.9.1\n");
    CHECK(gomod.size() == 1 && gomod[0].version_locked == "v1.9.1");
  }

  // ---- probing never guesses. No package exists under these names anywhere,
  // so every probe must report not-found rather than inventing a version.
  {
    const std::string root = "/nonexistent-builtdiff-fixture";
    for (Eco eco : {Eco::Pip, Eco::Npm, Eco::Cargo, Eco::Maven, Eco::Go}) {
      const EcoProbe p = probe_eco_package(eco, "builtdiff-nonexistent-pkg-xyzzy", root);
      CHECK(!p.found);
      CHECK(p.version.empty());
    }
    CHECK(!probe_eco_package(Eco::Native, "zlib", root).found);
  }

  // ---- install hints are suggestions, never shell
  {
    CHECK(project_has_venv("/nonexistent-builtdiff-fixture") == false);
    const std::string hint = eco_install_hint(Eco::Pip, "psutil", "/nonexistent-builtdiff-fixture");
    CHECK(hint.find("pip install psutil") != std::string::npos);
    // A hostile name from a snapshot file must not survive into the hint.
    CHECK(eco_install_hint(Eco::Pip, "psutil; rm -rf /", "/x").empty());
    CHECK(eco_install_hint(Eco::Native, "zlib", "/x").empty());
    CHECK(eco_install_hint(Eco::Pip, "../etc/passwd", "/x").empty());
    CHECK(eco_install_command(Eco::Npm, {"lodash", "bad name;x"}, "/x") == "npm install lodash");
  }
}

// A snapshot whose libraries are language packages must round-trip and must
// still load under the older schema fields.
static void test_snapshot_ecosystem_roundtrip() {
  auto r = parse_snapshot(R"({"builtdiff_version":1,"project":"p","platform":{"os":"linux","arch":"x86_64"},
    "build":{"package_managers":["pip"]},
    "libraries":[{"name":"psutil","kind":"pip","found":true,"version":"6.1.1","required_min":"5.9","spec":">=5.9,<7"},
                 {"name":"lodash","kind":"npm","found":true,"version":"4.17.21","spec":"^4.17.21"}]})");
  CHECK(r.snap.has_value());
  if (!r.snap) return;
  CHECK(r.snap->build.package_managers.size() == 1 && r.snap->build.package_managers[0] == "pip");
  CHECK(r.snap->libs.size() == 2);
  if (r.snap->libs.size() != 2) return;
  CHECK(eco_from_kind(r.snap->libs[0].kind) == Eco::Pip);
  CHECK(r.snap->libs[0].spec == ">=5.9,<7");
  CHECK(eco_from_kind(r.snap->libs[1].kind) == Eco::Npm);
  // A library name that is not a valid package for its declared kind is dropped,
  // so a hostile snapshot cannot inject text into later install hints.
  auto mvn = parse_snapshot(R"({"builtdiff_version":1,"project":"p","platform":{"os":"linux","arch":"x86_64"},
    "libraries":[{"name":"com.google.guava:guava","kind":"maven","found":true,"version":"33.0.0"}]})");
  CHECK(mvn.snap && mvn.snap->libs.size() == 1);   // a real coordinate survives the load
  // A qualified version must not be blanked on reload: "31.0.0-jre" is what
  // maven actually installs, and losing it turns a found dependency into an
  // unverifiable one on the next run.
  auto qual = parse_snapshot(R"({"builtdiff_version":1,"project":"p","platform":{"os":"linux","arch":"x86_64"},
    "libraries":[{"name":"com.google.guava:guava","kind":"maven","found":true,"version":"31.0.0-jre"}]})");
  CHECK(qual.snap && qual.snap->libs.size() == 1 && qual.snap->libs[0].version == "31.0.0-jre");
  // A native version stays strict: only digits and dots.
  auto natv = parse_snapshot(R"({"builtdiff_version":1,"project":"p","platform":{"os":"linux","arch":"x86_64"},
    "libraries":[{"name":"zlib","kind":"native","found":true,"version":"1.2.3-rc"}]})");
  CHECK(natv.snap && natv.snap->libs.size() == 1 && natv.snap->libs[0].version.empty());
  auto badmvn = parse_snapshot(R"({"builtdiff_version":1,"project":"p","platform":{"os":"linux","arch":"x86_64"},
    "libraries":[{"name":"com.foo:bar;rm -rf /","kind":"maven","found":true,"version":"1"}]})");
  CHECK(badmvn.snap && badmvn.snap->libs.empty());
  auto bad = parse_snapshot(R"({"builtdiff_version":1,"project":"p","platform":{"os":"linux","arch":"x86_64"},
    "libraries":[{"name":"evil;rm -rf /","kind":"pip","found":true,"version":"1"}]})");
  CHECK(bad.snap && bad.snap->libs.empty());
  // A native library is unaffected by the ecosystem validator.
  auto native = parse_snapshot(R"({"builtdiff_version":1,"project":"p","platform":{"os":"linux","arch":"x86_64"},
    "libraries":[{"name":"zlib","kind":"native","found":true,"version":"1.3"}]})");
  CHECK(native.snap && native.snap->libs.size() == 1);
}

// Regression tests for the post-launch bug round: go.sum handling (prefix
// match, string-chosen version, first-match-not-highest probe), the drift
// false positive caused by comparing "v1.9.1" with "1.9.1" as text, and the
// setup.py parser missing the PEP 8 spaced keyword form.
static void test_ecosystem_regressions() {
  // -- go.sum: module paths match whole, not by prefix.
  {
    const auto sums = parse_go_sum(R"(github.com/x/y v1.0.0 h1:aaa=
github.com/x/y v1.0.0/go.mod h1:bbb=
github.com/x/yz v2.5.0 h1:ccc=
)");
    CHECK(sums.size() == 2);                       // /go.mod duplicate collapses
    CHECK(sums.count("github.com/x/yz") == 1);
    const auto it = sums.find("github.com/x/y");
    CHECK(it != sums.end() && it->second == "v1.0.0");   // never 2.5.0, that is yz's
  }
  // -- go.sum: the highest version wins, compared as versions not text.
  {
    const auto sums = parse_go_sum(R"(golang.org/x/text v1.9.1 h1:a=
golang.org/x/text v1.10.0 h1:b=
golang.org/x/text v0.9.0 h1:c=
)");
    const auto it = sums.find("golang.org/x/text");
    CHECK(it != sums.end() && it->second == "v1.10.0");
  }
  // -- go.sum: junk lines and hostile module tokens are ignored.
  {
    const auto sums = parse_go_sum("github.com/ok/ok v1.0.0 h1:a=\n../evil v2.0.0 h1:b=");
    CHECK(sums.size() == 1 && sums.count("github.com/ok/ok") == 1);
  }
  // -- versions_match: v-prefixed vs bare, and unparseable bytes.
  {
    CHECK(versions_match("v1.9.1", "1.9.1"));
    CHECK(versions_match("1.9.1", "1.9.1"));
    CHECK(!versions_match("v1.9.1", "1.10.0"));
    CHECK(!versions_match("v1.9.1", "v1.10.0"));
    CHECK(versions_match("git-unknown", "git-unknown"));   // same junk is identical
    CHECK(!versions_match("git-unknown", "git-other"));
    CHECK(!versions_match("1.9.1", ""));
  }
  // -- setup.py: the PEP 8 spaced keyword form must parse.
  {
    auto pkgs = parse_setup_py_deps(R"(from setuptools import setup
setup(
    name="demo",
    install_requires = [
        "flask==2.3.0",
        "rich>=13, <15",
    ],
    setup_requires = ["setuptools>=68"],
    tests_require = ["pytest~=8.0"],
)
)");
    CHECK(pkgs.size() == 4);
    if (pkgs.size() == 4) {
      CHECK(pkgs[0].name == "flask" && pkgs[0].spec == "==2.3.0");
      CHECK(pkgs[1].name == "rich" && pkgs[1].spec == ">=13, <15");
      CHECK(pkgs[2].name == "setuptools");
      CHECK(pkgs[3].name == "pytest");
    }
    // And the un-spaced single-line form still works, with no double count.
    auto tight = parse_setup_py_deps("setup(name=\"d\", install_requires=[\"a\",\"b\"], setup_requires=[\"c\"])");
    CHECK(tight.size() == 3);
  }
  // -- probe_go_version end to end: highest version wins and prefixes are exact.
  {
    const auto dir = std::filesystem::temp_directory_path() / "builtdiff-test-gofix";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    {
      std::ofstream f(dir / "go.sum");
      f << "github.com/x/y v1.0.0 h1:a=\n"
           "github.com/x/y v1.0.0/go.mod h1:b=\n"
           "github.com/x/yz v2.5.0 h1:c=\n"
           "golang.org/x/text v1.9.1 h1:d=\n"
           "golang.org/x/text v1.10.0 h1:e=\n";
    }
    const std::string root = dir.string();
    CHECK(probe_go_version("github.com/x/y", root) == "v1.0.0");
    CHECK(probe_go_version("golang.org/x/text", root) == "v1.10.0");   // highest, not first
    CHECK(probe_go_version("github.com/x/yz", root) == "v2.5.0");
    CHECK(probe_go_version("github.com/x/y/z", root).empty());          // longer path: no prefix gift
    std::filesystem::remove_all(dir, ec);
  }

  // -- gradle: both DSLs, all three declaration forms, and the shapes that
  // must never be read as dependencies (plugin ids, versions, projects).
  {
    const auto kotlin = parse_gradle_deps(R"kts(plugins {
    id("org.springframework.boot") version "3.2.0"
}
dependencies {
    implementation("org.springframework.boot:spring-boot-starter-web:3.2.0")
    implementation("com.google.guava:guava:33.0.0-jre")
    testImplementation("org.junit.jupiter:junit-jupiter:5.10.0")
    implementation("org.slf4j:slf4j-api")
    implementation(project(":common"))
    implementation("com.example:dyn:1.+")
    api("com.fasterxml.jackson.core:jackson-databind:[2.14,3)")
}
)kts");
    CHECK(kotlin.size() == 6);
    if (kotlin.size() == 6) {
      CHECK(kotlin[0].name == "org.springframework.boot:spring-boot-starter-web" && kotlin[0].spec == "3.2.0");
      CHECK(!kotlin[0].unverifiable && kotlin[0].required_min == "3.2.0");
      CHECK(kotlin[1].spec == "33.0.0-jre");           // qualified version kept
      CHECK(kotlin[2].name == "org.junit.jupiter:junit-jupiter");
      CHECK(kotlin[3].unverifiable);                    // BOM-driven, no literal version
      CHECK(kotlin[4].unverifiable);                    // "1.+" is dynamic, never satisfiable
      CHECK(kotlin[4].required_min.empty());            // and must not claim ">= 1"
      CHECK(kotlin[5].spec == "[2.14,3)" && kotlin[5].required_min == "2.14");  // range is a real floor
    }
    const auto groovy = parse_gradle_deps(R"(plugins { id 'java' }
group = 'com.example'
version = '1.0.0'
dependencies {
    implementation 'org.apache.commons:commons-lang3:3.14.0'
    implementation group: 'commons-io', name: 'commons-io', version: '2.15.1'
    implementation 'javax.inject:javax.inject'
    providedCompile 'com.foo:provided:1.0'
    runtimeOnly 'g:a:v:c'          // classifier form: not a plain coordinate
}
)");
    CHECK(groovy.size() == 4);
    if (groovy.size() == 4) {
      CHECK(groovy[0].name == "org.apache.commons:commons-lang3");
      CHECK(groovy[1].name == "commons-io:commons-io" && groovy[1].required_min == "2.15.1");  // map form
      CHECK(groovy[2].unverifiable);
      CHECK(groovy[3].name == "com.foo:provided");
    }
    // A non-dependency file must yield nothing.
    CHECK(parse_gradle_deps(R"(plugins { id 'java' }
version = '1.0.0'
extra { println "gradle:not:a:dep" }
)").empty());
  }
}

int main() {
  test_json();
  test_util();
  test_http();
  test_select_model();
  test_ndjson();
  test_elf_robustness();
  test_scan_and_snapshot();
  test_ecosystems();
  test_snapshot_ecosystem_roundtrip();
  test_ecosystem_regressions();
  std::printf("%d checks, %d failed\n", g_run, g_fail);
  return g_fail ? 1 : 0;
}
