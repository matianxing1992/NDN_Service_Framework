#include "Stream.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>

namespace ndn_service_framework {

namespace {

void
appendString(ndn::Block& block, uint32_t type, const std::string& value)
{
  if (!value.empty()) {
    block.push_back(ndn::makeStringBlock(type, value));
  }
}

void
appendName(ndn::Block& block, uint32_t type, const ndn::Name& value)
{
  if (!value.empty()) {
    block.push_back(ndn::makeStringBlock(type, value.toUri()));
  }
}

void
appendNumber(ndn::Block& block, uint32_t type, uint64_t value)
{
  if (value > 0) {
    block.push_back(ndn::makeNonNegativeIntegerBlock(type, value));
  }
}

void
appendBool(ndn::Block& block, uint32_t type, bool value)
{
  if (value) {
    block.push_back(ndn::makeNonNegativeIntegerBlock(type, 1));
  }
}

void
appendMetadata(ndn::Block& block, const std::map<std::string, std::string>& metadata)
{
  for (const auto& item : metadata) {
    block.push_back(ndn::makeStringBlock(stream_tlv::StreamMetadataType,
                                         item.first + "=" + item.second));
  }
}

void
readMetadata(const ndn::Block& element, std::map<std::string, std::string>& metadata)
{
  const auto text = ndn::readString(element);
  const auto pos = text.find('=');
  if (pos == std::string::npos) {
    metadata[text] = "";
    return;
  }
  metadata[text.substr(0, pos)] = text.substr(pos + 1);
}

std::vector<uint8_t>
readBinary(const ndn::Block& element)
{
  return {element.value(), element.value() + element.value_size()};
}

} // namespace

const char*
toString(StreamHealthState state)
{
  switch (state) {
  case StreamHealthState::Active:
    return "ACTIVE";
  case StreamHealthState::Degraded:
    return "DEGRADED";
  case StreamHealthState::Congested:
    return "CONGESTED";
  case StreamHealthState::Stale:
    return "STALE";
  case StreamHealthState::Stopped:
    return "STOPPED";
  }
  return "UNKNOWN";
}

uint64_t
streamNowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

bool
StreamFecInfo::enabled() const
{
  return dataShards > 0 || parityShards > 0 || symbolCount > 0;
}

ndn::Block
StreamFecInfo::wireEncode() const
{
  ndn::Block block(stream_tlv::StreamFecInfoType);
  appendString(block, stream_tlv::StreamFecSchemeType, scheme);
  appendNumber(block, stream_tlv::StreamFecDataShardsType, dataShards);
  appendNumber(block, stream_tlv::StreamFecParityShardsType, parityShards);
  appendNumber(block, stream_tlv::StreamFecSymbolIndexType, symbolIndex);
  appendNumber(block, stream_tlv::StreamFecSymbolCountType, symbolCount);
  for (auto length : dataLengths) {
    block.push_back(ndn::makeNonNegativeIntegerBlock(stream_tlv::StreamFecDataLengthType,
                                                     length));
  }
  appendString(block, stream_tlv::StreamFecSourceBlockIdType, sourceBlockId);
  appendBool(block, stream_tlv::StreamFecRepairSymbolType, repairSymbol);
  appendMetadata(block, metadata);
  block.encode();
  return block;
}

bool
StreamFecInfo::wireDecode(const ndn::Block& block)
{
  if (block.type() != stream_tlv::StreamFecInfoType) {
    return false;
  }
  *this = StreamFecInfo{};
  block.parse();
  for (const auto& element : block.elements()) {
    switch (element.type()) {
    case stream_tlv::StreamFecSchemeType:
      scheme = ndn::readString(element);
      break;
    case stream_tlv::StreamFecDataShardsType:
      dataShards = ndn::readNonNegativeInteger(element);
      break;
    case stream_tlv::StreamFecParityShardsType:
      parityShards = ndn::readNonNegativeInteger(element);
      break;
    case stream_tlv::StreamFecSymbolIndexType:
      symbolIndex = ndn::readNonNegativeInteger(element);
      break;
    case stream_tlv::StreamFecSymbolCountType:
      symbolCount = ndn::readNonNegativeInteger(element);
      break;
    case stream_tlv::StreamFecDataLengthType:
      dataLengths.push_back(ndn::readNonNegativeInteger(element));
      break;
    case stream_tlv::StreamFecSourceBlockIdType:
      sourceBlockId = ndn::readString(element);
      break;
    case stream_tlv::StreamFecRepairSymbolType:
      repairSymbol = ndn::readNonNegativeInteger(element) > 0;
      break;
    case stream_tlv::StreamMetadataType:
      readMetadata(element, metadata);
      break;
    default:
      break;
    }
  }
  return true;
}

ndn::Name
StreamInfo::chunkName(uint64_t seq) const
{
  ndn::Name name(streamPrefix);
  name.appendNumber(seq);
  return name;
}

ndn::Block
StreamInfo::wireEncode() const
{
  ndn::Block block(stream_tlv::StreamInfoType);
  appendString(block, stream_tlv::StreamIdType, streamId);
  appendNumber(block, stream_tlv::StreamSessionEpochType, sessionEpoch);
  appendName(block, stream_tlv::StreamPrefixType, streamPrefix);
  appendNumber(block, stream_tlv::StreamSequenceType, nextSeq);
  appendString(block, stream_tlv::StreamContentTypeType, contentType);
  appendNumber(block, stream_tlv::StreamFreshnessMsType, freshnessMs);
  appendNumber(block, stream_tlv::StreamMaxPayloadBytesType, maxPayloadBytes);
  appendNumber(block, stream_tlv::StreamWindowType, window);
  appendNumber(block, stream_tlv::StreamLookaheadType, lookahead);
  appendNumber(block, stream_tlv::StreamInterestLifetimeMsType, interestLifetimeMs);
  appendNumber(block, stream_tlv::StreamMissingTimeoutMsType, missingTimeoutMs);
  appendString(block, stream_tlv::StreamReliabilityType, reliability);
  appendNumber(block, stream_tlv::StreamCreatedMsType, createdMs);
  appendMetadata(block, metadata);
  block.encode();
  return block;
}

bool
StreamInfo::wireDecode(const ndn::Block& block)
{
  if (block.type() != stream_tlv::StreamInfoType) {
    return false;
  }
  *this = StreamInfo{};
  block.parse();
  for (const auto& element : block.elements()) {
    switch (element.type()) {
    case stream_tlv::StreamIdType:
      streamId = ndn::readString(element);
      break;
    case stream_tlv::StreamSessionEpochType:
      sessionEpoch = ndn::readNonNegativeInteger(element);
      break;
    case stream_tlv::StreamPrefixType:
      streamPrefix = ndn::Name(ndn::readString(element));
      break;
    case stream_tlv::StreamSequenceType:
      nextSeq = ndn::readNonNegativeInteger(element);
      break;
    case stream_tlv::StreamContentTypeType:
      contentType = ndn::readString(element);
      break;
    case stream_tlv::StreamFreshnessMsType:
      freshnessMs = ndn::readNonNegativeInteger(element);
      break;
    case stream_tlv::StreamMaxPayloadBytesType:
      maxPayloadBytes = ndn::readNonNegativeInteger(element);
      break;
    case stream_tlv::StreamWindowType:
      window = ndn::readNonNegativeInteger(element);
      break;
    case stream_tlv::StreamLookaheadType:
      lookahead = ndn::readNonNegativeInteger(element);
      break;
    case stream_tlv::StreamInterestLifetimeMsType:
      interestLifetimeMs = ndn::readNonNegativeInteger(element);
      break;
    case stream_tlv::StreamMissingTimeoutMsType:
      missingTimeoutMs = ndn::readNonNegativeInteger(element);
      break;
    case stream_tlv::StreamReliabilityType:
      reliability = ndn::readString(element);
      break;
    case stream_tlv::StreamCreatedMsType:
      createdMs = ndn::readNonNegativeInteger(element);
      break;
    case stream_tlv::StreamMetadataType:
      readMetadata(element, metadata);
      break;
    default:
      break;
    }
  }
  return true;
}

ndn::Block
StreamChunk::wireEncode() const
{
  ndn::Block block(stream_tlv::StreamChunkType);
  appendString(block, stream_tlv::StreamIdType, streamId);
  appendNumber(block, stream_tlv::StreamSessionEpochType, sessionEpoch);
  block.push_back(ndn::makeNonNegativeIntegerBlock(stream_tlv::StreamSequenceType, seq));
  appendString(block, stream_tlv::StreamContentTypeType, contentType);
  appendNumber(block, stream_tlv::StreamCaptureMsType, captureMs);
  appendNumber(block, stream_tlv::StreamArrivalMsType, arrivalMs);
  appendNumber(block, stream_tlv::StreamDeadlineMsType, deadlineMs);
  appendBool(block, stream_tlv::StreamKeyChunkType, keyChunk);
  appendNumber(block, stream_tlv::StreamFrameIdType, frameId);
  appendNumber(block, stream_tlv::StreamFrameFirstSeqType, frameFirstSeq);
  appendNumber(block, stream_tlv::StreamFrameLastSeqType, frameLastSeq);
  appendNumber(block, stream_tlv::StreamSegmentIndexType, segmentIndex);
  appendNumber(block, stream_tlv::StreamSegmentCountType, segmentCount);
  if (fec) {
    block.push_back(fec->wireEncode());
  }
  appendMetadata(block, metadata);
  if (!payload.empty()) {
    block.push_back(ndn::makeBinaryBlock(stream_tlv::StreamPayloadType,
                                         payload.data(),
                                         payload.data() + payload.size()));
  }
  else {
    ndn::Block payloadBlock(stream_tlv::StreamPayloadType);
    payloadBlock.encode();
    block.push_back(payloadBlock);
  }
  block.encode();
  return block;
}

bool
StreamChunk::wireDecode(const ndn::Block& block)
{
  if (block.type() != stream_tlv::StreamChunkType) {
    return false;
  }
  *this = StreamChunk{};
  block.parse();
  for (const auto& element : block.elements()) {
    switch (element.type()) {
    case stream_tlv::StreamIdType:
      streamId = ndn::readString(element);
      break;
    case stream_tlv::StreamSessionEpochType:
      sessionEpoch = ndn::readNonNegativeInteger(element);
      break;
    case stream_tlv::StreamSequenceType:
      seq = ndn::readNonNegativeInteger(element);
      break;
    case stream_tlv::StreamContentTypeType:
      contentType = ndn::readString(element);
      break;
    case stream_tlv::StreamCaptureMsType:
      captureMs = ndn::readNonNegativeInteger(element);
      break;
    case stream_tlv::StreamArrivalMsType:
      arrivalMs = ndn::readNonNegativeInteger(element);
      break;
    case stream_tlv::StreamDeadlineMsType:
      deadlineMs = ndn::readNonNegativeInteger(element);
      break;
    case stream_tlv::StreamKeyChunkType:
      keyChunk = ndn::readNonNegativeInteger(element) > 0;
      break;
    case stream_tlv::StreamFrameIdType:
      frameId = ndn::readNonNegativeInteger(element);
      break;
    case stream_tlv::StreamFrameFirstSeqType:
      frameFirstSeq = ndn::readNonNegativeInteger(element);
      break;
    case stream_tlv::StreamFrameLastSeqType:
      frameLastSeq = ndn::readNonNegativeInteger(element);
      break;
    case stream_tlv::StreamSegmentIndexType:
      segmentIndex = ndn::readNonNegativeInteger(element);
      break;
    case stream_tlv::StreamSegmentCountType:
      segmentCount = ndn::readNonNegativeInteger(element);
      break;
    case stream_tlv::StreamFecInfoType: {
      StreamFecInfo decoded;
      if (decoded.wireDecode(element)) {
        fec = decoded;
      }
      break;
    }
    case stream_tlv::StreamMetadataType:
      readMetadata(element, metadata);
      break;
    case stream_tlv::StreamPayloadType:
      payload = readBinary(element);
      break;
    default:
      break;
    }
  }
  return true;
}

StreamProducerBuffer::StreamProducerBuffer(size_t maxChunks)
  : m_maxChunks(std::max<size_t>(1, maxChunks))
{
}

void
StreamProducerBuffer::put(const StreamChunk& chunk)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_chunks.find(chunk.seq) == m_chunks.end()) {
    m_order.push_back(chunk.seq);
  }
  m_chunks[chunk.seq] = chunk;
  ++m_metrics.produced;
  m_metrics.bytesProduced += chunk.payload.size();
  while (m_order.size() > m_maxChunks) {
    const auto oldSeq = m_order.front();
    m_order.pop_front();
    if (m_chunks.erase(oldSeq) > 0) {
      ++m_metrics.evicted;
    }
  }
}

