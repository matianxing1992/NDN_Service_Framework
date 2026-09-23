#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactPolicyAuthority.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <stdexcept>

namespace ndnsf::di {
namespace {

NativeJson
requestValue(const NativeSignedGrantRequest& request)
{
  return NativeJson{
    {"providerIdentity", request.providerIdentity},
    {"requestId", request.requestId},
    {"attempt", request.attempt},
    {"planCoreDigest", request.planCoreDigest},
    {"grantViewDigest", request.grantViewDigest},
    {"modelManifestDigest", request.modelManifestDigest},
    {"protectionEpoch", request.protectionEpoch},
    {"requesterIdentity", request.requesterIdentity},
    {"issuedAtMs", request.issuedAtMs},
    {"allowedResidencyTiers", request.allowedResidencyTiers},
    {"purpose", request.purpose},
    {"requesterSignature", request.requesterSignature},
  };
}

NativeSignedGrantRequest
requestFromValue(const NativeJson& value)
{
  if (!value.is_object()) {
    throw std::invalid_argument("grant authority request must be an object");
  }
  NativeSignedGrantRequest request;
  request.providerIdentity = value.at("providerIdentity").get<std::string>();
  request.requestId = value.at("requestId").get<std::string>();
  request.attempt = value.at("attempt").get<std::uint64_t>();
  request.planCoreDigest = value.at("planCoreDigest").get<std::string>();
  request.grantViewDigest = value.at("grantViewDigest").get<std::string>();
  request.modelManifestDigest = value.at("modelManifestDigest").get<std::string>();
  request.protectionEpoch = value.at("protectionEpoch").get<std::string>();
  request.requesterIdentity = value.at("requesterIdentity").get<std::string>();
  request.issuedAtMs = value.at("issuedAtMs").get<std::uint64_t>();
  request.allowedResidencyTiers = value.at("allowedResidencyTiers").get<std::vector<std::string>>();
  request.purpose = value.at("purpose").get<std::string>();
  request.requesterSignature = value.at("requesterSignature").get<std::string>();
  return request;
}

} // namespace

std::string
nativeGrantAuthorityRequestJson(const NativeGrantAuthorityRequest& value)
{
  if (value.expiresAtMs == 0 ||
      value.publishedManifestJson.size() > NativeGrantInlineManifestMaxBytes) {
    throw std::invalid_argument("grant authority request expiry or manifest is invalid");
  }
  const NativeJson envelope{
    {"schema", "ndnsf-di-native-grant-authority-request-v1"},
    {"request", requestValue(value.request)},
    {"expiresAtMs", value.expiresAtMs},
    {"publishedManifestJson", value.publishedManifestJson},
  };
  return nativeCanonicalJson(envelope);
}

NativeGrantAuthorityRequest
nativeGrantAuthorityRequestFromJson(const std::string& wire)
{
  const auto value = nativeParseJson(wire);
  if (value.value("schema", std::string{}) !=
      "ndnsf-di-native-grant-authority-request-v1") {
    throw std::invalid_argument("unsupported grant authority request schema");
  }
  NativeGrantAuthorityRequest result;
  result.request = requestFromValue(value.at("request"));
  result.expiresAtMs = value.at("expiresAtMs").get<std::uint64_t>();
  result.publishedManifestJson = value.at("publishedManifestJson").get<std::string>();
  if (result.expiresAtMs == 0 ||
      result.publishedManifestJson.size() > NativeGrantInlineManifestMaxBytes ||
      nativeGrantAuthorityRequestJson(result) != wire) {
    throw std::invalid_argument("grant authority request is not canonical");
  }
  return result;
}

std::string
nativeKeyGrantJson(const NativeKeyGrant& grant)
{
  if (grant.grantName.empty() || grant.grantDigest.empty() || grant.recipient.empty() ||
      grant.wireJson.empty() || grant.expiresAtMs == 0) {
    throw std::invalid_argument("native key grant is incomplete");
  }
  return nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-native-key-grant-v1"},
    {"grantName", grant.grantName},
    {"grantDigest", grant.grantDigest},
    {"recipient", grant.recipient},
    {"wireJson", grant.wireJson},
    {"expiresAtMs", grant.expiresAtMs},
  });
}

NativeKeyGrant
nativeKeyGrantFromJson(const std::string& wire)
{
  const auto value = nativeParseJson(wire);
  if (value.value("schema", std::string{}) != "ndnsf-di-native-key-grant-v1") {
    throw std::invalid_argument("unsupported native key grant schema");
  }
  NativeKeyGrant grant;
  grant.grantName = value.at("grantName").get<std::string>();
  grant.grantDigest = value.at("grantDigest").get<std::string>();
  grant.recipient = value.at("recipient").get<std::string>();
  grant.wireJson = value.at("wireJson").get<std::string>();
  grant.expiresAtMs = value.at("expiresAtMs").get<std::uint64_t>();
  if (nativeKeyGrantJson(grant) != wire) {
    throw std::invalid_argument("native key grant response is not canonical");
  }
  return grant;
}

} // namespace ndnsf::di
