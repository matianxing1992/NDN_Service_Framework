#ifndef NDNSF_DISTRIBUTED_REPO_DETAIL_STRICT_JSON_HPP
#define NDNSF_DISTRIBUTED_REPO_DETAIL_STRICT_JSON_HPP

#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ndnsf_distributed_repo::detail {

struct JsonValue
{
  enum class Kind { Null, Boolean, Unsigned, String, Array, Object };
  using Object = std::map<std::string, JsonValue>;

  Kind kind = Kind::Null;
  bool boolean = false;
  uint64_t number = 0;
  std::string string;
  std::vector<JsonValue> array;
  Object object;
};

inline bool
isValidUtf8(std::string_view text)
{
  size_t i = 0;
  while (i < text.size()) {
    const auto first = static_cast<uint8_t>(text[i]);
    if (first <= 0x7f) {
      ++i;
      continue;
    }
    size_t count = 0;
    uint32_t codepoint = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      count = 2;
      codepoint = first & 0x1f;
    }
    else if (first >= 0xe0 && first <= 0xef) {
      count = 3;
      codepoint = first & 0x0f;
    }
    else if (first >= 0xf0 && first <= 0xf4) {
      count = 4;
      codepoint = first & 0x07;
    }
    else {
      return false;
    }
    if (text.size() - i < count) {
      return false;
    }
    for (size_t j = 1; j < count; ++j) {
      const auto next = static_cast<uint8_t>(text[i + j]);
      if ((next & 0xc0) != 0x80) {
        return false;
      }
      codepoint = (codepoint << 6) | (next & 0x3f);
    }
    if ((count == 2 && codepoint < 0x80) ||
        (count == 3 && codepoint < 0x800) ||
        (count == 4 && codepoint < 0x10000) ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) {
      return false;
    }
    i += count;
  }
  return true;
}

inline void
appendUtf8(std::string& output, uint32_t codepoint)
{
  if (codepoint <= 0x7f) {
    output.push_back(static_cast<char>(codepoint));
  }
  else if (codepoint <= 0x7ff) {
    output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  }
  else if (codepoint <= 0xffff) {
    output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  }
  else {
    output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  }
}

class JsonParser
{
public:
  explicit JsonParser(std::string_view input)
    : m_input(input)
  {
  }

  JsonValue parse()
  {
    skipWhitespace();
    auto value = parseValue(0);
    skipWhitespace();
    if (m_cursor != m_input.size()) {
      fail("trailing data");
    }
    return value;
  }

private:
  [[noreturn]] void fail(const char* message) const
  {
    throw std::invalid_argument(std::string("artifact-json-invalid: ") + message);
  }