std::optional<StreamChunk>
StreamProducerBuffer::get(uint64_t seq) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_chunks.find(seq);
  if (it == m_chunks.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<ndn::Block>
StreamProducerBuffer::getEncoded(uint64_t seq) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_chunks.find(seq);
  if (it == m_chunks.end()) {
    return std::nullopt;
  }
  return it->second.wireEncode();
}

std::vector<uint64_t>
StreamProducerBuffer::sequences() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return {m_order.begin(), m_order.end()};
}

size_t
StreamProducerBuffer::size() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_chunks.size();
}

StreamMetrics
StreamProducerBuffer::metrics() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_metrics;
}

StreamConsumerReorderBuffer::StreamConsumerReorderBuffer(std::string streamId,
                                                         uint64_t sessionEpoch,
                                                         uint64_t nextSeq,
                                                         size_t maxPending,
                                                         size_t history)
  : m_streamId(std::move(streamId))
  , m_sessionEpoch(sessionEpoch)
  , m_nextSeq(nextSeq)
  , m_maxPending(std::max<size_t>(1, maxPending))
  , m_history(std::max<size_t>(1, history))
{
}

void
StreamConsumerReorderBuffer::reset(std::string streamId, uint64_t sessionEpoch,
                                   uint64_t nextSeq)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_streamId = std::move(streamId);
  m_sessionEpoch = sessionEpoch;
  m_nextSeq = nextSeq;
  m_pending.clear();
  m_completed.clear();
  m_completedOrder.clear();
}

