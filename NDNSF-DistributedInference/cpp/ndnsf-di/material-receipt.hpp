#ifndef NDNSF_DI_MATERIAL_RECEIPT_HPP
#define NDNSF_DI_MATERIAL_RECEIPT_HPP

#include <boost/property_tree/ptree.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace ndnsf::di {

/** Parse an already authenticated receipt with the maintained bounded schema
 * and exact model/material bindings. Object packet admission remains in Core.
 */
boost::property_tree::ptree
parseNativeCanonicalMaterialReceipt(const std::vector<std::uint8_t>& bytes,
  const std::string& sourceDigest, const std::string& graphDigest,
  const std::string& materialIdentityDigest, const std::string& materialManifestDigest);

} // namespace ndnsf::di

#endif // NDNSF_DI_MATERIAL_RECEIPT_HPP
