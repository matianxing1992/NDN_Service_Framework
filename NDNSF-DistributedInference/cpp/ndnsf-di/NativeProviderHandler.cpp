#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"

#include "ndn-service-framework/utils.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {
namespace {

std::vector<uint8_t>
bufferToVector(const ndn::Buffer& buffer)
{
  return std::vector<uint8_t>(buffer.begin(), buffer.end());
}

double
durationMs(std::chrono::steady_clock::time_point start,
           std::chrono::steady_clock::time_point end)
{
  return std::chrono::duration<double, std::milli>(end - start).count();
}

long long
epochMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

long long
approxEpochMs(std::chrono::steady_clock::time_point baseSteady,
              long long baseEpochMs,
              std::chrono::steady_clock::time_point point)
{
  return baseEpochMs + static_cast<long long>(durationMs(baseSteady, point));
}

std::string
plannedNameOrFalse(const std::string& plannedDataName)
{
  return plannedDataName.empty() ? "false" : plannedDataName;
}

void
logProviderTiming(const std::string& sessionId,
                  const std::string& role,
                  const ProviderRoleResult& result,
                  std::chrono::steady_clock::time_point baseSteady,
                  long long baseEpochMs)
{
  const auto queueWaitMs = durationMs(result.timing.queuedAt, result.timing.startedAt);
  const auto handlerMs = durationMs(result.timing.queuedAt, result.timing.finishedAt);
  const auto startEpoch = approxEpochMs(baseSteady, baseEpochMs, result.timing.startedAt);
  const auto endEpoch = approxEpochMs(baseSteady, baseEpochMs, result.timing.finishedAt);

  std::cout << std::fixed << std::setprecision(3)
            << "\nNDNSF_DI_PROVIDER_HANDLER_TIMING"
            << " event=start"
            << " session=" << sessionId
            << " role=" << role
            << " submitted_epoch_ms=" << baseEpochMs
            << " start_epoch_ms=" << startEpoch
            << " queue_wait_ms=" << queueWaitMs
            << " handler_ms=0"
            << std::endl;
  std::cout << std::fixed << std::setprecision(3)
            << "\nNDNSF_DI_PROVIDER_HANDLER_TIMING"
            << " event=end"
            << " session=" << sessionId
            << " role=" << role
            << " submitted_epoch_ms=" << baseEpochMs
            << " start_epoch_ms=" << startEpoch
            << " end_epoch_ms=" << endEpoch
            << " queue_wait_ms=" << queueWaitMs
            << " handler_ms=" << handlerMs
            << std::endl;

  for (const auto& timing : result.inputTimings) {
    const auto fetchMs = durationMs(timing.prefetchStartedAt, timing.fetchCompletedAt);
    const auto prefetchTotalMs = fetchMs;
    const auto prefetchOverlapMs = std::max(
      0.0,
      durationMs(timing.prefetchStartedAt, result.timing.startedAt));
    std::cout << std::fixed << std::setprecision(3)
              << "\nNDNSF_DI_DEPENDENCY_INPUT_TIMING"
              << " session=" << sessionId
              << " role=" << role
              << " producer=" << timing.producerRole
              << " scope=" << timing.scope
              << " future_wait_ms=" << fetchMs
              << " ref_wait_ms=0"
              << " fetch_ms=" << fetchMs
              << " decode_ms=0"
              << " prefetch_total_ms=" << prefetchTotalMs
              << " prefetch_overlap_ms=" << prefetchOverlapMs
              << " bytes=" << timing.bytes
              << " expected_segments=" << timing.expectedSegments
              << " expected_bytes=" << timing.expectedBytes
              << " planned_name=" << plannedNameOrFalse(timing.plannedDataName)
              << std::endl;
  }

  for (const auto& timing : result.outputTimings) {
    const auto publishMs = durationMs(timing.outputReadyAt, timing.publishDoneAt);
    std::cout << std::fixed << std::setprecision(3)
              << "\nNDNSF_DI_DEPENDENCY_OUTPUT_TIMING"
              << " session=" << sessionId
              << " role=" << role
              << " producer=" << timing.producerRole
              << " scope=" << timing.scope
              << " publish_ms=" << publishMs
              << " bytes=" << timing.bytes
              << " expected_segments=" << timing.expectedSegments
              << " expected_bytes=" << timing.expectedBytes
              << " output_ready_epoch_ms="
              << approxEpochMs(baseSteady, baseEpochMs, timing.outputReadyAt)
              << " publish_done_epoch_ms="
              << approxEpochMs(baseSteady, baseEpochMs, timing.publishDoneAt)
              << " planned_name=" << plannedNameOrFalse(timing.plannedDataName)
              << std::endl;
  }
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
      const auto submittedSteady = std::chrono::steady_clock::now();
      const auto submittedEpoch = epochMs();
      auto result = session.executeRoleAsync(
        ctx.sessionId(),
        role,
        std::move(initialInputs)).get();
      logProviderTiming(ctx.sessionId(), role, result, submittedSteady, submittedEpoch);

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