std::vector<StreamChunk>
StreamConsumerReorderBuffer::push(const StreamChunk& chunk)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (chunk.streamId != m_streamId || chunk.sessionEpoch != m_sessionEpoch) {
    ++m_metrics.stale;
    return {};
  }
  if (chunk.seq < m_nextSeq || m_pending.count(chunk.seq) > 0 ||
      m_completed.count(chunk.seq) > 0) {
    ++m_metrics.duplicates;
    return {};
  }
  if (m_pending.size() >= m_maxPending) {
    dropOldestPending();
  }
  auto stored = chunk;
  if (stored.arrivalMs == 0) {
    stored.arrivalMs = streamNowMs();
  }
  m_pending[stored.seq] = stored;
  ++m_metrics.received;
  m_metrics.bytesReceived += stored.payload.size();

  auto emitted = drainReadyUnlocked();
  if (emitted.empty() && !m_pending.empty()) {
    ++m_metrics.gaps;
  }
  m_metrics.emitted += emitted.size();
  return emitted;
}

std::vector<uint64_t>
StreamConsumerReorderBuffer::pendingSequences(size_t limit) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  std::vector<uint64_t> result;
  const auto count = limit == 0 ? m_pending.size() : std::min(limit, m_pending.size());
  result.reserve(count);
  for (const auto& item : m_pending) {
    if (limit != 0 && result.size() >= limit) {
      break;
    }
    result.push_back(item.first);
  }
  return result;
}

