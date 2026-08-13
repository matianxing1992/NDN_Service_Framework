#include "Stream.hpp"
#include "TimelineTrace.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
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

bool
hasSameWire(const ndn::Block& lhs, const ndn::Block& rhs)
{
  return lhs.isValid() && rhs.isValid() && lhs.size() == rhs.size() &&
         std::equal(lhs.begin(), lhs.end(), rhs.begin());
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

const char*
toString(StreamPrefetchPhase phase)
{
  switch (phase) {
  case StreamPrefetchPhase::Inactive:
    return "INACTIVE";
  case StreamPrefetchPhase::Chasing:
    return "CHASING";
  case StreamPrefetchPhase::Adjusting:
    return "ADJUSTING";
  case StreamPrefetchPhase::Fetching:
    return "FETCHING";
  case StreamPrefetchPhase::Recovering:
    return "RECOVERING";
  case StreamPrefetchPhase::Stopped:
    return "STOPPED";
  }
  return "INACTIVE";
}

uint64_t
streamNowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

StreamNameMapEntry
StreamNameMapEntry::fromName(const ndn::Name& name)
{
  StreamNameMapEntry entry;
  entry.originalName = name;
  return entry;
}

StreamNameMapEntry
StreamNameMapEntry::fromGroupedName(const ndn::Name& name,
                                    std::string groupIdValue,
                                    std::string sampleClassValue,
                                    uint64_t groupItemIndexValue,
                                    uint64_t predictedSourceItemsValue,
                                    uint64_t predictedRepairItemsValue)
{
  StreamNameMapEntry entry;
  entry.originalName = name;
  entry.groupId = std::move(groupIdValue);
  entry.sampleClass = std::move(sampleClassValue);
  entry.groupItemIndex = groupItemIndexValue;
  entry.predictedSourceItems = predictedSourceItemsValue;
  entry.predictedRepairItems = predictedRepairItemsValue;
  return entry;
}

StreamNameMapEntry
StreamNameMapEntry::makeTombstone()
{
  StreamNameMapEntry entry;
  entry.tombstone = true;
  return entry;
}

bool
StreamNameMapEntry::isTombstone() const
{
  return tombstone;
}

bool
StreamNameMapEntry::hasGroupBinding() const
{
  return !tombstone && !groupId.empty() && !sampleClass.empty() &&
         predictedSourceItems > 0 &&
         groupItemIndex < predictedGroupItems();
}

uint64_t
StreamNameMapEntry::predictedGroupItems() const
{
  if (predictedSourceItems > std::numeric_limits<uint64_t>::max() -
                               predictedRepairItems) {
    return 0;
  }
  return predictedSourceItems + predictedRepairItems;
}

namespace {

StreamContentDigest
digestOpaque(ndn::span<const uint8_t> bytes)
{
  const auto digest = ndn::util::Sha256::computeDigest(bytes);
  StreamContentDigest result{};
  std::copy(digest->begin(), digest->end(), result.begin());
  return result;
}

bool
isZeroDigest(const StreamContentDigest& digest)
{
  return std::all_of(digest.begin(), digest.end(), [] (uint8_t value) {
    return value == 0;
  });
}

} // namespace

StreamContentDigest
computeStreamContentDigest(ndn::span<const uint8_t> bytes)
{
  return digestOpaque(bytes);
}

const char*
toString(LiveStreamFecScheme scheme)
{
  switch (scheme) {
  case LiveStreamFecScheme::None:
    return "NONE";
  case LiveStreamFecScheme::XorOneRepair:
    return "XOR-ONE-REPAIR";
  case LiveStreamFecScheme::Gf256TwoRepair:
    return "GF256-TWO-REPAIR";
  }
  return "UNKNOWN";
}

LiveStreamFecOptions
LiveStreamFecOptions::none()
{
  return {};
}

LiveStreamFecOptions
LiveStreamFecOptions::xorOneRepair(size_t maxSourceItems,
                                   size_t maxSourceBytes,
                                   uint64_t recoveryBudgetMs)
{
  LiveStreamFecOptions result;
  result.scheme = LiveStreamFecScheme::XorOneRepair;
  result.maxSourceItems = maxSourceItems;
  result.maxSourceBytes = maxSourceBytes;
  result.recoveryBudgetMs = recoveryBudgetMs;
  result.repairSymbols = 1;
  if (const auto error = result.validate()) {
    throw std::invalid_argument("invalid LiveStream FEC options: " + *error);
  }
  return result;
}

LiveStreamFecOptions
LiveStreamFecOptions::gf256TwoRepair(size_t maxSourceItems,
                                    size_t maxSourceBytes,
                                    uint64_t recoveryBudgetMs)
{
  LiveStreamFecOptions result;
  result.scheme = LiveStreamFecScheme::Gf256TwoRepair;
  result.maxSourceItems = maxSourceItems;
  result.maxSourceBytes = maxSourceBytes;
  result.recoveryBudgetMs = recoveryBudgetMs;
  result.repairSymbols = 2;
  if (const auto error = result.validate()) {
    throw std::invalid_argument("invalid LiveStream FEC options: " + *error);
  }
  return result;
}

bool
LiveStreamFecOptions::enabled() const
{
  return scheme != LiveStreamFecScheme::None;
}

size_t
LiveStreamFecOptions::repairItemCount() const
{
  return enabled() ? repairSymbols : 0;
}

size_t
LiveStreamFecOptions::recoveryCapacity() const
{
  switch (scheme) {
  case LiveStreamFecScheme::None: return 0;
  case LiveStreamFecScheme::XorOneRepair: return 1;
  case LiveStreamFecScheme::Gf256TwoRepair: return 2;
  }
  return 0;
}

std::optional<std::string>
LiveStreamFecOptions::validate() const
{
  if (scheme != LiveStreamFecScheme::None &&
      scheme != LiveStreamFecScheme::XorOneRepair &&
      scheme != LiveStreamFecScheme::Gf256TwoRepair) {
    return "unknown-fec-scheme";
  }
  if (scheme == LiveStreamFecScheme::None) {
    if (maxSourceItems != 0 || maxSourceBytes != 0 || recoveryBudgetMs != 0 ||
        repairSymbols != 0) {
      return "disabled-fec-has-parameters";
    }
    return std::nullopt;
  }
  if (maxSourceItems < 1 || maxSourceItems > 64) {
    return "invalid-source-count";
  }
  if (maxSourceBytes == 0 || maxSourceBytes > ndn::MAX_NDN_PACKET_SIZE) {
    return "invalid-source-wire-cap";
  }
  if (recoveryBudgetMs == 0 || recoveryBudgetMs > 60000) {
    return "invalid-recovery-budget";
  }
  if ((scheme == LiveStreamFecScheme::XorOneRepair && repairSymbols != 1) ||
      (scheme == LiveStreamFecScheme::Gf256TwoRepair && repairSymbols != 2)) {
    return "invalid-repair-symbol-count";
  }
  return std::nullopt;
}

SampleClassProfile
SampleClassProfile::bounded(std::string id, size_t seed, size_t hardMax,
                            size_t history, size_t margin)
{
  SampleClassProfile result;
  result.classId = std::move(id);
  result.seedSourceItems = seed;
  result.hardMaxSourceItems = hardMax;
  result.historyCapacity = history;
  result.safetyMarginItems = margin;
  if (const auto error = result.validate()) {
    throw std::invalid_argument("invalid sample class profile: " + *error);
  }
  return result;
}

std::optional<std::string>
SampleClassProfile::validate() const
{
  if (classId.empty() || classId.size() > 64) return "invalid-class-id";
  if (seedSourceItems == 0 || hardMaxSourceItems == 0 ||
      seedSourceItems > hardMaxSourceItems || hardMaxSourceItems > 4096) {
    return "invalid-class-count-bounds";
  }
  if (historyCapacity == 0 || historyCapacity > 1024 ||
      safetyMarginItems > hardMaxSourceItems) {
    return "invalid-class-history-bounds";
  }
  return std::nullopt;
}

LiveStreamSamplePredictor::LiveStreamSamplePredictor(
  std::vector<SampleClassProfile> profiles)
{
  reset(std::move(profiles));
}

void
LiveStreamSamplePredictor::reset(std::vector<SampleClassProfile> profiles)
{
  std::map<std::string, ClassState> classes;
  for (auto& profile : profiles) {
    if (const auto error = profile.validate()) {
      throw std::invalid_argument("invalid sample class profile: " + *error);
    }
    ClassState state;
    state.profile = std::move(profile);
    state.status.classId = state.profile.classId;
    state.status.prediction = state.profile.seedSourceItems;
    if (!classes.emplace(state.profile.classId, std::move(state)).second) {
      throw std::invalid_argument("duplicate sample class profile");
    }
  }
  std::lock_guard<std::mutex> guard(m_mutex);
  m_classes = std::move(classes);
}

size_t
LiveStreamSamplePredictor::predict(const ClassState& state)
{
  if (state.history.empty()) {
    return state.profile.seedSourceItems;
  }
  size_t recentHigh = 0;
  for (const auto count : state.history) recentHigh = std::max(recentHigh, count);
  const auto withMargin = recentHigh > state.profile.hardMaxSourceItems -
                                      std::min(state.profile.safetyMarginItems,
                                               state.profile.hardMaxSourceItems)
    ? state.profile.hardMaxSourceItems
    : recentHigh + state.profile.safetyMarginItems;
  return std::clamp(withMargin, size_t{1}, state.profile.hardMaxSourceItems);
}

size_t
LiveStreamSamplePredictor::predict(const std::string& classId) const
{
  std::lock_guard<std::mutex> guard(m_mutex);
  const auto found = m_classes.find(classId);
  if (found == m_classes.end()) throw std::invalid_argument("unknown sample class");
  return predict(found->second);
}

bool
LiveStreamSamplePredictor::observe(const std::string& classId,
                                   size_t actualSourceItems)
{
  std::lock_guard<std::mutex> guard(m_mutex);
  const auto found = m_classes.find(classId);
  if (found == m_classes.end() || actualSourceItems == 0 ||
      actualSourceItems > found->second.profile.hardMaxSourceItems) {
    return false;
  }
  auto& state = found->second;
  const auto predicted = predict(state);
  if (actualSourceItems > predicted) {
    ++state.status.underpredictions;
    state.status.underpredictedItems += actualSourceItems - predicted;
  }
  else if (actualSourceItems < predicted) {
    ++state.status.overpredictions;
    state.status.overpredictedItems += predicted - actualSourceItems;
  }
  state.history.push_back(actualSourceItems);
  while (state.history.size() > state.profile.historyCapacity) {
    state.history.pop_front();
  }
  state.status.observations = state.history.size();
  state.status.prediction = predict(state);
  return true;
}

std::optional<SampleClassPredictionStatus>
LiveStreamSamplePredictor::status(const std::string& classId) const
{
  std::lock_guard<std::mutex> guard(m_mutex);
  const auto found = m_classes.find(classId);
  if (found == m_classes.end()) return std::nullopt;
  auto result = found->second.status;
  result.prediction = predict(found->second);
  return result;
}

std::map<std::string, SampleClassPredictionStatus>
LiveStreamSamplePredictor::statuses() const
{
  std::lock_guard<std::mutex> guard(m_mutex);
  std::map<std::string, SampleClassPredictionStatus> result;
  for (const auto& [classId, state] : m_classes) {
    auto status = state.status;
    status.prediction = predict(state);
    result.emplace(classId, std::move(status));
  }
  return result;
}

std::optional<std::string>
LiveStreamDefinition::validate() const
{
  if (contractVersion != STREAM_NAME_MAP_CONTRACT_VERSION_V1 &&
      contractVersion != STREAM_NAME_MAP_CONTRACT_VERSION_V2) {
    return "unsupported-contract-version";
  }
  if (streamId.empty() || streamId.size() > 256) {
    return "invalid-stream-id";
  }
  if (provider.empty() || semanticDataPrefix.empty() ||
      !provider.isPrefixOf(semanticDataPrefix)) {
    return "invalid-semantic-authority";
  }
  if (sessionEpoch == 0 || mappingVersion == 0) {
    return "invalid-session-version";
  }
  if (mappingBlockCapacity == 0 ||
      mappingBlockCapacity > STREAM_NAME_MAP_MAX_BLOCK_CAPACITY ||
      mappingAheadBlocks == 0 || mappingAheadBlocks > 1024) {
    return "invalid-mapping-bounds";
  }
  if (retainedItems == 0 || maxNameReservations == 0 ||
      maxNameReservations > STREAM_NAME_MAP_MAX_REVERSE_ENTRIES ||
      maxNameReservations < mappingBlockCapacity ||
      maxNameReservations % mappingBlockCapacity != 0 ||
      retainedItems > maxNameReservations || maxPendingInterests == 0 ||
      signedWireCap == 0 || signedWireCap > ndn::MAX_NDN_PACKET_SIZE) {
    return "invalid-runtime-bounds";
  }
  if (contractVersion == STREAM_NAME_MAP_CONTRACT_VERSION_V1) {
    if (samplePeriodMs != 0.0 || !sampleClasses.empty()) {
      return "v1-has-adaptive-sample-policy";
    }
  }
  else {
    if (!std::isfinite(samplePeriodMs) || samplePeriodMs <= 0.0 ||
        sampleClasses.empty() || sampleClasses.size() > 32) {
      return "invalid-adaptive-sample-policy";
    }
    std::set<std::string> classIds;
    for (const auto& profile : sampleClasses) {
      if (const auto error = profile.validate()) return error;
      if (!classIds.insert(profile.classId).second) return "duplicate-sample-class";
      const auto repairs = fec.repairItemCount();
      if (profile.hardMaxSourceItems + repairs > maxPendingInterests) {
        return "sample-class-exceeds-payload-capacity";
      }
      if (fec.enabled() && profile.hardMaxSourceItems > fec.maxSourceItems) {
        return "sample-class-exceeds-fec-capacity";
      }
    }
  }
  return fec.validate();
}

ndn::Name
LiveStreamDefinition::mappingRoot() const
{
  return makeStreamNameMapRoot(provider, streamId);
}

size_t
computeLiveStreamMappingLead(double rttMs, double productionPeriodMs,
                             double jitterMs, size_t minimumItems,
                             size_t maximumItems)
{
  if (!std::isfinite(rttMs) || rttMs < 0.0 ||
      !std::isfinite(productionPeriodMs) || productionPeriodMs <= 0.0 ||
      !std::isfinite(jitterMs) || jitterMs < 0.0 || minimumItems == 0 ||
      maximumItems < minimumItems) {
    throw std::invalid_argument("invalid LiveStream Mapping lead measurements");
  }
  const auto raw = static_cast<size_t>(std::ceil(
    (rttMs + jitterMs) / productionPeriodMs)) + 1;
  return std::clamp(raw, minimumItems, maximumItems);
}

bool
LiveStreamItemReservation::belongsTo(const LiveStreamDefinition& definition) const
{
  return sessionEpoch == definition.sessionEpoch &&
         mappingVersion == definition.mappingVersion &&
         !originalName.empty() &&
         (definition.semanticDataPrefix.isPrefixOf(originalName) ||
          definition.mappingRoot().isPrefixOf(originalName));
}

std::optional<std::string>
LiveStreamGroupReservation::validate(const LiveStreamDefinition& definition) const
{
  if (groupId.empty() || sources.empty()) {
    return "invalid-group";
  }
  if ((!definition.fec.enabled() && !repairs.empty()) ||
      (definition.fec.enabled() &&
       (sources.size() > definition.fec.maxSourceItems ||
        repairs.size() > definition.fec.repairItemCount()))) {
    return "invalid-group-cardinality";
  }
  std::set<ndn::Name> names;
  std::set<StreamCursor> cursors;
  for (const auto& item : sources) {
    if (!item.belongsTo(definition) || !names.insert(item.originalName).second ||
        !cursors.insert(item.cursor).second) {
      return "invalid-source-reservation";
    }
  }
  for (const auto& item : repairs) {
    if (!item.belongsTo(definition) || !names.insert(item.originalName).second ||
        !cursors.insert(item.cursor).second) {
      return "invalid-repair-reservation";
    }
  }
  return std::nullopt;
}

std::optional<std::string>
LiveStreamSampleReservation::validate(const LiveStreamDefinition& definition) const
{
  if (definition.contractVersion != STREAM_NAME_MAP_CONTRACT_VERSION_V2 ||
      sampleClass.empty() || predictedSourceItems == 0 ||
      group.groupId != std::to_string(sampleId) ||
      group.sources.size() != predictedSourceItems) {
    return "invalid-sample-reservation";
  }
  const auto profile = std::find_if(
    definition.sampleClasses.begin(), definition.sampleClasses.end(),
    [this] (const auto& value) { return value.classId == sampleClass; });
  if (profile == definition.sampleClasses.end() ||
      predictedSourceItems > profile->hardMaxSourceItems) {
    return "unknown-or-oversized-sample-class";
  }
  return group.validate(definition);
}

std::optional<std::string>
LiveStreamSampleEnvelope::validate() const
{
  if (groupId.empty() || groupId.size() > 128 || sampleClass.empty() ||
      sampleClass.size() > 64 || actualSourceItems == 0 ||
      actualSourceItems > 4096 || opaqueContent.empty()) {
    return "invalid-sample-envelope";
  }
  if (itemKind == LiveStreamItemKind::Source &&
      groupItemIndex >= actualSourceItems) {
    return "source-index-outside-actual-extent";
  }
  return std::nullopt;
}

ndn::Block
LiveStreamSampleEnvelope::wireEncode() const
{
  if (const auto error = validate()) {
    throw std::invalid_argument("invalid LiveStream sample envelope: " + *error);
  }
  ndn::Block block(stream_tlv::LiveStreamSampleEnvelopeType);
  block.push_back(ndn::makeStringBlock(stream_tlv::StreamGroupIdType, groupId));
  block.push_back(ndn::makeStringBlock(stream_tlv::StreamSampleClassType, sampleClass));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::StreamGroupItemIndexType, groupItemIndex));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::StreamActualSourceItemsType, actualSourceItems));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::StreamItemKindType,
    itemKind == LiveStreamItemKind::Source ? 0 : 1));
  block.push_back(ndn::makeBinaryBlock(
    stream_tlv::StreamPayloadType,
    ndn::span<const uint8_t>(opaqueContent.data(), opaqueContent.size())));
  block.encode();
  return block;
}

bool
LiveStreamSampleEnvelope::wireDecode(const ndn::Block& block)
{
  try {
    if (block.type() != stream_tlv::LiveStreamSampleEnvelopeType) return false;
    block.parse();
    if (block.elements().size() != 6) return false;
    const auto& fields = block.elements();
    if (fields[0].type() != stream_tlv::StreamGroupIdType ||
        fields[1].type() != stream_tlv::StreamSampleClassType ||
        fields[2].type() != stream_tlv::StreamGroupItemIndexType ||
        fields[3].type() != stream_tlv::StreamActualSourceItemsType ||
        fields[4].type() != stream_tlv::StreamItemKindType ||
        fields[5].type() != stream_tlv::StreamPayloadType) return false;
    LiveStreamSampleEnvelope decoded;
    decoded.groupId = ndn::readString(fields[0]);
    decoded.sampleClass = ndn::readString(fields[1]);
    decoded.groupItemIndex = ndn::readNonNegativeInteger(fields[2]);
    decoded.actualSourceItems = ndn::readNonNegativeInteger(fields[3]);
    const auto kind = ndn::readNonNegativeInteger(fields[4]);
    if (kind > 1) return false;
    decoded.itemKind = kind == 0 ? LiveStreamItemKind::Source :
                                  LiveStreamItemKind::Repair;
    decoded.opaqueContent.assign(fields[5].value_begin(), fields[5].value_end());
    if (decoded.validate() || !hasSameWire(decoded.wireEncode(), block)) return false;
    *this = std::move(decoded);
    return true;
  }
  catch (const std::exception&) {
    return false;
  }
}

std::string
LiveStreamFecRepair::validate(const LiveStreamDefinition& definition) const
{
  if (const auto error = definition.validate()) {
    return *error;
  }
  if (!definition.fec.enabled() || streamId != definition.streamId ||
      sessionEpoch != definition.sessionEpoch ||
      mappingVersion != definition.mappingVersion || groupId.empty()) {
    return "fec-session-mismatch";
  }
  if (scheme != definition.fec.scheme ||
      recoveryCapacity != definition.fec.recoveryCapacity() ||
      repairIndex >= definition.fec.repairItemCount()) {
    return "fec-declaration-mismatch";
  }
  const auto count = sourceNames.size();
  if (sourceNames.size() != count || sourceCursors.size() != count ||
      sourceLengths.size() != count || sourceDigests.size() != count ||
      count == 0 || count > definition.fec.maxSourceItems ||
      repairName.empty() || codedBytes.empty() ||
      codedBytes.size() > definition.fec.maxSourceBytes ||
      createdMs == 0 || expiresMs <= createdMs ||
      expiresMs - createdMs > definition.fec.recoveryBudgetMs) {
    return "invalid-fec-shape";
  }
  std::set<ndn::Name> names;
  std::set<StreamCursor> cursors;
  for (size_t i = 0; i < count; ++i) {
    if ((!definition.semanticDataPrefix.isPrefixOf(sourceNames[i]) &&
         !definition.mappingRoot().isPrefixOf(sourceNames[i])) ||
        !names.insert(sourceNames[i]).second ||
        !cursors.insert(sourceCursors[i]).second || sourceLengths[i] == 0 ||
        sourceLengths[i] > codedBytes.size() || isZeroDigest(sourceDigests[i])) {
      return "invalid-fec-source-binding";
    }
  }
  if ((!definition.semanticDataPrefix.isPrefixOf(repairName) &&
       !definition.mappingRoot().isPrefixOf(repairName)) ||
      !names.insert(repairName).second || !cursors.insert(repairCursor).second) {
    return "invalid-fec-repair-binding";
  }
  return {};
}

ndn::Block
LiveStreamFecRepair::wireEncode() const
{
  ndn::Block block(stream_tlv::LiveStreamFecRepairType);
  appendString(block, stream_tlv::StreamIdType, streamId);
  appendNumber(block, stream_tlv::StreamSessionEpochType, sessionEpoch);
  appendNumber(block, stream_tlv::StreamMappingVersionType, mappingVersion);
  appendString(block, stream_tlv::LiveStreamFecGroupIdType, groupId);
  appendNumber(block, stream_tlv::LiveStreamFecCreatedMsType, createdMs);
  appendNumber(block, stream_tlv::LiveStreamFecExpiresMsType, expiresMs);
  if (scheme != LiveStreamFecScheme::XorOneRepair) {
    appendNumber(block, stream_tlv::LiveStreamFecSchemeType,
                 static_cast<uint64_t>(scheme));
    appendNumber(block, stream_tlv::LiveStreamFecRecoveryCapacityType,
                 recoveryCapacity);
    // Symbol zero is meaningful and required. appendNumber intentionally
    // omits zero-valued optional fields, so encode this required NNI directly.
    block.push_back(ndn::makeNonNegativeIntegerBlock(
      stream_tlv::LiveStreamFecRepairIndexType, repairIndex));
  }
  for (size_t i = 0; i < sourceNames.size(); ++i) {
    ndn::Block source(stream_tlv::LiveStreamFecSourceType);
    source.push_back(sourceNames[i].wireEncode());
    source.push_back(ndn::makeNonNegativeIntegerBlock(
      stream_tlv::LiveStreamFecSourceCursorType, sourceCursors.at(i)));
    source.push_back(ndn::makeNonNegativeIntegerBlock(
      stream_tlv::LiveStreamFecSourceLengthType, sourceLengths.at(i)));
    source.push_back(ndn::makeBinaryBlock(
      stream_tlv::LiveStreamFecSourceDigestType,
      ndn::span<const uint8_t>(sourceDigests.at(i).data(),
                               sourceDigests.at(i).size())));
    source.encode();
    block.push_back(source);
  }
  block.push_back(ndn::makeStringBlock(stream_tlv::LiveStreamFecRepairNameType,
                                       repairName.toUri()));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::LiveStreamFecRepairCursorType, repairCursor));
  block.push_back(ndn::makeBinaryBlock(stream_tlv::LiveStreamFecCodedBytesType,
                                      ndn::span<const uint8_t>(codedBytes.data(),
                                                               codedBytes.size())));
  block.encode();
  return block;
}

bool
LiveStreamFecRepair::wireDecode(const ndn::Block& wire)
{
  try {
    if (wire.type() != stream_tlv::LiveStreamFecRepairType) {
      return false;
    }
    auto block = wire;
    block.parse();
    LiveStreamFecRepair decoded;
    size_t index = 0;
    const auto take = [&] (uint32_t type) -> const ndn::Block& {
      if (index >= block.elements().size() || block.elements()[index].type() != type) {
        throw std::invalid_argument("unexpected FEC field");
      }
      return block.elements()[index++];
    };
    decoded.streamId = ndn::readString(take(stream_tlv::StreamIdType));
    decoded.sessionEpoch = ndn::readNonNegativeInteger(
      take(stream_tlv::StreamSessionEpochType));
    decoded.mappingVersion = ndn::readNonNegativeInteger(
      take(stream_tlv::StreamMappingVersionType));
    decoded.groupId = ndn::readString(take(stream_tlv::LiveStreamFecGroupIdType));
    decoded.createdMs = ndn::readNonNegativeInteger(
      take(stream_tlv::LiveStreamFecCreatedMsType));
    decoded.expiresMs = ndn::readNonNegativeInteger(
      take(stream_tlv::LiveStreamFecExpiresMsType));
    if (index < block.elements().size() &&
        block.elements()[index].type() == stream_tlv::LiveStreamFecSchemeType) {
      const auto scheme = ndn::readNonNegativeInteger(
        take(stream_tlv::LiveStreamFecSchemeType));
      if (scheme != static_cast<uint64_t>(LiveStreamFecScheme::Gf256TwoRepair)) {
        return false;
      }
      decoded.scheme = LiveStreamFecScheme::Gf256TwoRepair;
      decoded.recoveryCapacity = ndn::readNonNegativeInteger(
        take(stream_tlv::LiveStreamFecRecoveryCapacityType));
      decoded.repairIndex = ndn::readNonNegativeInteger(
        take(stream_tlv::LiveStreamFecRepairIndexType));
    }
    while (index < block.elements().size() &&
           block.elements()[index].type() == stream_tlv::LiveStreamFecSourceType) {
      auto source = block.elements()[index++];
      source.parse();
      if (source.elements().size() != 4 ||
          source.elements()[0].type() != ndn::tlv::Name ||
          source.elements()[1].type() != stream_tlv::LiveStreamFecSourceCursorType ||
          source.elements()[2].type() != stream_tlv::LiveStreamFecSourceLengthType ||
          source.elements()[3].type() != stream_tlv::LiveStreamFecSourceDigestType ||
          source.elements()[3].value_size() != 32) {
        return false;
      }
      decoded.sourceNames.emplace_back(source.elements()[0]);
      decoded.sourceCursors.push_back(ndn::readNonNegativeInteger(source.elements()[1]));
      decoded.sourceLengths.push_back(ndn::readNonNegativeInteger(source.elements()[2]));
      StreamContentDigest digest{};
      std::copy(source.elements()[3].value(),
                source.elements()[3].value() + source.elements()[3].value_size(),
                digest.begin());
      decoded.sourceDigests.push_back(digest);
    }
    decoded.repairName = ndn::Name(ndn::readString(
      take(stream_tlv::LiveStreamFecRepairNameType)));
    decoded.repairCursor = ndn::readNonNegativeInteger(
      take(stream_tlv::LiveStreamFecRepairCursorType));
    decoded.codedBytes = readBinary(take(stream_tlv::LiveStreamFecCodedBytesType));
    if (index != block.elements().size() || !hasSameWire(decoded.wireEncode(), wire)) {
      return false;
    }
    *this = std::move(decoded);
    return true;
  }
  catch (const std::exception&) {
    return false;
  }
}

LiveStreamFecRepair
makeLiveStreamXorRepair(const LiveStreamDefinition& definition,
                        const std::string& groupId,
                        const std::vector<LiveStreamItemReservation>& sources,
                        const LiveStreamItemReservation& repair,
                        const std::vector<std::vector<uint8_t>>& opaqueSources,
                        uint64_t createdMs,
                        uint64_t expiresMs)
{
  LiveStreamGroupReservation group{groupId, sources, {repair}};
  if (const auto error = group.validate(definition)) {
    throw std::invalid_argument("invalid LiveStream group: " + *error);
  }
  if (opaqueSources.size() != sources.size()) {
    throw std::invalid_argument("opaque source count mismatch");
  }
  size_t width = 0;
  for (const auto& value : opaqueSources) {
    if (value.empty() || value.size() > definition.fec.maxSourceBytes) {
      throw std::invalid_argument("opaque source size exceeds FEC bounds");
    }
    width = std::max(width, value.size());
  }
  LiveStreamFecRepair result;
  result.streamId = definition.streamId;
  result.sessionEpoch = definition.sessionEpoch;
  result.mappingVersion = definition.mappingVersion;
  result.groupId = groupId;
  result.createdMs = createdMs;
  result.expiresMs = expiresMs;
  result.repairName = repair.originalName;
  result.repairCursor = repair.cursor;
  result.codedBytes.assign(width, 0);
  for (size_t i = 0; i < sources.size(); ++i) {
    result.sourceNames.push_back(sources[i].originalName);
    result.sourceCursors.push_back(sources[i].cursor);
    result.sourceLengths.push_back(opaqueSources[i].size());
    result.sourceDigests.push_back(digestOpaque(ndn::span<const uint8_t>(
      opaqueSources[i].data(), opaqueSources[i].size())));
    for (size_t j = 0; j < opaqueSources[i].size(); ++j) {
      result.codedBytes[j] ^= opaqueSources[i][j];
    }
  }
  if (const auto error = result.validate(definition); !error.empty()) {
    throw std::invalid_argument("invalid generated FEC repair: " + error);
  }
  if (result.wireEncode().size() > definition.signedWireCap) {
    throw std::length_error("FEC repair exceeds configured wire cap");
  }
  return result;
}

std::optional<std::vector<uint8_t>>
recoverLiveStreamXorSource(
  const LiveStreamDefinition& definition,
  const LiveStreamFecRepair& repair,
  const std::vector<std::optional<std::vector<uint8_t>>>& opaqueSources,
  size_t missingIndex,
  uint64_t nowMs)
{
  if (!repair.validate(definition).empty() || nowMs > repair.expiresMs ||
      missingIndex >= opaqueSources.size() || opaqueSources.size() != repair.sourceNames.size() ||
      opaqueSources[missingIndex]) {
    return std::nullopt;
  }
  size_t missing = 0;
  auto recovered = repair.codedBytes;
  for (size_t i = 0; i < opaqueSources.size(); ++i) {
    if (!opaqueSources[i]) {
      ++missing;
      continue;
    }
    if (opaqueSources[i]->size() != repair.sourceLengths[i] ||
        digestOpaque(ndn::span<const uint8_t>(opaqueSources[i]->data(),
                                              opaqueSources[i]->size())) !=
          repair.sourceDigests[i]) {
      return std::nullopt;
    }
    for (size_t j = 0; j < opaqueSources[i]->size(); ++j) {
      recovered[j] ^= (*opaqueSources[i])[j];
    }
  }
  if (missing != 1 || repair.sourceLengths[missingIndex] > recovered.size()) {
    return std::nullopt;
  }
  recovered.resize(repair.sourceLengths[missingIndex]);
  if (digestOpaque(ndn::span<const uint8_t>(recovered.data(), recovered.size())) !=
      repair.sourceDigests[missingIndex]) {
    return std::nullopt;
  }
  return recovered;
}

namespace {

uint8_t
gf256Multiply(uint8_t left, uint8_t right)
{
  uint8_t result = 0;
  while (right != 0) {
    if ((right & 1) != 0) result ^= left;
    const bool high = (left & 0x80) != 0;
    left <<= 1;
    if (high) left ^= 0x1d;
    right >>= 1;
  }
  return result;
}

uint8_t
gf256Power(uint8_t value, unsigned exponent)
{
  uint8_t result = 1;
  while (exponent != 0) {
    if ((exponent & 1) != 0) result = gf256Multiply(result, value);
    value = gf256Multiply(value, value);
    exponent >>= 1;
  }
  return result;
}

uint8_t
gf256Inverse(uint8_t value)
{
  if (value == 0) throw std::invalid_argument("zero GF256 inverse");
  return gf256Power(value, 254);
}

uint8_t
repairCoefficient(uint64_t repairIndex, size_t sourceIndex)
{
  return repairIndex == 0 ? uint8_t{1} :
    gf256Power(static_cast<uint8_t>(sourceIndex + 1),
               static_cast<unsigned>(repairIndex));
}

} // namespace

