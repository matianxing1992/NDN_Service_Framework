#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV4.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRunnerReuseCache.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanSealer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderArtifactCache.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeSessionCache.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"
#include <limits>
#include <locale>

#include <boost/test/unit_test.hpp>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cfenv>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <boost/asio/post.hpp>
#if defined(__linux__)
#include <locale.h>
#endif
#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__)) && defined(__SSE__)
#include <xmmintrin.h>
#endif

namespace ndn_service_framework {

struct ServiceProviderTestAccess
{
  static void useSigningKeyChainForPeerProbe(ServiceProvider& provider,
                                             ndn::KeyChain& keyChain)
  {
    provider.m_testSigningKeyChain = &keyChain;
  }
};

} // namespace ndn_service_framework

namespace {
using namespace ndnsf::di;
using ndn_service_framework::AckSelectionCandidate;

struct TestKey
{
  TestKey()
  {
    std::array<unsigned char, 32> seed{};
    for (std::size_t i = 0; i < seed.size(); ++i)
      seed[i] = static_cast<unsigned char>(i + 1);
    key = std::shared_ptr<EVP_PKEY>(
      EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                   seed.data(), seed.size()), EVP_PKEY_free);
    if (!key)
      throw std::runtime_error("failed to create Spec194 Ed25519 test key");
    std::array<unsigned char, 32> raw{};
    std::size_t rawSize = raw.size();
    if (EVP_PKEY_get_raw_public_key(key.get(), raw.data(), &rawSize) != 1 ||
        rawSize != raw.size())
      throw std::runtime_error("failed to export Spec194 test public key");
    keyId = nativePlanningDigest(std::string(
      reinterpret_cast<const char*>(raw.data()), raw.size()));
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), &BIO_free);
    if (!bio || PEM_write_bio_PUBKEY(bio.get(), key.get()) != 1)
      throw std::runtime_error("failed to encode Spec194 test public key");
    char* bytes = nullptr;
    const auto length = BIO_get_mem_data(bio.get(), &bytes);
    publicPem.assign(bytes, static_cast<std::size_t>(length));
  }

  std::string sign(const std::string& message) const
  {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(
      EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1)
      throw std::runtime_error("failed to initialize Spec194 test signer");
    std::array<unsigned char, 64> signature{};
    std::size_t size = signature.size();
    if (EVP_DigestSign(ctx.get(), signature.data(), &size,
          reinterpret_cast<const unsigned char*>(message.data()), message.size()) != 1 ||
        size != signature.size())
      throw std::runtime_error("failed to sign Spec194 test offer");
    std::string encoded(88, '\0');
    if (EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                        signature.data(), signature.size()) != 88)
      throw std::runtime_error("failed to encode Spec194 test signature");
    return encoded;
  }

  std::shared_ptr<EVP_PKEY> key;
  std::string keyId;
  std::string publicPem;
};

struct Fixture
{
  explicit Fixture(std::vector<std::string> providers = {"/provider/a", "/provider/b"})
    : roster(std::move(providers)) {}
  TestKey signer;
  const std::vector<std::string> roster{"/provider/a", "/provider/b"};
  const std::string requestId = "/request/spec194/1";
  const std::string requesterIdentity = "/spec194/user";
  const std::string service = "/service/spec194";
  const std::string modelDigest = "sha256:" + std::string(64, 'a');
  const std::string graphDigest = "sha256:" + std::string(64, 'b');
  const std::string candidateGraphDigest = nativePlanningDigest("canonical-candidate-graph");
  const std::string candidateSetDigest = nativePlanningDigest(
    nativeCanonicalJson(NativeJson(roster)));

  NativeOfferAdmission makeAdmission(const std::string& extraProvider = {}) const
  {
    const auto candidateDigest = nativePlanningDigest("spec194-v4-candidate");
    NativeJson policy = {
      {"schema", "spec180-provider-offer-trust-v1"},
      {"candidateId", "spec194-v4"},
      {"candidateDigest", candidateDigest},
      {"trustSchema", "/trust/spec194/v1"},
      {"entries", NativeJson::array({NativeJson{
        {"provider", "/provider/a"},
        {"service", service},
        {"keyLocatorPrefix", "/provider/a/KEY"},
        {"signerKeyId", signer.keyId},
        {"certificateName", "/provider/a/KEY/spec194/ISSUER/v=1"}}, NativeJson{
        {"provider", "/provider/b"},
        {"service", service},
        {"keyLocatorPrefix", "/provider/b/KEY"},
        {"signerKeyId", signer.keyId},
        {"certificateName", "/provider/b/KEY/spec194/ISSUER/v=1"}}})}};
    if (!extraProvider.empty())
      policy["entries"].push_back(NativeJson{
        {"provider", extraProvider}, {"service", service},
        {"keyLocatorPrefix", extraProvider + "/KEY"}, {"signerKeyId", signer.keyId},
        {"certificateName", extraProvider + "/KEY/spec194/ISSUER/v=1"}});
    return NativeOfferAdmission(nativeCanonicalJson(policy),
      {{signer.keyId, signer.publicPem}}, candidateDigest);
  }

  NativeOfferBindingContext binding() const
  {
    return binding(std::vector<NativeJson>{candidateArtifact()});
  }

  NativeOfferBindingContext binding(
      const std::vector<NativeJson>& candidateArtifacts) const
  {
    NativeOfferBindingContext context;
    context.requestId = requestId;
    context.requesterIdentity = requesterIdentity;
    context.attempt = 1;
    context.serviceName = service;
    context.modelDigest = modelDigest;
    context.graphDigest = graphDigest;
    context.deadlineMs = 10000;
    context.candidateSetDigest = candidateSetDigest;
    context.candidateProviders = roster;
    context.candidateArtifacts = candidateArtifacts;
    context.candidateArtifactSetDigest = nativePlanningDigest(
      nativeCanonicalJson(NativeJson(context.candidateArtifacts)));
    context.allowedRttPairs = rttPairs();
    context.allowedGoodputEdges = goodputEdges();
    context.probeScopeDigest = probeScopeDigest(
      context.candidateArtifactSetDigest);
    return context;
  }

  NativeJson rttPairs() const
  {
    return NativeJson::array({
      NativeJson{{"src_provider", roster[0]}, {"dst_provider", roster[1]}},
      NativeJson{{"src_provider", roster[1]}, {"dst_provider", roster[0]}}});
  }

  NativeJson goodputEdges() const
  {
    return rttPairs();
  }

  std::string probeScopeDigest(const std::string& artifactDigest,
                               std::uint64_t deadlineMs = 10000,
                               const std::string& requester = "/spec194/user") const
  {
    return nativePlanningDigest(nativeCanonicalJson(NativeJson{
      {"schema", "ndnsf-di-placement-probe-scope-v1"},
      {"request_id", requestId}, {"attempt", 1}, {"service", service},
      {"requester_identity", requester}, {"deadline_ms", deadlineMs},
      {"model_identity_hash", modelDigest}, {"graph_digest", graphDigest},
      {"candidate_set_digest", candidateSetDigest},
      {"candidate_artifact_set_digest", artifactDigest},
      {"rtt_pairs", rttPairs()}, {"goodput_edges", goodputEdges()}}));
  }

  NativeJson candidateArtifact() const
  {
    return NativeJson{{"artifact_digest", "sha256:" + std::string(64, 'c')},
      {"model_manifest_digest", "sha256:" + std::string(64, 'd')},
      {"graph_digest", candidateGraphDigest}, {"role", "encoder"}, {"rank", 0},
      {"backend", "onnxruntime"},
      {"protection_epoch", "plaintext-v1"}};
  }

  NativePreparedPlanningCache preparedCache(
      const std::vector<NativeJson>& artifacts) const
  {
    NativePreparedPlanningCache cache;
    cache.splitter = {"spec194-test-splitter", "1",
      nativePlanningDigest("spec194-test-splitter")};
    NativePreparedPlanningCandidate prepared;
    prepared.candidate.graphDigest = candidateGraphDigest;
    prepared.candidate.splitter = cache.splitter;
    std::vector<std::string> roleNames;
    for (const auto& artifact : artifacts) {
      NativeSelectionRoleV3 role;
      role.role = artifact.at("role").get<std::string>();
      role.selectedRole = role.role;
      role.rank = artifact.at("rank").get<std::uint64_t>();
      role.backend = artifact.at("backend").get<std::string>();
      role.artifactDigest = artifact.at("artifact_digest").get<std::string>();
      role.modelManifestDigest = artifact.at("model_manifest_digest").get<std::string>();
      role.graphDigest = artifact.at("graph_digest").get<std::string>();
      role.protectionEpoch = artifact.at("protection_epoch").get<std::string>();
      roleNames.push_back(role.selectedRole);
      prepared.roles.push_back(std::move(role));
    }
    prepared.candidate.executionPlan.roles = roleNames;
    for (std::size_t i = 0; i < roleNames.size(); ++i)
      prepared.candidate.nodeRoles["node-" + std::to_string(i)] = roleNames[i];
    if (roleNames.size() > 1) {
      prepared.candidate.executionPlan.dependencies.emplace_back(
        std::vector<std::string>{roleNames[0]},
        std::vector<std::string>{roleNames[1]},
        "spec194-test", "/spec194/test", "activation", 1, 4096,
        std::vector<std::string>{"activation-tensor"});
    }
    prepared.candidate.candidateDigest = nativePlanningDigest("prepared-test-candidate");
    cache.candidates.push_back(std::move(prepared));
    return cache;
  }

  NativeJson residencyRow() const
  {
    auto row = candidateArtifact();
    row["backend"] = "onnxruntime-cpu";
    row["device_set"] = NativeJson::array({"cpu"});
    row.update(NativeJson{{"state", "CANONICAL_ONLY"},
      {"process_epoch", "boot-provider-a-1"}, {"captured_at_ms", 950},
      {"expires_at_ms", 5000}, {"missing_verified_bytes", 1024},
      {"estimated_assembly_ms", 25.0}, {"estimated_load_ms", nullptr},
      {"runtime_generation", nullptr}});
    return row;
  }

  std::vector<std::uint8_t> requestPayload() const
  {
    return requestPayload(std::vector<NativeJson>{candidateArtifact()});
  }

  std::vector<std::uint8_t> requestPayload(
      const std::vector<NativeJson>& candidates) const
  {
    const auto artifactDigest = nativePlanningDigest(
      nativeCanonicalJson(NativeJson(candidates)));
    const auto rtt = rttPairs();
    const auto goodput = goodputEdges();
    const NativeJson request = {
      {"schema", "ndnsf-di-request-envelope-v2"},
      {"request_id", requestId},
      {"attempt", 1},
      {"service", service},
      {"requester_identity", requesterIdentity},
      {"model_identity_hash", modelDigest},
      {"graph_digest", graphDigest},
      {"candidate_set_digest", candidateSetDigest},
      {"candidate_providers", roster},
      {"candidate_artifacts", candidates},
      {"candidate_artifact_set_digest", artifactDigest},
      {"rtt_pairs", rtt}, {"goodput_edges", goodput},
      {"probe_scope_digest", probeScopeDigest(artifactDigest, 10000,
                                                requesterIdentity)},
      {"plan_deadline_ms", 10000}};
    const auto wire = nativeCanonicalJson(request);
    return {wire.begin(), wire.end()};
  }

  NativeProviderOfferV4Config offerConfig(std::string provider = {}) const
  {
    NativeProviderOfferV4Config config;
    if (provider.empty()) provider = roster.front();
    config.provider = std::move(provider);
    config.service = service;
    config.bootEpoch = config.provider == roster.front() ?
      "boot-provider-a-1" : "boot-provider-b-1";
    config.signerKeyId = signer.keyId;
    config.signDigest = [this] (const std::string& digest) {
      return signer.sign(digest);
    };
    return config;
  }

  AckSelectionCandidate ackFor(const std::string& value,
                               std::string provider = {}) const
  {
    if (provider.empty()) provider = roster.front();
    AckSelectionCandidate ack;
    ack.providerName = ndn::Name(provider);
    ack.serviceName = ndn::Name(service);
    ack.requestId = ndn::Name(requestId);
    ack.ack.setStatus(true);
    ndn_service_framework::ProviderCapabilityOffer capabilities;
    capabilities.setField(NATIVE_PROVIDER_OFFER_V4_FIELD, value);
    ack.ack.setProviderCapabilityOffer(capabilities);
    ack.authenticationEvidence = {
      provider, provider + "/KEY/spec194/ISSUER/v=1",
      nativePlanningDigest("authenticated-spec194-ack-" + provider), true};
    return ack;
  }

  NativeJson dataFetchRow() const
  {
    NativeJson row = {
      {"src_peer", "/provider/b"}, {"dst_peer", "/provider/a"},
      {"direction_semantics", "PAYLOAD_SOURCE_TO_RECEIVER"},
      {"kind", "DATA_FETCH"}, {"observer_peer", "/provider/a"},
      {"observer_boot_epoch", "boot-provider-a-1"},
      {"src_boot_epoch", nullptr},
      {"dst_boot_epoch", "boot-provider-a-1"}, {"topology_digest", nullptr},
      {"captured_at_ms", 950}, {"expires_at_ms", 5000}, {"sample_count", 1},
      {"rtt_ms", nullptr}, {"first_byte_ms", 40.0}, {"elapsed_ms", 80.0},
      {"payload_bytes", 8000}, {"wire_bytes", 9000}, {"encoded_bytes", 8500},
      {"received_segments", 4}, {"probe_profile_digest", nullptr},
      {"probe_segments_sent", nullptr}, {"probe_complete", nullptr},
      {"probe_signature_valid", nullptr}, {"probe_rtt_ms", nullptr},
      {"probe_interest_gaps_ms", nullptr},
      {"probe_data_arrival_gaps_ms", nullptr}, {"probe_data_wire_bytes", nullptr},
      {"timeout_count", 0}, {"nack_count", 0}, {"goodput_mbps", 0.8},
      {"source_digest", ""}};
    row.erase("source_digest");
    row["source_digest"] = nativePlanningDigest(nativeCanonicalJson(row));
    return row;
  }

  NativeProviderTelemetrySnapshot telemetry() const
  {
    return {};
  }

  NativeProviderTelemetrySnapshot measuredTelemetry(
      const std::string& provider, const std::string& boot,
      const std::string& topology) const
  {
    NativeProviderTelemetrySnapshot value;
    value.sampledAtMs = 2990;
    value.sequence = 1;
    value.resources.status = ResourceProbeStatus::Measured;
    value.resources.source = "spec194-test-probe";
    value.resources.providerName = provider;
    value.resources.providerBootId = boot;
    value.resources.sequence = 1;
    value.resources.measuredAtMs = 2990;
    value.resources.hostTotalMemoryBytes = 1024;
    value.resources.hostAvailableMemoryBytes = 512;
    value.resources.topologyDigest = topology;
    return value;
  }
};
}

BOOST_AUTO_TEST_SUITE(Spec194V4Offer)