std::vector<StreamChunk>
StreamConsumerReorderBuffer::drainReady()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  auto emitted = drainReadyUnlocked();
  m_metrics.emitted += emitted.size();
  return emitted;
}

std::vector<uint64_t>
StreamConsumerReorderBuffer::missingSequences(size_t limit) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  std::vector<uint64_t> missing;
  if (m_pending.empty() || limit == 0) {
    return missing;
  }
  const auto highest = m_pending.rbegin()->first;
  for (uint64_t seq = m_nextSeq; seq < highest && missing.size() < limit; ++seq) {
    if (m_pending.count(seq) == 0) {
      missing.push_back(seq);
    }
  }
  return missing;
}

void
StreamConsumerReorderBuffer::skipTo(uint64_t seq)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  for (auto it = m_pending.begin(); it != m_pending.end();) {
    if (it->first < seq) {
      it = m_pending.erase(it);
    }
    else {
      ++it;
    }
  }
  m_nextSeq = std::max(m_nextSeq, seq);
}

uint64_t
StreamConsumerReorderBuffer::nextSeq() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_nextSeq;
}

size_t
StreamConsumerReorderBuffer::pendingCount() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_pending.size();
}

size_t
StreamConsumerReorderBuffer::pendingBytes() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  size_t bytes = 0;
  for (const auto& item : m_pending) {
    bytes += item.second.payload.size();
  }
  return bytes;
}

StreamMetrics
StreamConsumerReorderBuffer::metrics() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_metrics;
}

void
StreamConsumerReorderBuffer::markCompleted(uint64_t seq)
{
  m_completed.insert(seq);
  m_completedOrder.push_back(seq);
  while (m_completedOrder.size() > m_history) {
    m_completed.erase(m_completedOrder.front());
    m_completedOrder.pop_front();
  }
}

void
StreamConsumerReorderBuffer::dropOldestPending()
{
  if (m_pending.empty()) {
    return;
  }
  m_pending.erase(m_pending.begin());
  ++m_metrics.stale;
  ++m_metrics.overflows;
}

std::vector<StreamChunk>
StreamConsumerReorderBuffer::drainReadyUnlocked()
{
  std::vector<StreamChunk> emitted;
  while (true) {
    auto it = m_pending.find(m_nextSeq);
    if (it == m_pending.end()) {
      break;
    }
    emitted.push_back(it->second);
    m_pending.erase(it);
    markCompleted(m_nextSeq);
    ++m_nextSeq;
  }
  return emitted;
}

