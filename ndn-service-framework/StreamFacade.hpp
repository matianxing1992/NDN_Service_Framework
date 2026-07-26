#ifndef NDN_SERVICE_FRAMEWORK_STREAM_FACADE_HPP
#define NDN_SERVICE_FRAMEWORK_STREAM_FACADE_HPP

#include "Stream.hpp"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

namespace ndn_service_framework {

class ServiceProvider;

struct StreamAdvancedOptions
{
  size_t mappingBlockCapacity = 16;
  size_t mappingAheadBlocks = 4;
  size_t retainedItems = 600;
  size_t maxNameReservations = 65536;
  size_t maxPendingInterests = 256;
  size_t signedWireCap = ndn::MAX_NDN_PACKET_SIZE;
  uint64_t startupTimeoutMs = 1000;
};

struct StreamConfig
{
  std::string streamId;
  ndn::Name dataPrefix;
  double samplePeriodMs = 0.0;
  std::vector<SampleClassProfile> sampleClasses;
  LiveStreamFecOptions fec = LiveStreamFecOptions::none();
  std::optional<uint64_t> sessionEpoch;
  StreamAdvancedOptions advanced;
};

class StreamPublisher
{
public:
  ~StreamPublisher();

  PredictiveStreamDescriptor start();

  void push(std::shared_ptr<ndn::Data> signedData);

  void flush();

  // ── Common ──
  LiveStreamStatus status() const;
  void stop();

private:
  friend class ServiceProvider;
  struct SessionClaim;

  enum class State
  {
    Created,
    Bootstrapping,
    Active,
    Failed,
    Stopped,
  };

  using LowLevelFactory =
    std::function<std::shared_ptr<LiveStreamPublisher>(
      const LiveStreamDefinition&)>;

  static std::shared_ptr<StreamPublisher>
  create(const StreamConfig& config, const ndn::Name& provider,
         const LowLevelFactory& factory);

  StreamPublisher(StreamConfig config, LiveStreamDefinition definition,
                  std::shared_ptr<LiveStreamPublisher> lowLevel,
                  std::shared_ptr<SessionClaim> sessionClaim);

