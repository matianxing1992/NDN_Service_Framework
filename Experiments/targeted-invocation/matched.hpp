#pragma once
#include "../../NDNSF-DistributedInference/cpp/vendor/nlohmann/json.hpp"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>

namespace matched203 {
inline uint64_t now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline int setting(const char* key, int fallback) {
  const char* value = std::getenv(key);
  if (!value) return fallback;
  size_t end = 0; int result = std::stoi(value, &end);
  if (end != std::string(value).size() || result < 0)
    throw std::invalid_argument(key);
  return result;
}
class Records {
  std::ofstream file;
public:
  Records() {
    if (const char* path = std::getenv("SPEC203_RECORDS")) {
      file.open(path);
      if (!file) throw std::runtime_error("cannot open matched records");
    }
  }
  bool enabled() const { return file.is_open(); }
  void write(nlohmann::json value) {
    if (!enabled()) return;
    value["schema"] = "ndnsf-spec203-record-v1";
    file << value.dump() << '\n'; file.flush();
    if (!file) throw std::runtime_error("cannot write matched record");
  }
};
inline std::string command(const std::string& run, int id) {
  std::string result = run + ":" + std::to_string(id);
  if (result.size() > 64) throw std::invalid_argument("command too long");
  result.resize(64, '_'); return result;
}
}
