#include "http.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>

#include "util.hpp"

namespace bd {

namespace {

using Clock = std::chrono::steady_clock;

struct Sock {
  int fd = -1;
  Sock() = default;
  explicit Sock(int f) : fd(f) {}
  Sock(const Sock&) = delete;
  Sock& operator=(const Sock&) = delete;
  ~Sock() { if (fd >= 0) ::close(fd); }
};

bool is_loopback(const sockaddr* sa) {
  if (sa->sa_family == AF_INET) {
    const auto* in = reinterpret_cast<const sockaddr_in*>(sa);
    return (ntohl(in->sin_addr.s_addr) >> 24) == 127;
  }
  if (sa->sa_family == AF_INET6) {
    const auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa);
    return IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr) != 0;
  }
  return false;
}

int ms_left(Clock::time_point deadline) {
  auto d = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
  if (d < 0) return 0;
  if (d > 1000000) return 1000000;
  return static_cast<int>(d);
}

bool wait_fd(int fd, short events, int timeout_ms) {
  pollfd p{fd, events, 0};
  while (true) {
    int r = ::poll(&p, 1, timeout_ms);
    if (r < 0 && errno == EINTR) continue;
    return r > 0;
  }
}

// Connects with a timeout. Every resolved address must be loopback unless allow_remote.
int connect_to(const HttpEndpoint& ep, std::chrono::milliseconds timeout, std::string& err) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const std::string port = std::to_string(ep.port);
  int rc = ::getaddrinfo(ep.host.c_str(), port.c_str(), &hints, &res);
  if (rc != 0 || !res) {
    err = std::string("cannot resolve host: ") + ::gai_strerror(rc);
    return -1;
  }
  int result = -1;
  bool refused_remote = false;
  for (addrinfo* a = res; a; a = a->ai_next) {
    if (!ep.allow_remote && !is_loopback(a->ai_addr)) {
      refused_remote = true;
      continue;
    }
    Sock s(::socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol));
    if (s.fd < 0) continue;
    int rc2 = ::connect(s.fd, a->ai_addr, a->ai_addrlen);
    if (rc2 != 0 && errno != EINPROGRESS) {
      err = std::string("connect failed: ") + std::strerror(errno);
      continue;
    }
    if (rc2 != 0) {
      if (!wait_fd(s.fd, POLLOUT, static_cast<int>(timeout.count()))) {
        err = "connect timed out";
        continue;
      }
      int soerr = 0;
      socklen_t len = sizeof soerr;
      ::getsockopt(s.fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
      if (soerr != 0) {
        err = std::string("connect failed: ") + std::strerror(soerr);
        continue;
      }
    }
    int one = 1;
    ::setsockopt(s.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    result = s.fd;
    s.fd = -1;
    break;
  }
  ::freeaddrinfo(res);
  if (result < 0 && err.empty()) {
    err = refused_remote ? "refusing to talk to a non-loopback address (use --allow-remote to override)"
                         : "no usable address";
  }
  return result;
}

bool send_all(int fd, std::string_view data, Clock::time_point deadline) {
  std::size_t off = 0;
  while (off < data.size()) {
    ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
    if (n > 0) {
      off += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (!wait_fd(fd, POLLOUT, ms_left(deadline))) return false;
      continue;
    }
    return false;
  }
  return true;
}

std::size_t parse_hex(std::string_view s, bool& ok) {
  ok = false;
  s = trim(s);
  if (auto semi = s.find(';'); semi != std::string_view::npos) s = trim(s.substr(0, semi));  // chunk extensions
  if (s.empty() || s.size() > 12) return 0;
  std::size_t v = 0;
  for (char c : s) {
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return 0;
    v = (v << 4) | static_cast<std::size_t>(d);
  }
  ok = true;
  return v;
}

}  // namespace

