#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeV3Placement.hpp"
#include "ndn-service-framework/NetworkProbeRate.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV4.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanProjectionBuilder.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativePlacementObservationExpired.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeTensorTransportDeclaration.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "tests/fixtures/spec182/native-sealing-fixture.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include <boost/test/unit_test.hpp>
#include <boost/property_tree/exceptions.hpp>
#include <algorithm>
#include <array>
#include <fstream>
#include <cstdlib>
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeAuthenticatedGrantClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantVerifier.hpp"
#include <openssl/evp.h>
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGroupKeyAdmission.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGroupProjectionBuilder.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-svs/security-options.hpp>
#include <ndn-svs/svspubsub.hpp>
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCatalogModelAdapter.hpp"
#include <thread>
#include <atomic>
#include <future>
#include <sstream>
#include <set>
#include <limits>
#include <locale>
#include <cfenv>

namespace {
using namespace ndnsf::di;
NativeJson oracle()
{
  std::ifstream file("tests/fixtures/spec182/placement-v3-oracle.json");
  if (!file) throw std::runtime_error("missing V3 placement oracle");
  return NativeJson::parse(file);
}
struct Input
{
  NativeOfferBindingContext context;
  std::vector<NativeSelectionRoleV3> roles;
  std::vector<NativeAdmittedOfferV3> offers;
  std::vector<ndn_service_framework::AckSelectionCandidate> acks;
  std::string ackDigest;
  NativeInspectedModel inspected;
  NativeSplitCandidate split;
  Input(const NativeJson& f, const NativeJson& sample)
  {
    context = {"request", 1, "/service", f.at("model_digest"), f.at("graph_digest"), 900};
    context.deadlineMs = sample.value<std::uint64_t>("deadline_ms", 900);
    ackDigest = f.at("ack_digest");
    const auto& r = f.at("role");
    NativePlanSealingInputs inputs;
    inputs.artifacts.artifactDigestByRole = {{r.at("role"), r.at("artifact_digest")}};
    inputs.artifacts.manifestDigest = r.at("model_manifest_digest");
    inputs.artifacts.recipeDigest = r.at("recipe_digest");
    inputs.artifacts.graphDigest = r.at("graph_digest");
    inputs.artifacts.canonicalGraphDigest = sample.value<std::string>(
      "canonical_graph_digest", r.at("graph_digest").get<std::string>());
    inputs.protectionEpoch = r.at("protection_epoch");
    fixture::assemblies(inputs);
    for (const auto& rank : sample.at("ranks")) {
      auto role = inputs.assemblyByRole.begin()->second;
      role.rank = rank.get<std::uint64_t>();
      if (role.rank != 0) role.artifactDigest = nativePlanningDigest("rank-artifact-" + std::to_string(role.rank));
      role.backend = "onnxruntime";
      role.requiredDeviceMemoryMb = 1024;
      roles.push_back(role);
    }
    NativeModelDescriptor descriptor{"QwenFixture", f.at("source_content_digest"), nativePlanningDigest("semantics"),
      context.graphDigest, "onnx", "fp32", roles.front().adapterId, roles.front().adapterVersion};
    descriptor = fixture::completeModel(descriptor);
    NativeGraphSnapshot graph;
    graph.graphDigest = context.graphDigest; graph.nodes = {{"node", "Identity", 0}};
    graph.topologicalOrder = {"node"};
    inspected = {descriptor, graph, "/catalog/model", nativePlanningDigest("source"),
      r.at("model_manifest_digest"), inputs.artifacts.canonicalGraphDigest};
    split.model = descriptor; split.graphDigest = context.graphDigest;
    split.splitter = {"fixture", "1", nativePlanningDigest("split")};
    split.source = "PRE_SPLIT";
    split.nodeRoles = {{"node", roles.front().role}};
    for (const auto& role : roles) {
      if (!split.tensorDegreesByRole.count(role.role)) split.executionPlan.roles.push_back(role.role);
      ++split.tensorDegreesByRole[role.role];
      split.fragmentsByRole[role.role] = nativePlanningDigest("fragment");
      split.artifactsByRole[role.role].push_back(role.artifactDigest);
      split.rankArtifactDigestsByRole[role.role].push_back(role.artifactDigest);
      split.requirementsByRole[role.role] = {{"onnxruntime"}, 1, 0, 0, 0, 0, 1.0};
    }
    if (roles.size() > 1) {
      NativeHybridPlan hybrid;
      hybrid.stages = 1; hybrid.tensorDegrees = {std::uint64_t(roles.size())};
      for (std::size_t rank = 0; rank < roles.size(); ++rank) hybrid.rankLabels.push_back("S0R" + std::to_string(rank));
      split.hybridPlan = std::move(hybrid);
    }
    split.candidateDigest = split.computedDigest();
    NativeRequestPreparation preparation(std::make_shared<NativeAdapterRegistry>(), {}, {},
      [&](const NativeInspectedModel&, const NativeSplitCandidate&, const NativeRequestControl&) { return roles; });
    NativeRequestControl control{"request", 1, std::chrono::steady_clock::now() + std::chrono::seconds(10), {}};
    roles = preparation.prepareRoles(inspected, split, control);
    NativeOfferAdmission admission(f.at("policy").dump(),
      {{f.at("key_id").get<std::string>(), f.at("public_pem").get<std::string>()}}, f.at("candidate"));
    for (const auto& item : sample.at("offers")) {
      const auto wire = item.get<std::string>();
      const auto offer = decodeNativeProviderOfferV3(wire);
      ndn_service_framework::AckSelectionCandidate ack;
      ack.providerName = ndn::Name(offer.provider);
      ack.serviceName = ndn::Name(offer.service);
      ack.requestId = ndn::Name(offer.requestId);
      ack.ack.setStatus(offer.status);
      ndn::Buffer payload(wire.begin(), wire.end());
      ack.ack.setPayload(payload, payload.size());
      // Fixture of Core evidence only; no claim of real NFD authentication.
      ack.authenticationEvidence = {offer.provider, offer.provider + "/KEY/fixture/issuer/v=1",
                                    "sha256:" + std::string(64, '1'), true};
      offers.push_back(admission.verify(ack, context, 200));
      acks.push_back(ack);
    }
  }
};

void makeInterRoleProbeInput(Input& input)
{
  auto roles = std::vector<NativeSelectionRoleV3>{input.roles[0], input.roles[1]};
  const auto roleA = roles[0].selectedRole;
  const auto roleB = roleA + "/successor";
  roles[0].role = roles[0].selectedRole = roleA;
  roles[1].role = roles[1].selectedRole = roleB;
  roles[0].rank = 0;
  roles[1].rank = 0;
  roles[0].nodeIndices = {0};
  roles[1].nodeIndices = {1};

  NativeGraphSnapshot graph;
  graph.graphDigest = input.inspected.graph.graphDigest;
  graph.nodes = {{"node-0", "Identity", 0}, {"node-1", "Identity", 1}};
  graph.topologicalOrder = {"node-0", "node-1"};
  graph.edges = {{"activation-tensor", "node-0", {"node-1"},
    NativeTensorContract{"activation-tensor", "float32", {std::int64_t{1}},
                         std::uint64_t{1024}}}};
  graph.legalCutEdges = {"activation-tensor"};
  input.inspected.graph = graph;
  input.roles = roles;

  auto& candidate = input.split;
  candidate.model = input.inspected.descriptor;
  candidate.graphDigest = graph.graphDigest;
  candidate.executionPlan.roles = {roleA, roleB};
  candidate.executionPlan.dependencies.clear();
  candidate.executionPlan.dependencies.emplace_back(
    std::vector<std::string>{roleA}, std::vector<std::string>{roleB},
    "spec196-test", "/spec196/test", "activation", 1, 1024,
    std::vector<std::string>{"activation-tensor"});
  candidate.nodeRoles = {{"node-0", roleA}, {"node-1", roleB}};
  candidate.fragmentsByRole.clear();
  candidate.artifactsByRole.clear();
  candidate.requirementsByRole.clear();
  candidate.crossPartitionTensors = {"activation-tensor"};
  candidate.tensorDegreesByRole.clear();
  candidate.rankArtifactDigestsByRole.clear();
  candidate.hybridPlan.reset();
  candidate.inputIngressRole = roleA;
  candidate.resultEgressRole = roleB;
  for (const auto& role : roles) {
    candidate.fragmentsByRole[role.selectedRole] =
      nativePlanningDigest("spec196-fragment-" + role.selectedRole);
    candidate.artifactsByRole[role.selectedRole] = {role.artifactDigest};
    candidate.requirementsByRole[role.selectedRole] =
      {{"onnxruntime"}, 1, 0, 0, 0, 0, 1.0};
    candidate.tensorDegreesByRole[role.selectedRole] = 1;
    candidate.rankArtifactDigestsByRole[role.selectedRole] = {role.artifactDigest};
  }
  candidate.candidateDigest = candidate.computedDigest();
}

std::string resignOfferAcceptingRole(
  const std::string& wire, const std::string& addedRole)
{
  auto offer = nativeParseJson(wire);
  auto acceptedRoles = offer.at("accepted_roles").get<std::vector<std::string>>();
  if (std::find(acceptedRoles.begin(), acceptedRoles.end(), addedRole) == acceptedRoles.end())
    acceptedRoles.push_back(addedRole);
  offer["accepted_roles"] = std::move(acceptedRoles);
  offer["signature"] = "pending";
  const auto digest = decodeNativeProviderOfferV3(nativeCanonicalJson(offer)).offerDigest;

  std::array<unsigned char, 32> seed{};
  for (std::size_t i = 0; i < seed.size(); ++i)
    seed[i] = static_cast<unsigned char>(i);
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> signingKey(
    EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()),
    EVP_PKEY_free);
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> signing(
    EVP_MD_CTX_new(), EVP_MD_CTX_free);
  std::array<unsigned char, 64> signature{};
  std::size_t signatureLength = signature.size();
  std::array<unsigned char, 89> encoded{};
  if (!signingKey || !signing ||
      EVP_DigestSignInit(signing.get(), nullptr, nullptr, nullptr, signingKey.get()) != 1 ||
      EVP_DigestSign(signing.get(), signature.data(), &signatureLength,
        reinterpret_cast<const unsigned char*>(digest.data()), digest.size()) != 1 ||
      signatureLength != signature.size() ||
      EVP_EncodeBlock(encoded.data(), signature.data(), static_cast<int>(signatureLength)) != 88)
    throw std::runtime_error("could not sign modified V3 placement fixture");
  offer["signature"] = std::string(reinterpret_cast<const char*>(encoded.data()), 88);
  return nativeCanonicalJson(offer);
}

NativeJson makeV4PlacementSnapshot(
  Input& input, const std::vector<NativeSelectionRoleV3>& roles,
  NativeJson estimatedBytes, const bool includeDataFetch)
{
  constexpr std::uint64_t capturedAtMs = 190;
  constexpr std::uint64_t payloadBytes = 1'000'000;
  const auto expiresAtMs = input.context.deadlineMs;
  input.context.requesterIdentity = "/requester";

  NativeJson providerOffers = NativeJson::array();
  NativeJson requesterSamples = NativeJson::array();
  NativeJson providerRoster = NativeJson::array();
  for (const auto& admitted : input.offers) {
    const auto& offer = admitted.observation();
    NativeJson residency = NativeJson::array();
    auto devices = offer.devices;
    if (devices.empty() && std::any_of(offer.backends.begin(), offer.backends.end(),
        [] (const auto& backend) {
          return backend.size() >= 4 &&
            backend.compare(backend.size() - 4, 4, "-cpu") == 0;
        }))
      devices.push_back("cpu");
    for (const auto& role : roles) {
      for (const auto& device : devices) {
        if (!role.deviceSet.empty() &&
            std::find(role.deviceSet.begin(), role.deviceSet.end(), device) == role.deviceSet.end())
          continue;
        auto backend = role.backend;
        const auto suffix = device == "cpu" ? "-cpu" : "-cuda";
        if (std::find(offer.backends.begin(), offer.backends.end(), backend + suffix) !=
            offer.backends.end())
          backend += suffix;
        NativeJson deviceSet = device == "cpu" ? NativeJson::array() :
          NativeJson::array({device});
        residency.push_back(NativeJson{
          {"artifact_digest", role.artifactDigest}, {"role", role.role},
          {"rank", role.rank}, {"model_manifest_digest", role.modelManifestDigest},
          {"graph_digest", role.graphDigest}, {"backend", backend},
          {"device_set", std::move(deviceSet)}, {"protection_epoch", role.protectionEpoch},
          {"state", "EXACT_LOADED"}, {"process_epoch", offer.bootEpoch},
          {"captured_at_ms", capturedAtMs}, {"expires_at_ms", expiresAtMs},
          {"runtime_generation", "fixture-runtime"},
          {"missing_verified_bytes", std::uint64_t{0}},
          {"estimated_assembly_ms", nullptr}, {"estimated_load_ms", nullptr}});
      }
    }
    providerOffers.push_back(NativeJson{
      {"provider", offer.provider}, {"boot_epoch", offer.bootEpoch},
      {"captured_at_ms", capturedAtMs}, {"expires_at_ms", expiresAtMs},
      {"offer_digest", offer.offerDigest}, {"signer_identity", offer.provider},
      {"signer_key_locator", offer.provider + "/KEY/fixture/issuer/v=1"},
      {"ack_wire_digest", "sha256:" + std::string(64, '1')},
      {"values", NativeJson{{"capacity_state", "OBSERVED"},
        {"capacity", NativeJson{{"worker_count", std::uint64_t{1}},
          {"active_worker_count", std::uint64_t{0}},
          {"ready_queue_depth", std::uint64_t{0}},
          {"ready_queue_capacity", std::uint64_t{0}},
          {"waiting_for_input_count", std::uint64_t{0}},
          {"dependency_wait_worker_count", std::uint64_t{0}},
          {"dependency_wait_queued_count", std::uint64_t{0}},
          {"dependency_wait_active_count", std::uint64_t{0}}, {"stopping", false}}},
        {"queue_depth", std::uint64_t{0}}, {"estimated_wait_ms", 0.0},
        {"residency_state", "OBSERVED"},
        {"residency", std::move(residency)}}}});
    providerRoster.push_back(offer.provider);
    const auto rttMs = offer.provider == "/provider/a" ? 10.0 : 3.0;
    requesterSamples.push_back(NativeJson{
      {"provider", offer.provider},
      {"sample", NativeJson{{"provider", offer.provider}, {"service", offer.service},
        {"kind", "ACK_RTT"}, {"rtt_ms", rttMs},
        {"observed_at_ms", capturedAtMs}, {"peer_boot_epoch", offer.bootEpoch}}}});
  }

  NativeJson preparedRoles = NativeJson::array();
  for (const auto& role : roles)
    preparedRoles.push_back(nativeAssemblyJson(role));
  NativeJson dependencies = NativeJson::array();
  if (roles.size() > 1) {
    const bool bytesKnown = !estimatedBytes.is_null();
    const auto roleKey = [] (const NativeSelectionRoleV3& role) {
      return role.role + "#" + std::to_string(role.rank);
    };
    dependencies.push_back(NativeJson{
      {"producer_role", roleKey(roles.front())},
      {"consumer_role", roleKey(roles.back())}, {"tensors", {"hidden"}},
      {"estimated_bytes", estimatedBytes},
      {"estimated_bytes_source", bytesKnown
        ? NativeJson("graph-tensor-contract") : NativeJson(nullptr)},
      {"expected_segments", std::uint64_t{1}}, {"operation_kind", "ACTIVATION"}});
  }
  NativeJson links = NativeJson::array();
  if (includeDataFetch) {
    const auto sourceDigest = nativePlanningDigest("fixture-v4-data-fetch");
    NativeJson row{
      {"src_peer", "/provider/a"}, {"dst_peer", "/provider/b"},
      {"kind", "DATA_FETCH"}, {"direction_semantics", "PAYLOAD_SOURCE_TO_RECEIVER"},
      {"observer_peer", "/provider/b"}, {"captured_at_ms", capturedAtMs},
      {"expires_at_ms", expiresAtMs}, {"sample_count", std::uint64_t{1}},
      {"payload_bytes", payloadBytes}, {"elapsed_ms", 1000.0},
      {"goodput_mbps", 8.0}, {"source_digest", sourceDigest}};
    // Keep the business download diagnostic alongside a separate dedicated
    // sample. Scoring must select the latter even with DATA_FETCH present.
    auto probe = row;
    probe["kind"] = "ACTIVE_PROBE_GOODPUT";
    probe["probe_complete"] = true;
    probe["probe_segments_sent"] = std::uint64_t{16};
    probe["probe_signature_valid"] = true;
    probe["probe_profile_digest"] = ndn_service_framework::NETWORK_PROBE_TRAIN_PROFILE_DIGEST;
    probe["probe_source_timing_validated"] = true;
    probe["probe_source_send_gaps_ms"] = NativeJson(std::vector<double>(14, 0.01));
    probe["probe_data_arrival_gaps_ms"] = NativeJson(std::vector<double>(15, 4.096));
    probe["probe_rate_status"] = "MEASURED_GOODPUT";
    probe["probe_rate_reason"] = "MEASURED_WARMED_PACKET_TRAIN";
    probe["probe_train_goodput_mbps"] = 8.0;
    probe["source_digest"] = nativePlanningDigest("fixture-v4-dedicated-train");
    links.push_back(NativeJson{{"src_peer", "/provider/a"},
      {"dst_peer", "/provider/b"}, {"row", std::move(probe)}});
    links.push_back(NativeJson{{"src_peer", "/provider/a"},
      {"dst_peer", "/provider/b"}, {"row", std::move(row)}});
  }

  return NativeJson{
    {"schema", "ndnsf-placement-observation-snapshot-v4"},
    {"request_id", input.context.requestId}, {"attempt", input.context.attempt},
    {"requester_identity", input.context.requesterIdentity},
    {"service", input.context.serviceName}, {"model_digest", input.context.modelDigest},
    {"graph_digest", input.context.graphDigest}, {"ack_closed_digest", input.ackDigest},
    {"provider_roster", std::move(providerRoster)},
    {"prepared_role_graph", NativeJson::array({NativeJson{
      {"candidate_digest", input.split.candidateDigest},
      {"graph_digest", input.split.graphDigest}, {"node_roles", input.split.nodeRoles},
      {"stage_order", input.split.executionPlan.roles}, {"roles", std::move(preparedRoles)},
      {"dependencies", std::move(dependencies)}}})},
    {"provider_offers", std::move(providerOffers)},
    {"requester_provider_samples", std::move(requesterSamples)},
    {"eligible_link_observations", std::move(links)},
    {"captured_at_ms", capturedAtMs}, {"expires_at_ms", expiresAtMs}};
}

NativePlacementPlanCore sealV4Proposal(
  const Input& input, const NativeRolePlacementProposalV3& proposal)
{
  const auto& context = proposal.context;
  NativeExecutionPlan execution = input.split.executionPlan;
  execution.serviceName = context.serviceName;
  execution.modelName = input.inspected.descriptor.modelName;
  execution.roles.clear();
  NativePlanSealingInputs sealing;
  sealing.requesterIdentity = context.requesterIdentity;
  sealing.protectionEpoch = input.roles.front().protectionEpoch;
  sealing.expiresAtMs = context.deadlineMs;
  sealing.placementObservationSnapshotDigest =
    context.placementObservationSnapshotDigest;
  auto& artifacts = sealing.artifacts;
  artifacts.requestId = context.requestId;
  artifacts.attempt = context.attempt;
  artifacts.modelDigest = input.inspected.descriptor.contentDigest;
  artifacts.graphDigest = context.graphDigest;
  artifacts.canonicalGraphDigest = input.inspected.canonicalGraphDigest;
  artifacts.manifestDigest = input.inspected.modelManifestDigest;
  artifacts.recipeDigest = input.roles.front().recipeDigest;
  for (const auto& role : proposal.roles) {
    execution.roles.push_back(role.selectedRole);
    artifacts.sourceByRole[role.selectedRole] = "/catalog/source";
    artifacts.artifactDigestByRole[role.selectedRole] = role.artifactDigest;
  }
  for (std::size_t i = 0; i < input.roles.size(); ++i)
    sealing.assemblyByRole.emplace(std::to_string(i), input.roles[i]);
  return NativePlanSealer::sealCore(input.inspected, input.split, proposal,
    execution, input.offers, input.ackDigest, sealing);
}

std::map<std::string, ndn::security::Certificate> groupCertificates(Input& input, ndn::security::KeyChain& keyChain)
{
  std::map<std::string, ndn::security::Certificate> certificates;
  for (auto& ack : input.acks) {
    const auto payload = ack.ack.getPayload();
    const auto offer = decodeNativeProviderOfferV3(std::string(payload.begin(), payload.end()));
    const auto cert = keyChain.createIdentity(ndn::Name(offer.provider), ndn::RsaKeyParams(2048))
      .getDefaultKey().getDefaultCertificate();
    certificates.emplace(offer.provider, cert);
    const auto bytes = cert.getPublicKey();
    ndn_service_framework::SelectionInputKeyOffer value;
    value.setField("schemaVersion", "1"); value.setField("recipient", offer.provider);
    value.setField("recipientCertName", cert.getName().toUri());
    value.setField("recipientPublicKey", ndn_service_framework::selectionGatedHex(bytes));
    value.setField("recipientCertDigest", nativePlanningDigest(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())));
    value.setField("providerBootEpoch", offer.provider + ":" + offer.bootEpoch);
    value.setField("ndnsfDataV1EndpointPrefix", offer.provider);
    ack.ack.setSelectionInputKeyOffer(value);
  }
  return certificates;
}
NativeSealedPlan projectionPlan(const NativePlacementPlanCore& core)
{
  // Grant metadata fixture only; this test does not claim grant issuance or
  // Provider cryptographic acceptance. The builder owns no grant authority.
  std::vector<NativeGrantBinding> grants;
  for (const auto& item : core.assignment.providerByRole)
    grants.push_back({item.second, item.first, "/grant/" + item.first,
      nativePlanningDigest("grant-" + item.first), item.second, "{}", core.expiresAtMs});
  return NativePlanSealer::finalizeSecurity(core, grants, {nativePlanningDigest("projection-policy"), true});
}

void recordProjectionOracle(const std::map<std::string, NativeRoleProjectionInputs>& values)
{
  // Optional raw evidence consumed by the independent offline SDK decoder.
  const auto* path = std::getenv("NDNSF_PROJECTION_ORACLE_OUTPUT");
  if (!path) return;
  std::ofstream file(path, std::ios::app);
  if (!file) throw std::runtime_error("cannot write projection oracle evidence");
  for (const auto& item : values) file << nativeCanonicalJson(nativeDataflowJson(item.second.dataflow)) << '\n';
}

// A native policy implements only the authenticated V3 contract. It does not
// need a legacy planning view or a Python trampoline to be injected.
class PreferProvider final : public NativePlacementStrategy
{
public:
  explicit PreferProvider(std::string provider) : m_provider(std::move(provider)) {}
  NativeStrategyIdentity identity() const override
  { return {"prefer-provider-fixture", "1", nativePlanningDigest(m_provider)}; }
  NativeRolePlacementProposalV3 proposeRoles(
    const NativeOfferBindingContext& context, const std::string& ackClosedDigest,
    const std::vector<NativeSelectionRoleV3>& roles,
    const std::vector<NativeAdmittedOfferV3>& offers, std::uint64_t nowMs) const override
  {
    auto preferred = offers;
    preferred.erase(std::remove_if(preferred.begin(), preferred.end(), [&](const auto& offer) {
      return offer.observation().provider != m_provider;
    }), preferred.end());
    return NativePreSplitFirstPlacement(identity()).proposeRoles(
      context, ackClosedDigest, roles, preferred, nowMs);
  }
private:
  const std::string m_provider;
};
}
BOOST_AUTO_TEST_SUITE(Spec182V3Placement)

