#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <algorithm>
#include <memory>
#include <set>
namespace ndnsf::di {
namespace {
void require(bool okay, const char* message = "DI_NATIVE_OFFER_REJECTED")
{
  if (!okay) throw std::runtime_error(message);
}
bool digest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
void keys(const NativeJson& value, const std::set<std::string>& allowed)
{
  require(value.is_object());
  for (auto it = value.begin(); it != value.end(); ++it) require(allowed.count(it.key()));
}
bool under(const std::string& name, const std::string& prefix)
{
  return ndn::Name(prefix).isPrefixOf(ndn::Name(name));
}
std::string string(const NativeJson& value, const char* field)
{
  const auto result = value.at(field).get<std::string>();
  require(!result.empty());
  return result;
}
std::array<unsigned char, 64> signature(const std::string& encoded)
{
  // Ed25519 signatures are exactly 64 bytes (88 base64 bytes ending with ==).
  require(encoded.size() == 88 && encoded.substr(86) == "==");
  require(std::all_of(encoded.begin(), encoded.begin() + 86, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '/';
  }));
  std::array<unsigned char, 66> decoded{};
  require(EVP_DecodeBlock(decoded.data(),
    reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size()) == 66);
  std::array<unsigned char, 89> canonical{};
  require(EVP_EncodeBlock(canonical.data(), decoded.data(), 64) == 88);
  require(encoded == reinterpret_cast<const char*>(canonical.data()));
  std::array<unsigned char, 64> result{};
  std::copy_n(decoded.begin(), result.size(), result.begin());
  return result;
}
}
NativeOfferAdmission::NativeOfferAdmission(
  const std::string& policyJson, const std::map<std::string, std::string>& publicKeyPemById,
  const std::string& candidateDigest)
{
  const auto policy = nativeParseJson(policyJson);
  keys(policy, {"schema", "candidateId", "candidateDigest", "trustSchema", "entries"});
  require(string(policy, "schema") == "spec180-provider-offer-trust-v1");
  const auto candidateId = string(policy, "candidateId");
  require(candidateId.find('/') == std::string::npos && candidateId.find(char(92)) == std::string::npos);
  require(digest(candidateDigest) && string(policy, "candidateDigest") == candidateDigest);
  require(string(policy, "trustSchema").front() == '/');
  for (const auto& item : publicKeyPemById) {
    require(item.second.size() <= 65536);
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(
      BIO_new_mem_buf(item.second.data(), static_cast<int>(item.second.size())), &BIO_free);
    require(bool(bio));
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
      PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr), &EVP_PKEY_free);
    require(key && EVP_PKEY_base_id(key.get()) == EVP_PKEY_ED25519);
    std::array<unsigned char, 32> raw{};
    std::size_t length = raw.size();
    require(EVP_PKEY_get_raw_public_key(key.get(), raw.data(), &length) == 1 && length == raw.size());
    require(item.first == nativePlanningDigest(std::string(
      reinterpret_cast<const char*>(raw.data()), raw.size())));
    m_keys.emplace(item.first, raw);
  }
  const auto& entries = policy.at("entries");
  require(entries.is_array() && !entries.empty());
  for (const auto& entry : entries) {
    keys(entry, {"provider", "service", "keyLocatorPrefix", "signerKeyId", "certificateName"});
    const auto provider = string(entry, "provider"), service = string(entry, "service");
    const auto prefix = string(entry, "keyLocatorPrefix"), keyId = string(entry, "signerKeyId");
    require(provider.front() == '/' && service.front() == '/' && prefix.front() == '/');
    require(under(prefix, provider + "/KEY") && ndn::Name(prefix).size() > ndn::Name(provider).size() + 1);
    require(under(string(entry, "certificateName"), prefix) && m_keys.count(keyId));
    require(m_entries.emplace(std::make_pair(provider, service), Entry{prefix, keyId}).second);
  }
}
NativeAdmittedOfferV3 NativeOfferAdmission::verify(
  const ndn_service_framework::AckSelectionCandidate& ack,
  const NativeOfferBindingContext& context, std::uint64_t nowMs) const
{
  const auto& auth = ack.authenticationEvidence;
  require(auth.trustSchemaValidated && !auth.signerIdentity.empty() &&
    !auth.signerKeyLocator.empty() && digest(auth.wireDigest),
    "DI_NATIVE_OFFER_REJECTED_UNAUTHENTICATED");
  const auto bytes = ack.ack.getPayload();
  const auto offer = decodeNativeProviderOfferV3(std::string(bytes.begin(), bytes.end()));
  const auto entry = m_entries.find({offer.provider, offer.service});
  require(entry != m_entries.end());
  require(auth.signerIdentity == offer.provider && ack.providerName == ndn::Name(offer.provider));
  require(under(auth.signerKeyLocator, entry->second.keyLocatorPrefix) &&
          under(auth.signerKeyLocator, offer.provider + "/KEY"));
  require(ack.serviceName == ndn::Name(offer.service) && context.serviceName == offer.service);
  require(ack.requestId == ndn::Name(offer.requestId) && context.requestId == offer.requestId);
  require(context.attempt && context.attempt == offer.attempt &&
          digest(context.modelDigest) && context.modelDigest == offer.modelDigest);
  require(digest(context.graphDigest) && (offer.graphDigest == context.graphDigest ||
          offer.graphDigest == "sha256:" + std::string(64, '0')));
  require(ack.ack.getStatus() == offer.status && !ack.ack.hasReservationLease());
  require(offer.capturedAtMs <= nowMs && offer.expiresAtMs > nowMs &&
          context.deadlineMs > nowMs && offer.expiresAtMs >= context.deadlineMs);
  require(offer.signerKeyId == entry->second.signerKeyId);
  const auto& raw = m_keys.at(offer.signerKeyId);
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
    EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, raw.data(), raw.size()), &EVP_PKEY_free);
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> crypto(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  require(key && crypto && EVP_DigestVerifyInit(crypto.get(), nullptr, nullptr, nullptr, key.get()) == 1);
  const auto sig = signature(offer.signature);
  require(EVP_DigestVerify(crypto.get(), sig.data(), sig.size(),
    reinterpret_cast<const unsigned char*>(offer.offerDigest.data()), offer.offerDigest.size()) == 1);
  return NativeAdmittedOfferV3(offer);
}
} // namespace ndnsf::di
