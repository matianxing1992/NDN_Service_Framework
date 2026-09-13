#include "OperationRuntime.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <string>
#include <vector>

namespace ndn_service_framework {

namespace {

/**
 * Joins a runtime worker whose last owner was released from that worker.
 * A worker cannot join itself, so this short-lived process-wide join service
 * provides the required join barrier without detaching the runtime thread.
 */
class RuntimeJoinReaper
{
public:
  static RuntimeJoinReaper& instance()
  {
    static RuntimeJoinReaper reaper;
    return reaper;
  }

  void enqueue(std::thread worker) noexcept
  {
    if (!worker.joinable())
      return;
    try {
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_workers.push_back(std::move(worker));
      }
      m_condition.notify_one();
    }
    catch (...) {
      // There is no legal self-join fallback.  Preserve the no-detach
      // invariant if the process cannot allocate the reaper queue.
      std::terminate();
    }
  }

private:
  RuntimeJoinReaper()
    : m_thread([this] { run(); })
  {
  }

  ~RuntimeJoinReaper() noexcept
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_stopping = true;
    }
    m_condition.notify_all();
    if (m_thread.joinable())
      m_thread.join();
  }

  void run() noexcept
  {
    for (;;) {
      std::thread worker;
      {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_condition.wait(lock, [this] { return m_stopping || !m_workers.empty(); });
        if (m_workers.empty() && m_stopping)
          return;
        worker = std::move(m_workers.front());
        m_workers.pop_front();
      }
      if (worker.joinable())
        worker.join();
    }
  }

  std::mutex m_mutex;
  std::condition_variable m_condition;
  std::deque<std::thread> m_workers;
  bool m_stopping = false;
  std::thread m_thread;
};

bool isDrainedLocked(const detail::RuntimeState& state)
{
  return state.closed && state.tickets == 0 && state.queued == 0 &&
         state.active == 0 && state.timers.empty();
}

bool isQuiescentLocked(const detail::RuntimeState& state)
{
  return state.tickets == 0 && state.queued == 0 && state.active == 0 &&
         state.timers.empty();
}

bool evaluateExtraReady(const std::shared_ptr<detail::RuntimeState::DrainWaiter>& waiter,
                        bool& failed) noexcept
{
  failed = false;
  if (!waiter->extraReady)
    return true;
  try {
    return waiter->extraReady();
  }
  catch (...) {
    // External owner probes are advisory callbacks.  A throwing probe must
    // complete its waiter with failure instead of escaping the worker or
    // leaving a waiter that can never be notified again.
    failed = true;
    return true;
  }
}

void notifyDrained(const std::shared_ptr<detail::RuntimeState>& state)
{
  state->condition.notify_all();
}

} // namespace

void
OperationRuntime::notifyWaiters() noexcept
{
  auto state = m_state;
  if (state)
    state->condition.notify_all();
}

detail::RuntimeState::~RuntimeState()
{
  if (!worker.joinable())
    return;
  if (std::this_thread::get_id() == worker.get_id())
    RuntimeJoinReaper::instance().enqueue(std::move(worker));
  else
    worker.join();
}

detail::RuntimeTaskHold::~RuntimeTaskHold()
{
  if (!state)
    return;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->tickets > 0)
      --state->tickets;
  }
  state->condition.notify_all();
}

void
OperationSubscription::cancel() noexcept
{
  if (m_control)
    m_control->cancel();
}

OperationSubscription&
OperationSubscription::operator=(OperationSubscription&& other) noexcept
{
  if (this == &other)
    return *this;
  cancel();
  m_control = std::move(other.m_control);
  return *this;
}

OperationRuntime::WorkTicket&
OperationRuntime::WorkTicket::operator=(WorkTicket&& other) noexcept
{
  if (this == &other)
    return *this;
  if (m_active && m_state) {
    auto state = std::move(m_state);
    m_active = false;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->tickets > 0)
        --state->tickets;
    }
    state->condition.notify_all();
  }
  m_state = std::move(other.m_state);
  m_active = std::exchange(other.m_active, false);
  return *this;
}

