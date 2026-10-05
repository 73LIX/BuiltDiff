// json.hpp - minimal, bounded JSON value / parser / writer.
//
// Designed for parsing UNTRUSTED input (a .builtdiff file arrives with a
// cloned repository, and Ollama replies are parsed with it too):
//   * hard limits on nesting depth, node count, string length, object size
//   * duplicate object keys are rejected
//   * no exceptions are thrown by the parser; errors are returned as values
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bd::json {

class Value {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };
  using Array = std::vector<Value>;
  using Member = std::pair<std::string, Value>;
  using Object = std::vector<Member>;  // insertion ordered => stable output

  Value() noexcept = default;
  Value(std::nullptr_t) noexcept {}
  Value(bool b) noexcept : type_(Type::Bool), b_(b) {}
  Value(int n) noexcept : type_(Type::Number), n_(n) {}
  Value(long long n) noexcept : type_(Type::Number), n_(static_cast<double>(n)) {}
  Value(unsigned long long n) noexcept : type_(Type::Number), n_(static_cast<double>(n)) {}
  Value(double d) noexcept : type_(Type::Number), n_(d) {}
  Value(const char* s) : type_(Type::String), s_(s) {}
  Value(std::string s) : type_(Type::String), s_(std::move(s)) {}

  static Value array() { Value v; v.type_ = Type::Array; return v; }
  static Value object() { Value v; v.type_ = Type::Object; return v; }

  Type type() const noexcept { return type_; }
  bool is_null() const noexcept { return type_ == Type::Null; }
  bool is_bool() const noexcept { return type_ == Type::Bool; }
  bool is_number() const noexcept { return type_ == Type::Number; }
  bool is_string() const noexcept { return type_ == Type::String; }
  bool is_array() const noexcept { return type_ == Type::Array; }
  bool is_object() const noexcept { return type_ == Type::Object; }

  // Checked accessors: never throw, return nullptr / nullopt on type mismatch.
  const std::string* as_string() const noexcept { return is_string() ? &s_ : nullptr; }
  std::optional<double> as_number() const noexcept {
    return is_number() ? std::optional<double>(n_) : std::nullopt;
  }
  std::optional<bool> as_bool() const noexcept {
    return is_bool() ? std::optional<bool>(b_) : std::nullopt;
  }
  const Array* as_array() const noexcept { return is_array() ? &arr_ : nullptr; }
  const Object* as_object() const noexcept { return is_object() ? &obj_ : nullptr; }

  // Object helpers.
  const Value* find(std::string_view key) const noexcept;
  Value& set(std::string key, Value v);  // converts Null -> Object; replaces existing key
  // Array helper.
  Value& push(Value v);  // converts Null -> Array

 private:
  Type type_ = Type::Null;
  bool b_ = false;
  double n_ = 0.0;
  std::string s_;
  Array arr_;
  Object obj_;
};

struct ParseOptions {
  std::size_t max_depth = 32;
  std::size_t max_nodes = 200000;
  std::size_t max_string = 1u << 20;  // bytes per string
  std::size_t max_members = 4096;     // per object
};

struct ParseResult {
  std::optional<Value> value;
  std::string error;      // empty on success
  std::size_t offset = 0; // byte offset of the error
};

ParseResult parse(std::string_view text, const ParseOptions& opts = {});

// indent < 0 => compact. Output is always valid JSON (control chars escaped).
std::string dump(const Value& v, int indent = -1);

// Convenience getters for typed extraction from an object.
std::string get_string(const Value& obj, std::string_view key, std::string_view fallback = "");
bool get_bool(const Value& obj, std::string_view key, bool fallback = false);
double get_number(const Value& obj, std::string_view key, double fallback = 0.0);

}  // namespace bd::json
