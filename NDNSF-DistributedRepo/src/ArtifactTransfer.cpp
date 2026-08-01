#include "ndnsf-distributed-repo/ArtifactTransfer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ndnsf_distributed_repo {

void
AdaptiveTransferOptions::validate() const
{
  if (minimumWindow == 0 || initialWindow < minimumWindow ||
      initialWindow > maximumWindow || maximumWindow == 0 ||
      verificationBacklogLimit == 0 || segmentTimeoutMs == 0) {
    throw std::invalid_argument(
      "repo-transfer-invalid-options: invalid window, backlog, or timeout bound");
  }
}

AdaptiveArtifactTransfer::AdaptiveArtifactTransfer(
  uint64_t totalSegments, AdaptiveTransferOptions options)
  : m_totalSegments(totalSegments)
  , m_options(std::move(options))
  , m_window(m_options.initialWindow)
{
  m_options.validate();
  if (m_totalSegments > (uint64_t{1} << 48)) {
    throw std::invalid_argument(
      "repo-transfer-invalid-segment-count: segment count exceeds safety bound");
  }
  m_metrics.totalSegments = m_totalSegments;
  m_metrics.congestionWindow = m_window;
  m_metrics.complete = m_totalSegments == 0;
}

bool
AdaptiveArtifactTransfer::canIssue() const
{
  return !m_metrics.complete && !m_metrics.failed &&
         m_inFlight.size() < static_cast<size_t>(std::floor(m_window)) &&
         m_inFlight.size() + m_received.size() <
           m_options.verificationBacklogLimit;
}

std::vector<ArtifactSegmentRequest>
AdaptiveArtifactTransfer::poll(uint64_t nowMs, size_t maximumRequests)
{
  std::vector<ArtifactSegmentRequest> requests;
  while (requests.size() < maximumRequests && canIssue()) {
    ArtifactSegmentRequest request;
    if (!m_retryQueue.empty()) {
      const auto item = m_retryQueue.begin();
      request.segmentNo = item->first;
      request.attempt = item->second;
      request.retransmission = true;
      m_retryQueue.erase(item);
    }
    else {
      if (m_nextSegment >= m_totalSegments) {
        break;
      }
      request.segmentNo = m_nextSegment++;
      request.attempt = 1;
      request.retransmission = false;
    }
    m_inFlight.emplace(
      request.segmentNo,
      InFlight{nowMs, request.attempt, request.retransmission});
    ++m_metrics.interestCount;
    if (request.retransmission) {
      ++m_metrics.retransmissionCount;
    }
    requests.push_back(request);
  }
  return requests;
}

ArtifactSegmentDisposition
AdaptiveArtifactTransfer::receive(uint64_t segmentNo, uint64_t logicalBytes,
                                  uint64_t wireBytes, uint64_t)
{
  if (segmentNo >= m_totalSegments) {
    ++m_metrics.rejectedCount;
    return ArtifactSegmentDisposition::Unsolicited;
  }
  if (m_received.count(segmentNo) != 0 || m_verified.count(segmentNo) != 0) {
    ++m_metrics.duplicateCount;
    return ArtifactSegmentDisposition::Duplicate;
  }
  auto inFlight = m_inFlight.find(segmentNo);
  const bool wasIssued =
    inFlight != m_inFlight.end() || m_retryQueue.count(segmentNo) != 0 ||
    segmentNo < m_nextSegment;
  if (!wasIssued) {
    ++m_metrics.rejectedCount;
    return ArtifactSegmentDisposition::Unsolicited;
  }
  bool wasRetransmission = false;
  if (inFlight != m_inFlight.end()) {
    wasRetransmission = inFlight->second.retransmission;
    m_inFlight.erase(inFlight);
  }
  m_retryQueue.erase(segmentNo);
  m_received.insert(segmentNo);
  m_metrics.logicalBytes += logicalBytes;
  m_metrics.wireBytes += wireBytes;
  if (wasRetransmission) {
    m_metrics.retransmittedBytes += wireBytes;
  }
  m_window = std::min<double>(
    m_options.maximumWindow, m_window + 1.0 / std::max(1.0, m_window));
  m_metrics.congestionWindow = m_window;
  return ArtifactSegmentDisposition::Accepted;
}

void
AdaptiveArtifactTransfer::markVerified(uint64_t segmentNo)
{
  const auto received = m_received.find(segmentNo);
  if (received == m_received.end()) {
    if (m_verified.count(segmentNo) != 0) {
      return;
    }
    throw std::invalid_argument(
      "repo-transfer-verify-before-receive: segment is not pending verification");
  }
  m_received.erase(received);
  m_verified.insert(segmentNo);
  m_metrics.verifiedSegments = m_verified.size();
  m_metrics.complete = m_verified.size() == m_totalSegments;
}

