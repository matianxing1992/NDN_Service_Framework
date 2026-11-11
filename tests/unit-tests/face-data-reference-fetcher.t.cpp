#include "NDNSF-DistributedRepo/src/face-data-reference-fetcher.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/encoding/tlv.hpp>
#include <ndn-cxx/lp/nack.hpp>
#include <ndn-cxx/lp/tags.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>
#include <ndn-cxx/util/scheduler.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace ndnsf_distributed_repo;
using namespace std::chrono_literals;

using WorkGuard = boost::asio::executor_work_guard<
  boost::asio::io_context::executor_type>;

struct FacePump
{
  boost::asio::io_context io;
  WorkGuard work = boost::asio::make_work_guard(io);
  ndn::DummyClientFace face{io};
  std::thread thread{[this] { io.run(); }};

  ~FacePump()
  {
    stop();
  }

  void
  stop()
  {
    work.reset();
    io.stop();
    if (thread.joinable()) {
      thread.join();
    }
  }
};

/** Stop callbacks before their borrowed fixture inputs leave scope on failure. */
struct FacePumpStop
{
  FacePump& pump;
  ~FacePumpStop()
  {
    pump.stop();
  }
};

ndn::Data
makeData(const ndn::Name& name, uint8_t content)
{
  ndn::Data data(name);
  data.setContent(ndn::span<const uint8_t>(&content, 1));
  data.setSignatureInfo(ndn::SignatureInfo(ndn::tlv::DigestSha256));
  const std::vector<uint8_t> signature(32, content);
  data.setSignatureValue(ndn::span<const uint8_t>(signature.data(), signature.size()));
  return data;
}

std::vector<uint8_t>
wireOf(const ndn::Data& data)
{
  const auto& wire = data.wireEncode();
  return {wire.begin(), wire.end()};
}

RepoDataReference
makeSegmentedReference(const ndn::Name& prefix, unsigned count,
                       std::vector<ndn::Data>& packets)
{
  RepoDataReference reference;
  reference.dataPrefix = prefix.toUri();
  reference.hasFinalSegment = true;
  reference.finalSegment = count - 1;
  for (unsigned i = 0; i < count; ++i) {
    auto data = makeData(ndn::Name(prefix).appendSegment(i), i);
    data.setFinalBlock(ndn::name::Component::fromSegment(count - 1));
    reference.expectedSize += wireOf(data).size();
    packets.push_back(std::move(data));
  }
  return reference;
}

} // namespace

BOOST_AUTO_TEST_SUITE(FaceDataReferenceFetcher)

BOOST_AUTO_TEST_CASE(CompleteSegmentedObjectUsesStandardFetcherAndKeepsOriginalWire)
{
  FacePump pump;
  const ndn::Name prefix("/producer/model/standard/part-a");
  std::vector<ndn::Data> packets;
  const auto reference = makeSegmentedReference(prefix, 64, packets);
  bool nativeBootstrap = false;
  unsigned issued = 0;
  auto connection = pump.face.onSendInterest.connect([&](const ndn::Interest& interest) {
    ++issued;
    const auto index = interest.getName()[-1].toSegment();
    if (interest.getCanBePrefix()) {
      nativeBootstrap = true;
      BOOST_CHECK(interest.getName() == packets.front().getName());
    }
    BOOST_REQUIRE_LT(index, packets.size());
    pump.face.receive(packets.at(index));
  });
  FacePumpStop stop{pump};
  const detail::FaceDataReferenceFetcher fetcher(pump.face, 1s, 3s);
  const auto result = fetcher(reference);
  pump.stop();
  BOOST_CHECK(nativeBootstrap);
  BOOST_REQUIRE_EQUAL(result->packetCount(), packets.size());
  BOOST_CHECK_LE(issued, packets.size() + 16);
  for (std::size_t i = 0; i < packets.size(); ++i)
    BOOST_CHECK(result->readPacket(i) == wireOf(packets[i]));
}

BOOST_AUTO_TEST_CASE(StandardStreamRetriesLostTailWithinWireLifetime)
{
  FacePump pump;
  const ndn::Name prefix("/producer/model/lost-tail");
  std::vector<ndn::Data> packets;
  const auto reference = makeSegmentedReference(prefix, 32, packets);
  unsigned tailRequests = 0;
  bool nativeBootstrap = false;
  auto connection = pump.face.onSendInterest.connect([&](const ndn::Interest& interest) {
    nativeBootstrap = nativeBootstrap || interest.getCanBePrefix();
    const auto index = interest.getName()[-1].toSegment();
    // The initial compatibility probe cannot reach this segment. Lose four
    // standard-stream attempts, then serve the unchanged original packet.
    if (index == packets.size() - 1 && ++tailRequests <= 4) return;
    pump.face.receive(packets.at(index));
  });
  FacePumpStop stop{pump};
  const detail::FaceDataReferenceFetcher fetcher(pump.face, 50ms, 2s);
  const auto result = fetcher(reference);
  pump.stop();
  BOOST_CHECK(nativeBootstrap);
  BOOST_CHECK_EQUAL(tailRequests, 5);
  BOOST_REQUIRE_EQUAL(result->packetCount(), packets.size());
  for (std::size_t i = 0; i < packets.size(); ++i)
    BOOST_CHECK(result->readPacket(i) == wireOf(packets[i]));
}

BOOST_AUTO_TEST_CASE(StandardStreamLeavesSlowStartBeforeBurstingHundredsOfInterests)
{
  FacePump pump;
  const ndn::Name prefix("/producer/model/bounded-slow-start");
  std::vector<ndn::Data> packets;
  const auto reference = makeSegmentedReference(prefix, 512, packets);
  ndn::Scheduler scheduler(pump.io);
  unsigned pending = 0, peakPending = 0;
  bool nativeBootstrap = false;
  auto connection = pump.face.onSendInterest.connect([&](const ndn::Interest& interest) {
    nativeBootstrap = nativeBootstrap || interest.getCanBePrefix();
    const auto index = interest.getName()[-1].toSegment();
    ++pending;
    peakPending = std::max(peakPending, pending);
    scheduler.schedule(ndn::time::milliseconds(5), [&, index] {
      --pending;
      pump.face.receive(packets.at(index));
    });
  });
  FacePumpStop stop{pump};
  const detail::FaceDataReferenceFetcher fetcher(pump.face, 1s, 3s);
  const auto result = fetcher(reference);
  pump.stop();
  BOOST_CHECK(nativeBootstrap);
  // Allow stock additive growth after the existing 64-packet threshold;
  // this is not a fixed congestion window or a replacement congestion driver.
  BOOST_CHECK_LE(peakPending, 80);
  BOOST_REQUIRE_EQUAL(result->packetCount(), packets.size());
  for (std::size_t i = 0; i < packets.size(); ++i)
    BOOST_CHECK(result->readPacket(i) == wireOf(packets[i]));
}

