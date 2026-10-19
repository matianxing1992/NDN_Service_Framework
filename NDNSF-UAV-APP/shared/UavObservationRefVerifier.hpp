#pragma once

#include <ndn-cxx/name.hpp>

#include <cstdint>
#include <string>

namespace ndnsf::examples::uav {

struct ParsedObservationReference
{
  ndn::Name exactName;
  ndn::Name producerIdentity;
  std::string contentSha256;
};

/** Parse and validate the canonical name, producer prefix, and SHA-256 fields. */
ParsedObservationReference
parseObservationReference(const std::string& exactName, const std::string& contentSha256,
                          const std::string& producerIdentity, const std::string& category);

/** Require a reference name to match the typed path for this frame and category. */
void
requireObservationReferenceNameShape(const ParsedObservationReference& reference,
                                     const std::string& category, const std::string& camera,
                                     uint64_t epoch, uint64_t sequence);

/** Bind a parsed reference to the exact expected object, signer prefix, and payload digest. */
void
validateObservationReference(const ParsedObservationReference& reference,
                             const ndn::Name& expectedName,
                             const ndn::Name& expectedProducer,
                             const std::string& expectedDigest,
                             const std::string& category);

} // namespace ndnsf::examples::uav
