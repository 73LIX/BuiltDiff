#include "util.hpp"

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace bd {

// ------------------------------------------------------------- strings

std::string_view trim(std::string_view s) noexcept {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
    s.remove_suffix(1);
  return s;
}

std::string to_lower(std::string_view s) {
  std::string r(s);
  for (char& c : r)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return r;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

std::vector<std::string> split(std::string_view s, char sep, bool keep_empty) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= s.size()) {
    std::size_t end = s.find(sep, start);
    if (end == std::string_view::npos) end = s.size();
    if (end > start || keep_empty) out.emplace_back(s.substr(start, end - start));
    start = end + 1;
  }
  return out;
}

std::string sanitize_text(std::string_view in, std::size_t max_len) {
  std::string out;
  out.reserve(std::min(in.size(), max_len));
  for (unsigned char c : in) {
    if (out.size() >= max_len) break;
    out.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?');
  }
  return out;
}

namespace {
bool is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }
}  // namespace

std::string Utf8Sanitizer::feed(std::string_view in) {
  std::string buf = std::move(pending_);
  pending_.clear();
  buf.append(in);
  std::string out;
  out.reserve(buf.size());
  const std::size_t n = buf.size();
  std::size_t i = 0;
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(buf[i]);
    if (c < 0x80) {
      if (c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F)) out.push_back(static_cast<char>(c));
      ++i;
      continue;
    }
    std::size_t len = 0;
    if (c >= 0xC2 && c <= 0xDF) len = 2;
    else if (c >= 0xE0 && c <= 0xEF) len = 3;
    else if (c >= 0xF0 && c <= 0xF4) len = 4;
    else { out.push_back('?'); ++i; continue; }  // stray continuation / invalid lead
    if (i + len > n) {
      // Possibly a sequence split across network chunks: keep it for next feed().
      bool plausible = true;
      for (std::size_t k = i + 1; k < n; ++k)
        if (!is_cont(static_cast<unsigned char>(buf[k]))) plausible = false;
      if (plausible) { pending_.assign(buf, i, n - i); break; }
      out.push_back('?');
      ++i;
      continue;
    }
    bool ok = true;
    for (std::size_t k = 1; k < len; ++k)
      if (!is_cont(static_cast<unsigned char>(buf[i + k]))) ok = false;
    if (ok) {
      const unsigned char c1 = static_cast<unsigned char>(buf[i + 1]);
      if (c == 0xE0 && c1 < 0xA0) ok = false;       // overlong
      if (c == 0xED && c1 > 0x9F) ok = false;       // surrogates
      if (c == 0xF0 && c1 < 0x90) ok = false;       // overlong
      if (c == 0xF4 && c1 > 0x8F) ok = false;       // > U+10FFFF
    }
    if (!ok) { out.push_back('?'); ++i; continue; }
    // Drop C1 controls (U+0080..U+009F) and bidi override/isolate controls.
    const unsigned char c1 = static_cast<unsigned char>(buf[i + 1]);
    const unsigned char c2 = len > 2 ? static_cast<unsigned char>(buf[i + 2]) : 0;
    bool drop = false;
    if (c == 0xC2 && c1 >= 0x80 && c1 <= 0x9F) drop = true;
    if (c == 0xE2 && c1 == 0x80 && c2 >= 0xAA && c2 <= 0xAE) drop = true;  // U+202A..202E
    if (c == 0xE2 && c1 == 0x81 && c2 >= 0xA6 && c2 <= 0xA9) drop = true;  // U+2066..2069
    if (!drop) out.append(buf, i, len);
    i += len;
  }
  return out;
}

std::string Utf8Sanitizer::flush() {
  std::string p = std::move(pending_);
  pending_.clear();
  return p.empty() ? std::string() : std::string("?");
}

bool is_safe_token(std::string_view s, std::size_t max_len) noexcept {
  if (s.empty() || s.size() > max_len || s.front() == '-') return false;
  for (unsigned char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == '+' || c == ':' || c == '@' || c == '-';
    if (!ok) return false;
  }
  return true;
}

bool is_safe_relpath(std::string_view s) noexcept {
  if (s.empty() || s.size() > 256) return false;
  for (unsigned char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == '+' || c == '@' || c == '-' || c == ' ' || c == '/';
    if (!ok) return false;
  }
  std::size_t start = 0;
  while (start <= s.size()) {
    std::size_t end = s.find('/', start);
    if (end == std::string_view::npos) end = s.size();
    std::string_view part = s.substr(start, end - start);
    if (part.empty() || part == "." || part == "..") return false;
    start = end + 1;
  }
  return true;
}

// ---------------------------------------------------------------- files

namespace {

struct Fd {
  int fd = -1;
  explicit Fd(int f = -1) : fd(f) {}
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  ~Fd() { if (fd >= 0) ::close(fd); }
  int release() { int f = fd; fd = -1; return f; }
  void reset(int f) { if (fd >= 0) ::close(fd); fd = f; }
};

std::optional<std::string> read_fd_bounded(int fd, std::size_t max_bytes) {
  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) return std::nullopt;
  if (st.st_size < 0 || static_cast<std::uint64_t>(st.st_size) > max_bytes) return std::nullopt;
  std::string data;
  data.reserve(static_cast<std::size_t>(st.st_size));
  std::array<char, 16384> buf;
  while (true) {
    ssize_t n = ::read(fd, buf.data(), buf.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::nullopt;
    }
    if (n == 0) break;
    if (data.size() + static_cast<std::size_t>(n) > max_bytes) return std::nullopt;  // grew while reading
    data.append(buf.data(), static_cast<std::size_t>(n));
  }
  return data;
}

}  // namespace

