#include "StreamFacade.hpp"

#include <ndn-cxx/util/logger.hpp>
#include <ndn-cxx/util/random.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>

NDN_LOG_INIT(ndn_service_framework.StreamFacade);

namespace ndn_service_framework {

namespace {

using SessionKey = std::tuple<std::string, std::string, uint64_t>;

std::mutex g_sessionMutex;
std::set<SessionKey> g_liveSessions;

std::string
toLowerHex(const StreamContentDigest& digest)
{
  static constexpr char DIGITS[] = "0123456789abcdef";
  std::string value;
  value.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    value.push_back(DIGITS[byte >> 4]);
    value.push_back(DIGITS[byte & 0x0f]);
  }
  return value;
}

SessionKey
makeSessionKey(const ndn::Name& provider, const ndn::Name& dataPrefix,
               uint64_t epoch)
{
  return {provider.toUri(), dataPrefix.toUri(), epoch};
}

uint64_t
claimEpoch(const ndn::Name& provider, const ndn::Name& dataPrefix,
           const std::optional<uint64_t>& requested)
{
  std::lock_guard<std::mutex> guard(g_sessionMutex);
  if (requested) {
    if (*requested == 0) {
      throw std::invalid_argument("Stream session epoch must be nonzero");
    }
    if (!g_liveSessions.insert(makeSessionKey(provider, dataPrefix, *requested)).second) {
      throw std::invalid_argument("Stream session epoch is already live");
    }
    return *requested;
  }

  for (size_t attempt = 0; attempt < 64; ++attempt) {
    const auto epoch = ndn::random::generateSecureWord64();
    if (epoch != 0 &&
        g_liveSessions.insert(makeSessionKey(provider, dataPrefix, epoch)).second) {
      return epoch;
    }
  }
  throw std::runtime_error("cannot allocate a unique Stream session epoch");
}

LiveStreamDefinition
deriveDefinition(const StreamConfig& config, const ndn::Name& provider,
                 uint64_t epoch)
{
  if (config.streamId.empty() || config.dataPrefix.empty() ||
      !std::isfinite(config.samplePeriodMs) || config.samplePeriodMs <= 0.0 ||
      config.sampleClasses.empty() || config.advanced.startupTimeoutMs == 0) {
    throw std::invalid_argument("invalid StreamConfig");
  }

  LiveStreamDefinition definition;
  definition.contractVersion = STREAM_NAME_MAP_CONTRACT_VERSION_V2;
  definition.streamId = config.streamId;
  definition.provider = provider;
  definition.semanticDataPrefix = config.dataPrefix;
  definition.semanticDataPrefix.appendVersion(epoch);
  definition.sessionEpoch = epoch;
  // Existing resolver authority requires the payload prefix's final Version
  // component to equal mappingVersion. Reuse the collision-free session epoch
  // for both so the facade adds exactly one version component.
  definition.mappingVersion = epoch;
  definition.mappingBlockCapacity = config.advanced.mappingBlockCapacity;
  definition.mappingAheadBlocks = config.advanced.mappingAheadBlocks;
  definition.retainedItems = config.advanced.retainedItems;
  definition.maxNameReservations = config.advanced.maxNameReservations;
  definition.maxPendingInterests = config.advanced.maxPendingInterests;
  definition.signedWireCap = config.advanced.signedWireCap;
  definition.samplePeriodMs = config.samplePeriodMs;
  definition.sampleClasses = config.sampleClasses;
  definition.fec = config.fec;
  if (const auto error = definition.validate()) {
    throw std::invalid_argument("invalid StreamConfig: " + *error);
  }
  return definition;
}

} // namespace

namespace detail {

uint64_t
computePredictiveFutureCursorHorizon(uint64_t lookahead,
                                     uint64_t aggregateWindow)
{
  const auto capacity = std::min(lookahead, aggregateWindow);
  if (capacity <= 1) {
    return capacity;
  }
  // Keep half of the active scheduler capacity available for production and
  // callback jitter plus exact-name retries. This rule depends only on generic
  // scheduler capacity; retries are still admitted before new work below.
  return std::max<uint64_t>(1, capacity / 2);
}

} // namespace detail

struct StreamPublisher::SessionClaim
{
  SessionClaim(ndn::Name provider, ndn::Name dataPrefix, uint64_t epoch)
    : key(makeSessionKey(provider, dataPrefix, epoch))
  {
  }

  ~SessionClaim()
  {
    std::lock_guard<std::mutex> guard(g_sessionMutex);
    g_liveSessions.erase(key);
  }

  SessionKey key;
};

std::shared_ptr<StreamPublisher>
StreamPublisher::create(const StreamConfig& config, const ndn::Name& provider,
                        const LowLevelFactory& factory)
{
  if (!factory) {
    throw std::invalid_argument("Stream publisher factory is required");
  }
  const auto epoch = claimEpoch(provider, config.dataPrefix, config.sessionEpoch);
  auto claim = std::make_shared<SessionClaim>(provider, config.dataPrefix, epoch);
  const auto definition = deriveDefinition(config, provider, epoch);
  auto lowLevel = factory(definition);
  if (!lowLevel) {
    throw std::runtime_error("Core returned an empty LiveStream publisher");
  }
  return std::shared_ptr<StreamPublisher>(
    new StreamPublisher(config, definition, std::move(lowLevel), std::move(claim)));
}

StreamPublisher::StreamPublisher(
  StreamConfig config, LiveStreamDefinition definition,
  std::shared_ptr<LiveStreamPublisher> lowLevel,
  std::shared_ptr<SessionClaim> sessionClaim)
  : m_config(std::move(config))
  , m_definition(std::move(definition))
  , m_lowLevel(std::move(lowLevel))
  , m_sessionClaim(std::move(sessionClaim))
{
}

StreamPublisher::~StreamPublisher()
{
  try {
    stop();
  }
  catch (...) {
  }
}

void
StreamPublisher::requireStateLocked(State expected, const char* operation) const
{
  if (m_state == State::Failed) {
    throw std::runtime_error(
      std::string(operation) + " rejected after Stream failure: " +
      m_failureReason);
  }
  if (m_state == State::Stopped) {
    throw std::logic_error(std::string(operation) +
                           " rejected after Stream stop");
  }
  if (m_state != expected) {
    throw std::logic_error(std::string(operation) +
                           " is invalid in the current Stream state");
  }
}

void
StreamPublisher::failLocked(const std::string& reason)
{
  m_state = State::Failed;
  m_failureReason = reason.empty() ? "underlying Stream operation failed" : reason;
  if (m_lowLevel) {
    m_lowLevel->stop();
  }
}

PredictiveStreamDescriptor
StreamPublisher::start()
{
  std::lock_guard<std::mutex> guard(m_mutex);
  requireStateLocked(State::Created, "start");
  m_state = State::Bootstrapping;

  try {
    m_lowLevel->startPredictive();
    m_lowLevel->waitUntilReady(
      std::chrono::milliseconds(m_config.advanced.startupTimeoutMs));
    m_lowLevel->activatePredictive(m_config.samplePeriodMs);

    PredictiveStreamDescriptor descriptor;
    descriptor.definition = m_definition;
    descriptor.checkpoint = m_lowLevel->predictiveFrontier().checkpoint;
    descriptor.frontierName =
      makePredictiveFrontierName(descriptor.definition.mappingRoot());
    descriptor.measuredSamplePeriodMs = m_config.samplePeriodMs;
    if (const auto error = descriptor.validate()) {
      throw std::logic_error("invalid predictive descriptor: " + *error);
    }
    m_state = State::Active;
    NDN_LOG_INFO("STREAM_API_ACTIVE role=provider mode=predictive"
                 << " stream=" << m_definition.streamId
                 << " epoch=" << m_definition.sessionEpoch);
    return descriptor;
  }
  catch (const std::exception& error) {
    failLocked(error.what());
    throw;
  }
  catch (...) {
    failLocked("unknown predictive Stream startup failure");
    throw;
  }
}