std::vector<LiveStreamFecRepair>
makeLiveStreamRepairSymbols(
  const LiveStreamDefinition& definition,
  const std::string& groupId,
  const std::vector<LiveStreamItemReservation>& sources,
  const std::vector<LiveStreamItemReservation>& repairs,
  const std::vector<std::vector<uint8_t>>& opaqueSources,
  uint64_t createdMs,
  uint64_t expiresMs)
{
  if (definition.fec.scheme == LiveStreamFecScheme::XorOneRepair) {
    if (repairs.size() != 1) throw std::invalid_argument("invalid XOR repair count");
    return {makeLiveStreamXorRepair(definition, groupId, sources, repairs.front(),
                                    opaqueSources, createdMs, expiresMs)};
  }
  LiveStreamGroupReservation group{groupId, sources, repairs};
  if (definition.fec.scheme != LiveStreamFecScheme::Gf256TwoRepair ||
      group.validate(definition) || opaqueSources.size() != sources.size()) {
    throw std::invalid_argument("invalid multi-erasure repair group");
  }
  size_t width = 0;
  for (const auto& source : opaqueSources) {
    if (source.empty() || source.size() > definition.fec.maxSourceBytes) {
      throw std::invalid_argument("opaque source size exceeds FEC bounds");
    }
    width = std::max(width, source.size());
  }
  std::vector<LiveStreamFecRepair> result;
  result.reserve(repairs.size());
  for (size_t repairIndex = 0; repairIndex < repairs.size(); ++repairIndex) {
    LiveStreamFecRepair symbol;
    symbol.scheme = definition.fec.scheme;
    symbol.recoveryCapacity = definition.fec.recoveryCapacity();
    symbol.repairIndex = repairIndex;
    symbol.streamId = definition.streamId;
    symbol.sessionEpoch = definition.sessionEpoch;
    symbol.mappingVersion = definition.mappingVersion;
    symbol.groupId = groupId;
    symbol.createdMs = createdMs;
    symbol.expiresMs = expiresMs;
    symbol.repairName = repairs[repairIndex].originalName;
    symbol.repairCursor = repairs[repairIndex].cursor;
    symbol.codedBytes.assign(width, 0);
    for (size_t sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex) {
      symbol.sourceNames.push_back(sources[sourceIndex].originalName);
      symbol.sourceCursors.push_back(sources[sourceIndex].cursor);
      symbol.sourceLengths.push_back(opaqueSources[sourceIndex].size());
      symbol.sourceDigests.push_back(digestOpaque(ndn::span<const uint8_t>(
        opaqueSources[sourceIndex].data(), opaqueSources[sourceIndex].size())));
      const auto coefficient = repairCoefficient(repairIndex, sourceIndex);
      for (size_t byte = 0; byte < opaqueSources[sourceIndex].size(); ++byte) {
        symbol.codedBytes[byte] ^= gf256Multiply(coefficient,
                                                 opaqueSources[sourceIndex][byte]);
      }
    }
    if (const auto error = symbol.validate(definition); !error.empty()) {
      throw std::invalid_argument("invalid generated FEC repair: " + error);
    }
    if (symbol.wireEncode().size() > definition.signedWireCap) {
      throw std::length_error("FEC repair exceeds configured wire cap");
    }
    result.push_back(std::move(symbol));
  }
  return result;
}

std::optional<std::vector<std::optional<std::vector<uint8_t>>>>
recoverLiveStreamSources(
  const LiveStreamDefinition& definition,
  const std::vector<LiveStreamFecRepair>& repairs,
  const std::vector<std::optional<std::vector<uint8_t>>>& opaqueSources,
  uint64_t nowMs)
{
  if (repairs.empty()) return std::nullopt;
  const auto& canonical = repairs.front();
  std::set<uint64_t> indices;
  for (const auto& repair : repairs) {
    if (!repair.validate(definition).empty() || nowMs > repair.expiresMs ||
        repair.groupId != canonical.groupId ||
        repair.sourceNames != canonical.sourceNames ||
        repair.sourceCursors != canonical.sourceCursors ||
        repair.sourceLengths != canonical.sourceLengths ||
        repair.sourceDigests != canonical.sourceDigests ||
        !indices.insert(repair.repairIndex).second) return std::nullopt;
  }
  if (opaqueSources.size() != canonical.sourceNames.size()) return std::nullopt;
  std::vector<size_t> missing;
  for (size_t i = 0; i < opaqueSources.size(); ++i) {
    if (!opaqueSources[i]) missing.push_back(i);
    else if (opaqueSources[i]->size() != canonical.sourceLengths[i] ||
             digestOpaque(ndn::span<const uint8_t>(opaqueSources[i]->data(),
                                                    opaqueSources[i]->size())) !=
               canonical.sourceDigests[i]) return std::nullopt;
  }
  if (missing.empty()) return opaqueSources;
  if (missing.size() > canonical.recoveryCapacity || repairs.size() < missing.size() ||
      missing.size() > 2) return std::nullopt;
  auto result = opaqueSources;
  const size_t width = canonical.codedBytes.size();
  std::vector<std::vector<uint8_t>> rhs;
  for (size_t equation = 0; equation < missing.size(); ++equation) {
    rhs.push_back(repairs[equation].codedBytes);
    if (rhs.back().size() != width) return std::nullopt;
    for (size_t source = 0; source < opaqueSources.size(); ++source) {
      if (!opaqueSources[source]) continue;
      const auto coefficient = repairCoefficient(repairs[equation].repairIndex, source);
      for (size_t byte = 0; byte < opaqueSources[source]->size(); ++byte) {
        rhs.back()[byte] ^= gf256Multiply(coefficient, (*opaqueSources[source])[byte]);
      }
    }
  }
  if (missing.size() == 1) {
    const auto coefficient = repairCoefficient(repairs[0].repairIndex, missing[0]);
    const auto inverse = gf256Inverse(coefficient);
    std::vector<uint8_t> recovered(width);
    for (size_t byte = 0; byte < width; ++byte) {
      recovered[byte] = gf256Multiply(inverse, rhs[0][byte]);
    }
    recovered.resize(canonical.sourceLengths[missing[0]]);
    result[missing[0]] = std::move(recovered);
  }
  else {
    const auto a = repairCoefficient(repairs[0].repairIndex, missing[0]);
    const auto b = repairCoefficient(repairs[0].repairIndex, missing[1]);
    const auto c = repairCoefficient(repairs[1].repairIndex, missing[0]);
    const auto d = repairCoefficient(repairs[1].repairIndex, missing[1]);
    const auto determinant = gf256Multiply(a, d) ^ gf256Multiply(b, c);
    if (determinant == 0) return std::nullopt;
    const auto inverse = gf256Inverse(determinant);
    std::vector<uint8_t> first(width), second(width);
    for (size_t byte = 0; byte < width; ++byte) {
      first[byte] = gf256Multiply(inverse,
        gf256Multiply(d, rhs[0][byte]) ^ gf256Multiply(b, rhs[1][byte]));
      second[byte] = gf256Multiply(inverse,
        gf256Multiply(c, rhs[0][byte]) ^ gf256Multiply(a, rhs[1][byte]));
    }
    first.resize(canonical.sourceLengths[missing[0]]);
    second.resize(canonical.sourceLengths[missing[1]]);
    result[missing[0]] = std::move(first);
    result[missing[1]] = std::move(second);
  }
  for (const auto index : missing) {
    if (!result[index] || digestOpaque(ndn::span<const uint8_t>(
          result[index]->data(), result[index]->size())) != canonical.sourceDigests[index]) {
      return std::nullopt;
    }
  }
  return result;
}

std::optional<std::string>
StreamNameMapBlock::validate() const
{
  if (contractVersion != STREAM_NAME_MAP_CONTRACT_VERSION_V1 &&
      contractVersion != STREAM_NAME_MAP_CONTRACT_VERSION_V2) {
    return "unsupported-contract-version";
  }
  if (streamId.empty() || streamId.size() > 256) {
    return "invalid-stream-id";
  }
  if (sessionEpoch == 0) {
    return "invalid-session-epoch";
  }
  if (mappingVersion == 0) {
    return "invalid-mapping-version";
  }
  if (blockCapacity == 0 || blockCapacity > STREAM_NAME_MAP_MAX_BLOCK_CAPACITY) {
    return "invalid-block-capacity";
  }
  if (blockNumber > std::numeric_limits<uint64_t>::max() / blockCapacity) {
    return "cursor-range-overflow";
  }
  const auto expectedFirst = blockNumber * blockCapacity;
  if (firstCursor != expectedFirst) {
    return "invalid-first-cursor";
  }
  if (firstCursor > std::numeric_limits<uint64_t>::max() - (blockCapacity - 1)) {
    return "cursor-range-overflow";
  }
  if (entries.size() != blockCapacity) {
    return "entry-count-mismatch";
  }
  if ((blockNumber == 0 && previousContentDigest) ||
      (blockNumber > 0 && !previousContentDigest)) {
    return "invalid-previous-content-digest";
  }

  std::set<ndn::Name> names;
  struct GroupTuple
  {
    std::string sampleClass;
    uint64_t sources = 0;
    uint64_t repairs = 0;
  };
  std::map<std::string, GroupTuple> groups;
  std::optional<std::string> activeGroup;
  uint64_t activeGroupIndex = 0;
  std::set<std::string> closedGroups;
  for (const auto& entry : entries) {
    if (entry.tombstone) {
      if (!entry.originalName.empty() || entry.hasGroupBinding() ||
          !entry.groupId.empty() || !entry.sampleClass.empty() ||
          entry.predictedSourceItems != 0 || entry.predictedRepairItems != 0) {
        return "tombstone-has-name";
      }
      if (activeGroup) {
        closedGroups.insert(*activeGroup);
        activeGroup.reset();
      }
      continue;
    }
    if (entry.originalName.empty()) {
      return "empty-original-name";
    }
    if (entry.originalName.wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE) {
      return "original-name-too-large";
    }
    if (!names.insert(entry.originalName).second) {
      return "duplicate-original-name";
    }
    if (contractVersion == STREAM_NAME_MAP_CONTRACT_VERSION_V1) {
      if (entry.hasGroupBinding() || !entry.groupId.empty() ||
          !entry.sampleClass.empty() || entry.predictedSourceItems != 0 ||
          entry.predictedRepairItems != 0 || entry.groupItemIndex != 0) {
        return "v1-entry-has-group-binding";
      }
      continue;
    }
    if (!entry.hasGroupBinding() || entry.groupId.size() > 128 ||
        entry.sampleClass.size() > 64 || entry.predictedGroupItems() > 4096) {
      return "invalid-v2-group-binding";
    }
    const GroupTuple tuple{entry.sampleClass, entry.predictedSourceItems,
                           entry.predictedRepairItems};
    const auto [found, inserted] = groups.emplace(entry.groupId, tuple);
    if (!inserted && (found->second.sampleClass != tuple.sampleClass ||
                      found->second.sources != tuple.sources ||
                      found->second.repairs != tuple.repairs)) {
      return "conflicting-v2-group-binding";
    }
    if (!activeGroup || *activeGroup != entry.groupId) {
      if (activeGroup) closedGroups.insert(*activeGroup);
      if (closedGroups.count(entry.groupId) != 0) {
        return "non-contiguous-v2-group";
      }
      activeGroup = entry.groupId;
      activeGroupIndex = entry.groupItemIndex;
    }
    else {
      if (activeGroupIndex == std::numeric_limits<uint64_t>::max() ||
          entry.groupItemIndex != activeGroupIndex + 1) {
        return "non-contiguous-v2-group-index";
      }
      activeGroupIndex = entry.groupItemIndex;
    }
  }
  return std::nullopt;
}

ndn::Block
StreamNameMapBlock::wireEncode() const
{
  if (const auto error = validate()) {
    throw std::invalid_argument("invalid StreamNameMapBlock: " + *error);
  }

  ndn::Block block(stream_tlv::StreamNameMapBlockType);
  // Zero is meaningful for blockNumber and firstCursor, so every required NNI
  // is encoded explicitly rather than through appendNumber().
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::StreamContractVersionType, contractVersion));
  block.push_back(ndn::makeStringBlock(stream_tlv::StreamIdType, streamId));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::StreamSessionEpochType, sessionEpoch));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::StreamMappingVersionType, mappingVersion));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::StreamMapBlockNumberType, blockNumber));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::StreamMapBlockCapacityType, blockCapacity));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::StreamMapFirstCursorType, firstCursor));
  if (previousContentDigest) {
    block.push_back(ndn::makeBinaryBlock(
      stream_tlv::StreamPreviousContentDigestType,
      previousContentDigest->begin(), previousContentDigest->end()));
  }
  for (const auto& entry : entries) {
    ndn::Block encodedEntry(stream_tlv::StreamNameMapEntryType);
    if (entry.tombstone) {
      encodedEntry.push_back(ndn::makeEmptyBlock(stream_tlv::StreamNameMapTombstoneType));
    }
    else {
      encodedEntry.push_back(entry.originalName.wireEncode());
      if (contractVersion == STREAM_NAME_MAP_CONTRACT_VERSION_V2) {
        encodedEntry.push_back(ndn::makeStringBlock(
          stream_tlv::StreamGroupIdType, entry.groupId));
        encodedEntry.push_back(ndn::makeStringBlock(
          stream_tlv::StreamSampleClassType, entry.sampleClass));
        encodedEntry.push_back(ndn::makeNonNegativeIntegerBlock(
          stream_tlv::StreamGroupItemIndexType, entry.groupItemIndex));
        encodedEntry.push_back(ndn::makeNonNegativeIntegerBlock(
          stream_tlv::StreamPredictedSourceItemsType,
          entry.predictedSourceItems));
        encodedEntry.push_back(ndn::makeNonNegativeIntegerBlock(
          stream_tlv::StreamPredictedRepairItemsType,
          entry.predictedRepairItems));
      }
    }
    encodedEntry.encode();
    block.push_back(encodedEntry);
  }
  block.encode();
  return block;
}

bool
StreamNameMapBlock::wireDecode(const ndn::Block& block)
{
  try {
    if (block.type() != stream_tlv::StreamNameMapBlockType) {
      return false;
    }
    block.parse();
    const auto& elements = block.elements();
    if (elements.size() < 8) {
      return false;
    }

    size_t index = 0;
    const auto take = [&] (uint32_t expectedType) -> const ndn::Block& {
      if (index >= elements.size() || elements[index].type() != expectedType) {
        throw ndn::tlv::Error("non-canonical StreamNameMapBlock field order");
      }
      return elements[index++];
    };

    StreamNameMapBlock decoded;
    decoded.contractVersion = ndn::readNonNegativeInteger(
      take(stream_tlv::StreamContractVersionType));
    decoded.streamId = ndn::readString(take(stream_tlv::StreamIdType));
    decoded.sessionEpoch = ndn::readNonNegativeInteger(
      take(stream_tlv::StreamSessionEpochType));
    decoded.mappingVersion = ndn::readNonNegativeInteger(
      take(stream_tlv::StreamMappingVersionType));
    decoded.blockNumber = ndn::readNonNegativeInteger(
      take(stream_tlv::StreamMapBlockNumberType));
    decoded.blockCapacity = ndn::readNonNegativeInteger(
      take(stream_tlv::StreamMapBlockCapacityType));
    decoded.firstCursor = ndn::readNonNegativeInteger(
      take(stream_tlv::StreamMapFirstCursorType));

    if (decoded.blockNumber > 0) {
      const auto& digest = take(stream_tlv::StreamPreviousContentDigestType);
      if (digest.value_size() != StreamContentDigest{}.size()) {
        return false;
      }
      StreamContentDigest bytes{};
      std::copy(digest.value_begin(), digest.value_end(), bytes.begin());
      decoded.previousContentDigest = bytes;
    }

    if (decoded.blockCapacity > STREAM_NAME_MAP_MAX_BLOCK_CAPACITY ||
        elements.size() - index != decoded.blockCapacity) {
      return false;
    }
    decoded.entries.reserve(static_cast<size_t>(decoded.blockCapacity));
    for (uint64_t slot = 0; slot < decoded.blockCapacity; ++slot) {
      const auto& encodedEntry = take(stream_tlv::StreamNameMapEntryType);
      encodedEntry.parse();
      if (encodedEntry.elements().empty()) return false;
      const auto& value = encodedEntry.elements().front();
      if (value.type() == ndn::tlv::Name) {
        const auto expectedElements =
          decoded.contractVersion == STREAM_NAME_MAP_CONTRACT_VERSION_V2 ? 6u : 1u;
        if (encodedEntry.elements().size() != expectedElements) return false;
        if (decoded.contractVersion == STREAM_NAME_MAP_CONTRACT_VERSION_V1) {
          decoded.entries.push_back(StreamNameMapEntry::fromName(ndn::Name(value)));
        }
        else {
          const auto& fields = encodedEntry.elements();
          if (fields[1].type() != stream_tlv::StreamGroupIdType ||
              fields[2].type() != stream_tlv::StreamSampleClassType ||
              fields[3].type() != stream_tlv::StreamGroupItemIndexType ||
              fields[4].type() != stream_tlv::StreamPredictedSourceItemsType ||
              fields[5].type() != stream_tlv::StreamPredictedRepairItemsType) {
            return false;
          }
          decoded.entries.push_back(StreamNameMapEntry::fromGroupedName(
            ndn::Name(value), ndn::readString(fields[1]), ndn::readString(fields[2]),
            ndn::readNonNegativeInteger(fields[3]),
            ndn::readNonNegativeInteger(fields[4]),
            ndn::readNonNegativeInteger(fields[5])));
        }
      }
      else if (value.type() == stream_tlv::StreamNameMapTombstoneType &&
               value.value_size() == 0 && encodedEntry.elements().size() == 1) {
        decoded.entries.push_back(StreamNameMapEntry::makeTombstone());
      }
      else {
        return false;
      }
    }
    if (index != elements.size() || decoded.validate()) {
      return false;
    }
    // This also rejects non-minimal NNI/TLV encodings, reordered fields, and
    // any otherwise parseable representation that is not the frozen v1 wire.
    if (!hasSameWire(decoded.wireEncode(), block)) {
      return false;
    }
    *this = std::move(decoded);
    return true;
  }
  catch (const std::exception&) {
    return false;
  }
}

ndn::Block
StreamNameMapBlock::canonicalContent() const
{
  ndn::Block content(ndn::tlv::Content);
  content.push_back(wireEncode());
  content.encode();
  return content;
}

StreamContentDigest
StreamNameMapBlock::contentDigest() const
{
  const auto content = canonicalContent();
  const auto digest = ndn::util::Sha256::computeDigest(
    ndn::span<const uint8_t>(content.begin(), content.size()));
  StreamContentDigest result{};
  std::copy(digest->begin(), digest->end(), result.begin());
  return result;
}

bool
StreamNameMapBlock::fitsSignedWireBudget(size_t signedEnvelopeOverhead,
                                         size_t configuredWireCap) const
{
  const auto effectiveCap = std::min(configuredWireCap, ndn::MAX_NDN_PACKET_SIZE);
  const auto contentSize = canonicalContent().size();
  return signedEnvelopeOverhead <= effectiveCap &&
         contentSize <= effectiveCap - signedEnvelopeOverhead;
}

StreamCursor
StreamNameMapBlock::lastCursor() const
{
  if (const auto error = validate()) {
    throw std::invalid_argument("invalid StreamNameMapBlock: " + *error);
  }
  return firstCursor + blockCapacity - 1;
}

ndn::Name
makeStreamNameMapRoot(const ndn::Name& provider, const std::string& streamId)
{
  if (provider.empty() || streamId.empty() || streamId.size() > 256) {
    throw std::invalid_argument("mapping root requires provider and bounded stream id");
  }
  ndn::Name root(provider);
  root.append("NDNSF").append("STREAM-MAP");
  root.append(ndn::name::Component(streamId));
  return root;
}

ndn::Name
makeStreamNameMapBlockName(const ndn::Name& mappingRoot,
                           uint64_t mappingVersion,
                           uint64_t blockNumber)
{
  if (mappingRoot.empty() || mappingVersion == 0) {
    throw std::invalid_argument("mapping block name requires root and version");
  }
  ndn::Name name(mappingRoot);
  name.appendVersion(mappingVersion);
  name.appendSequenceNumber(blockNumber);
  return name;
}

std::optional<std::string>
StreamCursorFrontiers::validate(uint64_t blockCapacity,
                                uint64_t checkpointBlock) const
{
  if (blockCapacity == 0 || blockCapacity > STREAM_NAME_MAP_MAX_BLOCK_CAPACITY) {
    return "invalid-block-capacity";
  }
  if (!(oldestRetained <= latestJoin &&
        latestJoin <= latestProduced &&
        latestProduced <= mappingCommittedThrough &&
        mappingCommittedThrough < nextReserved)) {
    return "invalid-frontier-order";
  }
  if (mappingCommittedThrough % blockCapacity != blockCapacity - 1) {
    return "committed-frontier-not-block-aligned";
  }
  if (latestJoin / blockCapacity != checkpointBlock) {
    return "checkpoint-does-not-cover-join";
  }
  return std::nullopt;
}

const char*
toString(StreamNameMapAdmissionDisposition disposition)
{
  switch (disposition) {
  case StreamNameMapAdmissionDisposition::Admitted:
    return "ADMITTED";
  case StreamNameMapAdmissionDisposition::Duplicate:
    return "DUPLICATE";
  case StreamNameMapAdmissionDisposition::Quarantined:
    return "QUARANTINED";
  case StreamNameMapAdmissionDisposition::Rejected:
    return "REJECTED";
  case StreamNameMapAdmissionDisposition::FatalSession:
    return "FATAL_SESSION";
  }
  return "REJECTED";
}

const char*
toString(StreamNameMapTiming timing)
{
  switch (timing) {
  case StreamNameMapTiming::Unclassified:
    return "UNCLASSIFIED";
  case StreamNameMapTiming::Ahead:
    return "AHEAD";
  case StreamNameMapTiming::Late:
    return "LATE";
  }
  return "UNCLASSIFIED";
}

bool
StreamNameMapAdmissionResult::accepted() const
{
  return disposition == StreamNameMapAdmissionDisposition::Admitted ||
         disposition == StreamNameMapAdmissionDisposition::Duplicate ||
         disposition == StreamNameMapAdmissionDisposition::Quarantined;
}

bool
StreamNameMapAdmissionResult::fatal() const
{
  return disposition == StreamNameMapAdmissionDisposition::FatalSession;
}

bool
StreamNameMapResolution::schedulable() const
{
  return !tombstone && !terminalUnproduced && !originalName.empty();
}

bool
StreamNameMapResolution::hasGroupBinding() const
{
  return !tombstone && !groupId.empty() && !sampleClass.empty() &&
         predictedSourceItems > 0 && groupItemIndex < predictedGroupItems();
}

uint64_t
StreamNameMapResolution::predictedGroupItems() const
{
  if (predictedSourceItems > std::numeric_limits<uint64_t>::max() -
                               predictedRepairItems) return 0;
  return predictedSourceItems + predictedRepairItems;
}

std::optional<std::string>
StreamNameResolverState::validateConfiguration(
  const StreamNameMapResolverConfig& config,
  const StreamNameMapCheckpoint& checkpoint) const
{
  if (config.contractVersion != STREAM_NAME_MAP_CONTRACT_VERSION_V1 &&
      config.contractVersion != STREAM_NAME_MAP_CONTRACT_VERSION_V2) {
    return "unsupported-contract-version";
  }
  if (config.streamId.empty() || config.streamId.size() > 256) {
    return "invalid-stream-id";
  }
  if (config.sessionEpoch == 0 || config.mappingVersion == 0) {
    return "invalid-session-or-version";
  }
  if (config.blockCapacity == 0 ||
      config.blockCapacity > STREAM_NAME_MAP_MAX_BLOCK_CAPACITY) {
    return "invalid-block-capacity";
  }
  if (config.expectedProvider.empty() || config.mappingRoot.empty() ||
      config.payloadPrefix.empty()) {
    return "missing-name-context";
  }
  if (config.mappingRoot != makeStreamNameMapRoot(config.expectedProvider,
                                                   config.streamId)) {
    return "wrong-mapping-root";
  }
  if (!config.expectedProvider.isPrefixOf(config.payloadPrefix) ||
      config.payloadPrefix.size() <= config.expectedProvider.size() ||
      !config.payloadPrefix[-1].isVersion() ||
      config.payloadPrefix[-1].toVersion() != config.mappingVersion) {
    return "invalid-versioned-payload-prefix";
  }
  if (config.signedWireCap == 0 ||
      config.signedWireCap > ndn::MAX_NDN_PACKET_SIZE) {
    return "invalid-signed-wire-cap";
  }
  if (config.maxVerifiedBlocks == 0 || config.maxQuarantineBlocks == 0 ||
      config.maxVerifiedBlocks > STREAM_NAME_MAP_MAX_RESOLVER_BLOCKS ||
      config.maxQuarantineBlocks > STREAM_NAME_MAP_MAX_RESOLVER_BLOCKS ||
      config.maxReverseEntries == 0 ||
      config.maxReverseEntries > STREAM_NAME_MAP_MAX_REVERSE_ENTRIES ||
      config.maxOriginalNameWireBytes == 0 ||
      config.maxOriginalNameWireBytes > ndn::MAX_NDN_PACKET_SIZE) {
    return "invalid-cache-bound";
  }
  if (const auto error = checkpoint.frontiers.validate(config.blockCapacity,
                                                        checkpoint.blockNumber)) {
    return error;
  }
  const auto oldestBlock = checkpoint.frontiers.oldestRetained /
                           config.blockCapacity;
  const auto committedBlock = checkpoint.frontiers.mappingCommittedThrough /
                              config.blockCapacity;
  const auto retainedBlockCount = committedBlock - oldestBlock + 1;
  if (retainedBlockCount > config.maxVerifiedBlocks) {
    return "verified-cache-too-small-for-frontier";
  }
  if (retainedBlockCount > config.maxReverseEntries / config.blockCapacity) {
    return "reverse-cache-too-small-for-frontier";
  }
  return std::nullopt;
}

void
StreamNameResolverState::reset(const StreamNameMapResolverConfig& config,
                               const StreamNameMapCheckpoint& checkpoint)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (const auto error = validateConfiguration(config, checkpoint)) {
    throw std::invalid_argument("invalid StreamNameMap resolver context: " + *error);
  }
  if (m_initialized) {
    if (config.sessionEpoch == m_config.sessionEpoch) {
      throw std::invalid_argument(
        "invalid StreamNameMap resolver context: reused-session-epoch");
    }
    if (config.mappingVersion == m_config.mappingVersion ||
        config.payloadPrefix == m_config.payloadPrefix) {
      throw std::invalid_argument(
        "invalid StreamNameMap resolver context: reused-session-namespace");
    }
  }
  m_initialized = true;
  m_faulted = false;
  m_config = config;
  m_checkpoint = checkpoint;
  m_blocks.clear();
  m_admittedBlockDigests.clear();
  m_nameReservations.clear();
  m_connectedBlocks.clear();
  m_bindings.clear();
  m_reverseBindings.clear();
  m_terminalUnproduced.clear();
  m_diagnostics.clear();
}

std::optional<std::string>
StreamNameResolverState::rebuild(
  const std::map<uint64_t, StoredBlock>& blocks,
  const StreamNameMapCheckpoint& checkpoint,
  RebuiltState& state) const
{
  state = RebuiltState{};

  // Validate all immutable names, including quarantined blocks, before any
  // entry becomes visible. This prevents a later block from reserving a name
  // that is already bound elsewhere in the same mapping version.
  std::map<ndn::Name, StreamCursor> allNames;
  struct ObservedGroup
  {
    std::string sampleClass;
    uint64_t sources = 0;
    uint64_t repairs = 0;
    std::map<uint64_t, StreamCursor> indexes;
  };
  std::map<std::string, ObservedGroup> allGroups;
  for (const auto& [blockNumber, stored] : blocks) {
    if (blockNumber != stored.block.blockNumber) {
      return "block-key-mismatch";
    }
    for (size_t slot = 0; slot < stored.block.entries.size(); ++slot) {
      const auto& entry = stored.block.entries[slot];
      if (entry.tombstone) {
        continue;
      }
      if (!m_config.payloadPrefix.isPrefixOf(entry.originalName) ||
          entry.originalName.size() <= m_config.payloadPrefix.size()) {
        return "original-name-outside-prefix";
      }
      if (entry.originalName.wireEncode().size() >
          m_config.maxOriginalNameWireBytes) {
        return "original-name-too-large";
      }
      const auto cursor = stored.block.firstCursor + slot;
      if (!allNames.emplace(entry.originalName, cursor).second) {
        return "original-name-reuse";
      }
      if (m_config.contractVersion == STREAM_NAME_MAP_CONTRACT_VERSION_V2) {
        auto [groupIt, inserted] = allGroups.emplace(
          entry.groupId,
          ObservedGroup{entry.sampleClass, entry.predictedSourceItems,
                        entry.predictedRepairItems, {}});
        if (!inserted &&
            (groupIt->second.sampleClass != entry.sampleClass ||
             groupIt->second.sources != entry.predictedSourceItems ||
             groupIt->second.repairs != entry.predictedRepairItems)) {
          return "cross-block-group-conflict";
        }
        if (!groupIt->second.indexes.emplace(entry.groupItemIndex, cursor).second) {
          return "duplicate-group-item-index";
        }
      }
    }
  }
  for (const auto& [groupId, group] : allGroups) {
    static_cast<void>(groupId);
    if (group.indexes.size() < 2) continue;
    for (auto it = std::next(group.indexes.begin()); it != group.indexes.end(); ++it) {
      const auto previous = std::prev(it);
      if (it->first != previous->first + 1 || it->second <= previous->second) {
        return "non-canonical-group-order";
      }
    }
  }

  // An adjacent pair has exactly one legal continuity edge regardless of
  // arrival order. Detect a fork before exposing either side.
  for (auto it = blocks.begin(); it != blocks.end(); ++it) {
    if (it->first == std::numeric_limits<uint64_t>::max()) {
      continue;
    }
    const auto next = blocks.find(it->first + 1);
    if (next == blocks.end()) {
      continue;
    }
    if (!next->second.block.previousContentDigest ||
        *next->second.block.previousContentDigest != it->second.digest) {
      return "continuity-fork";
    }
  }

  const auto anchor = blocks.find(checkpoint.blockNumber);
  if (anchor != blocks.end()) {
    if (anchor->second.digest != checkpoint.contentDigest) {
      return "checkpoint-digest-mismatch";
    }
    state.connectedBlocks.insert(checkpoint.blockNumber);
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& [blockNumber, stored] : blocks) {
      if (state.connectedBlocks.count(blockNumber) != 0) {
        continue;
      }
      if (blockNumber > 0 &&
          state.connectedBlocks.count(blockNumber - 1) != 0) {
        const auto& predecessor = blocks.at(blockNumber - 1);
        if (!stored.block.previousContentDigest ||
            *stored.block.previousContentDigest != predecessor.digest) {
          return "continuity-fork";
        }
        state.connectedBlocks.insert(blockNumber);
        changed = true;
        continue;
      }
      if (blockNumber < std::numeric_limits<uint64_t>::max() &&
          state.connectedBlocks.count(blockNumber + 1) != 0) {
        const auto& successor = blocks.at(blockNumber + 1);
        if (!successor.block.previousContentDigest ||
            *successor.block.previousContentDigest != stored.digest) {
          return "continuity-fork";
        }
        state.connectedBlocks.insert(blockNumber);
        changed = true;
      }
    }
  }

  for (const auto blockNumber : state.connectedBlocks) {
    const auto& stored = blocks.at(blockNumber);
    for (size_t slot = 0; slot < stored.block.entries.size(); ++slot) {
      const auto cursor = stored.block.firstCursor + slot;
      if (cursor < checkpoint.frontiers.oldestRetained ||
          cursor > checkpoint.frontiers.mappingCommittedThrough) {
        continue;
      }
      const auto& entry = stored.block.entries[slot];
      StreamNameMapResolution resolution;
      resolution.cursor = cursor;
      resolution.originalName = entry.originalName;
      resolution.tombstone = entry.tombstone;
      resolution.timing = stored.timing;
      resolution.groupId = entry.groupId;
      resolution.sampleClass = entry.sampleClass;
      resolution.groupItemIndex = entry.groupItemIndex;
      resolution.predictedSourceItems = entry.predictedSourceItems;
      resolution.predictedRepairItems = entry.predictedRepairItems;
      state.bindings.emplace(cursor, resolution);
      if (!entry.tombstone) {
        state.reverseBindings.emplace(entry.originalName, cursor);
      }
    }
  }
  return std::nullopt;
}

StreamNameMapAdmissionResult
StreamNameResolverState::reject(std::string reason, bool fatal,
                                StreamNameMapTiming timing)
{
  ++m_diagnostics[reason];
  if (fatal) {
    m_faulted = true;
  }
  StreamNameMapAdmissionResult result;
  result.disposition = fatal ? StreamNameMapAdmissionDisposition::FatalSession
                             : StreamNameMapAdmissionDisposition::Rejected;
  result.timing = timing;
  result.reason = std::move(reason);
  result.mappingCommittedThrough = m_checkpoint.frontiers.mappingCommittedThrough;
  return result;
}