void
AdaptiveArtifactTransfer::reject(uint64_t segmentNo, const std::string& reason)
{
  m_received.erase(segmentNo);
  m_inFlight.erase(segmentNo);
  m_retryQueue.erase(segmentNo);
  ++m_metrics.rejectedCount;
  fail("repo-transfer-segment-rejected: " + reason);
}

void
AdaptiveArtifactTransfer::queueRetransmission(uint64_t segmentNo,
                                              uint32_t nextAttempt)
{
  const uint32_t retriesUsed = nextAttempt - 1;
  if (retriesUsed > m_options.maximumRetries) {
    fail("repo-transfer-retry-exhausted: segment " + std::to_string(segmentNo));
    return;
  }
  m_retryQueue[segmentNo] = nextAttempt;
}

void
AdaptiveArtifactTransfer::onCongestion()
{
  m_window = std::max<double>(
    m_options.minimumWindow, std::floor(m_window / 2.0));
  m_metrics.congestionWindow = m_window;
}

void
AdaptiveArtifactTransfer::expire(uint64_t nowMs)
{
  std::vector<std::pair<uint64_t, uint32_t>> expired;
  for (const auto& [segmentNo, state] : m_inFlight) {
    if (nowMs >= state.sentAtMs &&
        nowMs - state.sentAtMs >= m_options.segmentTimeoutMs) {
      expired.emplace_back(segmentNo, state.attempt + 1);
    }
  }
  if (expired.empty()) {
    return;
  }
  onCongestion();
  for (const auto& [segmentNo, nextAttempt] : expired) {
    m_inFlight.erase(segmentNo);
    ++m_metrics.timeoutCount;
    queueRetransmission(segmentNo, nextAttempt);
  }
}

void
AdaptiveArtifactTransfer::fail(const std::string& reason)
{
  m_metrics.failed = true;
  m_metrics.failureReason = reason.empty()
                              ? "repo-transfer-failed: unspecified failure"
                              : reason;
  m_inFlight.clear();
  m_retryQueue.clear();
}

ArtifactTransferSnapshot
AdaptiveArtifactTransfer::snapshot() const
{
  auto result = m_metrics;
  result.inFlightSegments = m_inFlight.size();
  result.verificationBacklog = m_received.size();
  result.verifiedSegments = m_verified.size();
  result.complete = m_verified.size() == m_totalSegments && !result.failed;
  return result;
}

std::vector<uint64_t>
AdaptiveArtifactTransfer::missingSegments() const
{
  std::vector<uint64_t> missing;
  if (m_totalSegments > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    throw std::overflow_error(
      "repo-transfer-segment-count-overflow: cannot materialize missing list");
  }
  missing.reserve(static_cast<size_t>(m_totalSegments - m_verified.size()));
  for (uint64_t segmentNo = 0; segmentNo < m_totalSegments; ++segmentNo) {
    if (m_verified.count(segmentNo) == 0) {
      missing.push_back(segmentNo);
    }
  }
  return missing;
}

namespace {

bool
sameArtifactReference(const ArtifactReference& lhs,
                      const ArtifactReference& rhs)
{
  return lhs.logicalName == rhs.logicalName &&
         lhs.digestAlgorithm == rhs.digestAlgorithm &&
         lhs.contentDigest == rhs.contentDigest &&
         lhs.sizeBytes == rhs.sizeBytes &&
         lhs.formatVersion == rhs.formatVersion &&
         lhs.rootManifestName == rhs.rootManifestName &&
         lhs.publisherIdentity == rhs.publisherIdentity &&
         lhs.policyEpoch == rhs.policyEpoch;
}

uint64_t
checkedAdd(uint64_t lhs, uint64_t rhs, const char* message)
{
  if (rhs > std::numeric_limits<uint64_t>::max() - lhs) {
    throw std::overflow_error(message);
  }
  return lhs + rhs;
}

} // namespace

void
ArtifactResumeIdentity::validate(const ArtifactLimits& limits) const
{
  artifact.validate(limits);
  validateDigest("sha256", manifestRootDigest, "manifestRootDigest");
  if (packetPayloadBytes == 0 || packetPayloadBytes > limits.maxPacketPayloadBytes ||
      chunkBytes == 0 || chunkBytes > limits.maxChunkBytes ||
      chunkBytes < packetPayloadBytes ||
      chunkBytes % packetPayloadBytes != 0) {
    throw ArtifactValidationError(
      artifact_error::InvalidRange,
      "resume geometry must use bounded packet and integral chunk sizes");
  }
}