  void skipWhitespace()
  {
    while (m_cursor < m_input.size()) {
      const char ch = m_input[m_cursor];
      if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
        break;
      }
      ++m_cursor;
    }
  }

  bool consume(char ch)
  {
    if (m_cursor < m_input.size() && m_input[m_cursor] == ch) {
      ++m_cursor;
      return true;
    }
    return false;
  }

  uint32_t parseHex4()
  {
    if (m_input.size() - m_cursor < 4) {
      fail("truncated unicode escape");
    }
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
      const char ch = m_input[m_cursor++];
      value <<= 4;
      if (ch >= '0' && ch <= '9') value |= static_cast<uint32_t>(ch - '0');
      else if (ch >= 'a' && ch <= 'f') value |= static_cast<uint32_t>(ch - 'a' + 10);
      else if (ch >= 'A' && ch <= 'F') value |= static_cast<uint32_t>(ch - 'A' + 10);
      else fail("invalid unicode escape");
    }
    return value;
  }

  std::string parseString()
  {
    if (!consume('"')) fail("expected string");
    std::string result;
    while (m_cursor < m_input.size()) {
      const unsigned char ch = static_cast<unsigned char>(m_input[m_cursor++]);
      if (ch == '"') {
        if (!isValidUtf8(result)) fail("string is not valid UTF-8");
        return result;
      }
      if (ch < 0x20) fail("unescaped control character");
      if (ch != '\\') {
        result.push_back(static_cast<char>(ch));
        continue;
      }
      if (m_cursor >= m_input.size()) fail("truncated escape");
      switch (m_input[m_cursor++]) {
      case '"': result.push_back('"'); break;
      case '\\': result.push_back('\\'); break;
      case '/': result.push_back('/'); break;
      case 'b': result.push_back('\b'); break;
      case 'f': result.push_back('\f'); break;
      case 'n': result.push_back('\n'); break;
      case 'r': result.push_back('\r'); break;
      case 't': result.push_back('\t'); break;
      case 'u': {
        uint32_t codepoint = parseHex4();
        if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
          if (m_input.size() - m_cursor < 6 || m_input[m_cursor] != '\\' ||
              m_input[m_cursor + 1] != 'u') {
            fail("unpaired high surrogate");
          }
          m_cursor += 2;
          const uint32_t low = parseHex4();
          if (low < 0xdc00 || low > 0xdfff) fail("invalid surrogate pair");
          codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
        }
        else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
          fail("unpaired low surrogate");
        }
        appendUtf8(result, codepoint);
        break;
      }
      default: fail("invalid escape");
      }
    }
    fail("unterminated string");
  }

  JsonValue parseValue(size_t depth)
  {
    if (depth > 32 || m_cursor >= m_input.size()) fail("value is missing or too deep");
    const char ch = m_input[m_cursor];
    if (ch == '"') {
      JsonValue value;
      value.kind = JsonValue::Kind::String;
      value.string = parseString();
      return value;
    }
    if (ch == '{') return parseObject(depth + 1);
    if (ch == '[') return parseArray(depth + 1);
    if (m_input.substr(m_cursor, 4) == "true") {
      m_cursor += 4;
      JsonValue value;
      value.kind = JsonValue::Kind::Boolean;
      value.boolean = true;
      return value;
    }
    if (m_input.substr(m_cursor, 5) == "false") {
      m_cursor += 5;
      JsonValue value;
      value.kind = JsonValue::Kind::Boolean;
      return value;
    }
    if (m_input.substr(m_cursor, 4) == "null") {
      m_cursor += 4;
      return JsonValue{};
    }
    if (ch >= '0' && ch <= '9') return parseUnsigned();
    fail("unsupported value type");
  }

  JsonValue parseUnsigned()
  {
    const size_t start = m_cursor;
    if (consume('0') && m_cursor < m_input.size() &&
        m_input[m_cursor] >= '0' && m_input[m_cursor] <= '9') {
      fail("leading zero in integer");
    }
    else if (start == m_cursor) {
      while (m_cursor < m_input.size() && m_input[m_cursor] >= '0' &&
             m_input[m_cursor] <= '9') ++m_cursor;
    }
    uint64_t number = 0;
    const auto parsed = std::from_chars(m_input.data() + start,
                                        m_input.data() + m_cursor, number);
    if (parsed.ec != std::errc{} || parsed.ptr != m_input.data() + m_cursor) {
      fail("integer overflow");
    }
    if (m_cursor < m_input.size() &&
        (m_input[m_cursor] == '.' || m_input[m_cursor] == 'e' ||
         m_input[m_cursor] == 'E' || m_input[m_cursor] == '+')) {
      fail("only unsigned integer numbers are accepted");
    }
    JsonValue value;
    value.kind = JsonValue::Kind::Unsigned;
    value.number = number;
    return value;
  }

  JsonValue parseObject(size_t depth)
  {
    consume('{');
    JsonValue value;
    value.kind = JsonValue::Kind::Object;
    skipWhitespace();
    if (consume('}')) return value;
    for (;;) {
      skipWhitespace();
      const auto key = parseString();
      skipWhitespace();
      if (!consume(':')) fail("expected colon");
      skipWhitespace();
      auto member = parseValue(depth);
      if (!value.object.emplace(key, std::move(member)).second) {
        fail("duplicate object key");
      }
      skipWhitespace();
      if (consume('}')) return value;
      if (!consume(',')) fail("expected comma");
    }
  }

  JsonValue parseArray(size_t depth)
  {
    consume('[');
    JsonValue value;
    value.kind = JsonValue::Kind::Array;
    skipWhitespace();
    if (consume(']')) return value;
    for (;;) {
      skipWhitespace();
      value.array.push_back(parseValue(depth));
      skipWhitespace();
      if (consume(']')) return value;
      if (!consume(',')) fail("expected comma");
    }
  }

  std::string_view m_input;
  size_t m_cursor = 0;
};

