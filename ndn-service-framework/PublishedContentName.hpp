#pragma once

#include <ndn-cxx/name.hpp>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>

namespace ndn_service_framework {

enum class ContentDataKind {
  Blob = 0, ModelFile = 1, Weights = 2, Initializer = 3,
  Tokenizer = 4, ArtifactFile = 5, Material = 6, Input = 7, Assignment = 8
};

/** Which business object of N4 an instance is. The role, not the label, selects the name row:
 * `kind` states the wire family and `semanticRole` the exact object, and the two are validated together. */
enum class ContentSemanticRole : std::uint8_t {
  None = 0,
  SourceGraph = 1,
  Initializers = 2,
  MaterialBundle = 3,
  MaterialManifest = 4,
  AssemblyReceipt = 5,
  PreparedMetadata = 6,
  ModelManifest = 7,
  AuthorizationManifest = 8,
  FixedAssignment = 9,
  CanonicalCapsule = 10,
  StoredCapsule = 11,
  WeightsObject = 12,
  TokenizerObject = 13,
  ArtifactFileObject = 14,
  ArtifactManifest = 15,
  ManifestPage = 16,
  SemanticInput = 17,
  BlobObject = 18,
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

  // N4 semantic coordinates. Absent facts are stated explicitly, never guessed from a label.
  ContentSemanticRole semanticRole = ContentSemanticRole::None;
  std::string modelFamily;
  std::string modelSize;
  std::string quantization;
  std::string sourceRevisionLabel;
  std::optional<std::uint64_t> bundleIndex;
  std::string profileLabel;
  std::string useId;

  std::string objectIdentity() const;
  /** N4 `<ObjectPrefix>` without the `/rep/<RepresentationId>` tail. Throws when the instance carries no
   * semantic role, because the legacy shape is then still the only defined name. */
  ndn::Name semanticStem(const ndn::Name& producer) const;
  /** N4 `<ObjectPrefix>` including the representation tail. */
  ndn::Name representationPrefix(const ndn::Name& producer,
                                 const std::string& representationId) const;
  /** N2 `<ModelPath>` components: family, size, quantization, "revision", label. */
  std::vector<std::string> modelPathComponents() const;
  /** True when any N4 coordinate field is set, i.e. the extended wire layout applies. */
  bool hasSemanticCoordinates() const;
  ndn::Name contentPrefix(const ndn::Name& producer,
                          const std::string& plaintextDigest) const;
  std::vector<std::uint8_t> encode() const;
  static PublishedContentName decode(const std::vector<std::uint8_t>& wire);
};

} // namespace ndn_service_framework