void
StreamNameResolverState::install(std::map<uint64_t, StoredBlock> blocks,
                                 RebuiltState state)
{
  for (auto cursor = m_terminalUnproduced.begin();
       cursor != m_terminalUnproduced.end();) {
    const auto binding = state.bindings.find(*cursor);
    if (*cursor < m_checkpoint.frontiers.oldestRetained ||
        *cursor > m_checkpoint.frontiers.mappingCommittedThrough ||
        (binding != state.bindings.end() && binding->second.tombstone)) {
      cursor = m_terminalUnproduced.erase(cursor);
    }
    else if (binding != state.bindings.end()) {
      binding->second.terminalUnproduced = true;
      ++cursor;
    }
    else {
      // Preserve the terminal marker across local cache eviction so refetching
      // the immutable Mapping cannot accidentally make the name schedulable.
      ++cursor;
    }
  }
  m_blocks = std::move(blocks);
  m_connectedBlocks = std::move(state.connectedBlocks);
  m_bindings = std::move(state.bindings);
  m_reverseBindings = std::move(state.reverseBindings);
}

StreamNameMapAdmissionResult
StreamNameResolverState::admitVerifiedBlock(
  const VerifiedStreamNameMapData& input)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_initialized) {
    return reject("resolver-not-initialized", false);
  }
  if (m_faulted) {
    return reject("session-already-faulted", true);
  }
  if (input.verifiedProvider != m_config.expectedProvider) {
    return reject("wrong-provider", true);
  }
  if (input.contentType != ndn::tlv::ContentType_Manifest) {
    return reject("wrong-content-type", false);
  }
  if (input.hasFinalBlock) {
    return reject("final-block-not-allowed", false);
  }
  if (!input.content.isValid() || input.content.type() != ndn::tlv::Content) {
    return reject("invalid-content-envelope", false);
  }
  if (input.signedWireSize == 0 ||
      input.signedWireSize > m_config.signedWireCap ||
      input.signedWireSize > ndn::MAX_NDN_PACKET_SIZE ||
      input.content.size() > input.signedWireSize) {
    return reject("signed-wire-cap-exceeded", false);
  }

  StreamNameMapTiming timing = StreamNameMapTiming::Unclassified;
  if (input.requiredBeforeMonotonicMs != 0) {
    timing = input.receivedMonotonicMs < input.requiredBeforeMonotonicMs
               ? StreamNameMapTiming::Ahead
               : StreamNameMapTiming::Late;
  }

  StreamNameMapBlock block;
  try {
    input.content.parse();
    if (input.content.elements().size() != 1 ||
        !block.wireDecode(input.content.elements().front()) ||
        !hasSameWire(block.canonicalContent(), input.content)) {
      return reject("malformed-or-noncanonical-content", false, timing);
    }
  }
  catch (const std::exception&) {
    return reject("malformed-or-noncanonical-content", false, timing);
  }

  if (block.contractVersion != m_config.contractVersion) {
    return reject("stale-contract-version", true, timing);
  }
  if (block.streamId != m_config.streamId) {
    return reject("stale-stream", true, timing);
  }
  if (block.sessionEpoch != m_config.sessionEpoch) {
    return reject("stale-session", true, timing);
  }
  if (block.mappingVersion != m_config.mappingVersion) {
    return reject("stale-mapping-version", true, timing);
  }
  if (block.blockCapacity != m_config.blockCapacity) {
    return reject("capacity-mismatch", false, timing);
  }
  const auto expectedName = makeStreamNameMapBlockName(
    m_config.mappingRoot, m_config.mappingVersion, block.blockNumber);
  if (input.dataName != expectedName) {
    return reject("wrong-control-name", false, timing);
  }
  if (block.lastCursor() < m_checkpoint.frontiers.oldestRetained) {
    return reject("stale-block", false, timing);
  }
  const auto blockDigest = block.contentDigest();
  const auto admittedDigest = m_admittedBlockDigests.find(block.blockNumber);
  if (admittedDigest != m_admittedBlockDigests.end() &&
      admittedDigest->second != blockDigest) {
    return reject("same-name-different-content", true, timing);
  }
  const auto existing = m_blocks.find(block.blockNumber);
  if (existing != m_blocks.end()) {
    if (existing->second.digest == blockDigest &&
        hasSameWire(existing->second.block.canonicalContent(),
                    block.canonicalContent())) {
      ++m_diagnostics["duplicate"];
      StreamNameMapAdmissionResult result;
      result.disposition = StreamNameMapAdmissionDisposition::Duplicate;
      result.timing = existing->second.timing;
      result.reason = "duplicate";
      result.mappingCommittedThrough = m_checkpoint.frontiers.mappingCommittedThrough;
      return result;
    }
    return reject("same-name-different-content", true, timing);
  }

  const bool isRefetch = admittedDigest != m_admittedBlockDigests.end();
  if (!isRefetch) {
    if (m_admittedBlockDigests.size() >=
        m_config.maxReverseEntries / m_config.blockCapacity) {
      return reject("tracked-block-cache-full", false, timing);
    }
    size_t newNames = 0;
    for (size_t slot = 0; slot < block.entries.size(); ++slot) {
      const auto& entry = block.entries[slot];
      if (entry.tombstone) {
        continue;
      }
      ++newNames;
      const auto reservation = m_nameReservations.find(entry.originalName);
      if (reservation != m_nameReservations.end() &&
          reservation->second != block.firstCursor + slot) {
        return reject("original-name-reuse", true, timing);
      }
    }
    if (newNames > m_config.maxReverseEntries -
                     std::min(m_config.maxReverseEntries,
                              m_nameReservations.size())) {
      return reject("reverse-cache-full", false, timing);
    }
  }

  StoredBlock stored;
  stored.block = block;
  stored.digest = blockDigest;
  stored.dataName = input.dataName;
  stored.timing = timing;

  // The ordinary live-stream case is one authenticated successor of the
  // current checkpoint with no quarantined gap. Rebuilding every retained
  // block for that case makes Mapping admission O(retained blocks) per sample
  // and can consume the Face thread as a high-rate stream grows. Preserve the
  // full rebuild path below for out-of-order, fork, and quarantine handling,
  // but commit a strict successor incrementally after checking the same
  // continuity, name-reuse, and grouped-entry invariants.
  bool isStrictConnectedSuccessor =
    m_checkpoint.blockNumber < std::numeric_limits<uint64_t>::max() &&
    m_checkpoint.frontiers.mappingCommittedThrough <
      std::numeric_limits<uint64_t>::max() &&
    block.blockNumber == m_checkpoint.blockNumber + 1 &&
    block.firstCursor == m_checkpoint.frontiers.mappingCommittedThrough + 1 &&
    block.previousContentDigest &&
    *block.previousContentDigest == m_checkpoint.contentDigest &&
    m_blocks.size() == m_connectedBlocks.size();
  if (isStrictConnectedSuccessor &&
      m_config.contractVersion == STREAM_NAME_MAP_CONTRACT_VERSION_V2 &&
      !block.entries.empty() && !block.entries.front().tombstone &&
      block.entries.front().groupItemIndex > 0 &&
      m_bindings.count(block.firstCursor - 1) == 0) {
    // The descriptor checkpoint may bisect a group whose preceding Mapping
    // block is not locally cached. The full rebuild path retains the existing
    // quarantine/continuity semantics for that uncommon join boundary.
    isStrictConnectedSuccessor = false;
  }
  if (isStrictConnectedSuccessor) {
    std::map<StreamCursor, StreamNameMapResolution> additions;
    for (size_t slot = 0; slot < block.entries.size(); ++slot) {
      const auto cursor = block.firstCursor + slot;
      const auto& entry = block.entries[slot];
      StreamNameMapResolution resolution;
      resolution.cursor = cursor;
      resolution.originalName = entry.originalName;
      resolution.tombstone = entry.tombstone;
      resolution.timing = timing;
      resolution.groupId = entry.groupId;
      resolution.sampleClass = entry.sampleClass;
      resolution.groupItemIndex = entry.groupItemIndex;
      resolution.predictedSourceItems = entry.predictedSourceItems;
      resolution.predictedRepairItems = entry.predictedRepairItems;

      if (m_config.contractVersion == STREAM_NAME_MAP_CONTRACT_VERSION_V2 &&
          !entry.tombstone) {
        if (entry.groupItemIndex == 0) {
          const auto reused = std::find_if(
            m_bindings.begin(), m_bindings.end(),
            [&entry] (const auto& item) {
              return item.second.hasGroupBinding() &&
                     item.second.groupId == entry.groupId;
            });
          const auto reusedInBlock = std::find_if(
            additions.begin(), additions.end(),
            [&entry] (const auto& item) {
              return item.second.hasGroupBinding() &&
                     item.second.groupId == entry.groupId;
            });
          if (reused != m_bindings.end() || reusedInBlock != additions.end()) {
            return reject("duplicate-group-item-index", true, timing);
          }
        }
        else {
          const auto previousCursor = cursor - 1;
          const auto addedPrevious = additions.find(previousCursor);
          const auto retainedPrevious = m_bindings.find(previousCursor);
          const StreamNameMapResolution* previous = nullptr;
          if (addedPrevious != additions.end()) {
            previous = &addedPrevious->second;
          }
          else if (retainedPrevious != m_bindings.end()) {
            previous = &retainedPrevious->second;
          }
          if (previous == nullptr || !previous->hasGroupBinding() ||
              previous->groupId != entry.groupId ||
              previous->sampleClass != entry.sampleClass ||
              previous->predictedSourceItems != entry.predictedSourceItems ||
              previous->predictedRepairItems != entry.predictedRepairItems ||
              previous->groupItemIndex + 1 != entry.groupItemIndex) {
            return reject("non-canonical-group-order", true, timing);
          }
        }
      }
      additions.emplace(cursor, std::move(resolution));
    }

    m_blocks.emplace(block.blockNumber, std::move(stored));
    m_connectedBlocks.insert(block.blockNumber);
    for (auto& [cursor, resolution] : additions) {
      if (resolution.terminalUnproduced ||
          m_terminalUnproduced.count(cursor) != 0) {
        resolution.terminalUnproduced = true;
      }
      if (!resolution.tombstone) {
        m_reverseBindings.emplace(resolution.originalName, cursor);
      }
      m_bindings.emplace(cursor, std::move(resolution));
    }
    m_checkpoint.blockNumber = block.blockNumber;
    m_checkpoint.contentDigest = blockDigest;
    m_checkpoint.frontiers.mappingCommittedThrough = block.lastCursor();
    m_checkpoint.frontiers.nextReserved = block.lastCursor() + 1;

    while (m_connectedBlocks.size() > m_config.maxVerifiedBlocks) {
      const auto oldest = m_blocks.begin();
      if (oldest == m_blocks.end()) break;
      for (size_t slot = 0; slot < oldest->second.block.entries.size(); ++slot) {
        const auto cursor = oldest->second.block.firstCursor + slot;
        const auto binding = m_bindings.find(cursor);
        if (binding != m_bindings.end() && !binding->second.tombstone) {
          const auto reverse = m_reverseBindings.find(
            binding->second.originalName);
          if (reverse != m_reverseBindings.end() &&
              reverse->second == cursor) {
            m_reverseBindings.erase(reverse);
          }
        }
        m_bindings.erase(cursor);
      }
      m_connectedBlocks.erase(oldest->first);
      m_blocks.erase(oldest);
    }

    if (!isRefetch) {
      m_admittedBlockDigests.emplace(block.blockNumber, blockDigest);
      for (size_t slot = 0; slot < block.entries.size(); ++slot) {
        const auto& entry = block.entries[slot];
        if (!entry.tombstone) {
          m_nameReservations.emplace(entry.originalName,
                                     block.firstCursor + slot);
        }
      }
    }
    ++m_diagnostics["incremental-admitted"];
    StreamNameMapAdmissionResult result;
    result.disposition = StreamNameMapAdmissionDisposition::Admitted;
    result.timing = timing;
    result.reason = "admitted";
    result.stateChanged = true;
    result.mappingCommittedThrough =
      m_checkpoint.frontiers.mappingCommittedThrough;
    ++m_diagnostics[result.reason];
    return result;
  }

  auto candidate = m_blocks;
  candidate.emplace(block.blockNumber, std::move(stored));

  auto candidateCheckpoint = m_checkpoint;
  RebuiltState rebuilt;
  if (const auto error = rebuild(candidate, m_checkpoint, rebuilt)) {
    return reject(*error, true, timing);
  }
  const auto newestConnected = rebuilt.connectedBlocks.empty() ?
    candidateCheckpoint.blockNumber : *rebuilt.connectedBlocks.rbegin();
  const auto newest = candidate.find(newestConnected);
  if (newest != candidate.end() &&
      newest->second.block.lastCursor() >
        candidateCheckpoint.frontiers.mappingCommittedThrough) {
    // A Provider-signed block may advance the live Mapping frontier only after
    // its digest chain has connected to the descriptor-pinned checkpoint.
    candidateCheckpoint.blockNumber = newestConnected;
    candidateCheckpoint.contentDigest = newest->second.digest;
    candidateCheckpoint.frontiers.mappingCommittedThrough =
      newest->second.block.lastCursor();
    candidateCheckpoint.frontiers.nextReserved =
      newest->second.block.lastCursor() + 1;
    if (const auto error = rebuild(candidate, candidateCheckpoint, rebuilt)) {
      return reject(*error, true, timing);
    }
  }
  auto connectedCount = rebuilt.connectedBlocks.size();
  if (connectedCount > m_config.maxVerifiedBlocks) {
    const auto firstKept = block.blockNumber + 1 > m_config.maxVerifiedBlocks ?
      block.blockNumber + 1 - m_config.maxVerifiedBlocks : 0;
    for (auto it = candidate.begin(); it != candidate.end();) {
      if (it->first < firstKept) it = candidate.erase(it);
      else ++it;
    }
    if (const auto error = rebuild(candidate, candidateCheckpoint, rebuilt)) {
      return reject(*error, true, timing);
    }
    connectedCount = rebuilt.connectedBlocks.size();
  }
  const auto quarantineCount = candidate.size() - connectedCount;
  if (quarantineCount > m_config.maxQuarantineBlocks) {
    return reject("quarantine-cache-full", false, timing);
  }

  const bool connected = rebuilt.connectedBlocks.count(block.blockNumber) != 0;
  m_checkpoint = candidateCheckpoint;
  install(std::move(candidate), std::move(rebuilt));
  if (!isRefetch) {
    m_admittedBlockDigests.emplace(block.blockNumber, blockDigest);
    for (size_t slot = 0; slot < block.entries.size(); ++slot) {
      const auto& entry = block.entries[slot];
      if (!entry.tombstone) {
        m_nameReservations.emplace(entry.originalName,
                                   block.firstCursor + slot);
      }
    }
  }
  StreamNameMapAdmissionResult result;
  result.disposition = connected ? StreamNameMapAdmissionDisposition::Admitted
                                 : StreamNameMapAdmissionDisposition::Quarantined;
  result.timing = timing;
  result.reason = connected ? "admitted" : "awaiting-continuity";
  result.stateChanged = true;
  result.mappingCommittedThrough = m_checkpoint.frontiers.mappingCommittedThrough;
  ++m_diagnostics[result.reason];
  return result;
}

StreamNameMapAdmissionResult
StreamNameResolverState::refreshCheckpoint(
  const StreamNameMapCheckpoint& checkpoint)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_initialized) {
    return reject("resolver-not-initialized", false);
  }
  if (m_faulted) {
    return reject("session-already-faulted", true);
  }
  if (const auto error = validateConfiguration(m_config, checkpoint)) {
    return reject(*error, false);
  }
  const auto& old = m_checkpoint.frontiers;
  const auto& next = checkpoint.frontiers;
  if (next.oldestRetained < old.oldestRetained ||
      next.latestJoin < old.latestJoin ||
      next.latestProduced < old.latestProduced ||
      next.mappingCommittedThrough < old.mappingCommittedThrough ||
      next.nextReserved < old.nextReserved ||
      checkpoint.blockNumber < m_checkpoint.blockNumber) {
    return reject("frontier-regression", false);
  }
  if (checkpoint.blockNumber == m_checkpoint.blockNumber &&
      checkpoint.contentDigest != m_checkpoint.contentDigest) {
    return reject("checkpoint-equivocation", true);
  }
  if (checkpoint.blockNumber > m_checkpoint.blockNumber) {
    const auto anchor = m_blocks.find(checkpoint.blockNumber);
    if (anchor == m_blocks.end() ||
        m_connectedBlocks.count(checkpoint.blockNumber) == 0) {
      return reject("checkpoint-anchor-not-verified", false);
    }
    if (anchor->second.digest != checkpoint.contentDigest) {
      return reject("checkpoint-equivocation", true);
    }
  }

  auto candidate = m_blocks;
  for (auto it = candidate.begin(); it != candidate.end();) {
    if (it->second.block.lastCursor() < next.oldestRetained) {
      it = candidate.erase(it);
    }
    else {
      ++it;
    }
  }

  RebuiltState rebuilt;
  if (const auto error = rebuild(candidate, checkpoint, rebuilt)) {
    return reject(*error, true);
  }
  if (rebuilt.connectedBlocks.size() > m_config.maxVerifiedBlocks ||
      candidate.size() - rebuilt.connectedBlocks.size() >
        m_config.maxQuarantineBlocks) {
    return reject("checkpoint-cache-bound", false);
  }

  auto admittedBlockDigests = m_admittedBlockDigests;
  for (auto it = admittedBlockDigests.begin(); it != admittedBlockDigests.end();) {
    if (it->first <= std::numeric_limits<uint64_t>::max() /
                       m_config.blockCapacity &&
        it->first * m_config.blockCapacity + (m_config.blockCapacity - 1) <
          next.oldestRetained) {
      it = admittedBlockDigests.erase(it);
    }
    else {
      ++it;
    }
  }
  m_checkpoint = checkpoint;
  install(std::move(candidate), std::move(rebuilt));
  m_admittedBlockDigests = std::move(admittedBlockDigests);
  ++m_diagnostics["checkpoint-refreshed"];
  StreamNameMapAdmissionResult result;
  result.disposition = StreamNameMapAdmissionDisposition::Admitted;
  result.reason = "checkpoint-refreshed";
  result.stateChanged = true;
  result.mappingCommittedThrough = m_checkpoint.frontiers.mappingCommittedThrough;
  return result;
}

std::optional<StreamNameMapResolution>
StreamNameResolverState::lookup(StreamCursor cursor) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_initialized || m_faulted) {
    return std::nullopt;
  }
  const auto found = m_bindings.find(cursor);
  return found == m_bindings.end() ? std::nullopt
                                   : std::optional<StreamNameMapResolution>(found->second);
}

std::optional<ndn::Name>
StreamNameResolverState::resolve(StreamCursor cursor) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_initialized || m_faulted) {
    return std::nullopt;
  }
  const auto found = m_bindings.find(cursor);
  if (found == m_bindings.end() || !found->second.schedulable()) {
    return std::nullopt;
  }
  return found->second.originalName;
}

std::optional<StreamCursor>
StreamNameResolverState::reverseResolve(const ndn::Name& originalName) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_initialized || m_faulted) {
    return std::nullopt;
  }
  const auto found = m_reverseBindings.find(originalName);
  return found == m_reverseBindings.end() ? std::nullopt
                                          : std::optional<StreamCursor>(found->second);
}

bool
StreamNameResolverState::markTerminalUnproduced(StreamCursor cursor)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_initialized || m_faulted) {
    return false;
  }
  auto found = m_bindings.find(cursor);
  if (found == m_bindings.end() || found->second.tombstone ||
      found->second.originalName.empty()) {
    return false;
  }
  found->second.terminalUnproduced = true;
  m_terminalUnproduced.insert(cursor);
  ++m_diagnostics["terminal-unproduced"];
  return true;
}

bool
StreamNameResolverState::evictLocalBlock(uint64_t blockNumber)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_initialized || m_faulted || m_blocks.count(blockNumber) == 0) {
    return false;
  }
  auto candidate = m_blocks;
  candidate.erase(blockNumber);
  RebuiltState rebuilt;
  if (rebuild(candidate, m_checkpoint, rebuilt)) {
    return false;
  }
  if (rebuilt.connectedBlocks.size() > m_config.maxVerifiedBlocks ||
      candidate.size() - rebuilt.connectedBlocks.size() >
        m_config.maxQuarantineBlocks) {
    return false;
  }
  install(std::move(candidate), std::move(rebuilt));
  ++m_diagnostics["local-cache-eviction"];
  return true;
}

StreamCursorFrontiers
StreamNameResolverState::frontiers() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_checkpoint.frontiers;
}

StreamNameMapCheckpoint
StreamNameResolverState::checkpoint() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_checkpoint;
}

bool
StreamNameResolverState::faulted() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_faulted;
}

size_t
StreamNameResolverState::verifiedBlockCount() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_connectedBlocks.size();
}

size_t
StreamNameResolverState::quarantinedBlockCount() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_blocks.size() - m_connectedBlocks.size();
}

size_t
StreamNameResolverState::bindingCount() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_bindings.size();
}

std::map<std::string, uint64_t>
StreamNameResolverState::diagnostics() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_diagnostics;
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
  m_metrics = {};
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
  m_metrics.maxPending = std::max<uint64_t>(m_metrics.maxPending,
                                             m_pending.size());

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
StreamAdaptiveFetcherState::observePayloadDelay(double sampleMs,
                                                bool aheadOfJoinCheckpoint)
{
  if (!std::isfinite(sampleMs) || sampleMs <= 0.0) {
    ++m_invalidObservations;
    return;
  }

  // Before the live edge is stable, an ahead-mapped Interest measures
  // effective delay (DRD') and may have spent most of its lifetime waiting for
  // production.  Without adding consumer-specific dgen to immutable Data, the
  // sample is only a safe upper bound on network DRD: use it to correct an
  // overestimate downward, never to manufacture a path-delay increase. Even
  // after Fetching stabilizes, a bounded lookahead can retain non-zero dgen;
  // without a producer timestamp it is still unsafe to feed DRD' upward into
  // the network RTT estimator.
  if (!aheadOfJoinCheckpoint) {
    if (sampleMs < rttMs) rttMs = sampleMs;
    else observeRtt(sampleMs);
    // Authenticated Data known to be produced has no generation wait and is
    // the evidence that may refine the startup floor. This lets a retained
    // warm-up cohort replace the conservative 100 ms seed before live future
    // Interests begin, while cache-only future hits still cannot collapse it.
    m_liveRttFloorMs = std::min(m_liveRttFloorMs, rttMs);
  }
  else if (sampleMs < rttMs) {
    observeRtt(sampleMs);
    // A retry may be satisfied by a downstream Content Store and therefore
    // measure only local cache delay. During live-edge search that is not a
    // new end-to-end path RTT sample. Preserve the session's measured startup
    // floor until Fetching, where normal bidirectional adaptation resumes.
    rttMs = std::max(rttMs, m_liveRttFloorMs);
  }
}

void
StreamAdaptiveFetcherState::recordTimeout()
{
  timeoutPressure = std::min(1.0, timeoutPressure + 0.25);
}

void
StreamAdaptiveFetcherState::recordTimeout(uint64_t, bool knownProduced, bool wasFuture)
{
  if (!knownProduced && wasFuture) {
    m_futureWait = true;
    ++m_futureWaitCount;
    return;
  }
  recordTimeout();
}

void
StreamAdaptiveFetcherState::recordNack()
{
  nackPressure = std::min(1.0, nackPressure + 0.2);
}

void
StreamAdaptiveFetcherState::recordNack(uint64_t, const std::string& reason)
{
  recordNack();
  if (reason == "congestion" || reason == "CONGESTION") {
    m_congestionHoldUntilMs = std::max(
      m_congestionHoldUntilMs,
      m_lastSampleArrivalMs + std::max<uint64_t>(1000, detectionPeriodMs));
    if (m_liveMode && m_phase == StreamPrefetchPhase::Fetching) {
      m_phase = StreamPrefetchPhase::Adjusting;
    }
  }
}

