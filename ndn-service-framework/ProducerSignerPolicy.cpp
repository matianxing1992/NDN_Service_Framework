#include "ProducerSignerPolicy.hpp"
#include "ProducerDataNaming.hpp"

#include <ndn-cxx/security/pib/key.hpp>
#include <ndn-cxx/security/certificate.hpp>
#include <boost/property_tree/info_parser.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ndn_service_framework {

ProducerSignerPolicy::ProducerSignerPolicy(const std::string& permissionFile)
{
  if (permissionFile.empty()) return;
  std::error_code statusError;
  const auto status = std::filesystem::symlink_status(permissionFile, statusError);
  if (status.type() == std::filesystem::file_type::not_found &&
      (!statusError || statusError == std::errc::no_such_file_or_directory)) return;
  if (statusError) throw std::filesystem::filesystem_error(
    "cannot inspect Producer signer permissions", permissionFile, statusError);
  if (!std::filesystem::is_regular_file(permissionFile) ||
      std::filesystem::file_size(permissionFile) > 65536)
    throw std::invalid_argument("invalid Producer signer permission file");
  std::ifstream input(permissionFile, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read Producer signer permissions");
  std::string contents(65537, '\0');
  input.read(contents.data(), contents.size());
  if (input.bad() || (!input.eof() && input.fail()) || input.gcount() > 65536)
    throw std::invalid_argument("unreadable or oversized Producer signer permissions");
  contents.resize(static_cast<std::size_t>(input.gcount()));
  std::istringstream permissionStream(contents);
  boost::property_tree::ptree config;
  boost::property_tree::read_info(permissionStream, config);
  std::size_t count = 0;
  for (const auto& entry : config) {
    if (entry.first != "producer-signer" || !entry.second.data().empty() || ++count > 128)
      throw std::invalid_argument("invalid Producer signer permission entry");
    std::map<std::string, std::string> values;
    for (const auto& field : entry.second) {
      if ((field.first != "producer" && field.first != "signer") ||
          !field.second.empty() || field.second.data().empty() ||
          !values.emplace(field.first, field.second.data()).second)
        throw std::invalid_argument("invalid Producer signer permission field");
    }
    if (values.size() != 2 || values.at("producer").front() != '/' ||
        values.at("signer").front() != '/')
      throw std::invalid_argument("Producer signer identities must be complete Names");
    const ndn::Name producer(values.at("producer")), signer(values.at("signer"));
    if (producer.empty() || signer.empty())
      throw std::invalid_argument("root identity cannot authorize Producer signing");
    checkNamingNameSize(producer);
    checkNamingNameSize(signer);
    if (!m_delegates[producer].insert(signer).second)
      throw std::invalid_argument("duplicate Producer signer permission");
  }
}

bool ProducerSignerPolicy::allowsValidatedSigner(
  const ndn::Data& data, const ndn::Name& producer) const
{
  if (producer.empty() || !producer.isPrefixOf(data.getName())) return false;
  try {
    const auto& info = data.getSignatureInfo();
    if (!info.hasKeyLocator() || info.getKeyLocator().getType() != ndn::tlv::Name)
      return false;
    const auto& keyName = info.getKeyLocator().getName();
    const auto identity = ndn::security::isValidKeyName(keyName) ?
      ndn::security::extractIdentityFromKeyName(keyName) :
      ndn::security::extractIdentityFromCertName(keyName);
    if (identity == producer) return true;
    const auto delegates = m_delegates.find(producer);
    return delegates != m_delegates.end() && delegates->second.count(identity) != 0;
  }
  catch (const std::exception&) { return false; }
}

std::optional<ndn::Name> ProducerSignerPolicy::findValidatedInvocationProducer(
  const ndn::Data& data) const
{
  if (data.getContentType() == ndn::tlv::Data) return std::nullopt;
  std::set<ndn::Name> candidates;
  try {
    const auto& keyName = data.getSignatureInfo().getKeyLocator().getName();
    candidates.insert(ndn::security::isValidKeyName(keyName) ?
      ndn::security::extractIdentityFromKeyName(keyName) :
      ndn::security::extractIdentityFromCertName(keyName));
  }
  catch (const std::exception&) { return std::nullopt; }
  for (const auto& permission : m_delegates) candidates.insert(permission.first);
  std::optional<ndn::Name> result;
  const auto& name = data.getName();
  for (const auto& actor : candidates) {
    if (name.size() <= actor.size() + 1 || !allowsValidatedSigner(data, actor) ||
        name[actor.size()] != ndn::name::Component("NDNSF")) continue;
    const auto& category = name[actor.size() + 1];
    if (category != ndn::name::Component("REQUEST") && category != ndn::name::Component("ACK") &&
        category != ndn::name::Component("SELECTION") && category != ndn::name::Component("RESPONSE")) continue;
    if (result) throw std::invalid_argument("ambiguous authorized invocation Producer boundary");
    result = actor;
  }
  return result;
}

} // namespace ndn_service_framework
