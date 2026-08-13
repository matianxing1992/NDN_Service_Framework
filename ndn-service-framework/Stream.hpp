#ifndef NDN_SERVICE_FRAMEWORK_STREAM_HPP
#define NDN_SERVICE_FRAMEWORK_STREAM_HPP

#include "common.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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
  StreamNameMapBlockType = 0xF633,
  StreamContractVersionType = 0xF634,
  StreamMappingVersionType = 0xF635,
  StreamMapBlockNumberType = 0xF636,
  StreamMapBlockCapacityType = 0xF637,
  StreamMapFirstCursorType = 0xF638,
  StreamPreviousContentDigestType = 0xF639,
  StreamNameMapEntryType = 0xF63A,
  StreamNameMapTombstoneType = 0xF63B,
  LiveStreamFecRepairType = 0xF63C,
  LiveStreamFecGroupIdType = 0xF63D,
  LiveStreamFecCreatedMsType = 0xF63E,
  LiveStreamFecExpiresMsType = 0xF63F,
  LiveStreamFecSourceType = 0xF640,
  LiveStreamFecSourceNameType = 0xF641,
  LiveStreamFecSourceCursorType = 0xF642,
  LiveStreamFecSourceLengthType = 0xF643,
  LiveStreamFecSourceDigestType = 0xF644,
  LiveStreamFecRepairNameType = 0xF645,
  LiveStreamFecRepairCursorType = 0xF646,
  LiveStreamFecCodedBytesType = 0xF647,
  StreamGroupIdType = 0xF648,
  StreamSampleClassType = 0xF649,
  StreamGroupItemIndexType = 0xF64A,
  StreamPredictedSourceItemsType = 0xF64B,
  StreamPredictedRepairItemsType = 0xF64C,
  LiveStreamSampleEnvelopeType = 0xF64D,
  StreamActualSourceItemsType = 0xF64E,
  StreamItemKindType = 0xF64F,
  LiveStreamFecSchemeType = 0xF650,
  LiveStreamFecRecoveryCapacityType = 0xF651,
  LiveStreamFecRepairIndexType = 0xF652,
  PredictiveStreamGroupCommitType = 0xF653,
  PredictiveStreamFrontierType = 0xF654,
  PredictiveStreamSourceType = 0xF655,
  PredictiveStreamSourceWireLengthType = 0xF656,
  PredictiveStreamSourceWireDigestType = 0xF657,
  PredictiveStreamRepairNameType = 0xF658,
  PredictiveStreamLatestCommittedGroupType = 0xF659,
  PredictiveStreamGroupCommitNameType = 0xF65A,
  PredictiveStreamInitialSampleIdType = 0xF65B,
  PredictiveStreamOldestRetainedSampleIdType = 0xF65C,
  PredictiveStreamLatestProducedSampleIdType = 0xF65D,
  PredictiveStreamNextExpectedSampleIdType = 0xF65E,
  PredictiveStreamGroupFirstCursorType = 0xF65F,
  PredictiveStreamGroupLastCursorType = 0xF660,
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

using StreamCursor = uint64_t;
using StreamContentDigest = std::array<uint8_t, 32>;

StreamContentDigest
computeStreamContentDigest(ndn::span<const uint8_t> bytes);

inline constexpr uint64_t STREAM_NAME_MAP_CONTRACT_VERSION_V1 = 1;
inline constexpr uint64_t STREAM_NAME_MAP_CONTRACT_VERSION_V2 = 2;
inline constexpr uint64_t STREAM_NAME_MAP_CONTRACT_VERSION =
  STREAM_NAME_MAP_CONTRACT_VERSION_V1;
inline constexpr uint64_t STREAM_NAME_MAP_MAX_BLOCK_CAPACITY = 4096;
inline constexpr size_t STREAM_NAME_MAP_MAX_RESOLVER_BLOCKS = 4096;
inline constexpr size_t STREAM_NAME_MAP_MAX_REVERSE_ENTRIES = 1048576;

/**
 * One immutable slot in a StreamNameMapBlock.
 *
 * A named slot contains a real nested NDN Name TLV. A tombstone is fixed before
 * the block is published and never carries an application payload or nested
 * Data packet.
 */
struct StreamNameMapEntry
{
  ndn::Name originalName;
  bool tombstone = false;
  std::string groupId;
  std::string sampleClass;
  uint64_t groupItemIndex = 0;
  uint64_t predictedSourceItems = 0;
  uint64_t predictedRepairItems = 0;

  static StreamNameMapEntry fromName(const ndn::Name& name);
  static StreamNameMapEntry fromGroupedName(const ndn::Name& name,
                                            std::string groupId,
                                            std::string sampleClass,
                                            uint64_t groupItemIndex,
                                            uint64_t predictedSourceItems,
                                            uint64_t predictedRepairItems);
  static StreamNameMapEntry makeTombstone();
  bool isTombstone() const;
  bool hasGroupBinding() const;
  uint64_t predictedGroupItems() const;
};

/**
 * Canonical fixed-capacity cursor-to-semantic-name Mapping content.
 *
 * Core only owns this codec. It does not sign, publish, fetch, or validate the
 * trust chain of Mapping Data. canonicalContent() is the complete standard
 * Content TLV whose bytes are chained by contentDigest().
 */
struct StreamNameMapBlock
{
  uint64_t contractVersion = STREAM_NAME_MAP_CONTRACT_VERSION;
  std::string streamId;
  uint64_t sessionEpoch = 0;
  uint64_t mappingVersion = 0;
  uint64_t blockNumber = 0;
  uint64_t blockCapacity = 0;
  StreamCursor firstCursor = 0;
  std::optional<StreamContentDigest> previousContentDigest;
  std::vector<StreamNameMapEntry> entries;

  std::optional<std::string> validate() const;
  ndn::Block wireEncode() const;
  bool wireDecode(const ndn::Block& block);
  ndn::Block canonicalContent() const;
  StreamContentDigest contentDigest() const;
  bool fitsSignedWireBudget(size_t signedEnvelopeOverhead,
                            size_t configuredWireCap) const;
  StreamCursor lastCursor() const;
};

/** Base name before the typed mapping Version and SequenceNum components. */
ndn::Name makeStreamNameMapRoot(const ndn::Name& provider,
                                const std::string& streamId);

/** Exact Mapping Data name: root / Version(mappingVersion) / SequenceNum(block). */
ndn::Name makeStreamNameMapBlockName(const ndn::Name& mappingRoot,
                                     uint64_t mappingVersion,
                                     uint64_t blockNumber);

struct StreamCursorFrontiers
{
  StreamCursor oldestRetained = 0;
  StreamCursor latestJoin = 0;
  StreamCursor latestProduced = 0;
  StreamCursor mappingCommittedThrough = 0;
  StreamCursor nextReserved = 0;

  std::optional<std::string> validate(uint64_t blockCapacity,
                                      uint64_t checkpointBlock) const;
};

struct StreamNameMapCheckpoint
{
  StreamCursorFrontiers frontiers;
  uint64_t blockNumber = 0;
  StreamContentDigest contentDigest{};
};