BOOST_AUTO_TEST_CASE(AssemblyValidationRetainsEncodingAndLocaleChecks)
{
  const auto f = oracle();
  const auto sample = std::find_if(f.at("seal_cases").begin(), f.at("seal_cases").end(),
    [](const auto& value) { return value.at("name") == "cpu"; });
  BOOST_REQUIRE(sample != f.at("seal_cases").end());
  Input input(f, *sample);
  const auto proposal = NativePreSplitFirstPlacement().proposeRoles(
    input.context, input.ackDigest, input.roles, input.offers, 200);
  BOOST_REQUIRE_EQUAL(proposal.roles.size(), 1U);
  const auto role = proposal.roles.front();
  BOOST_REQUIRE(std::locale() == std::locale::classic());
  BOOST_CHECK_NO_THROW(validateNativeAssembly(role));
  auto nodeOrder = role;
  nodeOrder.nodeIndices = {0, 1};
  BOOST_CHECK_NO_THROW(validateNativeAssembly(nodeOrder));
  nodeOrder.nodeIndices = {1, 0};
  BOOST_CHECK_THROW(validateNativeAssembly(nodeOrder), std::invalid_argument);
  nodeOrder.nodeIndices = {0, 0};
  BOOST_CHECK_THROW(validateNativeAssembly(nodeOrder), std::invalid_argument);
  // This list follows the unchanged serializer, including fields whose
  // semantic validation would otherwise mask a missed UTF-8 check.
  const std::vector<std::string NativeSelectionRoleV3::*> encodedStrings{
    &NativeSelectionRoleV3::role, &NativeSelectionRoleV3::backend,
    &NativeSelectionRoleV3::artifactDigest, &NativeSelectionRoleV3::recipeDigest,
    &NativeSelectionRoleV3::roleKind, &NativeSelectionRoleV3::adapterId,
    &NativeSelectionRoleV3::adapterVersion, &NativeSelectionRoleV3::modelManifestDigest,
    &NativeSelectionRoleV3::artifactProfileDigest, &NativeSelectionRoleV3::graphDigest,
    &NativeSelectionRoleV3::canonicalInitializerDigest, &NativeSelectionRoleV3::adapterDescriptorDigest,
    &NativeSelectionRoleV3::assemblerDescriptorDigest, &NativeSelectionRoleV3::backendAbi,
    &NativeSelectionRoleV3::precision, &NativeSelectionRoleV3::quantization,
    &NativeSelectionRoleV3::layout, &NativeSelectionRoleV3::padding,
    &NativeSelectionRoleV3::protectionEpoch, &NativeSelectionRoleV3::mergeKind,
    &NativeSelectionRoleV3::postprocessIdentity, &NativeSelectionRoleV3::postprocessOutputName,
    &NativeSelectionRoleV3::postprocessSort};
  const std::string invalidUtf8(1, static_cast<char>(0xff));
  for (const auto field : encodedStrings) {
    auto changed = role;
    changed.*field = invalidUtf8;
    BOOST_CHECK_THROW(nativeCanonicalJson(nativeAssemblyJson(changed)), NativeJson::type_error);
    BOOST_CHECK_THROW(validateNativeAssembly(changed), NativeJson::type_error);
  }
  for (unsigned field = 0; field < 7; ++field) {
    auto changed = role;
    if (field == 0) changed.deviceSet = {invalidUtf8};
    else {
      auto& tensor = field <= 3 ? changed.expectedInputs.front() : changed.expectedOutputs.front();
      if ((field - 1) % 3 == 0) tensor.dtype = invalidUtf8;
      else if ((field - 1) % 3 == 1) tensor.name = invalidUtf8;
      else tensor.shape = {invalidUtf8};
    }
    BOOST_CHECK_THROW(nativeCanonicalJson(nativeAssemblyJson(changed)), NativeJson::type_error);
    BOOST_CHECK_THROW(validateNativeAssembly(changed), NativeJson::type_error);
  }
  auto unicode = role;
  unicode.expectedInputs.front().name = u8"激活";
  unicode.expectedInputs.front().shape = {u8"批次", std::numeric_limits<std::int64_t>::min(),
    std::numeric_limits<std::int64_t>::max()};
  BOOST_CHECK_NO_THROW(nativeCanonicalJson(nativeAssemblyJson(unicode)));
  BOOST_CHECK_NO_THROW(validateNativeAssembly(unicode));
  auto internalOnly = role;
  internalOnly.selectedRole = invalidUtf8;
  internalOnly.materializedRole = !internalOnly.materializedRole;
  BOOST_CHECK_NO_THROW(validateNativeAssembly(internalOnly));
  for (const auto confidence : {0.0, -0.0}) {
    auto changed = role;
    changed.postprocessConfidenceThreshold = confidence;
    BOOST_CHECK_NO_THROW(validateNativeAssembly(changed));
  }
  for (const auto confidence : {std::numeric_limits<double>::infinity(),
       -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
    auto changed = role;
    changed.postprocessConfidenceThreshold = confidence;
    BOOST_CHECK_THROW(nativeCanonicalJson(nativeAssemblyJson(changed)), std::invalid_argument);
    BOOST_CHECK_THROW(validateNativeAssembly(changed), std::invalid_argument);
  }
  struct RestoreRounding {
    int previous = std::fegetround();
    ~RestoreRounding() { std::fesetround(previous); }
  };
  {
    RestoreRounding restoreRounding;
    auto merge = role;
    merge.roleKind = "COMPONENT_SET";
    merge.layerBegin = merge.layerEnd = 0;
    merge.mergeKind = "NATIVE_POSTPROCESS";
    merge.postprocessIdentity = "fixture-native-merge";
    merge.postprocessOutputName = merge.expectedOutputs.front().name;
    merge.postprocessConfidenceThreshold = 0.1;
    merge.postprocessSort = "confidence-desc,class-asc,xyxy-asc";
    for (const auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
      BOOST_REQUIRE_EQUAL(std::fesetround(mode), 0);
      std::string oldEncodingFailure;
      try { (void)nativeCanonicalJson(nativeAssemblyJson(merge)); }
      catch (const std::invalid_argument& error) { oldEncodingFailure = error.what(); }
      if (oldEncodingFailure.empty()) BOOST_CHECK_NO_THROW(validateNativeAssembly(merge));
      else BOOST_CHECK_EXCEPTION(validateNativeAssembly(merge), std::invalid_argument,
        [&] (const std::invalid_argument& error) { return oldEncodingFailure == error.what(); });
    }
  }
  struct RejectUnsigned : std::num_get<char> {
    iter_type do_get(iter_type first, iter_type, std::ios_base&, std::ios_base::iostate& state,
                     unsigned long& value) const override {
      state |= std::ios_base::failbit; value = 0; return first;
    }
    iter_type do_get(iter_type first, iter_type, std::ios_base&, std::ios_base::iostate& state,
                     unsigned long long& value) const override {
      state |= std::ios_base::failbit; value = 0; return first;
    }
  };
  struct RestoreLocale {
    std::locale previous = std::locale();
    ~RestoreLocale() { std::locale::global(previous); }
  } restoreLocale;
  std::locale::global(std::locale(std::locale::classic(), new RejectUnsigned));
  BOOST_CHECK_THROW(validateNativeAssembly(role), boost::property_tree::ptree_bad_data);
}

BOOST_AUTO_TEST_CASE(FixedProviderPlacementBindsEachPipelineRankToConfiguredProvider)
{
  const auto f = oracle();
  Input input(f, f.at("cases")[11]);
  NativeFixedProviderPlacement strategy(std::map<std::string, std::string>{
    {"/LLM/Pipeline/Stage/0#0", "/provider/a"},
    {"/LLM/Pipeline/Stage/0#1", "/provider/b"}});
  ExtensionControl control{std::chrono::steady_clock::now() + std::chrono::seconds(5),
                           [] { return false; }};

  const auto proposal = strategy.proposeRoles(
    input.context, input.ackDigest, input.roles, input.offers, 200, control);
  BOOST_REQUIRE_NO_THROW(validateNativeRolePlacement(
    proposal, input.roles, input.offers, 200));
  BOOST_CHECK_EQUAL(proposal.strategy.name, strategy.identity().name);
  BOOST_CHECK_EQUAL(proposal.providerByRole.at("/LLM/Pipeline/Stage/0#0"), "/provider/a");
  BOOST_CHECK_EQUAL(proposal.providerByRole.at("/LLM/Pipeline/Stage/0#1"), "/provider/b");
}

BOOST_AUTO_TEST_CASE(AdmittedGroupKeysReachCoreRsaCapabilityUnwrap)
{
  const auto f = oracle();
  Input input(f, f.at("cases")[0]);
  NativeOfferAdmission admission(f.at("policy").dump(),
    {{f.at("key_id").get<std::string>(), f.at("public_pem").get<std::string>()}}, f.at("candidate"));
  ndn::security::KeyChain keyChain("pib-memory:spec182-group-key", "tpm-memory:spec182-group-key");
  std::map<std::string, ndn::security::Certificate> certificates;
  for (auto& ack : input.acks) {
    const auto payload = ack.ack.getPayload();
    const auto offer = decodeNativeProviderOfferV3(std::string(payload.begin(), payload.end()));
    const auto certificate = keyChain.createIdentity(ndn::Name(offer.provider), ndn::RsaKeyParams(2048))
      .getDefaultKey().getDefaultCertificate();
    certificates.emplace(offer.provider, certificate);
    const auto bytes = certificate.getPublicKey();
    ndn_service_framework::SelectionInputKeyOffer keyOffer;
    keyOffer.setField("schemaVersion", "1"); keyOffer.setField("recipient", offer.provider);
    keyOffer.setField("recipientCertName", certificate.getName().toUri());
    keyOffer.setField("recipientPublicKey", ndn_service_framework::selectionGatedHex(bytes));
    keyOffer.setField("recipientCertDigest", nativePlanningDigest(
      std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())));
    keyOffer.setField("providerBootEpoch", offer.provider + ":" + offer.bootEpoch);
    keyOffer.setField("ndnsfDataV1EndpointPrefix", offer.provider);
    ack.ack.setSelectionInputKeyOffer(keyOffer);
  }
  NativeGroupKeyAdmission keys(admission, input.acks, input.context, 200);
  std::vector<GroupMemberV1> members;
  for (const auto& pair : certificates) {
    members.push_back({pair.first, members.size(), keys.offer(pair.first).observation().offerDigest, keys.endpoint(pair.first)});
    BOOST_CHECK_EQUAL(keys.endpoint(pair.first), pair.first);
  }
  BOOST_REQUIRE_GE(members.size(), 2);
  // The wrap closure outlives its admission object and owns the key snapshot.
  auto options = [&] {
    NativeGroupKeyAdmission temporary(admission, input.acks, input.context, 200);
    return temporary.options();
  }();
  BOOST_CHECK_THROW(options.wrapEpochKey("/foreign", ProviderGroupBytes(32, 42)), std::out_of_range);
  BOOST_CHECK_THROW(options.wrapEpochKey(members[0].provider, ProviderGroupBytes(1, 42)), std::invalid_argument);
  ProviderGroupCoordinator producer(options);
  const auto capability = producer.createCapability(input.context.requestId, "attempt-1",
    nativePlanningDigest("sealed-group-plan"), "group-1", 1, members,
    {{7, "PIPELINE", {"0"}, {"1"}, nativePlanningDigest("layout"), 1024, 16}}, 1024, 100, 500);
  const auto provider = members[1].provider;
  ProviderGroupCoordinatorOptions receiver;
  receiver.localProvider = provider;
  receiver.unwrapEpochKey = [&](const std::string& id, const ProviderGroupBytes& wrapped) {
    const auto plain = ndn_service_framework::unwrapSelectionGatedInputKey(ndn::Buffer(wrapped.begin(), wrapped.end()),
      certificates.at(id).getName(), keyChain);
    return ProviderGroupBytes(plain.begin(), plain.end());
  };
  ProviderGroupCoordinator consumer(receiver);
  const auto projected = capability.projectForProvider(provider);
  BOOST_CHECK_EQUAL(projected.wrappedEpochKeyByProvider.size(), 1);
  const auto wire = ProviderGroupCoordinator::encodeCapability(projected);
  consumer.installCapability(ProviderGroupCoordinator::decodeCapability(wire), {}, true);
  BOOST_CHECK(consumer.hasCapability());
  BOOST_CHECK_EQUAL(consumer.capability().epochKeyId, capability.epochKeyId);
  auto tampered = projected; tampered.sealerSignature[0] ^= 1;
  ProviderGroupCoordinator rejecting(receiver);
  BOOST_CHECK_THROW(rejecting.installCapability(tampered, {}, true), std::runtime_error);
  for (const auto& change : std::vector<std::pair<std::string, std::string>>{
      {"schemaVersion", "2"}, {"recipient", "/foreign"}, {"providerBootEpoch", "foreign"},
      {"recipientCertName", "/foreign/KEY/a/issuer/v=1"}, {"ndnsfDataV1EndpointPrefix", "/foreign/data"},
      {"recipientPublicKey", "ABC"}, {"recipientCertDigest", nativePlanningDigest("wrong-key")}}) {
    auto changed = input.acks;
    auto offer = changed.front().ack.getSelectionInputKeyOffer();
    offer.setField(change.first, change.second); changed.front().ack.setSelectionInputKeyOffer(offer);
    BOOST_CHECK_THROW(NativeGroupKeyAdmission(admission, changed, input.context, 200), std::exception);
  }
  auto duplicate = input.acks; duplicate.push_back(duplicate.front());
  BOOST_CHECK_THROW(NativeGroupKeyAdmission(admission, duplicate, input.context, 200), std::exception);
  auto unauthenticated = input.acks; unauthenticated.front().authenticationEvidence.trustSchemaValidated = false;
  BOOST_CHECK_THROW(NativeGroupKeyAdmission(admission, unauthenticated, input.context, 200), std::exception);
}
BOOST_AUTO_TEST_CASE(ProjectionBuilderDerivesApplicationInputAndDependencyReadiness)
{
  const auto f = oracle();
  for (const auto& name : {"cpu", "rank_cover"}) {
    const auto sample = std::find_if(f.at("seal_cases").begin(), f.at("seal_cases").end(),
      [&](const auto& value) { return value.at("name") == name; });
    BOOST_REQUIRE(sample != f.at("seal_cases").end());
    Input input(f, *sample);
    const bool single = input.roles.size() == 1;
    if (single) {
      input.split.inputIngressRole = input.split.resultEgressRole = input.roles.front().role;
      input.split.candidateDigest = input.split.computedDigest();
    }
    const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
    auto proposal = NativePreSplitFirstPlacement().proposeRoles(input.context, input.ackDigest, input.roles, input.offers, now);
    auto execution = input.split.executionPlan;
    execution.serviceName = input.context.serviceName; execution.modelName = input.inspected.descriptor.modelName;
    execution.roles.clear();
    NativePlanSealingInputs sealing;
    sealing.requesterIdentity = "/requester"; sealing.protectionEpoch = input.roles.front().protectionEpoch;
    sealing.expiresAtMs = input.context.deadlineMs;
    auto& artifacts = sealing.artifacts;
    artifacts.requestId = input.context.requestId; artifacts.attempt = input.context.attempt;
    artifacts.modelDigest = input.inspected.descriptor.contentDigest; artifacts.graphDigest = input.context.graphDigest;
    artifacts.canonicalGraphDigest = input.inspected.canonicalGraphDigest;
    artifacts.manifestDigest = input.inspected.modelManifestDigest; artifacts.recipeDigest = input.roles.front().recipeDigest;
    for (const auto& role : proposal.roles) {
      execution.roles.push_back(role.selectedRole);
      artifacts.sourceByRole[role.selectedRole] = "/catalog/source";
      artifacts.artifactDigestByRole[role.selectedRole] = role.artifactDigest;
    }
    for (std::size_t i = 0; i < input.roles.size(); ++i) sealing.assemblyByRole.emplace(std::to_string(i), input.roles[i]);
    NativeProjectionContext context{now, 1000, 4096};
    if (single) { context.logicalInputDigest = nativePlanningDigest("input"); context.inputLayoutDigest = nativePlanningDigest("input-layout"); }
    else {
      NativeDependencySpec dependency;
      dependency.producers = {execution.roles.front()}; dependency.consumers = {execution.roles.back()};
      dependency.keyScope = "activation"; dependency.tensors = {"hidden", "mask"};
      dependency.operationKind = "ACTIVATION";
      execution.dependencies = {dependency};
      context.dependencies[0] = {"group", "epoch", {{proposal.providerByRole.at(execution.roles.front()), proposal.providerByRole.at(execution.roles.front())}}, 7};
    }
    const auto seal = [&](const NativeExecutionPlan& plan) {
      return projectionPlan(NativePlanSealer::sealCore(input.inspected, input.split, proposal,
        plan, input.offers, input.ackDigest, sealing));
    };
    const auto sealed = seal(execution);
    BOOST_CHECK_EQUAL(sealed.core.modelDigest, input.inspected.descriptor.intentDigest());
    BOOST_CHECK_EQUAL(sealed.core.sourceContentDigest, input.inspected.descriptor.contentDigest);
    BOOST_CHECK(sealed.core.modelDigest != sealed.core.sourceContentDigest);
    auto foreignSource = sealing;
    foreignSource.artifacts.modelDigest = input.context.modelDigest;
    BOOST_CHECK_THROW(NativePlanSealer::sealCore(input.inspected, input.split, proposal,
      execution, input.offers, input.ackDigest, foreignSource), std::invalid_argument);
    const auto built = NativePlanProjectionBuilder::build(sealed, input.split, input.offers, context);
    std::shared_ptr<const NativeStaticProjectionLayout> layout;
    auto cachedContext = context;
    cachedContext.layoutOwner = &layout;
    const auto initial = NativePlanProjectionBuilder::build(sealed, input.split, input.offers, cachedContext);
    BOOST_REQUIRE(layout);
    for (const auto& role : execution.roles)
      BOOST_CHECK(nativeDataflowJson(initial.at(role).dataflow) == nativeDataflowJson(built.at(role).dataflow));
    const auto retainedLayout = layout;
    cachedContext.nowMs += 1;
    cachedContext.noProgressMs = 777;
    cachedContext.maxSegments = 2048;
    cachedContext.inlineFirstSegment = true;
    if (single) cachedContext.logicalInputDigest = nativePlanningDigest("new-input");
    else {
      cachedContext.dependencies.at(0).groupId = "new-group";
      cachedContext.dependencies.at(0).groupEpoch = "new-epoch";
      cachedContext.dependencies.at(0).operationIndex = 19;
    }
    const auto reused = NativePlanProjectionBuilder::build(sealed, input.split, input.offers, cachedContext);
    BOOST_CHECK(layout == retainedLayout);
    auto uncachedContext = cachedContext;
    uncachedContext.layoutOwner = nullptr;
    const auto reference = NativePlanProjectionBuilder::build(sealed, input.split, input.offers, uncachedContext);
    for (const auto& role : execution.roles) {
      BOOST_CHECK(nativeDataflowJson(reused.at(role).dataflow) == nativeDataflowJson(reference.at(role).dataflow));
      BOOST_CHECK(nativeDeviceBindingJson(reused.at(role).deviceBinding) == nativeDeviceBindingJson(reference.at(role).deviceBinding));
    }
    auto expiredCacheContext = cachedContext;
    expiredCacheContext.nowMs = sealed.core.expiresAtMs;
    BOOST_CHECK_THROW(NativePlanProjectionBuilder::build(sealed, input.split, input.offers, expiredCacheContext), std::invalid_argument);
    BOOST_CHECK_THROW(NativePlanProjectionBuilder::build(sealed, input.split, {}, cachedContext), std::invalid_argument);
    if (!single) {
      auto renamedTransport = execution;
      renamedTransport.dependencies.front().topicPrefix = "/new-request/activation";
      renamedTransport.dependencies.front().objectNameTemplate = "new-request-object";
      renamedTransport.dependencies.front().expectedSegments = 13;
      renamedTransport.dependencies.front().expectedBytes = 100000;
      const auto renamed = NativePlanProjectionBuilder::build(seal(renamedTransport), input.split, input.offers, cachedContext);
      BOOST_CHECK(layout == retainedLayout);
      BOOST_CHECK_EQUAL(renamed.at(execution.roles.front()).dataflow.mayPublish.front().producerNamespace, proposal.providerByRole.at(execution.roles.front()));
      auto invalidBinding = cachedContext;
      invalidBinding.dependencies.at(0).producerNamespaces.begin()->second.clear();
      BOOST_CHECK_THROW(NativePlanProjectionBuilder::build(sealed, input.split, input.offers, invalidBinding), std::invalid_argument);
      invalidBinding = cachedContext;
      invalidBinding.dependencies.at(0).producerNamespaces.begin()->second += "/transport";
      BOOST_CHECK_THROW(NativePlanProjectionBuilder::build(sealed, input.split, input.offers, invalidBinding), std::invalid_argument);
      invalidBinding = cachedContext;
      invalidBinding.dependencies.erase(0);
      BOOST_CHECK_THROW(NativePlanProjectionBuilder::build(sealed, input.split, input.offers, invalidBinding), std::invalid_argument);
      auto changedGeometry = execution;
      changedGeometry.dependencies.front().tensors = {"changed-tensor"};
      const auto changed = NativePlanProjectionBuilder::build(seal(changedGeometry), input.split, input.offers, cachedContext);
      BOOST_CHECK(layout != retainedLayout);
      BOOST_CHECK_EQUAL(changed.at(execution.roles.front()).dataflow.mayPublish.front().tensorId, "changed-tensor");
    }
    recordProjectionOracle(built);
    BOOST_REQUIRE_EQUAL(built.size(), execution.roles.size());
    const auto& first = built.at(execution.roles.front()).dataflow;
    const auto& last = built.at(execution.roles.back()).dataflow;
    BOOST_CHECK(last.terminalResponseOwner);
    if (single) {
      BOOST_REQUIRE_EQUAL(first.mustFetch.size(), 1);
      BOOST_CHECK_EQUAL(first.mustFetch.front().sourceKind, "APPLICATION_INPUT");
      BOOST_CHECK_EQUAL(first.mustFetch.front().tensorDigest, context.logicalInputDigest);
      BOOST_CHECK_EQUAL(first.mustFetch.front().targetLayoutDigest, context.inputLayoutDigest);
      BOOST_CHECK_EQUAL(first.mustFetch.front().hardDeadlineMs, sealed.core.expiresAtMs - now);
      auto missing = context; missing.logicalInputDigest.clear();
      BOOST_CHECK_THROW(NativePlanProjectionBuilder::build(sealed, input.split, input.offers, missing), std::invalid_argument);
    }
    else {
      BOOST_CHECK(!first.terminalResponseOwner);
      BOOST_REQUIRE_EQUAL(first.mayPublish.size(), 2);
      BOOST_REQUIRE_EQUAL(last.mustFetch.size(), 2);
      BOOST_REQUIRE_EQUAL(last.waitFor.size(), 1);
      BOOST_CHECK_EQUAL(last.waitFor.front().mode, "ALL");
      {
        ndn::security::KeyChain keyChain("pib-memory:group-projection", "tpm-memory:group-projection");
        const auto certificates = groupCertificates(input, keyChain);
        NativeOfferAdmission admission(f.at("policy").dump(),
          {{f.at("key_id").get<std::string>(), f.at("public_pem").get<std::string>()}}, f.at("candidate"));
        NativeGroupKeyAdmission keys(admission, input.acks, input.context, now);
        // A locator descendant may be advertised, but cannot replace the
        // sealed assignment's complete Producer when allocating tensor Names.
        auto descendantAcks = input.acks;
        auto descendantOffer = descendantAcks.front().ack.getSelectionInputKeyOffer();
        descendantOffer.setField("ndnsfDataV1EndpointPrefix",
          descendantOffer.getFields().at("recipient") + "/transport");
        descendantAcks.front().ack.setSelectionInputKeyOffer(descendantOffer);
        NativeGroupKeyAdmission descendantKeys(admission, descendantAcks, input.context, now);
        BOOST_CHECK_EXCEPTION(NativeGroupProjectionBuilder::build(sealed, input.split,
          descendantKeys, NativeProjectionContext{now, 1000, 4096}), std::invalid_argument,
          [](const std::invalid_argument& error) {
            return std::string(error.what()) ==
              "advertised tensor endpoint differs from the actual Producer";
          });
        const auto original = proposal.providerByRole;
        const auto preparedRole = [&](auto& prepared, const std::string& selectedRole) -> NativeSelectionRoleV3& {
          const auto selected = std::find_if(proposal.roles.begin(), proposal.roles.end(),
            [&](const auto& role) { return role.selectedRole == selectedRole; });
          if (selected == proposal.roles.end())
            throw std::runtime_error("prefetch fixture placement role is missing");
          const auto found = std::find_if(prepared.assemblyByRole.begin(), prepared.assemblyByRole.end(),
            [&](const auto& entry) {
              return entry.second.role == selected->role && entry.second.rank == selected->rank;
            });
          if (found == prepared.assemblyByRole.end())
            throw std::runtime_error("prefetch fixture selected role is missing");
          return found->second;
        };
        const auto withPrefetchContracts = [&](auto& prepared, auto& placement) {
          auto& source = preparedRole(prepared, execution.roles.front());
          auto& target = preparedRole(prepared, execution.roles.back());
          source.expectedOutputs = {{"hidden", "float32", {1, 8}}, {"mask", "uint8", {1}}};
          source.expectedInputs = {{"source-input", "float32", {1, 8}}};
          target.expectedInputs = source.expectedOutputs;
          for (auto& role : placement.roles) {
            const auto& metadata = preparedRole(prepared, role.selectedRole);
            role.expectedInputs = metadata.expectedInputs;
            role.expectedOutputs = metadata.expectedOutputs;
          }
        };
        NamedTensor hidden;
        hidden.name = "hidden"; hidden.elementType = TensorElementType::Float32;
        hidden.shape = {1, 8}; hidden.payload.resize(32);
        NamedTensor mask;
        mask.name = "mask"; mask.elementType = TensorElementType::UInt8;
        mask.shape = {1}; mask.payload = {1};
        const auto actualBundleBytes = encodeTensorBundle({hidden, mask}).size();
        NativeProjectionContext prefetchContext{now, 1000, 4096};
        const auto sourceProvider = original.at(execution.roles.front());
        const auto targetProvider = original.at(execution.roles.back());
        prefetchContext.tensorSegmentBytesByProvider[sourceProvider] = 7000;
        prefetchContext.tensorSegmentBytesByProvider[targetProvider] = 4000;
        auto prefetchSealing = sealing;
        auto prefetchProposal = proposal;
        withPrefetchContracts(prefetchSealing, prefetchProposal);
        const auto prefetchSealed = projectionPlan(NativePlanSealer::sealCore(
          input.inspected, input.split, prefetchProposal, execution, input.offers,
          input.ackDigest, prefetchSealing));
        const auto prefetchGroups = NativeGroupProjectionBuilder::build(
          prefetchSealed, input.split, keys, prefetchContext, 2U << 20);
        for (const auto& endpoint : prefetchGroups.at(execution.roles.front()).dataflow.mayPublish) {
          BOOST_CHECK_EQUAL(endpoint.prefetchBundleBytes, actualBundleBytes);
          BOOST_CHECK_EQUAL(endpoint.prefetchSegmentBytes, 7000U);
        }
        auto legacyContext = prefetchContext;
        legacyContext.tensorSegmentBytesByProvider.erase(targetProvider);
        const auto legacyGroups = NativeGroupProjectionBuilder::build(
          prefetchSealed, input.split, keys, legacyContext, 2U << 20);
        for (const auto& endpoint : legacyGroups.at(execution.roles.front()).dataflow.mayPublish)
          BOOST_CHECK_EQUAL(endpoint.prefetchBundleBytes, 0U);
        // A dynamic contract requires an exact current-input estimate with
        // the same complete member set; unrelated hints remain unknown.
        auto dynamicSealing = prefetchSealing;
        auto dynamicProposal = prefetchProposal;
        preparedRole(dynamicSealing, execution.roles.front()).expectedOutputs[0].shape[1] = "seq";
        preparedRole(dynamicSealing, execution.roles.back()).expectedInputs[0].shape[1] = "seq";
        for (auto& role : dynamicProposal.roles) {
          const auto& metadata = preparedRole(dynamicSealing, role.selectedRole);
          role.expectedInputs = metadata.expectedInputs;
          role.expectedOutputs = metadata.expectedOutputs;
        }
        const auto dynamicSealed = projectionPlan(NativePlanSealer::sealCore(
          input.inspected, input.split, dynamicProposal, execution, input.offers,
          input.ackDigest, dynamicSealing));
        auto dynamicContext = prefetchContext;
        dynamicContext.tensorPrefetchByDependency[0] = {actualBundleBytes, {"hidden", "mask"}};
        const auto dynamicGroups = NativeGroupProjectionBuilder::build(
          dynamicSealed, input.split, keys, dynamicContext, 2U << 20);
        BOOST_CHECK_EQUAL(dynamicGroups.at(execution.roles.front()).dataflow.mayPublish.front()
          .prefetchBundleBytes, actualBundleBytes);
        dynamicContext.tensorPrefetchByDependency[0].tensorNames = {"foreign"};
        const auto foreignGroups = NativeGroupProjectionBuilder::build(
          dynamicSealed, input.split, keys, dynamicContext, 2U << 20);
        BOOST_CHECK_EQUAL(foreignGroups.at(execution.roles.front()).dataflow.mayPublish.front()
          .prefetchBundleBytes, 0U);
        {
          // A small deployment may place both stages in one Provider.  The
          // group contract must remain role-scoped even though its member
          // list and wrapped key projection contain one Provider identity.
          auto colocated = proposal;
          // The fixture's normal placement uses cuda:0 on both machines.  Use
          // the second device on the co-located Provider so the placement
          // invariant remains explicit rather than weakening device fencing.
          BOOST_REQUIRE_EQUAL(colocated.roles.size(), 2);
          colocated.roles.back().deviceSet = {"cuda:1"};
          const auto originalProposal = proposal;
          const auto originalDeviceSet = sealing.assemblyByRole.at("1").deviceSet;
          sealing.assemblyByRole.at("1").deviceSet = {"cuda:1"};
          const auto provider = original.at(execution.roles.front());
          colocated.providerByRole.at(execution.roles.back()) = provider;
          const auto offer = std::find_if(input.offers.begin(), input.offers.end(),
            [&](const auto& item) { return item.observation().provider == provider; });
          BOOST_REQUIRE(offer != input.offers.end());
          colocated.offerDigestByProvider = {{provider, offer->observation().offerDigest}};
          proposal = colocated;
          const auto colocatedPlan = seal(execution);
          NativeProjectionContext colocatedContext{now, 1000, 4096};
          const auto colocatedGroups = NativeGroupProjectionBuilder::build(
            colocatedPlan, input.split, keys, colocatedContext, 4096);
          BOOST_REQUIRE_EQUAL(colocatedGroups.size(), execution.roles.size());
          BOOST_CHECK(!colocatedGroups.at(execution.roles.front()).groupCapabilityV1.empty());
          BOOST_CHECK_EQUAL(colocatedGroups.at(execution.roles.front()).dataflow.mayPublish.size(), 2);
          BOOST_CHECK_EQUAL(colocatedGroups.at(execution.roles.back()).dataflow.mustFetch.size(), 2);
          proposal = originalProposal;
          sealing.assemblyByRole.at("1").deviceSet = originalDeviceSet;
        }
        std::swap(proposal.providerByRole.at(execution.roles.front()), proposal.providerByRole.at(execution.roles.back()));
        const auto swapped = seal(execution);
        NativeProjectionContext requestContext{now, 1000, 4096};
        const auto authorized = NativeGroupProjectionBuilder::build(swapped, input.split, keys, requestContext, 4096);
        const auto& source = authorized.at(execution.roles.front());
        const auto& target = authorized.at(execution.roles.back());
        BOOST_REQUIRE_EQUAL(source.dataflow.mayPublish.size(), 2);
        BOOST_CHECK_NE(source.dataflow.mayPublish[0].round, source.dataflow.mayPublish[1].round);
        BOOST_CHECK_NE(source.dataflow.mayPublish[0].producerRank, source.executionRole.rank);
        const auto openCoordinator = [&](const std::string& role) {
          const auto provider = swapped.core.assignment.providerByRole.at(role);
          ProviderGroupCoordinatorOptions options;
          options.localProvider = provider;
          options.unwrapEpochKey = [&](const std::string& id, const ProviderGroupBytes& wrapped) {
            const auto plain = ndn_service_framework::unwrapSelectionGatedInputKey(ndn::Buffer(wrapped.begin(), wrapped.end()),
              certificates.at(id).getName(), keyChain);
            return ProviderGroupBytes(plain.begin(), plain.end());
          };
          auto owner = std::make_shared<ProviderGroupCoordinator>(options);
          const auto wire = ndn_service_framework::selectionGatedUnhex(authorized.at(role).groupCapabilityV1);
          owner->installCapability(ProviderGroupCoordinator::decodeCapability({wire.begin(), wire.end()}), {}, true);
          return owner;
        };
        const auto producer = openCoordinator(execution.roles.front()), consumer = openCoordinator(execution.roles.back());
        BOOST_REQUIRE_EQUAL(producer->capability().permittedOperations.size(), 2);
        for (std::size_t i = 0; i < source.dataflow.mayPublish.size(); ++i) {
          const auto& endpoint = source.dataflow.mayPublish[i];
          BOOST_CHECK_EQUAL(endpoint.endpointDigest, target.dataflow.mustFetch[i].endpointDigest);
          BOOST_CHECK_EQUAL(endpoint.endpointDigest, target.dataflow.waitFor.front().endpointDigests[i]);
          const auto& operations = producer->capability().permittedOperations;
          const auto op = std::find_if(operations.begin(), operations.end(), [&](const auto& value) { return value.operationIndex == endpoint.round; });
          BOOST_REQUIRE(op != operations.end());
          const auto transfer = producer->sealOperation(*op, std::to_string(endpoint.producerRank),
            endpoint.layoutDigest, endpoint.targetLayoutDigest, endpoint.tensorDigest, {{1, 2, 3}}, now,
            {tensorObjectSegmentName(endpoint, 0)});
          BOOST_CHECK(consumer->openSegment(transfer.manifest, transfer.segments.front(),
            tensorObjectSegmentName(endpoint, 0)) == ProviderGroupBytes({1, 2, 3}));
        }
        {
          // Exercise trusted construction and the complete wire boundary, not
          // a hand-built endpoint/digest oracle. Keep the outer fixture intact.
          auto generationPlan = execution;
          // The older builder-only fixture omitted wire-required dependency
          // fields. Supply them on this copy before the parser roundtrip.
          generationPlan.dependencies.front().topicPrefix = "/tensor";
          generationPlan.dependencies.front().objectNameTemplate =
            "{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}/{sequence}";
          auto generationSealing = sealing;
          const auto options = NativeJson{{"useCache", true}, {"outputMode", "TOKEN_STREAMING"},
            {"maxNewTokens", 1}, {"tokenizerDigest", nativePlanningDigest("tokenizer")},
            {"eosTokenIds", NativeJson::array({2})}}.dump();
          generationSealing.generationContract = nativeGenerationFromOptions(
            {options.begin(), options.end()}, std::string(32, 'a'));
          generationSealing.generationContract.streamingOperationStride = 3;
          generationPlan.streamingOperationStride = 3;
          NativeDependencySpec feedback;
          feedback.producers = {execution.roles.back()};
          feedback.consumers = {execution.roles.front()};
          feedback.keyScope = "generation-feedback";
          feedback.topicPrefix = "/token-feedback";
          feedback.objectNameTemplate = "{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}/{sequence}";
          feedback.tensors = {"input_ids"};
          feedback.operationKind = "TOKEN_FEEDBACK";
          feedback.useNdnsfDataV1 = true;
          feedback.collectiveOperationIndex = 2;
          std::set<std::string> providers;
          for (const auto& assignment : proposal.providerByRole) providers.insert(assignment.second);
          feedback.collectiveProducerRank = std::to_string(std::distance(providers.begin(),
            providers.find(proposal.providerByRole.at(execution.roles.back()))));
          // These digests configure the input feedback contract only. All
          // endpoint/manifest identities below come from production builders.
          feedback.collectiveSourceLayoutDigest = feedback.collectiveTargetLayoutDigest =
            nativePlanningDigest(nativeCanonicalJson(NativeJson{{"tensor", "input_ids"},
              {"layout", "int64[1,1]"}, {"operation", "TOKEN_FEEDBACK"}}));
          feedback.collectiveTensorDigest = nativePlanningDigest(
            nativeCanonicalJson(NativeJson::array({"input_ids"})));
          generationPlan.dependencies.push_back(feedback);
          {
            // Production builder/sealer, not a hand-computed wire-size model.
            // Preserve the existing 4 MiB gate: an oversized 1024-token
            // projection must fail this regression rather than relax the gate.
            auto largeSealing = generationSealing;
            largeSealing.generationContract.maxGeneratedTokens = 1024;
            const auto oversizedPlan = projectionPlan(NativePlanSealer::sealCore(
              input.inspected, input.split, proposal, generationPlan, input.offers,
              input.ackDigest, largeSealing));
            const auto oversizedGroups = NativeGroupProjectionBuilder::build(
              oversizedPlan, input.split, keys, requestContext, 4096);
            for (const auto& role : execution.roles) {
              const auto projected = NativePlanSealer::project(oversizedPlan,
                proposal.providerByRole.at(role), oversizedGroups.at(role));
              BOOST_CHECK_EXCEPTION(NativePlanSealer::encode(projected), std::invalid_argument,
                [](const std::invalid_argument& error) {
                  return std::string(error.what()) ==
                    "V3 Selection exceeds external assignment wire limit";
                });
            }
            // Keep a complete, independently declared bounded large request;
            // the 1024-token projection above is never truncated or stripped.
            largeSealing.generationContract.maxGeneratedTokens = 512;
            const auto largePlan = projectionPlan(NativePlanSealer::sealCore(
              input.inspected, input.split, proposal, generationPlan, input.offers,
              input.ackDigest, largeSealing));
            const auto largeGroups = NativeGroupProjectionBuilder::build(
              largePlan, input.split, keys, requestContext, 4096);
            for (const auto& role : execution.roles) {
              const auto projected = NativePlanSealer::project(largePlan,
                proposal.providerByRole.at(role), largeGroups.at(role));
              const auto wire = NativePlanSealer::encode(projected);
              BOOST_TEST_MESSAGE("512-token Selection role=" << role << " bytes=" << wire.size());
              BOOST_REQUIRE_LE(wire.size(), 4U << 20);
              std::istringstream stream(std::string(wire.begin(), wire.end()));
              const auto decoded = nativeSelectionProjectionV3FromJson(stream, role);
              BOOST_CHECK_EQUAL(decoded.generationContract.maxGeneratedTokens, 512);
              const auto& endpoints = role == execution.roles.front() ?
                decoded.dataflow.mayPublish : decoded.dataflow.mustFetch;
              BOOST_REQUIRE_EQUAL(endpoints.size(), 2U * 513U);
              const auto& feedbackEndpoints = role == execution.roles.front() ?
                decoded.dataflow.mustFetch : decoded.dataflow.mayPublish;
              BOOST_CHECK_EQUAL(std::count_if(feedbackEndpoints.begin(), feedbackEndpoints.end(),
                [](const auto& endpoint) { return endpoint.operation == "TOKEN_FEEDBACK"; }), 513U);
              for (const std::uint64_t epoch : {0U, 64U, 512U}) {
                const auto local = roleSpecFromSelectionProjectionV3(decoded, decoded.provider, epoch);
                BOOST_CHECK_EQUAL((role == execution.roles.front() ? local.outputs : local.inputs).size(), 2);
                const auto& localFeedback = role == execution.roles.front() ? local.inputs : local.outputs;
                const auto feedback = std::find_if(localFeedback.begin(), localFeedback.end(),
                  [](const auto& edge) { return edge.operationKind == "TOKEN_FEEDBACK"; });
                BOOST_REQUIRE(feedback != localFeedback.end());
                BOOST_CHECK_EQUAL(feedback->round, 2 + 3 * epoch);
                BOOST_CHECK_EQUAL(feedback->collectiveOperationIndex, 2 + 3 * epoch);
                BOOST_CHECK_EQUAL(feedback->namingOperationStride, 3);
              }
              BOOST_CHECK_THROW(roleSpecFromSelectionProjectionV3(decoded, decoded.provider, 513),
                                std::invalid_argument);
            }
          }
          const auto generationSealed = projectionPlan(NativePlanSealer::sealCore(
            input.inspected, input.split, proposal, generationPlan, input.offers,
            input.ackDigest, generationSealing));
          const auto generationGroups = NativeGroupProjectionBuilder::build(
            generationSealed, input.split, keys, requestContext, 4096);
          auto multiRoleSelfPlan = generationPlan;
          multiRoleSelfPlan.dependencies.back().consumers =
            multiRoleSelfPlan.dependencies.back().producers;
          const auto multiRoleSelf = projectionPlan(NativePlanSealer::sealCore(
            input.inspected, input.split, proposal, multiRoleSelfPlan, input.offers,
            input.ackDigest, generationSealing));
          BOOST_CHECK_EXCEPTION(NativeGroupProjectionBuilder::build(multiRoleSelf,
            input.split, keys, requestContext, 4096), std::invalid_argument,
            [](const std::invalid_argument& error) {
              return std::string(error.what()) ==
                "self feedback requires the authenticated single-role local plan";
            });

          // Preserve the old warm/rebound layout oracle on actual signed
          // generation feedback instead of the unsigned cycle fixture below.
          std::shared_ptr<const NativeStaticProjectionLayout> feedbackLayout;
          auto warmGenerationContext = requestContext;
          warmGenerationContext.layoutOwner = &feedbackLayout;
          const auto cachedGeneration = NativeGroupProjectionBuilder::build(
            generationSealed, input.split, keys, warmGenerationContext, 4096);
          BOOST_REQUIRE(feedbackLayout);
          const auto retainedFeedbackLayout = feedbackLayout;
          generationPlan.dependencies.back().keyScope = "generation-feedback-rebound";
          const auto reboundGenerationSealed = projectionPlan(NativePlanSealer::sealCore(
            input.inspected, input.split, proposal, generationPlan, input.offers,
            input.ackDigest, generationSealing));
          const auto reboundGeneration = NativeGroupProjectionBuilder::build(
            reboundGenerationSealed, input.split, keys, warmGenerationContext, 4096);
          const auto freshGeneration = NativeGroupProjectionBuilder::build(
            reboundGenerationSealed, input.split, keys, requestContext, 4096);
          BOOST_CHECK(feedbackLayout == retainedFeedbackLayout);
          for (const auto& role : execution.roles) {
            BOOST_CHECK_EQUAL(reboundGeneration.at(role).dataflow.mayPublish.size(),
                              cachedGeneration.at(role).dataflow.mayPublish.size());
            BOOST_CHECK(nativeDataflowJson(reboundGeneration.at(role).dataflow) ==
                        nativeDataflowJson(freshGeneration.at(role).dataflow));
          }
          generationPlan.dependencies.back().keyScope = feedback.keyScope;
          auto hintedGenerationSealing = generationSealing;
          auto hintedGenerationProposal = proposal;
          withPrefetchContracts(hintedGenerationSealing, hintedGenerationProposal);
          const auto hintedGenerationSealed = projectionPlan(NativePlanSealer::sealCore(
            input.inspected, input.split, hintedGenerationProposal, generationPlan, input.offers,
            input.ackDigest, hintedGenerationSealing));
          auto hintedGenerationContext = requestContext;
          hintedGenerationContext.tensorSegmentBytesByProvider[
            proposal.providerByRole.at(execution.roles.front())] = 7000;
          hintedGenerationContext.tensorSegmentBytesByProvider[
            proposal.providerByRole.at(execution.roles.back())] = 4000;
          const auto hintedGenerationGroups = NativeGroupProjectionBuilder::build(
            hintedGenerationSealed, input.split, keys, hintedGenerationContext, 2U << 20);
          for (const auto& role : execution.roles) {
            const auto hintedProjection = NativePlanSealer::project(hintedGenerationSealed,
              proposal.providerByRole.at(role), hintedGenerationGroups.at(role));
            const auto wire = NativePlanSealer::encode(hintedProjection);
            std::istringstream stream(std::string(wire.begin(), wire.end()));
            const auto decoded = nativeSelectionProjectionV3FromJson(stream, role);
            const auto& endpoints = role == execution.roles.front() ?
              decoded.dataflow.mayPublish : decoded.dataflow.mustFetch;
            const auto wireValue = nativeParseJson(std::string(wire.begin(), wire.end()));
            for (const auto& invalid : {NativeJson("7000"), NativeJson(-1),
                 NativeJson(7000.0), NativeJson(nullptr), NativeJson(true), NativeJson(0),
                 NativeJson(std::uint64_t{ndn::MAX_NDN_PACKET_SIZE} + 1)}) {
              auto changed = wireValue;
              auto& endpoint = changed["dataflow"][role == execution.roles.front() ?
                "may_publish" : "must_fetch"][0];
              endpoint["prefetch_segment_bytes"] = invalid;
              std::istringstream invalidStream(nativeCanonicalJson(changed));
              BOOST_CHECK_THROW(nativeSelectionProjectionV3FromJson(invalidStream, role),
                                std::exception);
            }
            for (const auto* field : {"prefetch_bundle_bytes", "prefetch_segment_bytes"}) {
              auto changed = wireValue;
              changed["dataflow"][role == execution.roles.front() ?
                "may_publish" : "must_fetch"][0].erase(field);
              std::istringstream invalidStream(nativeCanonicalJson(changed));
              BOOST_CHECK_THROW(nativeSelectionProjectionV3FromJson(invalidStream, role),
                                std::exception);
            }
            for (const auto& endpoint : endpoints) {
              BOOST_CHECK_EQUAL(endpoint.prefetchBundleBytes,
                endpoint.round < 3 ? actualBundleBytes : 0U);
              BOOST_CHECK_EQUAL(endpoint.prefetchSegmentBytes,
                endpoint.round < 3 ? 7000U : 0U);
            }
            const auto local = roleSpecFromSelectionProjectionV3(decoded, decoded.provider, 0);
            for (const auto& edge : role == execution.roles.front() ? local.outputs : local.inputs) {
              BOOST_CHECK_EQUAL(edge.prefetchBundleBytes, actualBundleBytes);
              BOOST_CHECK_EQUAL(edge.expectedBytes, 0U);
              BOOST_CHECK_EQUAL(edge.expectedSegments, 0U);
            }
          }
          std::vector<NativeSelectionProjectionV3> parsed;
          for (const auto& role : execution.roles) {
            const auto projection = NativePlanSealer::project(generationSealed,
              proposal.providerByRole.at(role), generationGroups.at(role));
            const auto wire = NativePlanSealer::encode(projection);
            std::istringstream stream(std::string(wire.begin(), wire.end()));
            parsed.push_back(nativeSelectionProjectionV3FromJson(stream, role));
            BOOST_CHECK_EQUAL(parsed.back().generationContract.maxGeneratedTokens, 1);
            BOOST_CHECK_EQUAL(parsed.back().generationContract.streamingOperationStride, 3);
          }
          BOOST_REQUIRE_EQUAL(parsed.size(), 2);
          validateNativeSelectionProjectionSetV3(parsed);
          // Feedback remains subject to exact ownership and signed generation
          // admission even though it does not enter the initial-readiness DAG.
          auto missingFeedbackOwner = parsed;
          missingFeedbackOwner[1].dataflow.mayPublish.erase(
            missingFeedbackOwner[1].dataflow.mayPublish.begin());
          BOOST_CHECK_EXCEPTION(validateNativeSelectionProjectionSetV3(missingFeedbackOwner),
            std::invalid_argument, [](const std::invalid_argument& error) {
              return std::string(error.what()) == "V3 mustFetch has no exact single mayPublish owner";
            });
          auto changedFeedbackOwner = parsed;
          changedFeedbackOwner[0].dataflow.mustFetch.front().producerRole = "foreign-role";
          BOOST_CHECK_EXCEPTION(validateNativeSelectionProjectionSetV3(changedFeedbackOwner),
            std::invalid_argument, [](const std::invalid_argument& error) {
              return std::string(error.what()) == "V3 mustFetch has no exact single mayPublish owner";
            });
          auto unsignedFeedback = parsed;
          unsignedFeedback[0].generationContract.enabled = false;
          BOOST_CHECK_EXCEPTION(validateNativeSelectionProjectionSetV3(unsignedFeedback),
            std::invalid_argument, [](const std::invalid_argument& error) {
              return std::string(error.what()) == "V3 feedback endpoint differs from its signed generation";
            });
          auto changedGeneration = parsed;
          ++changedGeneration[1].generationContract.maxGeneratedTokens;
          BOOST_CHECK_EXCEPTION(validateNativeSelectionProjectionSetV3(changedGeneration),
            std::invalid_argument, [](const std::invalid_argument& error) {
              return std::string(error.what()) == "V3 feedback endpoint differs from its signed generation";
            });
          auto changedStride = parsed;
          for (auto& value : changedStride)
            for (auto* endpoints : {&value.dataflow.mayPublish, &value.dataflow.mustFetch})
              for (auto& endpoint : *endpoints)
                if (endpoint.operation == "TOKEN_FEEDBACK") ++endpoint.namingOperationStride;
          BOOST_CHECK_EXCEPTION(validateNativeSelectionProjectionSetV3(changedStride),
            std::invalid_argument, [](const std::invalid_argument& error) {
              return std::string(error.what()) == "V3 feedback endpoint differs from its signed generation";
            });
          auto ordinaryCycle = parsed;
          for (auto& value : ordinaryCycle) {
            for (auto* endpoints : {&value.dataflow.mayPublish, &value.dataflow.mustFetch})
              for (auto& endpoint : *endpoints)
                if (endpoint.operation == "TOKEN_FEEDBACK") endpoint.operation = "PIPELINE_TRANSFER";
          }
          BOOST_CHECK_EXCEPTION(validateNativeSelectionProjectionSetV3(ordinaryCycle),
            std::invalid_argument, [](const std::invalid_argument& error) {
              return std::string(error.what()) == "V3 role dataflow contains a cycle";
            });

          BOOST_REQUIRE_EQUAL(parsed[0].dataflow.mayPublish.size(), 4);
          BOOST_REQUIRE_EQUAL(parsed[1].dataflow.mustFetch.size(), 4);
          BOOST_REQUIRE_EQUAL(parsed[1].dataflow.waitFor.size(), 1);
          const auto& readiness = parsed[1].dataflow.waitFor.front().endpointDigests;
          BOOST_REQUIRE_EQUAL(readiness.size(), 2);
          for (const auto& endpoint : parsed[1].dataflow.mustFetch) {
            const bool ready = std::find(readiness.begin(), readiness.end(), endpoint.endpointDigest) != readiness.end();
            BOOST_CHECK_EQUAL(ready, endpoint.round < 3);
          }
          for (const auto& projection : parsed) {
            const auto capabilityWire = ndn_service_framework::selectionGatedUnhex(projection.groupCapabilityV1);
            const auto capability = ProviderGroupCoordinator::decodeCapability(
              {capabilityWire.begin(), capabilityWire.end()});
            BOOST_REQUIRE_EQUAL(capability.permittedOperations.size(), 6);
            for (std::uint64_t epoch = 0; epoch != 2; ++epoch) {
              const auto op = std::find_if(capability.permittedOperations.begin(), capability.permittedOperations.end(),
                [&](const auto& value) { return value.operationIndex == 2 + 3 * epoch; });
              BOOST_REQUIRE(op != capability.permittedOperations.end());
              BOOST_CHECK_EQUAL(op->kind, "TOKEN_FEEDBACK");
              BOOST_REQUIRE_EQUAL(op->producerRanks.size(), 1);
              BOOST_CHECK_EQUAL(op->producerRanks.front(), feedback.collectiveProducerRank);
            }
          }
          const auto generationCoordinator = [&](const NativeSelectionProjectionV3& projection) {
            ProviderGroupCoordinatorOptions coordinatorOptions;
            coordinatorOptions.localProvider = projection.provider;
            coordinatorOptions.unwrapEpochKey = [&](const std::string& id, const ProviderGroupBytes& wrapped) {
              const auto plain = ndn_service_framework::unwrapSelectionGatedInputKey(
                ndn::Buffer(wrapped.begin(), wrapped.end()), certificates.at(id).getName(), keyChain);
              return ProviderGroupBytes(plain.begin(), plain.end());
            };
            auto owner = std::make_shared<ProviderGroupCoordinator>(coordinatorOptions);
            const auto wire = ndn_service_framework::selectionGatedUnhex(projection.groupCapabilityV1);
            owner->installCapability(ProviderGroupCoordinator::decodeCapability({wire.begin(), wire.end()}), {}, true);
            return owner;
          };
          const auto generationProducer = generationCoordinator(parsed[0]);
          const auto generationConsumer = generationCoordinator(parsed[1]);
          for (const auto& endpoint : parsed[0].dataflow.mayPublish) {
            const auto& operations = generationProducer->capability().permittedOperations;
            const auto op = std::find_if(operations.begin(), operations.end(),
              [&](const auto& value) { return value.operationIndex == endpoint.round; });
            BOOST_REQUIRE(op != operations.end());
            BOOST_CHECK_EQUAL(op->kind, endpoint.operation);
            BOOST_REQUIRE_EQUAL(op->producerRanks.size(), 1);
            BOOST_CHECK_EQUAL(op->producerRanks.front(), std::to_string(endpoint.producerRank));
            const auto transfer = generationProducer->sealOperation(*op, std::to_string(endpoint.producerRank),
              endpoint.layoutDigest, endpoint.targetLayoutDigest, endpoint.tensorDigest, {{1, 2, 3}}, now,
              {tensorObjectSegmentName(endpoint, 0)});
            BOOST_CHECK(generationConsumer->openSegment(transfer.manifest, transfer.segments.front(),
              tensorObjectSegmentName(endpoint, 0)) ==
              ProviderGroupBytes({1, 2, 3}));
          }
          BOOST_REQUIRE_EQUAL(parsed[1].dataflow.mayPublish.size(), 2);
          BOOST_REQUIRE_EQUAL(parsed[0].dataflow.mustFetch.size(), 2);
          BOOST_CHECK(parsed[0].dataflow.waitFor.empty());
          for (std::size_t epoch = 0; epoch != 2; ++epoch) {
            const auto& output = parsed[1].dataflow.mayPublish[epoch];
            const auto& inputEndpoint = parsed[0].dataflow.mustFetch[epoch];
            BOOST_CHECK_EQUAL(output.operation, "TOKEN_FEEDBACK");
            BOOST_CHECK_EQUAL(output.round, 2 + 3 * epoch);
            BOOST_CHECK_EQUAL(output.namingOperationStride, 3);
            BOOST_CHECK_EQUAL(output.producerRole, execution.roles.back());
            BOOST_CHECK_EQUAL(output.endpointDigest, inputEndpoint.endpointDigest);
            BOOST_CHECK(ndn::Name(tensorObjectManifestName(output)) ==
                        ndn::Name(tensorObjectManifestName(inputEndpoint)));
            const auto& operations = generationConsumer->capability().permittedOperations;
            const auto op = std::find_if(operations.begin(), operations.end(),
              [&](const auto& value) { return value.operationIndex == output.round; });
            BOOST_REQUIRE(op != operations.end());
            const auto transfer = generationConsumer->sealOperation(*op,
              std::to_string(output.producerRank), output.layoutDigest,
              output.targetLayoutDigest, output.tensorDigest, {{1, 2, 3}}, now,
              {tensorObjectSegmentName(output, 0)});
            BOOST_CHECK(generationProducer->openSegment(transfer.manifest,
              transfer.segments.front(), tensorObjectSegmentName(inputEndpoint, 0)) ==
              ProviderGroupBytes({1, 2, 3}));
            const auto localSender = roleSpecFromSelectionProjectionV3(parsed[1], parsed[1].provider, epoch);
            const auto localReceiver = roleSpecFromSelectionProjectionV3(parsed[0], parsed[0].provider, epoch);
            BOOST_REQUIRE_EQUAL(localSender.outputs.size(), 1);
            BOOST_REQUIRE_EQUAL(localReceiver.inputs.size(), 1);
            BOOST_CHECK_EQUAL(localSender.outputs.front().plannedDataName,
                              localReceiver.inputs.front().plannedDataName);
            BOOST_CHECK_EQUAL(localSender.outputs.front().collectiveOperationIndex, 2 + 3 * epoch);
          }
          auto feedbackReadiness = NativeJson::parse(nativeSelectionProjectionV3ToJson(parsed[0]));
          feedbackReadiness["dataflow"]["wait_for"] = NativeJson::array({
            NativeJson{{"mode", "ALL"}, {"endpoint_digests", NativeJson::array({
              parsed[0].dataflow.mustFetch.front().endpointDigest})}}});
          std::istringstream feedbackReadinessStream(nativeCanonicalJson(feedbackReadiness));
          BOOST_CHECK_EXCEPTION(nativeSelectionProjectionV3FromJson(feedbackReadinessStream,
            parsed[0].executionRole.roleId), std::invalid_argument,
            [](const std::invalid_argument& error) {
              return std::string(error.what()) ==
                "TOKEN_FEEDBACK cannot be an initial readiness dependency";
            });
          const auto prefill = roleSpecFromSelectionProjectionV3(parsed[0], parsed[0].provider, 0);
          BOOST_REQUIRE_EQUAL(prefill.outputs.size(), 2);
          for (std::uint64_t epoch = 0; epoch != 2; ++epoch) {
            const auto sender = roleSpecFromSelectionProjectionV3(parsed[0], parsed[0].provider, epoch);
            const auto receiver = roleSpecFromSelectionProjectionV3(parsed[1], parsed[1].provider, epoch);
            BOOST_REQUIRE_EQUAL(sender.outputs.size(), 2);
            BOOST_REQUIRE_EQUAL(receiver.inputs.size(), 2);
            std::set<std::string> tensors;
            for (std::size_t i = 0; i != 2; ++i) {
              const auto& out = sender.outputs[i];
              const auto& in = receiver.inputs[i];
              BOOST_REQUIRE_EQUAL(out.tensors.size(), 1);
              BOOST_CHECK(out.tensors == in.tensors);
              tensors.insert(out.tensors.front());
              BOOST_CHECK_EQUAL(out.plannedDataName, in.plannedDataName);
              BOOST_CHECK_EQUAL(out.endpointDigest, in.endpointDigest);
              BOOST_CHECK_EQUAL(out.manifestContractDigest, in.manifestContractDigest);
              BOOST_CHECK_EQUAL(out.collectiveOperationIndex, prefill.outputs[i].collectiveOperationIndex + 3 * epoch);
              if (epoch) {
                BOOST_CHECK_NE(out.plannedDataName, prefill.outputs[i].plannedDataName);
                BOOST_CHECK_NE(out.endpointDigest, prefill.outputs[i].endpointDigest);
                BOOST_CHECK_NE(out.manifestContractDigest, prefill.outputs[i].manifestContractDigest);
              }
            }
            BOOST_CHECK(tensors == std::set<std::string>({"hidden", "mask"}));
          }
          for (const auto& projection : parsed) {
            BOOST_CHECK_THROW(roleSpecFromSelectionProjectionV3(projection, projection.provider, 2), std::invalid_argument);
            auto missing = projection;
            auto& missingEndpoints = missing.dataflow.mayPublish.empty() ? missing.dataflow.mustFetch : missing.dataflow.mayPublish;
            missingEndpoints.pop_back();
            BOOST_CHECK_THROW(roleSpecFromSelectionProjectionV3(missing, missing.provider, 0), std::invalid_argument);
            auto duplicate = projection;
            auto& duplicateEndpoints = duplicate.dataflow.mayPublish.empty() ? duplicate.dataflow.mustFetch : duplicate.dataflow.mayPublish;
            const auto endpoint = duplicateEndpoints.back();
            duplicateEndpoints.push_back(endpoint);
            BOOST_CHECK_THROW(roleSpecFromSelectionProjectionV3(duplicate, duplicate.provider, 1), std::invalid_argument);
          }
        }
        BOOST_CHECK_THROW(NativeGroupProjectionBuilder::build(swapped, input.split, keys, context), std::invalid_argument);
        auto expired = requestContext; expired.nowMs = swapped.core.expiresAtMs;
        BOOST_CHECK_THROW(NativeGroupProjectionBuilder::build(swapped, input.split, keys, expired), std::invalid_argument);
        auto redistributedPlan = execution;
        for (const auto& tensor : std::vector<std::string>{"hidden", "mask"})
          redistributedPlan.dependencies.front().redistributions.push_back(RedistributionSpec{{0}, {1}, tensor,
            "GATHER", "epoch", nativePlanningDigest(tensor), nativePlanningDigest("source-" + tensor),
            nativePlanningDigest("target-" + tensor), 0, 16, true});
        const auto redistributedGroups = NativeGroupProjectionBuilder::build(seal(redistributedPlan), input.split, keys, requestContext);
        const auto& transfers = redistributedGroups.at(execution.roles.front()).dataflow.mayPublish;
        BOOST_REQUIRE_EQUAL(transfers.size(), 2);
        BOOST_CHECK_NE(transfers[0].round, transfers[1].round);
        BOOST_CHECK_NE(transfers[0].targetLayoutDigest, transfers[1].targetLayoutDigest);
        auto feedbackPlan = execution;
        auto feedbackEdge = execution.dependencies.front();
        std::swap(feedbackEdge.producers, feedbackEdge.consumers); feedbackEdge.operationKind = "TOKEN_FEEDBACK";
        feedbackPlan.dependencies.push_back(feedbackEdge);
        BOOST_CHECK_THROW(NativeGroupProjectionBuilder::build(seal(feedbackPlan), input.split, keys, requestContext), std::invalid_argument);
        proposal.providerByRole = original;
      }
      for (std::size_t i = 0; i < 2; ++i) {
        BOOST_CHECK_EQUAL(first.mayPublish[i].endpointDigest, last.mustFetch[i].endpointDigest);
        BOOST_CHECK_EQUAL(first.mayPublish[i].tensorId, execution.dependencies.front().tensors[i]);
        BOOST_CHECK_EQUAL(first.mayPublish[i].producerNamespace, proposal.providerByRole.at(execution.roles.front()));
        BOOST_CHECK_EQUAL(first.mayPublish[i].round, 7);
        BOOST_CHECK_EQUAL(first.mayPublish[i].segmentCount, 4096);
        BOOST_CHECK_EQUAL(last.waitFor.front().endpointDigests[i], last.mustFetch[i].endpointDigest);
      }
      auto missing = context; missing.dependencies.clear();
      BOOST_CHECK_THROW(NativePlanProjectionBuilder::build(sealed, input.split, input.offers, missing), std::invalid_argument);
      auto cycle = execution; auto back = execution.dependencies.front();
      std::swap(back.producers, back.consumers); cycle.dependencies.push_back(back);
      auto cycleContext = context;
      cycleContext.dependencies[1] = {"back", "epoch", {{proposal.providerByRole.at(execution.roles.back()), proposal.providerByRole.at(execution.roles.back())}}, 8};
      BOOST_CHECK_THROW(NativePlanProjectionBuilder::build(seal(cycle), input.split, input.offers, cycleContext), std::invalid_argument);
      auto feedback = cycle; feedback.dependencies.back().operationKind = "TOKEN_FEEDBACK";
      // This historical cycle has no signed generation contract or feedback
      // tensor commitments. It is intentionally rejected, not a warm oracle.
      BOOST_CHECK_THROW(NativePlanProjectionBuilder::build(seal(feedback), input.split,
        input.offers, context), std::invalid_argument);
      BOOST_CHECK_THROW(NativePlanProjectionBuilder::build(seal(feedback), input.split,
        input.offers, cachedContext), std::invalid_argument);
      auto redistributed = execution;
      redistributed.dependencies.front().redistributions = {RedistributionSpec{{0}, {1}, "hidden", "GATHER", "epoch",
        nativePlanningDigest("tensor"), nativePlanningDigest("source-layout"), nativePlanningDigest("target-layout"), 0, 16, true}};
      const auto gathered = NativePlanProjectionBuilder::build(seal(redistributed), input.split, input.offers, context);
      recordProjectionOracle(gathered);
      const auto& endpoints = gathered.at(execution.roles.back()).dataflow.mustFetch;
      BOOST_REQUIRE_EQUAL(endpoints.size(), 1);
      BOOST_CHECK_EQUAL(endpoints.front().operation, "GATHER");
      BOOST_CHECK_EQUAL(endpoints.front().targetLayoutDigest, nativePlanningDigest("target-layout"));
      BOOST_CHECK_EQUAL(endpoints.front().producerRank, 0);
      redistributed.dependencies.front().redistributions.front().consumerRanks = {9};
      BOOST_CHECK_THROW(NativePlanProjectionBuilder::build(seal(redistributed), input.split, input.offers, context), std::invalid_argument);
    }
  }
}

