#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include <ndn-cxx/name.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <openssl/sha.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>

namespace ndnsf::di {
namespace {
std::runtime_error reject(const std::string& message)
{
  return std::runtime_error("DI_PROTECTED_GRANT_REJECTED: " + message);
}
std::uint64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string digest(const std::string& bytes)
{
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), hash);
  std::string out = "sha256:";
  for (const auto c : hash) {
    out += "0123456789abcdef"[c >> 4]; out += "0123456789abcdef"[c & 15];
  }
  return out;
}

void bindGroupMetadataFromSelection(
  ProtectedRuntimeBindingV1& binding,
  const NativeSelectionProjectionV3& projection,
  const std::map<std::string, std::string>& fields,
  const std::shared_ptr<ProviderGroupCoordinator>& group)
{
  if (group && group->hasCapability()) {
    const auto& capability = group->capability();
    binding.capabilityDigest = capability.capabilityDigest;
    binding.groupId = capability.groupId;
    binding.groupEpoch = capability.epoch;
    binding.epochKeyId = capability.epochKeyId;
    return;
  }

  // During the production overlap path the request-scoped coordinator is
  // still doing its local TPM unwrap.  The protected grant binding needs only
  // the immutable capability commitments at this point; the authenticated
  // coordinator is joined and compared before execution below.
  const auto field = fields.find("groupCapabilityV1");
  if (field == fields.end()) {
    return;
  }
  const auto decodedWire = ndn_service_framework::selectionGatedUnhex(field->second);
  auto capability = ProviderGroupCoordinator::decodeCapability(
    ProviderGroupBytes(decodedWire.begin(), decodedWire.end()));
  if (capability.requestId != projection.requestId ||
      capability.planDigest != projection.planDigest) {
    throw reject("group capability request/plan binding mismatch");
  }
  binding.capabilityDigest = capability.capabilityDigest;
  binding.groupId = capability.groupId;
  binding.groupEpoch = capability.epoch;
  binding.epochKeyId = capability.epochKeyId;
}
} // namespace

std::string nativeProtectedFencingToken(
  const NativeSelectionProjectionV3& projection, const std::string& providerBootId,
  const std::map<std::string, std::string>& fields)
{
  const auto leaseFence = nativeProviderFieldValue(fields,
    {"executionFencingToken", "fencingToken", "admissionFencingToken"});
  if (!leaseFence.empty()) return leaseFence;
  // Non-leased V3 requests have no lease token. Their local attempt fence is
  // bound to the authenticated Selection and this Provider incarnation.
  return digest("NDNSF-DI/protected-attempt-fence/v1\n" + providerBootId + "\n" +
    projection.provider + "\n" + projection.requestId + "\n" +
    std::to_string(projection.attempt) + "\n" + projection.planDigest);
}

void installNativeProviderGroupCoordinatorFactory(
  NativeProviderHandlerConfig& config, const std::string& localProvider,
  const std::string& providerCertificateName, ndn::KeyChain* keyChain)
{
  const bool requiresGroupCapability = std::any_of(
    config.plan.dependencies.begin(), config.plan.dependencies.end(),
    [] (const NativeDependencySpec& dependency) {
      return dependency.useNdnsfDataV1;
    });
  config.groupCoordinatorFactory =
    [requiresGroupCapability, localProvider,
     providerCertName = ndn::Name(providerCertificateName),
     expectedPlanDigest = config.planDigest, keyChain] (
        ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
        const std::map<std::string, std::string>& fields) {
      const auto field = fields.find("groupCapabilityV1");
      if (field == fields.end()) {
        if (requiresGroupCapability) {
          throw std::runtime_error(
            "NDNSF_DATA_V1 assignment is missing groupCapabilityV1");
        }
        return std::shared_ptr<ProviderGroupCoordinator>{};
      }
      const auto decodedWire =
        ndn_service_framework::selectionGatedUnhex(field->second);
      auto capability = ProviderGroupCoordinator::decodeCapability(
        ProviderGroupBytes(decodedWire.begin(), decodedWire.end()));
      const auto requestPlan = fields.find("executionPlanDigest");
      const auto authenticatedPlanDigest = requestPlan == fields.end()
        ? expectedPlanDigest : requestPlan->second;
      if (capability.requestId != ctx.sessionId() ||
          capability.planDigest != authenticatedPlanDigest) {
        throw std::runtime_error(
          "NDNSF_DATA_V1 capability request/plan binding mismatch");
      }
      const auto localMember = std::find_if(
        capability.orderedMembers.begin(), capability.orderedMembers.end(),
        [&localProvider] (const GroupMemberV1& member) {
          return member.provider == localProvider;
        });
      if (localMember == capability.orderedMembers.end() ||
          localMember->endpointPrefix.empty()) {
        throw std::runtime_error(
          "NDNSF_DATA_V1 capability omits the local Provider endpoint");
      }
      ProviderGroupCoordinatorOptions groupOptions;
      groupOptions.localProvider = localProvider;
      groupOptions.unwrapEpochKey =
        [keyChain, providerCertName, localProvider] (
            const std::string& providerName,
            const ProviderGroupBytes& wrapped) {
          if (providerName != localProvider) {
            throw std::runtime_error(
              "NDNSF_DATA_V1 wrapped key targets another Provider");
          }
          if (keyChain == nullptr) {
            throw std::runtime_error(
              "NDNSF_DATA_V1 Provider has no key chain for epoch key unwrap");
          }
          const auto plaintext =
            ndn_service_framework::unwrapSelectionGatedInputKey(
              ndn::Buffer(wrapped.data(), wrapped.size()),
              providerCertName, *keyChain);
          return ProviderGroupBytes(plaintext.begin(), plaintext.end());
        };
      auto coordinator = std::make_shared<ProviderGroupCoordinator>(
        std::move(groupOptions));
      // Empty plaintext key forces RSA unwrap of exactly the local Provider's
      // wrapped epoch key; the default inner authenticator is request-scoped.
      coordinator->installCapability(std::move(capability), {}, true);
      return coordinator;
    };
  // The coordinator's local key unwrap may overlap protected-grant acquisition.
  // NativeProviderHandler joins it before binding validation or execution.
  config.overlapGroupCoordinatorFactory = true;
}