OperationRuntime::WorkTicket::~WorkTicket() noexcept
{
  if (!m_active || !m_state)
    return;
  std::shared_ptr<detail::RuntimeState> state = std::move(m_state);
  m_active = false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->tickets > 0)
      --state->tickets;
  }
  state->condition.notify_all();
}

std::shared_ptr<OperationRuntime>
OperationRuntime::create(std::function<void(std::function<void()>)> submitHook)
{
  auto state = std::make_shared<detail::RuntimeState>();
  state->submitHook = std::move(submitHook);
  auto runtime = std::shared_ptr<OperationRuntime>(new OperationRuntime(state));
  // A submit hook only redirects posted work for deterministic tests; the
  // runtime worker remains present for timers and drain bookkeeping.
  state->worker = std::thread([state] {
    OperationRuntime::runWorker(state);
  });
  return runtime;
}

OperationRuntime::~OperationRuntime() noexcept
{
  close();
  auto state = m_state;
  if (!state || !state->worker.joinable())
    return;
  bool self = false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    self = state->workerId == std::this_thread::get_id();
  }
  // A worker cannot join itself.  Transfer the joinable thread to the
  // process-local reaper; it will join after this callback returns.
  // A deterministic submit hook may retain callbacks (and their accounting
  // holds) outside the runtime until a test pump invokes them.  Do not block
  // the owner destructor joining a timer worker that is waiting for those
  // external holds; the reaper still performs a real join once they drain.
  if (self || static_cast<bool>(state->submitHook))
    RuntimeJoinReaper::instance().enqueue(std::move(state->worker));
  else
    state->worker.join();
}

OperationRuntime::WorkTicket
OperationRuntime::acquire()
{
  auto state = m_state;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->closed)
    throw OperationError(OperationErrorCode::Closed, "operation runtime is closed");
  ++state->tickets;
  return WorkTicket(std::move(state));
}

void
OperationRuntime::post(WorkTicket& ticket, std::function<void()> task)
{
  if (!task)
    throw std::invalid_argument("operation runtime task is empty");
  auto state = m_state;
  if (!ticket.valid() || ticket.m_state != state)
    throw OperationError(OperationErrorCode::Closed, "operation ticket is invalid");

  auto wrapped = [state, task = std::move(task)]() mutable {
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->queued > 0)
        --state->queued;
      ++state->active;
    }
    try {
      task();
    }
    catch (...) {
      // Runtime callbacks are failure-isolated.  Domain owners report their
      // own exception through OperationState::fail.
    }
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->active > 0)
        --state->active;
      if (isDrainedLocked(*state))
        state->drained = true;
    }
    notifyDrained(state);
  };

  bool useHook = false;
  std::shared_ptr<detail::RuntimeTaskHold> hold;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->closed)
      throw OperationError(OperationErrorCode::Closed, "operation runtime is closed");
    hold = std::make_shared<detail::RuntimeTaskHold>(state);
    ++state->tickets;
    ++state->queued;
    useHook = static_cast<bool>(state->submitHook);
    if (!useHook) {
      try {
        state->queue.push_back({hold, std::move(wrapped)});
      }
      catch (...) {
        // RuntimeTaskHold releases tickets after this lock is gone; queued
        // must be rolled back here because it is not part of that token.
        --state->queued;
        throw;
      }
    }
  }
  if (useHook) {
    std::shared_ptr<std::shared_ptr<detail::RuntimeTaskHold>> holdBox;
    std::function<void()> guarded;
    const auto rollbackQueued = [&] {
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->queued > 0)
          --state->queued;
      }
      // RuntimeTaskHold must never be destroyed while state->mutex is held.
      holdBox.reset();
      hold.reset();
      state->condition.notify_all();
    };
    try {
      holdBox = std::make_shared<std::shared_ptr<detail::RuntimeTaskHold>>(std::move(hold));
      auto once = std::make_shared<std::atomic<bool>>(false);
      guarded = [once, holdBox, wrapped]() mutable {
        if (!once->exchange(true)) {
          wrapped();
          // A retained hook callback must not retain a completed task's
          // accounting ticket after its first dispatch.
          holdBox->reset();
        }
      };
    }
    catch (...) {
      rollbackQueued();
      throw;
    }
    try {
      state->submitHook(guarded);
    }
    catch (...) {
      // A test hook may run the task and then report its own failure.  The
      // once gate makes inline recovery harmless in that case.
      guarded();
    }
    return;
  }
  state->condition.notify_one();
}

