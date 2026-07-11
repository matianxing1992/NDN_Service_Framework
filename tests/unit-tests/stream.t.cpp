/* -*- Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil -*- */

#include "ndn-service-framework/Stream.hpp"
#include "tests/boost-test.hpp"

namespace ndn_service_framework::test {

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
  BOOST_REQUIRE_EQUAL(buffer.pendingSequences().size(), 2);
  BOOST_CHECK_EQUAL(buffer.pendingSequences()[0], 3);
  buffer.skipTo(3);
  const auto drained = buffer.drainReady();
  BOOST_REQUIRE_EQUAL(drained.size(), 2);
  BOOST_CHECK_EQUAL(drained[0].seq, 3);
  BOOST_CHECK_EQUAL(drained[1].seq, 4);
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

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