  void failLocked(const std::string& reason);
  void requireStateLocked(State expected, const char* operation) const;

private:
  StreamConfig m_config;
  LiveStreamDefinition m_definition;
  std::shared_ptr<LiveStreamPublisher> m_lowLevel;
  std::shared_ptr<SessionClaim> m_sessionClaim;
  std::vector<std::shared_ptr<ndn::Data>> m_pendingSegments;
  uint64_t m_nextRepairGroup = 0;
  State m_state = State::Created;
  std::string m_failureReason;
  mutable std::mutex m_mutex;
};

struct StreamSubscriptionOptions
{
  LiveStreamStart start = LiveStreamStart::Latest;
  std::optional<LiveStreamPrefetchPolicy> prefetchPolicy;
  size_t aggregateInterestLimit = 64;
  bool enableFecRecovery = true;
  bool requireFullDelivery = false;
  uint64_t interestLifetimeMs = 500;
  std::function<LiveStreamItemAdmission(const VerifiedLiveStreamItem&)> onItem;
  std::function<void(const LiveStreamStatus&)> onStatus;
};

class PredictiveStreamSubscriber
  : public std::enable_shared_from_this<PredictiveStreamSubscriber>
{
public:
  using ItemCallback =
    std::function<LiveStreamItemAdmission(const VerifiedLiveStreamItem&)>;
  using StatusCallback =
    std::function<void(const LiveStreamStatus&)>;

  PredictiveStreamSubscriber(ndn::Face& face,
                              std::shared_ptr<MessageValidator> validator,
                              PredictiveStreamDescriptor descriptor,
                              StreamSubscriptionOptions options);
  ~PredictiveStreamSubscriber();

  void start();
  LiveStreamStatus status() const;
  void stop();

private:
  void fetchFrontier(uint64_t generation);
  void installDescriptorCheckpointAndSchedule(uint64_t generation);
  void schedule();
  void fetchSegment(uint64_t cursor, bool retry, uint64_t generation);
  void onData(const ndn::Data& data, uint64_t cursor, uint64_t generation);
  void onNack(uint64_t cursor, uint64_t generation,
              const std::string& reason);
  void onTimeout(uint64_t cursor, uint64_t generation);
  void onValidatedData(const ndn::Data& data, uint64_t cursor,
                       uint64_t generation,
                       LiveStreamItemProvenance provenance =
                         LiveStreamItemProvenance::SignedData);
  void onValidationFailure(uint64_t cursor, uint64_t generation,
                           std::string reason);
  void retryOrDeclareGap(uint64_t cursor, uint64_t generation,
                         bool wasNack, std::string reason);
  bool beginRecovery(uint64_t cursor, uint64_t generation);
  void fetchRecoveryFrontier(uint64_t cursor, uint64_t generation);
  void fetchRecoveryGroup(
    uint64_t cursor,
    std::shared_ptr<const std::vector<ndn::Name>> groupNames,
    size_t reverseIndex, uint64_t generation);
  void handleRecoveryGroup(
    uint64_t cursor,
    std::shared_ptr<const std::vector<ndn::Name>> groupNames,
    size_t reverseIndex, const ndn::Name& groupName,
    const PredictiveStreamGroupCommit& group, uint64_t generation);
  void fetchRecoveryRepairs(
    uint64_t cursor, PredictiveStreamGroupCommit group,
    uint64_t generation);
  void attemptRecovery(
    uint64_t cursor, const PredictiveStreamGroupCommit& group,
    uint64_t generation);
  void finishRecoveryFailure(
    uint64_t cursor, uint64_t generation, std::string reason);
  void drainReady(uint64_t generation);
  bool hasExpectedProviderSignature(const ndn::Data& data) const;
  bool isActiveLocked(uint64_t generation) const;
  void emitStatus() const;

  ndn::Face& m_face;
  std::shared_ptr<MessageValidator> m_validator;
  PredictiveStreamDescriptor m_descriptor;
  StreamSubscriptionOptions m_options;
  std::unique_ptr<StreamAdaptiveFetcherState> m_fetcher;
  uint64_t m_nextScheduleCursor = 0;
  uint64_t m_nextDeliverCursor = 0;
  uint64_t m_latestKnownProducedCursor = 0;
  uint64_t m_futureCursorHorizon = 1;
  uint64_t m_generation = 1;
  std::optional<ndn::ScopedPendingInterestHandle> m_frontierInterest;
  std::set<uint64_t> m_scheduled;
  std::set<uint64_t> m_retryPending;
  std::set<uint64_t> m_inFlight;
  std::map<uint64_t, ndn::ScopedPendingInterestHandle> m_pendingInterests;
  std::set<uint64_t> m_processing;
  std::map<uint64_t,
           std::pair<std::shared_ptr<ndn::Data>, LiveStreamItemProvenance>>
    m_ready;
  std::set<uint64_t> m_terminalGaps;
  std::map<uint64_t, size_t> m_attempts;
  std::map<uint64_t, uint64_t> m_expressedAtMs;
  std::set<uint64_t> m_futureRequested;
  std::map<uint64_t, StreamContentDigest> m_admittedDigests;
  std::map<uint64_t, std::vector<uint8_t>> m_sourceWires;
  std::map<uint64_t, PredictiveStreamGroupCommit> m_recoveryGroups;
  std::map<std::string, PredictiveStreamGroupCommit> m_recoveryGroupCache;
  struct RecoveryGroupWaiter
  {
    uint64_t cursor = 0;
    std::shared_ptr<const std::vector<ndn::Name>> groupNames;
    size_t reverseIndex = 0;
    uint64_t generation = 0;
  };
  std::map<std::string, std::vector<RecoveryGroupWaiter>>
    m_recoveryGroupWaiters;
  bool m_recoveryFrontierPending = false;
  std::optional<ndn::ScopedPendingInterestHandle> m_recoveryFrontierInterest;
  std::set<uint64_t> m_recoveryFrontierWaiters;
  std::shared_ptr<const std::vector<ndn::Name>>
    m_recoveryFrontierGroupNames;
  std::shared_ptr<const std::vector<uint64_t>>
    m_recoveryFrontierGroupFirstCursors;
  std::shared_ptr<const std::vector<uint64_t>>
    m_recoveryFrontierGroupLastCursors;
  uint64_t m_recoveryFrontierLatestProduced = 0;
  std::map<uint64_t, std::vector<LiveStreamFecRepair>> m_recoveryRepairs;
  std::map<uint64_t, size_t> m_repairResponsesPending;
  std::set<uint64_t> m_recoveryAttempted;
  std::set<uint64_t> m_recoveryInProgress;
  std::map<std::string, ndn::ScopedPendingInterestHandle> m_controlInterests;
  bool m_draining = false;
  bool m_drainWakePending = false;
  uint64_t m_drainWakeCount = 0;
  uint64_t m_staleReadyDrops = 0;
  uint64_t m_terminalGapSuperseded = 0;
  uint64_t m_delivered = 0;
  uint64_t m_rejected = 0;
  uint64_t m_timeouts = 0;
  uint64_t m_nacks = 0;
  uint64_t m_retryAttempts = 0;
  uint64_t m_retrySuccesses = 0;
  uint64_t m_retryExhaustions = 0;
  uint64_t m_lateArrivals = 0;
  uint64_t m_terminalMissingSources = 0;
  uint64_t m_recovered = 0;
  uint64_t m_recoveryAttempts = 0;
  uint64_t m_recoveryExhaustions = 0;
  uint64_t m_recoverableGroups = 0;
  uint64_t m_recoveredGroups = 0;
  uint64_t m_recoveryControlInterests = 0;
  uint64_t m_recoveryFrontierInterests = 0;
  uint64_t m_recoveryGroupInterests = 0;
  uint64_t m_recoveryCoalescedWaiters = 0;
  uint64_t m_recoveryMetadataCacheHits = 0;
  uint64_t m_mappingInterests = 0;
  uint64_t m_mappingDataResponses = 0;
  uint64_t m_mappingNewDataResponses = 0;
  uint64_t m_payloadInterests = 0;
  uint64_t m_initialPayloadInterests = 0;
  uint64_t m_retryPayloadInterests = 0;
  uint64_t m_futurePayloadInterests = 0;
  uint64_t m_initialFuturePayloadInterests = 0;
  uint64_t m_retryFuturePayloadInterests = 0;
  uint64_t m_payloadSourceDataAdmissions = 0;
  uint64_t m_payloadRepairInterests = 0;
  uint64_t m_payloadRepairDataResponses = 0;
  uint64_t m_payloadRepairDataConsumed = 0;
  LiveStreamLifecycleState m_state = LiveStreamLifecycleState::Preparing;
  std::string m_reason;
  mutable std::mutex m_mutex;
};

namespace detail {

uint64_t
computePredictiveFutureCursorHorizon(uint64_t lookahead,
                                     uint64_t aggregateWindow);

LiveStreamOpenOptions
makeLiveStreamOpenOptions(const LiveStreamDescriptor& descriptor,
                          StreamSubscriptionOptions options);

} // namespace detail

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_STREAM_FACADE_HPP
