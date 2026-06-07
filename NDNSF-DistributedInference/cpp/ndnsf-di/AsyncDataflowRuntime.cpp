#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"

#include <stdexcept>
#include <utility>

namespace ndnsf::di {

DependencyEdge::DependencyEdge(std::string scope,
                               std::string producerRole,
                               std::string consumerRole,
                               std::string plannedDataName,
                               std::size_t expectedSegments,
                               std::size_t expectedBytes,
                               std::vector<std::string> tensors)
  : scope(std::move(scope))
  , producerRole(std::move(producerRole))
  , consumerRole(std::move(consumerRole))
  , plannedDataName(std::move(plannedDataName))
  , expectedSegments(expectedSegments)
  , expectedBytes(expectedBytes)
  , tensors(std::move(tensors))
{
}

AsyncDataflowRuntime::AsyncDataflowRuntime(std::size_t workerCount)
{
  if (workerCount == 0) {
    workerCount = 1;
  }
  m_workers.reserve(workerCount);
  for (std::size_t i = 0; i < workerCount; ++i) {
    m_workers.emplace_back([this] { workerLoop(); });
  }
}

AsyncDataflowRuntime::~AsyncDataflowRuntime()
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

DataflowResult
AsyncDataflowRuntime::run(const std::string& sessionId,
                          const std::vector<RoleSpec>& roles,
                          const std::map<std::string, TensorBundle>& initialInputsByScope,
                          const RoleRunner& runner)
{
  if (!runner) {
    throw std::invalid_argument("AsyncDataflowRuntime requires a role runner");
  }

  auto state = std::make_shared<RunState>();
  state->sessionId = sessionId;
  state->runner = runner;
  state->remainingRoles = roles.size();
  state->initialInputsByScope = initialInputsByScope;

  for (const auto& role : roles) {
    if (role.role.empty()) {
      throw std::invalid_argument("RoleSpec.role must not be empty");
    }
    if (!state->roles.emplace(role.role, role).second) {
      throw std::invalid_argument("duplicate RoleSpec.role: " + role.role);
    }
    for (const auto& edge : role.outputs) {
      if (edge.scope.empty()) {
        throw std::invalid_argument("DependencyEdge.scope must not be empty");
      }
      if (!edge.consumerRole.empty()) {
        state->consumersByScope[edge.scope].insert(edge.consumerRole);
      }
    }
  }

  for (const auto& item : initialInputsByScope) {
    publishToRun(*state, item.first, item.second);
  }

  for (const auto& role : roles) {
    if (readyToRun(*state, role)) {
      scheduleRole(state, role.role);
    }
  }

  std::unique_lock<std::mutex> doneLock(state->mutex);
  state->doneCv.wait(doneLock, [&] {
    return state->remainingRoles == 0 || state->failure.has_value();
  });
  if (state->failure) {
    std::rethrow_exception(*state->failure);
  }
  return DataflowResult{
    state->outputsByScope,
    state->roleTimings,
  };
}

bool
AsyncDataflowRuntime::readyToRun(const RunState& state, const RoleSpec& role)
{
  for (const auto& edge : role.inputs) {
    if (state.availableByScope.find(edge.scope) == state.availableByScope.end()) {
      return false;
    }
  }
  return true;
}

RoleExecutionContext
AsyncDataflowRuntime::makeContext(const RunState& state, const RoleSpec& role)
{
  RoleExecutionContext ctx;
  ctx.sessionId = state.sessionId;
  ctx.role = role.role;
  for (const auto& edge : role.inputs) {
    const auto found = state.availableByScope.find(edge.scope);
    if (found == state.availableByScope.end()) {
      throw std::logic_error("role scheduled before input was available: " + role.role);
    }
    ctx.inputsByScope.emplace(edge.scope, found->second);
  }
  return ctx;
}

void
AsyncDataflowRuntime::publishToRun(RunState& state,
                                   const std::string& scope,
                                   const TensorBundle& bundle)
{
  state.availableByScope[scope] = bundle;
  state.outputsByScope[scope] = bundle;
}

void
AsyncDataflowRuntime::scheduleRole(const std::shared_ptr<RunState>& state,
                                   const std::string& role)
{
  {
    std::lock_guard<std::mutex> stateLock(state->mutex);
    if (!state->scheduledRoles.insert(role).second) {
      return;
    }
  }
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push_back(WorkItem{state, role, std::chrono::steady_clock::now()});
  }
  m_cv.notify_one();
}

void
AsyncDataflowRuntime::workerLoop()
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
AsyncDataflowRuntime::execute(const WorkItem& item)
{
  auto state = item.state;
  RoleSpec role;
  RoleExecutionContext ctx;
  {
    std::lock_guard<std::mutex> stateLock(state->mutex);
    const auto found = state->roles.find(item.role);
    if (found == state->roles.end()) {
      failRun(*state, std::make_exception_ptr(
        std::logic_error("scheduled unknown role: " + item.role)));
      return;
    }
    role = found->second;
    ctx = makeContext(*state, role);
  }

  RoleTiming timing;
  timing.role = item.role;
  timing.queuedAt = item.queuedAt;
  timing.workerStartedAt = std::chrono::steady_clock::now();
  timing.startedAt = timing.workerStartedAt;

  std::map<std::string, TensorBundle> outputs;
  try {
    outputs = state->runner(ctx);
  }
  catch (...) {
    std::lock_guard<std::mutex> stateLock(state->mutex);
    failRun(*state, std::current_exception());
    return;
  }

  timing.finishedAt = std::chrono::steady_clock::now();
  std::vector<std::string> newlyReady;
  {
    std::lock_guard<std::mutex> stateLock(state->mutex);
    for (const auto& edge : role.outputs) {
      const auto found = outputs.find(edge.scope);
      if (found == outputs.end()) {
        failRun(*state, std::make_exception_ptr(
          std::logic_error("runner did not publish output scope: " + edge.scope)));
        return;
      }
      publishToRun(*state, edge.scope, found->second);
      const auto consumers = state->consumersByScope.find(edge.scope);
      if (consumers != state->consumersByScope.end()) {
        newlyReady.insert(newlyReady.end(), consumers->second.begin(), consumers->second.end());
      }
    }
    state->roleTimings.push_back(timing);
    if (state->remainingRoles > 0) {
      --state->remainingRoles;
    }
    if (state->remainingRoles == 0) {
      state->doneCv.notify_all();
    }
  }

  for (const auto& consumerRole : newlyReady) {
    bool ready = false;
    {
      std::lock_guard<std::mutex> stateLock(state->mutex);
      const auto found = state->roles.find(consumerRole);
      if (found != state->roles.end()) {
        ready = readyToRun(*state, found->second);
      }
    }
    if (ready) {
      scheduleRole(state, consumerRole);
    }
  }
}

void
AsyncDataflowRuntime::failRun(RunState& state, std::exception_ptr failure)
{
  if (!state.failure) {
    state.failure = failure;
  }
  state.doneCv.notify_all();
}

double
durationMs(std::chrono::steady_clock::time_point start,
           std::chrono::steady_clock::time_point end)
{
  return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
    end - start).count();
}

} // namespace ndnsf::di
