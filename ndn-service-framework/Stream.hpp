#ifndef NDN_SERVICE_FRAMEWORK_STREAM_HPP
#define NDN_SERVICE_FRAMEWORK_STREAM_HPP

#include "common.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ndn_service_framework {

namespace stream_tlv {
enum {
  StreamInfoType = 0xF610,
  StreamChunkType = 0xF611,
  StreamFecInfoType = 0xF612,
  StreamIdType = 0xF613,
  StreamSessionEpochType = 0xF614,
  StreamPrefixType = 0xF615,
  StreamSequenceType = 0xF616,
  StreamContentTypeType = 0xF617,
  StreamFreshnessMsType = 0xF618,
  StreamMaxPayloadBytesType = 0xF619,
  StreamWindowType = 0xF61A,
  StreamLookaheadType = 0xF61B,
  StreamInterestLifetimeMsType = 0xF61C,
  StreamMissingTimeoutMsType = 0xF61D,
  StreamReliabilityType = 0xF61E,
  StreamCreatedMsType = 0xF61F,
  StreamCaptureMsType = 0xF620,
  StreamArrivalMsType = 0xF621,
  StreamDeadlineMsType = 0xF622,
  StreamKeyChunkType = 0xF623,
  StreamFrameIdType = 0xF624,
  StreamFrameFirstSeqType = 0xF625,
  StreamFrameLastSeqType = 0xF626,
  StreamSegmentIndexType = 0xF627,
  StreamSegmentCountType = 0xF628,
  StreamFecSchemeType = 0xF629,
  StreamFecDataShardsType = 0xF62A,
  StreamFecParityShardsType = 0xF62B,
  StreamFecSymbolIndexType = 0xF62C,
  StreamFecSymbolCountType = 0xF62D,
  StreamFecDataLengthType = 0xF62E,
  StreamFecSourceBlockIdType = 0xF62F,
  StreamFecRepairSymbolType = 0xF630,
  StreamMetadataType = 0xF631,
  StreamPayloadType = 0xF632,
};
} // namespace stream_tlv

uint64_t streamNowMs();

struct StreamFecInfo
{
  std::string scheme;
  uint64_t dataShards = 0;
  uint64_t parityShards = 0;
  uint64_t symbolIndex = 0;
  uint64_t symbolCount = 0;
  std::vector<uint64_t> dataLengths;
  std::string sourceBlockId;
  bool repairSymbol = false;
  std::map<std::string, std::string> metadata;

  bool enabled() const;
  ndn::Block wireEncode() const;
  bool wireDecode(const ndn::Block& block);
};

/**
 * Metadata for a continuous or near-live sequence of named Data packets.
 *
 * Use StreamInfo/StreamChunk for data that evolves over time and benefits
 * from stream sequence, freshness, gap, duplicate, reorder, or FEC metadata:
 * video frames, telemetry, logs, and similar live feeds.
 *
 * This is not the right abstraction for large static objects such as files,
 * model artifacts, catalog snapshots, or planned DI tensor bundles. Those
 * objects already have exact NDN names and should use the large-data path:
 * CollaborationContext::publishLarge(), publishLargeNamed(), and fetchLarge(),
 * which are backed by segmented Data / SegmentFetcher-style retrieval.
 */
struct StreamInfo
{
  std::string streamId;
  uint64_t sessionEpoch = 0;
  ndn::Name streamPrefix;
  uint64_t nextSeq = 0;
  std::string contentType = "application/octet-stream";
  uint64_t freshnessMs = 80;
  uint64_t maxPayloadBytes = 3600;
  uint64_t window = 32;
  uint64_t lookahead = 8;
  uint64_t interestLifetimeMs = 500;
  uint64_t missingTimeoutMs = 300;
  std::string reliability = "best-effort";
  uint64_t createdMs = 0;
  std::map<std::string, std::string> metadata;

  ndn::Name chunkName(uint64_t seq) const;
  ndn::Block wireEncode() const;
  bool wireDecode(const ndn::Block& block);
};

/**
 * One chunk in a continuous or near-live StreamInfo sequence.
 *
 * A StreamChunk may carry arbitrary payload bytes, but the surrounding
 * metadata assumes a stream sequence. For exact-name large-object transfer,
 * prefer CollaborationContext::publishLarge(), publishLargeNamed(), and
 * fetchLarge() instead of wrapping the object as a stream.
 */
