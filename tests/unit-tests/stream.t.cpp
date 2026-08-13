/* -*- Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil -*- */

#include "ndn-service-framework/Stream.hpp"
#include "tests/boost-test.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>

#include <atomic>
#include <fstream>
#include <iterator>
#include <memory>
#include <thread>

namespace ndn_service_framework::test {

namespace {

std::string
toHex(const ndn::Block& block)
{
  static constexpr char DIGITS[] = "0123456789abcdef";
  std::string result;
  result.reserve(block.size() * 2);
  for (const auto octet : block) {
    result.push_back(DIGITS[octet >> 4]);
    result.push_back(DIGITS[octet & 0x0f]);
  }
  return result;
}

std::string
toHex(const StreamContentDigest& digest)
{
  static constexpr char DIGITS[] = "0123456789abcdef";
  std::string result;
  result.reserve(digest.size() * 2);
  for (const auto octet : digest) {
    result.push_back(DIGITS[octet >> 4]);
    result.push_back(DIGITS[octet & 0x0f]);
  }
  return result;
}

boost::property_tree::ptree
readStreamFixture(const std::string& fileName)
{
  const std::vector<std::string> candidates = {
    "tests/fixtures/stream-prefetch/" + fileName,
    "../tests/fixtures/stream-prefetch/" + fileName,
  };
  for (const auto& path : candidates) {
    std::ifstream input(path);
    if (input.good()) {
      boost::property_tree::ptree fixture;
      boost::property_tree::read_json(input, fixture);
      return fixture;
    }
  }
  throw std::runtime_error("cannot locate stream-prefetch fixture " + fileName);
}

const boost::property_tree::ptree&
fixtureOperation(const boost::property_tree::ptree& fixture, size_t index)
{
  const auto& operations = fixture.get_child("operations");
  if (index >= operations.size()) {
    throw std::out_of_range("stream fixture operation index");
  }
  auto it = operations.begin();
  std::advance(it, static_cast<std::ptrdiff_t>(index));
  return it->second;
}

std::vector<uint64_t>
fixtureNumbers(const boost::property_tree::ptree& operation,
               const std::string& key)
{
  std::vector<uint64_t> values;
  for (const auto& item : operation.get_child(key)) {
    values.push_back(item.second.get_value<uint64_t>());
  }
  return values;
}

ndn::Name
makePayloadPrefix(uint64_t version)
{
  ndn::Name prefix("/uav/7/video");
  prefix.appendVersion(version);
  return prefix;
}

ndn::Name
makePayloadName(const ndn::Name& prefix, uint64_t cursor)
{
  ndn::Name name(prefix);
  name.append("packet").appendSequenceNumber(cursor);
  return name;
}

StreamNameMapBlock
makeMapBlock(uint64_t blockNumber,
             const ndn::Name& payloadPrefix,
             const std::optional<StreamContentDigest>& previous = std::nullopt)
{
  StreamNameMapBlock block;
  block.streamId = "front-camera";
  block.sessionEpoch = 17;
  block.mappingVersion = 23;
  block.blockNumber = blockNumber;
  block.blockCapacity = 4;
  block.firstCursor = blockNumber * block.blockCapacity;
  block.previousContentDigest = previous;
  for (uint64_t slot = 0; slot < block.blockCapacity; ++slot) {
    const auto cursor = block.firstCursor + slot;
    if (blockNumber == 0 && slot == 2) {
      block.entries.push_back(StreamNameMapEntry::makeTombstone());
    }
    else {
      block.entries.push_back(StreamNameMapEntry::fromName(
        makePayloadName(payloadPrefix, cursor)));
    }
  }
  return block;
}

VerifiedStreamNameMapData
makeVerifiedMapData(const StreamNameMapBlock& block,
                    const ndn::Name& provider = ndn::Name("/uav/7"),
                    uint64_t receivedMs = 100,
                    uint64_t requiredBeforeMs = 200)
{
  VerifiedStreamNameMapData input;
  input.dataName = makeStreamNameMapBlockName(
    makeStreamNameMapRoot(ndn::Name("/uav/7"), block.streamId),
    block.mappingVersion, block.blockNumber);
  input.verifiedProvider = provider;
  input.contentType = ndn::tlv::ContentType_Manifest;
  input.hasFinalBlock = false;
  input.content = block.canonicalContent();
  input.signedWireSize = input.content.size() + 200;
  input.receivedMonotonicMs = receivedMs;
  input.requiredBeforeMonotonicMs = requiredBeforeMs;
  return input;
}

StreamNameMapResolverConfig
makeResolverConfig(const ndn::Name& payloadPrefix)
{
  StreamNameMapResolverConfig config;
  config.streamId = "front-camera";
  config.sessionEpoch = 17;
  config.mappingVersion = 23;
  config.blockCapacity = 4;
  config.expectedProvider = ndn::Name("/uav/7");
  config.mappingRoot = makeStreamNameMapRoot(config.expectedProvider, config.streamId);
  config.payloadPrefix = payloadPrefix;
  config.signedWireCap = ndn::MAX_NDN_PACKET_SIZE;
  config.maxVerifiedBlocks = 8;
  config.maxQuarantineBlocks = 4;
  config.maxOriginalNameWireBytes = 1024;
  return config;
}

StreamNameMapCheckpoint
makeCheckpoint(const StreamNameMapBlock& anchor,
               StreamCursor oldestRetained = 0,
               StreamCursor latestJoin = 1,
               StreamCursor latestProduced = 2,
               StreamCursor committedThrough = 7,
               StreamCursor nextReserved = 8)
{
  StreamNameMapCheckpoint checkpoint;
  checkpoint.frontiers.oldestRetained = oldestRetained;
  checkpoint.frontiers.latestJoin = latestJoin;
  checkpoint.frontiers.latestProduced = latestProduced;
  checkpoint.frontiers.mappingCommittedThrough = committedThrough;
  checkpoint.frontiers.nextReserved = nextReserved;
  checkpoint.blockNumber = anchor.blockNumber;
  checkpoint.contentDigest = anchor.contentDigest();
  return checkpoint;
}

} // namespace

BOOST_AUTO_TEST_SUITE(Stream)

BOOST_AUTO_TEST_CASE(StreamInfoRoundTrip)
{
  StreamInfo info;
  info.streamId = "stream-1";
  info.sessionEpoch = 7;
  info.streamPrefix = ndn::Name("/example/drone/video/stream-1");
  info.nextSeq = 3;
  info.contentType = "video/h264";
  info.window = 48;
  info.metadata["fps"] = "15";

  StreamInfo parsed;
  BOOST_REQUIRE(parsed.wireDecode(info.wireEncode()));

  BOOST_CHECK_EQUAL(parsed.streamId, "stream-1");
  BOOST_CHECK_EQUAL(parsed.sessionEpoch, 7);
  BOOST_CHECK(parsed.streamPrefix == ndn::Name("/example/drone/video/stream-1"));
  BOOST_CHECK(parsed.chunkName(9) == ndn::Name("/example/drone/video/stream-1/%09"));
  BOOST_CHECK_EQUAL(parsed.contentType, "video/h264");
  BOOST_CHECK_EQUAL(parsed.metadata.at("fps"), "15");
}

BOOST_AUTO_TEST_CASE(StreamChunkRoundTripKeepsPayloadAndFec)
{
  StreamChunk chunk;
  chunk.streamId = "stream-1";
  chunk.sessionEpoch = 1;
  chunk.seq = 42;
  chunk.payload = {0, 1, 2, 3, 4};
  chunk.contentType = "video/h264";
  chunk.captureMs = 1000;
  chunk.keyChunk = true;
  chunk.frameId = 10;
  chunk.frameFirstSeq = 40;
  chunk.frameLastSeq = 43;
  chunk.segmentIndex = 2;
  chunk.segmentCount = 4;
  chunk.metadata["roi"] = "foreground";

  StreamFecInfo fec;
  fec.scheme = "xor-parity";
  fec.dataShards = 3;
  fec.parityShards = 1;
  fec.symbolIndex = 2;
  fec.symbolCount = 4;
  fec.dataLengths = {10, 11, 12};
  fec.sourceBlockId = "frame-10";
  chunk.fec = fec;

  StreamChunk parsed;
  BOOST_REQUIRE(parsed.wireDecode(chunk.wireEncode()));

  BOOST_CHECK_EQUAL(parsed.streamId, chunk.streamId);
  BOOST_CHECK_EQUAL(parsed.sessionEpoch, chunk.sessionEpoch);
  BOOST_CHECK_EQUAL(parsed.seq, chunk.seq);
  BOOST_CHECK(parsed.payload == chunk.payload);
  BOOST_CHECK_EQUAL(parsed.frameId, 10);
  BOOST_CHECK(parsed.keyChunk);
  BOOST_REQUIRE(parsed.fec);
  BOOST_CHECK_EQUAL(parsed.fec->dataLengths.size(), 3);
  BOOST_CHECK_EQUAL(parsed.fec->dataLengths[1], 11);
  BOOST_CHECK_EQUAL(parsed.metadata.at("roi"), "foreground");
}

BOOST_AUTO_TEST_CASE(ProducerBufferEvictsOldChunks)
{
  StreamProducerBuffer buffer(2);
  for (uint64_t seq = 0; seq < 3; ++seq) {
    StreamChunk chunk;
    chunk.streamId = "s";
    chunk.sessionEpoch = 1;
    chunk.seq = seq;
    chunk.payload = {static_cast<uint8_t>(seq)};
    buffer.put(chunk);
  }

  const auto seqs = buffer.sequences();
  BOOST_REQUIRE_EQUAL(seqs.size(), 2);
  BOOST_CHECK_EQUAL(seqs[0], 1);
  BOOST_CHECK_EQUAL(seqs[1], 2);
  BOOST_CHECK(!buffer.get(0));
  BOOST_REQUIRE(buffer.get(1));
  BOOST_REQUIRE(buffer.getEncoded(2));
  BOOST_CHECK_EQUAL(buffer.metrics().produced, 3);
  BOOST_CHECK_EQUAL(buffer.metrics().evicted, 1);
}

BOOST_AUTO_TEST_CASE(ConsumerReorderRejectsDuplicatesAndStaleChunks)
{
  StreamConsumerReorderBuffer buffer("s", 3, 0);

  StreamChunk one;
  one.streamId = "s";
  one.sessionEpoch = 3;
  one.seq = 1;
  one.payload = {'1'};
  BOOST_CHECK(buffer.push(one).empty());
  BOOST_REQUIRE_EQUAL(buffer.missingSequences().size(), 1);
  BOOST_CHECK_EQUAL(buffer.missingSequences()[0], 0);

  StreamChunk zero = one;
  zero.seq = 0;
  zero.payload = {'0'};
  const auto emitted = buffer.push(zero);
  BOOST_REQUIRE_EQUAL(emitted.size(), 2);
  BOOST_CHECK_EQUAL(emitted[0].payload[0], '0');
  BOOST_CHECK_EQUAL(emitted[1].payload[0], '1');

  BOOST_CHECK(buffer.push(one).empty());
  StreamChunk stale = one;
  stale.streamId = "old";
  stale.sessionEpoch = 2;
  BOOST_CHECK(buffer.push(stale).empty());
  BOOST_CHECK_EQUAL(buffer.metrics().duplicates, 1);
  BOOST_CHECK_EQUAL(buffer.metrics().stale, 1);
  BOOST_CHECK_EQUAL(buffer.metrics().emitted, 2);
}

BOOST_AUTO_TEST_CASE(ConsumerSkipToUnblocksLaterChunks)
{
  StreamConsumerReorderBuffer buffer("s", 1, 0);
  StreamChunk two;
  two.streamId = "s";
  two.sessionEpoch = 1;
  two.seq = 2;
  two.payload = {'2'};
  BOOST_CHECK(buffer.push(two).empty());
  buffer.skipTo(2);
  StreamChunk three = two;
  three.seq = 3;
  three.payload = {'3'};
  const auto emitted = buffer.push(three);
  BOOST_REQUIRE_EQUAL(emitted.size(), 2);
  BOOST_CHECK_EQUAL(emitted[0].payload[0], '2');
  BOOST_CHECK_EQUAL(emitted[1].payload[0], '3');
}

BOOST_AUTO_TEST_CASE(ConsumerPendingStateAndOverflowAreObservable)
{
  StreamConsumerReorderBuffer buffer("s", 1, 0, 2);
  StreamChunk two;
  two.streamId = "s";
  two.sessionEpoch = 1;
  two.seq = 2;
  two.payload = {'2', '2'};
  StreamChunk three = two;
  three.seq = 3;
  three.payload = {'3', '3', '3'};
  StreamChunk four = two;
  four.seq = 4;
  four.payload = {'4'};

  buffer.push(two);
  buffer.push(three);
  BOOST_CHECK_EQUAL(buffer.pendingCount(), 2);
  BOOST_CHECK_EQUAL(buffer.pendingBytes(), 5);

  buffer.push(four);
  BOOST_CHECK_EQUAL(buffer.pendingCount(), 2);
  BOOST_CHECK_EQUAL(buffer.pendingBytes(), 4);
  BOOST_CHECK_EQUAL(buffer.metrics().overflows, 1);
  BOOST_CHECK_EQUAL(buffer.metrics().maxPending, 2);
  BOOST_REQUIRE_EQUAL(buffer.pendingSequences().size(), 2);
  BOOST_CHECK_EQUAL(buffer.pendingSequences()[0], 3);
  buffer.skipTo(3);
  const auto drained = buffer.drainReady();
  BOOST_REQUIRE_EQUAL(drained.size(), 2);
  BOOST_CHECK_EQUAL(drained[0].seq, 3);
  BOOST_CHECK_EQUAL(drained[1].seq, 4);

  buffer.reset("new-session", 2, 10);
  BOOST_CHECK_EQUAL(buffer.pendingCount(), 0);
  BOOST_CHECK_EQUAL(buffer.metrics().received, 0);
  BOOST_CHECK_EQUAL(buffer.metrics().overflows, 0);
  BOOST_CHECK_EQUAL(buffer.metrics().maxPending, 0);
}

BOOST_AUTO_TEST_CASE(ConsumerReorderAcceptsFecRecoveredChunkWithoutGap)
{
  StreamConsumerReorderBuffer buffer("video", 11, 0);

  StreamChunk zero;
  zero.streamId = "video";
  zero.sessionEpoch = 11;
  zero.seq = 0;
  zero.payload = {'0'};

  StreamChunk recovered = zero;
  recovered.seq = 1;
  recovered.payload = {'1'};
  recovered.metadata["source"] = "fec-recovered";

  StreamChunk two = zero;
  two.seq = 2;
  two.payload = {'2'};

  const auto first = buffer.push(zero);
  BOOST_REQUIRE_EQUAL(first.size(), 1);
  BOOST_CHECK_EQUAL(first[0].payload[0], '0');

  const auto second = buffer.push(recovered);
  BOOST_REQUIRE_EQUAL(second.size(), 1);
  BOOST_CHECK_EQUAL(second[0].payload[0], '1');
  BOOST_CHECK_EQUAL(second[0].metadata.at("source"), "fec-recovered");

  const auto third = buffer.push(two);
  BOOST_REQUIRE_EQUAL(third.size(), 1);
  BOOST_CHECK_EQUAL(third[0].payload[0], '2');
  BOOST_CHECK(buffer.missingSequences().empty());
  BOOST_CHECK_EQUAL(buffer.metrics().gaps, 0);
  BOOST_CHECK_EQUAL(buffer.metrics().emitted, 3);
}

BOOST_AUTO_TEST_CASE(AdaptiveFetcherReactsToPressure)
{
  StreamAdaptiveFetcherState state;
  state.rttMs = 100.0;
  state.baseWindow = 32;
  state.baseLookahead = 8;
  const auto stable = state.decide();

  state.recordTimeout();
  state.recordTimeout();
  state.recordNack();
  state.setBacklogPressure(0.8);
  const auto congested = state.decide();

  BOOST_CHECK_EQUAL(stable.reason, "stable");
  BOOST_CHECK_EQUAL(stable.window, 32);
  BOOST_CHECK_EQUAL(stable.lookahead, 8);
  BOOST_CHECK_EQUAL(congested.reason, "congested");
  BOOST_CHECK_LT(congested.window, stable.window);
  BOOST_CHECK_LT(congested.lookahead, stable.lookahead);
  BOOST_CHECK_GT(congested.interestLifetimeMs, stable.interestLifetimeMs);
}

BOOST_AUTO_TEST_CASE(LivePrefetchDetectsFrameRateAndNormalizesFrameGaps)
{
  StreamAdaptiveFetcherState state;
  state.rttMs = 100.0;
  state.baseWindow = 8;
  state.liveEdgeWindow = 2;
  state.liveEdgeStableRequired = 2;
  state.detectionPeriodMs = 0;
  state.liveEdgeChangeThreshold = 0.10;
  state.liveEdgePeriodSimilarity = 0.95;
  state.resetLive(7, 40, 33.0, 0);

  BOOST_CHECK_EQUAL(toString(state.phase()), "CHASING");
  BOOST_CHECK(state.observeAcceptedSample(7, 1, 100, 100, 3, false));
  // The skipped frame is normalized: (166 - 100) / (3 - 1) == 33 ms.
  BOOST_CHECK(state.observeAcceptedSample(7, 3, 166, 100, 3, false));
  BOOST_CHECK(state.observeAcceptedSample(7, 4, 199, 100, 3, false));
  BOOST_CHECK(state.observeAcceptedSample(7, 5, 232, 100, 3, false));
  BOOST_CHECK(state.observeAcceptedSample(7, 6, 265, 100, 3, false));
  BOOST_CHECK(state.observeAcceptedSample(7, 7, 298, 100, 3, false));

  const auto decision = state.decide();
  BOOST_CHECK_EQUAL(decision.policyMode, "live-v1");
  BOOST_CHECK(decision.phase == StreamPrefetchPhase::Adjusting ||
              decision.phase == StreamPrefetchPhase::Fetching);
  BOOST_CHECK_GE(decision.liveEdgeConfidence, 1.0);
  BOOST_CHECK_EQUAL(decision.sampleDemand, 4);
  BOOST_CHECK_GE(decision.packetDemand, 10);
  BOOST_CHECK_GE(decision.interestLifetimeMs, 100);
}

BOOST_AUTO_TEST_CASE(MappedLiveChasingWindowControlsActualCursorHorizon)
{
  StreamAdaptiveFetcherState state;
  state.minWindow = 4;
  state.baseWindow = 32;
  state.maxWindow = 128;
  state.minLookahead = 2;
  state.maxLookahead = 128;
  state.configureMappedLive(64, 4, 1, 16, "ndnsf-balanced-seed");
  state.resetMappedLive(19, 0, 33.0, 31, 127, 128, 1000);

  const auto decision = state.decide(1000, 0);
  BOOST_REQUIRE(decision.phase == StreamPrefetchPhase::Chasing);
  BOOST_CHECK_EQUAL(decision.window, 32);
  BOOST_CHECK_EQUAL(decision.lookahead, decision.window);
  BOOST_CHECK_EQUAL(decision.payloadBeginCursor, 0);
  BOOST_CHECK_EQUAL(decision.payloadEndCursor, 31);
  BOOST_CHECK_GT(decision.lookahead, decision.packetDemand);
}

BOOST_AUTO_TEST_CASE(MappedLiveHorizonCannotExceedExpressibleCapacity)
{
  StreamAdaptiveFetcherState state;
  state.minWindow = 4;
  state.baseWindow = 128;
  state.maxWindow = 128;
  state.minLookahead = 2;
  state.maxLookahead = 128;
  state.configureMappedLive(24, 4, 1, 16, "ndnsf-balanced-seed");
  state.resetMappedLive(25, 0, 33.0, 31, 127, 128, 1000);

  const auto decision = state.decide(1000, 0);
  BOOST_CHECK_EQUAL(decision.window, 24);
  BOOST_CHECK_EQUAL(decision.lookahead, 24);
  BOOST_CHECK_LE(decision.payloadEndCursor - decision.payloadBeginCursor + 1, 24);
}

BOOST_AUTO_TEST_CASE(PaperPrefetchDetectionHoldUsesDecisionClock)
{
  StreamAdaptiveFetcherState state;
  state.minWindow = 4;
  state.baseWindow = 8;
  state.maxWindow = 64;
  state.liveEdgeWindow = 2;
  state.liveEdgeStableRequired = 1;
  state.detectionPeriodMs = 1000;
  state.resetLive(71, 0, 33.0, 1000);

  BOOST_REQUIRE(state.observeAcceptedSample(71, 1, 1100, 20.0, 1, false));
  BOOST_REQUIRE(state.observeAcceptedSample(71, 2, 1133, 20.0, 1, false));
  BOOST_REQUIRE(state.observeAcceptedSample(71, 3, 1166, 20.0, 1, false));
  BOOST_REQUIRE(state.observeAcceptedSample(71, 4, 1199, 20.0, 1, false));
  BOOST_REQUIRE(state.observeAcceptedSample(71, 5, 1232, 20.0, 1, false));

  // Stability before the post-action detection deadline is evidence only; it
  // must not mutate the pipeline or phase yet.
  BOOST_CHECK(state.phase() == StreamPrefetchPhase::Chasing);
  BOOST_CHECK_EQUAL(state.decide(1300, 0).window, 8);
  BOOST_CHECK_EQUAL(state.decide(1300, 0).holdMs, 700);
  // Status uses the current decision clock, not the last sample arrival time.
  BOOST_CHECK_EQUAL(state.decide(2200, 0).holdMs, 0);
}

BOOST_AUTO_TEST_CASE(PaperPrefetchAdjustmentRestoresPreviousUsableWindow)
{
  StreamAdaptiveFetcherState state;
  state.rttMs = 20.0;
  state.minWindow = 4;
  state.baseWindow = 32;
  state.maxWindow = 64;
  state.liveEdgeWindow = 2;
  state.liveEdgeStableRequired = 1;
  state.detectionPeriodMs = 0;
  state.resetLive(72, 0, 33.0, 0);

  BOOST_REQUIRE(state.observeAcceptedSample(72, 1, 100, 20.0, 1, false));
  BOOST_REQUIRE(state.observeAcceptedSample(72, 2, 133, 20.0, 1, false));
  BOOST_REQUIRE(state.observeAcceptedSample(72, 3, 166, 20.0, 1, false));
  BOOST_REQUIRE(state.observeAcceptedSample(72, 4, 199, 20.0, 1, false));
  BOOST_REQUIRE(state.observeAcceptedSample(72, 5, 232, 20.0, 1, false));
  BOOST_REQUIRE(state.phase() == StreamPrefetchPhase::Adjusting);

  // One stable adjustment withholds 25% of the pipeline.
  BOOST_REQUIRE(state.observeAcceptedSample(72, 6, 265, 20.0, 1, false));
  BOOST_CHECK_EQUAL(state.decide(265, 0).window, 24);

  // The first stale arrival after that reduction proves it was too small. The
  // paper restores the previous lambda_p (32); it does not start doubling again.
  BOOST_REQUIRE(state.observeAcceptedSample(72, 7, 500, 20.0, 1, false));
  const auto restored = state.decide(500, 0);
  BOOST_CHECK(restored.phase == StreamPrefetchPhase::Fetching);
  BOOST_CHECK_EQUAL(restored.window, 32);
  BOOST_CHECK_EQUAL(restored.lookahead, 32);
}

BOOST_AUTO_TEST_CASE(PaperPrefetchSeparatesGenerationWaitFromNetworkDelay)
{
  StreamAdaptiveFetcherState state;
  state.rttMs = 20.0;
  state.liveEdgeWindow = 2;
  state.liveEdgeStableRequired = 1;
  state.detectionPeriodMs = 0;
  state.resetLive(73, 0, 33.0, 0);

  state.observePayloadDelay(24.0, false);
  BOOST_CHECK_CLOSE(state.rttMs, 21.0, 0.01);

  // During live-edge search, this is DRD' = DRD + dgen. It cannot make 180 ms
  // of generation wait look like a network-path RTT increase.
  state.observePayloadDelay(180.0, true);
  BOOST_CHECK_CLOSE(state.rttMs, 21.0, 0.01);

  // A smaller ahead-mapped observation is a safe upper bound and may correct an
  // overestimate downward without knowing the producer's private dgen.
  state.observePayloadDelay(17.0, true);
  BOOST_CHECK_LT(state.rttMs, 21.0);
}

BOOST_AUTO_TEST_CASE(MappedLiveSegmentedSampleReservesOneCompleteSourceGroup)
{
  StreamAdaptiveFetcherState state;
  state.rttMs = 20.0;
  state.recoveryReservePackets = 12;
  state.resetLive(20, 0, 33.0, 0);
  for (uint64_t sample = 1; sample <= 20; ++sample) {
    BOOST_REQUIRE(state.observeAcceptedSample(
      20, sample, 100 + sample * 33, 20.0, 12, true));
  }
  const auto decision = state.decide(800, 0);
  BOOST_CHECK_EQUAL(decision.sampleDemand, 1);
  BOOST_CHECK_GE(decision.packetDemand, 24);
}

BOOST_AUTO_TEST_CASE(MappedLiveReorderGapRaisesDemandUntilBoundedHistoryDecays)
{
  StreamAdaptiveFetcherState state;
  state.rttMs = 20.0;
  state.liveEdgeWindow = 4;
  state.liveEdgeStableRequired = 4;
  state.detectionPeriodMs = 1000;
  state.configureMappedLive(64, 4, 1, 6, "ndnsf-balanced-seed");
  state.resetMappedLive(22, 0, 40.0, 5, 63, 64, 1000);
  state.setPredictedSampleGroups({4, 5, 6, 4});
  BOOST_REQUIRE(state.observeAcceptedSample(22, 1, 1040, 0.0, 2, true));
  BOOST_REQUIRE(state.observeAcceptedSample(22, 2, 1080, 0.0, 3, true));
  BOOST_REQUIRE(state.observeAcceptedSample(22, 3, 1200, 0.0, 4, true));

  const auto decision = state.decide(1200, 0);
  BOOST_CHECK_EQUAL(decision.sampleDemand, 3);
  BOOST_CHECK_GE(decision.packetDemand, 15);
  BOOST_CHECK_GE(decision.window, 19);
  BOOST_CHECK_LE(decision.window, decision.aggregateInFlightLimit);
}

BOOST_AUTO_TEST_CASE(MappedLivePressureCannotShrinkBelowNextAtomicGroup)
{
  StreamAdaptiveFetcherState state;
  state.rttMs = 20.0;
  state.configureMappedLive(16, 4, 1, 8, "ndnsf-balanced-seed");
  state.resetMappedLive(21, 0, 33.0, 7, 15, 16, 0);
  state.timeoutPressure = 1.0;
  state.setPredictedSampleGroups({6, 4});
  const auto decision = state.decide(1000, 0);
  BOOST_CHECK_GE(decision.window, 6);
  BOOST_CHECK_GE(decision.payloadBudget, 6);
  BOOST_CHECK_LE(decision.mappingBudget + decision.payloadBudget +
                   decision.retransmissionBudget,
                 decision.aggregateInFlightLimit);
}

BOOST_AUTO_TEST_CASE(MappedLiveControlReserveCannotBeStarvedByPayload)
{
  StreamAdaptiveFetcherState state;
  state.configureMappedLive(12, 2, 1, 8, "ndnsf-balanced-seed");
  state.resetMappedLive(23, 0, 30.0, 0, 7, 8, 0);

  state.setInFlight(0, 11, 0);
  auto decision = state.decide(1, 0);
  BOOST_CHECK_EQUAL(decision.mappingBudget, 1);
  BOOST_CHECK_EQUAL(decision.payloadBudget, 0);

  state.setInFlight(2, 9, 0);
  decision = state.decide(2, 0);
  BOOST_CHECK_EQUAL(decision.mappingBudget, 0);
  BOOST_CHECK_EQUAL(decision.retransmissionBudget, 1);
  BOOST_CHECK_EQUAL(decision.payloadBudget, 0);

  state.setInFlight(2, 8, 1);
  decision = state.decide(3, 0);
  BOOST_CHECK_EQUAL(decision.payloadBudget, 1);
}

BOOST_AUTO_TEST_CASE(MappedLiveTimeoutDrainRestoresSchedulingBudget)
{
  StreamAdaptiveFetcherState state;
  state.configureMappedLive(24, 4, 1, 8, "ndnsf-balanced-seed");
  state.resetMappedLive(24, 0, 33.0, 0, 31, 32, 0);

  // Model a full live batch followed by its final timeout callback. The
  // consumer owns the authoritative Interest sets and synchronizes the
  // drained counts before decide(); a stale full-window snapshot must not
  // suppress the only scheduling wakeup left in the system.
  state.setInFlight(4, 20, 0);
  const auto full = state.decide(1000);
  BOOST_CHECK_EQUAL(full.mappingBudget, 0);
  BOOST_CHECK_EQUAL(full.payloadBudget, 0);

  state.setInFlight(0, 0, 0);
  const auto drained = state.decide(1001);
  BOOST_CHECK_GT(drained.mappingBudget, 0);
  BOOST_CHECK_GT(drained.payloadBudget, 0);
}

BOOST_AUTO_TEST_CASE(LivePrefetchRejectsStaleAndInvalidObservations)
{
  StreamAdaptiveFetcherState state;
  state.liveEdgeWindow = 2;
  state.liveEdgeStableRequired = 2;
  state.resetLive(9, 0, 40.0, 0);

  BOOST_CHECK(!state.observeAcceptedSample(8, 1, 100, 20, 1, true));
  BOOST_CHECK(state.observeAcceptedSample(9, 1, 100, 20, 1, true));
  BOOST_CHECK(!state.observeAcceptedSample(9, 1, 120, 20, 1, true));
  BOOST_CHECK(!state.observeAcceptedSample(9, 2, 90, 20, 1, true));
  state.recordInvalidObservation();
  BOOST_CHECK_EQUAL(state.invalidObservations(), 4);
  BOOST_CHECK_EQUAL(toString(state.phase()), "CHASING");

  state.stopLive();
  BOOST_CHECK_EQUAL(toString(state.phase()), "STOPPED");
  BOOST_CHECK_EQUAL(toString(state.decide().phase), "STOPPED");
  BOOST_CHECK(!state.observeAcceptedSample(9, 2, 140, 20, 1, true));
}

BOOST_AUTO_TEST_CASE(MappedLiveCacheHitCannotCollapseChasingPathRtt)
{
  StreamAdaptiveFetcherState state;
  state.rttMs = 120.0;
  state.resetMappedLive(22, 0, 30.0, 0, 31, 32, 0);

  state.observePayloadDelay(1.0, true);
  BOOST_CHECK(state.phase() == StreamPrefetchPhase::Chasing);
  BOOST_CHECK_CLOSE(state.rttMs, 120.0, 0.001);
  BOOST_CHECK_EQUAL(state.decide(1, 0).sampleDemand, 4);

  // A known-produced response is a valid end-to-end observation and retains
  // the existing adaptive behavior.
  state.observePayloadDelay(40.0, false);
  BOOST_CHECK_LT(state.rttMs, 120.0);
}

BOOST_AUTO_TEST_CASE(MappedLiveControllerClassifiesBudgetCongestionAndRecovery)
{
  const auto fixture = readStreamFixture("controller-traces-v1.json");
  const auto& config = fixture.get_child("mappedLive");

  StreamAdaptiveFetcherState state;
  state.rttMs = config.get<double>("retrievalDelayMs");
  state.liveEdgeWindow = 2;
  state.liveEdgeStableRequired = 2;
  state.detectionPeriodMs = 0;
  state.configureMappedLive(
    config.get<uint64_t>("aggregateInFlightLimit"),
    config.get<uint64_t>("mappingReserve"),
    config.get<uint64_t>("retransmissionReserve"),
    config.get<uint64_t>("mappingBlockCapacity"),
    config.get<std::string>("detectorProfile"));
  state.liveEdgeWindow = 2;
  state.liveEdgeStableRequired = 2;
  state.detectionPeriodMs = 0;
  state.resetMappedLive(
    config.get<uint64_t>("sessionEpoch"),
    config.get<uint64_t>("nextCursor"),
    config.get<double>("samplePeriodMs"),
    config.get<uint64_t>("latestProducedCursor"),
    config.get<uint64_t>("mappingCommittedThroughCursor"),
    config.get<uint64_t>("nextReservedCursor"), 0);

  auto sampleIt = config.get_child("sampleIds").begin();
  auto arrivalIt = config.get_child("arrivalMs").begin();
  for (; sampleIt != config.get_child("sampleIds").end(); ++sampleIt, ++arrivalIt) {
    BOOST_REQUIRE(state.observeAcceptedSample(
      config.get<uint64_t>("sessionEpoch"),
      sampleIt->second.get_value<uint64_t>(),
      arrivalIt->second.get_value<uint64_t>(),
      config.get<double>("retrievalDelayMs"),
      config.get<uint64_t>("segmentsPerSample"), true));
  }

  auto decision = state.decide(340, 0);
  BOOST_CHECK_EQUAL(decision.policyMode, "mapped-live-v1-future-on");
  BOOST_CHECK_EQUAL(decision.detectorProfile,
                    config.get<std::string>("detectorProfile"));
  BOOST_CHECK(decision.phase == StreamPrefetchPhase::Adjusting ||
              decision.phase == StreamPrefetchPhase::Fetching);
  BOOST_CHECK(decision.mappingReady);
  BOOST_CHECK_EQUAL(decision.payloadBeginCursor,
                    config.get<uint64_t>("nextCursor"));
  BOOST_CHECK_LE(decision.payloadEndCursor,
                 config.get<uint64_t>("mappingCommittedThroughCursor"));
  BOOST_CHECK_EQUAL(decision.mappingBeginBlock, 25);
  BOOST_CHECK_LE(decision.mappingBudget + decision.payloadBudget +
                   decision.retransmissionBudget,
                 config.get<uint64_t>("aggregateInFlightLimit"));
  BOOST_CHECK_EQUAL(decision.sampleDemand, 3);

  const auto pressureBeforeFutureWait = state.timeoutPressure;
  state.recordTimeout(110, false, true);
  BOOST_CHECK_EQUAL(state.timeoutPressure, pressureBeforeFutureWait);
  decision = state.decide(350, 0);
  BOOST_CHECK(decision.futureWait);
  BOOST_CHECK_EQUAL(decision.futureWaitCount, 1);

  state.recordTimeout(101, true, false);
  BOOST_CHECK_GT(state.timeoutPressure, pressureBeforeFutureWait);
  state.recordNack(101, "congestion");
  state.recordCongestionMark(101, 1);
  decision = state.decide(351, 0);
  BOOST_CHECK(decision.congestionHold);
  BOOST_CHECK_LT(decision.aggregateInFlightLimit,
                 config.get<uint64_t>("aggregateInFlightLimit"));

  state.observeSampleExtent(5, 3);
  state.observeSampleExtent(5, 7);
  decision = state.decide(352, 0);
  BOOST_CHECK_EQUAL(decision.terminalUnproducedAdvice, 2);
  BOOST_CHECK_EQUAL(decision.laterCursorAdvice, 2);

  StreamAdaptiveFetcherState starved;
  starved.configureMappedLive(12, 2, 1, 4, "ndnsf-balanced-seed");
  starved.resetMappedLive(43, 100, 40.0, 103, 103, 120, 0);
  starved.advanceNextCursor(112);
  BOOST_CHECK(!starved.decide(352, 0).mappingReady);
  BOOST_CHECK_EQUAL(starved.decide(352, 0).mappingWaitReason,
                    "mapping-starved");
  starved.updateMappingFrontier(119, 128);
  BOOST_CHECK(starved.decide(353, 0).mappingReady);
  BOOST_CHECK_THROW(starved.updateMappingFrontier(99, 129),
                    std::invalid_argument);
  starved.setMappedLivePolicyEnabled(false);
  BOOST_CHECK_EQUAL(starved.decide(354, 0).policyMode, "mapped-pressure");
  BOOST_CHECK(starved.decide(354, 0).mappingReady);
  starved.setMappedLivePolicyEnabled(true);
  BOOST_CHECK_EQUAL(starved.decide(355, 0).policyMode,
                    "mapped-live-v1-future-on");

  state.beginRecovery(400, 460);
  decision = state.decide(420, 460);
  BOOST_CHECK(decision.phase == StreamPrefetchPhase::Recovering);
  BOOST_CHECK(decision.retransmissionEligible);
  BOOST_CHECK_EQUAL(decision.remainingRecoveryBudgetMs, 40);
  decision = state.decide(470, 460);
  BOOST_CHECK(!decision.retransmissionEligible);
  BOOST_CHECK_EQUAL(decision.remainingRecoveryBudgetMs, 0);
  state.recordRecovery(true);
  BOOST_CHECK(state.phase() == StreamPrefetchPhase::Fetching);
}

BOOST_AUTO_TEST_CASE(PaperLiteralDetectorRemainsSeparateFromNdnsfProfiles)
{
  const auto fixture = readStreamFixture("controller-traces-v1.json");
  const auto& paper = fixture.get_child("paperLiteral");
  StreamAdaptiveFetcherState state;
  state.liveEdgeWindow = 2;
  state.liveEdgeStableRequired = 2;
  state.detectionPeriodMs = 0;
  state.configureMappedLive(12, 2, 1, 4,
                            paper.get<std::string>("detectorProfile"));
  state.liveEdgeWindow = 2;
  state.liveEdgeStableRequired = 2;
  state.detectionPeriodMs = 0;
  state.resetMappedLive(42, 0, paper.get<double>("samplePeriodMs"),
                        3, 7, 8, 0);
  auto sampleIt = paper.get_child("sampleIds").begin();
  auto arrivalIt = paper.get_child("arrivalMs").begin();
  for (; sampleIt != paper.get_child("sampleIds").end(); ++sampleIt, ++arrivalIt) {
    BOOST_REQUIRE(state.observeAcceptedSample(
      42, sampleIt->second.get_value<uint64_t>(),
      arrivalIt->second.get_value<uint64_t>(), 40.0, 1, true));
  }
  BOOST_CHECK_EQUAL(toString(state.phase()),
                    paper.get<std::string>("expectedPhase"));
  BOOST_CHECK_EQUAL(state.decide(340, 0).detectorProfile,
                    paper.get<std::string>("detectorProfile"));
}

BOOST_AUTO_TEST_CASE(StreamHealthClassifiesGenericStreamState)
{
  StreamInfo info;
  info.streamId = "video";
  info.sessionEpoch = 4;
  info.nextSeq = 10;

  StreamMetrics degradedMetrics;
  degradedMetrics.gaps = 1;
  const auto degraded = StreamHealth::fromStream(info, degradedMetrics, std::nullopt,
                                                 0, 0, false, 3000, 1000);
  BOOST_CHECK_EQUAL(toString(degraded.state), "DEGRADED");
  BOOST_CHECK_EQUAL(degraded.nextSeq, 10);

  StreamAdaptiveFetcherState fetcher;
  fetcher.setBacklogPressure(0.9);
  const auto congestedDecision = fetcher.decide();
  const auto congested = StreamHealth::fromStream(info, StreamMetrics{}, congestedDecision,
                                                  11, 0, false, 3000, 1000);
  BOOST_CHECK_EQUAL(toString(congested.state), "CONGESTED");
  BOOST_CHECK_EQUAL(congested.nextSeq, 11);

  const auto stale = StreamHealth::fromStream(info, StreamMetrics{}, std::nullopt,
                                              0, 1, false, 100, 1000);
  BOOST_CHECK_EQUAL(toString(stale.state), "STALE");
}

BOOST_AUTO_TEST_CASE(StreamNameMapWireContractIsCanonical)
{
  StreamNameMapBlock block;
  block.contractVersion = 1;
  block.streamId = "front-camera";
  block.sessionEpoch = 17;
  block.mappingVersion = 23;
  block.blockNumber = 0;
  block.blockCapacity = 4;
  block.firstCursor = 0;
  block.entries = {
    StreamNameMapEntry::fromName(ndn::Name("/uav/7/video/v=23/frame/0/seg=0")),
    StreamNameMapEntry::fromName(
      ndn::Name("/uav/7/video").appendVersion(23).appendSequenceNumber(1)),
    StreamNameMapEntry::makeTombstone(),
    StreamNameMapEntry::fromName(ndn::Name("/uav/7/video/v=23/frame/1/seg=0")),
  };

  const auto mapRoot = makeStreamNameMapRoot(ndn::Name("/uav/7"), "front-camera");
  const auto dataName = makeStreamNameMapBlockName(mapRoot, 23, 0);
  BOOST_REQUIRE_EQUAL(dataName.size(), mapRoot.size() + 2);
  BOOST_CHECK(dataName[-2].isVersion());
  BOOST_CHECK_EQUAL(dataName[-2].toVersion(), 23);
  BOOST_CHECK(dataName[-1].isSequenceNumber());
  BOOST_CHECK_EQUAL(dataName[-1].toSequenceNumber(), 0);

  const auto wire = block.wireEncode();
  const auto content = block.canonicalContent();
  const auto fixture = readStreamFixture("map-wire-v1.json");
  BOOST_CHECK_EQUAL(mapRoot.toUri(),
                    fixture.get<std::string>("typedName.mappingRootUri"));
  BOOST_CHECK_EQUAL(dataName.toUri(),
                    fixture.get<std::string>("typedName.dataNameUri"));
  BOOST_CHECK_EQUAL(toHex(dataName.wireEncode()),
                    fixture.get<std::string>("typedName.dataNameWireHex"));
  BOOST_CHECK_EQUAL(toHex(wire),
                    fixture.get<std::string>("canonicalExample.blockWireHex"));
  BOOST_CHECK_EQUAL(toHex(content),
                    fixture.get<std::string>("canonicalExample.contentWireHex"));
  BOOST_CHECK_EQUAL(toHex(block.contentDigest()),
                    fixture.get<std::string>("canonicalExample.contentDigestSha256Hex"));
  BOOST_CHECK_EQUAL(content.type(), ndn::tlv::Content);
  BOOST_CHECK_EQUAL(block.contentDigest().size(), 32);
  BOOST_CHECK(block.fitsSignedWireBudget(200, content.size() + 200));
  BOOST_CHECK(!block.fitsSignedWireBudget(200, content.size() + 199));

  StreamNameMapBlock parsed;
  BOOST_REQUIRE(parsed.wireDecode(wire));
  BOOST_CHECK(parsed.wireEncode() == wire);
  BOOST_CHECK(parsed.canonicalContent() == content);
  BOOST_CHECK(parsed.contentDigest() == block.contentDigest());
  BOOST_REQUIRE_EQUAL(parsed.entries.size(), 4);
  BOOST_CHECK(parsed.entries[2].isTombstone());
  BOOST_CHECK(parsed.entries[1].originalName[-2].isVersion());
  BOOST_CHECK(parsed.entries[1].originalName[-1].isSequenceNumber());
}

BOOST_AUTO_TEST_CASE(StreamNameMapV2GroupWireAndPredictorAreCanonical)
{
  StreamNameMapBlock block;
  block.contractVersion = STREAM_NAME_MAP_CONTRACT_VERSION_V2;
  block.streamId = "front-camera";
  block.sessionEpoch = 17;
  block.mappingVersion = 23;
  block.blockNumber = 0;
  block.blockCapacity = 4;
  block.firstCursor = 0;
  block.entries = {
    StreamNameMapEntry::fromGroupedName(
      ndn::Name("/uav/7/video/v=23/frame/42/source/0"), "42", "key", 0, 3, 1),
    StreamNameMapEntry::fromGroupedName(
      ndn::Name("/uav/7/video/v=23/frame/42/source/1"), "42", "key", 1, 3, 1),
    StreamNameMapEntry::fromGroupedName(
      ndn::Name("/uav/7/video/v=23/frame/42/source/2"), "42", "key", 2, 3, 1),
    StreamNameMapEntry::fromGroupedName(
      ndn::Name("/uav/7/video/v=23/frame/42/repair/0"), "42", "key", 3, 3, 1),
  };
  BOOST_REQUIRE(!block.validate());
  const auto fixture = readStreamFixture("map-wire-v2.json");
  BOOST_CHECK_EQUAL(toHex(block.wireEncode()),
                    fixture.get<std::string>("canonicalExample.blockWireHex"));
  BOOST_CHECK_EQUAL(toHex(block.canonicalContent()),
                    fixture.get<std::string>("canonicalExample.contentWireHex"));
  BOOST_CHECK_EQUAL(toHex(block.contentDigest()),
                    fixture.get<std::string>("canonicalExample.contentDigestSha256Hex"));

  StreamNameMapBlock parsed;
  BOOST_REQUIRE(parsed.wireDecode(block.wireEncode()));
  BOOST_CHECK_EQUAL(parsed.contractVersion, STREAM_NAME_MAP_CONTRACT_VERSION_V2);
  BOOST_REQUIRE_EQUAL(parsed.entries.size(), 4);
  BOOST_CHECK(parsed.entries[0].hasGroupBinding());
  BOOST_CHECK_EQUAL(parsed.entries[0].groupId, "42");
  BOOST_CHECK_EQUAL(parsed.entries[0].sampleClass, "key");
  BOOST_CHECK_EQUAL(parsed.entries[3].groupItemIndex, 3);

  auto malformed = block;
  malformed.entries[2].groupItemIndex = 3;
  BOOST_CHECK_EQUAL(*malformed.validate(), "non-contiguous-v2-group-index");
  malformed = block;
  malformed.entries[2].predictedSourceItems = 4;
  BOOST_CHECK_EQUAL(*malformed.validate(), "conflicting-v2-group-binding");

  LiveStreamSamplePredictor predictor({
    SampleClassProfile::bounded("key", 12, 32, 3, 1),
    SampleClassProfile::bounded("delta", 3, 8, 3, 1),
  });
  BOOST_CHECK_EQUAL(predictor.predict("key"), 12);
  BOOST_CHECK_EQUAL(predictor.predict("delta"), 3);
  BOOST_REQUIRE(predictor.observe("key", 20));
  BOOST_CHECK_EQUAL(predictor.predict("key"), 21);
  BOOST_CHECK_EQUAL(predictor.predict("delta"), 3);
  BOOST_REQUIRE(predictor.observe("key", 8));
  BOOST_REQUIRE(predictor.observe("key", 7));
  BOOST_REQUIRE(predictor.observe("key", 6));
  // The seed is a cold-start estimate, not a permanent prediction floor.
  BOOST_CHECK_EQUAL(predictor.predict("key"), 9);
  BOOST_CHECK(!predictor.observe("delta", 9));
  const auto key = predictor.status("key");
  BOOST_REQUIRE(key);
  BOOST_CHECK_EQUAL(key->observations, 3);
  BOOST_CHECK_EQUAL(key->underpredictions, 1);
  BOOST_CHECK_GE(key->overpredictions, 1);

  LiveStreamSamplePredictor stable({
    SampleClassProfile::bounded("delta", 3, 8, 4, 0),
  });
  BOOST_CHECK_EQUAL(stable.predict("delta"), 3);
  BOOST_REQUIRE(stable.observe("delta", 1));
  BOOST_CHECK_EQUAL(stable.predict("delta"), 1);
}

BOOST_AUTO_TEST_CASE(StreamNameResolverMatchesSharedGoldenTrace)
{
  const auto fixture = readStreamFixture("resolver-traces-v1.json");
  const auto payloadPrefix = makePayloadPrefix(23);
  const auto block0 = makeMapBlock(0, payloadPrefix);
  const auto block1 = makeMapBlock(1, payloadPrefix, block0.contentDigest());
  StreamNameResolverState resolver;
  resolver.reset(makeResolverConfig(payloadPrefix), makeCheckpoint(block0));

  const auto checkAdmission = [&] (size_t index,
                                   const StreamNameMapAdmissionResult& result) {
    const auto& expected = fixtureOperation(fixture, index);
    BOOST_CHECK_EQUAL(toString(result.disposition),
                      expected.get<std::string>("expectedDisposition"));
    BOOST_CHECK_EQUAL(result.reason,
                      expected.get<std::string>("expectedReason"));
    BOOST_CHECK_EQUAL(result.stateChanged,
                      expected.get<bool>("stateChanged"));
    if (const auto value = expected.get_optional<bool>("faulted")) {
      BOOST_CHECK_EQUAL(resolver.faulted(), *value);
    }
    if (const auto value = expected.get_optional<size_t>("verifiedBlocks")) {
      BOOST_CHECK_EQUAL(resolver.verifiedBlockCount(), *value);
    }
    if (const auto value = expected.get_optional<size_t>("quarantinedBlocks")) {
      BOOST_CHECK_EQUAL(resolver.quarantinedBlockCount(), *value);
    }
    if (const auto value = expected.get_optional<size_t>("bindingCount")) {
      BOOST_CHECK_EQUAL(resolver.bindingCount(), *value);
    }
    if (const auto resolvable = expected.get_child_optional("resolvable")) {
      const auto expectedCursors = fixtureNumbers(expected, "resolvable");
      std::vector<uint64_t> actual;
      for (uint64_t cursor = 0; cursor < 8; ++cursor) {
        if (resolver.resolve(cursor)) {
          actual.push_back(cursor);
        }
      }
      BOOST_CHECK(actual == expectedCursors);
    }
  };

  checkAdmission(0, resolver.admitVerifiedBlock(makeVerifiedMapData(block1)));
  checkAdmission(1, resolver.admitVerifiedBlock(makeVerifiedMapData(block0)));
  checkAdmission(2, resolver.admitVerifiedBlock(makeVerifiedMapData(block0)));

  const auto& terminal = fixtureOperation(fixture, 3);
  const auto terminalCursor = terminal.get<StreamCursor>("cursor");
  BOOST_CHECK_EQUAL(resolver.markTerminalUnproduced(terminalCursor),
                    terminal.get<bool>("changed"));
  BOOST_REQUIRE(resolver.lookup(terminalCursor));
  BOOST_CHECK_EQUAL(resolver.lookup(terminalCursor)->schedulable(),
                    terminal.get<bool>("schedulable"));
  BOOST_CHECK_EQUAL(static_cast<bool>(resolver.reverseResolve(
                      makePayloadName(payloadPrefix, terminalCursor))),
                    terminal.get<bool>("reverseBindingPreserved"));

  const auto& eviction = fixtureOperation(fixture, 4);
  BOOST_CHECK_EQUAL(resolver.evictLocalBlock(eviction.get<uint64_t>("block")),
                    eviction.get<bool>("changed"));
  BOOST_CHECK_EQUAL(resolver.frontiers().oldestRetained,
                    eviction.get<StreamCursor>("providerOldestRetained"));
  BOOST_CHECK_EQUAL(resolver.verifiedBlockCount(),
                    eviction.get<size_t>("verifiedBlocks"));
  BOOST_CHECK_EQUAL(resolver.quarantinedBlockCount(),
                    eviction.get<size_t>("quarantinedBlocks"));
  BOOST_CHECK_EQUAL(resolver.bindingCount(),
                    eviction.get<size_t>("bindingCount"));

  checkAdmission(5, resolver.admitVerifiedBlock(makeVerifiedMapData(block0)));
  const auto& refetch = fixtureOperation(fixture, 5);
  BOOST_REQUIRE(resolver.lookup(refetch.get<StreamCursor>("terminalCursor")));
  BOOST_CHECK_EQUAL(
    resolver.lookup(refetch.get<StreamCursor>("terminalCursor"))->schedulable(),
    refetch.get<bool>("terminalSchedulable"));

  const auto& refresh = fixtureOperation(fixture, 6);
  const auto refreshed = makeCheckpoint(
    block1, refresh.get<StreamCursor>("oldestRetained"), 4, 5, 7, 8);
  checkAdmission(6, resolver.refreshCheckpoint(refreshed));

  const auto& reset = fixtureOperation(fixture, 7);
  const auto nextVersion = reset.get<uint64_t>("mappingVersion");
  const auto newPrefix = makePayloadPrefix(nextVersion);
  auto newBlock = makeMapBlock(0, newPrefix);
  newBlock.sessionEpoch = reset.get<uint64_t>("sessionEpoch");
  newBlock.mappingVersion = nextVersion;
  auto newConfig = makeResolverConfig(newPrefix);
  newConfig.sessionEpoch = newBlock.sessionEpoch;
  newConfig.mappingVersion = nextVersion;
  resolver.reset(newConfig, makeCheckpoint(newBlock));
  const auto stale = resolver.admitVerifiedBlock(makeVerifiedMapData(block0));
  BOOST_CHECK_EQUAL(toString(stale.disposition),
                    reset.get<std::string>("oldSessionBlockExpectedDisposition"));
  BOOST_CHECK_EQUAL(stale.reason,
                    reset.get<std::string>("oldSessionBlockExpectedReason"));

  for (const auto& item : fixture.get_child("timingCases")) {
    StreamNameResolverState timingResolver;
    timingResolver.reset(makeResolverConfig(payloadPrefix), makeCheckpoint(block0));
    const auto result = timingResolver.admitVerifiedBlock(makeVerifiedMapData(
      block0, ndn::Name("/uav/7"),
      item.second.get<uint64_t>("receivedMonotonicMs"),
      item.second.get<uint64_t>("requiredBeforeMonotonicMs")));
    BOOST_CHECK_EQUAL(toString(result.timing),
                      item.second.get<std::string>("expected"));
  }
}

BOOST_AUTO_TEST_CASE(StreamNameResolverConnectsQuarantineAtomically)
{
  const auto payloadPrefix = makePayloadPrefix(23);
  const auto block0 = makeMapBlock(0, payloadPrefix);
  const auto block1 = makeMapBlock(1, payloadPrefix, block0.contentDigest());
  const auto fixture = readStreamFixture("map-wire-v1.json");
  BOOST_CHECK_EQUAL(toHex(block0.wireEncode()),
                    fixture.get<std::string>("resolverGenesis.blockWireHex"));
  BOOST_CHECK_EQUAL(toHex(block0.canonicalContent()),
                    fixture.get<std::string>("resolverGenesis.contentWireHex"));
  BOOST_CHECK_EQUAL(toHex(block0.contentDigest()),
                    fixture.get<std::string>("resolverGenesis.contentDigestSha256Hex"));
  BOOST_CHECK_EQUAL(toHex(block1.wireEncode()),
                    fixture.get<std::string>("resolverSuccessor.blockWireHex"));
  BOOST_CHECK_EQUAL(toHex(block1.canonicalContent()),
                    fixture.get<std::string>("resolverSuccessor.contentWireHex"));
  BOOST_CHECK_EQUAL(toHex(block1.contentDigest()),
                    fixture.get<std::string>("resolverSuccessor.contentDigestSha256Hex"));

  StreamNameResolverState resolver;
  resolver.reset(makeResolverConfig(payloadPrefix), makeCheckpoint(block0));

  const auto future = resolver.admitVerifiedBlock(makeVerifiedMapData(block1));
  BOOST_CHECK(future.disposition == StreamNameMapAdmissionDisposition::Quarantined);
  BOOST_CHECK(!resolver.resolve(4));
  BOOST_CHECK_EQUAL(resolver.quarantinedBlockCount(), 1);

  const auto anchor = resolver.admitVerifiedBlock(makeVerifiedMapData(block0));
  BOOST_CHECK(anchor.disposition == StreamNameMapAdmissionDisposition::Admitted);
  BOOST_REQUIRE(resolver.resolve(4));
  BOOST_CHECK(*resolver.resolve(4) == makePayloadName(payloadPrefix, 4));
  BOOST_REQUIRE(resolver.reverseResolve(makePayloadName(payloadPrefix, 4)));
  BOOST_CHECK_EQUAL(*resolver.reverseResolve(makePayloadName(payloadPrefix, 4)), 4);
  BOOST_CHECK_EQUAL(resolver.verifiedBlockCount(), 2);
  BOOST_CHECK_EQUAL(resolver.quarantinedBlockCount(), 0);

  const auto duplicate = resolver.admitVerifiedBlock(makeVerifiedMapData(block0));
  BOOST_CHECK(duplicate.disposition == StreamNameMapAdmissionDisposition::Duplicate);
  BOOST_CHECK(!duplicate.stateChanged);

  BOOST_CHECK(!resolver.resolve(2)); // predeclared tombstone
  BOOST_CHECK(resolver.markTerminalUnproduced(1));
  BOOST_CHECK(!resolver.resolve(1));
  BOOST_REQUIRE(resolver.reverseResolve(makePayloadName(payloadPrefix, 1)));
  const auto terminal = resolver.lookup(1);
  BOOST_REQUIRE(terminal);
  BOOST_CHECK(terminal->terminalUnproduced);
  BOOST_CHECK(!terminal->schedulable());
  BOOST_CHECK(terminal->timing == StreamNameMapTiming::Ahead);
}

BOOST_AUTO_TEST_CASE(StreamNameResolverAdvancesSignedLiveMappingFrontier)
{
  const auto payloadPrefix = makePayloadPrefix(23);
  const auto block0 = makeMapBlock(0, payloadPrefix);
  const auto block1 = makeMapBlock(1, payloadPrefix, block0.contentDigest());
  StreamNameResolverState resolver;
  resolver.reset(makeResolverConfig(payloadPrefix),
                 makeCheckpoint(block0, 0, 1, 1, 3, 4));

  BOOST_CHECK(resolver.admitVerifiedBlock(makeVerifiedMapData(block0)).accepted());
  const auto next = resolver.admitVerifiedBlock(makeVerifiedMapData(block1));
  BOOST_CHECK(next.disposition == StreamNameMapAdmissionDisposition::Admitted);
  BOOST_CHECK_EQUAL(resolver.frontiers().mappingCommittedThrough, 7);
  BOOST_CHECK_EQUAL(resolver.frontiers().nextReserved, 8);
  BOOST_REQUIRE(resolver.resolve(7));
  BOOST_CHECK(*resolver.resolve(7) == makePayloadName(payloadPrefix, 7));
}

BOOST_AUTO_TEST_CASE(StreamNameResolverSupportsBackwardCheckpointChainAndLocalEviction)
{
  const auto payloadPrefix = makePayloadPrefix(23);
  const auto block0 = makeMapBlock(0, payloadPrefix);
  const auto block1 = makeMapBlock(1, payloadPrefix, block0.contentDigest());

  StreamNameResolverState resolver;
  resolver.reset(makeResolverConfig(payloadPrefix),
                 makeCheckpoint(block1, 0, 4, 5, 7, 8));

  BOOST_CHECK(resolver.admitVerifiedBlock(makeVerifiedMapData(block0)).disposition ==
              StreamNameMapAdmissionDisposition::Quarantined);
  BOOST_CHECK(!resolver.resolve(0));
  BOOST_CHECK(resolver.admitVerifiedBlock(makeVerifiedMapData(block1)).disposition ==
              StreamNameMapAdmissionDisposition::Admitted);
  BOOST_REQUIRE(resolver.resolve(0));
  BOOST_CHECK(resolver.markTerminalUnproduced(1));

  const auto before = resolver.frontiers();
  BOOST_CHECK(resolver.evictLocalBlock(0));
  BOOST_CHECK(!resolver.resolve(0));
  BOOST_CHECK_EQUAL(resolver.frontiers().oldestRetained, before.oldestRetained);
  BOOST_CHECK(resolver.admitVerifiedBlock(makeVerifiedMapData(block0)).accepted());
  BOOST_REQUIRE(resolver.resolve(0));
  BOOST_CHECK(!resolver.resolve(1));
  BOOST_REQUIRE(resolver.reverseResolve(makePayloadName(payloadPrefix, 1)));

  const auto refreshed = makeCheckpoint(block1, 4, 4, 5, 7, 8);
  const auto refreshResult = resolver.refreshCheckpoint(refreshed);
  BOOST_CHECK(refreshResult.accepted());
  BOOST_CHECK_EQUAL(resolver.frontiers().oldestRetained, 4);
  BOOST_CHECK(!resolver.lookup(0));
  BOOST_REQUIRE(resolver.resolve(4));
}

BOOST_AUTO_TEST_CASE(StreamNameResolverRejectsBadEnvelopeAndClosesOnFork)
{
  const auto payloadPrefix = makePayloadPrefix(23);
  const auto block0 = makeMapBlock(0, payloadPrefix);
  const auto block1 = makeMapBlock(1, payloadPrefix, block0.contentDigest());
  StreamNameResolverState resolver;
  resolver.reset(makeResolverConfig(payloadPrefix), makeCheckpoint(block0));

  auto wrongName = makeVerifiedMapData(block0);
  wrongName.dataName.append("wrong");
  const auto rejected = resolver.admitVerifiedBlock(wrongName);
  BOOST_CHECK(rejected.disposition == StreamNameMapAdmissionDisposition::Rejected);
  BOOST_CHECK(!resolver.lookup(0));
  BOOST_CHECK(!resolver.faulted());

  BOOST_CHECK(resolver.admitVerifiedBlock(makeVerifiedMapData(block0)).accepted());
  auto fork = block1;
  fork.previousContentDigest = StreamContentDigest{};
  const auto forked = resolver.admitVerifiedBlock(makeVerifiedMapData(fork));
  BOOST_CHECK(forked.disposition == StreamNameMapAdmissionDisposition::FatalSession);
  BOOST_CHECK(resolver.faulted());
  BOOST_CHECK(!resolver.resolve(0));

  StreamNameResolverState secondResolver;
  secondResolver.reset(makeResolverConfig(payloadPrefix), makeCheckpoint(block0));
  const auto secondProvider = secondResolver.admitVerifiedBlock(
    makeVerifiedMapData(block0, ndn::Name("/uav/8")));
  BOOST_CHECK(secondProvider.disposition ==
              StreamNameMapAdmissionDisposition::FatalSession);
  BOOST_CHECK_EQUAL(secondProvider.reason, "wrong-provider");
}

BOOST_AUTO_TEST_CASE(StreamNameResolverValidatesFiveFrontiersAndStaleSession)
{
  const auto payloadPrefix = makePayloadPrefix(23);
  const auto block0 = makeMapBlock(0, payloadPrefix);
  const auto valid = makeCheckpoint(block0);
  const auto fixture = readStreamFixture("frontier-retention-v1.json");
  BOOST_CHECK_EQUAL(valid.frontiers.oldestRetained,
                    fixture.get<StreamCursor>("valid.oldestRetained"));
  BOOST_CHECK_EQUAL(valid.frontiers.latestJoin,
                    fixture.get<StreamCursor>("valid.latestJoin"));
  BOOST_CHECK_EQUAL(valid.frontiers.latestProduced,
                    fixture.get<StreamCursor>("valid.latestProduced"));
  BOOST_CHECK_EQUAL(valid.frontiers.mappingCommittedThrough,
                    fixture.get<StreamCursor>("valid.mappingCommittedThrough"));
  BOOST_CHECK_EQUAL(valid.frontiers.nextReserved,
                    fixture.get<StreamCursor>("valid.nextReserved"));
  BOOST_CHECK(!valid.frontiers.validate(
    fixture.get<uint64_t>("valid.blockCapacity"),
    fixture.get<uint64_t>("valid.checkpointBlock")));
  std::vector<StreamNameMapCheckpoint> invalid;
  invalid.push_back(valid);
  invalid.back().frontiers.oldestRetained = 2;
  invalid.push_back(valid);
  invalid.back().frontiers.latestJoin = 3;
  invalid.back().frontiers.latestProduced = 2;
  invalid.push_back(valid);
  invalid.back().frontiers.latestProduced = 8;
  invalid.push_back(valid);
  invalid.back().frontiers.nextReserved = 7;
  invalid.push_back(valid);
  invalid.back().blockNumber = 1;
  invalid.push_back(valid);
  invalid.back().frontiers.mappingCommittedThrough = 6;

  for (const auto& checkpoint : invalid) {
    StreamNameResolverState invalidResolver;
    BOOST_CHECK_THROW(
      invalidResolver.reset(makeResolverConfig(payloadPrefix), checkpoint),
      std::invalid_argument);
  }

  StreamNameResolverState resolver;
  resolver.reset(makeResolverConfig(payloadPrefix), makeCheckpoint(block0));
  auto stale = block0;
  stale.sessionEpoch = 16;
  const auto result = resolver.admitVerifiedBlock(makeVerifiedMapData(stale));
  BOOST_CHECK(result.disposition == StreamNameMapAdmissionDisposition::FatalSession);
  BOOST_CHECK_EQUAL(result.reason, "stale-session");
}

BOOST_AUTO_TEST_CASE(StreamNameMapRejectsMalformedRangesAndNoncanonicalContent)
{
  const auto payloadPrefix = makePayloadPrefix(23);
  const auto block0 = makeMapBlock(0, payloadPrefix);

  auto wrongFirst = block0;
  wrongFirst.firstCursor = 1;
  BOOST_REQUIRE(wrongFirst.validate());
  BOOST_CHECK_EQUAL(*wrongFirst.validate(), "invalid-first-cursor");
  BOOST_CHECK_THROW(wrongFirst.wireEncode(), std::invalid_argument);

  auto genesisWithPrevious = block0;
  genesisWithPrevious.previousContentDigest = StreamContentDigest{};
  BOOST_REQUIRE(genesisWithPrevious.validate());
  BOOST_CHECK_EQUAL(*genesisWithPrevious.validate(),
                    "invalid-previous-content-digest");

  auto missingPrevious = makeMapBlock(1, payloadPrefix);
  BOOST_REQUIRE(missingPrevious.validate());
  BOOST_CHECK_EQUAL(*missingPrevious.validate(),
                    "invalid-previous-content-digest");

  auto overflow = block0;
  overflow.blockNumber = std::numeric_limits<uint64_t>::max();
  overflow.blockCapacity = 2;
  overflow.firstCursor = 0;
  overflow.entries.resize(2);
  BOOST_REQUIRE(overflow.validate());
  BOOST_CHECK_EQUAL(*overflow.validate(), "cursor-range-overflow");

  StreamNameMapBlock parsed;
  BOOST_CHECK(!parsed.wireDecode(ndn::makeEmptyBlock(
    stream_tlv::StreamNameMapBlockType)));
  BOOST_CHECK(!parsed.wireDecode(ndn::makeEmptyBlock(ndn::tlv::Content)));

  StreamNameResolverState resolver;
  resolver.reset(makeResolverConfig(payloadPrefix), makeCheckpoint(block0));
  auto malformed = makeVerifiedMapData(block0);
  ndn::Block content(ndn::tlv::Content);
  content.push_back(block0.wireEncode());
  content.push_back(block0.wireEncode());
  content.encode();
  malformed.content = content;
  malformed.signedWireSize = content.size() + 200;
  const auto result = resolver.admitVerifiedBlock(malformed);
  BOOST_CHECK_EQUAL(result.reason, "malformed-or-noncanonical-content");
  BOOST_CHECK(!result.fatal());
  BOOST_CHECK_EQUAL(resolver.bindingCount(), 0);
}

BOOST_AUTO_TEST_CASE(StreamNameResolverEnforcesManifestFinalBlockAndWireCap)
{
  const auto payloadPrefix = makePayloadPrefix(23);
  const auto block0 = makeMapBlock(0, payloadPrefix);
  const auto fixture = readStreamFixture("map-rejections-v1.json");

  StreamNameResolverState resolver;
  resolver.reset(makeResolverConfig(payloadPrefix), makeCheckpoint(block0));

  auto wrongType = makeVerifiedMapData(block0);
  wrongType.contentType = ndn::tlv::ContentType_Blob;
  BOOST_CHECK_EQUAL(resolver.admitVerifiedBlock(wrongType).reason,
                    fixture.get<std::string>("reasonByCase.wrong-content-type"));

  auto finalBlock = makeVerifiedMapData(block0);
  finalBlock.hasFinalBlock = true;
  BOOST_CHECK_EQUAL(resolver.admitVerifiedBlock(finalBlock).reason,
                    fixture.get<std::string>("reasonByCase.final-block"));

  auto oversized = makeVerifiedMapData(block0);
  oversized.signedWireSize = ndn::MAX_NDN_PACKET_SIZE + 1;
  BOOST_CHECK_EQUAL(resolver.admitVerifiedBlock(oversized).reason,
                    fixture.get<std::string>("reasonByCase.wire-cap"));
  BOOST_CHECK_EQUAL(resolver.bindingCount(), 0);
  BOOST_CHECK(!resolver.faulted());
}

BOOST_AUTO_TEST_CASE(StreamNameResolverMatchesSharedRejectionVectors)
{
  const auto fixture = readStreamFixture("map-rejections-v1.json");
  const auto payloadPrefix = makePayloadPrefix(23);
  const auto block0 = makeMapBlock(0, payloadPrefix);
  const auto block1 = makeMapBlock(1, payloadPrefix, block0.contentDigest());
  std::map<std::string, std::pair<std::string, bool>> observed;

  const auto fresh = [&] {
    auto resolver = std::make_unique<StreamNameResolverState>();
    resolver->reset(makeResolverConfig(payloadPrefix), makeCheckpoint(block0));
    return resolver;
  };
  const auto remember = [&] (const std::string& key,
                             const StreamNameMapAdmissionResult& result) {
    observed[key] = {result.reason, result.fatal()};
  };

  auto resolver = fresh();
  auto wrongName = makeVerifiedMapData(block0);
  wrongName.dataName.append("wrong");
  remember("wrong-control-name", resolver->admitVerifiedBlock(wrongName));

  resolver = fresh();
  remember("wrong-provider", resolver->admitVerifiedBlock(
    makeVerifiedMapData(block0, ndn::Name("/uav/8"))));

  resolver = fresh();
  auto staleSession = block0;
  staleSession.sessionEpoch = 16;
  remember("stale-session", resolver->admitVerifiedBlock(
    makeVerifiedMapData(staleSession)));

  resolver = fresh();
  auto staleVersion = block0;
  staleVersion.mappingVersion = 22;
  remember("stale-mapping-version", resolver->admitVerifiedBlock(
    makeVerifiedMapData(staleVersion)));

  resolver = fresh();
  BOOST_REQUIRE(resolver->admitVerifiedBlock(makeVerifiedMapData(block0)).accepted());
  auto equivocation = block0;
  equivocation.entries[3] = StreamNameMapEntry::fromName(
    makePayloadName(payloadPrefix, 99));
  remember("same-name-different-content", resolver->admitVerifiedBlock(
    makeVerifiedMapData(equivocation)));

  resolver = fresh();
  BOOST_REQUIRE(resolver->admitVerifiedBlock(makeVerifiedMapData(block0)).accepted());
  auto fork = block1;
  fork.previousContentDigest = StreamContentDigest{};
  remember("continuity-fork", resolver->admitVerifiedBlock(
    makeVerifiedMapData(fork)));

  resolver = fresh();
  BOOST_REQUIRE(resolver->admitVerifiedBlock(makeVerifiedMapData(block0)).accepted());
  auto reuse = block1;
  reuse.entries[0] = block0.entries[0];
  remember("original-name-reuse", resolver->admitVerifiedBlock(
    makeVerifiedMapData(reuse)));

  resolver = fresh();
  auto wrongType = makeVerifiedMapData(block0);
  wrongType.contentType = ndn::tlv::ContentType_Blob;
  remember("wrong-content-type", resolver->admitVerifiedBlock(wrongType));

  resolver = fresh();
  auto finalBlock = makeVerifiedMapData(block0);
  finalBlock.hasFinalBlock = true;
  remember("final-block", resolver->admitVerifiedBlock(finalBlock));

  resolver = fresh();
  auto oversized = makeVerifiedMapData(block0);
  oversized.signedWireSize = ndn::MAX_NDN_PACKET_SIZE + 1;
  remember("wire-cap", resolver->admitVerifiedBlock(oversized));

  resolver = fresh();
  auto malformed = makeVerifiedMapData(block0);
  ndn::Block malformedContent(ndn::tlv::Content);
  malformedContent.push_back(block0.wireEncode());
  malformedContent.push_back(block0.wireEncode());
  malformedContent.encode();
  malformed.content = malformedContent;
  malformed.signedWireSize = malformedContent.size() + 200;
  remember("noncanonical", resolver->admitVerifiedBlock(malformed));

  resolver = fresh();
  auto outsidePrefix = block0;
  outsidePrefix.entries[0] = StreamNameMapEntry::fromName(
    ndn::Name("/attacker/not-authorized").appendSequenceNumber(0));
  remember("outside-prefix", resolver->admitVerifiedBlock(
    makeVerifiedMapData(outsidePrefix)));

  auto shortNameConfig = makeResolverConfig(payloadPrefix);
  shortNameConfig.maxOriginalNameWireBytes = 16;
  resolver = std::make_unique<StreamNameResolverState>();
  resolver->reset(shortNameConfig, makeCheckpoint(block0));
  remember("name-too-large", resolver->admitVerifiedBlock(
    makeVerifiedMapData(block0)));

  resolver = std::make_unique<StreamNameResolverState>();
  resolver->reset(makeResolverConfig(payloadPrefix),
                  makeCheckpoint(block1, 4, 4, 5, 7, 8));
  remember("stale-block", resolver->admitVerifiedBlock(
    makeVerifiedMapData(block0)));

  resolver = fresh();
  const auto block2 = makeMapBlock(2, payloadPrefix, block1.contentDigest());
  const auto aheadGap = resolver->admitVerifiedBlock(makeVerifiedMapData(block2));
  BOOST_CHECK(aheadGap.disposition == StreamNameMapAdmissionDisposition::Quarantined);
  BOOST_CHECK_EQUAL(aheadGap.reason, "awaiting-continuity");

  resolver = fresh();
  auto regressed = makeCheckpoint(block0);
  regressed.frontiers.latestProduced = 1;
  remember("frontier-regression", resolver->refreshCheckpoint(regressed));

  resolver = fresh();
  remember("checkpoint-anchor-not-verified", resolver->refreshCheckpoint(
    makeCheckpoint(block1, 0, 4, 5, 7, 8)));

  resolver = fresh();
  auto conflictingCheckpoint = makeCheckpoint(block0);
  conflictingCheckpoint.contentDigest = StreamContentDigest{};
  remember("checkpoint-equivocation", resolver->refreshCheckpoint(
    conflictingCheckpoint));

  for (const auto& item : fixture.get_child("rejections")) {
    const auto key = item.second.get<std::string>("case");
    BOOST_REQUIRE(observed.count(key) != 0);
    BOOST_CHECK_EQUAL(observed.at(key).first,
                      item.second.get<std::string>("reason"));
    BOOST_CHECK_EQUAL(observed.at(key).second,
                      item.second.get<bool>("fatal"));
  }
}

BOOST_AUTO_TEST_CASE(StreamNameResolverDetectsEquivocationAndNameReuse)
{
  const auto payloadPrefix = makePayloadPrefix(23);
  const auto block0 = makeMapBlock(0, payloadPrefix);
  const auto block1 = makeMapBlock(1, payloadPrefix, block0.contentDigest());

  StreamNameResolverState resolver;
  resolver.reset(makeResolverConfig(payloadPrefix), makeCheckpoint(block0));
  BOOST_REQUIRE(resolver.admitVerifiedBlock(makeVerifiedMapData(block0)).accepted());

  auto equivocation = block0;
  equivocation.entries[3] = StreamNameMapEntry::fromName(
    makePayloadName(payloadPrefix, 99));
  const auto changed = resolver.admitVerifiedBlock(makeVerifiedMapData(equivocation));
  BOOST_CHECK(changed.fatal());
  BOOST_CHECK_EQUAL(changed.reason, "same-name-different-content");

  StreamNameResolverState reuseResolver;
  reuseResolver.reset(makeResolverConfig(payloadPrefix), makeCheckpoint(block0));
  BOOST_REQUIRE(reuseResolver.admitVerifiedBlock(makeVerifiedMapData(block0)).accepted());
  auto reused = block1;
  reused.entries[0] = block0.entries[0];
  const auto reusedResult = reuseResolver.admitVerifiedBlock(makeVerifiedMapData(reused));
  BOOST_CHECK(reusedResult.fatal());
  BOOST_CHECK_EQUAL(reusedResult.reason, "original-name-reuse");
  BOOST_CHECK(!reuseResolver.resolve(0));

  // Local content eviction must not erase the immutable name reservation.
  StreamNameResolverState evictionResolver;
  evictionResolver.reset(makeResolverConfig(payloadPrefix),
                         makeCheckpoint(block1, 0, 4, 5, 11, 12));
  BOOST_REQUIRE(evictionResolver.admitVerifiedBlock(makeVerifiedMapData(block1)).accepted());
  BOOST_REQUIRE(evictionResolver.admitVerifiedBlock(makeVerifiedMapData(block0)).accepted());
  BOOST_REQUIRE(evictionResolver.evictLocalBlock(0));
  auto block2 = makeMapBlock(2, payloadPrefix, block1.contentDigest());
  block2.entries[0] = block0.entries[0];
  const auto reuseAfterEviction = evictionResolver.admitVerifiedBlock(
    makeVerifiedMapData(block2));
  BOOST_CHECK(reuseAfterEviction.fatal());
  BOOST_CHECK_EQUAL(reuseAfterEviction.reason, "original-name-reuse");
}

BOOST_AUTO_TEST_CASE(StreamNameResolverKeepsBoundsAndClassifiesLocalTiming)
{
  const auto payloadPrefix = makePayloadPrefix(23);
  const auto block0 = makeMapBlock(0, payloadPrefix);
  const auto block1 = makeMapBlock(1, payloadPrefix, block0.contentDigest());
  const auto block2 = makeMapBlock(2, payloadPrefix, block1.contentDigest());
  auto config = makeResolverConfig(payloadPrefix);
  config.maxQuarantineBlocks = 1;

  StreamNameResolverState resolver;
  resolver.reset(config, makeCheckpoint(block0, 0, 1, 2, 11, 12));
  auto equalDeadline = makeVerifiedMapData(block2, ndn::Name("/uav/7"), 200, 200);
  const auto first = resolver.admitVerifiedBlock(equalDeadline);
  BOOST_CHECK(first.disposition == StreamNameMapAdmissionDisposition::Quarantined);
  BOOST_CHECK(first.timing == StreamNameMapTiming::Late);

  const auto bounded = resolver.admitVerifiedBlock(makeVerifiedMapData(block1));
  BOOST_CHECK_EQUAL(bounded.reason, "quarantine-cache-full");
  BOOST_CHECK_EQUAL(resolver.quarantinedBlockCount(), 1);
  BOOST_CHECK_EQUAL(resolver.bindingCount(), 0);

  config = makeResolverConfig(payloadPrefix);
  config.maxReverseEntries = 7; // two retained blocks require eight worst-case slots
  StreamNameResolverState impossibleResolver;
  BOOST_CHECK_THROW(impossibleResolver.reset(config, makeCheckpoint(block0)),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(StreamNameResolverCommitsSequentialSuccessorsIncrementally)
{
  const auto payloadPrefix = makePayloadPrefix(23);
  auto previous = makeMapBlock(0, payloadPrefix);
  auto config = makeResolverConfig(payloadPrefix);
  config.maxVerifiedBlocks = 2;

  StreamNameResolverState resolver;
  resolver.reset(config, makeCheckpoint(previous, 0, 1, 2, 3, 4));
  for (uint64_t blockNumber = 1; blockNumber <= 20; ++blockNumber) {
    auto block = makeMapBlock(
      blockNumber, payloadPrefix, previous.contentDigest());
    const auto admitted = resolver.admitVerifiedBlock(
      makeVerifiedMapData(block));
    BOOST_REQUIRE_MESSAGE(
      admitted.disposition == StreamNameMapAdmissionDisposition::Admitted,
      admitted.reason);
    BOOST_CHECK_EQUAL(admitted.mappingCommittedThrough, block.lastCursor());
    BOOST_CHECK_LE(resolver.verifiedBlockCount(), 2);
    BOOST_CHECK_LE(resolver.bindingCount(), 8);
    previous = std::move(block);
  }

  const auto diagnostics = resolver.diagnostics();
  BOOST_REQUIRE(diagnostics.count("incremental-admitted") != 0);
  BOOST_CHECK_EQUAL(diagnostics.at("incremental-admitted"), 20);
  BOOST_CHECK(!resolver.resolve(4));
  BOOST_CHECK(resolver.resolve(80) == makePayloadName(payloadPrefix, 80));

  auto reused = makeMapBlock(
    21, payloadPrefix, previous.contentDigest());
  reused.entries[0] = StreamNameMapEntry::fromName(
    makePayloadName(payloadPrefix, 80));
  const auto rejected = resolver.admitVerifiedBlock(makeVerifiedMapData(reused));
  BOOST_CHECK(rejected.fatal());
  BOOST_CHECK_EQUAL(rejected.reason, "original-name-reuse");
}

BOOST_AUTO_TEST_CASE(StreamNameResolverResetRejectsOldVersionedCache)
{
  const auto oldPrefix = makePayloadPrefix(23);
  const auto oldBlock = makeMapBlock(0, oldPrefix);
  StreamNameResolverState resolver;
  resolver.reset(makeResolverConfig(oldPrefix), makeCheckpoint(oldBlock));

  const auto newPrefix = makePayloadPrefix(24);
  auto newBlock = makeMapBlock(0, newPrefix);
  newBlock.sessionEpoch = 18;
  newBlock.mappingVersion = 24;
  auto newConfig = makeResolverConfig(newPrefix);
  newConfig.sessionEpoch = 18;
  newConfig.mappingVersion = 24;
  resolver.reset(newConfig, makeCheckpoint(newBlock));

  const auto stale = resolver.admitVerifiedBlock(makeVerifiedMapData(oldBlock));
  BOOST_CHECK(stale.fatal());
  BOOST_CHECK_EQUAL(stale.reason, "stale-session");
  BOOST_CHECK_EQUAL(resolver.bindingCount(), 0);
}

BOOST_AUTO_TEST_CASE(StreamNameResolverRejectsNamespaceCheckpointAndRetentionCollisions)
{
  const auto payloadPrefix = makePayloadPrefix(23);
  const auto block0 = makeMapBlock(0, payloadPrefix);
  const auto block1 = makeMapBlock(1, payloadPrefix, block0.contentDigest());
  const auto block2 = makeMapBlock(2, payloadPrefix, block1.contentDigest());

  StreamNameResolverState resolver;
  resolver.reset(makeResolverConfig(payloadPrefix), makeCheckpoint(block0));

  BOOST_CHECK_EXCEPTION(
    resolver.reset(makeResolverConfig(payloadPrefix), makeCheckpoint(block0)),
    std::invalid_argument,
    [] (const std::invalid_argument& error) {
      return std::string(error.what()).find("reused-session-epoch") !=
             std::string::npos;
    });

  auto reusedSession = makeResolverConfig(payloadPrefix);
  reusedSession.sessionEpoch = 18;
  BOOST_CHECK_EXCEPTION(
    resolver.reset(reusedSession, makeCheckpoint(block0)),
    std::invalid_argument,
    [] (const std::invalid_argument& error) {
      return std::string(error.what()).find("reused-session-namespace") !=
             std::string::npos;
    });

  auto mismatchedPrefix = makeResolverConfig(makePayloadPrefix(22));
  BOOST_CHECK_THROW(
    StreamNameResolverState{}.reset(mismatchedPrefix, makeCheckpoint(block0)),
    std::invalid_argument);

  auto tooFewBlocks = makeResolverConfig(payloadPrefix);
  tooFewBlocks.maxVerifiedBlocks = 1;
  BOOST_CHECK_THROW(
    StreamNameResolverState{}.reset(tooFewBlocks, makeCheckpoint(block0)),
    std::invalid_argument);

  auto tooFewNames = makeResolverConfig(payloadPrefix);
  tooFewNames.maxReverseEntries = 7;
  BOOST_CHECK_THROW(
    StreamNameResolverState{}.reset(tooFewNames, makeCheckpoint(block0)),
    std::invalid_argument);

  auto conflictingCheckpoint = makeCheckpoint(block0);
  conflictingCheckpoint.contentDigest = StreamContentDigest{};
  const auto conflict = resolver.refreshCheckpoint(conflictingCheckpoint);
  BOOST_CHECK(conflict.fatal());
  BOOST_CHECK_EQUAL(conflict.reason, "checkpoint-equivocation");

  StreamNameResolverState checkpointResolver;
  checkpointResolver.reset(makeResolverConfig(payloadPrefix), makeCheckpoint(block0));
  const auto advancedCheckpoint = makeCheckpoint(block1, 0, 4, 5, 7, 8);
  const auto unverified = checkpointResolver.refreshCheckpoint(advancedCheckpoint);
  BOOST_CHECK(!unverified.accepted());
  BOOST_CHECK(!unverified.fatal());
  BOOST_CHECK_EQUAL(unverified.reason, "checkpoint-anchor-not-verified");
  BOOST_REQUIRE(checkpointResolver.admitVerifiedBlock(makeVerifiedMapData(block0)).accepted());
  BOOST_REQUIRE(checkpointResolver.admitVerifiedBlock(makeVerifiedMapData(block1)).accepted());
  BOOST_REQUIRE(checkpointResolver.refreshCheckpoint(advancedCheckpoint).accepted());

  const auto extendedCheckpoint = makeCheckpoint(block1, 4, 4, 5, 11, 12);
  BOOST_REQUIRE(checkpointResolver.refreshCheckpoint(extendedCheckpoint).accepted());
  auto reusedAfterRetention = block2;
  reusedAfterRetention.entries[0] = block0.entries[0];
  const auto reuse = checkpointResolver.admitVerifiedBlock(
    makeVerifiedMapData(reusedAfterRetention));
  BOOST_CHECK(reuse.fatal());
  BOOST_CHECK_EQUAL(reuse.reason, "original-name-reuse");

  auto boundedConfig = makeResolverConfig(payloadPrefix);
  boundedConfig.maxQuarantineBlocks = 1;
  StreamNameResolverState boundedResolver;
  boundedResolver.reset(boundedConfig,
                        makeCheckpoint(block0, 0, 1, 2, 11, 12));
  BOOST_REQUIRE(boundedResolver.admitVerifiedBlock(makeVerifiedMapData(block0)).accepted());
  BOOST_REQUIRE(boundedResolver.admitVerifiedBlock(makeVerifiedMapData(block1)).accepted());
  BOOST_REQUIRE(boundedResolver.admitVerifiedBlock(makeVerifiedMapData(block2)).accepted());
  BOOST_CHECK(!boundedResolver.evictLocalBlock(0));
  BOOST_CHECK_EQUAL(boundedResolver.verifiedBlockCount(), 3);
  BOOST_REQUIRE(boundedResolver.resolve(0));
}

BOOST_AUTO_TEST_CASE(StreamNameMapRejectsNonMinimalOuterWire)
{
  const auto canonical = makeMapBlock(0, makePayloadPrefix(23)).wireEncode();
  BOOST_REQUIRE_GE(canonical.size(), 5);
  BOOST_REQUIRE_EQUAL(canonical.begin()[0], 0xfd);
  BOOST_REQUIRE_LT(canonical.begin()[3], 0xfd);

  std::vector<uint8_t> nonMinimal;
  nonMinimal.insert(nonMinimal.end(), canonical.begin(), canonical.begin() + 3);
  nonMinimal.push_back(0xfd);
  nonMinimal.push_back(0x00);
  nonMinimal.push_back(canonical.begin()[3]);
  nonMinimal.insert(nonMinimal.end(), canonical.begin() + 4, canonical.end());

  bool rejected = false;
  try {
    const ndn::Block encoded(ndn::span<const uint8_t>(nonMinimal.data(),
                                                       nonMinimal.size()));
    StreamNameMapBlock decoded;
    rejected = !decoded.wireDecode(encoded);
  }
  catch (const std::exception&) {
    rejected = true;
  }
  BOOST_CHECK(rejected);
}

BOOST_AUTO_TEST_CASE(StreamNameResolverConcurrentAdmissionAndLookup)
{
  const auto payloadPrefix = makePayloadPrefix(23);
  const auto block0 = makeMapBlock(0, payloadPrefix);
  const auto block1 = makeMapBlock(1, payloadPrefix, block0.contentDigest());
  const auto verified0 = makeVerifiedMapData(block0);
  const auto verified1 = makeVerifiedMapData(block1);
  StreamNameResolverState resolver;
  resolver.reset(makeResolverConfig(payloadPrefix), makeCheckpoint(block0));

  std::atomic_bool failed{false};
  std::vector<std::thread> workers;
  for (size_t worker = 0; worker < 8; ++worker) {
    workers.emplace_back([&, worker] {
      for (size_t iteration = 0; iteration < 100; ++iteration) {
        // ndn::Block and ndn::Name populate wire/parse caches lazily. Give each
        // worker a local envelope so the test stresses the resolver mutex, not
        // shared fixture cache mutation.
        auto input = (worker + iteration) % 2 == 0 ? verified0 : verified1;
        const auto result = resolver.admitVerifiedBlock(input);
        if (!result.accepted() || result.fatal()) {
          failed = true;
        }
        static_cast<void>(resolver.lookup(iteration % 8));
        static_cast<void>(resolver.frontiers());
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  if (failed.load() || resolver.faulted()) {
    for (const auto& [reason, count] : resolver.diagnostics()) {
      BOOST_TEST_MESSAGE("resolver diagnostic " << reason << "=" << count);
    }
  }
  BOOST_CHECK(!failed.load());
  BOOST_CHECK(!resolver.faulted());
  BOOST_CHECK_EQUAL(resolver.verifiedBlockCount(), 2);
  BOOST_REQUIRE(resolver.resolve(7));
}

BOOST_AUTO_TEST_CASE(LiveStreamXorRepairRecoversOpaqueBytesAndRejectsTampering)
{
  LiveStreamDefinition definition;
  definition.streamId = "camera-front";
  definition.provider = ndn::Name("/memphis/uav/7");
  definition.semanticDataPrefix = ndn::Name("/memphis/uav/7/video/front/session-9");
  definition.sessionEpoch = 9;
  definition.mappingVersion = 23;
  definition.mappingBlockCapacity = 4;
  definition.fec = LiveStreamFecOptions::xorOneRepair(3, 8192);

  const std::vector<LiveStreamItemReservation> sources{
    {0, ndn::Name("/memphis/uav/7/video/front/session-9/frame/0/seg=0"), 9, 23},
    {1, ndn::Name("/memphis/uav/7/video/front/session-9/frame/0/seg=1"), 9, 23},
    {2, ndn::Name("/memphis/uav/7/video/front/session-9/frame/0/seg=2"), 9, 23},
  };
  const LiveStreamItemReservation repair{
    3, ndn::Name("/memphis/uav/7/video/front/session-9/frame/0/repair/0"), 9, 23};
  const std::vector<std::vector<uint8_t>> opaque{
    {0x01, 0x02, 0x00, 0x04},
    {0x05, 0x00},
    {0xff, 0x10, 0x20},
  };

  const auto generated = makeLiveStreamXorRepair(
    definition, "group-0", sources, repair, opaque, 1000, 1500);
  BOOST_REQUIRE(generated.validate(definition).empty());
  LiveStreamFecRepair decoded;
  BOOST_REQUIRE(decoded.wireDecode(generated.wireEncode()));
  BOOST_CHECK_EQUAL(decoded.groupId, generated.groupId);
  BOOST_CHECK_EQUAL_COLLECTIONS(decoded.codedBytes.begin(), decoded.codedBytes.end(),
                                generated.codedBytes.begin(), generated.codedBytes.end());
  const std::vector<uint8_t> expectedRepair{0xfb, 0x12, 0x20, 0x04};
  BOOST_CHECK_EQUAL_COLLECTIONS(generated.codedBytes.begin(), generated.codedBytes.end(),
                                expectedRepair.begin(), expectedRepair.end());

  for (size_t missing = 0; missing < opaque.size(); ++missing) {
    std::vector<std::optional<std::vector<uint8_t>>> received{
      opaque[0], opaque[1], opaque[2]};
    received[missing] = std::nullopt;
    const auto recovered = recoverLiveStreamXorSource(
      definition, generated, received, missing, 1200);
    BOOST_REQUIRE(recovered);
    BOOST_CHECK_EQUAL_COLLECTIONS(recovered->begin(), recovered->end(),
                                  opaque[missing].begin(), opaque[missing].end());
  }

  std::vector<std::optional<std::vector<uint8_t>>> received{
    opaque[0], std::nullopt, opaque[2]};

  auto tampered = generated;
  tampered.sourceDigests[1][0] ^= 0xff;
  BOOST_CHECK(!recoverLiveStreamXorSource(definition, tampered, received, 1, 1200));
  BOOST_CHECK(!recoverLiveStreamXorSource(definition, generated,
                                          {std::nullopt, std::nullopt, opaque[2]},
                                          0, 1200));
  BOOST_CHECK(!recoverLiveStreamXorSource(definition, generated, received, 1, 1600));

  auto corruptRepair = generated;
  corruptRepair.codedBytes[0] ^= 0xff;
  BOOST_CHECK(!recoverLiveStreamXorSource(
    definition, corruptRepair, received, 1, 1200));
  auto wrongLength = generated;
  wrongLength.sourceLengths[1] += 1;
  BOOST_CHECK(!recoverLiveStreamXorSource(
    definition, wrongLength, received, 1, 1200));
  auto wrongGroup = generated;
  wrongGroup.groupId.clear();
  BOOST_CHECK(!wrongGroup.validate(definition).empty());
  auto smallCap = definition;
  smallCap.fec = LiveStreamFecOptions::xorOneRepair(3, 2);
  BOOST_CHECK_THROW(makeLiveStreamXorRepair(
    smallCap, "oversize", sources, repair, opaque, 1000, 1500),
    std::invalid_argument);

  BOOST_CHECK_EQUAL(toString(LiveStreamPrefetchPolicy::MappedPressure),
                    "mapped-pressure");
  BOOST_CHECK_EQUAL(toString(LiveStreamPrefetchPolicy::MappedLiveFutureOn),
                    "mapped-live-v1-future-on");
  BOOST_CHECK_EQUAL(toString(LiveStreamPrefetchPolicy::MappedLiveFutureOff),
                    "mapped-live-v1-future-off");
  BOOST_CHECK_EQUAL(toString(LiveStreamPrefetchPolicy::AdaptiveSampleAtomic),
                    "adaptive-sample-atomic");
}

BOOST_AUTO_TEST_CASE(LiveStreamGf256RepairRecoversAnyTwoOpaqueSources)
{
  LiveStreamDefinition definition;
  definition.streamId = "opaque-stream";
  definition.provider = ndn::Name("/provider");
  definition.semanticDataPrefix = ndn::Name("/provider/stream/session-7");
  definition.sessionEpoch = 7;
  definition.mappingVersion = 3;
  definition.mappingBlockCapacity = 8;
  definition.fec = LiveStreamFecOptions::gf256TwoRepair(4, 8192);

  const std::vector<LiveStreamItemReservation> sources{
    {0, ndn::Name("/provider/stream/session-7/sample/0/seg=0"), 7, 3},
    {1, ndn::Name("/provider/stream/session-7/sample/0/seg=1"), 7, 3},
    {2, ndn::Name("/provider/stream/session-7/sample/0/seg=2"), 7, 3},
    {3, ndn::Name("/provider/stream/session-7/sample/0/seg=3"), 7, 3},
  };
  const std::vector<LiveStreamItemReservation> repairs{
    {4, ndn::Name("/provider/stream/session-7/sample/0/repair/0"), 7, 3},
    {5, ndn::Name("/provider/stream/session-7/sample/0/repair/1"), 7, 3},
  };
  const std::vector<std::vector<uint8_t>> opaque{
    {0x01, 0x02, 0x03, 0x04}, {0x10, 0x20},
    {0xff, 0x00, 0x80}, {0x42, 0x43, 0x44, 0x45, 0x46},
  };
  const auto symbols = makeLiveStreamRepairSymbols(
    definition, "sample-0", sources, repairs, opaque, 1000, 1500);
  BOOST_REQUIRE_EQUAL(symbols.size(), 2);
  BOOST_CHECK_EQUAL(symbols[0].recoveryCapacity, 2);
  BOOST_CHECK_EQUAL(symbols[1].repairIndex, 1);
  LiveStreamFecRepair decoded;
  BOOST_REQUIRE(decoded.wireDecode(symbols[0].wireEncode()));
  BOOST_CHECK_EQUAL(decoded.repairIndex, 0);
  BOOST_CHECK(decoded.validate(definition).empty());
  BOOST_REQUIRE(decoded.wireDecode(symbols[1].wireEncode()));
  BOOST_CHECK_EQUAL(decoded.repairIndex, 1);
  BOOST_CHECK(decoded.scheme == LiveStreamFecScheme::Gf256TwoRepair);
  BOOST_CHECK(decoded.validate(definition).empty());

  for (size_t first = 0; first < opaque.size(); ++first) {
    for (size_t second = first + 1; second < opaque.size(); ++second) {
      std::vector<std::optional<std::vector<uint8_t>>> received{
        opaque[0], opaque[1], opaque[2], opaque[3]};
      received[first] = std::nullopt;
      received[second] = std::nullopt;
      const auto recovered = recoverLiveStreamSources(
        definition, symbols, received, 1200);
      BOOST_REQUIRE(recovered);
      BOOST_REQUIRE(recovered->at(first));
      BOOST_REQUIRE(recovered->at(second));
      BOOST_CHECK_EQUAL_COLLECTIONS(recovered->at(first)->begin(),
                                    recovered->at(first)->end(),
                                    opaque[first].begin(), opaque[first].end());
      BOOST_CHECK_EQUAL_COLLECTIONS(recovered->at(second)->begin(),
                                    recovered->at(second)->end(),
                                    opaque[second].begin(), opaque[second].end());
    }
  }
  std::vector<std::optional<std::vector<uint8_t>>> beyond{
    std::nullopt, std::nullopt, std::nullopt, opaque[3]};
  BOOST_CHECK(!recoverLiveStreamSources(definition, symbols, beyond, 1200));
  auto duplicate = symbols;
  duplicate[1].repairIndex = 0;
  BOOST_CHECK(!recoverLiveStreamSources(definition, duplicate,
                                        {std::nullopt, opaque[1],
                                         std::nullopt, opaque[3]}, 1200));
  auto reversed = symbols;
  std::reverse(reversed.begin(), reversed.end());
  const auto outOfOrder = recoverLiveStreamSources(
    definition, reversed,
    {std::nullopt, opaque[1], std::nullopt, opaque[3]}, 1200);
  BOOST_REQUIRE(outOfOrder);
  BOOST_CHECK_EQUAL_COLLECTIONS(outOfOrder->at(0)->begin(), outOfOrder->at(0)->end(),
                                opaque[0].begin(), opaque[0].end());
  BOOST_CHECK_EQUAL_COLLECTIONS(outOfOrder->at(2)->begin(), outOfOrder->at(2)->end(),
                                opaque[2].begin(), opaque[2].end());
  BOOST_CHECK(!recoverLiveStreamSources(
    definition, symbols,
    {std::nullopt, opaque[1], std::nullopt, opaque[3]}, 1501));
}

BOOST_AUTO_TEST_CASE(LiveStreamSampleEnvelopeFreezesAuthenticatedActualExtent)
{
  LiveStreamSampleEnvelope envelope;
  envelope.groupId = "42";
  envelope.sampleClass = "key";
  envelope.groupItemIndex = 1;
  envelope.actualSourceItems = 3;
  envelope.itemKind = LiveStreamItemKind::Source;
  envelope.opaqueContent = {0x00, 0x01, 0xfe, 0xff};
  const auto wire = envelope.wireEncode();
  LiveStreamSampleEnvelope decoded;
  BOOST_REQUIRE(decoded.wireDecode(wire));
  BOOST_CHECK(decoded.wireEncode() == wire);
  BOOST_CHECK_EQUAL(decoded.groupId, "42");
  BOOST_CHECK_EQUAL(decoded.sampleClass, "key");
  BOOST_CHECK_EQUAL(decoded.actualSourceItems, 3);
  BOOST_CHECK_EQUAL_COLLECTIONS(decoded.opaqueContent.begin(), decoded.opaqueContent.end(),
                                envelope.opaqueContent.begin(), envelope.opaqueContent.end());
  auto invalid = envelope;
  invalid.groupItemIndex = 3;
  BOOST_CHECK_EQUAL(*invalid.validate(), "source-index-outside-actual-extent");
}

BOOST_AUTO_TEST_CASE(LiveStreamAdaptiveSamplesUseWholeGroupsAndVariableFec)
{
  boost::asio::io_context providerIo;
  boost::asio::io_context consumerIo;
  ndn::KeyChain keyChain("pib-memory:live-stream-sample-atomic",
                         "tpm-memory:live-stream-sample-atomic");
  const ndn::Name provider("/memphis/uav/adaptive");
  const auto identity = keyChain.createIdentity(provider);
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enablePacketLogging = true;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace providerFace(providerIo, keyChain, faceOptions);
  ndn::DummyClientFace consumerFace(consumerIo, keyChain, faceOptions);
  auto forwardConsumerInterest = consumerFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) { providerFace.receive(interest); });
  auto forwardProviderData = providerFace.onSendData.connect(
    [&] (const ndn::Data& data) { consumerFace.receive(data); });

  LiveStreamDefinition definition;
  definition.contractVersion = STREAM_NAME_MAP_CONTRACT_VERSION_V2;
  definition.streamId = "front-camera";
  definition.provider = provider;
  definition.semanticDataPrefix = ndn::Name(provider).append("video").appendVersion(7);
  definition.sessionEpoch = 101;
  definition.mappingVersion = 7;
  definition.mappingBlockCapacity = 4;
  definition.mappingAheadBlocks = 4;
  definition.retainedItems = 32;
  definition.maxPendingInterests = 32;
  definition.samplePeriodMs = 33.0;
  definition.sampleClasses = {
    SampleClassProfile::bounded("key", 3, 6, 4, 1),
    SampleClassProfile::bounded("delta", 2, 4, 4, 1),
  };
  definition.fec = LiveStreamFecOptions::xorOneRepair(6, 4096, 500);
  auto publisher = std::make_shared<LiveStreamPublisher>(
    definition, providerFace, keyChain,
    ndn::security::signingByCertificate(
      identity.getDefaultKey().getDefaultCertificate()));
  publisher->start();
  providerFace.processEvents(ndn::time::milliseconds(10));
  providerIo.restart();

  const auto nameFactory = [&definition] (uint64_t sampleId) {
    return [&definition, sampleId] (size_t index, LiveStreamItemKind kind) {
      auto name = ndn::Name(definition.semanticDataPrefix)
                    .append("frame").appendSequenceNumber(sampleId);
      name.append(kind == LiveStreamItemKind::Source ? "source" : "repair")
          .appendSegment(index);
      return name;
    };
  };
  const auto first = publisher->announceSample(1, "key", nameFactory(1));
  BOOST_CHECK_EQUAL(first.predictedSourceItems, 3);
  publisher->publishSample(first, {{1}, {2}}); // N < M, no empty padding
  const auto second = publisher->announceSample(2, "key", nameFactory(2));
  BOOST_CHECK_EQUAL(second.predictedSourceItems, 3);
  publisher->publishSample(second, {{3}, {4}, {5}, {6}, {7}}); // continuation
  const auto descriptor = publisher->activate({33.0, first.group.sources.front().cursor});

  std::atomic_uint64_t delivered{0};
  LiveStreamOpenOptions openOptions;
  openOptions.start = LiveStreamStart::Beginning;
  openOptions.prefetchPolicy = LiveStreamPrefetchPolicy::AdaptiveSampleAtomic;
  openOptions.aggregateInterestLimit = 16;
  openOptions.enableFecRecovery = true;
  openOptions.onItem = [&] (const VerifiedLiveStreamItem& item) {
    BOOST_CHECK(!item.content.empty());
    ++delivered;
    return LiveStreamItemAdmission::acceptItem();
  };
  auto consumer = std::make_shared<LiveStreamConsumerHandle>(
    descriptor, openOptions, consumerFace,
    std::make_shared<MessageValidator>("examples/trust-any.conf"));
  consumer->start();

  for (size_t iteration = 0; iteration < 800 && delivered.load() < 7; ++iteration) {
    providerFace.processEvents(ndn::time::milliseconds(2));
    consumerFace.processEvents(ndn::time::milliseconds(2));
    providerIo.restart();
    consumerIo.restart();
  }
  BOOST_CHECK_EQUAL(delivered.load(), 7);
  const auto consumerStatus = consumer->status();
  BOOST_CHECK(consumerStatus.state == LiveStreamLifecycleState::Active);
  BOOST_REQUIRE(consumerStatus.fetchDecision);
  BOOST_CHECK_GE(consumerStatus.fetchDecision->terminalUnproducedAdvice, 1);
  BOOST_CHECK_GE(consumerStatus.fetchDecision->laterCursorAdvice, 1);
  const auto publisherStatus = publisher->status();
  BOOST_REQUIRE(publisherStatus.sampleClassPredictions.count("key") == 1);
  BOOST_CHECK_EQUAL(publisherStatus.sampleClassPredictions.at("key").underpredictions, 1);
  BOOST_CHECK_EQUAL(publisherStatus.sampleClassPredictions.at("key").overpredictions, 1);
  consumer->stop();
  publisher->stop();
}

BOOST_AUTO_TEST_CASE(LiveStreamAdaptiveSkippedSourceCanRecoverExactlyOnceFromRepair)
{
  boost::asio::io_context providerIo;
  boost::asio::io_context consumerIo;
  ndn::KeyChain keyChain("pib-memory:live-stream-loss-recovery",
                         "tpm-memory:live-stream-loss-recovery");
  const ndn::Name provider("/memphis/uav/loss-recovery");
  const auto identity = keyChain.createIdentity(provider);
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enablePacketLogging = true;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace providerFace(providerIo, keyChain, faceOptions);
  ndn::DummyClientFace consumerFace(consumerIo, keyChain, faceOptions);

  const auto sourceNeedle = std::string("/frame/seq=2/source");
  const auto repairNeedle = std::string("/frame/seq=2/repair");
  size_t sourceNacks = 0;
  std::optional<ndn::Data> heldSource;
  std::optional<ndn::Data> heldRepair;
  auto forwardConsumerInterest = consumerFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) {
      if (interest.getName().toUri().find(sourceNeedle) != std::string::npos) {
        if (sourceNacks++ == 0) providerFace.receive(interest);
        ndn::lp::Nack nack(interest);
        nack.setReason(ndn::lp::NackReason::CONGESTION);
        consumerFace.receive(nack);
      }
      else {
        providerFace.receive(interest);
      }
    });
  auto forwardProviderData = providerFace.onSendData.connect(
    [&] (const ndn::Data& data) {
      const auto uri = data.getName().toUri();
      if (uri.find(sourceNeedle) != std::string::npos) heldSource = data;
      else if (uri.find(repairNeedle) != std::string::npos) heldRepair = data;
      else consumerFace.receive(data);
    });

  LiveStreamDefinition definition;
  definition.contractVersion = STREAM_NAME_MAP_CONTRACT_VERSION_V2;
  definition.streamId = "loss-recovery";
  definition.provider = provider;
  definition.semanticDataPrefix = ndn::Name(provider).append("video").appendVersion(1);
  definition.sessionEpoch = 201;
  definition.mappingVersion = 1;
  definition.mappingBlockCapacity = 8;
  definition.mappingAheadBlocks = 4;
  definition.retainedItems = 32;
  definition.maxPendingInterests = 32;
  definition.samplePeriodMs = 33.0;
  definition.sampleClasses = {SampleClassProfile::bounded("delta", 1, 1, 4, 0)};
  definition.fec = LiveStreamFecOptions::xorOneRepair(1, 4096, 5000);
  auto publisher = std::make_shared<LiveStreamPublisher>(
    definition, providerFace, keyChain,
    ndn::security::signingByCertificate(
      identity.getDefaultKey().getDefaultCertificate()));
  publisher->start();
  providerFace.processEvents(ndn::time::milliseconds(10));
  providerIo.restart();

  const auto names = [&definition] (uint64_t sampleId) {
    return [&definition, sampleId] (size_t index, LiveStreamItemKind kind) {
      auto name = ndn::Name(definition.semanticDataPrefix)
                    .append("frame").appendSequenceNumber(sampleId);
      name.append(kind == LiveStreamItemKind::Source ? "source" : "repair")
          .appendSegment(index);
      return name;
    };
  };
  const auto first = publisher->announceSample(1, "delta", names(1));
  publisher->publishSample(first, {{1}});
  const auto descriptor = publisher->activate({33.0, first.group.sources.front().cursor});

  std::vector<LiveStreamItemProvenance> provenance;
  LiveStreamOpenOptions options;
  options.start = LiveStreamStart::Latest;
  options.prefetchPolicy = LiveStreamPrefetchPolicy::AdaptiveSampleAtomic;
  options.aggregateInterestLimit = 16;
  options.enableFecRecovery = true;
  options.onItem = [&] (const VerifiedLiveStreamItem& item) {
    provenance.push_back(item.provenance);
    return LiveStreamItemAdmission::acceptItem();
  };
  auto consumer = std::make_shared<LiveStreamConsumerHandle>(
    descriptor, options, consumerFace,
    std::make_shared<MessageValidator>("examples/trust-any.conf"));
  consumer->start();

  const auto second = publisher->announceSample(2, "delta", names(2));
  publisher->publishSample(second, {{2}});
  for (size_t iteration = 0;
       iteration < 400 && (!heldRepair || consumer->status().rejected == 0);
       ++iteration) {
    providerFace.processEvents(ndn::time::milliseconds(2));
    consumerFace.processEvents(ndn::time::milliseconds(2));
    providerIo.restart();
    consumerIo.restart();
  }
  BOOST_REQUIRE(heldSource);
  BOOST_REQUIRE(heldRepair);
  BOOST_CHECK_GE(sourceNacks, 3);
  BOOST_CHECK_GE(consumer->status().rejected, 1);

  consumerFace.receive(*heldRepair);
  for (size_t iteration = 0; iteration < 200 && consumer->status().recovered == 0;
       ++iteration) {
    consumerFace.processEvents(ndn::time::milliseconds(2));
    consumerIo.restart();
  }
  const auto recoveredStatus = consumer->status();
  BOOST_CHECK_EQUAL(recoveredStatus.recovered, 1);
  BOOST_CHECK_EQUAL(recoveredStatus.terminalMissingSources, 1);
  BOOST_CHECK_EQUAL(recoveredStatus.recoverableGroups, 1);
  BOOST_CHECK_EQUAL(recoveredStatus.recoveredGroups, 1);
  BOOST_CHECK_EQUAL(recoveredStatus.rejected, 0);
  BOOST_CHECK_GT(recoveredStatus.mappingDataResponses, 0);
  BOOST_CHECK_LE(recoveredStatus.mappingDataResponses,
                 recoveredStatus.mappingInterests);
  BOOST_CHECK_LE(recoveredStatus.mappingNewDataResponses,
                 recoveredStatus.mappingDataResponses);
  BOOST_CHECK_EQUAL(recoveredStatus.payloadRepairDataResponses, 2);
  BOOST_CHECK_EQUAL(recoveredStatus.payloadRepairDataConsumed, 1);
  BOOST_CHECK_EQUAL(recoveredStatus.payloadProtectionOnlyInterests, 1);
  BOOST_CHECK_EQUAL(recoveredStatus.payloadApplicationUsefulInterests,
                    recoveredStatus.payloadSourceDataAdmissions +
                    recoveredStatus.payloadRepairDataConsumed);
  BOOST_CHECK_GE(recoveredStatus.payloadNonproductiveInterests, 3);
  BOOST_CHECK_EQUAL(
    recoveredStatus.payloadApplicationUsefulInterests +
      recoveredStatus.payloadProtectionOnlyInterests +
      recoveredStatus.payloadNonproductiveInterests +
      recoveredStatus.payloadUnresolvedInterests,
    recoveredStatus.payloadInterests);
  BOOST_CHECK_EQUAL(std::count(provenance.begin(), provenance.end(),
                               LiveStreamItemProvenance::FecRecovered), 1);

  // A network-late original Data no longer has a pending Interest and cannot
  // cross the application callback boundary a second time.
  const auto deliveredBeforeLate = provenance.size();
  consumerFace.receive(*heldSource);
  consumerFace.processEvents(ndn::time::milliseconds(10));
  BOOST_CHECK_EQUAL(provenance.size(), deliveredBeforeLate);
  consumer->stop();
  publisher->stop();
}

BOOST_AUTO_TEST_CASE(LiveStreamReorderedRepairDoesNotPreemptInFlightSignedSource)
{
  boost::asio::io_context providerIo;
  boost::asio::io_context consumerIo;
  ndn::KeyChain keyChain("pib-memory:live-stream-reordered-repair",
                         "tpm-memory:live-stream-reordered-repair");
  const ndn::Name provider("/lab/sensor/reordered-repair");
  const auto identity = keyChain.createIdentity(provider);
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enablePacketLogging = true;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace providerFace(providerIo, keyChain, faceOptions);
  ndn::DummyClientFace consumerFace(consumerIo, keyChain, faceOptions);
  std::optional<ndn::Data> heldSource;
  auto forwardConsumerInterest = consumerFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) { providerFace.receive(interest); });
  auto forwardProviderData = providerFace.onSendData.connect(
    [&] (const ndn::Data& data) {
      if (data.getName().toUri().find("/source/") != std::string::npos) {
        heldSource = data;
      }
      else {
        consumerFace.receive(data);
      }
    });

  LiveStreamDefinition definition;
  definition.contractVersion = STREAM_NAME_MAP_CONTRACT_VERSION_V2;
  definition.streamId = "reordered-repair";
  definition.provider = provider;
  definition.semanticDataPrefix =
    ndn::Name(provider).append("measurements").appendVersion(1);
  definition.sessionEpoch = 212;
  definition.mappingVersion = 1;
  definition.mappingBlockCapacity = 8;
  definition.mappingAheadBlocks = 2;
  definition.retainedItems = 32;
  definition.maxPendingInterests = 32;
  definition.samplePeriodMs = 40.0;
  definition.sampleClasses = {
    SampleClassProfile::bounded("sample", 1, 1, 4, 0),
  };
  definition.fec = LiveStreamFecOptions::xorOneRepair(1, 4096, 5000);
  auto publisher = std::make_shared<LiveStreamPublisher>(
    definition, providerFace, keyChain,
    ndn::security::signingByCertificate(
      identity.getDefaultKey().getDefaultCertificate()));
  publisher->start();
  providerFace.processEvents(ndn::time::milliseconds(10));
  providerIo.restart();

  const auto names = [&definition] (size_t index, LiveStreamItemKind kind) {
    return ndn::Name(definition.semanticDataPrefix)
      .append("sample").appendSequenceNumber(0)
      .append(kind == LiveStreamItemKind::Source ? "source" : "repair")
      .appendSegment(index);
  };
  const auto sample = publisher->announceSample(0, "sample", names);
  publisher->publishSample(sample, {{0x42}});
  const auto descriptor =
    publisher->activate({40.0, sample.group.sources.front().cursor});

  std::vector<LiveStreamItemProvenance> provenance;
  LiveStreamOpenOptions options;
  options.start = LiveStreamStart::Beginning;
  options.prefetchPolicy = LiveStreamPrefetchPolicy::AdaptiveSampleAtomic;
  options.aggregateInterestLimit = 16;
  options.enableFecRecovery = true;
  options.onItem = [&] (const VerifiedLiveStreamItem& item) {
    provenance.push_back(item.provenance);
    BOOST_CHECK_EQUAL(item.content.size(), 1);
    BOOST_CHECK_EQUAL(item.content.front(), 0x42);
    return LiveStreamItemAdmission::acceptItem();
  };
  auto consumer = std::make_shared<LiveStreamConsumerHandle>(
    descriptor, options, consumerFace,
    std::make_shared<MessageValidator>("examples/trust-any.conf"));
  consumer->start();

  const auto pump = [&] {
    providerFace.processEvents(ndn::time::milliseconds(2));
    consumerFace.processEvents(ndn::time::milliseconds(2));
    providerIo.restart();
    consumerIo.restart();
  };
  for (size_t i = 0; i < 400 && (!heldSource ||
       consumer->status().payloadRepairDataResponses == 0); ++i) {
    pump();
  }
  BOOST_REQUIRE(heldSource);
  const auto reordered = consumer->status();
  BOOST_REQUIRE_GT(reordered.payloadRepairDataResponses, 0);
  BOOST_CHECK_EQUAL(reordered.recoveryAttempts, 0);
  BOOST_CHECK_EQUAL(reordered.recoveryExhaustions, 0);
  BOOST_CHECK_EQUAL(reordered.recovered, 0);
  BOOST_CHECK(provenance.empty());

  consumerFace.receive(*heldSource);
  for (size_t i = 0; i < 200 && provenance.empty(); ++i) pump();
  BOOST_REQUIRE_EQUAL(provenance.size(), 1);
  BOOST_CHECK(provenance.front() == LiveStreamItemProvenance::SignedData);
  const auto finalStatus = consumer->status();
  BOOST_CHECK_EQUAL(finalStatus.recovered, 0);
  BOOST_CHECK_EQUAL(finalStatus.lateArrivals, 0);
  consumer->stop();
  publisher->stop();
}

BOOST_AUTO_TEST_CASE(LiveStreamAuthenticatedRepairRecoversAfterSourceTimeout)
{
  boost::asio::io_context providerIo;
  boost::asio::io_context consumerIo;
  ndn::KeyChain keyChain("pib-memory:live-stream-terminal-repair",
                         "tpm-memory:live-stream-terminal-repair");
  const ndn::Name provider("/lab/sensor/terminal-repair");
  const auto identity = keyChain.createIdentity(provider);
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enablePacketLogging = true;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace providerFace(providerIo, keyChain, faceOptions);
  ndn::DummyClientFace consumerFace(consumerIo, keyChain, faceOptions);
  std::optional<ndn::Interest> heldSourceInterest;
  size_t sourceInterests = 0;
  auto forwardConsumerInterest = consumerFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) {
      if (interest.getName().toUri().find("/source/") != std::string::npos) {
        ++sourceInterests;
        heldSourceInterest = interest;
      }
      else {
        providerFace.receive(interest);
      }
    });
  auto forwardProviderData = providerFace.onSendData.connect(
    [&] (const ndn::Data& data) { consumerFace.receive(data); });

  LiveStreamDefinition definition;
  definition.contractVersion = STREAM_NAME_MAP_CONTRACT_VERSION_V2;
  definition.streamId = "terminal-repair";
  definition.provider = provider;
  definition.semanticDataPrefix =
    ndn::Name(provider).append("measurements").appendVersion(1);
  definition.sessionEpoch = 213;
  definition.mappingVersion = 1;
  definition.mappingBlockCapacity = 8;
  definition.mappingAheadBlocks = 2;
  definition.retainedItems = 32;
  definition.maxPendingInterests = 32;
  definition.samplePeriodMs = 40.0;
  definition.sampleClasses = {
    SampleClassProfile::bounded("sample", 1, 1, 4, 0),
  };
  definition.fec = LiveStreamFecOptions::xorOneRepair(1, 4096, 5000);
  auto publisher = std::make_shared<LiveStreamPublisher>(
    definition, providerFace, keyChain,
    ndn::security::signingByCertificate(
      identity.getDefaultKey().getDefaultCertificate()));
  publisher->start();
  providerFace.processEvents(ndn::time::milliseconds(10));
  providerIo.restart();

  const auto names = [&definition] (size_t index, LiveStreamItemKind kind) {
    return ndn::Name(definition.semanticDataPrefix)
      .append("sample").appendSequenceNumber(0)
      .append(kind == LiveStreamItemKind::Source ? "source" : "repair")
      .appendSegment(index);
  };
  const auto sample = publisher->announceSample(0, "sample", names);
  publisher->publishSample(sample, {{0x53}});
  const auto descriptor =
    publisher->activate({40.0, sample.group.sources.front().cursor});

  std::vector<LiveStreamItemProvenance> provenance;
  LiveStreamOpenOptions options;
  options.start = LiveStreamStart::Beginning;
  options.prefetchPolicy = LiveStreamPrefetchPolicy::AdaptiveSampleAtomic;
  options.aggregateInterestLimit = 16;
  options.enableFecRecovery = true;
  options.onItem = [&] (const VerifiedLiveStreamItem& item) {
    provenance.push_back(item.provenance);
    BOOST_CHECK_EQUAL(item.content.size(), 1);
    BOOST_CHECK_EQUAL(item.content.front(), 0x53);
    return LiveStreamItemAdmission::acceptItem();
  };
  auto consumer = std::make_shared<LiveStreamConsumerHandle>(
    descriptor, options, consumerFace,
    std::make_shared<MessageValidator>("examples/trust-any.conf"));
  consumer->start();

  const auto pump = [&] {
    providerFace.processEvents(ndn::time::milliseconds(2));
    consumerFace.processEvents(ndn::time::milliseconds(2));
    providerIo.restart();
    consumerIo.restart();
  };
  for (size_t i = 0; i < 400 && (!heldSourceInterest ||
       consumer->status().payloadRepairDataResponses == 0); ++i) {
    pump();
  }
  BOOST_REQUIRE(heldSourceInterest);
  BOOST_REQUIRE_GT(consumer->status().payloadRepairDataResponses, 0);

  for (size_t i = 0; i < 400 && provenance.empty(); ++i) pump();

  BOOST_REQUIRE_EQUAL(provenance.size(), 1);
  BOOST_CHECK(provenance.front() == LiveStreamItemProvenance::FecRecovered);
  BOOST_CHECK_EQUAL(sourceInterests, 1);
  const auto status = consumer->status();
  BOOST_CHECK_EQUAL(status.nacks, 0);
  BOOST_CHECK_GE(status.timeouts, 1);
  BOOST_CHECK_EQUAL(status.recovered, 1);
  BOOST_CHECK_EQUAL(status.recoveryEligibleSources, 1);
  BOOST_CHECK_EQUAL(status.terminalMissingSources, 0);
  BOOST_CHECK_EQUAL(status.recoverableGroups, 1);
  BOOST_CHECK_EQUAL(status.recoveredGroups, 1);
  BOOST_CHECK_EQUAL(status.recoveryAttempts, 1);
  BOOST_CHECK_EQUAL(status.retryAttempts, 0);
  BOOST_CHECK_EQUAL(status.lateArrivals, 0);
  consumer->stop();
  publisher->stop();
}

