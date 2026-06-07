#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"

#include "ndn-service-framework/utils.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
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

std::string
plannedSegmentOrFalse(const std::vector<std::string>& plannedSegmentNames,
                      bool last = false)
{
  if (plannedSegmentNames.empty()) {
    return "false";
  }
  return last ? plannedSegmentNames.back() : plannedSegmentNames.front();
}

bool
runtimeTimingEnabled()
{
  const char* value = std::getenv("NDNSF_DI_RUNTIME_TIMING");
  if (value == nullptr) {
    return false;
  }
  const std::string text(value);
  return !(text.empty() || text == "0" || text == "false" || text == "FALSE" ||
           text == "off" || text == "OFF");
}

class NativeProviderHandlerState
{
public:
  explicit NativeProviderHandlerState(const NativeProviderHandlerConfig& config)
    : plan(config.plan)
    , baseAssignment(config.assignment)
    , runnerSpecs(config.runnerSpecs)
    , runnerFactory(config.runnerFactory)
    , runtime(config.workerCount)
  {
    if (!runnerFactory) {
      throw std::invalid_argument(
        "NativeProviderHandlerState requires NativeModelRunnerFactory");
    }
    for (const auto& spec : runnerSpecs) {
      runtime.registerRunner(spec.role, runnerFactory->create(spec));
    }
  }

  NativeExecutionPlan plan;
  NativeProviderAssignment baseAssignment;
  std::vector<NativeModelRunnerSpec> runnerSpecs;
  std::shared_ptr<NativeModelRunnerFactory> runnerFactory;
  NativeProviderRuntime runtime;
};

void
logProviderTiming(const std::string& sessionId,
                  const std::string& role,
                  const ProviderRoleResult& result,
                  std::chrono::steady_clock::time_point baseSteady,
                  long long baseEpochMs)
{
  if (!runtimeTimingEnabled()) {
    return;
  }

  const auto workerQueueWaitMs = durationMs(result.timing.queuedAt,
                                            result.timing.workerStartedAt);
  const auto inputFetchWaitMs = durationMs(result.timing.workerStartedAt,
                                           result.timing.startedAt);
  const auto runnerPublishMs = durationMs(result.timing.startedAt,
                                          result.timing.finishedAt);
  const auto handlerMs = durationMs(result.timing.workerStartedAt,
                                    result.timing.finishedAt);
  const auto totalMs = durationMs(result.timing.queuedAt,
                                  result.timing.finishedAt);
  const auto workerStartEpoch = approxEpochMs(baseSteady, baseEpochMs,
                                              result.timing.workerStartedAt);
  const auto startEpoch = approxEpochMs(baseSteady, baseEpochMs,
                                        result.timing.startedAt);
  const auto endEpoch = approxEpochMs(baseSteady, baseEpochMs, result.timing.finishedAt);

  std::cout << std::fixed << std::setprecision(3)
            << "\nNDNSF_DI_PROVIDER_HANDLER_TIMING"
            << " event=start"
            << " session=" << sessionId
            << " role=" << role
            << " submitted_epoch_ms=" << baseEpochMs
            << " worker_start_epoch_ms=" << workerStartEpoch
            << " start_epoch_ms=" << startEpoch
            << " queue_wait_ms=" << workerQueueWaitMs
            << " worker_queue_wait_ms=" << workerQueueWaitMs
            << " input_fetch_wait_ms=" << inputFetchWaitMs
            << " runner_publish_ms=0"
            << " total_ms=0"
            << " handler_ms=0"
            << std::endl;
  std::cout << std::fixed << std::setprecision(3)
            << "\nNDNSF_DI_PROVIDER_HANDLER_TIMING"
            << " event=end"
            << " session=" << sessionId
            << " role=" << role
            << " submitted_epoch_ms=" << baseEpochMs
            << " worker_start_epoch_ms=" << workerStartEpoch
            << " start_epoch_ms=" << startEpoch
            << " end_epoch_ms=" << endEpoch
            << " queue_wait_ms=" << workerQueueWaitMs
            << " worker_queue_wait_ms=" << workerQueueWaitMs
            << " input_fetch_wait_ms=" << inputFetchWaitMs
            << " runner_publish_ms=" << runnerPublishMs
            << " total_ms=" << totalMs
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
              << " planned_segment_count=" << timing.plannedSegmentNames.size()
              << " first_planned_segment="
              << plannedSegmentOrFalse(timing.plannedSegmentNames)
              << " last_planned_segment="
              << plannedSegmentOrFalse(timing.plannedSegmentNames, true)
              << " data_name=" << plannedNameOrFalse(timing.plannedDataName)
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
              << " planned_segment_count=" << timing.plannedSegmentNames.size()
              << " first_planned_segment="
              << plannedSegmentOrFalse(timing.plannedSegmentNames)
              << " last_planned_segment="
              << plannedSegmentOrFalse(timing.plannedSegmentNames, true)
              << " data_name=" << plannedNameOrFalse(timing.plannedDataName)
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
  auto state = std::make_shared<NativeProviderHandlerState>(config);

  return [config = std::move(config), state = std::move(state)] (
           ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
           const ndn_service_framework::RequestMessage& request) mutable {
    try {
      auto assignment = state->baseAssignment;
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

      const auto role = ctx.role();
      const auto roleSpec = roleSpecFor(state->plan,
                                        role,
                                        ctx.sessionId(),
                                        assignment,
                                        ctx.localProvider().toUri());
      auto initialInputs = roleSpec.inputs.empty()
        ? initialInputsFromRequest(ctx, request)
        : std::map<std::string, TensorBundle>{};
      const auto submittedSteady = std::chrono::steady_clock::now();
      const auto submittedEpoch = epochMs();
      auto result = state->runtime.executeRoleAsync(
        ctx.sessionId(),
        roleSpec,
        std::move(io),
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