bool parse_endpoint(std::string_view spec, HttpEndpoint& out) {
  spec = trim(spec);
  if (spec.substr(0, 7) == "http://") spec.remove_prefix(7);
  else if (spec.find("://") != std::string_view::npos) return false;  // https is not supported
  if (auto slash = spec.find('/'); slash != std::string_view::npos) spec = spec.substr(0, slash);
  if (spec.empty() || spec.size() > 255) return false;
  std::string host;
  std::string_view port_sv;
  if (spec.front() == '[') {  // [::1]:11434
    auto close = spec.find(']');
    if (close == std::string_view::npos) return false;
    host = std::string(spec.substr(1, close - 1));
    auto rest = spec.substr(close + 1);
    if (!rest.empty()) {
      if (rest.front() != ':') return false;
      port_sv = rest.substr(1);
    }
  } else {
    auto colon = spec.rfind(':');
    if (colon != std::string_view::npos && spec.find(':') == colon) {
      host = std::string(spec.substr(0, colon));
      port_sv = spec.substr(colon + 1);
    } else {
      host = std::string(spec);
    }
  }
  if (host.empty()) return false;
  for (char c : host)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == ':' || c == '_')) return false;
  unsigned port = 11434;
  if (!port_sv.empty()) {
    if (port_sv.size() > 5) return false;
    port = 0;
    for (char c : port_sv) {
      if (c < '0' || c > '9') return false;
      port = port * 10 + static_cast<unsigned>(c - '0');
    }
    if (port == 0 || port > 65535) return false;
  }
  out.host = host;
  out.port = static_cast<unsigned short>(port);
  return true;
}

bool ChunkedDecoder::feed(std::string_view in, std::string& out) {
  std::size_t i = 0;
  while (i < in.size() && state_ != State::Done && state_ != State::Error) {
    switch (state_) {
      case State::Size:
      case State::Trailer: {
        char c = in[i++];
        if (c == '\n') {
          std::string_view line = line_;
          if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
          if (state_ == State::Size) {
            bool ok = false;
            std::size_t n = parse_hex(line, ok);
            if (!ok) { state_ = State::Error; return false; }
            remaining_ = n;
            state_ = n == 0 ? State::Trailer : State::Data;
          } else if (line.empty()) {
            state_ = State::Done;
          }
          line_.clear();
        } else {
          if (line_.size() > 1024) { state_ = State::Error; return false; }
          line_.push_back(c);
        }
        break;
      }
      case State::Data: {
        std::size_t n = std::min(remaining_, in.size() - i);
        out.append(in.data() + i, n);
        i += n;
        remaining_ -= n;
        if (remaining_ == 0) state_ = State::DataEnd;
        break;
      }
      case State::DataEnd: {
        char c = in[i++];
        if (c == '\n') state_ = State::Size;
        else if (c != '\r') { state_ = State::Error; return false; }
        break;
      }
      default: break;
    }
  }
  return state_ != State::Error;
}

