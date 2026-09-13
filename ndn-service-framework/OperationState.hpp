#ifndef NDN_SERVICE_FRAMEWORK_OPERATION_STATE_HPP
#define NDN_SERVICE_FRAMEWORK_OPERATION_STATE_HPP

#include "OperationRuntime.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace ndn_service_framework {

namespace detail {

inline std::exception_ptr
operationException(OperationErrorCode code, const std::string& message)
{
  try { throw OperationError(code, message); }
  catch (...) { return std::current_exception(); }
}

template<typename Event>
struct OperationChannelData
{
  using ReaderCallback = std::function<void(std::optional<Event>, std::exception_ptr)>;
  struct EventRecord { Event event; std::size_t bytes = 0; };
  struct Observer {
    std::shared_ptr<SubscriptionControl> control;
    std::function<void(const Event&)> callback;
    std::size_t nextEvent = 0;
  };
  struct PendingRead {
    std::uint64_t id = 0;
    std::shared_ptr<SubscriptionControl> control;
    ReaderCallback callback;
    std::function<void()> cancelTimer;
    std::function<void()> commit;
    std::function<void()> rollback;
    std::uint64_t generation = 0;
  };

  explicit OperationChannelData(std::shared_ptr<OperationRuntime> runtime,
                                std::function<void()> cancelBusiness,
                                std::function<std::size_t(const Event&)> measure,
                                std::function<std::exception_ptr(OperationErrorCode,
                                                                  const std::string&)> mapError)
    : runtime(std::move(runtime)), cancelBusiness(std::move(cancelBusiness)),
      measure(std::move(measure)), mapError(std::move(mapError))
  {
    if (!this->runtime)
      throw std::invalid_argument("operation state requires a runtime");
    ticket.emplace(this->runtime->acquire());
  }

  std::shared_ptr<OperationRuntime> runtime;
  std::optional<OperationRuntime::WorkTicket> ticket;
  std::function<void()> cancelBusiness;
  std::function<std::size_t(const Event&)> measure;
  std::function<std::exception_ptr(OperationErrorCode, const std::string&)> mapError;
  mutable std::mutex mutex;
  std::condition_variable condition;
  bool terminal = false;
  bool succeeded = false;
  std::exception_ptr failure;
  std::uint64_t nextId = 0;
  std::map<std::uint64_t, Observer> observers;
  std::deque<EventRecord> events;
  std::size_t eventBytes = 0;
  bool eventGap = false;
  bool terminalEventPublished = false;
  bool readerActive = false;
  bool readerDispatchPending = false;
  bool readerGapPending = false;
  bool readerGapReported = false;
  std::uint64_t observerDropped = 0;
  std::uint64_t readerId = 0;
  std::uint64_t readerCursor = 0;
  std::optional<PendingRead> pendingRead;

  void reclaimConsumedLocked()
  {
    if (events.empty())
      return;
    // Reliable history is reclaimed only by the active reader.  An observer
    // is best-effort and must neither authorize reclamation nor make an
    // unread reliable prefix disappear before a reader is opened.
    if (!readerActive)
      return;
    const std::size_t reclaim = static_cast<std::size_t>(readerCursor);
    if (reclaim == 0)
      return;
    for (std::size_t i = 0; i < reclaim; ++i)
      eventBytes -= events[i].bytes;
    events.erase(events.begin(), events.begin() + static_cast<std::ptrdiff_t>(reclaim));
    if (readerActive)
      readerCursor -= reclaim;
    for (auto& entry : observers)
      entry.second.nextEvent = entry.second.nextEvent > reclaim
        ? entry.second.nextEvent - reclaim : 0;
  }
};

template<typename Result, typename Event>
struct OperationStateData : OperationChannelData<Event>
{
  using Base = OperationChannelData<Event>;
  using CompletionCallback = std::function<void()>;
  using ResultCallback = std::function<void(std::optional<Result>, std::exception_ptr)>;
  struct CompletionWaiter {
    std::shared_ptr<SubscriptionControl> control;
    CompletionCallback callback;
    ResultCallback resultCallback;
    std::function<void()> cancelTimer;
  };

  OperationStateData(std::shared_ptr<OperationRuntime> runtime,
                     std::function<void()> cancelBusiness,
                     std::function<std::size_t(const Event&)> measure,
                     std::function<std::exception_ptr(OperationErrorCode,
                                                       const std::string&)> mapError)
    : Base(std::move(runtime), std::move(cancelBusiness), std::move(measure),
           std::move(mapError)) {}

