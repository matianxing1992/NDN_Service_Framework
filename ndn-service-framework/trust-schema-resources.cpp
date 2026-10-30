#include "trust-schema-resources.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/property_tree/info_parser.hpp>

#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ndn_service_framework {
namespace trust_schema_detail {

boost::property_tree::ptree
readTrustSchema(std::istream& input, const std::string& filename)
{
  if (!input)
    throw std::runtime_error("Cannot open NDNSF trust schema: " + filename);
  boost::property_tree::ptree config;
  boost::property_tree::read_info(input, config);
  return config;
}

std::optional<std::filesystem::path>
fixedAnchorFile(const boost::property_tree::ptree& anchor, const std::string& filename)
{
  const auto type = anchor.get<std::string>("type", "");
  if (boost::iequals(type, "file")) {
    if (anchor.get_optional<std::string>("refresh"))
      throw std::runtime_error("NDNSF requires a fixed trust-anchor file without refresh");
    return std::filesystem::absolute(filename).parent_path() /
           anchor.get<std::string>("file-name");
  }
  if (boost::iequals(type, "base64")) {
    anchor.get<std::string>("base64-string");
    return std::nullopt;
  }
  throw std::runtime_error("NDNSF requires fixed file/base64 trust anchors; type " +
                           type + " is not allowed");
}

} // namespace trust_schema_detail

std::vector<TrustSchemaResource>
enumerateTrustSchemaResources(const std::string& filename)
{
  const auto schema = std::filesystem::absolute(filename).string();
  std::ifstream input(schema);
  if (!input)
    throw std::runtime_error("Cannot open NDNSF trust schema: " + schema);
  std::string bytes(MAX_TRUST_SCHEMA_INSPECTION_BYTES + 1, '\0');
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (input.bad() || (!input.eof() && input.fail()) ||
      input.gcount() > static_cast<std::streamsize>(MAX_TRUST_SCHEMA_INSPECTION_BYTES))
    throw std::runtime_error("trust schema exceeds inspection byte limit or is unreadable");
  bytes.resize(static_cast<size_t>(input.gcount()));
  std::istringstream stream(bytes);
  const auto config = trust_schema_detail::readTrustSchema(stream, schema);
  std::vector<TrustSchemaResource> resources{{schema, true}};
  std::set<std::string> seen{schema};
  bool hasAnchor = false;
  for (const auto& entry : config) {
    if (!boost::iequals(entry.first, "trust-anchor"))
      continue;
    hasAnchor = true;
    if (const auto file = trust_schema_detail::fixedAnchorFile(entry.second, schema)) {
      if (seen.insert(file->string()).second) {
        if (resources.size() >= MAX_TRUST_SCHEMA_RESOURCES - 1)
          throw std::runtime_error("trust schema exceeds inspection resource limit");
        resources.push_back({file->string(), true});
      }
    }
  }
  if (!hasAnchor)
    throw std::runtime_error("NDNSF trust schema must contain a fixed trust anchor");
  if (seen.insert(schema + ".producer-signers").second)
    resources.push_back({schema + ".producer-signers", false});
  return resources;
}

} // namespace ndn_service_framework
