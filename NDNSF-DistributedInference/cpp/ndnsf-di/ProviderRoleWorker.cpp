#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/DiTimelineTrace.hpp"
#ifdef NDNSF_DI_EXPERIMENT_FAULTS
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeFaultInjection.hpp"
#endif

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {

namespace {

void
appendUint64(std::ostringstream& os, std::uint64_t value)
{
  os.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void
appendString(std::ostringstream& os, const std::string& value)
{
  appendUint64(os, static_cast<std::uint64_t>(value.size()));
  os.write(value.data(), static_cast<std::streamsize>(value.size()));
}

void
appendBytes(std::ostringstream& os, const std::vector<std::uint8_t>& value)
{
  appendUint64(os, static_cast<std::uint64_t>(value.size()));
  if (!value.empty()) {
    os.write(reinterpret_cast<const char*>(value.data()),
             static_cast<std::streamsize>(value.size()));
  }
}

std::string
fnv1a64Hex(const std::string& value)
{
  std::uint64_t hash = 1469598103934665603ULL;
  for (const auto ch : value) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= 1099511628211ULL;
  }
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << hash;
  return out.str();
}

std::string
timelineRequestId(const std::string& requestId, const std::string& sessionId)
{
  return requestId.empty() ? "/ndnsf-di/session/" + sessionId : requestId;
}

std::string
stagePhaseForEdge(const DependencyEdge& edge)
{
  return canonicalStagePhase(edge.operationKind);
}

void
populateLineageObservation(StageTransferObservation& observation,
                           const TensorBundle& bundle)
{
  observation.lineagePresent = false;
  observation.lineageIdentity.clear();
  observation.positionDigest.clear();
  if (const auto lineage = extractGenerationEpochLineage(bundle)) {
    observation.lineagePresent = true;
    observation.positionDigest = lineage->positionDigest;
    observation.lineageIdentity = lineage->requestId + "|" +
      lineage->generationId + "|" + std::to_string(lineage->streamEpoch) +
      "|" + std::to_string(lineage->inferenceEpoch) + "|" +
      lineage->transitionKind + "|" + lineage->positionDigest + "|" +
      lineage->producerRole + "|" + lineage->consumerRole + "|" +
      std::to_string(lineage->operationIndex);
  }
}

std::shared_ptr<StageTransferObservation>
makeStageTransferObservation(const DependencyEdge& edge,
                             const TensorBundle& bundle,
                             const std::string& direction)
{
  auto observation = std::make_shared<StageTransferObservation>();
  observation->edgeScope = edge.scope;
  observation->plannedDataName = edge.plannedDataName;
  observation->direction = direction;
  observation->phase = stagePhaseForEdge(edge);
  observation->identity = edge.requestId + "|" +
    std::to_string(edge.attemptEpoch) + "|" + edge.scope + "|" +
    edge.plannedDataName + "|" + direction + "|" + edge.operationKind +
    "|" + std::to_string(edge.round) + "|" +
    std::to_string(edge.microbatch);
  populateLineageObservation(*observation, bundle);
  if (!observation->lineageIdentity.empty()) {
    observation->identity += "|" + observation->lineageIdentity;
  }
  observation->tensorBytes = tensorPayloadBytes(bundle);
  observation->tensorNames.clear();
  if (isEncodedTensorBundle(bundle.payload)) {
    for (const auto& tensor : decodeTensorBundle(bundle.payload)) {
      if (tensor.name != generationEpochLineageTensorName()) {
        observation->tensorNames.push_back(tensor.name);
      }
    }
  }
  observation->encodedPayloadBytes = bundle.payload.size();
  // This is the observable bundle handoff copy, not a claim about allocator
  // internals. Unknown transport fields remain unset until Core reports them.
  observation->localCopyBytes = bundle.payload.size();
  return observation;
}

void
completeStageTransferObservation(const DependencyEdge& edge,
                                 TensorBundle& bundle,
                                 const std::string& direction)
{
  if (!bundle.transferObservation) {
    bundle.transferObservation = makeStageTransferObservation(edge, bundle, direction);
  }
  else {
    auto& observation = *bundle.transferObservation;
    observation.edgeScope = edge.scope;
    observation.plannedDataName = edge.plannedDataName;
    observation.direction = direction;
    observation.phase = stagePhaseForEdge(edge);
    observation.identity = edge.requestId + "|" +
      std::to_string(edge.attemptEpoch) + "|" + edge.scope + "|" +
      edge.plannedDataName + "|" + direction + "|" + edge.operationKind +
      "|" + std::to_string(edge.round) + "|" +
      std::to_string(edge.microbatch);
    // The epoch coordinator removes generation lineage from the model-facing
    // payload after validating it.  In that path the runtime-only observation
    // is the remaining source of the authenticated position; do not erase it
    // merely because the payload has been normalized for the runner.
    if (extractGenerationEpochLineage(bundle)) {
      populateLineageObservation(observation, bundle);
    }
    if (!observation.lineageIdentity.empty()) {
      observation.identity += "|" + observation.lineageIdentity;
    }
    observation.tensorNames.clear();
    if (isEncodedTensorBundle(bundle.payload)) {
      for (const auto& tensor : decodeTensorBundle(bundle.payload)) {
        if (tensor.name != generationEpochLineageTensorName()) {
          observation.tensorNames.push_back(tensor.name);
        }
      }
    }
    observation.tensorBytes = tensorPayloadBytes(bundle);
    observation.encodedPayloadBytes = bundle.payload.size();
    if (!observation.localCopyBytes) {
      observation.localCopyBytes = bundle.payload.size();
    }
  }
  validateStageTransferObservation(*bundle.transferObservation);
}

TensorBundle
withoutProviderLocalState(const TensorBundle& bundle,
                          const std::vector<std::string>& stateOutputNames)
{
  if (stateOutputNames.empty() || !isEncodedTensorBundle(bundle.payload)) {
    return bundle;
  }
  auto tensors = decodeTensorBundle(bundle.payload);
  tensors.erase(
    std::remove_if(tensors.begin(), tensors.end(), [&] (const auto& tensor) {
      return std::find(stateOutputNames.begin(), stateOutputNames.end(),
                       tensor.name) != stateOutputNames.end();
    }),
    tensors.end());
  if (tensors.empty()) {
    throw std::logic_error(
      "planned dependency output contains only Provider-local decode state");
  }
  return makeEncodedTensorBundle(bundle.name, std::move(tensors));
}

TensorBundle
providerDecodeStateFromOutputs(
  const std::map<std::string, TensorBundle>& outputs,
  const RoleSpec& role)
{
  if (role.stateInputNames.size() != role.stateOutputNames.size() ||
      role.stateInputNames.empty()) {
    throw std::invalid_argument(
      "Provider decode-state input/output metadata length mismatch");
  }
  std::vector<NamedTensor> nextState;
  nextState.reserve(role.stateInputNames.size());
  for (std::size_t index = 0; index < role.stateOutputNames.size(); ++index) {
    bool found = false;
    for (const auto& output : outputs) {
      if (!isEncodedTensorBundle(output.second.payload)) {
        continue;
      }
      try {
        auto tensor = findTensor(
          decodeTensorBundle(output.second.payload), role.stateOutputNames[index]);
        tensor.name = role.stateInputNames[index];
        nextState.push_back(std::move(tensor));
        found = true;
        break;
      }
      catch (const std::out_of_range&) {
      }
    }
    if (!found) {
      throw std::runtime_error(
        "Provider role is missing decode-state output: " +
        role.stateOutputNames[index]);
    }
  }
  return makeEncodedTensorBundle("__ndnsf_provider_decode_state",
                                 std::move(nextState));
}

} // namespace

ProviderRoleWorker::ProviderRoleWorker(std::size_t workerCount,
                                       std::size_t dependencyWaitWorkers,
                                       std::size_t dependencyWaitQueueCapacity,
                                       std::chrono::milliseconds dependencyWaitTimeout,
                                       std::size_t readyQueueCapacity)
  : m_dependencyWaitScheduler(std::make_unique<DependencyWaitScheduler>(
      dependencyWaitWorkers, dependencyWaitQueueCapacity))
  , m_dependencyWaitTimeout(dependencyWaitTimeout)
  , m_readyQueueCapacity(readyQueueCapacity)
{
  if (workerCount == 0) {
    workerCount = 1;
  }
  if (m_readyQueueCapacity == 0) {
    throw std::invalid_argument(
      "ProviderRoleWorker ready queue capacity must be positive");
  }
  m_workers.reserve(workerCount);
  for (std::size_t i = 0; i < workerCount; ++i) {
    m_workers.emplace_back([this] { workerLoop(); });
  }
}

ProviderRoleWorker::~ProviderRoleWorker()
{
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stopping = true;
  }
  m_cv.notify_all();
  m_dependencyWaitScheduler->shutdown();
  for (auto& worker : m_workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

std::future<ProviderRoleResult>
ProviderRoleWorker::executeAsync(std::string sessionId,
                                 RoleSpec role,
                                 std::shared_ptr<DependencyIo> io,
                                 RoleRunner runner,
                                 std::map<std::string, TensorBundle> initialInputsByScope,
                                 RoleExecutionContext::StreamEventSink eventSink,
                                 std::function<void()> executionGuard)
{
  return executeAsyncImpl(std::move(sessionId),
                          std::move(role),
                          std::move(io),
                          makeNativeModelRunner(std::move(runner)),
                          {},
                          std::move(initialInputsByScope),
                          std::nullopt,
                          std::move(eventSink),
                          std::move(executionGuard));
}

std::future<ProviderRoleResult>
ProviderRoleWorker::executeAsync(std::string sessionId,
                                 RoleSpec role,
                                 std::shared_ptr<DependencyIo> io,
                                 std::shared_ptr<NativeModelRunner> runner,
                                 std::map<std::string, TensorBundle> initialInputsByScope,
                                 RoleExecutionContext::StreamEventSink eventSink,
                                 std::function<void()> executionGuard)
{
  return executeAsyncImpl(std::move(sessionId),
                          std::move(role),
                          std::move(io),
                          std::move(runner),
                          {},
                          std::move(initialInputsByScope),
                          std::nullopt,
                          std::move(eventSink),
                          std::move(executionGuard));
}

std::future<ProviderRoleResult>
ProviderRoleWorker::executePreparedAsync(
  std::string sessionId,
  RoleSpec role,
  std::shared_ptr<DependencyIo> io,
  NativeRunnerPreparation prepareRunner,
  std::map<std::string, TensorBundle> initialInputsByScope,
  RoleExecutionContext::StreamEventSink eventSink,
  std::function<void()> executionGuard)
{
  return executeAsyncImpl(std::move(sessionId),
                          std::move(role),
                          std::move(io),
                          nullptr,
                          std::move(prepareRunner),
                          std::move(initialInputsByScope),
                          std::nullopt,
                          std::move(eventSink),
                          std::move(executionGuard));
}

std::future<ProviderRoleResult>
ProviderRoleWorker::executeCollectiveAsync(
  std::string sessionId,
  RoleSpec role,
  std::shared_ptr<DependencyIo> io,
  RoleRunner runner,
  CollectiveExecutionBinding collective,
  std::map<std::string, TensorBundle> initialInputsByScope,
  RoleExecutionContext::StreamEventSink eventSink)
{
  return executeAsyncImpl(std::move(sessionId),
                          std::move(role),
                          std::move(io),
                          makeNativeModelRunner(std::move(runner)),
                          {},
                          std::move(initialInputsByScope),
                          std::move(collective),
                          std::move(eventSink));
}

std::future<ProviderRoleResult>
ProviderRoleWorker::executeCollectiveAsync(
  std::string sessionId,
  RoleSpec role,
  std::shared_ptr<DependencyIo> io,
  std::shared_ptr<NativeModelRunner> runner,
  CollectiveExecutionBinding collective,
  std::map<std::string, TensorBundle> initialInputsByScope,
  RoleExecutionContext::StreamEventSink eventSink)
{
  return executeAsyncImpl(std::move(sessionId),
                          std::move(role),
                          std::move(io),
                          std::move(runner),
                          {},
                          std::move(initialInputsByScope),
                          std::move(collective),
                          std::move(eventSink));
}

std::future<ProviderRoleResult>
ProviderRoleWorker::executeAsyncImpl(
  std::string sessionId,
  RoleSpec role,
  std::shared_ptr<DependencyIo> io,
  std::shared_ptr<NativeModelRunner> runner,
  NativeRunnerPreparation prepareRunner,
  std::map<std::string, TensorBundle> initialInputsByScope,
  std::optional<CollectiveExecutionBinding> collective,
  RoleExecutionContext::StreamEventSink eventSink,
  std::function<void()> executionGuard)
{
  if (role.role.empty()) {
    throw std::invalid_argument("ProviderRoleWorker requires a non-empty role");
  }
  if (!io) {
    throw std::invalid_argument("ProviderRoleWorker requires DependencyIo");
  }
  if (!runner && !prepareRunner) {
    throw std::invalid_argument(
      "ProviderRoleWorker requires NativeModelRunner or preparation callback");
  }
  if (collective.has_value()) {
    if (!collective->runtime || collective->rank.empty() ||
        collective->inputSequence == 0) {
      throw std::invalid_argument("ProviderRoleWorker requires a valid collective binding");
    }
    const auto group = collective->runtime->snapshot();
    const auto rank = group.ranks.find(collective->rank);
    if (rank == group.ranks.end()) {
      throw std::invalid_argument("ProviderRoleWorker collective rank is unknown");
    }
    if (!rank->second.authenticated || !rank->second.localReady) {
      throw std::logic_error(
        "ProviderRoleWorker collective rank is not authenticated and locally ready");
    }
    if (group.state != CollectiveRuntimeState::Pending &&
        group.state != CollectiveRuntimeState::Running) {
      throw std::logic_error("ProviderRoleWorker collective is already terminal");
    }
  }

  auto promise = std::make_shared<std::promise<ProviderRoleResult>>();
  auto future = promise->get_future();
  WorkItem item{
    std::move(sessionId),
    std::move(role),
    std::move(io),
    std::move(runner),
    std::move(prepareRunner),
    std::move(initialInputsByScope),
    std::move(eventSink),
    {},
    promise,
    std::chrono::steady_clock::now(),
    std::move(collective),
    std::move(executionGuard),
  };

  std::vector<PendingInput> pendingInputs;
  pendingInputs.reserve(item.role.inputs.size());
  try {
    for (const auto& edge : item.role.inputs) {
      InputFetchTiming timing;
      timing.producerRole = edge.producerRole;
      timing.scope = edge.scope;
      timing.plannedDataName = edge.plannedDataName;
      timing.plannedSegmentNames = plannedSegmentNamesForEdge(edge);
      timing.expectedSegments = edge.expectedSegments;
      timing.expectedBytes = edge.expectedBytes;
      timing.prefetchStartedAt = std::chrono::steady_clock::now();
      logDiTimelineTrace(
        "di-provider", "dependency_fetch_start",
        timelineRequestId(item.role.requestId, item.sessionId),
        {{"sessionId", item.sessionId},
         {"role", item.role.role},
         {"scope", edge.scope},
         {"inferenceEpoch", std::to_string(item.role.inferenceEpoch)},
         {"providerBootId", item.role.candidateDecodeStateIdentity ?
                              item.role.candidateDecodeStateIdentity->providerBootId : "none"},
         {"attemptEpoch", std::to_string(item.role.attemptEpoch)}});
      // A request-scoped epoch coordinator may have fetched and validated an
      // exact dependency before submitting this role.  Do not express a
      // second Interest for that same epoch; retaining the edge in the role
      // preserves the runner's input-edge contract while the pre-satisfied
      // bundle is consumed directly.
      const auto existing = item.initialInputsByScope.find(edge.scope);
      if (existing != item.initialInputsByScope.end()) {
        validateTensorBundleForEdge(edge, existing->second);
        timing.fetchCompletedAt = std::chrono::steady_clock::now();
        timing.bytes = existing->second.payload.size();
        completeStageTransferObservation(edge, existing->second, "receive");
        timing.transferObservation = existing->second.transferObservation;
        item.inputTimings.push_back(std::move(timing));
        logDiTimelineTrace(
          "di-provider", "dependency_fetch_pre_satisfied",
          timelineRequestId(item.role.requestId, item.sessionId),
          {{"sessionId", item.sessionId},
           {"role", item.role.role},
           {"scope", edge.scope},
           {"inferenceEpoch", std::to_string(item.role.inferenceEpoch)},
           {"providerBootId", item.role.candidateDecodeStateIdentity ?
                                item.role.candidateDecodeStateIdentity->providerBootId : "none"},
           {"attemptEpoch", std::to_string(item.role.attemptEpoch)}});
        continue;
      }
      pendingInputs.push_back(PendingInput{
        edge,
        item.io->prefetchInput(item.sessionId, edge),
        std::move(timing),
      });
    }
  }
  catch (...) {
    promise->set_exception(std::current_exception());
    return future;
  }

  if (pendingInputs.empty()) {
    enqueueReady(std::move(item));
  }
  else if (std::all_of(pendingInputs.begin(), pendingInputs.end(),
                       [] (const PendingInput& pending) {
                         return pending.future.wait_for(std::chrono::milliseconds(0)) ==
                                std::future_status::ready;
                       })) {
    try {
      for (auto& pending : pendingInputs) {
        auto bundle = pending.future.get();
        validateTensorBundleForEdge(pending.edge, bundle);
#ifdef NDNSF_DI_EXPERIMENT_FAULTS
        NativeFaultInjection::instance().checkpoint(
          NativeFaultPoint::DependencyFetched, item.role.role, item.sessionId);
#endif
        pending.timing.fetchCompletedAt = std::chrono::steady_clock::now();
        pending.timing.bytes = bundle.payload.size();
        completeStageTransferObservation(pending.edge, bundle, "receive");
        pending.timing.transferObservation = bundle.transferObservation;
        item.initialInputsByScope[pending.edge.scope] = std::move(bundle);
        item.inputTimings.push_back(std::move(pending.timing));
        logDiTimelineTrace(
          "di-provider", "dependency_fetch_done",
          timelineRequestId(item.role.requestId, item.sessionId),
          {{"sessionId", item.sessionId},
           {"role", item.role.role},
           {"scope", pending.edge.scope},
           {"inferenceEpoch", std::to_string(item.role.inferenceEpoch)},
           {"providerBootId", item.role.candidateDecodeStateIdentity ?
                                item.role.candidateDecodeStateIdentity->providerBootId : "none"},
           {"attemptEpoch", std::to_string(item.role.attemptEpoch)}});
      }
      enqueueReady(std::move(item));
    }
    catch (...) {
      promise->set_exception(std::current_exception());
    }
  }
  else {
    scheduleWhenInputsReady(std::move(item), std::move(pendingInputs));
  }
  return future;
}

void
ProviderRoleWorker::scheduleWhenInputsReady(WorkItem item,
                                            std::vector<PendingInput> pendingInputs)
{
  struct WaitState
  {
    WorkItem item;
    std::vector<PendingInput> pendingInputs;
  };
  auto state = std::make_shared<WaitState>(WaitState{
    std::move(item), std::move(pendingInputs),
  });
  const auto waitId = state->item.sessionId + "|" + state->item.role.role + "|" +
                      std::to_string(m_nextDependencyWaitId.fetch_add(1));
  const auto deadline = std::chrono::steady_clock::now() + m_dependencyWaitTimeout;
  const auto submitted = m_dependencyWaitScheduler->submit(
    waitId,
    deadline,
    [state] (const DependencyWaitControl& control) {
      try {
        for (auto& pending : state->pendingInputs) {
          while (pending.future.wait_for(std::chrono::milliseconds(2)) !=
                 std::future_status::ready) {
            if (control.isCancelled()) {
              return DependencyWaitStatus::Cancelled;
            }
            if (control.deadlineExpired()) {
              return DependencyWaitStatus::DeadlineExpired;
            }
          }
          auto bundle = pending.future.get();
          validateTensorBundleForEdge(pending.edge, bundle);
#ifdef NDNSF_DI_EXPERIMENT_FAULTS
          NativeFaultInjection::instance().checkpoint(
            NativeFaultPoint::DependencyFetched,
            state->item.role.role, state->item.sessionId);
#endif
          pending.timing.fetchCompletedAt = std::chrono::steady_clock::now();
          pending.timing.bytes = bundle.payload.size();
          completeStageTransferObservation(pending.edge, bundle, "receive");
          pending.timing.transferObservation = bundle.transferObservation;
          state->item.initialInputsByScope[pending.edge.scope] = std::move(bundle);
          state->item.inputTimings.push_back(std::move(pending.timing));
          logDiTimelineTrace(
            "di-provider", "dependency_fetch_done",
            timelineRequestId(state->item.role.requestId,
                              state->item.sessionId),
            {{"sessionId", state->item.sessionId},
             {"role", state->item.role.role},
             {"scope", pending.edge.scope},
             {"inferenceEpoch", std::to_string(state->item.role.inferenceEpoch)},
             {"providerBootId", state->item.role.candidateDecodeStateIdentity ?
                                  state->item.role.candidateDecodeStateIdentity->providerBootId : "none"},
             {"attemptEpoch", std::to_string(
                state->item.role.attemptEpoch)}});
        }
        return DependencyWaitStatus::Completed;
      }
      catch (...) {
        throw;
      }
    },
    [this, state] (const DependencyWaitResult& result) mutable {
      if (result.status == DependencyWaitStatus::Completed) {
        enqueueReady(std::move(state->item));
        return;
      }
      const auto reason = result.reason.empty() ? toString(result.status) : result.reason;
      std::ostringstream record;
      record << "NDNSF_DI_PROVIDER_WAIT_TERMINAL"
             << " session=" << state->item.sessionId
             << " role=" << state->item.role.role
             << " status=" << toString(result.status)
             << " reason=" << reason;
      logRuntimeWarn(record.str());
      failPromise(state->item.promise,
                  std::make_exception_ptr(std::runtime_error(
                    "dependency wait failed: " + reason)));
  });
  if (submitted != DependencyWaitSubmitResult::Accepted) {
    std::ostringstream record;
    record << "NDNSF_DI_PROVIDER_WAIT_ADMISSION"
           << " session=" << state->item.sessionId
           << " role=" << state->item.role.role
           << " status=" << toString(submitted);
    logRuntimeWarn(record.str());
    failPromise(state->item.promise,
                std::make_exception_ptr(std::runtime_error(
                  std::string("dependency wait admission failed: ") +
                  toString(submitted))));
  }
}

void
ProviderRoleWorker::enqueueReady(WorkItem item)
{
  if (item.collective.has_value()) {
    enqueueCollectiveReady(std::move(item));
    return;
  }
  item.queuedAt = std::chrono::steady_clock::now();
  logDiTimelineTrace(
    "di-provider", "role_queue_enter",
    timelineRequestId(item.role.requestId, item.sessionId),
    {{"sessionId", item.sessionId},
     {"role", item.role.role},
     {"attemptEpoch", std::to_string(item.role.attemptEpoch)}});
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping) {
      failPromise(item.promise,
                  std::make_exception_ptr(std::logic_error(
                    "ProviderRoleWorker is stopping")));
      return;
    }
    if (m_queue.size() >= m_readyQueueCapacity) {
      failPromise(item.promise,
                  std::make_exception_ptr(std::runtime_error(
                    "ProviderRoleWorker ready queue is full")));
      return;
    }
    m_queue.push_back(std::move(item));
  }
  if (std::getenv("NDNSF_DI_RUNTIME_TIMING") != nullptr) {
    logRuntimeTrace("NDNSF_DI_WORKER event=enqueue_ready");
  }
  m_cv.notify_one();
}