struct StreamNameMapResolverConfig
{
  uint64_t contractVersion = STREAM_NAME_MAP_CONTRACT_VERSION;
  std::string streamId;
  uint64_t sessionEpoch = 0;
  uint64_t mappingVersion = 0;
  uint64_t blockCapacity = 0;
  ndn::Name expectedProvider;
  ndn::Name mappingRoot;
  ndn::Name payloadPrefix;
  size_t signedWireCap = ndn::MAX_NDN_PACKET_SIZE;
  size_t maxVerifiedBlocks = 32;
  size_t maxQuarantineBlocks = 8;
  size_t maxReverseEntries = 65536;
  size_t maxOriginalNameWireBytes = 4096;
};

/**
 * Mapping Data envelope after application trust verification.
 *
 * The application verifies the signature and signer chain first, then supplies
 * the concrete signer identity here. Core intentionally owns no Validator,
 * KeyChain, Face, timer, or network I/O.
 */
struct VerifiedStreamNameMapData
{
  ndn::Name dataName;
  ndn::Name verifiedProvider;
  uint32_t contentType = ndn::tlv::ContentType_Blob;
  bool hasFinalBlock = false;
  size_t signedWireSize = 0;
  ndn::Block content;
  uint64_t receivedMonotonicMs = 0;
  uint64_t requiredBeforeMonotonicMs = 0;
};

enum class StreamNameMapAdmissionDisposition
{
  Admitted,
  Duplicate,
  Quarantined,
  Rejected,
  FatalSession,
};

const char* toString(StreamNameMapAdmissionDisposition disposition);

enum class StreamNameMapTiming
{
  Unclassified,
  Ahead,
  Late,
};

const char* toString(StreamNameMapTiming timing);

struct StreamNameMapAdmissionResult
{
  StreamNameMapAdmissionDisposition disposition =
    StreamNameMapAdmissionDisposition::Rejected;
  StreamNameMapTiming timing = StreamNameMapTiming::Unclassified;
  std::string reason;
  bool stateChanged = false;
  StreamCursor mappingCommittedThrough = 0;

  bool accepted() const;
  bool fatal() const;
};

struct StreamNameMapResolution
{
  StreamCursor cursor = 0;
  ndn::Name originalName;
  bool tombstone = false;
  bool terminalUnproduced = false;
  StreamNameMapTiming timing = StreamNameMapTiming::Unclassified;
  std::string groupId;
  std::string sampleClass;
  uint64_t groupItemIndex = 0;
  uint64_t predictedSourceItems = 0;
  uint64_t predictedRepairItems = 0;

  bool schedulable() const;
  bool hasGroupBinding() const;
  uint64_t predictedGroupItems() const;
};

/** Bounded, atomic Mapping continuity and semantic-name resolver. */
class StreamNameResolverState
{
public:
  void reset(const StreamNameMapResolverConfig& config,
             const StreamNameMapCheckpoint& checkpoint);

  StreamNameMapAdmissionResult
  admitVerifiedBlock(const VerifiedStreamNameMapData& input);

  /** Apply one application-verified descriptor/checkpoint snapshot atomically. */
  StreamNameMapAdmissionResult
  refreshCheckpoint(const StreamNameMapCheckpoint& checkpoint);

  std::optional<StreamNameMapResolution> lookup(StreamCursor cursor) const;
  std::optional<ndn::Name> resolve(StreamCursor cursor) const;
  std::optional<StreamCursor> reverseResolve(const ndn::Name& originalName) const;
  bool markTerminalUnproduced(StreamCursor cursor);

  /** Local cache eviction never advances the Provider retention frontier. */
  bool evictLocalBlock(uint64_t blockNumber);

  StreamCursorFrontiers frontiers() const;
  StreamNameMapCheckpoint checkpoint() const;
  bool faulted() const;
  size_t verifiedBlockCount() const;
  size_t quarantinedBlockCount() const;
  size_t bindingCount() const;
  std::map<std::string, uint64_t> diagnostics() const;

private:
  struct StoredBlock
  {
    StreamNameMapBlock block;
    StreamContentDigest digest{};
    ndn::Name dataName;
    StreamNameMapTiming timing = StreamNameMapTiming::Unclassified;
  };

  struct RebuiltState
  {
    std::set<uint64_t> connectedBlocks;
    std::map<StreamCursor, StreamNameMapResolution> bindings;
    std::map<ndn::Name, StreamCursor> reverseBindings;
  };

  std::optional<std::string>
  validateConfiguration(const StreamNameMapResolverConfig& config,
                        const StreamNameMapCheckpoint& checkpoint) const;
  std::optional<std::string>
  rebuild(const std::map<uint64_t, StoredBlock>& blocks,
          const StreamNameMapCheckpoint& checkpoint,
          RebuiltState& state) const;
  StreamNameMapAdmissionResult
  reject(std::string reason, bool fatal,
         StreamNameMapTiming timing = StreamNameMapTiming::Unclassified);
  void install(std::map<uint64_t, StoredBlock> blocks, RebuiltState state);

private:
  bool m_initialized = false;
  bool m_faulted = false;
  StreamNameMapResolverConfig m_config;
  StreamNameMapCheckpoint m_checkpoint;
  std::map<uint64_t, StoredBlock> m_blocks;
  // Compact block digests survive local eviction and retire only after an
  // authenticated retention advance. Name reservations survive for the full
  // Mapping-version lifetime so old cached payload names cannot be reused.
  std::map<uint64_t, StreamContentDigest> m_admittedBlockDigests;
  std::map<ndn::Name, StreamCursor> m_nameReservations;
  std::set<uint64_t> m_connectedBlocks;
  std::map<StreamCursor, StreamNameMapResolution> m_bindings;
  std::map<ndn::Name, StreamCursor> m_reverseBindings;
  std::set<StreamCursor> m_terminalUnproduced;
  std::map<std::string, uint64_t> m_diagnostics;
  mutable std::mutex m_mutex;
};

enum class LiveStreamFecScheme
{
  None,
  XorOneRepair,
  Gf256TwoRepair,
};

const char* toString(LiveStreamFecScheme scheme);

/** Optional bounded recovery over application-supplied opaque bytes. */
struct LiveStreamFecOptions
{
  LiveStreamFecScheme scheme = LiveStreamFecScheme::None;
  size_t maxSourceItems = 0;
  size_t maxSourceBytes = 0;
  uint64_t recoveryBudgetMs = 0;
  size_t repairSymbols = 0;

  static LiveStreamFecOptions none();
  static LiveStreamFecOptions xorOneRepair(size_t maxSourceItems,
                                           size_t maxSourceBytes,
                                           uint64_t recoveryBudgetMs = 500);
  static LiveStreamFecOptions gf256TwoRepair(size_t maxSourceItems,
                                             size_t maxSourceBytes,
                                             uint64_t recoveryBudgetMs = 500);
  bool enabled() const;
  size_t repairItemCount() const;
  size_t recoveryCapacity() const;
  std::optional<std::string> validate() const;
};