  std::optional<Result> result;
  std::map<std::uint64_t, CompletionWaiter> completionWaiters;
};

template<typename Event>
std::exception_ptr
mapChannelError(const std::shared_ptr<OperationChannelData<Event>>& data,
                OperationErrorCode code, const std::string& message)
{
  if (data->mapError)
    return data->mapError(code, message);
  return operationException(code, message);
}

template<typename Event>
std::exception_ptr
mapChannelErrorNoThrow(const std::shared_ptr<OperationChannelData<Event>>& data,
                       OperationErrorCode code, const std::string& message) noexcept
{
  try {
    return mapChannelError(data, code, message);
  }
  catch (...) {
    return operationException(code, message);
  }
}

template<typename Result, typename Event>
std::exception_ptr
mapStateError(const std::shared_ptr<OperationStateData<Result, Event>>& data,
              OperationErrorCode code, const std::string& message)
{
  return mapChannelError(std::static_pointer_cast<OperationChannelData<Event>>(data),
                         code, message);
}

template<typename Event>
void dispatchTask(const std::shared_ptr<OperationChannelData<Event>>& data,
                  std::function<void()> task);

template<typename Event>
void dispatchReader(const std::shared_ptr<OperationChannelData<Event>>& data,
                    typename OperationChannelData<Event>::PendingRead pending,
                    std::optional<Event> event, std::exception_ptr error)
{
  // The read is no longer pending once ownership reaches this function.  A
  // timer left in the runtime would otherwise extend drain past callback
  // completion (or fire a second delivery), so retire it before dispatch.
  auto cancelTimer = std::move(pending.cancelTimer);
  auto commit = std::move(pending.commit);
  auto control = pending.control;
  auto callback = std::move(pending.callback);
  auto rollback = std::move(pending.rollback);
  const auto generation = pending.generation;
  const auto clearDispatch = [data, generation] {
    std::lock_guard<std::mutex> lock(data->mutex);
    if (data->readerId == generation)
      data->readerDispatchPending = false;
  };
  const auto markGap = [data, generation] {
    std::lock_guard<std::mutex> lock(data->mutex);
    if (data->readerId == generation) {
      data->eventGap = true;
      data->readerGapPending = true;
      data->readerGapReported = false;
      data->readerDispatchPending = false;
    }
  };
  // Keep the original callback local until dispatch setup has succeeded.  A
  // failed std::function/lambda allocation must still be able to complete
  // this one-shot subscription with an explicit stream error.
  const auto failSetup = [&] {
    try { markGap(); } catch (...) {}
    try {
      if (control && control->begin()) {
        try {
          callback({}, detail::operationException(
            OperationErrorCode::EventGap, "event reader dispatch setup failed"));
        }
        catch (...) {}
        control->end(true);
      }
    }
    catch (...) {
      try { control->end(true); } catch (...) {}
    }
  };
  try {
    if (cancelTimer)
      cancelTimer();
    if (rollback && !control->setCancelFn(rollback)) {
      try { rollback(); } catch (...) { clearDispatch(); }
    }
    auto callbackForInvoke = callback;
    auto invoke = [control, callback = std::move(callbackForInvoke), event = std::move(event), error,
              commit = std::move(commit), rollback = std::move(rollback),
              clearDispatch, markGap]() mutable {
      if (!control->begin()) {
        try {
          if (rollback) rollback();
          else clearDispatch();
        }
        catch (...) { clearDispatch(); }
        return;
      }
      try {
        if (commit) commit();
        else clearDispatch();
      }
      catch (...) {
        try { markGap(); } catch (...) {}
        try {
          callback({}, detail::operationException(
            OperationErrorCode::EventGap, "event reader delivery commit failed"));
        }
        catch (...) {}
        control->end(true);
        return;
      }
      try { callback(std::move(event), error); } catch (...) {}
      control->end(true);
    };
    dispatchTask(data, std::move(invoke));
  }
  catch (...) {
    failSetup();
  }
}

template<typename Event>
void dispatchTask(const std::shared_ptr<OperationChannelData<Event>>& data,
                  std::function<void()> task)
{
  std::optional<OperationRuntime::WorkTicket> temporary;
  OperationRuntime::WorkTicket* ticket = nullptr;
  try {
    // Never borrow the operation's optional ticket: terminal cleanup and
    // reader close may move it concurrently.  A short independent ticket
    // keeps the queued callback valid until post has accepted it.
    temporary.emplace(data->runtime->acquire());
    ticket = &*temporary;
  }
  catch (...) {
    task();
    return;
  }
  std::function<void()> fallback;
  try {
    fallback = task;
  }
  catch (...) {
    try { task(); } catch (...) {}
    return;
  }
  try { data->runtime->post(*ticket, std::move(task)); }
  catch (...) { fallback(); }
}

} // namespace detail

template<typename Event>
class OperationReader;

/** Shared, typed completion and bounded event state owned by a Core operation. */
template<typename Result, typename Event>
class OperationState
{
public:
  static_assert(std::is_nothrow_move_constructible_v<Event>,
                "OperationState Event must be noexcept movable");
  using CancelFunction = std::function<void()>;
  using EventSizeFunction = std::function<std::size_t(const Event&)>;
  using ErrorMapper = std::function<std::exception_ptr(OperationErrorCode,
                                                        const std::string&)>;
  using Data = detail::OperationStateData<Result, Event>;
  using Base = detail::OperationChannelData<Event>;

  /** Construct a state and acquire one Core runtime ticket. */
  explicit OperationState(std::shared_ptr<OperationRuntime> runtime,
                          CancelFunction cancelBusiness = {},
                          EventSizeFunction measure = {},
                          ErrorMapper mapError = {})
    : m_data(std::make_shared<Data>(std::move(runtime), std::move(cancelBusiness),
                                    std::move(measure), std::move(mapError)))
  {
    if (!m_data->measure)
      m_data->measure = [](const Event&) { return std::size_t{1}; };
  }

  OperationState(const OperationState&) = delete;
  OperationState& operator=(const OperationState&) = delete;

  /** Complete once with an owning result; duplicate terminal claims return false. */
  bool complete(Result result)
  {
    std::vector<typename Data::CompletionWaiter> waiters;
    std::optional<typename Base::PendingRead> reader;
    {
      std::lock_guard<std::mutex> lock(m_data->mutex);
      if (m_data->terminal) return false;
      m_data->result.emplace(std::move(result));
      m_data->succeeded = true;
      m_data->terminal = true;
      for (auto& entry : m_data->completionWaiters)
        waiters.push_back(std::move(entry.second));
      m_data->completionWaiters.clear();
      if (m_data->pendingRead && m_data->readerCursor >= m_data->events.size()) {
        reader = std::move(m_data->pendingRead);
        m_data->pendingRead.reset();
        const auto generation = reader->generation;
        reader->commit = [data = std::static_pointer_cast<Base>(m_data), generation] {
          std::lock_guard<std::mutex> lock(data->mutex);
          if (data->readerId == generation)
            data->readerDispatchPending = false;
        };
        reader->rollback = reader->commit;
      }
    }
    m_data->condition.notify_all();
    dispatchCompletion(std::move(waiters), std::move(reader));
    return true;
  }