BOOST_AUTO_TEST_CASE(CurrentAckTransportHintHasBoundedIndependentParsing)
{
  auto value = NativeJson{{"schema", "ndnsf-di-tensor-transport-v1"},
    {"boot_epoch", "current-boot"}, {"segment_payload_bytes", std::uint64_t{7000}}};
  const auto parse = [&](const NativeJson& v, const std::string& boot = std::string{}) {
    return detail::nativeTensorTransportSegmentBytesV1(
      nativeCanonicalJson(v), ndn::MAX_NDN_PACKET_SIZE, boot);
  };
  BOOST_CHECK_EQUAL(parse(value), 7000U);
  BOOST_CHECK_EQUAL(parse(value, "current-boot"), 7000U);
  BOOST_CHECK_EQUAL(parse(value, "different-boot"), 0U);
  for (const auto& invalid : {NativeJson(0), NativeJson(-1), NativeJson(7000.0),
       NativeJson("7000"), NativeJson(true), NativeJson(nullptr),
       NativeJson(std::uint64_t{ndn::MAX_NDN_PACKET_SIZE} + 1)}) {
    auto malformed = value; malformed["segment_payload_bytes"] = invalid;
    BOOST_CHECK_EQUAL(parse(malformed), 0U);
  }
  for (const auto* field : {"schema", "boot_epoch", "segment_payload_bytes"}) {
    auto missing = value; missing.erase(field);
    BOOST_CHECK_EQUAL(parse(missing), 0U);
  }
  value["boot_epoch"] = ""; BOOST_CHECK_EQUAL(parse(value), 0U);
  value["boot_epoch"] = "current-boot";
  value["extra"] = 1; BOOST_CHECK_EQUAL(parse(value), 0U);
  BOOST_CHECK_EQUAL(detail::nativeTensorTransportSegmentBytesV1("not-json", 8800), 0U);
  BOOST_CHECK_EQUAL(detail::nativeTensorTransportSegmentBytesV1(std::string(257, 'x'), 8800), 0U);
}

BOOST_AUTO_TEST_CASE(InjectedV3PolicyUsesAdmittedOffersAndIndependentValidation)
{
  const auto f = oracle();
  Input input(f, f.at("seal_cases")[0]);
  const std::shared_ptr<const NativePlacementStrategy> strategy =
    std::make_shared<PreferProvider>("/provider/b");
  const auto proposal = strategy->proposeRoles(input.context, input.ackDigest, input.roles, input.offers, 200);
  BOOST_REQUIRE_EQUAL(proposal.providerByRole.size(), 1);
  BOOST_CHECK_EQUAL(proposal.providerByRole.begin()->second, "/provider/b");
  BOOST_CHECK_EQUAL(proposal.strategy.name, strategy->identity().name);
  BOOST_CHECK_EQUAL(proposal.strategy.configurationDigest, nativePlanningDigest("/provider/b"));
  BOOST_CHECK_NO_THROW(validateNativeRolePlacement(proposal, input.roles, input.offers, 200));
  auto changed = proposal;
  changed.offerDigestByProvider.begin()->second = nativePlanningDigest("foreign");
  BOOST_CHECK_THROW(validateNativeRolePlacement(changed, input.roles, input.offers, 200), std::invalid_argument);
  BOOST_CHECK_THROW(strategy->proposeRoles(input.context, input.ackDigest, input.roles, input.offers,
    input.context.deadlineMs), std::invalid_argument);
}
BOOST_AUTO_TEST_CASE(RequestPlannerComposesAuthenticatedGrantsAndCoreAssignments)
{
  const auto f = oracle();
  const auto sample = std::find_if(f.at("seal_cases").begin(), f.at("seal_cases").end(),
    [](const auto& value) { return value.at("name") == "cpu"; });
  BOOST_REQUIRE(sample != f.at("seal_cases").end());
  Input input(f, *sample);
  class Splitter final : public NativeModelSplitStrategy {
  public:
    explicit Splitter(NativeSplitCandidate candidate) : value(std::move(candidate)) {}
    NativeStrategyIdentity identity() const override { return value.splitter; }
    std::vector<NativeSplitCandidate> enumerate(const NativeModelDescriptor&, const NativeGraphSnapshot&,
        const NativeCandidateBudget&) const override { return {value}; }
    NativeSplitCandidate value;
  } splitter(input.split);
  unsigned publications = 0, grantPublications = 0;
  std::function<void()> publicationHook;
  NativeArtifactBinding binding;
  binding.manifestDigest = input.inspected.modelManifestDigest;
  binding.recipeDigest = input.roles.front().recipeDigest;
  for (const auto& role : input.roles) {
    binding.sourceByRole[role.selectedRole] = "/catalog/root";
    binding.artifactNameByRole[role.selectedRole] = "/catalog/artifact";
    binding.artifactDigestByRole[role.selectedRole] = role.artifactDigest;
  }
  auto registry = std::make_shared<NativeAdapterRegistry>();
  registry->freeze();
  NativeRequestPreparation preparation(registry, {},
    [&](const auto&, const auto&, const auto&, const auto&) {
      ++publications;
      if (publicationHook) publicationHook();
      return binding;
    },
    [&](const auto&, const auto&, const auto&) { return input.roles; });
  NativeOfferAdmission admission(f.at("policy").dump(), {{f.at("key_id"), f.at("public_pem")}}, f.at("candidate"));
  const auto key = [](char c) {
    const std::string bytes(32, c);
    return std::shared_ptr<EVP_PKEY>(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
      reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()), EVP_PKEY_free);
  };
  NativeGrantIssuerConfig issuerConfig;
  issuerConfig.authorityIdentity = "/authority"; issuerConfig.requesterIdentity = "/requester";
  issuerConfig.protectionEpoch = input.roles.front().protectionEpoch;
  issuerConfig.keyId = "fixture-key"; issuerConfig.authorityPrivateKey = key('a');
  issuerConfig.requesterPublicKey = key('b'); issuerConfig.allowedModelManifests = {binding.manifestDigest};
  for (const auto& offer : input.offers) issuerConfig.recipientPublicKeys[offer.observation().provider] = key('c');
  issuerConfig.contentKey = [](const auto&, const auto&) { return std::vector<std::uint8_t>(32, 42); };
  std::string authorityPublic(32, '\0'); std::size_t publicSize = authorityPublic.size();
  BOOST_REQUIRE_EQUAL(EVP_PKEY_get_raw_public_key(issuerConfig.authorityPrivateKey.get(),
    reinterpret_cast<unsigned char*>(authorityPublic.data()), &publicSize), 1);
  NativeRequestRuntime runtime;
  runtime.contract = {input.context.serviceName, "task", input.inspected.descriptor.adapterId,
    input.inspected.descriptor.adapter.descriptorDigest(), nativePlanningDigest("composition"), nativePlanningDigest("task")};
  runtime.requesterIdentity = "/requester"; runtime.protectionEpoch = issuerConfig.protectionEpoch;
  runtime.security = {nativePlanningDigest("runtime-policy"), true};
  runtime.budget.maxPolicyMs = 1000;
  runtime.grants = std::make_shared<NativeAuthenticatedGrantClient>("/requester", key('b'), "/authority",
    authorityPublic, std::make_shared<NativeArtifactGrantIssuer>(issuerConfig),
    [&](const auto& name, const auto&, const auto&) { ++grantPublications; return name; });
  NativeApplicationInput application;
  application.taskName = "task"; application.payload = {1};
  application.inputSchemaDigest = input.inspected.descriptor.adapter.inputSchemaDigest;
  application.optionsSchemaDigest = input.inspected.descriptor.adapter.optionsSchemaDigest;
  const auto encoded = encodeNativeRequestEnvelope(input.inspected.descriptor, application, runtime.contract,
    input.context.requestId, 1, input.context.deadlineMs);
  ndn_service_framework::CollaborationAckClosure closure;
  closure.requestId = ndn::Name(input.context.requestId); closure.candidates = input.acks; closure.digest = input.ackDigest;
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  NativeRequestControl control{input.context.requestId, 1, std::chrono::steady_clock::now() + std::chrono::seconds(30),
    [cancelled] { return cancelled->load(); }};
  const auto planned = planNativeRequest(runtime, {}, input.inspected, encoded, splitter,
    NativePreSplitFirstPlacement(), preparation, admission, closure, control, input.context.deadlineMs, cancelled);
  BOOST_CHECK_EQUAL(publications, 1U);
  BOOST_CHECK_EQUAL(grantPublications, 1U);
  BOOST_REQUIRE_EQUAL(planned.corePlan.roles.size(), 1U);
  BOOST_CHECK(planned.corePlan.roles.front().terminalResponseOwner);
  BOOST_CHECK_EQUAL(planned.sealed.core.modelDigest, encoded.modelIntentDigest);
  BOOST_CHECK(planned.sealed.core.modelDigest != planned.sealed.core.sourceContentDigest);
  BOOST_REQUIRE(planned.sealed.core.fixedCoreOwner);
  BOOST_CHECK(planned.sealed.core.assemblyByRole.empty());
  BOOST_CHECK_EQUAL(runtime.fixedRoleAssignments->placementCore(runtime.requesterIdentity, runtime.contract.serviceName).get(),
                    planned.sealed.core.fixedCoreOwner.get());
  const auto selected = planned.corePlan.participantSelector->select(closure.candidates, planned.corePlan.roles);
  BOOST_REQUIRE_EQUAL(selected.size(), 1U);
  BOOST_CHECK_EQUAL(selected.front().provider.toUri(), planned.terminalProvider);
  BOOST_CHECK_EQUAL(selected.front().artifactDataName.toUri(), "/catalog/root");
  BOOST_CHECK(!selected.front().assignmentPayload.empty());

  NativeConversationTurn leaseTurn;
  leaseTurn.parent.conversationId = "conversation-grant-lease-1";
  leaseTurn.parent.parentContextEpoch = 0;
  leaseTurn.parent.serviceName = input.context.serviceName;
  leaseTurn.parent.requestContractDigest = encoded.requestContractDigest;
  leaseTurn.parent.retentionDeadlineMs = 2'000'000'060'000ULL;
  leaseTurn.successorContextEpoch = 1;
  leaseTurn.requestId = input.context.requestId;
  leaseTurn.executionRequestId = input.context.requestId;
  leaseTurn.attempt = 1;
  const auto leaseFirst = planNativeRequest(runtime, {}, input.inspected, encoded, splitter,
    NativePreSplitFirstPlacement(), preparation, admission, closure, control,
    input.context.deadlineMs, cancelled, &leaseTurn);
  BOOST_REQUIRE(leaseFirst.grantLease.has_value());
  BOOST_REQUIRE(leaseFirst.grantLease->fixedContext);
  BOOST_REQUIRE(leaseFirst.grantLease->projectionLayout);
  auto grantPublicationsAfterFirstLease = grantPublications;
  leaseTurn.grantLease = leaseFirst.grantLease;
  const auto leaseSecond = planNativeRequest(runtime, {}, input.inspected, encoded, splitter,
    NativePreSplitFirstPlacement(), preparation, admission, closure, control,
    input.context.deadlineMs, cancelled, &leaseTurn);
  BOOST_REQUIRE(leaseSecond.grantLease.has_value());
  BOOST_CHECK(leaseSecond.sealed.core.reusedFixedCore);
  BOOST_CHECK_EQUAL(leaseSecond.sealed.core.fixedCoreOwner.get(), leaseFirst.sealed.core.fixedCoreOwner.get());
  BOOST_CHECK_EQUAL(&leaseSecond.sealed.core.roleContracts(), &leaseFirst.sealed.core.roleContracts());
  BOOST_CHECK(leaseSecond.grantLease->fixedContext == leaseFirst.grantLease->fixedContext);
  BOOST_CHECK_EQUAL(leaseSecond.grantLease->scope.scopeDigest, leaseFirst.grantLease->scope.scopeDigest);
  BOOST_CHECK(leaseSecond.grantLease->projectionLayout == leaseFirst.grantLease->projectionLayout);
  BOOST_CHECK_EQUAL(grantPublications, grantPublicationsAfterFirstLease);
  BOOST_CHECK_EQUAL(leaseSecond.sealed.grants.size(), leaseFirst.sealed.grants.size());

  // An extension can replace the shared cache during publication. This
  // request keeps the immutable owner pinned by its preflight validation.
  unsigned cacheClearsDuringPublication = 0;
  publicationHook = [&] {
    ++cacheClearsDuringPublication;
    runtime.fixedRoleAssignments->clear();
    BOOST_CHECK(!runtime.fixedRoleAssignments->placementCore(
      runtime.requesterIdentity, runtime.contract.serviceName));
  };
  const auto cacheClearedDuringPublication = planNativeRequest(runtime, {}, input.inspected, encoded, splitter,
    NativePreSplitFirstPlacement(), preparation, admission, closure, control,
    input.context.deadlineMs, cancelled, &leaseTurn);
  publicationHook = {};
  BOOST_REQUIRE_EQUAL(cacheClearsDuringPublication, 1U);
  BOOST_CHECK(cacheClearedDuringPublication.sealed.core.reusedFixedCore);
  BOOST_CHECK_EQUAL(cacheClearedDuringPublication.sealed.core.fixedCoreOwner.get(),
                    leaseSecond.sealed.core.fixedCoreOwner.get());
  BOOST_CHECK_EQUAL(grantPublications, grantPublicationsAfterFirstLease);

  auto missingScopeOwner = leaseTurn;
  missingScopeOwner.grantLease->fixedContext.reset();
  const auto upgradedLease = planNativeRequest(runtime, {}, input.inspected, encoded, splitter,
    NativePreSplitFirstPlacement(), preparation, admission, closure, control,
    input.context.deadlineMs, cancelled, &missingScopeOwner);
  BOOST_REQUIRE(upgradedLease.grantLease);
  BOOST_REQUIRE(upgradedLease.grantLease->fixedContext);
  BOOST_CHECK(upgradedLease.grantLease->fixedContext != leaseFirst.grantLease->fixedContext);
  BOOST_CHECK_EQUAL(upgradedLease.grantLease->scope.scopeDigest, leaseFirst.grantLease->scope.scopeDigest);
  BOOST_CHECK_EQUAL(grantPublications, grantPublicationsAfterFirstLease);

  auto changedPublicScope = leaseTurn;
  changedPublicScope.grantLease->scope.scopeDigest = "sha256:" + std::string(64, 'e');
  const auto repairedLease = planNativeRequest(runtime, {}, input.inspected, encoded, splitter,
    NativePreSplitFirstPlacement(), preparation, admission, closure, control,
    input.context.deadlineMs, cancelled, &changedPublicScope);
  BOOST_REQUIRE(repairedLease.grantLease);
  BOOST_REQUIRE(repairedLease.grantLease->fixedContext);
  BOOST_CHECK(repairedLease.grantLease->fixedContext != leaseFirst.grantLease->fixedContext);
  BOOST_CHECK_EQUAL(repairedLease.grantLease->scope.scopeDigest, leaseFirst.grantLease->scope.scopeDigest);
  BOOST_CHECK_EQUAL(grantPublications, grantPublicationsAfterFirstLease + 1);

  auto changedConversation = leaseTurn;
  changedConversation.parent.conversationId = "conversation-grant-lease-new-scope";
  const auto independentLease = planNativeRequest(runtime, {}, input.inspected, encoded, splitter,
    NativePreSplitFirstPlacement(), preparation, admission, closure, control,
    input.context.deadlineMs, cancelled, &changedConversation);
  BOOST_REQUIRE(independentLease.grantLease);
  BOOST_CHECK(independentLease.grantLease->fixedContext != leaseFirst.grantLease->fixedContext);
  BOOST_CHECK_NE(independentLease.grantLease->scope.scopeDigest, leaseFirst.grantLease->scope.scopeDigest);
  BOOST_CHECK_EQUAL(grantPublications, grantPublicationsAfterFirstLease + 2);
  grantPublicationsAfterFirstLease = grantPublications;

  // Exercise real planner forwarding before publication. The probe deliberately
  // stops at the strategy boundary; it is not a successful inference fixture.
  struct PreferenceObserved {};
  class PreferenceProbe final : public NativePlacementStrategy {
  public:
    std::map<std::string, std::string> expected;
    NativeStrategyIdentity identity() const override { return NativePreSplitFirstPlacement().identity(); }
    NativeRolePlacementProposalV3 proposeRoles(const NativeOfferBindingContext& context,
      const std::string&, const std::vector<NativeSelectionRoleV3>&,
      const std::vector<NativeAdmittedOfferV3>&, std::uint64_t) const override
    {
      BOOST_CHECK(context.preferredProvidersByRole == expected);
      throw PreferenceObserved{};
    }
  } probe;
  // A complete owner-side continuation reuses the sealed assignment. The
  // controlled parent receipt map exercises projection binding; runtime
  // receipt authentication is covered by the Conversation integration path.
  auto pinned = leaseTurn;
  pinned.parent.parentContextEpoch = 1;
  pinned.successorContextEpoch = 2;
  pinned.providersByRole = leaseFirst.sealed.core.assignment.providerByRole;
  pinned.parent.expectedRoles = leaseFirst.sealed.core.executionPlan.roles;
  NativeJson roleMap = NativeJson::array();
  NativeJson receipts = NativeJson::object();
  for (const auto& [role, provider] : pinned.providersByRole) {
    roleMap.push_back(NativeJson::array({role, provider}));
    receipts[role] = input.inspected.modelManifestDigest;
  }
  pinned.parent.planRoleMapDigest = nativePlanningDigest(nativeCanonicalJson(roleMap));
  pinned.parent.parentCheckpointDigest = input.inspected.modelManifestDigest;
  pinned.parent.parentCheckpointWire = nativeCanonicalJson(
    NativeJson{{"roleReceiptDigests", receipts}});
  probe.expected = pinned.providersByRole;
  for (auto& ack : closure.candidates) {
    auto fields = ack.ack.hasProviderCapabilityOffer()
      ? ack.ack.getProviderCapabilityOffer() : ndn_service_framework::ProviderCapabilityOffer{};
    BOOST_REQUIRE(!fields.hasField(NATIVE_PROVIDER_OFFER_V4_FIELD));
    fields.setField(NATIVE_PROVIDER_TENSOR_TRANSPORT_V1_FIELD, nativeCanonicalJson(NativeJson{
      {"schema", "ndnsf-di-tensor-transport-v1"}, {"boot_epoch", "current-provider-boot"},
      {"segment_payload_bytes", std::uint64_t{7000}}}));
    ack.ack.setProviderCapabilityOffer(fields);
  }
  const auto reused = planNativeRequest(runtime, {}, input.inspected, encoded, splitter,
    probe, preparation, admission, closure, control, input.context.deadlineMs, cancelled, &pinned);
  BOOST_CHECK(reused.sealed.core.assignment.providerByRole == pinned.providersByRole);
  BOOST_REQUIRE(reused.grantLease.has_value());
  BOOST_CHECK(reused.grantLease->fixedContext == pinned.grantLease->fixedContext);
  BOOST_CHECK(reused.grantLease->projectionLayout == pinned.grantLease->projectionLayout);
  BOOST_CHECK(!reused.placementProbeIntent);
  BOOST_CHECK_EQUAL(grantPublications, grantPublicationsAfterFirstLease);
  const auto successfulPublications = publications;
  const auto successfulGrantPublications = grantPublications;
  const auto leaseExpiry = pinned.grantLease->scope.expiresAtMs;
  pinned.grantLease->scope.expiresAtMs = input.context.deadlineMs;
  BOOST_CHECK_THROW(planNativeRequest(runtime, {}, input.inspected, encoded, splitter,
    probe, preparation, admission, closure, control, input.context.deadlineMs, cancelled,
    &pinned), PreferenceObserved);
  pinned.grantLease->scope.expiresAtMs = leaseExpiry;
  const auto roleMapDigest = pinned.parent.planRoleMapDigest;
  pinned.parent.planRoleMapDigest = input.inspected.modelManifestDigest;
  BOOST_CHECK_THROW(planNativeRequest(runtime, {}, input.inspected, encoded, splitter,
    probe, preparation, admission, closure, control, input.context.deadlineMs, cancelled,
    &pinned), PreferenceObserved);
  pinned.parent.planRoleMapDigest = roleMapDigest;
  pinned.attempt = 2;
  probe.expected.clear();
  BOOST_CHECK_THROW(planNativeRequest(runtime, {}, input.inspected, encoded, splitter,
    probe, preparation, admission, closure, control, input.context.deadlineMs, cancelled,
    &pinned), PreferenceObserved);
  NativeConversationTurn continuation;
  continuation.parent.parentContextEpoch = 1;
  continuation.providersByRole = {{input.roles.front().role, "/provider/b"}};
  probe.expected = continuation.providersByRole;
  BOOST_CHECK_THROW(planNativeRequest(runtime, {}, input.inspected, encoded, splitter,
    probe, preparation, admission, closure, control, input.context.deadlineMs, cancelled,
    &continuation), PreferenceObserved);
  continuation.attempt = 2;
  probe.expected.clear();
  BOOST_CHECK_THROW(planNativeRequest(runtime, {}, input.inspected, encoded, splitter,
    probe, preparation, admission, closure, control, input.context.deadlineMs, cancelled,
    &continuation), PreferenceObserved);
  continuation.attempt = 1;
  continuation.parent.parentContextEpoch = 0;
  BOOST_CHECK_THROW(planNativeRequest(runtime, {}, input.inspected, encoded, splitter,
    probe, preparation, admission, closure, control, input.context.deadlineMs, cancelled,
    &continuation), PreferenceObserved);
  cancelled->store(true);
  BOOST_CHECK_THROW(planNativeRequest(runtime, {}, input.inspected, encoded, splitter,
    NativePreSplitFirstPlacement(), preparation, admission, closure, control, input.context.deadlineMs, cancelled), std::runtime_error);
  BOOST_CHECK_EQUAL(publications, successfulPublications);
  BOOST_CHECK_EQUAL(grantPublications, successfulGrantPublications);
}

