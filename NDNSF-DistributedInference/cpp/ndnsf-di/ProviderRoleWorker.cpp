#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/DiTimelineTrace.hpp"
#include "request-work-statistics.hpp"
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
                           const GenerationEpochLineageV1& lineage)
{
  observation.lineagePresent = true;
  observation.positionDigest = lineage.positionDigest;
  observation.lineageIdentity = lineage.requestId + "|" +
    lineage.generationId + "|" + std::to_string(lineage.streamEpoch) +
    "|" + std::to_string(lineage.inferenceEpoch) + "|" +
    lineage.transitionKind + "|" + lineage.positionDigest + "|" +
    lineage.producerRole + "|" + lineage.consumerRole + "|" +
    std::to_string(lineage.operationIndex);
}

void
populatePayloadAccounting(StageTransferObservation& observation,
                          const TensorBundle& bundle)
{
  observation.tensorNames.clear();
  if (isEncodedTensorBundle(bundle.payload)) {
    // Count bytes and names from the same validated parse, avoiding another
    // allocation and copy of every activation just for accounting.
    const auto tensors = decodeTensorBundleViews(bundle.payload);
    observation.tensorBytes = 0;
    for (const auto& tensor : tensors) {
      if (tensor.name != generationEpochLineageTensorName()) {
        observation.tensorBytes += tensor.payload.size();
        observation.tensorNames.push_back(tensor.name);
      }
    }
  }
  else {
    observation.tensorBytes = bundle.payload.size();
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
  if (const auto lineage = extractGenerationEpochLineage(bundle))
    populateLineageObservation(*observation, *lineage);
  if (!observation->lineageIdentity.empty()) {
    observation->identity += "|" + observation->lineageIdentity;
  }
  populatePayloadAccounting(*observation, bundle);
  observation->encodedPayloadBytes = bundle.payload.size();
  // This is the observable bundle handoff copy, not a claim about allocator
  // internals. Unknown transport fields remain unset until Core reports them.
  observation->localCopyBytes = bundle.payload.size();
  return observation;
}

void
completeStageTransferObservationImpl(const DependencyEdge& edge,
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
    if (const auto lineage = extractGenerationEpochLineage(bundle)) {
      populateLineageObservation(observation, *lineage);
    }
    if (!observation.lineageIdentity.empty()) {
      observation.identity += "|" + observation.lineageIdentity;
    }
    populatePayloadAccounting(observation, bundle);
    observation.encodedPayloadBytes = bundle.payload.size();
    if (!observation.localCopyBytes) {
      observation.localCopyBytes = bundle.payload.size();
    }
  }
  validateStageTransferObservation(*bundle.transferObservation);
}

TensorBundle
withoutProviderLocalState(const TensorBundle& bundle,
                          const std::vector<std::string>& stateOutputNames,
                          const std::shared_ptr<NativeWorkStatistics>& workStatistics)
{
  if (stateOutputNames.empty() || !isEncodedTensorBundle(bundle.payload)) {
    return bundle;
  }
  // Resident runners already keep KV outputs outside the activation bundle.
  // Validate its framing without copying/re-encoding every tensor in that case.
  const auto views = decodeTensorBundleViews(bundle.payload, workStatistics);
  if (std::none_of(views.begin(), views.end(), [&] (const auto& tensor) {
        return std::find(stateOutputNames.begin(), stateOutputNames.end(),
                         tensor.name) != stateOutputNames.end();
      })) {
    return bundle;
  }
  auto tensors = decodeTensorBundle(bundle.payload, workStatistics);
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
  NativeWorkScope stateWork(role.workStatistics, "role-decode-state-pack",
                            NativeWorkKind::LOCAL);
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
          decodeTensorBundle(output.second.payload, role.workStatistics),
          role.stateOutputNames[index]);
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

/** Reject a sealed step template before any input fetch, runner preparation or
 * cached output is touched.
 *
 * The gate has to run first, not beside the execution branch: a step-carrying
 * role must not fetch peer partials or prepare a runner for a computation this
 * Provider cannot perform, and it must never be answered from the whole-role
 * output cache, whose key does not include the step template.
 *
 * The two failures stay distinguishable. An unsupported plan version is a
 * contract error the operator must fix; a supported version that this build
 * cannot execute is an implementation gap.
 */
