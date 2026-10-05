// util.hpp - small, dependency-free helpers shared by all modules.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bd {

inline constexpr const char* kToolVersion = "0.1.0";

// ------------------------------------------------------------- strings
std::string_view trim(std::string_view s) noexcept;
std::string to_lower(std::string_view s);
std::vector<std::string> split(std::string_view s, char sep, bool keep_empty = false);
bool iequals(std::string_view a, std::string_view b) noexcept;

// Replace everything that is not printable ASCII with '?' and truncate to
// max_len bytes. Used for EVERY string that comes from a snapshot, a child
// process or the network before it reaches a terminal or a model prompt, so
// terminal escape sequences and control characters can never get through.
std::string sanitize_text(std::string_view in, std::size_t max_len = 256);

// Streaming sanitizer for model output: keeps valid UTF-8 text, '\n' and '\t';
// drops every other control character (ESC, C1 controls, DEL ...), so a
// prompt-injected "\x1b]0;..." cannot manipulate the user's terminal.
class Utf8Sanitizer {
 public:
  std::string feed(std::string_view in);
  std::string flush();

 private:
  std::string pending_;
};

// A "token" is a conservative identifier: [A-Za-z0-9._+:@-], 1..max_len chars,
// must not start with '-' (so it can never be parsed as a command-line option).
bool is_safe_token(std::string_view s, std::size_t max_len = 64) noexcept;

// Relative path made of [A-Za-z0-9._+@ /-] with no empty, "." or ".." parts.
bool is_safe_relpath(std::string_view s) noexcept;

// ---------------------------------------------------------------- files
// Reads a regular file (no FIFOs/devices) of at most max_bytes. nullopt on error
// or if the file is bigger than the limit.
std::optional<std::string> read_file(const std::string& path, std::size_t max_bytes);

// Like read_file but `rel` must be a safe relative path, and neither the final
// component nor any parent directory below `root` may be a symlink.
std::optional<std::string> read_file_under(const std::string& root, std::string_view rel,
                                           std::size_t max_bytes);

// Opens rel (under root) read-only with the same symlink rules; returns fd or -1.
int open_under(const std::string& root, std::string_view rel);

// Write via temp file in the same directory + rename (never leaves a partial file).
bool write_file_atomic(const std::string& path, std::string_view data, unsigned mode = 0644);

bool file_exists(const std::string& path) noexcept;

// ----------------------------------------------------------------- misc
std::string sha256_hex(std::string_view data);
std::string iso_utc_now();

// Dotted numeric version, e.g. "13.3.0".
struct Version {
  std::vector<std::uint64_t> parts;
  bool empty() const noexcept { return parts.empty(); }
  std::uint64_t major() const noexcept { return parts.empty() ? 0 : parts[0]; }
};
// Finds the first dotted numeric run in `text` ("gcc (GCC) 15.2.1 2026" -> 15.2.1).
std::optional<Version> parse_version(std::string_view text);
int compare_versions(const Version& a, const Version& b) noexcept;
std::string version_string(const Version& v);

// Removes details that identify the developer from strings that end up in a
// snapshot: $HOME, user name and host name.
class Redactor {
 public:
  Redactor();
  std::string apply(std::string_view s) const;

 private:
  std::vector<std::pair<std::string, std::string>> subs_;
};

}  // namespace bd