  /** Fail once with an exception; the exception is rethrown by synchronous readers. */
  bool fail(std::exception_ptr error)
  {
    if (!error) {
      {
        std::lock_guard<std::mutex> lock(m_data->mutex);
        if (m_data->terminal)
          return false;
      }
      try {
        error = detail::mapStateError(m_data, OperationErrorCode::Cancelled,
                                      "operation failed without an exception");
      }
      catch (...) {
        // If another owner won while the mapper ran, preserve fail's
        // idempotent terminal contract rather than exposing a mapper error.
        std::lock_guard<std::mutex> lock(m_data->mutex);
        if (m_data->terminal)
          return false;
        throw;
      }
    }
    std::vector<typename Data::CompletionWaiter> waiters;
    std::optional<typename Base::PendingRead> reader;
    {
      std::lock_guard<std::mutex> lock(m_data->mutex);
      if (m_data->terminal) return false;
      m_data->failure = std::move(error);
      m_data->terminal = true;
      for (auto& entry : m_data->completionWaiters)
        waiters.push_back(std::move(entry.second));
      m_data->completionWaiters.clear();
      if (m_data->pendingRead && m_data->readerCursor >= m_data->events.size()) {
        reader = std::move(m_data->pendingRead);
        m_data->pendingRead.reset();
        const auto generation = reader->generation;
        reader->commit = [data = std::static_pointer_cast<Base>(m_data), generation] {
          std::lock_guard<std::mutex> lock(data->mutex);
          if (data->readerId == generation)
            data->readerDispatchPending = false;
        };
        reader->rollback = reader->commit;
      }
    }
    m_data->condition.notify_all();
    dispatchCompletion(std::move(waiters), std::move(reader));
    return true;
  }

  /** Request business cancellation and publish the Core Cancelled terminal. */
  bool cancel()
  {
    {
      std::lock_guard<std::mutex> lock(m_data->mutex);
      if (m_data->terminal)
        return false;
    }
    std::exception_ptr cancellation;
    try {
      cancellation = detail::mapStateError(m_data, OperationErrorCode::Cancelled,
                                            "operation was cancelled");
    }
    catch (...) {
      std::lock_guard<std::mutex> lock(m_data->mutex);
      if (m_data->terminal)
        return false;
      throw;
    }
    const bool won = fail(std::move(cancellation));
    if (won && m_data->cancelBusiness) {
      try { m_data->cancelBusiness(); } catch (...) {}
    }
    return won;
  }

  /** Return whether no terminal result has been published yet. */
  bool isPending() const noexcept
  {
    std::lock_guard<std::mutex> lock(m_data->mutex);
    return !m_data->terminal;
  }

  /** Return whether this state has reached a terminal result. */
  bool isTerminal() const noexcept
  {
    std::lock_guard<std::mutex> lock(m_data->mutex);
    return m_data->terminal;
  }

  /** Return whether the terminal result was successful. */
  bool succeeded() const noexcept
  {
    std::lock_guard<std::mutex> lock(m_data->mutex);
    return m_data->terminal && m_data->succeeded;
  }

  /** Return the terminal failure without changing its ownership. */
  std::exception_ptr failure() const noexcept
  {
    std::lock_guard<std::mutex> lock(m_data->mutex);
    return m_data->failure;
  }

  /** Wait for the result; a local timeout never changes the operation terminal. */
  Result result(std::chrono::milliseconds timeout)
  {
    if (timeout.count() < 0)
      throw std::invalid_argument("operation result timeout is negative");
    const bool onWorker = m_data->runtime->isWorkerThread();
    std::unique_lock<std::mutex> lock(m_data->mutex);
    if (!m_data->terminal) {
      if (timeout.count() == 0)
        throw OperationError(OperationErrorCode::Timeout, "operation result timed out");
      if (onWorker)
        throw OperationError(OperationErrorCode::WouldDeadlock,
                             "operation result wait from its worker would deadlock");
      if (!m_data->condition.wait_for(lock, timeout, [&] { return m_data->terminal; }))
        throw OperationError(OperationErrorCode::Timeout, "operation result timed out");
    }
    if (m_data->failure) std::rethrow_exception(m_data->failure);
    return *m_data->result;
  }

  /** Register one completion callback, subject to the shared 64-slot limit. */
  OperationSubscription onCompletion(typename Data::CompletionCallback callback)
  {
    if (!callback) throw std::invalid_argument("completion callback is empty");
    auto control = std::make_shared<detail::SubscriptionControl>();
    typename Data::CompletionWaiter waiter;
    waiter.control = control;
    waiter.callback = std::move(callback);
    bool immediate = false;
    m_data->runtime->withOpenRegistration([&] {
      std::lock_guard<std::mutex> lock(m_data->mutex);
      if (m_data->terminal) immediate = true;
      else {
        if (m_data->completionWaiters.size() >= 64)
          throw OperationError(OperationErrorCode::Capacity,
                               "completion subscription capacity exceeded");
        const auto id = ++m_data->nextId;
        control->cancelFn = [data = m_data, id] { removeCompletion(data, id); };
        m_data->completionWaiters.emplace(id, std::move(waiter));
      }
    });
    if (immediate) dispatchOneShot(control, std::move(waiter.callback));
    return OperationSubscription(std::move(control));
  }