BOOST_AUTO_TEST_CASE(RequestPlannerAcquiresIndependentRoleGrantsConcurrently)
{
  const auto f = oracle();
  const auto sample = std::find_if(f.at("seal_cases").begin(), f.at("seal_cases").end(),
    [](const auto& value) { return value.at("name") == "rank_cover"; });
  BOOST_REQUIRE(sample != f.at("seal_cases").end());
  Input input(f, *sample);
  BOOST_REQUIRE_EQUAL(input.roles.size(), 2U);
  auto preparedRoles = input.roles;
  for (auto& role : preparedRoles)
    role.selectedRole = role.role + "#" + std::to_string(role.rank);

  class Splitter final : public NativeModelSplitStrategy {
  public:
    explicit Splitter(NativeSplitCandidate candidate) : value(std::move(candidate)) {}
    NativeStrategyIdentity identity() const override { return value.splitter; }
    std::vector<NativeSplitCandidate> enumerate(const NativeModelDescriptor&, const NativeGraphSnapshot&,
        const NativeCandidateBudget&) const override { return {value}; }
    NativeSplitCandidate value;
  } splitter(input.split);

  NativeArtifactBinding binding;
  binding.manifestDigest = input.inspected.modelManifestDigest;
  binding.recipeDigest = preparedRoles.front().recipeDigest;
  for (const auto& role : preparedRoles) {
    binding.sourceByRole[role.selectedRole] = "/catalog/root";
    binding.artifactNameByRole[role.selectedRole] = "/catalog/artifact";
    binding.artifactDigestByRole[role.selectedRole] = role.artifactDigest;
  }
  auto registry = std::make_shared<NativeAdapterRegistry>();
  registry->freeze();
  NativeRequestPreparation preparation(registry, {},
    [&](const auto&, const auto&, const auto& roles, const auto&) {
      auto result = binding;
      result.sourceByRole.clear();
      result.artifactNameByRole.clear();
      result.artifactDigestByRole.clear();
      for (const auto& role : roles) {
        result.sourceByRole[role.selectedRole] = "/catalog/root";
        result.artifactNameByRole[role.selectedRole] = "/catalog/artifact";
        result.artifactDigestByRole[role.selectedRole] = role.artifactDigest;
      }
      return result;
    },
    [&](const auto&, const auto&, const auto&) { return preparedRoles; });
  NativeOfferAdmission admission(f.at("policy").dump(), {{f.at("key_id"), f.at("public_pem")}},
                                 f.at("candidate"));
  const auto key = [](char c) {
    const std::string bytes(32, c);
    return std::shared_ptr<EVP_PKEY>(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
      reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()), EVP_PKEY_free);
  };
  NativeGrantIssuerConfig issuerConfig;
  issuerConfig.authorityIdentity = "/authority";
  issuerConfig.requesterIdentity = "/requester";
  issuerConfig.protectionEpoch = input.roles.front().protectionEpoch;
  issuerConfig.keyId = "fixture-key";
  issuerConfig.authorityPrivateKey = key('a');
  issuerConfig.requesterPublicKey = key('b');
  issuerConfig.allowedModelManifests = {binding.manifestDigest};
  for (const auto& offer : input.offers)
    issuerConfig.recipientPublicKeys[offer.observation().provider] = key('c');
  issuerConfig.contentKey = [](const auto&, const auto&) {
    return std::vector<std::uint8_t>(32, 42);
  };
  std::string authorityPublic(32, '\0');
  std::size_t publicSize = authorityPublic.size();
  BOOST_REQUIRE_EQUAL(EVP_PKEY_get_raw_public_key(issuerConfig.authorityPrivateKey.get(),
    reinterpret_cast<unsigned char*>(authorityPublic.data()), &publicSize), 1);
  auto issuer = std::make_shared<NativeArtifactGrantIssuer>(issuerConfig);
  std::atomic<unsigned> activeIssues{0};
  std::atomic<unsigned> peakIssues{0};
  std::atomic<unsigned> issueCalls{0};
  const auto updatePeak = [&peakIssues](unsigned value) {
    auto observed = peakIssues.load(std::memory_order_relaxed);
    while (observed < value &&
           !peakIssues.compare_exchange_weak(observed, value, std::memory_order_relaxed)) {}
  };
  NativeAuthenticatedGrantClient::Issue issue =
    [issuer, &activeIssues, &issueCalls, updatePeak]
    (const auto& request, const auto& manifest, std::uint64_t expiresAtMs,
     const auto&) {
      ++issueCalls;
      const auto active = activeIssues.fetch_add(1) + 1;
      updatePeak(active);
      struct Guard {
        std::atomic<unsigned>& active;
        ~Guard() { active.fetch_sub(1); }
      } guard{activeIssues};
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
      return issuer->issue(request, request.issuedAtMs, expiresAtMs, manifest);
    };
  std::atomic<unsigned> grantPublications{0};
  auto grants = std::make_shared<NativeAuthenticatedGrantClient>(
    "/requester", key('b'), "/authority", authorityPublic,
    issuerConfig.protectionEpoch, std::move(issue),
    [&grantPublications](const auto& name, const auto&, const auto&) {
      ++grantPublications;
      return name;
    });
  NativeRequestRuntime runtime;
  runtime.contract = {input.context.serviceName, "task", input.inspected.descriptor.adapterId,
    input.inspected.descriptor.adapter.descriptorDigest(), nativePlanningDigest("composition"),
    nativePlanningDigest("task")};
  runtime.requesterIdentity = "/requester";
  runtime.protectionEpoch = issuerConfig.protectionEpoch;
  runtime.security = {nativePlanningDigest("runtime-policy"), true};
  runtime.budget.maxPolicyMs = 1000;
  runtime.grants = std::move(grants);
  NativeApplicationInput application;
  application.taskName = "task";
  application.payload = {1};
  application.inputSchemaDigest = input.inspected.descriptor.adapter.inputSchemaDigest;
  application.optionsSchemaDigest = input.inspected.descriptor.adapter.optionsSchemaDigest;
  const auto encoded = encodeNativeRequestEnvelope(input.inspected.descriptor, application,
    runtime.contract, input.context.requestId, 1, input.context.deadlineMs);
  ndn_service_framework::CollaborationAckClosure closure;
  closure.requestId = ndn::Name(input.context.requestId);
  closure.candidates = input.acks;
  closure.digest = input.ackDigest;
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  NativeRequestControl control{input.context.requestId, 1,
    std::chrono::steady_clock::now() + std::chrono::seconds(30),
    [cancelled] { return cancelled->load(); }};

  std::string rejection;
  try {
    (void)planNativeRequest(runtime, {}, input.inspected, encoded, splitter,
      NativePreSplitFirstPlacement(), preparation, admission, closure, control,
      input.context.deadlineMs, cancelled);
  }
  catch (const std::exception& error) {
    rejection = error.what();
  }
  BOOST_CHECK_EQUAL(rejection, "projection has no unique terminal response owner");
  BOOST_CHECK_EQUAL(issueCalls.load(), input.roles.size());
  BOOST_CHECK_EQUAL(grantPublications.load(), input.roles.size());
  BOOST_CHECK_GE(peakIssues.load(), 2U);
}

