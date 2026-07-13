#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

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

NativeProviderExecutionBindingResult
validateNativeProviderExecutionBinding(
  const std::map<std::string, std::string>& fields,
  const std::string& expectedProviderBootId,
  const std::string& expectedPlanDigest,
  ExecutionAttemptAuthority& authority)
{
  NativeProviderExecutionBindingResult result;
  result.attempt.requestId = nativeProviderFieldValue(
    fields, {"executionRequestId"});
  const auto epochText = nativeProviderFieldValue(
    fields, {"executionAttemptEpoch"});
  const auto providerBootId = nativeProviderFieldValue(
    fields, {"executionProviderBootId"});
  const auto planDigest = nativeProviderFieldValue(
    fields, {"executionPlanDigest", "executionLeasePlanDigest"});
  if (result.attempt.requestId.empty() || epochText.empty()) {
    result.reason = "DI_ATTEMPT_BINDING_MISSING";
    return result;
  }
  try {
    std::size_t consumed = 0;
    result.attempt.attemptEpoch = std::stoull(epochText, &consumed);
    if (consumed != epochText.size()) {
      throw std::invalid_argument("trailing epoch text");
    }
    result.attempt.validate();
  }
  catch (const std::exception&) {
    result.reason = "DI_ATTEMPT_EPOCH_INVALID";
    return result;
  }
  if (expectedProviderBootId.empty() || providerBootId != expectedProviderBootId) {
    result.reason = "DI_PROVIDER_BOOT_MISMATCH";
    return result;
  }
  if (expectedPlanDigest.empty() || planDigest != expectedPlanDigest) {
    result.reason = "DI_PLAN_BINDING_MISMATCH";
    return result;
  }
  const auto admission = authority.admit(result.attempt);
  if (admission != ExecutionAttemptAdmission::Accepted) {
    result.reason = std::string("DI_ATTEMPT_") + toString(admission);
    return result;
  }
  result.status = true;
  result.reason = "OK";
  return result;
}

