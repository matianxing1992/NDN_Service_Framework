#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/DiTimelineTrace.hpp"
#ifdef NDNSF_DI_EXPERIMENT_FAULTS
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeFaultInjection.hpp"
#endif

#include <algorithm>
#include <cstdint>
#include <exception>
#include <future>
#include <iomanip>
#include <iostream>
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
                                 std::map<std::string, TensorBundle> initialInputsByScope)
{
  return executeAsyncImpl(std::move(sessionId),
                          std::move(role),
                          std::move(io),
                          makeNativeModelRunner(std::move(runner)),
                          {},
                          std::move(initialInputsByScope),
                          std::nullopt);
}

std::future<ProviderRoleResult>
ProviderRoleWorker::executeAsync(std::string sessionId,
                                 RoleSpec role,
                                 std::shared_ptr<DependencyIo> io,
                                 std::shared_ptr<NativeModelRunner> runner,
                                 std::map<std::string, TensorBundle> initialInputsByScope)
{
  return executeAsyncImpl(std::move(sessionId),
                          std::move(role),
                          std::move(io),
                          std::move(runner),
                          {},
                          std::move(initialInputsByScope),
                          std::nullopt);
}

std::future<ProviderRoleResult>
ProviderRoleWorker::executePreparedAsync(
  std::string sessionId,
  RoleSpec role,
  std::shared_ptr<DependencyIo> io,
  NativeRunnerPreparation prepareRunner,
  std::map<std::string, TensorBundle> initialInputsByScope)
{
  return executeAsyncImpl(std::move(sessionId),
                          std::move(role),
                          std::move(io),
                          nullptr,
                          std::move(prepareRunner),
                          std::move(initialInputsByScope),
                          std::nullopt);
}

std::future<ProviderRoleResult>
ProviderRoleWorker::executeCollectiveAsync(
  std::string sessionId,
  RoleSpec role,
  std::shared_ptr<DependencyIo> io,
  RoleRunner runner,
  CollectiveExecutionBinding collective,
  std::map<std::string, TensorBundle> initialInputsByScope)
{
  return executeAsyncImpl(std::move(sessionId),
                          std::move(role),
                          std::move(io),
                          makeNativeModelRunner(std::move(runner)),
                          {},
                          std::move(initialInputsByScope),
                          std::move(collective));
}

std::future<ProviderRoleResult>
ProviderRoleWorker::executeCollectiveAsync(
  std::string sessionId,
  RoleSpec role,
  std::shared_ptr<DependencyIo> io,
  std::shared_ptr<NativeModelRunner> runner,
  CollectiveExecutionBinding collective,
  std::map<std::string, TensorBundle> initialInputsByScope)
{
  return executeAsyncImpl(std::move(sessionId),
                          std::move(role),
                          std::move(io),
                          std::move(runner),
                          {},
                          std::move(initialInputsByScope),
                          std::move(collective));
}