std::uint64_t
ProviderRoleWorker::collectiveNowMs()
{
  static const auto origin = std::chrono::steady_clock::now();
  return static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - origin).count());
}

void
ProviderRoleWorker::enqueueCollectiveReady(WorkItem item)
{
  auto& binding = *item.collective;
  const auto runtime = binding.runtime;
  const auto nowMs = collectiveNowMs();
  {
    std::lock_guard<std::mutex> collectiveLock(m_collectiveMutex);
    if (!runtime->markInputReady(binding.rank,
                                 runtime->groupEpoch(),
                                 binding.inputSequence,
                                 nowMs)) {
      failPromise(item.promise,
                  std::make_exception_ptr(std::runtime_error(
                    "collective input readiness rejected: " +
                    runtime->snapshot().lastError)));
      return;
    }

    item.queuedAt = std::chrono::steady_clock::now();
    logDiTimelineTrace(
      "di-provider", "collective_input_ready",
      timelineRequestId(item.role.requestId, item.sessionId),
      {{"sessionId", item.sessionId},
       {"role", item.role.role},
       {"rank", binding.rank},
       {"attemptEpoch", std::to_string(item.role.attemptEpoch)}});

    std::vector<WorkItem> ready;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_stopping) {
        failPromise(item.promise,
                    std::make_exception_ptr(std::logic_error(
                      "ProviderRoleWorker is stopping")));
        return;
      }
      auto& pending = m_collectivePending[runtime.get()];
      pending.push_back(std::move(item));

      if (runtime->state() == CollectiveRuntimeState::Pending) {
        if (!runtime->start(nowMs)) {
          const auto state = runtime->snapshot();
          if (state.lastError != "start:group-not-ready") {
            for (auto& pendingItem : pending) {
              failPromise(pendingItem.promise,
                          std::make_exception_ptr(std::runtime_error(
                            "collective start rejected: " + state.lastError)));
            }
            m_collectivePending.erase(runtime.get());
          }
          return;
        }
      }

      if (runtime->state() == CollectiveRuntimeState::Running) {
        ready = std::move(pending);
        m_collectivePending.erase(runtime.get());
        for (auto& readyItem : ready) {
          logDiTimelineTrace(
            "di-provider", "collective_role_release",
            timelineRequestId(readyItem.role.requestId, readyItem.sessionId),
            {{"sessionId", readyItem.sessionId},
             {"role", readyItem.role.role},
             {"rank", readyItem.collective->rank},
             {"attemptEpoch", std::to_string(readyItem.role.attemptEpoch)}});
          m_queue.push_back(std::move(readyItem));
        }
      }
    }
    if (!ready.empty()) {
      m_cv.notify_all();
    }
  }
}