void
StreamAdaptiveFetcherState::recordCongestionMark(uint64_t cursor, uint64_t mark)
{
  if (mark != 0) {
    recordNack(cursor, "congestion");
  }
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

void
StreamAdaptiveFetcherState::resetLive(uint64_t sessionEpoch, uint64_t nextSeq,
                                      double samplePeriodMs, uint64_t nowMs)
{
  if (sessionEpoch == 0 || samplePeriodMs <= 0.0 || !std::isfinite(samplePeriodMs)) {
    throw std::invalid_argument("live prefetch requires a session and positive sample period");
  }
  m_liveMode = true;
  m_mappedLive = false;
  m_mappedLivePolicyEnabled = true;
  m_sessionEpoch = sessionEpoch;
  m_nextSeq = nextSeq;
  m_latestProducedCursor = nextSeq;
  m_mappingCommittedThroughCursor = nextSeq;
  m_nextReservedCursor = nextSeq == std::numeric_limits<uint64_t>::max()
    ? nextSeq : nextSeq + 1;
  m_mappingInFlight = 0;
  m_payloadInFlight = 0;
  m_retransmissionInFlight = 0;
  m_futureWaitCount = 0;
  m_terminalUnproducedAdvice = 0;
  m_laterCursorAdvice = 0;
  m_congestionHoldUntilMs = 0;
  m_recoveryStartedMs = 0;
  m_recoveryDeadlineMs = 0;
  m_futureWait = false;
  m_samplePeriodMs = samplePeriodMs;
  m_liveRttFloorMs = std::max(1.0, rttMs);
  m_segmentsPerSample = 1.0;
  m_predictedSampleGroups.clear();
  m_phase = StreamPrefetchPhase::Chasing;
  m_lastSampleId = 0;
  m_lastSampleArrivalMs = 0;
  m_hasLastSample = false;
  m_sampleArrivalPeriods.clear();
  m_consecutiveStable = 0;
  m_lastActionMs = nowMs;
  m_liveWindow = std::min(maxWindow, std::max(minWindow, baseWindow));
  m_previousUsableWindow = 0;
  m_invalidObservations = 0;
}

void
StreamAdaptiveFetcherState::configureMappedLive(uint64_t aggregateLimit,
                                                 uint64_t mapReserve,
                                                 uint64_t retransmitReserve,
                                                 uint64_t blockCapacity,
                                                 std::string profile)
{
  if (aggregateLimit == 0 || blockCapacity == 0 ||
      retransmitReserve >= aggregateLimit ||
      mapReserve >= aggregateLimit - retransmitReserve ||
      chaseMultiplier < 1.0 || !std::isfinite(chaseMultiplier) ||
      adjustMultiplier <= 0.0 || adjustMultiplier > 1.0 ||
      !std::isfinite(adjustMultiplier) ||
      congestionDecreaseMultiplier <= 0.0 || congestionDecreaseMultiplier > 1.0 ||
      !std::isfinite(congestionDecreaseMultiplier)) {
    throw std::invalid_argument("invalid mapped live prefetch bounds");
  }

  if (profile == "ndnsf-fast-seed") {
    liveEdgeChangeThreshold = 0.60;
    liveEdgePeriodSimilarity = 0.50;
    liveEdgeWindow = 3;
    liveEdgeStableRequired = 4;
  }
  else if (profile == "ndnsf-balanced-seed") {
    liveEdgeChangeThreshold = 0.30;
    liveEdgePeriodSimilarity = 0.70;
    liveEdgeWindow = 10;
    liveEdgeStableRequired = 4;
  }
  else if (profile == "ndnsf-conservative-seed") {
    liveEdgeChangeThreshold = 0.10;
    liveEdgePeriodSimilarity = 0.95;
    liveEdgeWindow = 30;
    liveEdgeStableRequired = 4;
  }
  else if (profile == "paper-eq3-literal-low") {
    liveEdgeChangeThreshold = 0.60;
    liveEdgePeriodSimilarity = 0.50;
    liveEdgeWindow = 3;
    liveEdgeStableRequired = 4;
  }
  else if (profile == "paper-eq3-literal-medium") {
    liveEdgeChangeThreshold = 0.30;
    liveEdgePeriodSimilarity = 0.70;
    liveEdgeWindow = 10;
    liveEdgeStableRequired = 4;
  }
  else if (profile == "paper-eq3-literal-high") {
    liveEdgeChangeThreshold = 0.10;
    liveEdgePeriodSimilarity = 0.95;
    liveEdgeWindow = 30;
    liveEdgeStableRequired = 4;
  }
  else {
    throw std::invalid_argument("unknown live prefetch detector profile");
  }

  aggregateInFlightLimit = aggregateLimit;
  mappingReserve = mapReserve;
  retransmissionReserve = retransmitReserve;
  mappingBlockCapacity = blockCapacity;
  detectorProfile = std::move(profile);
}

void
StreamAdaptiveFetcherState::resetMappedLive(uint64_t sessionEpoch,
                                             uint64_t nextCursor,
                                             double samplePeriodMs,
                                             uint64_t latestProducedCursor,
                                             uint64_t mappingCommittedThroughCursor,
                                             uint64_t nextReservedCursor,
                                             uint64_t nowMs)
{
  if (nextCursor > latestProducedCursor ||
      latestProducedCursor > mappingCommittedThroughCursor ||
      mappingCommittedThroughCursor >= nextReservedCursor) {
    throw std::invalid_argument("invalid mapped live cursor frontiers");
  }
  resetLive(sessionEpoch, nextCursor, samplePeriodMs, nowMs);
  m_mappedLive = true;
  m_mappedLivePolicyEnabled = true;
  m_latestProducedCursor = latestProducedCursor;
  m_mappingCommittedThroughCursor = mappingCommittedThroughCursor;
  m_nextReservedCursor = nextReservedCursor;
}

void
StreamAdaptiveFetcherState::updateMappingFrontier(
  uint64_t mappingCommittedThroughCursor,
  uint64_t nextReservedCursor)
{
  if (!m_mappedLive ||
      mappingCommittedThroughCursor < m_mappingCommittedThroughCursor ||
      nextReservedCursor < m_nextReservedCursor ||
      mappingCommittedThroughCursor >= nextReservedCursor) {
    throw std::invalid_argument("mapping frontier is stale or invalid");
  }
  m_mappingCommittedThroughCursor = mappingCommittedThroughCursor;
  m_nextReservedCursor = nextReservedCursor;
}

void
StreamAdaptiveFetcherState::advanceNextCursor(uint64_t nextCursor)
{
  if (!m_liveMode || nextCursor < m_nextSeq ||
      // Equality is the valid mapping-starved boundary: every currently
      // reserved cursor is complete and the consumer is waiting for the next
      // signed Mapping block. Only advancing beyond that frontier is illegal.
      (m_mappedLive && nextCursor > m_nextReservedCursor)) {
    throw std::invalid_argument("next stream cursor is stale or unreserved");
  }
  m_nextSeq = nextCursor;
}

void
StreamAdaptiveFetcherState::setMappedLivePolicyEnabled(bool enabled)
{
  if (!m_mappedLive) {
    throw std::logic_error("mapped live policy requires mapped session state");
  }
  m_mappedLivePolicyEnabled = enabled;
  if (!enabled && m_phase != StreamPrefetchPhase::Stopped) {
    m_phase = StreamPrefetchPhase::Inactive;
  }
  else if (enabled && m_phase == StreamPrefetchPhase::Inactive) {
    m_phase = StreamPrefetchPhase::Chasing;
  }
}

void
StreamAdaptiveFetcherState::setPredictedSampleGroups(
  std::vector<uint64_t> groupItems)
{
  if (!m_mappedLive) {
    throw std::logic_error("predicted groups require mapped live state");
  }
  if (groupItems.size() > 1024 ||
      std::any_of(groupItems.begin(), groupItems.end(), [] (uint64_t value) {
        return value == 0 || value > 4096;
      })) {
    throw std::invalid_argument("invalid predicted sample groups");
  }
  m_predictedSampleGroups.assign(groupItems.begin(), groupItems.end());
}

void
StreamAdaptiveFetcherState::setInFlight(uint64_t mapping,
                                         uint64_t payload,
                                         uint64_t retransmission)
{
  if (mapping > aggregateInFlightLimit || payload > aggregateInFlightLimit ||
      retransmission > aggregateInFlightLimit ||
      payload > aggregateInFlightLimit - mapping ||
      retransmission > aggregateInFlightLimit - mapping - payload) {
    throw std::invalid_argument(
      "aggregate stream in-flight budget exceeded: mapping=" +
      std::to_string(mapping) + " payload=" + std::to_string(payload) +
      " retransmission=" + std::to_string(retransmission) +
      " limit=" + std::to_string(aggregateInFlightLimit));
  }
  m_mappingInFlight = mapping;
  m_payloadInFlight = payload;
  m_retransmissionInFlight = retransmission;
}

bool
StreamAdaptiveFetcherState::observeAcceptedSample(uint64_t sessionEpoch,
                                                  uint64_t sampleId,
                                                  uint64_t arrivalMs,
                                                  double retrievalDelayMs,
                                                  uint64_t segmentCount,
                                                  bool knownProduced)
{
  if (!m_liveMode || m_phase == StreamPrefetchPhase::Stopped ||
      sessionEpoch != m_sessionEpoch || arrivalMs == 0 || segmentCount == 0 ||
      (m_hasLastSample && (sampleId <= m_lastSampleId ||
                           arrivalMs <= m_lastSampleArrivalMs))) {
    ++m_invalidObservations;
    return false;
  }

  if (knownProduced && retrievalDelayMs > 0.0 && std::isfinite(retrievalDelayMs)) {
    observeRtt(retrievalDelayMs);
  }
  m_segmentsPerSample = std::max(1.0, m_segmentsPerSample * 0.75 +
                                       static_cast<double>(segmentCount) * 0.25);

  if (m_hasLastSample) {
    const auto sampleDistance = sampleId - m_lastSampleId;
    const auto arrivalDistance = arrivalMs - m_lastSampleArrivalMs;
    m_sampleArrivalPeriods.push_back(
      static_cast<double>(arrivalDistance) / static_cast<double>(sampleDistance));
    const auto capacity = std::max<uint64_t>(2, liveEdgeWindow * 2);
    while (m_sampleArrivalPeriods.size() > capacity) {
      m_sampleArrivalPeriods.pop_front();
    }
  }
  m_lastSampleId = sampleId;
  m_lastSampleArrivalMs = arrivalMs;
  m_hasLastSample = true;
  m_futureWait = false;
  evaluateLiveEdge(arrivalMs);
  return true;
}

void
StreamAdaptiveFetcherState::observeSampleExtent(uint64_t predictedCount,
                                                 uint64_t actualCount)
{
  if (!m_liveMode || predictedCount == 0 || actualCount == 0) {
    ++m_invalidObservations;
    return;
  }
  if (predictedCount > actualCount) {
    m_terminalUnproducedAdvice += predictedCount - actualCount;
  }
  else if (actualCount > predictedCount) {
    m_laterCursorAdvice += actualCount - predictedCount;
  }
}

void
StreamAdaptiveFetcherState::beginRecovery(uint64_t nowMs,
                                           uint64_t playoutDeadlineMs)
{
  if (!m_liveMode || m_phase == StreamPrefetchPhase::Stopped ||
      playoutDeadlineMs <= nowMs) {
    throw std::invalid_argument("recovery requires a live session and future deadline");
  }
  m_recoveryStartedMs = nowMs;
  m_recoveryDeadlineMs = playoutDeadlineMs;
  m_phase = StreamPrefetchPhase::Recovering;
}

void
StreamAdaptiveFetcherState::recordRecovery(bool completed)
{
  if (m_phase != StreamPrefetchPhase::Recovering) {
    ++m_invalidObservations;
    return;
  }
  m_phase = completed ? StreamPrefetchPhase::Fetching
                      : StreamPrefetchPhase::Adjusting;
  m_recoveryStartedMs = 0;
  m_recoveryDeadlineMs = 0;
}

void
StreamAdaptiveFetcherState::recordInvalidObservation()
{
  ++m_invalidObservations;
}

void
StreamAdaptiveFetcherState::stopLive()
{
  m_phase = StreamPrefetchPhase::Stopped;
  m_sampleArrivalPeriods.clear();
  m_consecutiveStable = 0;
  m_futureWait = false;
  m_recoveryStartedMs = 0;
  m_recoveryDeadlineMs = 0;
}

StreamPrefetchPhase
StreamAdaptiveFetcherState::phase() const
{
  return m_phase;
}

uint64_t
StreamAdaptiveFetcherState::invalidObservations() const
{
  return m_invalidObservations;
}

void
StreamAdaptiveFetcherState::evaluateLiveEdge(uint64_t nowMs)
{
  const auto window = std::clamp<uint64_t>(liveEdgeWindow, 1, 1024);
  if (m_sampleArrivalPeriods.size() < window * 2) {
    return;
  }

  double oldMean = 0.0;
  double newMean = 0.0;
  const auto split = m_sampleArrivalPeriods.size() - window;
  const auto begin = split - window;
  for (size_t i = begin; i < split; ++i) {
    oldMean += m_sampleArrivalPeriods[i];
  }
  for (size_t i = split; i < m_sampleArrivalPeriods.size(); ++i) {
    newMean += m_sampleArrivalPeriods[i];
  }
  oldMean /= static_cast<double>(window);
  newMean /= static_cast<double>(window);
  const bool stable = evaluateStability(oldMean, newMean);

  if (stable) {
    const auto required = std::max<uint64_t>(1, liveEdgeStableRequired);
    m_consecutiveStable = std::min(required, m_consecutiveStable + 1);
  }
  else {
    m_consecutiveStable = 0;
  }

  const auto holdExpired = nowMs >= m_lastActionMs + detectionPeriodMs;
  if (m_phase == StreamPrefetchPhase::Chasing) {
    if (holdExpired &&
        m_consecutiveStable >= std::max<uint64_t>(1, liveEdgeStableRequired)) {
      m_phase = StreamPrefetchPhase::Adjusting;
      m_previousUsableWindow = 0;
      m_lastActionMs = nowMs;
    }
    else if (holdExpired) {
      const auto burst = static_cast<uint64_t>(std::ceil(
        static_cast<double>(m_liveWindow) * chaseMultiplier));
      m_liveWindow = std::min(maxWindow, std::max(minWindow, burst));
      m_lastActionMs = nowMs;
    }
  }
  else if (m_phase == StreamPrefetchPhase::Adjusting && holdExpired) {
    if (!stable) {
      if (m_previousUsableWindow != 0) {
        m_liveWindow = std::min(maxWindow,
          std::max(minWindow, m_previousUsableWindow));
        m_previousUsableWindow = 0;
        m_phase = StreamPrefetchPhase::Fetching;
      }
      else {
        m_phase = StreamPrefetchPhase::Chasing;
      }
    }
    else {
      const auto target = livePacketDemand();
      if (m_liveWindow > target) {
        m_previousUsableWindow = m_liveWindow;
        const auto withheld = static_cast<uint64_t>(std::ceil(
          static_cast<double>(m_liveWindow) * adjustMultiplier));
        m_liveWindow = std::max(target, std::max(minWindow, withheld));
      }
      else {
        m_liveWindow = target;
        m_previousUsableWindow = 0;
        m_phase = StreamPrefetchPhase::Fetching;
      }
    }
    m_lastActionMs = nowMs;
  }
  else if (m_phase == StreamPrefetchPhase::Fetching && !stable && holdExpired) {
    m_liveWindow = std::max(m_liveWindow, livePacketDemand());
    m_previousUsableWindow = 0;
    m_phase = StreamPrefetchPhase::Adjusting;
    m_lastActionMs = nowMs;
  }
}

bool
StreamAdaptiveFetcherState::evaluateStability(double oldMean, double newMean) const
{
  if (detectorProfile.rfind("paper-eq3-literal-", 0) == 0) {
    // Literal printed Eq. (3): m1 is the newest window and m2 the previous
    // window. The second <= inequality is intentionally preserved even though
    // it conflicts with the paper's prose near the producer period.
    const auto attenuation = newMean / std::max(1.0, oldMean);
    const auto printedSimilarity = 1.0 -
      std::abs(newMean - m_samplePeriodMs) / std::max(1.0, m_samplePeriodMs);
    return attenuation <= liveEdgeChangeThreshold &&
           printedSimilarity <= liveEdgePeriodSimilarity;
  }
  const auto relativeChange = std::abs(newMean - oldMean) / std::max(1.0, oldMean);
  const auto periodSimilarity = 1.0 -
    std::abs(newMean - m_samplePeriodMs) / std::max(1.0, m_samplePeriodMs);
  return relativeChange <= liveEdgeChangeThreshold &&
         periodSimilarity >= liveEdgePeriodSimilarity;
}

uint64_t
StreamAdaptiveFetcherState::liveSampleDemand() const
{
  if (!m_liveMode || m_samplePeriodMs <= 0.0) return 1;
  auto demand = std::max<uint64_t>(
    1, static_cast<uint64_t>(std::ceil(rttMs / m_samplePeriodMs)));
  // A stable average arrival period can hide burst/reorder stalls: the
  // consumer may be draining at the producer's mean rate while remaining
  // several samples behind. Preserve enough packet demand for the largest
  // recently observed normalized inter-arrival gap. The deque is already
  // bounded by the live-edge detector, so a transient reserve decays without
  // adding a workload-specific window or threshold.
  for (const auto period : m_sampleArrivalPeriods) {
    if (period > 0.0 && std::isfinite(period)) {
      demand = std::max(demand, static_cast<uint64_t>(
        std::ceil(period / m_samplePeriodMs)));
    }
  }
  return demand;
}

uint64_t
StreamAdaptiveFetcherState::livePacketDemand() const
{
  if (!m_liveMode || m_samplePeriodMs <= 0.0) {
    return std::min(maxWindow, std::max(minWindow, baseWindow));
  }
  const auto sampleDemand = liveSampleDemand();
  if (!m_predictedSampleGroups.empty()) {
    uint64_t packetDemand = 0;
    size_t samples = 0;
    for (const auto groupItems : m_predictedSampleGroups) {
      if (samples++ >= sampleDemand ||
          packetDemand > std::numeric_limits<uint64_t>::max() - groupItems) break;
      packetDemand += groupItems;
    }
    // Mapping v2 extents already include the selected repair item, so do not
    // add a fixed packet reserve that could split the following sample.
    if (samples >= sampleDemand) {
      return std::min(maxWindow, std::max(minWindow, packetDemand));
    }
  }
  const auto packetDemand = static_cast<uint64_t>(
    std::ceil(static_cast<double>(sampleDemand) * m_segmentsPerSample)) +
    recoveryReservePackets;
  return std::min(maxWindow, std::max(minWindow, packetDemand));
}

StreamFetchDecision
StreamAdaptiveFetcherState::decide(uint64_t nowMs,
                                    uint64_t playoutDeadlineMs) const
{
  const auto pressure = std::max({
    timeoutPressure,
    nackPressure,
    duplicatePressure * 0.5,
    backlogPressure,
  });

  StreamFetchDecision decision;
  decision.pressure = std::max(0.0, std::min(1.0, pressure));
  if (m_liveMode && (!m_mappedLive || m_mappedLivePolicyEnabled)) {
    const auto demand = livePacketDemand();
    const auto sampleDemand = liveSampleDemand();
    decision.phase = m_phase;
    decision.policyMode = m_mappedLive ? "mapped-live-v1-future-on" : "live-v1";
    decision.detectorProfile = detectorProfile;
    decision.sampleDemand = sampleDemand;
    decision.packetDemand = demand;
    decision.liveEdgeConfidence = std::min(
      1.0, static_cast<double>(m_consecutiveStable) /
             static_cast<double>(std::max<uint64_t>(1, liveEdgeStableRequired)));
    decision.window = std::min(maxWindow, std::max(minWindow,
      m_phase == StreamPrefetchPhase::Fetching ? std::max(demand, m_liveWindow)
                                               : m_liveWindow));
    if (decision.pressure > 0.0) {
      decision.window = std::max(minWindow, static_cast<uint64_t>(std::llround(
        static_cast<double>(decision.window) / (1.0 + decision.pressure))));
    }
    // Mapping v2 exposes the next sample's authenticated predicted extent.
    // Pressure may remove later whole groups, but it must never shrink the
    // effective packet window below the first group and deadlock a key frame.
    if (m_mappedLive && !m_predictedSampleGroups.empty()) {
      decision.window = std::min(maxWindow,
        std::max(decision.window, m_predictedSampleGroups.front()));
    }
    // Congestion pressure may remove speculative capacity, but shrinking
    // below the measured whole-sample packet demand makes the consumer slower
    // than the producer and creates permanent live latency. demand is already
    // bounded by maxWindow and includes the selected recovery reserve.
    const auto jitterReserve = m_mappedLive && !m_predictedSampleGroups.empty()
      ? m_predictedSampleGroups.front() : uint64_t{0};
    const auto sustainableDemand = demand > maxWindow - std::min(maxWindow, jitterReserve)
      ? maxWindow : demand + jitterReserve;
    decision.window = std::max(decision.window, sustainableDemand);
    // A cursor horizon larger than the consumer can express concurrently is
    // not actionable. It only lengthens future-payload Interest lifetimes and
    // can turn sparse loss into seconds of head-of-line lag while Chasing.
    // Preserve the paper's phase controller inside the local safety capacity.
    if (m_mappedLive) {
      decision.window = std::min(decision.window, aggregateInFlightLimit);
    }
    // The paper varies the production/request rate while finding the live
    // edge: Chasing sends bursts and Adjusting withholds requests.  Using the
    // steady-state demand as the cursor horizon made those phase/window
    // changes observational only; schedule() could never issue the larger
    // burst.  Once Fetching is stable, collapse back to the measured demand.
    const auto phaseHorizon = decision.window;
    decision.lookahead = std::min(maxLookahead,
      std::max(minLookahead, phaseHorizon));
    decision.payloadBeginCursor = m_nextSeq;
    if (m_mappedLive && m_nextReservedCursor > m_nextSeq &&
        m_mappingCommittedThroughCursor >= m_nextSeq) {
      const auto maxByLookahead = m_nextSeq > std::numeric_limits<uint64_t>::max() -
                                                (decision.lookahead - 1)
        ? std::numeric_limits<uint64_t>::max()
        : m_nextSeq + decision.lookahead - 1;
      decision.payloadEndCursor = std::min({
        maxByLookahead,
        m_mappingCommittedThroughCursor,
        m_nextReservedCursor - 1,
      });
      decision.mappingReady = true;
      decision.mappingWaitReason = "ready";
    }
    else {
      decision.payloadEndCursor = m_nextSeq;
      decision.mappingReady = !m_mappedLive;
      decision.mappingWaitReason = m_mappedLive ? "mapping-starved" : "not-required";
    }
    const auto blockCapacity = std::max<uint64_t>(1, mappingBlockCapacity);
    decision.mappingBeginBlock = decision.payloadBeginCursor / blockCapacity;
    decision.mappingEndBlock = decision.payloadEndCursor / blockCapacity;

    const auto clockNow = nowMs == 0 ? m_lastSampleArrivalMs : nowMs;
    decision.congestionHold = m_congestionHoldUntilMs != 0 &&
                              clockNow < m_congestionHoldUntilMs;
    decision.aggregateInFlightLimit = aggregateInFlightLimit;
    if (decision.congestionHold) {
      decision.aggregateInFlightLimit = std::max<uint64_t>(1,
        static_cast<uint64_t>(std::floor(
          static_cast<double>(aggregateInFlightLimit) * congestionDecreaseMultiplier)));
    }
    if (m_mappedLive && !m_predictedSampleGroups.empty()) {
      const auto atomicCapacity = m_predictedSampleGroups.front() +
                                  mappingReserve + retransmissionReserve;
      decision.aggregateInFlightLimit = std::min(
        aggregateInFlightLimit,
        std::max(decision.aggregateInFlightLimit, atomicCapacity));
    }
    const auto used = std::min(decision.aggregateInFlightLimit,
      m_mappingInFlight + m_payloadInFlight + m_retransmissionInFlight);
    auto available = decision.aggregateInFlightLimit - used;
    const auto mappingDeficit = mappingReserve > m_mappingInFlight
      ? mappingReserve - m_mappingInFlight : uint64_t{0};
    decision.mappingBudget = std::min(mappingDeficit, available);
    available -= decision.mappingBudget;
    const auto retransmissionDeficit =
      retransmissionReserve > m_retransmissionInFlight
        ? retransmissionReserve - m_retransmissionInFlight : uint64_t{0};
    decision.retransmissionBudget = std::min(retransmissionDeficit, available);
    available -= decision.retransmissionBudget;
    decision.payloadBudget = available;
    decision.futureWait = m_futureWait;
    decision.futureWaitCount = m_futureWaitCount;
    decision.terminalUnproducedAdvice = m_terminalUnproducedAdvice;
    decision.laterCursorAdvice = m_laterCursorAdvice;
    const auto futureSamples = static_cast<uint64_t>(std::ceil(
      static_cast<double>(decision.lookahead) / std::max(1.0, m_segmentsPerSample)));
    const auto lifetime = rttMs + static_cast<double>(futureSamples) * m_samplePeriodMs +
                          m_samplePeriodMs;
    decision.interestLifetimeMs = std::min(maxInterestLifetimeMs,
      std::max(minInterestLifetimeMs, static_cast<uint64_t>(std::ceil(lifetime))));
    decision.missingTimeoutMs = std::min(maxMissingTimeoutMs,
      std::max(minMissingTimeoutMs,
               static_cast<uint64_t>(std::ceil(rttMs + m_samplePeriodMs))));
    decision.recoveryCheckpointMs = std::min(maxMissingTimeoutMs,
      std::max(minMissingTimeoutMs, static_cast<uint64_t>(std::ceil(rttMs))));
    if (m_phase == StreamPrefetchPhase::Recovering) {
      const auto deadline = playoutDeadlineMs == 0 ? m_recoveryDeadlineMs
                                                    : playoutDeadlineMs;
      decision.remainingRecoveryBudgetMs = deadline > clockNow
        ? deadline - clockNow : 0;
      decision.retransmissionEligible = decision.remainingRecoveryBudgetMs > 0 &&
                                        decision.retransmissionBudget > 0;
    }
    if (clockNow < m_lastActionMs + detectionPeriodMs) {
      decision.holdMs = m_lastActionMs + detectionPeriodMs - clockNow;
    }
    decision.reason = std::string("live-") + toString(m_phase);
    return decision;
  }

  decision.phase = StreamPrefetchPhase::Inactive;
  decision.policyMode = m_mappedLive ? "mapped-pressure" : "pressure-only";
  decision.detectorProfile = m_mappedLive ? detectorProfile : "none";
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
  if (m_mappedLive) {
    decision.payloadBeginCursor = m_nextSeq;
    const auto blockCapacity = std::max<uint64_t>(1, mappingBlockCapacity);
    decision.mappingBeginBlock = m_nextSeq / blockCapacity;
    decision.mappingReady = m_nextReservedCursor > m_nextSeq &&
                            m_mappingCommittedThroughCursor >= m_nextSeq;
    if (decision.mappingReady) {
      const auto maxByLookahead = m_nextSeq > std::numeric_limits<uint64_t>::max() -
                                                (decision.lookahead - 1)
        ? std::numeric_limits<uint64_t>::max()
        : m_nextSeq + decision.lookahead - 1;
      decision.payloadEndCursor = std::min({
        maxByLookahead,
        m_mappingCommittedThroughCursor,
        m_nextReservedCursor - 1,
      });
      decision.mappingEndBlock = decision.payloadEndCursor / blockCapacity;
      decision.mappingWaitReason = "ready";
    }
    else {
      decision.payloadEndCursor = m_nextSeq;
      decision.mappingEndBlock = decision.mappingBeginBlock;
      decision.mappingWaitReason = "mapping-starved";
    }
    decision.aggregateInFlightLimit = aggregateInFlightLimit;
    const auto used = std::min(aggregateInFlightLimit,
      m_mappingInFlight + m_payloadInFlight + m_retransmissionInFlight);
    auto available = aggregateInFlightLimit - used;
    const auto mappingDeficit = mappingReserve > m_mappingInFlight
      ? mappingReserve - m_mappingInFlight : uint64_t{0};
    decision.mappingBudget = std::min(mappingDeficit, available);
    available -= decision.mappingBudget;
    const auto retransmissionDeficit =
      retransmissionReserve > m_retransmissionInFlight
        ? retransmissionReserve - m_retransmissionInFlight : uint64_t{0};
    decision.retransmissionBudget = std::min(retransmissionDeficit, available);
    available -= decision.retransmissionBudget;
    decision.payloadBudget = available;
  }
  return decision;
}

std::optional<std::string>
LiveStreamDescriptor::validate() const
{
  if (const auto error = definition.validate()) return error;
  if (measuredSamplePeriodMs <= 0.0 || !std::isfinite(measuredSamplePeriodMs)) {
    return "invalid-sample-period";
  }
  if (safeJoinCursor < checkpoint.frontiers.oldestRetained ||
      safeJoinCursor > checkpoint.frontiers.latestProduced ||
      checkpoint.blockNumber != safeJoinCursor / definition.mappingBlockCapacity) {
    return "invalid-safe-join";
  }
  return checkpoint.frontiers.validate(definition.mappingBlockCapacity,
                                        checkpoint.blockNumber);
}

const char*
toString(LiveStreamLifecycleState state)
{
  switch (state) {
  case LiveStreamLifecycleState::Preparing: return "PREPARING";
  case LiveStreamLifecycleState::Active: return "ACTIVE";
  case LiveStreamLifecycleState::Stopped: return "STOPPED";
  case LiveStreamLifecycleState::Failed: return "FAILED";
  }
  return "UNKNOWN";
}

const char*
toString(LiveStreamItemProvenance provenance)
{
  switch (provenance) {
  case LiveStreamItemProvenance::SignedData: return "SIGNED-DATA";
  case LiveStreamItemProvenance::FecRecovered: return "FEC-RECOVERED";
  }
  return "UNKNOWN";
}

std::optional<std::string>
PredictiveStreamCheckpoint::validate() const
{
  if (oldestRetainedSampleId > latestProducedSampleId) {
    return "invalid predictive checkpoint: oldestRetained > latestProduced";
  }
  if (latestProducedSampleId > nextExpectedSampleId) {
    return "invalid predictive checkpoint: latestProduced > nextExpected";
  }
  if (initialSampleId > latestProducedSampleId && latestProducedSampleId != 0) {
    return "invalid predictive checkpoint: initial > latestProduced";
  }
  return std::nullopt;
}

ndn::Name
makePredictiveFrontierName(const ndn::Name& mappingRoot)
{
  ndn::Name name(mappingRoot);
  name.append("frontier");
  return name;
}

ndn::Name
makePredictiveDataName(const ndn::Name& mappingRoot,
                       uint64_t mappingVersion,
                       uint64_t sequence)
{
  ndn::Name name(mappingRoot);
  name.append("v").appendNumber(mappingVersion);
  name.appendSequenceNumber(sequence);
  return name;
}

ndn::Name
makePredictiveDataName(const LiveStreamDefinition& definition,
                       uint64_t sequence)
{
  return makePredictiveDataName(
    definition.mappingRoot(), definition.mappingVersion, sequence);
}

ndn::Name
makePredictiveGroupName(const LiveStreamDefinition& definition,
                        uint64_t groupId)
{
  ndn::Name name(definition.mappingRoot());
  name.append("v").appendNumber(definition.mappingVersion)
      .append("group").appendNumber(groupId);
  return name;
}

ndn::Name
makePredictiveRepairName(const LiveStreamDefinition& definition,
                         uint64_t groupId, uint64_t repairIndex)
{
  ndn::Name name(makePredictiveGroupName(definition, groupId));
  name.append("repair").appendNumber(repairIndex);
  return name;
}

std::optional<std::string>
PredictiveStreamGroupCommit::validate(
  const LiveStreamDefinition& definition) const
{
  if (const auto error = definition.validate()) {
    return error;
  }
  if (contractVersion != 1 || streamId != definition.streamId ||
      sessionEpoch != definition.sessionEpoch ||
      mappingVersion != definition.mappingVersion) {
    return "predictive group session mismatch";
  }
  if (createdMs == 0 || expiresMs <= createdMs ||
      sourceNames.empty() ||
      sourceNames.size() != sourceWireLengths.size() ||
      sourceNames.size() != sourceWireDigests.size()) {
    return "invalid predictive group shape";
  }
  if (definition.fec.enabled()) {
    if (sourceNames.size() > definition.fec.maxSourceItems ||
        repairNames.size() != definition.fec.repairItemCount() ||
        recoveryCapacity != definition.fec.recoveryCapacity()) {
      return "predictive group FEC declaration mismatch";
    }
  }
  else if (!repairNames.empty() || recoveryCapacity != 0) {
    return "disabled predictive group has repair declaration";
  }
  std::set<ndn::Name> names;
  const auto root = definition.mappingRoot();
  const auto& firstName = sourceNames.front();
  if (!root.isPrefixOf(firstName) || firstName.size() != root.size() + 3 ||
      firstName[root.size()].toUri() != "v" ||
      !firstName[root.size() + 1].isNumber() ||
      firstName[root.size() + 1].toNumber() != definition.mappingVersion ||
      !firstName[root.size() + 2].isSequenceNumber()) {
    return "invalid predictive source binding";
  }
  const auto firstSequence =
    firstName[root.size() + 2].toSequenceNumber();
  for (size_t i = 0; i < sourceNames.size(); ++i) {
    if (sourceNames[i] != makePredictiveDataName(
                            definition, firstSequence + i) ||
        !names.insert(sourceNames[i]).second ||
        sourceWireLengths[i] == 0 ||
        sourceWireLengths[i] > definition.signedWireCap ||
        isZeroDigest(sourceWireDigests[i])) {
      return "invalid predictive source binding";
    }
  }
  for (size_t i = 0; i < repairNames.size(); ++i) {
    if (repairNames[i] != makePredictiveRepairName(definition, groupId, i) ||
        !names.insert(repairNames[i]).second) {
      return "invalid predictive repair binding";
    }
  }
  return std::nullopt;
}

ndn::Block
PredictiveStreamGroupCommit::wireEncode() const
{
  ndn::Block block(stream_tlv::PredictiveStreamGroupCommitType);
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::StreamContractVersionType, contractVersion));
  block.push_back(ndn::makeStringBlock(stream_tlv::StreamIdType, streamId));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::StreamSessionEpochType, sessionEpoch));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::StreamMappingVersionType, mappingVersion));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::StreamGroupIdType, groupId));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::StreamCreatedMsType, createdMs));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::StreamDeadlineMsType, expiresMs));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::LiveStreamFecRecoveryCapacityType, recoveryCapacity));
  for (size_t i = 0; i < sourceNames.size(); ++i) {
    ndn::Block source(stream_tlv::PredictiveStreamSourceType);
    source.push_back(sourceNames[i].wireEncode());
    source.push_back(ndn::makeNonNegativeIntegerBlock(
      stream_tlv::PredictiveStreamSourceWireLengthType,
      sourceWireLengths.at(i)));
    source.push_back(ndn::makeBinaryBlock(
      stream_tlv::PredictiveStreamSourceWireDigestType,
      ndn::span<const uint8_t>(sourceWireDigests.at(i).data(),
                               sourceWireDigests.at(i).size())));
    source.encode();
    block.push_back(source);
  }
  for (const auto& repairName : repairNames) {
    block.push_back(ndn::makeStringBlock(
      stream_tlv::PredictiveStreamRepairNameType, repairName.toUri()));
  }
  block.encode();
  return block;
}

bool
PredictiveStreamGroupCommit::wireDecode(const ndn::Block& wire)
{
  try {
    if (wire.type() != stream_tlv::PredictiveStreamGroupCommitType) {
      return false;
    }
    auto block = wire;
    block.parse();
    PredictiveStreamGroupCommit decoded;
    size_t index = 0;
    const auto take = [&] (uint32_t type) -> const ndn::Block& {
      if (index >= block.elements().size() ||
          block.elements()[index].type() != type) {
        throw std::invalid_argument("unexpected predictive group field");
      }
      return block.elements()[index++];
    };
    decoded.contractVersion = ndn::readNonNegativeInteger(
      take(stream_tlv::StreamContractVersionType));
    decoded.streamId = ndn::readString(take(stream_tlv::StreamIdType));
    decoded.sessionEpoch = ndn::readNonNegativeInteger(
      take(stream_tlv::StreamSessionEpochType));
    decoded.mappingVersion = ndn::readNonNegativeInteger(
      take(stream_tlv::StreamMappingVersionType));
    decoded.groupId = ndn::readNonNegativeInteger(
      take(stream_tlv::StreamGroupIdType));
    decoded.createdMs = ndn::readNonNegativeInteger(
      take(stream_tlv::StreamCreatedMsType));
    decoded.expiresMs = ndn::readNonNegativeInteger(
      take(stream_tlv::StreamDeadlineMsType));
    decoded.recoveryCapacity = ndn::readNonNegativeInteger(
      take(stream_tlv::LiveStreamFecRecoveryCapacityType));
    while (index < block.elements().size() &&
           block.elements()[index].type() ==
             stream_tlv::PredictiveStreamSourceType) {
      auto source = block.elements()[index++];
      source.parse();
      if (source.elements().size() != 3 ||
          source.elements()[0].type() != ndn::tlv::Name ||
          source.elements()[1].type() !=
            stream_tlv::PredictiveStreamSourceWireLengthType ||
          source.elements()[2].type() !=
            stream_tlv::PredictiveStreamSourceWireDigestType ||
          source.elements()[2].value_size() != 32) {
        return false;
      }
      decoded.sourceNames.emplace_back(source.elements()[0]);
      decoded.sourceWireLengths.push_back(ndn::readNonNegativeInteger(
        source.elements()[1]));
      std::array<uint8_t, 32> digest{};
      std::copy(source.elements()[2].value(),
                source.elements()[2].value() + 32, digest.begin());
      decoded.sourceWireDigests.push_back(digest);
    }
    while (index < block.elements().size() &&
           block.elements()[index].type() ==
             stream_tlv::PredictiveStreamRepairNameType) {
      decoded.repairNames.emplace_back(
        ndn::readString(block.elements()[index++]));
    }
    if (index != block.elements().size() ||
        !hasSameWire(decoded.wireEncode(), wire)) {
      return false;
    }
    *this = std::move(decoded);
    return true;
  }
  catch (const std::exception&) {
    return false;
  }
}

std::optional<std::string>
PredictiveStreamDescriptor::validate() const
{
  if (const auto error = definition.validate()) {
    return error;
  }
  if (const auto error = checkpoint.validate()) {
    return error;
  }
  if (frontierName.empty()) {
    return "missing frontier name in predictive descriptor";
  }
  if (frontierName != makePredictiveFrontierName(definition.mappingRoot())) {
    return "predictive descriptor frontier name is not canonical";
  }
  return std::nullopt;
}

bool
PredictiveStreamDescriptor::isPredictive() const
{
  return true;
}

std::optional<std::string>
PredictiveStreamFrontier::validate(
  const LiveStreamDefinition& definition) const
{
  if (const auto error = definition.validate()) {
    return error;
  }
  if (contractVersion != 2 || streamId != definition.streamId ||
      sessionEpoch != definition.sessionEpoch ||
      mappingVersion != definition.mappingVersion) {
    return "predictive frontier session mismatch";
  }
  if (const auto error = checkpoint.validate()) {
    return error;
  }
  if (latestCommittedGroupId.has_value() !=
      !retainedGroupCommitNames.empty()) {
    return "predictive frontier group anchor mismatch";
  }
  if (retainedGroupCommitNames.size() !=
        retainedGroupFirstCursors.size() ||
      retainedGroupCommitNames.size() !=
        retainedGroupLastCursors.size()) {
    return "predictive frontier group range count mismatch";
  }
  if (latestCommittedGroupId) {
    if (retainedGroupCommitNames.back() !=
        makePredictiveGroupName(definition, *latestCommittedGroupId)) {
      return "predictive frontier latest group mismatch";
    }
    uint64_t previous = 0;
    bool first = true;
    for (size_t index = 0;
         index < retainedGroupCommitNames.size(); ++index) {
      const auto& name = retainedGroupCommitNames[index];
      const auto root = definition.mappingRoot();
      if (!root.isPrefixOf(name) || name.size() != root.size() + 4 ||
          name[root.size()].toUri() != "v" ||
          !name[root.size() + 1].isNumber() ||
          name[root.size() + 1].toNumber() != definition.mappingVersion ||
          name[root.size() + 2].toUri() != "group" ||
          !name[root.size() + 3].isNumber()) {
        return "invalid predictive frontier group name";
      }
      const auto groupId = name[root.size() + 3].toNumber();
      if ((!first && groupId <= previous) ||
          name != makePredictiveGroupName(definition, groupId)) {
        return "non-canonical predictive frontier group order";
      }
      if (retainedGroupFirstCursors[index] >
            retainedGroupLastCursors[index] ||
          (!first && retainedGroupFirstCursors[index] <=
                       retainedGroupLastCursors[index - 1])) {
        return "non-canonical predictive frontier group range";
      }
      previous = groupId;
      first = false;
    }
  }
  return std::nullopt;
}

ndn::Block
PredictiveStreamFrontier::wireEncode() const
{
  ndn::Block block(stream_tlv::PredictiveStreamFrontierType);
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::StreamContractVersionType, contractVersion));
  block.push_back(ndn::makeStringBlock(stream_tlv::StreamIdType, streamId));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::StreamSessionEpochType, sessionEpoch));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::StreamMappingVersionType, mappingVersion));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::PredictiveStreamInitialSampleIdType,
    checkpoint.initialSampleId));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::PredictiveStreamOldestRetainedSampleIdType,
    checkpoint.oldestRetainedSampleId));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::PredictiveStreamLatestProducedSampleIdType,
    checkpoint.latestProducedSampleId));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    stream_tlv::PredictiveStreamNextExpectedSampleIdType,
    checkpoint.nextExpectedSampleId));
  if (latestCommittedGroupId) {
    block.push_back(ndn::makeNonNegativeIntegerBlock(
      stream_tlv::PredictiveStreamLatestCommittedGroupType,
      *latestCommittedGroupId));
  }
  for (size_t index = 0;
       index < retainedGroupCommitNames.size(); ++index) {
    block.push_back(ndn::makeStringBlock(
      stream_tlv::PredictiveStreamGroupCommitNameType,
      retainedGroupCommitNames[index].toUri()));
    block.push_back(ndn::makeNonNegativeIntegerBlock(
      stream_tlv::PredictiveStreamGroupFirstCursorType,
      retainedGroupFirstCursors[index]));
    block.push_back(ndn::makeNonNegativeIntegerBlock(
      stream_tlv::PredictiveStreamGroupLastCursorType,
      retainedGroupLastCursors[index]));
  }
  block.encode();
  return block;
}

