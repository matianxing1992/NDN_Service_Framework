#ifndef NDN_SERVICE_FRAMEWORK_TRUST_SCHEMA_RESOURCES_HPP
#define NDN_SERVICE_FRAMEWORK_TRUST_SCHEMA_RESOURCES_HPP

#include <boost/property_tree/ptree_fwd.hpp>

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace ndn_service_framework {

struct TrustSchemaResource
{
  std::string path;
  bool required = true;
};

constexpr size_t MAX_TRUST_SCHEMA_INSPECTION_BYTES = 1024 * 1024;
constexpr size_t MAX_TRUST_SCHEMA_RESOURCES = 1024;

/** Enumerate fixed-schema inputs without opening referenced certificates.
 *  The optional Producer signer policy is returned even when absent.
 *  Inspection is bounded by MAX_TRUST_SCHEMA_INSPECTION_BYTES and
 *  MAX_TRUST_SCHEMA_RESOURCES; it does not replace runtime validation.
 */
std::vector<TrustSchemaResource>
enumerateTrustSchemaResources(const std::string& filename);

namespace trust_schema_detail {

/** Shared native INFO parsing and fixed-anchor interpretation, without credentials. */
boost::property_tree::ptree
readTrustSchema(std::istream& input, const std::string& filename);

std::optional<std::filesystem::path>
fixedAnchorFile(const boost::property_tree::ptree& anchor, const std::string& filename);

} // namespace trust_schema_detail
} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_TRUST_SCHEMA_RESOURCES_HPP