  /** Register a timed result callback without cancelling business work on timeout. */
  OperationSubscription resultAsync(std::chrono::milliseconds timeout,
                                    typename Data::ResultCallback callback)
  {
    if (timeout.count() < 0) throw std::invalid_argument("operation result timeout is negative");
    if (!callback) throw std::invalid_argument("result callback is empty");
    auto control = std::make_shared<detail::SubscriptionControl>();
    typename Data::CompletionWaiter waiter;
    waiter.control = control;
    waiter.resultCallback = std::move(callback);
    bool immediate = false;
    std::uint64_t id = 0;
    m_data->runtime->withOpenRegistration([&] {
      std::lock_guard<std::mutex> lock(m_data->mutex);
      if (m_data->terminal) immediate = true;
      else {
        if (m_data->completionWaiters.size() >= 64)
          throw OperationError(OperationErrorCode::Capacity,
                               "completion subscription capacity exceeded");
        id = ++m_data->nextId;
        control->cancelFn = [data = m_data, id] { removeCompletion(data, id); };
        m_data->completionWaiters.emplace(id, std::move(waiter));
      }
    });
    if (immediate) {
      std::optional<Result> result;
      std::exception_ptr deliveryError = currentError();
      try {
        result = currentResult();
      }
      catch (...) {
        deliveryError = std::current_exception();
      }
      dispatchResult(control, std::move(waiter.resultCallback),
                    std::move(result), deliveryError);
    }
    else {
      try { installCompletionTimer(id, timeout); }
      catch (...) {
        removeCompletion(m_data, id);
        throw;
      }
    }
    return OperationSubscription(std::move(control));
  }

  /**
   * Publish one bounded event; consumed prefixes are reclaimed and false
   * marks a sticky reader gap when unread history exceeds the cap.
   */
  bool publish(Event event, std::size_t encodedBytes)
  {
    return publishImpl(std::move(event), encodedBytes, false, {});
  }

  /**
   * Publish a non-terminal event only while an external owner admits it.
   * The admission callback runs under the Core state mutex, so an application
   * state transition can linearize against event insertion without making
   * Core depend on that application's state or locking order.
   */
  bool publishIf(Event event, std::size_t encodedBytes,
                 std::function<bool()> admission)
  {
    if (!admission)
      throw std::invalid_argument("event admission callback is empty");
    return publishImpl(std::move(event), encodedBytes, false, std::move(admission));
  }

  /** Publish the one terminal observation after completion has been claimed. */
  bool publishTerminal(Event event, std::size_t encodedBytes)
  {
    return publishImpl(std::move(event), encodedBytes, true, {});
  }

private:
  bool publishImpl(Event event, std::size_t encodedBytes, bool allowTerminal)
  {
    return publishImpl(std::move(event), encodedBytes, allowTerminal, {});
  }