namespace {

// Shared request path. `method` is one of the two literals below; an empty
// `body` means no Content-Type/Content-Length headers are sent.
HttpResult exchange(const HttpEndpoint& ep, const char* method, const std::string& path, const std::string& body,
                    const std::function<bool(int, std::string_view)>& on_body, const HttpLimits& limits) {
  HttpResult res;
  const auto start = Clock::now();
  const auto total_deadline = start + limits.total_timeout;

  Sock s(connect_to(ep, limits.connect_timeout, res.error));
  if (s.fd < 0) return res;

  std::string req = std::string(method) + " " + path + " HTTP/1.1\r\nHost: " + ep.host + ":" + std::to_string(ep.port) +
                    "\r\nUser-Agent: builtdiff/" + kToolVersion +
                    "\r\nAccept: application/x-ndjson, application/json\r\nConnection: close\r\n";
  if (!body.empty())
    req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
  req += "\r\n";
  req += body;
  if (!send_all(s.fd, req, total_deadline)) {
    res.error = "failed to send request";
    return res;
  }

  std::string head;
  bool headers_done = false;
  bool chunked = false;
  bool have_len = false;
  std::size_t content_len = 0, body_seen = 0;
  ChunkedDecoder dec;
  std::array<char, 16384> buf;

  while (true) {
    const int idle = static_cast<int>(limits.idle_timeout.count());
    const int wait_ms = std::min(idle, ms_left(total_deadline));
    if (ms_left(total_deadline) == 0) { res.error = "timed out"; return res; }
    if (!wait_fd(s.fd, POLLIN, wait_ms)) { res.error = "timed out waiting for the server"; return res; }
    ssize_t n = ::recv(s.fd, buf.data(), buf.size(), 0);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      res.error = std::string("read failed: ") + std::strerror(errno);
      return res;
    }
    if (n == 0) break;
    std::string_view chunk(buf.data(), static_cast<std::size_t>(n));

    if (!headers_done) {
      head.append(chunk);
      auto end = head.find("\r\n\r\n");
      if (end == std::string::npos) {
        if (head.size() > limits.max_header) { res.error = "response headers too large"; return res; }
        continue;
      }
      if (end > limits.max_header) { res.error = "response headers too large"; return res; }
      std::string rest = head.substr(end + 4);
      head.resize(end);
      headers_done = true;
      // status line
      auto eol = head.find("\r\n");
      std::string_view status_line = std::string_view(head).substr(0, eol);
      if (status_line.substr(0, 5) != "HTTP/" || status_line.size() < 12) { res.error = "malformed HTTP response"; return res; }
      int code = 0;
      for (std::size_t k = 9; k < 12; ++k) {
        if (status_line[k] < '0' || status_line[k] > '9') { res.error = "malformed HTTP status"; return res; }
        code = code * 10 + (status_line[k] - '0');
      }
      res.status = code;
      // headers we care about
      if (eol != std::string::npos) {
        for (const auto& line : split(std::string_view(head).substr(eol + 2), '\n')) {
          std::string_view l = trim(line);
          auto colon = l.find(':');
          if (colon == std::string_view::npos) continue;
          std::string key = to_lower(trim(l.substr(0, colon)));
          std::string_view val = trim(l.substr(colon + 1));
          if (key == "transfer-encoding" && to_lower(val).find("chunked") != std::string::npos) chunked = true;
          else if (key == "content-length" && val.size() <= 12) {
            std::size_t v = 0;
            bool ok = !val.empty();
            for (char c : val) { if (c < '0' || c > '9') { ok = false; break; } v = v * 10 + static_cast<std::size_t>(c - '0'); }
            if (ok) { have_len = true; content_len = v; }
          }
        }
      }
      if (have_len && !chunked && content_len > limits.max_body) { res.error = "response body too large"; return res; }
      res.ok = true;
      head.clear();
      if (rest.empty()) continue;
      chunk = rest;
      // fallthrough to body handling using `rest`, which stays alive for this iteration
      std::string decoded;
      std::string_view payload = chunk;
      if (chunked) {
        if (!dec.feed(chunk, decoded)) { res.ok = false; res.error = "malformed chunked encoding"; return res; }
        payload = decoded;
      }
      body_seen += payload.size();
      if (body_seen > limits.max_body) { res.ok = false; res.error = "response body too large"; return res; }
      if (!payload.empty() && !on_body(res.status, payload)) { res.aborted = true; return res; }
      if ((chunked && dec.done()) || (have_len && !chunked && body_seen >= content_len)) return res;
      continue;
    }

    std::string decoded;
    std::string_view payload = chunk;
    if (chunked) {
      if (!dec.feed(chunk, decoded)) { res.ok = false; res.error = "malformed chunked encoding"; return res; }
      payload = decoded;
    }
    body_seen += payload.size();
    if (body_seen > limits.max_body) { res.ok = false; res.error = "response body too large"; return res; }
    if (!payload.empty() && !on_body(res.status, payload)) { res.aborted = true; return res; }
    if ((chunked && dec.done()) || (have_len && !chunked && body_seen >= content_len)) return res;
  }
  if (!headers_done) {
    res.error = "connection closed before a response arrived";
    return res;
  }
  if (chunked && !dec.done()) { res.ok = false; res.error = "connection closed in the middle of a response"; }
  else if (have_len && !chunked && body_seen < content_len) { res.ok = false; res.error = "connection closed in the middle of a response"; }
  return res;
}

}  // namespace

HttpResult http_post_json(const HttpEndpoint& ep, const std::string& path, const std::string& json_body,
                          const std::function<bool(int, std::string_view)>& on_body, const HttpLimits& limits) {
  return exchange(ep, "POST", path, json_body, on_body, limits);
}

HttpResult http_get(const HttpEndpoint& ep, const std::string& path,
                    const std::function<bool(int, std::string_view)>& on_body, const HttpLimits& limits) {
  return exchange(ep, "GET", path, std::string(), on_body, limits);
}

}  // namespace bd
