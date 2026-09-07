#ifndef NDNSF_DI_NATIVE_REQUEST_PREPARATION_HPP
#define NDNSF_DI_NATIVE_REQUEST_PREPARATION_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"

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

  void validate() const;
};

struct NativeInspectedModel
{
  NativeModelDescriptor descriptor;
  NativeGraphSnapshot graph;
  std::string canonicalSourceName;
  std::string canonicalSourceDigest;

  void validate() const;
};

struct NativeArtifactBinding
{
  std::map<std::string, std::string> sourceByRole;
  std::map<std::string, std::string> artifactDigestByRole;
  std::string manifestDigest;
  std::string recipeDigest;

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
  using InspectPort = std::function<NativeGraphSnapshot(
    const NativePreparedInput&, const NativeModelDescriptor&)>;
  using ArtifactPort = std::function<NativeArtifactBinding(
    const NativeInspectedModel&, const NativePlacementProposal&, const NativeRequestControl&)>;

  explicit NativeRequestPreparation(std::shared_ptr<const NativeAdapterRegistry> adapters,
                                    InspectPort inspect = {}, ArtifactPort artifacts = {});

  NativePreparedInput prepareInput(const NativeModelDescriptor& model,
                                   std::string taskName,
                                   std::string inputSchemaDigest,
                                   std::string optionsSchemaDigest,
                                   std::vector<std::uint8_t> payload,
                                   std::string repositoryReference,
                                   std::chrono::steady_clock::time_point deadline) const;

  NativeInspectedModel inspectModel(const NativePreparedInput& input) const;

  NativeArtifactBinding ensureArtifacts(const NativeInspectedModel& model,
                                        const NativePlacementProposal& proposal,
                                        const NativeRequestControl& control) const;

private:
  std::shared_ptr<const NativeAdapterRegistry> m_adapters;
  InspectPort m_inspect;
  ArtifactPort m_artifacts;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_REQUEST_PREPARATION_HPP
