#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV4.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRunnerReuseCache.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanSealer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderArtifactCache.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeSessionCache.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"
#include <limits>

#include <boost/test/unit_test.hpp>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <boost/asio/post.hpp>

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

  NativeOfferAdmission makeAdmission() const
  {
    const auto candidateDigest = nativePlanningDigest("spec194-v4-candidate");
    const NativeJson policy = {
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
    });

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
  BOOST_CHECK(observedAck->getProviderCapabilityOffer().hasField(
    NATIVE_PROVIDER_OFFER_V4_FIELD));
  const auto issued = decodeNativeProviderOfferV4(
    observedAck->getProviderCapabilityOffer().getField(
      NATIVE_PROVIDER_OFFER_V4_FIELD));
  BOOST_CHECK_EQUAL(issued.values.at("residency_state").get<std::string>(), "KNOWN_EMPTY");
  BOOST_CHECK(issued.values.at("residency").is_array());
  BOOST_CHECK(issued.values.at("residency").empty());
  BOOST_CHECK_EQUAL(telemetrySnapshotCalls, 1U);
  BOOST_CHECK_EQUAL(probeInterestsSent, 0U);
  BOOST_CHECK_EQUAL(readinessInterestsSent, 0U);
  BOOST_CHECK_EQUAL(
    issued.values.at("resources").at("topology_digest").get<std::string>(),
    expectedTopologyDigest);
  const auto& issuedLinks = issued.values.at("link_observations");
  BOOST_CHECK(issuedLinks.empty());
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

  const auto makeProjection = [] (const std::string& requestId,
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
    output.tensorId = "activation";
    output.bundleTensorNames = {"hidden"};
    output.tensorDigest = "sha256:" + std::string(64, '4');
    output.segmentCount = 1;
    output.securityProfile = "NDNSF_DATA_V1";
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
  admitted.push_back(admission.verifyV4(
    f.ackFor(issuedA->capabilityFieldValue, "/provider/a"), binding, 3100));
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
  BOOST_CHECK_EQUAL(snapshot.value.at("plan_deadline_ms").get<std::uint64_t>(), 10000U);
  BOOST_CHECK_EQUAL(snapshot.capturedAtMs, 3200U);
  BOOST_CHECK_EQUAL(snapshot.expiresAtMs, 7990U);
  BOOST_CHECK_EQUAL(snapshot.value.at("expires_at_ms").get<std::uint64_t>(), 7990U);
  BOOST_REQUIRE_EQUAL(snapshot.value.at("eligible_link_observations").size(), 1U);
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
  BOOST_CHECK_EQUAL(requestEdge.at("estimated_bytes").get<std::uint64_t>(), 192U);
  BOOST_CHECK_EQUAL(requestEdge.at("estimated_bytes_source").get<std::string>(),
    "request-bound-bundle-payload");
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