BOOST_AUTO_TEST_CASE(StandardStreamRejectsChangedFinalBlockWithoutDowngrade)
{
  FacePump pump;
  const ndn::Name prefix("/producer/model/changed-final");
  std::vector<ndn::Data> packets;
  const auto reference = makeSegmentedReference(prefix, 64, packets);
  bool nativeBootstrap = false;
  auto connection = pump.face.onSendInterest.connect([&](const ndn::Interest& interest) {
    nativeBootstrap = nativeBootstrap || interest.getCanBePrefix();
    const auto index = interest.getName()[-1].toSegment();
    auto data = packets.at(index);
    if (index == 32) data.setFinalBlock(ndn::name::Component::fromSegment(62));
    pump.face.receive(data);
  });
  FacePumpStop stop{pump};
  const detail::FaceDataReferenceFetcher fetcher(pump.face, 1s, 3s);
  BOOST_CHECK_THROW(fetcher(reference), std::runtime_error);
  pump.stop();
  BOOST_CHECK(nativeBootstrap);
}

BOOST_AUTO_TEST_CASE(PartialSegmentRangeKeepsExactCompatibilityRetrieval)
{
  FacePump pump;
  const ndn::Name prefix("/producer/model/partial");
  std::vector<ndn::Data> packets;
  std::vector<ndn::Name> names;
  for (unsigned i = 0; i < 4; ++i) {
    auto data = makeData(ndn::Name(prefix).appendSegment(i), i);
    data.setFinalBlock(ndn::name::Component::fromSegment(7));
    names.push_back(data.getName());
    packets.push_back(std::move(data));
  }
  bool nativeBootstrap = false;
  auto connection = pump.face.onSendInterest.connect([&](const ndn::Interest& interest) {
    nativeBootstrap = nativeBootstrap || interest.getCanBePrefix();
    const auto index = interest.getName()[-1].toSegment();
    BOOST_REQUIRE_LT(index, packets.size());
    pump.face.receive(packets.at(index));
  });
  FacePumpStop stop{pump};
  const detail::FaceDataReferenceFetcher fetcher(pump.face, 1s, 3s);
  const auto result = fetcher.fetchNamed(names, 4096, ndn::Name("/repo"));
  pump.stop();
  BOOST_CHECK(!nativeBootstrap);
  for (std::size_t i = 0; i < packets.size(); ++i)
    BOOST_CHECK(result->readPacket(i) == wireOf(packets[i]));
}