struct SampleClassProfile
{
  std::string classId;
  size_t seedSourceItems = 1;
  size_t hardMaxSourceItems = 64;
  size_t historyCapacity = 32;
  size_t safetyMarginItems = 1;

  static SampleClassProfile bounded(std::string classId,
                                    size_t seedSourceItems,
                                    size_t hardMaxSourceItems,
                                    size_t historyCapacity = 32,
                                    size_t safetyMarginItems = 1);
  std::optional<std::string> validate() const;
};

struct SampleClassPredictionStatus
{
  std::string classId;
  size_t prediction = 0;
  size_t observations = 0;
  uint64_t underpredictions = 0;
  uint64_t underpredictedItems = 0;
  uint64_t overpredictions = 0;
  uint64_t overpredictedItems = 0;
};

/** Bounded, session-local conservative item-count predictor. */
class LiveStreamSamplePredictor
{
public:
  explicit LiveStreamSamplePredictor(std::vector<SampleClassProfile> profiles = {});
  void reset(std::vector<SampleClassProfile> profiles);
  size_t predict(const std::string& classId) const;
  bool observe(const std::string& classId, size_t actualSourceItems);
  std::optional<SampleClassPredictionStatus> status(const std::string& classId) const;
  std::map<std::string, SampleClassPredictionStatus> statuses() const;

private:
  struct ClassState
  {
    SampleClassProfile profile;
    std::deque<size_t> history;
    SampleClassPredictionStatus status;
  };
  static size_t predict(const ClassState& state);

private:
  std::map<std::string, ClassState> m_classes;
  mutable std::mutex m_mutex;
};

struct LiveStreamDefinition
{
  uint64_t contractVersion = STREAM_NAME_MAP_CONTRACT_VERSION_V1;
  std::string streamId;
  ndn::Name provider;
  ndn::Name semanticDataPrefix;
  uint64_t sessionEpoch = 0;
  uint64_t mappingVersion = 0;
  size_t mappingBlockCapacity = 16;
  size_t mappingAheadBlocks = 4;
  size_t retainedItems = 600;
  size_t maxNameReservations = 65536;
  size_t maxPendingInterests = 256;
  size_t signedWireCap = ndn::MAX_NDN_PACKET_SIZE;
  double samplePeriodMs = 0.0;
  std::vector<SampleClassProfile> sampleClasses;
  LiveStreamFecOptions fec = LiveStreamFecOptions::none();

  std::optional<std::string> validate() const;
  ndn::Name mappingRoot() const;
};

/**
 * Convert measured path/production timing into a bounded names-only Mapping lead.
 * The returned item count covers one RTT, the observed jitter margin, and one
 * additional production interval. Media bytes are never part of this horizon.
 */
size_t
computeLiveStreamMappingLead(double rttMs, double productionPeriodMs,
                             double jitterMs, size_t minimumItems,
                             size_t maximumItems);

/** Immutable binding returned only after its Mapping slot is committed. */
struct LiveStreamItemReservation
{
  StreamCursor cursor = 0;
  ndn::Name originalName;
  uint64_t sessionEpoch = 0;
  uint64_t mappingVersion = 0;

  bool belongsTo(const LiveStreamDefinition& definition) const;
};

struct LiveStreamGroupReservation
{
  std::string groupId;
  std::vector<LiveStreamItemReservation> sources;
  std::vector<LiveStreamItemReservation> repairs;

  std::optional<std::string> validate(const LiveStreamDefinition& definition) const;
};

enum class LiveStreamItemKind
{
  Source,
  Repair,
};

/** Provider-signed actual group extent around opaque APP bytes (not nested Data). */
struct LiveStreamSampleEnvelope
{
  std::string groupId;
  std::string sampleClass;
  uint64_t groupItemIndex = 0;
  uint64_t actualSourceItems = 0;
  LiveStreamItemKind itemKind = LiveStreamItemKind::Source;
  std::vector<uint8_t> opaqueContent;

  std::optional<std::string> validate() const;
  ndn::Block wireEncode() const;
  bool wireDecode(const ndn::Block& block);
};

struct LiveStreamSampleReservation
{
  uint64_t sampleId = 0;
  std::string sampleClass;
  size_t predictedSourceItems = 0;
  LiveStreamGroupReservation group;

  std::optional<std::string> validate(const LiveStreamDefinition& definition) const;
};

/** Canonical repair Content; codedBytes are opaque bytes, never nested Data. */
struct LiveStreamFecRepair
{
  LiveStreamFecScheme scheme = LiveStreamFecScheme::XorOneRepair;
  uint64_t recoveryCapacity = 1;
  uint64_t repairIndex = 0;
  std::string streamId;
  uint64_t sessionEpoch = 0;
  uint64_t mappingVersion = 0;
  std::string groupId;
  uint64_t createdMs = 0;
  uint64_t expiresMs = 0;
  std::vector<ndn::Name> sourceNames;
  std::vector<StreamCursor> sourceCursors;
  std::vector<uint64_t> sourceLengths;
  std::vector<StreamContentDigest> sourceDigests;
  ndn::Name repairName;
  StreamCursor repairCursor = 0;
  std::vector<uint8_t> codedBytes;

  std::string validate(const LiveStreamDefinition& definition) const;
  ndn::Block wireEncode() const;
  bool wireDecode(const ndn::Block& block);
};

LiveStreamFecRepair
makeLiveStreamXorRepair(const LiveStreamDefinition& definition,
                        const std::string& groupId,
                        const std::vector<LiveStreamItemReservation>& sources,
                        const LiveStreamItemReservation& repair,
                        const std::vector<std::vector<uint8_t>>& opaqueSources,
                        uint64_t createdMs,
                        uint64_t expiresMs);

std::vector<LiveStreamFecRepair>
makeLiveStreamRepairSymbols(
  const LiveStreamDefinition& definition,
  const std::string& groupId,
  const std::vector<LiveStreamItemReservation>& sources,
  const std::vector<LiveStreamItemReservation>& repairs,
  const std::vector<std::vector<uint8_t>>& opaqueSources,
  uint64_t createdMs,
  uint64_t expiresMs);

std::optional<std::vector<uint8_t>>
recoverLiveStreamXorSource(
  const LiveStreamDefinition& definition,
  const LiveStreamFecRepair& repair,
  const std::vector<std::optional<std::vector<uint8_t>>>& opaqueSources,
  size_t missingIndex,
  uint64_t nowMs);

std::optional<std::vector<std::optional<std::vector<uint8_t>>>>
recoverLiveStreamSources(
  const LiveStreamDefinition& definition,
  const std::vector<LiveStreamFecRepair>& repairs,
  const std::vector<std::optional<std::vector<uint8_t>>>& opaqueSources,
  uint64_t nowMs);

struct LiveStreamReadiness
{
  double measuredSamplePeriodMs = 0.0;
  StreamCursor safeJoinCursor = 0;
};

struct LiveStreamDescriptor
{
  LiveStreamDefinition definition;
  StreamNameMapCheckpoint checkpoint;
  double measuredSamplePeriodMs = 0.0;
  StreamCursor safeJoinCursor = 0;

