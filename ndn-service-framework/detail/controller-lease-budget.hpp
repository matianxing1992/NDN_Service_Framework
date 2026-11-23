#ifndef NDN_SERVICE_FRAMEWORK_DETAIL_CONTROLLER_LEASE_BUDGET_HPP
#define NDN_SERVICE_FRAMEWORK_DETAIL_CONTROLLER_LEASE_BUDGET_HPP

#include "../RevocationState.hpp"

#include <charconv>
#include <cstdlib>
#include <string_view>

namespace ndn_service_framework::detail {

inline PolicyLeaseBudget
controllerLeaseBudget()
{
  static const auto budget = [] {
    const auto read = [] (const char* name, uint64_t fallback, uint64_t min, uint64_t max) {
      const auto* value = std::getenv(name);
      if (value == nullptr)
        return std::optional<uint64_t>(fallback);
      const std::string_view text(value);
      uint64_t result = 0;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
      if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() ||
          result < min || result > max)
        return std::optional<uint64_t>{};
      return std::optional<uint64_t>(result);
    };
    const auto lease = read("NDNSF_CONTROLLER_POLICY_MAX_LEASE_MS", 60000, 5000, 300000);
    const auto skew = read("NDNSF_CONTROLLER_POLICY_CLOCK_SKEW_MS", 250, 0, 1000);
    return lease && skew ? PolicyLeaseBudget{*lease, *skew} : PolicyLeaseBudget{0, 0};
  }();
  return budget;
}

} // namespace ndn_service_framework::detail

#endif // NDN_SERVICE_FRAMEWORK_DETAIL_CONTROLLER_LEASE_BUDGET_HPP
