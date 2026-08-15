#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dsh_tui {

/// Minimal JSON value used by the dsh-tui wire protocol.
class Json {
 public:
  using Array = std::vector<Json>;
  using Object = std::map<std::string, Json>;

  enum class Type { Null, Bool, Integer, Number, String, Array, Object };

  Json() : value_(nullptr) {}
  Json(std::nullptr_t) : value_(nullptr) {}
  Json(bool v) : value_(v) {}
  Json(int v) : value_(static_cast<int64_t>(v)) {}
  Json(int64_t v) : value_(v) {}
  Json(double v) : value_(v) {}
  Json(std::string v) : value_(std::move(v)) {}
  Json(const char* v) : value_(std::string(v)) {}
  Json(Array v) : value_(std::move(v)) {}
  Json(Object v) : value_(std::move(v)) {}

  Type type() const {
    switch (value_.index()) {
      case 0: return Type::Null;
      case 1: return Type::Bool;
      case 2: return Type::Integer;
      case 3: return Type::Number;
      case 4: return Type::String;
      case 5: return Type::Array;
      default: return Type::Object;
    }
  }

  bool is_null() const { return type() == Type::Null; }
  bool is_bool() const { return type() == Type::Bool; }
  bool is_number() const { return type() == Type::Integer || type() == Type::Number; }
  bool is_string() const { return type() == Type::String; }
  bool is_array() const { return type() == Type::Array; }
  bool is_object() const { return type() == Type::Object; }

  bool as_bool() const { return std::get<bool>(value_); }
  int64_t as_integer() const {
    if (type() == Type::Integer) return std::get<int64_t>(value_);
    if (type() == Type::Number) return static_cast<int64_t>(std::get<double>(value_));
    return 0;
  }
  double as_number() const {
    if (type() == Type::Integer) return static_cast<double>(std::get<int64_t>(value_));
    if (type() == Type::Number) return std::get<double>(value_);
    return 0.0;
  }
  const std::string& as_string() const {
    static const std::string kEmpty;
    return is_string() ? std::get<std::string>(value_) : kEmpty;
  }
  const Array& as_array() const {
    static const Array kEmpty;
    return is_array() ? std::get<Array>(value_) : kEmpty;
  }
  const Object& as_object() const {
    static const Object kEmpty;
    return is_object() ? std::get<Object>(value_) : kEmpty;
  }

  const Json* find(std::string_view key) const {
    if (!is_object()) return nullptr;
    const auto& object = std::get<Object>(value_);
    auto it = object.find(std::string(key));
    return it == object.end() ? nullptr : &it->second;
  }

  const Json& at(std::string_view key) const {
    static const Json kNull;
    const Json* found = find(key);
    return found == nullptr ? kNull : *found;
  }

  static std::optional<Json> parse(std::string_view input) {
    Parser parser(input);
    parser.skip_whitespace();
    Json value;
    if (!parser.parse_value(value)) return std::nullopt;
    parser.skip_whitespace();
    if (!parser.done()) return std::nullopt;
    return value;
  }

  std::string dump() const {
    std::string out;
    dump_to(out);
    return out;
  }

