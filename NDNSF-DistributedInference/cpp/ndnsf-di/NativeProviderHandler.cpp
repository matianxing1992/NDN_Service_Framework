#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"

#include "ndn-service-framework/utils.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
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

bool
truthyEnv(const char* name)
{
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return false;
  }
  const std::string text(value);
  return !(text.empty() || text == "0" || text == "false" || text == "FALSE" ||
           text == "off" || text == "OFF");
}

bool
nativeTraceEnabled()
{
  return runtimeTimingEnabled() || std::getenv("NDNSF_COLLAB_ASSIGNMENT_FETCH_TRACE") != nullptr;
}

bool
streamChunkDependenciesEnvEnabled()
{
  static const bool enabled = truthyEnv("NDNSF_DI_STREAM_CHUNK_DEPENDENCIES");
  return enabled;
}

std::string
metadataValue(const NativeModelRunnerSpec& spec,
              std::initializer_list<const char*> names)
{
  for (const auto* name : names) {
    const auto found = spec.metadata.find(name);
    if (found != spec.metadata.end()) {
      return found->second;
    }
  }
  return "";
}

std::string
fragmentDigestFor(const NativeModelRunnerSpec& spec)
{
  auto digest = metadataValue(
    spec,
    {"fragmentDigest", "fragment_digest", "sha256", "digest"});
  if (!digest.empty()) {
    return digest;
  }
  return spec.role.empty() ? "unknown" : "role:" + spec.role;
}

