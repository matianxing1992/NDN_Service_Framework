#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_HANDLER_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_HANDLER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderSession.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"

#include <memory>
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

      if (roleSpec.outputs.empty() && !config.finalResponseScope.empty()) {
        const auto found = result.outputsByScope.find(config.finalResponseScope);
        if (found != result.outputsByScope.end()) {
          const auto& payload = found->second.payload;
          ctx.publishFinalResponse(ndn::Buffer(payload.data(), payload.size()));
        }
      }
    }
    catch (const std::exception& exc) {
      ctx.fail(exc.what());
    }
  };
}

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_HANDLER_HPP