  std::optional<std::string> validate() const;
};

enum class LiveStreamLifecycleState
{
  Preparing,
  Active,
  Stopped,
  Failed,
};

enum class LiveStreamItemProvenance
{
  SignedData,
  FecRecovered,
};

const char* toString(LiveStreamLifecycleState state);
const char* toString(LiveStreamItemProvenance provenance);

// ── Predictive Stream (Spec 148) ──

struct PredictiveStreamCheckpoint
{
  uint64_t initialSampleId = 0;
  uint64_t oldestRetainedSampleId = 0;
  uint64_t latestProducedSampleId = 0;
  uint64_t nextExpectedSampleId = 0;

  std::optional<std::string> validate() const;
};

ndn::Name
makePredictiveFrontierName(const ndn::Name& mappingRoot);

ndn::Name
makePredictiveDataName(const ndn::Name& mappingRoot,
                       uint64_t mappingVersion,
                       uint64_t sequence);

ndn::Name
makePredictiveDataName(const LiveStreamDefinition& definition,
                       uint64_t sequence);

ndn::Name
makePredictiveGroupName(const LiveStreamDefinition& definition,
                        uint64_t groupId);

ndn::Name
makePredictiveRepairName(const LiveStreamDefinition& definition,
                         uint64_t groupId, uint64_t repairIndex);

struct PredictiveStreamGroupCommit
{
  uint64_t contractVersion = 1;
  std::string streamId;
  uint64_t sessionEpoch = 0;
  uint64_t mappingVersion = 0;
  uint64_t groupId = 0;
  uint64_t createdMs = 0;
  uint64_t expiresMs = 0;
  std::vector<ndn::Name> sourceNames;
  std::vector<uint64_t> sourceWireLengths;
  std::vector<std::array<uint8_t, 32>> sourceWireDigests;
  std::vector<ndn::Name> repairNames;
  uint64_t recoveryCapacity = 0;

  std::optional<std::string>
  validate(const LiveStreamDefinition& definition) const;
  ndn::Block wireEncode() const;
  bool wireDecode(const ndn::Block& block);
};

struct PredictiveStreamDescriptor
{
  LiveStreamDefinition definition;
  PredictiveStreamCheckpoint checkpoint;
  ndn::Name frontierName;
  double measuredSamplePeriodMs = 0.0;

  std::optional<std::string> validate() const;
  bool isPredictive() const;
};

struct PredictiveStreamFrontier
{
  uint64_t contractVersion = 2;
  std::string streamId;
  uint64_t sessionEpoch = 0;
  uint64_t mappingVersion = 0;
  PredictiveStreamCheckpoint checkpoint;
  std::optional<uint64_t> latestCommittedGroupId;
  std::vector<ndn::Name> retainedGroupCommitNames;
  std::vector<uint64_t> retainedGroupFirstCursors;
  std::vector<uint64_t> retainedGroupLastCursors;

  std::optional<std::string>
  validate(const LiveStreamDefinition& definition) const;
  ndn::Block wireEncode() const;
  bool wireDecode(const ndn::Block& block);
};

struct VerifiedLiveStreamItem
{
  StreamCursor cursor = 0;
  ndn::Name originalName;
  ndn::Name verifiedProvider;
  std::vector<uint8_t> content;
  LiveStreamItemProvenance provenance = LiveStreamItemProvenance::SignedData;
  uint64_t receivedMs = 0;
};

struct LiveStreamItemAdmission
{
  bool accepted = false;
  std::string reason;

  static LiveStreamItemAdmission acceptItem();
  static LiveStreamItemAdmission rejectItem(std::string reason);
};

struct LiveStreamSampleObservation
{
  uint64_t sampleId = 0;
  uint64_t arrivalMs = 0;
  double retrievalDelayMs = 0.0;
  uint64_t itemCount = 1;
};

struct StreamFetchDecision;
class StreamAdaptiveFetcherState;

struct LiveStreamStatus
{
  LiveStreamLifecycleState state = LiveStreamLifecycleState::Preparing;
  StreamCursorFrontiers frontiers;
  size_t retainedItems = 0;
  size_t pendingInterests = 0;
  size_t mappingBlocks = 0;
  size_t inFlight = 0;
  uint64_t delivered = 0;
  uint64_t rejected = 0;
  uint64_t recovered = 0;
  uint64_t timeouts = 0;
  uint64_t nacks = 0;
  uint64_t retryAttempts = 0;
  uint64_t lateArrivals = 0;
  uint64_t deadlineSkips = 0;
  uint64_t retryExhaustions = 0;
  uint64_t mappingInterests = 0;
  // Validated Mapping Data responses, and the subset that advanced resolver
  // state. Their ratio distinguishes useful Mapping information from duplicate
  // control Data without inspecting application payloads.
  uint64_t mappingDataResponses = 0;
  uint64_t mappingNewDataResponses = 0;
  uint64_t payloadInterests = 0;
  uint64_t initialPayloadInterests = 0;
  uint64_t retryPayloadInterests = 0;
  uint64_t payloadSourceInterests = 0;
  uint64_t initialPayloadSourceInterests = 0;
  uint64_t retryPayloadSourceInterests = 0;
  uint64_t payloadRepairInterests = 0;
  uint64_t initialPayloadRepairInterests = 0;
  uint64_t retryPayloadRepairInterests = 0;
  uint64_t payloadUnclassifiedInterests = 0;
  // Generic Payload-Interest utility accounting. A valid source Data response
  // becomes application-useful only after the APP admits it. A valid repair
  // Data response remains protection-only unless a successful recovery
  // consumes that exact repair symbol. Every other terminal attempt is
  // nonproductive; an unresolved remainder is exposed instead of guessed.
  uint64_t payloadSourceDataAdmissions = 0;
  uint64_t payloadRepairDataResponses = 0;
  uint64_t payloadRepairDataConsumed = 0;
  uint64_t payloadApplicationUsefulInterests = 0;
  uint64_t payloadProtectionOnlyInterests = 0;
  uint64_t payloadNonproductiveInterests = 0;
  uint64_t payloadUnresolvedInterests = 0;
  // Consumer-side count of Interests beyond the immutable join checkpoint.
  // Provider future-interest/future-hit counters are authoritative for whether
  // an Interest actually arrived before payload materialization.
  uint64_t futurePayloadInterests = 0;
  uint64_t initialFuturePayloadInterests = 0;
  uint64_t retryFuturePayloadInterests = 0;
  uint64_t futureCursorHorizon = 0;
  uint64_t retrySuccesses = 0;
  uint64_t retrySuppressions = 0;
  std::map<std::string, uint64_t> retrySuppressionReasons;
  uint64_t declaredRecoveryCapacity = 0;
  uint64_t recoveryEligibleSources = 0;
  uint64_t terminalMissingSources = 0;
  uint64_t recoverableGroups = 0;
  uint64_t recoveredGroups = 0;
  uint64_t recoveryAttempts = 0;
  uint64_t recoveryExhaustions = 0;
  // Predictive repair metadata is a separate control class. It is not the
  // Mapping-first name-discovery path and must not inflate mappingInterests.
  uint64_t recoveryControlInterests = 0;
  uint64_t recoveryFrontierInterests = 0;
  uint64_t recoveryGroupInterests = 0;
  uint64_t recoveryCoalescedWaiters = 0;
  uint64_t recoveryMetadataCacheHits = 0;
  uint64_t nextDeliverCursor = 0;
  uint64_t readyQueueDepth = 0;
  uint64_t oldestReadyCursor = 0;
  uint64_t terminalGapQueueDepth = 0;
  uint64_t drainWakeCount = 0;
  uint64_t staleReadyDrops = 0;
  uint64_t terminalGapSuperseded = 0;
  uint64_t mappingBytes = 0;
  uint64_t providerFutureInterests = 0;
  uint64_t providerFutureHits = 0;
  uint64_t providerInitialFutureInterests = 0;
  uint64_t providerInitialFutureHits = 0;
  uint64_t providerRetryFutureInterests = 0;
  uint64_t providerRetryFutureHits = 0;
  std::map<std::string, SampleClassPredictionStatus> sampleClassPredictions;
  std::string reason;
  std::shared_ptr<StreamFetchDecision> fetchDecision;
};