void
StreamPublisher::push(std::shared_ptr<ndn::Data> signedData)
{
  std::lock_guard<std::mutex> guard(m_mutex);
  requireStateLocked(State::Active, "push");

  if (!signedData || signedData->getContent().value_size() == 0) {
    throw std::invalid_argument("pushed Data is empty");
  }
  const auto signatureType = signedData->getSignatureType();
  if ((signatureType != ndn::tlv::SignatureSha256WithRsa &&
       signatureType != ndn::tlv::SignatureSha256WithEcdsa) ||
      !signedData->getSignatureInfo().hasKeyLocator()) {
    throw std::invalid_argument(
      "pushed Data must have an RSA/ECDSA signature with KeyLocator");
  }

  const auto& name = signedData->getName();
  if (!m_definition.mappingRoot().isPrefixOf(name)) {
    throw std::invalid_argument(
      "pushed Data name is outside session prefix authority: " + name.toUri());
  }

  const auto wireSize = signedData->wireEncode().size();
  if (wireSize > m_config.advanced.signedWireCap ||
      wireSize > ndn::MAX_NDN_PACKET_SIZE) {
    throw std::length_error(
      "pushed Data exceeds wire budget: " + std::to_string(wireSize) + " > " +
      std::to_string(std::min(m_config.advanced.signedWireCap,
                               ndn::MAX_NDN_PACKET_SIZE)));
  }

  try {
    if (m_lowLevel->publishSignedData(signedData)) {
      const auto& pushedName = signedData->getName();
      const auto cursor =
        pushedName[pushedName.size() - 1].toSequenceNumber();
      const auto wire = signedData->wireEncode();
      const auto digest = computeStreamContentDigest(
        ndn::span<const uint8_t>(wire.begin(), wire.size()));
      NDN_LOG_INFO("STREAM_PUSH stream=" << m_definition.streamId
                   << " sequence=" << cursor
                   << " wire_sha256=" << toLowerHex(digest));
      m_pendingSegments.push_back(std::move(signedData));
    }
  }
  catch (...) {
    const auto lowLevelStatus = m_lowLevel->status();
    if (lowLevelStatus.state == LiveStreamLifecycleState::Failed) {
      m_state = State::Failed;
      m_failureReason = lowLevelStatus.reason.empty()
        ? "predictive source admission failed"
        : lowLevelStatus.reason;
    }
    throw;
  }
}

void
StreamPublisher::flush()
{
  std::lock_guard<std::mutex> guard(m_mutex);
  requireStateLocked(State::Active, "flush");

  if (m_pendingSegments.empty()) {
    return; // no-op
  }

  if (m_config.fec.enabled() &&
      m_pendingSegments.size() > m_config.fec.maxSourceItems) {
    throw std::length_error(
      "flush group exceeds configured FEC source capacity");
  }
  try {
    const auto sourceCount = m_pendingSegments.size();
    const auto repairCount = m_config.fec.enabled()
      ? m_config.fec.repairItemCount() : 0;
    m_lowLevel->commitPredictiveGroup(
      m_nextRepairGroup, m_pendingSegments);
    NDN_LOG_INFO("STREAM_FLUSH stream=" << m_definition.streamId
                 << " group=" << m_nextRepairGroup
                 << " sources=" << sourceCount
                 << " repairs=" << repairCount);
  }
  catch (...) {
    m_state = State::Failed;
    const auto lowLevelStatus = m_lowLevel->status();
    m_failureReason = lowLevelStatus.reason.empty()
      ? "predictive group commit failed"
      : lowLevelStatus.reason;
    m_lowLevel->stop();
    throw;
  }

  ++m_nextRepairGroup;
  m_pendingSegments.clear();
}

LiveStreamStatus
StreamPublisher::status() const
{
  std::lock_guard<std::mutex> guard(m_mutex);
  auto result = m_lowLevel->status();
  if (m_state == State::Failed) {
    result.state = LiveStreamLifecycleState::Failed;
    result.reason = m_failureReason;
  }
  else if (m_state == State::Stopped) {
    result.state = LiveStreamLifecycleState::Stopped;
    result.reason = "stopped";
  }
  return result;
}

void
StreamPublisher::stop()
{
  std::lock_guard<std::mutex> guard(m_mutex);
  if (m_state == State::Stopped) {
    return;
  }
  if (m_lowLevel) {
    m_lowLevel->stop();
  }
  m_pendingSegments.clear();
  m_sessionClaim.reset();
  m_state = State::Stopped;
  NDN_LOG_INFO("STREAM_API_STOP role=provider stream="
               << m_definition.streamId);
}

namespace detail {

LiveStreamOpenOptions
makeLiveStreamOpenOptions(const LiveStreamDescriptor& descriptor,
                          StreamSubscriptionOptions options)
{
  if (const auto error = descriptor.validate()) {
    throw std::invalid_argument("invalid LiveStream descriptor: " + *error);
  }
  if (!options.onItem || options.aggregateInterestLimit == 0 ||
      options.interestLifetimeMs == 0) {
    throw std::invalid_argument("invalid Stream subscription options");
  }

  LiveStreamOpenOptions lowLevel;
  lowLevel.start = options.start;
  lowLevel.prefetchPolicy = options.prefetchPolicy.value_or(
    descriptor.definition.contractVersion ==
        STREAM_NAME_MAP_CONTRACT_VERSION_V2
      ? LiveStreamPrefetchPolicy::AdaptiveSampleAtomic
      : LiveStreamPrefetchPolicy::MappedPressure);
  lowLevel.aggregateInterestLimit = options.aggregateInterestLimit;
  lowLevel.enableFecRecovery = options.enableFecRecovery;
  lowLevel.interestLifetimeMs = options.interestLifetimeMs;
  lowLevel.onItem = std::move(options.onItem);
  lowLevel.onStatus = std::move(options.onStatus);
  return lowLevel;
}

} // namespace detail

// ── PredictiveStreamSubscriber ──

PredictiveStreamSubscriber::PredictiveStreamSubscriber(
  ndn::Face& face,
  std::shared_ptr<MessageValidator> validator,
  PredictiveStreamDescriptor descriptor,
  StreamSubscriptionOptions options)
  : m_face(face)
  , m_validator(std::move(validator))
  , m_descriptor(std::move(descriptor))
  , m_options(std::move(options))
  , m_fetcher(std::make_unique<StreamAdaptiveFetcherState>())
{
  if (const auto error = m_descriptor.validate()) {
    throw std::invalid_argument("invalid predictive descriptor: " + *error);
  }
  if (!m_validator || !m_options.onItem ||
      m_options.aggregateInterestLimit == 0 ||
      m_options.interestLifetimeMs == 0) {
    throw std::invalid_argument("predictive subscriber requires onItem callback");
  }
  if (m_options.prefetchPolicy.has_value() &&
      *m_options.prefetchPolicy != LiveStreamPrefetchPolicy::AdaptiveSampleAtomic) {
    throw std::invalid_argument(
      "predictive subscriber only supports adaptive-sample-atomic prefetch");
  }
}

PredictiveStreamSubscriber::~PredictiveStreamSubscriber()
{
  stop();
}

void
PredictiveStreamSubscriber::start()
{
  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_state == LiveStreamLifecycleState::Active) {
      return;
    }
    if (m_state == LiveStreamLifecycleState::Stopped ||
        m_state == LiveStreamLifecycleState::Failed) {
      throw std::logic_error(
        "predictive subscriber cannot restart after stop/failure");
    }
    m_state = LiveStreamLifecycleState::Active;
    generation = m_generation;
  }
  if (m_options.start == LiveStreamStart::Latest) {
    fetchFrontier(generation);
  }
  else {
    installDescriptorCheckpointAndSchedule(generation);
  }
  NDN_LOG_INFO("STREAM_API_ACTIVE role=consumer mode=predictive"
               << " stream=" << m_descriptor.definition.streamId
               << " epoch=" << m_descriptor.definition.sessionEpoch);
}