std::future<ProviderRoleResult>
ProviderRoleWorker::executeAsyncImpl(
  std::string sessionId,
  RoleSpec role,
  std::shared_ptr<DependencyIo> io,
  std::shared_ptr<NativeModelRunner> runner,
  NativeRunnerPreparation prepareRunner,
  std::map<std::string, TensorBundle> initialInputsByScope,
  std::optional<CollectiveExecutionBinding> collective)
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
    {},
    promise,
    std::chrono::steady_clock::now(),
    std::move(collective),
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
         {"attemptEpoch", std::to_string(item.role.attemptEpoch)}});
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
        item.initialInputsByScope[pending.edge.scope] = std::move(bundle);
        item.inputTimings.push_back(std::move(pending.timing));
        logDiTimelineTrace(
          "di-provider", "dependency_fetch_done",
          timelineRequestId(item.role.requestId, item.sessionId),
          {{"sessionId", item.sessionId},
           {"role", item.role.role},
           {"scope", pending.edge.scope},
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
          state->item.initialInputsByScope[pending.edge.scope] = std::move(bundle);
          state->item.inputTimings.push_back(std::move(pending.timing));
          logDiTimelineTrace(
            "di-provider", "dependency_fetch_done",
            timelineRequestId(state->item.role.requestId,
                              state->item.sessionId),
            {{"sessionId", state->item.sessionId},
             {"role", state->item.role.role},
             {"scope", pending.edge.scope},
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
      std::cout << "\nNDNSF_DI_PROVIDER_WAIT_TERMINAL"
                << " session=" << state->item.sessionId
                << " role=" << state->item.role.role
                << " status=" << toString(result.status)
                << " reason=" << reason << std::endl;
      failPromise(state->item.promise,
                  std::make_exception_ptr(std::runtime_error(
                    "dependency wait failed: " + reason)));
    });
  if (submitted != DependencyWaitSubmitResult::Accepted) {
    std::cout << "\nNDNSF_DI_PROVIDER_WAIT_ADMISSION"
              << " session=" << state->item.sessionId
              << " role=" << state->item.role.role
              << " status=" << toString(submitted) << std::endl;
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
  appendUint64(os, reinterpret_cast<std::uintptr_t>(runner));
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
       {"attemptEpoch", std::to_string(item.role.attemptEpoch)}});
  }

  ProviderRoleResult result;
  result.timing.role = item.role.role;
  result.timing.queuedAt = item.queuedAt;
  result.timing.workerStartedAt = workerStartedAt;
  result.timing.startedAt = std::chrono::steady_clock::now();
  result.inputTimings = item.inputTimings;

  RoleExecutionContext ctx;
  ctx.sessionId = item.sessionId;
  ctx.role = item.role.role;
  ctx.inputsByScope = inputsByScope;
  for (const auto& edge : item.role.inputs) {
    const auto input = ctx.inputsByScope.find(edge.scope);
    if (input == ctx.inputsByScope.end()) {
      throw std::runtime_error(
        "Provider role is missing a declared input: " + edge.scope);
    }
    validateTensorBundleForEdge(edge, input->second);
    ctx.inputEdgesByScope.emplace(edge.scope, edge);
  }

  result.exactForwardCacheKey = exactForwardCacheKeyFor(
    item, runner.get(), inputsByScope);
  result.outputsByScope = getCachedOutputs(result.exactForwardCacheKey);
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
       {"attemptEpoch", std::to_string(item.role.attemptEpoch)}});
    result.outputsByScope = runner->run(ctx);
    logDiTimelineTrace(
      "di-provider", "role_compute_done", requestId,
      {{"sessionId", item.sessionId},
       {"role", item.role.role},
       {"attemptEpoch", std::to_string(item.role.attemptEpoch)}});
    putCachedOutputs(result.exactForwardCacheKey, result.outputsByScope);
  }
  result.executionEvidence = runner->executionEvidenceSnapshot();

  const auto outputReadyAt = std::chrono::steady_clock::now();
  std::vector<std::pair<DependencyEdge, TensorBundle>> stagedOutputs;
  stagedOutputs.reserve(item.role.outputs.size());
  for (const auto& edge : item.role.outputs) {
    auto bundle = outputForEdge(result.outputsByScope, edge);
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
    logDiTimelineTrace(
      "di-provider", "dependency_publish_start", requestId,
      {{"sessionId", item.sessionId},
       {"role", item.role.role},
       {"scope", edge.scope},
       {"attemptEpoch", std::to_string(item.role.attemptEpoch)}});
    item.io->publishOutput(item.sessionId, edge, bundle);
    logDiTimelineTrace(
      "di-provider", "dependency_publish_done", requestId,
      {{"sessionId", item.sessionId},
       {"role", item.role.role},
       {"scope", edge.scope},
       {"attemptEpoch", std::to_string(item.role.attemptEpoch)}});

    OutputPublishTiming timing;
    timing.producerRole = edge.producerRole;
    timing.scope = edge.scope;
    timing.plannedDataName = edge.plannedDataName;
    timing.plannedSegmentNames = plannedSegmentNamesForEdge(edge);
    timing.expectedSegments = edge.expectedSegments;
    timing.expectedBytes = edge.expectedBytes;
    timing.bytes = bundle.payload.size();
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
  return result;
}

TensorBundle
ProviderRoleWorker::outputForEdge(const std::map<std::string, TensorBundle>& outputsByScope,
                                  const DependencyEdge& edge)
{
  const auto found = outputsByScope.find(edge.scope);
  if (found != outputsByScope.end()) {
    if (!edge.tensors.empty() && isEncodedTensorBundle(found->second.payload)) {
      return selectTensorBundle(edge.scope, found->second, edge.tensors);
    }
    TensorBundle bundle = found->second;
    bundle.name = edge.scope;
    return bundle;
  }

  if (edge.tensors.empty() && outputsByScope.size() == 1) {
    return selectTensorBundle(edge.scope, outputsByScope.begin()->second, edge.tensors);
  }

  if (!edge.tensors.empty()) {
    if (edge.tensors.size() == 1) {
      const auto tensorOutput = outputsByScope.find(edge.tensors.front());
      if (tensorOutput != outputsByScope.end()) {
        TensorBundle bundle = tensorOutput->second;
        bundle.name = edge.tensors.front();
        return bundle;
      }
    }
    for (const auto& item : outputsByScope) {
      if (isEncodedTensorBundle(item.second.payload)) {
        try {
          return selectTensorBundle(edge.scope, item.second, edge.tensors);
        }
        catch (const std::out_of_range&) {
        }
      }
    }
  }

  throw std::logic_error("runner did not publish output scope: " + edge.scope);
}

} // namespace ndnsf::di
