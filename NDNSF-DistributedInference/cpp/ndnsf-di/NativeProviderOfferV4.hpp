#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderResidencyV4.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderReadiness.hpp"
#include "ndn-service-framework/NetworkTelemetry.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::di {

class ProviderArtifactCache;
class NativeProviderRunnerReuseCache;
class OnnxRuntimeSessionCache;
class NativeProtectedPlaintextCache;
class NativePersistentRecipeResidencyIndex;

inline constexpr char NATIVE_PROVIDER_OFFER_V4_FIELD[] =
  "ndnsf.di.placement.v4";

/** Shared packet-train metadata for ACK and targeted response producers. */
NativeJson nativeProbeTrainRateMetadataV4(
  const ndn_service_framework::NetworkTelemetrySnapshot& sample);

struct NativeProviderOfferV4Config
{
  std::string provider;
  std::string service;
  std::string bootEpoch;
  std::string signerKeyId;
  std::function<std::string(const std::string&)> signDigest;
  std::uint64_t telemetryMaxAgeMs = 5000;
  std::uint64_t offerTtlMs = 5000;
};

struct NativeProviderOfferV4Decision
{
  std::string capabilityFieldValue;
  std::vector<std::string> fallbackCapabilityFieldValues;
  std::uint64_t pendingStateTtlMs = 0;
};

/** Combine exact owner queries. Missing, duplicate, or incomplete owners make
 * the whole request-scoped residency view UNKNOWN; a local owner's empty
 * answer is never treated as provider-wide KNOWN_EMPTY. */
NativeProviderResidencySnapshotV4 combineNativeProviderResidencyOwnersV4(
  const std::vector<NativeProviderResidencyOwnerResultV4>& owners);

/** Query the five production cache owners through their bounded exact-match APIs. */
NativeProviderResidencySnapshotV4 queryNativeProviderResidencyOwnersV4(
  const std::vector<NativeJson>& candidateArtifacts,
  const std::string& processEpoch,
  std::uint64_t capturedAtMs,
  std::uint64_t expiresAtMs,
  const ProviderArtifactCache* artifactCache,
  const NativeProviderRunnerReuseCache* runnerCache,
  const OnnxRuntimeSessionCache* sessionCache,
  const NativeProtectedPlaintextCache* protectedPlaintextCache,
  const NativePersistentRecipeResidencyIndex* persistentRecipeIndex);

using NativeProviderResidencyQueryV4 = std::function<NativeProviderResidencySnapshotV4(
  const std::vector<NativeJson>& candidateArtifacts,
  const std::string& processEpoch,
  std::uint64_t capturedAtMs,
  std::uint64_t expiresAtMs)>;

/** Add bounded local resource and DI residency facts to an existing ACK.
 * Ordinary ACK processing never starts Provider-to-Provider probes; selected
 * measurements use the separate targeted invocation path. */
ndn_service_framework::ServiceProvider::AsyncAckStrategyHandler
makeNativeProviderObservationAckHandlerV4(
  ndn_service_framework::ServiceProvider::AckStrategyHandler baseAckHandler,
  NativeProviderOfferV4Config config,
  std::function<NativeProviderTelemetrySnapshot()> telemetrySnapshot,
  NativeProviderResidencyQueryV4 residencyQuery = {});

/** Convert fresh Core-owned directed samples into canonical signed V4 link
 * rows bound to the Provider's measured resource topology digest.
 * Unsupported or incomplete sample kinds are omitted. */
std::vector<NativeJson> makeNativeProviderLinkObservationsV4(
  const std::vector<ndn_service_framework::NetworkTelemetrySnapshot>& observations,
  const std::string& localProvider,
  const std::string& bootEpoch,
  const std::string& topologyDigest,
  std::uint64_t nowMs,
  std::uint64_t expiresAtMs);

/**
 * Issue the request-scoped, separately signed V4 observation extension.
 * Incomplete request binding returns nullopt so the unchanged V3 offer can
 * continue to be used.
 */
std::optional<NativeProviderOfferV4Decision>
issueNativeProviderOfferV4(const std::vector<std::uint8_t>& requestPayload,
                           const NativeProviderOfferV4Config& config,
                           const NativeProviderTelemetrySnapshot& telemetry,
                           std::uint64_t nowMs,
                           const std::vector<NativeJson>& linkObservations = {},
                           const NativeProviderResidencySnapshotV4& residency = {});

} // namespace ndnsf::di