  bool publishImpl(Event event, std::size_t encodedBytes, bool allowTerminal,
                   std::function<bool()> admission)
  {
    std::optional<typename Base::PendingRead> reader;
    std::optional<Event> readerEvent;
    std::exception_ptr readerError;
    bool overflowed = false;
    bool observerDeliveryDropped = false;
    std::vector<std::tuple<std::shared_ptr<detail::SubscriptionControl>,
                           std::function<void(const Event&)>, Event>> observers;
    {
      std::lock_guard<std::mutex> lock(m_data->mutex);
      if (admission && !admission())
        return false;
      // The bounded history is a total-stream cap.  It intentionally keeps
      // observer replay memory bounded even when a reader keeps up; once the
      // retained history is exhausted, EventGap is sticky for that state.
      if (m_data->terminal && (!allowTerminal || m_data->terminalEventPublished))
        return false;
      m_data->reclaimConsumedLocked();
      if (m_data->eventGap || m_data->events.size() >= 1024 ||
          encodedBytes > 16U * 1024U * 1024U - m_data->eventBytes) {
        m_data->eventGap = true;
        overflowed = true;
        if (m_data->pendingRead && m_data->readerCursor >= m_data->events.size()) {
          reader = std::move(m_data->pendingRead);
          m_data->pendingRead.reset();
          const auto generation = reader->generation;
          reader->commit = [data = std::static_pointer_cast<Base>(m_data), generation] {
            std::lock_guard<std::mutex> lock(data->mutex);
            if (data->readerId == generation)
              data->readerDispatchPending = false;
          };
          reader->rollback = [data = std::static_pointer_cast<Base>(m_data), generation] {
            std::lock_guard<std::mutex> lock(data->mutex);
            if (data->readerId == generation)
              data->readerDispatchPending = false;
          };
        }
      }
      else {
        m_data->events.push_back({std::move(event), encodedBytes});
        m_data->eventBytes += encodedBytes;
        if (allowTerminal)
          m_data->terminalEventPublished = true;
        // Move the reliable reader out before touching the best-effort
        // observer delivery list.  Copying an observer callback or Event can
        // allocate; that failure must not strand a pending reader behind a
        // terminal event.  The reader owns the terminal delivery decision,
        // while observer delivery is explicitly lossy/diagnostic.
        if (m_data->pendingRead && m_data->readerCursor < m_data->events.size()) {
          const auto index = static_cast<std::size_t>(m_data->readerCursor);
          // Claim the pending reader before copying the event.  Event is
          // caller-defined and its copy may allocate/throw; leaving the
          // reader in the state while that copy runs would let the following
          // Core completion turn the pending read into a permanent timeout.
          reader = std::move(m_data->pendingRead);
          m_data->pendingRead.reset();
          m_data->readerDispatchPending = true;
          const auto generation = m_data->readerId;
          reader->generation = generation;
          try {
            readerEvent = m_data->events[index].event;
          }
          catch (...) {
            // Resolve the claimed read with an explicit stream failure.  A
            // later read may retry the retained event, but it must never
            // remain parked until its timer reports a misleading timeout.
            readerError = detail::operationException(
              OperationErrorCode::EventGap, "event reader terminal delivery failed");
            m_data->readerGapPending = true;
            m_data->readerGapReported = false;
          }
          const bool readerEventCopied = readerEvent.has_value();
          try {
            reader->commit = [data = std::static_pointer_cast<Base>(m_data), index, generation,
                              readerEventCopied] {
              std::lock_guard<std::mutex> lock(data->mutex);
              if (!data->readerDispatchPending || data->readerId != generation)
                return;
              if (!readerEventCopied) {
                // Keep the cursor on the retained event and make the copy
                // failure sticky for this reader.  A later read gets
                // STREAM_GAP instead of replaying a misleading EOF or timeout.
                data->readerGapPending = true;
                data->readerGapReported = false;
                data->readerDispatchPending = false;
                return;
              }
              if (data->readerActive && data->readerCursor == index) {
                ++data->readerCursor;
                data->readerDispatchPending = false;
                data->reclaimConsumedLocked();
              }
            };
            reader->rollback = [data = std::static_pointer_cast<Base>(m_data), generation] {
              std::lock_guard<std::mutex> lock(data->mutex);
              if (data->readerId == generation)
                data->readerDispatchPending = false;
            };
          }
          catch (...) {
            reader->commit = {};
            reader->rollback = {};
            readerEvent.reset();
            readerError = detail::operationException(
              OperationErrorCode::EventGap, "event reader delivery setup failed");
            m_data->readerGapPending = true;
            m_data->readerGapReported = false;
            m_data->readerDispatchPending = true;
          }
        }
        try {
          for (const auto& entry : m_data->observers) {
            if (entry.second.nextEvent < m_data->events.size())
              observers.emplace_back(entry.second.control, entry.second.callback,
                                     m_data->events.back().event);
          }
        }
        catch (...) {
          observers.clear();
          ++m_data->observerDropped;
          observerDeliveryDropped = true;
        }
        for (auto& entry : m_data->observers) {
          if (entry.second.nextEvent < m_data->events.size())
            entry.second.nextEvent = m_data->events.size();
        }
      }
    }
    if (reader) {
      const auto generation = reader->generation;
      try {
        detail::dispatchReader(std::static_pointer_cast<Base>(m_data), std::move(*reader),
                               std::move(readerEvent), readerError ? readerError : (overflowed
                                 ? detail::mapChannelErrorNoThrow(
                                     std::static_pointer_cast<Base>(m_data),
                                     OperationErrorCode::EventGap,
                                     "operation event buffer exceeded its capacity")
                                 : std::exception_ptr{}));
      }
      catch (...) {
        try {
          std::lock_guard<std::mutex> lock(m_data->mutex);
          if (m_data->readerId == generation) {
            m_data->eventGap = true;
            m_data->readerGapPending = true;
            m_data->readerGapReported = false;
            m_data->readerDispatchPending = false;
          }
        }
        catch (...) {}
      }
    }
    if (overflowed) {
      m_data->condition.notify_all();
      return false;
    }
    for (auto& observer : observers) {
      try {
        dispatchObserver(std::get<0>(observer), std::get<1>(observer),
                        std::get<2>(observer));
      }
      catch (...) {
        std::lock_guard<std::mutex> lock(m_data->mutex);
        ++m_data->observerDropped;
        observerDeliveryDropped = true;
      }
    }
    (void)observerDeliveryDropped;
    m_data->condition.notify_all();
    return true;
  }

public:
  /** Register the independent best-effort observer channel. */
  OperationSubscription observe(std::function<void(const Event&)> callback)
  {
    if (!callback) throw std::invalid_argument("observer callback is empty");
    auto control = std::make_shared<detail::SubscriptionControl>();
    std::uint64_t id;
    auto replayCallback = callback;
    std::vector<Event> replay;
    m_data->runtime->withOpenRegistration([&] {
      std::lock_guard<std::mutex> lock(m_data->mutex);
      if (m_data->observers.size() >= 64)
        throw OperationError(OperationErrorCode::Capacity,
                             "observer subscription capacity exceeded");
      id = ++m_data->nextId;
      control->cancelFn = [data = m_data, id] { removeObserver(data, id); };
      typename Base::Observer observer;
      observer.control = control;
      observer.callback = std::move(callback);
      observer.nextEvent = m_data->events.size();
      for (const auto& record : m_data->events)
        replay.push_back(record.event);
      m_data->observers.emplace(id, std::move(observer));
    });
    for (auto& event : replay)
      dispatchObserver(control, replayCallback, std::move(event));
    return OperationSubscription(std::move(control));
  }

  /** Open the single reliable event reader. */
  OperationReader<Event> openReader();

  /** Return the number of best-effort observer deliveries that were dropped. */
  std::uint64_t observationDropped() const
  {
    std::lock_guard<std::mutex> lock(m_data->mutex);
    return m_data->observerDropped;
  }

  /** Mark the reliable stream truncated without changing the business result. */
  void failReader(OperationErrorCode code, const std::string& message) noexcept
  {
    std::optional<typename Base::PendingRead> reader;
    {
      std::lock_guard<std::mutex> lock(m_data->mutex);
      m_data->eventGap = true;
      m_data->readerGapPending = true;
      m_data->readerGapReported = false;
      if (m_data->pendingRead) {
        reader = std::move(m_data->pendingRead);
        m_data->pendingRead.reset();
        // Keep the gate until dispatchReader's callback takes execution
        // ownership.  The no-closure path clears it at callback start or via
        // the generation fallback if dispatch setup fails.
        m_data->readerDispatchPending = true;
      }
    }
    m_data->condition.notify_all();
    if (!reader)
      return;
    const auto error = detail::mapChannelErrorNoThrow(
      std::static_pointer_cast<Base>(m_data), code, message);
    const auto generation = reader->generation;
    try {
      detail::dispatchReader(std::static_pointer_cast<Base>(m_data), std::move(*reader), {}, error);
    }
    catch (...) {
      // Gap flags and pending-reader ownership are already retired.  Always
      // clear the dispatch marker if callback/task allocation fails so a
      // later read observes the sticky gap instead of READ_IN_PROGRESS.
      try {
        std::lock_guard<std::mutex> lock(m_data->mutex);
        if (m_data->readerId == generation)
          m_data->readerDispatchPending = false;
      }
      catch (...) {}
    }
  }

private:
  std::optional<Result> currentResult() const
  {
    std::lock_guard<std::mutex> lock(m_data->mutex);
    return m_data->result;
  }

