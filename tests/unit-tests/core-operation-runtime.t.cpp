#include "tests/boost-test.hpp"

#include "ndn-service-framework/OperationState.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>

namespace ndn_service_framework::test {

struct ThrowingEvent
{
  static inline std::atomic<bool> throwOnCopy{false};

  explicit ThrowingEvent(int value)
    : value(value)
  {
  }

  ThrowingEvent(const ThrowingEvent& other)
    : value(other.value)
  {
    if (throwOnCopy.load())
      throw std::runtime_error("event copy failed");
  }

  ThrowingEvent(ThrowingEvent&&) noexcept = default;
  ThrowingEvent& operator=(const ThrowingEvent&) = default;
  ThrowingEvent& operator=(ThrowingEvent&&) noexcept = default;

  int value;
};

struct ThrowingResult
{
  static inline std::atomic<bool> throwOnCopy{false};

  explicit ThrowingResult(int value)
    : value(value)
  {
  }

  ThrowingResult(const ThrowingResult& other)
    : value(other.value)
  {
    if (throwOnCopy.load())
      throw std::runtime_error("result copy failed");
  }

  ThrowingResult(ThrowingResult&&) noexcept = default;
  ThrowingResult& operator=(const ThrowingResult&) = default;
  ThrowingResult& operator=(ThrowingResult&&) noexcept = default;