void
OperationRuntime::close() noexcept
{
  auto state = m_state;
  if (!state)
    return;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->closed = true;
    if (isDrainedLocked(*state))
      state->drained = true;
  }
  state->condition.notify_all();
}

bool
OperationRuntime::drain(std::chrono::milliseconds timeout)
{
  if (timeout.count() < 0)
    throw std::invalid_argument("operation runtime drain timeout is negative");
  close();
  auto state = m_state;
  if (isWorkerThread())
    throw OperationError(OperationErrorCode::WouldDeadlock,
                         "operation runtime drain from its worker would deadlock");
  std::unique_lock<std::mutex> lock(state->mutex);
  const auto done = [&] { return state->drained || isDrainedLocked(*state); };
  if (done()) {
    state->drained = true;
    return true;
  }
  if (timeout.count() == 0)
    return false;
  if (!state->condition.wait_for(lock, timeout, done))
    return false;
  state->drained = true;
  return true;
}

OperationSubscription
OperationRuntime::drainAsync(std::chrono::milliseconds timeout,
                             std::function<void(bool)> callback)
{
  return drainAsync(timeout, std::move(callback), true, {});
}

OperationSubscription
OperationRuntime::drainAsync(std::chrono::milliseconds timeout,
                             std::function<void(bool)> callback,
                             bool closeRuntime)
{
  return drainAsync(timeout, std::move(callback), closeRuntime, {});
}

OperationSubscription
OperationRuntime::drainAsync(std::chrono::milliseconds timeout,
                             std::function<void(bool)> callback,
                             bool closeRuntime,
                             std::function<bool()> extraReady)
{
  if (timeout.count() < 0)
    throw std::invalid_argument("operation runtime drain timeout is negative");
  if (!callback)
    throw std::invalid_argument("operation runtime drain callback is empty");
  if (closeRuntime)
    close();
  auto control = std::make_shared<detail::SubscriptionControl>();
  auto state = m_state;
  auto waiter = std::make_shared<detail::RuntimeState::DrainWaiter>();
  waiter->control = control;
  waiter->callback = std::move(callback);
  waiter->deadline = std::chrono::steady_clock::now() + timeout;
  waiter->closeRuntime = closeRuntime;
  waiter->extraReady = std::move(extraReady);
  std::weak_ptr<detail::RuntimeState::DrainWaiter> weakWaiter = waiter;
  control->cancelFn = [state, weakWaiter] {
    auto waiter = weakWaiter.lock();
    if (!waiter)
      return;
    std::lock_guard<std::mutex> lock(state->mutex);
    waiter->callback = {};
    state->drainWaiters.erase(std::remove(state->drainWaiters.begin(),
                                          state->drainWaiters.end(), waiter),
                              state->drainWaiters.end());
  };
  bool immediate = false;
  bool immediateResult = false;
  {
    // Recheck and register under one lock.  The worker cannot mark drained or
    // exit between the decision and insertion, so no waiter can be stranded.
    std::lock_guard<std::mutex> lock(state->mutex);
    bool extraReadyFailed = false;
    const bool extraReadyNow = evaluateExtraReady(waiter, extraReadyFailed);
    if (extraReadyFailed) {
      immediate = true;
      immediateResult = false;
    }
    else if (extraReadyNow && (state->drained || (closeRuntime ? isDrainedLocked(*state)
                                                                  : isQuiescentLocked(*state)))) {
      if (state->closed)
        state->drained = true;
      immediate = true;
      immediateResult = true;
    }
    else if (timeout.count() == 0) {
      immediate = true;
      immediateResult = false;
    }
    else {
      if (state->drainWaiters.size() >= 64)
        throw OperationError(OperationErrorCode::Capacity,
                             "drain subscription capacity exceeded");
      state->drainWaiters.push_back(waiter);
    }
  }
  if (immediate) {
    auto immediateCallback = std::move(waiter->callback);
    auto deliver = std::make_shared<std::function<void()>>(
      [control, callback = std::move(immediateCallback),
       immediateResult] () mutable {
      if (!control->begin())
        return;
      try { callback(immediateResult); } catch (...) {}
      control->end(true);
      });
    bool queued = false;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      queued = !state->closed;
    }
    if (queued) {
      try {
        auto ticket = acquire();
        post(ticket, [deliver] { (*deliver)(); });
      }
      catch (...) {
        // The close race is allowed to collapse to a synchronous terminal
        // notification; no callback is run while state->mutex is held.
        (*deliver)();
      }
    }
    else {
      (*deliver)();
    }
    return OperationSubscription(std::move(control));
  }
  state->condition.notify_all();
  return OperationSubscription(std::move(control));
}

