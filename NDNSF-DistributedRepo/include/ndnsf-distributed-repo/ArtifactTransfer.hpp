#ifndef NDNSF_DISTRIBUTED_REPO_ARTIFACT_TRANSFER_HPP
#define NDNSF_DISTRIBUTED_REPO_ARTIFACT_TRANSFER_HPP

#include "ndnsf-distributed-repo/ArtifactTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ndnsf_distributed_repo {

struct AdaptiveTransferOptions
{
  uint32_t initialWindow = 4;
  uint32_t minimumWindow = 1;
  uint32_t maximumWindow = 64;
  uint32_t verificationBacklogLimit = 16;
  uint32_t maximumRetries = 5;
  uint64_t segmentTimeoutMs = 1000;

  void validate() const;
};

struct ArtifactSegmentRequest
{
  uint64_t segmentNo = 0;
  uint32_t attempt = 0;
  bool retransmission = false;
};

enum class ArtifactSegmentDisposition
{
  Accepted,
  Duplicate,
  Unsolicited,
};

struct ArtifactTransferSnapshot
{
  uint64_t totalSegments = 0;
  uint64_t verifiedSegments = 0;
  uint64_t inFlightSegments = 0;
  uint64_t verificationBacklog = 0;
  uint64_t logicalBytes = 0;
  uint64_t wireBytes = 0;
  uint64_t retransmittedBytes = 0;
  uint64_t interestCount = 0;
  uint64_t retransmissionCount = 0;
  uint64_t duplicateCount = 0;
  uint64_t timeoutCount = 0;
  uint64_t rejectedCount = 0;
  double congestionWindow = 0.0;
  bool complete = false;
  bool failed = false;
  std::string failureReason;
};

/**
 * Deterministic scheduler used by the live NDN fetcher and failure-injection
 * tests. Received segments leave the Interest window immediately but continue
 * to exert backpressure until the persistence/verifier calls markVerified().
 */
class AdaptiveArtifactTransfer
{
public:
  explicit AdaptiveArtifactTransfer(uint64_t totalSegments,
                                    AdaptiveTransferOptions options = {});

  std::vector<ArtifactSegmentRequest>
  poll(uint64_t nowMs,
       size_t maximumRequests = std::numeric_limits<size_t>::max());

  ArtifactSegmentDisposition
  receive(uint64_t segmentNo, uint64_t logicalBytes, uint64_t wireBytes,
          uint64_t nowMs);

  void markVerified(uint64_t segmentNo);
  void reject(uint64_t segmentNo, const std::string& reason);
  void expire(uint64_t nowMs);
  void fail(const std::string& reason);

  ArtifactTransferSnapshot snapshot() const;
  std::vector<uint64_t> missingSegments() const;

private:
  struct InFlight
  {
    uint64_t sentAtMs = 0;
    uint32_t attempt = 0;
    bool retransmission = false;
  };

  bool canIssue() const;
  void queueRetransmission(uint64_t segmentNo, uint32_t nextAttempt);
  void onCongestion();

private:
  uint64_t m_totalSegments;
  AdaptiveTransferOptions m_options;
  double m_window;
  uint64_t m_nextSegment = 0;
  std::map<uint64_t, InFlight> m_inFlight;
  std::map<uint64_t, uint32_t> m_retryQueue;
  std::set<uint64_t> m_received;
  std::set<uint64_t> m_verified;
  ArtifactTransferSnapshot m_metrics;
};

enum class ArtifactResumeState
{
  Open,
  Cancelled,
  Expired,
  Completed,
  Failed,
};

struct ArtifactResumeIdentity
{
  ArtifactReference artifact;
  std::string manifestRootDigest;
  uint64_t packetPayloadBytes = 0;
  uint64_t chunkBytes = 0;

  void validate(const ArtifactLimits& limits = {}) const;
};

struct ArtifactResumeSnapshot
{
  ArtifactResumeState state = ArtifactResumeState::Open;
  std::string operationId;
  std::string leaseId;
  uint64_t expiresAtMs = 0;
  uint64_t totalChunks = 0;
  uint64_t verifiedChunks = 0;
  uint64_t newlyVerifiedBytes = 0;
  uint64_t avoidedRetransmissionBytes = 0;
  bool preservesProgress = true;
};

/**
 * Exact-identity resumable transfer state. Durable storage owns the verified
 * ranges; this class owns the bounded state transitions and missing-work plan.
 */
class ArtifactResumeSession
{
public:
  ArtifactResumeSession(ArtifactResumeIdentity identity,
                        ArtifactUploadLease lease,
                        std::vector<ArtifactChunk> chunks,
                        uint64_t nowMs);

  void restoreVerified(const std::vector<uint64_t>& chunkIndices);
  bool markVerified(uint64_t chunkIndex, uint64_t nowMs);
  std::vector<uint64_t> missingChunks(uint64_t nowMs);
  void renewLease(ArtifactUploadLease lease, uint64_t nowMs);
  void resume(ArtifactResumeIdentity identity, ArtifactUploadLease lease,
              uint64_t nowMs);
  void cancel(bool preserveProgress);
  bool expire(uint64_t nowMs);
  void complete(uint64_t nowMs);
  void fail(const std::string& reason);

  ArtifactResumeSnapshot snapshot() const;
  const ArtifactResumeIdentity& identity() const noexcept;

private:
  void requireOpen(uint64_t nowMs);
  void requireExactIdentity(const ArtifactResumeIdentity& identity) const;
  void requireLeaseBinding(const ArtifactUploadLease& lease,
                           uint64_t nowMs) const;

private:
  ArtifactResumeIdentity m_identity;
  ArtifactUploadLease m_lease;
  std::vector<ArtifactChunk> m_chunks;
  std::set<uint64_t> m_verified;
  ArtifactResumeSnapshot m_snapshot;
  std::string m_failureReason;
};

std::string
toString(ArtifactResumeState state);

enum class ReplicaLeaseControlState
{
  Idle,
  CollaborationOpen,
  AckClosed,
  PlanCommitted,
  Failed,
};

struct ReplicaLeaseControlSnapshot
{
  ReplicaLeaseControlState state = ReplicaLeaseControlState::Idle;
  std::string requestId;
  uint64_t candidateCount = 0;
  uint64_t selectedReplicaCount = 0;
  uint64_t controlOperationCount = 0;
  std::vector<ArtifactUploadLease> leases;
};

/**
 * Repository-specific interpretation of the generic delayed-planning NDNSF
 * collaboration lifecycle. Segment count and artifact size are deliberately
 * absent, making the control-operation bound structurally testable.
 */
class ReplicaLeaseControlFlow
{
public:
  void beginCollaboration(std::string requestId);
  void closeAcks(uint64_t candidateCount);
  void commitPlan(std::vector<ArtifactUploadLease> selectedLeases,
                  uint64_t nowMs);
  void fail(const std::string& reason);

  ReplicaLeaseControlSnapshot snapshot() const;

private:
  ReplicaLeaseControlSnapshot m_snapshot;
  std::string m_failureReason;
};

std::string
toString(ReplicaLeaseControlState state);

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_ARTIFACT_TRANSFER_HPP