ProviderRoleWorkerSnapshot
ProviderRoleWorker::snapshot() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  ProviderRoleWorkerSnapshot snapshot;
  snapshot.workerCount = m_workers.size();
  snapshot.readyQueueDepth = m_queue.size();
  snapshot.readyQueueCapacity = m_readyQueueCapacity;
  const auto waitSnapshot = m_dependencyWaitScheduler->snapshot();
  snapshot.waitingForInputCount = waitSnapshot.queuedCount + waitSnapshot.activeCount;
  snapshot.dependencyWaitWorkerCount = waitSnapshot.workerCount;
  snapshot.dependencyWaitQueueCapacity = waitSnapshot.queueCapacity;
  snapshot.dependencyWaitQueuedCount = waitSnapshot.queuedCount;
  snapshot.dependencyWaitActiveCount = waitSnapshot.activeCount;
  snapshot.dependencyWaitCompleted = waitSnapshot.completed;
  snapshot.dependencyWaitCancelled = waitSnapshot.cancelled;
  snapshot.dependencyWaitDeadlineExpired = waitSnapshot.deadlineExpired;
  snapshot.dependencyWaitFailed = waitSnapshot.failed;
  snapshot.dependencyWaitRejected = waitSnapshot.rejected;
  snapshot.activeWorkerCount = m_activeWorkers;
  snapshot.stopping = m_stopping;
  return snapshot;
}