NativeProviderExecutionControlResult
applyNativeProviderExecutionControl(
  const std::map<std::string, std::string>& fields,
  ExecutionAttemptAuthority& authority)
{
  NativeProviderExecutionControlResult result;
  if (nativeProviderFieldValue(fields, {"schema"}) !=
      "ndnsf-di-execution-control-v1") {
    return result;
  }
  result.recognized = true;
  const auto operation = nativeProviderFieldValue(fields, {"operation"});
  result.attempt.requestId = nativeProviderFieldValue(fields, {"requestId"});
  try {
    result.attempt.attemptEpoch = std::stoull(
      nativeProviderFieldValue(fields, {"attemptEpoch"}));
    result.attempt.validate();
    if (operation == "CANCEL") {
      result.status = authority.cancel(result.attempt);
      result.reason = result.status ? "CANCELLED" : "CANCEL_REJECTED";
      return result;
    }
    if (operation == "SUPERSEDE") {
      const auto nextEpoch = std::stoull(nativeProviderFieldValue(
        fields, {"supersededByAttemptEpoch"}));
      authority.cancel(result.attempt);
      ExecutionAttemptKey replacement{result.attempt.requestId, nextEpoch};
      replacement.validate();
      const auto admitted = authority.admit(replacement);
      result.status = admitted == ExecutionAttemptAdmission::Accepted;
      result.reason = result.status ? "SUPERSEDED" :
        std::string("SUPERSEDE_") + toString(admitted);
      return result;
    }
    result.reason = "CONTROL_OPERATION_UNSUPPORTED";
  }
  catch (const std::exception&) {
    result.reason = "CONTROL_BINDING_INVALID";
  }
  return result;
}

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
nativeTraceEnabled()
{
  return runtimeTimingEnabled() || std::getenv("NDNSF_COLLAB_ASSIGNMENT_FETCH_TRACE") != nullptr;
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
      auto runner = runnerFactory->create(spec);
      if (auto evidence = runner->executionEvidenceSnapshot()) {
        executionEvidence.push_back(std::move(*evidence));
      }
      runtime.registerRunner(spec.role, std::move(runner));
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
  std::vector<ExecutionEvidence> executionEvidence;
  std::mutex executionLeaseMutex;
  std::map<std::string, std::set<std::string>> completedRolesByLease;
  ExecutionAttemptAuthority attemptAuthority;
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

std::uint64_t
parseRequiredUint64(const std::map<std::string, std::string>& fields,
                    const char* name)
{
  const auto value = nativeProviderFieldValue(fields, {name});
  if (value.empty()) {
    throw std::invalid_argument(std::string("missing KV binding field: ") + name);
  }
  std::size_t consumed = 0;
  const auto parsed = std::stoull(value, &consumed);
  if (consumed != value.size()) {
    throw std::invalid_argument(std::string("invalid KV binding field: ") + name);
  }
  return parsed;
}

KvStateBinding
kvBindingFromAssignment(const NativeModelRunnerSpec& spec,
                        const std::map<std::string, std::string>& fields,
                        const std::string& sessionId,
                        const std::string& role,
                        const std::string& providerName,
                        std::uint64_t expectedSecurityEpoch)
{
  KvStateBinding binding;
  binding.sessionId = nativeProviderFieldValue(fields, {"kvSessionId"});
  if (binding.sessionId.empty()) {
    binding.sessionId = sessionId;
  }
  binding.stage = role;
  binding.contextEpoch = parseRequiredUint64(fields, "kvContextEpoch");
  binding.providerName = providerName;
  binding.securityEpoch = parseRequiredUint64(fields, "kvSecurityEpoch");

  const auto expectedModel = nativeProviderFieldValue(
    spec.metadata, {"evidence.modelDigest"});
  const auto expectedPlan = nativeProviderFieldValue(
    spec.metadata, {"evidence.planDigest"});
  const auto expectedBoot = nativeProviderFieldValue(
    spec.metadata, {"evidence.providerBootId"});
  const auto requestedModel = nativeProviderFieldValue(fields, {"kvModelDigest"});
  const auto requestedPlan = nativeProviderFieldValue(fields, {"kvPlanDigest"});
  const auto requestedBoot = nativeProviderFieldValue(fields, {"kvProviderBootId"});
  if ((!requestedModel.empty() && requestedModel != expectedModel) ||
      (!requestedPlan.empty() && requestedPlan != expectedPlan) ||
      (!requestedBoot.empty() && requestedBoot != expectedBoot) ||
      binding.securityEpoch != expectedSecurityEpoch) {
    throw std::invalid_argument("KV_BINDING_MISMATCH");
  }
  binding.modelDigest = expectedModel;
  binding.planDigest = expectedPlan;
  binding.providerBootId = expectedBoot;
  binding.validate();
  return binding;
}

void
injectCachedKvInputs(std::map<std::string, TensorBundle>& inputs,
                     const NativeModelRunnerSpec& spec,
                     const TensorBundle& cached)
{
  const auto mapping = nativeProviderFieldValue(spec.metadata, {"kvTensorMap"});
  if (mapping.empty() || !isEncodedTensorBundle(cached.payload)) {
    throw std::invalid_argument("KV_STATE_UNAVAILABLE");
  }
  const auto tensors = decodeTensorBundle(cached.payload);
  std::size_t start = 0;
  while (start < mapping.size()) {
    const auto end = mapping.find(',', start);
    const auto item = mapping.substr(
      start, (end == std::string::npos ? mapping.size() : end) - start);
    const auto equals = item.find('=');
    if (equals == std::string::npos || equals == 0 || equals + 1 == item.size()) {
      throw std::invalid_argument("KV_BINDING_MISMATCH");
    }
    const auto inputName = item.substr(0, equals);
    auto tensor = findTensor(tensors, item.substr(equals + 1));
    tensor.name = inputName;
    inputs[inputName] = makeEncodedTensorBundle(inputName, {std::move(tensor)});
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
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
    if (result.executionEvidence && config.executionEvidenceObserver &&
        *config.executionEvidenceObserver) {
      (*config.executionEvidenceObserver)(*result.executionEvidence);
    }
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
  runtime.executionEvidence = state->executionEvidence;
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
      const auto controlFields = parseNativeProviderAssignmentFields(request.getPayload());
      const auto control = applyNativeProviderExecutionControl(
        controlFields, state->attemptAuthority);
      if (control.recognized) {
        std::cout << "\nNDNSF_DI_EXECUTION_ATTEMPT"
                  << " decision=" << (control.status ? "control-applied" : "control-rejected")
                  << " reason=" << control.reason
                  << " requestId=" << control.attempt.requestId
                  << " attemptEpoch=" << control.attempt.attemptEpoch
                  << std::endl;
        const auto response = std::string("schema=ndnsf-di-execution-control-v1;") +
          "status=" + (control.status ? "1;" : "0;") +
          "reason=" + control.reason + ";";
        ctx.publishFinalResponse(ndn::Buffer(
          reinterpret_cast<const std::uint8_t*>(response.data()), response.size()));
        return;
      }
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
        config.freshnessMs);

      const auto role = ctx.role();
      const auto assignmentFields = parseNativeProviderAssignmentFields(
        ctx.assignment().assignmentPayload);
      std::optional<ExecutionAttemptKey> executionAttempt;
      if (config.requireExecutionAttemptBinding) {
        auto binding = validateNativeProviderExecutionBinding(
          assignmentFields,
          config.providerBootId,
          config.planDigest,
          state->attemptAuthority);
        if (!binding.status) {
          std::cout << "\nNDNSF_DI_EXECUTION_ATTEMPT"
                    << " decision=reject"
                    << " reason=" << binding.reason
                    << " role=" << role << std::endl;
          ctx.fail(binding.reason);
          return;
        }
        executionAttempt = std::move(binding.attempt);
      }
      if (config.executionLeaseTable != nullptr) {
        const auto& fields = assignmentFields;
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
      const auto executionSessionId = executionAttempt
        ? executionAttempt->scopedSessionId()
        : ctx.sessionId();
      const auto roleSpec = executionAttempt
        ? roleSpecFor(state->plan,
                      role,
                      *executionAttempt,
                      assignment,
                      ctx.localProvider().toUri())
        : roleSpecFor(state->plan,
                      role,
                      executionSessionId,
                      assignment,
                      ctx.localProvider().toUri());
      if (const auto* spec = runnerSpecForRole(state->runnerSpecs, role)) {
        logFragmentInventoryEvent("EXECUTION_OBSERVED",
                                  *spec,
                                  ctx.localProvider().toUri());
      }
      std::optional<KvStateBinding> kvBinding;
      std::optional<TensorBundle> cachedKvState;
      const auto kvMode = nativeProviderFieldValue(assignmentFields, {"kvMode"});
      if (!kvMode.empty()) {
        const auto* runnerSpec = runnerSpecForRole(state->runnerSpecs, role);
        if (runnerSpec == nullptr || !config.kvStateStore) {
          completeExecutionLease();
          ctx.fail("KV_STATE_UNAVAILABLE");
          return;
        }
        try {
          kvBinding = kvBindingFromAssignment(
            *runnerSpec,
            assignmentFields,
            ctx.sessionId(),
            role,
            ctx.localProvider().toUri(),
            config.kvSecurityEpoch);
        }
        catch (const std::exception&) {
          completeExecutionLease();
          ctx.fail("KV_BINDING_MISMATCH");
          return;
        }
        if (kvMode == "cache-hit" || kvMode == "delta-only") {
          cachedKvState = config.kvStateStore->lookup(*kvBinding);
          std::cout << "\nNDNSF_DI_KV_STATE event=lookup"
                    << " session=" << kvBinding->sessionId
                    << " role=" << role
                    << " context_epoch=" << kvBinding->contextEpoch
                    << " mode=" << kvMode
                    << " status=" << (cachedKvState ? "hit" : "miss")
                    << std::endl;
          if (!cachedKvState) {
            completeExecutionLease();
            ctx.fail(kvMode == "delta-only" ?
                       "CACHE_MISS_FULL_CONTEXT_REQUIRED" : "KV_STATE_UNAVAILABLE");
            return;
          }
        }
        else if (kvMode != "full-context") {
          completeExecutionLease();
          ctx.fail("KV_BINDING_MISMATCH");
          return;
        }
      }
      const bool localFullPlan =
        roleSpec.outputs.empty() &&
        allPlanRolesAssignedToLocal(state->plan,
                                    assignment,
                                    ctx.localProvider().toUri());
      completedLocalPlan = localFullPlan;
      auto initialInputs = initialInputsFromRequest(ctx, request);
      if (cachedKvState) {
        const auto* runnerSpec = runnerSpecForRole(state->runnerSpecs, role);
        try {
          injectCachedKvInputs(initialInputs, *runnerSpec, *cachedKvState);
        }
        catch (const std::exception&) {
          completeExecutionLease();
          ctx.fail("KV_STATE_UNAVAILABLE");
          return;
        }
      }
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
                                                       executionSessionId,
                                                       assignment,
                                                       ctx.localProvider().toUri(),
                                                       initialInputs,
                                                       submittedSteady,
                                                       submittedEpoch);
        if (config.stageServiceTimeObserver && *config.stageServiceTimeObserver) {
          const auto elapsed = std::max(
            std::chrono::milliseconds(1),
            std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - submittedSteady));
          (*config.stageServiceTimeObserver)(elapsed);
        }
      }
      else {
        auto result = state->runtime.executeRoleAsync(
          executionSessionId,
          roleSpec,
          std::move(io),
          std::move(initialInputs)).get();
        if (result.executionEvidence && config.executionEvidenceObserver &&
            *config.executionEvidenceObserver) {
          (*config.executionEvidenceObserver)(*result.executionEvidence);
        }
        if (kvBinding && config.kvStateStore) {
          auto kvOutput = result.outputsByScope.find(config.kvOutputScope);
          if (kvOutput == result.outputsByScope.end()) {
            kvOutput = std::find_if(
              result.outputsByScope.begin(), result.outputsByScope.end(), [] (const auto& item) {
                return isEncodedTensorBundle(item.second.payload);
              });
          }
          auto storedBinding = *kvBinding;
          const auto nextEpoch = nativeProviderFieldValue(
            assignmentFields, {"kvNextContextEpoch"});
          if (!nextEpoch.empty()) {
            try {
              std::size_t consumed = 0;
              storedBinding.contextEpoch = std::stoull(nextEpoch, &consumed);
              if (consumed != nextEpoch.size() ||
                  storedBinding.contextEpoch <= kvBinding->contextEpoch) {
                throw std::invalid_argument("invalid next epoch");
              }
            }
            catch (const std::exception&) {
              completeExecutionLease();
              ctx.fail("KV_BINDING_MISMATCH");
              return;
            }
          }
          if (kvOutput != result.outputsByScope.end() &&
              !config.kvStateStore->put(std::move(storedBinding), kvOutput->second)) {
            completeExecutionLease();
            ctx.fail("KV_STATE_CAPACITY_EXCEEDED");
            return;
          }
          if (kvOutput != result.outputsByScope.end()) {
            std::cout << "\nNDNSF_DI_KV_STATE event=store"
                      << " session=" << kvBinding->sessionId
                      << " role=" << role
                      << " context_epoch="
                      << (nextEpoch.empty() ? kvBinding->contextEpoch : std::stoull(nextEpoch))
                      << " bytes=" << kvOutput->second.payload.size()
                      << std::endl;
          }
        }
        logProviderTiming(ctx.sessionId(), role, result, submittedSteady, submittedEpoch);
        if (config.stageServiceTimeObserver && *config.stageServiceTimeObserver) {
          const auto elapsed = std::max(
            std::chrono::milliseconds(1),
            std::chrono::duration_cast<std::chrono::milliseconds>(
              result.timing.finishedAt - result.timing.startedAt));
          (*config.stageServiceTimeObserver)(elapsed);
        }

        finalPayload = nativeProviderFinalResponsePayload(
          roleSpec,
          result,
          config.finalResponseScope);
      }
      logProviderCapacity(ctx.sessionId(),
                          role,
                          "after_complete",
                          state->runtime.snapshot());
      if (executionAttempt && !state->attemptAuthority.complete(*executionAttempt)) {
        std::cout << "\nNDNSF_DI_EXECUTION_ATTEMPT"
                  << " decision=reject"
                  << " reason=DI_ATTEMPT_DUPLICATE_TERMINAL"
                  << " requestId=" << executionAttempt->requestId
                  << " attemptEpoch=" << executionAttempt->attemptEpoch
                  << std::endl;
        completeExecutionLease();
        ctx.fail("DI_ATTEMPT_DUPLICATE_TERMINAL");
        return;
      }
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
