#ifndef NDNSF_DI_MATERIAL_RECEIPT_HPP
#define NDNSF_DI_MATERIAL_RECEIPT_HPP

#include <boost/property_tree/ptree.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::di {

/** Shared bound for temporary parser storage, checked by the publisher and assembler. */
inline constexpr std::optional<std::uint64_t>
nativeCanonicalMaterialParseReservation(std::uint64_t manifestBytes, std::uint64_t receiptBytes,
                                        std::uint64_t budget) noexcept
{
  constexpr std::uint64_t manifestMultiplier = 8;
  constexpr std::uint64_t receiptMultiplier = 16;
  if (manifestBytes > budget / manifestMultiplier ||
      receiptBytes > (budget - manifestBytes * manifestMultiplier) / receiptMultiplier)
    return std::nullopt;
  return manifestBytes * manifestMultiplier + receiptBytes * receiptMultiplier;
}

/** Parse an already authenticated receipt with the maintained bounded schema
 * and exact model/material bindings. Object packet admission remains in Core.
 */
boost::property_tree::ptree
parseNativeCanonicalMaterialReceipt(const std::vector<std::uint8_t>& bytes,
  const std::string& sourceDigest, const std::string& graphDigest,
  const std::string& materialIdentityDigest, const std::string& materialManifestDigest);

} // namespace ndnsf::di

#endif // NDNSF_DI_MATERIAL_RECEIPT_HPP