void
ProviderRoleWorker::failPromise(const std::shared_ptr<std::promise<ProviderRoleResult>>& promise,
                                std::exception_ptr failure)
{
  try {
    promise->set_exception(failure);
  }
  catch (const std::future_error&) {
  }
}

std::string
ProviderRoleWorker::exactForwardCacheKeyFor(
  const WorkItem& item,
  const NativeModelRunner* runner,
  const std::map<std::string, TensorBundle>& inputsByScope)
{
  std::ostringstream os;
  appendString(os, "ndnsf-di-provider-local-exact-forward-cache-v1");
  if (runner == nullptr) {
    throw std::invalid_argument(
      "ProviderRoleWorker exact-forward cache requires a runner");
  }
  appendUint64(os, runner->cacheIdentity());
  appendString(os, item.role.role);
  appendUint64(os, static_cast<std::uint64_t>(item.role.inputs.size()));
  for (const auto& edge : item.role.inputs) {
    appendString(os, edge.scope);
    appendString(os, edge.producerRole);
    appendString(os, edge.consumerRole);
    appendString(os, edge.plannedDataName);
    appendUint64(os, static_cast<std::uint64_t>(edge.expectedSegments));
    appendUint64(os, static_cast<std::uint64_t>(edge.expectedBytes));
    appendUint64(os, static_cast<std::uint64_t>(edge.tensors.size()));
    for (const auto& tensor : edge.tensors) {
      appendString(os, tensor);
    }
    appendUint64(os, static_cast<std::uint64_t>(edge.bundleTensorNames.size()));
    for (const auto& tensor : edge.bundleTensorNames) {
      appendString(os, tensor);
    }
  }
  appendUint64(os, static_cast<std::uint64_t>(item.role.outputs.size()));
  for (const auto& edge : item.role.outputs) {
    appendString(os, edge.scope);
    appendString(os, edge.producerRole);
    appendString(os, edge.consumerRole);
    appendUint64(os, static_cast<std::uint64_t>(edge.tensors.size()));
    for (const auto& tensor : edge.tensors) {
      appendString(os, tensor);
    }
    appendUint64(os, static_cast<std::uint64_t>(edge.bundleTensorNames.size()));
    for (const auto& tensor : edge.bundleTensorNames) {
      appendString(os, tensor);
    }
  }
  appendUint64(os, static_cast<std::uint64_t>(inputsByScope.size()));
  for (const auto& input : inputsByScope) {
    appendString(os, input.first);
    appendString(os, input.second.name);
    appendUint64(os, static_cast<std::uint64_t>(input.second.expectedSegments));
    appendUint64(os, static_cast<std::uint64_t>(input.second.expectedBytes));
    appendBytes(os, input.second.payload);
  }
  return fnv1a64Hex(os.str());
}

