#include "FaceDataReferenceFetcher.hpp"
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
    work.reset();
    io.stop();
    if (thread.joinable()) {
      thread.join();
    }
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
  metadata.publicationIdentity = "opaque-producer-publication";
  const auto decoded = parseDataReferenceJson(reference.toJson());
  BOOST_REQUIRE(decoded.packetSetMetadata);
  BOOST_CHECK_EQUAL(decoded.toJson(), reference.toJson());
  auto connection = pump.face.onSendInterest.connect([&](const ndn::Interest& interest) {
    const auto found = packets.find(interest.getName());
    if (found != packets.end()) pump.face.receive(found->second);
  });
  const detail::FaceDataReferenceFetcher fetcher(pump.face, 1s, 3s, 2);
  const auto spool = fetcher(decoded);
  BOOST_REQUIRE_EQUAL(spool->packetCount(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i)
    BOOST_CHECK(spool->readPacket(i) == expected[i]);

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

BOOST_AUTO_TEST_CASE(CancelsOtherPendingInterestsAfterNack)
{
  FacePump pump;
  const ndn::Name dataPrefix("/producer/kvcache/snapshot/v1");
  std::atomic<unsigned> interestCount{0};
  auto connection = pump.face.onSendInterest.connect(
    [&] (const ndn::Interest& interest) {
      interestCount.fetch_add(1);
      if (interest.getName().get(-1).toSegment() == 7) {
        ndn::lp::Nack nack(interest);
        nack.setReason(ndn::lp::NackReason::NO_ROUTE);
        pump.face.receive(nack);
      }
    });

  RepoDataReference reference;
  reference.dataPrefix = dataPrefix.toUri();
  reference.firstSegment = 7;
  reference.finalSegment = 9;
  reference.hasFinalSegment = true;
  reference.expectedSize = 300;

  const detail::FaceDataReferenceFetcher fetcher(pump.face, 1s, 2s, 2);
  BOOST_CHECK_THROW(fetcher(reference), std::runtime_error);
  BOOST_CHECK_EQUAL(interestCount.load(), 2);

  std::promise<std::size_t> pendingPromise;
  auto pending = pendingPromise.get_future();
  boost::asio::post(pump.io, [&] {
    pendingPromise.set_value(pump.face.getNPendingInterests());
  });
  BOOST_REQUIRE(pending.wait_for(1s) == std::future_status::ready);
  BOOST_CHECK_EQUAL(pending.get(), 0);
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
  unavailable.dataPrefix = "/producer/kvcache/snapshot/v1";
  unavailable.expectedSize = 100;
  BOOST_CHECK_THROW(fetcher(unavailable), std::runtime_error);
  BOOST_CHECK_GE(interestCount.load(), 1);
}

BOOST_AUTO_TEST_SUITE_END()
