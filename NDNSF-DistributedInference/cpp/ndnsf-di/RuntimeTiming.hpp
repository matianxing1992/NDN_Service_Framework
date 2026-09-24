#pragma once

#include <algorithm>
#include <mutex>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <set>
#include <utility>
#include <vector>
#include <cstddef>

namespace ndnsf::di {

/**
 * Native observation for one direction of one planned stage edge. Optional
 * transport fields are deliberately distinct from zero: a missing owner
 * observation is unknown, not a zero-cost transfer.
 */
struct StageTransferObservation
{
  std::string edgeScope;
  std::string plannedDataName;
  std::string direction;
  std::string phase;
  std::string identity;
  std::string actualDataName;
  std::string lineageIdentity;
  std::string positionDigest;
  bool lineagePresent = false;
  std::vector<std::string> tensorNames;
  std::size_t tensorBytes = 0;
  std::size_t encodedPayloadBytes = 0;
  // Core's encrypted/encoded transport payload. This is distinct from the
  // DI-side TensorBundle payload and from serialized Data wire bytes.
  std::optional<std::size_t> transportPayloadBytes;
  std::optional<std::size_t> metadataBytes;
  std::optional<std::size_t> wireBytes;
  std::optional<std::size_t> interestCount;
  std::optional<std::size_t> retryCount;
  std::optional<std::size_t> localCopyBytes;
  std::optional<std::size_t> transportLocalCopyBytes;
};

inline std::string
canonicalStagePhase(std::string_view operationKind)
{
  if (operationKind == "APPLICATION_INPUT" || operationKind == "PROMPT") {
    return "prompt";
  }
  if (operationKind == "TOKEN_FEEDBACK" || operationKind == "DECODE") {
    return "decode";
  }
  if (operationKind == "FINALIZE" || operationKind == "CHECKPOINT_FINALIZE") {
    return "finalize";
  }
  if (operationKind == "ACTIVATION" || operationKind == "DELTA") {
    return "delta";
  }
  return operationKind.empty() ? "unknown" : std::string(operationKind);
}

inline void
validateStageTransferObservation(const StageTransferObservation& observation)
{
  if (observation.edgeScope.empty() || observation.direction.empty() ||
      observation.phase.empty() || observation.identity.empty()) {
    throw std::invalid_argument("incomplete stage transfer observation");
  }
  if ((observation.phase == "decode" || observation.phase == "finalize") &&
      (!observation.lineagePresent || observation.positionDigest.empty())) {
    throw std::invalid_argument(
      "generation stage transfer is missing position/lineage");
  }
}

/**
 * Monotonic native budget accumulator. `deltaFrom` is the only supported way
 * to compare cumulative snapshots, preventing the same epoch snapshot from
 * being summed repeatedly.
 */
struct StageTransferBudget
{
  std::size_t tensorBytes = 0;
  std::size_t encodedPayloadBytes = 0;
  std::size_t transportPayloadBytes = 0;
  std::size_t metadataBytes = 0;
  std::size_t wireBytes = 0;
  std::size_t interestCount = 0;
  std::size_t retryCount = 0;
  std::size_t localCopyBytes = 0;
  std::size_t transportLocalCopyBytes = 0;
  bool metadataObserved = false;
  bool transportPayloadObserved = false;
  bool wireObserved = false;
  bool interestObserved = false;
  bool retryObserved = false;
  bool localCopyObserved = false;
  bool transportLocalCopyObserved = false;
  std::set<std::string> seenIdentities;
  std::string snapshotIdentity;

  void add(const StageTransferObservation& observation)
  {
    validateStageTransferObservation(observation);
    if (!seenIdentities.insert(observation.identity).second) {
      throw std::invalid_argument("duplicate stage transfer observation");
    }
    if (observation.transportLocalCopyBytes) {
      transportLocalCopyBytes += *observation.transportLocalCopyBytes;
      transportLocalCopyObserved = true;
    }
    tensorBytes += observation.tensorBytes;
    encodedPayloadBytes += observation.encodedPayloadBytes;
    if (observation.transportPayloadBytes) {
      transportPayloadBytes += *observation.transportPayloadBytes;
      transportPayloadObserved = true;
    }
    if (observation.localCopyBytes) {
      localCopyBytes += *observation.localCopyBytes;
      localCopyObserved = true;
    }
    if (observation.metadataBytes) {
      metadataBytes += *observation.metadataBytes;
      metadataObserved = true;
    }
    if (observation.wireBytes) {
      wireBytes += *observation.wireBytes;
      wireObserved = true;
    }
    if (observation.interestCount) {
      interestCount += *observation.interestCount;
      interestObserved = true;
    }
    if (observation.retryCount) {
      retryCount += *observation.retryCount;
      retryObserved = true;
    }
  }