std::map<std::string, TensorBundle>
ProviderRoleWorker::getCachedOutputs(const std::string& key)
{
  std::lock_guard<std::mutex> lock(m_exactForwardCacheMutex);
  const auto found = m_exactForwardCache.find(key);
  if (found == m_exactForwardCache.end()) {
    return {};
  }
  return found->second;
}

void
ProviderRoleWorker::putCachedOutputs(std::string key,
                                     std::map<std::string, TensorBundle> outputs)
{
  if (m_exactForwardCacheMaxEntries == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(m_exactForwardCacheMutex);
  if (m_exactForwardCache.find(key) == m_exactForwardCache.end()) {
    m_exactForwardCacheOrder.push_back(key);
  }
  m_exactForwardCache[std::move(key)] = std::move(outputs);
  while (m_exactForwardCacheOrder.size() > m_exactForwardCacheMaxEntries) {
    const auto evictKey = m_exactForwardCacheOrder.front();
    m_exactForwardCacheOrder.erase(m_exactForwardCacheOrder.begin());
    m_exactForwardCache.erase(evictKey);
  }
}

void
ProviderRoleWorker::workerLoop()
{
  while (true) {
    WorkItem item;
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_cv.wait(lock, [&] { return m_stopping || !m_queue.empty(); });
      if (m_stopping && m_queue.empty()) {
        return;
      }
      item = std::move(m_queue.front());
      m_queue.pop_front();
      ++m_activeWorkers;
    }
    execute(item);
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_activeWorkers > 0) {
        --m_activeWorkers;
      }
    }
  }
}