void
PredictiveStreamSubscriber::fetchFrontier(uint64_t generation)
{
  ndn::Interest interest(m_descriptor.frontierName);
  interest.setCanBePrefix(false);
  interest.setMustBeFresh(true);
  interest.setInterestLifetime(
    ndn::time::milliseconds(m_options.interestLifetimeMs));
  const auto weak = weak_from_this();
  auto handle = m_face.expressInterest(
    interest,
    [weak, generation] (const ndn::Interest&, const ndn::Data& data) {
      const auto self = weak.lock();
      if (!self) {
        return;
      }
      {
        std::lock_guard<std::mutex> guard(self->m_mutex);
        if (!self->isActiveLocked(generation)) {
          return;
        }
        self->m_frontierInterest.reset();
      }
      self->m_validator->validateData(
        data,
        [weak, generation] (const ndn::Data& validated) {
          const auto subscriber = weak.lock();
          if (!subscriber) {
            return;
          }
          bool valid = false;
          PredictiveStreamFrontier frontier;
          try {
            auto content = validated.getContent();
            content.parse();
            valid =
              validated.getName() == subscriber->m_descriptor.frontierName &&
              validated.wireEncode().size() <=
                subscriber->m_descriptor.definition.signedWireCap &&
              subscriber->hasExpectedProviderSignature(validated) &&
              content.elements().size() == 1 &&
              frontier.wireDecode(content.elements().front()) &&
              !frontier.validate(subscriber->m_descriptor.definition);
          }
          catch (const std::exception&) {
            valid = false;
          }
          if (!valid) {
            {
              std::lock_guard<std::mutex> guard(subscriber->m_mutex);
              if (subscriber->isActiveLocked(generation)) {
                subscriber->m_state = LiveStreamLifecycleState::Failed;
                subscriber->m_reason = "invalid predictive frontier";
                ++subscriber->m_rejected;
              }
            }
            subscriber->emitStatus();
            return;
          }
          {
            std::lock_guard<std::mutex> guard(subscriber->m_mutex);
            if (!subscriber->isActiveLocked(generation)) {
              return;
            }
            subscriber->m_descriptor.checkpoint = frontier.checkpoint;
          }
          subscriber->installDescriptorCheckpointAndSchedule(generation);
        },
        [weak, generation] (const ndn::Data&,
                           const ndn::security::ValidationError&) {
          const auto subscriber = weak.lock();
          if (!subscriber) {
            return;
          }
          {
            std::lock_guard<std::mutex> guard(subscriber->m_mutex);
            if (!subscriber->isActiveLocked(generation)) {
              return;
            }
            subscriber->m_state = LiveStreamLifecycleState::Failed;
            subscriber->m_reason =
              "predictive frontier signature validation failed";
            ++subscriber->m_rejected;
          }
          subscriber->emitStatus();
        });
    },
    [weak, generation] (const ndn::Interest&, const ndn::lp::Nack&) {
      const auto self = weak.lock();
      if (!self) {
        return;
      }
      {
        std::lock_guard<std::mutex> guard(self->m_mutex);
        if (!self->isActiveLocked(generation)) {
          return;
        }
        self->m_frontierInterest.reset();
        ++self->m_nacks;
        self->m_state = LiveStreamLifecycleState::Failed;
        self->m_reason = "predictive frontier unavailable after Nack";
      }
      self->emitStatus();
    },
    [weak, generation] (const ndn::Interest&) {
      const auto self = weak.lock();
      if (!self) {
        return;
      }
      {
        std::lock_guard<std::mutex> guard(self->m_mutex);
        if (!self->isActiveLocked(generation)) {
          return;
        }
        self->m_frontierInterest.reset();
        ++self->m_timeouts;
        self->m_state = LiveStreamLifecycleState::Failed;
        self->m_reason = "predictive frontier unavailable after timeout";
      }
      self->emitStatus();
    });
  std::lock_guard<std::mutex> guard(m_mutex);
  if (isActiveLocked(generation) && !m_frontierInterest) {
    m_frontierInterest.emplace(std::move(handle));
  }
}

void
PredictiveStreamSubscriber::installDescriptorCheckpointAndSchedule(
  uint64_t generation)
{
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!isActiveLocked(generation)) {
      return;
    }
    const auto& checkpoint = m_descriptor.checkpoint;
    const bool hasProduced =
      checkpoint.nextExpectedSampleId > checkpoint.initialSampleId;
    const auto startCursor =
      m_options.start == LiveStreamStart::Latest && hasProduced
        ? checkpoint.latestProducedSampleId
        : checkpoint.initialSampleId;
    m_nextScheduleCursor = startCursor;
    m_nextDeliverCursor = startCursor;
    m_latestKnownProducedCursor = checkpoint.latestProducedSampleId;
    m_fetcher->aggregateInFlightLimit = m_options.aggregateInterestLimit;
    m_fetcher->resetLive(
      m_descriptor.definition.sessionEpoch, startCursor,
      m_descriptor.measuredSamplePeriodMs);
  }
  schedule();
  emitStatus();
}

void
PredictiveStreamSubscriber::schedule()
{
  std::vector<std::pair<uint64_t, bool>> cursors;
  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_state != LiveStreamLifecycleState::Active) {
      return;
    }
    generation = m_generation;
    const auto retransmissions = static_cast<uint64_t>(std::count_if(
      m_inFlight.begin(), m_inFlight.end(),
      [this] (uint64_t cursor) {
        const auto attempt = m_attempts.find(cursor);
        return attempt != m_attempts.end() && attempt->second > 1;
      }));
    m_fetcher->setInFlight(
      0, m_inFlight.size() - retransmissions, retransmissions);
    const auto decision = m_fetcher->decide();
    const auto limit = std::min<uint64_t>(
      m_options.aggregateInterestLimit,
      std::max<uint64_t>(1, decision.window));
    // Predictive exact names need no Mapping lookup. Permit the scheduler to
    // fill the adaptive pipeline, while bounding the name horizon by both the
    // controller's lookahead and the actual aggregate capacity. packetDemand
    // describes one measured sample; using it as the cursor horizon left most
    // of a safe window idle whenever the producer outran the consumer.
    const auto futureHorizon =
      detail::computePredictiveFutureCursorHorizon(decision.lookahead, limit);
    m_futureCursorHorizon = futureHorizon;
    const auto maxScheduleCursor =
      m_latestKnownProducedCursor >
          std::numeric_limits<uint64_t>::max() - futureHorizon
        ? std::numeric_limits<uint64_t>::max()
        : m_latestKnownProducedCursor + futureHorizon;
    while (m_inFlight.size() + m_processing.size() + m_scheduled.size() <
             limit &&
           !m_retryPending.empty()) {
      const auto pending = m_retryPending.begin();
      const auto cursor = *pending;
      m_retryPending.erase(pending);
      if (cursor < m_nextDeliverCursor ||
          m_admittedDigests.count(cursor) != 0 ||
          m_ready.count(cursor) != 0 ||
          m_terminalGaps.count(cursor) != 0) {
        continue;
      }
      m_scheduled.insert(cursor);
      cursors.emplace_back(cursor, true);
    }
    while (m_inFlight.size() + m_processing.size() + m_scheduled.size() <
           limit) {
      if (m_nextScheduleCursor > maxScheduleCursor) {
        break;
      }
      const auto cursor = m_nextScheduleCursor++;
      if (m_ready.count(cursor) != 0 ||
          m_terminalGaps.count(cursor) != 0) {
        continue;
      }
      // Reserve the budget before dropping the mutex. Validation/recovery
      // callbacks can call schedule() concurrently, so a function-local
      // cursors count cannot prevent another scheduler from allocating the
      // same aggregate capacity.
      m_scheduled.insert(cursor);
      cursors.emplace_back(cursor, false);
    }
  }
  for (const auto& [cursor, retry] : cursors) {
    fetchSegment(cursor, retry, generation);
  }
}

void
PredictiveStreamSubscriber::fetchSegment(
  uint64_t cursor, bool retry, uint64_t generation)
{
  const auto interestName =
    makePredictiveDataName(m_descriptor.definition, cursor);
  ndn::Interest interest(interestName);
  interest.setCanBePrefix(false);
  interest.setMustBeFresh(false);
  interest.setInterestLifetime(
    ndn::time::milliseconds(m_options.interestLifetimeMs));
  bool future = false;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!isActiveLocked(generation)) {
      m_scheduled.erase(cursor);
      return;
    }
    if (m_inFlight.count(cursor) != 0 ||
        m_processing.count(cursor) != 0) {
      m_scheduled.erase(cursor);
      return;
    }
    m_scheduled.erase(cursor);
    m_inFlight.insert(cursor);
    ++m_attempts[cursor];
    m_expressedAtMs[cursor] = streamNowMs();
    ++m_payloadInterests;
    if (retry) {
      ++m_retryAttempts;
      ++m_retryPayloadInterests;
    }
    else {
      ++m_initialPayloadInterests;
    }
    if (cursor > m_latestKnownProducedCursor) {
      future = true;
      m_futureRequested.insert(cursor);
      ++m_futurePayloadInterests;
      if (retry) {
        ++m_retryFuturePayloadInterests;
      }
      else {
        ++m_initialFuturePayloadInterests;
      }
    }
  }
  if (future) {
    NDN_LOG_INFO("STREAM_FUTURE_INTEREST stream="
                 << m_descriptor.definition.streamId
                 << " sequence=" << cursor);
  }
  const auto weak = weak_from_this();
  auto handle = m_face.expressInterest(
    interest,
    [weak, cursor, generation] (const ndn::Interest&, const ndn::Data& data) {
      if (const auto self = weak.lock()) {
        self->onData(data, cursor, generation);
      }
    },
    [weak, cursor, generation] (const ndn::Interest&,
                                const ndn::lp::Nack& nack) {
      if (const auto self = weak.lock()) {
        self->onNack(
          cursor, generation,
          std::to_string(static_cast<int>(nack.getReason())));
      }
    },
    [weak, cursor, generation] (const ndn::Interest&) {
      if (const auto self = weak.lock()) {
        self->onTimeout(cursor, generation);
      }
    });
  std::lock_guard<std::mutex> guard(m_mutex);
  if (isActiveLocked(generation) && m_inFlight.count(cursor) != 0) {
    m_pendingInterests.emplace(cursor, std::move(handle));
  }
}