BOOST_AUTO_TEST_CASE(LiveStreamGf256AdaptiveRepairIsNotDeliveredAsApplicationPayload)
{
  boost::asio::io_context providerIo;
  boost::asio::io_context consumerIo;
  ndn::KeyChain keyChain("pib-memory:live-stream-gf256-end-to-end",
                         "tpm-memory:live-stream-gf256-end-to-end");
  const ndn::Name provider("/lab/sensor/gf256");
  const auto identity = keyChain.createIdentity(provider);
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enablePacketLogging = true;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace providerFace(providerIo, keyChain, faceOptions);
  ndn::DummyClientFace consumerFace(consumerIo, keyChain, faceOptions);
  size_t droppedRepairData = 0;
  auto forwardConsumerInterest = consumerFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) { providerFace.receive(interest); });
  auto forwardProviderData = providerFace.onSendData.connect(
    [&] (const ndn::Data& data) {
      if (data.getName().toUri().find("/repair/") != std::string::npos) {
        ++droppedRepairData;
        return;
      }
      consumerFace.receive(data);
    });

  LiveStreamDefinition definition;
  definition.contractVersion = STREAM_NAME_MAP_CONTRACT_VERSION_V2;
  definition.streamId = "variable-segment-stream";
  definition.provider = provider;
  definition.semanticDataPrefix = ndn::Name(provider).append("measurements").appendVersion(1);
  definition.sessionEpoch = 211;
  definition.mappingVersion = 1;
  definition.mappingBlockCapacity = 8;
  definition.mappingAheadBlocks = 4;
  definition.retainedItems = 32;
  definition.maxPendingInterests = 32;
  definition.samplePeriodMs = 20.0;
  definition.sampleClasses = {SampleClassProfile::bounded("sample", 1, 8, 4, 0)};
  definition.fec = LiveStreamFecOptions::gf256TwoRepair(8, 4096, 5000);
  auto publisher = std::make_shared<LiveStreamPublisher>(
    definition, providerFace, keyChain,
    ndn::security::signingByCertificate(
      identity.getDefaultKey().getDefaultCertificate()));
  publisher->start();
  providerFace.processEvents(ndn::time::milliseconds(10));
  providerIo.restart();

  const auto names = [&definition] (size_t sampleId) {
    return [&definition, sampleId] (size_t index, LiveStreamItemKind kind) {
      return ndn::Name(definition.semanticDataPrefix)
        .append("sample").appendSequenceNumber(sampleId)
        .append(kind == LiveStreamItemKind::Source ? "source" : "repair")
        .appendSegment(index);
    };
  };
  const auto sample = publisher->announceSample(0, "sample", names(0));
  publisher->publishSample(sample, {{0x01}});
  const auto descriptor = publisher->activate({20.0, sample.group.sources.front().cursor});

  std::vector<VerifiedLiveStreamItem> delivered;
  LiveStreamOpenOptions options;
  options.start = LiveStreamStart::Beginning;
  options.prefetchPolicy = LiveStreamPrefetchPolicy::AdaptiveSampleAtomic;
  options.aggregateInterestLimit = 16;
  options.enableFecRecovery = true;
  options.onItem = [&] (const VerifiedLiveStreamItem& item) {
    delivered.push_back(item);
    return LiveStreamItemAdmission::acceptItem();
  };
  auto consumer = std::make_shared<LiveStreamConsumerHandle>(
    descriptor, options, consumerFace,
    std::make_shared<MessageValidator>("examples/trust-any.conf"));
  consumer->start();
  for (size_t iteration = 0; iteration < 500; ++iteration) {
    providerFace.processEvents(ndn::time::milliseconds(2));
    consumerFace.processEvents(ndn::time::milliseconds(2));
    providerIo.restart();
    consumerIo.restart();
  }
  const auto status = consumer->status();
  BOOST_TEST_CONTEXT("state=" << toString(status.state) << " reason=" << status.reason) {
    for (const auto& item : delivered) {
      BOOST_TEST_MESSAGE("delivered cursor=" << item.cursor << " name=" <<
                         item.originalName << " bytes=" << item.content.size());
    }
    BOOST_CHECK(status.state == LiveStreamLifecycleState::Active);
    BOOST_CHECK_EQUAL(status.rejected, 0);
    BOOST_REQUIRE(status.fetchDecision);
    BOOST_CHECK_GE(status.fetchDecision->window,
      definition.fec.maxSourceItems + 2 * definition.fec.repairItemCount());
    if (delivered.size() > 1) {
      LiveStreamFecRepair observedRepair;
      const auto decoded = observedRepair.wireDecode(ndn::Block(
        ndn::span<const uint8_t>(delivered[1].content.data(), delivered[1].content.size())));
      BOOST_TEST_MESSAGE("repair decoded=" << decoded << " validation=" <<
                         (decoded ? observedRepair.validate(definition) : "not-decoded") <<
                         " name-match=" << (decoded && observedRepair.repairName ==
                                             delivered[1].originalName) <<
                         " cursor-match=" << (decoded && observedRepair.repairCursor ==
                                               delivered[1].cursor));
    }
    BOOST_CHECK_EQUAL(delivered.size(), 1);
    BOOST_CHECK_EQUAL(delivered.front().content.size(), 1);
    BOOST_CHECK_GT(droppedRepairData, 0);
    BOOST_CHECK_EQUAL(status.retryPayloadRepairInterests, 0);
    BOOST_CHECK_EQUAL(status.payloadUnresolvedInterests, 0);
    BOOST_CHECK_GE(status.retrySuppressions, 1);
    BOOST_CHECK_GE(status.retrySuppressionReasons.at("repair-retry-unneeded"), 1);
  }
  consumer->stop();
  publisher->stop();
}