ArtifactResumeSession::ArtifactResumeSession(ArtifactResumeIdentity identity,
                                             ArtifactUploadLease lease,
                                             std::vector<ArtifactChunk> chunks,
                                             uint64_t nowMs)
  : m_identity(std::move(identity))
  , m_lease(std::move(lease))
  , m_chunks(std::move(chunks))
{
  m_identity.validate();
  requireLeaseBinding(m_lease, nowMs);
  if (m_chunks.size() > ArtifactLimits{}.maxManifestChunks) {
    throw ArtifactValidationError(
      artifact_error::LimitExceeded,
      "resume chunk count exceeds the configured limit");
  }
  uint64_t nextOffset = 0;
  for (size_t position = 0; position < m_chunks.size(); ++position) {
    const auto& chunk = m_chunks[position];
    chunk.validate(m_identity.artifact);
    if (chunk.index != position || chunk.offsetBytes != nextOffset ||
        chunk.lengthBytes > m_identity.chunkBytes ||
        (position + 1 < m_chunks.size() &&
         chunk.lengthBytes != m_identity.chunkBytes)) {
      throw ArtifactValidationError(
        artifact_error::InvalidRange,
        "resume chunks must be contiguous, ordered, and use exact geometry");
    }
    const auto expectedFirst = uint64_t{0};
    const auto expectedFinal =
      (chunk.lengthBytes - 1) / m_identity.packetPayloadBytes;
    if (chunk.firstSegment != expectedFirst ||
        chunk.finalSegment != expectedFinal) {
      throw ArtifactValidationError(
        artifact_error::InvalidRange,
        "resume chunk segment coordinates do not match packet geometry");
    }
    nextOffset = checkedAdd(
      nextOffset, chunk.lengthBytes, "repo-resume-chunk-range-overflow");
  }
  if (nextOffset != m_identity.artifact.sizeBytes ||
      (m_identity.artifact.sizeBytes != 0 && m_chunks.empty())) {
    throw ArtifactValidationError(
      artifact_error::InvalidRange,
      "resume chunks must exactly cover the artifact");
  }
  m_snapshot.operationId = m_lease.operationId;
  m_snapshot.leaseId = m_lease.leaseId;
  m_snapshot.expiresAtMs = m_lease.expiresAtMs;
  m_snapshot.totalChunks = m_chunks.size();
}

void
ArtifactResumeSession::restoreVerified(
  const std::vector<uint64_t>& chunkIndices)
{
  if (m_snapshot.state != ArtifactResumeState::Open ||
      !m_verified.empty()) {
    throw std::logic_error(
      "repo-resume-invalid-restore: restore is allowed once on an open session");
  }
  for (const auto index : chunkIndices) {
    if (index >= m_chunks.size()) {
      throw std::out_of_range(
        "repo-resume-invalid-chunk: restored chunk index is out of range");
    }
    m_verified.insert(index);
  }
  m_snapshot.verifiedChunks = m_verified.size();
}

bool
ArtifactResumeSession::markVerified(uint64_t chunkIndex, uint64_t nowMs)
{
  requireOpen(nowMs);
  if (chunkIndex >= m_chunks.size()) {
    throw std::out_of_range(
      "repo-resume-invalid-chunk: verified chunk index is out of range");
  }
  if (m_verified.count(chunkIndex) != 0) {
    m_snapshot.avoidedRetransmissionBytes = checkedAdd(
      m_snapshot.avoidedRetransmissionBytes,
      m_chunks[chunkIndex].lengthBytes,
      "repo-resume-avoided-byte-count-overflow");
    return false;
  }
  m_verified.insert(chunkIndex);
  m_snapshot.verifiedChunks = m_verified.size();
  m_snapshot.newlyVerifiedBytes = checkedAdd(
    m_snapshot.newlyVerifiedBytes, m_chunks[chunkIndex].lengthBytes,
    "repo-resume-verified-byte-count-overflow");
  return true;
}

std::vector<uint64_t>
ArtifactResumeSession::missingChunks(uint64_t nowMs)
{
  requireOpen(nowMs);
  std::vector<uint64_t> missing;
  missing.reserve(m_chunks.size() - m_verified.size());
  for (uint64_t index = 0; index < m_chunks.size(); ++index) {
    if (m_verified.count(index) == 0) {
      missing.push_back(index);
    }
  }
  return missing;
}

