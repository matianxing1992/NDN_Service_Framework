#pragma once

#include <ndn-cxx/name.hpp>
#include <string>
#include <vector>
#include <cstdint>

namespace ndn_service_framework {

enum class ContentDataKind {
  Blob = 0, ModelFile = 1, Weights = 2, Initializer = 3,
  Tokenizer = 4, ArtifactFile = 5, Material = 6, Input = 7, Assignment = 8
};

/** Non-secret, immutable semantic metadata supplied by the publication owner.
 * Model identity must come from the verified model descriptor. No Producer,
 * request identity, encryption keys or replica locations belong in this value. */
struct PublishedContentName {
  ContentDataKind kind = ContentDataKind::Blob;
  std::string label;
  std::string modelName;
  std::string modelIdentity;
  std::string artifactKind;
  std::string coordinate;
  std::string profileIdentity;
  std::string inputKind;
  // Assignment uses label=selected role, verified modelName/modelIdentity;
  // its logical content prefix binds the complete canonical fixed payload.

  std::string objectIdentity() const;
  ndn::Name contentPrefix(const ndn::Name& producer,
                          const std::string& plaintextDigest) const;
  std::vector<std::uint8_t> encode() const;
  static PublishedContentName decode(const std::vector<std::uint8_t>& wire);
};

} // namespace ndn_service_framework