BOOST_AUTO_TEST_CASE(LiveStreamAdaptiveSchedulingPrioritizesSourcesAcrossGroups)
{
  boost::asio::io_context providerIo;
  boost::asio::io_context consumerIo;
  ndn::KeyChain keyChain("pib-memory:live-stream-source-first",
                         "tpm-memory:live-stream-source-first");
  const ndn::Name provider("/lab/sensor/source-first");
  const auto identity = keyChain.createIdentity(provider);
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enablePacketLogging = true;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace providerFace(providerIo, keyChain, faceOptions);
  ndn::DummyClientFace consumerFace(consumerIo, keyChain, faceOptions);
  std::vector<std::string> payloadInterestKinds;
  std::vector<std::string> payloadInterestNames;
  std::vector<ndn::Data> heldMappings;
  auto forwardConsumerInterest = consumerFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) {
      const auto uri = interest.getName().toUri();
      if (uri.find("/source/") != std::string::npos) {
        payloadInterestKinds.emplace_back("source");
        payloadInterestNames.push_back(uri);
      }
      else if (uri.find("/repair/") != std::string::npos) {
        payloadInterestKinds.emplace_back("repair");
        payloadInterestNames.push_back(uri);
      }
      providerFace.receive(interest);
    });
  auto forwardProviderData = providerFace.onSendData.connect(
    [&] (const ndn::Data& data) {
      const auto uri = data.getName().toUri();
      if (uri.find("/source/") != std::string::npos ||
          uri.find("/repair/") != std::string::npos) {
        consumerFace.receive(data);
        return;
      }
      heldMappings.push_back(data);
      if (heldMappings.size() == 3) {
        // Admit later signed Mapping blocks to quarantine first. The anchor
        // then connects all three atomically, giving one schedule pass three
        // authenticated groups whose source/repair priority is observable.
        for (auto it = heldMappings.rbegin(); it != heldMappings.rend(); ++it) {
          consumerFace.receive(*it);
        }
      }
    });

  LiveStreamDefinition definition;
  definition.contractVersion = STREAM_NAME_MAP_CONTRACT_VERSION_V2;
  definition.streamId = "source-first-stream";
  definition.provider = provider;
  definition.semanticDataPrefix =
    ndn::Name(provider).append("measurements").appendVersion(1);
  definition.sessionEpoch = 212;
  definition.mappingVersion = 1;
  definition.mappingBlockCapacity = 8;
  definition.mappingAheadBlocks = 4;
  definition.retainedItems = 32;
  definition.maxPendingInterests = 32;
  definition.samplePeriodMs = 20.0;
  definition.sampleClasses = {
    SampleClassProfile::bounded("sample", 1, 1, 1, 0),
  };
  definition.fec = LiveStreamFecOptions::gf256TwoRepair(1, 4096, 5000);
  auto publisher = std::make_shared<LiveStreamPublisher>(
    definition, providerFace, keyChain,
    ndn::security::signingByCertificate(
      identity.getDefaultKey().getDefaultCertificate()));
  publisher->start();
  providerFace.processEvents(ndn::time::milliseconds(10));
  providerIo.restart();

  const auto names = [&definition] (size_t sampleId) {
    return [&definition, sampleId] (size_t index, LiveStreamItemKind kind) {
      return ndn::Name(definition.semanticDataPrefix)
        .append("sample").appendSequenceNumber(sampleId)
        .append(kind == LiveStreamItemKind::Source ? "source" : "repair")
        .appendSegment(index);
    };
  };
  const auto first = publisher->announceSample(0, "sample", names(0));
  publisher->publishSample(first, {{0x01}});
  const auto second = publisher->announceSample(1, "sample", names(1));
  publisher->publishSample(second, {{0x02}});
  const auto third = publisher->announceSample(2, "sample", names(2));
  publisher->publishSample(third, {{0x03}});
  const auto descriptor =
    publisher->activate({20.0, first.group.sources.front().cursor});

  LiveStreamOpenOptions options;
  options.start = LiveStreamStart::Beginning;
  options.prefetchPolicy = LiveStreamPrefetchPolicy::AdaptiveSampleAtomic;
  options.aggregateInterestLimit = 16;
  options.enableFecRecovery = true;
  options.onItem = [] (const VerifiedLiveStreamItem&) {
    return LiveStreamItemAdmission::acceptItem();
  };
  auto consumer = std::make_shared<LiveStreamConsumerHandle>(
    descriptor, options, consumerFace,
    std::make_shared<MessageValidator>("examples/trust-any.conf"));
  consumer->start();
  for (size_t iteration = 0;
       iteration < 500 && payloadInterestKinds.size() < 5; ++iteration) {
    providerFace.processEvents(ndn::time::milliseconds(2));
    consumerFace.processEvents(ndn::time::milliseconds(2));
    providerIo.restart();
    consumerIo.restart();
  }

  BOOST_REQUIRE_GE(payloadInterestKinds.size(), 3);
  for (const auto& name : payloadInterestNames) {
    BOOST_TEST_MESSAGE("payload Interest " << name);
  }
  BOOST_CHECK_EQUAL(payloadInterestKinds[0], "source");
  BOOST_CHECK_EQUAL(payloadInterestKinds[1], "source");
  BOOST_CHECK_EQUAL(payloadInterestKinds[2], "source");
  consumer->stop();
  publisher->stop();
}

