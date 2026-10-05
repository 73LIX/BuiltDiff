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
#include <vector>

#include "compare.hpp"
#include "http.hpp"

namespace bd {

// Only used when the model cannot be discovered from the running Ollama server
// (no server, or it lists nothing usable). A hardcoded tag alone makes --analyze
// fail on any machine whose tags differ, so discovery is the normal path.
inline constexpr const char* kDefaultModel = "gemma4:e2b";

struct AnalyzeOptions {
  HttpEndpoint endpoint;
  std::string model = kDefaultModel;
  HttpLimits limits;
  // What /api/tags reported, so a 404 can tell the user what to retry with.
  std::vector<std::string> available;
};

// Compact, size-capped text description of everything that is not "ok".
std::string build_prompt_data(const Report& r, std::size_t max_bytes = 12 * 1024);
std::string system_prompt();

// Streams the answer to `out`. Returns false (with err filled) if the model is
// not reachable; the caller can then fall back to the plain report.
bool analyze_report(const Report& r, const AnalyzeOptions& opts, std::ostream& out, std::string& err);

// ------------------------------------------------------- model discovery
// Asks the server which models it has. GET /api/tags (Ollama answers POST with
// 405). Names are sanitized and token-checked; the body is capped before parsing.
std::vector<std::string> list_models(const HttpEndpoint& ep, const HttpLimits& limits, std::string& err);

// Picks a model from a parsed /api/tags body. Preference:
//   1. an exact match for `preferred` (usually kDefaultModel)
//   2. any gemma* model that can chat
//   3. the first chat-capable model that is not embedding-only
// Returns "" when nothing usable is installed. Pure function, no I/O.
std::string select_model(const std::vector<std::string>& installed, const std::string& preferred = kDefaultModel);

// list_models + select_model. err is filled only on a transport failure.
std::string discover_model(const HttpEndpoint& ep, const HttpLimits& limits, const std::string& preferred,
                           std::vector<std::string>& installed, std::string& err);

// Incremental NDJSON reader for Ollama's streamed replies (exposed for tests).
class NdjsonChat {
 public:
  // Feeds raw bytes; appends sanitized assistant text to `text`.
  // Returns false once an error object or bad JSON line is seen (err is set).
  bool feed(std::string_view chunk, std::string& text);
  bool done() const noexcept { return done_; }
  const std::string& error() const noexcept { return err_; }
  // Ollama's stop reason for the final chunk: "stop", "length" (hit the token
  // cap) or "load"/other. Empty if the stream ended without one. "length" means
  // the answer was cut short and the caller should say so.
  const std::string& done_reason() const noexcept { return done_reason_; }

 private:
  bool handle_line(std::string_view line, std::string& text);
  std::string buf_;
  bool done_ = false;
  std::string err_;
  std::string done_reason_;
};

}  // namespace bd