void
ArtifactResumeSession::renewLease(ArtifactUploadLease lease, uint64_t nowMs)
{
  if (m_snapshot.state != ArtifactResumeState::Open) {
    throw std::logic_error(
      "repo-resume-invalid-renewal-state: session is not open");
  }
  requireLeaseBinding(lease, nowMs);
  if (lease.issuedAtMs < m_lease.issuedAtMs ||
      lease.expiresAtMs <= m_lease.expiresAtMs ||
      lease.leaseId == m_lease.leaseId ||
      lease.replayId == m_lease.replayId) {
    throw ArtifactValidationError(
      artifact_error::InvalidLease,
      "renewal must advance expiry with fresh lease and replay identities");
  }
  m_lease = std::move(lease);
  m_snapshot.leaseId = m_lease.leaseId;
  m_snapshot.expiresAtMs = m_lease.expiresAtMs;
}

void
ArtifactResumeSession::resume(ArtifactResumeIdentity identity,
                              ArtifactUploadLease lease, uint64_t nowMs)
{
  if (m_snapshot.state != ArtifactResumeState::Cancelled &&
      m_snapshot.state != ArtifactResumeState::Expired) {
    throw std::logic_error(
      "repo-resume-invalid-state: only preserved cancelled or expired sessions resume");
  }
  if (!m_snapshot.preservesProgress) {
    throw std::logic_error(
      "repo-resume-progress-discarded: destructive cancellation cannot resume");
  }
  requireExactIdentity(identity);
  requireLeaseBinding(lease, nowMs);
  if (lease.expiresAtMs <= m_lease.expiresAtMs ||
      lease.leaseId == m_lease.leaseId ||
      lease.replayId == m_lease.replayId) {
    throw ArtifactValidationError(
      artifact_error::InvalidLease,
      "resume requires a fresh lease with a later expiry");
  }
  m_lease = std::move(lease);
  m_snapshot.leaseId = m_lease.leaseId;
  m_snapshot.expiresAtMs = m_lease.expiresAtMs;
  m_snapshot.state = ArtifactResumeState::Open;
}

void
ArtifactResumeSession::cancel(bool preserveProgress)
{
  if (m_snapshot.state != ArtifactResumeState::Open) {
    throw std::logic_error(
      "repo-resume-invalid-cancel: only open sessions may be cancelled");
  }
  m_snapshot.preservesProgress = preserveProgress;
  if (preserveProgress) {
    m_snapshot.state = ArtifactResumeState::Cancelled;
  }
  else {
    m_verified.clear();
    m_snapshot.verifiedChunks = 0;
    m_snapshot.state = ArtifactResumeState::Failed;
    m_failureReason = "repo-resume-cancelled-without-progress";
  }
}

bool
ArtifactResumeSession::expire(uint64_t nowMs)
{
  if (m_snapshot.state != ArtifactResumeState::Open ||
      nowMs < m_lease.expiresAtMs) {
    return false;
  }
  m_snapshot.state = ArtifactResumeState::Expired;
  return true;
}

void
ArtifactResumeSession::complete(uint64_t nowMs)
{
  requireOpen(nowMs);
  if (m_verified.size() != m_chunks.size()) {
    throw std::logic_error(
      "repo-resume-incomplete: every chunk must be verified before completion");
  }
  m_snapshot.state = ArtifactResumeState::Completed;
}

void
ArtifactResumeSession::fail(const std::string& reason)
{
  if (m_snapshot.state == ArtifactResumeState::Completed) {
    throw std::logic_error(
      "repo-resume-terminal: completed session is immutable");
  }
  m_failureReason = reason.empty() ? "repo-resume-failed" : reason;
  m_snapshot.state = ArtifactResumeState::Failed;
}

ArtifactResumeSnapshot
ArtifactResumeSession::snapshot() const
{
  return m_snapshot;
}

const ArtifactResumeIdentity&
ArtifactResumeSession::identity() const noexcept
{
  return m_identity;
}

void
ArtifactResumeSession::requireOpen(uint64_t nowMs)
{
  expire(nowMs);
  if (m_snapshot.state != ArtifactResumeState::Open) {
    throw std::logic_error(
      "repo-resume-session-not-open: operation cannot proceed");
  }
}

void
ArtifactResumeSession::requireExactIdentity(
  const ArtifactResumeIdentity& identity) const
{
  identity.validate();
  if (!sameArtifactReference(identity.artifact, m_identity.artifact) ||
      identity.manifestRootDigest != m_identity.manifestRootDigest ||
      identity.packetPayloadBytes != m_identity.packetPayloadBytes ||
      identity.chunkBytes != m_identity.chunkBytes) {
    throw ArtifactValidationError(
      artifact_error::InvalidManifest,
      "resume identity does not exactly match the durable session");
  }
}

