// Minimal self-contained test runner (no external framework).
#include <cstdio>
#include <random>
#include <string>

#include "analyze.hpp"
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

int main() {
  test_json();
  test_util();
  test_http();
  test_ndjson();
  test_elf_robustness();
  test_scan_and_snapshot();
  std::printf("%d checks, %d failed\n", g_run, g_fail);
  return g_fail ? 1 : 0;
}
