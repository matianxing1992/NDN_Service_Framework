#ifndef NDNSF_EXAMPLES_UAV_PERSON_JSON_HPP
#define NDNSF_EXAMPLES_UAV_PERSON_JSON_HPP

#include "NDNSF-DistributedInference/cpp/vendor/nlohmann/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndnsf::examples::uav::person_json {

using Json = nlohmann::json;

[[noreturn]] inline void
reject(const std::string& reason)
{
  throw std::invalid_argument("person-backend-invalid: " + reason);
}

inline void
exactKeys(const Json& object, std::initializer_list<const char*> keys)
{
  if (!object.is_object() || object.size() != keys.size()) reject("object fields");
  for (const auto* key : keys) {
    if (!object.contains(key)) reject(std::string("missing field ") + key);
  }
}

inline std::string
text(const Json& value)
{
  if (!value.is_string()) reject("string type");
  return value.get<std::string>();
}

inline uint64_t
unsignedNumber(const Json& value, uint64_t maximum = std::numeric_limits<uint64_t>::max())
{
  // Positive JSON tokens are unsigned in the maintained parser. Reject -0, bool and 1.0.
  if (!value.is_number_unsigned()) reject("unsigned integer type");
  const auto number = value.get<uint64_t>();
  if (number > maximum) reject("integer range");
  return number;
}

inline double
finiteNumber(const Json& value)
{
  if (!value.is_number()) reject("numeric type");
  const auto number = value.get<double>();
  if (!std::isfinite(number)) reject("nonfinite number");
  return number;
}

inline void
finiteArray(const Json& wire, size_t length)
{
  if (!wire.is_array() || wire.size() != length) reject("array dimensions");
  for (const auto& value : wire) {
    if (std::abs(finiteNumber(value)) > std::numeric_limits<float>::max()) {
      reject("array exceeds backend float32 range");
    }
  }
}

inline bool
boolean(const Json& value)
{
  if (!value.is_boolean()) reject("boolean type");
  return value.get<bool>();
}

inline bool
isDigest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [] (char ch) {
      return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

inline void
checkIdentifier(const std::string& value)
{
  if (value.empty() || value.size() > 1024 ||
      !std::all_of(value.begin(), value.end(), [] (unsigned char ch) {
        return ch >= 0x21 && ch <= 0x7e;
      })) reject("native identifier");
}

inline Json
parseBounded(const std::string& wire, uint64_t maximum)
{
  if (wire.empty() || wire.size() > maximum) reject("byte limit");
  std::vector<std::set<std::string>> keys;
  try {
    return Json::parse(wire, [&keys] (int depth, Json::parse_event_t event, Json& value) {
      if (depth > 32) reject("nesting limit");
      if (event == Json::parse_event_t::object_start) keys.emplace_back();
      else if (event == Json::parse_event_t::object_end) keys.pop_back();
      else if (event == Json::parse_event_t::key &&
               !keys.back().insert(value.get<std::string>()).second) reject("duplicate key");
      return true;
    });
  }
  catch (const Json::exception& error) {
    reject(std::string("JSON syntax/type: ") + error.what());
  }
}

} // namespace ndnsf::examples::uav::person_json

#endif // NDNSF_EXAMPLES_UAV_PERSON_JSON_HPP