bool
OperationRuntime::isClosed() const noexcept
{
  auto state = m_state;
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->closed;
}

bool
OperationRuntime::isWorkerThread() const noexcept
{
  auto state = m_state;
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->workerId == std::this_thread::get_id();
}

std::function<void()>
OperationRuntime::scheduleAt(WorkTicket& ticket,
                             std::chrono::steady_clock::time_point deadline,
                             std::function<void()> task)
{
  if (!task)
    throw std::invalid_argument("operation timer task is empty");
  auto state = m_state;
  if (!ticket.valid() || ticket.m_state != state)
    throw OperationError(OperationErrorCode::Closed, "operation ticket is invalid");
  std::uint64_t id;
  std::shared_ptr<detail::RuntimeTaskHold> timerTicket;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->closed)
      throw OperationError(OperationErrorCode::Closed, "operation runtime is closed");
    id = ++state->nextTimer;
    timerTicket = std::make_shared<detail::RuntimeTaskHold>(state);
    ++state->tickets;
    try {
      state->timers.emplace(deadline,
                            detail::RuntimeState::Timer{id, timerTicket, std::move(task)});
    }
    catch (...) {
      // timerTicket owns the single increment and is destroyed after this
      // lock is released; do not decrement tickets a second time here.
      throw;
    }
  }
  state->condition.notify_one();
  return [state, id] {
    std::function<void()> retired;
    std::shared_ptr<detail::RuntimeTaskHold> retiredTicket;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      for (auto it = state->timers.begin(); it != state->timers.end(); ++it) {
        if (it->second.id == id) {
          retiredTicket = std::move(it->second.ticket);
          retired = std::move(it->second.function);
          state->timers.erase(it);
          break;
        }
      }
      if (isDrainedLocked(*state))
        state->drained = true;
    }
    retiredTicket.reset();
    state->condition.notify_all();
  };
}

