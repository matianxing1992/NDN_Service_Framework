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

  void validate() const;
};

struct NativeRequestControl
{
  std::string requestId;
  std::uint64_t attempt = 0;
  std::chrono::steady_clock::time_point deadline;
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

private:
  std::shared_ptr<const NativeAdapterRegistry> m_adapters;
  InspectPort m_inspect;
  ArtifactPort m_artifacts;
  RolePort m_roles;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_REQUEST_PREPARATION_HPP
