// http.hpp - tiny HTTP/1.1 POST client for talking to a local Ollama server.
//
// Deliberately small and strict:
//   * plain TCP only; loopback addresses only unless allow_remote is set
//   * connect / idle / total timeouts, response header + body size caps
//   * supports Content-Length, chunked and read-until-close bodies
//   * body is delivered incrementally (needed for streamed NDJSON replies)
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace bd {

struct HttpEndpoint {
  std::string host = "127.0.0.1";
  unsigned short port = 11434;
  bool allow_remote = false;
};

struct HttpLimits {
  std::chrono::milliseconds connect_timeout{3000};
  std::chrono::milliseconds idle_timeout{120000};   // max silence from the server
  std::chrono::milliseconds total_timeout{600000};
  std::size_t max_header = 16 * 1024;
  std::size_t max_body = 8u << 20;
};

struct HttpResult {
  bool ok = false;      // transport succeeded (any HTTP status)
  int status = 0;
  std::string error;    // transport error description
  bool aborted = false; // callback asked to stop
};

// on_body is called with decoded body bytes. Return false to abort.
HttpResult http_post_json(const HttpEndpoint& ep, const std::string& path, const std::string& json_body,
                          const std::function<bool(int status, std::string_view chunk)>& on_body,
                          const HttpLimits& limits = {});

// Parses "host", "host:port", "http://host:port" (what OLLAMA_HOST usually looks like).
bool parse_endpoint(std::string_view spec, HttpEndpoint& out);

// Incremental decoder for "Transfer-Encoding: chunked" (exposed for tests).
class ChunkedDecoder {
 public:
  // Appends decoded bytes to out. Returns false on a protocol violation.
  bool feed(std::string_view in, std::string& out);
  bool done() const noexcept { return state_ == State::Done; }

 private:
  enum class State { Size, Data, DataEnd, Trailer, Done, Error };
  State state_ = State::Size;
  std::string line_;
  std::size_t remaining_ = 0;
};

}  // namespace bd