void
ArtifactResumeSession::requireLeaseBinding(const ArtifactUploadLease& lease,
                                           uint64_t nowMs) const
{
  lease.validate(nowMs);
  if (!sameArtifactReference(lease.artifact, m_identity.artifact) ||
      lease.operationId !=
        (m_snapshot.operationId.empty() ? lease.operationId
                                        : m_snapshot.operationId) ||
      (!m_lease.repoNode.empty() && lease.repoNode != m_lease.repoNode)) {
    throw ArtifactValidationError(
      artifact_error::InvalidLease,
      "lease does not bind the exact resumable operation and artifact");
  }
}

std::string
toString(ArtifactResumeState state)
{
  switch (state) {
  case ArtifactResumeState::Open:
    return "OPEN";
  case ArtifactResumeState::Cancelled:
    return "CANCELLED";
  case ArtifactResumeState::Expired:
    return "EXPIRED";
  case ArtifactResumeState::Completed:
    return "COMPLETED";
  case ArtifactResumeState::Failed:
    return "FAILED";
  }
  throw std::invalid_argument("repo-resume-invalid-state");
}

void
ReplicaLeaseControlFlow::beginCollaboration(std::string requestId)
{
  if (m_snapshot.state != ReplicaLeaseControlState::Idle || requestId.empty() ||
      requestId.size() > 256) {
    throw std::invalid_argument(
      "repo-lease-control-invalid-begin: exact request ID and idle state required");
  }
  m_snapshot.requestId = std::move(requestId);
  m_snapshot.state = ReplicaLeaseControlState::CollaborationOpen;
  m_snapshot.controlOperationCount = 1; // one NDNSF collaboration Request
}

void
ReplicaLeaseControlFlow::closeAcks(uint64_t candidateCount)
{
  if (m_snapshot.state != ReplicaLeaseControlState::CollaborationOpen) {
    throw std::logic_error(
      "repo-lease-control-invalid-ack-close: collaboration is not open");
  }
  m_snapshot.candidateCount = candidateCount;
  m_snapshot.state = ReplicaLeaseControlState::AckClosed;
}

void
ReplicaLeaseControlFlow::commitPlan(
  std::vector<ArtifactUploadLease> selectedLeases, uint64_t nowMs)
{
  if (m_snapshot.state != ReplicaLeaseControlState::AckClosed ||
      selectedLeases.empty() ||
      selectedLeases.size() > m_snapshot.candidateCount) {
    throw std::invalid_argument(
      "repo-lease-control-invalid-plan: selected leases exceed closed ACK set");
  }
  std::set<std::string> providers;
  std::set<std::string> leaseIds;
  for (const auto& lease : selectedLeases) {
    lease.validate(nowMs);
    if (!providers.insert(lease.repoNode).second ||
        !leaseIds.insert(lease.leaseId).second) {
      throw std::invalid_argument(
        "repo-lease-control-duplicate-selection: replica and lease IDs must be unique");
    }
  }
  m_snapshot.selectedReplicaCount = selectedLeases.size();
  // One Request plus one authenticated Selection per selected provider. Local
  // ACK closure and planning do not create per-segment wire operations.
  m_snapshot.controlOperationCount = 1 + selectedLeases.size();
  m_snapshot.leases = std::move(selectedLeases);
  m_snapshot.state = ReplicaLeaseControlState::PlanCommitted;
}

void
ReplicaLeaseControlFlow::fail(const std::string& reason)
{
  if (m_snapshot.state == ReplicaLeaseControlState::PlanCommitted) {
    throw std::logic_error(
      "repo-lease-control-terminal: committed lease plan is immutable");
  }
  m_failureReason = reason;
  m_snapshot.state = ReplicaLeaseControlState::Failed;
}

ReplicaLeaseControlSnapshot
ReplicaLeaseControlFlow::snapshot() const
{
  return m_snapshot;
}

std::string
toString(ReplicaLeaseControlState state)
{
  switch (state) {
  case ReplicaLeaseControlState::Idle:
    return "IDLE";
  case ReplicaLeaseControlState::CollaborationOpen:
    return "COLLABORATION_OPEN";
  case ReplicaLeaseControlState::AckClosed:
    return "ACK_CLOSED";
  case ReplicaLeaseControlState::PlanCommitted:
    return "PLAN_COMMITTED";
  case ReplicaLeaseControlState::Failed:
    return "FAILED";
  }
  throw std::invalid_argument("repo-lease-control-invalid-state");
}

} // namespace ndnsf_distributed_repo
