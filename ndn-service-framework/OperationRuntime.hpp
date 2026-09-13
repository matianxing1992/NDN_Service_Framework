#ifndef NDN_SERVICE_FRAMEWORK_OPERATION_RUNTIME_HPP
#define NDN_SERVICE_FRAMEWORK_OPERATION_RUNTIME_HPP

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ndn_service_framework {

/** Error categories raised by the generic native operation primitives. */
enum class OperationErrorCode {
  Closed,
  Timeout,
  Cancelled,
  Capacity,
  EventGap,
  WouldDeadlock,
};

/** Exception carrying a stable Core operation error category. */
class OperationError : public std::runtime_error
{
public:
  OperationError(OperationErrorCode code, const std::string& message)
    : std::runtime_error(message)
    , m_code(code)
  {
  }

  /** Return the machine-readable Core error category. */
  OperationErrorCode code() const noexcept { return m_code; }

private:
  OperationErrorCode m_code;
};

namespace detail {

struct SubscriptionControl
{
  mutable std::mutex mutex;
  std::function<void()> cancelFn;
  bool cancelled = false;
  bool finished = false;
  bool executing = false;

  void cancel() noexcept
  {
    std::function<void()> fn;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (cancelled || finished)
        return;
      cancelled = true;
      fn = cancelFn;
      cancelFn = {};
    }
    if (fn) {
      try {
        fn();
      }
      catch (...) {
        // Cancellation is explicitly noexcept; owners must not be revived by
        // a cleanup callback throwing across the public boundary.
      }
    }
  }

  bool begin() noexcept
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (cancelled || finished || executing)
      return false;
    executing = true;
    // Taking the execution right also retires any pre-dispatch cancellation
    // action.  A concurrent unsubscribe may cancel before this lock and
    // invoke rollback, or after it and observe no rollback to run; it cannot
    // revoke the delivery between begin and its commit.
    cancelFn = {};
    return true;
  }

  /** Install a cancellation action for work selected but not yet delivered. */
  bool setCancelFn(std::function<void()> fn) noexcept
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (cancelled || finished)
      return false;
    cancelFn = std::move(fn);
    return true;
  }

  void end(bool oneShot) noexcept
  {
    std::lock_guard<std::mutex> lock(mutex);
    executing = false;
    if (oneShot) {
      finished = true;
      // A completed one-shot must not retain its cancellation closure (and
      // therefore an entire OperationState) merely because the public
      // subscription object is still in scope.
      cancelFn = {};
    }
  }

  bool isCancelled() const noexcept
  {
    std::lock_guard<std::mutex> lock(mutex);
    return cancelled;
  }
};

struct RuntimeTaskHold;

struct RuntimeState
{
  struct Task
  {
    std::shared_ptr<RuntimeTaskHold> ticket;
    std::function<void()> function;
  };

  struct DrainWaiter
  {
    std::shared_ptr<SubscriptionControl> control;
    std::function<void(bool)> callback;
    std::chrono::steady_clock::time_point deadline;
    bool closeRuntime = true;
    std::function<bool()> extraReady;
  };

  struct Timer
  {
    std::uint64_t id = 0;
    std::shared_ptr<RuntimeTaskHold> ticket;
    std::function<void()> function;
  };

  mutable std::mutex mutex;
  std::condition_variable condition;
  std::deque<Task> queue;
  std::multimap<std::chrono::steady_clock::time_point, Timer> timers;
  std::uint64_t nextTimer = 0;
  std::size_t tickets = 0;
  std::size_t queued = 0;
  std::size_t active = 0;
  bool closed = false;
  bool drained = false;
  std::thread worker;
  std::thread::id workerId;
  std::function<void(std::function<void()>)> submitHook;
  std::vector<std::shared_ptr<DrainWaiter>> drainWaiters;

  ~RuntimeState();
};

/** Shared ownership token retained by a queued task until its invocation ends. */
struct RuntimeTaskHold
{
  explicit RuntimeTaskHold(std::shared_ptr<RuntimeState> state)
    : state(std::move(state)) {}
  ~RuntimeTaskHold();
  std::shared_ptr<RuntimeState> state;
};

} // namespace detail

/**
 * Move-only cancellation handle for one completion, observer, reader, or
 * drain wait.  Cancelling a subscription only removes that delivery; it does
 * not cancel the associated operation.
 */
class OperationSubscription
{
public:
  OperationSubscription() noexcept = default;
  OperationSubscription(OperationSubscription&&) noexcept = default;
  OperationSubscription& operator=(OperationSubscription&&) noexcept;
  OperationSubscription(const OperationSubscription&) = delete;
  OperationSubscription& operator=(const OperationSubscription&) = delete;
  ~OperationSubscription() noexcept { cancel(); }

