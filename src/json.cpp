#include "json.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <system_error>

namespace bd::json {

// ---------------------------------------------------------------- Value

const Value* Value::find(std::string_view key) const noexcept {
  if (!is_object()) return nullptr;
  for (const auto& m : obj_)
    if (m.first == key) return &m.second;
  return nullptr;
}

Value& Value::set(std::string key, Value v) {
  if (type_ == Type::Null) type_ = Type::Object;
  for (auto& m : obj_) {
    if (m.first == key) {
      m.second = std::move(v);
      return m.second;
    }
  }
  obj_.emplace_back(std::move(key), std::move(v));
  return obj_.back().second;
}

Value& Value::push(Value v) {
  if (type_ == Type::Null) type_ = Type::Array;
  arr_.push_back(std::move(v));
  return arr_.back();
}

// --------------------------------------------------------------- parser

namespace {

class Parser {
 public:
  Parser(std::string_view s, const ParseOptions& o) : s_(s), o_(o) {}

  ParseResult run() {
    ParseResult r;
    Value v;
    ws();
    if (!value(v, 0)) return fail_result();
    ws();
    if (i_ != s_.size()) {
      fail("trailing characters after JSON value");
      return fail_result();
    }
    r.value = std::move(v);
    return r;
  }

 private:
  std::string_view s_;
  const ParseOptions& o_;
  std::size_t i_ = 0;
  std::size_t nodes_ = 0;
  std::string err_;
  std::size_t err_off_ = 0;

  ParseResult fail_result() {
    ParseResult r;
    r.error = err_.empty() ? "parse error" : err_;
    r.offset = err_off_;
    return r;
  }

  bool fail(const char* msg) {
    if (err_.empty()) {
      err_ = msg;
      err_off_ = i_;
    }
    return false;
  }

  void ws() {
    while (i_ < s_.size()) {
      char c = s_[i_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i_;
      else break;
    }
  }

  bool literal(std::string_view lit) {
    if (s_.substr(i_, lit.size()) != lit) return fail("invalid literal");
    i_ += lit.size();
    return true;
  }

  bool value(Value& out, std::size_t depth) {
    if (depth > o_.max_depth) return fail("nesting too deep");
    if (++nodes_ > o_.max_nodes) return fail("too many JSON nodes");
    if (i_ >= s_.size()) return fail("unexpected end of input");
    char c = s_[i_];
    switch (c) {
      case '{': return object(out, depth);
      case '[': return array(out, depth);
      case '"': {
        std::string str;
        if (!string(str)) return false;
        out = Value(std::move(str));
        return true;
      }
      case 't':
        if (!literal("true")) return false;
        out = Value(true);
        return true;
      case 'f':
        if (!literal("false")) return false;
        out = Value(false);
        return true;
      case 'n':
        if (!literal("null")) return false;
        out = Value(nullptr);
        return true;
      default:
        if (c == '-' || (c >= '0' && c <= '9')) return number(out);
        return fail("unexpected character");
    }
  }

  bool number(Value& out) {
    const std::size_t start = i_;
    if (i_ < s_.size() && s_[i_] == '-') ++i_;
    if (i_ >= s_.size()) return fail("bad number");
    if (s_[i_] == '0') {
      ++i_;
    } else if (s_[i_] >= '1' && s_[i_] <= '9') {
      while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
    } else {
      return fail("bad number");
    }
    if (i_ < s_.size() && s_[i_] == '.') {
      ++i_;
      std::size_t d = 0;
      while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') { ++i_; ++d; }
      if (d == 0) return fail("bad number fraction");
    }
    if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
      ++i_;
      if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
      std::size_t d = 0;
      while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') { ++i_; ++d; }
      if (d == 0) return fail("bad number exponent");
    }
    double d = 0.0;
    auto res = std::from_chars(s_.data() + start, s_.data() + i_, d);
    if (res.ec != std::errc() || res.ptr != s_.data() + i_ || !std::isfinite(d))
      return fail("number out of range");
    out = Value(d);
    return true;
  }

  static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }

  bool hex4(std::uint32_t& cp) {
    if (s_.size() - i_ < 4) return fail("truncated \\u escape");
    std::uint32_t v = 0;
    for (int k = 0; k < 4; ++k) {
      int h = hex(s_[i_ + static_cast<std::size_t>(k)]);
      if (h < 0) return fail("bad \\u escape");
      v = (v << 4) | static_cast<std::uint32_t>(h);
    }
    i_ += 4;
    cp = v;
    return true;
  }

