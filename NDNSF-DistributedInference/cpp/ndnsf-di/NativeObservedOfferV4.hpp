#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <cstdint>
#include <string>

namespace ndnsf::di {

/** Strictly decoded V4 values remain data-only until ACK and offer admission. */
struct NativeObservedProviderOfferV4
{
  std::string requestId;
  std::uint64_t attempt = 0;
  std::string service;
  std::string provider;
  std::string modelDigest;
  std::string graphDigest;
  std::string candidateSetDigest;
  std::string candidateArtifactSetDigest;
  std::string probeScopeDigest;
  std::string bootEpoch;
  std::uint64_t capturedAtMs = 0;
  std::uint64_t expiresAtMs = 0;
  std::string signerKeyId;
  std::string signature;
  std::string offerDigest;
  NativeJson values;
};

NativeObservedProviderOfferV4
decodeNativeProviderOfferV4(const std::string& wire);

} // namespace ndnsf::di
