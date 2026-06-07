#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"

#include "ndn-service-framework/utils.hpp"

#include <stdexcept>
#include <utility>

namespace ndnsf::di {
namespace {

std::vector<uint8_t>
bufferToVector(const ndn::Buffer& buffer)
{
  return std::vector<uint8_t>(buffer.begin(), buffer.end());
}

std::map<std::string, TensorBundle>
initialInputsFromRequest(ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
                         const ndn_service_framework::RequestMessage& request)
{
  auto payload = request.getPayload();
  if (const auto reference = ndn_service_framework::parseLargeDataReferencePayload(payload)) {
    auto fetched = ctx.fetchEncryptedLargeData(reference->dataName);
    if (!fetched) {
      throw std::runtime_error("failed to fetch request input large-data reference: " +
                               reference->dataName.toUri());
    }
    payload = *fetched;
  }

  TensorBundle bundle;
  bundle.name = "request-input";
  bundle.payload = bufferToVector(payload);
  bundle.expectedBytes = bundle.payload.size();
  return {{"request-input", std::move(bundle)}};
}

} // namespace

std::optional<std::vector<uint8_t>>
nativeProviderFinalResponsePayload(const RoleSpec& roleSpec,
                                   const ProviderRoleResult& result,
                                   const std::string& finalResponseScope)
{
  if (!roleSpec.outputs.empty() || finalResponseScope.empty()) {
    return std::nullopt;
  }

  const auto found = result.outputsByScope.find(finalResponseScope);
  if (found != result.outputsByScope.end()) {
    return found->second.payload;
  }
  if (result.outputsByScope.size() == 1) {
    return result.outputsByScope.begin()->second.payload;
  }
  return std::nullopt;
}

ndn_service_framework::ServiceProvider::CollaborationHandler
makeNativeProviderCollaborationHandler(NativeProviderHandlerConfig config)
{
  if (!config.runnerFactory) {
    throw std::invalid_argument(
      "NativeProviderHandlerConfig requires NativeModelRunnerFactory");
  }

  return [config = std::move(config)] (
           ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
           const ndn_service_framework::RequestMessage& request) mutable {
    try {
      auto assignment = config.assignment;
      for (const auto& item : ctx.assignment().roleProviders) {
        assignment.providerByRole[item.first] = item.second.toUri();
      }
      if (!ctx.role().empty()) {
        assignment.providerByRole[ctx.role()] = ctx.localProvider().toUri();
      }

      auto io = std::make_shared<NdnsfCollaborationDependencyIo>(
        ctx,
        config.fetchTimeoutMs,
        config.maxSegmentSize,
        config.freshnessMs);
      NativeProviderSession session(
        config.plan,
        std::move(assignment),
        std::move(io),
        config.runnerFactory,
        config.workerCount);

      for (const auto& spec : config.runnerSpecs) {
        session.registerRunner(spec);
      }

      const auto role = ctx.role();
      const auto roleSpec = session.roleSpec(role, ctx.sessionId());
      auto initialInputs = roleSpec.inputs.empty()
        ? initialInputsFromRequest(ctx, request)
        : std::map<std::string, TensorBundle>{};
      auto result = session.executeRoleAsync(
        ctx.sessionId(),
        role,
        std::move(initialInputs)).get();

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
