#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeObservedOfferV3.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::di {

struct NativeWorkStatisticsSnapshot;
class NativeWorkStatistics;

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
  // Required for GPU offers: one native memory observation per visible device,
  // captured no more than two seconds ago. Called once; the same immutable
  // snapshot is signed and emitted. Missing/invalid samples yield a negative ACK.
  // The optional per-call collector is borrowed and must not be retained.
  std::function<std::vector<NativeDeviceResourceObservationV3>(
    std::uint64_t, const std::shared_ptr<NativeWorkStatistics>&)> observeResources;
  std::function<std::string(const std::string&)> signDigest;
  /** Local observations only; never changes the signed offer or admission. */
  std::function<void(const std::string&, const NativeWorkStatisticsSnapshot&)> workObserver;
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