enum class PublishedLiveStreamPacketKind
{
  Mapping,
  Source,
  Repair,
};

struct PublishedLiveStreamPacket
{
  PublishedLiveStreamPacketKind kind = PublishedLiveStreamPacketKind::Source;
  std::string streamId;
  uint64_t sessionEpoch = 0;
  uint64_t mappingVersion = 0;
  std::optional<StreamCursor> cursor;
  ndn::Name dataName;
  ndn::Name provider;
  ndn::Buffer signedDataWire;
  StreamContentDigest wireDigest{};
  uint64_t materializedMonotonicUs = 0;
};

struct PublishedPacketFeedOptions
{
  StreamCursor fromCursor = 0;
  size_t maxQueuedPackets = 1024;
  size_t maxQueuedBytes = 8 * 1024 * 1024;
};

struct PublishedPacketFeedStatus
{
  size_t queuedPackets = 0;
  size_t queuedBytes = 0;
  uint64_t droppedPackets = 0;
  std::optional<StreamCursor> firstDroppedCursor;
  std::optional<StreamCursor> lastDroppedCursor;
  bool closed = false;
};

class PublishedPacketFeed
{
public:
  std::vector<PublishedLiveStreamPacket> takeAvailable(size_t maxItems);
  PublishedPacketFeedStatus status() const;
  void close();

private:
  friend class LiveStreamPublisher;
  explicit PublishedPacketFeed(PublishedPacketFeedOptions options);
  void enqueue(PublishedLiveStreamPacket packet);

private:
  PublishedPacketFeedOptions m_options;
  std::deque<PublishedLiveStreamPacket> m_queue;
  size_t m_queuedBytes = 0;
  uint64_t m_droppedPackets = 0;
  std::optional<StreamCursor> m_firstDroppedCursor;
  std::optional<StreamCursor> m_lastDroppedCursor;
  bool m_closed = false;
  mutable std::mutex m_mutex;
};

/**
 * Serve immutable, already-signed Data wires under their original names.
 * Used by replay/cache adapters; it never re-signs or rewrites packets.
 */
class StoredSignedPacketProducer
{
public:
  StoredSignedPacketProducer(ndn::Face& face, ndn::Name routePrefix,
                             const std::vector<ndn::Buffer>& signedPacketWires);
  ~StoredSignedPacketProducer();

  void start();
  void stop();
  size_t packetCount() const;

private:
  void onInterest(const ndn::Interest& interest);

private:
  ndn::Face& m_face;
  ndn::Name m_routePrefix;
  std::map<ndn::Name, std::shared_ptr<ndn::Data>> m_packets;
  ndn::ScopedRegisteredPrefixHandle m_route;
  bool m_started = false;
  mutable std::mutex m_mutex;
};

enum class LiveStreamStart
{
  Beginning,
  Latest,
};

enum class LiveStreamPrefetchPolicy
{
  MappedPressure,
  MappedLiveFutureOn,
  MappedLiveFutureOff,
  AdaptiveSampleAtomic,
};

const char*
toString(LiveStreamPrefetchPolicy policy);

struct LiveStreamOpenOptions
{
  LiveStreamStart start = LiveStreamStart::Latest;
  LiveStreamPrefetchPolicy prefetchPolicy = LiveStreamPrefetchPolicy::MappedPressure;
  size_t aggregateInterestLimit = 64;
  bool enableFecRecovery = false;
  uint64_t interestLifetimeMs = 500;
  std::function<LiveStreamItemAdmission(const VerifiedLiveStreamItem&)> onItem;
  std::function<void(const LiveStreamStatus&)> onStatus;
};

/** Provider-side owner of Mapping, exact semantic Data, pending Interests and FEC. */
class LiveStreamPublisher : public std::enable_shared_from_this<LiveStreamPublisher>
{
public:
  LiveStreamPublisher(LiveStreamDefinition definition,
                      ndn::Face& face,
                      ndn::KeyChain& keyChain,
                      ndn::security::SigningInfo signingInfo);
  ~LiveStreamPublisher();

  void start();
  /** Register the single predictable-name route used by the high-level API. */
  void startPredictive();
  /**
   * Wait until both existing routes are registered.
   *
   * This lifecycle-only primitive must be called off the Face I/O thread.
   * It does not fetch Data or change stream algorithms.
   */
  void waitUntilReady(std::chrono::milliseconds timeout);
  /** Activate after the predictive route is registered; no Mapping bootstrap. */
  void activatePredictive(double measuredSamplePeriodMs);
  /**
   * Retain and serve the exact App-signed wire without wrapping or re-signing.
   * @return true when newly admitted; false for a byte-identical duplicate.
   */
  bool publishSignedData(const std::shared_ptr<ndn::Data>& signedData);
  /** Atomically publish Core-owned repair/group/frontier Data for one group. */
  PredictiveStreamFrontier commitPredictiveGroup(
    uint64_t groupId,
    const std::vector<std::shared_ptr<ndn::Data>>& signedSources);
  PredictiveStreamFrontier predictiveFrontier() const;
  LiveStreamItemReservation reserveAhead(const ndn::Name& originalName);
  std::vector<LiveStreamItemReservation>
  reserveAhead(const std::vector<ndn::Name>& originalNames);
  LiveStreamGroupReservation reserveGroup(const std::string& groupId,
                                           const std::vector<ndn::Name>& sourceNames,
                                           const std::vector<ndn::Name>& repairNames);
  LiveStreamSampleReservation announceSample(
    uint64_t sampleId,
    const std::string& sampleClass,
    const std::function<ndn::Name(size_t, LiveStreamItemKind)>& nameFactory);
  /** Resolve an authenticated APP-known real extent before name-bound protection. */
  std::vector<LiveStreamItemReservation>
  prepareSampleExtent(const LiveStreamSampleReservation& reservation,
                      size_t actualSourceItems);
  void publish(const LiveStreamItemReservation& reservation,
               const std::vector<uint8_t>& opaqueContent);
  void publishGroup(const LiveStreamGroupReservation& reservation,
                    const std::vector<std::vector<uint8_t>>& opaqueSources);
  void publishSample(const LiveStreamSampleReservation& reservation,
                     const std::vector<std::vector<uint8_t>>& opaqueSources);
  LiveStreamDescriptor activate(const LiveStreamReadiness& readiness);
  std::shared_ptr<PublishedPacketFeed>
  openPublishedPacketFeed(const PublishedPacketFeedOptions& options);
  LiveStreamStatus status() const;
  void stop();

private:
  struct PendingInterest
  {
    uint64_t order = 0;
    uint64_t expiresAtMs = 0;
    bool retry = false;
  };

