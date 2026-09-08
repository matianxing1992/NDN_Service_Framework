#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"

namespace ndn_service_framework {
class ServiceUser;
struct PreparedServiceRequest;
struct LargeDataPublishResult;
}

namespace ndnsf::di {

struct NativeCanonicalPublicationOptions
{
  std::string artifactRoot;
  // YOLO preserves its package provenance; Tiny Qwen supplies layer manifests.
  std::string packageManifestDigest;
  std::vector<std::string> layerManifestDigests;
};

/** Requester-side canonical publication through the existing Core crypto owner.
 * Source resolution is native and returns immutable owned bytes. It must honor
 * the supplied control and authenticate remote sources before returning them.
 * This object is copyable; artifactPort() retains its Core owner and source port.
 */
class NativeCanonicalArtifactPublisher
{
public:
  using SourcePort = std::function<std::shared_ptr<const NativeCanonicalSource>(
    const NativeInspectedModel&, const NativeRequestControl&)>;

  NativeCanonicalArtifactPublisher(std::shared_ptr<ndn_service_framework::ServiceUser> user,
    std::string serviceName, NativeCanonicalPublicationOptions options, SourcePort source);

  NativeArtifactBinding operator()(const NativeInspectedModel& model,
    const NativeSplitCandidate& candidate, const std::vector<NativeSelectionRoleV3>& roles,
    const NativeRequestControl& control) const;

  NativeRequestPreparation::ArtifactPort artifactPort() const;

private:
  // Test-only scheduling/transport seam. Production construction always binds
  // ServiceUser::postToIo/prepareServiceRequest/publishEncryptedLargeData.
  friend class NativeCanonicalPublisherTestAccess;
  struct Transport
  {
    std::function<void(std::function<void()>)> post;
    std::function<bool()> isOnIoThread;
    std::function<ndn_service_framework::PreparedServiceRequest()> begin;
    std::function<ndn_service_framework::LargeDataPublishResult(
      const ndn_service_framework::PreparedServiceRequest&, const std::vector<std::uint8_t>&,
      const std::string&)> publish;
  };
  NativeCanonicalArtifactPublisher(Transport transport, std::string serviceName,
    NativeCanonicalPublicationOptions options, SourcePort source);

  Transport m_transport;
  std::string m_serviceName;
  NativeCanonicalPublicationOptions m_options;
  SourcePort m_source;
};
}