bool
PredictiveStreamFrontier::wireDecode(const ndn::Block& wire)
{
  try {
    if (wire.type() != stream_tlv::PredictiveStreamFrontierType) {
      return false;
    }
    auto block = wire;
    block.parse();
    PredictiveStreamFrontier decoded;
    size_t index = 0;
    const auto take = [&] (uint32_t type) -> const ndn::Block& {
      if (index >= block.elements().size() ||
          block.elements()[index].type() != type) {
        throw std::invalid_argument("unexpected predictive frontier field");
      }
      return block.elements()[index++];
    };
    decoded.contractVersion = ndn::readNonNegativeInteger(
      take(stream_tlv::StreamContractVersionType));
    decoded.streamId = ndn::readString(take(stream_tlv::StreamIdType));
    decoded.sessionEpoch = ndn::readNonNegativeInteger(
      take(stream_tlv::StreamSessionEpochType));
    decoded.mappingVersion = ndn::readNonNegativeInteger(
      take(stream_tlv::StreamMappingVersionType));
    decoded.checkpoint.initialSampleId = ndn::readNonNegativeInteger(
      take(stream_tlv::PredictiveStreamInitialSampleIdType));
    decoded.checkpoint.oldestRetainedSampleId = ndn::readNonNegativeInteger(
      take(stream_tlv::PredictiveStreamOldestRetainedSampleIdType));
    decoded.checkpoint.latestProducedSampleId = ndn::readNonNegativeInteger(
      take(stream_tlv::PredictiveStreamLatestProducedSampleIdType));
    decoded.checkpoint.nextExpectedSampleId = ndn::readNonNegativeInteger(
      take(stream_tlv::PredictiveStreamNextExpectedSampleIdType));
    if (index < block.elements().size() &&
        block.elements()[index].type() ==
          stream_tlv::PredictiveStreamLatestCommittedGroupType) {
      decoded.latestCommittedGroupId = ndn::readNonNegativeInteger(
        block.elements()[index++]);
    }
    while (index < block.elements().size() &&
           block.elements()[index].type() ==
             stream_tlv::PredictiveStreamGroupCommitNameType) {
      decoded.retainedGroupCommitNames.emplace_back(
        ndn::readString(block.elements()[index++]));
      decoded.retainedGroupFirstCursors.push_back(
        ndn::readNonNegativeInteger(
          take(stream_tlv::PredictiveStreamGroupFirstCursorType)));
      decoded.retainedGroupLastCursors.push_back(
        ndn::readNonNegativeInteger(
          take(stream_tlv::PredictiveStreamGroupLastCursorType)));
    }
    if (index != block.elements().size() ||
        !hasSameWire(decoded.wireEncode(), wire)) {
      return false;
    }
    *this = std::move(decoded);
    return true;
  }
  catch (const std::exception&) {
    return false;
  }
}

LiveStreamItemAdmission
LiveStreamItemAdmission::acceptItem()
{
  return {true, {}};
}

LiveStreamItemAdmission
LiveStreamItemAdmission::rejectItem(std::string reason)
{
  return {false, std::move(reason)};
}

PublishedPacketFeed::PublishedPacketFeed(PublishedPacketFeedOptions options)
  : m_options(std::move(options))
{
  if (m_options.maxQueuedPackets == 0 || m_options.maxQueuedBytes == 0) {
    throw std::invalid_argument("published packet feed bounds must be positive");
  }
}

void
PublishedPacketFeed::enqueue(PublishedLiveStreamPacket packet)
{
  std::lock_guard<std::mutex> guard(m_mutex);
  if (m_closed) return;
  const auto bytes = packet.signedDataWire.size();
  if (bytes > m_options.maxQueuedBytes ||
      m_queue.size() >= m_options.maxQueuedPackets ||
      m_queuedBytes > m_options.maxQueuedBytes - bytes) {
    ++m_droppedPackets;
    if (packet.cursor) {
      if (!m_firstDroppedCursor) m_firstDroppedCursor = packet.cursor;
      m_lastDroppedCursor = packet.cursor;
    }
    return;
  }
  m_queuedBytes += bytes;
  m_queue.push_back(std::move(packet));
}

std::vector<PublishedLiveStreamPacket>
PublishedPacketFeed::takeAvailable(size_t maxItems)
{
  std::lock_guard<std::mutex> guard(m_mutex);
  std::vector<PublishedLiveStreamPacket> result;
  const auto count = std::min(maxItems, m_queue.size());
  result.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    m_queuedBytes -= m_queue.front().signedDataWire.size();
    result.push_back(std::move(m_queue.front()));
    m_queue.pop_front();
  }
  return result;
}

PublishedPacketFeedStatus
PublishedPacketFeed::status() const
{
  std::lock_guard<std::mutex> guard(m_mutex);
  return {m_queue.size(), m_queuedBytes, m_droppedPackets,
          m_firstDroppedCursor, m_lastDroppedCursor, m_closed};
}

void
PublishedPacketFeed::close()
{
  std::lock_guard<std::mutex> guard(m_mutex);
  if (m_closed) return;
  m_closed = true;
  // Closing detaches the feed from future publication but does not discard
  // already-materialized signed packets. Retention can therefore drain a
  // finite, immutable tail and commit a truthful final checkpoint.
}

StoredSignedPacketProducer::StoredSignedPacketProducer(
  ndn::Face& face, ndn::Name routePrefix,
  const std::vector<ndn::Buffer>& signedPacketWires)
  : m_face(face)
  , m_routePrefix(std::move(routePrefix))
{
  if (m_routePrefix.empty() || signedPacketWires.empty())
    throw std::invalid_argument("StoredSignedPacketProducer requires prefix and packets");
  for (const auto& bytes : signedPacketWires) {
    ndn::Block wire(ndn::span<const uint8_t>(bytes.data(), bytes.size()));
    wire.parse();
    auto packet = std::make_shared<ndn::Data>(wire);
    if (!m_routePrefix.isPrefixOf(packet->getName()))
      throw std::invalid_argument("stored signed Data is outside replay prefix");
    const auto [it, inserted] = m_packets.emplace(packet->getName(), packet);
    if (!inserted && it->second->wireEncode() != packet->wireEncode())
      throw std::invalid_argument("conflicting stored signed Data wire");
  }
}

StoredSignedPacketProducer::~StoredSignedPacketProducer()
{
  stop();
}

void
StoredSignedPacketProducer::start()
{
  std::lock_guard<std::mutex> guard(m_mutex);
  if (m_started) return;
  m_route = m_face.setInterestFilter(
    m_routePrefix,
    [this](const auto&, const ndn::Interest& interest) { onInterest(interest); },
    [] (const ndn::Name&) {},
    [] (const ndn::Name&, const std::string&) {});
  m_started = true;
}

void
StoredSignedPacketProducer::stop()
{
  std::lock_guard<std::mutex> guard(m_mutex);
  if (!m_started) return;
  m_route.cancel();
  m_started = false;
}

size_t
StoredSignedPacketProducer::packetCount() const
{
  std::lock_guard<std::mutex> guard(m_mutex);
  return m_packets.size();
}

void
StoredSignedPacketProducer::onInterest(const ndn::Interest& interest)
{
  std::shared_ptr<ndn::Data> packet;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    const auto found = m_packets.find(interest.getName());
    if (found != m_packets.end()) packet = found->second;
  }
  if (packet) m_face.put(*packet);
}

LiveStreamPublisher::LiveStreamPublisher(LiveStreamDefinition definition,
                                         ndn::Face& face,
                                         ndn::KeyChain& keyChain,
                                         ndn::security::SigningInfo signingInfo)
  : m_definition(std::move(definition))
  , m_face(face)
  , m_keyChain(keyChain)
  , m_signingInfo(std::move(signingInfo))
  , m_samplePredictor(m_definition.sampleClasses)
{
  if (const auto error = m_definition.validate()) {
    throw std::invalid_argument("invalid LiveStreamDefinition: " + *error);
  }
}

LiveStreamPublisher::~LiveStreamPublisher()
{
  stop();
}

PublishedLiveStreamPacket
LiveStreamPublisher::makePublishedPacket(PublishedLiveStreamPacketKind kind,
                                         const ndn::Data& data,
                                         std::optional<StreamCursor> cursor) const
{
  const auto& wire = data.wireEncode();
  PublishedLiveStreamPacket result;
  result.kind = kind;
  result.streamId = m_definition.streamId;
  result.sessionEpoch = m_definition.sessionEpoch;
  result.mappingVersion = m_definition.mappingVersion;
  result.cursor = cursor;
  result.dataName = data.getName();
  result.provider = m_definition.provider;
  result.signedDataWire = ndn::Buffer(wire.begin(), wire.end());
  result.wireDigest = digestOpaque(ndn::span<const uint8_t>(wire.begin(), wire.size()));
  result.materializedMonotonicUs = timelineSteadyMicroseconds();
  return result;
}

void
LiveStreamPublisher::notifyFeedsLocked(const PublishedLiveStreamPacket& packet)
{
  for (auto it = m_packetFeeds.begin(); it != m_packetFeeds.end();) {
    if (const auto feed = it->lock()) {
      feed->enqueue(packet);
      ++it;
    }
    else {
      it = m_packetFeeds.erase(it);
    }
  }
}

std::shared_ptr<PublishedPacketFeed>
LiveStreamPublisher::openPublishedPacketFeed(const PublishedPacketFeedOptions& options)
{
  auto feed = std::shared_ptr<PublishedPacketFeed>(new PublishedPacketFeed(options));
  std::lock_guard<std::mutex> guard(m_mutex);
  if (m_state == LiveStreamLifecycleState::Stopped ||
      m_state == LiveStreamLifecycleState::Failed) {
    throw std::logic_error("LiveStream publisher cannot open packet feed");
  }

  const auto firstBlock = options.fromCursor / m_definition.mappingBlockCapacity;
  for (const auto& [blockNumber, block] : m_mappingBlocks) {
    if (blockNumber < firstBlock) continue;
    const auto name = makeStreamNameMapBlockName(
      m_definition.mappingRoot(), m_definition.mappingVersion, blockNumber);
    const auto packet = m_mappingPackets.find(name);
    if (packet != m_mappingPackets.end()) {
      feed->enqueue(makePublishedPacket(PublishedLiveStreamPacketKind::Mapping,
                                       *packet->second, block.firstCursor));
    }
  }
  for (const auto& name : m_retentionOrder) {
    const auto reservation = m_reservations.find(name);
    const auto packet = m_payloadPackets.find(name);
    if (reservation == m_reservations.end() || packet == m_payloadPackets.end() ||
        reservation->second.cursor < options.fromCursor) {
      continue;
    }
    feed->enqueue(makePublishedPacket(
      m_repairCursors.count(reservation->second.cursor) != 0 ?
        PublishedLiveStreamPacketKind::Repair : PublishedLiveStreamPacketKind::Source,
                                     *packet->second, reservation->second.cursor));
  }
  m_packetFeeds.push_back(feed);
  return feed;
}

void
LiveStreamPublisher::start()
{
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_startCalled) return;
    if (m_state != LiveStreamLifecycleState::Preparing) {
      throw std::logic_error("LiveStream publisher cannot be started");
    }
    m_startCalled = true;
    m_expectedRoutes = 2;
  }
  auto weak = weak_from_this();
  m_mappingRoute = m_face.setInterestFilter(
    m_definition.mappingRoot(),
    [weak] (const auto&, const ndn::Interest& interest) {
      if (const auto self = weak.lock()) self->onMappingInterest(interest);
    },
	    [weak] (const ndn::Name&) {
	      if (const auto self = weak.lock()) {
	        {
	          std::lock_guard<std::mutex> guard(self->m_mutex);
	          ++self->m_routesReady;
	        }
	        self->m_routeCondition.notify_all();
	      }
	    },
    [weak] (const ndn::Name&, const std::string& reason) {
      if (const auto self = weak.lock()) {
	        {
	          std::lock_guard<std::mutex> guard(self->m_mutex);
	          self->m_routeFailed = true;
	          self->m_state = LiveStreamLifecycleState::Failed;
	          self->m_reason = "mapping-route-registration-failed:" + reason;
	        }
	        self->m_routeCondition.notify_all();
	      }
	    });
  m_payloadRoute = m_face.setInterestFilter(
    m_definition.semanticDataPrefix,
    [weak] (const auto&, const ndn::Interest& interest) {
      if (const auto self = weak.lock()) self->onPayloadInterest(interest);
    },
	    [weak] (const ndn::Name&) {
	      if (const auto self = weak.lock()) {
	        {
	          std::lock_guard<std::mutex> guard(self->m_mutex);
	          ++self->m_routesReady;
	        }
	        self->m_routeCondition.notify_all();
	      }
	    },
    [weak] (const ndn::Name&, const std::string& reason) {
      if (const auto self = weak.lock()) {
	        {
	          std::lock_guard<std::mutex> guard(self->m_mutex);
	          self->m_routeFailed = true;
	          self->m_state = LiveStreamLifecycleState::Failed;
	          self->m_reason = "payload-route-registration-failed:" + reason;
	        }
	        self->m_routeCondition.notify_all();
	      }
    });
}

void
LiveStreamPublisher::startPredictive()
{
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_startCalled) return;
    if (m_state != LiveStreamLifecycleState::Preparing) {
      throw std::logic_error("LiveStream publisher cannot be started");
    }
    m_startCalled = true;
    m_predictiveMode = true;
    m_expectedRoutes = 1;
  }
  auto weak = weak_from_this();
  m_payloadRoute = m_face.setInterestFilter(
    m_definition.mappingRoot(),
    [weak] (const auto&, const ndn::Interest& interest) {
      if (const auto self = weak.lock()) self->onPayloadInterest(interest);
    },
    [weak] (const ndn::Name&) {
      if (const auto self = weak.lock()) {
        {
          std::lock_guard<std::mutex> guard(self->m_mutex);
          ++self->m_routesReady;
        }
        self->m_routeCondition.notify_all();
      }
    },
    [weak] (const ndn::Name&, const std::string& reason) {
      if (const auto self = weak.lock()) {
        {
          std::lock_guard<std::mutex> guard(self->m_mutex);
          self->m_routeFailed = true;
          self->m_state = LiveStreamLifecycleState::Failed;
          self->m_reason = "predictive-route-registration-failed:" + reason;
        }
        self->m_routeCondition.notify_all();
      }
    });
}

void
LiveStreamPublisher::waitUntilReady(std::chrono::milliseconds timeout)
{
  if (timeout.count() <= 0) {
    throw std::invalid_argument("LiveStream route readiness timeout must be positive");
  }
  if (m_face.getIoContext().get_executor().running_in_this_thread()) {
    throw std::logic_error(
      "LiveStream route readiness cannot block the Face I/O thread");
  }

  std::unique_lock<std::mutex> lock(m_mutex);
  const auto ready = m_routeCondition.wait_for(lock, timeout, [this] {
    return m_routesReady >= m_expectedRoutes || m_routeFailed ||
           m_state == LiveStreamLifecycleState::Failed ||
           m_state == LiveStreamLifecycleState::Stopped;
  });
  if (!ready) {
    throw std::runtime_error("LiveStream route readiness timed out");
  }
  if (m_routeFailed || m_state == LiveStreamLifecycleState::Failed) {
    throw std::runtime_error(
      m_reason.empty() ? "LiveStream route registration failed" : m_reason);
  }
  if (m_state == LiveStreamLifecycleState::Stopped) {
    throw std::logic_error("LiveStream publisher stopped before route readiness");
  }
}

void
LiveStreamPublisher::activatePredictive(double measuredSamplePeriodMs)
{
  std::shared_ptr<ndn::Data> frontierPacket;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_state != LiveStreamLifecycleState::Preparing || m_routeFailed ||
        !m_predictiveMode || m_routesReady < m_expectedRoutes ||
        measuredSamplePeriodMs <= 0.0 ||
        !std::isfinite(measuredSamplePeriodMs)) {
      throw std::logic_error(
        "LiveStream publisher is not ready for predictive activation:"
        " state=" + std::to_string(static_cast<int>(m_state)) +
        " routeFailed=" + std::to_string(m_routeFailed) +
        " predictive=" + std::to_string(m_predictiveMode) +
        " routes=" + std::to_string(m_routesReady) +
        "/" + std::to_string(m_expectedRoutes) +
        " period=" + std::to_string(measuredSamplePeriodMs));
    }
    m_predictiveFrontier = {};
    m_predictiveFrontier.streamId = m_definition.streamId;
    m_predictiveFrontier.sessionEpoch = m_definition.sessionEpoch;
    m_predictiveFrontier.mappingVersion = m_definition.mappingVersion;
    frontierPacket = makePredictiveControlPacket(
      makePredictiveFrontierName(m_definition.mappingRoot()),
      m_predictiveFrontier.wireEncode());
    m_payloadPackets[frontierPacket->getName()] = frontierPacket;
    m_measuredSamplePeriodMs = measuredSamplePeriodMs;
    m_state = LiveStreamLifecycleState::Active;
  }
  putIfPending(frontierPacket);
}

std::vector<LiveStreamItemReservation>
LiveStreamPublisher::reserveEntryBlock(const std::vector<StreamNameMapEntry>& entries)
{
  if (entries.empty() || entries.size() > m_definition.mappingBlockCapacity) {
    throw std::invalid_argument("reservation must fit one Mapping block");
  }
  std::shared_ptr<ndn::Data> packet;
  std::vector<LiveStreamItemReservation> result;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_state == LiveStreamLifecycleState::Stopped ||
        m_state == LiveStreamLifecycleState::Failed) {
      throw std::logic_error("LiveStream publisher is not reservable");
    }
    if (m_nextCursor > m_definition.maxNameReservations -
                         m_definition.mappingBlockCapacity) {
      throw std::length_error("LiveStream name-reservation budget exhausted");
    }
    const auto blockNumber = m_mappingBlocks.size();
    StreamNameMapBlock block;
    block.contractVersion = m_definition.contractVersion;
    block.streamId = m_definition.streamId;
    block.sessionEpoch = m_definition.sessionEpoch;
    block.mappingVersion = m_definition.mappingVersion;
    block.blockNumber = blockNumber;
    block.blockCapacity = m_definition.mappingBlockCapacity;
    block.firstCursor = blockNumber * block.blockCapacity;
    if (blockNumber > 0) {
      block.previousContentDigest = m_mappingBlocks.at(blockNumber - 1).contentDigest();
    }
    std::set<ndn::Name> supplied;
    for (size_t slot = 0; slot < block.blockCapacity; ++slot) {
      if (slot >= entries.size()) {
        block.entries.push_back(StreamNameMapEntry::makeTombstone());
        continue;
      }
      const auto& entry = entries[slot];
      const auto& name = entry.originalName;
      if (!m_definition.semanticDataPrefix.isPrefixOf(name) ||
          entry.tombstone ||
          !supplied.insert(name).second || m_reservations.count(name) != 0) {
        throw std::invalid_argument("semantic Data name is outside authority or reused");
      }
      const LiveStreamItemReservation reservation{
        block.firstCursor + slot, name, m_definition.sessionEpoch,
        m_definition.mappingVersion};
      block.entries.push_back(entry);
      result.push_back(reservation);
    }
    if (const auto error = block.validate()) {
      throw std::logic_error("generated invalid Mapping block: " + *error);
    }
    if (!block.fitsSignedWireBudget(512, m_definition.signedWireCap)) {
      throw std::length_error("Mapping block exceeds signed wire budget");
    }
    packet = std::make_shared<ndn::Data>(makeStreamNameMapBlockName(
      m_definition.mappingRoot(), m_definition.mappingVersion, blockNumber));
    packet->setContentType(ndn::tlv::ContentType_Manifest);
    packet->setFreshnessPeriod(ndn::time::seconds(10));
    packet->setContent(block.canonicalContent());
    m_keyChain.sign(*packet, m_signingInfo);
    if (packet->wireEncode().size() > m_definition.signedWireCap) {
      throw std::length_error("signed Mapping Data exceeds wire budget");
    }
    m_mappingBlocks.emplace(blockNumber, block);
    m_mappingPackets.emplace(packet->getName(), packet);
    for (const auto& reservation : result) {
      m_reservations.emplace(reservation.originalName, reservation);
    }
    m_nextCursor = (blockNumber + 1) * block.blockCapacity;
    notifyFeedsLocked(makePublishedPacket(PublishedLiveStreamPacketKind::Mapping,
                                          *packet, block.firstCursor));
  }
  putIfPending(packet);
  logStreamTimelineTrace("provider", "mapping-available", m_definition.streamId,
                         m_definition.sessionEpoch, result.front().cursor,
                         {{"block", std::to_string(result.front().cursor /
                           m_definition.mappingBlockCapacity)}});
  return result;
}

std::vector<LiveStreamItemReservation>
LiveStreamPublisher::reserveBlock(const std::vector<ndn::Name>& originalNames)
{
  std::vector<StreamNameMapEntry> entries;
  entries.reserve(originalNames.size());
  for (const auto& name : originalNames) {
    entries.push_back(StreamNameMapEntry::fromName(name));
  }
  return reserveEntryBlock(entries);
}

std::vector<LiveStreamItemReservation>
LiveStreamPublisher::reserveEntries(const std::vector<StreamNameMapEntry>& entries)
{
  if (entries.empty()) throw std::invalid_argument("empty LiveStream reservation");
  std::vector<LiveStreamItemReservation> result;
  for (size_t offset = 0; offset < entries.size();) {
    const auto count = std::min(m_definition.mappingBlockCapacity,
                                entries.size() - offset);
    std::vector<StreamNameMapEntry> blockEntries(entries.begin() + offset,
                                                  entries.begin() + offset + count);
    auto block = reserveEntryBlock(blockEntries);
    result.insert(result.end(), block.begin(), block.end());
    offset += count;
  }
  return result;
}

LiveStreamItemReservation
LiveStreamPublisher::reserveAhead(const ndn::Name& originalName)
{
  return reserveBlock({originalName}).front();
}

std::vector<LiveStreamItemReservation>
LiveStreamPublisher::reserveAhead(const std::vector<ndn::Name>& originalNames)
{
  if (originalNames.empty()) throw std::invalid_argument("empty LiveStream reservation");
  std::vector<LiveStreamItemReservation> result;
  for (size_t offset = 0; offset < originalNames.size();) {
    const auto count = std::min(m_definition.mappingBlockCapacity,
                                originalNames.size() - offset);
    std::vector<ndn::Name> blockNames(originalNames.begin() + offset,
                                      originalNames.begin() + offset + count);
    auto block = reserveBlock(blockNames);
    result.insert(result.end(), block.begin(), block.end());
    offset += count;
  }
  return result;
}

LiveStreamGroupReservation
LiveStreamPublisher::reserveGroup(const std::string& groupId,
                                  const std::vector<ndn::Name>& sourceNames,
                                  const std::vector<ndn::Name>& repairNames)
{
  std::vector<ndn::Name> all = sourceNames;
  all.insert(all.end(), repairNames.begin(), repairNames.end());
  auto reservations = reserveAhead(all);
  LiveStreamGroupReservation group;
  group.groupId = groupId;
  group.sources.assign(reservations.begin(), reservations.begin() + sourceNames.size());
  group.repairs.assign(reservations.begin() + sourceNames.size(), reservations.end());
  if (const auto error = group.validate(m_definition)) {
    throw std::invalid_argument("invalid LiveStream group: " + *error);
  }
  return group;
}

LiveStreamSampleReservation
LiveStreamPublisher::announceSample(
  uint64_t sampleId,
  const std::string& sampleClass,
  const std::function<ndn::Name(size_t, LiveStreamItemKind)>& nameFactory)
{
  if (m_definition.contractVersion != STREAM_NAME_MAP_CONTRACT_VERSION_V2 ||
      !nameFactory) {
    throw std::logic_error("adaptive sample announcement requires Mapping v2");
  }
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_sampleNameFactories.count(sampleId) != 0) {
      throw std::invalid_argument("sample id is already announced");
    }
  }
  const auto predictedSources = m_samplePredictor.predict(sampleClass);
  const auto predictedRepairs = m_definition.fec.repairItemCount();
  const auto groupId = std::to_string(sampleId);
  std::vector<StreamNameMapEntry> entries;
  entries.reserve(predictedSources + predictedRepairs);
  for (size_t index = 0; index < predictedSources; ++index) {
    entries.push_back(StreamNameMapEntry::fromGroupedName(
      nameFactory(index, LiveStreamItemKind::Source), groupId, sampleClass,
      index, predictedSources, predictedRepairs));
  }
  for (size_t index = 0; index < predictedRepairs; ++index) {
    entries.push_back(StreamNameMapEntry::fromGroupedName(
      nameFactory(index, LiveStreamItemKind::Repair), groupId, sampleClass,
      predictedSources + index, predictedSources, predictedRepairs));
  }
  auto reservations = reserveEntries(entries);
  LiveStreamSampleReservation result;
  result.sampleId = sampleId;
  result.sampleClass = sampleClass;
  result.predictedSourceItems = predictedSources;
  result.group.groupId = groupId;
  result.group.sources.assign(reservations.begin(),
                              reservations.begin() + predictedSources);
  result.group.repairs.assign(reservations.begin() + predictedSources,
                              reservations.end());
  if (const auto error = result.validate(m_definition)) {
    throw std::logic_error("generated invalid sample reservation: " + *error);
  }
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!m_sampleNameFactories.emplace(sampleId, nameFactory).second) {
      throw std::logic_error("concurrent duplicate sample announcement");
    }
  }
  return result;
}

void
LiveStreamPublisher::cleanupPendingLocked(uint64_t nowMs)
{
  const auto eraseExpired = [nowMs] (PendingInterestTable& table) {
    for (auto it = table.begin(); it != table.end();) {
      if (it->second.expiresAtMs <= nowMs) it = table.erase(it);
      else ++it;
    }
  };
  eraseExpired(m_pendingMappings);
  eraseExpired(m_pendingPayloads);
}

bool
LiveStreamPublisher::admitPendingLocked(PendingInterestTable& table,
                                        size_t capacity,
                                        const ndn::Name& name,
                                        uint64_t order,
                                        uint64_t expiresAtMs)
{
  if (capacity == 0 || expiresAtMs <= streamNowMs()) return false;
  if (table.count(name) != 0) return true;
  if (table.size() >= capacity) {
    const auto farthest = std::max_element(
      table.begin(), table.end(), [] (const auto& left, const auto& right) {
        return left.second.order < right.second.order;
      });
    if (farthest == table.end() || farthest->second.order <= order) return false;
    table.erase(farthest);
  }
  table.emplace(name, PendingInterest{order, expiresAtMs, false});
  return true;
}

void
LiveStreamPublisher::putIfPending(const std::shared_ptr<ndn::Data>& data)
{
  bool shouldPut = false;
  bool payloadHit = false;
  std::optional<StreamCursor> payloadCursor;
  bool payloadRetry = false;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    cleanupPendingLocked(streamNowMs());
    shouldPut = m_pendingMappings.erase(data->getName()) != 0;
    const auto pendingPayload = m_pendingPayloads.find(data->getName());
    payloadHit = pendingPayload != m_pendingPayloads.end();
    if (payloadHit) {
      payloadRetry = pendingPayload->second.retry;
      m_pendingPayloads.erase(pendingPayload);
    }
    shouldPut = shouldPut || payloadHit;
    if (payloadHit) {
      ++m_providerFutureHits;
      if (payloadRetry) ++m_providerRetryFutureHits;
      else ++m_providerInitialFutureHits;
      const auto reservation = m_reservations.find(data->getName());
      if (reservation != m_reservations.end()) payloadCursor = reservation->second.cursor;
    }
  }
  if (shouldPut) {
    m_face.put(*data);
    if (payloadCursor) {
      logStreamTimelineTrace("provider", "data-put", m_definition.streamId,
                             m_definition.sessionEpoch, *payloadCursor);
    }
  }
}

bool
LiveStreamPublisher::publishSignedData(
  const std::shared_ptr<ndn::Data>& signedData)
{
  if (!signedData || signedData->getContent().value_size() == 0) {
    throw std::invalid_argument("predictive Data is empty");
  }
  const auto& name = signedData->getName();
  const auto root = m_definition.mappingRoot();
  if (!root.isPrefixOf(name) || name.size() != root.size() + 3 ||
      name[root.size()].toUri() != "v" ||
      !name[root.size() + 1].isNumber() ||
      name[root.size() + 1].toNumber() != m_definition.mappingVersion ||
      !name[root.size() + 2].isSequenceNumber()) {
    throw std::invalid_argument("non-canonical predictive Data name");
  }
  const auto cursor = name[root.size() + 2].toSequenceNumber();
  const auto wire = signedData->wireEncode();
  if (wire.size() > m_definition.signedWireCap ||
      wire.size() > ndn::MAX_NDN_PACKET_SIZE) {
    throw std::length_error("predictive Data exceeds signed wire budget");
  }
  if (!verifyPredictiveSourceSignature(*signedData)) {
    throw std::invalid_argument(
      "predictive Data signature is invalid or outside provider authority");
  }

  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!m_predictiveMode || m_state != LiveStreamLifecycleState::Active) {
      throw std::logic_error("predictive publisher is not active");
    }
    const auto existing = m_payloadPackets.find(name);
    if (existing != m_payloadPackets.end()) {
      if (existing->second->wireEncode() == wire) {
        ++m_predictiveDuplicates;
        return false;
      }
      m_state = LiveStreamLifecycleState::Failed;
      m_reason = "predictive-name-equivocation";
      throw std::logic_error(m_reason);
    }
    if (cursor != m_predictiveNextExpectedCursor) {
      throw std::invalid_argument(
        "predictive Data sequence is not the next expected cursor");
    }
    const LiveStreamItemReservation reservation{
      cursor, name, m_definition.sessionEpoch, m_definition.mappingVersion};
    m_reservations.emplace(name, reservation);
    m_materialized.insert(cursor);
    m_payloadPackets.emplace(name, signedData);
    m_retentionOrder.push_back(name);
    while (m_retentionOrder.size() > m_definition.retainedItems) {
      const auto oldest = m_retentionOrder.front();
      m_retentionOrder.pop_front();
      const auto oldReservation = m_reservations.find(oldest);
      if (oldReservation != m_reservations.end()) {
        m_materialized.erase(oldReservation->second.cursor);
        m_reservations.erase(oldReservation);
      }
      m_payloadPackets.erase(oldest);
    }
    m_predictiveLatestProducedCursor = cursor;
    ++m_predictiveNextExpectedCursor;
    notifyFeedsLocked(makePublishedPacket(
      PublishedLiveStreamPacketKind::Source, *signedData, cursor));
  }
  logStreamTimelineTrace("provider", "signed-and-materialized",
                         m_definition.streamId,
                         m_definition.sessionEpoch, cursor);
  putIfPending(signedData);
  return true;
}

bool
LiveStreamPublisher::verifyPredictiveSourceSignature(
  const ndn::Data& data) const
{
  try {
    const auto signatureType = data.getSignatureType();
    if ((signatureType != ndn::tlv::SignatureSha256WithRsa &&
         signatureType != ndn::tlv::SignatureSha256WithEcdsa) ||
        !data.getSignatureInfo().hasKeyLocator()) {
      return false;
    }
    const auto keyLocator = data.getSignatureInfo().getKeyLocator();
    if (keyLocator.getType() != ndn::tlv::Name) {
      return false;
    }
    const auto& locatorName = keyLocator.getName();
    ndn::Name signerIdentity;
    ndn::security::Certificate certificate;
    if (ndn::security::Certificate::isValidName(locatorName)) {
      signerIdentity =
        ndn::security::extractIdentityFromCertName(locatorName);
      const auto keyName =
        ndn::security::extractKeyNameFromCertName(locatorName);
      certificate = m_keyChain.getPib().getIdentity(signerIdentity)
                      .getKey(keyName).getCertificate(locatorName);
    }
    else if (ndn::security::isValidKeyName(locatorName)) {
      signerIdentity = ndn::security::extractIdentityFromKeyName(locatorName);
      certificate = m_keyChain.getPib().getIdentity(signerIdentity)
                      .getKey(locatorName).getDefaultCertificate();
    }
    else {
      return false;
    }
    return signerIdentity == m_definition.provider &&
           ndn::security::verifySignature(data, certificate);
  }
  catch (const std::exception&) {
    return false;
  }
}

std::shared_ptr<ndn::Data>
LiveStreamPublisher::makePredictiveControlPacket(
  const ndn::Name& name, const ndn::Block& content) const
{
  auto packet = std::make_shared<ndn::Data>(name);
  packet->setContentType(ndn::tlv::ContentType_Manifest);
  packet->setFreshnessPeriod(ndn::time::milliseconds(
    std::max<uint64_t>(100, static_cast<uint64_t>(
      std::ceil(m_definition.samplePeriodMs * 4.0)))));
  packet->setContent(content);
  m_keyChain.sign(*packet, m_signingInfo);
  if (packet->wireEncode().size() > m_definition.signedWireCap) {
    throw std::length_error(
      "signed predictive control Data exceeds wire budget");
  }
  return packet;
}