BOOST_AUTO_TEST_CASE(UnsignedDigestReusesOnlyCanonicalRootSignatureBytes)
{
  Fixture f;
  const auto check = [&](const std::string& wire) {
    const auto decoded = decodeNativeProviderOfferV4(wire);
    auto unsignedValue = decoded.values;
    BOOST_CHECK_EQUAL(decoded.signature, unsignedValue.at("signature").get<std::string>());
    unsignedValue.erase("signature");
    BOOST_CHECK_EQUAL(decoded.offerDigest,
      nativePlanningDigest(nativeCanonicalJson(unsignedValue)));
    BOOST_CHECK_EQUAL(nativeCanonicalJson(decoded.values), wire);
  };
  struct RestoreRounding {
    int mode = std::fegetround();
    ~RestoreRounding() { std::fesetround(mode); }
  } restore;
  for (const int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    BOOST_REQUIRE_EQUAL(std::fesetround(mode), 0);
    auto config = f.offerConfig();
    // Quotes, brackets and apparent members remain inside a string token.
    config.bootEpoch = "boot-{[\\\"signature\\\":\\\"nested\\\"]}-\n\"signature\":true";
    const auto issued = issueNativeProviderOfferV4(
      f.requestPayload(), config, f.telemetry(), 1000);
    BOOST_REQUIRE(issued);
    check(issued->capabilityFieldValue);
    for (const auto& wire : issued->fallbackCapabilityFieldValues) check(wire);
    auto value = nativeParseJson(issued->capabilityFieldValue);
    for (const auto& signature : {std::string("sig\\\"},{\"signature\":\"shadow\""),
                                   std::string("escape\\\n\t\"\r")}) {
      value["signature"] = signature;
      check(nativeCanonicalJson(value));
    }
  }
  BOOST_REQUIRE_EQUAL(std::fesetround(FE_TONEAREST), 0);
  struct CommaDecimal : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
  };
  struct RestoreLocale {
    std::locale prior;
    ~RestoreLocale() { std::locale::global(prior); }
  } restoreLocale;
  std::locale::global(std::locale(std::locale::classic(), new CommaDecimal));
  for (std::uint64_t i = 1; i <= 128; ++i) {
    auto row = f.dataFetchRow();
    row["first_byte_ms"] = i / 17.0;
    row["elapsed_ms"] = 30.0 + i / 11.0;
    row["goodput_mbps"] = 8000.0 * 8.0 / row.at("elapsed_ms").get<double>() / 1000.0;
    row.erase("source_digest");
    row["source_digest"] = nativePlanningDigest(nativeCanonicalJson(row));
    const auto issued = issueNativeProviderOfferV4(
      f.requestPayload(), f.offerConfig(), f.telemetry(), 1000, {row});
    BOOST_REQUIRE(issued);
    check(issued->capabilityFieldValue);
    for (const auto& wire : issued->fallbackCapabilityFieldValues) check(wire);
  }
  const auto plain = issueNativeProviderOfferV4(
    f.requestPayload(), f.offerConfig(), f.telemetry(), 1000);
  BOOST_REQUIRE(plain);
  auto whitespace = plain->capabilityFieldValue;
  whitespace.insert(1, " ");
  BOOST_CHECK_EXCEPTION(decodeNativeProviderOfferV4(whitespace), std::invalid_argument,
    [](const std::invalid_argument& error) {
      return std::string(error.what()) == "V4 Provider offer is not canonical JSON";
    });
  auto duplicate = plain->capabilityFieldValue;
  duplicate.insert(1, "\"signature\":\"duplicate\",");
  // Duplicate keys are rejected by the parser before canonical validation.
  BOOST_CHECK_THROW(decodeNativeProviderOfferV4(duplicate), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RequestOwnedDecodeReuseNeverReusesAdmissionAuthority)
{
  Fixture f;
  const auto issued = issueNativeProviderOfferV4(
    f.requestPayload(), f.offerConfig(), f.telemetry(), 1000, {f.dataFetchRow()});
  BOOST_REQUIRE(issued);
  const auto ack = f.ackFor(issued->capabilityFieldValue);
  const auto admission = f.makeAdmission();
  const auto context = f.binding();
  NativeOfferDecodeCache cache;
  const auto check = [&](const AckSelectionCandidate& candidate,
                         const NativeOfferBindingContext& binding, std::uint64_t now) {
    std::string freshReason, cachedReason;
    const auto fresh = admission.tryVerifyV4Batch({candidate}, binding, now, &freshReason);
    const auto cached = admission.tryVerifyV4Batch({candidate}, binding, now, &cachedReason, &cache);
    BOOST_CHECK_EQUAL(freshReason, cachedReason);
    BOOST_CHECK_EQUAL(bool(fresh), bool(cached));
    if (fresh && cached) {
      BOOST_CHECK_EQUAL(nativeCanonicalJson(fresh->front().observation().values),
                        nativeCanonicalJson(cached->front().observation().values));
      BOOST_CHECK_EQUAL(fresh->front().observation().offerDigest,
                        cached->front().observation().offerDigest);
      BOOST_CHECK_EQUAL(fresh->front().ackWireDigest(), cached->front().ackWireDigest());
    }
  };
  check(ack, context, 1200);
  BOOST_CHECK_EQUAL(cache.decodeReuseCount(), 0);
  check(ack, context, 1201);
#if defined(__linux__) && defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__)) && defined(__SSE__)
  BOOST_REQUIRE_GT(cache.decodeReuseCount(), 0);
#else
  BOOST_CHECK_EQUAL(cache.decodeReuseCount(), 0);
#endif
  const auto candidateDigest = nativePlanningDigest("spec194-v4-candidate");
  const NativeJson changedPolicy = {
    {"schema", "spec180-provider-offer-trust-v1"}, {"candidateId", "spec194-v4"},
    {"candidateDigest", candidateDigest}, {"trustSchema", "/trust/spec194/v1"},
    {"entries", NativeJson::array({NativeJson{
      {"provider", "/provider/a"}, {"service", f.service},
      {"keyLocatorPrefix", "/provider/a/KEY/replaced"}, {"signerKeyId", f.signer.keyId},
      {"certificateName", "/provider/a/KEY/replaced/ISSUER/v=1"}}})}};
  const NativeOfferAdmission changedAdmission(nativeCanonicalJson(changedPolicy),
    {{f.signer.keyId, f.signer.publicPem}}, candidateDigest);
  BOOST_CHECK(!changedAdmission.tryVerifyV4Batch({ack}, context, 1200, nullptr, &cache));
  check(ack, context, 10000);
  for (int i = 0; i < 7; ++i) {
    auto changed = context;
    switch (i) {
      case 0: changed.attempt = 2; break;
      case 1: changed.requesterIdentity = "invalid"; break;
      case 2: changed.requestId += "-new"; break;
      case 3: changed.modelDigest = "sha256:" + std::string(64, '0'); break;
      case 4: changed.allowedGoodputEdges.clear(); break;
      case 5: changed.candidateProviders.clear(); break;
      case 6: changed.deadlineMs = 1200; break;
    }
    check(ack, changed, 1200);
  }
  for (int i = 0; i < 5; ++i) {
    auto changed = ack;
    switch (i) {
      case 0: changed.authenticationEvidence.trustSchemaValidated = false; break;
      case 1: changed.authenticationEvidence.signerIdentity = "/provider/b"; break;
      case 2: changed.authenticationEvidence.signerKeyLocator = "/foreign/KEY/key"; break;
      case 3: changed.ack.setStatus(false); break;
      case 4: changed.authenticationEvidence.wireDigest = "bad"; break;
    }
    check(changed, context, 1200);
  }
  auto changedWire = nativeParseJson(issued->capabilityFieldValue);
  changedWire["signature"] = std::string(128, '0');
  check(f.ackFor(nativeCanonicalJson(changedWire)), context, 1200);
  auto invalid = issued->capabilityFieldValue;
  invalid.insert(1, " ");
  const auto beforeInvalid = cache.decodeReuseCount();
  check(f.ackFor(invalid), context, 1200);
  check(f.ackFor(invalid), context, 1200);
  BOOST_CHECK_EQUAL(cache.decodeReuseCount(), beforeInvalid);

  // Owned admitted values cannot mutate immutable cached JSON.
  auto output = admission.tryVerifyV4Batch({ack}, context, 1200, nullptr, &cache);
  BOOST_REQUIRE(output);
  const_cast<NativeObservedProviderOfferV4&>(output->front().observation())
    .values["boot_epoch"] = "changed-owned-output";
  check(ack, context, 1200);
  struct RestoreEnvironment {
    int round = std::fegetround();
    std::locale locale;
    ~RestoreEnvironment() { std::fesetround(round); std::locale::global(locale); }
  } restore;
  for (const int mode : {FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    BOOST_REQUIRE_EQUAL(std::fesetround(mode), 0);
    const auto before = cache.decodeReuseCount();
    check(ack, context, 1200);
    BOOST_CHECK_EQUAL(cache.decodeReuseCount(), before);
  }
  BOOST_REQUIRE_EQUAL(std::fesetround(FE_TONEAREST), 0);
  struct CommaDecimal : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
  };
  std::locale::global(std::locale(std::locale::classic(), new CommaDecimal));
  const auto beforeLocale = cache.decodeReuseCount();
  check(ack, context, 1200);
  BOOST_CHECK_EQUAL(cache.decodeReuseCount(), beforeLocale);
  std::locale::global(std::locale::classic());
#if defined(__linux__)
  struct RestoreThreadLocale {
    locale_t owned = newlocale(LC_NUMERIC_MASK, "C", nullptr);
    locale_t previous = static_cast<locale_t>(0);
    ~RestoreThreadLocale() {
      if (previous) uselocale(previous);
      if (owned) freelocale(owned);
    }
  } threadLocale;
  BOOST_REQUIRE(threadLocale.owned);
  threadLocale.previous = uselocale(threadLocale.owned);
  BOOST_REQUIRE(threadLocale.previous);
  const auto beforeThreadLocale = cache.decodeReuseCount();
  check(ack, context, 1200);
  BOOST_CHECK_EQUAL(cache.decodeReuseCount(), beforeThreadLocale);
  uselocale(threadLocale.previous);
#endif
  // Each nondefault control state must use the fresh decoder, not cached data.
#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__)) && defined(__SSE__)
  struct RestoreControls {
    unsigned int mxcsr = _mm_getcsr();
    unsigned short x87 = 0;
    RestoreControls() { asm volatile("fnstcw %0" : "=m"(x87)); }
    ~RestoreControls() {
      _mm_setcsr(mxcsr);
      asm volatile("fldcw %0" : : "m"(x87));
    }
  } controls;
  for (const auto bit : {0x8000u, 0x0040u}) {
    _mm_setcsr(controls.mxcsr | bit);
    const auto before = cache.decodeReuseCount();
    check(ack, context, 1200);
    BOOST_CHECK_EQUAL(cache.decodeReuseCount(), before);
    _mm_setcsr(controls.mxcsr);
  }
  const unsigned short reducedPrecision = 0x027f;
  asm volatile("fldcw %0" : : "m"(reducedPrecision));
  const auto beforePrecision = cache.decodeReuseCount();
  check(ack, context, 1200);
  BOOST_CHECK_EQUAL(cache.decodeReuseCount(), beforePrecision);
  asm volatile("fldcw %0" : : "m"(controls.x87));
#endif
}

BOOST_AUTO_TEST_CASE(RequestOwnedDecodeCapacityAndConcurrentReaders)
{
  Fixture f;
  const auto admission = f.makeAdmission();
  const auto context = f.binding();
  auto cache = std::make_shared<NativeOfferDecodeCache>();
  std::weak_ptr<NativeOfferDecodeCache> lifetime = cache;
  std::vector<AckSelectionCandidate> acks;
  for (int i = 0; i < 18; ++i) {
    auto config = f.offerConfig();
    config.bootEpoch += "-" + std::to_string(i);
    const auto issued = issueNativeProviderOfferV4(
      f.requestPayload(), config, f.telemetry(), 1000);
    BOOST_REQUIRE(issued);
    acks.push_back(f.ackFor(issued->capabilityFieldValue));
    BOOST_REQUIRE(admission.tryVerifyV4Batch({acks.back()}, context, 1200, nullptr, cache.get()));
  }
  const auto beforeFull = cache->decodeReuseCount();
  BOOST_REQUIRE(admission.tryVerifyV4Batch({acks.back()}, context, 1200, nullptr, cache.get()));
  BOOST_CHECK_EQUAL(cache->decodeReuseCount(), beforeFull);
  std::atomic<bool> okay{true};
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&, owner = cache] {
      try {
        for (int i = 0; i < 8; ++i) {
          const auto result = admission.tryVerifyV4Batch({acks.front()}, context, 1200,
                                                       nullptr, owner.get());
          if (!result || result->front().observation().bootEpoch != "boot-provider-a-1-0")
            okay.store(false);
        }
      }
      catch (...) { okay.store(false); }
    });
  }
  for (auto& reader : readers) reader.join();
  BOOST_CHECK(okay.load());
#if defined(__linux__) && defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__)) && defined(__SSE__)
  BOOST_CHECK_EQUAL(cache->decodeReuseCount() - beforeFull, 32);
#else
  BOOST_CHECK_EQUAL(cache->decodeReuseCount(), 0);
#endif
  NativeOfferDecodeCache otherRequest;
  BOOST_REQUIRE(admission.tryVerifyV4Batch({acks.front()}, context, 1200, nullptr, &otherRequest));
  BOOST_CHECK_EQUAL(otherRequest.decodeReuseCount(), 0);
  cache.reset();
  BOOST_CHECK(lifetime.expired());
}

BOOST_AUTO_TEST_CASE(ProbePairScopeDeterministicallyTruncatesAndKeepsUnknownPairsOmitted)
{
  Fixture f;
  const auto cache = f.preparedCache(
    std::vector<NativeJson>{f.candidateArtifact(),
      NativeJson{{"artifact_digest", nativePlanningDigest("decoder-artifact")},
        {"model_manifest_digest", nativePlanningDigest("decoder-manifest")},
        {"graph_digest", f.candidateGraphDigest}, {"role", "decoder"},
        {"rank", 1}, {"backend", "onnxruntime"},
        {"protection_epoch", "plaintext-v1"}}});
  const std::vector<std::string> roster{
    "/provider/e", "/provider/c", "/provider/a", "/provider/d", "/provider/b"};
  const auto allowlist = buildNativePlacementProbeAllowlistV4(cache, 4, roster);
  BOOST_REQUIRE(allowlist);
  BOOST_REQUIRE_EQUAL(allowlist->rttPairs.size(), 16U);
  BOOST_REQUIRE_EQUAL(allowlist->goodputEdges.size(), 16U);
  BOOST_CHECK(std::is_sorted(allowlist->rttPairs.begin(), allowlist->rttPairs.end()));
  BOOST_CHECK(std::is_sorted(allowlist->goodputEdges.begin(), allowlist->goodputEdges.end()));
  BOOST_CHECK_EQUAL(allowlist->rttPairs.front().sourceProvider, "/provider/a");
  BOOST_CHECK_EQUAL(allowlist->rttPairs.front().destinationProvider, "/provider/b");
  BOOST_CHECK_EQUAL(allowlist->rttPairs.back().sourceProvider, "/provider/d");
  BOOST_CHECK_EQUAL(allowlist->rttPairs.back().destinationProvider, "/provider/e");

  const auto sameRoleCache = f.preparedCache(
    std::vector<NativeJson>{f.candidateArtifact()});
  const auto noCrossRoleScope = buildNativePlacementProbeAllowlistV4(
    sameRoleCache, 4, roster);
  BOOST_REQUIRE(noCrossRoleScope);
  BOOST_CHECK(noCrossRoleScope->rttPairs.empty());
  BOOST_CHECK(noCrossRoleScope->goodputEdges.empty());
}

BOOST_AUTO_TEST_CASE(CoreRttSampleBecomesFreshDirectedV4LinkRow)
{
  Fixture f;
  ndn_service_framework::NetworkTelemetrySnapshot sample;
  sample.providerName = ndn::Name(f.roster[0]);
  sample.peerName = ndn::Name(f.roster[1]);
  sample.kind = ndn_service_framework::NetworkTelemetrySampleKind::ActiveProbeRtt;
  sample.rttMs = 2.5;
  sample.wireBytes = 196;
  sample.encodedBytes = 140;
  sample.receivedSegments = 1;
  sample.sampleCount = 1;
  sample.lastUpdatedMs = 2990;
  sample.peerBootEpoch = "boot-provider-b-1";
  const auto topology = nativePlanningDigest("provider-a-topology");
  const auto telemetry = f.measuredTelemetry(
    f.roster[0], "boot-provider-a-1", topology);

  const auto rows = makeNativeProviderLinkObservationsV4(
    {sample}, f.roster[0], "boot-provider-a-1", topology, 3000, 5000);
  BOOST_REQUIRE_EQUAL(rows.size(), 1U);
  BOOST_CHECK_EQUAL(rows[0].at("kind").get<std::string>(), "ACTIVE_PROBE_RTT");
  BOOST_CHECK_EQUAL(rows[0].at("src_peer").get<std::string>(), f.roster[0]);
  BOOST_CHECK_EQUAL(rows[0].at("dst_peer").get<std::string>(), f.roster[1]);
  BOOST_CHECK_EQUAL(rows[0].at("observer_boot_epoch").get<std::string>(),
                    "boot-provider-a-1");
  BOOST_CHECK_EQUAL(rows[0].at("topology_digest").get<std::string>(), topology);
  BOOST_CHECK(rows[0].at("source_digest").is_string());
  const auto offer = issueNativeProviderOfferV4(
    f.requestPayload(), f.offerConfig(), telemetry, 3000, rows);
  BOOST_CHECK(offer.has_value());
}