void
StreamAdaptiveFetcherState::observeRtt(double sampleMs, double alpha)
{
  sampleMs = std::max(1.0, sampleMs);
  alpha = std::max(0.0, std::min(1.0, alpha));
  rttMs = rttMs * (1.0 - alpha) + sampleMs * alpha;
}

void
StreamAdaptiveFetcherState::recordTimeout()
{
  timeoutPressure = std::min(1.0, timeoutPressure + 0.25);
}

void
StreamAdaptiveFetcherState::recordNack()
{
  nackPressure = std::min(1.0, nackPressure + 0.2);
}

void
StreamAdaptiveFetcherState::recordDuplicate()
{
  duplicatePressure = std::min(1.0, duplicatePressure + 0.1);
}

void
StreamAdaptiveFetcherState::setBacklogPressure(double pressure)
{
  backlogPressure = std::max(0.0, std::min(1.0, pressure));
}

void
StreamAdaptiveFetcherState::decay(double factor)
{
  factor = std::max(0.0, std::min(1.0, factor));
  timeoutPressure *= factor;
  nackPressure *= factor;
  duplicatePressure *= factor;
  backlogPressure *= factor;
}

StreamFetchDecision
StreamAdaptiveFetcherState::decide() const
{
  const auto pressure = std::max({
    timeoutPressure,
    nackPressure,
    duplicatePressure * 0.5,
    backlogPressure,
  });

  StreamFetchDecision decision;
  decision.pressure = std::max(0.0, std::min(1.0, pressure));
  if (decision.pressure >= 0.65) {
    decision.reason = "congested";
  }
  else if (decision.pressure >= 0.25) {
    decision.reason = "cautious";
  }
  else {
    decision.reason = "stable";
  }

  const auto window = static_cast<uint64_t>(
    std::llround(static_cast<double>(baseWindow) / (1.0 + decision.pressure * 2.0)));
  const auto lookahead = static_cast<uint64_t>(
    std::llround(static_cast<double>(baseLookahead) / (1.0 + decision.pressure * 1.5)));
  const auto lifetime = static_cast<uint64_t>(
    std::llround(std::max(2.0 * rttMs, static_cast<double>(minInterestLifetimeMs)) *
                 (1.0 + decision.pressure)));
  const auto missing = static_cast<uint64_t>(
    std::llround(std::max(1.5 * rttMs, static_cast<double>(minMissingTimeoutMs)) *
                 (1.0 + decision.pressure)));

  decision.window = std::min(maxWindow, std::max(minWindow, window));
  decision.lookahead = std::min(maxLookahead, std::max(minLookahead, lookahead));
  decision.interestLifetimeMs =
    std::min(maxInterestLifetimeMs, std::max(minInterestLifetimeMs, lifetime));
  decision.missingTimeoutMs =
    std::min(maxMissingTimeoutMs, std::max(minMissingTimeoutMs, missing));
  return decision;
}

StreamHealth
StreamHealth::fromStream(const StreamInfo& info,
                         const StreamMetrics& metrics,
                         const std::optional<StreamFetchDecision>& fetchDecision,
                         uint64_t nextSeq,
                         uint64_t lastChunkMs,
                         bool stopped,
                         uint64_t staleAfterMs,
                         uint64_t nowMs)
{
  StreamHealth health;
  health.streamId = info.streamId;
  health.sessionEpoch = info.sessionEpoch;
  health.nextSeq = nextSeq == 0 ? info.nextSeq : nextSeq;
  health.lastChunkMs = lastChunkMs;
  health.updatedMs = nowMs == 0 ? streamNowMs() : nowMs;
  health.metrics = metrics;
  if (fetchDecision) {
    health.fetchDecision = *fetchDecision;
  }

  if (stopped) {
    health.state = StreamHealthState::Stopped;
    health.reason = "stopped";
  }
  else if (lastChunkMs > 0 && staleAfterMs > 0 && health.updatedMs > lastChunkMs &&
           health.updatedMs - lastChunkMs > staleAfterMs) {
    health.state = StreamHealthState::Stale;
    health.reason = "stale";
  }
  else if (fetchDecision && fetchDecision->reason == "congested") {
    health.state = StreamHealthState::Congested;
    health.reason = "congested";
  }
  else if (metrics.gaps > 0 || metrics.timeouts > 0 || metrics.nacks > 0) {
    health.state = StreamHealthState::Degraded;
    health.reason = "loss-or-gap";
  }
  else {
    health.state = StreamHealthState::Active;
    health.reason = "active";
  }

  return health;
}

} // namespace ndn_service_framework