  using PendingInterestTable = std::map<ndn::Name, PendingInterest>;

  std::vector<LiveStreamItemReservation>
  reserveBlock(const std::vector<ndn::Name>& originalNames);
  std::vector<LiveStreamItemReservation>
  reserveEntryBlock(const std::vector<StreamNameMapEntry>& entries);
  std::vector<LiveStreamItemReservation>
  reserveEntries(const std::vector<StreamNameMapEntry>& entries);
  std::shared_ptr<ndn::Data>
  makePayloadPacket(const LiveStreamItemReservation& reservation,
                    const std::vector<uint8_t>& opaqueContent,
                    const LiveStreamSampleEnvelope* envelope = nullptr) const;
  void publishGroupImpl(const LiveStreamGroupReservation& reservation,
                        const std::vector<std::vector<uint8_t>>& opaqueSources,
                        const std::string* sampleClass = nullptr,
                        size_t actualSourceItems = 0);
  void onMappingInterest(const ndn::Interest& interest);
  void onPayloadInterest(const ndn::Interest& interest);
  void putIfPending(const std::shared_ptr<ndn::Data>& data);
  void cleanupPendingLocked(uint64_t nowMs);
  bool admitPendingLocked(PendingInterestTable& table, size_t capacity,
                          const ndn::Name& name, uint64_t order,
                          uint64_t expiresAtMs);
  PublishedLiveStreamPacket makePublishedPacket(
    PublishedLiveStreamPacketKind kind, const ndn::Data& data,
    std::optional<StreamCursor> cursor) const;
  void notifyFeedsLocked(const PublishedLiveStreamPacket& packet);
  bool verifyPredictiveSourceSignature(const ndn::Data& data) const;
  std::shared_ptr<ndn::Data>
  makePredictiveControlPacket(const ndn::Name& name,
                              const ndn::Block& content) const;

private:
  LiveStreamDefinition m_definition;
  ndn::Face& m_face;
  ndn::KeyChain& m_keyChain;
  ndn::security::SigningInfo m_signingInfo;
  LiveStreamLifecycleState m_state = LiveStreamLifecycleState::Preparing;
  std::string m_reason;
  StreamCursor m_nextCursor = 0;
  std::map<uint64_t, StreamNameMapBlock> m_mappingBlocks;
  std::map<ndn::Name, std::shared_ptr<ndn::Data>> m_mappingPackets;
  std::map<ndn::Name, LiveStreamItemReservation> m_reservations;
  std::set<StreamCursor> m_materialized;
  std::set<StreamCursor> m_repairCursors;
  std::map<ndn::Name, std::shared_ptr<ndn::Data>> m_payloadPackets;
  std::set<ndn::Name> m_seenFuturePayloadNames;
  std::vector<std::weak_ptr<PublishedPacketFeed>> m_packetFeeds;
  std::deque<ndn::Name> m_retentionOrder;
  PendingInterestTable m_pendingMappings;
  PendingInterestTable m_pendingPayloads;
  size_t m_routesReady = 0;
  size_t m_expectedRoutes = 2;
  bool m_startCalled = false;
  bool m_predictiveMode = false;
  uint64_t m_predictiveNextExpectedCursor = 0;
  std::optional<uint64_t> m_predictiveLatestProducedCursor;
  PredictiveStreamFrontier m_predictiveFrontier;
  std::map<uint64_t, PredictiveStreamGroupCommit> m_predictiveGroups;
  std::deque<uint64_t> m_predictiveGroupRetentionOrder;
  uint64_t m_predictiveDuplicates = 0;
  bool m_routeFailed = false;
  StreamCursor m_latestJoinCursor = 0;
  double m_measuredSamplePeriodMs = 0.0;
  uint64_t m_recovered = 0;
  uint64_t m_providerFutureInterests = 0;
  uint64_t m_providerFutureHits = 0;
  uint64_t m_providerInitialFutureInterests = 0;
  uint64_t m_providerInitialFutureHits = 0;
  uint64_t m_providerRetryFutureInterests = 0;
  uint64_t m_providerRetryFutureHits = 0;
  LiveStreamSamplePredictor m_samplePredictor;
  std::map<uint64_t,
           std::function<ndn::Name(size_t, LiveStreamItemKind)>> m_sampleNameFactories;
  std::map<uint64_t, std::pair<size_t, LiveStreamGroupReservation>>
    m_preparedSampleContinuations;
  ndn::ScopedRegisteredPrefixHandle m_mappingRoute;
  ndn::ScopedRegisteredPrefixHandle m_payloadRoute;
  std::condition_variable m_routeCondition;
  mutable std::mutex m_mutex;
};

/** Consumer-side exact-name prefetch lifecycle over one validated descriptor. */
class LiveStreamConsumerHandle : public std::enable_shared_from_this<LiveStreamConsumerHandle>
{
public:
  LiveStreamConsumerHandle(LiveStreamDescriptor descriptor,
                           LiveStreamOpenOptions options,
                           ndn::Face& face,
                           std::shared_ptr<MessageValidator> validator);
  ~LiveStreamConsumerHandle();