  static void put_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool string(std::string& out) {
    ++i_;  // opening quote
    while (true) {
      if (i_ >= s_.size()) return fail("unterminated string");
      unsigned char c = static_cast<unsigned char>(s_[i_]);
      if (c == '"') { ++i_; return true; }
      if (c < 0x20) return fail("control character in string");
      if (out.size() >= o_.max_string) return fail("string too long");
      if (c != '\\') { out.push_back(static_cast<char>(c)); ++i_; continue; }
      ++i_;
      if (i_ >= s_.size()) return fail("unterminated escape");
      char e = s_[i_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          std::uint32_t cp = 0;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (s_.size() - i_ < 2 || s_[i_] != '\\' || s_[i_ + 1] != 'u')
              return fail("lone high surrogate");
            i_ += 2;
            std::uint32_t lo = 0;
            if (!hex4(lo)) return false;
            if (lo < 0xDC00 || lo > 0xDFFF) return fail("invalid surrogate pair");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return fail("lone low surrogate");
          }
          put_utf8(out, cp);
          break;
        }
        default: return fail("invalid escape");
      }
    }
  }

  bool array(Value& out, std::size_t depth) {
    ++i_;  // [
    out = Value::array();
    ws();
    if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
    while (true) {
      Value elem;
      ws();
      if (!value(elem, depth + 1)) return false;
      out.push(std::move(elem));
      ws();
      if (i_ >= s_.size()) return fail("unterminated array");
      if (s_[i_] == ',') { ++i_; continue; }
      if (s_[i_] == ']') { ++i_; return true; }
      return fail("expected ',' or ']'");
    }
  }

  bool object(Value& out, std::size_t depth) {
    ++i_;  // {
    out = Value::object();
    std::size_t members = 0;
    ws();
    if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
    while (true) {
      ws();
      if (i_ >= s_.size() || s_[i_] != '"') return fail("expected object key");
      std::string key;
      if (!string(key)) return false;
      ws();
      if (i_ >= s_.size() || s_[i_] != ':') return fail("expected ':'");
      ++i_;
      ws();
      Value v;
      if (!value(v, depth + 1)) return false;
      if (++members > o_.max_members) return fail("too many object members");
      if (out.find(key) != nullptr) return fail("duplicate object key");
      out.set(std::move(key), std::move(v));
      ws();
      if (i_ >= s_.size()) return fail("unterminated object");
      if (s_[i_] == ',') { ++i_; continue; }
      if (s_[i_] == '}') { ++i_; return true; }
      return fail("expected ',' or '}'");
    }
  }
};

void escape_into(std::string& out, const std::string& s) {
  static const char* hexd = "0123456789abcdef";
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20 || c == 0x7F) {
          out += "\\u00";
          out.push_back(hexd[c >> 4]);
          out.push_back(hexd[c & 0xF]);
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
}

void dump_into(std::string& out, const Value& v, int indent, int level) {
  auto newline = [&](int lvl) {
    if (indent < 0) return;
    out.push_back('\n');
    out.append(static_cast<std::size_t>(lvl * indent), ' ');
  };
  switch (v.type()) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += *v.as_bool() ? "true" : "false"; break;
    case Value::Type::Number: {
      double d = *v.as_number();
      if (!std::isfinite(d)) { out += "null"; break; }
      char buf[64];
      std::to_chars_result r{};
      if (d == std::floor(d) && std::fabs(d) < 1e15)
        r = std::to_chars(buf, buf + sizeof buf, static_cast<long long>(d));
      else
        r = std::to_chars(buf, buf + sizeof buf, d);
      out.append(buf, r.ptr);
      break;
    }
    case Value::Type::String: escape_into(out, *v.as_string()); break;
    case Value::Type::Array: {
      const auto& a = *v.as_array();
      if (a.empty()) { out += "[]"; break; }
      out.push_back('[');
      for (std::size_t k = 0; k < a.size(); ++k) {
        if (k) out.push_back(',');
        newline(level + 1);
        dump_into(out, a[k], indent, level + 1);
      }
      newline(level);
      out.push_back(']');
      break;
    }
    case Value::Type::Object: {
      const auto& o = *v.as_object();
      if (o.empty()) { out += "{}"; break; }
      out.push_back('{');
      for (std::size_t k = 0; k < o.size(); ++k) {
        if (k) out.push_back(',');
        newline(level + 1);
        escape_into(out, o[k].first);
        out.push_back(':');
        if (indent >= 0) out.push_back(' ');
        dump_into(out, o[k].second, indent, level + 1);
      }
      newline(level);
      out.push_back('}');
      break;
    }
  }
}

}  // namespace

ParseResult parse(std::string_view text, const ParseOptions& opts) {
  return Parser(text, opts).run();
}

std::string dump(const Value& v, int indent) {
  std::string out;
  dump_into(out, v, indent, 0);
  if (indent >= 0) out.push_back('\n');
  return out;
}

std::string get_string(const Value& obj, std::string_view key, std::string_view fallback) {
  if (const Value* v = obj.find(key))
    if (const std::string* s = v->as_string()) return *s;
  return std::string(fallback);
}

bool get_bool(const Value& obj, std::string_view key, bool fallback) {
  if (const Value* v = obj.find(key))
    if (auto b = v->as_bool()) return *b;
  return fallback;
}

double get_number(const Value& obj, std::string_view key, double fallback) {
  if (const Value* v = obj.find(key))
    if (auto n = v->as_number()) return *n;
  return fallback;
}

}  // namespace bd::json
