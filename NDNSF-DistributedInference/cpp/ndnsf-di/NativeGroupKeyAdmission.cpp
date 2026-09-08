#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGroupKeyAdmission.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include <ndn-cxx/security/transform/public-key.hpp>
#include <algorithm>

namespace ndnsf::di {
namespace {
void require(bool value)
{
  if (!value) throw std::invalid_argument("DI_NATIVE_GROUP_KEY_OFFER_REJECTED");
}
}
NativeGroupKeyAdmission::NativeGroupKeyAdmission(const NativeOfferAdmission& admission,
  const std::vector<ndn_service_framework::AckSelectionCandidate>& acks,
  const NativeOfferBindingContext& context, std::uint64_t nowMs)
{
  auto entries = std::make_shared<std::map<std::string, Entry>>();
  for (const auto& ack : acks) {
    auto admitted = admission.verify(ack, context, nowMs);
    const auto& observed = admitted.observation();
    require(ack.ack.hasSelectionInputKeyOffer() && ack.ack.getSelectionInputKeyOffer().getVersion() == 1);
    const auto& fields = ack.ack.getSelectionInputKeyOffer().getFields();
    const auto field = [&](const char* name) -> const std::string& {
      const auto it = fields.find(name);
      require(it != fields.end() && !it->second.empty());
      return it->second;
    };
    require(field("schemaVersion") == "1" && field("recipient") == observed.provider);
    const auto& epoch = field("providerBootEpoch");
    require(epoch == observed.bootEpoch || epoch == observed.provider + ":" + observed.bootEpoch);
    const auto& cert = field("recipientCertName");
    require(cert.front() == '/' && ndn::Name(observed.provider + "/KEY").isPrefixOf(ndn::Name(cert)));
    auto prefix = field("ndnsfDataV1EndpointPrefix");
    while (prefix.size() > 1 && prefix.back() == '/') prefix.pop_back();
    require(prefix.front() == '/' && ndn::Name(observed.provider).isPrefixOf(ndn::Name(prefix)));
    const auto& hex = field("recipientPublicKey");
    require(hex.size() % 2 == 0 && hex.size() <= 8192 &&
      std::all_of(hex.begin(), hex.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      }));
    const auto decoded = ndn_service_framework::selectionGatedUnhex(hex);
    require(field("recipientCertDigest") == nativePlanningDigest(
      std::string(reinterpret_cast<const char*>(decoded.data()), decoded.size())));
    ndn::security::transform::PublicKey publicKey;
    publicKey.loadPkcs8(decoded);
    require(publicKey.getKeyType() == ndn::KeyType::RSA);
    // No mutable alias to ACK fields or the caller's key map survives admission.
    const auto provider = observed.provider;
    require(entries->emplace(provider, Entry{std::move(admitted), std::move(prefix),
      ProviderGroupBytes(decoded.begin(), decoded.end())}).second);
  }
  m_entries = std::move(entries);
}

const NativeAdmittedOfferV3& NativeGroupKeyAdmission::offer(const std::string& provider) const
{
  return m_entries->at(provider).offer;
}
const std::string& NativeGroupKeyAdmission::endpoint(const std::string& provider) const
{
  return m_entries->at(provider).endpoint;
}
ProviderGroupCoordinatorOptions NativeGroupKeyAdmission::options() const
{
  ProviderGroupCoordinatorOptions options;
  options.wrapEpochKey = [entries = m_entries](const std::string& provider, const ProviderGroupBytes& key) {
    if (key.size() != 32) throw std::invalid_argument("group epoch key must be 32 bytes");
    const auto& publicKey = entries->at(provider).publicKey;
    const auto wrapped = ndn_service_framework::wrapSelectionGatedInputKey(
      ndn::Buffer(key.begin(), key.end()), publicKey);
    return ProviderGroupBytes(wrapped.begin(), wrapped.end());
  };
  return options;
}
} // namespace ndnsf::di
