#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV4.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeTensorTransportDeclaration.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <algorithm>
#include <cfenv>
#include <clocale>
#include <cstring>
#include <memory>
#include <set>
#if defined(__linux__)
#include <locale.h>
#endif
#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__)) && defined(__SSE__)
#include <xmmintrin.h>
#endif
namespace ndnsf::di {
namespace {
bool reusableDecodeEnvironment()
{
#if defined(__linux__) && defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__)) && defined(__SSE__)
  unsigned short control = 0;
  asm volatile("fnstcw %0" : "=m"(control));
  const auto* numericLocale = std::setlocale(LC_NUMERIC, nullptr);
  return uselocale(static_cast<locale_t>(0)) == LC_GLOBAL_LOCALE &&
    std::locale() == std::locale::classic() &&
    std::fegetround() == FE_TONEAREST && control == 0x037f &&
    (_mm_getcsr() & ~0x3fu) == 0x1f80 && numericLocale &&
    (std::strcmp(numericLocale, "C") == 0 || std::strcmp(numericLocale, "POSIX") == 0);
#else
  return false;
#endif
}
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
bool canonicalName(const std::string& value)
{
  try {
    const ndn::Name name(value);
    return !name.empty() && name.toUri() == value;
  }
  catch (const std::exception&) {
    return false;
  }
}
bool validPairList(const std::vector<NativeJson>& pairs,
                   const std::vector<std::string>& roster)
{
  // verifyV4 has already validated the canonical, sorted, unique roster.
  // Exact membership below also proves each endpoint is canonical.
  if (pairs.size() > 16) return false;
  std::pair<std::string, std::string> previous;
  bool havePrevious = false;
  for (const auto& pair : pairs) {
    if (!pair.is_object() || pair.size() != 2 ||
        !pair.contains("src_provider") || !pair.contains("dst_provider") ||
        !pair.at("src_provider").is_string() || !pair.at("dst_provider").is_string())
      return false;
    const auto source = pair.at("src_provider").get<std::string>();
    const auto destination = pair.at("dst_provider").get<std::string>();
    const std::pair<std::string, std::string> current{source, destination};
    if (source == destination ||
        !std::binary_search(roster.begin(), roster.end(), source) ||
        !std::binary_search(roster.begin(), roster.end(), destination) ||
        (havePrevious && !(previous < current)))
      return false;
    previous = current;
    havePrevious = true;
  }
  return true;
}
bool pairAllowed(const std::vector<NativeJson>& pairs,
                 const std::string& source,
                 const std::string& destination)
{
  return std::any_of(pairs.begin(), pairs.end(), [&] (const NativeJson& pair) {
    return pair.at("src_provider").get<std::string>() == source &&
           pair.at("dst_provider").get<std::string>() == destination;
  });
}
std::string probeScopeDigest(const NativeOfferBindingContext& context)
{
  return nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-placement-probe-scope-v1"},
    {"request_id", context.requestId}, {"attempt", context.attempt},
    {"requester_identity", context.requesterIdentity},
    {"deadline_ms", context.deadlineMs},
    {"service", context.serviceName}, {"model_identity_hash", context.modelDigest},
    {"graph_digest", context.graphDigest},
    {"candidate_set_digest", context.candidateSetDigest},
    {"candidate_artifact_set_digest", context.candidateArtifactSetDigest},
    {"rtt_pairs", context.allowedRttPairs},
    {"goodput_edges", context.allowedGoodputEdges}}));
}
std::string string(const NativeJson& value, const char* field)
{
  const auto result = value.at(field).get<std::string>();
  require(!result.empty());
  return result;
}
NativeJson candidateArtifactIdentityV4(const NativeJson& value)
{
  const std::set<std::string> fields{"artifact_digest", "model_manifest_digest",
    "graph_digest", "role", "rank", "backend", "protection_epoch"};
  keys(value, fields);
  require(value.size() == fields.size());
  NativeJson identity = NativeJson::object();
  for (const auto& field : fields) {
    require(value.contains(field));
    identity[field] = value.at(field);
  }
  return identity;
}
NativeJson residencyCandidateIdentityV4(const NativeJson& row)
{
  NativeJson identity = NativeJson::object();
  for (const auto* field : {"artifact_digest", "model_manifest_digest", "graph_digest",
                            "role", "rank", "backend", "protection_epoch"}) {
    require(row.is_object() && row.contains(field));
    identity[field] = row.at(field);
  }
  return candidateArtifactIdentityV4(identity);
}
bool residencyMatchesCandidateV4(const NativeJson& row,
                                  const NativeJson& candidates)
{
  const auto observed = residencyCandidateIdentityV4(row);
  const auto observedBackend = observed.at("backend").get<std::string>();
  for (const auto& value : candidates) {
    const auto candidate = candidateArtifactIdentityV4(value);
    bool sameArtifact = true;
    for (const auto* field : {"artifact_digest", "model_manifest_digest", "graph_digest",
                              "role", "rank", "protection_epoch"}) {
      if (candidate.at(field) != observed.at(field)) {
        sameArtifact = false;
        break;
      }
    }
    const auto baseBackend = candidate.at("backend").get<std::string>();
    if (sameArtifact && (observedBackend == baseBackend ||
        observedBackend == baseBackend + "-cpu" ||
        observedBackend == baseBackend + "-cuda"))
      return true;
  }
  return false;
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
    // A provider's default NDN certificate key name is generated by the
    // keychain at runtime (for example ``/provider/KEY/ksk-...``), so the
    // policy must be able to bind the whole provider KEY namespace.  Requiring
    // an additional fixed component here made a valid ``/provider/KEY/``
    // prefix unusable even though the subsequent signer-identity and key
    // namespace checks already prevent cross-provider substitution.
    require(under(prefix, provider + "/KEY") &&
            ndn::Name(prefix).size() >= ndn::Name(provider).size() + 1);
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

struct NativeOfferAdmission::V4ContextChecks
{
  bool rosterValidated = false;
  bool artifactsValidated = false;
  bool probeScopeValidated = false;
  NativeJson candidateArtifacts;
  std::string candidateArtifactSetDigest;
};

std::size_t NativeOfferDecodeCache::decodeReuseCount() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_reuses;
}