BOOST_AUTO_TEST_CASE(LiveStreamFuturePayloadRetriesSameNameAndDeliversOnce)
{
  boost::asio::io_context providerIo;
  boost::asio::io_context consumerIo;
  ndn::KeyChain keyChain("pib-memory:live-stream-generic-retry",
                         "tpm-memory:live-stream-generic-retry");
  const ndn::Name provider("/lab/sensor/provider");
  const auto identity = keyChain.createIdentity(provider);
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enablePacketLogging = true;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace providerFace(providerIo, keyChain, faceOptions);
  ndn::DummyClientFace consumerFace(consumerIo, keyChain, faceOptions);
  auto forwardConsumerInterest = consumerFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) { providerFace.receive(interest); });
  auto forwardProviderData = providerFace.onSendData.connect(
    [&] (const ndn::Data& data) { consumerFace.receive(data); });

  LiveStreamDefinition definition;
  definition.contractVersion = STREAM_NAME_MAP_CONTRACT_VERSION_V2;
  definition.streamId = "periodic-temperature";
  definition.provider = provider;
  definition.semanticDataPrefix = ndn::Name(provider).append("temperature").appendVersion(1);
  definition.sessionEpoch = 301;
  definition.mappingVersion = 1;
  definition.mappingBlockCapacity = 1;
  definition.mappingAheadBlocks = 2;
  definition.retainedItems = 16;
  definition.maxPendingInterests = 8;
  definition.samplePeriodMs = 10.0;
  definition.sampleClasses = {
    SampleClassProfile::bounded("reading", 1, 1, 1, 0),
  };
  definition.fec = LiveStreamFecOptions::none();
  auto publisher = std::make_shared<LiveStreamPublisher>(
    definition, providerFace, keyChain,
    ndn::security::signingByCertificate(
      identity.getDefaultKey().getDefaultCertificate()));
  publisher->start();
  providerFace.processEvents(ndn::time::milliseconds(10));
  providerIo.restart();

  const auto names = [&definition] (uint64_t sampleId) {
    return [&definition, sampleId] (size_t index, LiveStreamItemKind) {
      return ndn::Name(definition.semanticDataPrefix)
        .append("reading").appendSequenceNumber(sampleId).appendSegment(index);
    };
  };
  const auto first = publisher->announceSample(1, "reading", names(1));
  publisher->publishSample(first, {{0x01}});
  const auto descriptor = publisher->activate({10.0, first.group.sources.front().cursor});

  std::vector<StreamCursor> delivered;
  LiveStreamOpenOptions options;
  options.start = LiveStreamStart::Beginning;
  options.prefetchPolicy = LiveStreamPrefetchPolicy::AdaptiveSampleAtomic;
  options.aggregateInterestLimit = 8;
  options.interestLifetimeMs = 40;
  options.enableFecRecovery = false;
  options.onItem = [&] (const VerifiedLiveStreamItem& item) {
    delivered.push_back(item.cursor);
    return LiveStreamItemAdmission::acceptItem();
  };
  auto consumer = std::make_shared<LiveStreamConsumerHandle>(
    descriptor, options, consumerFace,
    std::make_shared<MessageValidator>("examples/trust-any.conf"));
  consumer->start();

  const auto pump = [&] {
    providerFace.processEvents(ndn::time::milliseconds(2));
    consumerFace.processEvents(ndn::time::milliseconds(2));
    providerIo.restart();
    consumerIo.restart();
  };
  for (size_t i = 0; i < 400 && delivered.size() < 1; ++i) pump();
  BOOST_REQUIRE_EQUAL(delivered.size(), 1);

  const auto second = publisher->announceSample(2, "reading", names(2));
  const auto secondName = second.group.sources.front().originalName;
  for (size_t i = 0; i < 1000; ++i) {
    pump();
    if (consumer->status().retryFuturePayloadInterests > 0 &&
        publisher->status().providerRetryFutureInterests > 0) break;
  }
  const auto beforePublish = consumer->status();
  const auto providerBeforePublish = publisher->status();
  BOOST_REQUIRE_GT(beforePublish.retryFuturePayloadInterests, 0);
  BOOST_REQUIRE_GT(providerBeforePublish.providerRetryFutureInterests, 0);

  publisher->publishSample(second, {{0x02}});
  for (size_t i = 0; i < 400 && delivered.size() < 2; ++i) pump();
  BOOST_REQUIRE_EQUAL(delivered.size(), 2);
  BOOST_CHECK_EQUAL(std::count(delivered.begin(), delivered.end(),
                               second.group.sources.front().cursor), 1);

  std::vector<ndn::Name> secondAttempts;
  for (const auto& interest : consumerFace.sentInterests) {
    if (interest.getName() == secondName) secondAttempts.push_back(interest.getName());
  }
  BOOST_CHECK_GE(secondAttempts.size(), 2);
  BOOST_CHECK(std::all_of(secondAttempts.begin(), secondAttempts.end(),
                         [&] (const auto& name) { return name == secondName; }));
  const auto status = consumer->status();
  const auto providerStatus = publisher->status();
  BOOST_CHECK_EQUAL(status.payloadInterests,
                    status.initialPayloadInterests + status.retryPayloadInterests);
  BOOST_CHECK_EQUAL(status.payloadInterests, status.payloadSourceInterests);
  BOOST_CHECK_EQUAL(status.payloadSourceInterests,
                    status.initialPayloadSourceInterests +
                      status.retryPayloadSourceInterests);
  BOOST_CHECK_EQUAL(status.payloadRepairInterests, 0);
  BOOST_CHECK_EQUAL(status.payloadUnclassifiedInterests, 0);
  BOOST_CHECK_EQUAL(status.futurePayloadInterests,
                    status.initialFuturePayloadInterests +
                      status.retryFuturePayloadInterests);
  BOOST_CHECK_GT(status.retrySuccesses, 0);
  BOOST_CHECK_EQUAL(status.rejected, 0);
  BOOST_CHECK_EQUAL(status.declaredRecoveryCapacity, 0);
  BOOST_CHECK_GT(providerStatus.providerRetryFutureHits, 0);
  BOOST_CHECK_EQUAL(providerStatus.providerFutureInterests,
                    providerStatus.providerInitialFutureInterests +
                      providerStatus.providerRetryFutureInterests);
  BOOST_CHECK_EQUAL(providerStatus.providerFutureHits,
                    providerStatus.providerInitialFutureHits +
                      providerStatus.providerRetryFutureHits);
  consumer->stop();
  publisher->stop();
}

