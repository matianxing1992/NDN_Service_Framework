#pragma once
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"

namespace ndnsf::di {

struct NativeGenerationRecovery
{
  std::string generationId;
  std::string originalRequestId;
  std::string recoveryRequestId;
  std::string originalInputManifestDigest;
  std::string priorPlanDigest;
  std::string failedProvider;
  std::vector<std::int64_t> committedTokenIds;
};

struct NativeEncodedRequest
{
  std::vector<std::uint8_t> wire;
  std::string modelIntentDigest;
  std::string logicalInputDigest;
  std::string inputManifestDigest;
  std::string invocationId;
  std::string requestContractDigest;
  std::optional<NativeGenerationRecovery> recovery;
};

/** Return whether a request contract uses a generation mode understood by the
 * native requester/provider wire contract.  All public construction paths use
 * this predicate so direct C++ callers cannot bypass the JSON configuration
 * gate. */
bool
isSupportedNativeGenerationMode(const std::string& mode) noexcept;

/** Derive the generation values from the same application options bytes bound
 * into the request envelope. Placement adds stride and authenticated recovery
 * prefixes later; it cannot substitute different sampling or tokenizer data. */
NativeGenerationExecutionContractV1 nativeGenerationFromOptions(
  const std::vector<std::uint8_t>& options, const std::string& generationId);

/** Exact DIRequestEnvelopeV2 compatibility wire, shared by native requester
 * and offline SDK parity tests. Payload is already encoded by its adapter. */
NativeEncodedRequest encodeNativeRequestEnvelope(
  const NativeModelDescriptor& model, const NativeApplicationInput& input,
  const NativeRequestContract& contract, const std::string& requestId,
  std::uint64_t attempt, std::uint64_t deadlineMs,
  const std::optional<NativeGenerationRecovery>& recovery = std::nullopt);

} // namespace ndnsf::di