void
ProviderRoleWorker::execute(const WorkItem& item)
{
  std::atomic<bool> watchdogDone{false};
  std::thread watchdog;
  if (item.collective.has_value()) {
    const auto runtime = item.collective->runtime;
    const auto tickMs = std::max<std::uint64_t>(
      1, runtime->snapshot().schedulerTickMs);
    watchdog = std::thread([this, runtime, tickMs, &watchdogDone] {
      while (!watchdogDone.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(tickMs));
        if (watchdogDone.load(std::memory_order_acquire)) {
          break;
        }
        std::lock_guard<std::mutex> collectiveLock(m_collectiveMutex);
        runtime->poll(collectiveNowMs());
      }
    });
  }
  try {
    auto result = runReadyRole(item);
    watchdogDone.store(true, std::memory_order_release);
    if (watchdog.joinable()) {
      watchdog.join();
    }
    item.promise->set_value(std::move(result));
  }
  catch (...) {
    watchdogDone.store(true, std::memory_order_release);
    if (watchdog.joinable()) {
      watchdog.join();
    }
    if (item.collective.has_value()) {
      const auto& binding = *item.collective;
      std::lock_guard<std::mutex> collectiveLock(m_collectiveMutex);
      binding.runtime->fail(
        "NDNSF_COLLECTIVE_RANK_FAILURE:" + binding.rank,
        collectiveNowMs());
    }
    item.promise->set_exception(std::current_exception());
  }
}