BOOST_AUTO_TEST_CASE(LiveStreamCompletedGroupsDoNotConsumeUnresolvedHorizon)
{
  boost::asio::io_context providerIo;
  boost::asio::io_context consumerIo;
  ndn::KeyChain keyChain("pib-memory:live-stream-completed-horizon",
                         "tpm-memory:live-stream-completed-horizon");
  const ndn::Name provider("/lab/sensor/completed-horizon");
  const auto identity = keyChain.createIdentity(provider);
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enablePacketLogging = true;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace providerFace(providerIo, keyChain, faceOptions);
  ndn::DummyClientFace consumerFace(consumerIo, keyChain, faceOptions);
  std::optional<ndn::Data> heldFirstSource;
  std::vector<ndn::Name> sourceNames;
  auto forwardConsumerInterest = consumerFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) { providerFace.receive(interest); });
  auto forwardProviderData = providerFace.onSendData.connect(
    [&] (const ndn::Data& data) {
      if (!sourceNames.empty() && data.getName() == sourceNames.front()) {
        heldFirstSource = data;
      }
      else {
        consumerFace.receive(data);
      }
    });

  LiveStreamDefinition definition;
  definition.contractVersion = STREAM_NAME_MAP_CONTRACT_VERSION_V2;
  definition.streamId = "completed-horizon";
  definition.provider = provider;
  definition.semanticDataPrefix =
    ndn::Name(provider).append("measurements").appendVersion(1);
  definition.sessionEpoch = 214;
  definition.mappingVersion = 1;
  definition.mappingBlockCapacity = 8;
  definition.mappingAheadBlocks = 4;
  definition.retainedItems = 256;
  definition.maxNameReservations = 256;
  definition.maxPendingInterests = 64;
  definition.samplePeriodMs = 40.0;
  definition.sampleClasses = {
    SampleClassProfile::bounded("sample", 1, 1, 4, 0),
  };
  definition.fec = LiveStreamFecOptions::none();
  auto publisher = std::make_shared<LiveStreamPublisher>(
    definition, providerFace, keyChain,
    ndn::security::signingByCertificate(
      identity.getDefaultKey().getDefaultCertificate()));
  publisher->start();
  providerFace.processEvents(ndn::time::milliseconds(10));
  providerIo.restart();

  StreamCursor firstCursor = 0;
  for (uint64_t sampleId = 0; sampleId < 24; ++sampleId) {
    const auto names = [&definition, sampleId] (size_t index,
                                                LiveStreamItemKind kind) {
      return ndn::Name(definition.semanticDataPrefix)
        .append("sample").appendSequenceNumber(sampleId)
        .append(kind == LiveStreamItemKind::Source ? "source" : "repair")
        .appendSegment(index);
    };
    const auto sample = publisher->announceSample(sampleId, "sample", names);
    if (sampleId == 0) firstCursor = sample.group.sources.front().cursor;
    sourceNames.push_back(sample.group.sources.front().originalName);
    publisher->publishSample(sample, {{static_cast<uint8_t>(sampleId)}});
  }
  const auto descriptor = publisher->activate({40.0, firstCursor});

  std::atomic_uint64_t delivered{0};
  LiveStreamOpenOptions options;
  options.start = LiveStreamStart::Beginning;
  options.prefetchPolicy = LiveStreamPrefetchPolicy::AdaptiveSampleAtomic;
  options.aggregateInterestLimit = 8;
  options.interestLifetimeMs = 500;
  options.enableFecRecovery = true;
  options.onItem = [&] (const VerifiedLiveStreamItem&) {
    ++delivered;
    return LiveStreamItemAdmission::acceptItem();
  };
  auto consumer = std::make_shared<LiveStreamConsumerHandle>(
    descriptor, options, consumerFace,
    std::make_shared<MessageValidator>("examples/trust-any.conf"));
  consumer->start();

  for (size_t iteration = 0; iteration < 40; ++iteration) {
    providerFace.processEvents(ndn::time::milliseconds(1));
    consumerFace.processEvents(ndn::time::milliseconds(1));
    providerIo.restart();
    consumerIo.restart();
  }
  BOOST_REQUIRE(heldFirstSource);
  // The first cursor is deliberately unresolved. Later completed groups must
  // be skipped while filling the same bounded unresolved horizon, allowing
  // the pipeline to move beyond its initial eight-group span before timeout.
  BOOST_CHECK(std::any_of(
    consumerFace.sentInterests.begin(), consumerFace.sentInterests.end(),
    [&] (const auto& interest) { return interest.getName() == sourceNames[16]; }));
  BOOST_CHECK_GT(delivered.load(), 8);
  BOOST_CHECK_LE(consumer->status().inFlight, options.aggregateInterestLimit);

  consumer->stop();
  publisher->stop();
}