std::shared_ptr<const NativeObservedProviderOfferV4>
NativeOfferDecodeCache::decode(const std::string& wire)
{
  const bool reusable = reusableDecodeEnvironment();
  if (reusable) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& entry : m_entries) {
      if (entry.first == wire) {
        ++m_reuses;
        return entry.second;
      }
    }
  }
  auto decoded = std::make_shared<const NativeObservedProviderOfferV4>(
    decodeNativeProviderOfferV4(wire));
  if (reusable && reusableDecodeEnvironment()) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& entry : m_entries)
      if (entry.first == wire) return entry.second;
    if (m_entries.size() < 16) m_entries.emplace_back(wire, decoded);
  }
  return decoded;
}

NativeAdmittedOfferV4 NativeOfferAdmission::verifyV4(
  const ndn_service_framework::AckSelectionCandidate& ack,
  const NativeOfferBindingContext& context, std::uint64_t nowMs) const
{
  return verifyV4Impl(ack, context, nowMs, nullptr);
}

std::optional<std::vector<NativeAdmittedOfferV4>> NativeOfferAdmission::tryVerifyV4Batch(
  const std::vector<ndn_service_framework::AckSelectionCandidate>& acks,
  const NativeOfferBindingContext& context, std::uint64_t nowMs,
  std::string* unavailableReason) const
{
  return tryVerifyV4Batch(acks, context, nowMs, unavailableReason, nullptr);
}

std::optional<std::vector<NativeAdmittedOfferV4>> NativeOfferAdmission::tryVerifyV4Batch(
  const std::vector<ndn_service_framework::AckSelectionCandidate>& acks,
  const NativeOfferBindingContext& context, std::uint64_t nowMs,
  std::string* unavailableReason, NativeOfferDecodeCache* decodeCache) const
{
  if (unavailableReason) unavailableReason->clear();
  const auto reject = [unavailableReason](const char* reason)
      -> std::optional<std::vector<NativeAdmittedOfferV4>> {
    if (unavailableReason) *unavailableReason = reason;
    return std::nullopt;
  };
  try {
    if (acks.empty()) return reject("PROVIDER_V4_OFFER_REJECTED");
    // Owned immutable data: no proof survives this batch or aliases mutable
    // caller context across independently authenticated Provider offers.
    const NativeOfferBindingContext frozenContext = context;
    V4ContextChecks checks;
    std::vector<NativeAdmittedOfferV4> result;
    result.reserve(acks.size());
    for (const auto& ack : acks) {
      if (!ack.ack.hasProviderCapabilityOffer() ||
          !ack.ack.getProviderCapabilityOffer().hasField(NATIVE_PROVIDER_OFFER_V4_FIELD))
        return reject("PROVIDER_V4_OFFER_MISSING");
      result.push_back(verifyV4Impl(ack, frozenContext, nowMs, &checks, decodeCache));
    }
    return result;
  }
  catch (const std::exception&) {
    return reject("PROVIDER_V4_OFFER_REJECTED");
  }
}