  std::exception_ptr currentError() const
  {
    std::lock_guard<std::mutex> lock(m_data->mutex);
    return m_data->failure;
  }

  static void removeCompletion(const std::shared_ptr<Data>& data, std::uint64_t id) noexcept
  {
    std::function<void()> timer;
    {
      std::lock_guard<std::mutex> lock(data->mutex);
      auto it = data->completionWaiters.find(id);
      if (it == data->completionWaiters.end()) return;
      timer = std::move(it->second.cancelTimer);
      data->completionWaiters.erase(it);
    }
    if (timer) timer();
  }

  static void removeObserver(const std::shared_ptr<Data>& data, std::uint64_t id) noexcept
  {
    std::lock_guard<std::mutex> lock(data->mutex);
    data->observers.erase(id);
  }

  void installCompletionTimer(std::uint64_t id, std::chrono::milliseconds timeout)
  {
    OperationRuntime::WorkTicket timerTicket = m_data->runtime->acquire();
    auto timer = m_data->runtime->scheduleAt(
      timerTicket, std::chrono::steady_clock::now() + timeout,
      [data = m_data, id] { timeoutCompletion(data, id); });
    std::lock_guard<std::mutex> lock(m_data->mutex);
    auto it = m_data->completionWaiters.find(id);
    if (it != m_data->completionWaiters.end()) it->second.cancelTimer = std::move(timer);
    else timer();
  }

  static void timeoutCompletion(const std::shared_ptr<Data>& data, std::uint64_t id)
  {
    typename Data::CompletionWaiter waiter;
    {
      std::lock_guard<std::mutex> lock(data->mutex);
      auto it = data->completionWaiters.find(id);
      if (it == data->completionWaiters.end() || data->terminal) return;
      waiter = std::move(it->second);
      data->completionWaiters.erase(it);
    }
    const auto error = detail::mapStateError(data, OperationErrorCode::Timeout,
                                              "operation result timed out");
    dispatchResultStatic(data, waiter.control, std::move(waiter.resultCallback), {}, error);
  }

  static void dispatchOneShotStatic(const std::shared_ptr<Data>& data,
                                    const std::shared_ptr<detail::SubscriptionControl>& control,
                                    std::function<void()> callback)
  {
    auto invoke = [control, callback = std::move(callback)]() mutable {
      if (!control->begin()) return;
      try { callback(); } catch (...) {}
      control->end(true);
    };
    detail::dispatchTask(std::static_pointer_cast<Base>(data), std::move(invoke));
  }

  void dispatchOneShot(const std::shared_ptr<detail::SubscriptionControl>& control,
                      std::function<void()> callback)
  { dispatchOneShotStatic(m_data, control, std::move(callback)); }

  static void dispatchResultStatic(const std::shared_ptr<Data>& data,
                                   const std::shared_ptr<detail::SubscriptionControl>& control,
                                   typename Data::ResultCallback callback,
                                   std::optional<Result> result, std::exception_ptr error)
  {
    auto invoke = [control, callback = std::move(callback), result = std::move(result), error]() mutable {
      if (!control->begin()) return;
      try { callback(std::move(result), error); } catch (...) {}
      control->end(true);
    };
    detail::dispatchTask(std::static_pointer_cast<Base>(data), std::move(invoke));
  }

  void dispatchResult(const std::shared_ptr<detail::SubscriptionControl>& control,
                      typename Data::ResultCallback callback,
                      std::optional<Result> result, std::exception_ptr error)
  { dispatchResultStatic(m_data, control, std::move(callback), std::move(result), error); }

  void dispatchObserver(const std::shared_ptr<detail::SubscriptionControl>& control,
                        std::function<void(const Event&)> callback, Event event)
  {
    auto invoke = [control, callback = std::move(callback), event = std::move(event)]() mutable {
      if (!control->begin()) return;
      try { callback(event); } catch (...) {}
      control->end(false);
    };
    detail::dispatchTask(std::static_pointer_cast<Base>(m_data), std::move(invoke));
  }

  void dispatchCompletion(std::vector<typename Data::CompletionWaiter> waiters,
                          std::optional<typename Base::PendingRead> reader)
  {
    for (auto& waiter : waiters) {
      if (waiter.cancelTimer) waiter.cancelTimer();
      if (waiter.resultCallback) {
        std::optional<Result> result;
        std::exception_ptr deliveryError = currentError();
        try {
          result = currentResult();
        }
        catch (...) {
          // Result is user-owned and may have a throwing copy constructor.
          // Convert a failed snapshot into this waiter's delivery error so
          // later waiters still receive exactly one callback.
          deliveryError = std::current_exception();
        }
        dispatchResult(waiter.control, std::move(waiter.resultCallback),
                       std::move(result), deliveryError);
      }
      else if (waiter.callback)
        dispatchOneShot(waiter.control, std::move(waiter.callback));
    }
    if (reader)
      detail::dispatchReader(std::static_pointer_cast<Base>(m_data), std::move(*reader), {}, currentError());
    releaseTicketIfIdle();
  }

  void releaseTicketIfIdle() noexcept
  {
    std::optional<OperationRuntime::WorkTicket> ticket;
    {
      std::lock_guard<std::mutex> lock(m_data->mutex);
      if (m_data->terminal && !m_data->readerActive && !m_data->pendingRead)
        ticket = std::move(m_data->ticket);
    }
    ticket.reset();
  }