void installNativeProtectedGrantFactory(NativeProviderHandlerConfig& config)
{
  const auto boot = config.providerBootId;
  const auto modelFamily = nativeProtectedModelFamily(config.plan.modelName);
  const auto grantFetcher = config.protectedGrantFetcher;
  config.protectedRuntimeFactory = [boot, modelFamily, grantFetcher] (
      ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
      const NativeSelectionProjectionV3& projection,
      const std::shared_ptr<ProviderGroupCoordinator>& group) {
    auto keys = loadNativeProtectedGrantConfig(ctx.localProvider().toUri(), modelFamily,
                                              projection.selectedRoleContract().protectionEpoch);
    keys.modelManifestDigest = projection.selectedRoleContract().modelManifestDigest;
    keys.publicationIdentity = ctx.requesterName().toUri(ndn::name::UriFormat::CANONICAL);
    if (keys.modelManifestDigest.empty())
      throw reject("sealed current grant lacks model commitment");
    const auto fields = parseNativeProviderAssignmentFields(
      ctx.assignment().assignmentPayload, projection.executionRole.roleId, &projection);
    ProtectedRuntimeBindingV1 binding;
    binding.provider = projection.provider;
    binding.role = projection.executionRole.roleId;
    binding.requestId = projection.requestId;
    binding.attempt = projection.attempt;
    binding.planCoreDigest = projection.planCoreDigest;
    binding.planDigest = projection.planDigest;
    if (projection.grantLeaseScope) {
      binding.grantRequestId = projection.grantLeaseScope->requestId;
      binding.grantAttempt = projection.grantLeaseScope->attempt;
      binding.grantPlanCoreDigest = projection.grantLeaseScope->planCoreDigest;
      binding.grantExpiresAtMs = projection.grantLeaseScope->expiresAtMs;
    }
    binding.securityPolicySnapshotDigest = projection.securityPolicySnapshotDigest;
    binding.protectionEpoch = projection.selectedRoleContract().protectionEpoch;
    binding.grantName = projection.grantName;
    binding.grantDigest = projection.grantDigest;
    binding.providerBootId = boot;
    binding.fencingToken = nativeProtectedFencingToken(projection, boot, fields);
    binding.expiresAtMs = projection.deadlineMs;
    for (const auto& endpoint : projection.dataflow.mayPublish) {
      binding.mayPublishEndpointDigests.insert(endpoint.endpointDigest);
      binding.mayPublishConsumerByEndpoint[endpoint.endpointDigest] = endpoint.consumerRole;
    }
    for (const auto& endpoint : projection.dataflow.mustFetch) {
      if (endpoint.sourceKind == "APPLICATION_INPUT" || endpoint.operation == "APPLICATION_INPUT") continue;
      binding.mustFetchEndpointDigests.insert(endpoint.endpointDigest);
      binding.mustFetchProducerByEndpoint[endpoint.endpointDigest] = endpoint.producerRole;
    }
    bindGroupMetadataFromSelection(binding, projection, fields, group);
    const auto now = nowMs();
    if (now >= projection.deadlineMs) throw reject("request expired before grant acquisition");
    keys.shouldCancel = [&ctx] { return ctx.isStreamed() && ctx.streamCancelled(); };
    keys.fetchGrant = [limit = static_cast<int>(std::min<std::uint64_t>(
                        30000, projection.deadlineMs - now)),
                       cancelled = keys.shouldCancel, grantFetcher] (const std::string& name) {
      if (grantFetcher)
        return grantFetcher(name, limit, cancelled);
      return fetchNativeProtectedGrant(name, limit, cancelled);
    };
    auto runtime = std::make_shared<ProtectedRuntime>(binding, std::move(keys));
    runtime->verifyGrant(binding, now);
    return runtime;
  };
}
} // namespace ndnsf::di