BOOST_AUTO_TEST_CASE(LiveStreamAdaptiveConsumerPrefetchesNextUnpublishedMappingBlock)
{
  boost::asio::io_context providerIo;
  boost::asio::io_context consumerIo;
  ndn::KeyChain keyChain("pib-memory:live-stream-future-mapping",
                         "tpm-memory:live-stream-future-mapping");
  const ndn::Name provider("/memphis/uav/future-mapping");
  const auto identity = keyChain.createIdentity(provider);
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enablePacketLogging = true;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace providerFace(providerIo, keyChain, faceOptions);
  ndn::DummyClientFace consumerFace(consumerIo, keyChain, faceOptions);
  bool nackedFirstBlockOneInterest = false;
  auto forwardConsumerInterest = consumerFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) {
      const auto& name = interest.getName();
      const bool isBlockOne = name.size() >= 2 &&
        name[name.size() - 2].isVersion() &&
        name[name.size() - 1].isSequenceNumber() &&
        name[name.size() - 1].toSequenceNumber() == 1;
      if (isBlockOne && !nackedFirstBlockOneInterest) {
        nackedFirstBlockOneInterest = true;
        ndn::lp::Nack nack(interest);
        nack.setReason(ndn::lp::NackReason::CONGESTION);
        consumerFace.receive(nack);
      }
      else {
        providerFace.receive(interest);
      }
    });
  auto forwardProviderData = providerFace.onSendData.connect(
    [&] (const ndn::Data& data) { consumerFace.receive(data); });

  LiveStreamDefinition definition;
  definition.contractVersion = STREAM_NAME_MAP_CONTRACT_VERSION_V2;
  definition.streamId = "future-mapping";
  definition.provider = provider;
  definition.semanticDataPrefix = ndn::Name(provider).append("video").appendVersion(1);
  definition.sessionEpoch = 102;
  definition.mappingVersion = 1;
  // Leave four signed tombstones after each 3+1 predicted group. This forces
  // the scheduler to cross the frontier while scanning the same Mapping block,
  // rather than entering schedule() already positioned at the next block.
  definition.mappingBlockCapacity = 8;
  definition.mappingAheadBlocks = 2;
  definition.retainedItems = 16;
  definition.maxPendingInterests = 16;
  definition.samplePeriodMs = 33.0;
  definition.sampleClasses = {
    SampleClassProfile::bounded("delta", 3, 4, 4, 1),
  };
  definition.fec = LiveStreamFecOptions::xorOneRepair(4, 4096, 500);
  auto publisher = std::make_shared<LiveStreamPublisher>(
    definition, providerFace, keyChain,
    ndn::security::signingByCertificate(
      identity.getDefaultKey().getDefaultCertificate()));
  publisher->start();
  providerFace.processEvents(ndn::time::milliseconds(10));
  providerIo.restart();

  const auto nameFactory = [&definition] (uint64_t sampleId) {
    return [&definition, sampleId] (size_t index, LiveStreamItemKind kind) {
      auto name = ndn::Name(definition.semanticDataPrefix)
                    .append("frame").appendSequenceNumber(sampleId);
      name.append(kind == LiveStreamItemKind::Source ? "source" : "repair")
          .appendSegment(index);
      return name;
    };
  };
  const auto first = publisher->announceSample(0, "delta", nameFactory(0));
  publisher->publishSample(first, {{0x01}, {0x02}});
  const auto descriptor = publisher->activate({33.0, first.group.sources.front().cursor});

  std::atomic_uint64_t delivered{0};
  LiveStreamOpenOptions openOptions;
  openOptions.start = LiveStreamStart::Beginning;
  openOptions.prefetchPolicy = LiveStreamPrefetchPolicy::AdaptiveSampleAtomic;
  openOptions.aggregateInterestLimit = 16;
  openOptions.enableFecRecovery = true;
  openOptions.interestLifetimeMs = 500;
  openOptions.onItem = [&] (const VerifiedLiveStreamItem&) {
    ++delivered;
    return LiveStreamItemAdmission::acceptItem();
  };
  auto consumer = std::make_shared<LiveStreamConsumerHandle>(
    descriptor, openOptions, consumerFace,
    std::make_shared<MessageValidator>("examples/trust-any.conf"));
  consumer->start();

  const auto pump = [&] (size_t iterations) {
    for (size_t iteration = 0; iteration < iterations; ++iteration) {
      providerFace.processEvents(ndn::time::milliseconds(2));
      consumerFace.processEvents(ndn::time::milliseconds(2));
      providerIo.restart();
      consumerIo.restart();
    }
  };
  for (size_t iteration = 0; iteration < 400 && delivered.load() < 2; ++iteration) {
    pump(1);
  }
  BOOST_REQUIRE_EQUAL(delivered.load(), 2);
  // Allow the one injected Mapping Nack to be retried. The unrelated
  // speculative block retains enough lifetime to remain pending while this
  // exact block is retried.
  // The congestion Nack above is injected synchronously. Ten pump rounds are
  // enough to process its retry while keeping the observation comfortably
  // before the 100--166 ms Mapping lifetimes under test. Waiting 30 rounds
  // sampled the publisher PIT at its expiry boundary and made this assertion
  // depend on host scheduling rather than prefetch behavior.
  pump(10);
  // pump() handles the Provider before the Consumer. Drain any Provider
  // callback queued by the final Consumer slice without advancing the clock,
  // so the publisher and consumer status snapshots describe the same traffic.
  providerIo.poll();
  providerIo.restart();
  // Keep the complete bounded Mapping lookahead window pending. Fetching only
  // block 1 serializes Mapping -> payload once network delay is non-zero and
  // eventually lets a live consumer fall behind Provider retention.
  BOOST_CHECK_GE(publisher->status().pendingInterests, 2);
  BOOST_CHECK_GE(consumer->status().mappingInterests, 3);
  const auto nextBlockName = makeStreamNameMapBlockName(
    definition.mappingRoot(), definition.mappingVersion, 1);
  std::vector<uint64_t> nextBlockLifetimes;
  for (const auto& interest : consumerFace.sentInterests) {
    if (definition.mappingRoot().isPrefixOf(interest.getName())) {
      // 100 ms initial DRD + one 33 ms missing period + one remaining
      // 33 ms Mapping lead period. The loss-detection attempt must not inherit
      // the caller's 500 ms live-edge-search horizon. Payload Interests retain
      // that longer horizon because they may still be waiting for production.
      BOOST_CHECK_LE(interest.getInterestLifetime().count(), 166);
      if (interest.getName() == nextBlockName) {
        nextBlockLifetimes.push_back(interest.getInterestLifetime().count());
      }
    }
  }
  // Block 1 enters the bounded lookahead once, receives one injected
  // congestion Nack, and is re-expressed once. The retry must retain the
  // measured Mapping loss-detection horizon instead of the old hard-coded
  // 20 ms lifetime that formed a Nack/retry feedback loop.
  BOOST_REQUIRE_GE(nextBlockLifetimes.size(), 2);
  BOOST_CHECK(std::all_of(nextBlockLifetimes.begin(), nextBlockLifetimes.end(),
                          [] (auto lifetime) { return lifetime > 20; }));

  // Block 1 did not exist when the consumer reached cursor 4. Its already
  // pending exact Mapping Interest must be satisfied by this announcement.
  const auto second = publisher->announceSample(1, "delta", nameFactory(1));
  publisher->publishSample(second, {{0x03}, {0x04}});
  for (size_t iteration = 0; iteration < 400 && delivered.load() < 4; ++iteration) {
    pump(1);
  }
  BOOST_CHECK_EQUAL(delivered.load(), 4);
  const auto finalStatus = consumer->status();
  BOOST_CHECK(finalStatus.state == LiveStreamLifecycleState::Active);
  BOOST_CHECK_GE(finalStatus.mappingInterests, 2);
  BOOST_CHECK_EQUAL(finalStatus.rejected, 0);
  consumer->stop();
  publisher->stop();
}