void
requireConsistentStepTemplate(const RoleSpec& role)
{
  // A pointer is not a complete contract: an empty template, a version without a
  // template, or a degree above one without a template are all inconsistent and
  // must be refused before any work starts.
  const bool hasTemplate = role.steps != nullptr && !role.steps->empty();
  if (role.stepPlanVersion.empty() && !hasTemplate && role.tensorDegree <= 1) return;
  if (role.stepPlanVersion != NATIVE_STEP_PLAN_VERSION_V1)
    throw std::invalid_argument(
      "Provider role carries an unsupported step plan version");
  if (!hasTemplate)
    throw std::invalid_argument(
      "Provider role declares a step plan without a step template");
  if (role.tensorDegree < 2)
    throw std::invalid_argument(
      "Provider role step template requires a tensor degree of at least two");
  for (const auto& step : *role.steps) {
    if (step.unitId != role.role)
      throw std::invalid_argument(
        "Provider role step template contains another unit's step");
  }
}

} // namespace

void
completeStageTransferObservation(const DependencyEdge& edge,
                                 TensorBundle& bundle,
                                 const std::string& direction)
{
  completeStageTransferObservationImpl(edge, bundle, direction);
}

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

std::future<std::shared_ptr<NativeModelRunner>>
ProviderRoleWorker::prepareRunnerAsync(
  NativeRunnerPreparation prepareRunner,
  std::function<void()> executionGuard)
{
  if (!prepareRunner) {
    throw std::invalid_argument(
      "ProviderRoleWorker requires a runner preparation callback");
  }
  auto promise = std::make_shared<
    std::promise<std::shared_ptr<NativeModelRunner>>>();
  auto future = promise->get_future();
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping) {
      promise->set_exception(std::make_exception_ptr(std::logic_error(
        "ProviderRoleWorker is stopping")));
      return future;
    }
    if (m_queue.size() + m_preparationQueue.size() >= m_readyQueueCapacity) {
      promise->set_exception(std::make_exception_ptr(std::runtime_error(
        "ProviderRoleWorker preparation queue is full")));
      return future;
    }
    m_preparationQueue.push_back(PreparationItem{
      std::move(prepareRunner), std::move(promise), std::move(executionGuard)});
  }
  m_cv.notify_one();
  return future;
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
  // Before any fetch, preparation or cache lookup: a step-carrying role must
  // declare a complete template. The step path now exists, so the gate rejects
  // only an inconsistent declaration rather than the whole capability.
  requireConsistentStepTemplate(role);
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
        timing.preSatisfied = true;
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
      // A step-carrying unit must not sign an Interest for a peer partial before the step
      // that consumes it runs. Deferring only the wait is not enough: the Interest is
      // issued when the prefetch is submitted, so two ranks each request a partial the
      // other has not computed yet and neither ever starts. The step loop starts this
      // fetch itself, at the moment it needs the value.
      if (item.role.steps && !item.role.steps->empty() && !edge.consumerStepId.empty()) {
        item.deferredInputsByScope.emplace(edge.scope, edge);
        item.inputTimings.push_back(std::move(timing));
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

  // A step-carrying unit must not wait for every input before it starts. Its peer
  // produces the partials for later steps only after running its earlier ones, so
  // a wait-all gate makes both ranks wait for something neither can produce yet.
  // The prefetches stay in flight and each step awaits only its own slots.
  if (!pendingInputs.empty() && item.role.steps && !item.role.steps->empty()) {
    for (auto& pending : pendingInputs) {
      item.pendingInputsByScope.emplace(
        pending.edge.scope,
        std::shared_future<TensorBundle>(std::move(pending.future)));
      item.inputTimings.push_back(std::move(pending.timing));
    }
    enqueueReady(std::move(item));
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
      NativeWorkScope admissionWork(state->item.role.workStatistics,
                                    "role-dependency-admission",
                                    NativeWorkKind::LOCAL);
      try {
        for (auto& pending : state->pendingInputs) {
          {
            NativeWorkScope inputWait(state->item.role.workStatistics,
                                      "role-dependency-fetch-wait",
                                      NativeWorkKind::WAIT);
            while (pending.future.wait_for(std::chrono::milliseconds(2)) !=
                   std::future_status::ready) {
              if (control.isCancelled()) {
                return DependencyWaitStatus::Cancelled;
              }
              if (control.deadlineExpired()) {
                return DependencyWaitStatus::DeadlineExpired;
              }
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
  if (const auto* timing = std::getenv("NDNSF_DI_RUNTIME_TIMING");
      timing != nullptr && *timing != '\0' && *timing != '0') {
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
  // Preserve the public meaning of readyQueueDepth: preparation work is
  // bounded by the same capacity but is not a role admitted to execution.
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
    std::optional<WorkItem> item;
    std::optional<PreparationItem> preparation;
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_cv.wait(lock, [&] {
        return m_stopping || !m_queue.empty() || !m_preparationQueue.empty();
      });
      if (m_stopping && m_queue.empty() && m_preparationQueue.empty()) {
        return;
      }
      if (!m_preparationQueue.empty()) {
        preparation.emplace(std::move(m_preparationQueue.front()));
        m_preparationQueue.pop_front();
      }
      else {
        item.emplace(std::move(m_queue.front()));
        m_queue.pop_front();
        ++m_activeWorkers;
      }
    }
    if (preparation) {
      executePreparation(*preparation);
      continue;
    }
    execute(*item);
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_activeWorkers > 0) {
        --m_activeWorkers;
      }
    }
  }
}

void
ProviderRoleWorker::executePreparation(const PreparationItem& item)
{
  try {
    if (item.executionGuard) {
      item.executionGuard();
    }
    auto runner = item.prepareRunner();
    if (!runner) {
      throw std::runtime_error(
        "Provider runner preparation returned no NativeModelRunner");
    }
    if (item.executionGuard) {
      item.executionGuard();
    }
    item.promise->set_value(std::move(runner));
  }
  catch (...) {
    try {
      item.promise->set_exception(std::current_exception());
    }
    catch (const std::future_error&) {
    }
  }
}

void
ProviderRoleWorker::execute(WorkItem& item)
{
  std::atomic<bool> watchdogDone{false};
  std::thread watchdog;
  if (item.collective.has_value()) {
    const auto runtime = item.collective->runtime;
    const auto workStatistics = item.role.workStatistics;
    const auto tickMs = std::max<std::uint64_t>(
      1, runtime->snapshot().schedulerTickMs);
    watchdog = std::thread([this, runtime, tickMs, workStatistics,
                            &watchdogDone] {
      NativeWorkScope watchdogWork(workStatistics,
                                   "role-collective-watchdog",
                                   NativeWorkKind::LOCAL);
      while (!watchdogDone.load(std::memory_order_acquire)) {
        {
          NativeWorkScope tickWait(workStatistics,
                                   "role-collective-watchdog-tick",
                                   NativeWorkKind::WAIT);
          std::this_thread::sleep_for(std::chrono::milliseconds(tickMs));
        }
        if (watchdogDone.load(std::memory_order_acquire)) {
          break;
        }
        {
          NativeWorkScope lockWait(workStatistics,
                                   "role-collective-watchdog-lock",
                                   NativeWorkKind::WAIT);
          std::lock_guard<std::mutex> collectiveLock(m_collectiveMutex);
          lockWait.finish();
          NativeWorkScope pollWork(workStatistics,
                                   "role-collective-watchdog-poll",
                                   NativeWorkKind::UNCLASSIFIED);
          runtime->poll(collectiveNowMs());
        }
      }
    });
  }
  try {
    // `item` is local to this execution, so recording which scopes were published
    // early cannot leak into a retry of the same role.
    auto result = runReadyRole(item);
    watchdogDone.store(true, std::memory_order_release);
    if (watchdog.joinable()) {
      NativeWorkScope watchdogJoin(item.role.workStatistics,
                                   "role-collective-watchdog-join",
                                   NativeWorkKind::WAIT);
      watchdog.join();
      watchdogJoin.finish();
    }
    item.promise->set_value(std::move(result));
  }
  catch (...) {
    watchdogDone.store(true, std::memory_order_release);
    if (watchdog.joinable()) {
      NativeWorkScope watchdogJoin(item.role.workStatistics,
                                   "role-collective-watchdog-join",
                                   NativeWorkKind::WAIT);
      watchdog.join();
      watchdogJoin.finish();
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
ProviderRoleWorker::runSealedSteps(const RoleExecutionContext& ctx, WorkItem& item)
{
  if (!item.role.steps || item.role.steps->empty()) {
    throw std::invalid_argument("runSealedSteps requires a sealed step template");
  }
  if (!item.runner) {
    throw std::invalid_argument("runSealedSteps requires a prepared runner");
  }
  // One token, one unit, one template. Ordering follows the template, which the
  // plan validated as a DAG, so a single pass in template order reaches a
  // fixpoint: every producer is visited before its consumers.
  std::map<std::string, TensorBundle> produced;
  // State successors collected from the adapter, keyed by their declared names.
  std::map<std::string, TensorBundle> stateScopes;
  ProviderRoleResult result;
  result.runner = item.runner;

  const auto& steps = *item.role.steps;
  for (const auto& step : steps) {
    if (step.unitId != item.role.role) {
      throw std::invalid_argument(
        "sealed step belongs to another unit than the executing role");
    }

    // Resolve this step's inputs from the values already produced in this unit.
    // Every remote slot arrives here through the admitted role inputs, which the
    // worker has already validated; matching is by the sealed producer step and
    // slot, never by tensor name.
    std::map<std::string, TensorBundle> inputs;
    for (const auto& slot : step.inputSlots) {
      // A LOCAL_STEP is produced by an earlier step of this same unit. A
      // REMOTE_STEP names a producer in the peer unit, which this worker never
      // executes, so it is resolved from the admitted inputs below instead.
      if (slot.producerStepId.empty() || slot.source == "REMOTE_STEP") continue;
      const auto key = slot.producerStepId + "\n" + slot.producerSlotId;
      const auto found = produced.find(key);
      if (found == produced.end()) {
        throw std::runtime_error(
          "sealed step input was never produced: " + slot.slotId);
      }
      inputs.emplace(slot.slotId, found->second);
    }
    // Remote partials are awaited per step, not up front: a step runs as soon as
    // its own slots are present, which is what lets the peer produce later
    // partials. Awaiting the whole role's inputs first is the deadlock this path
    // exists to remove.
    for (const auto& edge : item.role.inputs) {
      if (edge.consumerStepId != step.stepId) continue;
      auto admitted = ctx.inputsByScope.find(edge.scope);
      if (admitted == ctx.inputsByScope.end()) {
        // A deferred edge is started here, on the step that needs it. This is the only
        // place the Interest is signed for a peer partial, so the producing rank has
        // already had its chance to run the step that publishes it.
        if (item.deferredInputsByScope.erase(edge.scope) != 0) {
          auto started = item.io->prefetchInput(item.sessionId, edge);
          const auto inserted = item.pendingInputsByScope.emplace(
            edge.scope, std::shared_future<TensorBundle>(std::move(started)));
          if (!inserted.second) {
            throw std::runtime_error(
              "sealed step remote input started twice: " + edge.scope);
          }
        }
        const auto pending = item.pendingInputsByScope.find(edge.scope);
        if (pending == item.pendingInputsByScope.end()) {
          throw std::runtime_error(
            "sealed step remote input was never started: " + edge.scope);
        }
        // Wait only for this step's own contribution, and never without a bound:
        // a peer that never publishes still has to end the unit rather than hold
        // a compute worker hostage. The deadline is the whole unit's, measured
        // from admission, so a template cannot buy more time by having more steps.
        const auto deadline = item.queuedAt + m_dependencyWaitTimeout;
        // Poll rather than block once, so a destroyed worker releases its compute
        // thread promptly. Without this the destructor's cancellation would not be
        // observed here and the unit would only end at its deadline.
        while (true) {
          {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping) {
              throw std::runtime_error(
                "dependency wait failed: CANCELLED for " + edge.scope);
            }
          }
          if (pending->second.wait_for(std::chrono::milliseconds(2)) ==
              std::future_status::ready) {
            break;
          }
          if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(
              "dependency wait failed: DEADLINE_EXPIRED for " + edge.scope);
          }
        }
        auto bundle = pending->second.get();
        validateTensorBundleForEdge(edge, bundle);
        // The admitted map belongs to the caller's context, which is const here;
        // the value is bound directly below instead.
        if (!inputs.emplace(edge.consumerSlotId, std::move(bundle)).second) {
          throw std::runtime_error(
            "sealed step has duplicate contributions for one slot: " +
            edge.consumerSlotId);
        }
        continue;
      }
      // Two edges feeding one slot would let a contribution be counted twice,
      // which inflates the reduction without looking wrong. The sealed template
      // declares one dependency per contribution, so a second one is a contract
      // violation rather than something to merge.
      if (!inputs.emplace(edge.consumerSlotId, admitted->second).second) {
        throw std::runtime_error(
          "sealed step has duplicate contributions for one slot: " +
          edge.consumerSlotId);
      }
    }
    // EPOCH_INPUT slots are the request's own inputs, already admitted by scope.
    for (const auto& slot : step.inputSlots) {
      if (slot.source != "EPOCH_INPUT") continue;
      const auto admitted = ctx.inputsByScope.find(slot.slotId);
      if (admitted != ctx.inputsByScope.end())
        inputs.emplace(slot.slotId, admitted->second);
    }

    if (step.operation == "SUM") {
      // A SUM is admitted as a whole: every contribution it declares must already
      // be present, because a reduction over a subset would silently produce a
      // different tensor. This is the worker's admission duty, which is why the
      // arithmetic function deliberately keeps no admission state of its own.
      for (const auto& slot : step.inputSlots) {
        if (inputs.find(slot.slotId) == inputs.end()) {
          throw std::runtime_error(
            "sealed SUM step is missing an admitted contribution: " + slot.slotId);
        }
      }
      // Fixed rank order, not arrival order: C5 requires the same sum regardless
      // of which partial arrived first. The sealed template lists the local slot
      // first and the peers in rank order, so following it is deterministic.
      std::map<std::string, TensorBundle> accumulator;
      for (const auto& slot : step.inputSlots) {
        const auto input = inputs.find(slot.slotId);
        accumulateDecodedTensorBundle(accumulator, step.outputSlots.front().slotId,
          decodeTensorBundle(input->second.payload), input->second.name);
      }
      const auto merged = accumulator.find(step.outputSlots.front().slotId);
      if (merged == accumulator.end()) {
        throw std::runtime_error("sealed SUM step produced no value");
      }
      produced.emplace(step.stepId + "\n" + step.outputSlots.front().slotId,
                       merged->second);
      continue;
    }

    if (step.operation != "CALL_SUBGRAPH") {
      throw std::invalid_argument("sealed step has an unsupported operation");
    }

    // One runner call for exactly this step. The context names the step and its
    // slot values, so the adapter resolves its entry from the sealed table rather
    // than deciding anything about ordering.
    RoleExecutionContext stepContext = ctx;
    stepContext.activeStep =
      std::make_shared<const NativeExecutionStepSpec>(step);
    stepContext.roleSteps = item.role.steps;
    stepContext.stepInputsBySlot = inputs;
    stepContext.streamEventSink = {};
    if (item.executionGuard) item.executionGuard();
    const auto outputs = item.runner->run(stepContext);
    if (item.executionGuard) item.executionGuard();

    // A step's output is keyed by its own declared slot, so the next step can
    // bind it without knowing the adapter's internal naming.
    for (const auto& slot : step.outputSlots) {
      const auto found = outputs.find(slot.slotId);
      if (found == outputs.end()) {
        // Name the step and everything the adapter did return. Without them the message
        // points at a slot id with no way to tell which entry lost the binding.
        std::string returned;
        for (const auto& item : outputs) {
          if (!returned.empty()) returned += ',';
          returned += item.first;
        }
        throw std::runtime_error(
          "sealed step did not produce its declared output: " + slot.slotId +
          " step=" + step.stepId + " returned={" + returned + "}");
      }
      produced.emplace(step.stepId + "\n" + slot.slotId, found->second);
    }
    // Keep the decode-state successors the adapter returned. They are not step slots
    // -- the runtime packs state from the result by tensor name -- so dropping them
    // here would make every epoch report a missing state output even though the
    // adapter produced it. Only declared state names are kept, so an adapter cannot
    // smuggle an undeclared scope into the result.
    for (const auto& name : item.role.stateOutputNames) {
      const auto found = outputs.find(name);
      if (found == outputs.end()) continue;
      stateScopes.emplace(name, found->second);
    }

    // Publish this step's remote partial now, not after the unit finishes. A peer
    // waiting for this contribution would otherwise wait for work that itself
    // waits on the peer. The scope is recorded so the final pass does not publish
    // the same object twice.
    for (const auto& edge : item.role.outputs) {
      if (edge.producerStepId != step.stepId) continue;
      const auto key = step.stepId + "\n" + edge.producerSlotId;
      const auto value = produced.find(key);
      if (value == produced.end()) {
        throw std::runtime_error(
          "sealed step output edge has no produced value: " + edge.scope);
      }
      auto bundle = value->second;
      // The same stripping the final pass applies: Provider-local state never
      // travels, and a peer must not receive a bundle that would be rejected as a
      // dependency for naming state.
      for (const auto& tensor : edge.tensors) {
        if (std::find(item.role.stateOutputNames.begin(),
                      item.role.stateOutputNames.end(), tensor) !=
            item.role.stateOutputNames.end()) {
          throw std::logic_error(
            "planned dependency explicitly names Provider-local decode state");
        }
      }
      auto stripped = withoutProviderLocalState(bundle, item.role.stateOutputNames,
                                                item.role.workStatistics);
      validateTensorBundleForEdge(edge, stripped, false);
      // Charge the transfer where it actually happens. The final pass skips this
      // edge, so leaving it out here would make the timing ledger claim the partial
      // was never sent.
      completeStageTransferObservation(edge, stripped, "send");
      if (item.executionGuard) item.executionGuard();
      item.io->publishOutput(item.sessionId, edge, stripped);
      item.publishedScopes.insert(edge.scope);
      if (std::getenv("NDNSF_DI_RUNTIME_TIMING") != nullptr) {
        logRuntimeTrace("NDNSF_DI_WORKER event=step_partial_published role=" +
                        item.role.role + " scope=" + edge.scope);
      }
    }
  }

  // The unit is complete only now, so its declared role outputs are packed from
  // the terminal steps. Every output edge names the step that produced it. An edge
  // whose step already published it is returned but not sent again: republishing
  // the same name and content would be idempotent for a reader and wasteful for
  // the transport, and a second send after a peer consumed the first is exactly
  // the duplicate the contract asks to ignore rather than repeat.
  for (const auto& edge : item.role.outputs) {
    if (item.publishedScopes.count(edge.scope) != 0) {
      const auto key = edge.producerStepId + "\n" + edge.producerSlotId;
      const auto found = produced.find(key);
      if (found == produced.end()) {
        throw std::runtime_error(
          "sealed unit did not produce a declared output edge: " + edge.scope);
      }
      result.outputsByScope.emplace(edge.scope, found->second);
      continue;
    }
    const auto key = edge.producerStepId + "\n" + edge.producerSlotId;
    const auto found = produced.find(key);
    if (found == produced.end()) {
      throw std::runtime_error(
        "sealed unit did not produce a declared output edge: " + edge.scope);
    }
    result.outputsByScope.emplace(edge.scope, found->second);
  }

  // Decode-state successors travel under their own names, before the terminal-value
  // exposure, so the runtime finds them by tensor name.
  for (auto& item : stateScopes)
    result.outputsByScope.emplace(item.first, std::move(item.second));

  // Surface terminal step values that no edge carries. A sharded unit's head
  // produces logits that no peer consumes -- the sampler reads them -- and relying
  // on an output edge would require inventing a consumer that does not exist. Only
  // values with no consumer inside this unit are surfaced, so intermediate
  // activations stay out of the result.
  std::set<std::string> consumedKeys;
  for (const auto& step : steps) {
    for (const auto& slot : step.inputSlots) {
      if (slot.producerStepId.empty()) continue;
      consumedKeys.insert(slot.producerStepId + "\n" + slot.producerSlotId);
    }
  }
  std::set<std::string> edgeKeys;
  for (const auto& edge : item.role.outputs)
    edgeKeys.insert(edge.producerStepId + "\n" + edge.producerSlotId);
  for (const auto& step : steps) {
    for (const auto& slot : step.outputSlots) {
      const auto key = step.stepId + "\n" + slot.slotId;
      if (consumedKeys.count(key) != 0) continue;
      if (edgeKeys.count(key) != 0) continue;
      const auto found = produced.find(key);
      if (found == produced.end()) continue;
      // Keyed by slot id so a reader finds it by the name the template declared.
      result.outputsByScope.emplace(slot.slotId, found->second);
    }
  }
  return result;
}

ProviderRoleResult
ProviderRoleWorker::runReadyRole(WorkItem& item)
{
  NativeWorkScope roleWork(item.role.workStatistics, "role-worker",
                          NativeWorkKind::UNCLASSIFIED, item.role.inferenceEpoch);
  if (item.executionGuard) item.executionGuard();
  if (const auto* timing = std::getenv("NDNSF_DI_RUNTIME_TIMING");
      timing != nullptr && *timing != '\0' && *timing != '0') {
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
    // Record the prepared runner on the item as well. Every later consumer reads
    // `item.runner` -- the sealed-step path among them -- so leaving the prepared
    // value only in this local would make a lazily prepared step-carrying role fail
    // with "requires a prepared runner" even though preparation succeeded.
    item.runner = runner;
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

  NativeWorkScope inputWork(item.role.workStatistics, "role-input-bind", NativeWorkKind::LOCAL);
  RoleExecutionContext ctx;
  ctx.workStatistics = item.role.workStatistics;
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
  // A step-carrying unit admits each remote input when the step that needs it
  // runs, so requiring every declared input here would reintroduce the wait-all
  // gate this path exists to remove. Each awaited bundle is validated at that
  // point instead, so an unvalidated input still cannot reach a step.
  const bool stepsAwaitTheirOwnInputs =
    item.role.steps && !item.role.steps->empty();
  for (const auto& edge : item.role.inputs) {
    const auto input = ctx.inputsByScope.find(edge.scope);
    if (input == ctx.inputsByScope.end()) {
      if (stepsAwaitTheirOwnInputs) continue;
      throw std::runtime_error(
        "Provider role is missing a declared input: " + edge.scope);
    }
    validateTensorBundleForEdge(edge, input->second);
    ctx.inputEdgesByScope.emplace(edge.scope, edge);
  }
  if (std::getenv("NDNSF_DI_RUNTIME_TIMING") != nullptr) {
    NativeWorkScope traceWork(item.role.workStatistics, "role-input-trace",
                              NativeWorkKind::UNCLASSIFIED);
    logRuntimeTrace("NDNSF_DI_WORKER event=inputs_validated role=" + item.role.role);
  }

  result.exactForwardCacheKey = exactForwardCacheKeyFor(
    item, runner.get(), inputsByScope);
  if (std::getenv("NDNSF_DI_RUNTIME_TIMING") != nullptr) {
    NativeWorkScope traceWork(item.role.workStatistics, "role-input-trace",
                              NativeWorkKind::UNCLASSIFIED);
    logRuntimeTrace("NDNSF_DI_WORKER event=cache_key role=" + item.role.role);
  }
  // A streamed terminal runner has an externally visible side effect: it
  // submits token/event payloads through the sink. Reusing only its Tensor
  // outputs would suppress those events on a retry and could turn a complete
  // transcript into the legacy single-final-event fallback. Keep this cache
  // limited to pure dependency-producing executions.
  if (item.executionGuard) {
    NativeWorkScope guardWork(item.role.workStatistics, "role-input-authorization",
                              NativeWorkKind::UNCLASSIFIED);
    item.executionGuard();
  }
  const bool hasStreamSideEffect = static_cast<bool>(item.eventSink);
  // A generation epoch may update adapter-owned KV or depend on lineage not
  // represented in the pure-output cache key, even with equal tensor bytes.
  // Reusing a loaded runner must not suppress that state transition.
  const bool canReuseOutputs = !hasStreamSideEffect &&
    !item.role.streamingStateExecution && !item.role.generationLineage;
  inputWork.finish();
  if (item.role.workStatistics) {
    const auto admitted = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
    for (const auto& timing : result.inputTimings) {
      if (timing.transferObservation) {
        // A coordinator-supplied bundle has already completed its fetch.
        // These worker timestamps describe consumption, not that earlier I/O.
        item.role.workStatistics->observeTransfer(*timing.transferObservation,
          item.role.inferenceEpoch,
          timing.preSatisfied ? 0 : std::chrono::duration_cast<std::chrono::nanoseconds>(
            timing.prefetchStartedAt.time_since_epoch()).count(),
          timing.preSatisfied ? 0 : std::chrono::duration_cast<std::chrono::nanoseconds>(
            timing.fetchCompletedAt.time_since_epoch()).count(), admitted,
          NativeTransferPoint::WORKER_INPUT);
      }
    }
  }
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
    NativeWorkScope adapterWork(item.role.workStatistics, "role-adapter",
                               NativeWorkKind::UNCLASSIFIED);
    // A sealed tensor-parallel unit runs its template instead of making one
    // whole-role call. The template is validated and its steps belong to this
    // role, so the runner is invoked once per step rather than once per unit.
    const bool sealedSteps = item.role.steps && !item.role.steps->empty();
    if (sealedSteps) {
      auto stepped = runSealedSteps(ctx, item);
      result.outputsByScope = std::move(stepped.outputsByScope);
      if (std::getenv("NDNSF_DI_RUNTIME_TIMING") != nullptr) {
        logRuntimeTrace("NDNSF_DI_WORKER event=runner_steps role=" + item.role.role);
      }
    }
    else if (hasStreamSideEffect && !coordinatorOwnsStreaming) {
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
    if (item.role.generationLineage) {
      result.executionEvidence->planDigest = item.role.generationLineage->planDigest;
    }
    bindExecutionObservation(*result.executionEvidence,
                             item.role.requestId, item.role.attemptEpoch,
                             result.exactForwardCacheHit);
  }
  result.runtimeMetrics = runner->runtimeMetricsSnapshot();
  if (result.runtimeMetrics && result.runtimeMetrics->lastBackendExecution &&
      (result.exactForwardCacheHit || !result.runtimeMetrics->lastBackendExecution->matches(ctx))) {
    // Another serialized caller may have advanced a shared runner's snapshot.
    // A cached result did not run the backend either. Never charge either call.
    result.runtimeMetrics->lastBackendExecution.reset();
  }
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
  NativeWorkScope outputWork(item.role.workStatistics, "role-output-pack", NativeWorkKind::LOCAL);
  std::vector<std::pair<DependencyEdge, TensorBundle>> stagedOutputs;
  stagedOutputs.reserve(item.role.outputs.size());
  for (const auto& edge : item.role.outputs) {
    // Already sent by the step that produced it. Skipping here is what keeps the
    // transport from receiving the same object twice, and it is also why the final
    // pass must not re-validate an edge whose bundle was already accepted.
    if (item.publishedScopes.count(edge.scope) != 0) continue;
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
      item.role.stateOutputNames, item.role.workStatistics);
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
  outputWork.finish();
  result.outputTimings.reserve(item.role.outputs.size());
  for (auto& staged : stagedOutputs) {
    const auto& edge = staged.first;
#ifdef NDNSF_DI_EXPERIMENT_FAULTS
    NativeFaultInjection::instance().checkpoint(
      NativeFaultPoint::BeforePublish, item.role.role, item.sessionId);
#endif
    NativeWorkScope stageWork(item.role.workStatistics, "role-output-stage", NativeWorkKind::LOCAL);
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
    stageWork.finish();
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
    const auto publishStartedAt = std::chrono::steady_clock::now();
    item.io->publishOutput(item.sessionId, edge, bundle);
    const auto publishDoneAt = std::chrono::steady_clock::now();
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
    timing.publishDoneAt = publishDoneAt;
    timing.publishStartedAt = publishStartedAt;
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
      if (item.role.workStatistics) {
        item.role.workStatistics->observeTransfer(*timing.transferObservation,
          item.role.inferenceEpoch,
          std::chrono::duration_cast<std::chrono::nanoseconds>(
            timing.publishStartedAt.time_since_epoch()).count(),
          std::chrono::duration_cast<std::chrono::nanoseconds>(
            timing.publishDoneAt.time_since_epoch()).count(), 0,
          NativeTransferPoint::OUTPUT_PUBLICATION,
          std::chrono::duration_cast<std::chrono::nanoseconds>(
            timing.outputReadyAt.time_since_epoch()).count());
      }
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