void
PredictiveStreamSubscriber::onData(
  const ndn::Data& data, uint64_t cursor, uint64_t generation)
{
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!isActiveLocked(generation) ||
        m_inFlight.erase(cursor) == 0) {
      ++m_lateArrivals;
      return;
    }
    m_pendingInterests.erase(cursor);
    m_processing.insert(cursor);
  }
  const auto weak = weak_from_this();
  m_validator->validateData(
    data,
    [weak, cursor, generation] (const ndn::Data& validated) {
      if (const auto self = weak.lock()) {
        self->onValidatedData(validated, cursor, generation);
      }
    },
    [weak, cursor, generation] (
      const ndn::Data&, const ndn::security::ValidationError&) {
      if (const auto self = weak.lock()) {
        self->onValidationFailure(
          cursor, generation, "signature-validation-failed");
      }
    });
  schedule();
}

void
PredictiveStreamSubscriber::onNack(
  uint64_t cursor, uint64_t generation, const std::string& reason)
{
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!isActiveLocked(generation) ||
        m_inFlight.erase(cursor) == 0) {
      return;
    }
    m_pendingInterests.erase(cursor);
    ++m_nacks;
    m_fetcher->recordNack(cursor, reason);
  }
  retryOrDeclareGap(cursor, generation, true, reason);
}

void
PredictiveStreamSubscriber::onTimeout(
  uint64_t cursor, uint64_t generation)
{
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!isActiveLocked(generation) ||
        m_inFlight.erase(cursor) == 0) {
      return;
    }
    m_pendingInterests.erase(cursor);
    ++m_timeouts;
    m_fetcher->recordTimeout(
      cursor, cursor <= m_latestKnownProducedCursor,
      cursor > m_latestKnownProducedCursor);
  }
  retryOrDeclareGap(cursor, generation, true, "timeout");
}

void
PredictiveStreamSubscriber::onValidatedData(
  const ndn::Data& data, uint64_t cursor, uint64_t generation,
  LiveStreamItemProvenance provenance)
{
  bool invalid = false;
  try {
    invalid =
      data.getName() != makePredictiveDataName(
                          m_descriptor.definition, cursor) ||
      data.wireEncode().size() >
        m_descriptor.definition.signedWireCap ||
      !hasExpectedProviderSignature(data);
  }
  catch (const std::exception&) {
    invalid = true;
  }
  if (invalid) {
    onValidationFailure(
      cursor, generation, "predictive-source-contract-mismatch");
    return;
  }

  const auto wire = data.wireEncode();
  const auto digest = computeStreamContentDigest(
    ndn::span<const uint8_t>(wire.begin(), wire.size()));
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_processing.erase(cursor);
    if (!isActiveLocked(generation)) {
      ++m_lateArrivals;
      return;
    }
    const auto admitted = m_admittedDigests.find(cursor);
    if (admitted != m_admittedDigests.end()) {
      if (admitted->second != digest) {
        m_state = LiveStreamLifecycleState::Failed;
        m_reason = "predictive source equivocation";
        ++m_rejected;
      }
      else {
        m_fetcher->recordDuplicate();
      }
      return;
    }
    if (cursor < m_nextDeliverCursor) {
      ++m_lateArrivals;
      ++m_staleReadyDrops;
      return;
    }
    if (m_terminalGaps.erase(cursor) != 0) {
      ++m_terminalGapSuperseded;
    }
    m_admittedDigests.emplace(cursor, digest);
    // A successful exact retry or recovered Data completes the one bounded
    // FEC attempt for this cursor.  Clearing the marker here also keeps the
    // recovery-eligibility accounting bounded by live ready state.
    m_recoveryAttempted.erase(cursor);
    m_latestKnownProducedCursor =
      std::max(m_latestKnownProducedCursor, cursor);
    m_sourceWires[cursor] =
      std::vector<uint8_t>(wire.begin(), wire.end());
    while (m_sourceWires.size() >
           m_descriptor.definition.retainedItems) {
      m_sourceWires.erase(m_sourceWires.begin());
    }
    m_ready.emplace(
      cursor,
      std::make_pair(std::make_shared<ndn::Data>(data), provenance));
    if (provenance == LiveStreamItemProvenance::FecRecovered) {
      ++m_recovered;
    }
    if (m_attempts[cursor] > 1) {
      ++m_retrySuccesses;
    }
  }
  drainReady(generation);
  schedule();
}

void
PredictiveStreamSubscriber::onValidationFailure(
  uint64_t cursor, uint64_t generation, std::string reason)
{
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_processing.erase(cursor);
    if (!isActiveLocked(generation)) {
      return;
    }
    ++m_rejected;
  }
  retryOrDeclareGap(cursor, generation, true, std::move(reason));
}

void
PredictiveStreamSubscriber::retryOrDeclareGap(
  uint64_t cursor, uint64_t generation, bool allowRecovery,
  std::string reason)
{
  // Recovery is a bounded first-class attempt.  Once it has failed, the
  // cursor must spend its remaining finite attempts on exact source retries;
  // otherwise every timeout re-enters recovery and can starve ordered drain.
  if (allowRecovery && beginRecovery(cursor, generation)) {
    return;
  }
  bool retry = false;
  bool drain = false;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!isActiveLocked(generation)) {
      return;
    }
    constexpr size_t MAX_ATTEMPTS = 3;
    retry = m_attempts[cursor] < MAX_ATTEMPTS;
    if (retry) {
      m_retryPending.insert(cursor);
    }
    if (!retry) {
      if (m_admittedDigests.count(cursor) != 0 ||
          m_ready.count(cursor) != 0 ||
          cursor < m_nextDeliverCursor) {
        drain = true;
      }
      else {
        ++m_retryExhaustions;
        ++m_terminalMissingSources;
        m_terminalGaps.insert(cursor);
        m_recoveryAttempted.erase(cursor);
        m_futureRequested.erase(cursor);
        m_reason = "terminal-gap:" + reason;
        if (m_options.requireFullDelivery) {
          m_state = LiveStreamLifecycleState::Failed;
        }
        else {
          drain = true;
        }
      }
    }
  }
  if (!retry && drain) {
    drainReady(generation);
  }
  schedule();
  emitStatus();
}

bool
PredictiveStreamSubscriber::beginRecovery(
  uint64_t cursor, uint64_t generation)
{
  std::shared_ptr<const std::vector<ndn::Name>> cachedGroupNames;
  bool cachedFrontierApplies = false;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!isActiveLocked(generation) ||
        !m_options.enableFecRecovery ||
        !m_descriptor.definition.fec.enabled() ||
        cursor > m_latestKnownProducedCursor ||
        m_admittedDigests.count(cursor) != 0 ||
        m_ready.count(cursor) != 0 ||
        cursor < m_nextDeliverCursor ||
        m_recoveryAttempted.count(cursor) != 0 ||
        m_recoveryInProgress.count(cursor) != 0) {
      return false;
    }
    m_recoveryAttempted.insert(cursor);
    m_recoveryInProgress.insert(cursor);
    ++m_recoveryAttempts;
    if (m_recoveryFrontierGroupNames &&
        m_recoveryFrontierGroupFirstCursors &&
        m_recoveryFrontierGroupLastCursors &&
        !m_recoveryFrontierGroupNames->empty() &&
        cursor <= m_recoveryFrontierLatestProduced) {
      cachedFrontierApplies = true;
      for (size_t index = 0;
           index < m_recoveryFrontierGroupNames->size(); ++index) {
        if ((*m_recoveryFrontierGroupFirstCursors)[index] <= cursor &&
            cursor <= (*m_recoveryFrontierGroupLastCursors)[index]) {
          cachedGroupNames =
            std::make_shared<const std::vector<ndn::Name>>(
              std::vector<ndn::Name>{
                (*m_recoveryFrontierGroupNames)[index]});
          ++m_recoveryMetadataCacheHits;
          break;
        }
      }
    }
  }
  if (cachedGroupNames) {
    fetchRecoveryGroup(
      cursor, cachedGroupNames, cachedGroupNames->size() - 1, generation);
  }
  else if (cachedFrontierApplies) {
    finishRecoveryFailure(
      cursor, generation, "recovery-group-not-retained");
  }
  else {
    fetchRecoveryFrontier(cursor, generation);
  }
  return true;
}

