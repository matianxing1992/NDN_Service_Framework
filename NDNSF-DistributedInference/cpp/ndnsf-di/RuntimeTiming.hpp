#pragma once

#include <mutex>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ndnsf::di {

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