  std::shared_ptr<Data> m_data;
  template<typename E> friend class OperationReader;
};

/** Move-only reliable reader over an OperationState's bounded event buffer. */
template<typename Event>
class OperationReader
{
public:
  using Callback = std::function<void(std::optional<Event>, std::exception_ptr)>;
  using Data = detail::OperationChannelData<Event>;

  OperationReader() noexcept = default;
  OperationReader(OperationReader&& other) noexcept
    : m_data(std::move(other.m_data)), m_id(std::exchange(other.m_id, 0)),
      m_open(std::exchange(other.m_open, false)) {}
  OperationReader& operator=(OperationReader&& other) noexcept
  {
    if (this != &other) {
      close();
      m_data = std::move(other.m_data);
      m_id = std::exchange(other.m_id, 0);
      m_open = std::exchange(other.m_open, false);
    }
    return *this;
  }
  OperationReader(const OperationReader&) = delete;
  OperationReader& operator=(const OperationReader&) = delete;
  ~OperationReader() noexcept { close(); }

  /** Read the next event, EOF, or a terminal error with a local timeout. */
  std::optional<Event> next(std::chrono::milliseconds timeout)
  {
    if (timeout.count() < 0) throw std::invalid_argument("event reader timeout is negative");
    if (!m_data)
      throw OperationError(OperationErrorCode::Closed, "event reader is closed");
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const bool onWorker = m_data->runtime->isWorkerThread();
    std::unique_lock<std::mutex> lock(m_data->mutex);
    if (!m_open)
      throw OperationError(OperationErrorCode::Closed, "event reader is closed");
    ensureOpenLocked();
    for (;;) {
      if (m_data->readerDispatchPending)
        throw OperationError(OperationErrorCode::Capacity,
                             "an event reader delivery is already in progress");
      if (m_data->readerGapPending) {
        m_data->readerGapReported = true;
        std::rethrow_exception(detail::mapChannelError(
          m_data, OperationErrorCode::EventGap,
          "event reader opened after its history exceeded capacity"));
      }
      if (m_data->readerCursor < m_data->events.size()) {
        const auto index = static_cast<std::size_t>(m_data->readerCursor);
        Event event = m_data->events[index].event;
        ++m_data->readerCursor;
        m_data->reclaimConsumedLocked();
        return event;
      }
      if (m_data->eventGap && !m_data->readerGapReported) {
        m_data->readerGapPending = true;
        m_data->readerGapReported = true;
        std::rethrow_exception(detail::mapChannelError(m_data, OperationErrorCode::EventGap,
                                                       "operation event buffer exceeded its capacity"));
      }
      if (m_data->terminal) {
        if (m_data->failure) std::rethrow_exception(m_data->failure);
        return std::nullopt;
      }
      if (timeout.count() == 0)
        throw OperationError(OperationErrorCode::Timeout, "event reader timed out");
      if (onWorker)
        throw OperationError(OperationErrorCode::WouldDeadlock,
                             "event reader wait from its worker would deadlock");
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline)
        throw OperationError(OperationErrorCode::Timeout, "event reader timed out");
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      if (remaining.count() <= 0)
        throw OperationError(OperationErrorCode::Timeout, "event reader timed out");
      if (!m_data->condition.wait_for(lock, remaining, [&] {
            return m_data->readerCursor < m_data->events.size() || m_data->terminal ||
                   m_data->eventGap || !m_open;
          }))
        throw OperationError(OperationErrorCode::Timeout, "event reader timed out");
      ensureOpenLocked();
    }
  }

  /** Register one asynchronous read; only one read may be in flight. */
  OperationSubscription nextAsync(std::chrono::milliseconds timeout, Callback callback)
  {
    if (timeout.count() < 0) throw std::invalid_argument("event reader timeout is negative");
    if (!callback) throw std::invalid_argument("event reader callback is empty");
    if (!m_data)
      throw OperationError(OperationErrorCode::Closed, "event reader is closed");
    auto control = std::make_shared<detail::SubscriptionControl>();
    std::optional<Event> immediate;
    std::exception_ptr immediateError;
    std::uint64_t id = 0;
    m_data->runtime->withOpenRegistration([&] {
      std::lock_guard<std::mutex> lock(m_data->mutex);
      if (!m_open)
        throw OperationError(OperationErrorCode::Closed, "event reader is closed");
      ensureOpenLocked();
      if (m_data->pendingRead)
        throw OperationError(OperationErrorCode::Capacity,
                             "an event reader read is already in progress");
      if (m_data->readerDispatchPending)
        throw OperationError(OperationErrorCode::Capacity,
                             "an event reader delivery is already in progress");
      if (m_data->readerGapPending) {
        m_data->readerGapReported = true;
        immediateError = detail::mapChannelError(
          m_data, OperationErrorCode::EventGap,
          "event reader opened after its history exceeded capacity");
      }
      else if (m_data->readerCursor < m_data->events.size()) {
        immediate = m_data->events[static_cast<std::size_t>(m_data->readerCursor)].event;
        m_data->readerDispatchPending = true;
      }
      else if (m_data->eventGap && !m_data->readerGapReported) {
        m_data->readerGapPending = true;
        m_data->readerGapReported = true;
        immediateError = detail::mapChannelError(m_data, OperationErrorCode::EventGap,
                                                 "operation event buffer exceeded its capacity");
      }
      else if (m_data->terminal)
        immediateError = m_data->failure;
      else {
        id = ++m_data->nextId;
        typename Data::PendingRead pending;
        pending.id = id;
        pending.generation = m_data->readerId;
        pending.control = control;
        pending.callback = std::move(callback);
        m_data->pendingRead = std::move(pending);
        control->cancelFn = [data = m_data, id] { cancelPending(data, id); };
      }
    });
    if (id != 0) {
      try { installReadTimer(id, timeout); }
      catch (...) {
        cancelPending(m_data, id);
        throw;
      }
      return OperationSubscription(std::move(control));
    }
    typename Data::PendingRead ready;
    ready.control = control;
    ready.callback = std::move(callback);
    if (immediate) {
      const auto index = static_cast<std::size_t>(m_data->readerCursor);
      const auto generation = m_data->readerId;
      ready.generation = generation;
      ready.commit = [data = m_data, index, generation] {
        std::lock_guard<std::mutex> lock(data->mutex);
        if (data->readerDispatchPending && data->readerId == generation &&
            data->readerActive && data->readerCursor == index) {
          ++data->readerCursor;
          data->readerDispatchPending = false;
          data->reclaimConsumedLocked();
        }
      };
      ready.rollback = [data = m_data, generation] {
        std::lock_guard<std::mutex> lock(data->mutex);
        if (data->readerId == generation)
          data->readerDispatchPending = false;
      };
    }
    detail::dispatchReader(m_data, std::move(ready), std::move(immediate), immediateError);
    return OperationSubscription(std::move(control));
  }

  /** Close this reader and release its single-reader lease. */
  void close() noexcept
  {
    if (!m_data) return;
    std::optional<typename Data::PendingRead> pending;
    {
      std::lock_guard<std::mutex> lock(m_data->mutex);
      if (!m_open) return;
      if (m_data->readerActive && m_data->readerId == m_id) {
        m_data->reclaimConsumedLocked();
        m_data->readerDispatchPending = false;
        m_data->readerActive = false;
        m_open = false;
        if (m_data->pendingRead) {
          pending = std::move(m_data->pendingRead);
          m_data->pendingRead.reset();
        }
      }
      else m_open = false;
    }
    if (pending) {
      const auto generation = pending->generation;
      pending->commit = [data = m_data, generation] {
        std::lock_guard<std::mutex> lock(data->mutex);
        if (data->readerId == generation)
          data->readerDispatchPending = false;
      };
      pending->rollback = pending->commit;
      std::exception_ptr closedError;
      try {
        closedError = detail::mapChannelError(m_data, OperationErrorCode::Closed,
                                              "event reader is closed");
      }
      catch (...) {
        closedError = detail::operationException(OperationErrorCode::Closed,
                                                 "event reader is closed");
      }
      try {
        detail::dispatchReader(m_data, std::move(*pending), {}, closedError);
      }
      catch (...) {
        // close() is noexcept; the reader lease and timer are already retired.
      }
    }
    std::optional<OperationRuntime::WorkTicket> ticket;
    {
      std::lock_guard<std::mutex> lock(m_data->mutex);
      if (m_data->terminal && !m_data->pendingRead)
        ticket = std::move(m_data->ticket);
    }
    ticket.reset();
    m_data->condition.notify_all();
  }