void
PredictiveStreamSubscriber::fetchRecoveryFrontier(
  uint64_t cursor, uint64_t generation)
{
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!isActiveLocked(generation) ||
        m_recoveryInProgress.count(cursor) == 0) {
      return;
    }
    const auto inserted = m_recoveryFrontierWaiters.insert(cursor).second;
    if (!inserted) {
      return;
    }
    if (m_recoveryFrontierPending) {
      ++m_recoveryCoalescedWaiters;
      return;
    }
    m_recoveryFrontierPending = true;
    ++m_recoveryControlInterests;
    ++m_recoveryFrontierInterests;
  }

  ndn::Interest interest(m_descriptor.frontierName);
  interest.setCanBePrefix(false);
  interest.setMustBeFresh(true);
  interest.setInterestLifetime(
    ndn::time::milliseconds(m_options.interestLifetimeMs));
  const auto weak = weak_from_this();
  auto handle = m_face.expressInterest(
    interest,
    [weak, generation] (
      const ndn::Interest&, const ndn::Data& data) {
      const auto self = weak.lock();
      if (!self) {
        return;
      }
      self->m_validator->validateData(
        data,
        [weak, generation] (const ndn::Data& validated) {
          const auto subscriber = weak.lock();
          if (!subscriber) {
            return;
          }
          PredictiveStreamFrontier frontier;
          bool valid = false;
          try {
            auto content = validated.getContent();
            content.parse();
            valid =
              validated.wireEncode().size() <=
                subscriber->m_descriptor.definition.signedWireCap &&
              validated.getName() == subscriber->m_descriptor.frontierName &&
              subscriber->hasExpectedProviderSignature(validated) &&
              content.elements().size() == 1 &&
              frontier.wireDecode(content.elements().front()) &&
              !frontier.validate(subscriber->m_descriptor.definition);
          }
          catch (const std::exception&) {
            valid = false;
          }
          if (!valid) {
            std::vector<uint64_t> waiters;
            {
              std::lock_guard<std::mutex> guard(subscriber->m_mutex);
              if (!subscriber->isActiveLocked(generation)) {
                return;
              }
              waiters.assign(
                subscriber->m_recoveryFrontierWaiters.begin(),
                subscriber->m_recoveryFrontierWaiters.end());
              subscriber->m_recoveryFrontierWaiters.clear();
              subscriber->m_recoveryFrontierPending = false;
              subscriber->m_recoveryFrontierInterest.reset();
            }
            for (const auto waitingCursor : waiters) {
              subscriber->finishRecoveryFailure(
                waitingCursor, generation, "invalid-recovery-frontier");
            }
            return;
          }
          std::vector<uint64_t> waiters;
          auto names = std::make_shared<const std::vector<ndn::Name>>(
            frontier.retainedGroupCommitNames);
          auto firstCursors =
            std::make_shared<const std::vector<uint64_t>>(
              frontier.retainedGroupFirstCursors);
          auto lastCursors =
            std::make_shared<const std::vector<uint64_t>>(
              frontier.retainedGroupLastCursors);
          {
            std::lock_guard<std::mutex> guard(subscriber->m_mutex);
            if (!subscriber->isActiveLocked(generation)) {
              return;
            }
            subscriber->m_latestKnownProducedCursor =
              std::max(subscriber->m_latestKnownProducedCursor,
                       frontier.checkpoint.latestProducedSampleId);
            subscriber->m_recoveryFrontierGroupNames = names;
            subscriber->m_recoveryFrontierGroupFirstCursors = firstCursors;
            subscriber->m_recoveryFrontierGroupLastCursors = lastCursors;
            subscriber->m_recoveryFrontierLatestProduced =
              frontier.checkpoint.latestProducedSampleId;
            waiters.assign(
              subscriber->m_recoveryFrontierWaiters.begin(),
              subscriber->m_recoveryFrontierWaiters.end());
            subscriber->m_recoveryFrontierWaiters.clear();
            subscriber->m_recoveryFrontierPending = false;
            subscriber->m_recoveryFrontierInterest.reset();

            std::set<std::string> retained;
            for (const auto& name : *names) {
              retained.insert(name.toUri());
            }
            for (auto it = subscriber->m_recoveryGroupCache.begin();
                 it != subscriber->m_recoveryGroupCache.end();) {
              if (retained.count(it->first) == 0) {
                it = subscriber->m_recoveryGroupCache.erase(it);
              }
              else {
                ++it;
              }
            }
          }
          for (const auto waitingCursor : waiters) {
            std::shared_ptr<const std::vector<ndn::Name>> selected;
            for (size_t index = 0; index < names->size(); ++index) {
              if ((*firstCursors)[index] <= waitingCursor &&
                  waitingCursor <= (*lastCursors)[index]) {
                selected = std::make_shared<const std::vector<ndn::Name>>(
                  std::vector<ndn::Name>{(*names)[index]});
                break;
              }
            }
            if (selected) {
              subscriber->fetchRecoveryGroup(
                waitingCursor, selected, 0, generation);
            }
            else {
              subscriber->finishRecoveryFailure(
                waitingCursor, generation, "recovery-group-not-retained");
            }
          }
        },
        [weak, generation] (
          const ndn::Data&, const ndn::security::ValidationError&) {
          if (const auto subscriber = weak.lock()) {
            std::vector<uint64_t> waiters;
            {
              std::lock_guard<std::mutex> guard(subscriber->m_mutex);
              if (!subscriber->isActiveLocked(generation)) {
                return;
              }
              waiters.assign(
                subscriber->m_recoveryFrontierWaiters.begin(),
                subscriber->m_recoveryFrontierWaiters.end());
              subscriber->m_recoveryFrontierWaiters.clear();
              subscriber->m_recoveryFrontierPending = false;
              subscriber->m_recoveryFrontierInterest.reset();
            }
            for (const auto waitingCursor : waiters) {
              subscriber->finishRecoveryFailure(
                waitingCursor, generation,
                "recovery-frontier-validation-failed");
            }
          }
        });
    },
    [weak, generation] (
      const ndn::Interest&, const ndn::lp::Nack&) {
      if (const auto self = weak.lock()) {
        std::vector<uint64_t> waiters;
        {
          std::lock_guard<std::mutex> guard(self->m_mutex);
          if (!self->isActiveLocked(generation)) {
            return;
          }
          waiters.assign(
            self->m_recoveryFrontierWaiters.begin(),
            self->m_recoveryFrontierWaiters.end());
          self->m_recoveryFrontierWaiters.clear();
          self->m_recoveryFrontierPending = false;
          self->m_recoveryFrontierInterest.reset();
          ++self->m_nacks;
        }
        for (const auto waitingCursor : waiters) {
          self->finishRecoveryFailure(
            waitingCursor, generation, "recovery-frontier-nack");
        }
      }
    },
    [weak, generation] (const ndn::Interest&) {
      if (const auto self = weak.lock()) {
        std::vector<uint64_t> waiters;
        {
          std::lock_guard<std::mutex> guard(self->m_mutex);
          if (!self->isActiveLocked(generation)) {
            return;
          }
          waiters.assign(
            self->m_recoveryFrontierWaiters.begin(),
            self->m_recoveryFrontierWaiters.end());
          self->m_recoveryFrontierWaiters.clear();
          self->m_recoveryFrontierPending = false;
          self->m_recoveryFrontierInterest.reset();
          ++self->m_timeouts;
        }
        for (const auto waitingCursor : waiters) {
          self->finishRecoveryFailure(
            waitingCursor, generation, "recovery-frontier-timeout");
        }
      }
    });
  std::lock_guard<std::mutex> guard(m_mutex);
  if (isActiveLocked(generation) && m_recoveryFrontierPending) {
    m_recoveryFrontierInterest.emplace(std::move(handle));
  }
}

