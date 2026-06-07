#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <exception>
#include <stdexcept>
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
    promise,
    std::chrono::steady_clock::now(),
  };
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping) {
      throw std::logic_error("ProviderRoleWorker is stopping");
    }
    m_queue.push_back(std::move(item));
  }
  m_cv.notify_one();
  return future;
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
    }
    execute(item);
  }
}

void
ProviderRoleWorker::execute(const WorkItem& item)
{
  try {
    item.promise->set_value(runRole(item));
  }
  catch (...) {
    item.promise->set_exception(std::current_exception());
  }
}

ProviderRoleResult
ProviderRoleWorker::runRole(const WorkItem& item)
{
  std::vector<std::future<TensorBundle>> futures;
  std::vector<InputFetchTiming> inputTimings;
  futures.reserve(item.role.inputs.size());
  inputTimings.reserve(item.role.inputs.size());

  for (const auto& edge : item.role.inputs) {
    InputFetchTiming timing;
    timing.producerRole = edge.producerRole;
    timing.scope = edge.scope;
    timing.plannedDataName = edge.plannedDataName;
    timing.expectedSegments = edge.expectedSegments;
    timing.expectedBytes = edge.expectedBytes;
    timing.prefetchStartedAt = std::chrono::steady_clock::now();
    futures.push_back(item.io->prefetchInput(item.sessionId, edge));
    inputTimings.push_back(timing);
  }

  std::map<std::string, TensorBundle> inputsByScope = item.initialInputsByScope;
  for (std::size_t i = 0; i < futures.size(); ++i) {
    auto bundle = futures[i].get();
    inputTimings[i].fetchCompletedAt = std::chrono::steady_clock::now();
    inputTimings[i].bytes = bundle.payload.size();
    inputsByScope[item.role.inputs[i].scope] = std::move(bundle);
  }

  ProviderRoleResult result;
  result.timing.role = item.role.role;
  result.timing.queuedAt = item.queuedAt;
  result.timing.startedAt = std::chrono::steady_clock::now();
  result.inputTimings = std::move(inputTimings);

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