BOOST_AUTO_TEST_CASE(StandardStreamCancellationDrainsBeforeReadmission)
{
  FacePump pump;
  const ndn::Name prefix("/producer/model/standard-cancel");
  std::vector<ndn::Data> packets;
  const auto reference = makeSegmentedReference(prefix, 64, packets);
  std::atomic<bool> current{true};
  std::atomic<bool> cancelOnce{true};
  std::atomic<bool> nativeBootstrap{false};
  auto lifetime = std::make_shared<int>(0);
  auto connection = pump.face.onSendInterest.connect([&](const ndn::Interest& interest) {
    if (interest.getCanBePrefix()) {
      nativeBootstrap = true;
      if (cancelOnce.exchange(false)) {
        current = false;
        return;
      }
    }
    pump.face.receive(packets.at(interest.getName()[-1].toSegment()));
  });
  FacePumpStop stop{pump};
  const detail::FaceDataReferenceFetcher fetcher(pump.face, 1s, 3s, 0, lifetime);
  BOOST_CHECK_THROW(fetcher(reference, [&] {
    if (!current.load()) throw std::runtime_error("owner closed");
  }), std::runtime_error);
  BOOST_REQUIRE(nativeBootstrap.load());
  // Only the fixture and fetcher retain this token once actual admission/drain ends.
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (lifetime.use_count() != 2 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  BOOST_REQUIRE_EQUAL(lifetime.use_count(), 2);
  current = true;
  const auto result = fetcher(reference);
  pump.stop();
  BOOST_REQUIRE_EQUAL(result->packetCount(), packets.size());
  for (std::size_t i = 0; i < packets.size(); ++i)
    BOOST_CHECK(result->readPacket(i) == wireOf(packets[i]));
}

void
checkRetryRecoveryWithFreshAcks(bool markFirst)
{
  FacePump pump;
  const ndn::Name prefix("/producer/model/retry-recovery");
  RepoDataReference reference;
  reference.dataPrefix = prefix.toUri();
  reference.hasFinalSegment = true;
  reference.finalSegment = 127;
  std::vector<ndn::Data> packets;
  std::vector<unsigned> attempts(128, 0);
  for (unsigned i = 0; i < 128; ++i) {
    packets.push_back(makeData(ndn::Name(prefix).appendSegment(i), i));
    if (markFirst && i == 0)
      packets.back().setTag(std::make_shared<ndn::lp::CongestionMarkTag>(1));
    reference.expectedSize += wireOf(packets.back()).size();
  }
  ndn::Scheduler scheduler(pump.io);
  auto connection = pump.face.onSendInterest.connect([&](const ndn::Interest& interest) {
    const auto index = interest.getName()[-1].toSegment();
    const auto attempt = ++attempts.at(index);
    if (index == 4 && attempt < 3)
      return;
    // Other original ACKs keep the shared RTT estimate low during recovery.
    // The final permitted attempt must tolerate a slower response from this packet.
    scheduler.schedule(ndn::time::milliseconds(index == 4 ? 150 : 10),
      [&, data = packets.at(index)] { pump.face.receive(data); });
  });
  FacePumpStop stop{pump};
  const detail::FaceDataReferenceFetcher fetcher(pump.face, 1s, 3s, 4);
  const auto result = fetcher(reference);
  pump.stop();
  BOOST_REQUIRE_EQUAL(result->packetCount(), packets.size());
  BOOST_CHECK_EQUAL(attempts.at(4), 3U);
  for (std::size_t i = 0; i < packets.size(); ++i) {
    BOOST_CHECK_LE(attempts[i], 3U);
    BOOST_CHECK(result->readPacket(i) == wireOf(packets[i]));
  }
}

BOOST_AUTO_TEST_CASE(RetryRecoverySurvivesFreshFastAcksBetweenLostAttempts)
{
  checkRetryRecoveryWithFreshAcks(false);
}

BOOST_AUTO_TEST_CASE(RetryRecoverySurvivesMarkedFastAcksBetweenLostAttempts)
{
  checkRetryRecoveryWithFreshAcks(true);
}

BOOST_AUTO_TEST_CASE(RetransmissionsWaitForReducedWindowBeforeSendingNewPackets)
{
  FacePump pump;
  const ndn::Name prefix("/producer/model/retry-window");
  RepoDataReference reference;
  reference.dataPrefix = prefix.toUri();
  reference.hasFinalSegment = true;
  reference.finalSegment = 23;
  std::vector<ndn::Data> packets;
  std::vector<unsigned> attempts(24, 0);
  for (unsigned i = 0; i < 24; ++i) {
    packets.push_back(makeData(ndn::Name(prefix).appendSegment(i), i));
    reference.expectedSize += wireOf(packets.back()).size();
  }
  std::promise<void> reducedFlight;
  auto ready = reducedFlight.get_future();
  std::atomic<unsigned> retries{0};
  std::atomic<bool> prematureNewPacket{false};
  bool release = false;
  auto connection = pump.face.onSendInterest.connect([&] (const ndn::Interest& interest) {
    const auto index = interest.getName()[-1].toSegment();
    const auto attempt = ++attempts.at(index);
    if (!release) {
      if (index >= 8)
        prematureNewPacket.store(true);
      if (attempt > 1 && retries.fetch_add(1) == 3)
        reducedFlight.set_value();
      return;
    }
    pump.face.receive(packets.at(index));
  });
  FacePumpStop stop{pump};
  const detail::FaceDataReferenceFetcher fetcher(pump.face, 200ms, 3s, 8);
  auto fetched = std::async(std::launch::async, [&] { return fetcher(reference); });
  BOOST_REQUIRE(ready.wait_for(1s) == std::future_status::ready);
  // Let the rest of the original flight's timeout callbacks drain, while the
  // four retransmissions remain unanswered and below their own RTO.
  std::this_thread::sleep_for(10ms);
  BOOST_CHECK_EQUAL(retries.load(), 4U);
  BOOST_CHECK(!prematureNewPacket.load());
  boost::asio::post(pump.io, [&] {
    release = true;
    for (std::size_t i = 0; i < 8; ++i) {
      if (attempts[i] > 1)
        pump.face.receive(packets[i]);
    }
  });
  const auto result = fetched.get();
  pump.stop();
  BOOST_REQUIRE_EQUAL(result->packetCount(), packets.size());
  for (std::size_t i = 0; i < packets.size(); ++i)
    BOOST_CHECK(result->readPacket(i) == wireOf(packets[i]));
}

BOOST_AUTO_TEST_CASE(DefaultWindowUsesFiniteStartupAndContinuesGrowing)
{
  FacePump pump;
  const ndn::Name prefix("/producer/model/finite-startup");
  RepoDataReference reference;
  reference.dataPrefix = prefix.toUri();
  reference.hasFinalSegment = true;
  reference.finalSegment = 511;
  std::vector<ndn::Data> packets;
  for (unsigned i = 0; i < 512; ++i) {
    packets.push_back(makeData(ndn::Name(prefix).appendSegment(i), i));
    reference.expectedSize += wireOf(packets.back()).size();
  }
  ndn::Scheduler scheduler(pump.io);
  std::size_t outstanding = 0;
  std::size_t peak = 0;
  std::size_t issued = 0;
  std::vector<std::size_t> held;
  bool released = false;
  const auto respond = [&](std::size_t index) {
    scheduler.schedule(ndn::time::milliseconds(5), [&, index] {
      --outstanding;
      pump.face.receive(packets.at(index));
    });
  };
  auto connection = pump.face.onSendInterest.connect([&](const ndn::Interest& interest) {
    const auto index = interest.getName()[-1].toSegment();
    ++issued;
    peak = std::max(peak, ++outstanding);
    if (issued > 256 && !released) {
      held.push_back(index);
      if (held.size() != 65) return;
      released = true;
      for (const auto packet : held) respond(packet);
      held.clear();
    }
    else {
      respond(index);
    }
  });
  FacePumpStop stop{pump};
  const detail::FaceDataReferenceFetcher fetcher(pump.face, 1s, 5s);
  const auto result = fetcher(reference);
  pump.stop();
  BOOST_REQUIRE_EQUAL(result->packetCount(), packets.size());
  BOOST_CHECK_EQUAL(issued, packets.size());
  BOOST_CHECK_GT(peak, 64U);
  BOOST_CHECK_LT(peak, 128U);
  for (std::size_t i = 0; i < packets.size(); ++i)
    BOOST_CHECK(result->readPacket(i) == wireOf(packets[i]));
}

BOOST_AUTO_TEST_CASE(DefaultWindowGrowsBeyond256WithoutChangingOriginalPackets)
{
  FacePump pump;
  const ndn::Name prefix("/producer/model/automatic-window");
  RepoDataReference reference;
  reference.dataPrefix = prefix.toUri();
  reference.hasFinalSegment = true;
  reference.finalSegment = 32767;
  std::vector<ndn::Data> packets;
  for (unsigned i = 0; i < 32768; ++i) {
    packets.push_back(makeData(ndn::Name(prefix).appendSegment(i), i));
    reference.expectedSize += wireOf(packets.back()).size();
  }
  std::promise<void> expandedFlight;
  auto expanded = expandedFlight.get_future();
  std::vector<std::size_t> held;
  std::atomic<std::size_t> issued{0};
  bool release = false;
  auto connection = pump.face.onSendInterest.connect([&] (const ndn::Interest& interest) {
    const auto count = ++issued;
    const auto index = interest.getName()[-1].toSegment();
    // Additive growth needs enough acknowledged packets before a >256 flight.
    if (release || count <= 32000) {
      pump.face.receive(packets.at(index));
      return;
    }
    held.push_back(index);
    if (held.size() != 257)
      return;
    auto ready = std::move(held);
    held.clear();
    expandedFlight.set_value();
    release = true;
    for (const auto packet : ready)
      pump.face.receive(packets.at(packet));
  });
  FacePumpStop stop{pump};
  // This enlarged additive-growth fixture processes 32K packets under sanitizers.
  // Its fixture budget does not change per-Interest lifetime or product deadlines.
  const detail::FaceDataReferenceFetcher fetcher(pump.face, 1s, 40s);
  auto fetched = std::async(std::launch::async, [&] { return fetcher(reference); });
  const auto expansion = expanded.wait_for(30s);
  BOOST_TEST_MESSAGE("FINITE_STARTUP_GROWTH issued=" << issued.load()
    << " expanded=" << (expansion == std::future_status::ready)
    << " fetchReady=" << (fetched.wait_for(0s) == std::future_status::ready));
  if (expansion != std::future_status::ready &&
      fetched.wait_for(0s) == std::future_status::ready)
    fetched.get();
  BOOST_REQUIRE(expansion == std::future_status::ready);
  const auto result = fetched.get();
  pump.stop();
  BOOST_REQUIRE_EQUAL(result->packetCount(), packets.size());
  BOOST_CHECK_EQUAL(issued.load(), packets.size());
  for (std::size_t i = 0; i < packets.size(); ++i)
    BOOST_CHECK(result->readPacket(i) == wireOf(packets[i]));
}

BOOST_AUTO_TEST_CASE(AdaptiveWindowGrowsBeyondInitialFlight)
{
  FacePump pump;
  const ndn::Name prefix("/producer/model/adaptive");
  RepoDataReference reference;
  reference.dataPrefix = prefix.toUri();
  reference.hasFinalSegment = true;
  reference.finalSegment = 95;
  std::vector<ndn::Data> packets;
  for (unsigned i = 0; i < 96; ++i) {
    packets.push_back(makeData(ndn::Name(prefix).appendSegment(i), i));
    reference.expectedSize += wireOf(packets.back()).size();
  }
  std::promise<void> initialFlight;
  std::promise<void> expandedFlight;
  auto initial = initialFlight.get_future();
  auto expanded = expandedFlight.get_future();
  unsigned issued = 0;
  bool release = false;
  auto connection = pump.face.onSendInterest.connect([&] (const ndn::Interest& interest) {
    ++issued;
    if (issued == 16)
      initialFlight.set_value();
    if (issued == 48)
      expandedFlight.set_value();
    if (release)
      pump.face.receive(packets.at(interest.getName()[-1].toSegment()));
  });
  FacePumpStop stop{pump};
  const detail::FaceDataReferenceFetcher fetcher(pump.face, 1s, 3s, 64);
  auto fetched = std::async(std::launch::async, [&] { return fetcher(reference); });
  BOOST_REQUIRE(initial.wait_for(1s) == std::future_status::ready);
  boost::asio::post(pump.io, [&] {
    for (unsigned i = 0; i < 16; ++i)
      pump.face.receive(packets[i]);
  });
  BOOST_REQUIRE(expanded.wait_for(1s) == std::future_status::ready);
  boost::asio::post(pump.io, [&] {
    release = true;
    for (unsigned i = 16; i < 48; ++i)
      pump.face.receive(packets[i]);
  });
  const auto result = fetched.get();
  pump.stop();
  BOOST_REQUIRE_EQUAL(result->packetCount(), packets.size());
  for (unsigned i = 0; i < packets.size(); ++i)
    BOOST_CHECK(result->readPacket(i) == wireOf(packets[i]));
  BOOST_CHECK_EQUAL(issued, packets.size());
}

BOOST_AUTO_TEST_CASE(NamedReadUsesUpperBoundWithoutInventingExactManifest)
{
  FacePump pump;
  const std::vector<ndn::Name> names{
    ndn::Name("/publisher/model/root"), ndn::Name("/publisher/model/index/v=0/seg=0")};
  std::map<ndn::Name, ndn::Data> packets;
  std::vector<std::vector<uint8_t>> expected;
  for (std::size_t i = 0; i < names.size(); ++i) {
    auto data = makeData(names[i], static_cast<uint8_t>(i));
    expected.push_back(wireOf(data));
    packets.emplace(names[i], std::move(data));
  }
  std::atomic<bool> routeMatches{true};
  auto connection = pump.face.onSendInterest.connect([&] (const ndn::Interest& interest) {
    if (interest.getForwardingHint().size() != 1 ||
        interest.getForwardingHint().front() != ndn::Name("/repo/actor") ||
        interest.getCanBePrefix() || interest.getMustBeFresh())
      routeMatches.store(false);
    const auto found = packets.find(interest.getName());
    if (found != packets.end())
      pump.face.receive(found->second);
  });
  FacePumpStop stop{pump};
  const detail::FaceDataReferenceFetcher fetcher(pump.face, 100ms, 1s, 2);
  const auto spool = fetcher.fetchNamed(names, 4096, ndn::Name("/repo/actor"));
  BOOST_REQUIRE_EQUAL(spool->packetCount(), 2);
  BOOST_CHECK_LT(spool->totalWireBytes(), 4096);
  BOOST_CHECK(spool->readPacket(0) == expected[0]);
  BOOST_CHECK(spool->readPacket(1) == expected[1]);
  BOOST_CHECK(routeMatches.load());
}

BOOST_AUTO_TEST_CASE(NamedReadRejectsDuplicateBudgetAndCancelledOwnerBeforeInterests)
{
  FacePump pump;
  std::atomic<std::size_t> interests{0};
  auto connection = pump.face.onSendInterest.connect([&] (const auto&) { ++interests; });
  FacePumpStop stop{pump};
  const detail::FaceDataReferenceFetcher fetcher(pump.face, 100ms, 1s, 2);
  const ndn::Name name("/publisher/model/root");
  std::vector<ndn::Name> tooMany;
  for (std::size_t i = 0; i <= detail::MAX_NAMED_PACKET_COUNT; ++i)
    tooMany.push_back(ndn::Name("/publisher/model").appendSegment(i));
  BOOST_CHECK_THROW(fetcher.fetchNamed(tooMany, detail::MAX_NAMED_PACKET_WIRE_BYTES,
    ndn::Name("/repo")), std::invalid_argument);
  BOOST_CHECK_THROW(fetcher.fetchNamed({name, name}, 4096, ndn::Name("/repo")),
                    std::invalid_argument);
  BOOST_CHECK_THROW(fetcher.fetchNamed({name}, detail::MAX_NAMED_PACKET_WIRE_BYTES + 1,
    ndn::Name("/repo")),
                    std::invalid_argument);
  BOOST_CHECK_THROW(fetcher.fetchNamed({name}, 4096, ndn::Name("/repo"), [] {
    throw std::runtime_error("owner closed");
  }), std::runtime_error);
  BOOST_CHECK_EQUAL(interests.load(), 0);
}

void
checkOwnerCancellation(bool exactRead)
{
  FacePump pump;
  const auto name = ndn::Name("/publisher/model/root");
  const auto data = makeData(name, 7);
  std::atomic<bool> cancelled{false};
  std::atomic<bool> respond{false};
  auto connection = pump.face.onSendInterest.connect(
    [&] (const ndn::Interest& interest) {
      if (respond.load(std::memory_order_acquire) && interest.getName() == name)
        pump.face.receive(data);
    });
  FacePumpStop stop{pump};
  const detail::FaceDataReferenceFetcher fetcher(pump.face, 1s, 2s, 1);
  RepoDataReference reference;
  reference.objectName = name.toUri();
  reference.dataPrefix = name.toUri();
  reference.expectedSize = wireOf(data).size();
  const auto fetch = [&](std::function<void()> requireCurrent = {}) {
    return exactRead ? fetcher(reference, std::move(requireCurrent))
      : fetcher.fetchNamed({name}, 4096, ndn::Name("/repo"), std::move(requireCurrent));
  };
  std::thread cancelOwner([&] {
    std::this_thread::sleep_for(50ms);
    cancelled.store(true, std::memory_order_release);
  });
  const auto start = std::chrono::steady_clock::now();
  BOOST_CHECK_EXCEPTION(
    fetch([&] {
      if (cancelled.load(std::memory_order_acquire))
        throw std::runtime_error("owner closed");
    }), std::runtime_error, [] (const std::runtime_error& error) {
      return std::string(error.what()) == "repo-original-packet-fetch-cancelled";
    });
  cancelOwner.join();
  BOOST_CHECK(std::chrono::steady_clock::now() - start < 500ms);

  // Cancellation is posted to the Face strand. A new operation may start only
  // after that owner has cancelled its pending Interest and drained the lease.
  respond.store(true, std::memory_order_release);
  const auto drainDeadline = std::chrono::steady_clock::now() + 1s;
  bool fetched = false;
  while (!fetched && std::chrono::steady_clock::now() < drainDeadline) {
    try {
      const auto spool = fetch();
      fetched = spool->packetCount() == 1 && spool->readPacket(0) == wireOf(data);
    }
    catch (const std::runtime_error& error) {
      if (std::string(error.what()) != "repo-original-packet-fetch-busy")
        throw;
      std::this_thread::sleep_for(5ms);
    }
  }
  BOOST_CHECK(fetched);
}

BOOST_AUTO_TEST_CASE(NamedReadWakesOnOwnerCancellationAndRetainsAdmissionUntilDrain)
{
  checkOwnerCancellation(false);
}

BOOST_AUTO_TEST_CASE(ExactReadWakesOnOwnerCancellationAndRetainsAdmissionUntilDrain)
{
  checkOwnerCancellation(true);
}

BOOST_AUTO_TEST_CASE(NamedReadKeepsLeaseUntilPausedCallbacksDrain)
{
  ndn::DummyClientFace face;
  std::weak_ptr<int> observed;
  {
    auto owner = std::make_shared<int>(1);
    observed = owner;
    const detail::FaceDataReferenceFetcher fetcher(face, 10ms, 5ms, 1, owner);
    owner.reset();
    BOOST_CHECK_THROW(fetcher.fetchNamed({ndn::Name("/publisher/model/root")}, 4096,
                                        ndn::Name("/repo")), std::runtime_error);
  }
  BOOST_CHECK(!observed.expired());
  face.getIoContext().restart();
  (void)face.getIoContext().poll();
  BOOST_CHECK(observed.expired());
}

BOOST_AUTO_TEST_CASE(PreservesOriginalPacketsAndExactInterestNames)
{
  FacePump pump;
  const ndn::Name dataPrefix("/producer/kvcache/snapshot/v1");
  const ndn::Name forwardingHint("/producer/route");
  std::map<std::string, ndn::Data> sourcePackets;
  std::vector<std::vector<uint8_t>> expectedWire;
  std::vector<uint8_t> expectedStream;
  for (uint64_t segment = 7; segment <= 9; ++segment) {
    auto name = dataPrefix;
    name.appendSegment(segment);
    auto data = makeData(name, static_cast<uint8_t>(segment));
    expectedWire.push_back(wireOf(data));
    expectedStream.insert(expectedStream.end(), expectedWire.back().begin(),
                          expectedWire.back().end());
    sourcePackets.emplace(name.toUri(), std::move(data));
  }

  std::vector<ndn::Name> receivedNames;
  std::atomic<bool> interestsMatch{true};
  auto connection = pump.face.onSendInterest.connect(
    [&] (const ndn::Interest& interest) {
      receivedNames.push_back(interest.getName());
      if (interest.getCanBePrefix() || interest.getMustBeFresh() ||
          interest.getForwardingHint().size() != 1 ||
          interest.getForwardingHint().front() != forwardingHint) {
        interestsMatch.store(false);
      }
      const auto it = sourcePackets.find(interest.getName().toUri());
      if (it != sourcePackets.end()) {
        auto firstName = dataPrefix;
        firstName.appendSegment(7);
        if (interest.getName() == firstName) {
          return;
        }
        pump.face.receive(it->second);
        if (interest.getName().get(-1).toSegment() == 8) {
          pump.face.receive(sourcePackets.at(firstName.toUri()));
        }
      }
    });

  FacePumpStop stop{pump};
  RepoDataReference reference;
  reference.dataPrefix = dataPrefix.toUri();
  reference.firstSegment = 7;
  reference.finalSegment = 9;
  reference.hasFinalSegment = true;
  reference.forwardingHint = forwardingHint.toUri();
  for (const auto& wire : expectedWire) {
    reference.expectedSize += wire.size();
  }

  const detail::FaceDataReferenceFetcher fetcher(pump.face, 1s, 3s, 2);
  const auto actualWire = fetcher(reference);

  BOOST_CHECK(interestsMatch.load());
  BOOST_REQUIRE_EQUAL(receivedNames.size(), 3);
  for (std::size_t i = 0; i < receivedNames.size(); ++i) {
    auto expectedName = dataPrefix;
    expectedName.appendSegment(reference.firstSegment + i);
    BOOST_CHECK_EQUAL(receivedNames[i].toUri(), expectedName.toUri());
  }
  BOOST_REQUIRE(actualWire);
  BOOST_REQUIRE_EQUAL(actualWire->packetCount(), expectedWire.size());
  BOOST_CHECK_EQUAL(actualWire->totalWireBytes(), reference.expectedSize);
  for (std::size_t i = 0; i < expectedWire.size(); ++i) {
    BOOST_CHECK(actualWire->readPacket(i) == expectedWire[i]);
  }
  BOOST_CHECK(actualWire->readRange(0, expectedStream.size()) == expectedStream);
  const auto crossPacketOffset = expectedWire.front().size() - 2;
  const std::vector<uint8_t> expectedCrossPacket(
    expectedStream.begin() + static_cast<std::ptrdiff_t>(crossPacketOffset),
    expectedStream.begin() + static_cast<std::ptrdiff_t>(crossPacketOffset + 7));
  BOOST_CHECK(actualWire->readRange(crossPacketOffset, 7) == expectedCrossPacket);
}

BOOST_AUTO_TEST_CASE(ExactIndexPreservesManifestSegmentsAndAuxiliaryNames)
{
  FacePump pump;
  RepoDataReference reference;
  reference.objectName = "/producer/kvcache/snapshot/manifest";
  reference.dataPrefix = "/producer";
  reference.packetSetMetadata.emplace();
  auto& metadata = *reference.packetSetMetadata;
  metadata.objectName = reference.objectName;
  metadata.objectType = "original-packet-set";
  metadata.packetNames = {reference.objectName, "/producer/kvcache/snapshot/seg=0",
                          "/producer/keys/snapshot/key"};
  std::map<ndn::Name, ndn::Data> packets;
  std::vector<std::vector<uint8_t>> expected;
  for (const auto& text : metadata.packetNames) {
    auto packet = makeData(ndn::Name(text), static_cast<uint8_t>(packets.size()));
    expected.push_back(wireOf(packet));
    reference.expectedSize += expected.back().size();
    const auto name = packet.getName();
    packets.emplace(name, std::move(packet));
  }
  metadata.size = reference.expectedSize;
  std::vector<uint8_t> originalWire;
  for (const auto& wire : expected) {
    originalWire.insert(originalWire.end(), wire.begin(), wire.end());
  }
  reference.expectedSha256 = RepoClient::makeManifest(reference.objectName,
    "original-packet-set", originalWire, 1, {}, "").sha256;
  metadata.sha256 = reference.expectedSha256;
  metadata.publicationIdentity = "opaque-producer-publication";
  const auto decoded = parseDataReferenceJson(reference.toJson());
  BOOST_REQUIRE(decoded.packetSetMetadata);
  BOOST_CHECK_EQUAL(decoded.toJson(), reference.toJson());
  auto connection = pump.face.onSendInterest.connect([&](const ndn::Interest& interest) {
    const auto found = packets.find(interest.getName());
    if (found != packets.end()) {
      pump.face.receive(found->second);
    }
  });
  FacePumpStop stop{pump};
  const detail::FaceDataReferenceFetcher fetcher(pump.face, 1s, 3s, 2);
  const auto spool = fetcher(decoded);
  BOOST_REQUIRE_EQUAL(spool->packetCount(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    BOOST_CHECK(spool->readPacket(i) == expected[i]);
  }

  auto invalid = decoded;
  invalid.hasFinalSegment = true;
  BOOST_CHECK_THROW(fetcher(invalid), std::invalid_argument);
  invalid = decoded;
  invalid.packetSetMetadata->packetNames.back() = "/another-producer/key";
  BOOST_CHECK_THROW(fetcher(invalid), std::invalid_argument);
  invalid = decoded;
  invalid.packetSetMetadata->packetNames.back() = invalid.objectName;
  BOOST_CHECK_THROW(fetcher(invalid), std::invalid_argument);
  invalid = decoded;
  invalid.packetSetMetadata->packetNames.clear();
  BOOST_CHECK_THROW(fetcher(invalid), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RetransmitsLostInitialWindowWithoutChangingOriginalPackets)
{
  FacePump pump;
  const ndn::Name prefix("/producer/kv/lost-window");
  std::vector<ndn::Data> packets;
  RepoDataReference reference;
  reference.dataPrefix = prefix.toUri();
  reference.hasFinalSegment = true;
  reference.finalSegment = 31;
  for (unsigned i = 0; i < 32; ++i) {
    packets.push_back(makeData(ndn::Name(prefix).appendSegment(i), i));
    reference.expectedSize += wireOf(packets.back()).size();
  }
  std::map<unsigned, std::vector<ndn::Interest::Nonce>> nonces;
  auto connection = pump.face.onSendInterest.connect([&] (const ndn::Interest& interest) {
    const auto index = static_cast<unsigned>(interest.getName()[-1].toSegment());
    auto& attempts = nonces[index];
    attempts.push_back(interest.getNonce());
    if (index >= 1 && index <= 14 && attempts.size() == 1)
      return;
    pump.face.receive(packets.at(index));
  });
  FacePumpStop stop{pump};
  const detail::FaceDataReferenceFetcher fetcher(pump.face, 40ms, 1s, 16);
  const auto result = fetcher(reference);
  pump.stop();
  BOOST_REQUIRE_EQUAL(result->packetCount(), packets.size());
  for (unsigned i = 0; i < packets.size(); ++i) {
    BOOST_CHECK(result->readPacket(i) == wireOf(packets[i]));
    BOOST_REQUIRE_EQUAL(nonces[i].size(), i >= 1 && i <= 14 ? 2U : 1U);
    if (nonces[i].size() == 2)
      BOOST_CHECK(nonces[i][0] != nonces[i][1]);
  }
  BOOST_CHECK_EQUAL(pump.face.getNPendingInterests(), 0U);
}

BOOST_AUTO_TEST_CASE(CancelsOtherPendingInterestsAfterNack)
{
  FacePump pump;
  const ndn::Name dataPrefix("/producer/kvcache/snapshot/v1");
  std::atomic<unsigned> interestCount{0};
  std::atomic<unsigned> firstCount{0}, siblingCount{0}, nextCount{0};
  std::atomic<bool> lifetimeBounded{true};
  auto connection = pump.face.onSendInterest.connect(
    [&] (const ndn::Interest& interest) {
      interestCount.fetch_add(1);
      if (interest.getInterestLifetime() > ndn::time::seconds(1))
        lifetimeBounded.store(false);
      if (interest.getName().get(-1).toSegment() == 7) {
        ++firstCount;
        ndn::lp::Nack nack(interest);
        nack.setReason(ndn::lp::NackReason::NO_ROUTE);
        pump.face.receive(nack);
      }
      else if (interest.getName().get(-1).toSegment() == 8)
        ++siblingCount;
      else
        ++nextCount;
    });

  FacePumpStop stop{pump};
  RepoDataReference reference;
  reference.dataPrefix = dataPrefix.toUri();
  reference.firstSegment = 7;
  reference.finalSegment = 9;
  reference.hasFinalSegment = true;
  reference.expectedSize = 300;

  const detail::FaceDataReferenceFetcher fetcher(pump.face, 5s, 2s, 2);
  BOOST_CHECK_THROW(fetcher(reference), std::runtime_error);
  BOOST_CHECK_EQUAL(interestCount.load(), 4);
  BOOST_CHECK_EQUAL(firstCount.load(), 3);
  BOOST_CHECK_EQUAL(siblingCount.load(), 1);
  BOOST_CHECK_EQUAL(nextCount.load(), 0);
  BOOST_CHECK(lifetimeBounded.load());

  auto pendingPromise = std::make_shared<std::promise<std::size_t>>();
  auto pending = pendingPromise->get_future();
  boost::asio::post(pump.io, [pendingPromise, face = &pump.face] {
    pendingPromise->set_value(face->getNPendingInterests());
  });
  BOOST_REQUIRE(pending.wait_for(1s) == std::future_status::ready);
  BOOST_CHECK_EQUAL(pending.get(), 0);
}

BOOST_AUTO_TEST_CASE(PersistentPacketLossStopsAfterThreeAttemptsAndReleasesPending)
{
  FacePump pump;
  std::atomic<unsigned> attempts{0};
  auto connection = pump.face.onSendInterest.connect([&] (const ndn::Interest&) { ++attempts; });
  FacePumpStop stop{pump};
  RepoDataReference reference;
  reference.dataPrefix = "/producer/kv/unavailable";
  reference.expectedSize = 100;
  const detail::FaceDataReferenceFetcher fetcher(pump.face, 30ms, 1s, 1);
  BOOST_CHECK_EXCEPTION(fetcher(reference), std::runtime_error, [] (const auto& error) {
    return std::string(error.what()) == "repo-original-packet-timeout";
  });
  BOOST_CHECK_EQUAL(attempts.load(), 3);
  auto promise = std::make_shared<std::promise<std::size_t>>();
  auto pending = promise->get_future();
  boost::asio::post(pump.io, [promise, face = &pump.face] {
    promise->set_value(face->getNPendingInterests());
  });
  BOOST_REQUIRE(pending.wait_for(1s) == std::future_status::ready);
  BOOST_CHECK_EQUAL(pending.get(), 0U);
}

BOOST_AUTO_TEST_CASE(RejectsInvalidReferenceAndBoundsUnavailableProducer)
{
  FacePump pump;
  const detail::FaceDataReferenceFetcher fetcher(pump.face, 200ms, 100ms, 1);

  RepoDataReference invalid;
  invalid.expectedSize = 1;
  BOOST_CHECK_THROW(fetcher(invalid), std::invalid_argument);

  std::atomic<unsigned> interestCount{0};
  auto connection = pump.face.onSendInterest.connect(
    [&] (const ndn::Interest&) {
      interestCount.fetch_add(1);
    });
  RepoDataReference unavailable;
  FacePumpStop stop{pump};
  unavailable.dataPrefix = "/producer/kvcache/snapshot/v1";
  unavailable.expectedSize = 100;
  BOOST_CHECK_THROW(fetcher(unavailable), std::runtime_error);
  BOOST_CHECK_GE(interestCount.load(), 1);
}

BOOST_AUTO_TEST_CASE(StoppedFaceCannotAccumulateTimedOutFetchesAcrossCopies)
{
  ndn::DummyClientFace face;
  const detail::FaceDataReferenceFetcher fetcher(face, 100ms, 5ms, 1);
  const auto copy = fetcher;
  RepoDataReference reference;
  reference.dataPrefix = "/producer/kv/stopped";
  reference.expectedSize = 100;
  BOOST_CHECK_THROW(fetcher(reference), std::runtime_error);
  for (unsigned i = 0; i < 3; ++i) {
    BOOST_CHECK_EXCEPTION(copy(reference), std::runtime_error,
      [](const std::runtime_error& error) {
        return std::string(error.what()) == "repo-original-packet-fetch-busy";
      });
  }
  face.getIoContext().restart();
  (void)face.getIoContext().poll();
  // Draining the abandoned operation must release admission for a new attempt.
  BOOST_CHECK_EXCEPTION(copy(reference), std::runtime_error,
    [](const std::runtime_error& error) {
      return std::string(error.what()) == "repo-original-packet-fetch-deadline";
    });
  face.getIoContext().restart();
  (void)face.getIoContext().poll();
}

BOOST_AUTO_TEST_CASE(CallerLeaseSurvivesFetcherDestructionUntilLateCallbacksDrain)
{
  ndn::DummyClientFace face;
  std::weak_ptr<int> observed;
  {
    auto lifetime = std::make_shared<int>(1);
    observed = lifetime;
    const detail::FaceDataReferenceFetcher fetcher(face, 100ms, 5ms, 1, lifetime);
    lifetime.reset();
    RepoDataReference reference;
    reference.dataPrefix = "/producer/kv/stopped";
    reference.expectedSize = 100;
    BOOST_CHECK_THROW(fetcher(reference), std::runtime_error);
  }
  BOOST_CHECK(!observed.expired());
  face.getIoContext().restart();
  (void)face.getIoContext().poll();
  BOOST_CHECK(observed.expired());
}

BOOST_AUTO_TEST_CASE(SpoolRejectsUnboundedBudgetsBeforeAllocatingPacketIndex)
{
  BOOST_CHECK_THROW(detail::FaceDataReferencePacketSpool(0, 100), std::invalid_argument);
  BOOST_CHECK_THROW(detail::FaceDataReferencePacketSpool(1000001, 1000001),
    std::invalid_argument);
  BOOST_CHECK_THROW(detail::FaceDataReferencePacketSpool(1, REPO_USER_STORAGE_OBJECT_MAX_BYTES + 1),
    std::invalid_argument);
  detail::FaceDataReferencePacketSpool spool(2, 8);
  BOOST_CHECK_THROW(spool.finalize(), std::runtime_error);
  spool.append(1, {3, 4});
  BOOST_CHECK_THROW(spool.append(1, {3, 4}), std::runtime_error);
  spool.append(0, {1, 2});
  spool.finalize();
  BOOST_CHECK(spool.readRange(0, 4) == std::vector<uint8_t>({1, 2, 3, 4}));
  BOOST_CHECK_THROW(spool.readRange(4, 1), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(SpoolSequentialWireBytesRemainExactAcrossBufferBoundaries)
{
  constexpr std::size_t count = 512;
  constexpr std::size_t bytes = 7000;
  detail::FaceDataReferencePacketSpool spool(count, count * bytes);
  std::vector<uint8_t> expected;
  for (std::size_t i = 0; i < count; ++i) {
    const std::vector<uint8_t> wire(bytes, static_cast<uint8_t>(i));
    expected.insert(expected.end(), wire.begin(), wire.end());
    spool.append(i, wire);
  }
  spool.finalize();
  BOOST_CHECK_EQUAL(spool.totalWireBytes(), expected.size());
  BOOST_CHECK(spool.readRange(0, expected.size()) == expected);
}

BOOST_AUTO_TEST_CASE(SpoolReadAppendTransitionsPreservePhysicalAndLogicalOrder)
{
  constexpr std::size_t count = 64;
  constexpr std::size_t bytes = 7000;
  detail::FaceDataReferencePacketSpool spool(count, count * bytes);
  std::vector<std::vector<uint8_t>> wires;
  std::vector<uint8_t> expected;
  for (std::size_t i = 0; i < count; ++i) {
    wires.emplace_back(bytes, static_cast<uint8_t>(i * 17));
    expected.insert(expected.end(), wires.back().begin(), wires.back().end());
  }
  spool.append(count - 1, wires.back());
  BOOST_CHECK(spool.readPacket(count - 1) == wires.back());
  for (std::size_t i = 0; i < count - 1; ++i) {
    spool.append(i, wires[i]);
    if (i == 15) BOOST_CHECK(spool.readPacket(0) == wires[0]);
    // Read exactly to physical EOF, then switch back to append without a
    // short read setting EOF. Update streams still require a positioning call.
    if (i == 31) BOOST_CHECK(spool.readPacket(i) == wires[i]);
  }
  spool.finalize();
  BOOST_CHECK(spool.readRange(0, expected.size()) == expected);
  const std::vector<uint8_t> crossing(expected.begin() + 65000, expected.begin() + 85000);
  BOOST_CHECK(spool.readRange(65000, crossing.size()) == crossing);
  BOOST_CHECK(spool.readPacket(0) == wires.front());
  BOOST_CHECK(spool.readPacket(count - 1) == wires.back());
}

BOOST_AUTO_TEST_CASE(LateProducerCompletionCannotReturnSuccessfulSpool)
{
  FacePump pump;
  const auto data = makeData(ndn::Name("/producer/kv/late"), 7);
  auto completed = std::make_shared<std::atomic<bool>>(false);
  auto connection = pump.face.onSendInterest.connect([data, completed, face = &pump.face](const auto&) {
    std::this_thread::sleep_for(100ms);
    face->receive(data);
    completed->store(true);
  });
  RepoDataReference reference;
  reference.dataPrefix = data.getName().toUri();
  reference.expectedSize = wireOf(data).size();
  const detail::FaceDataReferenceFetcher fetcher(pump.face, 200ms, 50ms, 1);
  BOOST_CHECK_THROW(fetcher(reference), std::runtime_error);
  auto barrier = std::make_shared<std::promise<void>>();
  auto drained = barrier->get_future();
  boost::asio::post(pump.io, [barrier] { barrier->set_value(); });
  BOOST_REQUIRE(drained.wait_for(1s) == std::future_status::ready);
  BOOST_CHECK(completed->load());
}

BOOST_AUTO_TEST_SUITE_END()