void
PredictiveStreamSubscriber::fetchRecoveryGroup(
  uint64_t cursor,
  std::shared_ptr<const std::vector<ndn::Name>> groupNames,
  size_t reverseIndex, uint64_t generation)
{
  const auto groupName = groupNames->at(reverseIndex);
  const auto cacheKey = groupName.toUri();
  std::optional<PredictiveStreamGroupCommit> cached;
  const auto interestKey = "recovery-group:" + cacheKey;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!isActiveLocked(generation) ||
        m_recoveryInProgress.count(cursor) == 0) {
      return;
    }
    const auto found = m_recoveryGroupCache.find(cacheKey);
    if (found != m_recoveryGroupCache.end()) {
      cached = found->second;
      ++m_recoveryMetadataCacheHits;
    }
    else {
      auto& waiters = m_recoveryGroupWaiters[cacheKey];
      waiters.push_back(
        RecoveryGroupWaiter{cursor, groupNames, reverseIndex, generation});
      if (waiters.size() > 1) {
        ++m_recoveryCoalescedWaiters;
        return;
      }
      ++m_recoveryControlInterests;
      ++m_recoveryGroupInterests;
    }
  }
  if (cached) {
    handleRecoveryGroup(
      cursor, std::move(groupNames), reverseIndex, groupName, *cached,
      generation);
    return;
  }

  ndn::Interest interest(groupName);
  interest.setCanBePrefix(false);
  interest.setMustBeFresh(false);
  interest.setInterestLifetime(
    ndn::time::milliseconds(m_options.interestLifetimeMs));
  const auto weak = weak_from_this();
  auto handle = m_face.expressInterest(
    interest,
    [weak, generation, cacheKey, interestKey, groupName] (
      const ndn::Interest&, const ndn::Data& data) {
      const auto self = weak.lock();
      if (!self) {
        return;
      }
      self->m_validator->validateData(
        data,
        [weak, generation, cacheKey, interestKey, groupName] (
          const ndn::Data& validated) {
          const auto subscriber = weak.lock();
          if (!subscriber) {
            return;
          }
          PredictiveStreamGroupCommit group;
          bool valid = false;
          try {
            auto content = validated.getContent();
            content.parse();
            valid =
              validated.wireEncode().size() <=
                subscriber->m_descriptor.definition.signedWireCap &&
              validated.getName() == groupName &&
              subscriber->hasExpectedProviderSignature(validated) &&
              content.elements().size() == 1 &&
              group.wireDecode(content.elements().front()) &&
              !group.validate(subscriber->m_descriptor.definition);
          }
          catch (const std::exception&) {
            valid = false;
          }
          std::vector<RecoveryGroupWaiter> waiters;
          {
            std::lock_guard<std::mutex> guard(subscriber->m_mutex);
            if (!subscriber->isActiveLocked(generation)) {
              return;
            }
            const auto found = subscriber->m_recoveryGroupWaiters.find(cacheKey);
            if (found != subscriber->m_recoveryGroupWaiters.end()) {
              waiters = std::move(found->second);
              subscriber->m_recoveryGroupWaiters.erase(found);
            }
            subscriber->m_controlInterests.erase(interestKey);
            if (valid) {
              subscriber->m_recoveryGroupCache[cacheKey] = group;
            }
          }
          if (!valid) {
            for (const auto& waiter : waiters) {
              subscriber->finishRecoveryFailure(
                waiter.cursor, waiter.generation, "invalid-recovery-group");
            }
            return;
          }
          for (const auto& waiter : waiters) {
            subscriber->handleRecoveryGroup(
              waiter.cursor, waiter.groupNames, waiter.reverseIndex,
              groupName, group, waiter.generation);
          }
        },
        [weak, generation, cacheKey, interestKey] (
          const ndn::Data&, const ndn::security::ValidationError&) {
          if (const auto subscriber = weak.lock()) {
            std::vector<RecoveryGroupWaiter> waiters;
            {
              std::lock_guard<std::mutex> guard(subscriber->m_mutex);
              if (!subscriber->isActiveLocked(generation)) {
                return;
              }
              const auto found =
                subscriber->m_recoveryGroupWaiters.find(cacheKey);
              if (found != subscriber->m_recoveryGroupWaiters.end()) {
                waiters = std::move(found->second);
                subscriber->m_recoveryGroupWaiters.erase(found);
              }
              subscriber->m_controlInterests.erase(interestKey);
            }
            for (const auto& waiter : waiters) {
              subscriber->finishRecoveryFailure(
                waiter.cursor, waiter.generation,
                "recovery-group-validation-failed");
            }
          }
        });
    },
    [weak, generation, cacheKey, interestKey] (
      const ndn::Interest&, const ndn::lp::Nack&) {
      if (const auto self = weak.lock()) {
        std::vector<RecoveryGroupWaiter> waiters;
        {
          std::lock_guard<std::mutex> guard(self->m_mutex);
          if (!self->isActiveLocked(generation)) {
            return;
          }
          const auto found = self->m_recoveryGroupWaiters.find(cacheKey);
          if (found != self->m_recoveryGroupWaiters.end()) {
            waiters = std::move(found->second);
            self->m_recoveryGroupWaiters.erase(found);
          }
          self->m_controlInterests.erase(interestKey);
          ++self->m_nacks;
        }
        for (const auto& waiter : waiters) {
          self->finishRecoveryFailure(
            waiter.cursor, waiter.generation, "recovery-group-nack");
        }
      }
    },
    [weak, generation, cacheKey, interestKey] (const ndn::Interest&) {
      if (const auto self = weak.lock()) {
        std::vector<RecoveryGroupWaiter> waiters;
        {
          std::lock_guard<std::mutex> guard(self->m_mutex);
          if (!self->isActiveLocked(generation)) {
            return;
          }
          const auto found = self->m_recoveryGroupWaiters.find(cacheKey);
          if (found != self->m_recoveryGroupWaiters.end()) {
            waiters = std::move(found->second);
            self->m_recoveryGroupWaiters.erase(found);
          }
          self->m_controlInterests.erase(interestKey);
          ++self->m_timeouts;
        }
        for (const auto& waiter : waiters) {
          self->finishRecoveryFailure(
            waiter.cursor, waiter.generation, "recovery-group-timeout");
        }
      }
    });
  std::lock_guard<std::mutex> guard(m_mutex);
  if (isActiveLocked(generation) &&
      m_recoveryGroupWaiters.count(cacheKey) != 0) {
    m_controlInterests.emplace(interestKey, std::move(handle));
  }
}

void
PredictiveStreamSubscriber::handleRecoveryGroup(
  uint64_t cursor,
  std::shared_ptr<const std::vector<ndn::Name>> groupNames,
  size_t reverseIndex, const ndn::Name&,
  const PredictiveStreamGroupCommit& group, uint64_t generation)
{
  const auto sourceName =
    makePredictiveDataName(m_descriptor.definition, cursor);
  if (std::find(group.sourceNames.begin(), group.sourceNames.end(),
                sourceName) == group.sourceNames.end()) {
    if (reverseIndex == 0) {
      finishRecoveryFailure(
        cursor, generation, "recovery-group-not-found");
    }
    else {
      fetchRecoveryGroup(
        cursor, std::move(groupNames), reverseIndex - 1, generation);
    }
    return;
  }
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!isActiveLocked(generation) ||
        m_recoveryInProgress.count(cursor) == 0) {
      return;
    }
    m_recoveryGroups[group.groupId] = group;
    ++m_recoverableGroups;
  }
  fetchRecoveryRepairs(cursor, group, generation);
}

