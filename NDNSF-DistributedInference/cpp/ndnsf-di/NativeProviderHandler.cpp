#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/DistributedExecutionConsistency.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include "ndn-service-framework/utils.hpp"

#include <algorithm>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace ndnsf::di {

std::map<std::string, std::string>
parseNativeProviderAssignmentFields(const ndn::Buffer& payload,
                                    const std::string& selectedRole)
{
  std::map<std::string, std::string> fields;
  const std::string text(reinterpret_cast<const char*>(payload.data()),
                         payload.size());
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first != std::string::npos && text[first] == '{') {
    boost::property_tree::ptree root;
    try {
      std::istringstream input(text);
      boost::property_tree::read_json(input, root);
    }
    catch (const boost::property_tree::json_parser::json_parser_error& exc) {
      throw std::invalid_argument(
        std::string("malformed V3 Selection projection: ") + exc.what());
    }
    if (root.get<std::string>("schema", "") != "ndnsf-di-selection-v3" ||
        root.get<int>("schema_version", 0) != 3) {
      throw std::invalid_argument("V3 Selection projection schema mismatch");
    }
    std::istringstream projectionInput(text);
    const auto projection = nativeSelectionProjectionV3FromJson(
      projectionInput, selectedRole);
    const auto assign = [&] (const char* outputName, const char* inputName) {
      const auto value = root.get_optional<std::string>(inputName);
      if (value && !value->empty()) {
        fields[outputName] = *value;
      }
    };
    assign("provider", "provider");
    assign("executionRequestId", "request_id");
    assign("planCoreDigest", "plan_core_digest");
    assign("executionPlanDigest", "plan_digest");
    assign("securityPolicySnapshotDigest",
           "security_policy_snapshot_digest");
    assign("groupCapabilityV1", "group_capability_v1");
    const auto attempt = root.get_optional<std::uint64_t>("attempt");
    if (attempt) {
      fields["executionAttemptEpoch"] = std::to_string(*attempt);
    }
    const auto deadline = root.get_optional<std::uint64_t>("deadline_ms");
    if (deadline) {
      fields["deadlineMs"] = std::to_string(*deadline);
    }

    const auto& selected = projection.selectedRole;
    fields["role"] = selected.selectedRole;
    fields["backend"] = selected.backend;
    fields["artifactDigest"] = selected.artifactDigest;
    fields["fragmentDigest"] = selected.artifactDigest;
    fields["recipeDigest"] = selected.recipeDigest;
    fields["roleKind"] = selected.roleKind;
    fields["adapterId"] = selected.adapterId;
    fields["adapterVersion"] = selected.adapterVersion;
    fields["modelManifestDigest"] = selected.modelManifestDigest;
    fields["artifactProfileDigest"] = selected.artifactProfileDigest;
    fields["protectionEpoch"] = selected.protectionEpoch;
    if (projection.hasGrantBinding) {
      fields["grantName"] = projection.grantName;
      fields["grantDigest"] = projection.grantDigest;
    }
    fields["graphDigest"] = selected.graphDigest;
    fields["canonicalInitializerDigest"] = selected.canonicalInitializerDigest;
    fields["adapterDescriptorDigest"] = selected.adapterDescriptorDigest;
    fields["assemblerDescriptorDigest"] = selected.assemblerDescriptorDigest;
    fields["backendAbi"] = selected.backendAbi;
    fields["precision"] = selected.precision;
    fields["quantization"] = selected.quantization;
    fields["layout"] = selected.layout;
    fields["padding"] = selected.padding;
    fields["maxSourceBytes"] = std::to_string(selected.maxSourceBytes);
    fields["maxAssembledBytes"] = std::to_string(selected.maxAssembledBytes);
    fields["maxNodes"] = std::to_string(selected.maxNodes);
    fields["rank"] = std::to_string(selected.rank);
    if (selected.deviceSet.size() == 1) {
      fields["device"] = selected.deviceSet.front();
    }
    else if (projection.deviceBinding.mode == "CPU") {
      fields["device"] = "cpu:0";
    }

    // A DATA_V1 Selection may also carry execution-lease and activation
    // bindings. These are scoped to the projection's single local role. Do
    // not fall back to the legacy semicolon assignment for this path.
    if (const auto executionBindings =
          root.get_child_optional("execution_bindings")) {
        const auto& bindingRole = selected.selectedRole;
        const boost::property_tree::ptree* binding = nullptr;
        for (const auto& item : *executionBindings) {
          if (item.first == bindingRole) {
            if (binding != nullptr) {
              throw std::invalid_argument(
                "V3 Selection projection contains duplicate execution binding");
            }
            binding = &item.second;
          }
        }
        if (binding == nullptr) {
          throw std::invalid_argument(
            "V3 Selection projection is missing current execution binding");
        }
        const auto assignBinding = [&] (const char* outputName,
                                        const char* inputName) {
          const auto value = binding->get_optional<std::string>(inputName);
          if (value && !value->empty()) {
            fields[outputName] = *value;
          }
        };
        assignBinding("executionProviderBootId", "provider_boot_id");
        assignBinding("executionLeaseId", "lease_id");
        assignBinding("executionLeaseEpoch", "lease_epoch");
        assignBinding("executionLeasePlanDigest", "lease_plan_digest");
        assignBinding("executionLeaseBindingProof", "lease_binding_proof");
        assignBinding("executionLeaseProviderRoleCount",
                      "lease_provider_role_count");
        assignBinding("executionActivationDigest", "activation_digest");
        assignBinding("executionActivationMembers", "activation_members");
        assignBinding("executionActivationLocalMember",
                      "activation_local_member");
        assignBinding("executionFencingToken", "fencing_token");
    }
    return fields;
  }

  std::size_t pos = 0;
  while (pos < text.size()) {
    const auto eq = text.find('=', pos);
    if (eq == std::string::npos) {
      break;
    }
    const auto end = text.find(';', eq + 1);
    fields[text.substr(pos, eq - pos)] =
      text.substr(eq + 1, (end == std::string::npos ? text.size() : end) - eq - 1);
    if (end == std::string::npos) {
      break;
    }
    pos = end + 1;
  }
  return fields;
}

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
  const auto schema = nativeProviderFieldValue(fields, {"schema"});
  if (schema != "ndnsf-di-execution-control-v2" &&
      schema != "ndnsf-di-execution-control-v1") {
    return result;
  }
  result.recognized = true;
  if (schema == "ndnsf-di-execution-control-v1") {
    std::clog << "NDNSF_DI_LEGACY_IMPORT kind=execution-control-v1 count=1"
              << std::endl;
  }
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