ProviderRoleResult
ProviderRoleWorker::runReadyRole(const WorkItem& item)
{
  if (item.executionGuard) item.executionGuard();
  if (std::getenv("NDNSF_DI_RUNTIME_TIMING") != nullptr) {
    logRuntimeTrace("NDNSF_DI_WORKER event=run_ready role=" + item.role.role);
  }
  if (item.collective.has_value()) {
    const auto& binding = *item.collective;
    std::lock_guard<std::mutex> collectiveLock(m_collectiveMutex);
    const auto state = binding.runtime->snapshot();
    if (state.state != CollectiveRuntimeState::Running) {
      throw std::runtime_error(
        "collective role released outside RUNNING state: " + state.lastError);
    }
  }
  const auto workerStartedAt = std::chrono::steady_clock::now();
  const auto requestId = timelineRequestId(item.role.requestId, item.sessionId);
  logDiTimelineTrace(
    "di-provider", "role_queue_exit", requestId,
    {{"sessionId", item.sessionId},
     {"role", item.role.role},
     {"attemptEpoch", std::to_string(item.role.attemptEpoch)}});
  std::map<std::string, TensorBundle> inputsByScope = item.initialInputsByScope;

  auto runner = item.runner;
  if (!runner) {
    logDiTimelineTrace(
      "di-provider", "role_preparation_start", requestId,
      {{"sessionId", item.sessionId},
       {"role", item.role.role},
       {"inferenceEpoch", std::to_string(item.role.inferenceEpoch)},
       {"providerBootId", item.role.candidateDecodeStateIdentity ?
                            item.role.candidateDecodeStateIdentity->providerBootId : "none"},
       {"attemptEpoch", std::to_string(item.role.attemptEpoch)}});
    runner = item.prepareRunner();
    if (!runner) {
      throw std::runtime_error(
        "Provider role preparation returned no NativeModelRunner");
    }
    logDiTimelineTrace(
      "di-provider", "role_preparation_done", requestId,
      {{"sessionId", item.sessionId},
       {"role", item.role.role},
       {"inferenceEpoch", std::to_string(item.role.inferenceEpoch)},
       {"providerBootId", item.role.candidateDecodeStateIdentity ?
                            item.role.candidateDecodeStateIdentity->providerBootId : "none"},
       {"attemptEpoch", std::to_string(item.role.attemptEpoch)}});
  }

  if (item.executionGuard) item.executionGuard();
  ProviderRoleResult result;
  result.runner = runner;
  result.transferBudget.snapshotIdentity = item.sessionId + "|" +
    item.role.requestId + "|" + std::to_string(item.role.inferenceEpoch) +
    "|" + std::to_string(item.role.attemptEpoch);
  result.runnerSupportsOpaqueStateHandles = runner->supportsOpaqueStateHandles();
  result.timing.role = item.role.role;
  result.timing.queuedAt = item.queuedAt;
  result.timing.workerStartedAt = workerStartedAt;
  result.timing.startedAt = std::chrono::steady_clock::now();
  result.inputTimings = item.inputTimings;

  RoleExecutionContext ctx;
  ctx.sessionId = item.sessionId;
  ctx.role = item.role.role;
  ctx.requestId = item.role.requestId;
  ctx.providerBootId = item.role.candidateDecodeStateIdentity ?
    item.role.candidateDecodeStateIdentity->providerBootId : "none";
  ctx.attemptEpoch = item.role.attemptEpoch;
  ctx.inferenceEpoch = item.role.inferenceEpoch;
  ctx.streamingStateExecution = item.role.streamingStateExecution;
  ctx.generationLineage = item.role.generationLineage;
  if (item.role.candidateDecodeStateIdentity) {
    const auto current =
      item.role.candidateDecodeStateIdentity->prefixTokenCount;
    std::uint32_t previous = 0;
    if (item.role.predecessorDecodeStateIdentity) {
      previous = item.role.predecessorDecodeStateIdentity->prefixTokenCount;
    }
    else if (item.role.conversationStateBinding) {
      previous =
        item.role.conversationStateBinding->identity.prefixTokenCount;
    }
    if (current <= previous) {
      throw std::logic_error(
        "Provider generation input token count is not a strict prefix extension");
    }
    ctx.generationInputTokenCount = current - previous;
  }
  ctx.inputsByScope = inputsByScope;
  ctx.streamEventSink = item.eventSink;
  if (item.eventSink && item.executionGuard) {
    ctx.streamEventSink = [sink = item.eventSink, guard = item.executionGuard] (const auto& payload) {
      guard();
      return sink(payload);
    };
  }
  for (const auto& edge : item.role.inputs) {
    const auto input = ctx.inputsByScope.find(edge.scope);
    if (input == ctx.inputsByScope.end()) {
      throw std::runtime_error(
        "Provider role is missing a declared input: " + edge.scope);
    }
    validateTensorBundleForEdge(edge, input->second);
    ctx.inputEdgesByScope.emplace(edge.scope, edge);
  }
  if (std::getenv("NDNSF_DI_RUNTIME_TIMING") != nullptr) {
    logRuntimeTrace("NDNSF_DI_WORKER event=inputs_validated role=" + item.role.role);
  }

  result.exactForwardCacheKey = exactForwardCacheKeyFor(
    item, runner.get(), inputsByScope);
  if (std::getenv("NDNSF_DI_RUNTIME_TIMING") != nullptr) {
    logRuntimeTrace("NDNSF_DI_WORKER event=cache_key role=" + item.role.role);
  }
  // A streamed terminal runner has an externally visible side effect: it
  // submits token/event payloads through the sink. Reusing only its Tensor
  // outputs would suppress those events on a retry and could turn a complete
  // transcript into the legacy single-final-event fallback. Keep this cache
  // limited to pure dependency-producing executions.
  if (item.executionGuard) item.executionGuard();
  const bool hasStreamSideEffect = static_cast<bool>(item.eventSink);
  // A generation epoch may update adapter-owned KV or depend on lineage not
  // represented in the pure-output cache key, even with equal tensor bytes.
  // Reusing a loaded runner must not suppress that state transition.
  const bool canReuseOutputs = !hasStreamSideEffect &&
    !item.role.streamingStateExecution && !item.role.generationLineage;
  if (canReuseOutputs) {
    result.outputsByScope = getCachedOutputs(result.exactForwardCacheKey);
  }
  result.exactForwardCacheHit = !result.outputsByScope.empty();
  if (!result.exactForwardCacheHit) {
#ifdef NDNSF_DI_EXPERIMENT_FAULTS
    NativeFaultInjection::instance().checkpoint(
      NativeFaultPoint::BeforeCompute, item.role.role, item.sessionId);
#endif
    logDiTimelineTrace(
      "di-provider", "role_compute_start", requestId,
      {{"sessionId", item.sessionId},
       {"role", item.role.role},
       {"inferenceEpoch", std::to_string(item.role.inferenceEpoch)},
       {"providerBootId", item.role.candidateDecodeStateIdentity ?
                            item.role.candidateDecodeStateIdentity->providerBootId : "none"},
       {"attemptEpoch", std::to_string(item.role.attemptEpoch)}});
    // The epoch coordinator owns the authenticated generation loop.  Its
    // lineage-bearing work items represent exactly one epoch, so invoking a
    // runner's full streamed loop here would generate the whole transcript
    // once per epoch and duplicate token events.  Direct streamed requests
    // (which have no coordinator lineage) retain the incremental runner path.
    const bool coordinatorOwnsStreaming = item.role.generationLineage.has_value();
    if (hasStreamSideEffect && !coordinatorOwnsStreaming) {
      if (std::getenv("NDNSF_DI_RUNTIME_TIMING") != nullptr) {
        logRuntimeTrace("NDNSF_DI_WORKER event=runner_streamed role=" + item.role.role);
      }
      if (item.executionGuard) item.executionGuard();
      const auto streamedOutputs = runner->runStreamed(ctx);
      if (item.executionGuard) item.executionGuard();
      result.outputsByScope = streamedOutputs.has_value()
        ? *streamedOutputs : runner->run(ctx);
    }
    else {
      if (std::getenv("NDNSF_DI_RUNTIME_TIMING") != nullptr) {
        logRuntimeTrace("NDNSF_DI_WORKER event=runner_run role=" + item.role.role);
      }
      if (item.executionGuard) item.executionGuard();
      result.outputsByScope = runner->run(ctx);
    }
    if (std::getenv("NDNSF_DI_RUNTIME_TIMING") != nullptr) {
      logRuntimeTrace("NDNSF_DI_WORKER event=runner_done role=" + item.role.role);
    }
    logDiTimelineTrace(
      "di-provider", "role_compute_done", requestId,
      {{"sessionId", item.sessionId},
       {"role", item.role.role},
       {"inferenceEpoch", std::to_string(item.role.inferenceEpoch)},
       {"providerBootId", item.role.candidateDecodeStateIdentity ?
                            item.role.candidateDecodeStateIdentity->providerBootId : "none"},
       {"attemptEpoch", std::to_string(item.role.attemptEpoch)}});
    if (item.executionGuard) item.executionGuard();
    if (canReuseOutputs) {
      putCachedOutputs(result.exactForwardCacheKey, result.outputsByScope);
    }
  }
  if (item.executionGuard) item.executionGuard();
  result.executionEvidence = runner->executionEvidenceSnapshot();
  if (result.executionEvidence) {
    bindExecutionObservation(*result.executionEvidence,
                             item.role.requestId, item.role.attemptEpoch,
                             result.exactForwardCacheHit);
  }
  result.runtimeMetrics = runner->runtimeMetricsSnapshot();
  if (item.role.streamingStateExecution &&
      !result.runnerSupportsOpaqueStateHandles) {
    // Capture the state successor before the dependency publication loop
    // removes Provider-local tensors from a bundle.  Qwen's assembled output
    // scope is intentionally the same as its pipeline edge scope, so keeping
    // only outputsByScope would overwrite the state-bearing bundle before the
    // outer NativeProviderRuntime can stage its decode-state transaction.
    result.providerDecodeState = providerDecodeStateFromOutputs(
      result.outputsByScope, item.role);
  }
  if (item.role.streamingStateExecution &&
      runner->supportsOpaqueStateHandles()) {
    if (const auto handle = runner->stateHandleSnapshot(item.sessionId)) {
      handle->validate();
      if (handle->sessionId != item.sessionId || handle->role != item.role.role) {
        throw std::runtime_error(
          "PROVIDER_OPAQUE_STATE_HANDLE_BINDING_MISMATCH");
      }
      result.stateHandle = *handle;
    }
  }

  const auto outputReadyAt = std::chrono::steady_clock::now();
  std::vector<std::pair<DependencyEdge, TensorBundle>> stagedOutputs;
  stagedOutputs.reserve(item.role.outputs.size());
  for (const auto& edge : item.role.outputs) {
    for (const auto& tensor : edge.tensors) {
      if (std::find(item.role.stateOutputNames.begin(),
                    item.role.stateOutputNames.end(), tensor) !=
          item.role.stateOutputNames.end()) {
        throw std::logic_error(
          "planned dependency explicitly names Provider-local decode state");
      }
    }
    for (const auto& tensor : edge.bundleTensorNames) {
      if (std::find(item.role.stateOutputNames.begin(),
                    item.role.stateOutputNames.end(), tensor) !=
          item.role.stateOutputNames.end()) {
        throw std::logic_error(
          "planned dependency bundle explicitly names Provider-local decode state");
      }
    }
    auto bundle = withoutProviderLocalState(
      outputForEdge(result.outputsByScope, edge),
      item.role.stateOutputNames);
    if (item.role.generationLineage) {
      auto lineage = *item.role.generationLineage;
      if (lineage.requestId != item.role.requestId ||
          lineage.attemptEpoch != item.role.attemptEpoch ||
          lineage.inferenceEpoch != item.role.inferenceEpoch) {
        throw std::logic_error(
          "role generation lineage does not match the execution epoch");
      }
      if (edge.producerRole.empty() || edge.producerRole != item.role.role) {
        throw std::logic_error(
          "generation lineage output edge producer role is not the executing role: " +
          item.role.role + " operation=" + edge.operationKind +
          " scope=" + edge.scope + " producer=" + edge.producerRole +
          " consumer=" + edge.consumerRole);
      }
      lineage.producerRole = edge.producerRole;
      lineage.consumerRole = edge.consumerRole;
      lineage.operationIndex = edge.collectiveOperationIndex;
      bundle = attachGenerationEpochLineage(bundle, lineage);
    }
    validateTensorBundleForEdge(edge, bundle, false);
    stagedOutputs.emplace_back(edge, std::move(bundle));
  }
  result.outputTimings.reserve(item.role.outputs.size());
  for (auto& staged : stagedOutputs) {
    const auto& edge = staged.first;
#ifdef NDNSF_DI_EXPERIMENT_FAULTS
    NativeFaultInjection::instance().checkpoint(
      NativeFaultPoint::BeforePublish, item.role.role, item.sessionId);
#endif
    auto& bundle = staged.second;
    result.outputsByScope[edge.scope] = bundle;
    completeStageTransferObservation(edge, bundle, "send");
    result.outputsByScope[edge.scope].transferObservation = bundle.transferObservation;
    // A final-token state-only pass must carry the real activation to the
    // next role, but only after the coordinator commits this role's state.
    // The already validated, lineage-bound bundle is returned for that step.
    if (item.role.generationLineage &&
        item.role.generationLineage->transitionKind ==
          GenerationEpochLineageV1::CHECKPOINT_FINALIZE) {
      OutputPublishTiming timing;
      timing.producerRole = edge.producerRole;
      timing.scope = edge.scope;
      timing.plannedDataName = edge.plannedDataName;
      timing.plannedSegmentNames = plannedSegmentNamesForEdge(edge);
      timing.expectedSegments = edge.expectedSegments;
      timing.expectedBytes = edge.expectedBytes;
      timing.bytes = bundle.payload.size();
      timing.transferObservation = bundle.transferObservation;
      timing.outputReadyAt = outputReadyAt;
      timing.publishDoneAt = outputReadyAt;
      timing.publishDeferred = true;
      result.outputTimings.push_back(std::move(timing));
      continue;
    }
    logDiTimelineTrace(
      "di-provider", "dependency_publish_start", requestId,
      {{"sessionId", item.sessionId},
       {"role", item.role.role},
       {"scope", edge.scope},
       {"inferenceEpoch", std::to_string(item.role.inferenceEpoch)},
       {"providerBootId", item.role.candidateDecodeStateIdentity ?
                            item.role.candidateDecodeStateIdentity->providerBootId : "none"},
       {"attemptEpoch", std::to_string(item.role.attemptEpoch)}});
    if (item.executionGuard) item.executionGuard();
    item.io->publishOutput(item.sessionId, edge, bundle);
    logDiTimelineTrace(
      "di-provider", "dependency_publish_done", requestId,
      {{"sessionId", item.sessionId},
       {"role", item.role.role},
       {"scope", edge.scope},
       {"inferenceEpoch", std::to_string(item.role.inferenceEpoch)},
       {"providerBootId", item.role.candidateDecodeStateIdentity ?
                            item.role.candidateDecodeStateIdentity->providerBootId : "none"},
       {"attemptEpoch", std::to_string(item.role.attemptEpoch)}});

    OutputPublishTiming timing;
    timing.producerRole = edge.producerRole;
    timing.scope = edge.scope;
    timing.plannedDataName = edge.plannedDataName;
    timing.plannedSegmentNames = plannedSegmentNamesForEdge(edge);
    timing.expectedSegments = edge.expectedSegments;
    timing.expectedBytes = edge.expectedBytes;
    timing.bytes = bundle.payload.size();
    timing.transferObservation = bundle.transferObservation;
    timing.outputReadyAt = outputReadyAt;
    timing.publishDoneAt = std::chrono::steady_clock::now();
    result.outputTimings.push_back(std::move(timing));
  }

  result.timing.finishedAt = std::chrono::steady_clock::now();
  if (item.collective.has_value()) {
    const auto& binding = *item.collective;
    std::lock_guard<std::mutex> collectiveLock(m_collectiveMutex);
    if (!binding.runtime->completeRank(binding.rank,
                                       binding.runtime->groupEpoch(),
                                       binding.inputSequence,
                                       collectiveNowMs())) {
      throw std::runtime_error(
        "collective rank completion rejected: " +
        binding.runtime->snapshot().lastError);
    }
    logDiTimelineTrace(
      "di-provider", "collective_rank_complete",
      requestId,
      {{"sessionId", item.sessionId},
       {"role", item.role.role},
       {"rank", binding.rank},
       {"attemptEpoch", std::to_string(item.role.attemptEpoch)}});
  }
  if (item.executionGuard) item.executionGuard();
  for (const auto& timing : result.inputTimings) {
    if (timing.transferObservation) {
      result.transferBudget.add(*timing.transferObservation);
    }
  }
  for (const auto& timing : result.outputTimings) {
    if (timing.transferObservation && !timing.publishDeferred) {
      result.transferBudget.add(*timing.transferObservation);
    }
  }
  return result;
}