  static std::string escape(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8);
    out.push_back('"');
    for (char c : input) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          unsigned char u = static_cast<unsigned char>(c);
          if (u < 0x20) {
            constexpr char hex[] = "0123456789abcdef";
            out += "\\u00";
            out.push_back(hex[(u >> 4U) & 0xFU]);
            out.push_back(hex[u & 0xFU]);
          } else {
            out.push_back(c);
          }
      }
    }
    out.push_back('"');
    return out;
  }

 private:
  std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object> value_;

  void dump_to(std::string& out) const {
    switch (type()) {
      case Type::Null: out += "null"; break;
      case Type::Bool: out += as_bool() ? "true" : "false"; break;
      case Type::Integer: out += std::to_string(as_integer()); break;
      case Type::Number: {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.17g", as_number());
        out += buffer;
        break;
      }
      case Type::String: out += escape(as_string()); break;
      case Type::Array: {
        out.push_back('[');
        bool first = true;
        for (const auto& item : as_array()) {
          if (!first) out.push_back(',');
          first = false;
          item.dump_to(out);
        }
        out.push_back(']');
        break;
      }
      case Type::Object: {
        out.push_back('{');
        bool first = true;
        for (const auto& [key, value] : as_object()) {
          if (!first) out.push_back(',');
          first = false;
          out += escape(key);
          out.push_back(':');
          value.dump_to(out);
        }
        out.push_back('}');
        break;
      }
    }
  }

  class Parser {
   public:
    explicit Parser(std::string_view input) : input_(input) {}

    bool done() const { return pos_ >= input_.size(); }

    void skip_whitespace() {
      while (pos_ < input_.size()) {
        char c = input_[pos_];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
        else break;
      }
    }

    bool parse_value(Json& out) {
      if (pos_ >= input_.size()) return false;
      switch (input_[pos_]) {
        case 'n': return consume_literal("null", Json(nullptr), out);
        case 't': return consume_literal("true", Json(true), out);
        case 'f': return consume_literal("false", Json(false), out);
        case '"': return parse_string(out);
        case '[': return parse_array(out);
        case '{': return parse_object(out);
        default: return parse_number(out);
      }
    }

   private:
    std::string_view input_;
    size_t pos_ = 0;

    bool consume_literal(std::string_view literal, Json value, Json& out) {
      if (input_.substr(pos_, literal.size()) != literal) return false;
      pos_ += literal.size();
      out = std::move(value);
      return true;
    }

    bool parse_string(Json& out) {
      if (pos_ >= input_.size() || input_[pos_] != '"') return false;
      ++pos_;
      std::string value;
      while (pos_ < input_.size()) {
        unsigned char c = static_cast<unsigned char>(input_[pos_++]);
        if (c == '"') {
          out = std::move(value);
          return true;
        }
        if (c == '\\') {
          if (pos_ >= input_.size()) return false;
          char escape = input_[pos_++];
          switch (escape) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case 'u': {
              if (pos_ + 4 > input_.size()) return false;
              unsigned code = 0;
              for (int i = 0; i < 4; ++i) {
                char hex = input_[pos_++];
                code <<= 4U;
                if (hex >= '0' && hex <= '9') code |= static_cast<unsigned>(hex - '0');
                else if (hex >= 'a' && hex <= 'f') code |= static_cast<unsigned>(hex - 'a' + 10);
                else if (hex >= 'A' && hex <= 'F') code |= static_cast<unsigned>(hex - 'A' + 10);
                else return false;
              }
              append_utf8(value, code);
              break;
            }
            default: return false;
          }
        } else if (c < 0x20) {
          return false;
        } else {
          value.push_back(static_cast<char>(c));
        }
      }
      return false;
    }

    static void append_utf8(std::string& out, unsigned code) {
      if (code <= 0x7F) {
        out.push_back(static_cast<char>(code));
      } else if (code <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0U | (code >> 6U)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
      } else {
        out.push_back(static_cast<char>(0xE0U | (code >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
      }
    }

    bool parse_number(Json& out) {
      size_t start = pos_;
      if (pos_ < input_.size() && input_[pos_] == '-') ++pos_;
      bool is_double = false;
      while (pos_ < input_.size()) {
        char c = input_[pos_];
        if (c >= '0' && c <= '9') ++pos_;
        else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') { is_double = true; ++pos_; }
        else break;
      }
      if (pos_ == start) return false;
      std::string token(input_.substr(start, pos_ - start));
      if (!is_double) {
        try {
          size_t consumed = 0;
          long long value = std::stoll(token, &consumed, 10);
          if (consumed == token.size()) { out = static_cast<int64_t>(value); return true; }
        } catch (...) {}
      }
      try {
        size_t consumed = 0;
        double value = std::stod(token, &consumed);
        if (consumed == token.size()) { out = value; return true; }
      } catch (...) {}
      return false;
    }

    bool parse_array(Json& out) {
      if (pos_ >= input_.size() || input_[pos_] != '[') return false;
      ++pos_;
      Array values;
      skip_whitespace();
      if (pos_ < input_.size() && input_[pos_] == ']') { ++pos_; out = std::move(values); return true; }
      while (true) {
        Json value;
        if (!parse_value(value)) return false;
        values.push_back(std::move(value));
        skip_whitespace();
        if (pos_ >= input_.size()) return false;
        char c = input_[pos_++];
        if (c == ']') { out = std::move(values); return true; }
        if (c != ',') return false;
        skip_whitespace();
      }
    }

    bool parse_object(Json& out) {
      if (pos_ >= input_.size() || input_[pos_] != '{') return false;
      ++pos_;
      Object values;
      skip_whitespace();
      if (pos_ < input_.size() && input_[pos_] == '}') { ++pos_; out = std::move(values); return true; }
      while (true) {
        Json key;
        if (!parse_string(key)) return false;
        skip_whitespace();
        if (pos_ >= input_.size() || input_[pos_] != ':') return false;
        ++pos_;
        Json value;
        if (!parse_value(value)) return false;
        values[key.as_string()] = std::move(value);
        skip_whitespace();
        if (pos_ >= input_.size()) return false;
        char c = input_[pos_++];
        if (c == '}') { out = std::move(values); return true; }
        if (c != ',') return false;
        skip_whitespace();
      }
    }
  };
};

}  // namespace dsh_tui