void
PredictiveStreamSubscriber::fetchRecoveryRepairs(
  uint64_t cursor, PredictiveStreamGroupCommit group,
  uint64_t generation)
{
  if (group.repairNames.empty()) {
    finishRecoveryFailure(cursor, generation, "group-has-no-repair");
    return;
  }
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!isActiveLocked(generation) ||
        m_recoveryInProgress.count(cursor) == 0) {
      return;
    }
    // Recovery state is owned by the missing cursor, not merely by the FEC
    // group. Multiple lost cursors may enter recovery for the same group at
    // the same time; a group-only key lets one attempt overwrite another's
    // pending count and can leave a cursor permanently in recoveryInProgress.
    m_repairResponsesPending[cursor] = group.repairNames.size();
  }
  for (size_t index = 0; index < group.repairNames.size(); ++index) {
    const auto repairName = group.repairNames[index];
    ndn::Interest interest(repairName);
    interest.setCanBePrefix(false);
    interest.setMustBeFresh(false);
    interest.setInterestLifetime(
      ndn::time::milliseconds(m_options.interestLifetimeMs));
    const auto key = "repair:" + std::to_string(cursor) + ":" +
                     std::to_string(index);
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      ++m_payloadInterests;
      ++m_payloadRepairInterests;
    }
    const auto weak = weak_from_this();
    auto handle = m_face.expressInterest(
      interest,
      [weak, cursor, generation, key, group, repairName] (
        const ndn::Interest&, const ndn::Data& data) {
        const auto self = weak.lock();
        if (!self) {
          return;
        }
        {
          std::lock_guard<std::mutex> guard(self->m_mutex);
          self->m_controlInterests.erase(key);
          if (!self->isActiveLocked(generation) ||
              self->m_recoveryInProgress.count(cursor) == 0) {
            return;
          }
        }
        self->m_validator->validateData(
          data,
          [weak, cursor, generation, group, repairName] (
            const ndn::Data& validated) {
            const auto subscriber = weak.lock();
            if (!subscriber) {
              return;
            }
            LiveStreamFecRepair repair;
            bool valid = false;
            try {
              auto content = validated.getContent();
              content.parse();
              valid =
                validated.wireEncode().size() <=
                  subscriber->m_descriptor.definition.signedWireCap &&
                validated.getName() == repairName &&
                subscriber->hasExpectedProviderSignature(validated) &&
                content.elements().size() == 1 &&
                repair.wireDecode(content.elements().front()) &&
                repair.validate(subscriber->m_descriptor.definition).empty();
            }
            catch (const std::exception&) {
              valid = false;
            }
            if (!valid) {
              subscriber->finishRecoveryFailure(
                cursor, generation, "invalid-repair-data");
              return;
            }
            bool complete = false;
            {
              std::lock_guard<std::mutex> guard(subscriber->m_mutex);
              if (!subscriber->isActiveLocked(generation) ||
                  subscriber->m_recoveryInProgress.count(cursor) == 0) {
                return;
              }
              ++subscriber->m_payloadRepairDataResponses;
              auto& repairs =
                subscriber->m_recoveryRepairs[cursor];
              repairs.push_back(std::move(repair));
              auto pending =
                subscriber->m_repairResponsesPending.find(cursor);
              if (pending != subscriber->m_repairResponsesPending.end() &&
                  pending->second > 0) {
                complete = --pending->second == 0;
              }
            }
            if (complete) {
              subscriber->attemptRecovery(cursor, group, generation);
            }
          },
          [weak, cursor, generation] (
            const ndn::Data&, const ndn::security::ValidationError&) {
            if (const auto subscriber = weak.lock()) {
              subscriber->finishRecoveryFailure(
                cursor, generation, "repair-validation-failed");
            }
          });
      },
      [weak, cursor, generation, key] (
        const ndn::Interest&, const ndn::lp::Nack&) {
        if (const auto self = weak.lock()) {
          {
            std::lock_guard<std::mutex> guard(self->m_mutex);
            self->m_controlInterests.erase(key);
            ++self->m_nacks;
          }
          self->finishRecoveryFailure(
            cursor, generation, "repair-nack");
        }
      },
      [weak, cursor, generation, key] (const ndn::Interest&) {
        if (const auto self = weak.lock()) {
          {
            std::lock_guard<std::mutex> guard(self->m_mutex);
            self->m_controlInterests.erase(key);
            ++self->m_timeouts;
          }
          self->finishRecoveryFailure(
            cursor, generation, "repair-timeout");
        }
      });
    std::lock_guard<std::mutex> guard(m_mutex);
    if (isActiveLocked(generation) &&
        m_recoveryInProgress.count(cursor) != 0) {
      m_controlInterests.emplace(key, std::move(handle));
    }
  }
}

void
PredictiveStreamSubscriber::attemptRecovery(
  uint64_t cursor, const PredictiveStreamGroupCommit& group,
  uint64_t generation)
{
  std::vector<LiveStreamFecRepair> repairs;
  std::vector<std::optional<std::vector<uint8_t>>> sources;
  size_t missing = 0;
  size_t targetIndex = group.sourceNames.size();
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!isActiveLocked(generation) ||
        m_recoveryInProgress.count(cursor) == 0) {
      return;
    }
    repairs = m_recoveryRepairs[cursor];
    for (size_t index = 0; index < group.sourceNames.size(); ++index) {
      const auto sequence =
        group.sourceNames[index][group.sourceNames[index].size() - 1]
          .toSequenceNumber();
      if (sequence == cursor) {
        targetIndex = index;
      }
      const auto source = m_sourceWires.find(sequence);
      if (source == m_sourceWires.end()) {
        sources.push_back(std::nullopt);
        ++missing;
      }
      else {
        sources.push_back(source->second);
      }
    }
  }
  if (targetIndex >= group.sourceNames.size() ||
      missing == 0 || missing > group.recoveryCapacity ||
      repairs.size() < missing) {
    finishRecoveryFailure(
      cursor, generation, "insufficient-repair-capacity");
    return;
  }
  const auto recovered = recoverLiveStreamSources(
    m_descriptor.definition, repairs, sources, streamNowMs());
  if (!recovered || !recovered->at(targetIndex)) {
    finishRecoveryFailure(
      cursor, generation, "repair-decoding-failed");
    return;
  }
  ndn::Data recoveredData;
  try {
    const auto& wire = *recovered->at(targetIndex);
    recoveredData.wireDecode(ndn::Block(
      ndn::span<const uint8_t>(wire.data(), wire.size())));
  }
  catch (const std::exception&) {
    finishRecoveryFailure(
      cursor, generation, "recovered-wire-malformed");
    return;
  }
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!isActiveLocked(generation) ||
        m_recoveryInProgress.erase(cursor) == 0) {
      return;
    }
    m_processing.insert(cursor);
    ++m_recoveredGroups;
    m_payloadRepairDataConsumed += missing;
    m_repairResponsesPending.erase(cursor);
    m_recoveryRepairs.erase(cursor);
  }
  const auto weak = weak_from_this();
  m_validator->validateData(
    recoveredData,
    [weak, cursor, generation] (const ndn::Data& validated) {
      if (const auto self = weak.lock()) {
        self->onValidatedData(
          validated, cursor, generation,
          LiveStreamItemProvenance::FecRecovered);
      }
    },
    [weak, cursor, generation] (
      const ndn::Data&, const ndn::security::ValidationError&) {
      if (const auto self = weak.lock()) {
        self->onValidationFailure(
          cursor, generation, "recovered-source-validation-failed");
      }
    });
}

void
PredictiveStreamSubscriber::finishRecoveryFailure(
  uint64_t cursor, uint64_t generation, std::string reason)
{
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!isActiveLocked(generation) ||
        m_recoveryInProgress.erase(cursor) == 0) {
      return;
    }
    m_repairResponsesPending.erase(cursor);
    m_recoveryRepairs.erase(cursor);
    ++m_recoveryExhaustions;
  }
  retryOrDeclareGap(cursor, generation, false, std::move(reason));
}