  void start();
  bool observeAcceptedSample(const LiveStreamSampleObservation& observation);
  LiveStreamStatus status() const;
  void stop();

private:
  void schedule();
  void fetchMapping(uint64_t blockNumber, uint64_t interestLifetimeMs,
                    uint64_t requestToken);
  void fetchPayload(StreamCursor cursor, const ndn::Name& name,
                    uint64_t interestLifetimeMs, bool aheadOfJoinCheckpoint,
                    bool retryAttempt);
  void skipPayload(StreamCursor cursor, std::string reason);
  void advanceCompleted();
  void tryRecover(const std::string& groupId);
  void retireRecoveryGroupLocked(const std::string& groupId);
  bool hasExpectedProviderSignature(const ndn::Data& data) const;
  bool isActive(uint64_t generation) const;
  bool isCurrentMappingRequest(uint64_t blockNumber, uint64_t requestToken,
                               uint64_t generation) const;
  void emitStatus() const;
  void fail(std::string reason);

private:
  LiveStreamDescriptor m_descriptor;
  LiveStreamOpenOptions m_options;
  ndn::Face& m_face;
  std::shared_ptr<MessageValidator> m_validator;
  LiveStreamLifecycleState m_state = LiveStreamLifecycleState::Preparing;
  std::string m_reason;
  StreamNameResolverState m_resolver;
  std::unique_ptr<StreamAdaptiveFetcherState> m_fetcher;
  StreamCursor m_nextCursor = 0;
  uint64_t m_nextMappingBlock = 0;
  std::set<uint64_t> m_mappingInFlight;
  // Tracks which live Mapping requests are speculative. A request retains
  // single ownership until Data, Nack, or timeout; becoming the current gap
  // does not create a duplicate exact-name Interest.
  std::set<uint64_t> m_mappingFutureInFlight;
  std::map<uint64_t, uint64_t> m_mappingRequestTokens;
  // Mapping blocks may validate out of order and remain quarantined until a
  // predecessor arrives. Remember every accepted block independently from
  // the connected resolver frontier so bounded lookahead does not refetch it.
  std::set<uint64_t> m_mappingReceived;
  std::set<StreamCursor> m_payloadInFlight;
  // Network ownership and application processing are deliberately separate.
  // A received Data packet frees its Interest-pipeline slot immediately, but
  // its cursor remains here until validation and the APP callback finish.
  std::set<StreamCursor> m_payloadProcessing;
  std::map<StreamCursor, uint64_t> m_payloadExpressedAtMs;
  std::set<StreamCursor> m_completed;
  // Sources with a terminal finite-retry outcome remain eligible for one
  // later FEC recovery. An individual timeout/Nack is not proof of permanent
  // loss, and in-flight or validating sources never enter this set.
  // Membership is consumed before the application callback, preserving
  // exactly-once delivery even though the scheduling cursor has advanced.
  std::set<StreamCursor> m_recoverableSkips;
  // A timed-out source may be reconstructed from already authenticated repair
  // symbols before spending the remaining exact-name retries. Unlike a
  // terminal skip, this state does not advance m_nextCursor by itself.
  std::set<StreamCursor> m_recoveryEligibleTimeouts;
  std::set<StreamCursor> m_fecRecoveryInFlight;
  std::map<StreamCursor, std::vector<uint8_t>> m_signedOpaque;
  std::map<std::string, std::map<uint64_t, LiveStreamFecRepair>> m_repairs;
  // Direct authenticated relation used to trigger recovery for one source
  // without scanning every historical repair group on each Data admission.
  std::map<StreamCursor, std::string> m_recoveryGroupBySource;
  std::set<StreamCursor> m_consumedRepairCursors;
  std::set<std::string> m_recoveryExhaustedGroups;
  std::set<std::string> m_pendingRecoverableGroups;
  std::set<std::string> m_recoverySucceededGroups;
  std::set<std::string> m_observedSampleGroups;
  std::map<uint64_t, size_t> m_mappingAttempts;
  std::map<StreamCursor, size_t> m_payloadAttempts;
  uint64_t m_generation = 1;
  uint64_t m_delivered = 0;
  uint64_t m_rejected = 0;
  uint64_t m_recovered = 0;
  uint64_t m_timeouts = 0;
  uint64_t m_nacks = 0;
  uint64_t m_retryAttempts = 0;
  uint64_t m_lateArrivals = 0;
  uint64_t m_deadlineSkips = 0;
  uint64_t m_retryExhaustions = 0;
  uint64_t m_mappingInterests = 0;
  uint64_t m_mappingDataResponses = 0;
  uint64_t m_mappingNewDataResponses = 0;
  uint64_t m_payloadInterests = 0;
  uint64_t m_futurePayloadInterests = 0;
  uint64_t m_initialPayloadInterests = 0;
  uint64_t m_retryPayloadInterests = 0;
  uint64_t m_payloadSourceInterests = 0;
  uint64_t m_initialPayloadSourceInterests = 0;
  uint64_t m_retryPayloadSourceInterests = 0;
  uint64_t m_payloadRepairInterests = 0;
  uint64_t m_initialPayloadRepairInterests = 0;
  uint64_t m_retryPayloadRepairInterests = 0;
  uint64_t m_payloadUnclassifiedInterests = 0;
  uint64_t m_payloadSourceDataAdmissions = 0;
  uint64_t m_payloadRepairDataResponses = 0;
  uint64_t m_payloadRepairDataConsumed = 0;
  uint64_t m_payloadNonproductiveInterests = 0;
  uint64_t m_initialFuturePayloadInterests = 0;
  uint64_t m_retryFuturePayloadInterests = 0;
  uint64_t m_retrySuccesses = 0;
  uint64_t m_retrySuppressions = 0;
  std::map<std::string, uint64_t> m_retrySuppressionReasons;
  uint64_t m_recoveryEligibleSources = 0;
  uint64_t m_terminalMissingSources = 0;
  uint64_t m_recoverableGroups = 0;
  uint64_t m_recoveredGroups = 0;
  uint64_t m_recoveryAttempts = 0;
  uint64_t m_recoveryExhaustions = 0;
  uint64_t m_mappingBytes = 0;
  uint64_t m_atomicExpansions = 0;
  uint64_t m_atomicDeferrals = 0;
  std::string m_atomicCapacityReason;
  mutable std::mutex m_mutex;
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
  uint64_t overflows = 0;
  uint64_t maxPending = 0;
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
  StreamMetrics metrics() const;

private:
  size_t m_maxChunks;
  std::map<uint64_t, StreamChunk> m_chunks;
  std::deque<uint64_t> m_order;
  StreamMetrics m_metrics;
  mutable std::mutex m_mutex;
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
  std::vector<uint64_t> pendingSequences(size_t limit = 0) const;
  std::vector<StreamChunk> drainReady();
  void skipTo(uint64_t seq);
  uint64_t nextSeq() const;
  size_t pendingCount() const;
  size_t pendingBytes() const;
  StreamMetrics metrics() const;

private:
  void markCompleted(uint64_t seq);
  void dropOldestPending();
  std::vector<StreamChunk> drainReadyUnlocked();

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
  mutable std::mutex m_mutex;
};

enum class StreamPrefetchPhase
{
  Inactive,
  Chasing,
  Adjusting,
  Fetching,
  Recovering,
  Stopped,
};

const char*
toString(StreamPrefetchPhase phase);

struct StreamFetchDecision
{
  uint64_t window = 0;
  uint64_t lookahead = 0;
  uint64_t interestLifetimeMs = 0;
  uint64_t missingTimeoutMs = 0;
  uint64_t sampleDemand = 0;
  uint64_t packetDemand = 0;
  uint64_t holdMs = 0;
  uint64_t recoveryCheckpointMs = 0;
  uint64_t remainingRecoveryBudgetMs = 0;
  uint64_t mappingBeginBlock = 0;
  uint64_t mappingEndBlock = 0;
  uint64_t payloadBeginCursor = 0;
  uint64_t payloadEndCursor = 0;
  uint64_t aggregateInFlightLimit = 0;
  uint64_t mappingBudget = 0;
  uint64_t payloadBudget = 0;
  uint64_t retransmissionBudget = 0;
  uint64_t futureWaitCount = 0;
  uint64_t terminalUnproducedAdvice = 0;
  uint64_t laterCursorAdvice = 0;
  uint64_t atomicExpansions = 0;
  uint64_t atomicDeferrals = 0;
  double pressure = 0.0;
  double liveEdgeConfidence = 0.0;
  bool mappingReady = false;
  bool futureWait = false;
  bool congestionHold = false;
  bool retransmissionEligible = false;
  StreamPrefetchPhase phase = StreamPrefetchPhase::Inactive;
  std::string policyMode = "pressure-only";
  std::string detectorProfile = "none";
  std::string mappingWaitReason = "inactive";
  std::string capacityReason;
  std::string reason = "stable";
};

