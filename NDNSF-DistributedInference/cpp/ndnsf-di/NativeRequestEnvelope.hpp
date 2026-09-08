#pragma once
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"

namespace ndnsf::di {

struct NativeEncodedRequest
{
  std::vector<std::uint8_t> wire;
  std::string modelIntentDigest;
  std::string logicalInputDigest;
  std::string inputManifestDigest;
  std::string invocationId;
  std::string requestContractDigest;
};

/** Exact DIRequestEnvelopeV2 compatibility wire, shared by native requester
 * and offline SDK parity tests. Payload is already encoded by its adapter. */
NativeEncodedRequest encodeNativeRequestEnvelope(
  const NativeModelDescriptor& model, const NativeApplicationInput& input,
  const NativeRequestContract& contract, const std::string& requestId,
  std::uint64_t attempt, std::uint64_t deadlineMs);

} // namespace ndnsf::di