BOOST_AUTO_TEST_CASE(LiveStreamAdaptiveFirstFutureGroupKeepsOneProductionPeriod)
{
  boost::asio::io_context providerIo;
  boost::asio::io_context consumerIo;
  ndn::KeyChain keyChain("pib-memory:live-stream-first-future",
                         "tpm-memory:live-stream-first-future");
  const ndn::Name provider("/memphis/sensor/first-future");
  const auto identity = keyChain.createIdentity(provider);
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enablePacketLogging = true;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace providerFace(providerIo, keyChain, faceOptions);
  ndn::DummyClientFace consumerFace(consumerIo, keyChain, faceOptions);
  auto forwardConsumerInterest = consumerFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) { providerFace.receive(interest); });
  auto forwardProviderData = providerFace.onSendData.connect(
    [&] (const ndn::Data& data) { consumerFace.receive(data); });

  LiveStreamDefinition definition;
  definition.contractVersion = STREAM_NAME_MAP_CONTRACT_VERSION_V2;
  definition.streamId = "first-future";
  definition.provider = provider;
  definition.semanticDataPrefix = ndn::Name(provider).append("sensor").appendVersion(1);
  definition.sessionEpoch = 103;
  definition.mappingVersion = 1;
  definition.mappingBlockCapacity = 1;
  definition.mappingAheadBlocks = 2;
  definition.retainedItems = 16;
  definition.maxPendingInterests = 16;
  definition.samplePeriodMs = 50.0;
  definition.sampleClasses = {
    SampleClassProfile::bounded("state", 1, 1, 4, 0),
  };
  definition.fec = LiveStreamFecOptions::none();
  auto publisher = std::make_shared<LiveStreamPublisher>(
    definition, providerFace, keyChain,
    ndn::security::signingByCertificate(
      identity.getDefaultKey().getDefaultCertificate()));
  publisher->start();
  providerFace.processEvents(ndn::time::milliseconds(10));
  providerIo.restart();

  const auto nameFor = [&definition] (uint64_t sampleId) {
    return [&definition, sampleId] (size_t index, LiveStreamItemKind kind) {
      return ndn::Name(definition.semanticDataPrefix)
        .append("sample").appendSequenceNumber(sampleId)
        .append(kind == LiveStreamItemKind::Source ? "source" : "repair")
        .appendSegment(index);
    };
  };
  const auto first = publisher->announceSample(0, "state", nameFor(0));
  publisher->publishSample(first, {{0x01}});
  const auto descriptor = publisher->activate(
    {definition.samplePeriodMs, first.group.sources.front().cursor});

  std::atomic_uint64_t delivered{0};
  LiveStreamOpenOptions openOptions;
  openOptions.start = LiveStreamStart::Beginning;
  openOptions.prefetchPolicy = LiveStreamPrefetchPolicy::AdaptiveSampleAtomic;
  openOptions.aggregateInterestLimit = 16;
  openOptions.interestLifetimeMs = 500;
  openOptions.onItem = [&] (const VerifiedLiveStreamItem&) {
    ++delivered;
    return LiveStreamItemAdmission::acceptItem();
  };
  auto consumer = std::make_shared<LiveStreamConsumerHandle>(
    descriptor, openOptions, consumerFace,
    std::make_shared<MessageValidator>("examples/trust-any.conf"));
  consumer->start();

  const auto pump = [&] (size_t iterations) {
    for (size_t iteration = 0; iteration < iterations; ++iteration) {
      providerFace.processEvents(ndn::time::milliseconds(2));
      consumerFace.processEvents(ndn::time::milliseconds(2));
      providerIo.restart();
      consumerIo.restart();
    }
  };
  for (size_t iteration = 0; iteration < 400 && delivered.load() < 1; ++iteration) {
    pump(1);
  }
  BOOST_REQUIRE_EQUAL(delivered.load(), 1);

  const auto second = publisher->announceSample(1, "state", nameFor(1));
  for (size_t iteration = 0; iteration < 100; ++iteration) {
    pump(1);
    const auto found = std::find_if(
      consumerFace.sentInterests.rbegin(), consumerFace.sentInterests.rend(),
      [&] (const auto& interest) {
        return interest.getName() == second.group.sources.front().originalName;
      });
    if (found != consumerFace.sentInterests.rend()) break;
  }
  const auto futureInterest = std::find_if(
    consumerFace.sentInterests.rbegin(), consumerFace.sentInterests.rend(),
    [&] (const auto& interest) {
      return interest.getName() == second.group.sources.front().originalName;
    });
  BOOST_REQUIRE(futureInterest != consumerFace.sentInterests.rend());
  BOOST_CHECK_GE(futureInterest->getInterestLifetime().count(), 100);

  // sentInterests is populated before DummyClientFace necessarily delivers its
  // onSendInterest signal to the provider io_context. Wait for the observable
  // provider-side future-Interest admission instead of assuming twelve polling
  // iterations represent one deterministic wall-clock period under load. The
  // lifetime assertion above is the deterministic production-period contract.
  for (size_t iteration = 0;
       iteration < 400 && publisher->status().providerFutureInterests < 1;
       ++iteration) {
    pump(1);
  }
  BOOST_REQUIRE_GE(publisher->status().providerFutureInterests, 1);
  publisher->publishSample(second, {{0x02}});
  for (size_t iteration = 0; iteration < 400 && delivered.load() < 2; ++iteration) {
    pump(1);
  }
  BOOST_CHECK_EQUAL(delivered.load(), 2);
  BOOST_CHECK_GE(publisher->status().providerFutureInterests, 1);
  BOOST_CHECK_GE(publisher->status().providerFutureHits, 1);
  consumer->stop();
  publisher->stop();
}

