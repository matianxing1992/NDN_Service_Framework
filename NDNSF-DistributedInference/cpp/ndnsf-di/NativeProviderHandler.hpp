#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_HANDLER_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_HANDLER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderSession.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ndnsf::di {

struct NativeProviderHandlerConfig
{
  NativeExecutionPlan plan;
  NativeProviderAssignment assignment;
  std::shared_ptr<NativeModelRunnerFactory> runnerFactory;
  std::vector<NativeModelRunnerSpec> runnerSpecs;
  std::string finalResponseScope = "final-response";
  int fetchTimeoutMs = 10000;
  std::size_t maxSegmentSize = 7000;
  int freshnessMs = 60000;
  std::size_t workerCount = 1;
};

inline std::optional<std::vector<uint8_t>>
nativeProviderFinalResponsePayload(const RoleSpec& roleSpec,
                                   const ProviderRoleResult& result,
                                   const std::string& finalResponseScope)
{
  if (!roleSpec.outputs.empty() || finalResponseScope.empty()) {
    return std::nullopt;
  }

  const auto found = result.outputsByScope.find(finalResponseScope);
  if (found == result.outputsByScope.end()) {
    return std::nullopt;
  }
  return found->second.payload;
}

inline ndn_service_framework::ServiceProvider::CollaborationHandler
makeNativeProviderCollaborationHandler(NativeProviderHandlerConfig config)
{
  if (!config.runnerFactory) {
    throw std::invalid_argument(
      "NativeProviderHandlerConfig requires NativeModelRunnerFactory");
  }

  return [config = std::move(config)] (
           ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
           const ndn_service_framework::RequestMessage&) mutable {
    try {
      auto io = std::make_shared<NdnsfCollaborationDependencyIo>(
        ctx,
        config.fetchTimeoutMs,
        config.maxSegmentSize,
        config.freshnessMs);
      NativeProviderSession session(
        config.plan,
        config.assignment,
        std::move(io),
        config.runnerFactory,
        config.workerCount);

      for (const auto& spec : config.runnerSpecs) {
        session.registerRunner(spec);
      }

      const auto role = ctx.role();
      const auto roleSpec = session.roleSpec(role, ctx.sessionId());
      auto result = session.executeRoleAsync(ctx.sessionId(), role).get();

      const auto finalPayload = nativeProviderFinalResponsePayload(
        roleSpec,
        result,
        config.finalResponseScope);
      if (finalPayload) {
        ctx.publishFinalResponse(ndn::Buffer(finalPayload->data(), finalPayload->size()));
      }
    }
    catch (const std::exception& exc) {
      ctx.fail(exc.what());
    }
  };
}

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_HANDLER_HPP