PredictiveStreamFrontier
LiveStreamPublisher::commitPredictiveGroup(
  uint64_t groupId,
  const std::vector<std::shared_ptr<ndn::Data>>& signedSources)
{
  if (signedSources.empty()) {
    throw std::invalid_argument("cannot commit an empty predictive group");
  }

  std::vector<std::shared_ptr<ndn::Data>> stagedPackets;
  PredictiveStreamGroupCommit group;
  PredictiveStreamFrontier frontier;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!m_predictiveMode || m_state != LiveStreamLifecycleState::Active) {
      throw std::logic_error("predictive publisher is not active");
    }
    if (m_predictiveGroups.count(groupId) != 0 ||
        (!m_predictiveGroupRetentionOrder.empty() &&
         groupId != m_predictiveGroupRetentionOrder.back() + 1) ||
        (m_predictiveGroupRetentionOrder.empty() && groupId != 0)) {
      throw std::invalid_argument(
        "predictive group id is not the next expected group");
    }
    if (m_definition.fec.enabled() &&
        signedSources.size() > m_definition.fec.maxSourceItems) {
      throw std::length_error(
        "predictive group exceeds configured FEC source capacity");
    }

    group.streamId = m_definition.streamId;
    group.sessionEpoch = m_definition.sessionEpoch;
    group.mappingVersion = m_definition.mappingVersion;
    group.groupId = groupId;
    group.createdMs = streamNowMs();
    group.expiresMs = group.createdMs +
      (m_definition.fec.enabled()
         ? m_definition.fec.recoveryBudgetMs
         : 1000);
    group.recoveryCapacity = m_definition.fec.recoveryCapacity();

    std::vector<LiveStreamItemReservation> sourceReservations;
    std::vector<LiveStreamItemReservation> repairReservations;
    std::vector<std::vector<uint8_t>> sourceWires;
    sourceReservations.reserve(signedSources.size());
    sourceWires.reserve(signedSources.size());
    for (const auto& source : signedSources) {
      if (!source) {
        throw std::invalid_argument("null source in predictive group");
      }
      const auto found = m_payloadPackets.find(source->getName());
      if (found == m_payloadPackets.end() ||
          found->second->wireEncode() != source->wireEncode()) {
        throw std::invalid_argument(
          "predictive group source was not admitted byte-for-byte");
      }
      const auto& name = source->getName();
      const auto cursor =
        name[name.size() - 1].toSequenceNumber();
      const auto wire = source->wireEncode();
      std::vector<uint8_t> bytes(wire.begin(), wire.end());
      group.sourceNames.push_back(name);
      group.sourceWireLengths.push_back(bytes.size());
      group.sourceWireDigests.push_back(computeStreamContentDigest(
        ndn::span<const uint8_t>(bytes.data(), bytes.size())));
      sourceReservations.push_back(
        {cursor, name, m_definition.sessionEpoch, m_definition.mappingVersion});
      sourceWires.push_back(std::move(bytes));
    }

    const auto repairCount = m_definition.fec.repairItemCount();
    for (size_t index = 0; index < repairCount; ++index) {
      const auto repairName =
        makePredictiveRepairName(m_definition, groupId, index);
      group.repairNames.push_back(repairName);
      repairReservations.push_back({
        sourceReservations.back().cursor + 1 + index,
        repairName,
        m_definition.sessionEpoch,
        m_definition.mappingVersion,
      });
    }
    if (const auto error = group.validate(m_definition)) {
      throw std::logic_error(
        "generated invalid predictive group: " + *error);
    }

    if (!repairReservations.empty()) {
      const auto repairs = makeLiveStreamRepairSymbols(
        m_definition, std::to_string(groupId), sourceReservations,
        repairReservations, sourceWires, group.createdMs, group.expiresMs);
      for (size_t index = 0; index < repairs.size(); ++index) {
        stagedPackets.push_back(makePredictiveControlPacket(
          group.repairNames.at(index), repairs[index].wireEncode()));
      }
    }

    stagedPackets.push_back(makePredictiveControlPacket(
      makePredictiveGroupName(m_definition, groupId), group.wireEncode()));

    frontier = m_predictiveFrontier;
    frontier.latestCommittedGroupId = groupId;
    frontier.retainedGroupCommitNames.push_back(
      makePredictiveGroupName(m_definition, groupId));
    frontier.retainedGroupFirstCursors.push_back(
      sourceReservations.front().cursor);
    frontier.retainedGroupLastCursors.push_back(
      sourceReservations.back().cursor);
    frontier.checkpoint.initialSampleId = 0;
    frontier.checkpoint.nextExpectedSampleId =
      m_predictiveNextExpectedCursor;
    frontier.checkpoint.latestProducedSampleId =
      m_predictiveLatestProducedCursor.value_or(0);
    if (!m_retentionOrder.empty()) {
      const auto& oldest = m_retentionOrder.front();
      frontier.checkpoint.oldestRetainedSampleId =
        oldest[oldest.size() - 1].toSequenceNumber();
    }
    auto maxGroups = std::max<size_t>(
      1, m_definition.retainedItems /
           std::max<size_t>(1, signedSources.size()));
    while (frontier.retainedGroupCommitNames.size() > maxGroups) {
      frontier.retainedGroupCommitNames.erase(
        frontier.retainedGroupCommitNames.begin());
      frontier.retainedGroupFirstCursors.erase(
        frontier.retainedGroupFirstCursors.begin());
      frontier.retainedGroupLastCursors.erase(
        frontier.retainedGroupLastCursors.begin());
    }
    if (const auto error = frontier.validate(m_definition)) {
      throw std::logic_error(
        "generated invalid predictive frontier: " + *error);
    }
    std::shared_ptr<ndn::Data> frontierPacket;
    while (!frontierPacket) {
      try {
        frontierPacket = makePredictiveControlPacket(
          makePredictiveFrontierName(m_definition.mappingRoot()),
          frontier.wireEncode());
      }
      catch (const std::length_error&) {
        // Retention is bounded by both item capacity and the single-packet
        // authenticated frontier wire budget.  Drop only the oldest recovery
        // reference; the newly committed group must always remain reachable.
        if (frontier.retainedGroupCommitNames.size() <= 1) {
          throw;
        }
        frontier.retainedGroupCommitNames.erase(
          frontier.retainedGroupCommitNames.begin());
        frontier.retainedGroupFirstCursors.erase(
          frontier.retainedGroupFirstCursors.begin());
        frontier.retainedGroupLastCursors.erase(
          frontier.retainedGroupLastCursors.begin());
      }
    }
    maxGroups = std::min(
      maxGroups, frontier.retainedGroupCommitNames.size());
    stagedPackets.push_back(std::move(frontierPacket));

    m_predictiveGroups.emplace(groupId, group);
    m_predictiveGroupRetentionOrder.push_back(groupId);
    while (m_predictiveGroupRetentionOrder.size() > maxGroups) {
      const auto evicted = m_predictiveGroupRetentionOrder.front();
      m_predictiveGroupRetentionOrder.pop_front();
      const auto evictedGroup = m_predictiveGroups.find(evicted);
      if (evictedGroup != m_predictiveGroups.end()) {
        for (const auto& repairName : evictedGroup->second.repairNames) {
          m_payloadPackets.erase(repairName);
        }
        m_payloadPackets.erase(
          makePredictiveGroupName(m_definition, evicted));
        m_predictiveGroups.erase(evictedGroup);
      }
    }
    for (const auto& packet : stagedPackets) {
      m_payloadPackets[packet->getName()] = packet;
    }
    m_predictiveFrontier = frontier;
  }

  for (const auto& packet : stagedPackets) {
    putIfPending(packet);
  }
  return frontier;
}

PredictiveStreamFrontier
LiveStreamPublisher::predictiveFrontier() const
{
  std::lock_guard<std::mutex> guard(m_mutex);
  if (!m_predictiveMode) {
    throw std::logic_error("publisher is not in predictive mode");
  }
  return m_predictiveFrontier;
}

void
LiveStreamPublisher::publish(const LiveStreamItemReservation& reservation,
                             const std::vector<uint8_t>& opaqueContent)
{
  if (opaqueContent.empty()) throw std::invalid_argument("LiveStream content is empty");
  std::shared_ptr<ndn::Data> packet;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    const auto found = m_reservations.find(reservation.originalName);
    if (!reservation.belongsTo(m_definition) || found == m_reservations.end() ||
        found->second.cursor != reservation.cursor) {
      throw std::invalid_argument("unknown LiveStream reservation");
    }
    if (!m_materialized.insert(reservation.cursor).second) {
      throw std::logic_error("LiveStream reservation already materialized");
    }
    try {
      packet = makePayloadPacket(reservation, opaqueContent);
    }
    catch (...) {
      m_materialized.erase(reservation.cursor);
      throw;
    }
    m_payloadPackets.emplace(reservation.originalName, packet);
    m_retentionOrder.push_back(reservation.originalName);
    while (m_retentionOrder.size() > m_definition.retainedItems) {
      m_payloadPackets.erase(m_retentionOrder.front());
      m_retentionOrder.pop_front();
    }
    notifyFeedsLocked(makePublishedPacket(PublishedLiveStreamPacketKind::Source,
                                          *packet, reservation.cursor));
  }
  logStreamTimelineTrace("provider", "signed-and-materialized",
                         m_definition.streamId, m_definition.sessionEpoch,
                         reservation.cursor);
  putIfPending(packet);
}

std::shared_ptr<ndn::Data>
LiveStreamPublisher::makePayloadPacket(
  const LiveStreamItemReservation& reservation,
  const std::vector<uint8_t>& opaqueContent,
  const LiveStreamSampleEnvelope* envelope) const
{
  if (opaqueContent.empty()) throw std::invalid_argument("LiveStream content is empty");
  auto packet = std::make_shared<ndn::Data>(reservation.originalName);
  packet->setFreshnessPeriod(ndn::time::milliseconds(1000));
  if (envelope) {
    auto protectedEnvelope = *envelope;
    protectedEnvelope.opaqueContent = opaqueContent;
    const auto wire = protectedEnvelope.wireEncode();
    packet->setContent(ndn::span<const uint8_t>(wire.begin(), wire.size()));
  }
  else {
    packet->setContent(ndn::span<const uint8_t>(opaqueContent.data(), opaqueContent.size()));
  }
  m_keyChain.sign(*packet, m_signingInfo);
  if (packet->wireEncode().size() > m_definition.signedWireCap) {
    throw std::length_error("signed LiveStream Data exceeds wire budget");
  }
  return packet;
}

void
LiveStreamPublisher::publishGroup(
  const LiveStreamGroupReservation& reservation,
  const std::vector<std::vector<uint8_t>>& opaqueSources)
{
  publishGroupImpl(reservation, opaqueSources);
}

void
LiveStreamPublisher::publishGroupImpl(
  const LiveStreamGroupReservation& reservation,
  const std::vector<std::vector<uint8_t>>& opaqueSources,
  const std::string* sampleClass,
  size_t actualSourceItems)
{
  if (const auto error = reservation.validate(m_definition)) {
    throw std::invalid_argument("invalid LiveStream group: " + *error);
  }
  if (opaqueSources.size() != reservation.sources.size()) {
    throw std::invalid_argument("opaque source count mismatch");
  }
  const bool adaptive = sampleClass != nullptr;
  if (adaptive && (sampleClass->empty() ||
                   actualSourceItems < opaqueSources.size())) {
    throw std::invalid_argument("invalid adaptive sample extent");
  }
  std::vector<std::vector<uint8_t>> repairWires;
  if (!reservation.repairs.empty()) {
    const auto now = streamNowMs();
    const auto repairs = makeLiveStreamRepairSymbols(
      m_definition, reservation.groupId, reservation.sources,
      reservation.repairs, opaqueSources, now,
      now + m_definition.fec.recoveryBudgetMs);
    for (const auto& repair : repairs) {
      const auto wire = repair.wireEncode();
      repairWires.emplace_back(wire.begin(), wire.end());
    }
  }
  std::vector<std::shared_ptr<ndn::Data>> packets;
  packets.reserve(reservation.sources.size() + reservation.repairs.size());
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    auto stage = [this, &packets] (const LiveStreamItemReservation& item,
                                   const std::vector<uint8_t>& content,
                                   const LiveStreamSampleEnvelope* envelope) {
      const auto found = m_reservations.find(item.originalName);
      if (!item.belongsTo(m_definition) || found == m_reservations.end() ||
          found->second.cursor != item.cursor) {
        throw std::invalid_argument("unknown LiveStream group reservation");
      }
      if (m_materialized.count(item.cursor) != 0) {
        throw std::logic_error("LiveStream group reservation already materialized");
      }
      packets.push_back(makePayloadPacket(item, content, envelope));
    };
    for (size_t i = 0; i < opaqueSources.size(); ++i) {
      LiveStreamSampleEnvelope envelope;
      if (adaptive) {
        envelope.groupId = reservation.groupId;
        envelope.sampleClass = *sampleClass;
        envelope.groupItemIndex = i;
        envelope.actualSourceItems = actualSourceItems;
        envelope.itemKind = LiveStreamItemKind::Source;
      }
      stage(reservation.sources[i], opaqueSources[i], adaptive ? &envelope : nullptr);
    }
    for (size_t repairIndex = 0; repairIndex < reservation.repairs.size(); ++repairIndex) {
      LiveStreamSampleEnvelope envelope;
      if (adaptive) {
        envelope.groupId = reservation.groupId;
        envelope.sampleClass = *sampleClass;
        envelope.groupItemIndex = reservation.repairs[repairIndex].cursor -
                                  reservation.sources.front().cursor;
        envelope.actualSourceItems = actualSourceItems;
        envelope.itemKind = LiveStreamItemKind::Repair;
      }
      stage(reservation.repairs[repairIndex], repairWires.at(repairIndex),
            adaptive ? &envelope : nullptr);
    }

    // Commit only after every source and repair packet has been staged.
    for (size_t i = 0; i < packets.size(); ++i) {
      const auto& item = i < reservation.sources.size() ?
        reservation.sources[i] : reservation.repairs.at(i - reservation.sources.size());
      m_materialized.insert(item.cursor);
      if (i >= reservation.sources.size()) m_repairCursors.insert(item.cursor);
      m_payloadPackets[item.originalName] = packets[i];
      m_retentionOrder.push_back(item.originalName);
      notifyFeedsLocked(makePublishedPacket(
        i < reservation.sources.size() ? PublishedLiveStreamPacketKind::Source :
                                         PublishedLiveStreamPacketKind::Repair,
        *packets[i], item.cursor));
    }
    while (m_retentionOrder.size() > m_definition.retainedItems) {
      m_payloadPackets.erase(m_retentionOrder.front());
      m_retentionOrder.pop_front();
    }
  }
  for (size_t i = 0; i < packets.size(); ++i) {
    const auto& item = i < reservation.sources.size() ?
      reservation.sources[i] : reservation.repairs.at(i - reservation.sources.size());
    logStreamTimelineTrace("provider", "signed-and-materialized",
                           m_definition.streamId, m_definition.sessionEpoch,
                           item.cursor,
                           {{"packet_kind", i < reservation.sources.size() ?
                                             "source" : "repair"}});
    putIfPending(packets[i]);
  }
}

std::vector<LiveStreamItemReservation>
LiveStreamPublisher::prepareSampleExtent(
  const LiveStreamSampleReservation& reservation,
  size_t actualSourceItems)
{
  if (const auto error = reservation.validate(m_definition)) {
    throw std::invalid_argument("invalid LiveStream sample: " + *error);
  }
  const auto profile = std::find_if(
    m_definition.sampleClasses.begin(), m_definition.sampleClasses.end(),
    [&reservation] (const auto& value) {
      return value.classId == reservation.sampleClass;
    });
  if (actualSourceItems == 0 || profile == m_definition.sampleClasses.end() ||
      actualSourceItems > profile->hardMaxSourceItems) {
    throw std::invalid_argument("sample source count exceeds class bounds");
  }
  std::function<ndn::Name(size_t, LiveStreamItemKind)> nameFactory;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    const auto prepared = m_preparedSampleContinuations.find(reservation.sampleId);
    if (prepared != m_preparedSampleContinuations.end()) {
      if (prepared->second.first != actualSourceItems) {
        throw std::logic_error("sample extent cannot change after preparation");
      }
      std::vector<LiveStreamItemReservation> result(
        reservation.group.sources.begin(),
        reservation.group.sources.begin() +
          std::min(actualSourceItems, reservation.predictedSourceItems));
      result.insert(result.end(), prepared->second.second.sources.begin(),
                    prepared->second.second.sources.end());
      return result;
    }
    const auto found = m_sampleNameFactories.find(reservation.sampleId);
    if (found == m_sampleNameFactories.end()) {
      throw std::invalid_argument("sample was not announced by this publisher");
    }
    nameFactory = found->second;
  }

  LiveStreamGroupReservation tail;
  if (actualSourceItems > reservation.predictedSourceItems) {
    const auto tailCount = actualSourceItems - reservation.predictedSourceItems;
    tail.groupId = reservation.group.groupId + ":continuation";
    std::vector<StreamNameMapEntry> entries;
    entries.reserve(tailCount);
    for (size_t index = 0; index < tailCount; ++index) {
      entries.push_back(StreamNameMapEntry::fromGroupedName(
        nameFactory(reservation.predictedSourceItems + index,
                    LiveStreamItemKind::Source),
        tail.groupId, reservation.sampleClass, index, tailCount, 0));
    }
    tail.sources = reserveEntries(entries);
  }
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!m_preparedSampleContinuations.emplace(
          reservation.sampleId, std::make_pair(actualSourceItems, tail)).second) {
      throw std::logic_error("concurrent sample extent preparation");
    }
  }
  std::vector<LiveStreamItemReservation> result(
    reservation.group.sources.begin(),
    reservation.group.sources.begin() +
      std::min(actualSourceItems, reservation.predictedSourceItems));
  result.insert(result.end(), tail.sources.begin(), tail.sources.end());
  return result;
}

void
LiveStreamPublisher::publishSample(
  const LiveStreamSampleReservation& reservation,
  const std::vector<std::vector<uint8_t>>& opaqueSources)
{
  if (const auto error = reservation.validate(m_definition)) {
    throw std::invalid_argument("invalid LiveStream sample: " + *error);
  }
  const auto profile = std::find_if(
    m_definition.sampleClasses.begin(), m_definition.sampleClasses.end(),
    [&reservation] (const auto& value) {
      return value.classId == reservation.sampleClass;
    });
  if (opaqueSources.empty() || profile == m_definition.sampleClasses.end() ||
      opaqueSources.size() > profile->hardMaxSourceItems) {
    throw std::invalid_argument("sample source count exceeds class bounds");
  }
  const auto preparedSources = prepareSampleExtent(reservation, opaqueSources.size());
  if (preparedSources.size() != opaqueSources.size()) {
    throw std::logic_error("prepared sample extent does not match source bytes");
  }
  LiveStreamGroupReservation tail;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    tail = m_preparedSampleContinuations.at(reservation.sampleId).second;
  }

  const auto baseCount = std::min(opaqueSources.size(),
                                  reservation.predictedSourceItems);
  LiveStreamGroupReservation base = reservation.group;
  base.sources.resize(baseCount);
  std::vector<std::vector<uint8_t>> baseOpaque(
    opaqueSources.begin(), opaqueSources.begin() + baseCount);
  publishGroupImpl(base, baseOpaque, &reservation.sampleClass,
                   opaqueSources.size());

  if (opaqueSources.size() > reservation.predictedSourceItems) {
    const auto tailCount = opaqueSources.size() - reservation.predictedSourceItems;
    std::vector<std::vector<uint8_t>> tailOpaque(
      opaqueSources.begin() + reservation.predictedSourceItems,
      opaqueSources.end());
    publishGroupImpl(tail, tailOpaque, &reservation.sampleClass, tailCount);
  }
  if (!m_samplePredictor.observe(reservation.sampleClass, opaqueSources.size())) {
    throw std::logic_error("published sample was not admitted by predictor");
  }
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_sampleNameFactories.erase(reservation.sampleId);
    m_preparedSampleContinuations.erase(reservation.sampleId);
  }
}

LiveStreamDescriptor
LiveStreamPublisher::activate(const LiveStreamReadiness& readiness)
{
  std::lock_guard<std::mutex> guard(m_mutex);
  if ((m_state != LiveStreamLifecycleState::Preparing &&
       m_state != LiveStreamLifecycleState::Active) || m_routeFailed ||
      m_routesReady != 2 || readiness.measuredSamplePeriodMs <= 0.0 ||
      !std::isfinite(readiness.measuredSamplePeriodMs) || m_mappingBlocks.empty() ||
      m_materialized.count(readiness.safeJoinCursor) == 0) {
    throw std::logic_error("LiveStream publisher is not ready for atomic activation");
  }
  const auto anchor = readiness.safeJoinCursor / m_definition.mappingBlockCapacity;
  LiveStreamDescriptor descriptor;
  descriptor.definition = m_definition;
  descriptor.measuredSamplePeriodMs = readiness.measuredSamplePeriodMs;
  descriptor.safeJoinCursor = readiness.safeJoinCursor;
  descriptor.checkpoint.blockNumber = anchor;
  descriptor.checkpoint.contentDigest = m_mappingBlocks.at(anchor).contentDigest();
  descriptor.checkpoint.frontiers.oldestRetained = m_retentionOrder.empty() ?
    readiness.safeJoinCursor : m_reservations.at(m_retentionOrder.front()).cursor;
  descriptor.checkpoint.frontiers.latestJoin = readiness.safeJoinCursor;
  descriptor.checkpoint.frontiers.latestProduced = *m_materialized.rbegin();
  descriptor.checkpoint.frontiers.mappingCommittedThrough = m_nextCursor - 1;
  descriptor.checkpoint.frontiers.nextReserved = m_nextCursor;
  if (const auto error = descriptor.validate()) {
    throw std::logic_error("invalid activated LiveStream descriptor: " + *error);
  }
  m_latestJoinCursor = readiness.safeJoinCursor;
  m_measuredSamplePeriodMs = readiness.measuredSamplePeriodMs;
  m_state = LiveStreamLifecycleState::Active;
  return descriptor;
}

LiveStreamStatus
LiveStreamPublisher::status() const
{
  std::lock_guard<std::mutex> guard(m_mutex);
  LiveStreamStatus result;
  result.state = m_state;
  result.retainedItems = m_payloadPackets.size();
  const auto nowMs = streamNowMs();
  const auto activeCount = [nowMs] (const PendingInterestTable& table) {
    return static_cast<size_t>(std::count_if(
      table.begin(), table.end(), [nowMs] (const auto& entry) {
        return entry.second.expiresAtMs > nowMs;
      }));
  };
  result.pendingInterests = activeCount(m_pendingMappings) + activeCount(m_pendingPayloads);
  result.mappingBlocks = m_mappingBlocks.size();
  result.reason = m_reason;
  result.frontiers.oldestRetained = m_retentionOrder.empty() ? 0 :
    m_reservations.at(m_retentionOrder.front()).cursor;
  result.frontiers.latestProduced = m_materialized.empty() ? 0 : *m_materialized.rbegin();
  result.frontiers.latestJoin = m_latestJoinCursor;
  result.frontiers.mappingCommittedThrough = m_nextCursor == 0 ? 0 : m_nextCursor - 1;
  result.frontiers.nextReserved = m_nextCursor;
  result.providerFutureInterests = m_providerFutureInterests;
  result.providerFutureHits = m_providerFutureHits;
  result.providerInitialFutureInterests = m_providerInitialFutureInterests;
  result.providerInitialFutureHits = m_providerInitialFutureHits;
  result.providerRetryFutureInterests = m_providerRetryFutureInterests;
  result.providerRetryFutureHits = m_providerRetryFutureHits;
  result.declaredRecoveryCapacity = m_definition.fec.recoveryCapacity();
  result.sampleClassPredictions = m_samplePredictor.statuses();
  return result;
}

void
LiveStreamPublisher::stop()
{
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_state == LiveStreamLifecycleState::Stopped) return;
    m_state = LiveStreamLifecycleState::Stopped;
    m_reason = "stopped";
    m_pendingMappings.clear();
    m_pendingPayloads.clear();
    for (auto& weakFeed : m_packetFeeds) {
      if (auto feed = weakFeed.lock()) feed->close();
    }
    m_packetFeeds.clear();
    m_mappingRoute.cancel();
    m_payloadRoute.cancel();
  }
  m_routeCondition.notify_all();
}

void
LiveStreamPublisher::onMappingInterest(const ndn::Interest& interest)
{
  std::shared_ptr<ndn::Data> packet;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    const auto found = m_mappingPackets.find(interest.getName());
    if (found != m_mappingPackets.end()) packet = found->second;
    else {
      const auto nowMs = streamNowMs();
      cleanupPendingLocked(nowMs);
      const auto root = m_definition.mappingRoot();
      const auto& name = interest.getName();
      if (root.isPrefixOf(name) && name.size() == root.size() + 2 &&
          name[root.size()].isVersion() &&
          name[root.size()].toVersion() == m_definition.mappingVersion &&
          name[root.size() + 1].isSequenceNumber()) {
        const auto block = name[root.size() + 1].toSequenceNumber();
        const auto nextBlock = static_cast<uint64_t>(m_mappingBlocks.size());
        const auto lifetimeMs = std::min<uint64_t>(
            30000, std::max<int64_t>(1, interest.getInterestLifetime().count()));
        const auto effectiveAhead = computeLiveStreamMappingLead(
          static_cast<double>(lifetimeMs),
          std::max(1.0, m_definition.samplePeriodMs), 0.0,
          m_definition.mappingAheadBlocks,
          std::max(m_definition.mappingAheadBlocks,
                   m_definition.maxPendingInterests));
        if (block >= nextBlock && block - nextBlock < effectiveAhead) {
          admitPendingLocked(m_pendingMappings,
            effectiveAhead, name, block,
            nowMs + lifetimeMs);
        }
      }
    }
  }
  if (packet) m_face.put(*packet);
}

void
LiveStreamPublisher::onPayloadInterest(const ndn::Interest& interest)
{
  std::shared_ptr<ndn::Data> packet;
  std::optional<StreamCursor> futureCursor;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    const auto found = m_payloadPackets.find(interest.getName());
    if (found != m_payloadPackets.end()) packet = found->second;
    else {
      std::optional<StreamCursor> pendingCursor;
      if (const auto reservation = m_reservations.find(interest.getName());
          reservation != m_reservations.end() &&
          m_materialized.count(reservation->second.cursor) == 0) {
        pendingCursor = reservation->second.cursor;
      }
      else if (m_predictiveMode) {
        const auto root = m_definition.mappingRoot();
        const auto& name = interest.getName();
        if (root.isPrefixOf(name) && name.size() == root.size() + 3 &&
            name[root.size()].toUri() == "v" &&
            name[root.size() + 1].isNumber() &&
            name[root.size() + 1].toNumber() == m_definition.mappingVersion &&
            name[root.size() + 2].isSequenceNumber()) {
          pendingCursor = name[root.size() + 2].toSequenceNumber();
        }
      }
      if (pendingCursor) {
        const auto nowMs = streamNowMs();
        cleanupPendingLocked(nowMs);
        const auto lifetimeMs = std::min<uint64_t>(
          30000, std::max<int64_t>(1, interest.getInterestLifetime().count()));
        const bool alreadyPending =
          m_pendingPayloads.count(interest.getName()) != 0;
        if (admitPendingLocked(m_pendingPayloads,
                              m_definition.maxPendingInterests,
                              interest.getName(), *pendingCursor,
                              nowMs + lifetimeMs) && !alreadyPending) {
          const bool retry =
            !m_seenFuturePayloadNames.insert(interest.getName()).second;
          m_pendingPayloads.at(interest.getName()).retry = retry;
          ++m_providerFutureInterests;
          if (retry) ++m_providerRetryFutureInterests;
          else ++m_providerInitialFutureInterests;
          futureCursor = *pendingCursor;
        }
      }
    }
  }
  if (futureCursor) {
    logStreamTimelineTrace("provider", "payload-interest-arrived",
                           m_definition.streamId, m_definition.sessionEpoch,
                           *futureCursor);
  }
  if (packet) m_face.put(*packet);
}

LiveStreamConsumerHandle::LiveStreamConsumerHandle(
  LiveStreamDescriptor descriptor, LiveStreamOpenOptions options,
  ndn::Face& face, std::shared_ptr<MessageValidator> validator)
  : m_descriptor(std::move(descriptor))
  , m_options(std::move(options))
  , m_face(face)
  , m_validator(std::move(validator))
  , m_fetcher(std::make_unique<StreamAdaptiveFetcherState>())
{
  if (const auto error = m_descriptor.validate()) {
    throw std::invalid_argument("invalid LiveStream descriptor: " + *error);
  }
  if (!m_validator || !m_options.onItem || m_options.aggregateInterestLimit == 0) {
    throw std::invalid_argument("invalid LiveStream consumer options");
  }
  const bool adaptive = m_options.prefetchPolicy ==
                        LiveStreamPrefetchPolicy::AdaptiveSampleAtomic;
  if (adaptive != (m_descriptor.definition.contractVersion ==
                   STREAM_NAME_MAP_CONTRACT_VERSION_V2)) {
    throw std::invalid_argument("Mapping v2 requires adaptive-sample-atomic policy");
  }
  if (adaptive) {
    const auto largest = std::max_element(
      m_descriptor.definition.sampleClasses.begin(),
      m_descriptor.definition.sampleClasses.end(),
      [] (const auto& left, const auto& right) {
        return left.hardMaxSourceItems < right.hardMaxSourceItems;
      });
    const auto largestGroup = largest->hardMaxSourceItems +
                              m_descriptor.definition.fec.repairItemCount();
    constexpr size_t CONTROL_RESERVE = 5; // 4 Mapping + 1 retransmission
    if (m_options.aggregateInterestLimit <= CONTROL_RESERVE ||
        largestGroup > m_options.aggregateInterestLimit - CONTROL_RESERVE) {
      throw std::invalid_argument("aggregate Interest limit cannot hold one sample group");
    }
  }
}

const char*
toString(LiveStreamPrefetchPolicy policy)
{
  switch (policy) {
    case LiveStreamPrefetchPolicy::MappedPressure: return "mapped-pressure";
    case LiveStreamPrefetchPolicy::MappedLiveFutureOn: return "mapped-live-v1-future-on";
    case LiveStreamPrefetchPolicy::MappedLiveFutureOff: return "mapped-live-v1-future-off";
    case LiveStreamPrefetchPolicy::AdaptiveSampleAtomic: return "adaptive-sample-atomic";
  }
  return "unknown";
}

LiveStreamConsumerHandle::~LiveStreamConsumerHandle()
{
  stop();
}

void
LiveStreamConsumerHandle::start()
{
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_state == LiveStreamLifecycleState::Active) return;
    if (m_state != LiveStreamLifecycleState::Preparing) {
      throw std::logic_error("LiveStream consumer cannot be restarted");
    }
    StreamNameMapResolverConfig config;
    config.contractVersion = m_descriptor.definition.contractVersion;
    config.streamId = m_descriptor.definition.streamId;
    config.sessionEpoch = m_descriptor.definition.sessionEpoch;
    config.mappingVersion = m_descriptor.definition.mappingVersion;
    config.blockCapacity = m_descriptor.definition.mappingBlockCapacity;
    config.expectedProvider = m_descriptor.definition.provider;
    config.mappingRoot = m_descriptor.definition.mappingRoot();
    config.payloadPrefix = m_descriptor.definition.semanticDataPrefix;
    config.signedWireCap = m_descriptor.definition.signedWireCap;
    const auto resolverAhead = computeLiveStreamMappingLead(
      static_cast<double>(m_options.interestLifetimeMs),
      std::max(1.0, m_descriptor.measuredSamplePeriodMs), 0.0,
      m_descriptor.definition.mappingAheadBlocks,
      std::max(m_descriptor.definition.mappingAheadBlocks,
               m_descriptor.definition.maxPendingInterests));
    const auto retainedBlockBudget =
      (m_descriptor.definition.retainedItems +
       m_descriptor.definition.mappingBlockCapacity - 1) /
      m_descriptor.definition.mappingBlockCapacity;
    config.maxVerifiedBlocks = std::min<size_t>(
      STREAM_NAME_MAP_MAX_RESOLVER_BLOCKS,
      std::max<size_t>(2, retainedBlockBudget +
                           resolverAhead + 1));
    config.maxQuarantineBlocks = std::max<size_t>(1, resolverAhead);
    config.maxReverseEntries = m_descriptor.definition.maxNameReservations;
    m_resolver.reset(config, m_descriptor.checkpoint);
    m_nextCursor = m_options.start == LiveStreamStart::Latest ?
      m_descriptor.safeJoinCursor : m_descriptor.checkpoint.frontiers.oldestRetained;
    m_nextMappingBlock = m_nextCursor / m_descriptor.definition.mappingBlockCapacity;
    m_fetcher->configureMappedLive(m_options.aggregateInterestLimit, 4, 1,
                                   m_descriptor.definition.mappingBlockCapacity,
                                   "ndnsf-balanced-seed");
    // The paper's lambda_d counts one Data per media sample.  In this generic
    // API a sample may be a segmented/FEC group, so a one-packet reserve is not
    // equivalent: it leaves no room for Mapping latency or burst jitter.  Keep
    // one complete authenticated group as the bounded jitter/recovery reserve.
    // The fallback demand estimator observes application source items, while a
    // Mapping-v2 atomic issue also carries repair items. Account for the current
    // sample's repair items plus one complete following group; otherwise the
    // fallback window is smaller than two pipelined groups whenever the next
    // predicted Mapping extent is temporarily unavailable.
    if (m_descriptor.definition.fec.enabled()) {
      m_fetcher->recoveryReservePackets = std::max<uint64_t>(
        m_fetcher->recoveryReservePackets,
        m_descriptor.definition.fec.maxSourceItems +
          2 * m_descriptor.definition.fec.repairItemCount());
    }
    m_fetcher->resetMappedLive(m_descriptor.definition.sessionEpoch, m_nextCursor,
      m_descriptor.measuredSamplePeriodMs,
      m_descriptor.checkpoint.frontiers.latestProduced,
      m_descriptor.checkpoint.frontiers.mappingCommittedThrough,
      m_descriptor.checkpoint.frontiers.nextReserved, streamNowMs());
    if (m_options.prefetchPolicy == LiveStreamPrefetchPolicy::MappedPressure) {
      m_fetcher->setMappedLivePolicyEnabled(false);
    }
    m_state = LiveStreamLifecycleState::Active;
  }
  schedule();
  emitStatus();
}

