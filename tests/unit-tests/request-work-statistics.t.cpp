#include "NDNSF-DistributedInference/cpp/ndnsf-di/request-work-statistics.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"
#include "ndn-service-framework/control-byte-statistics.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/control-byte-inventory.hpp"
#ifndef SPEC220_COLLECTOR_ONLY
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#endif

#include <boost/test/unit_test.hpp>

#include <stdexcept>
#include <future>
#include <limits>
#include <string>
#include <thread>

namespace ndnsf::di::tests {

BOOST_AUTO_TEST_SUITE(NativeRequestWorkStatistics)

BOOST_AUTO_TEST_CASE(ControlInventoryMergeAndIndependentCounterBound)
{
  using namespace ndn_service_framework;
  ControlByteStatistics source(true);
  source.record(ControlByteKind::REQUEST_PREPARED, 19);
  source.record(ControlByteKind::RESPONSE_DECODED, 23);
  NativeWorkStatistics destination;
  BOOST_REQUIRE(detail::mergeControlByteInventory(destination, source.seal(),
    detail::ControlInventoryScope::GRANT_AUTHORITY));
  auto result = destination.snapshot();
  BOOST_REQUIRE(result.complete);
  BOOST_CHECK_EQUAL(result.counts.at("grant.core.requestPreparedBytes"), 19U);
  BOOST_CHECK_EQUAL(result.counts.at("grant.core.responseDecodedBytes"), 23U);
  BOOST_CHECK_EQUAL(result.counts.at("grant.core.responseCaptureEnabled"), 1U);
  BOOST_CHECK_EQUAL(result.counts.at("grant.core.controlInventoryComplete"), 1U);

  NativeWorkStatistics bounded;
  for (unsigned i = 0; i < 127; ++i)
    BOOST_REQUIRE(bounded.tryAddCount("counter-" + std::to_string(i), i));
  BOOST_CHECK_EQUAL(bounded.snapshot().counts.size(), 127U);
  BOOST_REQUIRE(bounded.tryAddCount("counter-127", 127));
  BOOST_CHECK_EQUAL(bounded.snapshot().counts.size(), 128U);
  BOOST_CHECK(!bounded.tryAddCount("counter-128", 128));
  BOOST_CHECK(!bounded.snapshot().complete);
  BOOST_CHECK_EQUAL(bounded.snapshot().counts.size(), 128U);
  BOOST_CHECK(!detail::mergeControlByteInventory(bounded, source.snapshot(),
    detail::ControlInventoryScope::MAIN));
  BOOST_CHECK_EQUAL(bounded.snapshot().counts.count("core.controlInventoryComplete"), 0U);

  ControlByteStatistics invalid;
  invalid.invalidate();
  NativeWorkStatistics failed;
  BOOST_REQUIRE(detail::mergeControlByteInventory(failed, invalid.seal(),
    detail::ControlInventoryScope::MAIN));
  BOOST_CHECK_EQUAL(failed.snapshot().counts.at("core.controlInventoryComplete"), 0U);
}

BOOST_AUTO_TEST_CASE(CoreByteInventoryOverflowSealAndConcurrentRecords)
{
  using ndn_service_framework::ControlByteKind;
  using ndn_service_framework::ControlByteStatistics;
  const auto request = static_cast<std::size_t>(ControlByteKind::REQUEST_PREPARED);
  ControlByteStatistics statistics;
  std::atomic<bool> coherent{true};
  std::vector<std::thread> workers;
  for (unsigned i = 0; i < 4; ++i) {
    workers.emplace_back([&] {
      for (unsigned j = 0; j < 100; ++j) {
        statistics.record(ControlByteKind::REQUEST_PREPARED, 7);
        const auto current = statistics.snapshot();
        if (current.entries[request].bytes != 7 * current.entries[request].calls)
          coherent.store(false);
      }
    });
  }
  for (auto& worker : workers) worker.join();
  BOOST_CHECK(coherent.load());
  const auto sealed = statistics.seal();
  BOOST_CHECK(sealed.complete);
  BOOST_CHECK(sealed.sealed);
  BOOST_CHECK_EQUAL(sealed.entries[request].calls, 400U);
  BOOST_CHECK_EQUAL(sealed.entries[request].bytes, 2800U);
  for (std::size_t i = 1; i < sealed.entries.size(); ++i) {
    BOOST_CHECK_EQUAL(sealed.entries[i].bytes, 0U);
    BOOST_CHECK_EQUAL(sealed.entries[i].calls, 0U);
  }
  statistics.record(ControlByteKind::REQUEST_PREPARED, 999);
  BOOST_CHECK_EQUAL(statistics.snapshot().entries[request].bytes, 2800U);
  BOOST_CHECK_EQUAL(statistics.seal().entries[request].calls, 400U);

  ControlByteStatistics overflow;
  overflow.record(ControlByteKind::REQUEST_PREPARED,
                  std::numeric_limits<std::uint64_t>::max());
  overflow.record(ControlByteKind::REQUEST_PREPARED, 1);
  const auto failed = overflow.seal();
  BOOST_CHECK(!failed.complete);
  BOOST_CHECK_EQUAL(failed.entries[request].bytes,
                    std::numeric_limits<std::uint64_t>::max());
  BOOST_CHECK_EQUAL(failed.entries[request].calls, 1U);
  ControlByteStatistics invalid;
  invalid.record(ControlByteKind::COUNT, 0);
  BOOST_CHECK(!invalid.snapshot().complete);
  ControlByteStatistics empty;
  empty.record(ControlByteKind::ACK_DECODED, 0);
  const auto zero = empty.snapshot();
  const auto ack = static_cast<std::size_t>(ControlByteKind::ACK_DECODED);
  BOOST_CHECK(zero.complete);
  BOOST_CHECK_EQUAL(zero.entries[ack].bytes, 0U);
  BOOST_CHECK_EQUAL(zero.entries[ack].calls, 1U);
}

BOOST_AUTO_TEST_CASE(ExclusiveNestedWallAndCpuRemainDistinct)
{
  NativeWorkClock now{0, 0};
  auto statistics = std::make_shared<NativeWorkStatistics>([&] { return now; });
  {
    NativeWorkScope parent(statistics, "local", NativeWorkKind::LOCAL);
    now = {10, 5};
    {
      NativeWorkScope child(statistics, "wait", NativeWorkKind::WAIT);
      now = {70, 7};
    }
    now = {100, 20};
  }
  const auto result = statistics->snapshot();
  BOOST_REQUIRE(result.complete);
  BOOST_CHECK_EQUAL(result.phases.at("local").exclusiveWallNs, 40U);
  BOOST_CHECK_EQUAL(result.phases.at("wait").exclusiveWallNs, 60U);
  BOOST_CHECK_EQUAL(*result.phases.at("local").firstStartedSteadyNs, 0);
  BOOST_CHECK_EQUAL(*result.phases.at("local").lastFinishedSteadyNs, 100);
  BOOST_CHECK_EQUAL(*result.phases.at("wait").firstStartedSteadyNs, 10);
  BOOST_CHECK_EQUAL(*result.phases.at("wait").lastFinishedSteadyNs, 70);
  BOOST_REQUIRE(result.phases.at("local").exclusiveThreadCpuNs);
  BOOST_CHECK_EQUAL(*result.phases.at("local").exclusiveThreadCpuNs, 18U);
  BOOST_CHECK_EQUAL(*result.phases.at("wait").exclusiveThreadCpuNs, 2U);
  BOOST_CHECK(!nativeParseJson(nativeWorkStatisticsToJson(result)).at("localWorkComplete"));
}

BOOST_AUTO_TEST_CASE(EarlyCloseExceptionAndMissingCpu)
{
  NativeWorkClock now{0, -1};
  auto statistics = std::make_shared<NativeWorkStatistics>([&] { return now; });
  try {
    NativeWorkScope work(statistics, "decode", NativeWorkKind::LOCAL);
    now.wallNs = 7;
    work.finish();
    now.wallNs = 100;
    throw std::runtime_error("execution failure");
  }
  catch (const std::runtime_error&) {
  }
  const auto result = statistics->snapshot();
  BOOST_REQUIRE(result.complete);
  BOOST_CHECK_EQUAL(result.phases.at("decode").calls, 1U);
  BOOST_CHECK_EQUAL(result.phases.at("decode").exclusiveWallNs, 7U);
  BOOST_CHECK(!result.phases.at("decode").exclusiveThreadCpuNs);
  const auto wire = nativeParseJson(nativeWorkStatisticsToJson(result));
  BOOST_CHECK(wire.at("phases").at("decode").at("exclusiveThreadCpuNs").is_null());
}

BOOST_AUTO_TEST_CASE(IncompleteOrInvalidObservationCannotClaimTotals)
{
  NativeWorkClock now{10, 10};
  NativeWorkStatistics statistics([&] { return now; });
  const auto first = statistics.begin("outer", NativeWorkKind::LOCAL);
  const auto child = statistics.begin("inner", NativeWorkKind::LOCAL);
  BOOST_CHECK(!statistics.snapshot().complete);
  statistics.finish(first);
  now = {20, 20};
  statistics.finish(child);
  statistics.finish(first);
  auto wire = nativeParseJson(nativeWorkStatisticsToJson(statistics.snapshot()));
  BOOST_CHECK(!wire.at("complete").get<bool>());
  BOOST_CHECK(wire.at("phases").at("outer").at("exclusiveWallNs").is_null());

  NativeWorkStatistics backwards([&] { return now; });
  const auto token = backwards.begin("clock", NativeWorkKind::LOCAL);
  now.wallNs = 1;
  backwards.finish(token);
  BOOST_CHECK(!backwards.snapshot().complete);
  BOOST_CHECK_EQUAL(backwards.snapshot().activeSpans, 0U);
}

BOOST_AUTO_TEST_CASE(RequestAndThreadOwnershipAreBounded)
{
  auto statistics = std::make_shared<NativeWorkStatistics>([] {
    return NativeWorkClock{100, -1};
  });
  std::thread worker([&] {
    NativeWorkScope work(statistics, "worker", NativeWorkKind::LOCAL);
  });
  worker.join();
  BOOST_REQUIRE(statistics->snapshot().complete);
  NativeWorkStatistics other;
  BOOST_CHECK(other.snapshot().phases.empty());
  for (std::size_t i = 0; i < 20000; ++i) {
    const auto token = statistics->begin("worker", NativeWorkKind::LOCAL);
    statistics->finish(token);
  }
  const auto result = statistics->snapshot();
  BOOST_CHECK(!result.complete);
  BOOST_CHECK_GT(result.droppedSpans, 0U);
  BOOST_CHECK_EQUAL(result.activeSpans, 0U);
  BOOST_CHECK_EQUAL(result.phases.size(), 1U);
  BOOST_CHECK_LE(result.phases.at("worker").calls, 16384U);
}

BOOST_AUTO_TEST_CASE(CurrentPhaseVocabularyFitsBound)
{
  NativeWorkStatistics statistics([] {
    return NativeWorkClock{1, 1};
  });
  for (std::size_t i = 0; i < 128; ++i) {
    const auto phase = "phase-" + std::to_string(i);
    const auto token = statistics.begin(phase, NativeWorkKind::LOCAL);
    BOOST_REQUIRE(token != 0);
    statistics.finish(token);
  }

  const auto full = statistics.snapshot();
  BOOST_REQUIRE(full.complete);
  BOOST_CHECK_EQUAL(full.phases.size(), 128U);
  BOOST_CHECK_EQUAL(full.droppedSpans, 0U);

  BOOST_CHECK_EQUAL(statistics.begin("phase-overflow", NativeWorkKind::LOCAL), 0U);
  const auto overflow = statistics.snapshot();
  BOOST_CHECK(!overflow.complete);
  BOOST_CHECK_EQUAL(overflow.droppedSpans, 1U);
  BOOST_CHECK_EQUAL(overflow.phases.size(), 128U);
}

#ifndef SPEC220_COLLECTOR_ONLY
BOOST_AUTO_TEST_CASE(ProviderStatisticsSerializationIncludesWorkMetadata)
{
  NativeRequestStatistics statistics;
  statistics.requestId = "request-a";
  statistics.workStatistics = std::make_shared<NativeWorkStatistics>();
  {
    NativeWorkScope work(statistics.workStatistics, "sample", NativeWorkKind::LOCAL);
  }
  const auto wire = nativeParseJson(nativeRequestStatisticsToJson(statistics));
  BOOST_CHECK_EQUAL(wire.at("requestId").get<std::string>(), "request-a");
  BOOST_CHECK(wire.at("workStatistics").at("complete").get<bool>());
  BOOST_CHECK_EQUAL(wire.at("workStatistics").at("phases").at("sample").at("calls"), 1);
  BOOST_CHECK(wire.at("clockDomain").is_string() || wire.at("clockDomain").is_null());
}
#endif

BOOST_AUTO_TEST_CASE(BackendIsExcludedAndConcurrentThreadsRemainServiceWork)
{
  const auto mainThread = std::this_thread::get_id();
  NativeWorkClock mainClock{0, 0};
  NativeWorkClock workerClock{10, 3};
  NativeWorkStatistics statistics([&] {
    return std::this_thread::get_id() == mainThread ? mainClock : workerClock;
  });
  const auto parent = statistics.begin("main", NativeWorkKind::LOCAL);
  mainClock = {10, 2};
  const auto backend = statistics.begin("backend", NativeWorkKind::BACKEND);
  mainClock = {30, 7};
  statistics.finish(backend);
  std::promise<void> begun;
  std::promise<void> finish;
  auto finishFuture = finish.get_future();
  std::thread worker([&] {
    const auto token = statistics.begin("worker", NativeWorkKind::LOCAL);
    begun.set_value();
    finishFuture.wait();
    workerClock = {110, 13};
    statistics.finish(token);
  });
  begun.get_future().wait();
  mainClock = {50, 20};
  statistics.finish(parent);
  finish.set_value();
  worker.join();
  const auto result = statistics.snapshot();
  BOOST_REQUIRE(result.complete);
  BOOST_CHECK_EQUAL(result.phases.at("main").exclusiveWallNs, 30U);
  BOOST_CHECK_EQUAL(result.phases.at("backend").exclusiveWallNs, 20U);
  BOOST_CHECK_EQUAL(result.phases.at("worker").exclusiveWallNs, 100U);
  BOOST_CHECK_EQUAL(*result.phases.at("main").exclusiveThreadCpuNs, 15U);
  BOOST_CHECK_EQUAL(*result.phases.at("worker").exclusiveThreadCpuNs, 10U);
}

BOOST_AUTO_TEST_CASE(WrongThreadAndThrowingClockCannotClaimCompleteness)
{
  NativeWorkStatistics statistics([] { return NativeWorkClock{1, 1}; });
  const auto token = statistics.begin("owner", NativeWorkKind::LOCAL);
  std::thread wrong([&] { statistics.finish(token); });
  wrong.join();
  statistics.finish(token);
  BOOST_CHECK(!statistics.snapshot().complete);
  BOOST_CHECK_EQUAL(statistics.snapshot().activeSpans, 0U);
  NativeWorkStatistics throwing([] () -> NativeWorkClock {
    throw std::runtime_error("clock unavailable");
  });
  BOOST_CHECK_EQUAL(throwing.begin("clock", NativeWorkKind::LOCAL), 0U);
  BOOST_CHECK(!throwing.snapshot().complete);
  BOOST_CHECK_EQUAL(throwing.snapshot().droppedSpans, 1U);
}

BOOST_AUTO_TEST_CASE(PhaseAndDepthBoundsRejectBeforeUnboundedGrowth)
{
  NativeWorkStatistics phases([] { return NativeWorkClock{1, 1}; });
  for (std::size_t i = 0; i < 129; ++i) {
    const auto token = phases.begin("phase-" + std::to_string(i), NativeWorkKind::LOCAL);
    phases.finish(token);
  }
  BOOST_CHECK_EQUAL(phases.snapshot().phases.size(), 128U);
  BOOST_CHECK_EQUAL(phases.snapshot().droppedSpans, 1U);
  NativeWorkStatistics depth([] { return NativeWorkClock{1, 1}; });
  std::vector<NativeWorkStatistics::Token> tokens;
  for (std::size_t i = 0; i < 33; ++i)
    tokens.push_back(depth.begin("nested", NativeWorkKind::LOCAL));
  for (auto i = tokens.rbegin(); i != tokens.rend(); ++i) depth.finish(*i);
  BOOST_CHECK_EQUAL(depth.snapshot().activeSpans, 0U);
  BOOST_CHECK_EQUAL(depth.snapshot().phases.at("nested").calls, 32U);
  BOOST_CHECK_EQUAL(depth.snapshot().droppedSpans, 1U);
}

BOOST_AUTO_TEST_CASE(ControlCountsOverflowOrInvalidNamesAreUnknown)
{
  NativeWorkStatistics statistics;
  statistics.addCount("controlBytes", 12);
  statistics.addCount("controlBytes", 18);
  BOOST_CHECK_EQUAL(statistics.snapshot().counts.at("controlBytes"), 30U);
  statistics.addCount("controlBytes", std::numeric_limits<std::uint64_t>::max());
  const auto wire = nativeParseJson(nativeWorkStatisticsToJson(statistics.snapshot()));
  BOOST_CHECK(!wire.at("complete").get<bool>());
  BOOST_CHECK(wire.at("counts").at("controlBytes").is_null());
  NativeWorkStatistics bounded;
  bounded.addCount(std::string(97, 'x'), 1);
  BOOST_CHECK(!bounded.snapshot().complete);
  BOOST_CHECK(bounded.snapshot().counts.empty());
}

BOOST_AUTO_TEST_CASE(MaterializationHighWaterUsesTheBoundedCounterOwner)
{
  NativeWorkStatistics statistics;
  statistics.observeMaximum("peak.slots", 12);
  statistics.observeMaximum("peak.slots", 4);
  statistics.observeMaximum("peak.slots", 20);
  statistics.observeMaximum("peak.empty", 0);
  auto snapshot = statistics.snapshot();
  BOOST_REQUIRE(snapshot.complete);
  BOOST_CHECK_EQUAL(snapshot.counts.at("peak.slots"), 20U);
  BOOST_CHECK_EQUAL(snapshot.counts.at("peak.empty"), 0U);
  NativeWorkStatistics bounded;
  for (std::size_t index = 0; index != 128; ++index)
    bounded.observeMaximum("peak." + std::to_string(index), index);
  BOOST_CHECK(bounded.snapshot().complete);
  bounded.observeMaximum("peak.extra", 1);
  const auto wire = nativeParseJson(nativeWorkStatisticsToJson(bounded.snapshot()));
  BOOST_CHECK(!wire.at("complete").get<bool>());
  BOOST_CHECK(wire.at("counts").at("peak.0").is_null());
  NativeWorkStatistics invalid;
  invalid.observeMaximum(std::string(97, 'x'), 1);
  BOOST_CHECK(!invalid.snapshot().complete);
}

BOOST_AUTO_TEST_CASE(ThreadRegistryCannotGrowBeyondItsRequestBound)
{
  NativeWorkStatistics statistics([] { return NativeWorkClock{1, 1}; });
  std::promise<void> release;
  auto ready = release.get_future().share();
  std::vector<std::promise<void>> entered(33);
  std::vector<std::thread> workers;
  workers.reserve(33);
  try {
    for (std::size_t i = 0; i < entered.size(); ++i) {
      workers.emplace_back([&, i] {
        const auto token = statistics.begin("worker", NativeWorkKind::LOCAL);
        entered[i].set_value();
        ready.wait();
        statistics.finish(token);
      });
    }
  }
  catch (...) {
    release.set_value();
    for (auto& worker : workers) worker.join();
    throw;
  }
  for (auto& entry : entered) entry.get_future().wait();
  release.set_value();
  for (auto& worker : workers) worker.join();
  const auto result = statistics.snapshot();
  BOOST_CHECK(!result.complete);
  BOOST_CHECK_EQUAL(result.droppedSpans, 1U);
  BOOST_CHECK_EQUAL(result.activeSpans, 0U);
  BOOST_CHECK_EQUAL(result.phases.at("worker").calls, 32U);
}

BOOST_AUTO_TEST_CASE(TransferMetadataRetainsMissingValuesAndRejectsInvalidIntervals)
{
  NativeWorkStatistics statistics;
  StageTransferObservation observation;
  observation.plannedDataName = "/producer/activation/1";
  observation.direction = "receive";
  observation.phase = "delta";
  observation.tensorBytes = 4096;
  observation.encodedPayloadBytes = 4200;
  statistics.observeTransfer(observation, 7, 10, 20, 25);
  observation.tensorBytes = 0;
  const auto result = statistics.snapshot();
  BOOST_REQUIRE(result.complete);
  BOOST_REQUIRE_EQUAL(result.transfers.size(), 1U);
  BOOST_CHECK_EQUAL(result.transfers.front().tensorBytes, 4096U);
  const auto wire = nativeParseJson(nativeWorkStatisticsToJson(result));
  BOOST_CHECK(wire.at("transfers").at(0).at("wireBytes").is_null());
  BOOST_CHECK_EQUAL(wire.at("transfers").at(0).at("admittedSteadyNs"), 25);
  statistics.observeTransfer(observation, 8, 20, 10);
  BOOST_CHECK(!statistics.snapshot().complete);
  BOOST_CHECK_EQUAL(statistics.snapshot().transfers.size(), 1U);
  NativeWorkStatistics bounded;
  for (std::size_t i = 0; i < 8193; ++i)
    bounded.observeTransfer(observation, i, 1, 2);
  BOOST_CHECK(!bounded.snapshot().complete);
  BOOST_CHECK_EQUAL(bounded.snapshot().transfers.size(), 8192U);
}

BOOST_AUTO_TEST_CASE(TransferStringBudgetAndAdmissionOrderingAreBounded)
{
  StageTransferObservation observation;
  observation.plannedDataName = "/" + std::string(4095, 'x');
  observation.direction = "receive";
  observation.phase = "delta";
  NativeWorkStatistics bytes;
  for (std::size_t i = 0; i < 1100; ++i) bytes.observeTransfer(observation, i, 1, 2, 3);
  const auto result = bytes.snapshot();
  BOOST_CHECK(!result.complete);
  BOOST_CHECK_GT(result.transfers.size(), 0U);
  BOOST_CHECK_LT(result.transfers.size(), 1100U);
  NativeWorkStatistics name;
  observation.plannedDataName.push_back('x');
  name.observeTransfer(observation, 0, 1, 2, 3);
  BOOST_CHECK(!name.snapshot().complete);
  BOOST_CHECK(name.snapshot().transfers.empty());
  NativeWorkStatistics ordering;
  observation.plannedDataName = "/producer/activation/1";
  ordering.observeTransfer(observation, 0, 1, 3, 2);
  BOOST_CHECK(!ordering.snapshot().complete);
  BOOST_CHECK(ordering.snapshot().transfers.empty());
}

BOOST_AUTO_TEST_CASE(TransferPointsSeparateOutputReadinessFetchAndAdmission)
{
  NativeWorkStatistics statistics;
  StageTransferObservation observation;
  observation.plannedDataName = "/producer/activation/1";
  observation.direction = "send";
  observation.phase = "delta";
  statistics.observeTransfer(observation, 1, 10, 20, 0,
                             NativeTransferPoint::OUTPUT_PUBLICATION, 5);
  observation.direction = "receive";
  statistics.observeTransfer(observation, 1, 12, 22, 0,
                             NativeTransferPoint::COORDINATOR_FETCH);
  statistics.observeTransfer(observation, 1, 0, 0, 25, NativeTransferPoint::WORKER_INPUT);
  const auto result = statistics.snapshot();
  BOOST_REQUIRE(result.complete);
  BOOST_REQUIRE_EQUAL(result.transfers.size(), 3U);
  BOOST_CHECK_EQUAL(result.transfers[0].outputReadySteadyNs, 5);
  BOOST_CHECK_EQUAL(result.transfers[0].admittedSteadyNs, 0);
  const auto wire = nativeParseJson(nativeWorkStatisticsToJson(result));
  BOOST_CHECK_EQUAL(wire["transfers"][0]["point"], "output-publication");
  BOOST_CHECK_EQUAL(wire["transfers"][0]["outputReadySteadyNs"], 5);
  BOOST_CHECK(wire["transfers"][0]["admittedSteadyNs"].is_null());
  BOOST_CHECK_EQUAL(wire["transfers"][1]["point"], "coordinator-fetch");
  BOOST_CHECK(wire["transfers"][1]["outputReadySteadyNs"].is_null());
  BOOST_CHECK_EQUAL(wire["transfers"][2]["point"], "worker-input");
  BOOST_CHECK(wire["transfers"][2]["startedSteadyNs"].is_null());
  BOOST_CHECK_EQUAL(wire["transfers"][2]["admittedSteadyNs"], 25);
  NativeWorkStatistics invalid;
  observation.direction = "send";
  invalid.observeTransfer(observation, 1, 10, 20, 0,
                          NativeTransferPoint::OUTPUT_PUBLICATION, 11);
  BOOST_CHECK(!invalid.snapshot().complete);
  BOOST_CHECK(invalid.snapshot().transfers.empty());
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