TensorBundle
ProviderRoleWorker::outputForEdge(const std::map<std::string, TensorBundle>& outputsByScope,
                                  const DependencyEdge& edge)
{
  const auto found = outputsByScope.find(edge.scope);
  const auto& bundleNames = edge.bundleTensorNames.empty()
    ? edge.tensors : edge.bundleTensorNames;
  if (found != outputsByScope.end()) {
    if (!bundleNames.empty() && isEncodedTensorBundle(found->second.payload)) {
      return selectTensorBundle(edge.scope, found->second, bundleNames);
    }
    TensorBundle bundle = found->second;
    bundle.name = edge.scope;
    return bundle;
  }

  if (bundleNames.empty() && outputsByScope.size() == 1) {
    return selectTensorBundle(edge.scope, outputsByScope.begin()->second, bundleNames);
  }

  if (!bundleNames.empty()) {
    if (bundleNames.size() == 1) {
      const auto tensorOutput = outputsByScope.find(bundleNames.front());
      if (tensorOutput != outputsByScope.end()) {
        TensorBundle bundle = tensorOutput->second;
        bundle.name = bundleNames.front();
        return bundle;
      }
    }
    for (const auto& item : outputsByScope) {
      if (isEncodedTensorBundle(item.second.payload)) {
        try {
          return selectTensorBundle(edge.scope, item.second, bundleNames);
        }
        catch (const std::out_of_range&) {
        }
      }
    }
  }

  throw std::logic_error("runner did not publish output scope: " + edge.scope);
}

} // namespace ndnsf::di
