#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include <openssl/sha.h>
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
                                              projection.selectedRole.protectionEpoch);
    keys.modelManifestDigest = projection.selectedRole.modelManifestDigest;
    if (keys.modelManifestDigest.empty()) {
      const auto offset = projection.grantName.find("/MODEL/");
      if (offset == std::string::npos) throw reject("sealed grant lacks model commitment");
      keys.modelManifestDigest = "sha256:" + projection.grantName.substr(offset + 7, 64);
    }
    const auto fields = parseNativeProviderAssignmentFields(
      ctx.assignment().assignmentPayload, projection.executionRole.roleId);
    ProtectedRuntimeBindingV1 binding;
    binding.provider = projection.provider;
    binding.role = projection.executionRole.roleId;
    binding.requestId = projection.requestId;
    binding.attempt = projection.attempt;
    binding.planCoreDigest = projection.planCoreDigest;
    binding.planDigest = projection.planDigest;
    binding.securityPolicySnapshotDigest = projection.securityPolicySnapshotDigest;
    binding.protectionEpoch = projection.selectedRole.protectionEpoch;
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
    if (group && group->hasCapability()) {
      const auto& capability = group->capability();
      binding.capabilityDigest = capability.capabilityDigest;
      binding.groupId = capability.groupId;
      binding.groupEpoch = capability.epoch;
      binding.epochKeyId = capability.epochKeyId;
    }
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
