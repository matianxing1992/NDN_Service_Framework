#pragma once

#include "PublishedContentName.hpp"
#include "ProducerDataNaming.hpp"

#include <ndn-cxx/name.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ndn_service_framework {

/** Non-secret immutable descriptor. Signature validation belongs to the
 * transport caller; decoding also requires its trusted Producer context. */
struct ProtectedBlobManifest
{
  ndn::Name producer;
  ndn::Name dataPrefix;
  std::vector<std::uint8_t> plaintextDigest;
  std::uint64_t plaintextBytes = 0;
  std::uint64_t envelopeBytes = 0;
  std::uint64_t segmentBytes = 0;
  std::uint64_t segmentCount = 0;
  std::string representationSpec;
  std::string protectionSpec;
  std::string authorizationScope;
  std::string protectionEpoch;
  std::string keyReferenceId;
  std::string keyIdentifier;
  ndn::Name manifestSuffix;
  std::optional<PublishedContentName> contentName;

  void validate(DataNameReadMode mode = DataNameReadMode::StrictCanonical) const;
  std::vector<std::uint8_t> encode(DataNameReadMode mode = DataNameReadMode::StrictCanonical) const;
  ndn::Name getName(DataNameReadMode mode = DataNameReadMode::StrictCanonical) const;
  static void validateSuffix(const ndn::Name& suffix,
                            DataNameReadMode mode = DataNameReadMode::StrictCanonical);
  static std::optional<ndn::Name> inferProducer(const ndn::Name& manifestName,
                            DataNameReadMode mode = DataNameReadMode::StrictCanonical);
  static std::optional<ndn::Name> inferDataProducer(const ndn::Name& dataPrefix,
                            DataNameReadMode mode = DataNameReadMode::StrictCanonical);
  static ProtectedBlobManifest decode(
    const std::vector<std::uint8_t>& payload,
    const ndn::Name& expectedProducer, const ndn::Name& expectedManifestName,
    DataNameReadMode mode = DataNameReadMode::StrictCanonical);
};
} // namespace ndn_service_framework