void runPublicClientScenario(int scenario, bool spoofTargetedSigner,
                             bool timeoutTargetedMeasurement = false,
                             bool cancelAfterTargetedMeasurement = false,
                             bool spoofMeasurementSigner = false,
                             bool verifyLateBootstrapRelease = false,
                             bool timeoutCacheQuery = false,
                             bool cancelDuringCacheQuery = false)
{
  using namespace ndn_service_framework;
  class User final : public test::LocalServiceUser {
  public:
    using test::LocalServiceUser::LocalServiceUser;
    std::map<std::string, SelectionInputKeyOffer> groupKeys;
    std::map<std::string, std::string> providerBootEpochs;
    void seedAckTelemetry(const std::string& provider, const std::string& bootEpoch)
    {
      NetworkTelemetryKey key;
      key.providerName = ndn::Name(provider);
      key.serviceName = ndn::Name("/service");
      key.kind = NetworkTelemetrySampleKind::AckRtt;
      NetworkTelemetrySample sample;
      sample.kind = NetworkTelemetrySampleKind::AckRtt;
      sample.rttMs = 5.0;
      sample.elapsedMs = 5.0;
      sample.peerBootEpoch = bootEpoch;
      m_networkTelemetry.update(key, sample);
    }
    void offer(const ndn::Name& id, const std::string& wire,
               const std::string& v4Capability = {}, bool closeAckWindow = true) {
      auto& call = m_pendingCalls.at(id);
      RequestAckMessage ack;
      ack.setStatus(true); ack.setUserToken(call.requestMessage.getUserToken());
      ack.setProviderToken("provider-token");
      ndn::Buffer bytes(wire.begin(), wire.end()); ack.setPayload(bytes, bytes.size());
      if (!v4Capability.empty()) {
        ProviderCapabilityOffer capabilities;
        capabilities.setField(NATIVE_PROVIDER_OFFER_V4_FIELD, v4Capability);
        ack.setProviderCapabilityOffer(capabilities);
      }
      const auto decoded = decodeNativeProviderOfferV3(wire);
      providerBootEpochs[decoded.provider] = decoded.bootEpoch;
      seedAckTelemetry(decoded.provider, v4Capability.empty() ? decoded.bootEpoch :
                       "stale-boot-for-" + decoded.provider);
      if (groupKeys.count(decoded.provider)) ack.setSelectionInputKeyOffer(groupKeys.at(decoded.provider));
      // This supplies the Core authentication boundary as a local fixture.
      // Offer admission still verifies the actual Ed25519 signature below.
      call.requestAcks.push_back({ndn::Name(decoded.provider), call.serviceName, id, ack,
        {decoded.provider, decoded.provider + "/KEY/fixture/issuer/v=1", nativePlanningDigest(wire), true},
        v4Capability.empty() ? std::nullopt : std::optional<double>{5.0},
        v4Capability.empty() ? 0 : static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count())});
      call.providerTokens[decoded.provider] = "provider-token";
      if (closeAckWindow)
        closeOffers(id);
    }
    void closeOffers(const ndn::Name& id) {
      auto& call = m_pendingCalls.at(id);
      call.ackWindowExpired = true;
      closeDeferredCollaborationAcks(id, call);
    }
    bool committed(const ndn::Name& id) const {
      const auto it = m_pendingCalls.find(id);
      return it != m_pendingCalls.end() && it->second.collaborationPlanCommitted;
    }
    ndn::Name terminalResponseProvider(const ndn::Name& id) const {
      const auto it = m_pendingCalls.find(id);
      if (it == m_pendingCalls.end()) return {};
      for (const auto& role : it->second.collaborationPlan.roles) {
        if (!role.terminalResponseOwner) continue;
        const auto participant = std::find_if(
          it->second.collaborationCommittedParticipants.begin(),
          it->second.collaborationCommittedParticipants.end(),
          [&role](const auto& value) { return value.role == role.role; });
        if (participant != it->second.collaborationCommittedParticipants.end())
          return ndn::Name(participant->provider);
      }
      return {};
    }
    ResponseHandler responseCallback(const ndn::Name& id) { return m_pendingCalls.at(id).responseHandler; }
    TimeoutHandler timeoutCallback(const ndn::Name& id) { return m_pendingCalls.at(id).timeoutHandler; }
    auto streamCallbacks(const ndn::Name& id) { return m_streamStates.at(id); }
    std::vector<ndn::Name> pendingIds() const {
      std::vector<ndn::Name> result;
      for (const auto& pair : m_pendingCalls) result.push_back(pair.first);
      return result;
    }
    std::vector<std::pair<ndn::Name, ndn::Name>> pendingTargetedCalls() const {
      std::vector<std::pair<ndn::Name, ndn::Name>> result;
      for (const auto& pair : m_pendingCalls) {
        const auto provider = pair.second.requestMessage.getTargetProvider();
        if (!provider.empty()) result.emplace_back(pair.first, provider);
      }
      return result;
    }
    std::string requestWire(const ndn::Name& id) {
      const auto bytes = m_pendingCalls.at(id).requestMessage.getPayload();
      return std::string(bytes.begin(), bytes.end());
    }
    void deliver(const ndn::Name& id, const ndn::Name& provider, const ResponseMessage& response) {
      handleResponse(id, provider, response);
    }
    void seedConversationScope(const ndn::Name& id, const std::string& scope,
                               const ndn::Buffer& key) {
      std::lock_guard<std::mutex> lock(m_verifiedCollaborationMutex);
      m_userCollaborationScopeKeys[id][scope] = key;
    }
    void seedConversationData(VerifiedCollaborationData data) {
      std::lock_guard<std::mutex> lock(m_verifiedCollaborationMutex);
      m_verifiedCollaborationData[data.requestId].push_back(std::move(data));
      m_verifiedCollaborationCv.notify_all();
    }
    std::string assignmentField(const ndn::Name& id, const ndn::Name& provider,
                                const char* field) const {
      auto payload = getSelectionAssignmentPayloadForTest(id, provider);
      if (payload.empty()) {
        const auto pending = m_pendingCalls.find(id);
        if (pending != m_pendingCalls.end()) {
          for (const auto& participant : pending->second.collaborationCommittedParticipants) {
            if (participant.provider == provider) {
              const auto role = std::find_if(pending->second.collaborationPlan.roles.begin(),
                pending->second.collaborationPlan.roles.end(),
                [&](const auto& value) { return value.role == participant.role; });
              if (role != pending->second.collaborationPlan.roles.end()) {
                payload = role->assignmentPayload;
                break;
              }
            }
          }
        }
      }
      BOOST_REQUIRE(!payload.empty());
      const auto value = nativeParseJson(std::string(payload.begin(), payload.end()));
      BOOST_REQUIRE(value.is_object());
      return value.value(field, std::string{});
    }
  };
  const auto f = oracle();
  const auto sample = std::find_if(f.at("seal_cases").begin(), f.at("seal_cases").end(),
    [](const auto& value) { return value.at("name") == "cpu"; });
  BOOST_REQUIRE(sample != f.at("seal_cases").end());
  auto probeSample = std::find_if(f.at("seal_cases").begin(), f.at("seal_cases").end(),
    [](const auto& value) { return value.at("name") == "rank_cover"; });
  if (scenario == 16)
  BOOST_REQUIRE(probeSample != f.at("seal_cases").end());
  auto ownedInput = std::make_shared<Input>(f,
    scenario == 16 ? *probeSample : *sample);
  if (scenario == 16)
    makeInterRoleProbeInput(*ownedInput);
  const auto& input = *ownedInput;
  class Splitter final : public NativeModelSplitStrategy {
  public:
    explicit Splitter(NativeSplitCandidate candidate) : value(std::move(candidate)) {}
    NativeStrategyIdentity identity() const override { return value.splitter; }
    std::vector<NativeSplitCandidate> enumerate(const NativeModelDescriptor&, const NativeGraphSnapshot&,
      const NativeCandidateBudget&) const override { return {value}; }
    NativeSplitCandidate value;
  };
  class ProbeOncePlacement final : public NativePlacementStrategy {
  public:
    explicit ProbeOncePlacement(bool invalidIntent, bool useAllowedRttPair = false,
                                bool useObservationAwareStrategy = false)
      : invalidIntent(invalidIntent), useAllowedRttPair(useAllowedRttPair),
        useObservationAwareStrategy(useObservationAwareStrategy)
    {}
    NativeStrategyIdentity identity() const override
    {
      return useObservationAwareStrategy ? observationAware.identity() : baseline.identity();
    }
    NativeRolePlacementProposalV3 proposeRoles(
      const NativeOfferBindingContext& context, const std::string& ackClosedDigest,
      const std::vector<NativeSelectionRoleV3>& roles,
      const std::vector<NativeAdmittedOfferV3>& offers,
      std::uint64_t nowMs) const override
    {
      ++calls;
      auto result = useObservationAwareStrategy ?
        observationAware.proposeRoles(context, ackClosedDigest, roles, offers, nowMs) :
        baseline.proposeRoles(context, ackClosedDigest, roles, offers, nowMs);
      if (!context.placementProbeRoundCompleted) {
        firstSnapshotDigest = context.placementObservationSnapshotDigest;
        if (result.placementProbeIntent &&
            !result.placementProbeIntent->observers.empty()) {
          const auto& probeObserver = result.placementProbeIntent->observers.front();
          requestedObserver = probeObserver.observerProvider;
          if (!probeObserver.rttPairs.empty())
            requestedRttTarget = probeObserver.rttPairs.front().destinationProvider;
        }
        NativePlacementProbeObserverV1 observer;
        if (useObservationAwareStrategy) {
          // Scenario 16 exercises the production V4 strategy's own bounded
          // intent and scoring path through NativeInferenceClient.
        }
        else if (invalidIntent) {
          observer.observerProvider = "/provider/b";
          observer.goodputEdges.push_back({"/provider/a", "/provider/b"});
        }
        else if (useAllowedRttPair && !context.allowedRttPairs.empty()) {
          const auto& pair = context.allowedRttPairs.front();
          observer.observerProvider = pair.at("src_provider").get<std::string>();
          requestedRttTarget = pair.at("dst_provider").get<std::string>();
          requestedObserver = observer.observerProvider;
          observer.rttPairs.push_back(
            {observer.observerProvider, requestedRttTarget});
        }
        else {
          observer.observerProvider = "/provider/b";
          requestedRttTarget = "/provider/a";
          requestedObserver = observer.observerProvider;
          observer.rttPairs.push_back({"/provider/b", "/provider/a"});
        }
        if (!useObservationAwareStrategy) {
          NativePlacementProbeIntentV1 intent;
          intent.observers.push_back(std::move(observer));
          result.roles.clear();
          result.providerByRole.clear();
          result.offerDigestByProvider.clear();
          result.placementProbeIntent = std::move(intent);
        }
      }
      else {
        finalSnapshotDigest = context.placementObservationSnapshotDigest;
        if (context.placementObservationSnapshot.is_object())
          finalRequesterSamples = context.placementObservationSnapshot.value(
            "requester_provider_samples", NativeJson::array());
        if (context.placementObservationSnapshot.is_object() &&
            context.placementObservationSnapshot.contains("provider_offers")) {
          finalProviderBoots = NativeJson::array();
          for (const auto& offer :
               context.placementObservationSnapshot.at("provider_offers")) {
            finalProviderBoots.push_back({
              {"provider", offer.value("provider", std::string{})},
              {"boot_epoch", offer.value("boot_epoch", std::string{})}});
          }
        }
        if (result.placementDecisionV4)
          finalDecision = *result.placementDecisionV4;
        sawProbeRound.store(true);
      }
      return result;
    }
    std::size_t callCount() const { return calls.load(); }
    bool sawTargetedRound() const { return sawProbeRound.load(); }
    const std::string& initialSnapshotDigest() const { return firstSnapshotDigest; }
    const std::string& mergedSnapshotDigest() const { return finalSnapshotDigest; }
    const std::string& rttTarget() const { return requestedRttTarget; }
    const std::string& observer() const { return requestedObserver; }
    const NativeJson& decision() const { return finalDecision; }
    const NativeJson& requesterSamples() const { return finalRequesterSamples; }
    const NativeJson& providerBoots() const { return finalProviderBoots; }
  private:
    NativePreSplitFirstPlacement baseline;
    NativeObservationAwarePlacementV4 observationAware;
    bool invalidIntent = false;
    bool useAllowedRttPair = false;
    bool useObservationAwareStrategy = false;
    mutable std::atomic<std::size_t> calls{0};
    mutable std::atomic<bool> sawProbeRound{false};
    mutable std::string firstSnapshotDigest;
    mutable std::string finalSnapshotDigest;
    mutable std::string requestedRttTarget;
    mutable std::string requestedObserver;
    mutable NativeJson finalDecision;
    mutable NativeJson finalRequesterSamples;
    mutable NativeJson finalProviderBoots;
  };
  struct BootstrapOwners {
    // Reverse member destruction keeps KeyChain alive through Face teardown.
    ndn::security::KeyChain keyChain{"pib-memory:", "tpm-memory:"};
    ndn::DummyClientFace face{keyChain};
  };
  auto owners = std::make_shared<BootstrapOwners>();
  const std::weak_ptr<BootstrapOwners> weakOwners = owners;
  auto& keyChain = owners->keyChain;
  auto& face = owners->face;
  const auto requesterCert = test::makeRsaIdentity(keyChain, ndn::Name("/requester"));
  const auto authorityCert = test::makeRsaIdentity(keyChain, ndn::Name("/authority"));
  // A derived member dies before the ServiceUser base destructor. Keep both
  // external bootstrap resources in the control-block deleter instead: they
  // survive delete (including base teardown), even on a late worker release.
  auto user = std::shared_ptr<User>(new User(face, ndn::Name("/client"),
    requesterCert, authorityCert, "examples/trust-any.conf"),
    [owners](User* value) mutable {
      delete value;
      // Weak User references may retain its control block after delete.
      // Release the bootstrap resources now, rather than forming a cycle
      // through Face callbacks that keep those weak references.
      owners.reset();
    });
  owners.reset();
  BOOST_REQUIRE(!weakOwners.expired());
  if (scenario == 16) {
    user->setRequestPublisher([] (const auto&, const auto&, const auto&,
                                 const auto&, const auto&, std::size_t) {});
  }
  // Every public-client scenario reaches Core plan commit and therefore needs
  // the same existing REQUEST-LARGE wrapped-key and policy state. Without
  // this test-only bootstrap the production path correctly tries NAC-ABE
  // wrapping, but the LocalMock fixture has no Controller/AA to answer it.
  user->prepareHybridSendKeyForTest(ndn::Name("/service"), "REQUEST-LARGE");
  if (scenario == 14 || scenario == 15 || scenario == 16) {
    PermissionResponse permissions;
    permissions.setTargetIdentity("/requester");
    permissions.setPermissionKind(tlv::UserPermission);
    permissions.setPolicyEpoch(1);
    std::vector<ndn::Name> authorizedServices{ndn::Name("/service")};
    if (scenario == 16)
      authorizedServices.emplace_back(NETWORK_PROBE_SERVICE);
    for (const auto& service : authorizedServices) {
      for (const auto* provider : {"/provider/a", "/provider/b"}) {
        PermissionEntry entry;
        entry.setProviderName(provider);
        entry.setServiceName(service.toUri());
        entry.setToken("");
        entry.setTtl(0);
        entry.setVersion(1);
        permissions.addEntry(entry);
      }
    }
    user->applyPermissionResponse(permissions);
  }
  else {
    user->applyPermissionResponse(test::makePermissionResponse(
      ndn::Name("/requester"), tlv::UserPermission,
      ndn::Name("/provider/a"), ndn::Name("/service")));
  }
  // This fixture needs the requester key for protected signing, but it does
  // not need the asynchronous NAC consumer/producer bootstrap.  The latter
  // belongs to full integration fixtures and leaves cross-scenario work on
  // the LocalMock Face.
  user->useSigningKeyChainForSigningOnlyForTest(keyChain);
  std::shared_ptr<NativeConversationCoordinator> conversations;
  std::optional<NativeConversationContinuation> conversation;
  if (scenario == 13) {
    NativeConversationConfig conversationConfig;
    conversationConfig.authenticationKeys = {std::vector<std::uint8_t>(32, 0x5a)};
    conversationConfig.requesterIdentity = "/requester";
    conversationConfig.serviceName = "/service";
    conversationConfig.securityDomainDigest = nativePlanningDigest("policy");
    conversationConfig.nowMs = [] { return std::uint64_t{2'000'000'000'000}; };
    conversations = std::make_shared<NativeConversationCoordinator>(
      std::move(conversationConfig));
    ndn::svs::SecurityOptions security(keyChain);
    security.interestSigner = std::make_shared<ndn::svs::BaseSigner>();
    security.dataSigner->signingInfo = ndn::security::signingByCertificate(requesterCert);
    security.pubSigner->signingInfo = ndn::security::signingByCertificate(requesterCert);
    security.validator = std::make_shared<ndn::svs::BaseValidator>();
    security.encapsulatedDataValidator = std::make_shared<ndn::svs::BaseValidator>();
    ndn::svs::SVSPubSubOptions svsOptions;
    svsOptions.useTimestamp = false;
    auto pubSub = std::make_shared<ndn::svs::SVSPubSub>(
      ndn::Name("/spec182/public/sync"), ndn::Name("/spec182/public/user/0"), face,
      [] (const std::vector<ndn::svs::MissingDataInfo>&) {}, svsOptions, security);
    user->attachLocalMockPubSubForTest(std::move(pubSub));
  }
  if ((scenario >= 3 && scenario <= 13) || scenario == 15 || scenario == 16) {
    for (const auto& provider : {std::string("/provider/a"), std::string("/provider/b")}) {
      const auto cert = keyChain.createIdentity(ndn::Name(provider), ndn::RsaKeyParams(2048))
        .getDefaultKey().getDefaultCertificate();
      const auto bytes = cert.getPublicKey();
      auto& value = user->groupKeys[provider];
      value.setField("schemaVersion", "1"); value.setField("recipient", provider);
      value.setField("recipientCertName", cert.getName().toUri());
      value.setField("recipientPublicKey", selectionGatedHex(bytes));
      value.setField("recipientCertDigest", nativePlanningDigest(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())));
      const auto offer = decodeNativeProviderOfferV3(sample->at("offers").front().get<std::string>());
      value.setField("providerBootEpoch", provider + ":" + offer.bootEpoch);
      value.setField("ndnsfDataV1EndpointPrefix", provider);
    }
  }
  auto registry = std::make_shared<NativeAdapterRegistry>();
  registry->registerAdapter(std::make_shared<NativeCatalogModelAdapter>(
    std::vector<NativeModelDescriptor>{input.inspected.descriptor}, NativeCatalogModelAdapter::Format::OpaqueBytes, 1024));
  registry->freeze();
  NativeArtifactBinding binding;
  binding.manifestDigest = input.inspected.modelManifestDigest; binding.recipeDigest = input.roles.front().recipeDigest;
  for (const auto& role : input.roles) {
    binding.sourceByRole[role.selectedRole] = "/catalog/root";
    binding.artifactNameByRole[role.selectedRole] = "/catalog/artifact";
    binding.artifactDigestByRole[role.selectedRole] = role.artifactDigest;
  }
  auto preparation = std::make_shared<NativeRequestPreparation>(registry,
    [ownedInput](const auto&, const auto&) { return ownedInput->inspected; },
    [binding](const auto&, const auto&, const auto&, const auto&) { return binding; },
    [ownedInput](const auto&, const auto&, const auto&) { return ownedInput->roles; });
  auto admission = std::make_shared<NativeOfferAdmission>(f.at("policy").dump(),
    std::map<std::string, std::string>{{f.at("key_id"), f.at("public_pem")}}, f.at("candidate"));
  const auto key = [](char seed) {
    const std::string bytes(32, seed);
    return std::shared_ptr<EVP_PKEY>(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
      reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()), EVP_PKEY_free);
  };
  NativeGrantIssuerConfig issuer;
  issuer.authorityIdentity = "/authority"; issuer.requesterIdentity = "/requester";
  issuer.protectionEpoch = input.roles.front().protectionEpoch; issuer.keyId = "fixture-key";
  issuer.authorityPrivateKey = key('a'); issuer.requesterPublicKey = key('b');
  issuer.allowedModelManifests = {binding.manifestDigest};
  issuer.recipientPublicKeys = {{"/provider/a", key('c')}};
  if ((scenario >= 3 && scenario <= 13) || scenario == 14 || scenario == 15 || scenario == 16)
    issuer.recipientPublicKeys.emplace("/provider/b", key('d'));
  issuer.contentKey = [](const auto&, const auto&) { return std::vector<std::uint8_t>(32, 42); };
  std::string authorityPublic(32, '\0'); std::size_t size = authorityPublic.size();
  BOOST_REQUIRE_EQUAL(EVP_PKEY_get_raw_public_key(issuer.authorityPrivateKey.get(),
    reinterpret_cast<unsigned char*>(authorityPublic.data()), &size), 1);
  NativeRequestRuntime runtime;
  runtime.contract = {"/service", "task", input.inspected.descriptor.adapterId,
    input.inspected.descriptor.adapter.descriptorDigest(), nativePlanningDigest("composition"), nativePlanningDigest("task")};
  if (scenario >= 3 && scenario <= 13) {
    runtime.contract.generationMode = "TOKEN_STREAMING";
    runtime.contract.tokenizerDigest = nativePlanningDigest("tokenizer");
  }
  runtime.requesterIdentity = "/requester"; runtime.protectionEpoch = issuer.protectionEpoch;
  runtime.security = {nativePlanningDigest("policy"), true}; runtime.budget.maxPolicyMs = 1000;
  if (scenario == 16)
    runtime.inputLayoutDigest = nativePlanningDigest("spec196-application-input-layout");
  runtime.grants = std::make_shared<NativeAuthenticatedGrantClient>("/requester", key('b'), "/authority", authorityPublic,
    std::make_shared<NativeArtifactGrantIssuer>(issuer),
    [](const auto& name, const auto&, const auto&) { return name; });
  NativeModelRef model; static_cast<NativeModelDescriptor&>(model) = input.inspected.descriptor;
  NativeApplicationInput application; application.taskName = "task"; application.payload = {1};
  application.inputSchemaDigest = model.adapter.inputSchemaDigest;
  application.optionsSchemaDigest = model.adapter.optionsSchemaDigest;
  const auto pumpUntil = [&](const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
      face.getIoContext().restart(); face.getIoContext().poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
  };
  auto splitter = std::make_shared<Splitter>(input.split);
  if (scenario == 14 || scenario == 15 || scenario == 16) {
    // Model the prepare-time cache consumed by V4 request encoding. Production
    // NativeRequestCatalog::load creates this cache before client construction.
    auto planningCache = std::make_shared<NativePreparedPlanningCache>();
    planningCache->splitter = splitter->identity();
    planningCache->candidates.push_back({input.split, input.roles});
    runtime.planningCache = std::move(planningCache);
  }
  {
    NativeInferenceClient client(user, registry, runtime, conversations, preparation, admission);
    NativeRequestOptions requestOptions;
    std::shared_ptr<ProbeOncePlacement> probePlacement;
    std::shared_ptr<const NativePlacementStrategy> placement;
    if (scenario == 14 || scenario == 15 || scenario == 16) {
      probePlacement = std::make_shared<ProbeOncePlacement>(
        scenario == 14, scenario == 16, scenario == 16);
      placement = probePlacement;
      requestOptions.providerNames = {ndn::Name("/provider/a"), ndn::Name("/provider/b")};
    }
    else {
      placement = std::make_shared<NativePreSplitFirstPlacement>();
    }
    std::atomic<unsigned> accepted{0};
    if (scenario >= 3 && scenario <= 13) {
      const auto values = nativeCanonicalJson(NativeJson{{"useCache", true}, {"outputMode", "TOKEN_STREAMING"},
        {"maxNewTokens", 4}, {"eosTokenIds", {2}}, {"tokenizerDigest", nativePlanningDigest("tokenizer")},
        {"tokenInputName", "x"}, {"stateInputNames", {"x"}}, {"stateOutputNames", {"y"}}});
      application.options.assign(values.begin(), values.end());
      requestOptions.stream = StreamRequestOptions{};
      requestOptions.stream->generationId.fill(0x11);
      requestOptions.stream->allowReplacement = true;
      requestOptions.stream->maxReplacements = 1;
      requestOptions.generation = nativeGenerationFromOptions(application.options, std::string(32, '1'));
      if (scenario == 13) {
        NativeConversationContinuation value;
        value.conversationId = "conv-spec182-public-1";
        value.parentContextEpoch = 0;
        value.serviceName = "/service";
        const auto role = input.roles.front().selectedRole;
        value.planRoleMapDigest = nativePlanningDigest(nativeCanonicalJson(
          NativeJson::array({NativeJson::array({role, "/provider/a"})})));
        value.retentionDeadlineMs = 2'000'000'060'000ULL;
        value.mode = "FULL_CONTEXT";
        value.generationId = requestOptions.generation->generationId;
        value.expectedRoles = {role};
        conversation = value;
        requestOptions.conversation = value;
      }
      requestOptions.onGenerationEvent = [&](const auto&) {
        ++accepted;
        if (scenario == 7) throw std::runtime_error("application callback fixture failure");
      };
    }
    auto handle = client.request(model, application, splitter, placement, requestOptions);
    std::shared_ptr<std::vector<NativeInferenceEvent>> observedEvents;
    std::shared_ptr<std::promise<void>> observedTerminal;
    std::future<void> observedDone;
    if (scenario >= 3 && scenario <= 13) {
      observedEvents = std::make_shared<std::vector<NativeInferenceEvent>>();
      observedTerminal = std::make_shared<std::promise<void>>();
      observedDone = observedTerminal->get_future();
      handle.observe([observedEvents, observedTerminal](const NativeInferenceEvent& event) {
        observedEvents->push_back(event);
        if (event.terminal)
          observedTerminal->set_value();
      });
    }
    const ndn::Name id(handle.requestId());
    BOOST_REQUIRE(pumpUntil([&] { return user->hasPendingCall(id) || handle.status() != NativeRequestStatus::Pending; }));
    if (handle.status() != NativeRequestStatus::Pending) handle.result(std::chrono::milliseconds(0));
    BOOST_REQUIRE(user->hasPendingCall(id));
    const auto submitOffer = [&](const ndn::Name& attemptId, const std::string& provider, std::uint64_t attempt) {
    auto offer = nativeParseJson(sample->at("offers").front().get<std::string>());
    offer["request_id"] = attemptId.toUri(); offer["provider"] = provider; offer["attempt"] = attempt;
    offer["topology"]["provider"] = provider;
    if (scenario == 16) {
      auto acceptedRoles = offer.at("accepted_roles").get<std::vector<std::string>>();
      acceptedRoles.push_back(input.roles[1].selectedRole);
      std::sort(acceptedRoles.begin(), acceptedRoles.end());
      acceptedRoles.erase(std::unique(acceptedRoles.begin(), acceptedRoles.end()),
                          acceptedRoles.end());
      offer["accepted_roles"] = std::move(acceptedRoles);
    }
    const auto unsignedWire = nativeCanonicalJson(offer);
    const auto digest = decodeNativeProviderOfferV3(unsignedWire).offerDigest;
    std::array<unsigned char, 32> seed{};
    for (std::size_t i = 0; i < seed.size(); ++i) seed[i] = i;
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> signingKey(
      EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()), EVP_PKEY_free);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> signing(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    BOOST_REQUIRE_EQUAL(EVP_DigestSignInit(signing.get(), nullptr, nullptr, nullptr, signingKey.get()), 1);
    std::array<unsigned char, 64> signature{}; std::size_t length = signature.size();
    BOOST_REQUIRE_EQUAL(EVP_DigestSign(signing.get(), signature.data(), &length,
      reinterpret_cast<const unsigned char*>(digest.data()), digest.size()), 1);
    std::array<unsigned char, 89> encoded{};
    BOOST_REQUIRE_EQUAL(EVP_EncodeBlock(encoded.data(), signature.data(), length), 88);
    offer["signature"] = std::string(reinterpret_cast<char*>(encoded.data()), 88);
    const auto wire = nativeCanonicalJson(offer);
    std::string v4Capability;
    if (scenario == 16) {
      const auto requestWire = user->requestWire(attemptId);
      const std::vector<std::uint8_t> requestPayload(requestWire.begin(), requestWire.end());
      NativeProviderOfferV4Config config;
      config.provider = provider;
      config.service = "/service";
      config.bootEpoch = offer.at("boot_epoch").get<std::string>();
      config.signerKeyId = f.at("key_id").get<std::string>();
      config.signDigest = [&signingKey](const std::string& value) {
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
          EVP_MD_CTX_new(), EVP_MD_CTX_free);
        if (!context || EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr,
                                            signingKey.get()) != 1)
          throw std::runtime_error("V4 fixture signer initialization failed");
        std::array<unsigned char, 64> signature{};
        std::size_t signatureLength = signature.size();
        std::array<unsigned char, 89> encodedSignature{};
        if (EVP_DigestSign(context.get(), signature.data(), &signatureLength,
              reinterpret_cast<const unsigned char*>(value.data()), value.size()) != 1 ||
            signatureLength != signature.size() ||
            EVP_EncodeBlock(encodedSignature.data(), signature.data(),
                            static_cast<int>(signatureLength)) != 88)
          throw std::runtime_error("V4 fixture signing failed");
        return std::string(reinterpret_cast<const char*>(encodedSignature.data()), 88);
      };
      NativeProviderTelemetrySnapshot telemetry;
      const auto nowMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
      NativeProviderResidencySnapshotV4 residency;
      residency.known = true;
      const auto requestEnvelope = nativeParseJson(requestWire);
      for (const auto& artifact : requestEnvelope.at("candidate_artifacts")) {
        auto row = artifact;
        auto backend = artifact.at("backend").get<std::string>();
        const bool hasCpuSuffix = backend.size() >= 4 &&
          backend.compare(backend.size() - 4, 4, "-cpu") == 0;
        const bool hasCudaSuffix = backend.size() >= 5 &&
          backend.compare(backend.size() - 5, 5, "-cuda") == 0;
        if (!hasCpuSuffix && !hasCudaSuffix)
          backend += "-cpu";
        row["backend"] = std::move(backend);
        row["device_set"] = NativeJson::array();
        row["state"] = "EXACT_LOADED";
        row["process_epoch"] = config.bootEpoch;
        row["captured_at_ms"] = nowMs;
        row["expires_at_ms"] = nowMs + 4000;
        row["missing_verified_bytes"] = std::uint64_t{0};
        row["estimated_assembly_ms"] = nullptr;
        row["estimated_load_ms"] = nullptr;
        row["runtime_generation"] = "spec196-fixture-loaded";
        residency.rows.push_back(std::move(row));
      }
      const auto issued = issueNativeProviderOfferV4(
        requestPayload, config, telemetry, nowMs, {}, residency);
      BOOST_REQUIRE_MESSAGE(issued, "could not issue the V4 placement fixture offer");
      v4Capability = issued->capabilityFieldValue;
    }
    const bool closeAckWindow = scenario != 16;
    user->postToIo([&, attemptId, wire, v4Capability, closeAckWindow] {
      user->offer(attemptId, wire, v4Capability, closeAckWindow);
    });
    };
    submitOffer(id, "/provider/a", 1);
    if (scenario == 14 || scenario == 15 || scenario == 16) {
      submitOffer(id, "/provider/b", 1);
    }
    if (scenario == 16) {
      user->postToIo([user, id] { user->closeOffers(id); });
    }
    if (scenario == 14) {
      BOOST_REQUIRE(pumpUntil([&] {
        return handle.status() != NativeRequestStatus::Pending;
      }));
      std::string failure;
      try { (void)handle.result(std::chrono::milliseconds(0)); }
      catch (const NativeDiError& error) { failure = error.what(); }
      BOOST_CHECK_MESSAGE(
        failure.find("placement probe intent has an invalid directed edge") !=
          std::string::npos, failure);
      BOOST_CHECK(user->pendingTargetedCalls().empty());
      BOOST_CHECK(!user->committed(id));
      return;
    }
    if (scenario == 15) {
      BOOST_REQUIRE(pumpUntil([&] {
        return handle.status() != NativeRequestStatus::Pending;
      }));
      std::string failure;
      try { (void)handle.result(std::chrono::milliseconds(0)); }
      catch (const NativeDiError& error) { failure = error.what(); }
      BOOST_CHECK_MESSAGE(
        failure.find("placement probe intent has an invalid directed edge") !=
          std::string::npos, failure);
      BOOST_CHECK(user->pendingTargetedCalls().empty());
      BOOST_CHECK(!user->committed(id));
      return;
    }
    if (scenario == 16) {
      std::set<std::string> handledTargetedCalls;
      std::vector<std::pair<std::string, std::string>> targetedActions;
      const auto answerTargetedCalls = [&] (bool deferMeasurement = false) {
        for (const auto& pending : user->pendingTargetedCalls()) {
          const auto& targetedId = pending.first;
          const auto& target = pending.second;
          const auto idText = targetedId.toUri();
          if (!handledTargetedCalls.insert(idText).second)
            continue;
          const auto request = nativeParseJson(user->requestWire(targetedId));
          const auto action = request.at("action").get<std::string>();
          targetedActions.emplace_back(action, target.toUri());
          NativeJson payload;
          if (action == "authorize") {
            payload = NativeJson{
              {"schema", "ndnsf-di-placement-probe-scope-ack-v1"},
              {"provider", target.toUri()},
              {"observer_provider", request.at("observer_provider")},
              {"scope_digest", request.at("scope_digest")},
              {"status", "authorized"}};
          }
          else {
            BOOST_REQUIRE(action == "measure" || action == "cache");
            if (action == "cache" && cancelDuringCacheQuery) continue;
            if (action == "cache" && timeoutCacheQuery) {
              user->timeoutCallback(targetedId)(targetedId);
              ResponseMessage lateUnavailable;
              lateUnavailable.setStatus(false);
              user->deliver(targetedId, target, lateUnavailable);
              continue;
            }
            if (action == "measure" && deferMeasurement)
              continue;
            if (action == "measure" && timeoutTargetedMeasurement) {
              const auto timeout = user->timeoutCallback(targetedId);
              timeout(targetedId);
              ResponseMessage lateUnavailable;
              lateUnavailable.setStatus(false);
              lateUnavailable.setErrorInfo("late fixture response after timeout");
              user->deliver(targetedId, target, lateUnavailable);
              continue;
            }
            const auto completedAtMs = static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            payload = NativeJson{
              {"schema", "ndnsf-di-placement-probe-response-v1"},
              {"request_id", request.at("request_id")},
              {"attempt", request.at("attempt")},
              {"requester_identity", request.at("requester_identity")},
              {"observer_provider", request.at("observer_provider")},
              {"service", request.at("service")},
              {"observer_boot_epoch", user->providerBootEpochs.at(target.toUri())},
              {"scope_digest", request.at("scope_digest")},
              {"intent_nonce", request.at("intent_nonce")},
              {"deadline_ms", request.at("deadline_ms")},
              {"completed_at_ms", completedAtMs},
              {"requested_rtt_count", request.at("rtt_pairs").size()},
              {"requested_goodput_count", request.at("goodput_edges").size()},
              {"omitted_rtt_count", 0}, {"omitted_goodput_count", 0},
              {"observations", NativeJson::array()}};
            const auto expiresAtMs = std::min(
              request.at("deadline_ms").get<std::uint64_t>(),
              completedAtMs + std::uint64_t{5000});
            for (const auto& pair : request.at("rtt_pairs")) {
              const auto observer = pair.at("src_provider").get<std::string>();
              const auto targetProvider = pair.at("dst_provider").get<std::string>();
              payload["observations"].push_back(NativeJson{
                {"observer", observer}, {"target", targetProvider},
                {"kind", "active_probe_rtt"}, {"value", 40.0},
                {"observed_at_ms", completedAtMs}, {"expires_at_ms", expiresAtMs},
                {"peer_boot_epoch", user->providerBootEpochs.at(targetProvider)},
                {"target_signature_validated", true},
                {"data_name", targetProvider + "/NDNSF-DI/probe/rtt/" +
                  std::to_string(completedAtMs)}});
            }
            for (const auto& edge : request.at("goodput_edges")) {
              const auto source = edge.at("src_provider").get<std::string>();
              const auto destination = edge.at("dst_provider").get<std::string>();
              payload["observations"].push_back(NativeJson{
                {"observer", destination}, {"target", source},
                {"kind", "active_probe_goodput"}, {"value", 8.0},
                {"observed_at_ms", completedAtMs}, {"expires_at_ms", expiresAtMs},
                {"peer_boot_epoch", user->providerBootEpochs.at(source)},
                {"target_signature_validated", true},
                {"probe_profile_validated", true}, {"train_complete", true},
                {"sent_segments", std::uint64_t{16}},
                {"received_segments", std::uint64_t{16}},
                {"payload_bytes", std::uint64_t{69000}}, {"elapsed_ms", 69.0},
                {"profile_digest", ndn_service_framework::NETWORK_PROBE_TRAIN_PROFILE_DIGEST},
                {"probe_source_send_gaps_ms", NativeJson(std::vector<double>(14, 0.01))},
                {"probe_source_timing_validated", true},
                {"probe_train_goodput_mbps", 8.0},
                {"probe_rate_status", "MEASURED_GOODPUT"},
                {"probe_rate_reason", "MEASURED_WARMED_PACKET_TRAIN"},
                {"probe_rtt_ms", NativeJson(std::vector<double>(16, 1.3))},
                {"probe_interest_gaps_ms", NativeJson(std::vector<double>(15, 0.01))},
                {"probe_data_arrival_gaps_ms", NativeJson(std::vector<double>(15, 4.096))},
                {"probe_data_wire_bytes", NativeJson(std::vector<std::uint64_t>(16, 4400))},
                {"data_name", source + "/NDNSF-DI/probe/goodput/" +
                  std::to_string(completedAtMs)}});
            }
            if (action == "cache") {
              BOOST_CHECK_EQUAL(request.at("peer_boot_epochs").at(
                probePlacement->rttTarget()).get<std::string>(),
                user->providerBootEpochs.at(probePlacement->rttTarget()));
              payload["cache_complete"] = false;
              payload["observations"] = NativeJson::array();
              payload["omitted_rtt_count"] = request.at("rtt_pairs").size();
              payload["omitted_goodput_count"] = request.at("goodput_edges").size();
            }
          }
          const auto responseWire = nativeCanonicalJson(payload);
          ndn::Buffer responsePayload(responseWire.begin(), responseWire.end());
          ResponseMessage response;
          response.setStatus(true);
          response.setUserToken("bootstrap-user-token");
          response.setTokens({{"targeted.0.provider", "provider-token-0"},
                               {"targeted.0.user", "user-token-0"},
                               {"targeted.count", "1"}});
          response.setPayload(responsePayload, responsePayload.size());
      const auto signerProvider =
        (spoofTargetedSigner && action == "authorize") ||
        (spoofMeasurementSigner && action == "measure") ?
        (target.toUri() == "/provider/a" ? "/provider/b" : "/provider/a") :
        target.toUri();
      response.setAuthenticatedTransportEvidence(
        target.toUri() + "/response",
        signerProvider + "/KEY/fixture/issuer/v=1",
        nativePlanningDigest(responseWire));
          user->deliver(targetedId, target, response);
        }
      };
      if (cancelAfterTargetedMeasurement || cancelDuringCacheQuery) {
        const auto findMeasurement = [&] ()
          -> std::optional<std::pair<ndn::Name, ndn::Name>> {
          for (const auto& pending : user->pendingTargetedCalls()) {
            const auto request = nativeParseJson(user->requestWire(pending.first));
            if (request.value("action", std::string{}) ==
                  (cancelDuringCacheQuery ? "cache" : "measure"))
              return pending;
          }
          return std::nullopt;
        };
        BOOST_REQUIRE(pumpUntil([&] {
          answerTargetedCalls(true);
          return findMeasurement().has_value();
        }));
        const auto pending = *findMeasurement();
        handle.cancel();
        BOOST_CHECK(handle.status() == NativeRequestStatus::Cancelled);
        const auto lateTimeout = user->timeoutCallback(pending.first);
        lateTimeout(pending.first);
        ResponseMessage lateUnavailable;
        lateUnavailable.setStatus(false);
        lateUnavailable.setErrorInfo("fixture completion after cancellation");
        user->deliver(pending.first, pending.second, lateUnavailable);
        BOOST_REQUIRE(pumpUntil([&] {
          return !user->hasPendingCall(pending.first);
        }));
        client.close();
        face.getIoContext().restart();
        face.getIoContext().poll();
        BOOST_CHECK(handle.status() == NativeRequestStatus::Cancelled);
        BOOST_CHECK(!user->committed(id));
        BOOST_CHECK_EQUAL(probePlacement->callCount(), 1U);
        if (cancelDuringCacheQuery) {
          BOOST_REQUIRE_EQUAL(targetedActions.size(), 1U);
          BOOST_CHECK_EQUAL(targetedActions.front().first, "cache");
        }
        return;
      }
      BOOST_REQUIRE(pumpUntil([&] {
        answerTargetedCalls();
        return user->committed(id) ||
          handle.status() != NativeRequestStatus::Pending;
      }));
      if (handle.status() != NativeRequestStatus::Pending)
        handle.result(std::chrono::milliseconds(0));
      BOOST_REQUIRE(user->committed(id));
      const auto actionSummary = [&] {
        NativeJson values = NativeJson::array();
        for (const auto& action : targetedActions)
          values.push_back({{"action", action.first}, {"target", action.second}});
        return nativeCanonicalJson(values);
      }();
      BOOST_CHECK_MESSAGE(targetedActions.size() == (spoofTargetedSigner ? 2U : 3U),
                          "targeted calls=" + actionSummary);
      BOOST_REQUIRE(!targetedActions.empty());
      BOOST_CHECK_EQUAL(targetedActions[0].first, "cache");
      BOOST_CHECK_EQUAL(targetedActions[0].second, probePlacement->observer());
      if (!spoofTargetedSigner && targetedActions.size() == 3) {
        BOOST_CHECK_EQUAL(targetedActions[1].first, "authorize");
        BOOST_CHECK_EQUAL(targetedActions[1].second, probePlacement->rttTarget());
        BOOST_CHECK_EQUAL(targetedActions[2].first, "measure");
        BOOST_CHECK_EQUAL(targetedActions[2].second, probePlacement->observer());
      }
      else if (spoofTargetedSigner && targetedActions.size() == 2) {
        BOOST_CHECK_EQUAL(targetedActions[1].first, "authorize");
        BOOST_CHECK_EQUAL(targetedActions[1].second, probePlacement->rttTarget());
      }
      BOOST_CHECK_EQUAL(probePlacement->callCount(), 2U);
      BOOST_CHECK(probePlacement->sawTargetedRound());
      BOOST_CHECK_MESSAGE(!probePlacement->initialSnapshotDigest().empty(),
                          "initial V4 snapshot was unavailable");
      BOOST_CHECK_MESSAGE(!probePlacement->mergedSnapshotDigest().empty(),
                          "merged V4 snapshot was unavailable");
      BOOST_CHECK_MESSAGE(probePlacement->initialSnapshotDigest() !=
                            probePlacement->mergedSnapshotDigest(),
                          "targeted round did not change the snapshot digest");
      if (!spoofTargetedSigner) {
        BOOST_REQUIRE(probePlacement->decision().is_object());
        if (timeoutTargetedMeasurement || spoofMeasurementSigner) {
          BOOST_CHECK_EQUAL(probePlacement->decision().at("status")
                              .get<std::string>(), "BASELINE_FALLBACK");
          BOOST_CHECK(probePlacement->decision().at("edge_costs").empty());
        }
        else {
          BOOST_CHECK_MESSAGE(
            probePlacement->decision().at("status").get<std::string>() == "SCORED",
            "V4 placement decision=" + probePlacement->decision().dump() +
              " requester samples=" + probePlacement->requesterSamples().dump() +
              " provider boots=" + probePlacement->providerBoots().dump());
          if (probePlacement->decision().at("status").get<std::string>() == "SCORED") {
            BOOST_REQUIRE_EQUAL(probePlacement->decision().at("edge_costs").size(), 1U);
            BOOST_CHECK_EQUAL(probePlacement->decision().at("edge_costs")[0]
                                .at("rate_method").get<std::string>(),
                              "ACTIVE_PROBE_GOODPUT");
          }
        }
      }
    }
    BOOST_REQUIRE(pumpUntil([&] { return user->committed(id) || handle.status() != NativeRequestStatus::Pending; }));
    if (handle.status() != NativeRequestStatus::Pending) handle.result(std::chrono::milliseconds(0));
    BOOST_REQUIRE(user->committed(id));
    if (scenario == 13) {
      BOOST_REQUIRE(conversation.has_value());
      const auto role = input.roles.front().selectedRole;
      const auto planRoleMapDigest = conversation->planRoleMapDigest;
      const auto planDigest = user->assignmentField(id, ndn::Name("/provider/a"), "plan_digest");
      BOOST_REQUIRE(!planDigest.empty());
      ProviderConversationStateReceiptV1 receipt;
      receipt.conversationId = conversation->conversationId;
      receipt.parentContextEpoch = 0;
      receipt.successorContextEpoch = 1;
      receipt.originRequestId = id.toUri();
      receipt.originGenerationId = requestOptions.generation->generationId;
      receipt.serviceName = "/service";
      receipt.requesterIdentity = "/requester";
      receipt.securityDomainDigest = runtime.security.policyDigest;
      receipt.modelDigest = model.intentDigest();
      receipt.graphSemanticDigest = model.semanticsDigest;
      receipt.adapterDigest = model.adapter.descriptorDigest();
      receipt.roleName = role;
      receipt.roleSplitDigest = input.roles.front().recipeDigest;
      receipt.layoutDigest = input.roles.front().artifactProfileDigest;
      receipt.planRoleMapDigest = planRoleMapDigest;
      receipt.providerIdentity = "/provider/a";
      receipt.providerBootId = "/provider/a-boot";
      receipt.cacheEpoch = 1;
      receipt.prefixDigest = nativeConversationPrefixDigest({1, 2});
      receipt.prefixTokenCount = 2;
      receipt.positionDigest = nativePlanningDigest("position");
      receipt.stateSchemaDigest = nativePlanningDigest("state-schema");
      receipt.stateComponentDigests = {nativePlanningDigest("state-component")};
      receipt.expiresAtMs = 2'000'000'060'000ULL;
      const auto receiptJson = receipt.toJson();
      const auto seedRecord = [&] (const ndn::Name& topic, const ndn::Buffer& payload) {
        VerifiedCollaborationData value;
        value.dataName = ndn::Name("/provider/a/NDNSF/DI/conversation").append(topic);
        value.requestId = id;
        value.keyScope = "ndnsf-di-conversation-state-v1";
        value.topic = topic;
        value.producer = ndn::Name("/provider/a");
        value.producerRole = role;
        value.sequence = 1;
        value.payload = payload;
        user->seedConversationData(std::move(value));
      };
      const ndn::Buffer receiptPayload(receiptJson.begin(), receiptJson.end());
      user->seedConversationScope(id, "ndnsf-di-conversation-state-v1", ndn::Buffer(32, 0x5a));
      seedRecord(ndn::Name("/ndnsf-di/conversation/receipt").append(role), receiptPayload);

      NativeConversationConfig shadowConfig;
      shadowConfig.authenticationKeys = {std::vector<std::uint8_t>(32, 0x5a)};
      shadowConfig.requesterIdentity = "/requester";
      shadowConfig.serviceName = "/service";
      shadowConfig.securityDomainDigest = runtime.security.policyDigest;
      shadowConfig.nowMs = [] { return std::uint64_t{2'000'000'000'000}; };
      NativeConversationCoordinator shadow(std::move(shadowConfig));
      auto shadowContinuation = *conversation;
      shadowContinuation.requestContractDigest = nativePlanningDigest("contract");
      const auto shadowTurn = shadow.beginTurn(shadowContinuation, id.toUri(), 1);
      shadow.acceptTokenPrefix(shadowTurn, {1, 2});
      NativeCompletedAttempt completed;
      completed.requestId = id.toUri();
      completed.attempt = 1;
      completed.tokenIds = {1, 2};
      completed.complete = true;
      completed.generationId = requestOptions.generation->generationId;
      completed.modelContractDigest = model.intentDigest();
      completed.tokenizerDigest = requestOptions.generation->tokenizerDigest;
      completed.chatTemplateDigest = model.semanticsDigest;
      completed.applicationMessages.assign(application.payload.begin(), application.payload.end());
      completed.authenticatedReceipts = {nativeParseJson(receiptJson)};
      completed.commitProviderState = [] (const std::string&) {};
      completed.rollbackProviderState = [] {};
      const auto checkpoint = shadow.prepareCheckpoint(shadowTurn, completed);
      auto ack = nativeCanonicalJson(NativeJson{
        {"schema", "ndnsf-di-provider-conversation-commit-ack-v1"},
        {"requestId", id.toUri()}, {"attemptEpoch", 1},
        {"generationId", requestOptions.generation->generationId}, {"planDigest", planDigest},
        {"conversationId", conversation->conversationId}, {"parentContextEpoch", 0},
        {"successorContextEpoch", 1}, {"serviceName", "/service"},
        {"planRoleMapDigest", planRoleMapDigest}, {"roleName", role},
        {"receiptDigest", receipt.computedDigest()}, {"checkpointDigest", checkpoint.checkpointDigest},
        {"providerIdentity", "/provider/a"}, {"providerBootId", receipt.providerBootId},
        {"cacheEpoch", 1}, {"committed", true}});
      seedRecord(ndn::Name("/ndnsf-di/conversation/commit").append(role),
                 ndn::Buffer(ack.begin(), ack.end()));
    }
    if (scenario >= 3 && scenario <= 13) {
      const auto callbacks = user->streamCallbacks(id);
      const auto tokenEvent = [&](std::int64_t token, std::size_t epoch, const std::string& prefix,
                                  const std::string& delta, const std::string& hint) {
        const auto wire = nativeCanonicalJson(NativeJson{{"schema", "GenerationTokenEventV1"},
          {"tokenId", token}, {"tokenEpoch", epoch}, {"acceptedPrefixDigest", nativePlanningDigest(prefix)},
          {"textDelta", delta}, {"finishHint", hint}, {"samplingDigest", requestOptions.generation->samplingDigest}});
        return ndn::Buffer(wire.begin(), wire.end());
      };
      const auto finalPayload = [&](const std::string& text) {
        const auto wire = nativeCanonicalJson(NativeJson{{"schema", "NDNSF-DI-FINAL-V1"},
          {"tokenIds", {1, 2}}, {"text", text}, {"finishHint", "EOS"}, {"finishReason", "eos"}});
        return ndn::Buffer(wire.begin(), wire.end());
      };
      const auto firstToken = tokenEvent(1, 1, "1", "a", scenario == 5 ? "EOS" : "NONE");
      if (scenario != 9) callbacks->onEvent(firstToken);
      if (scenario != 9)
        BOOST_REQUIRE(pumpUntil([&] { return accepted.load() == 1 || handle.status() != NativeRequestStatus::Pending; }));
      if (scenario == 4) callbacks->onEvent(tokenEvent(3, 3, "1,3", "bad", "NONE"));
      else if (scenario >= 10 && scenario != 13) {
        auto invalid = tokenEvent(2, 2, "1,2", "b", "EOS");
        if (scenario == 10) invalid = firstToken; // Duplicate accepted epoch.
        else if (scenario == 11) invalid = tokenEvent(2, 2, "1,9", "b", "EOS");
        else {
          auto value = nativeParseJson(std::string(invalid.begin(), invalid.end()));
          if (scenario == 12) value["requestId"] = "/foreign/request";
          else value["tokenId"] = 2.0; // Numeric equality must not erase the integer wire contract.
          const auto wire = nativeCanonicalJson(value); invalid.assign(wire.begin(), wire.end());
        }
        callbacks->onEvent(invalid);
      }
      else if (scenario == 5 || scenario == 8 || scenario == 9) {
        callbacks->onError(StreamedInvocationError(StreamedInvocationErrorCode::ProviderFailure,
          "provider commit fixture failure", id, 0, ndn::Name("/provider/a")));
        if (scenario != 5) {
          BOOST_REQUIRE(pumpUntil([&] {
            const auto ids = user->pendingIds();
            return std::any_of(ids.begin(), ids.end(), [&](const auto& value) { return value != id; }) ||
              handle.status() != NativeRequestStatus::Pending;
          }));
          if (handle.status() != NativeRequestStatus::Pending) handle.result(std::chrono::milliseconds(0));
          const auto ids = user->pendingIds();
          const auto replacement = std::find_if(ids.begin(), ids.end(), [&](const auto& value) { return value != id; });
          BOOST_REQUIRE(replacement != ids.end());
          const auto nextId = *replacement;
          const auto request = nativeParseJson(user->requestWire(nextId));
          if (const auto path = std::getenv("NDNSF_STREAM_REQUEST_WIRE")) {
            std::ofstream output(path, std::ios::app);
            BOOST_REQUIRE(output.good());
            output << user->requestWire(nextId) << '\n';
          }
          const auto& recovery = request.at("task").at("generation_recovery");
          BOOST_CHECK_EQUAL(recovery.at("attempt"), 2);
          BOOST_CHECK_EQUAL(recovery.at("failed_provider"), "/provider/a");
          BOOST_CHECK_EQUAL(recovery.at("committed_token_count"), scenario == 9 ? 0 : 1);
          BOOST_CHECK(recovery.at("committed_token_ids") == (scenario == 9 ? NativeJson::array() : NativeJson::array({1})));
          BOOST_CHECK_EQUAL(handle.requestId(), id.toUri());
          // These retained callbacks come from the old Core attempt.
          callbacks->onEvent(tokenEvent(9, 2, "1,9", "stale", "NONE"));
          callbacks->onComplete(finalPayload("stale"));
          submitOffer(nextId, "/provider/b", 2);
          BOOST_REQUIRE(pumpUntil([&] { return user->committed(nextId) || handle.status() != NativeRequestStatus::Pending; }));
          if (handle.status() != NativeRequestStatus::Pending) handle.result(std::chrono::milliseconds(0));
          BOOST_REQUIRE(user->committed(nextId));
          const auto next = user->streamCallbacks(nextId);
          if (scenario == 9) next->onEvent(tokenEvent(1, 1, "1", "a", "NONE"));
          next->onEvent(tokenEvent(2, 2, "1,2", "b", "EOS"));
          next->onComplete(finalPayload("ab"));
        }
      }
      else if (scenario != 7) {
        callbacks->onEvent(tokenEvent(2, 2, "1,2", "b", "EOS"));
        callbacks->onComplete(finalPayload(scenario == 6 ? "wrong" : "ab"));
      }
      BOOST_REQUIRE(pumpUntil([&] { return handle.status() != NativeRequestStatus::Pending; }));
      if (scenario == 3 || scenario == 8 || scenario == 9 || scenario == 13) {
        BOOST_CHECK(handle.status() == NativeRequestStatus::Succeeded);
        if (scenario == 13) {
          BOOST_REQUIRE(observedDone.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
          BOOST_REQUIRE_GE(observedEvents->size(), 3U);
          BOOST_CHECK(!observedEvents->at(0).terminal);
          BOOST_CHECK(!observedEvents->at(1).terminal);
          BOOST_CHECK(observedEvents->back().terminal);
          BOOST_CHECK_EQUAL(observedEvents->at(0).requestId, handle.requestId());
          BOOST_CHECK_EQUAL(observedEvents->at(1).requestId, handle.requestId());
          BOOST_CHECK(nativeParseJson(std::string(
            observedEvents->at(0).payload.begin(), observedEvents->at(0).payload.end()))
            .at("schema") == "GenerationTokenEventV1");
          BOOST_CHECK(nativeParseJson(std::string(
            observedEvents->at(1).payload.begin(), observedEvents->at(1).payload.end()))
            .at("schema") == "GenerationTokenEventV1");
        }
        BOOST_CHECK_EQUAL(accepted.load(), 2);
        const auto result = handle.result(std::chrono::milliseconds(0));
        BOOST_CHECK_EQUAL(nativeParseJson(std::string(result.payload.begin(), result.payload.end())).at("text"), "ab");
      }
      else {
        BOOST_CHECK(handle.status() == NativeRequestStatus::Failed);
        BOOST_CHECK_EQUAL(accepted.load(), scenario == 6 ? 2 : 1);
        const auto expected = scenario == 6 ? "StreamFinalMismatch" : scenario == 7 ? "StreamCallbackFailed" :
          scenario == 5 ? "NATIVE_STREAM_FAILED" : "StreamEventLineageMismatch";
        BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
          [&](const auto& error) { return error.code() == expected; });
      }
      callbacks->onEvent(firstToken); callbacks->onComplete(finalPayload("ab"));
      client.close();
      BOOST_REQUIRE(pumpUntil([&] { return user->pendingIds().empty(); }));
      return;
    }
    auto lateResponse = user->responseCallback(id);
    auto lateTimeout = user->timeoutCallback(id);
    ndn::Name responseProvider("/provider/a");
    if (scenario == 16) {
      responseProvider = user->terminalResponseProvider(id);
      BOOST_REQUIRE(!responseProvider.empty());
    }
    ResponseMessage response; response.setStatus(true);
    ndn::Buffer payload{42, 43}; response.setPayload(payload, payload.size());
    response.setAuthenticatedTransportEvidence(
      scenario == 1 ? "/foreign/response" : responseProvider.toUri() + "/response",
      responseProvider.toUri() + "/KEY/fixture/issuer/v=1",
      nativePlanningDigest("response-wire"));
    if (scenario == 2) handle.cancel();
    else user->postToIo([&, responseProvider, response] {
      user->deliver(id, responseProvider, response);
    });
    BOOST_REQUIRE(pumpUntil([&] { return handle.status() != NativeRequestStatus::Pending && !user->hasPendingCall(id); }));
    const auto terminal = handle.status();
    if (scenario == 0 || scenario == 14 || scenario == 15 || scenario == 16) {
      BOOST_CHECK(terminal == NativeRequestStatus::Succeeded);
      const auto result = handle.result(std::chrono::milliseconds(0));
      BOOST_CHECK(result.payload == std::vector<std::uint8_t>({42, 43}));
      BOOST_CHECK_EQUAL(result.modelDigest, model.intentDigest());
      BOOST_CHECK(!result.planDigest.empty());
    }
    else if (scenario == 1) {
      BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
        [](const auto& error) { return error.code() == "NATIVE_RESPONSE_BINDING_REJECTED"; });
    }
    else BOOST_CHECK(terminal == NativeRequestStatus::Cancelled);
    // Replay callbacks retained before Core erased its PendingCall.
    lateResponse(response); lateTimeout(id); handle.cancel(); client.close();
    face.getIoContext().restart(); face.getIoContext().poll();
    BOOST_CHECK(handle.status() == terminal);
    BOOST_CHECK(!user->hasPendingCall(id));
  }
  if (verifyLateBootstrapRelease) {
    // Exercise the actual fixture User/base teardown on a late worker,
    // with no outer owner keeping either bootstrap resource alive.
    // Client close can finish before its final queued Face callback releases
    // the User. Drain that actual work within the existing fixture budget.
    BOOST_REQUIRE(pumpUntil([&] { return user.use_count() == 1; }));
    const std::weak_ptr<User> weakUser = user;
    std::promise<void> parked;
    std::promise<void> release;
    auto parkedSignal = parked.get_future();
    auto releaseSignal = release.get_future();
    std::thread worker([held = std::move(user), signal = std::move(releaseSignal),
                        &parked]() mutable {
      parked.set_value();
      signal.wait();
      held.reset();
    });
    const auto ready = parkedSignal.wait_for(std::chrono::seconds(2));
    const bool userAlive = !weakUser.expired();
    const bool ownersAlive = !weakOwners.expired();
    release.set_value();
    worker.join();
    BOOST_REQUIRE(ready == std::future_status::ready);
    BOOST_CHECK(userAlive);
    BOOST_CHECK(ownersAlive);
    BOOST_CHECK(weakUser.expired());
    BOOST_CHECK(weakOwners.expired());
  }
}

void runPublicClientScenario(int scenario)
{
  runPublicClientScenario(scenario, false);
}

BOOST_AUTO_TEST_CASE(Spec197PublicClientBootstrapOwnersSurviveLateRelease)
{
  runPublicClientScenario(16, false, false, false, false, true);
}

BOOST_AUTO_TEST_CASE(PublicClientCommitsSignedOfferAndIgnoresLateTerminalCallbacks)
{
  for (int scenario = 0; scenario != 3; ++scenario) runPublicClientScenario(scenario);
}

BOOST_AUTO_TEST_CASE(PublicClientConversationCommitsSeededReceiptAndCheckpoint)
{
  runPublicClientScenario(13);
}

BOOST_AUTO_TEST_CASE(Spec196PublicClientRejectsUnscopedProbeBeforeDispatch)
{
  runPublicClientScenario(14);
}

BOOST_AUTO_TEST_CASE(Spec196PublicClientRejectsUnallowlistedRttBeforeDispatch)
{
  runPublicClientScenario(15);
}

BOOST_AUTO_TEST_CASE(Spec196PublicClientDispatchesTargetedProbeAndPlansMergedSnapshot)
{
  runPublicClientScenario(16);
}

BOOST_AUTO_TEST_CASE(Spec196PublicClientRejectsTargetedAuthorizationFromWrongSigner)
{
  runPublicClientScenario(16, true);
}

BOOST_AUTO_TEST_CASE(Spec196PublicClientFallsBackAfterTargetedMeasurementTimeout)
{
  runPublicClientScenario(16, false, true);
}

