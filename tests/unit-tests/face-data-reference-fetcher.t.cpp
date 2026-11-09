#include "NDNSF-DistributedRepo/src/face-data-reference-fetcher.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/encoding/tlv.hpp>
#include <ndn-cxx/lp/nack.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/test/unit_test.hpp>

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

} // namespace

BOOST_AUTO_TEST_SUITE(FaceDataReferenceFetcher)

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
  BOOST_CHECK_THROW(fetcher.fetchNamed({name, name}, 4096, ndn::Name("/repo")),
                    std::invalid_argument);
  BOOST_CHECK_THROW(fetcher.fetchNamed({name}, (8U << 20) + 1, ndn::Name("/repo")),
                    std::invalid_argument);
  BOOST_CHECK_THROW(fetcher.fetchNamed({name}, 4096, ndn::Name("/repo"), [] {
    throw std::runtime_error("owner closed");
  }), std::runtime_error);
  BOOST_CHECK_EQUAL(interests.load(), 0);
}

BOOST_AUTO_TEST_CASE(NamedReadWakesOnOwnerCancellationAndRetainsAdmissionUntilDrain)
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
  std::thread cancelOwner([&] {
    std::this_thread::sleep_for(50ms);
    cancelled.store(true, std::memory_order_release);
  });
  const auto start = std::chrono::steady_clock::now();
  BOOST_CHECK_EXCEPTION(
    fetcher.fetchNamed({name}, 4096, ndn::Name("/repo"), [&] {
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
      const auto spool = fetcher.fetchNamed({name}, 4096, ndn::Name("/repo"));
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
