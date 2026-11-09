#ifndef NDNSF_DI_DETAIL_NATIVE_CANCELLATION_HPP
#define NDNSF_DI_DETAIL_NATIVE_CANCELLATION_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <cstdint>
#include <string>

namespace ndnsf {
namespace di {
namespace detail {

inline constexpr char NATIVE_CANCELLATION_SCOPE[] = "ndnsf-di-execution-cancellation-v1";
inline constexpr char NATIVE_CANCELLATION_TOPIC[] = "/ndnsf-di/execution/cancellation";
inline constexpr char NATIVE_CANCELLATION_PROOF_TOPIC[] = "/ndnsf-di/execution/cancellation-proof";

/** Immutable binding taken from the selected, sealed assignment. */
struct NativeCancellationBinding
{
  std::string requestId;
  std::uint64_t attempt = 0;
  std::string role;
  std::string planDigest;
};

inline std::string
nativeCancellationWire(const NativeCancellationBinding& binding, const std::string& phase)
{
  return nativeCanonicalJson(NativeJson{{"schema", "ndnsf-di-execution-cancellation-v1"},
    {"requestId", binding.requestId}, {"attempt", binding.attempt},
    {"role", binding.role}, {"planDigest", binding.planDigest}, {"phase", phase}});
}

/** Return only a bounded, exactly bound control or proof phase. */
inline std::string
nativeCancellationPhase(const std::string& wire, const NativeCancellationBinding& binding)
{
  if (wire.empty() || wire.size() > 16384) return {};
  try {
    const auto value = nativeParseJson(wire);
    if (!value.is_object() || value.size() != 6 ||
        !value.at("attempt").is_number_unsigned() ||
        value.at("schema") != "ndnsf-di-execution-cancellation-v1" ||
        value.at("requestId") != binding.requestId || value.at("attempt") != binding.attempt ||
        value.at("role") != binding.role || value.at("planDigest") != binding.planDigest)
      return {};
    const auto phase = value.at("phase").get<std::string>();
    return phase == "cancel" || phase == "fenced" || phase == "drained" ? phase : "";
  }
  catch (const std::exception&) { return {}; }
}

} // namespace detail
} // namespace di
} // namespace ndnsf

#endif // NDNSF_DI_DETAIL_NATIVE_CANCELLATION_HPP