enum class StreamHealthState
{
  Active,
  Degraded,
  Congested,
  Stale,
  Stopped,
};

struct StreamHealth
{
  std::string streamId;
  uint64_t sessionEpoch = 0;
  StreamHealthState state = StreamHealthState::Active;
  uint64_t nextSeq = 0;
  uint64_t lastChunkMs = 0;
  uint64_t updatedMs = 0;
  StreamMetrics metrics;
  StreamFetchDecision fetchDecision;
  std::string reason;
  std::map<std::string, std::string> metadata;

  static StreamHealth fromStream(const StreamInfo& info,
                                 const StreamMetrics& metrics,
                                 const std::optional<StreamFetchDecision>& fetchDecision = std::nullopt,
                                 uint64_t nextSeq = 0,
                                 uint64_t lastChunkMs = 0,
                                 bool stopped = false,
                                 uint64_t staleAfterMs = 3000,
                                 uint64_t nowMs = 0);
};

const char*
toString(StreamHealthState state);

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
  double liveEdgeChangeThreshold = 0.10;
  double liveEdgePeriodSimilarity = 0.95;
  uint64_t liveEdgeWindow = 30;
  uint64_t liveEdgeStableRequired = 4;
  uint64_t detectionPeriodMs = 1000;
  uint64_t recoveryReservePackets = 1;
  uint64_t aggregateInFlightLimit = 64;
  uint64_t mappingReserve = 4;
  uint64_t retransmissionReserve = 1;
  uint64_t mappingBlockCapacity = 16;
  double chaseMultiplier = 2.0;
  double adjustMultiplier = 0.75;
  double congestionDecreaseMultiplier = 0.5;
  std::string detectorProfile = "ndnsf-conservative-seed";

  void observeRtt(double sampleMs, double alpha = 0.25);
  /**
   * Observe Interest-expression to Data-reception delay for one payload.
   *
   * An ahead-of-join-checkpoint Interest may include producer generation wait
   * (DRD' = DRD + dgen). During live-edge search that upper bound may correct
   * an RTT estimate downward, but cannot raise it. Once Fetching is stable,
   * generation wait is expected to be minimal and normal adaptation resumes.
   */
  void observePayloadDelay(double sampleMs, bool aheadOfJoinCheckpoint);
  void recordTimeout();
  void recordTimeout(uint64_t cursor, bool knownProduced, bool wasFuture);
  void recordNack();
  void recordNack(uint64_t cursor, const std::string& reason);
  void recordCongestionMark(uint64_t cursor, uint64_t mark);
  void recordDuplicate();
  void setBacklogPressure(double pressure);
  void decay(double factor = 0.85);
  void resetLive(uint64_t sessionEpoch, uint64_t nextSeq,
                 double samplePeriodMs, uint64_t nowMs = 0);
  void configureMappedLive(uint64_t aggregateLimit,
                           uint64_t mapReserve,
                           uint64_t retransmitReserve,
                           uint64_t blockCapacity,
                           std::string profile);
  void resetMappedLive(uint64_t sessionEpoch,
                       uint64_t nextCursor,
                       double samplePeriodMs,
                       uint64_t latestProducedCursor,
                       uint64_t mappingCommittedThroughCursor,
                       uint64_t nextReservedCursor,
                       uint64_t nowMs = 0);
  void updateMappingFrontier(uint64_t mappingCommittedThroughCursor,
                             uint64_t nextReservedCursor);
  void advanceNextCursor(uint64_t nextCursor);
  void setMappedLivePolicyEnabled(bool enabled);
  /** Replace the bounded lookahead with signed Mapping v2 group extents. */
  void setPredictedSampleGroups(std::vector<uint64_t> groupItems);
  void setInFlight(uint64_t mapping,
                   uint64_t payload,
                   uint64_t retransmission);
  bool observeAcceptedSample(uint64_t sessionEpoch, uint64_t sampleId,
                             uint64_t arrivalMs, double retrievalDelayMs,
                             uint64_t segmentCount = 1,
                             bool knownProduced = true);
  void observeSampleExtent(uint64_t predictedCount, uint64_t actualCount);
  void beginRecovery(uint64_t nowMs, uint64_t playoutDeadlineMs);
  void recordRecovery(bool completed);
  void recordInvalidObservation();
  void stopLive();
  StreamPrefetchPhase phase() const;
  uint64_t invalidObservations() const;
  StreamFetchDecision decide(uint64_t nowMs = 0,
                             uint64_t playoutDeadlineMs = 0) const;

private:
  void evaluateLiveEdge(uint64_t nowMs);
  bool evaluateStability(double oldMean, double newMean) const;
  uint64_t liveSampleDemand() const;
  uint64_t livePacketDemand() const;

private:
  bool m_liveMode = false;
  bool m_mappedLive = false;
  bool m_mappedLivePolicyEnabled = true;
  uint64_t m_sessionEpoch = 0;
  uint64_t m_nextSeq = 0;
  uint64_t m_latestProducedCursor = 0;
  uint64_t m_mappingCommittedThroughCursor = 0;
  uint64_t m_nextReservedCursor = 0;
  uint64_t m_mappingInFlight = 0;
  uint64_t m_payloadInFlight = 0;
  uint64_t m_retransmissionInFlight = 0;
  uint64_t m_futureWaitCount = 0;
  uint64_t m_terminalUnproducedAdvice = 0;
  uint64_t m_laterCursorAdvice = 0;
  uint64_t m_congestionHoldUntilMs = 0;
  uint64_t m_recoveryStartedMs = 0;
  uint64_t m_recoveryDeadlineMs = 0;
  bool m_futureWait = false;
  double m_samplePeriodMs = 0.0;
  double m_liveRttFloorMs = 0.0;
  double m_segmentsPerSample = 1.0;
  std::deque<uint64_t> m_predictedSampleGroups;
  StreamPrefetchPhase m_phase = StreamPrefetchPhase::Inactive;
  uint64_t m_lastSampleId = 0;
  uint64_t m_lastSampleArrivalMs = 0;
  bool m_hasLastSample = false;
  std::deque<double> m_sampleArrivalPeriods;
  uint64_t m_consecutiveStable = 0;
  uint64_t m_lastActionMs = 0;
  uint64_t m_liveWindow = 0;
  uint64_t m_previousUsableWindow = 0;
  uint64_t m_invalidObservations = 0;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_STREAM_HPP