bool
LiveStreamConsumerHandle::isActive(uint64_t generation) const
{
  std::lock_guard<std::mutex> guard(m_mutex);
  return m_state == LiveStreamLifecycleState::Active && m_generation == generation;
}

bool
LiveStreamConsumerHandle::isCurrentMappingRequest(uint64_t blockNumber,
                                                  uint64_t requestToken,
                                                  uint64_t generation) const
{
  std::lock_guard<std::mutex> guard(m_mutex);
  const auto found = m_mappingRequestTokens.find(blockNumber);
  return m_state == LiveStreamLifecycleState::Active &&
         m_generation == generation && found != m_mappingRequestTokens.end() &&
         found->second == requestToken;
}

void
LiveStreamConsumerHandle::emitStatus() const
{
  if (!m_options.onStatus) return;
  try {
    m_options.onStatus(status());
  }
  catch (const std::exception&) {
    // Observability callbacks must not unwind the Face event loop.
  }
}

bool
LiveStreamConsumerHandle::hasExpectedProviderSignature(const ndn::Data& data) const
{
  try {
    return data.getSignatureInfo().hasKeyLocator() &&
      data.getSignatureInfo().getKeyLocator().getType() == ndn::tlv::Name &&
      ndn::security::extractIdentityFromCertName(
        data.getSignatureInfo().getKeyLocator().getName()) ==
        m_descriptor.definition.provider;
  }
  catch (const std::exception&) {
    return false;
  }
}

void
LiveStreamConsumerHandle::fetchMapping(uint64_t blockNumber,
                                       uint64_t interestLifetimeMs,
                                       uint64_t requestToken)
{
  const auto weak = weak_from_this();
  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    generation = m_generation;
    ++m_mappingInterests;
  }
  ndn::Interest interest(makeStreamNameMapBlockName(
    m_descriptor.definition.mappingRoot(), m_descriptor.definition.mappingVersion,
    blockNumber));
  interest.setCanBePrefix(false);
  interest.setMustBeFresh(false);
  interest.setInterestLifetime(ndn::time::milliseconds(
    std::max<uint64_t>(1, interestLifetimeMs)));
  m_face.expressInterest(
    interest,
    [weak, blockNumber, requestToken, generation]
    (const ndn::Interest&, const ndn::Data& data) {
      if (const auto self = weak.lock()) {
        if (!self->isCurrentMappingRequest(blockNumber, requestToken, generation)) return;
        self->m_validator->validate(
          data,
          [weak, blockNumber, requestToken, generation] (const ndn::Data& validated) {
            const auto self = weak.lock();
            if (!self || !self->isCurrentMappingRequest(
                           blockNumber, requestToken, generation)) return;
            if (!self->hasExpectedProviderSignature(validated)) {
              self->fail("mapping-provider-mismatch");
              return;
            }
            VerifiedStreamNameMapData input;
            input.dataName = validated.getName();
            input.verifiedProvider = self->m_descriptor.definition.provider;
            input.contentType = validated.getContentType();
            input.hasFinalBlock = validated.getFinalBlock().has_value();
            input.signedWireSize = validated.wireEncode().size();
            input.content = validated.getContent();
            input.receivedMonotonicMs = streamNowMs();
            input.requiredBeforeMonotonicMs = input.receivedMonotonicMs + 1;
            const auto result = self->m_resolver.admitVerifiedBlock(input);
            const auto verifiedFrontiers = self->m_resolver.frontiers();
            {
              std::lock_guard<std::mutex> guard(self->m_mutex);
              self->m_mappingInFlight.erase(blockNumber);
              self->m_mappingFutureInFlight.erase(blockNumber);
              self->m_mappingRequestTokens.erase(blockNumber);
              self->m_mappingAttempts.erase(blockNumber);
              ++self->m_mappingDataResponses;
              if (result.stateChanged) ++self->m_mappingNewDataResponses;
              if (!result.accepted()) {
                ++self->m_rejected;
                if (result.fatal()) {
                  self->m_state = LiveStreamLifecycleState::Failed;
                  self->m_reason = result.reason;
                }
              }
              else {
                self->m_mappingReceived.insert(blockNumber);
                try {
                  // The resolver is the authority for signed Mapping
                  // continuity. Keep the adaptive scheduler's reservation
                  // guard synchronized before any payload callback can
                  // advance beyond the descriptor's initial horizon.
                  self->m_fetcher->updateMappingFrontier(
                    verifiedFrontiers.mappingCommittedThrough,
                    verifiedFrontiers.nextReserved);
                  self->m_nextMappingBlock = std::max(self->m_nextMappingBlock,
                                                       blockNumber + 1);
                  self->m_mappingBytes += validated.wireEncode().size();
                }
                catch (const std::exception& error) {
                  ++self->m_generation;
                  self->m_state = LiveStreamLifecycleState::Failed;
                  self->m_reason = std::string("mapping-frontier-handoff-failed:") +
                                   error.what();
                  self->m_mappingInFlight.clear();
                  self->m_mappingFutureInFlight.clear();
                  self->m_mappingRequestTokens.clear();
                  self->m_payloadInFlight.clear();
                  self->m_payloadProcessing.clear();
                  self->m_payloadExpressedAtMs.clear();
                  self->m_fetcher->stopLive();
                }
              }
            }
            self->schedule();
            self->emitStatus();
          },
          [weak, blockNumber, requestToken, generation] (const ndn::Data&,
                              const ndn::security::ValidationError&) {
            const auto self = weak.lock();
            if (!self || !self->isCurrentMappingRequest(
                           blockNumber, requestToken, generation)) return;
            {
              std::lock_guard<std::mutex> guard(self->m_mutex);
              self->m_mappingInFlight.erase(blockNumber);
              self->m_mappingFutureInFlight.erase(blockNumber);
              self->m_mappingRequestTokens.erase(blockNumber);
              self->m_mappingAttempts.erase(blockNumber);
              ++self->m_rejected;
            }
            self->fail("mapping-validation-failed");
          });
      }
    },
    [weak, blockNumber, requestToken, generation]
    (const ndn::Interest&, const ndn::lp::Nack& nack) {
      if (const auto self = weak.lock(); self && self->isCurrentMappingRequest(
            blockNumber, requestToken, generation)) {
        bool exhausted = false;
        {
          std::lock_guard<std::mutex> guard(self->m_mutex);
          self->m_mappingInFlight.erase(blockNumber);
          self->m_mappingFutureInFlight.erase(blockNumber);
          self->m_mappingRequestTokens.erase(blockNumber);
          ++self->m_nacks;
          self->m_fetcher->recordNack(blockNumber, "mapping-nack");
          exhausted = self->m_mappingAttempts[blockNumber] >= 3;
        }
        logStreamTimelineTrace(
          "consumer", "mapping-attempt-terminal",
          self->m_descriptor.definition.streamId,
          self->m_descriptor.definition.sessionEpoch, blockNumber,
          {{"outcome", "nack"},
           {"nack_reason", std::to_string(
              static_cast<uint64_t>(nack.getReason()))}});
        if (exhausted) {
          {
            std::lock_guard<std::mutex> guard(self->m_mutex);
            ++self->m_retryExhaustions;
            ++self->m_retrySuppressions;
            ++self->m_retrySuppressionReasons["mapping-nack-round-exhausted"];
            self->m_mappingAttempts.erase(blockNumber);
          }
          // Mapping is an indefinitely advancing control stream, unlike a
          // finite payload item with a usefulness deadline. Keep each round
          // bounded to three attempts and one in-flight slot, but start a new
          // round while the authenticated session remains active so a sparse
          // burst cannot permanently stop all later payload groups.
        }
        self->schedule();
        self->emitStatus();
      }
    },
    [weak, blockNumber, requestToken, generation] (const ndn::Interest&) {
      if (const auto self = weak.lock(); self && self->isCurrentMappingRequest(
            blockNumber, requestToken, generation)) {
        bool exhausted = false;
        uint64_t attemptsInRound = 0;
        {
          std::lock_guard<std::mutex> guard(self->m_mutex);
          self->m_mappingInFlight.erase(blockNumber);
          self->m_mappingFutureInFlight.erase(blockNumber);
          self->m_mappingRequestTokens.erase(blockNumber);
          ++self->m_timeouts;
          self->m_fetcher->recordTimeout(blockNumber, false, true);
          attemptsInRound = self->m_mappingAttempts[blockNumber];
          exhausted = attemptsInRound >= 3;
        }
        logStreamTimelineTrace(
          "consumer", "mapping-attempt-terminal",
          self->m_descriptor.definition.streamId,
          self->m_descriptor.definition.sessionEpoch, blockNumber,
          {{"outcome", "timeout"},
           {"attempts_in_round", std::to_string(attemptsInRound)}});
        if (exhausted) {
          {
            std::lock_guard<std::mutex> guard(self->m_mutex);
            ++self->m_retryExhaustions;
            ++self->m_retrySuppressions;
            ++self->m_retrySuppressionReasons["mapping-timeout-round-exhausted"];
            self->m_mappingAttempts.erase(blockNumber);
          }
          self->schedule();
        }
        else self->schedule();
        self->emitStatus();
      }
    });
}

void
LiveStreamConsumerHandle::fetchPayload(StreamCursor cursor, const ndn::Name& name,
                                       uint64_t interestLifetimeMs,
                                       bool aheadOfJoinCheckpoint,
                                       bool retryAttempt)
{
  const auto weak = weak_from_this();
  const auto binding = m_resolver.lookup(cursor);
  const bool sourceInterest = binding &&
    binding->groupItemIndex < binding->predictedSourceItems;
  const bool repairInterest = binding &&
    binding->groupItemIndex >= binding->predictedSourceItems;
  uint64_t generation = 0;
  uint64_t attemptNumber = 0;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    generation = m_generation;
    const auto attempt = m_payloadAttempts.find(cursor);
    attemptNumber = attempt == m_payloadAttempts.end() ? 1 : attempt->second;
    ++m_payloadInterests;
    if (retryAttempt) {
      ++m_retryPayloadInterests;
      if (sourceInterest) ++m_retryPayloadSourceInterests;
      else if (repairInterest) ++m_retryPayloadRepairInterests;
    }
    else {
      ++m_initialPayloadInterests;
      if (sourceInterest) ++m_initialPayloadSourceInterests;
      else if (repairInterest) ++m_initialPayloadRepairInterests;
    }
    if (sourceInterest) ++m_payloadSourceInterests;
    else if (repairInterest) ++m_payloadRepairInterests;
    else ++m_payloadUnclassifiedInterests;
    m_payloadExpressedAtMs[cursor] = streamNowMs();
    if (aheadOfJoinCheckpoint) {
      ++m_futurePayloadInterests;
      if (retryAttempt) ++m_retryFuturePayloadInterests;
      else ++m_initialFuturePayloadInterests;
    }
  }
  logStreamTimelineTrace(
    "consumer", "payload-attempt-expressed", m_descriptor.definition.streamId,
    m_descriptor.definition.sessionEpoch, cursor,
    {{"attempt", std::to_string(attemptNumber)},
     {"retry", retryAttempt ? "true" : "false"},
     {"future", aheadOfJoinCheckpoint ? "true" : "false"},
     {"name", name.toUri()}});
  ndn::Interest interest(name);
  interest.setCanBePrefix(false);
  interest.setMustBeFresh(false);
  // The adaptive decision already normalizes the cursor lookahead by the
  // observed items-per-sample rate. Re-expanding the immutable descriptor's
  // initial production frontier once per item turned a sub-second live
  // deadline into a 30-second wait after the consumer fell behind retention.
  interest.setInterestLifetime(ndn::time::milliseconds(
    std::max<uint64_t>(1, interestLifetimeMs)));
  m_face.expressInterest(
    interest,
    [weak, cursor, name, generation, aheadOfJoinCheckpoint, retryAttempt,
     attemptNumber] (const ndn::Interest&,
                                                             const ndn::Data& data) {
      if (const auto self = weak.lock()) {
        if (!self->isActive(generation)) return;
        const auto receivedMs = streamNowMs();
        {
          std::lock_guard<std::mutex> guard(self->m_mutex);
          // DRD is Interest-expression -> matching Data reception.  Free the
          // network slot before validation/decryption/APP processing and keep
          // separate cursor ownership so schedule() cannot request it again.
          if (self->m_payloadInFlight.erase(cursor) == 0 ||
              self->m_completed.count(cursor) != 0 ||
              !self->m_payloadProcessing.insert(cursor).second) {
            ++self->m_lateArrivals;
            ++self->m_payloadNonproductiveInterests;
            self->m_payloadExpressedAtMs.erase(cursor);
            logStreamTimelineTrace(
              "consumer", "payload-attempt-terminal",
              self->m_descriptor.definition.streamId,
              self->m_descriptor.definition.sessionEpoch, cursor,
              {{"attempt", std::to_string(attemptNumber)},
               {"outcome", "late"}});
            return;
          }
          self->m_recoveryEligibleTimeouts.erase(cursor);
          if (retryAttempt) ++self->m_retrySuccesses;
          const auto expressed = self->m_payloadExpressedAtMs.find(cursor);
          if (expressed != self->m_payloadExpressedAtMs.end()) {
            if (receivedMs > expressed->second) {
              self->m_fetcher->observePayloadDelay(
                static_cast<double>(receivedMs - expressed->second),
                aheadOfJoinCheckpoint);
            }
            self->m_payloadExpressedAtMs.erase(expressed);
          }
        }
        // Keep the paper's lambda_p Interest pipeline full independently of
        // signature validation and the application's item-processing time.
        self->schedule();
        auto onValidated =
          [weak, cursor, name, generation, attemptNumber] (const ndn::Data& validated) {
            const auto self = weak.lock();
            if (!self || !self->isActive(generation)) return;
            if (validated.wireEncode().size() >
                  self->m_descriptor.definition.signedWireCap ||
                !self->hasExpectedProviderSignature(validated) ||
                validated.getName() != name) {
              {
                std::lock_guard<std::mutex> guard(self->m_mutex);
                ++self->m_payloadNonproductiveInterests;
              }
              logStreamTimelineTrace(
                "consumer", "payload-attempt-terminal",
                self->m_descriptor.definition.streamId,
                self->m_descriptor.definition.sessionEpoch, cursor,
                {{"attempt", std::to_string(attemptNumber)},
                 {"outcome", "provider-or-name-mismatch"}});
              self->fail("payload-provider-or-name-mismatch");
              return;
            }
            {
              std::lock_guard<std::mutex> guard(self->m_mutex);
              // Only the Data that acquired processing ownership may cross the
              // application callback boundary for this cursor.
              if (self->m_payloadProcessing.count(cursor) == 0 ||
                  self->m_completed.count(cursor) != 0) {
                self->m_payloadProcessing.erase(cursor);
                return;
              }
              self->m_payloadAttempts.erase(cursor);
            }
            std::vector<uint8_t> opaque(validated.getContent().value(),
              validated.getContent().value() + validated.getContent().value_size());
            if (self->m_descriptor.definition.contractVersion ==
                STREAM_NAME_MAP_CONTRACT_VERSION_V2) {
              LiveStreamSampleEnvelope envelope;
              bool decoded = false;
              try {
                decoded = envelope.wireDecode(ndn::Block(
                  ndn::span<const uint8_t>(opaque.data(), opaque.size())));
              }
              catch (const std::exception&) {
                decoded = false;
              }
              const auto binding = self->m_resolver.lookup(cursor);
              const auto profile = std::find_if(
                self->m_descriptor.definition.sampleClasses.begin(),
                self->m_descriptor.definition.sampleClasses.end(),
                [&envelope] (const auto& value) {
                  return value.classId == envelope.sampleClass;
                });
              const bool kindMatches = binding &&
                ((envelope.itemKind == LiveStreamItemKind::Source &&
                  binding->groupItemIndex < binding->predictedSourceItems) ||
                 (envelope.itemKind == LiveStreamItemKind::Repair &&
                  binding->groupItemIndex >= binding->predictedSourceItems));
              if (!decoded || !binding || !binding->hasGroupBinding() ||
                  binding->groupId != envelope.groupId ||
                  binding->sampleClass != envelope.sampleClass ||
                  binding->groupItemIndex != envelope.groupItemIndex ||
                  profile == self->m_descriptor.definition.sampleClasses.end() ||
                  envelope.actualSourceItems > profile->hardMaxSourceItems ||
                  !kindMatches) {
                {
                  std::lock_guard<std::mutex> guard(self->m_mutex);
                  ++self->m_payloadNonproductiveInterests;
                }
                logStreamTimelineTrace(
                  "consumer", "payload-attempt-terminal",
                  self->m_descriptor.definition.streamId,
                  self->m_descriptor.definition.sessionEpoch, cursor,
                  {{"attempt", std::to_string(attemptNumber)},
                   {"outcome", "invalid-authenticated-sample-extent"}});
                self->fail("invalid-authenticated-sample-extent");
                return;
              }
              const auto groupStart = cursor - binding->groupItemIndex;
              bool firstObservation = false;
              {
                std::lock_guard<std::mutex> guard(self->m_mutex);
                firstObservation = self->m_observedSampleGroups.insert(
                  envelope.groupId).second;
                if (firstObservation) {
                  self->m_fetcher->observeSampleExtent(
                    binding->predictedSourceItems,
                    envelope.actualSourceItems);
                }
              }
              if (firstObservation &&
                  envelope.actualSourceItems < binding->predictedSourceItems) {
                std::lock_guard<std::mutex> guard(self->m_mutex);
                for (uint64_t index = envelope.actualSourceItems;
                     index < binding->predictedSourceItems; ++index) {
                  const auto terminal = groupStart + index;
                  self->m_resolver.markTerminalUnproduced(terminal);
                  self->m_payloadInFlight.erase(terminal);
                  self->m_payloadExpressedAtMs.erase(terminal);
                  self->m_completed.insert(terminal);
                }
              }
              opaque = std::move(envelope.opaqueContent);
            }
            LiveStreamFecRepair fec;
            bool isRepair = false;
            if (self->m_options.enableFecRecovery && self->m_descriptor.definition.fec.enabled()) {
              try {
                const ndn::Block wire(ndn::span<const uint8_t>(opaque.data(), opaque.size()));
                isRepair = fec.wireDecode(wire) &&
                  fec.validate(self->m_descriptor.definition).empty() &&
                  fec.repairName == name && fec.repairCursor == cursor;
              }
              catch (const std::exception&) {
                isRepair = false;
              }
            }
            if (isRepair) {
              {
                std::lock_guard<std::mutex> guard(self->m_mutex);
                self->m_payloadProcessing.erase(cursor);
                self->m_completed.insert(cursor);
                self->m_repairs[fec.groupId][fec.repairIndex] = fec;
                for (const auto sourceCursor : fec.sourceCursors) {
                  self->m_recoveryGroupBySource[sourceCursor] = fec.groupId;
                }
                ++self->m_payloadRepairDataResponses;
                self->advanceCompleted();
              }
              logStreamTimelineTrace(
                "consumer", "payload-attempt-data",
                self->m_descriptor.definition.streamId,
                self->m_descriptor.definition.sessionEpoch, cursor,
                {{"attempt", std::to_string(attemptNumber)},
                 {"packet_kind", "repair"}});
              self->tryRecover(fec.groupId);
              self->schedule();
              self->emitStatus();
              return;
            }
            VerifiedLiveStreamItem item{cursor, name,
              self->m_descriptor.definition.provider, opaque,
              LiveStreamItemProvenance::SignedData, streamNowMs()};
            LiveStreamItemAdmission admission;
            try {
              admission = self->m_options.onItem(item);
            }
            catch (...) {
              {
                std::lock_guard<std::mutex> guard(self->m_mutex);
                ++self->m_payloadNonproductiveInterests;
              }
              logStreamTimelineTrace(
                "consumer", "payload-attempt-terminal",
                self->m_descriptor.definition.streamId,
                self->m_descriptor.definition.sessionEpoch, cursor,
                {{"attempt", std::to_string(attemptNumber)},
                 {"outcome", "item-callback-failed"}});
              self->fail("item-callback-failed");
              self->emitStatus();
              return;
            }
            std::vector<std::string> recoveryGroups;
            std::string rejectionReason;
            {
              std::lock_guard<std::mutex> guard(self->m_mutex);
              self->m_payloadProcessing.erase(cursor);
              if (admission.accepted) {
                self->m_recoveryEligibleTimeouts.erase(cursor);
                if (const auto resolution = self->m_resolver.lookup(cursor);
                    resolution && resolution->hasGroupBinding()) {
                  const bool groupStillEligible = std::any_of(
                    self->m_recoveryEligibleTimeouts.begin(),
                    self->m_recoveryEligibleTimeouts.end(),
                    [&] (const auto eligibleCursor) {
                      const auto eligible = self->m_resolver.lookup(eligibleCursor);
                      return eligible && eligible->hasGroupBinding() &&
                             eligible->groupId == resolution->groupId;
                    });
                  if (!groupStillEligible) {
                    self->m_pendingRecoverableGroups.erase(resolution->groupId);
                  }
                }
                self->m_signedOpaque[cursor] = std::move(opaque);
                self->m_completed.insert(cursor);
                ++self->m_delivered;
                ++self->m_payloadSourceDataAdmissions;
                self->advanceCompleted();
                const auto group = self->m_recoveryGroupBySource.find(cursor);
                if (group != self->m_recoveryGroupBySource.end()) {
                  recoveryGroups.push_back(group->second);
                }
              }
              else {
                ++self->m_rejected;
                ++self->m_payloadNonproductiveInterests;
                self->m_fetcher->recordInvalidObservation();
                rejectionReason = admission.reason.empty() ?
                  "item-admission-rejected" : "item-admission-rejected:" + admission.reason;
              }
            }
            if (!rejectionReason.empty()) {
              logStreamTimelineTrace(
                "consumer", "payload-attempt-terminal",
                self->m_descriptor.definition.streamId,
                self->m_descriptor.definition.sessionEpoch, cursor,
                {{"attempt", std::to_string(attemptNumber)},
                 {"outcome", "application-rejected"}});
              self->fail(std::move(rejectionReason));
              return;
            }
            logStreamTimelineTrace(
              "consumer", "payload-attempt-data",
              self->m_descriptor.definition.streamId,
              self->m_descriptor.definition.sessionEpoch, cursor,
              {{"attempt", std::to_string(attemptNumber)},
               {"packet_kind", "source"},
               {"outcome", "application-useful"}});
            for (const auto& groupId : recoveryGroups) self->tryRecover(groupId);
            self->schedule();
            self->emitStatus();
          };
        auto onValidationFailure =
          [weak, cursor, generation, attemptNumber] (const ndn::Data&,
                         const ndn::security::ValidationError&) {
            const auto self = weak.lock();
            if (!self || !self->isActive(generation)) return;
            {
              std::lock_guard<std::mutex> guard(self->m_mutex);
              self->m_payloadProcessing.erase(cursor);
              self->m_payloadExpressedAtMs.erase(cursor);
              if (const auto attempt = self->m_payloadAttempts.find(cursor);
                  attempt != self->m_payloadAttempts.end() && attempt->second < 3) {
                ++self->m_retrySuppressions;
                ++self->m_retrySuppressionReasons["validation-failure"];
              }
              self->m_payloadAttempts.erase(cursor);
              ++self->m_rejected;
              ++self->m_payloadNonproductiveInterests;
            }
            logStreamTimelineTrace(
              "consumer", "payload-attempt-terminal",
              self->m_descriptor.definition.streamId,
              self->m_descriptor.definition.sessionEpoch, cursor,
              {{"attempt", std::to_string(attemptNumber)},
               {"outcome", "validation-failure"}});
            self->fail("payload-validation-failed");
          };
        self->m_validator->validate(
          data, onValidated, onValidationFailure);
      }
    },
    [weak, cursor, generation, attemptNumber, repairInterest]
    (const ndn::Interest&, const ndn::lp::Nack& nack) {
      if (const auto self = weak.lock(); self && self->isActive(generation)) {
        bool exhausted = false;
        bool protectionComplete = false;
        {
          std::lock_guard<std::mutex> guard(self->m_mutex);
          self->m_payloadInFlight.erase(cursor);
          self->m_payloadExpressedAtMs.erase(cursor);
          ++self->m_nacks;
          ++self->m_payloadNonproductiveInterests;
          const auto resolution = self->m_resolver.lookup(cursor);
          if (repairInterest && resolution && resolution->hasGroupBinding() &&
              resolution->groupItemIndex >= resolution->predictedSourceItems) {
            const auto groupStart = cursor - resolution->groupItemIndex;
            protectionComplete = true;
            for (uint64_t index = 0;
                 index < resolution->predictedSourceItems; ++index) {
              protectionComplete =
                protectionComplete &&
                self->m_completed.count(groupStart + index) != 0;
            }
          }
          if (protectionComplete) {
            self->m_payloadAttempts.erase(cursor);
            self->m_completed.insert(cursor);
            ++self->m_retrySuppressions;
            ++self->m_retrySuppressionReasons["repair-retry-unneeded"];
            self->advanceCompleted();
          }
          else {
            self->m_fetcher->recordNack(cursor, "payload-nack");
            exhausted = self->m_payloadAttempts[cursor] >= 3;
          }
        }
        logStreamTimelineTrace(
          "consumer", "payload-attempt-terminal",
          self->m_descriptor.definition.streamId,
          self->m_descriptor.definition.sessionEpoch, cursor,
          {{"attempt", std::to_string(attemptNumber)},
           {"outcome", protectionComplete ? "unneeded-repair-nack" : "nack"},
           {"nack_reason", std::to_string(
              static_cast<uint64_t>(nack.getReason()))}});
        if (protectionComplete) {
          self->schedule();
        }
        else if (exhausted) {
          {
            std::lock_guard<std::mutex> guard(self->m_mutex);
            ++self->m_retryExhaustions;
          }
          self->skipPayload(cursor, "payload-nack-retry-budget-exhausted");
        }
        else self->schedule();
        self->emitStatus();
      }
    },
    [weak, cursor, generation, aheadOfJoinCheckpoint,
     attemptNumber, repairInterest] (const ndn::Interest&) {
      if (const auto self = weak.lock(); self && self->isActive(generation)) {
        bool exhausted = false;
        bool protectionComplete = false;
        std::string recoveryGroup;
        {
          std::lock_guard<std::mutex> guard(self->m_mutex);
          self->m_payloadInFlight.erase(cursor);
          self->m_payloadExpressedAtMs.erase(cursor);
          ++self->m_timeouts;
          ++self->m_payloadNonproductiveInterests;
          const auto resolution = self->m_resolver.lookup(cursor);
          if (repairInterest && resolution && resolution->hasGroupBinding() &&
              resolution->groupItemIndex >= resolution->predictedSourceItems) {
            const auto groupStart = cursor - resolution->groupItemIndex;
            protectionComplete = true;
            for (uint64_t index = 0;
                 index < resolution->predictedSourceItems; ++index) {
              protectionComplete =
                protectionComplete &&
                self->m_completed.count(groupStart + index) != 0;
            }
          }
          if (protectionComplete) {
            self->m_payloadAttempts.erase(cursor);
            self->m_completed.insert(cursor);
            ++self->m_retrySuppressions;
            ++self->m_retrySuppressionReasons["repair-retry-unneeded"];
            self->advanceCompleted();
          }
          else {
            self->m_fetcher->recordTimeout(cursor, !aheadOfJoinCheckpoint,
                                           aheadOfJoinCheckpoint);
            exhausted = self->m_payloadAttempts[cursor] >= 3;
          }
          if (!protectionComplete && self->m_options.enableFecRecovery && resolution &&
              resolution->hasGroupBinding() &&
              resolution->groupItemIndex < resolution->predictedSourceItems) {
            recoveryGroup = resolution->groupId;
            if (self->m_recoveryEligibleTimeouts.insert(cursor).second) {
              ++self->m_recoveryEligibleSources;
            }
            if (self->m_pendingRecoverableGroups.insert(recoveryGroup).second) {
              ++self->m_recoverableGroups;
            }
          }
        }
        logStreamTimelineTrace(
          "consumer", "payload-attempt-terminal",
          self->m_descriptor.definition.streamId,
          self->m_descriptor.definition.sessionEpoch, cursor,
          {{"attempt", std::to_string(attemptNumber)},
           {"outcome", protectionComplete ?
              "unneeded-repair-timeout" : "timeout"}});
        if (protectionComplete) {
          self->schedule();
          self->emitStatus();
          return;
        }
        if (!recoveryGroup.empty()) self->tryRecover(recoveryGroup);
        bool recovered = false;
        {
          std::lock_guard<std::mutex> guard(self->m_mutex);
          recovered = self->m_completed.count(cursor) != 0;
        }
        if (recovered) {
          self->schedule();
        }
        else if (exhausted) {
          {
            std::lock_guard<std::mutex> guard(self->m_mutex);
            ++self->m_retryExhaustions;
          }
          self->skipPayload(cursor, "payload-timeout-retry-budget-exhausted");
        }
        else self->schedule();
        self->emitStatus();
      }
    });
}

void
LiveStreamConsumerHandle::skipPayload(StreamCursor cursor, std::string reason)
{
  std::string recoveryGroup;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_state != LiveStreamLifecycleState::Active) return;
    m_payloadInFlight.erase(cursor);
    m_payloadProcessing.erase(cursor);
    m_payloadExpressedAtMs.erase(cursor);
    m_payloadAttempts.erase(cursor);
    if (!m_completed.insert(cursor).second) return;
    const auto resolution = m_resolver.lookup(cursor);
    if (resolution && resolution->hasGroupBinding() &&
        resolution->groupItemIndex < resolution->predictedSourceItems) {
      m_recoverableSkips.insert(cursor);
      recoveryGroup = resolution->groupId;
      ++m_terminalMissingSources;
      if (m_recoveryEligibleTimeouts.insert(cursor).second) {
        ++m_recoveryEligibleSources;
      }
      if (m_pendingRecoverableGroups.insert(recoveryGroup).second) {
        ++m_recoverableGroups;
      }
    }
    ++m_rejected;
    if (reason.find("deadline-expired") != std::string::npos) {
      ++m_deadlineSkips;
    }
    m_reason = "skipped:" + std::move(reason);
    advanceCompleted();
  }
  if (!recoveryGroup.empty()) tryRecover(recoveryGroup);
  schedule();
}

void
LiveStreamConsumerHandle::advanceCompleted()
{
  while (m_completed.count(m_nextCursor) != 0) ++m_nextCursor;
  m_fetcher->advanceNextCursor(m_nextCursor);
  while (m_signedOpaque.size() > m_descriptor.definition.retainedItems) {
    m_signedOpaque.erase(m_signedOpaque.begin());
  }
}

void
LiveStreamConsumerHandle::retireRecoveryGroupLocked(const std::string& groupId)
{
  const auto found = m_repairs.find(groupId);
  if (found == m_repairs.end() || found->second.empty()) return;
  const auto sourceCursors = found->second.begin()->second.sourceCursors;
  const bool terminal = std::all_of(sourceCursors.begin(), sourceCursors.end(),
    [this] (const auto cursor) {
      return m_completed.count(cursor) != 0 &&
             m_recoverableSkips.count(cursor) == 0 &&
             m_recoveryEligibleTimeouts.count(cursor) == 0 &&
             m_fecRecoveryInFlight.count(cursor) == 0;
    });
  if (!terminal) return;
  for (const auto cursor : sourceCursors) {
    const auto indexed = m_recoveryGroupBySource.find(cursor);
    if (indexed != m_recoveryGroupBySource.end() && indexed->second == groupId) {
      m_recoveryGroupBySource.erase(indexed);
    }
  }
  m_recoveryExhaustedGroups.erase(groupId);
  m_pendingRecoverableGroups.erase(groupId);
  m_recoverySucceededGroups.erase(groupId);
  m_repairs.erase(found);
}