  StageTransferBudget deltaFrom(const StageTransferBudget& previous) const
  {
    if (snapshotIdentity.empty() || previous.snapshotIdentity.empty()) {
      throw std::invalid_argument("stage transfer snapshot identity is missing");
    }
    if (snapshotIdentity == previous.snapshotIdentity ||
        seenIdentities == previous.seenIdentities ||
        !std::includes(seenIdentities.begin(), seenIdentities.end(),
                       previous.seenIdentities.begin(), previous.seenIdentities.end())) {
      throw std::invalid_argument("duplicate stage transfer snapshot");
    }
    if (tensorBytes < previous.tensorBytes ||
        encodedPayloadBytes < previous.encodedPayloadBytes ||
        transportPayloadBytes < previous.transportPayloadBytes ||
        metadataBytes < previous.metadataBytes ||
        wireBytes < previous.wireBytes ||
        interestCount < previous.interestCount ||
        retryCount < previous.retryCount ||
        localCopyBytes < previous.localCopyBytes ||
        transportLocalCopyBytes < previous.transportLocalCopyBytes) {
      throw std::invalid_argument("stage transfer snapshots are out of order");
    }
    StageTransferBudget delta;
    delta.tensorBytes = tensorBytes - previous.tensorBytes;
    delta.encodedPayloadBytes = encodedPayloadBytes - previous.encodedPayloadBytes;
    delta.transportPayloadBytes =
      transportPayloadBytes - previous.transportPayloadBytes;
    delta.metadataBytes = metadataBytes - previous.metadataBytes;
    delta.wireBytes = wireBytes - previous.wireBytes;
    delta.interestCount = interestCount - previous.interestCount;
    delta.retryCount = retryCount - previous.retryCount;
    if (transportLocalCopyObserved) {
      delta.transportLocalCopyBytes =
        transportLocalCopyBytes - (previous.transportLocalCopyObserved ?
          previous.transportLocalCopyBytes : 0);
      delta.transportLocalCopyObserved = true;
    }
    if (localCopyObserved) {
      delta.localCopyBytes = localCopyBytes - (previous.localCopyObserved ?
        previous.localCopyBytes : 0);
      delta.localCopyObserved = true;
    }
    delta.metadataObserved = metadataObserved;
    delta.transportPayloadObserved = transportPayloadObserved;
    delta.wireObserved = wireObserved;
    delta.interestObserved = interestObserved;
    delta.retryObserved = retryObserved;
    delta.snapshotIdentity = snapshotIdentity;
    return delta;
  }
};

// Runtime timing records are parsed as line-oriented evidence.  All native
// producers of those records must use the same process-wide mutex so that a
// concurrent ONNX timing line cannot split a dependency timing record.
inline std::mutex&
runtimeTimingOutputMutex()
{
  static std::mutex mutex;
  return mutex;
}

// Submit one complete machine-readable record through the ndn-cxx logging
// backend.  Callers must assemble the whole record before invoking this
// function; mixing multi-insertion stdout records with NDN_LOG output can
// corrupt a line when stdout and stderr are redirected to the same file.
void
logRuntimeEvidence(const std::string& record);

// Diagnostic records use the same process-safe ndn-cxx component but expose
// the caller's intended severity so operators can keep routine traces quiet
// while retaining warnings/errors.  Records must not contain prompt/answer
// text, token keys, logits, state tensors, or key bytes.
void
logRuntimeTrace(const std::string& record);

void
logRuntimeInfo(const std::string& record);

void
logRuntimeWarn(const std::string& record);

void
logRuntimeError(const std::string& record);

/** One immutable phase observation parsed from an NDNSF_PHASE_TIMING line. */
struct RuntimePhaseObservation
{
  // `role` is the emitting component (user, di-provider, or di-cli).  The
  // execution role is kept separately so a provider's stage role cannot
  // overwrite the component identity.
  std::string role;
  std::string executionRole;
  std::string phase;
  std::string scope;
  std::string requestId;
  std::string attempt;
  std::string providerBootId;
  std::string providerName;
  std::string sessionId;
  std::string conversationId;
  std::string inferenceEpoch;
  std::string contextEpoch;
  std::uint64_t tokenIndex = 0;
  std::uint64_t steadyUs = 0;
  std::uint64_t wallUs = 0;
};

/**
 * Parse the canonical phase record emitted by the native timing path.
 * Missing or malformed fields return nullopt; a missing field is never
 * represented as a zero timestamp.
 */
std::optional<RuntimePhaseObservation>
parseRuntimePhaseObservation(std::string_view record);

/** Validate one request/attempt sequence without merging retry identities. */
bool
validateRuntimePhaseSequence(const std::vector<RuntimePhaseObservation>& observations,
                             std::string* error = nullptr);

/** Emit one structured phase record when NDNSF_PHASE_TIMING is enabled. */
void
logRuntimePhase(const std::string& role,
                const std::string& phase,
                const std::string& requestId,
                const std::string& attempt = {},
                const std::vector<std::pair<std::string, std::string>>& fields = {});

} // namespace ndnsf::di
