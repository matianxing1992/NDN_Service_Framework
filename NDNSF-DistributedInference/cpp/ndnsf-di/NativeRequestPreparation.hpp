#ifndef NDNSF_DI_NATIVE_REQUEST_PREPARATION_HPP
#define NDNSF_DI_NATIVE_REQUEST_PREPARATION_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ndnsf::di {

struct NativePreparedInput
{
  std::string modelName;
  std::string modelDigest;
  std::string taskName;
  std::string inputSchemaDigest;
  std::string optionsSchemaDigest;
  std::vector<std::uint8_t> payload;
  std::string repositoryReference;
  std::chrono::steady_clock::time_point deadline;
  std::string adapterId;
  std::string adapterVersion;
  bool encoded = false;
  NativeModelDescriptor expectedModel;

  void validate() const;
};

struct NativeInspectedModel
{
  NativeModelDescriptor descriptor;
  NativeGraphSnapshot graph;
  std::string canonicalSourceName;
  std::string canonicalSourceDigest;
  std::string modelManifestDigest;
  // The adapter's planning graph and the canonical ONNX assembly graph have
  // separate identities; inspection must resolve both from the model source.
  std::string canonicalGraphDigest;

  // Resolved object facts required when a publisher replaces the business root.
  // The initializer object hash differs from the normalized initializer identity.
  std::uint64_t canonicalSourceBytes = 0;
  std::string canonicalInitializerObjectDigest;
  std::uint64_t canonicalInitializerBytes = 0;

  void validate() const;
};

struct NativeArtifactBinding
{
  std::map<std::string, std::string> sourceByRole;
  std::map<std::string, std::string> artifactDigestByRole;
  std::string manifestDigest;
  std::string recipeDigest;
  // Filled by ensureArtifacts from the checked request/model, not by its
  // publication port. The sealer refuses a foreign or unbound result.
  std::string requestId;
  std::uint64_t attempt = 0;
  std::string modelDigest;
  std::string graphDigest;

  std::string canonicalGraphDigest;

  // Optional publication receipt: exact business root bytes, not Core transport
  // metadata. Stable artifact identities are separate from sourceByRole fetch names.
  std::string canonicalManifestJson;
  std::map<std::string, std::string> artifactNameByRole;

  void validate() const;
};

struct NativeRequestControl
{
  std::string requestId;
  std::uint64_t attempt = 0;
  std::chrono::steady_clock::time_point deadline;
  // Thread-safe request-owner predicate: preparation and Core I/O publication
  // may observe cancellation concurrently. Captured state must outlive callbacks.
  std::function<bool()> cancelled;

  void requireActive() const;
};

class NativeRequestPreparation
{
public:
  // Native catalog/inspection owner returns actual authenticated source metadata.
  using InspectPort = std::function<NativeInspectedModel(
    const NativePreparedInput&, const NativeModelDescriptor&)>;
  using ArtifactPort = std::function<NativeArtifactBinding(
    const NativeInspectedModel&, const NativeSplitCandidate&,
    const std::vector<NativeSelectionRoleV3>&, const NativeRequestControl&)>;
  using RolePort = std::function<std::vector<NativeSelectionRoleV3>(
    const NativeInspectedModel&, const NativeSplitCandidate&, const NativeRequestControl&)>;

  explicit NativeRequestPreparation(std::shared_ptr<const NativeAdapterRegistry> adapters,
                                    InspectPort inspect = {}, ArtifactPort artifacts = {}, RolePort roles = {});

  NativePreparedInput prepareInput(const NativeModelDescriptor& model,
                                   std::string taskName,
                                   std::string inputSchemaDigest,
                                   std::string optionsSchemaDigest,
                                   std::vector<std::uint8_t> payload,
                                   std::string repositoryReference,
                                   std::chrono::steady_clock::time_point deadline) const;

  NativeInspectedModel inspectModel(const NativePreparedInput& input) const;

  std::vector<NativeSelectionRoleV3> prepareRoles(const NativeInspectedModel& model,
    const NativeSplitCandidate& candidate, const NativeRequestControl& control) const;

  static void validateRoles(const NativeInspectedModel& model, const NativeSplitCandidate& candidate,
    const std::vector<NativeSelectionRoleV3>& roles);

  NativeArtifactBinding ensureArtifacts(const NativeInspectedModel& model,
                                        const NativeSplitCandidate& candidate,
                                        const NativeRolePlacementProposalV3& proposal,
                                        const NativeRequestControl& control) const;

  // Validate publication against inspection and derive final certificates without
  // changing the selected artifacts, devices, role cover or placement identity.
  static std::vector<NativeSelectionRoleV3> bindPublishedRoles(
    const NativeInspectedModel& model, const NativeSplitCandidate& candidate,
    const std::vector<NativeSelectionRoleV3>& roles, const NativeArtifactBinding& artifacts);

private:
  std::shared_ptr<const NativeAdapterRegistry> m_adapters;
  InspectPort m_inspect;
  ArtifactPort m_artifacts;
  RolePort m_roles;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_REQUEST_PREPARATION_HPP