inline JsonValue
parseJson(std::string_view wire, size_t maximumBytes)
{
  if (wire.empty() || wire.size() > maximumBytes || !isValidUtf8(wire)) {
    throw std::invalid_argument("artifact-json-invalid: envelope is empty, oversized, or not UTF-8");
  }
  return JsonParser(wire).parse();
}

inline const JsonValue::Object&
requireObject(const JsonValue& value)
{
  if (value.kind != JsonValue::Kind::Object) {
    throw std::invalid_argument("artifact-json-invalid: expected object");
  }
  return value.object;
}

inline void
requireExactKeys(const JsonValue::Object& object,
                 std::initializer_list<std::string_view> keys)
{
  if (object.size() != keys.size()) {
    throw std::invalid_argument("artifact-json-invalid: object keys do not match schema");
  }
  for (const auto key : keys) {
    if (object.find(std::string(key)) == object.end()) {
      throw std::invalid_argument("artifact-json-invalid: required key is missing");
    }
  }
}

inline const JsonValue&
member(const JsonValue::Object& object, const char* key, JsonValue::Kind kind)
{
  const auto found = object.find(key);
  if (found == object.end() || found->second.kind != kind) {
    throw std::invalid_argument(std::string("artifact-json-invalid: invalid member ") + key);
  }
  return found->second;
}

inline std::string
quoteJson(std::string_view value)
{
  if (!isValidUtf8(value)) {
    throw std::invalid_argument("artifact-json-invalid: string is not UTF-8");
  }
  static constexpr char HEX[] = "0123456789abcdef";
  std::string output;
  output.reserve(value.size() + 2);
  output.push_back('"');
  for (const unsigned char ch : value) {
    switch (ch) {
    case '"': output += "\\\""; break;
    case '\\': output += "\\\\"; break;
    case '\b': output += "\\b"; break;
    case '\f': output += "\\f"; break;
    case '\n': output += "\\n"; break;
    case '\r': output += "\\r"; break;
    case '\t': output += "\\t"; break;
    default:
      if (ch < 0x20) {
        output += "\\u00";
        output.push_back(HEX[ch >> 4]);
        output.push_back(HEX[ch & 0x0f]);
      }
      else output.push_back(static_cast<char>(ch));
    }
  }
  output.push_back('"');
  return output;
}

inline std::string
renderJson(const JsonValue& value)
{
  switch (value.kind) {
  case JsonValue::Kind::Null:
    return "null";
  case JsonValue::Kind::Boolean:
    return value.boolean ? "true" : "false";
  case JsonValue::Kind::Unsigned:
    return std::to_string(value.number);
  case JsonValue::Kind::String:
    return quoteJson(value.string);
  case JsonValue::Kind::Array: {
    std::string output = "[";
    for (size_t i = 0; i < value.array.size(); ++i) {
      if (i != 0) output.push_back(',');
      output += renderJson(value.array[i]);
    }
    output.push_back(']');
    return output;
  }
  case JsonValue::Kind::Object: {
    std::string output = "{";
    bool first = true;
    for (const auto& member : value.object) {
      if (!first) output.push_back(',');
      first = false;
      output += quoteJson(member.first) + ":" + renderJson(member.second);
    }
    output.push_back('}');
    return output;
  }
  }
  throw std::invalid_argument("artifact-json-invalid: unknown JSON value kind");
}

} // namespace ndnsf_distributed_repo::detail

#endif // NDNSF_DISTRIBUTED_REPO_DETAIL_STRICT_JSON_HPP
