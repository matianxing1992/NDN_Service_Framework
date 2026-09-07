#ifndef NDNSF_DI_NATIVE_CANONICAL_JSON_HPP
#define NDNSF_DI_NATIVE_CANONICAL_JSON_HPP

#include "NDNSF-DistributedInference/cpp/vendor/nlohmann/json.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace ndnsf::di {

// Internal typed JSON value. The library owns parsing, escaping and JSON types;
// canonical rendering follows sdk/placement.py's json.dumps contract.
using NativeJson = nlohmann::json;

namespace canonical_json_detail {

inline std::string floating(double value)
{
  if (!std::isfinite(value)) throw std::invalid_argument("canonical JSON requires finite numbers");
  if (value == 0) return std::signbit(value) ? "-0.0" : "0.0";
  std::string digits;
  int exponent = 0;
  // Select the shortest round-tripping, nearest decimal at each precision.
  // Classic locale prevents process locale from changing the identity bytes.
  for (int precision = 1; precision <= 17; ++precision) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::scientific << std::setprecision(precision - 1) << std::abs(value);
    const auto candidate = stream.str();
    std::istringstream input(candidate);
    input.imbue(std::locale::classic());
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

inline void append(std::string& output, const NativeJson& value)
{
  if (value.is_number_float()) {
    output += floating(value.get<double>());
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
