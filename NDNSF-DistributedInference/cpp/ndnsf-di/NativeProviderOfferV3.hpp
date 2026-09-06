#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::di {

struct NativeProviderOfferV3Config
{
  std::string provider;
  std::string service;
  std::string bootEpoch;
  std::string signerKeyId;
  std::vector<std::string> acceptedRoles;
  std::vector<std::string> backends;
  std::vector<std::string> devices;
  bool canProvision = true;
  bool hasModel = false;
  std::function<std::string(const std::string&)> signDigest;
};

struct NativeProviderOfferV3Decision
{
  bool status = false;
  std::string message;
  std::string payload;
  std::uint64_t pendingStateTtlMs = 0;
};

/**
 * Issue the signed observational offer used by DI_PLACEMENT_V3.
 *
 * A non-V3 request returns std::nullopt so callers can retain their generic
 * readiness ACK. A malformed or expired V3 request returns a negative
 * decision and never calls the signer.
 */
std::optional<NativeProviderOfferV3Decision>
issueNativeProviderOfferV3(const std::vector<std::uint8_t>& requestPayload,
                           const NativeProviderOfferV3Config& config,
                           std::uint64_t nowMs);

} // namespace ndnsf::di
