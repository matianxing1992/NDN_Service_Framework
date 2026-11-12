#ifndef NDNSF_DI_NATIVE_AUTOMATIC_SHAPE_SEED_HPP
#define NDNSF_DI_NATIVE_AUTOMATIC_SHAPE_SEED_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ndnsf::di {

inline constexpr std::uint64_t NATIVE_AUTOMATIC_SHAPE_SEED_MAX_BYTES = 4U << 20;

/** Portable request metadata; parsing it grants no trust or execution permission. */
struct NativeAutomaticShapeSeed
{
  std::string contractDigest;
  std::string bindingDigest;
  std::string budgetDigest;
  std::string envelopeDigest;
  std::uint64_t maxContext = 0;
  std::uint64_t maxNewTokens = 0;
  std::map<std::string, std::vector<std::int64_t>> inputShapes;
};

/** byteLimit can only tighten the protocol cap; no tensor payload is retained. */
NativeJson
nativeAutomaticShapeSeedToJson(const NativeAutomaticShapeSeed& seed,
  std::uint64_t byteLimit = NATIVE_AUTOMATIC_SHAPE_SEED_MAX_BYTES);

NativeAutomaticShapeSeed
nativeAutomaticShapeSeedFromJson(const NativeJson& value,
  std::uint64_t byteLimit = NATIVE_AUTOMATIC_SHAPE_SEED_MAX_BYTES);

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_AUTOMATIC_SHAPE_SEED_HPP