void
OperationRuntime::runWorker(const std::shared_ptr<detail::RuntimeState>& state) noexcept
{
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->workerId = std::this_thread::get_id();
  }
  for (;;) {
    std::function<void()> task;
    // Keep the task's internal ticket alive until after the callback returns.
    // It must not be destroyed while the worker still owns state->mutex, and
    // it must cover the callback's full execution for drain accounting.
    std::shared_ptr<detail::RuntimeTaskHold> taskTicket;
    bool timerTaskActive = false;
    struct Notification
    {
      std::shared_ptr<detail::SubscriptionControl> control;
      std::function<void(bool)> callback;
      bool result = false;
    };
    std::vector<Notification> notifications;
    {
      std::unique_lock<std::mutex> lock(state->mutex);
      for (;;) {
        if (!state->queue.empty()) {
          auto queuedTask = std::move(state->queue.front());
          state->queue.pop_front();
          taskTicket = std::move(queuedTask.ticket);
          task = std::move(queuedTask.function);
          break;
        }
        if (!state->timers.empty() &&
            state->timers.begin()->first <= std::chrono::steady_clock::now()) {
          auto timer = std::move(state->timers.begin()->second);
          state->timers.erase(state->timers.begin());
          taskTicket = std::move(timer.ticket);
          task = std::move(timer.function);
          ++state->active;
          timerTaskActive = true;
          break;
        }
        if (isQuiescentLocked(*state) && !state->drainWaiters.empty()) {
          for (auto it = state->drainWaiters.begin();
               it != state->drainWaiters.end();) {
            bool extraReadyFailed = false;
            const bool extraReady = evaluateExtraReady(*it, extraReadyFailed);
            if (extraReadyFailed ||
                ((!(*it)->closeRuntime || state->closed) && extraReady)) {
              if ((*it)->callback)
                notifications.push_back(Notification{(*it)->control,
                                                      std::move((*it)->callback),
                                                      !extraReadyFailed});
              it = state->drainWaiters.erase(it);
            }
            else {
              ++it;
            }
          }
          if (!notifications.empty()) {
            lock.unlock();
            for (auto& notification : notifications) {
              if (notification.callback && notification.control->begin()) {
                try { notification.callback(notification.result); } catch (...) {}
                notification.control->end(true);
              }
            }
            notifications.clear();
            state->condition.notify_all();
            lock.lock();
            continue;
          }
        }
        if (state->closed && state->tickets == 0 && state->queued == 0 &&
            state->active == 0 && state->timers.empty()) {
          for (auto it = state->drainWaiters.begin();
               it != state->drainWaiters.end();) {
            bool extraReadyFailed = false;
            const bool extraReady = evaluateExtraReady(*it, extraReadyFailed);
            if (extraReadyFailed || extraReady) {
              if ((*it)->callback)
                notifications.push_back(Notification{(*it)->control,
                                                      std::move((*it)->callback),
                                                      !extraReadyFailed});
              it = state->drainWaiters.erase(it);
            }
            else {
              ++it;
            }
          }
          if (!state->drainWaiters.empty()) {
            // An external owner still has work.  Keep the Core worker alive
            // until notifyWaiters() observes that owner barrier.
            if (!notifications.empty()) {
              lock.unlock();
              for (auto& notification : notifications) {
                if (notification.callback && notification.control->begin()) {
                  try { notification.callback(notification.result); } catch (...) {}
                  notification.control->end(true);
                }
              }
              notifications.clear();
              state->condition.notify_all();
              lock.lock();
              continue;
            }
          }
          else {
            state->drained = true;
            lock.unlock();
            for (auto& notification : notifications) {
              if (notification.callback && notification.control->begin()) {
                try { notification.callback(notification.result); } catch (...) {}
                notification.control->end(true);
              }
            }
            state->condition.notify_all();
            return;
          }
        }
        const auto now = std::chrono::steady_clock::now();
        for (auto it = state->drainWaiters.begin(); it != state->drainWaiters.end();) {
          if (it->get()->deadline <= now) {
            if ((*it)->callback)
              notifications.push_back(Notification{(*it)->control,
                                                    std::move((*it)->callback), false});
            it = state->drainWaiters.erase(it);
          }
          else {
            ++it;
          }
        }
        if (!notifications.empty()) {
          lock.unlock();
          for (auto& notification : notifications) {
            if (notification.callback && notification.control->begin()) {
              try { notification.callback(notification.result); } catch (...) {}
              notification.control->end(true);
            }
          }
          notifications.clear();
          lock.lock();
          continue;
        }
        if (state->timers.empty() && state->drainWaiters.empty()) {
          state->condition.wait(lock);
        }
        else if (!state->timers.empty()) {
          auto wake = state->timers.begin()->first;
          for (const auto& waiter : state->drainWaiters)
            wake = std::min(wake, waiter->deadline);
          state->condition.wait_until(lock, wake);
        }
        else if (!state->drainWaiters.empty()) {
          auto wake = state->drainWaiters.front()->deadline;
          for (const auto& waiter : state->drainWaiters)
            wake = std::min(wake, waiter->deadline);
          state->condition.wait_until(lock, wake);
        }
      }
    }
    try {
      task();
    }
    catch (...) {
      // Timer callbacks are failure-isolated just like queued callbacks.
      // Their owning OperationState reports domain failures explicitly.
    }
    if (timerTaskActive) {
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->active > 0)
          --state->active;
        if (isDrainedLocked(*state))
          state->drained = true;
      }
      state->condition.notify_all();
    }
    taskTicket.reset();
  }
}

} // namespace ndn_service_framework