void
PredictiveStreamSubscriber::drainReady(uint64_t generation)
{
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!isActiveLocked(generation)) {
      return;
    }
    ++m_drainWakeCount;
    if (m_draining) {
      m_drainWakePending = true;
      return;
    }
    m_draining = true;
    m_drainWakePending = false;
  }
  while (true) {
    std::shared_ptr<ndn::Data> data;
    LiveStreamItemProvenance provenance =
      LiveStreamItemProvenance::SignedData;
    uint64_t cursor = 0;
    uint64_t expressedAt = 0;
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      if (!isActiveLocked(generation)) {
        m_draining = false;
        return;
      }
      while (m_terminalGaps.erase(m_nextDeliverCursor) != 0) {
        ++m_nextDeliverCursor;
        m_fetcher->advanceNextCursor(m_nextDeliverCursor);
      }
      const auto ready = m_ready.find(m_nextDeliverCursor);
      if (ready == m_ready.end()) {
        if (m_drainWakePending) {
          m_drainWakePending = false;
          continue;
        }
        m_draining = false;
        break;
      }
      cursor = ready->first;
      data = std::move(ready->second.first);
      provenance = ready->second.second;
      m_ready.erase(ready);
      const auto expressed = m_expressedAtMs.find(cursor);
      if (expressed != m_expressedAtMs.end()) {
        expressedAt = expressed->second;
      }
    }

    VerifiedLiveStreamItem item;
    item.cursor = cursor;
    item.originalName = data->getName();
    item.verifiedProvider = m_descriptor.definition.provider;
    item.content.assign(data->getContent().value_begin(),
                        data->getContent().value_end());
    item.provenance = provenance;
    item.receivedMs = streamNowMs();
    LiveStreamItemAdmission admission;
    try {
      admission = m_options.onItem(item);
    }
    catch (const std::exception& error) {
      admission = LiveStreamItemAdmission::rejectItem(error.what());
    }
    catch (...) {
      admission =
        LiveStreamItemAdmission::rejectItem("application callback failed");
    }

    {
      std::lock_guard<std::mutex> guard(m_mutex);
      if (!isActiveLocked(generation)) {
        m_draining = false;
        return;
      }
      if (admission.accepted) {
        ++m_delivered;
        ++m_payloadSourceDataAdmissions;
        NDN_LOG_INFO("STREAM_ITEM_ADMITTED stream="
                     << m_descriptor.definition.streamId
                     << " sequence=" << cursor
                     << " provenance=" << toString(provenance)
                     << " wire_sha256="
                     << toLowerHex(m_admittedDigests.at(cursor)));
        const auto delay = expressedAt == 0 || item.receivedMs < expressedAt
          ? 0.0
          : static_cast<double>(item.receivedMs - expressedAt);
        const bool wasFuture = m_futureRequested.erase(cursor) != 0;
        m_fetcher->observePayloadDelay(delay, wasFuture);
        m_fetcher->observeAcceptedSample(
          m_descriptor.definition.sessionEpoch, cursor, item.receivedMs,
          delay, 1, !wasFuture);
      }
      else {
        ++m_rejected;
        m_reason = admission.reason.empty()
          ? "application rejected predictive item"
          : admission.reason;
      }
      ++m_nextDeliverCursor;
      m_fetcher->advanceNextCursor(m_nextDeliverCursor);
      m_expressedAtMs.erase(cursor);
    }
  }
  schedule();
  emitStatus();
}

bool
PredictiveStreamSubscriber::hasExpectedProviderSignature(
  const ndn::Data& data) const
{
  try {
    if (!data.getSignatureInfo().hasKeyLocator()) {
      return false;
    }
    const auto locator = data.getSignatureInfo().getKeyLocator();
    if (locator.getType() != ndn::tlv::Name) {
      return false;
    }
    const auto& name = locator.getName();
    const auto signer = ndn::security::Certificate::isValidName(name)
      ? ndn::security::extractIdentityFromCertName(name)
      : ndn::security::isValidKeyName(name)
          ? ndn::security::extractIdentityFromKeyName(name)
          : ndn::Name();
    return signer == m_descriptor.definition.provider;
  }
  catch (const std::exception&) {
    return false;
  }
}

bool
PredictiveStreamSubscriber::isActiveLocked(uint64_t generation) const
{
  return m_state == LiveStreamLifecycleState::Active &&
         m_generation == generation;
}

LiveStreamStatus
PredictiveStreamSubscriber::status() const
{
  std::lock_guard<std::mutex> guard(m_mutex);
  LiveStreamStatus result;
  result.state = m_state;
  result.frontiers.oldestRetained =
    m_descriptor.checkpoint.oldestRetainedSampleId;
  result.frontiers.latestProduced = m_latestKnownProducedCursor;
  result.frontiers.nextReserved = m_nextScheduleCursor;
  result.delivered = m_delivered;
  result.rejected = m_rejected;
  result.recovered = m_recovered;
  result.timeouts = m_timeouts;
  result.nacks = m_nacks;
  result.retryAttempts = m_retryAttempts;
  result.retrySuccesses = m_retrySuccesses;
  result.retryExhaustions = m_retryExhaustions;
  result.lateArrivals = m_lateArrivals;
  result.terminalMissingSources = m_terminalMissingSources;
  result.declaredRecoveryCapacity =
    m_descriptor.definition.fec.recoveryCapacity();
  result.recoveryEligibleSources = m_recoveryAttempted.size();
  result.recoverableGroups = m_recoverableGroups;
  result.recoveredGroups = m_recoveredGroups;
  result.recoveryAttempts = m_recoveryAttempts;
  result.recoveryExhaustions = m_recoveryExhaustions;
  result.recoveryControlInterests = m_recoveryControlInterests;
  result.recoveryFrontierInterests = m_recoveryFrontierInterests;
  result.recoveryGroupInterests = m_recoveryGroupInterests;
  result.recoveryCoalescedWaiters = m_recoveryCoalescedWaiters;
  result.recoveryMetadataCacheHits = m_recoveryMetadataCacheHits;
  result.nextDeliverCursor = m_nextDeliverCursor;
  result.readyQueueDepth = m_ready.size();
  result.oldestReadyCursor =
    m_ready.empty() ? m_nextDeliverCursor : m_ready.begin()->first;
  result.terminalGapQueueDepth = m_terminalGaps.size();
  result.drainWakeCount = m_drainWakeCount;
  result.staleReadyDrops = m_staleReadyDrops;
  result.terminalGapSuperseded = m_terminalGapSuperseded;
  result.mappingInterests = m_mappingInterests;
  result.mappingDataResponses = m_mappingDataResponses;
  result.mappingNewDataResponses = m_mappingNewDataResponses;
  result.payloadInterests = m_payloadInterests;
  result.initialPayloadInterests = m_initialPayloadInterests;
  result.retryPayloadInterests = m_retryPayloadInterests;
  result.payloadSourceInterests = m_payloadInterests;
  result.initialPayloadSourceInterests = m_initialPayloadInterests;
  result.retryPayloadSourceInterests = m_retryPayloadInterests;
  result.payloadRepairInterests = m_payloadRepairInterests;
  result.initialPayloadRepairInterests = m_payloadRepairInterests;
  result.payloadRepairDataResponses = m_payloadRepairDataResponses;
  result.payloadRepairDataConsumed = m_payloadRepairDataConsumed;
  result.futurePayloadInterests = m_futurePayloadInterests;
  result.initialFuturePayloadInterests = m_initialFuturePayloadInterests;
  result.retryFuturePayloadInterests = m_retryFuturePayloadInterests;
  result.futureCursorHorizon = m_futureCursorHorizon;
  result.payloadSourceDataAdmissions = m_payloadSourceDataAdmissions;
  result.payloadApplicationUsefulInterests =
    m_payloadSourceDataAdmissions;
  result.payloadProtectionOnlyInterests =
    m_payloadRepairDataResponses - std::min(
      m_payloadRepairDataResponses, m_payloadRepairDataConsumed);
  result.payloadNonproductiveInterests =
    m_terminalMissingSources;
  result.payloadUnresolvedInterests =
    m_inFlight.size() + m_processing.size();
  result.inFlight = m_inFlight.size() + m_processing.size();
  result.reason = m_reason;
  result.fetchDecision =
    std::make_shared<StreamFetchDecision>(m_fetcher->decide());
  return result;
}

void
PredictiveStreamSubscriber::emitStatus() const
{
  if (m_options.onStatus) {
    try {
      m_options.onStatus(status());
    }
    catch (...) {
      // Observability callbacks must not unwind the Face event loop.
    }
  }
}

void
PredictiveStreamSubscriber::stop()
{
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_state == LiveStreamLifecycleState::Stopped) {
      return;
    }
    ++m_generation;
    m_state = LiveStreamLifecycleState::Stopped;
    m_reason = "stopped";
    m_frontierInterest.reset();
    m_recoveryFrontierInterest.reset();
    m_recoveryFrontierPending = false;
    m_recoveryFrontierWaiters.clear();
    m_recoveryFrontierGroupNames.reset();
    m_recoveryFrontierGroupFirstCursors.reset();
    m_recoveryFrontierGroupLastCursors.reset();
    m_recoveryFrontierLatestProduced = 0;
    m_recoveryGroupWaiters.clear();
    m_recoveryGroupCache.clear();
    m_recoveryGroups.clear();
    m_recoveryRepairs.clear();
    m_repairResponsesPending.clear();
    m_recoveryInProgress.clear();
    m_pendingInterests.clear();
    m_controlInterests.clear();
    m_scheduled.clear();
    m_retryPending.clear();
    m_inFlight.clear();
    m_processing.clear();
    m_ready.clear();
    m_futureRequested.clear();
    m_draining = false;
    m_drainWakePending = false;
    m_fetcher->stopLive();
  }
  emitStatus();
  NDN_LOG_INFO("STREAM_API_STOP role=consumer stream="
               << m_descriptor.definition.streamId);
}

} // namespace ndn_service_framework
