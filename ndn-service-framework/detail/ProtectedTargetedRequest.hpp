#ifndef NDN_SERVICE_FRAMEWORK_DETAIL_PROTECTED_TARGETED_REQUEST_HPP
#define NDN_SERVICE_FRAMEWORK_DETAIL_PROTECTED_TARGETED_REQUEST_HPP

#include "TargetedRequestRouting.hpp"
#include "../NDNSFMessages.hpp"
#include "../utils.hpp"

#include <stdexcept>
#include <charconv>
#include <cstddef>

namespace ndn_service_framework::detail {

inline constexpr const char* TARGETED_KEY_ENVELOPE_FIELD =
  "TargetedRequestScopedKeyEnvelopeV1";
inline constexpr const char* TARGETED_INLINE_INPUT_FIELD = "TargetedInlineSignedInputV1";
inline constexpr std::size_t TARGETED_INLINE_INPUT_MAX_BYTES = 4096;
inline constexpr std::size_t TARGETED_INLINE_REQUEST_MAX_BYTES = 8192;

inline bool hasOptionalInlineTargetedInput(const RequestMessage& request)
{
  return request.getRequestMode() == tlv::TargetedRequest &&
    request.hasRequestCapabilities() &&
    request.getRequestCapabilities().hasField("RequestScopedConfidentialityV1") &&
    request.getRequestCapabilities().getField("RequestScopedConfidentialityV1") == "required" &&
    request.getRequestCapabilities().hasField(TARGETED_INLINE_INPUT_FIELD) &&
    request.getRequestCapabilities().getField(TARGETED_INLINE_INPUT_FIELD) == "optional";
}

inline bool parseProtectedTargetedUint64(const std::string& text, uint64_t& value)
{
  if (text.empty()) return false;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
         std::to_string(value) == text;
}

inline bool isScopedTargetedRequest(const RequestMessage& request)
{
  return request.getRequestMode() == tlv::TargetedRequest &&
         request.hasRequestCapabilities() &&
         request.getRequestCapabilities().hasField("RequestScopedConfidentialityV1") &&
         request.getRequestCapabilities().getField("RequestScopedConfidentialityV1") == "required";
}

inline bool hasProtectedTargetedCarrier(const RequestMessage& request)
{
  return request.getRequestMode() == tlv::TargetedRequest &&
         request.hasRequestCapabilities() &&
         request.getRequestCapabilities().hasField(TARGETED_KEY_ENVELOPE_FIELD);
}

// This local descriptor is crypto AAD, never a transmitted Selection or an
// authorization decision. Both peers derive it from authenticated Request facts.
inline ServiceSelectionMessage makeProtectedTargetedBindingSelection(
  const ndn::Name& requester, const ndn::Name& provider,
  const ndn::Name& service, const ndn::Name& requestId,
  const RequestMessage& request)
{
  const auto encodedTarget = targetedRequestProvider(requestId);
  if (requester.empty() || provider.empty() || service.empty() ||
      !encodedTarget || *encodedTarget != provider ||
      request.getRequestMode() != tlv::TargetedRequest ||
      request.getTargetProvider() != provider ||
      !request.hasControllerVersion() ||
      !request.getControllerVersion().isValid() ||
      request.getUserToken().empty() || request.getProviderToken().empty() ||
      !request.hasUserEncryptionCertificate() ||
      request.hasStreamRequestOptions() || request.hasConversationContinuation() ||
      request.hasDeploymentIntent() || !isScopedTargetedRequest(request)) {
    throw std::invalid_argument("invalid protected Targeted request binding");
  }
  const auto& original = request.getRequestCapabilities();
  if (original.getVersion() != DeploymentControlMessage::VERSION ||
      !original.hasField("RequestScopedConfidentialityV1") ||
      original.getField("RequestScopedConfidentialityV1") != "required") {
    throw std::invalid_argument("protected Targeted request requires scoped mode");
  }
  // Exclude only the envelope itself to avoid a circular digest. Keep all
  // other Request fields, including both tokens, certificate and policy version.
  RequestCapabilities capabilities;
  capabilities.setVersion(original.getVersion());
  for (const auto& [name, value] : original.getFields()) {
    if (name != TARGETED_KEY_ENVELOPE_FIELD)
      capabilities.setField(name, value);
  }
  RequestMessage normalized(request);
  normalized.setRequestCapabilities(capabilities);
  if (original.hasField(TARGETED_INLINE_INPUT_FIELD)) {
    if (!hasOptionalInlineTargetedInput(request))
      throw std::invalid_argument("invalid protected Targeted inline input mode");
    // The embedded Data is independently User-signed and its AEAD authenticates
    // this binding. Include the negotiated mode, excluding only its Data bytes.
    ndn::Buffer empty;
    normalized.setPayload(empty, 0);
  }
  const auto wire = normalized.WireEncode();
  const std::string domain = "NDNSF-TARGETED-REQUEST-SCOPED-KEY-V1:";
  ndn::Buffer transcript(domain.begin(), domain.end());
  transcript.insert(transcript.end(), wire.begin(), wire.end());

  ServiceSelectionMessage binding;
  binding.setRequestIDs({requestId.toUri()});
  binding.setAttempt(1); // A fresh Core RPC ID owns one transport invocation.
  binding.setPolicyEpoch(request.getPolicyEpoch());
  binding.setControllerVersion(request.getControllerVersion());
  binding.setAssignmentPayload(transcript);
  SelectionProviderEntry entry;
  entry.providerName = provider;
  entry.providerTokenHash = computeSelectionProviderTokenProofHash(
    requester, provider, service, request.getProviderToken());
  binding.addProviderEntry(entry);
  return binding;
}

} // namespace ndn_service_framework::detail

#endif