struct StreamChunk
{
  std::string streamId;
  uint64_t sessionEpoch = 0;
  uint64_t seq = 0;
  std::vector<uint8_t> payload;
  std::string contentType = "application/octet-stream";
  uint64_t captureMs = 0;
  uint64_t arrivalMs = 0;
  uint64_t deadlineMs = 0;
  bool keyChunk = false;
  uint64_t frameId = 0;
  uint64_t frameFirstSeq = 0;
  uint64_t frameLastSeq = 0;
  uint64_t segmentIndex = 0;
  uint64_t segmentCount = 1;
  std::optional<StreamFecInfo> fec;
  std::map<std::string, std::string> metadata;

  ndn::Block wireEncode() const;
  bool wireDecode(const ndn::Block& block);
};

struct StreamMetrics
{
  uint64_t produced = 0;
  uint64_t evicted = 0;
  uint64_t received = 0;
  uint64_t emitted = 0;
  uint64_t duplicates = 0;
  uint64_t stale = 0;
  uint64_t gaps = 0;
  uint64_t timeouts = 0;
  uint64_t nacks = 0;
  uint64_t bytesProduced = 0;
  uint64_t bytesReceived = 0;
};

class StreamProducerBuffer
{
public:
  explicit StreamProducerBuffer(size_t maxChunks = 600);

  void put(const StreamChunk& chunk);
  std::optional<StreamChunk> get(uint64_t seq) const;
  std::optional<ndn::Block> getEncoded(uint64_t seq) const;
  std::vector<uint64_t> sequences() const;
  size_t size() const;
  const StreamMetrics& metrics() const;

private:
  size_t m_maxChunks;
  std::map<uint64_t, StreamChunk> m_chunks;
  std::deque<uint64_t> m_order;
  StreamMetrics m_metrics;
};

class StreamConsumerReorderBuffer
{
public:
  StreamConsumerReorderBuffer(std::string streamId,
                              uint64_t sessionEpoch,
                              uint64_t nextSeq = 0,
                              size_t maxPending = 512,
                              size_t history = 1024);

  void reset(std::string streamId, uint64_t sessionEpoch, uint64_t nextSeq = 0);
  std::vector<StreamChunk> push(const StreamChunk& chunk);
  std::vector<uint64_t> missingSequences(size_t limit = 32) const;
  void skipTo(uint64_t seq);
  uint64_t nextSeq() const;
  const StreamMetrics& metrics() const;

private:
  void markCompleted(uint64_t seq);
  void dropOldestPending();

private:
  std::string m_streamId;
  uint64_t m_sessionEpoch = 0;
  uint64_t m_nextSeq = 0;
  size_t m_maxPending = 512;
  size_t m_history = 1024;
  std::map<uint64_t, StreamChunk> m_pending;
  std::set<uint64_t> m_completed;
  std::deque<uint64_t> m_completedOrder;
  StreamMetrics m_metrics;
};

struct StreamFetchDecision
{
  uint64_t window = 0;
  uint64_t lookahead = 0;
  uint64_t interestLifetimeMs = 0;
  uint64_t missingTimeoutMs = 0;
  double pressure = 0.0;
  std::string reason = "stable";
};

class StreamAdaptiveFetcherState
{
public:
  double rttMs = 100.0;
  double timeoutPressure = 0.0;
  double nackPressure = 0.0;
  double duplicatePressure = 0.0;
  double backlogPressure = 0.0;
  uint64_t minWindow = 4;
  uint64_t baseWindow = 32;
  uint64_t maxWindow = 256;
  uint64_t minLookahead = 2;
  uint64_t baseLookahead = 8;
  uint64_t maxLookahead = 128;
  uint64_t minInterestLifetimeMs = 100;
  uint64_t maxInterestLifetimeMs = 2000;
  uint64_t minMissingTimeoutMs = 80;
  uint64_t maxMissingTimeoutMs = 1500;

  void observeRtt(double sampleMs, double alpha = 0.25);
  void recordTimeout();
  void recordNack();
  void recordDuplicate();
  void setBacklogPressure(double pressure);
  void decay(double factor = 0.85);
  StreamFetchDecision decide() const;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_STREAM_HPP
