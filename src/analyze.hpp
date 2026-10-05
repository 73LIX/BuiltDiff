// analyze.hpp - turns a comparison Report into a beginner friendly explanation
// using a LOCAL open-weight model (Gemma) served by Ollama.
//
// Privacy / safety model:
//   * only the already-sanitized Report is sent, never raw files or environment
//   * the connection is restricted to loopback unless --allow-remote is given
//   * the snapshot is untrusted input => the prompt marks it as data, and the
//     model's reply is stripped of control characters before it is printed
#pragma once

#include <iosfwd>
#include <string>

#include "compare.hpp"
#include "http.hpp"

namespace bd {

inline constexpr const char* kDefaultModel = "gemma3";

struct AnalyzeOptions {
  HttpEndpoint endpoint;
  std::string model = kDefaultModel;
  HttpLimits limits;
};

// Compact, size-capped text description of everything that is not "ok".
std::string build_prompt_data(const Report& r, std::size_t max_bytes = 12 * 1024);
std::string system_prompt();

// Streams the answer to `out`. Returns false (with err filled) if the model is
// not reachable; the caller can then fall back to the plain report.
bool analyze_report(const Report& r, const AnalyzeOptions& opts, std::ostream& out, std::string& err);

// Incremental NDJSON reader for Ollama's streamed replies (exposed for tests).
class NdjsonChat {
 public:
  // Feeds raw bytes; appends sanitized assistant text to `text`.
  // Returns false once an error object or bad JSON line is seen (err is set).
  bool feed(std::string_view chunk, std::string& text);
  bool done() const noexcept { return done_; }
  const std::string& error() const noexcept { return err_; }

 private:
  bool handle_line(std::string_view line, std::string& text);
  std::string buf_;
  bool done_ = false;
  std::string err_;
};

}  // namespace bd