// Runtime timing records are parsed as line-oriented evidence.  Several role
// workers can finish concurrently, so serialise the complete diagnostic block
// rather than allowing individual operator<< calls from different records to
// interleave and produce fields belonging to different edges.
std::mutex&
runtimeTimingOutputMutex()
{
  static std::mutex mutex;
  return mutex;
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
    , runtime(config.workerCount, config.workerQueueCapacity)
    , executionLeaseTable(config.executionLeaseTable)
    , executionLeaseCleanupIntervalMs(config.executionLeaseCleanupIntervalMs)
  {
    if (!runnerFactory) {
      throw std::invalid_argument(
        "NativeProviderHandlerState requires NativeModelRunnerFactory");
    }
    if (!config.runnerPreparationFactory) {
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
    if (executionLeaseTable != nullptr && executionLeaseCleanupIntervalMs > 0) {
      executionLeaseCleanupThread = std::thread([this] {
        std::unique_lock<std::mutex> lock(executionLeaseCleanupMutex);
        const auto interval = std::chrono::milliseconds(
          executionLeaseCleanupIntervalMs);
        while (!executionLeaseCleanupStop) {
          if (executionLeaseCleanupCondition.wait_for(
                lock, interval, [this] { return executionLeaseCleanupStop; })) {
            break;
          }
          lock.unlock();
          const auto now = static_cast<uint64_t>(
            std::max<long long>(0, epochMs()));
          executionLeaseTable->cleanupExpired(now);
          lock.lock();
        }
      });
    }
  }

  ~NativeProviderHandlerState()
  {
    {
      std::lock_guard<std::mutex> lock(executionLeaseCleanupMutex);
      executionLeaseCleanupStop = true;
    }
    executionLeaseCleanupCondition.notify_all();
    if (executionLeaseCleanupThread.joinable()) {
      executionLeaseCleanupThread.join();
    }
    if (executionLeaseTable != nullptr) {
      const auto now = static_cast<uint64_t>(
        std::max<long long>(0, epochMs()));
      executionLeaseTable->cleanupExpired(now);
    }
    if (!runnerSpecs.empty()) {
      for (const auto& spec : runnerSpecs) {
        logFragmentInventoryEvent("EVICTED", spec, localProviderName);
      }
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
  ndn_service_framework::ProviderExecutionLeaseTable* executionLeaseTable = nullptr;
  uint64_t executionLeaseCleanupIntervalMs = 0;
  std::mutex executionLeaseCleanupMutex;
  std::condition_variable executionLeaseCleanupCondition;
  bool executionLeaseCleanupStop = false;
  std::thread executionLeaseCleanupThread;
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
  std::lock_guard<std::mutex> outputLock(runtimeTimingOutputMutex());

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
  std::lock_guard<std::mutex> outputLock(runtimeTimingOutputMutex());
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
    // A missing mapping is not evidence that this provider owns the role.
    // Treating it as local (via providerForRole's fallback) can make a
    // distributed request enter the local full-plan path and ask a provider
    // that only hosts /Merge to execute /Backbone as well.
    const auto found = assignment.providerByRole.find(role);
    if (found == assignment.providerByRole.end() ||
        found->second != localProvider) {
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
                                const NativeExecutionPlan& plan,
                                const std::string& sessionId,
                                const NativeProviderAssignment& assignment,
                                const std::string& localProvider,
                                const std::map<std::string, TensorBundle>& initialInputs,
                                std::chrono::steady_clock::time_point submittedSteady,
                                long long submittedEpoch,
                                ProviderRoleWorker::NativeRunnerPreparation
                                  prepareRunner = {})
{
  auto io = std::make_shared<LocalDependencyIo>();
  std::vector<std::pair<std::string, std::future<ProviderRoleResult>>> futures;
  futures.reserve(plan.roles.size());
  for (const auto& role : plan.roles) {
    auto roleSpec = roleSpecFor(plan,
                                role,
                                sessionId,
                                assignment,
                                localProvider);
    auto roleInputs = roleSpec.inputs.empty()
      ? initialInputs : std::map<std::string, TensorBundle>{};
    if (prepareRunner) {
      if (plan.roles.size() != 1) {
        throw std::runtime_error(
          "post-Selection runner preparation requires one local role");
      }
      futures.emplace_back(
        role,
        state.runtime.executePreparedRoleAsync(
          sessionId, roleSpec, io, prepareRunner, std::move(roleInputs)));
    }
    else {
      futures.emplace_back(
        role,
        state.runtime.executeRoleAsync(
          sessionId, roleSpec, io, std::move(roleInputs)));
    }
  }

  std::optional<std::vector<uint8_t>> finalPayload;
  for (auto& item : futures) {
    auto roleSpec = roleSpecFor(plan,
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

bool
nativeProviderShouldExecuteLocalPlan(const NativeExecutionPlan& plan,
                                     const NativeProviderAssignment& assignment,
                                     const RoleSpec& currentRole,
                                     const std::string& localProvider)
{
  // An intermediate role (for example /Backbone) normally has outputs.  Its
  // output edges describe the dataflow graph; they must not force the handler
  // back into the one-role network path when every role is assigned locally.
  if (currentRole.role.empty()) {
    return false;
  }
  return allPlanRolesAssignedToLocal(plan, assignment, localProvider);
}

void
validateNativeProviderExecutionPolicy(
  const NativeProviderHandlerConfig& config)
{
  if (config.plan.executionPolicy != config.executionPolicy) {
    throw std::invalid_argument(
      "native Provider execution policy is not plan-bound");
  }
  if (config.executionPolicy == "DATA_DRIVEN_V2") {
    if (config.requireExecutionActivation ||
        config.allowLegacyPeerReadinessBarrier) {
      throw std::invalid_argument(
        "DATA_DRIVEN_V2 rejects legacy execution activation/barrier flags");
    }
    return;
  }
  if (config.executionPolicy == "LEGACY_READY_SET_V1") {
    if (!config.requireExecutionActivation ||
        !config.allowLegacyPeerReadinessBarrier) {
      throw std::invalid_argument(
        "LEGACY_READY_SET_V1 requires explicit activation and readiness barrier");
    }
    return;
  }
  throw std::invalid_argument("unsupported NDNSF-DI execution policy");
}

std::optional<std::string>
validateNativeProviderRuntimeReadiness(
  const ExecutionEvidence& evidence,
  const std::string& expectedRole,
  const std::string& expectedBackend,
  const std::string& expectedDevice,
  const std::string& expectedArtifactDigest)
{
  if (expectedRole.empty() || expectedBackend.empty() ||
      expectedDevice.empty() || expectedArtifactDigest.empty()) {
    return "DI_RUNTIME_ASSIGNMENT_INCOMPLETE";
  }
  const bool deviceRequestsCuda = expectedDevice.rfind("cuda:", 0) == 0;
  const bool backendRequestsCuda = expectedBackend == "onnxruntime-cuda";
  const bool backendRequestsCpu = expectedBackend == "cpu" ||
                                  expectedBackend == "onnxruntime-cpu";
  const bool backendIsGenericOnnx = expectedBackend == "onnxruntime";
  if (!backendRequestsCuda && !backendRequestsCpu && !backendIsGenericOnnx) {
    return "DI_RUNTIME_BACKEND_MISMATCH";
  }
  const bool expectsCuda = backendRequestsCuda ||
                           (backendIsGenericOnnx && deviceRequestsCuda);
  const bool expectsCpu = backendRequestsCpu ||
                          (backendIsGenericOnnx && !deviceRequestsCuda);
  try {
    evidence.validate();
  }
  catch (const std::exception&) {
    return "DI_RUNTIME_EVIDENCE_INVALID";
  }
  if (expectsCuda) {
    if (!evidence.realCompute ||
        evidence.runnerKind != RunnerKind::OnnxRuntimeCuda ||
        evidence.deviceKind != "cuda" || evidence.cpuFallbackUsed) {
      return "DI_RUNTIME_CUDA_REQUIRED";
    }
  }
  else {
    // CPU/no-GPU is a first-class V3 execution mode. It must still be a real
    // ONNX Runtime CPU execution and may not be advertised as a silent
    // CUDA-to-CPU fallback.
    if (evidence.cpuFallbackUsed) {
      return "DI_RUNTIME_CPU_FALLBACK_USED";
    }
    if (!evidence.realCompute ||
        evidence.runnerKind != RunnerKind::OnnxRuntimeCpu ||
        evidence.deviceKind != "cpu") {
      return "DI_RUNTIME_CPU_REQUIRED";
    }
  }
  if (!evidence.loadCompleted) {
    return "DI_RUNTIME_MODEL_NOT_LOADED";
  }
  if (!evidence.warmupCompleted) {
    return "DI_RUNTIME_WARMUP_INCOMPLETE";
  }
  if (std::find(evidence.roles.begin(), evidence.roles.end(), expectedRole) ==
      evidence.roles.end()) {
    return "DI_RUNTIME_ROLE_MISMATCH";
  }
  if (expectsCuda) {
    auto expectedDeviceId = expectedDevice;
    if (expectedDeviceId.rfind("cuda:", 0) == 0) {
      expectedDeviceId = expectedDeviceId.substr(5);
    }
    if (expectedDevice.rfind("cuda:", 0) != 0 ||
        expectedDeviceId.empty() || evidence.deviceId != expectedDeviceId) {
      return "DI_RUNTIME_DEVICE_MISMATCH";
    }
  }
  else if (expectedDevice != "cpu") {
    auto expectedDeviceId = expectedDevice;
    if (expectedDeviceId.rfind("cpu:", 0) == 0) {
      expectedDeviceId = expectedDeviceId.substr(4);
    }
    if (expectedDeviceId.empty() ||
        (evidence.deviceId != expectedDeviceId &&
         evidence.deviceId != "cpu" + expectedDeviceId)) {
      return "DI_RUNTIME_DEVICE_MISMATCH";
    }
  }
  const auto artifact = evidence.artifactDigests.find(expectedRole);
  if (artifact == evidence.artifactDigests.end() ||
      !nativeProviderDigestEquals(artifact->second, expectedArtifactDigest)) {
    return "DI_RUNTIME_ARTIFACT_MISMATCH";
  }
  return std::nullopt;
}

std::optional<std::string>
validateNativePreparedRunnerSpec(
  const NativeSelectionProjectionV3& projection,
  const NativeModelRunnerSpec& spec)
{
  const auto& assembly = projection.assembly;
  if (assembly.modelManifestDigest.empty() ||
      assembly.artifactProfileDigest.empty() || assembly.graphDigest.empty() ||
      assembly.canonicalInitializerDigest.empty() ||
      assembly.adapterDescriptorDigest.empty() ||
      assembly.assemblerDescriptorDigest.empty() || assembly.backendAbi.empty() ||
      assembly.nodeIndices.empty() || assembly.expectedInputs.empty() ||
      assembly.expectedOutputs.empty() || assembly.precision.empty() ||
      assembly.quantization.empty() || assembly.layout.empty() ||
      assembly.padding.empty() || assembly.maxSourceBytes == 0 ||
      assembly.maxAssembledBytes == 0 || assembly.maxNodes == 0) {
    return "DI_PROVIDER_ASSEMBLY_IDENTITY_INCOMPLETE";
  }
  if (spec.role != assembly.selectedRole || spec.backend != assembly.backend ||
      spec.path.empty()) {
    return "DI_PROVIDER_ASSEMBLY_RUNNER_MISMATCH";
  }
  const std::filesystem::path modelPath(spec.path);
  if (!modelPath.is_absolute() || modelPath.filename() != "model.onnx") {
    return "DI_PROVIDER_ASSEMBLY_PATH_UNSAFE";
  }
  for (const auto& part : modelPath) {
    if (part == "..") {
      return "DI_PROVIDER_ASSEMBLY_PATH_UNSAFE";
    }
  }
  const auto matches = [&spec] (
    std::initializer_list<const char*> keys, const std::string& expected) {
    const auto actual = metadataValue(spec, keys);
    return !actual.empty() && actual == expected;
  };
  if (!matches({"fragmentDigest", "fragment_digest"},
               assembly.artifactDigest) ||
      !matches({"recipeDigest", "recipe_digest"}, assembly.recipeDigest) ||
      !matches({"modelManifestDigest", "model_manifest_digest"},
               assembly.modelManifestDigest) ||
      !matches({"artifactProfileDigest", "artifact_profile_digest"},
               assembly.artifactProfileDigest) ||
      !matches({"graphDigest", "graph_digest"}, assembly.graphDigest) ||
      !matches({"canonicalInitializerDigest", "canonical_initializer_digest"},
               assembly.canonicalInitializerDigest) ||
      !matches({"adapterDescriptorDigest", "adapter_descriptor_digest"},
               assembly.adapterDescriptorDigest) ||
      !matches({"assemblerDescriptorDigest", "assembler_descriptor_digest"},
               assembly.assemblerDescriptorDigest) ||
      !matches({"backendAbi", "backend_abi"}, assembly.backendAbi) ||
      !matches({"precision"}, assembly.precision) ||
      !matches({"quantization"}, assembly.quantization) ||
      !matches({"layout"}, assembly.layout) ||
      !matches({"padding"}, assembly.padding) ||
      !matches({"maxSourceBytes", "max_source_bytes"},
               std::to_string(assembly.maxSourceBytes)) ||
      !matches({"maxAssembledBytes", "max_assembled_bytes"},
               std::to_string(assembly.maxAssembledBytes)) ||
      !matches({"maxNodes", "max_nodes"},
               std::to_string(assembly.maxNodes))) {
    return "DI_PROVIDER_ASSEMBLY_METADATA_MISMATCH";
  }
  return std::nullopt;
}

std::optional<std::string>
validateProtectedRuntimeBinding(
  const NativeSelectionProjectionV3& projection,
  const ProtectedRuntime& runtime,
  const std::shared_ptr<ProviderGroupCoordinator>& groupCoordinator,
  const std::string& expectedProviderBootId,
  const std::string& expectedFencingToken)
{
  const auto& binding = runtime.binding();
  std::set<std::string> mayPublish;
  std::set<std::string> mustFetch;
  std::map<std::string, std::string> mayPublishConsumers;
  std::map<std::string, std::string> mustFetchProducers;
  for (const auto& endpoint : projection.dataflow.mayPublish) {
    mayPublish.insert(endpoint.endpointDigest);
    mayPublishConsumers[endpoint.endpointDigest] = endpoint.consumerRole;
  }
  for (const auto& endpoint : projection.dataflow.mustFetch) {
    mustFetch.insert(endpoint.endpointDigest);
    mustFetchProducers[endpoint.endpointDigest] = endpoint.producerRole;
  }
  if (!projection.hasGrantBinding || expectedProviderBootId.empty() ||
      expectedFencingToken.empty() || binding.provider != projection.provider ||
      binding.role != projection.executionRole.roleId ||
      binding.requestId != projection.requestId ||
      binding.attempt != projection.attempt ||
      binding.planCoreDigest != projection.planCoreDigest ||
      binding.planDigest != projection.planDigest ||
      binding.securityPolicySnapshotDigest !=
        projection.securityPolicySnapshotDigest ||
      binding.protectionEpoch != projection.selectedRole.protectionEpoch ||
      binding.grantName != projection.grantName ||
      binding.grantDigest != projection.grantDigest ||
      binding.providerBootId != expectedProviderBootId ||
      binding.fencingToken != expectedFencingToken ||
      binding.mayPublishEndpointDigests != mayPublish ||
      binding.mustFetchEndpointDigests != mustFetch ||
      binding.mayPublishConsumerByEndpoint != mayPublishConsumers ||
      binding.mustFetchProducerByEndpoint != mustFetchProducers) {
    return "DI_PROTECTED_RUNTIME_BINDING_MISMATCH";
  }
  if (groupCoordinator && groupCoordinator->hasCapability()) {
    const auto& capability = groupCoordinator->capability();
    if (binding.capabilityDigest != capability.capabilityDigest ||
        binding.groupId != capability.groupId ||
        binding.groupEpoch != capability.epoch ||
        binding.epochKeyId != capability.epochKeyId ||
        capability.requestId != projection.requestId ||
        capability.attemptId !=
          "attempt-" + std::to_string(projection.attempt) ||
        capability.planDigest != projection.planDigest) {
      return "DI_PROTECTED_CAPABILITY_BINDING_MISMATCH";
    }
  }
  else if (!binding.capabilityDigest.empty() || !binding.groupId.empty() ||
           binding.groupEpoch != 0 || !binding.epochKeyId.empty()) {
    return "DI_PROTECTED_CAPABILITY_BINDING_MISMATCH";
  }
  return std::nullopt;
}

NativeProviderCollaborationRuntime
makeNativeProviderCollaborationRuntime(NativeProviderHandlerConfig config)
{
  validateNativeProviderExecutionPolicy(config);
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
    std::shared_ptr<ProtectedRuntime> protectedRuntime;
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
        const auto response = std::string("schema=ndnsf-di-execution-control-v2;") +
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

      const auto role = ctx.role();
      const auto assignmentFields = parseNativeProviderAssignmentFields(
        ctx.assignment().assignmentPayload, role);
      std::optional<NativeSelectionProjectionV3> selectionProjection;
      const std::string assignmentText(
        reinterpret_cast<const char*>(ctx.assignment().assignmentPayload.data()),
        ctx.assignment().assignmentPayload.size());
      const auto assignmentFirst = assignmentText.find_first_not_of(" \t\r\n");
      if (assignmentFirst != std::string::npos &&
          assignmentText[assignmentFirst] == '{') {
        std::istringstream projectionInput(assignmentText);
        selectionProjection = nativeSelectionProjectionV3FromJson(
          projectionInput, role);
        if (selectionProjection->provider != ctx.localProvider().toUri()) {
          ctx.fail("DI_SELECTION_PROVIDER_MISMATCH");
          return;
        }
        if (selectionProjection->requestId != ctx.sessionId()) {
          ctx.fail("DI_SELECTION_REQUEST_MISMATCH");
          return;
        }
        if (!config.runnerPreparationFactory &&
            !config.allowPreassembledV3Compatibility) {
          ctx.fail("DI_PROVIDER_ASSEMBLY_FACTORY_MISSING");
          return;
        }
        // Model/backend allow-lists remain Provider configuration.  The
        // request-scoped roles and dependency graph come only from the sealed
        // Selection projection, as required by Placement V3.
        selectionProjection->plan.serviceName = state->plan.serviceName;
        selectionProjection->plan.modelName = state->plan.modelName;
        selectionProjection->plan.modelFamily = state->plan.modelFamily;
        selectionProjection->plan.modelFormat = state->plan.modelFormat;
        selectionProjection->plan.plannerKind = state->plan.plannerKind;
      }
      const NativeExecutionPlan& executionPlan = selectionProjection
        ? selectionProjection->plan : state->plan;
      const std::string requestPlanDigest = selectionProjection
        ? selectionProjection->planDigest : config.planDigest;
      auto groupCoordinator = config.groupCoordinator;
      if (config.groupCoordinatorFactory) {
        groupCoordinator = config.groupCoordinatorFactory(ctx, assignmentFields);
      }
      if (selectionProjection &&
          selectionProjection->selectedRole.protectionEpoch != "plaintext-v1") {
        if (!config.protectedRuntimeFactory) {
          ctx.fail("DI_PROTECTED_RUNTIME_FACTORY_MISSING");
          return;
        }
        protectedRuntime = config.protectedRuntimeFactory(
          ctx, *selectionProjection, groupCoordinator);
        if (!protectedRuntime ||
            protectedRuntime->state() == ProtectedRuntimeState::NoGrant ||
            protectedRuntime->state() == ProtectedRuntimeState::FailedClosed) {
          ctx.fail("DI_PROTECTED_GRANT_NOT_VERIFIED");
          return;
        }
        const auto fencingToken = nativeProviderFieldValue(
          assignmentFields,
          {"executionFencingToken", "fencingToken", "admissionFencingToken"});
        if (const auto error = validateProtectedRuntimeBinding(
              *selectionProjection, *protectedRuntime, groupCoordinator,
              config.providerBootId, fencingToken)) {
          protectedRuntime->cancel(*error);
          ctx.fail(*error);
          return;
        }
      }
      auto io = std::make_shared<NdnsfCollaborationDependencyIo>(
        ctx,
        collaborationFetchTimeoutMs(config.fetchTimeoutMs),
        config.maxSegmentSize,
        config.freshnessMs,
        groupCoordinator,
        protectedRuntime);
      const auto assignmentExecutionPolicy = nativeProviderFieldValue(
        assignmentFields, {"executionPolicy"});
      const bool legacyPolicy =
        config.executionPolicy == "LEGACY_READY_SET_V1";
      if ((!assignmentExecutionPolicy.empty() &&
           assignmentExecutionPolicy != config.executionPolicy) ||
          (legacyPolicy && assignmentExecutionPolicy != config.executionPolicy)) {
        ctx.fail("DI_EXECUTION_POLICY_MISMATCH");
        return;
      }
      std::optional<ExecutionAttemptKey> executionAttempt;
      if (config.requireExecutionAttemptBinding) {
        auto binding = validateNativeProviderExecutionBinding(
          assignmentFields,
          config.providerBootId,
          requestPlanDigest,
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
          fields, {"executionLeaseTransactionId", "executionRequestId"});
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
        if (config.requireExecutionActivation) {
          const auto activationDigest = nativeProviderFieldValue(
            fields, {"executionActivationDigest"});
          const auto activationMembers = nativeProviderFieldValue(
            fields, {"executionActivationMembers"});
          const auto activationLocalMember = nativeProviderFieldValue(
            fields, {"executionActivationLocalMember"});
          if (activationDigest.empty() || activationMembers.empty() ||
              activationLocalMember.empty() ||
              activationMembers.find(activationLocalMember) == std::string::npos) {
            ctx.fail("DI_EXECUTION_ACTIVATION_BINDING_MISMATCH");
            return;
          }
        }
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
      const auto bindingError = (selectionProjection &&
                                 config.runnerPreparationFactory)
        ? std::optional<std::string>{}
        : validateNativeProviderAssignmentPayload(
            state->runnerSpecs, role, ctx.assignment().assignmentPayload);
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
      const auto roleSpec = selectionProjection
        ? roleSpecFromSelectionProjectionV3(
            *selectionProjection, ctx.localProvider().toUri())
        : (executionAttempt
            ? roleSpecFor(executionPlan,
                          role,
                          *executionAttempt,
                          assignment,
                          ctx.localProvider().toUri())
            : roleSpecFor(executionPlan,
                          role,
                          executionSessionId,
                          assignment,
                          ctx.localProvider().toUri()));
      const auto* readinessRunnerSpec = runnerSpecForRole(state->runnerSpecs, role);
      const auto deploymentRevision = nativeProviderFieldValue(
        assignmentFields, {"deploymentRevision", "revision", "planRevision"});
      const auto adapterIdentity = readinessRunnerSpec == nullptr
        ? std::string("native")
        : (!readinessRunnerSpec->backend.empty()
             ? readinessRunnerSpec->backend
             : readinessRunnerSpec->kind);
      const auto reportStatus = [&] (const std::string& operationId,
                                     const std::string& operation,
                                     const std::string& stateName,
                                     std::uint64_t sequence,
                                     double progress,
                                     const std::string& phase) {
        ndn_service_framework::ServiceProvider::ServiceOperationStatus status;
        status.operationId = operationId;
        status.operation = operation;
        status.role = role;
        status.attempt = executionAttempt ? executionAttempt->attemptEpoch : 1;
        status.epoch = 1;
        status.sequence = sequence;
        status.state = stateName;
        status.progressKnown = true;
        status.progress = progress;
        status.createdAtMs = static_cast<std::uint64_t>(
          std::max<long long>(0, epochMs()));
        status.updatedAtMs = status.createdAtMs;
        status.expiresAtMs = status.createdAtMs + 120000;
        status.detailsSchema = "ndnsf-di-preparation-progress-v1";
        const auto details = std::string("{\"phase\":\"") + phase +
          "\",\"deploymentRevision\":\"" +
              (deploymentRevision.empty() ? requestPlanDigest : deploymentRevision) +
          "\",\"adapter\":\"" + adapterIdentity + "\"}";
        status.detailsPayload = ndn::Buffer(
          reinterpret_cast<const std::uint8_t*>(details.data()), details.size());
        ctx.reportOperationStatus(std::move(status));
      };
      const auto readinessOperationId =
        ctx.assignment().selectionDigest + ":" + role + ":readiness";
      const auto executionOperationId =
        ctx.assignment().selectionDigest + ":" + role + ":execution";
      ProviderRoleWorker::NativeRunnerPreparation prepareRunner;
      if (config.executionPolicy == "DATA_DRIVEN_V2") {
        const auto expectedBackend = nativeProviderFieldValue(
          assignmentFields, {"backend", "executionBackend"});
        const auto expectedDevice = nativeProviderFieldValue(
          assignmentFields, {"device", "executionDevice"});
        const auto expectedArtifact = nativeProviderFieldValue(
          assignmentFields,
          {"artifactDigest", "fragmentDigest", "modelFragmentDigest"});
        if (selectionProjection && config.runnerPreparationFactory) {
          const auto projection = *selectionProjection;
          const auto preparationFactory = config.runnerPreparationFactory;
          const auto runnerFactory = state->runnerFactory;
          prepareRunner = [projection, preparationFactory, runnerFactory,
                           expectedBackend, expectedDevice, expectedArtifact,
                           role, reportStatus, readinessOperationId] {
            auto spec = preparationFactory(projection);
            if (const auto error = validateNativePreparedRunnerSpec(
                  projection, spec)) {
              throw std::runtime_error(*error);
            }
            auto runner = runnerFactory->create(spec);
            const auto evidence = runner->executionEvidenceSnapshot();
            if (!evidence) {
              throw std::runtime_error("DI_RUNTIME_EVIDENCE_MISSING");
            }
            if (const auto error = validateNativeProviderRuntimeReadiness(
                  *evidence, role, expectedBackend, expectedDevice,
                  expectedArtifact)) {
              throw std::runtime_error(*error);
            }
            reportStatus(readinessOperationId, "ensure-deployment", "DONE",
                         1, 1.0, "READY");
            return runner;
          };
        }
        else {
          const auto evidence = std::find_if(
            state->executionEvidence.begin(), state->executionEvidence.end(),
            [&role] (const ExecutionEvidence& item) {
              return std::find(item.roles.begin(), item.roles.end(), role) !=
                item.roles.end();
            });
          if (evidence == state->executionEvidence.end()) {
            completeExecutionLease();
            ctx.fail("DI_RUNTIME_EVIDENCE_MISSING");
            return;
          }
          const auto readinessError = validateNativeProviderRuntimeReadiness(
            *evidence, role, expectedBackend, expectedDevice, expectedArtifact);
          if (readinessError) {
            completeExecutionLease();
            ctx.fail(*readinessError);
            return;
          }
          // Preassembled compatibility is READY before queue execution because
          // its configured runner was already loaded and warmed at startup.
          reportStatus(readinessOperationId, "ensure-deployment", "DONE", 1,
                       1.0, "READY");
        }
      }
      else {
        reportStatus(readinessOperationId, "ensure-deployment", "DONE", 1,
                     1.0, "READY");
      }

      // Readiness status is observational.  Before entering any runner, every
      // selected role rendezvous over one request-scoped encrypted
      // Collaboration channel and validates exact selection/revision/plan
      // membership.  This is a DI payload above NDNSF Core, not a new base
      // protocol message.
      if (config.allowLegacyPeerReadinessBarrier) {
        constexpr const char* readinessScope = "ndnsf-di-readiness-v1";
        const ndn::Name readinessTopic("/ndnsf-di/readiness");
        auto expected = ctx.assignment().roleProviders;
        expected[role] = ctx.localProvider();
        const auto activationDigest = nativeProviderFieldValue(
          assignmentFields, {"executionActivationDigest"});
        const auto activationMembersText = nativeProviderFieldValue(
          assignmentFields, {"executionActivationMembers"});
        const auto localMember = nativeProviderFieldValue(
          assignmentFields, {"executionActivationLocalMember"});
        const auto declaredBindingDigest = nativeProviderFieldValue(
          assignmentFields, {"readinessBindingDigest"});
        const auto declaredRolesText = nativeProviderFieldValue(
          assignmentFields, {"readinessRoles"});
        const auto declaredRoleCountText = nativeProviderFieldValue(
          assignmentFields, {"readinessRoleCount"});
        std::set<std::string> activationMembers;
        std::size_t memberStart = 0;
        while (memberStart <= activationMembersText.size()) {
          const auto comma = activationMembersText.find(',', memberStart);
          const auto member = activationMembersText.substr(
            memberStart,
            comma == std::string::npos ? std::string::npos : comma - memberStart);
          if (!member.empty()) {
            activationMembers.insert(member);
          }
          if (comma == std::string::npos) {
            break;
          }
          memberStart = comma + 1;
        }
        const bool activationBound = !activationDigest.empty() &&
          !localMember.empty() && !activationMembers.empty();
        if (activationBound && activationMembers.count(localMember) == 0) {
          completeExecutionLease();
          ctx.fail("DI_READINESS_ACTIVATION_MEMBER_MISMATCH");
          return;
        }
        std::set<std::string> declaredRoles;
        memberStart = 0;
        while (memberStart <= declaredRolesText.size()) {
          const auto comma = declaredRolesText.find(',', memberStart);
          const auto declaredRole = declaredRolesText.substr(
            memberStart,
            comma == std::string::npos ? std::string::npos : comma - memberStart);
          if (!declaredRole.empty()) {
            declaredRoles.insert(declaredRole);
          }
          if (comma == std::string::npos) {
            break;
          }
          memberStart = comma + 1;
        }
        std::size_t declaredRoleCount = 0;
        try {
          declaredRoleCount = declaredRoleCountText.empty()
            ? 0 : static_cast<std::size_t>(std::stoull(declaredRoleCountText));
        }
        catch (const std::exception&) {
          declaredRoleCount = 0;
        }
        const bool declaredBound = !activationBound &&
          !declaredBindingDigest.empty() && !declaredRoles.empty() &&
          declaredRoleCount == declaredRoles.size() &&
          declaredRoles.count(role) != 0;
        const auto expectedReadinessCount = activationBound
          ? activationMembers.size()
          : (declaredBound ? declaredRoles.size() : expected.size());
        const auto readinessBindingDigest = activationBound
          ? activationDigest
          : (declaredBound ? declaredBindingDigest
                           : ctx.assignment().selectionDigest);
        const auto effectiveRevision = deploymentRevision.empty()
          ? requestPlanDigest : deploymentRevision;
        const auto effectivePlanDigest = requestPlanDigest.empty()
          ? effectiveRevision : requestPlanDigest;
        const auto attemptEpoch = executionAttempt
          ? executionAttempt->attemptEpoch : 1;
        const auto artifactDigest = readinessRunnerSpec == nullptr
          ? std::string("role:") + role
          : fragmentDigestFor(*readinessRunnerSpec);
        const auto localPayload = std::string("schema=ndnsf-di-readiness-v1;") +
          "revision=" + effectiveRevision + ";" +
          "planDigest=" + effectivePlanDigest + ";" +
          "bindingDigest=" + readinessBindingDigest + ";" +
          "memberId=" + (activationBound ? localMember : role) + ";" +
          "role=" + role + ";" +
          "provider=" + ctx.localProvider().toUri() + ";" +
          "attempt=" + std::to_string(attemptEpoch) + ";" +
          "adapter=" + adapterIdentity + ";" +
          "artifactDigest=" + artifactDigest + ";";
        const auto publishReadiness = [&] {
          ctx.publish(
            readinessScope,
            readinessTopic,
            ndn::Buffer(reinterpret_cast<const std::uint8_t*>(localPayload.data()),
                        localPayload.size()));
        };
        publishReadiness();

        std::map<std::string, std::string> observed;
        observed.emplace(activationBound ? localMember : role, localPayload);
        std::map<std::string, std::string> observedRoleProviders;
        observedRoleProviders.emplace(role, ctx.localProvider().toUri());
        const auto barrierDeadline = std::chrono::steady_clock::now() +
          std::chrono::milliseconds(collaborationFetchTimeoutMs(config.fetchTimeoutMs));
        auto nextReadinessPublish = std::chrono::steady_clock::now() +
          std::chrono::milliseconds(250);
        while (observed.size() < expectedReadinessCount &&
               std::chrono::steady_clock::now() < barrierDeadline) {
          if (std::chrono::steady_clock::now() >= nextReadinessPublish) {
            // Collaboration notifications may race assignment/scope-key
            // installation.  Re-publish the same idempotent snapshot at a
            // bounded rate so late peers recover without an event log.
            publishReadiness();
            nextReadinessPublish = std::chrono::steady_clock::now() +
              std::chrono::milliseconds(250);
          }
          const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            barrierDeadline - std::chrono::steady_clock::now()).count();
          const auto items = ctx.waitFor(
            readinessScope, readinessTopic, 1,
            static_cast<int>(std::max<long long>(1, std::min<long long>(100, remaining))));
          for (const auto& item : items) {
            const std::string payload(item.payload.begin(), item.payload.end());
            const auto fields = parseNativeProviderAssignmentFields(item.payload);
            const auto itemRole = nativeProviderFieldValue(fields, {"role"});
            const auto providerText = nativeProviderFieldValue(fields, {"provider"});
            const auto memberId = nativeProviderFieldValue(fields, {"memberId"});
            const auto expectedProvider = expected.find(itemRole);
            const bool exactMember = activationBound
              ? activationMembers.count(memberId) != 0
              : (declaredBound ? declaredRoles.count(itemRole) != 0
                               : expectedProvider != expected.end());
            const bool exactProvider = activationBound
              ? !providerText.empty() && item.producer.toUri() == providerText
              : (expectedProvider != expected.end()
                   ? item.producer.equals(expectedProvider->second) &&
                     providerText == expectedProvider->second.toUri()
                   : declaredBound && !providerText.empty() &&
                     item.producer.toUri() == providerText);
            if (nativeProviderFieldValue(fields, {"schema"}) !=
                  "ndnsf-di-readiness-v1" ||
                !exactMember || !exactProvider || itemRole.empty() ||
                item.producerRole != itemRole ||
                nativeProviderFieldValue(fields, {"revision"}) != effectiveRevision ||
                nativeProviderFieldValue(fields, {"planDigest"}) != effectivePlanDigest ||
                nativeProviderFieldValue(fields, {"bindingDigest"}) !=
                  readinessBindingDigest ||
                nativeProviderFieldValue(fields, {"attempt"}) !=
                  std::to_string(attemptEpoch) ||
                nativeProviderFieldValue(fields, {"adapter"}).empty() ||
                nativeProviderFieldValue(fields, {"artifactDigest"}).empty()) {
              completeExecutionLease();
              ctx.fail("DI_READINESS_BINDING_MISMATCH");
              return;
            }
            const auto roleOwner = observedRoleProviders.find(itemRole);
            if (roleOwner != observedRoleProviders.end() &&
                roleOwner->second != providerText) {
              completeExecutionLease();
              ctx.fail("DI_READINESS_ROLE_CONFLICT");
              return;
            }
            const auto previous = observed.find(memberId);
            if (previous != observed.end() && previous->second != payload) {
              completeExecutionLease();
              ctx.fail("DI_READINESS_REPLAY_CONFLICT");
              return;
            }
            observedRoleProviders[itemRole] = providerText;
            observed[memberId] = payload;
          }
          if (observed.size() < expectedReadinessCount) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
          }
        }
        if (observed.size() != expectedReadinessCount) {
          completeExecutionLease();
          ctx.fail("DI_READINESS_BARRIER_TIMEOUT");
          return;
        }
        std::cout << "\nNDNSF_DI_READINESS_BARRIER"
                  << " status=ready"
                  << " session=" << ctx.sessionId()
                  << " role=" << role
                  << " observed_roles=" << observed.size()
                  << " revision=" << effectiveRevision
                  << " binding_digest=" << readinessBindingDigest
                  << std::endl;
      }
      reportStatus(executionOperationId, "distributed-inference", "RUNNING", 1,
                   0.0, "EXECUTING");
      if (const auto* spec = runnerSpecForRole(state->runnerSpecs, role)) {
        logFragmentInventoryEvent("EXECUTION_OBSERVED",
                                  *spec,
                                  ctx.localProvider().toUri());
      }
      std::optional<KvStateBinding> kvBinding;
      std::optional<TensorBundle> cachedKvState;
      const auto kvMode = nativeProviderFieldValue(assignmentFields, {"kvMode"});
      if (!kvMode.empty()) {
        if (prepareRunner) {
          completeExecutionLease();
          ctx.fail("KV_STATE_POST_SELECTION_PREPARATION_UNSUPPORTED");
          return;
        }
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
      const bool localFullPlan = nativeProviderShouldExecuteLocalPlan(
        executionPlan,
        assignment,
        roleSpec,
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
                                                       executionPlan,
                                                       executionSessionId,
                                                       assignment,
                                                       ctx.localProvider().toUri(),
                                                       initialInputs,
                                                       submittedSteady,
                                                       submittedEpoch,
                                                       prepareRunner);
        if (config.stageServiceTimeObserver && *config.stageServiceTimeObserver) {
          const auto elapsed = std::max(
            std::chrono::milliseconds(1),
            std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - submittedSteady));
          (*config.stageServiceTimeObserver)(elapsed);
        }
      }
      else {
        std::vector<RoleSpec> localRoleSpecs;
        bool selectionRoleMatched = false;
        for (const auto& plannedRole : executionPlan.roles) {
          const auto assigned = assignment.providerByRole.find(plannedRole);
          if (assigned == assignment.providerByRole.end() ||
              assigned->second != ctx.localProvider().toUri()) {
            continue;
          }
          if (selectionProjection) {
            if (plannedRole != selectionProjection->executionRole.roleId) {
              // A V3 assignment envelope authorizes exactly one local role.
              // Other roles owned by this Provider are launched by their own
              // envelope; they must not make this envelope fail closed.
              continue;
            }
            selectionRoleMatched = true;
            localRoleSpecs.push_back(roleSpecFromSelectionProjectionV3(
              *selectionProjection, ctx.localProvider().toUri()));
          }
          else {
            localRoleSpecs.push_back(
              executionAttempt
                ? roleSpecFor(executionPlan,
                              plannedRole,
                              *executionAttempt,
                              assignment,
                              ctx.localProvider().toUri())
                : roleSpecFor(executionPlan,
                              plannedRole,
                              executionSessionId,
                              assignment,
                              ctx.localProvider().toUri()));
          }
        }
        if (selectionProjection && !selectionRoleMatched) {
          throw std::invalid_argument(
            "V3 Selection cannot execute an undeclared local role");
        }
        // Register every local consumer's dependency wait before a colocated
        // source can publish.  SVSPubSub subscriptions are prospective; this
        // ordering removes a same-Provider source/consumer startup race while
        // preserving the plan's data dependencies and concurrent execution.
        std::stable_sort(
          localRoleSpecs.begin(), localRoleSpecs.end(),
          [] (const RoleSpec& lhs, const RoleSpec& rhs) {
            return !lhs.inputs.empty() && rhs.inputs.empty();
          });
        std::vector<std::pair<RoleSpec, std::future<ProviderRoleResult>>> localRoles;
        for (auto& localRoleSpec : localRoleSpecs) {
          auto roleInputs = localRoleSpec.inputs.empty()
            ? initialInputs : std::map<std::string, TensorBundle>{};
          if (prepareRunner) {
            if (localRoleSpecs.size() != 1 || localRoleSpec.role != role) {
              throw std::runtime_error(
                "post-Selection runner preparation requires one local role");
            }
            localRoles.emplace_back(
              localRoleSpec,
              state->runtime.executePreparedRoleAsync(
                executionSessionId, localRoleSpec, io, prepareRunner,
                std::move(roleInputs)));
          }
          else {
            localRoles.emplace_back(
              localRoleSpec,
              state->runtime.executeRoleAsync(
                executionSessionId, localRoleSpec, io,
                std::move(roleInputs)));
          }
        }
        if (localRoles.empty()) {
          throw std::runtime_error(
            "V3 Selection projection assigns no executable role to Provider");
        }

        for (auto& localRole : localRoles) {
          auto result = localRole.second.get();
          const auto& executedRoleSpec = localRole.first;
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
                        << " role=" << executedRoleSpec.role
                        << " context_epoch="
                        << (nextEpoch.empty() ? kvBinding->contextEpoch : std::stoull(nextEpoch))
                        << " bytes=" << kvOutput->second.payload.size()
                        << std::endl;
            }
          }
          logProviderTiming(ctx.sessionId(), executedRoleSpec.role,
                            result, submittedSteady, submittedEpoch);
          if (config.stageServiceTimeObserver && *config.stageServiceTimeObserver) {
            const auto elapsed = std::max(
              std::chrono::milliseconds(1),
              std::chrono::duration_cast<std::chrono::milliseconds>(
                result.timing.finishedAt - result.timing.startedAt));
            (*config.stageServiceTimeObserver)(elapsed);
          }

          auto localFinalPayload = nativeProviderFinalResponsePayload(
            executedRoleSpec,
            result,
            config.finalResponseScope);
          if (localFinalPayload) {
            finalPayload = std::move(localFinalPayload);
          }
        }
      }
      logProviderCapacity(ctx.sessionId(),
                          role,
                          "after_complete",
                          state->runtime.snapshot());
      if (protectedRuntime) {
        protectedRuntime->complete();
      }
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
      reportStatus(executionOperationId, "distributed-inference", "DONE", 2,
                   1.0, "EXECUTED");
      if (nativeTraceEnabled()) {
        std::cout << "\nNDNSF_DI_NATIVE_FINAL_RESPONSE_DECISION"
                  << " session=" << ctx.sessionId()
                  << " role=" << role
                  << " role_outputs=" << roleSpec.outputs.size()
                  << " local_full_plan="
                  << (localFullPlan ?
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
	      if (protectedRuntime) {
	        try {
	          protectedRuntime->cancel(exc.what());
	        }
	        catch (...) {
	          // The runtime remains FailedClosed; preserve the original failure.
	        }
	      }
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