  /** Cancel an undelivered callback.  This method is idempotent and noexcept. */
  void cancel() noexcept;

  /** Compatibility spelling for the public C-07 subscription API. */
  void unsubscribe() noexcept { cancel(); }

  /** Create a subscription from a Core-owned control object. */
  static OperationSubscription fromControl(
    std::shared_ptr<detail::SubscriptionControl> control) noexcept
  {
    return OperationSubscription(std::move(control));
  }

private:
  explicit OperationSubscription(std::shared_ptr<detail::SubscriptionControl> control)
    : m_control(std::move(control))
  {
  }

  std::shared_ptr<detail::SubscriptionControl> m_control;

  friend class OperationRuntime;
  template<typename Result, typename Event> friend class OperationState;
  template<typename Event> friend class OperationReader;
};

/**
 * Core-owned serial work, timer, ticket, and drain runtime.  It has no DI,
 * model, Python, or network protocol dependency.
 */
class OperationRuntime : public std::enable_shared_from_this<OperationRuntime>
{
public:
  class WorkTicket
  {
  public:
    WorkTicket() noexcept = default;
    WorkTicket(WorkTicket&& other) noexcept
      : m_state(std::move(other.m_state)), m_active(std::exchange(other.m_active, false))
    {
    }
    WorkTicket& operator=(WorkTicket&& other) noexcept;
    WorkTicket(const WorkTicket&) = delete;
    WorkTicket& operator=(const WorkTicket&) = delete;
    ~WorkTicket() noexcept;

    /** Whether this ticket still owns one runtime activity slot. */
    bool valid() const noexcept { return m_active && static_cast<bool>(m_state); }

  private:
    explicit WorkTicket(std::shared_ptr<detail::RuntimeState> state)
      : m_state(std::move(state)), m_active(true)
    {
    }

    std::shared_ptr<detail::RuntimeState> m_state;
    bool m_active = false;

    friend class OperationRuntime;
  };

  /** Create a runtime with its own worker; the hook is an internal test port. */
  static std::shared_ptr<OperationRuntime>
  create(std::function<void(std::function<void()>)> submitHook = {});

  ~OperationRuntime() noexcept;

  /** Acquire one activity ticket or throw OperationError(Closed). */
  WorkTicket acquire();

  /** Queue work associated with a ticket; the callback never runs under a caller lock. */
  void post(WorkTicket& ticket, std::function<void()> task);

  /** Schedule work associated with a ticket and return its cancellation action. */
  std::function<void()> scheduleAt(WorkTicket& ticket,
                                   std::chrono::steady_clock::time_point deadline,
                                   std::function<void()> task);

  /** Stop accepting tickets and allow already queued work to settle. */
  void close() noexcept;

  /** Wake drain waiters whose owner supplies an external readiness predicate. */
  void notifyWaiters() noexcept;

  /** Close and wait for all tickets, queued work, and timers to settle. */
  bool drain(std::chrono::milliseconds timeout);

  /** Register a non-business drain notification. */
  OperationSubscription
  drainAsync(std::chrono::milliseconds timeout, std::function<void(bool)> callback);

  /** Register a drain notification with an explicit lifecycle mode. */
  OperationSubscription
  drainAsync(std::chrono::milliseconds timeout, std::function<void(bool)> callback,
             bool closeRuntime);

  /**
   * Register a drain notification without changing the runtime lifecycle.
   * The two- and three-argument overloads retain the historical close-and-drain
   * behavior; this overload additionally accepts an external readiness probe.
   */
  OperationSubscription
  drainAsync(std::chrono::milliseconds timeout, std::function<void(bool)> callback,
             bool closeRuntime, std::function<bool()> extraReady);

  /** Return whether this runtime has entered its one-way closing phase. */
  bool isClosed() const noexcept;

  /** Return whether the calling thread is this runtime's worker. */
  bool isWorkerThread() const noexcept;

private:
  explicit OperationRuntime(std::shared_ptr<detail::RuntimeState> state)
    : m_state(std::move(state))
  {
  }

  /** Linearize a State subscription against close under the runtime lock. */
  template<typename Function>
  void withOpenRegistration(Function&& function)
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (m_state->closed)
      throw OperationError(OperationErrorCode::Closed, "operation runtime is closed");
    std::forward<Function>(function)();
  }

  void releaseTicket(const std::shared_ptr<detail::RuntimeState>& state) noexcept;
  static void runWorker(const std::shared_ptr<detail::RuntimeState>& state) noexcept;

  std::shared_ptr<detail::RuntimeState> m_state;

  template<typename Result, typename Event> friend class OperationState;
  template<typename Event> friend class OperationReader;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_OPERATION_RUNTIME_HPP