void
LiveStreamConsumerHandle::tryRecover(const std::string& groupId)
{
  std::vector<LiveStreamFecRepair> repairs;
  std::vector<std::optional<std::vector<uint8_t>>> sources;
  std::vector<size_t> missingIndices;
  std::set<StreamCursor> recoveringSkipped;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    const auto found = m_repairs.find(groupId);
    if (found == m_repairs.end() || found->second.empty()) return;
    for (const auto& [index, repair] : found->second) repairs.push_back(repair);
    const auto& declaration = repairs.front();
    for (size_t i = 0; i < declaration.sourceCursors.size(); ++i) {
      const auto cursor = declaration.sourceCursors[i];
      const auto value = m_signedOpaque.find(cursor);
      if (value == m_signedOpaque.end()) {
        missingIndices.push_back(i);
        sources.push_back(std::nullopt);
      }
      else sources.push_back(value->second);
    }
    if (missingIndices.empty()) {
      retireRecoveryGroupLocked(groupId);
      return;
    }
    bool allTerminal = true;
    for (const auto index : missingIndices) {
      const auto cursor = declaration.sourceCursors[index];
      // Absence from m_signedOpaque is not evidence of loss: under harmless
      // reordering the original exact-name Data can still own the cursor in
      // the network or validation path. Recovery is authorized only after an
      // attempt has timed out (or exhausted its finite retry budget), and
      // never while the exact-name Data remains in flight or under validation.
      if (m_payloadInFlight.count(cursor) != 0 ||
          m_payloadProcessing.count(cursor) != 0 ||
          m_fecRecoveryInFlight.count(cursor) != 0 ||
          (m_recoverableSkips.count(cursor) == 0 &&
           m_recoveryEligibleTimeouts.count(cursor) == 0) ||
          (m_recoverableSkips.count(cursor) != 0 &&
           m_completed.count(cursor) == 0)) {
        return;
      }
      const bool terminal = m_recoverableSkips.count(cursor) != 0;
      allTerminal = allTerminal && terminal;
      if (terminal) recoveringSkipped.insert(cursor);
    }
    if (missingIndices.size() > declaration.recoveryCapacity) {
      if (!allTerminal) return;
      if (m_recoveryExhaustedGroups.insert(groupId).second) {
        ++m_recoveryExhaustions;
      }
      // Every missing cursor has already reached a terminal retry outcome and
      // the authenticated FEC declaration proves that this group can never be
      // recovered. Retire eligibility and repair bytes instead of retaining an
      // impossible group for the rest of a long-running session.
      for (const auto index : missingIndices) {
        m_recoverableSkips.erase(declaration.sourceCursors[index]);
        m_recoveryEligibleTimeouts.erase(declaration.sourceCursors[index]);
      }
      retireRecoveryGroupLocked(groupId);
      return;
    }
    if (repairs.size() < missingIndices.size()) return;
    for (const auto index : missingIndices) {
      m_fecRecoveryInFlight.insert(declaration.sourceCursors[index]);
    }
    ++m_recoveryAttempts;
  }
  const auto recovered = recoverLiveStreamSources(
    m_descriptor.definition, repairs, sources, streamNowMs());
  if (!recovered) {
    std::lock_guard<std::mutex> guard(m_mutex);
    for (const auto index : missingIndices) {
      m_fecRecoveryInFlight.erase(repairs.front().sourceCursors[index]);
    }
    return;
  }
  std::vector<std::pair<VerifiedLiveStreamItem, LiveStreamItemAdmission>> admitted;
  try {
    for (const auto index : missingIndices) {
      VerifiedLiveStreamItem item{repairs.front().sourceCursors[index],
        repairs.front().sourceNames[index], m_descriptor.definition.provider,
        *recovered->at(index), LiveStreamItemProvenance::FecRecovered, streamNowMs()};
      admitted.emplace_back(item, m_options.onItem(item));
    }
  }
  catch (const std::exception& e) {
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      for (const auto index : missingIndices) {
        m_fecRecoveryInFlight.erase(repairs.front().sourceCursors[index]);
      }
    }
    fail(std::string("item-callback-failed:") + e.what());
    emitStatus();
    return;
  }
  std::lock_guard<std::mutex> guard(m_mutex);
  const bool allAccepted = std::all_of(admitted.begin(), admitted.end(),
    [] (const auto& value) { return value.second.accepted; });
  if (allAccepted) {
    if (m_recoverySucceededGroups.insert(groupId).second) {
      ++m_recoveredGroups;
    }
    for (size_t equation = 0; equation < missingIndices.size(); ++equation) {
      const auto repairCursor = repairs[equation].repairCursor;
      if (m_consumedRepairCursors.insert(repairCursor).second) {
        ++m_payloadRepairDataConsumed;
        logStreamTimelineTrace(
          "consumer", "recovery-repair-consumed",
          m_descriptor.definition.streamId,
          m_descriptor.definition.sessionEpoch, repairCursor,
          {{"group", groupId}});
      }
    }
  }
  for (const auto& [item, admission] : admitted) {
    m_fecRecoveryInFlight.erase(item.cursor);
    if (allAccepted) {
      const bool firstCompletion = m_completed.insert(item.cursor).second;
      const bool wasRecoveringSkipped = recoveringSkipped.erase(item.cursor) != 0;
      if (firstCompletion || wasRecoveringSkipped) {
        m_recoverableSkips.erase(item.cursor);
        m_recoveryEligibleTimeouts.erase(item.cursor);
        m_signedOpaque[item.cursor] = item.content;
        ++m_delivered;
        ++m_recovered;
        if (wasRecoveringSkipped && m_rejected > 0) --m_rejected;
      }
    }
    else if (!admission.accepted) {
      ++m_rejected;
      m_fetcher->recordInvalidObservation();
    }
  }
  advanceCompleted();
  retireRecoveryGroupLocked(groupId);
}

void
LiveStreamConsumerHandle::schedule()
{
  struct ScheduledMapping
  {
    uint64_t block = 0;
    uint64_t interestLifetimeMs = 0;
    uint64_t requestToken = 0;
  };
  std::vector<ScheduledMapping> maps;
  struct ScheduledPayload
  {
    StreamCursor cursor = 0;
    ndn::Name name;
    uint64_t interestLifetimeMs = 0;
    bool aheadOfJoinCheckpoint = false;
    bool retryAttempt = false;
  };
  std::vector<ScheduledPayload> payloads;
  struct DeferredRepair
  {
    StreamCursor cursor = 0;
    ndn::Name name;
    uint64_t interestLifetimeMs = 0;
    bool aheadOfJoinCheckpoint = false;
  };
  std::vector<DeferredRepair> deferredRepairs;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_state != LiveStreamLifecycleState::Active) return;
    const bool sampleAtomic = m_options.prefetchPolicy ==
                              LiveStreamPrefetchPolicy::AdaptiveSampleAtomic;
    if (sampleAtomic) {
      std::vector<uint64_t> predictedGroups;
      auto cursor = m_nextCursor;
      for (size_t count = 0; count < 64; ++count) {
        auto first = m_resolver.lookup(cursor);
        if (!first) break;
        if (first->tombstone || first->terminalUnproduced) {
          ++cursor;
          continue;
        }
        if (!first->hasGroupBinding() || first->groupItemIndex != 0) break;
        const auto groupItems = first->predictedGroupItems();
        bool complete = true;
        for (uint64_t index = 0; index < groupItems; ++index) {
          const auto item = m_resolver.lookup(cursor + index);
          if (!item || !item->hasGroupBinding() ||
              item->groupId != first->groupId ||
              item->sampleClass != first->sampleClass ||
              item->groupItemIndex != index ||
              item->predictedSourceItems != first->predictedSourceItems ||
              item->predictedRepairItems != first->predictedRepairItems) {
            complete = false;
            break;
          }
        }
        if (!complete) break;
        predictedGroups.push_back(groupItems);
        cursor += groupItems;
      }
      m_fetcher->setPredictedSampleGroups(std::move(predictedGroups));
    }
    const auto decision = m_fetcher->decide(streamNowMs());
    // Mapping lookahead controls how many exact block names stay pending; it
    // must not also lengthen loss detection for every block in that window.
    // missingTimeoutMs already covers one measured DRD plus one production
    // period, including a block that was not materialized when first asked.
    // Keeping the window depth and the per-Interest retry clock independent
    // preserves prefetch while preventing one lost Mapping Interest from
    // stalling an otherwise continuous stream for several sample periods.
    const auto liveAttemptLifetimeMs = std::max<uint64_t>(1,
      std::min(decision.interestLifetimeMs, decision.missingTimeoutMs));
    // Payload retry does not need Mapping's multi-block production lead. Its
    // usefulness boundary is one measured delivery delay plus one production
    // period, so waiting through the Mapping horizon can create a visible
    // multi-period stall before the first exact-name retry is even issued.
    // Do not subtract that production period again: missingTimeoutMs already
    // includes it. Doing so reduced a 20+/-10 ms bidirectional path to an
    // approximately 40 ms retry lifetime and classified ordinary RTT-tail
    // reordering as three consecutive source losses.
    const auto retryCheckpointMs = decision.recoveryCheckpointMs == 0
      ? decision.missingTimeoutMs : decision.recoveryCheckpointMs;
    const auto payloadAttemptLifetimeMs = std::max<uint64_t>(1,
      std::min(decision.interestLifetimeMs,
               std::max<uint64_t>(
                 20, std::min(retryCheckpointMs, decision.missingTimeoutMs))));
    const auto effectiveMappingAhead = computeLiveStreamMappingLead(
      static_cast<double>(liveAttemptLifetimeMs),
      std::max(1.0, m_descriptor.measuredSamplePeriodMs), 0.0,
      m_descriptor.definition.mappingAheadBlocks,
      m_descriptor.definition.mappingAheadBlocks);
    const auto inFlight = m_mappingInFlight.size() + m_payloadInFlight.size();
    const auto effectiveLimit = std::min<uint64_t>(
      m_options.aggregateInterestLimit,
      decision.aggregateInFlightLimit == 0 ? m_options.aggregateInterestLimit :
                                             decision.aggregateInFlightLimit);
    if (inFlight >= effectiveLimit) return;
    auto available = effectiveLimit - inFlight;
    auto mappingBudget = std::min<uint64_t>(available, decision.mappingBudget);
    auto payloadBudget = std::min<uint64_t>(available, decision.payloadBudget);
    // The final timeout callback can drain the authoritative Interest sets
    // while the controller still holds the previous full-batch snapshot. Do
    // not reset that snapshot: it carries congestion/phase memory. Instead,
    // recover this one lost wakeup with the existing aggregate and window
    // limits; the successfully scheduled batch is synchronized below.
    if (inFlight == 0 && mappingBudget == 0 && payloadBudget == 0) {
      mappingBudget = available;
      payloadBudget = available;
    }
    // Network and processing ownership are deliberately separate: receiving
    // Data frees a network slot immediately, while validation/application work
    // retains the cursor so it cannot be fetched twice.  Bound the processing
    // side independently so a stalled validator cannot create an unbounded
    // callback backlog.
    if (m_payloadProcessing.size() >= m_options.aggregateInterestLimit) {
      payloadBudget = 0;
    }
    else {
      payloadBudget = std::min<uint64_t>(
        payloadBudget, m_options.aggregateInterestLimit - m_payloadProcessing.size());
    }
    if (m_payloadInFlight.size() >= decision.window) payloadBudget = 0;
    else payloadBudget = std::min<uint64_t>(
      payloadBudget, decision.window - m_payloadInFlight.size());
    const auto begin = decision.payloadBeginCursor;
    const auto end = m_options.prefetchPolicy ==
      LiveStreamPrefetchPolicy::MappedLiveFutureOff ? begin : decision.payloadEndCursor;
    if (sampleAtomic) {
      auto cursor = begin;
      uint64_t admittedGroups = 0;
      uint64_t futureLeadGroups = 0;
      const auto frontiers = m_resolver.frontiers();
      const auto scheduleMappingWindow = [&] (uint64_t firstBlock) {
        const auto ahead = effectiveMappingAhead;
        // Keep the Mapping window anchored to authenticated payload progress,
        // including out-of-order completions beyond an earlier retrying gap.
        // Anchoring only to m_nextCursor makes one lost payload freeze Mapping
        // discovery even while all later signed Data has already completed.
        // This still cannot race ahead of payload work: the anchor moves only
        // after an authenticated cursor reaches terminal completion.
        const auto progressCursor = m_completed.empty()
          ? m_nextCursor : std::max(m_nextCursor, *m_completed.rbegin());
        const auto anchorBlock =
          progressCursor / m_descriptor.definition.mappingBlockCapacity;
        const auto maximumBlock = anchorBlock >
            std::numeric_limits<uint64_t>::max() - ahead
          ? std::numeric_limits<uint64_t>::max() : anchorBlock + ahead;
        for (size_t offset = 0;
             offset < ahead && mappingBudget > 0 && available > 0; ++offset) {
          if (firstBlock > std::numeric_limits<uint64_t>::max() - offset) break;
          const auto block = firstBlock + offset;
          if (block > maximumBlock) break;
          if (m_mappingReceived.count(block) != 0) continue;
          const bool inserted = m_mappingInFlight.insert(block).second;
          if (!inserted) {
            // One exact-name request owns a Mapping block until Data, Nack, or
            // timeout. Re-expressing a still-live speculative request merely
            // because it became the current gap creates duplicate control
            // traffic in zero loss and makes the original valid Data callback
            // inert through request-token fencing.
            continue;
          }
          else if (offset != 0) {
            m_mappingFutureInFlight.insert(block);
          }
          const auto priorAttempts = m_mappingAttempts[block]++;
          const auto requestToken = ++m_mappingRequestTokens[block];
          if (priorAttempts > 0) ++m_retryAttempts;
          const auto maximumLeadOffset =
            std::max<size_t>(1, m_descriptor.definition.mappingAheadBlocks) - 1;
          const auto boundedLeadOffset = std::min(offset, maximumLeadOffset);
          // A nonzero offset is speculative control prefetch. Its position in
          // the local scan is not a provider production timestamp, especially
          // when a variable-size group spans Mapping blocks. Give every such
          // block the complete declared Mapping-ahead horizon. Offset zero is
          // the current gap and remains RTT-driven for fast loss recovery.
          const auto leadPeriods = boundedLeadOffset == 0 ? size_t{0} :
            m_descriptor.definition.mappingAheadBlocks;
          const auto blockLeadMs = static_cast<uint64_t>(std::ceil(
            static_cast<double>(leadPeriods) *
            m_descriptor.measuredSamplePeriodMs));
          const auto mappingBaseLifetimeMs = boundedLeadOffset == 0
            ? liveAttemptLifetimeMs : payloadAttemptLifetimeMs;
          const auto mappingLifetimeMs = mappingBaseLifetimeMs >
              std::numeric_limits<uint64_t>::max() - blockLeadMs
            ? std::numeric_limits<uint64_t>::max()
            : mappingBaseLifetimeMs + blockLeadMs;
          // A retry remains an ordinary exact-name Interest with the same
          // measured loss-detection horizon. A hard-coded 20 ms retry can
          // expire before the previous PIT state drains on a realistic path;
          // immediate re-expression then receives another Nack and forms a
          // self-sustaining Mapping retry storm.
          maps.push_back({block,
            std::min(decision.interestLifetimeMs, mappingLifetimeMs),
            requestToken});
          --mappingBudget;
          --available;
        }
      };
      const auto scheduleNextMapping = [&] (StreamCursor next) {
        if (next <= frontiers.mappingCommittedThrough) return;
        scheduleMappingWindow(
          next / m_descriptor.definition.mappingBlockCapacity);
      };
      // The phase controller expresses Chasing/Adjusting/Fetching through the
      // packet window. Capping this loop at sampleDemand made that window
      // observational only and serialized whole groups whenever DRD rounded
      // down to one sample period. Let the packet budget be authoritative;
      // this group-count guard only bounds metadata scanning. Pressure still
      // contracts decision.window and therefore payloadBudget above.
      // This is only a CPU bound on authenticated Mapping inspection. Using
      // decision.window as a group-count limit was incorrect because that
      // value is already enforced below as a packet/in-flight budget. In
      // particular, a one-packet window would inspect only the first group and
      // repeatedly spend the freed slot on its optional repair while the next
      // group's source was never considered.
      constexpr uint64_t maxMappedGroupsPerPass = 64;
      // Crossing the currently verified frontier is exactly where live
      // prefetch must issue the next Mapping Interest. Restricting the loop to
      // mappingCommittedThrough without this probe deadlocks at the initial
      // descriptor horizon: no later Mapping can arrive, so no later payload
      // name can ever become schedulable.
      scheduleNextMapping(cursor);
      // Maintain a sliding authenticated Mapping window instead of fetching a
      // batch only after payload consumption reaches the current frontier.
      // The payload cursor anchors the maximum block, so rapid Mapping replies
      // cannot drain an application's entire announcement horizon.
      scheduleMappingWindow(m_nextMappingBlock);
      while (cursor <= frontiers.mappingCommittedThrough && available > 0 &&
             admittedGroups < maxMappedGroupsPerPass) {
        if (m_completed.count(cursor) != 0) {
          ++cursor;
          continue;
        }
        auto first = m_resolver.lookup(cursor);
        if (!first) {
          scheduleMappingWindow(
            cursor / m_descriptor.definition.mappingBlockCapacity);
          break;
        }
        if (first->tombstone || first->terminalUnproduced) {
          m_completed.insert(cursor);
          advanceCompleted();
          ++cursor;
          continue;
        }
        // An asynchronous Data callback may advance m_nextCursor into a group
        // while sibling Interests from that same atomic issue are still in
        // flight. Re-anchor at the signed group boundary instead of treating
        // normal partial completion as malformed Mapping.
        if (first->hasGroupBinding() && first->groupItemIndex > 0) {
          if (first->groupItemIndex > cursor) {
            ++m_generation;
            m_state = LiveStreamLifecycleState::Failed;
            m_reason = "adaptive-sample-group-index-underflow";
            m_atomicCapacityReason = m_reason;
            break;
          }
          cursor -= first->groupItemIndex;
          first = m_resolver.lookup(cursor);
        }
        if (!first || !first->hasGroupBinding() || first->groupItemIndex != 0) {
          ++m_generation;
          m_state = LiveStreamLifecycleState::Failed;
          m_reason = "adaptive-sample-does-not-start-at-group-boundary";
          m_atomicCapacityReason = m_reason;
          break;
        }
        const auto groupItems = first->predictedGroupItems();
        std::vector<StreamNameMapResolution> group;
        group.reserve(groupItems);
        bool mappingMissing = false;
        for (uint64_t index = 0; index < groupItems; ++index) {
          const auto item = m_resolver.lookup(cursor + index);
          if (!item) {
            mappingMissing = true;
            scheduleMappingWindow(
              (cursor + index) /
              m_descriptor.definition.mappingBlockCapacity);
            continue;
          }
          if (!item->hasGroupBinding() || item->groupId != first->groupId ||
              item->sampleClass != first->sampleClass ||
              item->groupItemIndex != index ||
              item->predictedSourceItems != first->predictedSourceItems ||
              item->predictedRepairItems != first->predictedRepairItems) {
            ++m_generation;
            m_state = LiveStreamLifecycleState::Failed;
            m_reason = "conflicting-adaptive-sample-group";
            m_atomicCapacityReason = m_reason;
            mappingMissing = true;
            break;
          }
          group.push_back(*item);
        }
        if (mappingMissing || group.size() != groupItems) break;

        uint64_t unscheduledSources = 0;
        bool started = false;
        bool allSourcesCompleted = true;
        for (uint64_t index = 0; index < groupItems; ++index) {
          const auto itemCursor = cursor + index;
          const bool completed = m_completed.count(itemCursor) != 0;
          const bool owned = completed ||
                             m_payloadInFlight.count(itemCursor) != 0 ||
                             m_payloadProcessing.count(itemCursor) != 0;
          started = started || owned;
          if (index < first->predictedSourceItems) {
            allSourcesCompleted = allSourcesCompleted && completed;
            if (!owned) ++unscheduledSources;
          }
        }
        // Repair symbols are protection for incomplete source data, not
        // application payload. Once every source in an authenticated group is
        // complete, an optional repair that was never expressed must not keep
        // the cursor horizon unresolved or consume a future Interest slot.
        if (allSourcesCompleted) {
          for (uint64_t index = first->predictedSourceItems;
               index < groupItems; ++index) {
            const auto repairCursor = cursor + index;
            if (m_payloadInFlight.count(repairCursor) == 0 &&
                m_payloadProcessing.count(repairCursor) == 0) {
              m_payloadAttempts.erase(repairCursor);
              m_completed.insert(repairCursor);
            }
          }
          advanceCompleted();
        }
        bool allCompleted = true;
        for (uint64_t index = 0; index < groupItems; ++index) {
          allCompleted = allCompleted &&
                         m_completed.count(cursor + index) != 0;
        }
        // A loss near m_nextCursor may leave many later groups completed
        // out-of-order. Those groups consume neither an Interest slot nor
        // application-processing capacity, so they must not consume the
        // bounded unresolved-group horizon. Counting them here made each
        // retry pause all scheduling after a fixed number of already-finished
        // groups and accumulated latency under otherwise recoverable loss.
        if (allCompleted) {
          cursor += groupItems;
          continue;
        }
        if (!started && (unscheduledSources > payloadBudget ||
                         unscheduledSources > available)) {
          ++m_atomicDeferrals;
          m_atomicCapacityReason =
            "whole-sample-source-extent-exceeds-current-payload-budget";
          break;
        }
        if (cursor + groupItems - 1 > end) ++m_atomicExpansions;
        // Count production lead from the signed latest-produced frontier, not
        // from the number of incomplete groups already visited in this pass.
        // When begin is already the first unpublished group, the old
        // unresolvedLeadGroups value was zero and gave that future Interest an
        // RTT-only lifetime. It therefore expired one full sample period before
        // normal production. The first future group is one period ahead, the
        // second is two periods ahead, and so on; already-produced incomplete
        // groups do not add production lead.
        const bool groupIsFuture = cursor > frontiers.latestProduced;
        const auto groupLeadPeriods = groupIsFuture ? ++futureLeadGroups : 0;
        const auto groupLeadMs = groupLeadPeriods == 0 ? uint64_t{0} :
          static_cast<uint64_t>(std::ceil(
            static_cast<double>(groupLeadPeriods) *
            m_descriptor.measuredSamplePeriodMs));
        const auto initialFutureLifetimeMs = std::min(
          decision.interestLifetimeMs,
          decision.missingTimeoutMs > std::numeric_limits<uint64_t>::max() - groupLeadMs
            ? std::numeric_limits<uint64_t>::max()
            : decision.missingTimeoutMs + groupLeadMs);
        for (uint64_t index = 0; index < groupItems; ++index) {
          const auto itemCursor = cursor + index;
          if (m_completed.count(itemCursor) != 0 ||
              m_payloadInFlight.count(itemCursor) != 0 ||
              m_payloadProcessing.count(itemCursor) != 0) continue;
          const bool future = itemCursor > frontiers.latestProduced;
          const auto lifetime = future
            ? initialFutureLifetimeMs : payloadAttemptLifetimeMs;
          if (index >= first->predictedSourceItems) {
            // Scan the complete authenticated packet horizon before admitting
            // optional repair symbols. This preserves source progress under
            // loss without changing the controller's window, retry clock, FEC
            // capacity, or workload-specific configuration. Repairs consume
            // only payload capacity left after all eligible source Interests.
            deferredRepairs.push_back(
              {itemCursor, group[index].originalName, lifetime, future});
            continue;
          }
          if (payloadBudget == 0 || available == 0) {
            ++m_atomicDeferrals;
            m_atomicCapacityReason = "started-sample-waits-for-recovery-slot";
            break;
          }
          m_payloadInFlight.insert(itemCursor);
          const bool retryAttempt = m_payloadAttempts[itemCursor]++ > 0;
          if (retryAttempt) ++m_retryAttempts;
          payloads.push_back({itemCursor, group[index].originalName,
                              future && !retryAttempt ? initialFutureLifetimeMs :
                                                        payloadAttemptLifetimeMs,
                              future,
                              retryAttempt});
          --payloadBudget;
          --available;
        }
        ++admittedGroups;
        cursor += groupItems;
      }
      for (const auto& repair : deferredRepairs) {
        if (payloadBudget == 0 || available == 0) break;
        if (m_completed.count(repair.cursor) != 0 ||
            m_payloadInFlight.count(repair.cursor) != 0 ||
            m_payloadProcessing.count(repair.cursor) != 0) {
          continue;
        }
        m_payloadInFlight.insert(repair.cursor);
        const bool retryAttempt = m_payloadAttempts[repair.cursor]++ > 0;
        if (retryAttempt) ++m_retryAttempts;
        payloads.push_back(
          {repair.cursor, repair.name,
           repair.aheadOfJoinCheckpoint && !retryAttempt
             ? repair.interestLifetimeMs : payloadAttemptLifetimeMs,
           repair.aheadOfJoinCheckpoint, retryAttempt});
        --payloadBudget;
        --available;
      }
      // The loop can itself cross the frontier while consuming trailing
      // tombstones. No Data callback remains to wake the scheduler in that
      // case, so arm the next exact Mapping Interest before returning.
      scheduleNextMapping(cursor);
    }
    else for (StreamCursor cursor = begin;
         cursor <= end && available > 0 &&
           (mappingBudget > 0 || payloadBudget > 0); ++cursor) {
      if (m_completed.count(cursor) != 0 || m_payloadInFlight.count(cursor) != 0 ||
          m_payloadProcessing.count(cursor) != 0) continue;
      const auto resolution = m_resolver.lookup(cursor);
      if (resolution && resolution->tombstone) {
        m_completed.insert(cursor);
        advanceCompleted();
      }
      else if (resolution && resolution->schedulable() && payloadBudget > 0) {
        m_payloadInFlight.insert(cursor);
        const bool retryAttempt = m_payloadAttempts[cursor]++ > 0;
        if (retryAttempt) ++m_retryAttempts;
        const auto frontiers = m_resolver.frontiers();
        payloads.push_back({cursor, resolution->originalName,
                            payloadAttemptLifetimeMs,
                            cursor > frontiers.latestProduced,
                            retryAttempt});
        --payloadBudget;
        --available;
      }
      else if (!resolution && mappingBudget > 0) {
        const auto block = cursor / m_descriptor.definition.mappingBlockCapacity;
        if (m_mappingInFlight.insert(block).second) {
          const auto priorAttempts = m_mappingAttempts[block]++;
          const auto requestToken = ++m_mappingRequestTokens[block];
          if (priorAttempts > 0) ++m_retryAttempts;
          maps.push_back({block, liveAttemptLifetimeMs, requestToken});
          --mappingBudget;
          --available;
        }
      }
    }
    m_fetcher->setInFlight(m_mappingInFlight.size(), m_payloadInFlight.size(), 0);
  }
  for (const auto& mapping : maps) {
    fetchMapping(mapping.block, mapping.interestLifetimeMs,
                 mapping.requestToken);
  }
  for (const auto& payload : payloads) {
    fetchPayload(payload.cursor, payload.name, payload.interestLifetimeMs,
                 payload.aheadOfJoinCheckpoint, payload.retryAttempt);
  }
}

bool
LiveStreamConsumerHandle::observeAcceptedSample(const LiveStreamSampleObservation& observation)
{
  std::lock_guard<std::mutex> guard(m_mutex);
  if (m_state != LiveStreamLifecycleState::Active || observation.itemCount == 0) return false;
  // The handle measures network DRD from its own exact Interest/Data pair.
  // This caller-provided value is application latency (often capture->receive)
  // and must not inflate the Interest pipeline's RTT estimate.
  return m_fetcher->observeAcceptedSample(m_descriptor.definition.sessionEpoch,
    observation.sampleId, observation.arrivalMs, 0.0,
    observation.itemCount, true);
}

LiveStreamStatus
LiveStreamConsumerHandle::status() const
{
  std::lock_guard<std::mutex> guard(m_mutex);
  LiveStreamStatus result;
  result.state = m_state;
  result.frontiers = m_resolver.frontiers();
  result.retainedItems = m_signedOpaque.size();
  result.inFlight = m_mappingInFlight.size() + m_payloadInFlight.size();
  result.mappingBlocks = m_resolver.verifiedBlockCount();
  result.delivered = m_delivered;
  result.rejected = m_rejected;
  result.recovered = m_recovered;
  result.timeouts = m_timeouts;
  result.nacks = m_nacks;
  result.retryAttempts = m_retryAttempts;
  result.lateArrivals = m_lateArrivals;
  result.deadlineSkips = m_deadlineSkips;
  result.retryExhaustions = m_retryExhaustions;
  result.mappingInterests = m_mappingInterests;
  result.mappingDataResponses = m_mappingDataResponses;
  result.mappingNewDataResponses = m_mappingNewDataResponses;
  result.payloadInterests = m_payloadInterests;
  result.futurePayloadInterests = m_futurePayloadInterests;
  result.initialPayloadInterests = m_initialPayloadInterests;
  result.retryPayloadInterests = m_retryPayloadInterests;
  result.payloadSourceInterests = m_payloadSourceInterests;
  result.initialPayloadSourceInterests = m_initialPayloadSourceInterests;
  result.retryPayloadSourceInterests = m_retryPayloadSourceInterests;
  result.payloadRepairInterests = m_payloadRepairInterests;
  result.initialPayloadRepairInterests = m_initialPayloadRepairInterests;
  result.retryPayloadRepairInterests = m_retryPayloadRepairInterests;
  result.payloadUnclassifiedInterests = m_payloadUnclassifiedInterests;
  result.payloadSourceDataAdmissions = m_payloadSourceDataAdmissions;
  result.payloadRepairDataResponses = m_payloadRepairDataResponses;
  result.payloadRepairDataConsumed = m_payloadRepairDataConsumed;
  result.payloadApplicationUsefulInterests =
    m_payloadSourceDataAdmissions + m_payloadRepairDataConsumed;
  result.payloadProtectionOnlyInterests =
    m_payloadRepairDataResponses >= m_payloadRepairDataConsumed
      ? m_payloadRepairDataResponses - m_payloadRepairDataConsumed : 0;
  result.payloadNonproductiveInterests = m_payloadNonproductiveInterests;
  const auto classified = result.payloadApplicationUsefulInterests +
                          result.payloadProtectionOnlyInterests +
                          result.payloadNonproductiveInterests;
  result.payloadUnresolvedInterests =
    m_payloadInterests >= classified ? m_payloadInterests - classified : 0;
  result.initialFuturePayloadInterests = m_initialFuturePayloadInterests;
  result.retryFuturePayloadInterests = m_retryFuturePayloadInterests;
  result.retrySuccesses = m_retrySuccesses;
  result.retrySuppressions = m_retrySuppressions;
  result.retrySuppressionReasons = m_retrySuppressionReasons;
  result.declaredRecoveryCapacity = m_descriptor.definition.fec.recoveryCapacity();
  result.recoveryEligibleSources = m_recoveryEligibleSources;
  result.terminalMissingSources = m_terminalMissingSources;
  result.recoverableGroups = m_recoverableGroups;
  result.recoveredGroups = m_recoveredGroups;
  result.recoveryAttempts = m_recoveryAttempts;
  result.recoveryExhaustions = m_recoveryExhaustions;
  result.mappingBytes = m_mappingBytes;
  result.reason = m_reason;
  result.fetchDecision = std::make_shared<StreamFetchDecision>(m_fetcher->decide(streamNowMs()));
  result.fetchDecision->atomicExpansions = m_atomicExpansions;
  result.fetchDecision->atomicDeferrals = m_atomicDeferrals;
  result.fetchDecision->capacityReason = m_atomicCapacityReason;
  if (m_options.prefetchPolicy == LiveStreamPrefetchPolicy::MappedLiveFutureOff) {
    result.fetchDecision->policyMode = toString(m_options.prefetchPolicy);
    result.fetchDecision->lookahead = 0;
  }
  return result;
}

void
LiveStreamConsumerHandle::fail(std::string reason)
{
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_state == LiveStreamLifecycleState::Stopped ||
        m_state == LiveStreamLifecycleState::Failed) return;
    ++m_generation;
    m_state = LiveStreamLifecycleState::Failed;
    m_reason = std::move(reason);
    m_mappingInFlight.clear();
    m_mappingFutureInFlight.clear();
    m_mappingRequestTokens.clear();
    m_payloadInFlight.clear();
    m_payloadProcessing.clear();
    m_payloadExpressedAtMs.clear();
    m_fetcher->stopLive();
  }
  emitStatus();
}

void
LiveStreamConsumerHandle::stop()
{
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_state == LiveStreamLifecycleState::Stopped) return;
    ++m_generation;
    m_state = LiveStreamLifecycleState::Stopped;
    m_reason = "stopped";
    const auto retryable = static_cast<uint64_t>(std::count_if(
      m_payloadAttempts.begin(), m_payloadAttempts.end(),
      [] (const auto& value) { return value.second > 0 && value.second < 3; }));
    if (retryable > 0) {
      m_retrySuppressions += retryable;
      m_retrySuppressionReasons["stop-fencing"] += retryable;
    }
    m_mappingInFlight.clear();
    m_mappingFutureInFlight.clear();
    m_mappingRequestTokens.clear();
    m_payloadInFlight.clear();
    m_payloadProcessing.clear();
    m_payloadExpressedAtMs.clear();
    m_fetcher->stopLive();
  }
  emitStatus();
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