private:
  void ensureOpenLocked() const
  {
    if (!m_open || !m_data || !m_data->readerActive || m_data->readerId != m_id)
      throw OperationError(OperationErrorCode::Closed, "event reader is closed");
  }

  static void cancelPending(const std::shared_ptr<Data>& data, std::uint64_t id) noexcept
  {
    std::function<void()> timer;
    {
      std::lock_guard<std::mutex> lock(data->mutex);
      if (data->pendingRead && data->pendingRead->id == id) {
        timer = std::move(data->pendingRead->cancelTimer);
        data->pendingRead.reset();
      }
    }
    if (timer) timer();
  }

  void installReadTimer(std::uint64_t id, std::chrono::milliseconds timeout)
  {
    if (timeout.count() == 0) {
      timeoutPending(id);
      return;
    }
    OperationRuntime::WorkTicket timerTicket = m_data->runtime->acquire();
    auto timer = m_data->runtime->scheduleAt(
      timerTicket, std::chrono::steady_clock::now() + timeout,
      [data = m_data, id] { timeoutPendingStatic(data, id); });
    std::lock_guard<std::mutex> lock(m_data->mutex);
    if (m_data->pendingRead && m_data->pendingRead->id == id)
      m_data->pendingRead->cancelTimer = std::move(timer);
    else timer();
  }

  void timeoutPending(std::uint64_t id) { timeoutPendingStatic(m_data, id); }

  static void timeoutPendingStatic(const std::shared_ptr<Data>& data, std::uint64_t id)
  {
    std::optional<typename Data::PendingRead> pending;
    {
      std::lock_guard<std::mutex> lock(data->mutex);
      if (!data->pendingRead || data->pendingRead->id != id) return;
      pending = std::move(data->pendingRead);
      data->pendingRead.reset();
    }
    if (pending)
      detail::dispatchReader(data, std::move(*pending), {},
        detail::mapChannelErrorNoThrow(data, OperationErrorCode::Timeout,
                                       "event reader timed out"));
  }

  std::shared_ptr<Data> m_data;
  std::uint64_t m_id = 0;
  bool m_open = false;
  template<typename Result, typename EventT> friend class OperationState;
};

template<typename Result, typename Event>
OperationReader<Event>
OperationState<Result, Event>::openReader()
{
  OperationReader<Event> reader;
  m_data->runtime->withOpenRegistration([&] {
    std::lock_guard<std::mutex> lock(m_data->mutex);
    if (m_data->readerActive)
      throw OperationError(OperationErrorCode::Capacity, "only one event reader is allowed");
    m_data->readerActive = true;
    m_data->readerId = ++m_data->nextId;
    m_data->readerCursor = 0;
    m_data->readerGapPending = m_data->eventGap;
    m_data->readerGapReported = false;
    reader.m_data = std::static_pointer_cast<detail::OperationChannelData<Event>>(m_data);
    reader.m_id = m_data->readerId;
    reader.m_open = true;
  });
  return reader;
}

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_OPERATION_STATE_HPP