std::optional<std::string> read_file(const std::string& path, std::size_t max_bytes) {
  Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY));
  if (fd.fd < 0) return std::nullopt;
  return read_fd_bounded(fd.fd, max_bytes);
}

int open_under(const std::string& root, std::string_view rel) {
  if (!is_safe_relpath(rel)) return -1;
  Fd dir(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (dir.fd < 0) return -1;
  const auto parts = split(rel, '/');
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const bool last = (i + 1 == parts.size());
    int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY | (last ? O_NONBLOCK : O_DIRECTORY);
    Fd next(::openat(dir.fd, parts[i].c_str(), flags));
    if (next.fd < 0) return -1;  // includes ELOOP for symlinks
    if (last) return next.release();
    dir.reset(next.release());
  }
  return -1;
}

std::optional<std::string> read_file_under(const std::string& root, std::string_view rel,
                                           std::size_t max_bytes) {
  Fd fd(open_under(root, rel));
  if (fd.fd < 0) return std::nullopt;
  return read_fd_bounded(fd.fd, max_bytes);
}

bool write_file_atomic(const std::string& path, std::string_view data, unsigned mode) {
  std::string dir = ".";
  if (auto slash = path.rfind('/'); slash != std::string::npos) dir = slash == 0 ? "/" : path.substr(0, slash);
  std::string tmpl = (dir == "/" ? "" : dir) + "/.builtdiff.tmp.XXXXXX";
  std::vector<char> name(tmpl.begin(), tmpl.end());
  name.push_back('\0');
  int fd = ::mkstemp(name.data());
  if (fd < 0) return false;
  bool ok = ::fchmod(fd, static_cast<mode_t>(mode)) == 0;
  std::size_t off = 0;
  while (ok && off < data.size()) {
    ssize_t n = ::write(fd, data.data() + off, data.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      ok = false;
    } else {
      off += static_cast<std::size_t>(n);
    }
  }
  if (ok && ::fsync(fd) != 0) ok = false;
  if (::close(fd) != 0) ok = false;
  if (ok && ::rename(name.data(), path.c_str()) != 0) ok = false;
  if (!ok) ::unlink(name.data());
  return ok;
}

bool file_exists(const std::string& path) noexcept {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0;
}

// ----------------------------------------------------------------- misc

namespace {
constexpr std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
}  // namespace

std::string sha256_hex(std::string_view data) {
  std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::string msg(data);
  const std::uint64_t bitlen = static_cast<std::uint64_t>(data.size()) * 8u;
  msg.push_back(static_cast<char>(0x80));
  while (msg.size() % 64 != 56) msg.push_back('\0');
  for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bitlen >> (i * 8)) & 0xFF));

  for (std::size_t off = 0; off < msg.size(); off += 64) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      const auto* p = reinterpret_cast<const unsigned char*>(msg.data() + off + static_cast<std::size_t>(i) * 4);
      w[i] = (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
             (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
    }
    for (int i = 16; i < 64; ++i) {
      std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      std::uint32_t ch = (e & f) ^ (~e & g);
      std::uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
      std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
      std::uint32_t t2 = S0 + mj;
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  static const char* hexd = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (std::uint32_t v : h)
    for (int s = 28; s >= 0; s -= 4) out.push_back(hexd[(v >> s) & 0xF]);
  return out;
}

std::string iso_utc_now() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::optional<Version> parse_version(std::string_view text) {
  std::size_t i = 0;
  while (i < text.size() && !(text[i] >= '0' && text[i] <= '9')) ++i;
  if (i >= text.size()) return std::nullopt;
  Version v;
  while (i < text.size() && v.parts.size() < 6) {
    std::size_t j = i;
    while (j < text.size() && text[j] >= '0' && text[j] <= '9') ++j;
    if (j == i) break;
    std::uint64_t n = 0;
    if (j - i > 18) return std::nullopt;
    auto r = std::from_chars(text.data() + i, text.data() + j, n);
    if (r.ec != std::errc()) return std::nullopt;
    v.parts.push_back(n);
    i = j;
    if (i + 1 < text.size() && text[i] == '.' && text[i + 1] >= '0' && text[i + 1] <= '9') ++i;
    else break;
  }
  return v;
}

int compare_versions(const Version& a, const Version& b) noexcept {
  const std::size_t n = std::max(a.parts.size(), b.parts.size());
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint64_t x = i < a.parts.size() ? a.parts[i] : 0;
    const std::uint64_t y = i < b.parts.size() ? b.parts[i] : 0;
    if (x < y) return -1;
    if (x > y) return 1;
  }
  return 0;
}

std::string version_string(const Version& v) {
  std::string out;
  for (std::size_t i = 0; i < v.parts.size(); ++i) {
    if (i) out.push_back('.');
    out += std::to_string(v.parts[i]);
  }
  return out;
}

// ------------------------------------------------------------- Redactor

namespace {
void replace_all(std::string& s, const std::string& from, const std::string& to) {
  if (from.empty()) return;
  std::size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
}
}  // namespace

Redactor::Redactor() {
  if (const char* home = std::getenv("HOME"); home && std::strlen(home) > 1 && std::strcmp(home, "/") != 0)
    subs_.emplace_back(home, "~");
  std::string user;
  if (const char* u = std::getenv("USER"); u) user = u;
  if (user.empty())
    if (struct passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_name) user = pw->pw_name;
  if (user.size() >= 3 && user != "root") subs_.emplace_back(user, "<user>");
  char host[256] = {0};
  if (::gethostname(host, sizeof host - 1) == 0 && std::strlen(host) >= 3 &&
      std::strcmp(host, "localhost") != 0)
    subs_.emplace_back(host, "<host>");
}

std::string Redactor::apply(std::string_view s) const {
  std::string out(s);
  for (const auto& [from, to] : subs_) replace_all(out, from, to);
  return out;
}

}  // namespace bd
