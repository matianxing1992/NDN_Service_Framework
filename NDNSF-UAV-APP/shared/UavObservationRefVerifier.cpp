#include "UavObservationRefVerifier.hpp"

#include <algorithm>
#include <charconv>
#include <regex>
#include <stdexcept>

namespace ndnsf::examples::uav {

ndn::Name
makeUavStaticObservationPrefix(const ndn::Name& revisionPrefix,
                               const std::string& canonicalContentSha256)
{
  if (revisionPrefix.empty() ||
      !std::regex_match(canonicalContentSha256, std::regex("sha256:[0-9a-f]{64}"))) {
    throw std::runtime_error("static observation prefix or digest is malformed");
  }
  auto prefix = revisionPrefix;
  return prefix.append(canonicalContentSha256.substr(7));
}

ParsedObservationReference
parseObservationReference(const std::string& exactName, const std::string& contentSha256,
                          const std::string& producerIdentity, const std::string& category)
{
  if (!std::regex_match(contentSha256, std::regex("sha256:[0-9a-f]{64}"))) {
    throw std::runtime_error(category + " DataRef digest is malformed");
  }
  ndn::Name name(exactName);
  ndn::Name producer(producerIdentity);
  if (name.toUri() != exactName || producer.toUri() != producerIdentity ||
      producer.empty() || !producer.isPrefixOf(name) || name.size() <= producer.size()) {
    throw std::runtime_error(category + " DataRef name is not canonical or producer-owned");
  }
  return {name, producer, contentSha256};
}

void
requireObservationReferenceNameShape(const ParsedObservationReference& reference,
                                     const std::string& category, const std::string& camera,
                                     uint64_t epoch, uint64_t sequence)
{
  ndn::Name prefix = reference.producerIdentity;
  prefix.append("UAV");
  const bool isStatic = category == "CLOCK" || category == "CALIBRATION" || category == "GROUND";
  if (isStatic) {
    prefix.append(category);
    if (category == "CALIBRATION") prefix.append(camera);
  }
  else if (category == "POSE") {
    prefix.append("OBS").append(camera).append(std::to_string(epoch))
      .append(std::to_string(sequence)).append("POSE");
  }
  else {
    throw std::runtime_error("unsupported observation reference category");
  }
  const auto reject = [&] {
    throw std::runtime_error(category + " DataRef has an invalid typed name shape");
  };
  const auto suffixSize = reference.exactName.size() -
    std::min(reference.exactName.size(), prefix.size());
  if (reference.producerIdentity.empty() || !prefix.isPrefixOf(reference.exactName) ||
      (isStatic ? suffixSize != 2 && suffixSize != 3 : suffixSize != 1)) {
    reject();
  }
  if (isStatic) {
    const auto& component = reference.exactName.get(prefix.size());
    const auto revision = component.toUri();
    uint64_t value = 0;
    if (!component.isGeneric() || revision.empty() || revision.front() == '0') reject();
    const auto parsed = std::from_chars(revision.data(), revision.data() + revision.size(), value);
    if (parsed.ec != std::errc() || parsed.ptr != revision.data() + revision.size() || value == 0)
      reject();
    if (suffixSize == 3) {
      const auto& digest = reference.exactName.get(-2);
      if (!digest.isGeneric() || reference.contentSha256.size() != 71 ||
          !std::regex_match(reference.contentSha256, std::regex("sha256:[0-9a-f]{64}")) ||
          digest.toUri() != reference.contentSha256.substr(7)) reject();
    }
  }
  const auto& version = reference.exactName.get(-1);
  try {
    if (!version.isVersion() || version.toVersion() == 0 ||
        version != ndn::name::Component::fromVersion(version.toVersion())) reject();
  }
  catch (const ndn::name::Component::Error&) {
    reject();
  }
}

void
validateObservationReference(const ParsedObservationReference& reference,
                             const ndn::Name& expectedName,
                             const ndn::Name& expectedProducer,
                             const std::string& expectedDigest,
                             const std::string& category)
{
  bool nameMatches = reference.exactName == expectedName;
  const bool isStatic = category == "CLOCK" || category == "CALIBRATION" || category == "GROUND";
  if (!nameMatches && isStatic && !expectedName.empty() &&
      expectedName.get(-1).isVersion()) {
    auto boundName = makeUavStaticObservationPrefix(expectedName.getPrefix(-1), expectedDigest);
    boundName.append(expectedName.get(-1));
    nameMatches = reference.exactName == boundName;
  }
  if (!nameMatches || reference.contentSha256 != expectedDigest ||
      reference.producerIdentity != expectedProducer) {
    throw std::runtime_error("observation " + category +
                             " DataRef does not match its exact object");
  }
}

} // namespace ndnsf::examples::uav
