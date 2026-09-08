#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ndnsf::di {

struct NativeDeviceResourceObservationV3
{
  std::string device;
  std::uint64_t totalMemoryMb = 0;
  std::uint64_t freeMemoryMb = 0;
  std::uint64_t activeRequests = 0;
  std::uint64_t resourceSequence = 1;
  std::uint64_t capturedAtMs = 1;
  std::string topologyDigest;
};

struct NativeResidencyObservationV3
{
  std::string artifactDigest;
  std::string role;
  std::uint64_t rank = 0;
  std::string tier;
  std::vector<std::string> deviceSet;
  std::string bootEpoch;
  std::string processEpoch;
  std::string topologyDigest;
  std::uint64_t capturedAtMs = 1;
  std::uint64_t expiresAtMs = 2;
  std::string proofDigest;
  std::string residencyClass = "CANONICAL";
  std::string identityDigest;
  std::string assemblySpecDigest;
  std::string modelManifestDigest;
  std::string artifactProfileDigest;
  std::string graphDigest;
  std::string backend;
  std::string protectionEpoch;
  std::uint64_t runtimeGeneration = 0;
  std::string fencingToken;
  std::uint64_t missingVerifiedBytes = 0;
  double estimatedAssemblyMs = 0;
  double estimatedLoadMs = 0;
};

/** Data-only decoded observation: no authentication, admission or lease authority.
 * Both Core ACK provenance and the policy-bound offer signature must be verified
 * before these values may be passed to planning. hasModel is never exact reuse.
 */
struct NativeObservedProviderOfferV3
{
  std::string requestId;
  std::uint64_t attempt = 0;
  std::string service;
  std::string provider;
  std::string modelDigest;
  std::string graphDigest;
  bool status = false;
  std::string executionDisposition;
  bool preparationAccepted = false;
  std::vector<std::string> devices;
  std::string topologyBackend;
  std::string declaredTopologyDigest;
  std::string topologyDigest;
  std::vector<NativeDeviceResourceObservationV3> resources;
  std::vector<NativeResidencyObservationV3> residency;
  std::vector<std::string> acceptedRoles;
  std::vector<std::string> backends;
  std::uint64_t queueDepth = 0;
  double estimatedWaitMs = 0;
  double rttMs = 0;
  double bandwidthMbps = 0;
  std::string bootEpoch;
  std::uint64_t capturedAtMs = 1;
  std::uint64_t expiresAtMs = 2;
  std::string signerKeyId;
  std::string signature;
  bool canProvision = false;
  bool hasModel = false;
  std::string offerDigest;
};

NativeObservedProviderOfferV3 decodeNativeProviderOfferV3(const std::string& wire);

} // namespace ndnsf::di