  int value;
};

BOOST_AUTO_TEST_SUITE(Spec185CoreOperation)

BOOST_AUTO_TEST_CASE(RuntimeQueuesWorkAndDrainsAfterTicketRelease)
{
  auto runtime = OperationRuntime::create();
  auto ticket = runtime->acquire();
  std::promise<void> ran;
  auto future = ran.get_future();
  runtime->post(ticket, [&ran] { ran.set_value(); });
  BOOST_REQUIRE(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  ticket = OperationRuntime::WorkTicket{};
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(CompletionHasOneTerminalAuthorityAndLocalTimeout)
{
  auto runtime = OperationRuntime::create();
  OperationState<int, std::string> state(runtime);
  BOOST_CHECK_THROW(state.result(std::chrono::milliseconds(0)), OperationError);
  BOOST_CHECK(state.fail(detail::operationException(OperationErrorCode::Cancelled, "cancelled")));
  BOOST_CHECK(!state.complete(7));
  BOOST_CHECK_THROW(state.result(std::chrono::milliseconds(100)), OperationError);
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(CompletionCallbackCanUnsubscribeAndDrainCallbackCanClose)
{
  auto runtime = OperationRuntime::create();
  OperationState<int, std::string> state(runtime);
  std::atomic<int> called{0};
  auto subscription = state.onCompletion([&] { ++called; });
  subscription.unsubscribe();
  BOOST_CHECK(state.complete(11));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  BOOST_CHECK_EQUAL(called.load(), 0);
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(AsyncTimeoutIsLocalAndDrainNotificationIsCancellable)
{
  auto runtime = OperationRuntime::create();
  OperationState<int, std::string> state(runtime);
  std::promise<OperationErrorCode> timedOut;
  auto timedFuture = timedOut.get_future();
  auto resultSubscription = state.resultAsync(std::chrono::milliseconds(5),
    [&timedOut] (std::optional<int> result, std::exception_ptr error) {
      if (result) {
        timedOut.set_value(OperationErrorCode::Closed);
        return;
      }
      try { if (error) std::rethrow_exception(error); }
      catch (const OperationError& operationError) {
        timedOut.set_value(operationError.code());
        return;
      }
      timedOut.set_value(OperationErrorCode::Closed);
    });
  BOOST_REQUIRE(timedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK(timedFuture.get() == OperationErrorCode::Timeout);
  BOOST_CHECK_THROW(state.result(std::chrono::milliseconds(0)), OperationError);
  state.cancel();
  auto ticket = runtime->acquire();
  std::promise<bool> drained;
  auto drainFuture = drained.get_future();
  auto drainSubscription = runtime->drainAsync(std::chrono::seconds(1),
    [&drained] (bool value) { drained.set_value(value); });
  ticket = OperationRuntime::WorkTicket{};
  BOOST_REQUIRE(drainFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK(drainFuture.get());
  drainSubscription.unsubscribe();
  BOOST_CHECK(runtime->drain(std::chrono::milliseconds(0)));
  resultSubscription.unsubscribe();
}

BOOST_AUTO_TEST_CASE(NonClosingDrainNotificationLeavesCoreOpen)
{
  auto runtime = OperationRuntime::create();
  auto ticket = runtime->acquire();
  std::promise<bool> timedOut;
  auto timedOutFuture = timedOut.get_future();
  auto subscription = runtime->drainAsync(
    std::chrono::milliseconds(20), [&timedOut] (bool value) { timedOut.set_value(value); }, false);
  BOOST_REQUIRE(timedOutFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK(!timedOutFuture.get());
  BOOST_CHECK(!runtime->isClosed());
  ticket = OperationRuntime::WorkTicket{};
  subscription.cancel();

  std::promise<bool> completed;
  auto future = completed.get_future();
  auto retry = runtime->drainAsync(
    std::chrono::seconds(1), [&completed] (bool value) { completed.set_value(value); }, false);
  BOOST_REQUIRE(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK(future.get());
  BOOST_CHECK(!runtime->isClosed());
  auto retryTicket = runtime->acquire();
  retryTicket = OperationRuntime::WorkTicket{};
  retry.cancel();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(ThrowingExtraReadyFailsDrainWithoutStallingTheWorker)
{
  auto runtime = OperationRuntime::create();
  std::promise<bool> completed;
  auto future = completed.get_future();
  auto subscription = runtime->drainAsync(
    std::chrono::seconds(1), [&completed] (bool value) { completed.set_value(value); },
    false, [] () -> bool { throw std::runtime_error("external owner probe failed"); });
  BOOST_REQUIRE(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK(!future.get());
  subscription.cancel();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(TerminalReplayUsesAShortLivedCoreTicket)
{
  auto runtime = OperationRuntime::create();
  OperationState<int, std::string> state(runtime);
  BOOST_REQUIRE(state.complete(42));
  std::promise<void> called;
  auto future = called.get_future();
  auto subscription = state.onCompletion([&called] { called.set_value(); });
  BOOST_REQUIRE(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  subscription.unsubscribe();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(MoveAssignmentUnsubscribesTheReplacedSlot)
{
  auto runtime = OperationRuntime::create();
  OperationState<int, std::string> state(runtime);
  std::atomic<int> firstCalls{0};
  std::atomic<int> secondCalls{0};
  auto first = state.onCompletion([&] { ++firstCalls; });
  auto second = state.onCompletion([&] { ++secondCalls; });
  second = std::move(first);
  BOOST_REQUIRE(state.complete(9));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  BOOST_CHECK_EQUAL(firstCalls.load(), 1);
  BOOST_CHECK_EQUAL(secondCalls.load(), 0);
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(SubmitHookFailureStillDeliversExactlyOnce)
{
  std::atomic<int> calls{0};
  std::function<void()> retained;
  auto runtime = OperationRuntime::create([&calls, &retained] (std::function<void()> task) {
    retained = task;
    task();
    ++calls;
    throw std::runtime_error("hook reported after dispatch");
  });
  auto ticket = runtime->acquire();
  std::promise<void> ran;
  auto future = ran.get_future();
  runtime->post(ticket, [&ran] { ran.set_value(); });
  BOOST_REQUIRE(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK_EQUAL(calls.load(), 1);
  ticket = OperationRuntime::WorkTicket{};
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
  retained();
}

BOOST_AUTO_TEST_CASE(DrainSubscriptionsHaveASharedCapacityLimit)
{
  auto runtime = OperationRuntime::create();
  auto ticket = runtime->acquire();
  std::vector<OperationSubscription> subscriptions;
  subscriptions.reserve(64);
  for (std::size_t i = 0; i < 64; ++i)
    subscriptions.push_back(runtime->drainAsync(std::chrono::seconds(1), [] (bool) {}));
  BOOST_CHECK_THROW(runtime->drainAsync(std::chrono::seconds(1), [] (bool) {}), OperationError);
  ticket = OperationRuntime::WorkTicket{};
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(MovedFromReaderIsRejectedWithoutDereferencingNullState)
{
  auto runtime = OperationRuntime::create();
  OperationState<int, std::string> state(runtime);
  auto reader = state.openReader();
  auto moved = std::move(reader);
  BOOST_CHECK_THROW(reader.next(std::chrono::milliseconds(0)), OperationError);
  BOOST_CHECK_THROW(reader.nextAsync(std::chrono::milliseconds(0), [] (auto, auto) {}),
                    OperationError);
  moved.close();
  state.cancel();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(CancelBusinessHookIsNotCalledAfterACompletedTerminal)
{
  auto runtime = OperationRuntime::create();
  std::atomic<int> cancellations{0};
  OperationState<int, std::string> state(runtime, [&] { ++cancellations; });
  BOOST_REQUIRE(state.complete(17));
  BOOST_CHECK(!state.cancel());
  BOOST_CHECK_EQUAL(cancellations.load(), 0);
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(ReaderIsBoundedAndDoesNotTurnGapIntoEof)
{
  auto runtime = OperationRuntime::create();
  OperationState<int, std::string> state(runtime);
  auto reader = state.openReader();
  BOOST_CHECK_THROW(state.openReader(), OperationError);
  BOOST_REQUIRE(state.publish("event", 5));
  auto event = reader.next(std::chrono::milliseconds(0));
  BOOST_REQUIRE(event);
  BOOST_CHECK_EQUAL(*event, "event");
  reader.close();
  OperationState<int, std::string> bounded(runtime);
  auto boundedReader = bounded.openReader();
  for (std::size_t i = 0; i < 1024; ++i)
    BOOST_REQUIRE(bounded.publish("x", 1));
  BOOST_CHECK(!bounded.publish("overflow", 1));
  for (std::size_t i = 0; i < 1024; ++i)
    BOOST_REQUIRE(boundedReader.next(std::chrono::milliseconds(0)));
  BOOST_CHECK_THROW(boundedReader.next(std::chrono::milliseconds(0)), OperationError);
  boundedReader.close();
  state.complete(3);
  bounded.complete(4);
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(WorkerCannotSynchronouslyWaitOnItsOwnOperation)
{
  auto runtime = OperationRuntime::create();
  OperationState<int, std::string> state(runtime);
  auto ticket = runtime->acquire();
  std::promise<bool> rejected;
  auto future = rejected.get_future();
  runtime->post(ticket, [&state, &rejected] {
    try {
      (void)state.result(std::chrono::milliseconds(50));
      rejected.set_value(false);
    }
    catch (const OperationError& error) {
      rejected.set_value(error.code() == OperationErrorCode::WouldDeadlock);
    }
  });
  BOOST_REQUIRE(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK(future.get());
  ticket = OperationRuntime::WorkTicket{};
  state.cancel();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(TimerHoldsDrainBarrierUntilCallbackReturns)
{
  auto runtime = OperationRuntime::create();
  std::promise<void> entered;
  auto enteredFuture = entered.get_future();
  std::promise<void> release;
  auto releaseFuture = release.get_future().share();
  auto enteredOnce = std::make_shared<std::atomic<bool>>(false);
  OperationState<int, std::string> state(
    runtime, {}, {}, [&entered, releaseFuture, enteredOnce] (OperationErrorCode code,
                                                               const std::string& message) {
      if (code == OperationErrorCode::Timeout &&
          !enteredOnce->exchange(true)) {
        entered.set_value();
        releaseFuture.wait();
      }
      return detail::operationException(code, message);
    });
  auto resultSubscription = state.resultAsync(std::chrono::milliseconds(5),
    [] (std::optional<int>, std::exception_ptr) {});
  BOOST_REQUIRE(enteredFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  runtime->close();
  BOOST_CHECK(!runtime->drain(std::chrono::milliseconds(5)));
  release.set_value();
  state.cancel();
  resultSubscription.unsubscribe();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(TimerExceptionsAreIsolatedFromTheWorker)
{
  auto runtime = OperationRuntime::create();
  std::atomic<int> callbacks{0};
  OperationState<int, std::string> state(
    runtime, {}, {}, [] (OperationErrorCode code, const std::string& message) -> std::exception_ptr {
      if (code == OperationErrorCode::Timeout)
        throw std::runtime_error("error mapper failed");
      return detail::operationException(code, message);
    });
  auto subscription = state.resultAsync(std::chrono::milliseconds(5),
    [&callbacks] (std::optional<int>, std::exception_ptr) { ++callbacks; });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  BOOST_CHECK_EQUAL(callbacks.load(), 0);
  BOOST_CHECK(state.cancel());
  subscription.unsubscribe();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(ReaderCloseFallsBackWhenErrorMapperThrows)
{
  auto runtime = OperationRuntime::create();
  OperationState<int, std::string> state(
    runtime, {}, {}, [] (OperationErrorCode code, const std::string& message) -> std::exception_ptr {
      if (code == OperationErrorCode::Closed)
        throw std::runtime_error("error mapper failed");
      return detail::operationException(code, message);
    });
  auto reader = state.openReader();
  std::promise<OperationErrorCode> closed;
  auto closedFuture = closed.get_future();
  auto subscription = reader.nextAsync(std::chrono::seconds(1),
    [&closed] (std::optional<std::string> event, std::exception_ptr error) {
      if (event || !error) {
        closed.set_value(OperationErrorCode::Timeout);
        return;
      }
      try { if (error) std::rethrow_exception(error); }
      catch (const OperationError& operationError) {
        closed.set_value(operationError.code());
        return;
      }
      closed.set_value(OperationErrorCode::Timeout);
    });
  reader.close();
  BOOST_REQUIRE(closedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK(closedFuture.get() == OperationErrorCode::Closed);
  subscription.unsubscribe();
  state.cancel();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(RuntimeWorkerSelfReleaseUsesAJoinBarrier)
{
  auto runtime = OperationRuntime::create();
  auto ticket = runtime->acquire();
  std::promise<void> released;
  auto releasedFuture = released.get_future();
  auto owner = runtime;
  runtime->post(ticket, [owner = std::move(owner), &released] () mutable {
    owner.reset();
    released.set_value();
  });
  ticket = OperationRuntime::WorkTicket{};
  runtime.reset();
  BOOST_REQUIRE(releasedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
}

BOOST_AUTO_TEST_CASE(PendingReaderCompletionRetiresItsTimerBeforeDrain)
{
  auto runtime = OperationRuntime::create();
  OperationState<int, std::string> state(runtime);
  auto reader = state.openReader();
  std::promise<bool> delivered;
  auto deliveredFuture = delivered.get_future();
  auto subscription = reader.nextAsync(std::chrono::seconds(1),
    [&delivered] (std::optional<std::string> event, std::exception_ptr error) {
      delivered.set_value(!event && !error);
    });
  BOOST_REQUIRE(state.complete(1));
  BOOST_REQUIRE(deliveredFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK(deliveredFuture.get());
  reader.close();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::milliseconds(100)));
  subscription.unsubscribe();
}

BOOST_AUTO_TEST_CASE(ReaderWakesWithEventGapOnOverflow)
{
  auto runtime = OperationRuntime::create();
  OperationState<int, std::string> state(runtime);
  auto reader = state.openReader();
  std::promise<OperationErrorCode> outcome;
  auto outcomeFuture = outcome.get_future();
  std::thread waiting([&reader, &outcome] {
    try {
      (void)reader.next(std::chrono::seconds(1));
      outcome.set_value(OperationErrorCode::Closed);
    }
    catch (const OperationError& error) {
      outcome.set_value(error.code());
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  BOOST_CHECK(!state.publish("overflow", 16U * 1024U * 1024U + 1));
  BOOST_REQUIRE(outcomeFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  waiting.join();
  BOOST_CHECK(outcomeFuture.get() == OperationErrorCode::EventGap);
  reader.close();
  state.cancel();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(TerminalStateDoesNotTurnLatePublishIntoEventGap)
{
  auto runtime = OperationRuntime::create();
  OperationState<int, std::string> state(runtime);
  auto reader = state.openReader();
  BOOST_REQUIRE(state.complete(3));
  BOOST_CHECK(!state.publish("late", 1));
  BOOST_CHECK(!reader.next(std::chrono::milliseconds(0)));
  reader.close();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(ImmediateReaderCallbackUsesRuntimeDispatch)
{
  auto runtime = OperationRuntime::create();
  auto blockerTicket = runtime->acquire();
  std::promise<void> blockerStarted;
  auto blockerStartedFuture = blockerStarted.get_future();
  std::promise<void> releaseBlock;
  auto releaseBlockFuture = releaseBlock.get_future().share();
  runtime->post(blockerTicket, [&blockerStarted, releaseBlockFuture] {
    blockerStarted.set_value();
    releaseBlockFuture.wait();
  });
  BOOST_REQUIRE(blockerStartedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  OperationState<int, std::string> state(runtime);
  auto reader = state.openReader();
  BOOST_REQUIRE(state.publish("ready", 5));
  std::atomic<bool> returned{false};
  std::promise<bool> observed;
  auto observedFuture = observed.get_future();
  auto subscription = reader.nextAsync(std::chrono::milliseconds(0),
    [&returned, &observed] (std::optional<std::string> event, std::exception_ptr error) {
      observed.set_value(returned.load() && event && *event == "ready" && !error);
    });
  returned.store(true);
  releaseBlock.set_value();
  blockerTicket = OperationRuntime::WorkTicket{};
  BOOST_REQUIRE(observedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK(observedFuture.get());
  subscription.unsubscribe();
  reader.close();
  state.cancel();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(ReaderOpenedAfterInitialPublishRetainsRequestHistory)
{
  auto runtime = OperationRuntime::create();
  OperationState<int, std::string> state(runtime);
  BOOST_REQUIRE(state.publish("before-reader-1", 15));
  BOOST_REQUIRE(state.publish("before-reader-2", 15));

  auto reader = state.openReader();
  auto first = reader.next(std::chrono::milliseconds(0));
  auto second = reader.next(std::chrono::milliseconds(0));
  BOOST_REQUIRE(first);
  BOOST_REQUIRE(second);
  BOOST_CHECK_EQUAL(*first, "before-reader-1");
  BOOST_CHECK_EQUAL(*second, "before-reader-2");

  reader.close();
  state.cancel();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(ReaderReopenedAfterPublishRetainsUnconsumedHistory)
{
  auto runtime = OperationRuntime::create();
  OperationState<int, std::string> state(runtime);
  auto firstReader = state.openReader();
  BOOST_REQUIRE(state.publish("before-close", 12));
  firstReader.close();
  BOOST_REQUIRE(state.publish("while-closed", 12));

  auto secondReader = state.openReader();
  auto first = secondReader.next(std::chrono::milliseconds(0));
  auto second = secondReader.next(std::chrono::milliseconds(0));
  BOOST_REQUIRE(first);
  BOOST_REQUIRE(second);
  BOOST_CHECK_EQUAL(*first, "before-close");
  BOOST_CHECK_EQUAL(*second, "while-closed");

  secondReader.close();
  state.cancel();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(ReaderUnsubscribeReleasesSelectedDelivery)
{
  auto runtime = OperationRuntime::create();
  auto blockerTicket = runtime->acquire();
  std::promise<void> blockerStarted;
  auto blockerStartedFuture = blockerStarted.get_future();
  std::promise<void> releaseBlock;
  auto releaseBlockFuture = releaseBlock.get_future().share();
  runtime->post(blockerTicket, [&blockerStarted, releaseBlockFuture] {
    blockerStarted.set_value();
    releaseBlockFuture.wait();
  });
  BOOST_REQUIRE(blockerStartedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);

  OperationState<int, std::string> state(runtime);
  auto reader = state.openReader();
  BOOST_REQUIRE(state.publish("selected", 8));
  std::atomic<int> callbacks{0};
  auto subscription = reader.nextAsync(std::chrono::milliseconds(0),
    [&callbacks] (std::optional<std::string>, std::exception_ptr) { ++callbacks; });
  subscription.unsubscribe();

  auto event = reader.next(std::chrono::milliseconds(0));
  BOOST_REQUIRE(event);
  BOOST_CHECK_EQUAL(*event, "selected");
  releaseBlock.set_value();
  blockerTicket = OperationRuntime::WorkTicket{};
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  BOOST_CHECK_EQUAL(callbacks.load(), 0);

  reader.close();
  state.cancel();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(ReaderEventCopyFailurePreservesCursorAndPendingRead)
{
  auto runtime = OperationRuntime::create();
  OperationState<int, ThrowingEvent> state(runtime);
  auto reader = state.openReader();
  BOOST_REQUIRE(state.publish(ThrowingEvent(7), 1));
  ThrowingEvent::throwOnCopy.store(true);
  BOOST_CHECK_THROW(reader.next(std::chrono::milliseconds(0)), std::runtime_error);
  ThrowingEvent::throwOnCopy.store(false);
  auto event = reader.next(std::chrono::milliseconds(0));
  BOOST_REQUIRE(event);
  BOOST_CHECK_EQUAL(event->value, 7);
  reader.close();
  state.cancel();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(PendingReaderEventCopyFailureReportsStickyGap)
{
  auto runtime = OperationRuntime::create();
  OperationState<int, ThrowingEvent> state(runtime);
  auto reader = state.openReader();
  std::atomic<int> callbacks{0};
  std::atomic<int> errorCode{-1};
  std::promise<int> delivered;
  auto deliveredFuture = delivered.get_future();
  auto subscription = reader.nextAsync(std::chrono::seconds(5),
    [&delivered, &callbacks, &errorCode] (std::optional<ThrowingEvent> event,
                                          std::exception_ptr error) {
      ++callbacks;
      if (error) {
        try {
          std::rethrow_exception(error);
        }
        catch (const OperationError& operationError) {
          errorCode.store(static_cast<int>(operationError.code()));
        }
        catch (...) {}
      }
      delivered.set_value(!error && event ? event->value : -1);
    });
  ThrowingEvent::throwOnCopy.store(true);
  BOOST_REQUIRE(state.publish(ThrowingEvent(11), 1));
  ThrowingEvent::throwOnCopy.store(false);
  BOOST_REQUIRE(deliveredFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK_EQUAL(deliveredFuture.get(), -1);
  BOOST_CHECK_EQUAL(callbacks.load(), 1);
  BOOST_CHECK_EQUAL(errorCode.load(), static_cast<int>(OperationErrorCode::EventGap));
  BOOST_CHECK_EXCEPTION(reader.next(std::chrono::milliseconds(0)), OperationError,
                        [] (const OperationError& error) {
                          return error.code() == OperationErrorCode::EventGap;
                        });
  subscription.unsubscribe();
  reader.close();
  state.cancel();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(ResultCopyFailureStillDeliversEveryCompletionWaiter)
{
  auto runtime = OperationRuntime::create();
  OperationState<ThrowingResult, std::string> state(runtime);
  std::atomic<int> callbacks{0};
  std::atomic<bool> callbackValuesValid{true};
  std::promise<void> delivered;
  auto deliveredFuture = delivered.get_future();
  auto callback = [&callbacks, &callbackValuesValid, &delivered] (std::optional<ThrowingResult> result,
                                             std::exception_ptr error) {
    if (result || !error)
      callbackValuesValid.store(false, std::memory_order_release);
    if (callbacks.fetch_add(1) + 1 == 2)
      delivered.set_value();
  };
  auto first = state.resultAsync(std::chrono::seconds(1), callback);
  auto second = state.resultAsync(std::chrono::seconds(1), callback);
  ThrowingResult::throwOnCopy.store(true);
  BOOST_CHECK(state.complete(ThrowingResult(42)));
  ThrowingResult::throwOnCopy.store(false);
  BOOST_REQUIRE(deliveredFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK(callbackValuesValid.load(std::memory_order_acquire));
  BOOST_CHECK_EQUAL(callbacks.load(), 2);
  first.unsubscribe();
  second.unsubscribe();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(TerminalResultCopyFailureStillCallsImmediateWaiter)
{
  auto runtime = OperationRuntime::create();
  OperationState<ThrowingResult, std::string> state(runtime);
  BOOST_REQUIRE(state.complete(ThrowingResult(9)));

  std::promise<bool> delivered;
  auto deliveredFuture = delivered.get_future();
  ThrowingResult::throwOnCopy.store(true);
  auto subscription = state.resultAsync(std::chrono::seconds(1),
    [&delivered] (std::optional<ThrowingResult> result, std::exception_ptr error) {
      delivered.set_value(!result && static_cast<bool>(error));
    });
  BOOST_REQUIRE(deliveredFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK(deliveredFuture.get());
  ThrowingResult::throwOnCopy.store(false);
  subscription.unsubscribe();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(ClosedRuntimeRejectsNewStateRegistrations)
{
  auto runtime = OperationRuntime::create();
  OperationState<int, std::string> state(runtime);
  runtime->close();
  BOOST_CHECK_THROW(state.onCompletion([] {}), OperationError);
  BOOST_CHECK_THROW(state.resultAsync(std::chrono::milliseconds(0),
                                      [] (std::optional<int>, std::exception_ptr) {}),
                    OperationError);
  BOOST_CHECK_THROW(state.observe([] (const std::string&) {}), OperationError);
  BOOST_CHECK_THROW(state.openReader(), OperationError);
  state.cancel();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(ObserverDoesNotAuthorizeReliableHistoryReclamation)
{
  auto runtime = OperationRuntime::create();
  OperationState<int, std::string> state(runtime);
  auto observerSubscription = state.observe([] (const std::string&) {});
  BOOST_REQUIRE(state.publish("observer-history-1", 18));
  BOOST_REQUIRE(state.publish("observer-history-2", 18));

  auto reader = state.openReader();
  auto first = reader.next(std::chrono::milliseconds(0));
  auto second = reader.next(std::chrono::milliseconds(0));
  BOOST_REQUIRE(first);
  BOOST_REQUIRE(second);
  BOOST_CHECK_EQUAL(*first, "observer-history-1");
  BOOST_CHECK_EQUAL(*second, "observer-history-2");

  observerSubscription.unsubscribe();
  reader.close();
  state.cancel();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(LateReaderReportsGapBeforeReturningTruncatedHistory)
{
  auto runtime = OperationRuntime::create();
  OperationState<int, std::string> state(runtime);
  for (std::size_t i = 0; i < 1024; ++i)
    BOOST_REQUIRE(state.publish("retained", 1));
  BOOST_CHECK(!state.publish("overflow", 1));

  auto reader = state.openReader();
  BOOST_CHECK_THROW(reader.next(std::chrono::milliseconds(0)), OperationError);
  BOOST_CHECK_THROW(reader.next(std::chrono::milliseconds(0)), OperationError);
  reader.close();
  state.cancel();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(ReaderGenerationPreservesAnUnsubscribedQueuedEvent)
{
  auto runtime = OperationRuntime::create();
  auto blockerTicket = runtime->acquire();
  std::promise<void> blockerStarted;
  auto blockerStartedFuture = blockerStarted.get_future();
  std::promise<void> releaseBlock;
  auto releaseBlockFuture = releaseBlock.get_future().share();
  runtime->post(blockerTicket, [&blockerStarted, releaseBlockFuture] {
    blockerStarted.set_value();
    releaseBlockFuture.wait();
  });
  BOOST_REQUIRE(blockerStartedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  OperationState<int, std::string> state(runtime);
  auto firstReader = state.openReader();
  BOOST_REQUIRE(state.publish("preserve", 8));
  auto subscription = firstReader.nextAsync(std::chrono::milliseconds(0),
    [] (std::optional<std::string>, std::exception_ptr) {
      BOOST_FAIL("unsubscribed reader callback was delivered");
    });
  subscription.unsubscribe();
  firstReader.close();
  auto secondReader = state.openReader();
  releaseBlock.set_value();
  blockerTicket = OperationRuntime::WorkTicket{};
  auto event = secondReader.next(std::chrono::milliseconds(100));
  BOOST_REQUIRE(event);
  BOOST_CHECK_EQUAL(*event, "preserve");
  secondReader.close();
  state.cancel();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(ReaderGenerationFencesClosedPendingReadAfterReopen)
{
  auto runtime = OperationRuntime::create();
  auto blockerTicket = runtime->acquire();
  std::promise<void> blockerStarted;
  auto blockerStartedFuture = blockerStarted.get_future();
  std::promise<void> releaseBlock;
  auto releaseBlockFuture = releaseBlock.get_future().share();
  runtime->post(blockerTicket, [&blockerStarted, releaseBlockFuture] {
    blockerStarted.set_value();
    releaseBlockFuture.wait();
  });
  BOOST_REQUIRE(blockerStartedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);

  OperationState<int, std::string> state(runtime);
  auto firstReader = state.openReader();
  std::promise<bool> closed;
  auto closedFuture = closed.get_future();
  auto closedSubscription = firstReader.nextAsync(std::chrono::seconds(1),
    [&closed] (std::optional<std::string> event, std::exception_ptr error) {
      closed.set_value(!event && static_cast<bool>(error));
    });
  firstReader.close();

  auto secondReader = state.openReader();
  std::promise<bool> delivered;
  auto deliveredFuture = delivered.get_future();
  auto deliveredSubscription = secondReader.nextAsync(std::chrono::seconds(1),
    [&delivered] (std::optional<std::string> event, std::exception_ptr error) {
      delivered.set_value(event && *event == "after-reopen" && !error);
    });
  BOOST_REQUIRE(state.publish("after-reopen", 12));

  releaseBlock.set_value();
  blockerTicket = OperationRuntime::WorkTicket{};
  BOOST_REQUIRE(closedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK(closedFuture.get());
  BOOST_REQUIRE(deliveredFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK(deliveredFuture.get());
  BOOST_CHECK_THROW(secondReader.next(std::chrono::milliseconds(0)), OperationError);

  deliveredSubscription.unsubscribe();
  closedSubscription.unsubscribe();
  secondReader.close();
  state.cancel();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(ExternalTerminalAdmissionRejectsLateEventBeforeCorePublish)
{
  auto runtime = OperationRuntime::create();
  OperationState<int, std::string> state(runtime);
  std::atomic<bool> ownerTerminal{false};
  std::promise<void> admissionEntered;
  auto entered = admissionEntered.get_future();
  std::promise<void> releaseAdmission;
  auto release = releaseAdmission.get_future().share();
  auto publishing = std::async(std::launch::async, [&] {
    return state.publishIf("late-event", 1, [&] {
      admissionEntered.set_value();
      release.wait();
      return !ownerTerminal.load(std::memory_order_acquire);
    });
  });
  BOOST_REQUIRE(entered.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  ownerTerminal.store(true, std::memory_order_release);
  releaseAdmission.set_value();
  BOOST_CHECK(!publishing.get());
  BOOST_CHECK(state.isPending());
  auto reader = state.openReader();
  BOOST_CHECK_THROW(reader.next(std::chrono::milliseconds(0)), OperationError);
  reader.close();
  runtime->close();
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
