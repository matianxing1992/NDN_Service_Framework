#ifndef NDN_SERVICE_FRAMEWORK_DETAIL_TARGETED_REQUEST_ROUTING_HPP
#define NDN_SERVICE_FRAMEWORK_DETAIL_TARGETED_REQUEST_ROUTING_HPP

#include <ndn-cxx/name.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace ndn_service_framework::detail {

inline constexpr std::string_view TARGETED_REQUEST_ID_PREFIX = "NDNSF-TARGETED-V1:";
inline constexpr size_t MAX_TARGETED_REQUEST_ID_BYTES = 4096;

// Core-generated targeted IDs reserve this prefix. The one-component ID stays
// opaque to legacy V2 parsers and is bound by Data signatures and existing AAD.
inline ndn::Name makeTargetedRequestId(const ndn::Name& provider,
                                     const ndn::Name& freshId)
{
  if (provider.empty() || freshId.size() != 1)
    return freshId;
  const auto value = std::string(TARGETED_REQUEST_ID_PREFIX) +
                     provider.toUri() + "|" + freshId.toUri();
  // Large or unusual legacy IDs keep their original processing path.
  if (value.size() > MAX_TARGETED_REQUEST_ID_BYTES)
    return freshId;
  return ndn::Name().append(value.begin(), value.end());
}

inline std::optional<ndn::Name> targetedRequestProvider(const ndn::Name& requestId)
{
  if (requestId.size() != 1 ||
      requestId[0].type() != ndn::tlv::GenericNameComponent ||
      requestId[0].value_size() > MAX_TARGETED_REQUEST_ID_BYTES)
    return std::nullopt;
  const auto& component = requestId[0];
  const std::string_view value(reinterpret_cast<const char*>(component.value()),
                               component.value_size());
  if (value.substr(0, TARGETED_REQUEST_ID_PREFIX.size()) != TARGETED_REQUEST_ID_PREFIX)
    return std::nullopt;
  const auto separator = value.find('|', TARGETED_REQUEST_ID_PREFIX.size());
  if (separator == std::string_view::npos)
    return std::nullopt;
  const std::string target(value.substr(TARGETED_REQUEST_ID_PREFIX.size(),
                                      separator - TARGETED_REQUEST_ID_PREFIX.size()));
  const std::string nonce(value.substr(separator + 1));
  try {
    const ndn::Name provider(target);
    const ndn::Name originalId(nonce);
    if (provider.empty() || originalId.size() != 1 ||
        provider.toUri() != target || originalId.toUri() != nonce)
      return std::nullopt;
    return provider;
  }
  catch (const ndn::Name::Error&) {
    return std::nullopt;
  }
}

} // namespace ndn_service_framework::detail

#endif