BOOST_AUTO_TEST_CASE(LiveStreamPublisherCommitsMappingBeforeSemanticDataAndActivatesAtomically)
{
  ndn::KeyChain keyChain("pib-memory:live-stream-publisher",
                         "tpm-memory:live-stream-publisher");
  const ndn::Name provider("/memphis/uav/7");
  const auto identity = keyChain.createIdentity(provider);
  ndn::DummyClientFace::Options options;
  options.enablePacketLogging = true;
  options.enableRegistrationReply = true;
  ndn::DummyClientFace face(keyChain, options);

  LiveStreamDefinition definition;
  definition.streamId = "front-camera";
  definition.provider = provider;
  definition.semanticDataPrefix = ndn::Name(provider).append("video").append("front").append("s9");
  definition.sessionEpoch = 9;
  definition.mappingVersion = 23;
  definition.mappingBlockCapacity = 4;
  definition.retainedItems = 2;
  definition.maxPendingInterests = 4;

  auto publisher = std::make_shared<LiveStreamPublisher>(
    definition, face, keyChain,
    ndn::security::signingByCertificate(
      identity.getDefaultKey().getDefaultCertificate()));
  publisher->start();
  face.processEvents(ndn::time::milliseconds(10));
  face.getIoContext().restart();

  const auto reservation = publisher->reserveAhead(
    ndn::Name(definition.semanticDataPrefix).append("frame").appendSequenceNumber(0));
  BOOST_CHECK_EQUAL(reservation.cursor, 0);
  BOOST_CHECK_EQUAL(publisher->status().mappingBlocks, 1);
  BOOST_CHECK_THROW(publisher->activate({33.0, 0}), std::logic_error);

  const std::vector<uint8_t> opaque{0x00, 0x01, 0xfe, 0xff};
  publisher->publish(reservation, opaque);
  BOOST_CHECK_THROW(publisher->publish(reservation, opaque), std::logic_error);
  const auto descriptor = publisher->activate({33.0, 0});
  BOOST_CHECK(descriptor.validate() == std::nullopt);
  BOOST_CHECK(descriptor.definition.semanticDataPrefix.isPrefixOf(
    reservation.originalName));
  BOOST_CHECK_EQUAL(descriptor.checkpoint.frontiers.latestProduced, 0);
  BOOST_CHECK_EQUAL(descriptor.checkpoint.frontiers.mappingCommittedThrough, 3);
  BOOST_CHECK(publisher->status().state == LiveStreamLifecycleState::Active);

  const auto next = publisher->reserveAhead(
    ndn::Name(definition.semanticDataPrefix).append("frame").appendSequenceNumber(1));
  BOOST_CHECK_EQUAL(next.cursor, 4);
  publisher->publish(next, opaque);
  const auto refreshed = publisher->activate({40.0, next.cursor});
  BOOST_CHECK_EQUAL(refreshed.safeJoinCursor, next.cursor);
  BOOST_CHECK_EQUAL(refreshed.checkpoint.blockNumber, 1);
  BOOST_CHECK_EQUAL(refreshed.checkpoint.frontiers.latestJoin, next.cursor);
  BOOST_CHECK_EQUAL(publisher->status().frontiers.latestJoin, next.cursor);

  publisher->stop();
  publisher->stop();
  BOOST_CHECK(publisher->status().state == LiveStreamLifecycleState::Stopped);
}

BOOST_AUTO_TEST_CASE(LiveStreamConsumerContinuesAcrossVerifiedMappingFrontiers)
{
  boost::asio::io_context providerIo;
  boost::asio::io_context consumerIo;
  ndn::KeyChain keyChain("pib-memory:live-stream-continuity",
                         "tpm-memory:live-stream-continuity");
  const ndn::Name provider("/memphis/uav/continuity");
  const auto identity = keyChain.createIdentity(provider);
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enablePacketLogging = true;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace providerFace(providerIo, keyChain, faceOptions);
  ndn::DummyClientFace consumerFace(consumerIo, keyChain, faceOptions);

  auto forwardConsumerInterest = consumerFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) { providerFace.receive(interest); });
  auto forwardProviderData = providerFace.onSendData.connect(
    [&] (const ndn::Data& data) { consumerFace.receive(data); });

  LiveStreamDefinition definition;
  definition.streamId = "continuity";
  definition.provider = provider;
  definition.semanticDataPrefix = ndn::Name(provider).append("video").append("front")
                                    .appendVersion(1);
  definition.sessionEpoch = 71;
  definition.mappingVersion = 1;
  definition.mappingBlockCapacity = 4;
  definition.mappingAheadBlocks = 2;
  definition.retainedItems = 32;
  definition.maxPendingInterests = 32;

  auto publisher = std::make_shared<LiveStreamPublisher>(
    definition, providerFace, keyChain,
    ndn::security::signingByCertificate(
      identity.getDefaultKey().getDefaultCertificate()));
  publisher->start();
  providerFace.processEvents(ndn::time::milliseconds(10));
  providerIo.restart();

  const auto reserveAndPublish = [&] (uint64_t firstCursor) {
    std::vector<ndn::Name> names;
    for (uint64_t cursor = firstCursor; cursor < firstCursor + 4; ++cursor) {
      names.push_back(ndn::Name(definition.semanticDataPrefix)
                        .append("source").appendSequenceNumber(cursor));
    }
    const auto reservations = publisher->reserveAhead(names);
    BOOST_REQUIRE_EQUAL(reservations.size(), 4);
    for (const auto& reservation : reservations) {
      publisher->publish(reservation,
                         {static_cast<uint8_t>(reservation.cursor)});
    }
  };

  reserveAndPublish(0);
  const auto descriptor = publisher->activate({20.0, 0});
  std::atomic_uint64_t delivered{0};
  LiveStreamOpenOptions openOptions;
  openOptions.start = LiveStreamStart::Beginning;
  openOptions.aggregateInterestLimit = 16;
  // Deliberately larger than the adaptive controller cap. The consumer must
  // apply the current decision, not blindly reuse this legacy fallback value.
  openOptions.interestLifetimeMs = 5000;
  openOptions.onItem = [&] (const VerifiedLiveStreamItem&) {
    ++delivered;
    return LiveStreamItemAdmission::acceptItem();
  };
  auto consumer = std::make_shared<LiveStreamConsumerHandle>(
    descriptor, openOptions, consumerFace,
    std::make_shared<MessageValidator>("examples/trust-any.conf"));
  consumer->start();

  // Capture-to-receive is application latency, not the paper's DRD. Feeding a
  // deliberately huge value through the public observation must not inflate
  // the Core Interest demand; payload Interest/Data timing is measured inside
  // LiveStreamConsumerHandle instead.
  BOOST_REQUIRE(consumer->observeAcceptedSample({1, 100, 5000.0, 1}));
  BOOST_CHECK_LT(consumer->status().fetchDecision->packetDemand, 20);

  bool callbackThrew = false;
  std::string callbackError;
  const auto pumpUntil = [&] (uint64_t target) {
    for (size_t iteration = 0; iteration < 400 && delivered.load() < target; ++iteration) {
      try {
        providerFace.processEvents(ndn::time::milliseconds(2));
        consumerFace.processEvents(ndn::time::milliseconds(2));
      }
      catch (const std::exception& error) {
        callbackThrew = true;
        callbackError = error.what();
        break;
      }
      providerIo.restart();
      consumerIo.restart();
    }
  };

  pumpUntil(4);
  if (delivered.load() != 4) {
    BOOST_TEST_MESSAGE("continuity status=" <<
      toString(consumer->status().state) << " reason=" << consumer->status().reason <<
      " timeouts=" << consumer->status().timeouts <<
      " retries=" << consumer->status().retryAttempts <<
      " payload=" << consumer->status().payloadInterests <<
      " payload-retry=" << consumer->status().retryPayloadInterests <<
      " mapping=" << consumer->status().mappingInterests);
  }
  BOOST_REQUIRE_EQUAL(delivered.load(), 4);
  for (const auto& interest : consumerFace.sentInterests) {
    if (definition.semanticDataPrefix.isPrefixOf(interest.getName())) {
      BOOST_CHECK_LE(interest.getInterestLifetime().count(), 2000);
    }
  }
  reserveAndPublish(4);
  pumpUntil(8);
  reserveAndPublish(8);
  pumpUntil(12);

  BOOST_CHECK(!callbackThrew);
  if (!callbackError.empty()) {
    BOOST_TEST_MESSAGE("live continuity callback error=" << callbackError);
  }
  BOOST_CHECK_EQUAL(delivered.load(), 12);
  BOOST_CHECK(consumer->status().state == LiveStreamLifecycleState::Active);
  BOOST_CHECK_GE(consumer->status().frontiers.nextReserved, 12);
  consumer->stop();
  publisher->stop();
}

BOOST_AUTO_TEST_CASE(LiveStreamPublisherGroupFailureLeavesNoPartialMaterialization)
{
  ndn::KeyChain keyChain("pib-memory:live-stream-group",
                         "tpm-memory:live-stream-group");
  const ndn::Name provider("/memphis/uav/7");
  const auto identity = keyChain.createIdentity(provider);
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace face(keyChain, faceOptions);

  LiveStreamDefinition definition;
  definition.streamId = "front-camera";
  definition.provider = provider;
  definition.semanticDataPrefix = ndn::Name(provider).append("video").append("group");
  definition.sessionEpoch = 10;
  definition.mappingVersion = 24;
  definition.mappingBlockCapacity = 4;
  definition.retainedItems = 8;
  definition.signedWireCap = 4096;
  definition.fec = LiveStreamFecOptions::xorOneRepair(3, 8192, 500);
  auto publisher = std::make_shared<LiveStreamPublisher>(
    definition, face, keyChain,
    ndn::security::signingByCertificate(
      identity.getDefaultKey().getDefaultCertificate()));
  publisher->start();
  face.processEvents(ndn::time::milliseconds(10));
  face.getIoContext().restart();

  const auto group = publisher->reserveGroup("frame-0", {
    ndn::Name(definition.semanticDataPrefix).append("frame").appendSequenceNumber(0).appendSegment(0),
    ndn::Name(definition.semanticDataPrefix).append("frame").appendSequenceNumber(0).appendSegment(1),
    ndn::Name(definition.semanticDataPrefix).append("frame").appendSequenceNumber(0).appendSegment(2),
  }, {
    ndn::Name(definition.semanticDataPrefix).append("frame").appendSequenceNumber(0)
      .append("repair").appendSegment(0),
  });
  std::vector<std::vector<uint8_t>> oversized{{1, 2}, {3, 4},
                                               std::vector<uint8_t>(5000, 5)};
  BOOST_CHECK_THROW(publisher->publishGroup(group, oversized), std::length_error);

  const std::vector<std::vector<uint8_t>> valid{{1, 2}, {3, 4}, {5, 6}};
  BOOST_CHECK_NO_THROW(publisher->publishGroup(group, valid));
  BOOST_CHECK_EQUAL(publisher->status().frontiers.latestProduced, 3);
}

BOOST_AUTO_TEST_CASE(LiveStreamPublisherBoundsExpiresAndPrioritizesPendingInterests)
{
  ndn::KeyChain keyChain("pib-memory:live-stream-pending",
                         "tpm-memory:live-stream-pending");
  const ndn::Name provider("/memphis/uav/7");
  const auto identity = keyChain.createIdentity(provider);
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enablePacketLogging = true;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace face(keyChain, faceOptions);

  LiveStreamDefinition definition;
  definition.streamId = "pending-bounds";
  definition.provider = provider;
  definition.semanticDataPrefix = ndn::Name(provider).append("video").append("pending");
  definition.sessionEpoch = 11;
  definition.mappingVersion = 25;
  definition.mappingBlockCapacity = 4;
  definition.mappingAheadBlocks = 2;
  definition.retainedItems = 16;
  definition.maxPendingInterests = 2;
  auto publisher = std::make_shared<LiveStreamPublisher>(
    definition, face, keyChain,
    ndn::security::signingByCertificate(
      identity.getDefaultKey().getDefaultCertificate()));
  publisher->start();
  face.processEvents(ndn::time::milliseconds(10));
  face.getIoContext().restart();

  ndn::Interest predictedMap(makeStreamNameMapBlockName(
    definition.mappingRoot(), definition.mappingVersion, 0));
  predictedMap.setInterestLifetime(ndn::time::milliseconds(100));
  face.receive(predictedMap);
  face.processEvents(ndn::time::milliseconds(2));
  face.getIoContext().restart();
  BOOST_CHECK_EQUAL(publisher->status().pendingInterests, 1);

  std::vector<ndn::Name> names;
  for (uint64_t cursor = 0; cursor < 8; ++cursor) {
    names.push_back(ndn::Name(definition.semanticDataPrefix)
                      .append("sample").appendSequenceNumber(cursor));
  }
  const auto reservations = publisher->reserveAhead(names);
  BOOST_CHECK_EQUAL(publisher->status().pendingInterests, 0);

  const auto receivePayload = [&face] (const ndn::Name& name, uint64_t lifetimeMs) {
    ndn::Interest interest(name);
    interest.setInterestLifetime(ndn::time::milliseconds(lifetimeMs));
    face.receive(interest);
    face.processEvents(ndn::time::milliseconds(2));
    face.getIoContext().restart();
  };
  receivePayload(reservations[7].originalName, 500);
  receivePayload(reservations[6].originalName, 500);
  BOOST_CHECK_EQUAL(publisher->status().pendingInterests, 2);

  receivePayload(ndn::Name(definition.semanticDataPrefix).append("unmapped"), 100);
  BOOST_CHECK_EQUAL(publisher->status().pendingInterests, 2);

  // A nearer mapped cursor evicts the farthest future entry at the independent
  // payload cap instead of being starved by attacker-selected far names.
  receivePayload(reservations[0].originalName, 100);
  BOOST_CHECK_EQUAL(publisher->status().pendingInterests, 2);
  publisher->publish(reservations[0], {0x01});
  BOOST_CHECK_EQUAL(publisher->status().pendingInterests, 1);
  BOOST_CHECK_EQUAL(publisher->status().providerFutureHits, 1);

  publisher->publish(reservations[6], {0x06});
  BOOST_CHECK_EQUAL(publisher->status().pendingInterests, 0);
  BOOST_CHECK_EQUAL(publisher->status().providerFutureHits, 2);

  receivePayload(reservations[5].originalName, 25);
  BOOST_CHECK_EQUAL(publisher->status().pendingInterests, 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  publisher->publish(reservations[5], {0x05});
  BOOST_CHECK_EQUAL(publisher->status().pendingInterests, 0);
  BOOST_CHECK_EQUAL(publisher->status().providerFutureHits, 2);
  publisher->stop();
}

BOOST_AUTO_TEST_CASE(LiveStreamPublisherNeverReclassifiesEvictedPayloadAsFuture)
{
  ndn::KeyChain keyChain("pib-memory:live-stream-evicted",
                         "tpm-memory:live-stream-evicted");
  const ndn::Name provider("/memphis/uav/evicted");
  const auto identity = keyChain.createIdentity(provider);
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enablePacketLogging = true;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace face(keyChain, faceOptions);

  LiveStreamDefinition definition;
  definition.streamId = "evicted-is-not-future";
  definition.provider = provider;
  definition.semanticDataPrefix = ndn::Name(provider).append("video").append("evicted");
  definition.sessionEpoch = 12;
  definition.mappingVersion = 26;
  definition.mappingBlockCapacity = 4;
  definition.retainedItems = 2;
  definition.maxPendingInterests = 4;
  auto publisher = std::make_shared<LiveStreamPublisher>(
    definition, face, keyChain,
    ndn::security::signingByCertificate(
      identity.getDefaultKey().getDefaultCertificate()));
  publisher->start();
  face.processEvents(ndn::time::milliseconds(10));
  face.getIoContext().restart();

  std::vector<ndn::Name> names;
  for (uint64_t cursor = 0; cursor < 4; ++cursor) {
    names.push_back(ndn::Name(definition.semanticDataPrefix)
                      .append("sample").appendSequenceNumber(cursor));
  }
  const auto reservations = publisher->reserveAhead(names);
  publisher->publish(reservations[0], {0x00});
  publisher->publish(reservations[1], {0x01});
  publisher->publish(reservations[2], {0x02}); // cursor 0 is now evicted

  ndn::Interest evicted(reservations[0].originalName);
  evicted.setInterestLifetime(ndn::time::milliseconds(500));
  face.receive(evicted);
  face.processEvents(ndn::time::milliseconds(2));
  face.getIoContext().restart();
  BOOST_CHECK_EQUAL(publisher->status().pendingInterests, 0);
  BOOST_CHECK_EQUAL(publisher->status().providerFutureInterests, 0);

  ndn::Interest genuineFuture(reservations[3].originalName);
  genuineFuture.setInterestLifetime(ndn::time::milliseconds(500));
  face.receive(genuineFuture);
  face.processEvents(ndn::time::milliseconds(2));
  face.getIoContext().restart();
  BOOST_CHECK_EQUAL(publisher->status().pendingInterests, 1);
  BOOST_CHECK_EQUAL(publisher->status().providerFutureInterests, 1);
  publisher->publish(reservations[3], {0x03});
  BOOST_CHECK_EQUAL(publisher->status().pendingInterests, 0);
  BOOST_CHECK_EQUAL(publisher->status().providerFutureHits, 1);
  publisher->stop();
}

BOOST_AUTO_TEST_CASE(LiveStreamPublishedPacketFeedPreservesExactWiresAndBounds)
{
  ndn::KeyChain keyChain("pib-memory:live-stream-feed", "tpm-memory:live-stream-feed");
  const ndn::Name provider("/memphis/uav/feed");
  const auto identity = keyChain.createIdentity(provider);
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace face(keyChain, faceOptions);

  LiveStreamDefinition definition;
  definition.streamId = "canonical-feed";
  definition.provider = provider;
  definition.semanticDataPrefix = ndn::Name(provider).append("video/front/session-1");
  definition.sessionEpoch = 31;
  definition.mappingVersion = 41;
  definition.mappingBlockCapacity = 2;
  definition.retainedItems = 8;
  auto publisher = std::make_shared<LiveStreamPublisher>(
    definition, face, keyChain,
    ndn::security::signingByCertificate(identity.getDefaultKey().getDefaultCertificate()));
  publisher->start();
  face.processEvents(ndn::time::milliseconds(10));
  face.getIoContext().restart();

  PublishedPacketFeedOptions feedOptions;
  feedOptions.maxQueuedPackets = 8;
  feedOptions.maxQueuedBytes = 32768;
  const auto feed = publisher->openPublishedPacketFeed(feedOptions);
  const auto first = publisher->reserveAhead(
    ndn::Name(definition.semanticDataPrefix).append("frame").appendSequenceNumber(0));
  publisher->publish(first, {0x00, 0x01, 0xfe, 0xff});

  auto records = feed->takeAvailable(8);
  BOOST_REQUIRE_EQUAL(records.size(), 2);
  BOOST_CHECK(records[0].kind == PublishedLiveStreamPacketKind::Mapping);
  BOOST_CHECK(records[1].kind == PublishedLiveStreamPacketKind::Source);
  BOOST_REQUIRE(records[1].cursor.has_value());
  BOOST_CHECK_EQUAL(*records[1].cursor, first.cursor);
  BOOST_CHECK(records[1].dataName == first.originalName);
  BOOST_CHECK(records[1].provider == provider);
  BOOST_CHECK(!records[1].signedDataWire.empty());
  BOOST_CHECK(!std::all_of(records[1].wireDigest.begin(), records[1].wireDigest.end(),
                           [] (uint8_t value) { return value == 0; }));

  ndn::Data decoded;
  decoded.wireDecode(ndn::Block(records[1].signedDataWire));
  BOOST_CHECK(decoded.getName() == first.originalName);
  BOOST_CHECK_EQUAL(decoded.getContent().value_size(), 4);

  const auto snapshot = publisher->openPublishedPacketFeed(feedOptions);
  auto snapshotRecords = snapshot->takeAvailable(8);
  BOOST_REQUIRE_EQUAL(snapshotRecords.size(), 2);
  BOOST_CHECK(snapshotRecords[1].signedDataWire == records[1].signedDataWire);
  BOOST_CHECK(snapshotRecords[1].wireDigest == records[1].wireDigest);

  PublishedPacketFeedOptions boundedOptions;
  boundedOptions.maxQueuedPackets = 1;
  boundedOptions.maxQueuedBytes = 32768;
  const auto bounded = publisher->openPublishedPacketFeed(boundedOptions);
  const auto boundedStatus = bounded->status();
  BOOST_CHECK_EQUAL(boundedStatus.queuedPackets, 1);
  BOOST_CHECK_GE(boundedStatus.droppedPackets, 1);
  bounded->close();
  BOOST_CHECK(bounded->status().closed);
  BOOST_CHECK_EQUAL(bounded->takeAvailable(1).size(), 1);
  BOOST_CHECK(bounded->takeAvailable(1).empty());
  publisher->stop();
}

BOOST_AUTO_TEST_CASE(LiveStreamMappingLeadUsesMeasuredTimingAndBounds)
{
  BOOST_CHECK_EQUAL(computeLiveStreamMappingLead(120.0, 20.0, 20.0, 4, 64), 8);
  // A 30-fps sample containing 13 Data packets has a 33/13 ms item period.
  // The ahead Mapping must cover packet demand, not merely frame demand.
  BOOST_CHECK_EQUAL(computeLiveStreamMappingLead(
    120.0, 33.0 / 13.0, 20.0, 16, 1024), 57);
  BOOST_CHECK_EQUAL(computeLiveStreamMappingLead(0.0, 20.0, 0.0, 4, 64), 4);
  BOOST_CHECK_EQUAL(computeLiveStreamMappingLead(5000.0, 1.0, 1000.0, 4, 64), 64);
  BOOST_CHECK_THROW(computeLiveStreamMappingLead(120.0, 0.0, 20.0, 4, 64),
                    std::invalid_argument);
  BOOST_CHECK_THROW(computeLiveStreamMappingLead(120.0, 20.0, 20.0, 0, 64),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(StoredSignedPacketProducerPreservesWireAndRejectsConflicts)
{
  ndn::KeyChain keyChain("pib-memory:stored-live-stream",
                         "tpm-memory:stored-live-stream");
  const auto identity = keyChain.createIdentity(ndn::Name("/uav/drone/A"));
  ndn::DummyClientFace face(keyChain);
  ndn::Data packet(ndn::Name("/uav/drone/A/video/front/s/source/0"));
  packet.setContent(ndn::span<const uint8_t>(
    reinterpret_cast<const uint8_t*>("h264"), 4));
  keyChain.sign(packet, ndn::security::signingByCertificate(
    identity.getDefaultKey().getDefaultCertificate()));
  const auto wire = packet.wireEncode();
  ndn::Buffer exact(wire.begin(), wire.end());

  StoredSignedPacketProducer producer(
    face, ndn::Name("/uav/drone/A"), {exact});
  BOOST_CHECK_EQUAL(producer.packetCount(), 1);
  BOOST_CHECK_THROW(StoredSignedPacketProducer(
    face, ndn::Name("/other/provider"), {exact}), std::invalid_argument);

  ndn::Data conflicting(packet.getName());
  conflicting.setContent(ndn::span<const uint8_t>(
    reinterpret_cast<const uint8_t*>("evil"), 4));
  keyChain.sign(conflicting, ndn::security::signingByCertificate(
    identity.getDefaultKey().getDefaultCertificate()));
  const auto conflictingWire = conflicting.wireEncode();
  BOOST_CHECK_THROW(StoredSignedPacketProducer(
    face, ndn::Name("/uav/drone/A"),
    {exact, ndn::Buffer(conflictingWire.begin(), conflictingWire.end())}),
    std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
