#ifndef NDNSF_DI_NATIVE_CANONICAL_JSON_HPP
#define NDNSF_DI_NATIVE_CANONICAL_JSON_HPP

#include "NDNSF-DistributedInference/cpp/vendor/nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <locale>
#include <sstream>
#include <set>
#include <stdexcept>

namespace ndnsf::di {

// Internal typed JSON value. The library owns parsing, escaping and JSON types;
// canonical rendering follows sdk/placement.py's json.dumps contract.
using NativeJson = nlohmann::json;

inline NativeJson nativeParseJson(const std::string& wire)
{
  std::vector<std::set<std::string>> objectKeys;
  try {
    return NativeJson::parse(wire, [&objectKeys] (int, NativeJson::parse_event_t event,
                                                 NativeJson& value) {
      if (event == NativeJson::parse_event_t::object_start) objectKeys.emplace_back();
      else if (event == NativeJson::parse_event_t::object_end) objectKeys.pop_back();
      else if (event == NativeJson::parse_event_t::key &&
               !objectKeys.back().insert(value.get<std::string>()).second) {
        throw std::invalid_argument("duplicate canonical JSON object key");
      }
      return true;
    });
  }
  catch (const NativeJson::exception& error) {
    throw std::invalid_argument(std::string("invalid native JSON: ") + error.what());
  }
}

namespace canonical_json_detail {

template<typename Integer>
inline void appendInteger(std::string& output, Integer value)
{
  // All JSON integer types fit in 20 decimal digits plus a possible sign.
  // Append from the stack instead of allocating a temporary decimal string.
  std::array<char, 32> buffer;
  const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (converted.ec != std::errc{})
    throw std::invalid_argument("canonical JSON integer formatting failed");
  output.append(buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data()));
}

inline bool appendUnescapedAsciiString(std::string& output, const std::string& text)
{
  // ensure_ascii also escapes DEL in the vendored serializer. Leave every
  // escape and Unicode case to the original dump path without changing output.
  if (!std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return c >= 0x20 && c < 0x7f && c != '"' && c != '\\';
      })) return false;
  output += '"';
  output += text;
  output += '"';
  return true;
}

inline std::string floatingUncached(double value)
{
  if (!std::isfinite(value)) throw std::invalid_argument("canonical JSON requires finite numbers");
  if (value == 0) return std::signbit(value) ? "-0.0" : "0.0";
  // Below 2^53, integral doubles are exact decimal integers with exponent <16.
  // Their existing fixed representation needs no precision search. Keep zero
  // and larger/scientific values on their original paths.
  if (std::abs(value) < 9007199254740992.0 && std::trunc(value) == value)
    return std::to_string(static_cast<std::int64_t>(value)) + ".0";
  std::string digits;
  int exponent = 0;
  // Select the shortest round-tripping, nearest decimal at each precision.
  // Classic locale prevents process locale from changing the identity bytes.
  std::ostringstream stream;
  stream.imbue(std::locale::classic());
  std::istringstream input;
  input.imbue(std::locale::classic());
  for (int precision = 1; precision <= 17; ++precision) {
    // Reuse only this call's streams; an earlier parse may leave eof/fail set.
    stream.str(std::string{});
    stream.clear();
    stream << std::scientific << std::setprecision(precision - 1) << std::abs(value);
    const auto candidate = stream.str();
    input.str(candidate);
    input.clear();
    double parsed = 0;
    input >> parsed;
    if (!input || parsed != std::abs(value)) continue;
    const auto e = candidate.find('e');
    digits = candidate.substr(0, e);
    digits.erase(std::remove(digits.begin(), digits.end(), '.'), digits.end());
    exponent = std::stoi(candidate.substr(e + 1));
    break;
  }
  if (digits.empty()) throw std::invalid_argument("canonical float cannot round trip");
  std::string result;
  if (exponent < -4 || exponent >= 16) {
    result = digits.substr(0, 1);
    if (digits.size() > 1) result += "." + digits.substr(1);
    result += exponent < 0 ? "e-" : "e+";
    const auto power = std::to_string(std::abs(exponent));
    result += (power.size() == 1 ? "0" : "") + power;
  }
  else if (exponent < 0) {
    result = "0." + std::string(-exponent - 1, '0') + digits;
  }
  else {
    const auto point = static_cast<std::size_t>(exponent + 1);
    if (point >= digits.size()) result = digits + std::string(point - digits.size(), '0') + ".0";
    else result = digits.substr(0, point) + "." + digits.substr(point);
  }
  return std::signbit(value) ? "-" + result : result;
}

inline std::string floating(double value)
{
  // Cache only deterministic fractional conversions. Other rounding modes,
  // errors, signed zero and the existing integer shortcut retain their path.
  if (!std::isfinite(value) || value == 0 ||
      (std::abs(value) < 9007199254740992.0 && std::trunc(value) == value) ||
      std::fegetround() != FE_TONEAREST)
    return floatingUncached(value);
  struct Entry {
    double value = 0;
    std::string wire;
  };
  thread_local std::array<Entry, 64> entries;
  auto& entry = entries[std::hash<double>{}(value) % entries.size()];
  if (entry.value == value) return entry.wire;
  auto wire = floatingUncached(value);
  entry.wire = wire;
  entry.value = value;
  return wire;
}

inline void append(std::string& output, const NativeJson& value)
{
  if (value.is_number_float()) {
    output += floating(value.get<double>());
  }
  else if (value.is_null()) {
    output += "null";
  }
  else if (value.is_boolean()) {
    output += value.get<bool>() ? "true" : "false";
  }
  else if (value.is_number_unsigned()) {
    appendInteger(output, value.get<NativeJson::number_unsigned_t>());
  }
  else if (value.is_number_integer()) {
    appendInteger(output, value.get<NativeJson::number_integer_t>());
  }
  else if (value.is_string()) {
    if (!appendUnescapedAsciiString(output, value.get_ref<const NativeJson::string_t&>()))
      output += value.dump(-1, ' ', true);
  }
  else if (value.is_array()) {
    output += '[';
    bool first = true;
    for (const auto& item : value) {
      if (!first) output += ',';
      first = false;
      append(output, item);
    }
    output += ']';
  }
  else if (value.is_object()) {
    output += '{';
    bool first = true;
    // std::map UTF-8 order equals Unicode scalar order for valid UTF-8 keys.
    for (auto it = value.begin(); it != value.end(); ++it) {
      if (!first) output += ',';
      first = false;
      if (!appendUnescapedAsciiString(output, it.key()))
        output += NativeJson(it.key()).dump(-1, ' ', true);
      output += ':';
      append(output, it.value());
    }
    output += '}';
  }
  else {
    if (value.is_binary() || value.is_discarded()) {
      throw std::invalid_argument("canonical JSON requires a JSON value");
    }
    output += value.dump(-1, ' ', true);
  }
}

} // namespace canonical_json_detail

inline std::string nativeCanonicalJson(const NativeJson& value)
{
  std::string result;
  canonical_json_detail::append(result, value);
  return result;
}

} // namespace ndnsf::di

#endif
