#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <future>
#include <utility>

namespace ndnsf::di {

ProviderRoleWorker::ProviderRoleWorker(std::size_t workerCount)
{
  if (workerCount == 0) {
    workerCount = 1;
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
  for (auto& waiter : m_inputWaiters) {
    if (waiter.joinable()) {
      waiter.join();
    }
  }
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
  return executeAsync(std::move(sessionId),
                      std::move(role),
                      std::move(io),
                      makeNativeModelRunner(std::move(runner)),
                      std::move(initialInputsByScope));
}

std::future<ProviderRoleResult>
ProviderRoleWorker::executeAsync(std::string sessionId,
                                 RoleSpec role,
                                 std::shared_ptr<DependencyIo> io,
                                 std::shared_ptr<NativeModelRunner> runner,
                                 std::map<std::string, TensorBundle> initialInputsByScope)
{
  if (role.role.empty()) {
    throw std::invalid_argument("ProviderRoleWorker requires a non-empty role");
  }
  if (!io) {
    throw std::invalid_argument("ProviderRoleWorker requires DependencyIo");
  }
  if (!runner) {
    throw std::invalid_argument("ProviderRoleWorker requires NativeModelRunner");
  }

  auto promise = std::make_shared<std::promise<ProviderRoleResult>>();
  auto future = promise->get_future();
  WorkItem item{
    std::move(sessionId),
    std::move(role),
    std::move(io),
    std::move(runner),
    std::move(initialInputsByScope),
    {},
    promise,
    std::chrono::steady_clock::now(),
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
        pending.timing.fetchCompletedAt = std::chrono::steady_clock::now();
        pending.timing.bytes = bundle.payload.size();
        item.initialInputsByScope[pending.edge.scope] = std::move(bundle);
        item.inputTimings.push_back(std::move(pending.timing));
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
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping) {
      failPromise(item.promise,
                  std::make_exception_ptr(std::logic_error(
                    "ProviderRoleWorker is stopping")));
      return;
    }
    ++m_waitingForInputs;
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  m_inputWaiters.emplace_back(
    [this, item = std::move(item), pendingInputs = std::move(pendingInputs)] () mutable {
      try {
        for (auto& pending : pendingInputs) {
          auto bundle = pending.future.get();
          pending.timing.fetchCompletedAt = std::chrono::steady_clock::now();
          pending.timing.bytes = bundle.payload.size();
          item.initialInputsByScope[pending.edge.scope] = std::move(bundle);
          item.inputTimings.push_back(std::move(pending.timing));
        }
        enqueueReady(std::move(item));
      }
      catch (...) {
        failPromise(item.promise, std::current_exception());
      }
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_waitingForInputs > 0) {
        --m_waitingForInputs;
      }
    });
}

void
ProviderRoleWorker::enqueueReady(WorkItem item)
{
  item.queuedAt = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping) {
      failPromise(item.promise,
                  std::make_exception_ptr(std::logic_error(
                    "ProviderRoleWorker is stopping")));
      return;
    }
    m_queue.push_back(std::move(item));
  }
  m_cv.notify_one();
}

ProviderRoleWorkerSnapshot
ProviderRoleWorker::snapshot() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  ProviderRoleWorkerSnapshot snapshot;
  snapshot.workerCount = m_workers.size();
  snapshot.readyQueueDepth = m_queue.size();
  snapshot.waitingForInputCount = m_waitingForInputs;
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
  try {
    item.promise->set_value(runReadyRole(item));
  }
  catch (...) {
    item.promise->set_exception(std::current_exception());
  }
}

ProviderRoleResult
ProviderRoleWorker::runReadyRole(const WorkItem& item)
{
  const auto workerStartedAt = std::chrono::steady_clock::now();
  std::map<std::string, TensorBundle> inputsByScope = item.initialInputsByScope;

  ProviderRoleResult result;
  result.timing.role = item.role.role;
  result.timing.queuedAt = item.queuedAt;
  result.timing.workerStartedAt = workerStartedAt;
  result.timing.startedAt = std::chrono::steady_clock::now();
  result.inputTimings = item.inputTimings;

  RoleExecutionContext ctx;
  ctx.sessionId = item.sessionId;
  ctx.role = item.role.role;
  ctx.inputsByScope = std::move(inputsByScope);
  result.outputsByScope = item.runner->run(ctx);

  const auto outputReadyAt = std::chrono::steady_clock::now();
  result.outputTimings.reserve(item.role.outputs.size());
  for (const auto& edge : item.role.outputs) {
    auto bundle = outputForEdge(result.outputsByScope, edge);
    result.outputsByScope[edge.scope] = bundle;
    item.io->publishOutput(item.sessionId, edge, bundle);

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