std::string
loadedResidencyFor(const NativeModelRunnerSpec& spec)
{
  auto device = metadataValue(
    spec,
    {"device", "runtimeDevice", "runtime_device", "executionProvider", "execution_provider"});
  std::transform(device.begin(), device.end(), device.begin(), [] (unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  if (device.find("cuda") != std::string::npos ||
      device.find("gpu") != std::string::npos) {
    return "GPU_LOADED";
  }
  return "CPU_RESIDENT";
}

void
logFragmentInventoryEvent(const char* event,
                          const NativeModelRunnerSpec& spec,
                          const std::string& provider = "")
{
  if (!nativeTraceEnabled()) {
    return;
  }
  std::cout << "\nNDNSF_DI_FRAGMENT_INVENTORY"
            << " event=" << event
            << " provider=" << (provider.empty() ? "unknown" : provider)
            << " role=" << spec.role
            << " fragmentDigest=" << fragmentDigestFor(spec)
            << " backend=" << (spec.backend.empty() ? "unknown" : spec.backend)
            << " path=" << (spec.path.empty() ? "none" : spec.path)
            << " residency="
            << (std::string(event) == "EVICTED" ||
                std::string(event) == "DISK_RESIDENT" ? "DISK_RESIDENT" : loadedResidencyFor(spec))
            << " epoch_ms=" << epochMs()
            << std::endl;
}

const NativeModelRunnerSpec*
runnerSpecForRole(const std::vector<NativeModelRunnerSpec>& specs,
                  const std::string& role)
{
  const auto found = std::find_if(specs.begin(), specs.end(),
                                  [&role] (const NativeModelRunnerSpec& spec) {
                                    return spec.role == role;
                                  });
  return found == specs.end() ? nullptr : &*found;
}

int
collaborationFetchTimeoutMs(int configured)
{
  const char* value = std::getenv("NDNSF_COLLAB_LARGE_INTEREST_LIFETIME_MS");
  if (value == nullptr || std::string(value).empty()) {
    return std::max(50, configured);
  }
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value || parsed <= 0) {
    return std::max(50, configured);
  }
  return static_cast<int>(std::max<long>(50, parsed));
}

class NativeProviderHandlerState
{
public:
  explicit NativeProviderHandlerState(const NativeProviderHandlerConfig& config)
    : plan(config.plan)
    , baseAssignment(config.assignment)
    , runnerSpecs(config.runnerSpecs)
    , runnerFactory(config.runnerFactory)
    , localProviderName(config.localProviderName)
    , runtime(config.workerCount)
  {
    if (!runnerFactory) {
      throw std::invalid_argument(
        "NativeProviderHandlerState requires NativeModelRunnerFactory");
    }
    for (const auto& spec : runnerSpecs) {
      logFragmentInventoryEvent("DISK_RESIDENT", spec, localProviderName);
      runtime.registerRunner(spec.role, runnerFactory->create(spec));
      logFragmentInventoryEvent(loadedResidencyFor(spec).c_str(), spec, localProviderName);
    }
  }

  ~NativeProviderHandlerState()
  {
    for (const auto& spec : runnerSpecs) {
      logFragmentInventoryEvent("EVICTED", spec, localProviderName);
    }
  }

  void
  completeExecutionLease(
    ndn_service_framework::ProviderExecutionLeaseTable* table,
    const std::string& leaseId,
    const std::string& providerEpoch,
    const std::string& requesterName,
    const std::string& role,
    std::size_t expectedRoles,
    bool completedLocalPlan)
  {
    if (table == nullptr || leaseId.empty()) {
      return;
    }
    bool shouldRelease = false;
    {
      std::lock_guard<std::mutex> lock(executionLeaseMutex);
      auto& completed = completedRolesByLease[leaseId];
      completed.insert(role);
      shouldRelease = completedLocalPlan ||
        completed.size() >= std::max<std::size_t>(1, expectedRoles);
      if (shouldRelease) {
        completedRolesByLease.erase(leaseId);
      }
    }
    if (shouldRelease) {
      const auto now = static_cast<uint64_t>(std::max<long long>(0, epochMs()));
      table->release(leaseId,
                     providerEpoch,
                     requesterName,
                     "provider-complete:" + leaseId,
                     now);
    }
  }

  NativeExecutionPlan plan;
  NativeProviderAssignment baseAssignment;
  std::vector<NativeModelRunnerSpec> runnerSpecs;
  std::shared_ptr<NativeModelRunnerFactory> runnerFactory;
  std::string localProviderName;
  NativeProviderRuntime runtime;
  std::mutex executionLeaseMutex;
  std::map<std::string, std::set<std::string>> completedRolesByLease;
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

void
logProviderCapacity(const std::string& sessionId,
                    const std::string& role,
                    const char* event,
                    const ProviderRoleWorkerSnapshot& snapshot)
{
  if (!nativeTraceEnabled()) {
    return;
  }
  std::cout << "\nNDNSF_DI_PROVIDER_CAPACITY"
            << " event=" << event
            << " session=" << sessionId
            << " role=" << role
            << " workers=" << snapshot.workerCount
            << " active_workers=" << snapshot.activeWorkerCount
            << " idle_workers=" << snapshot.idleWorkerCount()
            << " ready_queue=" << snapshot.readyQueueDepth
            << " waiting_inputs=" << snapshot.waitingForInputCount
            << " pending_work=" << snapshot.pendingWorkCount()
            << " stopping=" << (snapshot.stopping ? "true" : "false")
            << std::endl;
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

class LocalDependencyIo final : public DependencyIo
{
public:
  std::future<TensorBundle>
  prefetchInput(const std::string& sessionId, const DependencyEdge& edge) final
  {
    auto promise = std::make_shared<std::promise<TensorBundle>>();
    auto future = promise->get_future();
    const auto itemKey = key(sessionId, edge);
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      const auto found = m_available.find(itemKey);
      if (found != m_available.end()) {
        promise->set_value(found->second);
        return future;
      }
      m_waiters[itemKey].push_back(std::move(promise));
    }
    return future;
  }

  void
  publishOutput(const std::string& sessionId,
                const DependencyEdge& edge,
                const TensorBundle& bundle) final
  {
    std::vector<std::shared_ptr<std::promise<TensorBundle>>> ready;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      const auto itemKey = key(sessionId, edge);
      m_available[itemKey] = bundle;
      const auto found = m_waiters.find(itemKey);
      if (found != m_waiters.end()) {
        ready = std::move(found->second);
        m_waiters.erase(found);
      }
    }
    for (auto& promise : ready) {
      promise->set_value(bundle);
    }
  }

private:
  static std::string
  key(const std::string& sessionId, const DependencyEdge& edge)
  {
    return sessionId + "|" + edge.plannedDataName;
  }

private:
  std::mutex m_mutex;
  std::map<std::string, TensorBundle> m_available;
  std::map<std::string, std::vector<std::shared_ptr<std::promise<TensorBundle>>>> m_waiters;
};

bool
allPlanRolesAssignedToLocal(const NativeExecutionPlan& plan,
                            const NativeProviderAssignment& assignment,
                            const std::string& localProvider)
{
  if (localProvider.empty()) {
    return false;
  }
  for (const auto& role : plan.roles) {
    if (providerForRole(assignment, role, localProvider) != localProvider) {
      return false;
    }
  }
  return true;
}

std::optional<std::vector<uint8_t>>
executeLocalPlanAndFinalPayload(NativeProviderHandlerState& state,
                                const NativeProviderHandlerConfig& config,
                                const std::string& sessionId,
                                const NativeProviderAssignment& assignment,
                                const std::string& localProvider,
                                const std::map<std::string, TensorBundle>& initialInputs,
                                std::chrono::steady_clock::time_point submittedSteady,
                                long long submittedEpoch)
{
  auto io = std::make_shared<LocalDependencyIo>();
  std::vector<std::pair<std::string, std::future<ProviderRoleResult>>> futures;
  futures.reserve(state.plan.roles.size());
  for (const auto& role : state.plan.roles) {
    auto roleSpec = roleSpecFor(state.plan,
                                role,
                                sessionId,
                                assignment,
                                localProvider);
    futures.emplace_back(
      role,
      state.runtime.executeRoleAsync(
        sessionId,
        roleSpec,
        io,
        roleSpec.inputs.empty() ? initialInputs : std::map<std::string, TensorBundle>{}));
  }

  std::optional<std::vector<uint8_t>> finalPayload;
  for (auto& item : futures) {
    auto roleSpec = roleSpecFor(state.plan,
                                item.first,
                                sessionId,
                                assignment,
                                localProvider);
    auto result = item.second.get();
    logProviderTiming(sessionId,
                      item.first,
                      result,
                      submittedSteady,
                      submittedEpoch);
    auto payload = nativeProviderFinalResponsePayload(
      roleSpec,
      result,
      config.finalResponseScope);
    if (payload) {
      finalPayload = std::move(payload);
    }
  }
  return finalPayload;
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
  return std::nullopt;
}

NativeProviderCollaborationRuntime
makeNativeProviderCollaborationRuntime(NativeProviderHandlerConfig config)
{
  if (!config.runnerFactory) {
    throw std::invalid_argument(
      "NativeProviderHandlerConfig requires NativeModelRunnerFactory");
  }
  auto state = std::make_shared<NativeProviderHandlerState>(config);

  NativeProviderCollaborationRuntime runtime;
  runtime.capacitySnapshot = [state] {
    return state->runtime.snapshot();
  };
  runtime.handler = [config = std::move(config), state = std::move(state)] (
	           ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
	           const ndn_service_framework::RequestMessage& request) mutable {
    std::string activatedLeaseId;
    std::string activatedProviderEpoch;
    std::string activatedRequester;
    std::string activatedRole;
    std::size_t expectedProviderRoles = 1;
    bool completedLocalPlan = false;
    auto completeExecutionLease = [&] {
      state->completeExecutionLease(config.executionLeaseTable,
                                    activatedLeaseId,
                                    activatedProviderEpoch,
                                    activatedRequester,
                                    activatedRole,
                                    expectedProviderRoles,
                                    completedLocalPlan);
      activatedLeaseId.clear();
    };
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
        collaborationFetchTimeoutMs(config.fetchTimeoutMs),
        config.maxSegmentSize,
        config.freshnessMs,
        config.streamChunkDependencies ||
          streamChunkDependenciesEnvEnabled());

      const auto role = ctx.role();
      if (config.executionLeaseTable != nullptr) {
        const auto fields = parseNativeProviderAssignmentFields(
          ctx.assignment().assignmentPayload);
        const auto leaseId = nativeProviderFieldValue(fields, {"executionLeaseId"});
        const auto providerEpoch = nativeProviderFieldValue(
          fields, {"executionLeaseEpoch"});
        const auto transactionId = nativeProviderFieldValue(
          fields, {"executionLeaseTransactionId"});
        const auto planDigest = nativeProviderFieldValue(
          fields, {"executionLeasePlanDigest"});
        const auto bindingProofText = nativeProviderFieldValue(
          fields, {"executionLeaseBindingProof"});
        const auto providerRoleCountText = nativeProviderFieldValue(
          fields, {"executionLeaseProviderRoleCount"});
        if (leaseId.empty() || providerEpoch.empty() || transactionId.empty() ||
            planDigest.empty() || bindingProofText.empty() ||
            config.executionLeaseTargetService.empty()) {
          ctx.fail("LEASE_BINDING_MISMATCH");
          return;
        }
        ndn_service_framework::ExecutionLeaseBinding binding;
        binding.requesterName = ctx.requesterName().toUri();
        binding.requestId = transactionId;
        binding.serviceName = config.executionLeaseTargetService;
        binding.planDigest = planDigest;
        binding.resourceBindingSchema = "ndnsf-di-binding-v1";
        binding.resourceBindingProof = ndn::Buffer(
          reinterpret_cast<const uint8_t*>(bindingProofText.data()),
          bindingProofText.size());
        const auto now = static_cast<uint64_t>(std::max<long long>(0, epochMs()));
        const auto activated = config.executionLeaseTable->validateAndActivate(
          leaseId,
          providerEpoch,
          binding,
          "activate:" + transactionId,
          now,
          now + std::max<uint64_t>(1, config.executionLeaseHardDeadlineMs));
        if (!activated.status) {
          ctx.fail(activated.reasonCode);
          return;
        }
        activatedLeaseId = leaseId;
        activatedProviderEpoch = providerEpoch;
        activatedRequester = binding.requesterName;
        activatedRole = role;
        if (!providerRoleCountText.empty()) {
          try {
            expectedProviderRoles = std::max<std::size_t>(
              1, static_cast<std::size_t>(std::stoull(providerRoleCountText)));
          }
          catch (const std::exception&) {
            completeExecutionLease();
            ctx.fail("LEASE_BINDING_MISMATCH");
            return;
          }
        }
      }
      const auto bindingError =
        validateNativeProviderAssignmentPayload(state->runnerSpecs,
                                                role,
                                                ctx.assignment().assignmentPayload);
      if (bindingError) {
        if (nativeTraceEnabled()) {
          std::cout << "\nNDNSF_DI_RESOURCE_BINDING_REJECTED"
                    << " session=" << ctx.sessionId()
                    << " role=" << role
                    << " reason=" << *bindingError
                    << std::endl;
        }
        completeExecutionLease();
        ctx.fail(*bindingError);
        return;
      }
      const auto roleSpec = roleSpecFor(state->plan,
                                        role,
                                        ctx.sessionId(),
                                        assignment,
                                        ctx.localProvider().toUri());
      if (const auto* spec = runnerSpecForRole(state->runnerSpecs, role)) {
        logFragmentInventoryEvent("EXECUTION_OBSERVED",
                                  *spec,
                                  ctx.localProvider().toUri());
      }
      const bool localFullPlan =
        roleSpec.outputs.empty() &&
        allPlanRolesAssignedToLocal(state->plan,
                                    assignment,
                                    ctx.localProvider().toUri());
      completedLocalPlan = localFullPlan;
      auto initialInputs = (localFullPlan || roleSpec.inputs.empty())
        ? initialInputsFromRequest(ctx, request)
        : std::map<std::string, TensorBundle>{};
      const auto submittedSteady = std::chrono::steady_clock::now();
      const auto submittedEpoch = epochMs();
      logProviderCapacity(ctx.sessionId(),
                          role,
                          "before_submit",
                          state->runtime.snapshot());
      std::optional<std::vector<uint8_t>> finalPayload;
      if (localFullPlan) {
        finalPayload = executeLocalPlanAndFinalPayload(*state,
                                                       config,
                                                       ctx.sessionId(),
                                                       assignment,
                                                       ctx.localProvider().toUri(),
                                                       initialInputs,
                                                       submittedSteady,
                                                       submittedEpoch);
      }
      else {
        auto result = state->runtime.executeRoleAsync(
          ctx.sessionId(),
          roleSpec,
          std::move(io),
          std::move(initialInputs)).get();
        logProviderTiming(ctx.sessionId(), role, result, submittedSteady, submittedEpoch);

        finalPayload = nativeProviderFinalResponsePayload(
          roleSpec,
          result,
          config.finalResponseScope);
      }
      logProviderCapacity(ctx.sessionId(),
                          role,
                          "after_complete",
                          state->runtime.snapshot());
      completeExecutionLease();
      if (nativeTraceEnabled()) {
        std::cout << "\nNDNSF_DI_NATIVE_FINAL_RESPONSE_DECISION"
                  << " session=" << ctx.sessionId()
                  << " role=" << role
                  << " role_outputs=" << roleSpec.outputs.size()
                  << " local_full_plan="
                  << (allPlanRolesAssignedToLocal(state->plan,
                                                  assignment,
                                                  ctx.localProvider().toUri()) ?
                      "true" : "false")
                  << " final_scope=" << config.finalResponseScope
                  << " has_payload=" << (finalPayload ? "true" : "false");
        std::cout << std::endl;
      }
      if (finalPayload) {
        ctx.publishFinalResponse(ndn::Buffer(finalPayload->data(), finalPayload->size()));
      }
    }
	    catch (const std::exception& exc) {
	      completeExecutionLease();
	      ctx.fail(exc.what());
	    }
	  };
  return runtime;
}

ndn_service_framework::ServiceProvider::CollaborationHandler
makeNativeProviderCollaborationHandler(NativeProviderHandlerConfig config)
{
  return makeNativeProviderCollaborationRuntime(std::move(config)).handler;
}

} // namespace ndnsf::di