BOOST_AUTO_TEST_CASE(Spec196PublicClientIgnoresTargetedCompletionAfterCancellation)
{
  runPublicClientScenario(16, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec196PublicClientFallsBackOnWrongMeasurementSigner)
{
  runPublicClientScenario(16, false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec199PublicClientCacheTimeoutAndLateReplyFallBackOnce)
{
  runPublicClientScenario(16, false, false, false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec199PublicClientCancelledCacheDoesNotAuthorizePeers)
{
  runPublicClientScenario(16, false, false, false, false, false, false, true);
}

BOOST_AUTO_TEST_CASE(AdmittedPlacementSealsSdkCoreAndRejectsTampering)
{
  const auto f = oracle();
  const NativeStrategyIdentity identity{f["strategy"]["name"], f["strategy"]["version"], f["strategy"]["state"]};
  for (const auto& sample : f.at("seal_cases")) {
    BOOST_TEST_CONTEXT(sample.at("name").get<std::string>()) {
      Input input(f, sample);
      const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
      const auto proposal = NativePreSplitFirstPlacement(identity).proposeRoles(
        input.context, input.ackDigest, input.roles, input.offers, now);
      NativeExecutionPlan execution = input.split.executionPlan;
      execution.roles.clear(); execution.serviceName = input.context.serviceName;
      execution.modelName = input.inspected.descriptor.modelName;
      NativePlanSealingInputs inputs;
      inputs.requesterIdentity = "/requester"; inputs.protectionEpoch = input.roles.front().protectionEpoch;
      inputs.expiresAtMs = input.context.deadlineMs;
      inputs.artifacts.requestId = input.context.requestId; inputs.artifacts.attempt = input.context.attempt;
      inputs.artifacts.modelDigest = input.inspected.descriptor.contentDigest; inputs.artifacts.graphDigest = input.context.graphDigest;
      inputs.artifacts.manifestDigest = input.inspected.modelManifestDigest;
      inputs.artifacts.recipeDigest = input.roles.front().recipeDigest;
      for (const auto& role : proposal.roles) {
        execution.roles.push_back(role.selectedRole);
        inputs.artifacts.sourceByRole[role.selectedRole] = "/catalog/source";
        inputs.artifacts.artifactDigestByRole[role.selectedRole] = role.artifactDigest;
      }
      for (std::size_t i = 0; i < input.roles.size(); ++i)
        inputs.assemblyByRole.emplace(std::to_string(i), input.roles[i]);
      unsigned publications = 0;
      auto published = inputs.artifacts;
      if (sample.contains("root_json")) {
        input.inspected.canonicalSourceBytes = 6;
        published.canonicalManifestJson = sample.at("root_json");
        published.manifestDigest = sample.at("published_manifest_digest");
        if (sample.at("name") == "published_rank_cover") {
          input.inspected.canonicalInitializerBytes = 7;
          input.inspected.canonicalInitializerObjectDigest = nativePlanningDigest("weights");
          input.inspected.canonicalInitializerDigest = input.roles.front().canonicalInitializerDigest;
        }
        for (auto& item : published.sourceByRole) {
          published.artifactNameByRole[item.first] = "/artifact/stable/" + nativePlanningDigest(item.first).substr(7);
          item.second = "/encrypted/root";
        }
      }
      published.requestId = "foreign-port-request"; published.attempt = 99;
      NativeRequestPreparation preparation(std::make_shared<NativeAdapterRegistry>(), {},
        [&](const NativeInspectedModel& model, const NativeSplitCandidate& candidate,
            const std::vector<NativeSelectionRoleV3>& selected, const NativeRequestControl&) {
          ++publications;
          BOOST_CHECK_EQUAL(&candidate, &input.split);
          BOOST_CHECK_EQUAL(model.canonicalSourceName, input.inspected.canonicalSourceName);
          BOOST_REQUIRE_EQUAL(selected.size(), proposal.roles.size());
          for (std::size_t i = 0; i < selected.size(); ++i) {
            BOOST_CHECK_EQUAL(selected[i].selectedRole, proposal.roles[i].selectedRole);
            BOOST_CHECK(selected[i].deviceSet == proposal.roles[i].deviceSet);
          }
          return published;
        });
      NativeRequestControl control{input.context.requestId, input.context.attempt,
        std::chrono::steady_clock::now() + std::chrono::seconds(10), {}};
      // Authenticate/validate the placement first, then publish through the
      // actual V3 preparation API. No legacy planning DTO is reconstructed.
      validateNativeRolePlacement(proposal, input.roles, input.offers, now);
      inputs.artifacts = preparation.ensureArtifacts(input.inspected, input.split, proposal, control);
      BOOST_CHECK_EQUAL(inputs.artifacts.requestId, control.requestId);
      BOOST_CHECK_EQUAL(inputs.artifacts.attempt, control.attempt);
      auto badGraph = input.inspected;
      badGraph.graph.nodes.clear();
      BOOST_CHECK_THROW(preparation.ensureArtifacts(badGraph, input.split, proposal, control),
                        std::invalid_argument);
      auto duplicateNodeModel = input.inspected;
      auto duplicateNode = duplicateNodeModel.graph.nodes.front();
      duplicateNode.ordinal = duplicateNodeModel.graph.nodes.size();
      duplicateNodeModel.graph.nodes.push_back(duplicateNode);
      duplicateNodeModel.graph.topologicalOrder.push_back(duplicateNode.id);
      BOOST_CHECK_THROW(preparation.ensureArtifacts(
                          duplicateNodeModel, input.split, proposal, control),
                        std::invalid_argument);
      if (!input.inspected.graph.edges.empty()) {
        auto duplicateEdgeModel = input.inspected;
        const auto edge = duplicateEdgeModel.graph.edges.front();
        duplicateEdgeModel.graph.edges.push_back(edge);
        BOOST_CHECK_THROW(preparation.ensureArtifacts(
                            duplicateEdgeModel, input.split, proposal, control),
                          std::invalid_argument);
        auto duplicateConsumerModel = input.inspected;
        const auto consumer = duplicateConsumerModel.graph.edges.front().consumers.front();
        duplicateConsumerModel.graph.edges.front().consumers.push_back(consumer);
        BOOST_CHECK_THROW(preparation.ensureArtifacts(
                            duplicateConsumerModel, input.split, proposal, control),
                          std::invalid_argument);
      }
      if (!input.inspected.graph.legalCutEdges.empty()) {
        auto duplicateCutModel = input.inspected;
        const auto cut = duplicateCutModel.graph.legalCutEdges.front();
        duplicateCutModel.graph.legalCutEdges.push_back(cut);
        BOOST_CHECK_THROW(preparation.ensureArtifacts(
                            duplicateCutModel, input.split, proposal, control),
                          std::invalid_argument);
      }
      auto badCandidate = input.split;
      badCandidate.graphDigest = nativePlanningDigest("foreign-graph");
      BOOST_CHECK_THROW(preparation.ensureArtifacts(input.inspected, badCandidate, proposal, control),
                        std::invalid_argument);
      BOOST_CHECK_EQUAL(publications, 1);

      // A port is an extension boundary: model checks must run again after it.
      auto portMutatedModel = input.inspected;
      unsigned mutatingPublications = 0;
      NativeRequestPreparation mutatingPreparation(std::make_shared<NativeAdapterRegistry>(), {},
        [&](const NativeInspectedModel&, const NativeSplitCandidate&,
            const std::vector<NativeSelectionRoleV3>&, const NativeRequestControl&) {
          ++mutatingPublications;
          portMutatedModel.graph.nodes.clear();
          return published;
        }, {});
      BOOST_CHECK_THROW(mutatingPreparation.ensureArtifacts(
                          portMutatedModel, input.split, proposal, control),
                        std::invalid_argument);
      BOOST_CHECK_EQUAL(mutatingPublications, 1U);
      auto foreign = proposal;
      foreign.roles[0].artifactDigest = nativePlanningDigest("foreign-artifact");
      BOOST_CHECK_THROW(preparation.ensureArtifacts(input.inspected, input.split, foreign, control),
                        std::runtime_error);
      foreign = proposal; foreign.providerByRole.clear();
      BOOST_CHECK_THROW(preparation.ensureArtifacts(input.inspected, input.split, foreign, control),
                        std::runtime_error);
      BOOST_CHECK_EQUAL(publications, 1);
      published.artifactDigestByRole.begin()->second = nativePlanningDigest("foreign-artifact");
      BOOST_CHECK_THROW(preparation.ensureArtifacts(input.inspected, input.split, proposal, control),
                        std::runtime_error);
      BOOST_CHECK_EQUAL(publications, 2);
      const auto seal = [&](const auto& value) { return NativePlanSealer::sealCore(input.inspected,
        input.split, value, execution, input.offers, input.ackDigest, inputs); };
      if (sample.value("publication_reject", false)) {
        // The previously loaded recipe is not the newly published certificate;
        // ACCEPT_IF_EXACT_REUSE alone cannot authorize its preparation.
        BOOST_CHECK_THROW(seal(proposal), std::invalid_argument);
        auto legacyInputs = inputs;
        legacyInputs.artifacts.canonicalManifestJson.clear();
        legacyInputs.artifacts.manifestDigest = input.inspected.modelManifestDigest;
        const auto legacyCore = NativePlanSealer::shareFixedCore(NativePlanSealer::sealCore(
          input.inspected, input.split, proposal, execution, input.offers, input.ackDigest, legacyInputs));
        auto warmInputs = inputs;
        warmInputs.assemblyByRole.clear();
        warmInputs.borrowedPreparedRoles = &input.roles;
        warmInputs.previousFixedCore = legacyCore.fixedCoreOwner;
        // Original loaded-device admission succeeds above. Its old cache
        // cannot authorize the different newly published recipe below.
        BOOST_CHECK_THROW(NativePlanSealer::sealCore(input.inspected, input.split,
          proposal, execution, input.offers, input.ackDigest, warmInputs), std::invalid_argument);
        continue;
      }
      const auto core = seal(proposal);
      BOOST_CHECK_EQUAL(core.coreDigest, sample.at("core_digest").get<std::string>());
      const auto cachedCore = NativePlanSealer::shareFixedCore(core);
      BOOST_REQUIRE(cachedCore.fixedCoreOwner);
      auto warmInputs = inputs;
      warmInputs.assemblyByRole.clear();
      warmInputs.borrowedPreparedRoles = &input.roles;
      warmInputs.previousFixedCore = cachedCore.fixedCoreOwner;
      const auto warmCore = NativePlanSealer::sealCore(input.inspected, input.split,
        proposal, execution, input.offers, input.ackDigest, warmInputs);
      BOOST_CHECK(warmCore.reusedFixedCore);
      BOOST_CHECK(warmCore.assemblyByRole.empty());
      BOOST_CHECK_EQUAL(warmCore.fixedCoreOwner.get(), cachedCore.fixedCoreOwner.get());
      // The oracle is frozen independently of the warm serializer/cache path.
      BOOST_CHECK_EQUAL(warmCore.coreDigest, sample.at("core_digest").get<std::string>());
      BOOST_CHECK_EQUAL(warmCore.coreDigest, core.coreDigest);
      warmCore.validate();
      auto mixedInputs = warmInputs;
      mixedInputs.assemblyByRole = inputs.assemblyByRole;
      BOOST_CHECK_THROW(NativePlanSealer::sealCore(input.inspected, input.split,
        proposal, execution, input.offers, input.ackDigest, mixedInputs), std::invalid_argument);
      BOOST_CHECK_THROW(NativePlanSealer::sealCore(input.inspected, input.split,
        proposal, execution, {}, input.ackDigest, warmInputs), std::invalid_argument);
      if (sample.contains("root_json")) {
        BOOST_CHECK(cachedCore.fixedCoreOwner->canonicalRecipes());
        auto legacyInputs = inputs;
        legacyInputs.artifacts.canonicalManifestJson.clear();
        legacyInputs.artifacts.manifestDigest = input.inspected.modelManifestDigest;
        const auto legacyCore = NativePlanSealer::shareFixedCore(NativePlanSealer::sealCore(
          input.inspected, input.split, proposal, execution, input.offers, input.ackDigest, legacyInputs));
        BOOST_REQUIRE(legacyCore.fixedCoreOwner);
        BOOST_CHECK(!legacyCore.fixedCoreOwner->canonicalRecipes());
        auto fallbackInputs = warmInputs;
        fallbackInputs.previousFixedCore = legacyCore.fixedCoreOwner;
        const auto fallbackCore = NativePlanSealer::sealCore(input.inspected, input.split,
          proposal, execution, input.offers, input.ackDigest, fallbackInputs);
        BOOST_CHECK(!fallbackCore.reusedFixedCore);
        BOOST_CHECK(!fallbackCore.fixedCoreOwner);
        BOOST_CHECK_EQUAL(fallbackCore.coreDigest, sample.at("core_digest").get<std::string>());
        fallbackCore.validate();

        auto materializedRoles = input.roles;
        auto materializedProposal = proposal;
        for (auto& role : materializedRoles) role.materializedRole = !role.materializedRole;
        for (auto& role : materializedProposal.roles) role.materializedRole = !role.materializedRole;
        auto materializedInputs = warmInputs;
        materializedInputs.borrowedPreparedRoles = &materializedRoles;
        const auto changedCore = NativePlanSealer::sealCore(input.inspected, input.split,
          materializedProposal, execution, input.offers, input.ackDigest, materializedInputs);
        BOOST_CHECK(!changedCore.reusedFixedCore);
        materializedInputs.previousFixedCore.reset();
        const auto changedColdCore = NativePlanSealer::sealCore(input.inspected, input.split,
          materializedProposal, execution, input.offers, input.ackDigest, materializedInputs);
        BOOST_CHECK_EQUAL(changedCore.coreDigest, changedColdCore.coreDigest);
        changedCore.validate();
      }
      if (sample.at("name") == "published_cpu") {
        auto mergeInput = input;
        auto& role = mergeInput.roles.front();
        role.roleKind = "COMPONENT_SET";
        role.layerBegin = role.layerEnd = 0;
        role.mergeKind = "NATIVE_POSTPROCESS";
        role.postprocessIdentity = "fixture-native-merge";
        role.postprocessOutputName = role.expectedOutputs.front().name;
        role.postprocessConfidenceThreshold = 0.25;
        role.postprocessSort = "confidence-desc,class-asc,xyxy-asc";
        mergeInput.split.inputIngressRole = mergeInput.split.resultEgressRole = role.role;
        mergeInput.split.mergeKind = role.mergeKind;
        mergeInput.split.postprocessingJson = nativeCanonicalJson(NativeJson{
          {"identity", role.postprocessIdentity}, {"outputName", role.postprocessOutputName},
          {"confidenceThreshold", role.postprocessConfidenceThreshold}, {"sort", role.postprocessSort}});
        mergeInput.split.candidateDigest = mergeInput.split.computedDigest();
        const auto mergeProposal = NativePreSplitFirstPlacement(identity).proposeRoles(
          mergeInput.context, mergeInput.ackDigest, mergeInput.roles, mergeInput.offers, now);
        auto mergeInputs = inputs;
        mergeInputs.assemblyByRole.clear();
        mergeInputs.borrowedPreparedRoles = &mergeInput.roles;
        const auto mergeCold = NativePlanSealer::sealCore(mergeInput.inspected, mergeInput.split,
          mergeProposal, execution, mergeInput.offers, mergeInput.ackDigest, mergeInputs);
        const auto mergeCached = NativePlanSealer::shareFixedCore(mergeCold);
        BOOST_CHECK(mergeCached.fixedCoreOwner->canonicalRecipes());
        mergeInputs.previousFixedCore = mergeCached.fixedCoreOwner;
        const auto mergeWarm = NativePlanSealer::sealCore(mergeInput.inspected, mergeInput.split,
          mergeProposal, execution, mergeInput.offers, mergeInput.ackDigest, mergeInputs);
        BOOST_CHECK(mergeWarm.reusedFixedCore);
        BOOST_CHECK_EQUAL(mergeWarm.coreDigest, mergeCold.coreDigest);
        BOOST_CHECK_EQUAL(mergeWarm.fixedCoreOwner.get(), mergeCached.fixedCoreOwner.get());
        auto changedProposal = mergeProposal;
        const auto changedRecipe = nativePlanningDigest("changed-native-merge-recipe");
        changedProposal.roles.front().recipeDigest = changedRecipe;
        mergeInput.roles.front().recipeDigest = changedRecipe;
        const auto mergeChanged = NativePlanSealer::sealCore(mergeInput.inspected, mergeInput.split,
          changedProposal, execution, mergeInput.offers, mergeInput.ackDigest, mergeInputs);
        BOOST_CHECK(!mergeChanged.reusedFixedCore);
        BOOST_CHECK_EQUAL(mergeChanged.roleContracts().begin()->second.recipeDigest, changedRecipe);
        BOOST_CHECK_NE(mergeChanged.coreDigest, mergeCold.coreDigest);
        mergeChanged.validate();
      }
      if (sample.at("name") == "cpu") {
        auto snapshotProposal = proposal;
        auto snapshotInputs = inputs;
        const auto expiry = input.context.deadlineMs;
        BOOST_REQUIRE_GT(expiry, now);
        const auto snapshotExpiry = now + (expiry - now) / 2;
        BOOST_REQUIRE_GT(snapshotExpiry, now);
        BOOST_REQUIRE_LT(snapshotExpiry, expiry);
        auto& snapshotContext = snapshotProposal.context;
        snapshotContext.requesterIdentity = "/spec182/requester";
        for (const auto& offer : input.offers)
          snapshotContext.candidateProviders.push_back(offer.observation().provider);
        std::sort(snapshotContext.candidateProviders.begin(),
                  snapshotContext.candidateProviders.end());
        snapshotContext.candidateProviders.erase(std::unique(
          snapshotContext.candidateProviders.begin(),
          snapshotContext.candidateProviders.end()), snapshotContext.candidateProviders.end());
        snapshotContext.candidateSetDigest = nativePlanningDigest(nativeCanonicalJson(
          NativeJson(snapshotContext.candidateProviders)));
        for (const auto& role : input.roles) {
          snapshotContext.candidateArtifacts.push_back(NativeJson{
            {"artifact_digest", role.artifactDigest},
            {"model_manifest_digest", role.modelManifestDigest},
            {"graph_digest", role.graphDigest}, {"role", role.role},
            {"rank", role.rank}, {"backend", role.backend},
            {"protection_epoch", role.protectionEpoch}});
        }
        std::sort(snapshotContext.candidateArtifacts.begin(),
          snapshotContext.candidateArtifacts.end(), [](const auto& left, const auto& right) {
            return nativeCanonicalJson(left) < nativeCanonicalJson(right);
          });
        snapshotContext.candidateArtifactSetDigest = nativePlanningDigest(
          nativeCanonicalJson(NativeJson(snapshotContext.candidateArtifacts)));
        snapshotContext.probeScopeDigest = nativePlanningDigest(nativeCanonicalJson(NativeJson{
          {"schema", "ndnsf-di-placement-probe-scope-v1"},
          {"request_id", snapshotContext.requestId},
          {"attempt", snapshotContext.attempt},
          {"requester_identity", snapshotContext.requesterIdentity},
          {"deadline_ms", snapshotContext.deadlineMs},
          {"service", snapshotContext.serviceName},
          {"model_identity_hash", snapshotContext.modelDigest},
          {"graph_digest", snapshotContext.graphDigest},
          {"candidate_set_digest", snapshotContext.candidateSetDigest},
          {"candidate_artifact_set_digest", snapshotContext.candidateArtifactSetDigest},
          {"rtt_pairs", snapshotContext.allowedRttPairs},
          {"goodput_edges", snapshotContext.allowedGoodputEdges}}));
        NativeJson preparedRoles = NativeJson::array();
        for (const auto& role : input.roles)
          preparedRoles.push_back(nativeAssemblyJson(role));
        NativeJson providerOffers = NativeJson::array();
        for (const auto& offer : input.offers) {
          const auto provider = offer.observation().provider;
          providerOffers.push_back(NativeJson{
            {"provider", provider}, {"signer_identity", provider},
            {"signer_key_locator", provider + "/KEY/fixture/issuer/v=1"},
            {"offer_digest", offer.observation().offerDigest},
            {"ack_wire_digest", nativePlanningDigest("fixture-ack-" + provider)}});
        }
        std::sort(providerOffers.begin(), providerOffers.end(), [](const auto& left, const auto& right) {
          return left.at("provider").template get<std::string>() <
            right.at("provider").template get<std::string>();
        });
        NativeJson requesterSamples = NativeJson::array();
        for (const auto& provider : snapshotContext.candidateProviders)
          requesterSamples.push_back(NativeJson{{"provider", provider}, {"sample", nullptr}});
        snapshotProposal.context.placementObservationSnapshot = NativeJson{
          {"schema", "ndnsf-placement-observation-snapshot-v4"},
          {"request_id", snapshotContext.requestId},
          {"attempt", snapshotContext.attempt},
          {"requester_identity", snapshotContext.requesterIdentity},
          {"service", snapshotContext.serviceName},
          {"model_digest", snapshotContext.modelDigest},
          {"graph_digest", snapshotContext.graphDigest},
          {"ack_closed_digest", proposal.ackClosedDigest},
          {"candidate_set_digest", snapshotContext.candidateSetDigest},
          {"candidate_artifact_set_digest", snapshotContext.candidateArtifactSetDigest},
          {"probe_scope_digest", snapshotContext.probeScopeDigest},
          {"candidate_artifacts", snapshotContext.candidateArtifacts},
          {"provider_roster", snapshotContext.candidateProviders},
          {"rtt_pairs", snapshotContext.allowedRttPairs},
          {"goodput_edges", snapshotContext.allowedGoodputEdges},
          {"prepared_role_graph", NativeJson::array({NativeJson{
            {"candidate_digest", input.split.candidateDigest},
            {"graph_digest", input.split.graphDigest},
            {"node_roles", input.split.nodeRoles}, {"roles", preparedRoles},
            {"dependencies", NativeJson::array()}}})},
          {"requester_provider_samples", requesterSamples},
          {"provider_offers", providerOffers},
          {"eligible_link_observations", NativeJson::array()},
          {"ineligible_link_observation_count", 0},
          {"plan_deadline_ms", snapshotContext.deadlineMs},
          {"captured_at_ms", now}, {"expires_at_ms", snapshotExpiry}};
        snapshotProposal.context.placementObservationSnapshotDigest =
          nativePlanningDigest(nativeCanonicalJson(
            snapshotProposal.context.placementObservationSnapshot));
        snapshotInputs.placementObservationSnapshotDigest =
          snapshotProposal.context.placementObservationSnapshotDigest;
        const auto snapshotCore = NativePlanSealer::sealCore(input.inspected,
          input.split, snapshotProposal, execution, input.offers,
          input.ackDigest, snapshotInputs);
        BOOST_CHECK_EQUAL(snapshotCore.expiresAtMs, expiry);
        BOOST_CHECK_EQUAL(snapshotCore.placementObservationSnapshotDigest,
          snapshotInputs.placementObservationSnapshotDigest);
        BOOST_CHECK(snapshotCore.coreDigest != core.coreDigest);
        auto tamperedSnapshot = snapshotProposal;
        tamperedSnapshot.context.placementObservationSnapshot["expires_at_ms"] = expiry - 1;
        BOOST_CHECK_THROW(NativePlanSealer::sealCore(input.inspected, input.split,
          tamperedSnapshot, execution, input.offers, input.ackDigest,
          snapshotInputs), std::invalid_argument);
        auto mismatchedExpiry = snapshotInputs;
        mismatchedExpiry.expiresAtMs = expiry + 1;
        BOOST_CHECK_THROW(NativePlanSealer::sealCore(input.inspected, input.split,
          snapshotProposal, execution, input.offers, input.ackDigest,
          mismatchedExpiry), std::invalid_argument);

        auto expiredSnapshotProposal = snapshotProposal;
        expiredSnapshotProposal.context.placementObservationSnapshot["expires_at_ms"] = now;
        expiredSnapshotProposal.context.placementObservationSnapshotDigest =
          nativePlanningDigest(nativeCanonicalJson(
            expiredSnapshotProposal.context.placementObservationSnapshot));
        auto expiredSnapshotInputs = snapshotInputs;
        expiredSnapshotInputs.placementObservationSnapshotDigest =
          expiredSnapshotProposal.context.placementObservationSnapshotDigest;
        BOOST_CHECK_THROW(NativePlanSealer::sealCore(input.inspected, input.split,
          expiredSnapshotProposal, execution, input.offers, input.ackDigest,
          expiredSnapshotInputs), detail::NativePlacementObservationExpired);

        const auto key = [](char value) {
          const std::string seed(32, value);
          return std::shared_ptr<EVP_PKEY>(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
            reinterpret_cast<const unsigned char*>(seed.data()), seed.size()), EVP_PKEY_free);
        };
        const auto authorityKey = key('a');
        std::string authorityPublic(32, '\0'); std::size_t size = authorityPublic.size();
        BOOST_REQUIRE_EQUAL(EVP_PKEY_get_raw_public_key(authorityKey.get(),
          reinterpret_cast<unsigned char*>(authorityPublic.data()), &size), 1);
        NativeGrantIssuerConfig config;
        config.authorityIdentity = "/authority"; config.requesterIdentity = core.requesterIdentity;
        config.protectionEpoch = core.protectionEpoch; config.keyId = "fixture-content-key";
        config.authorityPrivateKey = authorityKey; config.requesterPublicKey = key('b');
        config.allowedModelManifests = {core.artifacts.manifestDigest};
        const auto provider = core.assignment.providerByRole.begin()->second;
        config.recipientPublicKeys = {{provider, key('c')}};
        config.contentKey = [](const auto&, const auto&) { return std::vector<std::uint8_t>(32, 42); };
        auto issuer = std::make_shared<NativeArtifactGrantIssuer>(config);
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        NativeGrantControl grantControl{std::chrono::system_clock::now() + std::chrono::seconds(5), cancelled};
        unsigned publications = 0; int mode = 0;
        NativeAuthenticatedGrantClient client(core.requesterIdentity, key('b'), "/authority", authorityPublic,
          issuer, [&](const std::string& name, const std::string&, const NativeGrantControl&) {
            ++publications;
            if (mode == 1) cancelled->store(true);
            return mode == 2 ? name + "/wrong" : name;
          }, [now] { return now; });
        const auto admitted = std::find_if(input.offers.begin(), input.offers.end(), [&](const auto& o) {
          return o.observation().provider == provider;
        });
        BOOST_REQUIRE(admitted != input.offers.end());
        NativeSecurityPolicySnapshot policy{nativePlanningDigest("real-issuer-policy"), true};
        const auto binding = client.acquire(core, *admitted, policy, grantControl);
        auto opened = verifyAndUnwrapNativeGrant(binding.wireJson, authorityPublic,
          {NativeRecipientKey::Kind::Ed25519Seed, std::string(32, 'c')}, provider,
          core.requestId, core.attempt, core.coreDigest, core.artifacts.manifestDigest,
          core.protectionEpoch, now, "/authority", binding.grantDigest);
        BOOST_REQUIRE_MESSAGE(opened.verified, opened.reason);
        BOOST_CHECK(opened.contentKey == std::vector<std::uint8_t>(32, 42));
        const auto authorized = NativePlanSealer::finalizeSecurity(core, {binding}, policy);
        const auto projections = NativePlanProjectionBuilder::build(authorized, input.split, input.offers,
          NativeProjectionContext{now, 1000, 4096});
        BOOST_CHECK_NO_THROW(NativePlanSealer::project(authorized, provider, projections.begin()->second));
        mode = 1;
        BOOST_CHECK_THROW(client.acquire(core, *admitted, policy, grantControl), std::runtime_error);
        const auto prior = publications;
        BOOST_CHECK_THROW(client.acquire(core, *admitted, policy, grantControl), std::runtime_error);
        BOOST_CHECK_EQUAL(publications, prior);
        cancelled->store(false); mode = 2;
        BOOST_CHECK_THROW(client.acquire(core, *admitted, policy, grantControl), std::runtime_error);
        auto expired = grantControl; expired.deadline = std::chrono::system_clock::now();
        BOOST_CHECK_THROW(client.acquire(core, *admitted, policy, expired), std::runtime_error);
      }
      if (proposal.roles.size() == 1) {
        std::ifstream expectedFile("tests/fixtures/spec182/projection-oracle.json");
        BOOST_REQUIRE(expectedFile.good());
        const auto expected = NativeJson::parse(expectedFile).at(sample.at("name").get<std::string>());
        const auto sealed = projectionPlan(core);
        NativeProjectionContext context{now, 1000, 4096};
        const auto values = NativePlanProjectionBuilder::build(sealed, input.split, input.offers, context);
        BOOST_REQUIRE_EQUAL(values.size(), 1);
        const auto& value = values.begin()->second;
        BOOST_CHECK_EQUAL(sealed.planDigest, expected.at("plan_digest").get<std::string>());
        BOOST_CHECK_EQUAL(nativeCanonicalJson(nativeDataflowJson(value.dataflow)), nativeCanonicalJson(expected.at("dataflow")));
        BOOST_CHECK_EQUAL(nativeCanonicalJson(nativeDeviceBindingJson(value.deviceBinding)), nativeCanonicalJson(expected.at("device")));
        BOOST_CHECK_EQUAL(value.executionRole.roleId, core.assemblyByRole.begin()->first);
        BOOST_CHECK_EQUAL(value.executionRole.backend, core.assemblyByRole.begin()->second.backend);
        BOOST_CHECK_THROW(NativePlanProjectionBuilder::build(sealed, input.split, {}, context), std::invalid_argument);
        auto expired = context; expired.nowMs = core.expiresAtMs;
        BOOST_CHECK_THROW(NativePlanProjectionBuilder::build(sealed, input.split, input.offers, expired), std::invalid_argument);
        auto extra = context; extra.dependencies[0] = {};
        BOOST_CHECK_THROW(NativePlanProjectionBuilder::build(sealed, input.split, input.offers, extra), std::invalid_argument);
      }
      BOOST_CHECK_EQUAL(core.graphDigest, input.context.graphDigest);
      BOOST_CHECK_EQUAL(core.artifacts.canonicalGraphDigest, input.inspected.canonicalGraphDigest);
      BOOST_CHECK_EQUAL(core.assemblyByRole.begin()->second.graphDigest, input.inspected.canonicalGraphDigest);
      if (sample.contains("root_json")) {
        BOOST_CHECK_NE(core.artifacts.manifestDigest, input.inspected.modelManifestDigest);
        BOOST_CHECK(core.assignment.providerByRole == proposal.providerByRole);
        for (std::size_t i = 0; i < proposal.roles.size(); ++i) {
          const auto& before = proposal.roles[i];
          const auto& after = core.assemblyByRole.at(before.selectedRole);
          BOOST_CHECK_EQUAL(after.recipeDigest, sample.at("published_recipe_digests")[i].get<std::string>());
          BOOST_CHECK_EQUAL(after.modelManifestDigest, core.artifacts.manifestDigest);
          BOOST_CHECK_EQUAL(after.artifactDigest, before.artifactDigest);
          BOOST_CHECK(after.deviceSet == before.deviceSet);
          BOOST_CHECK_NE(core.artifacts.artifactNameByRole.at(before.selectedRole),
                         core.artifacts.sourceByRole.at(before.selectedRole));
        }
        auto bad = inputs.artifacts;
        const auto reject = [&] { NativeRequestPreparation::bindPublishedRoles(input.inspected,
          input.split, proposal.roles, bad); };
        bad.canonicalManifestJson += " ";
        BOOST_CHECK_THROW(reject(), std::invalid_argument);
        for (const auto& field : {"modelIdentityDigest", "modelName", "artifactProfileDigest", "state"}) {
          bad = inputs.artifacts;
          auto root = nativeParseJson(bad.canonicalManifestJson); root[field] = "foreign";
          bad.canonicalManifestJson = root.dump(); bad.manifestDigest = nativePlanningDigest(bad.canonicalManifestJson);
          BOOST_CHECK_THROW(reject(), std::invalid_argument);
        }
        for (const auto& field : {"canonicalSourceDigest", "canonicalSourceBytes", "packageManifestDigest"}) {
          bad = inputs.artifacts;
          auto root = nativeParseJson(bad.canonicalManifestJson); root["metadata"][field] = "foreign";
          bad.canonicalManifestJson = root.dump(); bad.manifestDigest = nativePlanningDigest(bad.canonicalManifestJson);
          BOOST_CHECK_THROW(reject(), std::invalid_argument);
        }
        bad = inputs.artifacts; bad.artifactNameByRole.clear();
        BOOST_CHECK_THROW(reject(), std::invalid_argument);
        bad = inputs.artifacts; bad.canonicalManifestJson.clear();
        BOOST_CHECK_THROW(reject(), std::runtime_error);
        if (sample.at("name") == "published_rank_cover") {
          bad = inputs.artifacts;
          auto root = nativeParseJson(bad.canonicalManifestJson);
          root["metadata"]["canonicalInitializerObjectDigest"] = nativePlanningDigest("foreign");
          bad.canonicalManifestJson = root.dump(); bad.manifestDigest = nativePlanningDigest(bad.canonicalManifestJson);
          BOOST_CHECK_THROW(reject(), std::invalid_argument);
        }
      }
      if (sample.at("name") == "distinct_graph_spaces") {
        BOOST_CHECK_NE(core.graphDigest, core.artifacts.canonicalGraphDigest);
        auto changed = proposal;
        changed.roles[0].graphDigest = input.context.graphDigest;
        BOOST_CHECK_THROW(seal(changed), std::invalid_argument);
        auto changedCore = core;
        changedCore.artifacts.canonicalGraphDigest = input.context.graphDigest;
        BOOST_CHECK_THROW(changedCore.validate(), std::invalid_argument);
      }
      for (const auto& admitted : input.offers) {
        if (!core.offerDigestByProvider.count(admitted.observation().provider)) continue;
        const auto grant = NativePlanSealer::grantView(core, admitted, {nativePlanningDigest("policy"), true});
        BOOST_CHECK_EQUAL(grant.provider, admitted.observation().provider);
        BOOST_CHECK_EQUAL(grant.modelManifestDigest, core.artifacts.manifestDigest);
      }
      auto changed = proposal; changed.roles[0].artifactDigest = nativePlanningDigest("foreign");
      BOOST_CHECK_THROW(seal(changed), std::invalid_argument);
      changed = proposal; changed.ackClosedDigest = nativePlanningDigest("foreign");
      BOOST_CHECK_THROW(seal(changed), std::invalid_argument);
      changed = proposal; changed.offerDigestByProvider.begin()->second = nativePlanningDigest("foreign");
      BOOST_CHECK_THROW(seal(changed), std::invalid_argument);
      if (sample.at("name") == "loaded_second_device") {
        changed = proposal; changed.roles[0].deviceSet = {"cuda:0"};
        BOOST_CHECK_THROW(seal(changed), std::invalid_argument);
      }
      if (sample.at("name") == "cpu") {
        changed = proposal; changed.providerByRole.begin()->second = "/provider/b";
        changed.offerDigestByProvider = {{"/provider/b", input.offers[1].observation().offerDigest}};
        // A feasible custom choice need not equal default cost ordering.
        BOOST_CHECK_NO_THROW(seal(changed));
      }
      if (sample.at("name") == "rank_cover") {
        std::reverse(execution.roles.begin(), execution.roles.end());
        BOOST_CHECK_THROW(seal(proposal), std::invalid_argument);
      }
    }
  }
}
BOOST_AUTO_TEST_CASE(RealSdkPlacementAndExactReuseBoundaries)
{
  const auto f = oracle();
  for (const auto& sample : f.at("cases")) {
    BOOST_TEST_CONTEXT(sample.at("name").get<std::string>()) {
      Input input(f, sample);
      const NativePreSplitFirstPlacement implementation;
      const NativePlacementStrategy& strategy = implementation;
      if (sample.at("expected").is_null()) {
        BOOST_CHECK_THROW(strategy.proposeRoles(input.context, input.ackDigest, input.roles, input.offers, 200),
                          std::runtime_error);
        continue;
      }
      const auto result = strategy.proposeRoles(input.context, input.ackDigest, input.roles, input.offers, 200);
      const auto expected = sample.at("expected");
      BOOST_CHECK((result.providerByRole == expected.at("providers").get<std::map<std::string, std::string>>()));
      BOOST_REQUIRE_EQUAL(result.roles.size(), expected.at("roles").size());
      for (std::size_t i = 0; i < result.roles.size(); ++i) {
        BOOST_CHECK_EQUAL(result.roles[i].role, expected["roles"][i]["role"].get<std::string>());
        BOOST_CHECK_EQUAL(result.roles[i].rank, expected["roles"][i]["rank"].get<std::uint64_t>());
        BOOST_CHECK_EQUAL(result.providerByRole.count(result.roles[i].selectedRole), 1);
        BOOST_CHECK_EQUAL(result.roles[i].backend, expected["roles"][i]["backend"].get<std::string>());
        BOOST_CHECK(result.roles[i].deviceSet == expected["roles"][i]["device_set"].get<std::vector<std::string>>());
      }
      std::reverse(input.roles.begin(), input.roles.end());
      std::reverse(input.offers.begin(), input.offers.end());
      BOOST_CHECK(result.providerByRole == strategy.proposeRoles(input.context, input.ackDigest,
        input.roles, input.offers, 200).providerByRole);
    }
  }
}

BOOST_AUTO_TEST_CASE(ConversationAffinityPrecedesCostButNotFeasibility)
{
  const auto f = oracle();
  Input input(f, f.at("cases")[0]); // Both CPU Providers feasible; default chooses a.
  NativePreSplitFirstPlacement strategy;
  const auto role = input.roles.front().role;
  BOOST_CHECK_EQUAL(strategy.proposeRoles(input.context, input.ackDigest,
    input.roles, input.offers, 200).providerByRole.at(role), "/provider/a");
  input.context.preferredProvidersByRole = {{role, "/provider/b"}};
  auto result = strategy.proposeRoles(input.context, input.ackDigest, input.roles, input.offers, 200);
  BOOST_CHECK_EQUAL(result.providerByRole.at(role), "/provider/b");
  BOOST_CHECK_NO_THROW(validateNativeRolePlacement(result, input.roles, input.offers, 200));
  input.context.preferredProvidersByRole[role] = "/not-admitted";
  BOOST_CHECK_EQUAL(strategy.proposeRoles(input.context, input.ackDigest,
    input.roles, input.offers, 200).providerByRole.at(role), "/provider/a");
  // A preferred name cannot make an unsupported backend feasible.
  input.context.preferredProvidersByRole[role] = "/provider/b";
  input.roles.front().backend = "unsupported";
  BOOST_CHECK_THROW(strategy.proposeRoles(input.context, input.ackDigest,
    input.roles, input.offers, 200), NativeNoFeasiblePlacement);
}

BOOST_AUTO_TEST_CASE(ConversationAffinityPrecedesSpreadingAndPreservesDeviceExclusion)
{
  const auto f = oracle();
  Input input(f, f.at("cases")[11]); // rank_cover: two devices per Provider.
  for (const auto& role : input.roles)
    input.context.preferredProvidersByRole[role.role + "#" + std::to_string(role.rank)] = "/provider/a";
  const auto result = NativePreSplitFirstPlacement().proposeRoles(
    input.context, input.ackDigest, input.roles, input.offers, 200);
  BOOST_REQUIRE_EQUAL(result.providerByRole.size(), 2);
  for (const auto& item : result.providerByRole) BOOST_CHECK_EQUAL(item.second, "/provider/a");
  BOOST_CHECK(result.roles[0].deviceSet != result.roles[1].deviceSet);
  BOOST_CHECK_NO_THROW(validateNativeRolePlacement(result, input.roles, input.offers, 200));
}

BOOST_AUTO_TEST_CASE(V3PlacementCoLocatesRanksOnDistinctDevicesWhenTopologyIsSmall)
{
  const auto f = oracle();
  Input input(f, f.at("cases")[11]); // rank_cover: two ranks, two GPU devices per offer
  input.offers.erase(input.offers.begin() + 1, input.offers.end());
  const auto now = std::uint64_t{200};
  const auto proposal = NativePreSplitFirstPlacement().proposeRoles(
    input.context, input.ackDigest, input.roles, input.offers, now);
  BOOST_REQUIRE_EQUAL(proposal.roles.size(), 2);
  BOOST_CHECK_EQUAL(proposal.providerByRole.at(proposal.roles[0].selectedRole),
                    input.offers.front().observation().provider);
  BOOST_CHECK_EQUAL(proposal.providerByRole.at(proposal.roles[1].selectedRole),
                    input.offers.front().observation().provider);
  BOOST_REQUIRE_EQUAL(proposal.roles[0].deviceSet.size(), 1);
  BOOST_REQUIRE_EQUAL(proposal.roles[1].deviceSet.size(), 1);
  BOOST_CHECK_NE(proposal.roles[0].deviceSet.front(), proposal.roles[1].deviceSet.front());
  BOOST_CHECK_NO_THROW(validateNativeRolePlacement(
    proposal, input.roles, input.offers, now));
}

BOOST_AUTO_TEST_CASE(RejectForeignSnapshotAndIncompleteRankCover)
{
  const auto f = oracle();
  Input input(f, f.at("cases")[0]);
  NativePreSplitFirstPlacement strategy;
  auto bad = input.context; bad.modelDigest = "sha256:" + std::string(64, '1');
  BOOST_CHECK_THROW(strategy.proposeRoles(bad, input.ackDigest, input.roles, input.offers, 200), std::invalid_argument);
  const auto manifest = input.roles[0].modelManifestDigest;
  input.roles[0].modelManifestDigest = "invalid";
  BOOST_CHECK_THROW(strategy.proposeRoles(input.context, input.ackDigest, input.roles, input.offers, 200), std::invalid_argument);
  input.roles[0].modelManifestDigest = manifest;
  input.roles[0].rank = 1;
  BOOST_CHECK_THROW(strategy.proposeRoles(input.context, input.ackDigest, input.roles, input.offers, 200), std::invalid_argument);
  input.roles[0].rank = 0;
  input.roles.push_back(input.roles[0]);
  BOOST_CHECK_THROW(strategy.proposeRoles(input.context, input.ackDigest, input.roles, input.offers, 200), std::invalid_argument);
  input.roles.pop_back();
  input.offers.push_back(input.offers[0]);
  BOOST_CHECK_THROW(strategy.proposeRoles(input.context, input.ackDigest, input.roles, input.offers, 200), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(Spec194ChirpOnlyReportsConservativeBoundWhenGapsDoNotExpand)
{
  const std::array<double, 3> sendGaps{8.0, 4.0, 2.0};
  const std::array<std::uint64_t, 4> wireBytes{4096, 4096, 4096, 4096};
  const auto clear = estimateNativeChirpBandwidthV4(
    sendGaps, sendGaps, wireBytes, true, true);
  BOOST_CHECK_EQUAL(clear.status, "LOWER_BOUND_ONLY");
  BOOST_CHECK_EQUAL(clear.reason, "NO_OBSERVED_GAP_EXPANSION");
  BOOST_REQUIRE(clear.lowerMbps);
  BOOST_CHECK_CLOSE(*clear.lowerMbps, 16.384, 1e-9);
  BOOST_CHECK(!clear.upperMbps);

  const auto expanded = estimateNativeChirpBandwidthV4(
    sendGaps, std::array<double, 3>{8.0, 5.2, 2.0}, wireBytes, true, true);
  BOOST_CHECK_EQUAL(expanded.status, "UNKNOWN");
  BOOST_CHECK_EQUAL(expanded.reason, "PROVIDER_DELAY_AMBIGUOUS");
  BOOST_CHECK(!expanded.lowerMbps);
  BOOST_CHECK(!expanded.upperMbps);

  const auto invalid = estimateNativeChirpBandwidthV4(
    sendGaps, sendGaps, wireBytes, true, false);
  BOOST_CHECK_EQUAL(invalid.reason, "INCOMPLETE_OR_INVALID_SIGNATURE");
  BOOST_CHECK(!invalid.lowerMbps);
}

BOOST_AUTO_TEST_CASE(Spec194V4FallbackKeepsACompleteValidatedBaselineAssignment)
{
  const auto f = oracle();
  Input input(f, f.at("cases")[0]);
  const NativeObservationAwarePlacementV4 strategy;
  const auto proposal = strategy.proposeRoles(
    input.context, input.ackDigest, input.roles, input.offers, 200);
  BOOST_CHECK_EQUAL(proposal.strategy.name, "native-observation-aware-v4");
  BOOST_REQUIRE(proposal.placementDecisionV4);
  BOOST_CHECK_EQUAL(proposal.placementDecisionV4->at("status").get<std::string>(),
                    "BASELINE_FALLBACK");
  BOOST_CHECK_EQUAL(proposal.placementDecisionV4->at("fallback_reason").get<std::string>(),
                    "SNAPSHOT_UNAVAILABLE");
  BOOST_CHECK(proposal.placementDecisionV4->at("snapshot_digest").is_null());
  BOOST_REQUIRE_EQUAL(proposal.roles.size(), input.roles.size());
  BOOST_CHECK_NO_THROW(validateNativeRolePlacement(
    proposal, input.roles, input.offers, 200));

  auto tampered = proposal;
  tampered.placementDecisionV4->at("selected_assignments")[0]["provider"] = "/provider/foreign";
  BOOST_CHECK_THROW(validateNativeRolePlacement(
    tampered, input.roles, input.offers, 200), std::invalid_argument);
  BOOST_CHECK_EQUAL(strategy.identity().configurationDigest,
                    NativeObservationAwarePlacementV4().identity().configurationDigest);
  BOOST_CHECK_NE(strategy.identity().configurationDigest,
                 NativeObservationAwarePlacementV4(16, 2048).identity().configurationDigest);
}

BOOST_AUTO_TEST_CASE(Spec194V4FallbackUsesFreshAuthenticatedNonNetworkOfferFacts)
{
  const auto f = oracle();
  Input input(f, f.at("cases")[0]);
  const NativeObservationAwarePlacementV4 strategy;
  const auto proposalFor = [&](NativeJson snapshot) {
    input.context.placementObservationSnapshot = std::move(snapshot);
    input.context.placementObservationSnapshotDigest = nativePlanningDigest(
      nativeCanonicalJson(input.context.placementObservationSnapshot));
    return strategy.proposeRoles(
      input.context, input.ackDigest, input.roles, input.offers, 200);
  };
  const auto forceBaseline = [&](NativeJson& snapshot) {
    snapshot["prepared_role_graph"] = NativeJson::array();
  };
  const auto setKnownEmpty = [&](NativeJson& snapshot) {
    for (auto& offer : snapshot["provider_offers"]) {
      offer["values"]["residency_state"] = "KNOWN_EMPTY";
      offer["values"]["residency"] = NativeJson::array();
    }
  };
  const auto setQueueDepth = [](NativeJson& offer, std::uint64_t depth) {
    auto& values = offer["values"];
    values["capacity_state"] = "OBSERVED";
    values["capacity"]["ready_queue_depth"] = depth;
    values["capacity"]["ready_queue_capacity"] = depth;
    values["queue_depth"] = depth;
  };

  auto residencySnapshot = makeV4PlacementSnapshot(
    input, input.roles, NativeJson(nullptr), false);
  forceBaseline(residencySnapshot);
  residencySnapshot["provider_offers"][0]["values"]["residency_state"] = "KNOWN_EMPTY";
  residencySnapshot["provider_offers"][0]["values"]["residency"] = NativeJson::array();
  const auto residencyProposal = proposalFor(residencySnapshot);
  BOOST_REQUIRE(residencyProposal.placementDecisionV4);
  BOOST_CHECK_EQUAL(residencyProposal.placementDecisionV4->at("fallback_reason").get<std::string>(),
                    "PREPARED_ROLE_GRAPH_MISSING");
  BOOST_CHECK_EQUAL(residencyProposal.providerByRole.begin()->second, "/provider/b");
  BOOST_CHECK_NO_THROW(validateNativeRolePlacement(
    residencyProposal, input.roles, input.offers, 200));

  auto knownMissSnapshot = makeV4PlacementSnapshot(
    input, input.roles, NativeJson(nullptr), false);
  forceBaseline(knownMissSnapshot);
  for (auto& offer : knownMissSnapshot["provider_offers"]) {
    const bool preferred = offer.at("provider") == "/provider/b";
    for (auto& row : offer["values"]["residency"]) {
      row["state"] = "KNOWN_MISS";
      row["runtime_generation"] = nullptr;
      row["estimated_assembly_ms"] = preferred ? 1.0 : 50.0;
      row["estimated_load_ms"] = preferred ? 1.0 : 50.0;
    }
  }
  const auto knownMissProposal = proposalFor(knownMissSnapshot);
  BOOST_CHECK_EQUAL(knownMissProposal.providerByRole.begin()->second, "/provider/b");

  auto queueSnapshot = makeV4PlacementSnapshot(
    input, input.roles, NativeJson(nullptr), false);
  forceBaseline(queueSnapshot);
  setKnownEmpty(queueSnapshot);
  setQueueDepth(queueSnapshot["provider_offers"][0], 20);
  setQueueDepth(queueSnapshot["provider_offers"][1], 0);
  const auto queueProposal = proposalFor(queueSnapshot);
  BOOST_CHECK_EQUAL(queueProposal.providerByRole.begin()->second, "/provider/b");

  auto waitSnapshot = makeV4PlacementSnapshot(
    input, input.roles, NativeJson(nullptr), false);
  forceBaseline(waitSnapshot);
  setKnownEmpty(waitSnapshot);
  for (auto& offer : waitSnapshot["provider_offers"]) {
    offer["values"]["capacity_state"] = "UNKNOWN";
    offer["values"]["capacity"] = nullptr;
    offer["values"]["queue_depth"] = nullptr;
    offer["values"]["estimated_wait_ms"] =
      offer.at("provider") == "/provider/a" ? 20.0 : 1.0;
  }
  const auto waitProposal = proposalFor(waitSnapshot);
  BOOST_CHECK_EQUAL(waitProposal.providerByRole.begin()->second, "/provider/b");

  auto unknownResidencySnapshot = makeV4PlacementSnapshot(
    input, input.roles, NativeJson(nullptr), false);
  forceBaseline(unknownResidencySnapshot);
  for (auto& offer : unknownResidencySnapshot["provider_offers"]) {
    if (offer.at("provider") == "/provider/a") {
      offer["values"]["residency_state"] = "UNKNOWN";
      offer["values"]["residency"] = nullptr;
      continue;
    }
    for (auto& row : offer["values"]["residency"]) {
      row["state"] = "KNOWN_MISS";
      row["runtime_generation"] = nullptr;
      row["estimated_assembly_ms"] = 50.0;
      row["estimated_load_ms"] = 50.0;
    }
  }
  const auto unknownResidencyProposal = proposalFor(unknownResidencySnapshot);
  BOOST_CHECK_EQUAL(unknownResidencyProposal.providerByRole.begin()->second, "/provider/b");

  auto unknownQueueSnapshot = makeV4PlacementSnapshot(
    input, input.roles, NativeJson(nullptr), false);
  forceBaseline(unknownQueueSnapshot);
  setKnownEmpty(unknownQueueSnapshot);
  unknownQueueSnapshot["provider_offers"][0]["values"]["capacity_state"] = "UNKNOWN";
  unknownQueueSnapshot["provider_offers"][0]["values"]["capacity"] = nullptr;
  unknownQueueSnapshot["provider_offers"][0]["values"]["queue_depth"] = nullptr;
  setQueueDepth(unknownQueueSnapshot["provider_offers"][1], 5);
  const auto unknownQueueProposal = proposalFor(unknownQueueSnapshot);
  BOOST_CHECK_EQUAL(unknownQueueProposal.providerByRole.begin()->second, "/provider/b");

  auto unknownWaitSnapshot = makeV4PlacementSnapshot(
    input, input.roles, NativeJson(nullptr), false);
  forceBaseline(unknownWaitSnapshot);
  setKnownEmpty(unknownWaitSnapshot);
  for (auto& offer : unknownWaitSnapshot["provider_offers"]) {
    offer["values"]["capacity_state"] = "UNKNOWN";
    offer["values"]["capacity"] = nullptr;
    offer["values"]["queue_depth"] = nullptr;
    offer["values"]["estimated_wait_ms"] = nullptr;
    if (offer.at("provider") == "/provider/b")
      offer["values"]["estimated_wait_ms"] = 5.0;
  }
  const auto unknownWaitProposal = proposalFor(unknownWaitSnapshot);
  BOOST_CHECK_EQUAL(unknownWaitProposal.providerByRole.begin()->second, "/provider/b");

  auto allUnknownSnapshot = makeV4PlacementSnapshot(
    input, input.roles, NativeJson(nullptr), false);
  forceBaseline(allUnknownSnapshot);
  for (auto& offer : allUnknownSnapshot["provider_offers"]) {
    offer["values"]["residency_state"] = "UNKNOWN";
    offer["values"]["residency"] = nullptr;
    offer["values"]["capacity_state"] = "UNKNOWN";
    offer["values"]["capacity"] = nullptr;
    offer["values"]["queue_depth"] = nullptr;
    offer["values"]["estimated_wait_ms"] = nullptr;
  }
  const auto unknownProposal = proposalFor(allUnknownSnapshot);
  BOOST_CHECK_EQUAL(unknownProposal.providerByRole.begin()->second, "/provider/a");

  auto unauthenticatedSnapshot = makeV4PlacementSnapshot(
    input, input.roles, NativeJson(nullptr), false);
  forceBaseline(unauthenticatedSnapshot);
  unauthenticatedSnapshot["provider_offers"][0]["values"]["residency_state"] = "KNOWN_EMPTY";
  unauthenticatedSnapshot["provider_offers"][0]["values"]["residency"] = NativeJson::array();
  unauthenticatedSnapshot["provider_offers"][1]["signer_identity"] = "/provider/attacker";
  const auto unauthenticatedProposal = proposalFor(unauthenticatedSnapshot);
  BOOST_CHECK_EQUAL(unauthenticatedProposal.providerByRole.begin()->second, "/provider/a");
}

BOOST_AUTO_TEST_CASE(Spec194V4RanksCompleteAssignmentsByObservedControlPath)
{
  const auto f = oracle();
  Input input(f, f.at("cases")[0]);
  auto context = input.context;
  context.requesterIdentity = "/requester";
  NativeJson providerOffers = NativeJson::array();
  NativeJson requesterSamples = NativeJson::array();
  for (const auto& admitted : input.offers) {
    const auto& offer = admitted.observation();
    const auto& role = input.roles.front();
    auto backend = role.backend;
    const auto& suffix = offer.devices.empty() ? "-cpu" : "-cuda";
    if (std::find(offer.backends.begin(), offer.backends.end(), backend + suffix) !=
        offer.backends.end())
      backend += suffix;
    constexpr double waitMs = 0.0;
    providerOffers.push_back(NativeJson{
      {"provider", offer.provider}, {"boot_epoch", offer.bootEpoch},
      {"captured_at_ms", std::uint64_t{190}},
      {"expires_at_ms", context.deadlineMs},
      {"offer_digest", offer.offerDigest}, {"signer_identity", offer.provider},
      {"signer_key_locator", offer.provider + "/KEY/fixture/issuer/v=1"},
      {"ack_wire_digest", "sha256:" + std::string(64, '1')},
      {"values", NativeJson{
        {"queue_depth", std::uint64_t{0}}, {"estimated_wait_ms", waitMs},
        {"residency_state", "OBSERVED"},
        {"residency", NativeJson::array({NativeJson{
          {"artifact_digest", role.artifactDigest}, {"role", role.role},
          {"rank", role.rank}, {"model_manifest_digest", role.modelManifestDigest},
          {"graph_digest", role.graphDigest}, {"backend", backend},
          {"device_set", role.deviceSet}, {"protection_epoch", role.protectionEpoch},
          {"state", "EXACT_LOADED"}, {"process_epoch", offer.bootEpoch},
          {"captured_at_ms", std::uint64_t{190}},
          {"expires_at_ms", context.deadlineMs},
          {"runtime_generation", "fixture-runtime"},
          {"missing_verified_bytes", std::uint64_t{0}},
          {"estimated_assembly_ms", nullptr}, {"estimated_load_ms", nullptr}}})}}}});
    const auto rttMs = offer.provider == "/provider/a" ? 10.0 : 3.0;
    requesterSamples.push_back(NativeJson{
      {"provider", offer.provider},
      {"sample", NativeJson{{"provider", offer.provider}, {"service", offer.service},
        {"kind", "ACK_RTT"}, {"rtt_ms", rttMs}, {"observed_at_ms", std::uint64_t{190}},
        {"peer_boot_epoch", offer.bootEpoch}}}});
  }
  NativeJson preparedRoles = NativeJson::array();
  for (const auto& role : input.roles)
    preparedRoles.push_back(nativeAssemblyJson(role));
  context.placementObservationSnapshot = NativeJson{
    {"schema", "ndnsf-placement-observation-snapshot-v4"},
    {"request_id", context.requestId}, {"attempt", context.attempt},
    {"requester_identity", context.requesterIdentity}, {"service", context.serviceName},
    {"model_digest", context.modelDigest}, {"graph_digest", context.graphDigest},
    {"ack_closed_digest", input.ackDigest}, {"provider_roster", {"/provider/a", "/provider/b"}},
    {"prepared_role_graph", NativeJson::array({NativeJson{
      {"candidate_digest", input.split.candidateDigest},
      {"graph_digest", input.split.graphDigest}, {"node_roles", input.split.nodeRoles},
      {"stage_order", input.split.executionPlan.roles}, {"roles", preparedRoles},
      {"dependencies", NativeJson::array()}}})},
    {"provider_offers", providerOffers}, {"requester_provider_samples", requesterSamples},
    {"eligible_link_observations", NativeJson::array()},
    {"captured_at_ms", std::uint64_t{190}}, {"expires_at_ms", context.deadlineMs}};
  context.placementObservationSnapshotDigest = nativePlanningDigest(
    nativeCanonicalJson(context.placementObservationSnapshot));

  const auto proposal = NativeObservationAwarePlacementV4().proposeRoles(
    context, input.ackDigest, input.roles, input.offers, 200);
  BOOST_REQUIRE(proposal.placementDecisionV4);
  BOOST_CHECK_EQUAL(proposal.placementDecisionV4->at("status").get<std::string>(), "SCORED");
  BOOST_CHECK_EQUAL(proposal.providerByRole.begin()->second, "/provider/b");
  BOOST_CHECK_EQUAL(proposal.placementDecisionV4->at("control_path_ms").get<double>(), 3.0);
  BOOST_CHECK_EQUAL(proposal.placementDecisionV4->at("data_flow_critical_path_ms").get<double>(), 0.0);
  BOOST_CHECK_EQUAL(proposal.placementDecisionV4->at("score_ms").get<double>(), 3.0);
  BOOST_CHECK_NO_THROW(validateNativeRolePlacement(proposal, input.roles, input.offers, 200));

  Input sealInput(f, f.at("seal_cases")[0]);
  auto fallbackContext = sealInput.context;
  fallbackContext.requesterIdentity = "/requester";
  const auto fallbackProposal = NativeObservationAwarePlacementV4().proposeRoles(
    fallbackContext, sealInput.ackDigest, sealInput.roles, sealInput.offers, 200);
  BOOST_REQUIRE(fallbackProposal.placementDecisionV4);
  BOOST_CHECK_EQUAL(fallbackProposal.placementDecisionV4->at("status").get<std::string>(),
                    "BASELINE_FALLBACK");
  const auto sealedWithDecision = sealV4Proposal(sealInput, fallbackProposal);
  auto withoutDecision = fallbackProposal;
  withoutDecision.placementDecisionV4.reset();
  const auto sealedWithoutDecision = sealV4Proposal(sealInput, withoutDecision);
  BOOST_CHECK_NE(sealedWithDecision.coreDigest, sealedWithoutDecision.coreDigest);

  const auto repeated = NativeObservationAwarePlacementV4().proposeRoles(
    context, input.ackDigest, input.roles, input.offers, 200);
  BOOST_CHECK_EQUAL(nativeCanonicalJson(*proposal.placementDecisionV4),
                    nativeCanonicalJson(*repeated.placementDecisionV4));

  auto tiedContext = context;
  for (auto& peerSample : tiedContext.placementObservationSnapshot["requester_provider_samples"])
    peerSample["sample"]["rtt_ms"] = 5.0;
  tiedContext.placementObservationSnapshotDigest = nativePlanningDigest(
    nativeCanonicalJson(tiedContext.placementObservationSnapshot));
  const auto tied = NativeObservationAwarePlacementV4().proposeRoles(
    tiedContext, input.ackDigest, input.roles, input.offers, 200);
  BOOST_REQUIRE(tied.placementDecisionV4);
  BOOST_CHECK_EQUAL(tied.placementDecisionV4->at("score_ms").get<double>(), 5.0);
  BOOST_CHECK_EQUAL(tied.providerByRole.begin()->second, "/provider/a");

  const auto bounded = NativeObservationAwarePlacementV4(32, 2).proposeRoles(
    context, input.ackDigest, input.roles, input.offers, 200);
  BOOST_REQUIRE(bounded.placementDecisionV4);
  BOOST_CHECK_EQUAL(bounded.placementDecisionV4->at("status").get<std::string>(),
                    "BASELINE_FALLBACK");
  BOOST_CHECK_EQUAL(bounded.placementDecisionV4->at("fallback_reason").get<std::string>(),
                    "MAX_ASSIGNMENT_STATES");
  BOOST_CHECK_EQUAL(bounded.roles.size(), input.roles.size());
  BOOST_CHECK_NO_THROW(validateNativeRolePlacement(bounded, input.roles, input.offers, 200));

  const ExtensionControl exhaustedPolicy{
    std::chrono::steady_clock::now() - std::chrono::milliseconds(1), [] { return false; }};
  const auto timedOut = NativeObservationAwarePlacementV4().proposeRoles(
    context, input.ackDigest, input.roles, input.offers, 200, exhaustedPolicy);
  BOOST_REQUIRE(timedOut.placementDecisionV4);
  BOOST_CHECK_EQUAL(timedOut.placementDecisionV4->at("status").get<std::string>(),
                    "BASELINE_FALLBACK");
  BOOST_CHECK_EQUAL(timedOut.placementDecisionV4->at("fallback_reason").get<std::string>(),
                    "POLICY_BUDGET_EXHAUSTED");
  BOOST_CHECK_EQUAL(timedOut.roles.size(), input.roles.size());

  auto requestDeadlineContext = context;
  requestDeadlineContext.deadlineMs = 201;
  requestDeadlineContext.placementObservationSnapshot["expires_at_ms"] = 201;
  requestDeadlineContext.placementObservationSnapshotDigest = nativePlanningDigest(
    nativeCanonicalJson(requestDeadlineContext.placementObservationSnapshot));
  const ExtensionControl requestDeadline{
    std::chrono::steady_clock::now() + std::chrono::milliseconds(1), [] {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      return false;
    }};
  BOOST_CHECK_EXCEPTION(
    NativeObservationAwarePlacementV4().proposeRoles(
      requestDeadlineContext, input.ackDigest, input.roles, input.offers, 200,
      requestDeadline),
    std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()) == "native placement request deadline exceeded";
    });
}

BOOST_AUTO_TEST_CASE(Spec194V4PricesDirectedTransferAndKeepsUnknownInputsUnavailable)
{
  const auto f = oracle();
  Input input(f, f.at("cases")[11]);
  const NativeObservationAwarePlacementV4 strategy;
  const auto estimatedBytes = NativeJson(std::uint64_t{1'000'000});
  const auto baseSnapshot = makeV4PlacementSnapshot(
    input, input.roles, estimatedBytes, true);
  const auto decide = [&] (NativeJson snapshot) {
    input.context.placementObservationSnapshot = std::move(snapshot);
    input.context.placementObservationSnapshotDigest = nativePlanningDigest(
      nativeCanonicalJson(input.context.placementObservationSnapshot));
    return strategy.proposeRoles(input.context, input.ackDigest, input.roles,
                                 input.offers, 200);
  };

  const auto scored = decide(baseSnapshot);
  BOOST_REQUIRE(scored.placementDecisionV4);
  BOOST_CHECK_EQUAL(scored.placementDecisionV4->at("status").get<std::string>(), "SCORED");
  const auto producerKey = input.roles.front().role + "#" +
    std::to_string(input.roles.front().rank);
  const auto consumerKey = input.roles.back().role + "#"+
    std::to_string(input.roles.back().rank);
  BOOST_CHECK_EQUAL(scored.providerByRole.at(producerKey), "/provider/a");
  BOOST_CHECK_EQUAL(scored.providerByRole.at(consumerKey), "/provider/b");
  BOOST_CHECK_EQUAL(scored.placementDecisionV4->at("control_path_ms").get<double>(), 10.0);
  BOOST_CHECK_EQUAL(scored.placementDecisionV4->at("data_flow_critical_path_ms").get<double>(), 1000.0);
  BOOST_CHECK_EQUAL(scored.placementDecisionV4->at("score_ms").get<double>(), 1010.0);
  BOOST_REQUIRE_EQUAL(scored.placementDecisionV4->at("edge_costs").size(), 1U);
  BOOST_CHECK_EQUAL(scored.placementDecisionV4->at("edge_costs")[0]
    .at("rate_method").get<std::string>(), "ACTIVE_PROBE_GOODPUT");
  BOOST_CHECK_EQUAL(scored.placementDecisionV4->at("edge_costs")[0]
    .at("transfer_ms").get<double>(), 1000.0);
  BOOST_CHECK_NO_THROW(validateNativeRolePlacement(
    scored, input.roles, input.offers, 200));

  auto stageEndpointSnapshot = baseSnapshot;
  stageEndpointSnapshot["prepared_role_graph"][0]["dependencies"][0]["producer_role"] =
    input.roles.front().role;
  stageEndpointSnapshot["prepared_role_graph"][0]["dependencies"][0]["consumer_role"] =
    input.roles.back().role;
  const auto sameStageEdge = decide(std::move(stageEndpointSnapshot));
  BOOST_REQUIRE(sameStageEdge.placementDecisionV4);
  BOOST_CHECK_EQUAL(sameStageEdge.placementDecisionV4->at("status").get<std::string>(),
                    "BASELINE_FALLBACK");
  BOOST_CHECK_EQUAL(sameStageEdge.placementDecisionV4->at("fallback_reason").get<std::string>(),
                    "ROLE_DAG_NOT_TOPOLOGICAL");

  auto reversedRankEdgeSnapshot = baseSnapshot;
  reversedRankEdgeSnapshot["prepared_role_graph"][0]["dependencies"][0]["producer_role"] =
    consumerKey;
  reversedRankEdgeSnapshot["prepared_role_graph"][0]["dependencies"][0]["consumer_role"] =
    producerKey;
  const auto reversedRankEdge = decide(std::move(reversedRankEdgeSnapshot));
  BOOST_REQUIRE(reversedRankEdge.placementDecisionV4);
  BOOST_CHECK_EQUAL(reversedRankEdge.placementDecisionV4->at("status").get<std::string>(),
                    "BASELINE_FALLBACK");
  BOOST_CHECK_EQUAL(reversedRankEdge.placementDecisionV4->at("fallback_reason").get<std::string>(),
                    "ROLE_DAG_NOT_TOPOLOGICAL");

  auto activeProbeSnapshot = baseSnapshot;
  auto& activeRow = activeProbeSnapshot["eligible_link_observations"][0]["row"];
  activeRow["kind"] = "ACTIVE_PROBE_GOODPUT";
  activeRow["payload_bytes"] = std::uint64_t{8192};
  activeRow["elapsed_ms"] = 8.192;
  activeRow["probe_complete"] = true;
  activeRow["probe_segments_sent"] = std::uint64_t{16};
  activeRow["probe_profile_digest"] = ndn_service_framework::NETWORK_PROBE_TRAIN_PROFILE_DIGEST;
  activeRow["probe_signature_valid"] = true;
  activeRow["probe_interest_gaps_ms"] = NativeJson(std::vector<double>(15, 0.01));
  activeRow["probe_data_arrival_gaps_ms"] = NativeJson(std::vector<double>(15, 4.096));
  activeRow["probe_data_wire_bytes"] = NativeJson::array(
    {std::uint64_t{4096}, std::uint64_t{4096}, std::uint64_t{4096}, std::uint64_t{4096}});
  const auto activeScored = decide(std::move(activeProbeSnapshot));
  BOOST_REQUIRE(activeScored.placementDecisionV4);
  BOOST_CHECK_EQUAL(activeScored.placementDecisionV4->at("status").get<std::string>(), "SCORED");
  BOOST_CHECK_EQUAL(activeScored.placementDecisionV4->at("edge_costs")[0]
    .at("rate_method").get<std::string>(), "ACTIVE_PROBE_GOODPUT");
  BOOST_CHECK_EQUAL(activeScored.placementDecisionV4->at("score_ms").get<double>(), 1010.0);

  auto invalidSignatureSnapshot = baseSnapshot;
  auto& invalidSignatureRow =
    invalidSignatureSnapshot["eligible_link_observations"][0]["row"];
  invalidSignatureRow["kind"] = "ACTIVE_PROBE_GOODPUT";
  invalidSignatureRow["payload_bytes"] = std::uint64_t{8192};
  invalidSignatureRow["elapsed_ms"] = 8.192;
  invalidSignatureRow["probe_complete"] = true;
  invalidSignatureRow["probe_segments_sent"] = std::uint64_t{4};
  invalidSignatureRow["probe_profile_digest"] =
    nativePlanningDigest("fixture-v4-probe-profile");
  invalidSignatureRow["probe_signature_valid"] = false;
  invalidSignatureRow["probe_interest_gaps_ms"] = NativeJson(std::vector<double>(15, 0.01));
  invalidSignatureRow["probe_data_arrival_gaps_ms"] = NativeJson(std::vector<double>(15, 0.01));
  invalidSignatureRow["probe_data_wire_bytes"] = NativeJson::array(
    {std::uint64_t{4096}, std::uint64_t{4096}, std::uint64_t{4096}, std::uint64_t{4096}});
  const auto invalidSignatureScored = decide(std::move(invalidSignatureSnapshot));
  BOOST_REQUIRE(invalidSignatureScored.placementDecisionV4);
  BOOST_CHECK_EQUAL(invalidSignatureScored.placementDecisionV4->at("status")
                      .get<std::string>(), "BASELINE_FALLBACK");

  auto unknownBytesSnapshot = baseSnapshot;
  unknownBytesSnapshot["prepared_role_graph"][0]["dependencies"][0]["estimated_bytes"] = nullptr;
  unknownBytesSnapshot["prepared_role_graph"][0]["dependencies"][0]["estimated_bytes_source"] = nullptr;
  const auto unknownBytes = decide(std::move(unknownBytesSnapshot));
  BOOST_REQUIRE(unknownBytes.placementDecisionV4);
  BOOST_CHECK_EQUAL(unknownBytes.placementDecisionV4->at("status").get<std::string>(),
                    "BASELINE_FALLBACK");
  BOOST_CHECK_EQUAL(unknownBytes.placementDecisionV4->at("fallback_reason").get<std::string>(),
                    "TRANSFER_BYTES_UNKNOWN");
  BOOST_CHECK_EQUAL(unknownBytes.roles.size(), input.roles.size());
  BOOST_CHECK_NO_THROW(validateNativeRolePlacement(
    unknownBytes, input.roles, input.offers, 200));

  auto unknownRateSnapshot = baseSnapshot;
  unknownRateSnapshot["eligible_link_observations"] = NativeJson::array();
  const auto unknownRate = decide(std::move(unknownRateSnapshot));
  BOOST_REQUIRE(unknownRate.placementDecisionV4);
  BOOST_CHECK_EQUAL(unknownRate.placementDecisionV4->at("status").get<std::string>(),
                    "BASELINE_FALLBACK");
  BOOST_CHECK_EQUAL(unknownRate.placementDecisionV4->at("fallback_reason").get<std::string>(),
                    "TRANSFER_RATE_UNKNOWN");
  BOOST_CHECK_EQUAL(unknownRate.roles.size(), input.roles.size());
}

BOOST_AUTO_TEST_CASE(Spec196NetworkOnlyScoresColdCacheWithoutInventingPreparation)
{
  const auto f = oracle();
  Input input(f, f.at("cases")[11]);
  const NativeObservationAwarePlacementV4 network(32, 4096, "network-only");
  const NativeObservationAwarePlacementV4 combined;
  BOOST_CHECK_NE(network.identity().configurationDigest,
                 combined.identity().configurationDigest);
  BOOST_CHECK_THROW(NativeObservationAwarePlacementV4(32, 4096, "unknown"),
                    std::invalid_argument);
  auto snapshot = makeV4PlacementSnapshot(
    input, input.roles, NativeJson(std::uint64_t{1'000'000}), true);
  for (auto& offer : snapshot["provider_offers"]) {
    offer["values"]["residency_state"] = "KNOWN_EMPTY";
    offer["values"]["residency"] = NativeJson::array();
    offer["values"]["estimated_wait_ms"] = 1000.0;
  }
  const auto decide = [&](const NativeObservationAwarePlacementV4& policy,
                          const NativeJson& facts) {
    input.context.placementObservationSnapshot = facts;
    input.context.placementObservationSnapshotDigest = nativePlanningDigest(
      nativeCanonicalJson(facts));
    return policy.proposeRoles(input.context, input.ackDigest, input.roles,
                               input.offers, 200);
  };
  const auto scored = decide(network, snapshot);
  BOOST_REQUIRE(scored.placementDecisionV4);
  const auto& decision = *scored.placementDecisionV4;
  BOOST_CHECK_EQUAL(decision.at("status").get<std::string>(), "SCORED");
  BOOST_CHECK_EQUAL(decision.at("score_mode").get<std::string>(), "network-only");
  BOOST_CHECK(!decision.at("score_is_end_to_end_latency").get<bool>());
  BOOST_CHECK_EQUAL(decision.at("score_ms").get<double>(), 1010.0);
  BOOST_CHECK_EQUAL(decision.at("data_flow_critical_path_ms").get<double>(), 1000.0);
  for (const auto& assignment : decision.at("selected_assignments")) {
    BOOST_CHECK(assignment.at("preparation_ms").is_null());
    BOOST_CHECK_EQUAL(assignment.at("queue_wait_ms").get<double>(), 1000.0);
  }
  BOOST_CHECK_NO_THROW(validateNativeRolePlacement(
    scored, input.roles, input.offers, 200));
  const auto fallback = decide(combined, snapshot);
  BOOST_REQUIRE(fallback.placementDecisionV4);
  BOOST_CHECK_EQUAL(fallback.placementDecisionV4->at("fallback_reason")
                    .get<std::string>(), "PREPARATION_COST_UNKNOWN");

  auto known = makeV4PlacementSnapshot(
    input, input.roles, NativeJson(std::uint64_t{1'000'000}), true);
  for (auto& offer : known["provider_offers"]) {
    offer["values"]["estimated_wait_ms"] = 1000.0;
    for (auto& row : offer["values"]["residency"]) {
      row["state"] = "KNOWN_MISS";
      row["estimated_assembly_ms"] = 10.0;
      row["estimated_load_ms"] = 20.0;
    }
  }
  const auto knownNetwork = decide(network, known);
  const auto knownCombined = decide(combined, known);
  BOOST_REQUIRE(knownNetwork.placementDecisionV4);
  BOOST_REQUIRE(knownCombined.placementDecisionV4);
  BOOST_CHECK_EQUAL(knownNetwork.placementDecisionV4->at("score_ms").get<double>(), 1010.0);
  BOOST_CHECK_EQUAL(knownCombined.placementDecisionV4->at("score_ms").get<double>(), 3070.0);

  input.context.placementProbeRoundCompleted = true;
  auto unavailable = snapshot;
  unavailable["eligible_link_observations"] = NativeJson::array();
  const auto missingRate = decide(network, unavailable);
  BOOST_REQUIRE(missingRate.placementDecisionV4);
  BOOST_CHECK_EQUAL(missingRate.placementDecisionV4->at("fallback_reason")
                    .get<std::string>(), "TRANSFER_RATE_UNKNOWN");
  unavailable = snapshot;
  unavailable["prepared_role_graph"][0]["dependencies"][0]["estimated_bytes"] = nullptr;
  const auto missingBytes = decide(network, unavailable);
  BOOST_REQUIRE(missingBytes.placementDecisionV4);
  BOOST_CHECK_EQUAL(missingBytes.placementDecisionV4->at("fallback_reason")
                    .get<std::string>(), "TRANSFER_BYTES_UNKNOWN");
}

BOOST_AUTO_TEST_CASE(Spec197DecisionReportsTrainGoodputWithoutCapacityBounds)
{
  const auto f = oracle();
  Input input(f, f.at("cases")[11]);
  auto snapshot = makeV4PlacementSnapshot(
    input, input.roles, NativeJson(std::uint64_t{1'000'000}), true);
  // Force the existing stale-snapshot exit to inspect the same production
  // diagnostic builder without depending on assignment search choices.
  snapshot["expires_at_ms"] = std::uint64_t{199};
  const auto inspect = [&] {
    input.context.placementObservationSnapshot = snapshot;
    input.context.placementObservationSnapshotDigest =
      nativePlanningDigest(nativeCanonicalJson(snapshot));
    const auto proposal = NativeObservationAwarePlacementV4(32, 4096, "network-only")
      .proposeRoles(input.context, input.ackDigest, input.roles, input.offers, 200);
    BOOST_REQUIRE(proposal.placementDecisionV4);
    const auto& decision = *proposal.placementDecisionV4;
    BOOST_CHECK(!decision.contains("chirp_estimates"));
    BOOST_REQUIRE_EQUAL(decision.at("probe_train_estimates").size(), 1U);
    return decision.at("probe_train_estimates")[0];
  };
  auto estimate = inspect();
  BOOST_CHECK_EQUAL(estimate.at("status").get<std::string>(), "MEASURED_GOODPUT");
  BOOST_CHECK_CLOSE(estimate.at("goodput_mbps").get<double>(), 8.0, 0.0001);
  BOOST_CHECK_EQUAL(estimate.at("capacity_status").get<std::string>(), "UNKNOWN");
  BOOST_CHECK(!estimate.contains("lower_mbps"));
  BOOST_CHECK(!estimate.contains("upper_mbps"));
  auto& row = snapshot["eligible_link_observations"][0]["row"];
  row["probe_data_arrival_gaps_ms"] = NativeJson(std::vector<double>(15, 0.04));
  row["probe_rate_status"] = "UNKNOWN";
  row["probe_rate_reason"] = "CLOCK_RESOLUTION";
  row["probe_train_goodput_mbps"] = nullptr;
  estimate = inspect();
  BOOST_CHECK_EQUAL(estimate.at("status").get<std::string>(), "UNKNOWN");
  BOOST_CHECK_EQUAL(estimate.at("reason").get<std::string>(), "CLOCK_RESOLUTION");
  BOOST_CHECK(estimate.at("goodput_mbps").is_null());
  BOOST_CHECK_EQUAL(estimate.at("capacity_status").get<std::string>(), "UNKNOWN");
  row["probe_profile_digest"] = "legacy-profile";
  estimate = inspect();
  BOOST_CHECK_EQUAL(estimate.at("status").get<std::string>(), "UNKNOWN");
  BOOST_CHECK_EQUAL(estimate.at("reason").get<std::string>(), "PROBE_PROFILE_INVALID");
}

BOOST_AUTO_TEST_CASE(Spec197DataFetchCannotScoreOrSuppressDedicatedProbe)
{
  const auto f = oracle();
  Input input(f, f.at("cases")[11]);
  input.context.candidateProviders = {"/provider/a", "/provider/b"};
  input.context.allowedGoodputEdges = {
    NativeJson{{"src_provider", "/provider/a"}, {"dst_provider", "/provider/b"}},
    NativeJson{{"src_provider", "/provider/b"}, {"dst_provider", "/provider/a"}}};
  input.context.allowedRttPairs = input.context.allowedGoodputEdges;
  auto snapshot = makeV4PlacementSnapshot(
    input, input.roles, NativeJson(std::uint64_t{1'000'000}), true);
  BOOST_REQUIRE_EQUAL(snapshot["eligible_link_observations"].size(), 2U);
  snapshot["eligible_link_observations"].erase(
    snapshot["eligible_link_observations"].begin());
  BOOST_CHECK_EQUAL(snapshot["eligible_link_observations"][0]["row"]
    .at("kind").get<std::string>(), "DATA_FETCH");
  for (const bool cold : {true, false}) {
    auto cacheSnapshot = snapshot;
    for (auto& offer : cacheSnapshot["provider_offers"]) {
      if (cold) {
        offer["values"]["residency_state"] = "KNOWN_EMPTY";
        offer["values"]["residency"] = NativeJson::array();
      }
      else {
        BOOST_REQUIRE_EQUAL(offer["values"]["residency_state"].get<std::string>(),
                            "OBSERVED");
        BOOST_REQUIRE(!offer["values"]["residency"].empty());
        for (const auto& entry : offer["values"]["residency"])
          BOOST_CHECK_EQUAL(entry.at("state").get<std::string>(), "EXACT_LOADED");
      }
    }
    input.context.placementObservationSnapshot = cacheSnapshot;
    input.context.placementObservationSnapshotDigest =
      nativePlanningDigest(nativeCanonicalJson(cacheSnapshot));
    const auto proposal = NativeObservationAwarePlacementV4(32, 4096, "network-only")
      .proposeRoles(input.context, input.ackDigest, input.roles, input.offers, 200);
    BOOST_REQUIRE(proposal.placementProbeIntent);
    BOOST_REQUIRE_EQUAL(proposal.placementProbeIntent->observers.size(), 1U);
    BOOST_CHECK_EQUAL(proposal.placementProbeIntent->observers.front().goodputEdges.size(), 1U);
    BOOST_CHECK(!proposal.placementDecisionV4 ||
      proposal.placementDecisionV4->at("status") != "SCORED");
  }
}

BOOST_AUTO_TEST_CASE(Spec196PlacementRequestsOnlyItsMissingDirectedGoodputEdges)
{
  const auto f = oracle();
  Input input(f, f.at("cases")[11]);
  input.context.candidateProviders = {"/provider/a", "/provider/b"};
  input.context.allowedGoodputEdges = {
    NativeJson{{"src_provider", "/provider/a"},
               {"dst_provider", "/provider/b"}},
    NativeJson{{"src_provider", "/provider/b"},
               {"dst_provider", "/provider/a"}}};
  input.context.allowedRttPairs = {
    NativeJson{{"src_provider", "/provider/a"},
               {"dst_provider", "/provider/b"}},
    NativeJson{{"src_provider", "/provider/b"},
               {"dst_provider", "/provider/a"}}};
  const NativeObservationAwarePlacementV4 strategy(32, 4096, "network-only");
  auto snapshot = makeV4PlacementSnapshot(
    input, input.roles, NativeJson(std::uint64_t{1'000'000}), false);
  for (auto& offer : snapshot["provider_offers"]) {
    offer["values"]["residency_state"] = "KNOWN_EMPTY";
    offer["values"]["residency"] = NativeJson::array();
  }
  snapshot["prepared_role_graph"][0]["dependencies"][0]["estimated_bytes"] =
    std::uint64_t{1000};
  input.context.placementObservationSnapshot = snapshot;
  input.context.placementObservationSnapshotDigest = nativePlanningDigest(
    nativeCanonicalJson(snapshot));

  const auto firstPass = strategy.proposeRoles(input.context, input.ackDigest,
    input.roles, input.offers, 200);
  BOOST_REQUIRE(firstPass.placementProbeIntent);
  BOOST_CHECK_NO_THROW(validateNativeRolePlacement(
    firstPass, input.roles, input.offers, 200));
  auto assignedProbe = firstPass;
  assignedProbe.providerByRole.emplace("/LLM/Pipeline/Stage/0", "/provider/a");
  BOOST_CHECK_THROW(validateNativeRolePlacement(
    assignedProbe, input.roles, input.offers, 200), std::invalid_argument);
  BOOST_REQUIRE_EQUAL(firstPass.placementProbeIntent->observers.size(), 1U);
  const auto& requested = firstPass.placementProbeIntent->observers.front();
  BOOST_REQUIRE_EQUAL(requested.rttPairs.size(), 1U);
  BOOST_CHECK_EQUAL(requested.rttPairs.front().sourceProvider,
                    requested.observerProvider);
  BOOST_CHECK_EQUAL(requested.rttPairs.front().destinationProvider,
                    requested.goodputEdges.front().sourceProvider);
  BOOST_REQUIRE_EQUAL(requested.goodputEdges.size(), 1U);
  BOOST_CHECK_EQUAL(requested.observerProvider,
                    requested.goodputEdges.front().destinationProvider);
  BOOST_CHECK_NE(requested.goodputEdges.front().sourceProvider,
                 requested.goodputEdges.front().destinationProvider);

  const auto& edge = requested.goodputEdges.front();
  const auto rosterDigest = nativePlanningDigest(
    nativeCanonicalJson(NativeJson(input.context.candidateProviders)));
  const std::string nonce = "spec196-test-probe-nonce-0001";
  NativeJson scope{
    {"schema", "ndnsf-di-placement-probe-scope-v1"},
    {"request_id", input.context.requestId}, {"attempt", input.context.attempt},
    {"requester_identity", input.context.requesterIdentity},
    {"observer_provider", requested.observerProvider},
    {"service", input.context.serviceName},
    {"provider_roster", input.context.candidateProviders},
    {"roster_digest", rosterDigest},
    {"rtt_pairs", NativeJson::array({NativeJson{
      {"src_provider", requested.rttPairs.front().sourceProvider},
      {"dst_provider", requested.rttPairs.front().destinationProvider}}})},
    {"goodput_edges", NativeJson::array({NativeJson{
      {"src_provider", edge.sourceProvider}, {"dst_provider", edge.destinationProvider}}})},
    {"deadline_ms", input.context.deadlineMs}, {"intent_nonce", nonce}};
  const auto scopeDigest = nativePlanningDigest(nativeCanonicalJson(scope));
  const auto bootFor = [&snapshot](const std::string& provider) {
    for (const auto& offer : snapshot["provider_offers"])
      if (offer.at("provider").get<std::string>() == provider)
        return offer.at("boot_epoch").get<std::string>();
    return std::string{};
  };
  NativePlacementProbeRoundV1 round;
  round.intent = *firstPass.placementProbeIntent;
  round.intentNonceByObserver.emplace(requested.observerProvider, nonce);
  round.responseByObserver.emplace(requested.observerProvider, NativeJson{
    {"schema", "ndnsf-di-placement-probe-response-v1"},
    {"request_id", input.context.requestId}, {"attempt", input.context.attempt},
    {"requester_identity", input.context.requesterIdentity},
    {"observer_provider", requested.observerProvider},
    {"service", input.context.serviceName},
    {"observer_boot_epoch", bootFor(requested.observerProvider)},
    {"scope_digest", scopeDigest}, {"intent_nonce", nonce},
    {"deadline_ms", input.context.deadlineMs}, {"completed_at_ms", std::uint64_t{200}},
    {"requested_rtt_count", std::uint64_t{1}},
    {"requested_goodput_count", std::uint64_t{1}},
    {"omitted_rtt_count", std::uint64_t{0}},
    {"omitted_goodput_count", std::uint64_t{0}},
    {"observations", NativeJson::array({
      NativeJson{{"observer", requested.observerProvider},
        {"target", edge.sourceProvider}, {"kind", "active_probe_rtt"},
        {"value", 100.0}, {"observed_at_ms", std::uint64_t{200}},
        {"expires_at_ms", std::uint64_t{900}},
        {"peer_boot_epoch", bootFor(edge.sourceProvider)},
        {"target_signature_validated", true},
        {"data_name", "/provider/a/NDNSF-DI/probe/unique-rtt-data"}},
      NativeJson{{"observer", requested.observerProvider},
        {"target", edge.sourceProvider}, {"kind", "active_probe_goodput"},
        {"value", 8.0}, {"observed_at_ms", std::uint64_t{200}},
        {"expires_at_ms", std::uint64_t{900}},
        {"peer_boot_epoch", bootFor(edge.sourceProvider)},
        {"data_name", "/provider/a/NDNSF-DI/probe/unique-goodput-data"},
        {"train_complete", true}, {"target_signature_validated", true},
        {"probe_profile_validated", true},
        {"sent_segments", std::uint64_t{16}},
        {"received_segments", std::uint64_t{16}},
        {"payload_bytes", std::uint64_t{69000}}, {"elapsed_ms", 69.0},
        {"profile_digest", ndn_service_framework::NETWORK_PROBE_TRAIN_PROFILE_DIGEST},
        {"probe_source_timing_validated", true},
        {"probe_source_send_gaps_ms", NativeJson(std::vector<double>(14, 0.01))},
        {"probe_train_goodput_mbps", 8.0},
        {"probe_rate_status", "MEASURED_GOODPUT"},
        {"probe_rate_reason", "MEASURED_WARMED_PACKET_TRAIN"},
        {"probe_rtt_ms", NativeJson(std::vector<double>(16, 1.3))},
        {"probe_interest_gaps_ms", NativeJson(std::vector<double>(15, 0.01))},
        {"probe_data_arrival_gaps_ms", NativeJson(std::vector<double>(15, 4.096))},
        {"probe_data_wire_bytes", NativeJson(std::vector<std::uint64_t>(16, 4400))}}})}});
  const NativePlacementObservationSnapshotV4 typedSnapshot{
    snapshot, nativePlanningDigest(nativeCanonicalJson(snapshot)),
    snapshot.at("captured_at_ms").get<std::uint64_t>(),
    snapshot.at("expires_at_ms").get<std::uint64_t>()};
  auto merged = mergeNativePlacementProbeRoundV1(
    typedSnapshot, round, input.context, 200);
  auto verifiedContext = input.context;
  verifiedContext.placementProbeRoundCompleted = true;
  verifiedContext.placementObservationSnapshot = merged.value;
  verifiedContext.placementObservationSnapshotDigest = merged.digest;
  const auto verifiedPlan = strategy.proposeRoles(verifiedContext,
    input.ackDigest, input.roles, input.offers, 200);
  BOOST_REQUIRE(verifiedPlan.placementDecisionV4);
  BOOST_CHECK_EQUAL(verifiedPlan.placementDecisionV4->at("status")
    .get<std::string>(), "SCORED");
  BOOST_REQUIRE_EQUAL(verifiedPlan.placementDecisionV4->at("edge_costs").size(), 1U);
  BOOST_CHECK_EQUAL(verifiedPlan.placementDecisionV4->at("edge_costs")[0]
    .at("rate_method").get<std::string>(), "ACTIVE_PROBE_GOODPUT");
  const auto rejectForgedMetadata = [&] (const char* field, NativeJson value) {
    auto forged = round;
    forged.responseByObserver.at(requested.observerProvider)["observations"][1][field] =
      std::move(value);
    BOOST_CHECK_MESSAGE(mergeNativePlacementProbeRoundV1(
      typedSnapshot, forged, input.context, 200).value.at("eligible_link_observations").empty(),
      "forged field=" << field);
  };
  rejectForgedMetadata("probe_train_goodput_mbps", 16.0);
  rejectForgedMetadata("probe_rate_status", "UNKNOWN");
  rejectForgedMetadata("probe_rate_reason", "UNBOUND_RATE");
  rejectForgedMetadata("probe_source_send_gaps_ms", NativeJson::array({-1.0, 0.01}));
  rejectForgedMetadata("probe_source_send_gaps_ms", NativeJson::array({500.001, 0.01}));
  rejectForgedMetadata("probe_source_send_gaps_ms", NativeJson::array({0.01}));
  BOOST_CHECK(selectNativePlacementProbeRemeasurementV1(input.context, round, 200).observers.empty());
  auto unstable = round;
  auto& unstableRow = unstable.responseByObserver.at(requested.observerProvider)["observations"][1];
  auto gaps = std::vector<double>(15, 4.096);
  constexpr auto half = ndn_service_framework::NETWORK_PROBE_WARMED_MEASURED_SEGMENTS / 2;
  for (std::size_t i = 0; i < half; ++i)
    gaps[ndn_service_framework::NETWORK_PROBE_WARMED_START + half + i] *= 3;
  unstableRow["probe_data_arrival_gaps_ms"] = gaps;
  unstableRow["probe_train_goodput_mbps"] = nullptr;
  unstableRow["probe_rate_status"] = "UNKNOWN";
  unstableRow["probe_rate_reason"] = "ARRIVAL_DISPERSION_UNSTABLE";
  const auto remeasurement = selectNativePlacementProbeRemeasurementV1(input.context, unstable, 200);
  BOOST_REQUIRE_EQUAL(remeasurement.observers.size(), 1U);
  BOOST_CHECK_EQUAL(remeasurement.observers.front().observerProvider, requested.observerProvider);
  BOOST_CHECK_EQUAL(remeasurement.observers.front().goodputEdges.size(), requested.goodputEdges.size());
  BOOST_CHECK_EQUAL(remeasurement.observers.front().rttPairs.size(), requested.rttPairs.size());
  auto forgedRetry = unstable;
  forgedRetry.responseByObserver.at(requested.observerProvider)["observations"][1]
    ["probe_source_timing_validated"] = false;
  BOOST_CHECK(selectNativePlacementProbeRemeasurementV1(input.context, forgedRetry, 200).observers.empty());
  auto wrongNonce = unstable;
  wrongNonce.responseByObserver.at(requested.observerProvider)["intent_nonce"] = "foreign";
  BOOST_CHECK(selectNativePlacementProbeRemeasurementV1(input.context, wrongNonce, 200).observers.empty());
  auto missingRow = unstable;
  missingRow.responseByObserver.at(requested.observerProvider)["observations"].erase(0);
  missingRow.responseByObserver.at(requested.observerProvider)["omitted_rtt_count"] = std::uint64_t{1};
  BOOST_CHECK(selectNativePlacementProbeRemeasurementV1(input.context, missingRow, 200).observers.empty());
  BOOST_CHECK(selectNativePlacementProbeRemeasurementV1(input.context, unstable,
    input.context.deadlineMs).observers.empty());
  auto invalidGoodputProfile = round;
  invalidGoodputProfile.responseByObserver.at(requested.observerProvider)
    ["observations"][1]["probe_profile_validated"] = false;
  auto& invalidMetadata = invalidGoodputProfile.responseByObserver.at(requested.observerProvider)
    ["observations"][1];
  invalidMetadata["probe_train_goodput_mbps"] = nullptr;
  invalidMetadata["probe_rate_status"] = "UNKNOWN";
  invalidMetadata["probe_rate_reason"] = "SOURCE_TIMING_UNVERIFIED";
  const auto partial = mergeNativePlacementProbeRoundV1(
    typedSnapshot, invalidGoodputProfile, input.context, 200);
  BOOST_REQUIRE_EQUAL(partial.value.at("eligible_link_observations").size(), 2U);
  BOOST_CHECK_EQUAL(partial.value.at("eligible_link_observations")[0]
                      .at("row").at("kind").get<std::string>(),
                    "ACTIVE_PROBE_RTT");
  BOOST_CHECK_EQUAL(partial.value.at("eligible_link_observations")[0]
                      .at("row").at("rtt_ms").get<double>(), 100.0);
  const auto& measuredRow = partial.value.at("eligible_link_observations")[1].at("row");
  BOOST_CHECK_EQUAL(measuredRow.at("kind").get<std::string>(), "ACTIVE_PROBE_GOODPUT");
  BOOST_CHECK(measuredRow.at("probe_signature_valid").get<bool>());
  BOOST_CHECK(!measuredRow.at("probe_profile_validated").get<bool>());
  auto sampledContext = input.context;
  sampledContext.placementProbeRoundCompleted = true;
  sampledContext.placementObservationSnapshot = partial.value;
  sampledContext.placementObservationSnapshotDigest = partial.digest;
  const auto sampledPlan = strategy.proposeRoles(sampledContext,
    input.ackDigest, input.roles, input.offers, 200);
  BOOST_REQUIRE(sampledPlan.placementDecisionV4);
  BOOST_CHECK_EQUAL(sampledPlan.placementDecisionV4->at("status")
                      .get<std::string>(), "BASELINE_FALLBACK");
  auto unauthenticatedGoodput = invalidGoodputProfile;
  unauthenticatedGoodput.responseByObserver.at(requested.observerProvider)
    ["observations"][1]["target_signature_validated"] = false;
  BOOST_CHECK(mergeNativePlacementProbeRoundV1(typedSnapshot, unauthenticatedGoodput,
    input.context, 200).value.at("eligible_link_observations").empty());
  auto truncatedGoodput = invalidGoodputProfile;
  truncatedGoodput.responseByObserver.at(requested.observerProvider)
    ["observations"][1]["received_segments"] = std::uint64_t{3};
  BOOST_CHECK(mergeNativePlacementProbeRoundV1(typedSnapshot, truncatedGoodput,
    input.context, 200).value.at("eligible_link_observations").empty());
  auto underreportedRound = round;
  underreportedRound.responseByObserver.at(requested.observerProvider)
    ["observations"] = NativeJson::array();
  const auto underreported = mergeNativePlacementProbeRoundV1(
    typedSnapshot, underreportedRound, input.context, 200);
  BOOST_CHECK(underreported.value.at("eligible_link_observations").empty());
  auto unauthenticatedRtt = round;
  unauthenticatedRtt.responseByObserver.at(requested.observerProvider)
    ["observations"][0]["target_signature_validated"] = false;
  const auto rejectedRtt = mergeNativePlacementProbeRoundV1(
    typedSnapshot, unauthenticatedRtt, input.context, 200);
  BOOST_CHECK(rejectedRtt.value.at("eligible_link_observations").empty());
  auto unavailableContext = input.context;
  unavailableContext.placementProbeRoundCompleted = true;
  unavailableContext.placementObservationSnapshot = rejectedRtt.value;
  unavailableContext.placementObservationSnapshotDigest = nativePlanningDigest(
    nativeCanonicalJson(rejectedRtt.value));
  const auto unavailablePlan = strategy.proposeRoles(unavailableContext,
    input.ackDigest, input.roles, input.offers, 200);
  BOOST_REQUIRE(unavailablePlan.placementDecisionV4);
  BOOST_CHECK_EQUAL(unavailablePlan.placementDecisionV4->at("status")
                      .get<std::string>(), "BASELINE_FALLBACK");
  BOOST_CHECK_EQUAL(unavailablePlan.placementDecisionV4->at("edge_costs").size(), 0U);
  input.context.placementProbeRoundCompleted = true;
  input.context.placementObservationSnapshot = merged.value;
  input.context.placementObservationSnapshotDigest = nativePlanningDigest(
    nativeCanonicalJson(merged.value));
  const auto finalPass = strategy.proposeRoles(input.context, input.ackDigest,
    input.roles, input.offers, 200);
  BOOST_CHECK(!finalPass.placementProbeIntent);
  BOOST_REQUIRE(finalPass.placementDecisionV4);
  BOOST_CHECK_EQUAL(finalPass.placementDecisionV4->at("status").get<std::string>(), "SCORED");
  BOOST_REQUIRE_EQUAL(finalPass.placementDecisionV4->at("edge_costs").size(), 1U);
  BOOST_CHECK_EQUAL(finalPass.placementDecisionV4->at("edge_costs")[0]
                      .at("rate_method").get<std::string>(), "ACTIVE_PROBE_GOODPUT");
  BOOST_CHECK_EQUAL(finalPass.placementDecisionV4->at("edge_costs")[0]
                      .at("round_trip_ms").get<double>(), 100.0);
  BOOST_CHECK_EQUAL(finalPass.placementDecisionV4->at("edge_costs")[0]
                      .at("transfer_ms").get<double>(), 100.0);
}

BOOST_AUTO_TEST_CASE(Spec196ProbesCandidateLinkWhenTransferSizeIsUnknown)
{
  const auto f = oracle();
  Input input(f, f.at("cases")[11]);
  input.context.candidateProviders = {"/provider/a", "/provider/b"};
  input.context.allowedGoodputEdges = {
    NativeJson{{"src_provider", "/provider/a"},
               {"dst_provider", "/provider/b"}},
    NativeJson{{"src_provider", "/provider/b"},
               {"dst_provider", "/provider/a"}}};
  const NativeObservationAwarePlacementV4 strategy;
  auto snapshot = makeV4PlacementSnapshot(
    input, input.roles, NativeJson(std::uint64_t{1'000'000}), false);
  auto& dependency = snapshot["prepared_role_graph"][0]["dependencies"][0];
  dependency["estimated_bytes"] = nullptr;
  dependency["estimated_bytes_source"] = nullptr;
  input.context.placementObservationSnapshot = snapshot;
  input.context.placementObservationSnapshotDigest = nativePlanningDigest(
    nativeCanonicalJson(snapshot));

  const auto firstPass = strategy.proposeRoles(input.context, input.ackDigest,
    input.roles, input.offers, 200);
  BOOST_REQUIRE(firstPass.placementProbeIntent);
  BOOST_REQUIRE_EQUAL(firstPass.placementProbeIntent->observers.size(), 1U);
  BOOST_REQUIRE_EQUAL(firstPass.placementProbeIntent->observers.front()
                        .goodputEdges.size(), 1U);

  input.context.placementProbeRoundCompleted = true;
  const auto finalPass = strategy.proposeRoles(input.context, input.ackDigest,
    input.roles, input.offers, 200);
  BOOST_CHECK(!finalPass.placementProbeIntent);
  BOOST_REQUIRE(finalPass.placementDecisionV4);
  BOOST_CHECK_EQUAL(finalPass.placementDecisionV4->at("status").get<std::string>(),
                    "BASELINE_FALLBACK");
  BOOST_CHECK_EQUAL(finalPass.placementDecisionV4->at("fallback_reason")
                      .get<std::string>(), "TRANSFER_BYTES_UNKNOWN");
}

BOOST_AUTO_TEST_CASE(Spec194V4ExpandsStageDependenciesAcrossRanks)
{
  const auto f = oracle();
  Input input(f, f.at("cases")[11]);
  const auto producerStage = input.roles.front().role;
  const std::string consumerStage = "/LLM/Pipeline/Stage/1";
  NativeOfferAdmission admission(f.at("policy").dump(),
    {{f.at("key_id").get<std::string>(), f.at("public_pem").get<std::string>()}},
    f.at("candidate"));
  input.offers.clear();
  for (auto& ack : input.acks) {
    const auto originalPayload = ack.ack.getPayload();
    const std::string originalWire(originalPayload.begin(), originalPayload.end());
    const auto wire = resignOfferAcceptingRole(originalWire, consumerStage);
    ndn::Buffer payload(wire.begin(), wire.end());
    ack.ack.setPayload(payload, payload.size());
    ack.authenticationEvidence.wireDigest = nativePlanningDigest(wire);
    input.offers.push_back(admission.verify(ack, input.context, 200));
  }

  auto roles = input.roles;
  for (auto role : input.roles) {
    role.role = consumerStage;
    role.selectedRole = consumerStage;
    roles.push_back(std::move(role));
  }
  auto snapshot = makeV4PlacementSnapshot(
    input, roles, NativeJson(std::uint64_t{1'000'000}), true);
  auto& candidateGraph = snapshot["prepared_role_graph"][0];
  candidateGraph["stage_order"] = NativeJson::array({producerStage, consumerStage});
  candidateGraph["node_roles"] = NativeJson{
    {"producer-node", producerStage}, {"consumer-node", consumerStage}};
  candidateGraph["dependencies"] = NativeJson::array({NativeJson{
    {"producer_role", producerStage}, {"consumer_role", consumerStage},
    {"tensors", {"hidden"}}, {"estimated_bytes", std::uint64_t{1'000'000}},
    {"estimated_bytes_source", "graph-tensor-contract"},
    {"expected_segments", std::uint64_t{1}}, {"operation_kind", "ACTIVATION"}}});
  input.context.placementObservationSnapshot = snapshot;
  input.context.placementObservationSnapshotDigest = nativePlanningDigest(
    nativeCanonicalJson(snapshot));
  for (const auto& role : roles) {
    input.context.preferredProvidersByRole[role.role + "#" +
      std::to_string(role.rank)] = role.role == producerStage ?
        "/provider/a" : "/provider/b";
  }

  const auto proposal = NativeObservationAwarePlacementV4().proposeRoles(
    input.context, input.ackDigest, roles, input.offers, 200);
  BOOST_REQUIRE(proposal.placementDecisionV4);
  BOOST_CHECK_EQUAL(proposal.placementDecisionV4->at("status").get<std::string>(), "SCORED");
  BOOST_CHECK_EQUAL(proposal.providerByRole.at(producerStage + "#0"), "/provider/a");
  BOOST_CHECK_EQUAL(proposal.providerByRole.at(producerStage + "#1"), "/provider/a");
  BOOST_CHECK_EQUAL(proposal.providerByRole.at(consumerStage + "#0"), "/provider/b");
  BOOST_CHECK_EQUAL(proposal.providerByRole.at(consumerStage + "#1"), "/provider/b");
  BOOST_REQUIRE_EQUAL(proposal.placementDecisionV4->at("edge_costs").size(), 4U);
  for (const auto& edgeCost : proposal.placementDecisionV4->at("edge_costs")) {
    BOOST_CHECK_EQUAL(edgeCost.at("rate_method").get<std::string>(), "ACTIVE_PROBE_GOODPUT");
    BOOST_CHECK_EQUAL(edgeCost.at("transfer_ms").get<double>(), 1000.0);
  }
  BOOST_CHECK_EQUAL(
    proposal.placementDecisionV4->at("data_flow_critical_path_ms").get<double>(), 1000.0);
  BOOST_CHECK_EQUAL(proposal.placementDecisionV4->at("score_ms").get<double>(), 1010.0);
  BOOST_CHECK_NO_THROW(validateNativeRolePlacement(proposal, roles, input.offers, 200));
}

BOOST_AUTO_TEST_CASE(Spec194V4HonorsPreferredProviderAndDeviceReservations)
{
  const auto f = oracle();
  Input input(f, f.at("cases")[11]);
  auto roles = input.roles;
  for (auto& role : roles)
    role.requiredDeviceMemoryMb = 3072;
  const auto snapshot = makeV4PlacementSnapshot(
    input, roles, NativeJson(std::uint64_t{1'000'000}), true);
  input.context.placementObservationSnapshot = snapshot;
  input.context.placementObservationSnapshotDigest = nativePlanningDigest(
    nativeCanonicalJson(snapshot));
  for (const auto& role : roles)
    input.context.preferredProvidersByRole[
      role.role + "#" + std::to_string(role.rank)] = "/provider/a";

  const auto proposal = NativeObservationAwarePlacementV4().proposeRoles(
    input.context, input.ackDigest, roles, input.offers, 200);
  BOOST_REQUIRE(proposal.placementDecisionV4);
  BOOST_CHECK_EQUAL(proposal.placementDecisionV4->at("status").get<std::string>(), "SCORED");
  BOOST_REQUIRE_EQUAL(proposal.roles.size(), 2U);
  BOOST_CHECK_EQUAL(proposal.providerByRole.at(roles.front().role + "#0"), "/provider/a");
  BOOST_CHECK_EQUAL(proposal.providerByRole.at(roles.back().role + "#1"), "/provider/a");
  BOOST_REQUIRE_EQUAL(proposal.roles[0].deviceSet.size(), 1U);
  BOOST_REQUIRE_EQUAL(proposal.roles[1].deviceSet.size(), 1U);
  BOOST_CHECK_NE(proposal.roles[0].deviceSet.front(), proposal.roles[1].deviceSet.front());
  BOOST_CHECK_NO_THROW(validateNativeRolePlacement(proposal, roles, input.offers, 200));
}
BOOST_AUTO_TEST_SUITE_END()