BOOST_AUTO_TEST_CASE(CachedDedicatedTrainAckKeepsLifetimeAndSignedBudgetFallbacks)
{
  using namespace ndn_service_framework;
  Fixture f;
  NetworkTelemetrySnapshot sample;
  sample.providerName = ndn::Name(f.roster[0]);
  sample.peerName = ndn::Name(f.roster[1]);
  sample.peerBootEpoch = "boot-provider-b-1";
  sample.kind = NetworkTelemetrySampleKind::ActiveProbeGoodput;
  sample.sampleCount = 1;
  sample.lastUpdatedMs = 2990;
  sample.probeComplete = sample.probeSignatureValid = true;
  sample.targetSignatureValidated = sample.probeSourceTimingValidated = true;
  sample.probeSegmentsSent = sample.receivedSegments = NETWORK_PROBE_WARMED_SEGMENTS;
  sample.probeProfileDigest = NETWORK_PROBE_TRAIN_PROFILE_DIGEST;
  sample.payloadBytes = NETWORK_PROBE_WARMED_SEGMENTS * NETWORK_PROBE_WARMED_PAYLOAD_BYTES;
  sample.wireBytes = sample.payloadBytes + 10000;
  sample.encodedBytes = sample.payloadBytes;
  sample.firstByteMs = 1.0;
  sample.elapsedMs = 30.0;
  sample.goodputMbps = sample.payloadBytes * 8.0 / 30000.0;
  sample.probeRttMs.fill(20.0);
  sample.probeInterestGapsMs.fill(4.0);
  sample.probeDataArrivalGapsMs.fill(4.0);
  sample.probeDataWireBytes.fill(5000);
  sample.probeSourceSendGapsMs.fill(0.5);
  const auto topology = nativePlanningDigest("cached-train-topology");
  const auto telemetry = f.measuredTelemetry(f.roster[0], "boot-provider-a-1", topology);
  auto rows = makeNativeProviderLinkObservationsV4(
    {sample}, f.roster[0], "boot-provider-a-1", topology, 3000, 5000);
  BOOST_REQUIRE_EQUAL(rows.size(), 1U);
  BOOST_CHECK_EQUAL(rows[0].at("captured_at_ms").get<std::uint64_t>(), 2990U);
  BOOST_CHECK_EQUAL(rows[0].at("expires_at_ms").get<std::uint64_t>(), 5000U);
  const auto issued = issueNativeProviderOfferV4(
    f.requestPayload(), f.offerConfig(), telemetry, 3000, rows);
  BOOST_REQUIRE(issued);
  const auto check = [&](const std::string& wire) {
    BOOST_CHECK_LE(wire.size(), 4096U);
    const auto decoded = decodeNativeProviderOfferV4(wire);
    for (const auto& row : decoded.values.at("link_observations")) {
      BOOST_CHECK_EQUAL(row.at("captured_at_ms").get<std::uint64_t>(), 2990U);
      BOOST_CHECK_EQUAL(row.at("expires_at_ms").get<std::uint64_t>(), 5000U);
    }
    BOOST_CHECK_NO_THROW(f.makeAdmission().verifyV4(f.ackFor(wire), f.binding(), 3100));
  };
  BOOST_TEST_MESSAGE("Cached dedicated train primary links=" <<
    decodeNativeProviderOfferV4(issued->capabilityFieldValue).values.at("link_observations").size()
    << " bytes=" << issued->capabilityFieldValue.size()
    << " fallbacks=" << issued->fallbackCapabilityFieldValues.size());
  check(issued->capabilityFieldValue);
  for (const auto& wire : issued->fallbackCapabilityFieldValues) check(wire);
  rows[0]["expires_at_ms"] = std::uint64_t{3000};
  BOOST_CHECK_THROW(issueNativeProviderOfferV4(
    f.requestPayload(), f.offerConfig(), telemetry, 3000, rows), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(Spec200OptionalObservationPreservesBaseExecutionLifetime)
{
  using namespace ndn_service_framework;
  Fixture f;
  const auto nowMs = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  const auto bytes = f.requestPayload();
  auto envelope = nativeParseJson(std::string(bytes.begin(), bytes.end()));
  envelope["plan_deadline_ms"] = nowMs + 90000;
  envelope["probe_scope_digest"] = f.probeScopeDigest(
    envelope.at("candidate_artifact_set_digest").get<std::string>(),
    nowMs + 90000, f.requesterIdentity);
  const auto text = nativeCanonicalJson(envelope);
  RequestMessage request;
  ndn::Buffer payload(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
  request.setPayload(payload, payload.size());
  RequestCapabilities capabilities;
  capabilities.setField("DI_PLACEMENT_V4", "optional");
  request.setRequestCapabilities(capabilities);
  auto telemetry = f.measuredTelemetry(
    f.roster[0], "boot-provider-a-1", nativePlanningDigest("ttl-test-topology"));
  telemetry.sampledAtMs = static_cast<std::int64_t>(nowMs);
  telemetry.resources.measuredAtMs = static_cast<std::int64_t>(nowMs);
  ServiceProvider::AckRequestContext context;
  context.requesterIdentity = ndn::Name(f.requesterIdentity);
  context.serviceName = ndn::Name(f.service);
  context.requestId = ndn::Name(f.requestId);
  context.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  context.registrationGeneration = 1;
  context.cancelled = std::make_shared<std::atomic<bool>>(false);
  for (const std::uint64_t baseTtl : {90000ULL, 0ULL}) {
    const auto handler = makeNativeProviderObservationAckHandlerV4(
      [baseTtl] (const RequestMessage&) {
        ServiceProvider::AckDecision decision;
        decision.status = true;
        decision.message = "base admission";
        decision.pendingStateTtlMs = baseTtl;
        decision.providerCapabilityFields["BASE_CAPABILITY"] = "unchanged";
        return decision;
      }, f.offerConfig(), [telemetry] { return telemetry; });
    std::optional<ServiceProvider::AckDecision> result;
    handler(context, request, [&] (ServiceProvider::AckDecision decision) {
      result = std::move(decision);
    });
    BOOST_REQUIRE(result);
    BOOST_CHECK(result->status);
    BOOST_CHECK_EQUAL(result->message, "base admission");
    BOOST_CHECK_EQUAL(result->pendingStateTtlMs, baseTtl);
    BOOST_CHECK_EQUAL(result->providerCapabilityFields.at("BASE_CAPABILITY"), "unchanged");
    BOOST_REQUIRE(result->providerCapabilityFields.count(NATIVE_PROVIDER_OFFER_V4_FIELD));
    const auto offer = decodeNativeProviderOfferV4(
      result->providerCapabilityFields.at(NATIVE_PROVIDER_OFFER_V4_FIELD));
    BOOST_CHECK_EQUAL(offer.expiresAtMs - offer.capturedAtMs, 5000U);
    BOOST_REQUIRE(!result->providerCapabilityFieldFallbacks.empty());
    BOOST_CHECK(result->providerCapabilityFieldFallbacks.back().empty());
  }
}

BOOST_AUTO_TEST_CASE(Spec194ProductionAsyncAckIssuesLocalOfferWithoutPeerProbes)
{
  using namespace ndn_service_framework;
  using namespace ndn_service_framework::test;

  Fixture f;
  ndn::security::KeyChain keyChain("pib-memory:spec194-production-ack",
                                   "tpm-memory:spec194-production-ack");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name providerName(f.roster[0]);
  const ndn::Name serviceName(f.service);
  const ndn::Name requestId(f.requestId);
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  const ndn::Name peerProviderName(f.roster[1]);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec194/aa-production-ack"));
  auto provider = std::make_shared<LocalServiceProvider>(
    face, ndn::Name("/spec194/group"), providerCert, aaCert,
    "examples/trust-any.conf");
  provider->applyPermissionResponse(
    makePermissionResponse(providerName, tlv::ProviderPermission,
                           providerName, serviceName));

  const auto nowMs = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  const auto requestBytes = f.requestPayload();
  auto requestEnvelope = nativeParseJson(std::string(
    requestBytes.begin(), requestBytes.end()));
  requestEnvelope["plan_deadline_ms"] = nowMs + 10000;
  requestEnvelope["probe_scope_digest"] = f.probeScopeDigest(
    requestEnvelope.at("candidate_artifact_set_digest").get<std::string>(),
    nowMs + 10000, f.requesterIdentity);
  const auto payloadText = nativeCanonicalJson(requestEnvelope);
  ndn::Buffer payload(reinterpret_cast<const std::uint8_t*>(payloadText.data()),
                       payloadText.size());
  RequestMessage request;
  request.setUserToken("spec194-production-ack-user-token");
  request.setPayload(payload, payload.size());
  RequestCapabilities capabilities;
  capabilities.setField("DI_PLACEMENT_V4", "optional");
  request.setRequestCapabilities(capabilities);

  auto telemetry = f.measuredTelemetry(
    f.roster[0], "boot-provider-a-1", nativePlanningDigest("production-topology"));
  telemetry.sampledAtMs = static_cast<std::int64_t>(nowMs);
  telemetry.resources.measuredAtMs = static_cast<std::int64_t>(nowMs);
  const auto expectedTopologyDigest = nativePlanningDigest(
    "production-topology-local-snapshot");
  std::size_t telemetrySnapshotCalls = 0;
  ProviderArtifactCache residencyCache;
  NativeProviderRunnerReuseCache runnerCache(2);
  OnnxRuntimeSessionCache sessionCache;
  const auto residencyRoot = std::filesystem::temp_directory_path() /
    ("spec194-production-residency-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  struct CleanupResidencyRoot
  {
    std::filesystem::path path;
    ~CleanupResidencyRoot()
    {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanupResidencyRoot{residencyRoot};
  NativeProtectedPlaintextCache plaintextCache(residencyRoot / "plaintext", 2);
  NativePersistentRecipeResidencyIndex persistentIndex(residencyRoot / "persistent");
  NetworkTelemetryStore cachedTelemetry;
  cachedTelemetry.updateActiveProbe(providerName, peerProviderName, "historical-scope",
    "boot-provider-b-1", ndn::Name("/provider/b/probe/historical"), 12.0);
  const auto originalSamples = cachedTelemetry.getCachedActiveProbeSamples(
    providerName, {peerProviderName});
  BOOST_REQUIRE_EQUAL(originalSamples.size(), 1U);
  std::size_t cachedLinkCalls = 0;
  const auto handler = makeNativeProviderObservationAckHandlerV4(
    [] (const RequestMessage&) {
      ServiceProvider::AckDecision decision;
      decision.status = true;
      decision.message = "accepted by base provider policy";
      return decision;
    }, f.offerConfig(f.roster[0]),
    [&] {
      ++telemetrySnapshotCalls;
      auto snapshot = telemetry;
      snapshot.resources.topologyDigest = expectedTopologyDigest;
      return snapshot;
    },
    [&] (const std::vector<NativeJson>& candidateArtifacts,
         const std::string& processEpoch,
         const std::uint64_t capturedAtMs,
         const std::uint64_t expiresAtMs) {
      return queryNativeProviderResidencyOwnersV4(
        candidateArtifacts, processEpoch, capturedAtMs, expiresAtMs,
        &residencyCache, &runnerCache, &sessionCache, &plaintextCache,
        &persistentIndex);
    },
    [&] (const NativeJson& envelope, const NativeProviderTelemetrySnapshot& current,
         std::uint64_t checkedAtMs, std::uint64_t offerExpiry) {
      ++cachedLinkCalls;
      BOOST_CHECK_EQUAL(envelope.at("request_id").get<std::string>(), f.requestId);
      std::vector<NativeJson> rows;
      for (const auto& item : cachedTelemetry.getCachedActiveProbeSamples(
             providerName, {peerProviderName})) {
        auto formatted = makeNativeProviderLinkObservationsV4(
          {item.first}, f.roster[0], "boot-provider-a-1",
          current.resources.topologyDigest, checkedAtMs, std::min(item.second, offerExpiry));
        rows.insert(rows.end(), formatted.begin(), formatted.end());
      }
      return rows;
    }, 4000);

  std::size_t probeInterestsSent = 0;
  std::size_t readinessInterestsSent = 0;
  ndn::Name rttProbePrefix = peerProviderName;
  rttProbePrefix.append("NDNSF").append("PEER-PROBE");
  ndn::Name readinessProbePrefix = peerProviderName;
  readinessProbePrefix.append("NDNSF").append("PEER-PROBE-READY");
  ndn::Name goodputProbePrefix = peerProviderName;
  goodputProbePrefix.append("NDNSF").append("PEER-GOODPUT");
  auto targetProviderProbeResponder = face.onSendInterest.connect(
    [&] (const ndn::Interest& interest) {
      const auto& name = interest.getName();
      if (readinessProbePrefix.isPrefixOf(name)) {
        ++readinessInterestsSent;
      }
      if (rttProbePrefix.isPrefixOf(name) ||
          goodputProbePrefix.isPrefixOf(name))
        ++probeInterestsSent;
    });

  std::optional<RequestAckMessage> observedAck;
  provider->setLocalPublicationHandler(
    [&] (const ndn::Name&, const ndn::Buffer& wire) {
      auto [ok, block] = ndn::Block::fromBuffer(
        ndn::span<const std::uint8_t>(wire.data(), wire.size()));
      if (ok) {
        RequestAckMessage ack;
        if (ack.WireDecode(block))
          observedAck = std::move(ack);
      }
    });
  provider->setAckThreads(1);
  auto handle = provider->addScopedAsyncCollaborationHandler(
    serviceName, std::vector<CollaborationRole>{}, handler,
    ServiceProvider::CollaborationHandler(
      [] (ServiceProvider::CollaborationContext&, const RequestMessage&) {}));
  const auto requestBlock = request.WireEncode();
  const ndn::Buffer requestWire(requestBlock.data(), requestBlock.size());
  provider->OnRequestDecryptionSuccessCallbackV2(
    ndn::Name("/spec194/user"), serviceName, requestId, requestWire);

  face.getIoContext().restart();
  const auto ackDeadline = std::chrono::steady_clock::now() +
                           std::chrono::seconds(2);
  while (!observedAck && std::chrono::steady_clock::now() < ackDeadline) {
    face.getIoContext().run_for(std::chrono::milliseconds(5));
  }
  BOOST_REQUIRE(observedAck);
  BOOST_CHECK(observedAck->getStatus());
  BOOST_REQUIRE(observedAck->hasProviderCapabilityOffer());
  BOOST_REQUIRE(observedAck->getProviderCapabilityOffer().hasField(
    NATIVE_PROVIDER_TENSOR_TRANSPORT_V1_FIELD));
  const auto transport = nativeParseJson(observedAck->getProviderCapabilityOffer().getField(
    NATIVE_PROVIDER_TENSOR_TRANSPORT_V1_FIELD));
  BOOST_CHECK_EQUAL(transport.at("boot_epoch").get<std::string>(), "boot-provider-a-1");
  BOOST_CHECK_EQUAL(transport.at("segment_payload_bytes").get<std::uint64_t>(), 4000U);
  BOOST_CHECK(observedAck->getProviderCapabilityOffer().hasField(
    NATIVE_PROVIDER_OFFER_V4_FIELD));
  const auto issued = decodeNativeProviderOfferV4(
    observedAck->getProviderCapabilityOffer().getField(
      NATIVE_PROVIDER_OFFER_V4_FIELD));
  BOOST_CHECK_EQUAL(issued.values.at("residency_state").get<std::string>(), "KNOWN_EMPTY");
  BOOST_CHECK(issued.values.at("residency").is_array());
  BOOST_CHECK(issued.values.at("residency").empty());
  BOOST_CHECK_EQUAL(telemetrySnapshotCalls, 1U);
  BOOST_CHECK_EQUAL(cachedLinkCalls, 1U);
  BOOST_CHECK_EQUAL(probeInterestsSent, 0U);
  BOOST_CHECK_EQUAL(readinessInterestsSent, 0U);
  BOOST_CHECK_EQUAL(
    issued.values.at("resources").at("topology_digest").get<std::string>(),
    expectedTopologyDigest);
  const auto& issuedLinks = issued.values.at("link_observations");
  BOOST_REQUIRE_EQUAL(issuedLinks.size(), 1U);
  BOOST_CHECK_EQUAL(issuedLinks[0].at("captured_at_ms").get<std::uint64_t>(),
                    originalSamples.front().first.lastUpdatedMs);
  BOOST_CHECK_EQUAL(issuedLinks[0].at("expires_at_ms").get<std::uint64_t>(),
                    originalSamples.front().second);
  BOOST_CHECK_EQUAL(issuedLinks[0].at("dst_boot_epoch").get<std::string>(), "boot-provider-b-1");
  BOOST_CHECK_EQUAL(issuedLinks[0].at("topology_digest").get<std::string>(), expectedTopologyDigest);
  BOOST_CHECK_LE(observedAck->getProviderCapabilityOffer().getField(
                   NATIVE_PROVIDER_OFFER_V4_FIELD).size(), 4096U);
  BOOST_CHECK(provider->hasPendingRequestForTokenTest(
    ndn::Name("/spec194/user"), serviceName, ndn::Name(f.requestId)));
}

BOOST_AUTO_TEST_CASE(Spec194ProductionAsyncAckDoesNotProbeAfterWireDeadline)
{
  using namespace ndn_service_framework;
  using namespace ndn_service_framework::test;

  Fixture f;
  ndn::security::KeyChain keyChain("pib-memory:spec194-expired-probe-ack",
                                   "tpm-memory:spec194-expired-probe-ack");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name providerName(f.roster[0]);
  const ndn::Name serviceName(f.service);
  const ndn::Name requestId(f.requestId);
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec194/aa-expired-probe"));
  auto provider = std::make_shared<LocalServiceProvider>(
    face, ndn::Name("/spec194/group"), providerCert, aaCert,
    "examples/trust-any.conf");
  provider->applyPermissionResponse(
    makePermissionResponse(providerName, tlv::ProviderPermission,
                           providerName, serviceName));

  const auto nowMs = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  const auto requestBytes = f.requestPayload();
  auto requestEnvelope = nativeParseJson(std::string(
    requestBytes.begin(), requestBytes.end()));
  requestEnvelope["plan_deadline_ms"] = nowMs + 20;
  requestEnvelope["probe_scope_digest"] = f.probeScopeDigest(
    requestEnvelope.at("candidate_artifact_set_digest").get<std::string>(),
    nowMs + 20, f.requesterIdentity);
  const auto payloadText = nativeCanonicalJson(requestEnvelope);
  ndn::Buffer payload(reinterpret_cast<const std::uint8_t*>(payloadText.data()),
                       payloadText.size());
  RequestMessage request;
  request.setUserToken("spec194-expired-probe-user-token");
  request.setPayload(payload, payload.size());
  RequestCapabilities capabilities;
  capabilities.setField("DI_PLACEMENT_V4", "optional");
  request.setRequestCapabilities(capabilities);

  auto telemetry = f.measuredTelemetry(
    f.roster[0], "boot-provider-a-1", nativePlanningDigest("expired-probe-topology"));
  telemetry.sampledAtMs = static_cast<std::int64_t>(nowMs);
  telemetry.resources.measuredAtMs = static_cast<std::int64_t>(nowMs);
  std::size_t expiredCachedLinkCalls = 0;
  const auto handler = makeNativeProviderObservationAckHandlerV4(
    [] (const RequestMessage&) {
      ServiceProvider::AckDecision decision;
      decision.status = true;
      decision.message = "accepted by base provider policy";
      return decision;
    }, f.offerConfig(f.roster[0]),
    [telemetry] {
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
      return telemetry;
    }, {},
    [&] (const NativeJson&, const NativeProviderTelemetrySnapshot&, std::uint64_t, std::uint64_t) {
      ++expiredCachedLinkCalls;
      return std::vector<NativeJson>{};
    });

  std::optional<RequestAckMessage> observedAck;
  provider->setLocalPublicationHandler(
    [&] (const ndn::Name&, const ndn::Buffer& wire) {
      auto [ok, block] = ndn::Block::fromBuffer(
        ndn::span<const std::uint8_t>(wire.data(), wire.size()));
      if (ok) {
        RequestAckMessage ack;
        if (ack.WireDecode(block))
          observedAck = std::move(ack);
      }
    });
  provider->setAckThreads(1);
  auto handle = provider->addScopedAsyncCollaborationHandler(
    serviceName, std::vector<CollaborationRole>{}, handler,
    ServiceProvider::CollaborationHandler(
      [] (ServiceProvider::CollaborationContext&, const RequestMessage&) {}));
  const auto requestBlock = request.WireEncode();
  const ndn::Buffer requestWire(requestBlock.data(), requestBlock.size());
  provider->OnRequestDecryptionSuccessCallbackV2(
    ndn::Name("/spec194/user/expired-probe"), serviceName,
    requestId, requestWire);

  face.getIoContext().restart();
  face.getIoContext().run_for(std::chrono::milliseconds(300));
  BOOST_REQUIRE(observedAck);
  BOOST_CHECK(observedAck->getStatus());
  BOOST_CHECK_EQUAL(expiredCachedLinkCalls, 0U);
  BOOST_CHECK(!observedAck->hasProviderCapabilityOffer() ||
    !observedAck->getProviderCapabilityOffer().hasField(
      NATIVE_PROVIDER_OFFER_V4_FIELD));
  BOOST_CHECK(std::none_of(face.sentInterests.begin(), face.sentInterests.end(),
    [] (const ndn::Interest& interest) {
      const auto& name = interest.getName();
      for (std::size_t i = 0; i + 1 < name.size(); ++i) {
        if (name[i].toUri() == "NDNSF" &&
            (name[i + 1].toUri() == "PEER-PROBE" ||
             name[i + 1].toUri() == "PEER-GOODPUT"))
          return true;
      }
      return false;
    }));
}

BOOST_AUTO_TEST_CASE(ProbeScopeBindsRequesterIdentityAndOriginalDeadline)
{
  Fixture f;
  NativeModelDescriptor model;
  model.modelName = "spec194-probe-scope";
  model.contentDigest = nativePlanningDigest("spec194-probe-scope-content");
  model.semanticsDigest = nativePlanningDigest("spec194-probe-scope-semantics");
  model.graphDigest = nativePlanningDigest("spec194-probe-scope-graph");
  model.modelFormat = "onnx";
  model.precision = "float32";
  model.adapterId = "spec194-probe-scope-adapter";
  model.adapterVersion = "1";
  model.adapter = fixture::modelAdapter(
    model.adapterId, model.adapterVersion, model.modelFormat, model.precision);

  NativeRequestContract contract;
  contract.serviceName = f.service;
  contract.taskName = "task";
  contract.adapterName = model.adapterId;
  contract.adapterDescriptorDigest = model.adapter.descriptorDigest();
  contract.adapterCompositionDigest = nativePlanningDigest("composition");
  contract.taskDescriptorDigest = nativePlanningDigest("task");
  NativeApplicationInput input;
  input.taskName = "task";
  input.inputSchemaDigest = model.adapter.inputSchemaDigest;
  input.optionsSchemaDigest = model.adapter.optionsSchemaDigest;
  input.payload = {1};

  NativeCandidateArtifactInventoryV4 inventory;
  inventory.identities = NativeJson::array({NativeJson{
    {"artifact_digest", nativePlanningDigest("probe-scope-artifact")},
    {"model_manifest_digest", nativePlanningDigest("probe-scope-manifest")},
    {"graph_digest", model.graphDigest}, {"role", "encoder"}, {"rank", 0},
    {"backend", "onnxruntime"}, {"protection_epoch", "epoch-1"}}});
  inventory.digest = nativePlanningDigest(
    nativeCanonicalJson(NativeJson(inventory.identities)));
  const NativePlacementProbeAllowlistV4 allowlist;
  const auto encode = [&] (const std::string& requester, std::uint64_t deadline) {
    return encodeNativeRequestEnvelope(model, input, contract, f.requestId, 1,
      deadline, std::nullopt, f.roster, inventory, allowlist, requester);
  };

  const auto original = encode(f.requesterIdentity, 10000);
  const auto wire = NativeJson::parse(original.wire);
  BOOST_CHECK_EQUAL(wire.at("requester_identity").get<std::string>(), f.requesterIdentity);
  BOOST_CHECK_EQUAL(wire.at("plan_deadline_ms").get<std::uint64_t>(), 10000U);
  BOOST_CHECK_NE(encode("/spec194/other-user", 10000).probeScopeDigest,
                 original.probeScopeDigest);
  BOOST_CHECK_NE(encode(f.requesterIdentity, 10001).probeScopeDigest,
                 original.probeScopeDigest);
  BOOST_CHECK_THROW(encodeNativeRequestEnvelope(model, input, contract, f.requestId, 1,
    10000, std::nullopt, f.roster, inventory, allowlist), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(CandidateInventoryUsesPreparedPrefixAndRejectsIncompleteOrOversizedSet)
{
  Fixture f;
  NativePreparedPlanningCache cache;
  cache.splitter = {"spec194-test-splitter", "1", nativePlanningDigest("splitter")};
  for (std::uint64_t rank = 0; rank < 33; ++rank) {
    NativeSelectionRoleV3 role;
    role.role = "encoder";
    role.rank = rank;
    role.backend = "onnxruntime";
    role.artifactDigest = nativePlanningDigest("artifact-" + std::to_string(rank));
    role.modelManifestDigest = nativePlanningDigest("manifest");
    role.graphDigest = f.candidateGraphDigest;
    role.protectionEpoch = "epoch-1";
    NativePreparedPlanningCandidate candidate;
    candidate.roles.push_back(std::move(role));
    cache.candidates.push_back(std::move(candidate));
  }

  const auto prefix = buildNativeCandidateArtifactInventoryV4(cache, 1);
  BOOST_REQUIRE(prefix);
  BOOST_REQUIRE_EQUAL(prefix->identities.size(), 1U);
  BOOST_CHECK_EQUAL(prefix->identities.front().at("rank").get<std::uint64_t>(), 0U);
  BOOST_CHECK_EQUAL(prefix->digest,
    nativePlanningDigest(nativeCanonicalJson(NativeJson(prefix->identities))));
  BOOST_CHECK(!buildNativeCandidateArtifactInventoryV4(cache, 33));

  auto incomplete = cache;
  incomplete.candidates.front().roles.front().artifactDigest.clear();
  BOOST_CHECK(!buildNativeCandidateArtifactInventoryV4(incomplete, 1));
  BOOST_CHECK(!buildNativeCandidateArtifactInventoryV4(cache, 0));
  BOOST_CHECK(!issueNativeProviderOfferV4(
    f.requestPayload(std::vector<NativeJson>{}), f.offerConfig(), f.telemetry(), 1000));
}

BOOST_AUTO_TEST_CASE(IssuesAndIndependentlyAdmitsSignedRequestBoundOffer)
{
  static_assert(!std::is_default_constructible_v<NativeAdmittedOfferV4>);
  Fixture f;
  const auto issued = issueNativeProviderOfferV4(
    f.requestPayload(), f.offerConfig(), f.telemetry(), 1000);
  BOOST_REQUIRE(issued);
  BOOST_CHECK_LE(issued->capabilityFieldValue.size(), 4096);
  const auto decoded = decodeNativeProviderOfferV4(issued->capabilityFieldValue);
  BOOST_CHECK_EQUAL(decoded.provider, f.roster.front());
  BOOST_CHECK_EQUAL(decoded.candidateSetDigest, f.candidateSetDigest);
  BOOST_CHECK_EQUAL(decoded.candidateArtifactSetDigest,
    f.binding().candidateArtifactSetDigest);
  BOOST_CHECK_EQUAL(decoded.values.at("resources_state").get<std::string>(), "UNKNOWN");
  BOOST_CHECK(decoded.values.at("residency").is_null());

  auto ack = f.ackFor(issued->capabilityFieldValue);
  const auto admitted = f.makeAdmission().verifyV4(ack, f.binding(), 1200);
  BOOST_CHECK_EQUAL(admitted.observation().offerDigest, decoded.offerDigest);
}

BOOST_AUTO_TEST_CASE(TensorTransportDeclarationUsesCurrentAuthenticatedAckAndBoot)
{
  Fixture f;
  const auto issued = issueNativeProviderOfferV4(
    f.requestPayload(), f.offerConfig(), f.telemetry(), 1000);
  BOOST_REQUIRE(issued);
  const auto admission = f.makeAdmission();
  const auto context = f.binding();
  const auto baseAck = f.ackFor(issued->capabilityFieldValue);
  BOOST_CHECK_EQUAL(admission.verifyV4(baseAck, context, 1200).tensorSegmentBytes(), 0U);
  const auto valid = NativeJson{{"schema", "ndnsf-di-tensor-transport-v1"},
    {"boot_epoch", "boot-provider-a-1"}, {"segment_payload_bytes", std::uint64_t{4000}}};
  const auto withTransport = [&](const std::string& wire) {
    auto ack = baseAck;
    auto fields = ack.ack.getProviderCapabilityOffer();
    fields.setField(NATIVE_PROVIDER_TENSOR_TRANSPORT_V1_FIELD, wire);
    ack.ack.setProviderCapabilityOffer(fields);
    return ack;
  };
  const auto ack = withTransport(nativeCanonicalJson(valid));
  const auto accepted = admission.verifyV4(ack, context, 1200);
  BOOST_CHECK_EQUAL(accepted.tensorSegmentBytes(), 4000U);
  BOOST_CHECK_EQUAL(accepted.observation().offerDigest,
                    decodeNativeProviderOfferV4(issued->capabilityFieldValue).offerDigest);
  BOOST_CHECK(!accepted.observation().values.contains("tensor_segment_payload_bytes"));
  std::vector<NativeJson> invalid;
  for (const auto& bytes : {NativeJson(0), NativeJson(-1), NativeJson(4000.0),
       NativeJson("4000"), NativeJson(nullptr), NativeJson(true),
       NativeJson(std::uint64_t{ndn::MAX_NDN_PACKET_SIZE} + 1),
       NativeJson(std::numeric_limits<std::uint64_t>::max())}) {
    auto value = valid;
    value["segment_payload_bytes"] = bytes;
    invalid.push_back(std::move(value));
  }
  for (const auto& field : {"schema", "boot_epoch", "segment_payload_bytes"}) {
    auto value = valid;
    value.erase(field);
    invalid.push_back(std::move(value));
  }
  auto staleBoot = valid;
  staleBoot["boot_epoch"] = "boot-provider-a-0";
  invalid.push_back(staleBoot);
  auto foreignBoot = valid;
  foreignBoot["boot_epoch"] = "boot-provider-b-1";
  invalid.push_back(foreignBoot);
  auto futureSchema = valid;
  futureSchema["schema"] = "ndnsf-di-tensor-transport-v2";
  invalid.push_back(futureSchema);
  auto unknownField = valid;
  unknownField["extra"] = 1;
  invalid.push_back(unknownField);
  for (const auto& value : invalid)
    BOOST_CHECK_EQUAL(admission.verifyV4(withTransport(nativeCanonicalJson(value)),
                                        context, 1200).tensorSegmentBytes(), 0U);
  for (const auto& wire : {std::string("{"), std::string(257, ' '),
       std::string("{\"schema\":\"ndnsf-di-tensor-transport-v1\",\"boot_epoch\":\"boot-provider-a-1\","
                   "\"segment_payload_bytes\":4000,\"segment_payload_bytes\":7000}")})
    BOOST_CHECK_EQUAL(admission.verifyV4(withTransport(wire), context, 1200)
                      .tensorSegmentBytes(), 0U);
  auto unauthenticated = ack;
  unauthenticated.authenticationEvidence.trustSchemaValidated = false;
  BOOST_CHECK_THROW(admission.verifyV4(unauthenticated, context, 1200), std::runtime_error);
  auto wrongRequest = context;
  wrongRequest.requestId = "/request/other";
  BOOST_CHECK_THROW(admission.verifyV4(ack, wrongRequest, 1200), std::runtime_error);
  NativeOfferDecodeCache cache;
  const auto cached = admission.tryVerifyV4Batch({ack}, context, 1200, nullptr, &cache);
  BOOST_REQUIRE(cached);
  BOOST_CHECK_EQUAL(cached->front().tensorSegmentBytes(), 4000U);
  auto changed = valid;
  changed["segment_payload_bytes"] = std::uint64_t{6000};
  const auto reused = admission.tryVerifyV4Batch(
    {withTransport(nativeCanonicalJson(changed))}, context, 1200, nullptr, &cache);
  BOOST_REQUIRE(reused);
  BOOST_CHECK_EQUAL(reused->front().tensorSegmentBytes(), 6000U);
  const auto absent = admission.tryVerifyV4Batch({baseAck}, context, 1200, nullptr, &cache);
  BOOST_REQUIRE(absent);
  BOOST_CHECK_EQUAL(absent->front().tensorSegmentBytes(), 0U);
}

BOOST_AUTO_TEST_CASE(V4BatchReusesOnlyImmutableContextAndPreservesSequentialRejections)
{
  Fixture f;
  const auto issuedA = issueNativeProviderOfferV4(
    f.requestPayload(), f.offerConfig("/provider/a"), f.telemetry(), 1000);
  const auto issuedB = issueNativeProviderOfferV4(
    f.requestPayload(), f.offerConfig("/provider/b"), f.telemetry(), 1000);
  BOOST_REQUIRE(issuedA);
  BOOST_REQUIRE(issuedB);
  const auto a = f.ackFor(issuedA->capabilityFieldValue, "/provider/a");
  const auto b = f.ackFor(issuedB->capabilityFieldValue, "/provider/b");
  const auto admission = f.makeAdmission();
  const auto check = [&](const std::vector<AckSelectionCandidate>& acks,
                         const NativeOfferBindingContext& context, std::uint64_t now) {
    std::vector<std::string> expected;
    std::string expectedReason;
    try {
      for (const auto& ack : acks) {
        if (!ack.ack.hasProviderCapabilityOffer() ||
            !ack.ack.getProviderCapabilityOffer().hasField(NATIVE_PROVIDER_OFFER_V4_FIELD)) {
          expectedReason = "PROVIDER_V4_OFFER_MISSING";
          break;
        }
        expected.push_back(admission.verifyV4(ack, context, now).observation().offerDigest);
      }
    }
    catch (const std::exception&) { expectedReason = "PROVIDER_V4_OFFER_REJECTED"; }
    std::string reason = "must-be-cleared";
    const auto batch = admission.tryVerifyV4Batch(acks, context, now, &reason);
    BOOST_CHECK_EQUAL(reason, expectedReason);
    BOOST_CHECK_EQUAL(bool(batch), expectedReason.empty());
    if (batch) {
      BOOST_REQUIRE_EQUAL(batch->size(), expected.size());
      for (std::size_t i = 0; i < batch->size(); ++i)
        BOOST_CHECK_EQUAL((*batch)[i].observation().offerDigest, expected[i]);
    }
  };
  const auto context = f.binding();
  check({a, b}, context, 1200);
  check({b, a}, context, 1200);
  std::string reason;
  BOOST_REQUIRE(admission.tryVerifyV4Batch({a, b}, context, 1200, &reason));
  check({a, b}, context, 10000); // A prior successful batch cannot cache lifetime.
  auto missing = b;
  missing.ack.clearProviderCapabilityOffer();
  auto forged = b;
  forged.authenticationEvidence.trustSchemaValidated = false;
  check({a, forged}, context, 1200); // Exercise failure after common proof exists.
  check({forged, missing}, context, 1200);
  check({missing, forged}, context, 1200);
  check({a, missing}, context, 1200);
  auto badSignatureValues = nativeParseJson(issuedB->capabilityFieldValue);
  auto& badSignature = badSignatureValues["signature"].get_ref<std::string&>();
  badSignature[0] = badSignature[0] == 'A' ? 'B' : 'A';
  const auto badSignatureAck = f.ackFor(nativeCanonicalJson(badSignatureValues), "/provider/b");
  BOOST_CHECK_EQUAL(decodeNativeProviderOfferV4(nativeCanonicalJson(badSignatureValues)).offerDigest,
                    decodeNativeProviderOfferV4(issuedB->capabilityFieldValue).offerDigest);
  check({a, badSignatureAck}, context, 1200);
  auto statusMismatch = b;
  statusMismatch.ack.setStatus(false);
  check({a, statusMismatch}, context, 1200);
  auto reservation = b;
  reservation.ack.setReservationLease(ndn_service_framework::ReservationLease{});
  check({a, reservation}, context, 1200);
  for (bool futureCapture : {false, true}) {
    auto timed = nativeParseJson(issuedB->capabilityFieldValue);
    timed["captured_at_ms"] = futureCapture ? 1300 : 1001;
    timed["expires_at_ms"] = futureCapture ? 5000 : 1100;
    timed.erase("signature");
    timed["signature"] = f.signer.sign(nativePlanningDigest(nativeCanonicalJson(timed)));
    check({a, f.ackFor(nativeCanonicalJson(timed), "/provider/b")}, context, 1200);
  }
  // C is genuinely authorized by the immutable policy and has a valid signed
  // offer. Rebind only its candidate/scope digests to the original A/B context
  // and resign: its rejection must retain per-offer roster membership after A.
  Fixture extended({"/provider/a", "/provider/b", "/provider/c"});
  const auto extendedAdmission = extended.makeAdmission("/provider/c");
  auto configC = extended.offerConfig("/provider/c");
  configC.bootEpoch = "boot-provider-c-1";
  const auto issuedC = issueNativeProviderOfferV4(
    extended.requestPayload(), configC, extended.telemetry(), 1000);
  BOOST_REQUIRE(issuedC);
  BOOST_CHECK_NO_THROW(extendedAdmission.verifyV4(
    extended.ackFor(issuedC->capabilityFieldValue, "/provider/c"), extended.binding(), 1200));
  auto foreignValues = nativeParseJson(issuedC->capabilityFieldValue);
  foreignValues["candidate_set_digest"] = context.candidateSetDigest;
  foreignValues["probe_scope_digest"] = context.probeScopeDigest;
  foreignValues.erase("signature");
  foreignValues["signature"] = extended.signer.sign(nativePlanningDigest(nativeCanonicalJson(foreignValues)));
  const auto foreignAck = extended.ackFor(nativeCanonicalJson(foreignValues), "/provider/c");
  BOOST_CHECK_NO_THROW(extendedAdmission.verifyV4(a, context, 1200));
  BOOST_CHECK_THROW(extendedAdmission.verifyV4(foreignAck, context, 1200), std::runtime_error);
  BOOST_CHECK(!extendedAdmission.tryVerifyV4Batch({a, foreignAck}, context, 1200, &reason));
  BOOST_CHECK_EQUAL(reason, "PROVIDER_V4_OFFER_REJECTED");
  for (int mutation = 0; mutation < 6; ++mutation) {
    auto bad = b;
    switch (mutation) {
      case 0: bad.providerName = ndn::Name("/provider/a"); break;
      case 1: bad.serviceName = ndn::Name("/other/service"); break;
      case 2: bad.requestId = ndn::Name("/other/request"); break;
      case 3: bad.authenticationEvidence.signerIdentity = "/provider/a"; break;
      case 4: bad.authenticationEvidence.signerKeyLocator = "/other/KEY/1"; break;
      case 5: bad = f.ackFor("invalid-json", "/provider/b"); break;
    }
    check({a, bad}, context, 1200);
  }
  for (int mutation = 0; mutation < 8; ++mutation) {
    auto changed = context;
    switch (mutation) {
      case 0: changed.candidateProviders.pop_back(); break;
      case 1: changed.candidateProviders[0] = "/other/provider"; break;
      case 2: changed.candidateArtifacts[0]["rank"] = 0.0; break;
      case 3: changed.candidateArtifacts[0]["backend"] = "changed"; break;
      case 4: changed.probeScopeDigest = nativePlanningDigest("changed"); break;
      case 5: changed.allowedRttPairs.clear(); break;
      case 6: changed.deadlineMs = 1199; break;
      case 7: changed.attempt = 2; break;
    }
    check({a, b}, changed, 1200);
    check({a, b}, context, 1200); // No state leaks between batch invocations.
  }
  BOOST_CHECK(!admission.tryVerifyV4Batch({}, context, 1200, &reason));
  BOOST_CHECK_EQUAL(reason, "PROVIDER_V4_OFFER_REJECTED");
}

BOOST_AUTO_TEST_CASE(DistinguishesUnknownKnownEmptyAndObservedResidency)
{
  Fixture f;
  NativeProviderResidencySnapshotV4 knownEmpty;
  knownEmpty.known = true;
  auto emptyIssued = issueNativeProviderOfferV4(
    f.requestPayload(), f.offerConfig(), f.telemetry(), 1000, {}, knownEmpty);
  BOOST_REQUIRE(emptyIssued);
  const auto empty = decodeNativeProviderOfferV4(emptyIssued->capabilityFieldValue);
  BOOST_CHECK_EQUAL(empty.values.at("residency_state").get<std::string>(), "KNOWN_EMPTY");
  BOOST_CHECK(empty.values.at("residency").empty());
  BOOST_CHECK_EQUAL(f.makeAdmission().verifyV4(
    f.ackFor(emptyIssued->capabilityFieldValue), f.binding(), 1200).observation()
      .values.at("residency_state").get<std::string>(), "KNOWN_EMPTY");

  NativeProviderResidencySnapshotV4 observed;
  observed.known = true;
  observed.rows = {f.residencyRow()};
  auto observedIssued = issueNativeProviderOfferV4(
    f.requestPayload(), f.offerConfig(), f.telemetry(), 1000, {}, observed);
  BOOST_REQUIRE(observedIssued);
  const auto decoded = decodeNativeProviderOfferV4(observedIssued->capabilityFieldValue);
  BOOST_CHECK_EQUAL(decoded.values.at("residency_state").get<std::string>(), "OBSERVED");
  BOOST_REQUIRE_EQUAL(decoded.values.at("residency").size(), 1);
  BOOST_CHECK_NO_THROW(f.makeAdmission().verifyV4(
    f.ackFor(observedIssued->capabilityFieldValue), f.binding(), 1200));

  auto outside = f.residencyRow();
  outside["artifact_digest"] = "sha256:" + std::string(64, 'e');
  observed.rows = {outside};
  BOOST_CHECK_THROW(issueNativeProviderOfferV4(
    f.requestPayload(), f.offerConfig(), f.telemetry(), 1000, {}, observed),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ResidencyOwnerAggregationRequiresCompleteOwnerSet)
{
  Fixture f;
  const std::vector<NativeProviderResidencyOwnerV4> owners{
    NativeProviderResidencyOwnerV4::ProviderArtifactCache,
    NativeProviderResidencyOwnerV4::RunnerReuseCache,
    NativeProviderResidencyOwnerV4::OnnxRuntimeSessionCache,
    NativeProviderResidencyOwnerV4::ProtectedPlaintextCache,
    NativeProviderResidencyOwnerV4::PersistentRecipeCache};

  auto partial = combineNativeProviderResidencyOwnersV4({
    {owners.front(), true, {f.residencyRow()}}});
  BOOST_CHECK(!partial.known);
  BOOST_CHECK(partial.rows.empty());

  std::vector<NativeProviderResidencyOwnerResultV4> complete;
  for (const auto owner : owners)
    complete.push_back({owner, true, {}});
  auto knownEmpty = combineNativeProviderResidencyOwnersV4(complete);
  BOOST_CHECK(knownEmpty.known);
  BOOST_CHECK(knownEmpty.rows.empty());

  complete.front().rows.push_back(f.residencyRow());
  auto observed = combineNativeProviderResidencyOwnersV4(complete);
  BOOST_CHECK(observed.known);
  BOOST_REQUIRE_EQUAL(observed.rows.size(), 1U);
  BOOST_CHECK_EQUAL(observed.rows.front().at("state").get<std::string>(),
                    "CANONICAL_ONLY");

  auto exactLoaded = f.residencyRow();
  exactLoaded["state"] = "EXACT_LOADED";
  exactLoaded["runtime_generation"] = "boot-a:runner-1";
  complete[1].rows.push_back(std::move(exactLoaded));
  auto merged = combineNativeProviderResidencyOwnersV4(complete);
  BOOST_CHECK(merged.known);
  BOOST_REQUIRE_EQUAL(merged.rows.size(), 1U);
  BOOST_CHECK_EQUAL(merged.rows.front().at("state").get<std::string>(), "EXACT_LOADED");

  complete.back().complete = false;
  auto unsupported = combineNativeProviderResidencyOwnersV4(complete);
  BOOST_CHECK(!unsupported.known);
  BOOST_CHECK(unsupported.rows.empty());
}

BOOST_AUTO_TEST_CASE(RunnerReuseResidencyQueryIsBoundedAndReadOnly)
{
  class TestRunner final : public NativeModelRunner
  {
  public:
    std::map<std::string, TensorBundle>
    run(const RoleExecutionContext&) override
    {
      return {};
    }
  };

  NativeProviderRunnerReuseCache cache(2);
  NativeSelectionProjectionV3 projection;
  projection.provider = "/provider/a";
  projection.requestId = "/NDNSF/DI/REQUEST/runner-cache-residency-1";
  projection.attempt = 1;
  projection.planDigest = "sha256:" + std::string(64, 'd');
  projection.executionRole.roleId = "encoder";
  projection.dataflow.requestId = projection.requestId;
  projection.dataflow.attempt = projection.attempt;
  projection.dataflow.planDigest = projection.planDigest;
  projection.dataflow.role = projection.executionRole.roleId;
  projection.dataflow.terminalResponseOwner = true;
  projection.canonicalArtifactName = "/model/spec194";
  projection.assembly.role = "encoder";
  projection.assembly.selectedRole = "encoder";
  projection.assembly.rank = 3;
  projection.assembly.artifactDigest = "sha256:" + std::string(64, 'a');
  projection.assembly.modelManifestDigest = "sha256:" + std::string(64, 'b');
  projection.assembly.graphDigest = "sha256:" + std::string(64, 'c');
  projection.assembly.backend = "onnxruntime-cpu";
  projection.assembly.protectionEpoch = "plaintext-v1";
  projection.assembly.deviceSet = {"cpu"};
  auto runner = std::make_shared<TestRunner>();
  cache.publish(projection, "/provider/a", "boot-a", {}, runner);

  const std::vector<NativeJson> candidateArtifacts{NativeJson{
    {"artifact_digest", projection.assembly.artifactDigest},
    {"model_manifest_digest", projection.assembly.modelManifestDigest},
    {"graph_digest", projection.assembly.graphDigest}, {"role", "encoder"},
    {"rank", 3U}, {"backend", "onnxruntime"},
    {"protection_epoch", "plaintext-v1"}}};
  const auto observed = cache.queryPlacementResidencyV4(
    candidateArtifacts, "boot-a", 1000, 5000);
  BOOST_CHECK(observed.complete);
  BOOST_CHECK(observed.owner == NativeProviderResidencyOwnerV4::RunnerReuseCache);
  BOOST_REQUIRE_EQUAL(observed.rows.size(), 1U);
  BOOST_CHECK_EQUAL(observed.rows.front().at("state").get<std::string>(), "EXACT_LOADED");
  BOOST_CHECK_EQUAL(observed.rows.front().at("device_set")[0].get<std::string>(), "cpu");
  BOOST_CHECK(observed.rows.front().at("runtime_generation").is_string());

  auto absent = candidateArtifacts;
  absent.front()["artifact_digest"] = "sha256:" + std::string(64, 'd');
  const auto knownAbsent = cache.queryPlacementResidencyV4(
    absent, "boot-a", 1000, 5000);
  BOOST_CHECK(knownAbsent.complete);
  BOOST_CHECK(knownAbsent.rows.empty());
}

BOOST_AUTO_TEST_CASE(RunnerReuseRequiresMatchingOutputBinding)
{
  class TestRunner final : public NativeModelRunner
  {
  public:
    std::map<std::string, TensorBundle>
    run(const RoleExecutionContext&) override
    {
      return {};
    }
  };

  const auto sealEndpoint = [] (NativeTensorEndpointV3& endpoint) {
    auto identity = nativeEndpointJson(endpoint);
    identity.erase("endpoint_digest");
    identity.erase("consumer_role");
    endpoint.endpointDigest = nativePlanningDigest(nativeCanonicalJson(identity));
  };
  const auto makeProjection = [&sealEndpoint] (const std::string& requestId,
                                  const std::string& planDigest,
                                  const std::string& outputScope) {
    NativeSelectionProjectionV3 projection;
    projection.provider = "/provider/a";
    projection.requestId = requestId;
    projection.attempt = 1;
    projection.planDigest = planDigest;
    projection.canonicalArtifactName = "/model/spec194";
    projection.executionRole.roleId = "encoder";
    projection.assembly.role = "encoder";
    projection.assembly.selectedRole = "encoder";
    projection.assembly.rank = 3;
    projection.assembly.artifactDigest = "sha256:" + std::string(64, 'a');
    projection.assembly.modelManifestDigest = "sha256:" + std::string(64, 'b');
    projection.assembly.graphDigest = "sha256:" + std::string(64, 'c');
    projection.assembly.backend = "onnxruntime-cpu";
    projection.assembly.protectionEpoch = "plaintext-v1";
    projection.assembly.deviceSet = {"cpu"};
    projection.dataflow.requestId = requestId;
    projection.dataflow.attempt = projection.attempt;
    projection.dataflow.planDigest = planDigest;
    projection.dataflow.role = projection.executionRole.roleId;

    NativeTensorEndpointV3 output;
    output.producerNamespace = projection.provider;
    output.requester = "/requester";
    output.requestId = requestId;
    output.attempt = projection.attempt;
    output.planDigest = planDigest;
    output.groupId = outputScope;
    output.groupEpoch = "1";
    output.operation = "PIPELINE";
    output.sourceKind = "ROLE";
    output.producerRole = projection.dataflow.role;
    output.consumerRole = "decoder";
    output.consumerRoles = {output.consumerRole};
    output.tensorId = "activation";
    output.bundleTensorNames = {"hidden"};
    output.tensorDigest = "sha256:" + std::string(64, '4');
    output.layoutDigest = "sha256:" + std::string(64, '5');
    output.targetLayoutDigest = output.layoutDigest;
    output.manifestDigest = "sha256:" + std::string(64, '6');
    output.endpointDigest = "sha256:" + std::string(64, '7');
    output.segmentCount = 1;
    output.securityProfile = "NDNSF_DATA_V1";
    output.noProgressDeadlineMs = 5000;
    output.hardDeadlineMs = 10000;
    sealEndpoint(output);
    projection.dataflow.mayPublish.push_back(std::move(output));
    return projection;
  };

  NativeProviderRunnerReuseCache cache(2);
  const auto first = makeProjection(
    "/NDNSF/DI/REQUEST/runner-cache-1", "sha256:" + std::string(64, '1'), "group-a");
  auto runner = std::make_shared<TestRunner>();
  cache.publish(first, "/provider/a", "boot-a", {}, runner);

  // New request and plan identities may still reuse the complete runner when
  // the authenticated output mapping embedded in its spec is identical.
  const auto sameOutput = makeProjection(
    "/NDNSF/DI/REQUEST/runner-cache-2", "sha256:" + std::string(64, '2'), "group-a");
  BOOST_CHECK(cache.lookup(sameOutput, "/provider/a", "boot-a", {}) == runner);

  const auto changedOutput = makeProjection(
    "/NDNSF/DI/REQUEST/runner-cache-3", "sha256:" + std::string(64, '3'), "group-b");
  BOOST_CHECK(!cache.lookup(changedOutput, "/provider/a", "boot-a", {}));

  NativeProviderRunnerReuseCache genericCache(
    2, NativeProviderRunnerReuseCache::OutputBinding::RoleContract);
  genericCache.publish(first, "/provider/a", "boot-a", {}, runner);
  BOOST_CHECK(genericCache.lookup(changedOutput, "/provider/a", "boot-a", {}) == runner);
  auto differentContract = changedOutput;
  differentContract.dataflow.mayPublish.front().bundleTensorNames = {"other"};
  sealEndpoint(differentContract.dataflow.mayPublish.front());
  BOOST_CHECK(!genericCache.lookup(differentContract, "/provider/a", "boot-a", {}));
  differentContract = changedOutput;
  differentContract.assembly.backend = "onnxruntime-cuda";
  BOOST_CHECK(!genericCache.lookup(differentContract, "/provider/a", "boot-a", {}));
  differentContract = changedOutput;
  differentContract.assembly.recipeDigest = "sha256:" + std::string(64, '9');
  BOOST_CHECK(!genericCache.lookup(differentContract, "/provider/a", "boot-a", {}));
  differentContract = changedOutput;
  differentContract.canonicalArtifactName = "/model/other";
  BOOST_CHECK(!genericCache.lookup(differentContract, "/provider/a", "boot-a", {}));
  BOOST_CHECK(!genericCache.lookup(changedOutput, "/provider/a", "boot-b", {}));

  NativeProviderRunnerReuseCache boundCache(1);
  auto mutableProjection = first;
  auto bound = boundCache.bindPublisher(mutableProjection, "/provider/a", "boot-a", {});
  mutableProjection.dataflow.mayPublish.front().groupId = "mutated-after-bind";
  sealEndpoint(mutableProjection.dataflow.mayPublish.front());
  bound(runner);
  BOOST_CHECK(boundCache.lookup(first, "/provider/a", "boot-a", {}) == runner);
  BOOST_CHECK(!boundCache.lookup(mutableProjection, "/provider/a", "boot-a", {}));
  boundCache.publish(changedOutput, "/provider/a", "boot-a", {}, runner);
  BOOST_CHECK(!boundCache.lookup(first, "/provider/a", "boot-a", {}));
  bound(runner);
  BOOST_CHECK(boundCache.lookup(first, "/provider/a", "boot-a", {}) == runner);
  boundCache.clear();
  bound(runner);
  BOOST_CHECK(boundCache.lookup(first, "/provider/a", "boot-a", {}) == runner);
  auto invalid = first;
  invalid.dataflow.requestId = "/wrong/request";
  BOOST_CHECK_THROW(boundCache.bindPublisher(invalid, "/provider/a", "boot-a", {}),
                    std::invalid_argument);

  auto streaming = first;
  streaming.generationContract.enabled = true;
  streaming.generationContract.maxGeneratedTokens = 128;
  streaming.generationContract.streamingOperationStride = 1;
  streaming.dataflow.mayPublish.clear();
  for (std::uint64_t epoch = 0; epoch <= 128; ++epoch) {
    auto endpoint = first.dataflow.mayPublish.front();
    endpoint.round = epoch;
    endpoint.namingOperationStride = 1;
    sealEndpoint(endpoint);
    streaming.dataflow.mayPublish.push_back(std::move(endpoint));
  }
  NativeProviderRunnerReuseCache repeatedCache(1);
  const auto begin = std::chrono::steady_clock::now();
  auto repeated = repeatedCache.bindPublisher(streaming, "/provider/a", "boot-a", {});
  const auto prepared = std::chrono::steady_clock::now();
  for (std::size_t epoch = 0; epoch < 128; ++epoch)
    repeated(runner);
  const auto boundDone = std::chrono::steady_clock::now();
  NativeProviderRunnerReuseCache oneShotCache(1);
  for (std::size_t epoch = 0; epoch < 128; ++epoch)
    oneShotCache.publish(streaming, "/provider/a", "boot-a", {}, runner);
  const auto directDone = std::chrono::steady_clock::now();
  BOOST_CHECK(repeatedCache.lookup(streaming, "/provider/a", "boot-a", {}) == runner);
  BOOST_CHECK(oneShotCache.lookup(streaming, "/provider/a", "boot-a", {}) == runner);
  const auto micros = [] (const auto& elapsed) {
    return std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
  };
  BOOST_TEST_MESSAGE("SPEC216_RUNNER_REUSE_BENCH bind_us=" << micros(prepared - begin)
    << " bound_publish_128_us=" << micros(boundDone - prepared)
    << " direct_publish_128_us=" << micros(directDone - boundDone));
}

BOOST_AUTO_TEST_CASE(RoleContractRunnerOutputsKeepModelAndStateMetadata)
{
  NativeModelRunnerSpec spec;
  spec.kind = "onnx";
  spec.metadata = {{"outputScope", "old-group"}, {"outputScope.hidden", "old-edge"},
    {"output_scope.0", "old-edge"}, {"outputBundleScope", "old-bundle"},
    {"output_bundle_scope", "old-bundle"}, {"passthroughTensors", "images"},
    {"outputAlias.hidden", "activation"}, {"kvOutputScope", "state"},
    {"forceOutputBundle", "true"}, {"protectedIdentity", "protected-model"}};
  const auto retained = std::map<std::string, std::string>{
    {"passthroughTensors", "images"}, {"outputAlias.hidden", "activation"},
    {"kvOutputScope", "state"}, {"forceOutputBundle", "true"},
    {"protectedIdentity", "protected-model"}};
  NativeProviderRunnerReuseCache::prepareRoleContractOutputs(spec);
  BOOST_CHECK(spec.metadata == retained);
  spec.metadata["is_final"] = "true";
  NativeProviderRunnerReuseCache::prepareRoleContractOutputs(spec);
  BOOST_CHECK_EQUAL(spec.metadata.at("outputScope"), "final-response");
  spec.kind = "native-yolo-merge";
  spec.metadata["outputScope"] = "merge-output";
  const auto original = spec.metadata;
  NativeProviderRunnerReuseCache::prepareRoleContractOutputs(spec);
  BOOST_CHECK(spec.metadata == original);
}

BOOST_AUTO_TEST_CASE(OnnxSessionResidencyQueryRequiresExactIdentityAndIsReadOnly)
{
  OnnxRuntimeSessionCache cache(
    OnnxRuntimeSessionCache::Config{std::chrono::milliseconds(120000), 2});
  NativeProviderResidencyIdentityV4 identity{
    "sha256:" + std::string(64, 'a'),
    "sha256:" + std::string(64, 'b'),
    "sha256:" + std::string(64, 'c'),
    "encoder", 3, "onnxruntime-cpu", "plaintext-v1", {"cpu"}};
  const std::vector<NativeJson> candidates{identity.candidateArtifact()};
  const auto before = cache.counters();
  auto empty = cache.queryPlacementResidencyV4(candidates, "boot-a", 1000, 5000);
  BOOST_CHECK(empty.complete);
  BOOST_CHECK(empty.rows.empty());
  BOOST_CHECK(empty.owner == NativeProviderResidencyOwnerV4::OnnxRuntimeSessionCache);

  auto lease = cache.acquire("session-a", [] {
    return std::static_pointer_cast<void>(std::make_shared<int>(7));
  }, std::chrono::steady_clock::time_point::max(), {}, {identity});
  BOOST_REQUIRE(lease);
  const auto loaded = cache.queryPlacementResidencyV4(candidates, "boot-a", 1000, 5000);
  BOOST_CHECK(loaded.complete);
  BOOST_REQUIRE_EQUAL(loaded.rows.size(), 1U);
  BOOST_CHECK_EQUAL(loaded.rows.front().at("state").get<std::string>(), "EXACT_LOADED");
  BOOST_CHECK_EQUAL(loaded.rows.front().at("runtime_generation").get<std::string>(),
                    "boot-a:session-a");
  const auto after = cache.counters();
  BOOST_CHECK_EQUAL(after.loads, before.loads + 1);
  BOOST_CHECK_EQUAL(after.hits, before.hits);

  lease = {};
  auto unidentified = cache.acquire("session-b", [] {
    return std::static_pointer_cast<void>(std::make_shared<int>(8));
  });
  BOOST_REQUIRE(unidentified);
  const auto unknown = cache.queryPlacementResidencyV4(candidates, "boot-a", 1000, 5000);
  BOOST_CHECK(!unknown.complete);
  BOOST_CHECK(unknown.rows.empty());
}

BOOST_AUTO_TEST_CASE(PersistentRecipeResidencyIndexUsesExactCandidatePaths)
{
  const auto root = std::filesystem::temp_directory_path() /
    ("spec194-residency-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup
  {
    std::filesystem::path path;
    ~Cleanup()
    {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  };
  Cleanup cleanup{root};
  NativePersistentRecipeResidencyIndex index(root);
  NativeProviderResidencyIdentityV4 identity{
    "sha256:" + std::string(64, 'a'),
    "sha256:" + std::string(64, 'b'),
    "sha256:" + std::string(64, 'c'),
    "encoder", 3, "onnxruntime-cpu", "plaintext-v1", {"cpu"}};
  const auto candidate = identity.candidateArtifact();
  auto empty = index.queryPlacementResidencyV4({candidate}, "boot-a", 1000, 5000);
  BOOST_REQUIRE(empty.complete);
  BOOST_CHECK(empty.rows.empty());

  BOOST_REQUIRE(index.beginUpdate());
  const auto duringUpdate = index.queryPlacementResidencyV4(
    {candidate}, "boot-a", 1000, 5000);
  BOOST_CHECK(!duringUpdate.complete);
  BOOST_CHECK(duringUpdate.rows.empty());
  const auto directory = root / "assembled" / "encoder" / std::string(64, 'd');
  std::filesystem::create_directories(directory);
  {
    std::ofstream model(directory / "model.onnx", std::ios::binary);
    model << "metadata-only-query-fixture";
    BOOST_REQUIRE(model.good());
  }
  {
    const NativeJson manifest{
      {"artifactDigest", identity.artifactDigest},
      {"modelManifestDigest", identity.modelManifestDigest},
      {"graphDigest", identity.graphDigest}, {"role", identity.role},
      {"rank", identity.rank}, {"recipeDigest", "sha256:" + std::string(64, 'd')}};
    std::ofstream output(directory / "manifest.json", std::ios::binary);
    output << nativeCanonicalJson(manifest);
    BOOST_REQUIRE(output.good());
  }
  BOOST_REQUIRE(index.record(identity, directory));
  const auto observed = index.queryPlacementResidencyV4({candidate}, "boot-a", 1000, 5000);
  BOOST_REQUIRE(observed.complete);
  BOOST_REQUIRE_EQUAL(observed.rows.size(), 1U);
  BOOST_CHECK_EQUAL(observed.rows.front().at("state").get<std::string>(), "ASSEMBLED");
  BOOST_CHECK_EQUAL(observed.rows.front().at("device_set")[0].get<std::string>(), "cpu");

  std::filesystem::remove(directory / "model.onnx");
  const auto stale = index.queryPlacementResidencyV4({candidate}, "boot-a", 1000, 5000);
  BOOST_CHECK(stale.complete);
  BOOST_CHECK(stale.rows.empty());
}

BOOST_AUTO_TEST_CASE(ProtectedPlaintextResidencyQueryUsesTrackedIdentity)
{
  const auto root = std::filesystem::temp_directory_path() /
    ("spec194-plaintext-residency-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup
  {
    std::filesystem::path path;
    ~Cleanup()
    {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{root};
  NativeProtectedPlaintextCache cache(root, 2);
  NativeProtectedPlaintextCacheKey key;
  key.encryptedArtifactDigest = "sha256:" + std::string(64, 'a');
  key.modelManifestDigest = "sha256:" + std::string(64, 'b');
  key.graphDigest = "sha256:" + std::string(64, 'c');
  key.initializerDigest = "sha256:" + std::string(64, 'd');
  key.role = "encoder";
  key.recipeDigest = "sha256:" + std::string(64, 'e');
  key.backendAbi = "onnx-abi-v1";
  key.roleAssemblySpecDigest = "sha256:" + std::string(64, 'f');
  key.keyReferenceDigest = "sha256:" + std::string(64, '1');
  NativeProviderResidencyIdentityV4 identity{
    "sha256:" + std::string(64, 'a'), key.modelManifestDigest, key.graphDigest,
    "encoder", 3, "onnxruntime-cpu", "protected-v1", {"cpu"}};
  NativeRequestControl control{"/request/spec194/plaintext", 1,
    std::chrono::steady_clock::now() + std::chrono::seconds(5), {}};
  auto lease = cache.acquire(key, control, 1024,
    [] (const std::filesystem::path& path) {
      std::ofstream output(path, std::ios::binary);
      output << "plaintext fixture";
      if (!output.good())
        throw std::runtime_error("could not write protected plaintext fixture");
    }, identity);
  BOOST_REQUIRE(lease);
  const auto observed = cache.queryPlacementResidencyV4(
    {identity.candidateArtifact()}, "boot-a", 1000, 5000);
  BOOST_CHECK(observed.complete);
  BOOST_REQUIRE_EQUAL(observed.rows.size(), 1U);
  BOOST_CHECK_EQUAL(observed.rows.front().at("state").get<std::string>(), "ASSEMBLED");
  cache.stop();
}

BOOST_AUTO_TEST_CASE(ProductionResidencyQueryRequiresEveryLiveOwner)
{
  const auto root = std::filesystem::temp_directory_path() /
    ("spec194-owner-query-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup
  {
    std::filesystem::path path;
    ~Cleanup()
    {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{root};

  NativeProviderResidencyIdentityV4 identity{
    "sha256:" + std::string(64, 'a'),
    "sha256:" + std::string(64, 'b'),
    "sha256:" + std::string(64, 'c'),
    "encoder", 3, "onnxruntime-cpu", "plaintext-v1", {"cpu"}};
  ProviderArtifactCache artifactCache;
  NativeProviderRunnerReuseCache runnerCache(2);
  OnnxRuntimeSessionCache sessionCache;
  NativeProtectedPlaintextCache plaintextCache(root, 2);
  NativePersistentRecipeResidencyIndex persistentIndex(root);

  const auto empty = queryNativeProviderResidencyOwnersV4(
    {identity.candidateArtifact()}, "boot-a", 1000, 5000,
    &artifactCache, &runnerCache, &sessionCache, &plaintextCache, &persistentIndex);
  BOOST_CHECK(empty.known);
  BOOST_CHECK(empty.rows.empty());

  const auto unavailable = queryNativeProviderResidencyOwnersV4(
    {identity.candidateArtifact()}, "boot-a", 1000, 5000,
    &artifactCache, &runnerCache, &sessionCache, &plaintextCache, nullptr);
  BOOST_CHECK(!unavailable.known);
  BOOST_CHECK(unavailable.rows.empty());
}

BOOST_AUTO_TEST_CASE(ResidencyUniquenessUsesFullArtifactIdentity)
{
  Fixture f;
  const std::vector<NativeJson> candidates{f.candidateArtifact()};
  const auto request = f.requestPayload(candidates);

  auto firstRow = f.residencyRow();
  auto secondRow = firstRow;
  secondRow["backend"] = "onnxruntime-cuda";
  secondRow["device_set"] = NativeJson::array({"gpu0"});
  NativeProviderResidencySnapshotV4 observed;
  observed.known = true;
  observed.rows = {firstRow, secondRow};

  auto issued = issueNativeProviderOfferV4(
    request, f.offerConfig(), f.telemetry(), 1000, {}, observed);
  BOOST_REQUIRE(issued);
  const auto decoded = decodeNativeProviderOfferV4(issued->capabilityFieldValue);
  BOOST_REQUIRE_EQUAL(decoded.values.at("residency").size(), 2);
  BOOST_CHECK_NO_THROW(f.makeAdmission().verifyV4(
    f.ackFor(issued->capabilityFieldValue), f.binding(candidates), 1200));

  auto unsupportedBackend = f.residencyRow();
  unsupportedBackend["backend"] = "onnxruntime-openvino";
  observed.rows = {unsupportedBackend};
  BOOST_CHECK_THROW(issueNativeProviderOfferV4(
    request, f.offerConfig(), f.telemetry(), 1000, {}, observed),
    std::invalid_argument);

  observed.rows = {firstRow, firstRow};
  BOOST_CHECK_THROW(issueNativeProviderOfferV4(
    request, f.offerConfig(), f.telemetry(), 1000, {}, observed),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(PreservesMeasuredZeroWorkerCapacity)
{
  Fixture f;
  auto telemetry = f.telemetry();
  telemetry.sequence = 1;
  telemetry.sampledAtMs = 990;
  telemetry.capacity.workerCount = 0;
  telemetry.capacity.activeWorkerCount = 0;
  telemetry.capacity.readyQueueDepth = 0;
  telemetry.capacity.readyQueueCapacity = 0;
  const auto issued = issueNativeProviderOfferV4(
    f.requestPayload(), f.offerConfig(), telemetry, 1000);
  BOOST_REQUIRE(issued);
  const auto decoded = decodeNativeProviderOfferV4(issued->capabilityFieldValue);
  BOOST_CHECK_EQUAL(decoded.values.at("capacity_state").get<std::string>(), "OBSERVED");
  BOOST_CHECK_EQUAL(decoded.values.at("capacity").at("worker_count").get<std::uint64_t>(), 0U);
}

BOOST_AUTO_TEST_CASE(ReportsDirectedFetchAndProducesResignedDeterministicTrim)
{
  Fixture f;
  const auto row = f.dataFetchRow();
  const auto issued = issueNativeProviderOfferV4(
    f.requestPayload(), f.offerConfig(), f.telemetry(), 1000, {row});
  BOOST_REQUIRE(issued);
  BOOST_REQUIRE_EQUAL(issued->fallbackCapabilityFieldValues.size(), 1);
  const auto complete = decodeNativeProviderOfferV4(issued->capabilityFieldValue);
  BOOST_REQUIRE_EQUAL(complete.values.at("link_observations").size(), 1);
  BOOST_CHECK_EQUAL(complete.values.at("link_observations")[0].at("kind").get<std::string>(), "DATA_FETCH");
  BOOST_CHECK_EQUAL(complete.values.at("link_observations")[0].at("src_peer").get<std::string>(), "/provider/b");
  BOOST_CHECK_EQUAL(complete.values.at("link_observations")[0].at("dst_peer").get<std::string>(), "/provider/a");
  const auto trimmed = decodeNativeProviderOfferV4(
    issued->fallbackCapabilityFieldValues.front());
  BOOST_CHECK(trimmed.values.at("link_observations").empty());
  BOOST_CHECK(trimmed.values.at("observations_truncated").get<bool>());
  const auto admitted = f.makeAdmission().verifyV4(
    f.ackFor(issued->fallbackCapabilityFieldValues.front()), f.binding(), 1200);
  BOOST_CHECK_EQUAL(admitted.observation().candidateSetDigest, f.candidateSetDigest);
}

BOOST_AUTO_TEST_CASE(FreezesWholeRosterSnapshotWithBootTopologyAndLifetimeBindings)
{
  Fixture f;
  const auto topologyA = nativePlanningDigest("topology-provider-a");
  const auto topologyB = nativePlanningDigest("topology-provider-b");
  const auto makeRow = [&](const std::string& peerBoot, const std::string& topology,
                           std::uint64_t capturedAt = 2990) {
    auto row = f.dataFetchRow();
    row["src_boot_epoch"] = peerBoot;
    row["topology_digest"] = topology;
    row["captured_at_ms"] = capturedAt;
    row["expires_at_ms"] = 9000;
    row.erase("source_digest");
    row["source_digest"] = nativePlanningDigest(nativeCanonicalJson(row));
    return row;
  };
  auto encoderArtifact = f.candidateArtifact();
  auto decoderArtifact = encoderArtifact;
  decoderArtifact["artifact_digest"] = nativePlanningDigest("decoder-artifact");
  decoderArtifact["role"] = "decoder";
  decoderArtifact["rank"] = 1;
  std::vector<NativeJson> artifacts{encoderArtifact, decoderArtifact};
  std::sort(artifacts.begin(), artifacts.end(), [](const auto& left, const auto& right) {
    return nativeCanonicalJson(left) < nativeCanonicalJson(right);
  });
  const auto payload = f.requestPayload(artifacts);
  const auto binding = f.binding(artifacts);
  const auto cache = f.preparedCache(artifacts);
  auto configA = f.offerConfig("/provider/a");
  auto configB = f.offerConfig("/provider/b");
  configA.offerTtlMs = 9000;
  configB.offerTtlMs = 9000;
  const auto issuedA = issueNativeProviderOfferV4(
    payload, configA,
    f.measuredTelemetry("/provider/a", "boot-provider-a-1", topologyA),
    3000, {makeRow("boot-provider-b-1", topologyA)});
  const auto issuedB = issueNativeProviderOfferV4(
    payload, configB,
    f.measuredTelemetry("/provider/b", "boot-provider-b-1", topologyB), 3000);
  BOOST_REQUIRE(issuedA);
  BOOST_REQUIRE(issuedB);
  auto admission = f.makeAdmission();
  std::vector<NativeAdmittedOfferV4> admitted;
  auto ackA = f.ackFor(issuedA->capabilityFieldValue, "/provider/a");
  auto capabilitiesA = ackA.ack.getProviderCapabilityOffer();
  capabilitiesA.setField(NATIVE_PROVIDER_TENSOR_TRANSPORT_V1_FIELD,
    nativeCanonicalJson(NativeJson{{"schema", "ndnsf-di-tensor-transport-v1"},
      {"boot_epoch", "boot-provider-a-1"}, {"segment_payload_bytes", std::uint64_t{6000}}}));
  ackA.ack.setProviderCapabilityOffer(capabilitiesA);
  admitted.push_back(admission.verifyV4(
    ackA, binding, 3100));
  admitted.push_back(admission.verifyV4(
    f.ackFor(issuedB->capabilityFieldValue, "/provider/b"), binding, 3100));
  const auto sampleA = NativeJson{
    {"provider", "/provider/a"},
    {"sample", NativeJson{{"provider", "/provider/a"},
      {"service", f.service}, {"observed_at_ms", std::uint64_t{3100}},
      {"kind", "ACK_RTT"}}}};
  const auto sampleB = NativeJson{{"provider", "/provider/b"}, {"sample", nullptr}};
  const auto snapshot = buildNativePlacementObservationSnapshotV4(
    binding, nativePlanningDigest("closed-ack-roster"), admitted,
    {sampleB, sampleA}, cache, 1, 3200);
  BOOST_CHECK_EQUAL(snapshot.value.at("provider_offers")[0]
    .at("tensor_segment_payload_bytes").get<std::uint64_t>(), 6000U);
  BOOST_CHECK(!snapshot.value.at("provider_offers")[1]
    .contains("tensor_segment_payload_bytes"));
  // Reusing canonical inventory bytes must still reject changed input even
  // when the advertised digest and prepared-cache digest remain identical.
  auto changedInventory = binding;
  BOOST_REQUIRE(!changedInventory.candidateArtifacts.empty());
  changedInventory.candidateArtifacts.front()["unexpected_identity_field"] = "changed";
  BOOST_CHECK_EXCEPTION(buildNativePlacementObservationSnapshotV4(
    changedInventory, nativePlanningDigest("closed-ack-roster"), admitted,
    {sampleB, sampleA}, cache, 1, 3200), std::invalid_argument,
    [] (const std::invalid_argument& error) {
      return std::string(error.what()) == "V4 prepared graph differs from the advertised inventory";
    });
  auto emptyInventory = binding;
  emptyInventory.candidateArtifacts.clear();
  BOOST_CHECK_THROW(buildNativePlacementObservationSnapshotV4(
    emptyInventory, nativePlanningDigest("closed-ack-roster"), admitted,
    {sampleB, sampleA}, cache, 1, 3200), std::invalid_argument);
  BOOST_CHECK_EQUAL(snapshot.value.at("plan_deadline_ms").get<std::uint64_t>(), 10000U);
  BOOST_CHECK_EQUAL(snapshot.capturedAtMs, 3200U);
  BOOST_CHECK_EQUAL(snapshot.expiresAtMs, 7990U);
  BOOST_CHECK_EQUAL(snapshot.value.at("expires_at_ms").get<std::uint64_t>(), 7990U);
  BOOST_REQUIRE_EQUAL(snapshot.value.at("eligible_link_observations").size(), 1U);
  BOOST_CHECK_EQUAL(snapshot.value.at("eligible_link_observations")[0]
    .at("source_kind").get<std::string>(), "ACK_CACHE");
  BOOST_REQUIRE_EQUAL(snapshot.value.at("prepared_role_graph").size(), 1U);
  BOOST_REQUIRE_EQUAL(snapshot.value.at("prepared_role_graph")[0]
    .at("roles").size(), 2U);
  BOOST_REQUIRE_EQUAL(snapshot.value.at("prepared_role_graph")[0]
    .at("dependencies").size(), 1U);
  BOOST_CHECK_EQUAL(snapshot.value.at("prepared_role_graph")[0]
    .at("dependencies")[0].at("estimated_bytes").get<std::uint64_t>(), 4096U);
  auto requestCache = cache;
  auto& requestCandidate = requestCache.candidates.front();
  requestCandidate.candidate.executionPlan.dependencies.front().expectedBytes = 0;
  requestCandidate.candidate.executionPlan.dependencies.front().operationKind = "ACTIVATION";
  requestCandidate.roles[0].expectedOutputs = {{"actual-hidden", "float32", {"batch", "seq", 16}}};
  requestCandidate.roles[1].expectedInputs = requestCandidate.roles[0].expectedOutputs;
  const auto requestProfile = NativeJson{{"schema", "ndnsf-di-request-transfer-profile-v1"},
    {"source_digest", nativePlanningDigest("source")},
    {"input_digest", nativePlanningDigest("request-input")}, {"initial_context", true},
    {"request_contract_digest", nativePlanningDigest("request-envelope")},
    {"dependencies", NativeJson::array({NativeJson{
      {"producer_role", requestCandidate.roles[0].selectedRole},
      {"consumer_role", requestCandidate.roles[1].selectedRole},
      {"tensors", NativeJson::array({NativeJson{{"name", "actual-hidden"},
        {"dtype", "float32"}, {"shape", {1, 3, 16}}}})}}})}};
  const auto requestSnapshot = buildNativePlacementObservationSnapshotV4(
    binding, nativePlanningDigest("closed-ack-roster"), admitted,
    {sampleB, sampleA}, requestCache, 1, 3200, nullptr, &requestProfile);
  const auto& requestEdge = requestSnapshot.value.at("prepared_role_graph")[0].at("dependencies")[0];
  BOOST_CHECK_EQUAL(requestEdge.at("estimated_bytes").get<std::uint64_t>(), 261U);
  BOOST_CHECK_EQUAL(requestEdge.at("estimated_bytes").get<std::uint64_t>(),
    encodeTensorBundle({NamedTensor{"actual-hidden", TensorElementType::Float32,
      {1, 3, 16}, std::vector<std::uint8_t>(192)}}).size());
  BOOST_CHECK_EQUAL(requestEdge.at("estimated_bytes_source").get<std::string>(),
    "request-bound-encoded-tensor-bundle");
  BOOST_CHECK(requestSnapshot.value.at("request_transfer_profile") == requestProfile);
  BOOST_CHECK(requestSnapshot.digest != snapshot.digest);
  BOOST_CHECK_EQUAL(snapshot.digest,
    nativePlanningDigest(nativeCanonicalJson(snapshot.value)));
  BOOST_CHECK_EQUAL(snapshot.value.at("requester_provider_samples")[0]
    .at("provider").get<std::string>(), "/provider/a");
  auto sealerContext = binding;
  sealerContext.placementObservationSnapshot = snapshot.value;
  sealerContext.placementObservationSnapshotDigest = snapshot.digest;
  NativePlanSealingInputs sealerInputs;
  sealerInputs.expiresAtMs = snapshot.expiresAtMs;
  sealerInputs.placementObservationSnapshotDigest = snapshot.digest;
  const auto closedAckDigest = nativePlanningDigest("closed-ack-roster");
  BOOST_CHECK_NO_THROW(NativePlanSealer::validateObservationSnapshotBinding(
    sealerContext, sealerInputs, closedAckDigest, 3200));
  auto changedSnapshot = sealerContext;
  changedSnapshot.placementObservationSnapshot["expires_at_ms"] = 7900;
  BOOST_CHECK_THROW(NativePlanSealer::validateObservationSnapshotBinding(
    changedSnapshot, sealerInputs, closedAckDigest, 3200), std::invalid_argument);
  auto foreignAck = closedAckDigest;
  foreignAck[10] = foreignAck[10] == '0' ? '1' : '0';
  BOOST_CHECK_THROW(NativePlanSealer::validateObservationSnapshotBinding(
    sealerContext, sealerInputs, foreignAck, 3200), std::invalid_argument);
  BOOST_CHECK_THROW(buildNativePlacementObservationSnapshotV4(
    binding, nativePlanningDigest("closed-ack-roster"), {admitted.front()},
    {sampleA, sampleB}, cache, 1, 1400), std::invalid_argument);

  const auto makeSnapshotForRemote = [&](const std::string& remoteBoot,
                                         const std::string& rowTopology) {
    const auto replaced = issueNativeProviderOfferV4(
      payload, configA,
      f.measuredTelemetry("/provider/a", "boot-provider-a-1", topologyA),
      3000, {makeRow(remoteBoot, rowTopology)});
    if (!replaced)
      throw std::runtime_error("failed to issue peer-binding test offer");
    std::vector<NativeAdmittedOfferV4> rows;
    rows.push_back(admission.verifyV4(
      f.ackFor(replaced->capabilityFieldValue, "/provider/a"), binding, 3100));
    rows.push_back(admission.verifyV4(
      f.ackFor(issuedB->capabilityFieldValue, "/provider/b"), binding, 3100));
    return buildNativePlacementObservationSnapshotV4(
      binding, nativePlanningDigest("closed-ack-roster"), rows,
      {sampleA, sampleB}, cache, 1, 3200);
  };
  const auto oldBoot = makeSnapshotForRemote("old-provider-b-boot", topologyA);
  BOOST_CHECK(oldBoot.value.at("eligible_link_observations").empty());
  BOOST_CHECK_EQUAL(oldBoot.value.at("ineligible_link_observation_count").get<std::size_t>(), 1U);
  const auto oldTopology = makeSnapshotForRemote(
    "boot-provider-b-1", nativePlanningDigest("changed-topology"));
  BOOST_CHECK(oldTopology.value.at("eligible_link_observations").empty());
  BOOST_CHECK_EQUAL(oldTopology.value.at("ineligible_link_observation_count").get<std::size_t>(), 1U);

  const auto staleLink = [&] {
    const auto replaced = issueNativeProviderOfferV4(
      payload, configA,
      f.measuredTelemetry("/provider/a", "boot-provider-a-1", topologyA),
      3000, {makeRow("boot-provider-b-1", topologyA, 950)});
    if (!replaced)
      throw std::runtime_error("failed to issue stale link test offer");
    std::vector<NativeAdmittedOfferV4> rows{
      admission.verifyV4(f.ackFor(replaced->capabilityFieldValue, "/provider/a"),
        binding, 3100),
      admission.verifyV4(f.ackFor(issuedB->capabilityFieldValue, "/provider/b"),
        binding, 3100)};
    return buildNativePlacementObservationSnapshotV4(
      binding, nativePlanningDigest("closed-ack-roster"), rows,
      {sampleA, sampleB}, cache, 1, 3200);
  }();
  BOOST_CHECK_EQUAL(staleLink.expiresAtMs, 5950U);

  auto staleResidencyRow = f.residencyRow();
  staleResidencyRow["process_epoch"] = "boot-provider-b-1";
  staleResidencyRow["expires_at_ms"] = 9000;
  NativeProviderResidencySnapshotV4 residency;
  residency.known = true;
  residency.rows = {staleResidencyRow};
  const auto withStaleResidency = issueNativeProviderOfferV4(
    payload, configB,
    f.measuredTelemetry("/provider/b", "boot-provider-b-1", topologyB),
    3000, {}, residency);
  BOOST_REQUIRE(withStaleResidency);
  std::vector<NativeAdmittedOfferV4> residencyOffers{
    admitted.front(), admission.verifyV4(
      f.ackFor(withStaleResidency->capabilityFieldValue, "/provider/b"), binding, 3100)};
  const auto staleResidency = buildNativePlacementObservationSnapshotV4(
    binding, nativePlanningDigest("closed-ack-roster"), residencyOffers,
    {sampleA, sampleB}, cache, 1, 3200);
  BOOST_CHECK_EQUAL(staleResidency.expiresAtMs, 5950U);

  auto staleSample = sampleA;
  staleSample["sample"]["observed_at_ms"] = 1;
  BOOST_CHECK_THROW(buildNativePlacementObservationSnapshotV4(
    binding, nativePlanningDigest("closed-ack-roster"), admitted,
    {staleSample, sampleB}, cache, 1, 3200), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TensorBundleEncodingSizeMatchesWireAndPreservesLimits)
{
  const std::vector<NamedTensor> tensors{
    {"hidden", TensorElementType::Float32, {1, 4}, std::vector<std::uint8_t>(16)},
    {"mask", TensorElementType::Bool, {1, 8}, std::vector<std::uint8_t>(8)},
    {"positions", TensorElementType::Int64, {2}, std::vector<std::uint8_t>(16)},
    {"零", TensorElementType::Float16, {0}, {}},
    {"scalar", TensorElementType::UInt8, {}, {0xab}}};
  std::vector<TensorBundleEncodingMember> members;
  for (const auto& tensor : tensors)
    members.push_back({tensor.name.size(), tensor.shape.size(), tensor.payload.size()});
  const auto wire = encodeTensorBundle(tensors);
  BOOST_CHECK_EQUAL(tensorBundleEncodedSize(members), wire.size());
  const auto decoded = decodeTensorBundle(wire);
  BOOST_REQUIRE_EQUAL(decoded.size(), tensors.size());
  for (std::size_t i = 0; i < tensors.size(); ++i) {
    BOOST_CHECK_EQUAL(decoded[i].name, tensors[i].name);
    BOOST_CHECK(decoded[i].shape == tensors[i].shape);
    BOOST_CHECK(decoded[i].elementType == tensors[i].elementType);
    BOOST_CHECK(decoded[i].payload == tensors[i].payload);
  }
  const std::vector<std::uint8_t> golden{
    'N','D','I','T','B','0','0','1', 1,0,0,0, 1,0,0,0, 'x',
    5,0,0,0, 0,0,0,0, 1,0,0,0,0,0,0,0, 0xab};
  BOOST_CHECK(encodeTensorBundle({NamedTensor{"x", TensorElementType::UInt8, {}, {0xab}}}) == golden);
  BOOST_CHECK_EQUAL(tensorBundleEncodedSize({}), encodeTensorBundle({}).size());
  BOOST_CHECK_THROW(tensorBundleEncodedSize({{0, 0, 1}}), std::invalid_argument);
  BOOST_CHECK_THROW(tensorBundleEncodedSize({{1025, 0, 1}}), std::invalid_argument);
  BOOST_CHECK_THROW(tensorBundleEncodedSize({{1, 17, 1}}), std::invalid_argument);
  BOOST_CHECK_THROW(tensorBundleEncodedSize({{1, 0, (512ULL << 20) + 1}}), std::invalid_argument);
  BOOST_CHECK_THROW(tensorBundleEncodedSize(std::vector<TensorBundleEncodingMember>(257, {1, 0, 1})),
                    std::invalid_argument);
  BOOST_CHECK_EQUAL(tensorBundleEncodedSize({{1024, 16, 512ULL << 20}}),
                    (512ULL << 20) + 12 + 20 + 1024 + 16 * 8);
  auto invalid = tensors;
  invalid.back().payload.clear();
  BOOST_CHECK_THROW(encodeTensorBundle(invalid), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(SourceShapeTemplateBindsDifferentCurrentInput)
{
  const auto source = nativePlanningDigest("source-shape-template");
  NativeJson profile{{"schema", "ndnsf-di-request-transfer-profile-v1"},
    {"source_digest", source}, {"input_digest", nativePlanningDigest("old-input")},
    {"initial_context", true}, {"dependencies", NativeJson::array()},
    {"shape_template", {
      {"bindings", NativeJson::array({
        NativeJson{{"symbol", "batch"}, {"tensor", "tokens"}, {"axis", 0}},
        NativeJson{{"symbol", "seq"}, {"tensor", "tokens"}, {"axis", 1}}})},
      {"constants", {{"past", 0}}},
      {"dependencies", NativeJson::array({NativeJson{{"producer_role", "encoder"},
        {"consumer_role", "decoder"}, {"tensors", NativeJson::array({
          NativeJson{{"name", "hidden"}, {"dtype", "float32"},
            {"shape", NativeJson::array({"batch", NativeJson{{"add", {"past", "seq"}}}, 4})}}})}}})}}}};
  NativeEncodedRequest request;
  const auto payload = encodeTensorBundle({NamedTensor{
    "tokens", TensorElementType::Int64, {1, 5}, std::vector<std::uint8_t>(40)}});
  request.logicalInputDigest = nativePlanningDigest(payload.data(), payload.size());
  std::string base64(4 * ((payload.size() + 2) / 3) + 1, '\0');
  const auto size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(base64.data()),
                                  payload.data(), payload.size());
  BOOST_REQUIRE_GE(size, 0);
  base64.resize(size);
  const auto envelope = NativeJson{{"schema", "ndnsf-di-request-envelope-v2"},
    {"input_transport", "INLINE"}, {"input_payload_b64", base64}}.dump();
  request.wire.assign(envelope.begin(), envelope.end());
  const auto bound = bindNativeRequestTransferProfileV1(profile, source, request);
  BOOST_REQUIRE(bound);
  BOOST_CHECK((*bound)["input_digest"] == request.logicalInputDigest);
  BOOST_CHECK(!bound->contains("shape_template"));
  BOOST_CHECK((*bound)["dependencies"][0]["tensors"][0]["shape"] == NativeJson({1, 5, 4}));
  NativeSelectionRoleV3 producer, consumer;
  producer.selectedRole = "encoder"; consumer.selectedRole = "decoder";
  producer.expectedOutputs = {{"hidden", "float32", {"batch", "seq", 4}}};
  consumer.expectedInputs = producer.expectedOutputs;
  NativeDependencySpec dependency;
  dependency.producers = {"encoder"}; dependency.consumers = {"decoder"};
  dependency.tensors = {"logical-hidden"}; dependency.operationKind = "ACTIVATION";
  const auto bytes = estimateNativeRequestTransferEncodedBytesV1(
    *bound, source, request.logicalInputDigest, dependency, {producer, consumer});
  BOOST_REQUIRE(bytes);
  BOOST_CHECK_EQUAL(*bytes, encodeTensorBundle({NamedTensor{
    "hidden", TensorElementType::Float32, {1, 5, 4}, std::vector<std::uint8_t>(80)}}).size());
  BOOST_CHECK(!bindNativeRequestTransferProfileV1(profile, nativePlanningDigest("foreign"), request));
  auto wrongRequest = request; wrongRequest.logicalInputDigest = nativePlanningDigest("wrong-input");
  BOOST_CHECK(!bindNativeRequestTransferProfileV1(profile, source, wrongRequest));
  auto changed = profile; changed["initial_context"] = false;
  BOOST_CHECK(!bindNativeRequestTransferProfileV1(changed, source, request));
  changed = profile; changed["shape_template"]["bindings"][1]["axis"] = -1;
  BOOST_CHECK(!bindNativeRequestTransferProfileV1(changed, source, request));
  changed = profile; changed["shape_template"]["bindings"][1]["tensor"] = "missing";
  BOOST_CHECK(!bindNativeRequestTransferProfileV1(changed, source, request));
  changed = profile; changed["shape_template"]["constants"]["batch"] = 1;
  BOOST_CHECK(!bindNativeRequestTransferProfileV1(changed, source, request));
  changed = profile;
  changed["shape_template"]["dependencies"][0]["tensors"][0]["shape"][1] =
    NativeJson{{"multiply", {"seq", 2}}};
  BOOST_CHECK(!bindNativeRequestTransferProfileV1(changed, source, request));
  changed = profile; changed["shape_template"]["constants"]["past"] =
    std::numeric_limits<std::int64_t>::max();
  BOOST_CHECK(!bindNativeRequestTransferProfileV1(changed, source, request));
  changed = profile;
  NativeJson expression = "seq";
  for (unsigned depth = 0; depth < 18; ++depth) expression = NativeJson{{"add", {0, expression}}};
  changed["shape_template"]["dependencies"][0]["tensors"][0]["shape"][1] = expression;
  BOOST_CHECK(!bindNativeRequestTransferProfileV1(changed, source, request));

  auto contextProfile = profile;
  contextProfile["shape_template"]["past_sequence_symbol"] = "past";
  auto contextRequest = request;
  auto contextEnvelope = NativeJson::parse(envelope);
  contextEnvelope["request_id"] = "/request/current-append";
  const auto contextWire = nativeCanonicalJson(contextEnvelope);
  contextRequest.wire.assign(contextWire.begin(), contextWire.end());
  contextRequest.requestContractDigest = nativePlanningDigest(contextWire);
  // The conversation owner's authentication boundary is exercised by its
  // separate production fixtures. Here the already-issued turn is supplied
  // to the geometry binder and its identity/freshness bindings are tested.
  NativeConversationTurn turn;
  turn.ticket = "issued-owner-ticket";
  turn.requestId = "/request/current-append";
  turn.parent.mode = "APPEND_DELTA";
  turn.parent.conversationId = "conversation";
  turn.parent.parentContextEpoch = 1;
  turn.parent.parentCheckpointDigest = nativePlanningDigest("parent-checkpoint");
  turn.parent.planRoleMapDigest = nativePlanningDigest("role-map");
  turn.parent.requestContractDigest = contextRequest.requestContractDigest;
  turn.parent.parentCheckpointWire = nativeCanonicalJson(NativeJson{
    {"conversationId", turn.parent.conversationId}, {"contextEpoch", 1},
    {"checkpointDigest", turn.parent.parentCheckpointDigest},
    {"planRoleMapDigest", turn.parent.planRoleMapDigest}, {"prefixTokenCount", 15}});
  const auto continued = bindNativeRequestTransferProfileV1(
    contextProfile, source, contextRequest, &turn);
  BOOST_REQUIRE(continued);
  BOOST_CHECK((*continued)["schema"] == "ndnsf-di-request-transfer-profile-v2");
  BOOST_CHECK((*continued)["initial_context"] == false);
  BOOST_CHECK((*continued)["dependencies"][0]["tensors"][0]["shape"] == NativeJson({1, 20, 4}));
  const auto contextBytes = estimateNativeRequestTransferEncodedBytesV1(
    *continued, source, contextRequest.logicalInputDigest, dependency, {producer, consumer});
  BOOST_REQUIRE(contextBytes);
  BOOST_CHECK_EQUAL(*contextBytes, encodeTensorBundle({NamedTensor{
    "hidden", TensorElementType::Float32, {1, 20, 4}, std::vector<std::uint8_t>(320)}}).size());
  auto staleTurn = turn; staleTurn.parent.parentContextEpoch = 2;
  BOOST_CHECK(!bindNativeRequestTransferProfileV1(contextProfile, source, contextRequest, &staleTurn));
  staleTurn = turn; staleTurn.parent.parentCheckpointDigest = nativePlanningDigest("wrong-parent");
  BOOST_CHECK(!bindNativeRequestTransferProfileV1(contextProfile, source, contextRequest, &staleTurn));
  staleTurn = turn; staleTurn.parent.requestContractDigest = nativePlanningDigest("old-request");
  BOOST_CHECK(!bindNativeRequestTransferProfileV1(contextProfile, source, contextRequest, &staleTurn));
  staleTurn = turn; staleTurn.requestId = "/wrong-request";
  BOOST_CHECK(!bindNativeRequestTransferProfileV1(contextProfile, source, contextRequest, &staleTurn));
  staleTurn = turn; staleTurn.aborted = true;
  BOOST_CHECK(!bindNativeRequestTransferProfileV1(contextProfile, source, contextRequest, &staleTurn));
  staleTurn = turn; staleTurn.ticket.clear();
  BOOST_CHECK(!bindNativeRequestTransferProfileV1(contextProfile, source, contextRequest, &staleTurn));
  staleTurn = turn; staleTurn.attempt = 2;
  BOOST_CHECK(!bindNativeRequestTransferProfileV1(contextProfile, source, contextRequest, &staleTurn));
  auto recoveryRequest = contextRequest; recoveryRequest.recovery = NativeGenerationRecovery{};
  BOOST_CHECK(!bindNativeRequestTransferProfileV1(contextProfile, source, recoveryRequest, &turn));
  auto unboundProfile = contextProfile;
  unboundProfile["shape_template"].erase("past_sequence_symbol");
  BOOST_CHECK(!bindNativeRequestTransferProfileV1(unboundProfile, source, contextRequest, &turn));
  auto unboundContext = *continued; unboundContext.erase("request_contract_digest");
  BOOST_CHECK(!estimateNativeRequestTransferEncodedBytesV1(
    unboundContext, source, contextRequest.logicalInputDigest, dependency, {producer, consumer}));
}

BOOST_AUTO_TEST_CASE(RequestTransferProfileRequiresCompleteSourceBoundBundle)
{
  const auto source = nativePlanningDigest("pinned-source");
  const auto input = nativePlanningDigest("exact-initial-token-bundle");
  NativeSelectionRoleV3 producer, consumer;
  producer.selectedRole = "encoder"; consumer.selectedRole = "decoder";
  producer.expectedOutputs = {
    {"mask", "float32", {"batch", 1, "seq", "past+seq"}},
    {"rotary-cos", "float32", {"batch", 1, "seq", 128}},
    {"rotary-sin", "float32", {"batch", 1, "seq", 128}},
    {"hidden", "float32", {"batch", "seq", 1024}}};
  consumer.expectedInputs = producer.expectedOutputs;
  NativeDependencySpec dependency;
  dependency.producers = {"encoder"}; dependency.consumers = {"decoder"};
  dependency.tensors = {"logical-hidden"}; dependency.operationKind = "ACTIVATION";
  NativeJson profile{{"schema", "ndnsf-di-request-transfer-profile-v1"},
    {"source_digest", source}, {"input_digest", input}, {"initial_context", true},
    {"dependencies", NativeJson::array({NativeJson{{"producer_role", "encoder"},
      {"consumer_role", "decoder"}, {"tensors", NativeJson::array({
        NativeJson{{"name", "mask"}, {"dtype", "float32"}, {"shape", {1, 1, 3, 3}}},
        NativeJson{{"name", "rotary-cos"}, {"dtype", "float32"}, {"shape", {1, 1, 3, 128}}},
        NativeJson{{"name", "rotary-sin"}, {"dtype", "float32"}, {"shape", {1, 1, 3, 128}}},
        NativeJson{{"name", "hidden"}, {"dtype", "float32"}, {"shape", {1, 3, 1024}}}})}}})}};
  const auto estimate = [&](const NativeJson& value, const std::string& pinnedSource,
                             const std::string& actualInput) {
    return estimateNativeRequestTransferBytesV1(value, pinnedSource, actualInput,
      dependency, {producer, consumer});
  };
  const auto bytes = estimate(profile, source, input);
  BOOST_REQUIRE(bytes); BOOST_CHECK_EQUAL(*bytes, 15396U);
  const auto encodedBytes = estimateNativeRequestTransferEncodedBytesV1(
    profile, source, input, dependency, {producer, consumer});
  BOOST_REQUIRE(encodedBytes);
  std::vector<NamedTensor> tensors;
  for (const auto& row : profile.at("dependencies")[0].at("tensors")) {
    NamedTensor tensor;
    tensor.name = row.at("name").get<std::string>();
    tensor.shape = row.at("shape").get<std::vector<std::int64_t>>();
    std::size_t payload = sizeof(float);
    for (const auto dimension : tensor.shape) payload *= static_cast<std::size_t>(dimension);
    tensor.payload.resize(payload);
    tensors.push_back(std::move(tensor));
  }
  BOOST_CHECK_EQUAL(*encodedBytes, encodeTensorBundle(tensors).size());
  BOOST_CHECK_EQUAL(*encodedBytes, 15638U);
  BOOST_CHECK(!estimateNativeRequestTransferEncodedBytesV1(
    profile, nativePlanningDigest("foreign-source"), input, dependency, {producer, consumer}));
  auto warmProfile = profile;
  warmProfile["initial_context"] = false;
  BOOST_CHECK(!estimateNativeRequestTransferEncodedBytesV1(
    warmProfile, source, input, dependency, {producer, consumer}));
  BOOST_CHECK(!estimate(profile, nativePlanningDigest("other-source"), input));
  BOOST_CHECK(!estimate(profile, source, nativePlanningDigest("other-input")));
  auto changed = profile;
  changed["dependencies"][0]["tensors"].erase(0);
  BOOST_CHECK(!estimate(changed, source, input));
  changed = profile; changed["dependencies"][0]["tensors"][3]["shape"][2] = 512;
  BOOST_CHECK(!estimate(changed, source, input));
  changed = profile; changed["dependencies"][0]["tensors"][0]["shape"][0] =
    std::numeric_limits<std::int64_t>::max();
  BOOST_CHECK(!estimate(changed, source, input));
  changed = profile; changed["initial_context"] = false;
  BOOST_CHECK(!estimate(changed, source, input));
  changed = profile; changed["dependencies"][0]["tensors"][0] =
    changed["dependencies"][0]["tensors"][1];
  BOOST_CHECK(!estimate(changed, source, input));
}

BOOST_AUTO_TEST_CASE(WholeRosterV4ProfileSelectionIsAtomicInProductionHelper)
{
  Fixture f;
  const std::vector<NativeJson> artifacts{f.candidateArtifact()};
  const auto payload = f.requestPayload(artifacts);
  const auto binding = f.binding(artifacts);
  const auto cache = f.preparedCache(artifacts);
  auto configA = f.offerConfig("/provider/a");
  auto configB = f.offerConfig("/provider/b");
  configA.offerTtlMs = 5000;
  configB.offerTtlMs = 5000;
  const auto issuedA = issueNativeProviderOfferV4(
    payload, configA,
    f.measuredTelemetry("/provider/a", "boot-provider-a-1",
                        nativePlanningDigest("atomic-profile-topology-a")),
    3000);
  const auto issuedB = issueNativeProviderOfferV4(
    payload, configB,
    f.measuredTelemetry("/provider/b", "boot-provider-b-1",
                        nativePlanningDigest("atomic-profile-topology-b")),
    3000);
  BOOST_REQUIRE(issuedA);
  BOOST_REQUIRE(issuedB);
  const auto ackA = f.ackFor(issuedA->capabilityFieldValue, "/provider/a");
  const auto ackB = f.ackFor(issuedB->capabilityFieldValue, "/provider/b");
  const auto admission = f.makeAdmission();
  ndn_service_framework::CollaborationAckClosure closure;
  closure.requestId = ndn::Name(f.requestId);
  closure.digest = nativePlanningDigest("production-closed-roster");
  closure.candidates = {ackB, ackA};

  const auto selected = trySelectNativePlacementObservationSnapshotV4(
    binding, closure, admission, cache, 1, 3100);
  BOOST_REQUIRE(selected);
  BOOST_CHECK_EQUAL(selected->value.at("ack_closed_digest").get<std::string>(),
                    closure.digest);
  BOOST_CHECK_EQUAL(selected->value.at("provider_roster"),
                    NativeJson(f.roster));
  BOOST_CHECK_EQUAL(selected->value.at("provider_offers").size(), 2U);

  auto incomplete = closure;
  incomplete.candidates = {ackA};
  std::string unavailableReason;
  BOOST_CHECK(!trySelectNativePlacementObservationSnapshotV4(
    binding, incomplete, admission, cache, 1, 3100, nullptr,
    &unavailableReason));
  BOOST_CHECK_EQUAL(unavailableReason, "ACK_CLOSURE_INVALID");

  auto duplicate = closure;
  duplicate.candidates = {ackA, ackA};
  BOOST_CHECK(!trySelectNativePlacementObservationSnapshotV4(
    binding, duplicate, admission, cache, 1, 3100));

  auto mixedProfile = closure;
  mixedProfile.candidates[0] = f.ackFor("", "/provider/b");
  BOOST_CHECK(!trySelectNativePlacementObservationSnapshotV4(
    binding, mixedProfile, admission, cache, 1, 3100));

  auto replayed = closure;
  replayed.candidates[0].requestId = ndn::Name("/request/spec194/replayed");
  BOOST_CHECK(!trySelectNativePlacementObservationSnapshotV4(
    binding, replayed, admission, cache, 1, 3100));

  BOOST_CHECK(!trySelectNativePlacementObservationSnapshotV4(
    binding, closure, admission, cache, 1, 8500));
}

BOOST_AUTO_TEST_CASE(Spec196MergesOnlyFreshTargetedGoodputBoundToIntentAndProviderBoots)
{
  Fixture f;
  const std::vector<NativeJson> artifacts{f.candidateArtifact()};
  const auto payload = f.requestPayload(artifacts);
  const auto binding = f.binding(artifacts);
  const auto cache = f.preparedCache(artifacts);
  const auto topologyA = nativePlanningDigest("spec196-topology-a");
  const auto topologyB = nativePlanningDigest("spec196-topology-b");
  const auto issuedA = issueNativeProviderOfferV4(
    payload, f.offerConfig("/provider/a"),
    f.measuredTelemetry("/provider/a", "boot-provider-a-1", topologyA), 3000);
  const auto issuedB = issueNativeProviderOfferV4(
    payload, f.offerConfig("/provider/b"),
    f.measuredTelemetry("/provider/b", "boot-provider-b-1", topologyB), 3000);
  BOOST_REQUIRE(issuedA);
  BOOST_REQUIRE(issuedB);
  const auto admission = f.makeAdmission();
  std::vector<NativeAdmittedOfferV4> offers{
    admission.verifyV4(f.ackFor(issuedA->capabilityFieldValue, "/provider/a"), binding, 3100),
    admission.verifyV4(f.ackFor(issuedB->capabilityFieldValue, "/provider/b"), binding, 3100)};
  const std::vector<NativeJson> samples{
    NativeJson{{"provider", "/provider/a"}, {"sample", nullptr}},
    NativeJson{{"provider", "/provider/b"}, {"sample", nullptr}}};
  const auto original = buildNativePlacementObservationSnapshotV4(
    binding, nativePlanningDigest("spec196-ack-closure"), offers,
    samples, cache, 1, 3200);

  const std::string nonce = "spec196-fresh-probe-nonce-0123456789";
  const auto rttPairs = NativeJson::array();
  const auto goodputEdges = NativeJson::array({NativeJson{
    {"src_provider", "/provider/a"}, {"dst_provider", "/provider/b"}}});
  const auto rosterDigest = nativePlanningDigest(nativeCanonicalJson(NativeJson(f.roster)));
  const auto scope = NativeJson{
    {"schema", "ndnsf-di-placement-probe-scope-v1"},
    {"request_id", f.requestId}, {"attempt", 1},
    {"requester_identity", f.requesterIdentity},
    {"observer_provider", "/provider/b"}, {"service", f.service},
    {"provider_roster", f.roster}, {"roster_digest", rosterDigest},
    {"rtt_pairs", rttPairs}, {"goodput_edges", goodputEdges},
    {"deadline_ms", binding.deadlineMs}, {"intent_nonce", nonce}};
  const auto scopeDigest = nativePlanningDigest(nativeCanonicalJson(scope));
  NativePlacementProbeRoundV1 round;
  round.intent.observers.push_back({"/provider/b", {}, {{"/provider/a", "/provider/b"}}});
  round.intentNonceByObserver.emplace("/provider/b", nonce);
  round.responseByObserver.emplace("/provider/b", NativeJson{
    {"schema", "ndnsf-di-placement-probe-response-v1"},
    {"request_id", f.requestId}, {"attempt", 1},
    {"requester_identity", f.requesterIdentity},
    {"observer_provider", "/provider/b"}, {"service", f.service},
    {"observer_boot_epoch", "boot-provider-b-1"},
    {"scope_digest", scopeDigest}, {"intent_nonce", nonce},
    {"deadline_ms", binding.deadlineMs}, {"completed_at_ms", std::uint64_t{3300}},
    {"requested_rtt_count", std::uint64_t{0}}, {"omitted_rtt_count", std::uint64_t{0}},
    {"requested_goodput_count", std::uint64_t{1}},
    {"omitted_goodput_count", std::uint64_t{0}},
    {"observations", NativeJson::array({NativeJson{
      {"observer", "/provider/b"}, {"target", "/provider/a"},
      {"kind", "active_probe_goodput"}, {"value", 8.0},
      {"observed_at_ms", std::uint64_t{3300}}, {"expires_at_ms", std::uint64_t{8300}},
      {"peer_boot_epoch", "boot-provider-a-1"},
      {"data_name", "/provider/a/NDNSF/DI/DATA/probe/ts=3300"},
      {"train_complete", true}, {"probe_profile_validated", true},
      {"target_signature_validated", true},
      {"received_segments", std::uint64_t{16}}, {"sent_segments", std::uint64_t{16}},
      {"profile_digest", nativePlanningDigest("spec196-probe-profile")},
      {"probe_rtt_ms", NativeJson(std::vector<double>(16, 1.3))},
      {"probe_interest_gaps_ms", NativeJson(std::vector<double>(15, 4.0))},
      {"probe_data_arrival_gaps_ms", NativeJson(std::vector<double>(15, 5.0))},
      {"probe_data_wire_bytes", NativeJson(std::vector<std::uint64_t>(16, 2203))},
      {"payload_bytes", std::uint64_t{8192}}, {"wire_bytes", std::uint64_t{8448}},
      {"elapsed_ms", 8.192}}})}});

  const auto merged = mergeNativePlacementProbeRoundV1(original, round, binding, 3300);
  BOOST_REQUIRE_EQUAL(merged.value.at("eligible_link_observations").size(), 1U);
  const auto& entry = merged.value.at("eligible_link_observations").front();
  BOOST_CHECK_EQUAL(entry.at("source_kind").get<std::string>(), "TARGETED_RESPONSE");
  // Targeted goodput rows intentionally omit optional ACK-only evidence.
  BOOST_CHECK(!entry.at("row").contains("observer_peer"));
  BOOST_CHECK(!entry.at("row").contains("topology_digest"));
  BOOST_CHECK_EQUAL(entry.at("reported_by").get<std::string>(), "/provider/b");
  BOOST_CHECK_EQUAL(entry.at("row").at("src_peer").get<std::string>(), "/provider/a");
  BOOST_CHECK_EQUAL(entry.at("row").at("dst_peer").get<std::string>(), "/provider/b");
  BOOST_CHECK_EQUAL(entry.at("row").at("kind").get<std::string>(), "ACTIVE_PROBE_GOODPUT");
  BOOST_CHECK_EQUAL(entry.at("row").at("probe_segments_sent").get<std::uint64_t>(), 16U);
  BOOST_REQUIRE(entry.at("row").contains("probe_rtt_ms"));
  BOOST_REQUIRE(entry.at("row").contains("probe_interest_gaps_ms"));
  BOOST_REQUIRE(entry.at("row").contains("probe_data_arrival_gaps_ms"));
  BOOST_REQUIRE(entry.at("row").contains("probe_data_wire_bytes"));
  BOOST_CHECK_EQUAL(entry.at("row").at("probe_rtt_ms").size(), 16U);
  BOOST_CHECK_EQUAL(entry.at("row").at("probe_interest_gaps_ms").size(), 15U);
  BOOST_CHECK_EQUAL(entry.at("row").at("probe_data_arrival_gaps_ms").size(), 15U);
  BOOST_CHECK_EQUAL(entry.at("row").at("probe_data_wire_bytes").size(), 16U);
  BOOST_CHECK_EQUAL(entry.at("row").at("probe_interest_gaps_ms")[1].get<double>(), 4.0);
  BOOST_CHECK_EQUAL(entry.at("row").at("probe_data_wire_bytes")[3].get<std::uint64_t>(), 2203U);
  BOOST_CHECK_EQUAL(merged.digest, nativePlanningDigest(nativeCanonicalJson(merged.value)));
  BOOST_CHECK(original.value.at("eligible_link_observations").empty());

  // Exercise the exact independent response limit. This legacy-profile row
  // remains non-scoring; the response envelope's existing extension field
  // provides deterministic size without changing the observation identity.
  auto boundary = round;
  auto& boundaryResponse = boundary.responseByObserver.at("/provider/b");
  boundaryResponse["padding"] = "";
  const auto fixedBytes = nativeCanonicalJson(boundaryResponse).size();
  BOOST_REQUIRE_LT(fixedBytes, NATIVE_PLACEMENT_PROBE_RESPONSE_MAX_BYTES);
  boundaryResponse["padding"] = std::string(
    NATIVE_PLACEMENT_PROBE_RESPONSE_MAX_BYTES - fixedBytes, 'x');
  BOOST_REQUIRE_EQUAL(nativeCanonicalJson(boundaryResponse).size(),
                      NATIVE_PLACEMENT_PROBE_RESPONSE_MAX_BYTES);
  const auto atLimit = mergeNativePlacementProbeRoundV1(original, boundary, binding, 3300);
  BOOST_CHECK_EQUAL(atLimit.value.at("eligible_link_observations").size(), 1U);
  boundaryResponse["padding"].get_ref<std::string&>().push_back('x');
  const auto aboveLimit = mergeNativePlacementProbeRoundV1(original, boundary, binding, 3300);
  BOOST_CHECK(aboveLimit.value.at("eligible_link_observations").empty());

  auto wrongBoot = round;
  wrongBoot.responseByObserver["/provider/b"]["observations"][0]["peer_boot_epoch"] =
    "old-provider-a-boot";
  const auto rejected = mergeNativePlacementProbeRoundV1(original, wrongBoot, binding, 3300);
  BOOST_CHECK(rejected.value.at("eligible_link_observations").empty());

  const auto expectUnavailable = [&] (const char* name, const auto& mutate,
                                      std::uint64_t mergeAtMs) {
    BOOST_TEST_CONTEXT(name) {
      auto invalid = round;
      mutate(invalid.responseByObserver.at("/provider/b"));
      const auto unavailable = mergeNativePlacementProbeRoundV1(
        original, invalid, binding, mergeAtMs);
      BOOST_CHECK(unavailable.value.at("eligible_link_observations").empty());
      BOOST_CHECK_EQUAL(unavailable.expiresAtMs, original.expiresAtMs);
    }
  };
  expectUnavailable("replayed nonce", [] (NativeJson& response) {
    response["intent_nonce"] = "replayed-probe-nonce-0123456789";
  }, 3300);
  expectUnavailable("missing chirp gap data", [] (NativeJson& response) {
    response["observations"][0].erase("probe_interest_gaps_ms");
  }, 3300);
  expectUnavailable("stale observation", [] (NativeJson& response) {
    response["observations"][0]["observed_at_ms"] = std::uint64_t{3000};
    response["observations"][0]["expires_at_ms"] = std::uint64_t{9000};
  }, 8301);
  expectUnavailable("wrong request identity", [] (NativeJson& response) {
    response["request_id"] = "/request/foreign";
  }, 3300);
  expectUnavailable("wrong attempt", [] (NativeJson& response) {
    response["attempt"] = std::uint64_t{2};
  }, 3300);
  expectUnavailable("wrong requester identity", [] (NativeJson& response) {
    response["requester_identity"] = "/requester/foreign";
  }, 3300);
  expectUnavailable("wrong observer", [] (NativeJson& response) {
    response["observer_provider"] = "/provider/a";
  }, 3300);
  expectUnavailable("wrong service", [] (NativeJson& response) {
    response["service"] = "/service/foreign";
  }, 3300);
  expectUnavailable("wrong observer boot", [] (NativeJson& response) {
    response["observer_boot_epoch"] = "old-provider-b-boot";
  }, 3300);
  expectUnavailable("wrong scope", [] (NativeJson& response) {
    response["scope_digest"] = nativePlanningDigest("foreign-probe-scope");
  }, 3300);
  expectUnavailable("wrong directed edge", [] (NativeJson& response) {
    auto& observation = response["observations"][0];
    observation["target"] = "/provider/b";
    observation["peer_boot_epoch"] = "boot-provider-b-1";
    observation["data_name"] = "/provider/b/NDNSF/DI/DATA/probe/ts=3301";
  }, 3300);
  expectUnavailable("duplicate observation", [] (NativeJson& response) {
    response["observations"].push_back(response["observations"][0]);
  }, 3300);
  expectUnavailable("partial goodput train", [] (NativeJson& response) {
    response["observations"][0]["train_complete"] = false;
  }, 3300);
  expectUnavailable("oversized response", [] (NativeJson& response) {
    response["padding"] = std::string(NATIVE_PLACEMENT_PROBE_RESPONSE_MAX_BYTES, 'x');
  }, 3300);
  expectUnavailable("omission count contradicts observation", [] (NativeJson& response) {
    response["omitted_goodput_count"] = std::uint64_t{1};
  }, 3300);
  {
    auto timedOut = round;
    timedOut.responseByObserver.erase("/provider/b");
    const auto unavailable = mergeNativePlacementProbeRoundV1(
      original, timedOut, binding, 3300);
    BOOST_CHECK(unavailable.value.at("eligible_link_observations").empty());
    BOOST_CHECK_EQUAL(unavailable.expiresAtMs, original.expiresAtMs);
  }
}

BOOST_AUTO_TEST_CASE(RejectsDuplicateStaleOrMisboundSamplesAndForgedOffers)
{
  Fixture f;
  const auto valid = f.dataFetchRow();
  BOOST_CHECK_THROW(issueNativeProviderOfferV4(
    f.requestPayload(), f.offerConfig(), f.telemetry(), 1000, {valid, valid}),
    std::invalid_argument);
  auto stale = valid;
  stale["captured_at_ms"] = 1;
  stale.erase("source_digest");
  stale["source_digest"] = nativePlanningDigest(nativeCanonicalJson(stale));
  BOOST_CHECK_THROW(issueNativeProviderOfferV4(
    f.requestPayload(), f.offerConfig(), f.telemetry(), 6000, {stale}),
    std::invalid_argument);

  auto reversed = valid;
  reversed["observer_peer"] = "/provider/b";
  reversed.erase("source_digest");
  reversed["source_digest"] = nativePlanningDigest(nativeCanonicalJson(reversed));
  BOOST_CHECK_THROW(issueNativeProviderOfferV4(
    f.requestPayload(), f.offerConfig(), f.telemetry(), 1000, {reversed}),
    std::invalid_argument);
  auto wrongObserverEpoch = valid;
  wrongObserverEpoch["dst_boot_epoch"] = "old-provider-boot";
  wrongObserverEpoch.erase("source_digest");
  wrongObserverEpoch["source_digest"] =
    nativePlanningDigest(nativeCanonicalJson(wrongObserverEpoch));
  BOOST_CHECK_THROW(issueNativeProviderOfferV4(
    f.requestPayload(), f.offerConfig(), f.telemetry(), 1000, {wrongObserverEpoch}),
    std::invalid_argument);

  const auto issued = issueNativeProviderOfferV4(
    f.requestPayload(), f.offerConfig(), f.telemetry(), 1000);
  BOOST_REQUIRE(issued);
  auto ack = f.ackFor(issued->capabilityFieldValue);
  const auto verifier = f.makeAdmission();
  auto wrongRoster = f.binding();
  wrongRoster.candidateProviders = {"/provider/a", "/provider/c"};
  BOOST_CHECK_THROW(verifier.verifyV4(ack, wrongRoster, 1200), std::runtime_error);
  ack.authenticationEvidence.trustSchemaValidated = false;
  BOOST_CHECK_THROW(verifier.verifyV4(ack, f.binding(), 1200), std::runtime_error);
  ack = f.ackFor(issued->capabilityFieldValue);
  auto tampered = nativeParseJson(issued->capabilityFieldValue);
  tampered["observations_truncated"] = true;
  ndn_service_framework::ProviderCapabilityOffer changed;
  changed.setField(NATIVE_PROVIDER_OFFER_V4_FIELD, nativeCanonicalJson(tampered));
  ack.ack.setProviderCapabilityOffer(changed);
  BOOST_CHECK_THROW(verifier.verifyV4(ack, f.binding(), 1200), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()