NativeAdmittedOfferV4 NativeOfferAdmission::verifyV4Impl(
  const ndn_service_framework::AckSelectionCandidate& ack,
  const NativeOfferBindingContext& context, std::uint64_t nowMs,
  V4ContextChecks* checks, NativeOfferDecodeCache* decodeCache) const
{
  require(canonicalName(context.requesterIdentity));
  const auto& auth = ack.authenticationEvidence;
  require(auth.trustSchemaValidated && !auth.signerIdentity.empty() &&
    !auth.signerKeyLocator.empty() && digest(auth.wireDigest),
    "DI_NATIVE_V4_OFFER_REJECTED_UNAUTHENTICATED");
  require(ack.ack.hasProviderCapabilityOffer());
  const auto& capabilities = ack.ack.getProviderCapabilityOffer();
  require(capabilities.hasField(NATIVE_PROVIDER_OFFER_V4_FIELD));
  const auto& wire = capabilities.getField(NATIVE_PROVIDER_OFFER_V4_FIELD);
  std::optional<NativeObservedProviderOfferV4> fresh;
  std::shared_ptr<const NativeObservedProviderOfferV4> decoded;
  if (decodeCache) decoded = decodeCache->decode(wire);
  else fresh = decodeNativeProviderOfferV4(wire);
  const auto& offer = decoded ? *decoded : *fresh;
  const auto entry = m_entries.find({offer.provider, offer.service});
  require(entry != m_entries.end());
  require(auth.signerIdentity == offer.provider &&
          ack.providerName == ndn::Name(offer.provider));
  require(under(auth.signerKeyLocator, entry->second.keyLocatorPrefix) &&
          under(auth.signerKeyLocator, offer.provider + "/KEY"));
  require(ack.serviceName == ndn::Name(offer.service) &&
          context.serviceName == offer.service);
  require(ack.requestId == ndn::Name(offer.requestId) &&
          context.requestId == offer.requestId);
  require(context.attempt != 0 && context.attempt == offer.attempt &&
          digest(context.modelDigest) && context.modelDigest == offer.modelDigest &&
          digest(context.graphDigest) && context.graphDigest == offer.graphDigest);
  require(digest(context.candidateSetDigest) &&
          context.candidateSetDigest == offer.candidateSetDigest);
  if (!checks || !checks->rosterValidated) {
    require(!context.candidateProviders.empty() && context.candidateProviders.size() <= 16 &&
          std::is_sorted(context.candidateProviders.begin(),
                         context.candidateProviders.end()) &&
          std::adjacent_find(context.candidateProviders.begin(),
                             context.candidateProviders.end()) ==
            context.candidateProviders.end() &&
          std::all_of(context.candidateProviders.begin(), context.candidateProviders.end(),
                      canonicalName) &&
          std::find(context.candidateProviders.begin(), context.candidateProviders.end(),
                    offer.provider) != context.candidateProviders.end() &&
          nativePlanningDigest(nativeCanonicalJson(
            NativeJson(context.candidateProviders))) == context.candidateSetDigest);
    if (checks) checks->rosterValidated = true;
  }
  // Membership is offer-specific even when the roster validation is reused.
  require(std::binary_search(context.candidateProviders.begin(),
                             context.candidateProviders.end(), offer.provider));
  NativeJson localArtifacts;
  std::string localArtifactSetDigest;
  auto& candidateArtifacts = checks ? checks->candidateArtifacts : localArtifacts;
  auto& candidateArtifactSetDigest = checks ? checks->candidateArtifactSetDigest : localArtifactSetDigest;
  if (!checks || !checks->artifactsValidated) {
    require(context.candidateArtifacts.size() <= MAX_NATIVE_PLACEMENT_CANDIDATE_ARTIFACTS);
    require(!context.candidateArtifacts.empty());
    candidateArtifacts = NativeJson::array();
    std::string previousCandidate;
    for (const auto& candidate : context.candidateArtifacts) {
      const auto identity = candidateArtifactIdentityV4(candidate);
      const auto canonical = nativeCanonicalJson(identity);
      require(previousCandidate.empty() || previousCandidate < canonical);
      previousCandidate = canonical;
      candidateArtifacts.push_back(identity);
    }
    candidateArtifactSetDigest = nativePlanningDigest(nativeCanonicalJson(candidateArtifacts));
  }
  require(digest(context.candidateArtifactSetDigest) &&
          context.candidateArtifactSetDigest == candidateArtifactSetDigest &&
          candidateArtifactSetDigest == offer.candidateArtifactSetDigest);
  if (checks) checks->artifactsValidated = true;
  require(digest(context.probeScopeDigest) &&
          context.probeScopeDigest == offer.probeScopeDigest);
  if (!checks || !checks->probeScopeValidated) {
    require(context.probeScopeDigest == probeScopeDigest(context) &&
            validPairList(context.allowedRttPairs, context.candidateProviders) &&
            validPairList(context.allowedGoodputEdges, context.candidateProviders));
    if (checks) checks->probeScopeValidated = true;
  }
  for (const auto& row : offer.values.at("link_observations")) {
    require(std::binary_search(context.candidateProviders.begin(),
                               context.candidateProviders.end(),
                               row.at("src_peer").get<std::string>()) &&
            std::binary_search(context.candidateProviders.begin(),
                               context.candidateProviders.end(),
                               row.at("dst_peer").get<std::string>()));
    const auto kind = row.at("kind").get<std::string>();
    const auto source = row.at("src_peer").get<std::string>();
    const auto destination = row.at("dst_peer").get<std::string>();
    require(kind == "ACTIVE_PROBE_RTT"
        ? pairAllowed(context.allowedRttPairs, source, destination)
        : ((kind == "ACTIVE_PROBE_GOODPUT" || kind == "DATA_FETCH") &&
           pairAllowed(context.allowedGoodputEdges, source, destination)));
  }
  for (const auto& row : offer.values.at("residency")) {
    require(residencyMatchesCandidateV4(row, candidateArtifacts));
  }
  require(ack.ack.getStatus() &&
          ack.ack.getStatus() == offer.values.at("status").get<bool>() &&
          !ack.ack.hasReservationLease());
  require(offer.capturedAtMs <= nowMs && offer.expiresAtMs > nowMs &&
          context.deadlineMs > nowMs && offer.expiresAtMs <= context.deadlineMs);
  require(offer.signerKeyId == entry->second.signerKeyId);

  const auto& raw = m_keys.at(offer.signerKeyId);
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
    EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, raw.data(), raw.size()),
    &EVP_PKEY_free);
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> crypto(
    EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  require(key && crypto &&
          EVP_DigestVerifyInit(crypto.get(), nullptr, nullptr, nullptr, key.get()) == 1);
  const auto sig = signature(offer.signature);
  require(EVP_DigestVerify(crypto.get(), sig.data(), sig.size(),
             reinterpret_cast<const unsigned char*>(offer.offerDigest.data()),
             offer.offerDigest.size()) == 1);
  // This optional field belongs to the enclosing, authenticated Core ACK,
  // not the separately signed V4 values. Never cache it by V4 wire alone.
  std::uint64_t tensorSegmentBytes = 0;
  if (capabilities.hasField(NATIVE_PROVIDER_TENSOR_TRANSPORT_V1_FIELD)) {
    try {
      const auto& transportWire = capabilities.getField(
        NATIVE_PROVIDER_TENSOR_TRANSPORT_V1_FIELD);
      tensorSegmentBytes = detail::nativeTensorTransportSegmentBytesV1(
        transportWire, ndn::MAX_NDN_PACKET_SIZE, offer.bootEpoch);
    }
    catch (const std::exception&) {
      // Unsupported transport metadata cannot invalidate normal V4 admission.
    }
  }
  return NativeAdmittedOfferV4(decoded ? offer : std::move(*fresh), auth.signerIdentity,
                               auth.signerKeyLocator, auth.wireDigest, tensorSegmentBytes);
}
} // namespace ndnsf::di
