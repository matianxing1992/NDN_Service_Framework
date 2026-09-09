#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeV3Placement.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanProjectionBuilder.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "tests/fixtures/spec182/native-sealing-fixture.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include <boost/test/unit_test.hpp>
#include <algorithm>
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
    value.setField("ndnsfDataV1EndpointPrefix", offer.provider + "/NDNSF-DI/data/");
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
    keyOffer.setField("ndnsfDataV1EndpointPrefix", offer.provider + "/NDNSF-DI/data/");
    ack.ack.setSelectionInputKeyOffer(keyOffer);
  }
  NativeGroupKeyAdmission keys(admission, input.acks, input.context, 200);
  std::vector<GroupMemberV1> members;
  for (const auto& pair : certificates) {
    members.push_back({pair.first, members.size(), keys.offer(pair.first).observation().offerDigest, keys.endpoint(pair.first)});
    BOOST_CHECK_EQUAL(keys.endpoint(pair.first), pair.first + "/NDNSF-DI/data");
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
      context.dependencies[0] = {"group", "epoch", {{proposal.providerByRole.at(execution.roles.front()), "/producer/tensors"}}, 7};
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
        const auto original = proposal.providerByRole;
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
            endpoint.layoutDigest, endpoint.targetLayoutDigest, endpoint.tensorDigest, {{1, 2, 3}}, now);
          BOOST_CHECK(consumer->openSegment(transfer.manifest, transfer.segments.front()) == ProviderGroupBytes({1, 2, 3}));
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
        BOOST_CHECK_EQUAL(first.mayPublish[i].producerNamespace, "/producer/tensors");
        BOOST_CHECK_EQUAL(first.mayPublish[i].round, 7);
        BOOST_CHECK_EQUAL(first.mayPublish[i].segmentCount, 4096);
        BOOST_CHECK_EQUAL(last.waitFor.front().endpointDigests[i], last.mustFetch[i].endpointDigest);
      }
      auto missing = context; missing.dependencies.clear();
      BOOST_CHECK_THROW(NativePlanProjectionBuilder::build(sealed, input.split, input.offers, missing), std::invalid_argument);
      auto cycle = execution; auto back = execution.dependencies.front();
      std::swap(back.producers, back.consumers); cycle.dependencies.push_back(back);
      auto cycleContext = context;
      cycleContext.dependencies[1] = {"back", "epoch", {{proposal.providerByRole.at(execution.roles.back()), "/tail/tensors"}}, 8};
      BOOST_CHECK_THROW(NativePlanProjectionBuilder::build(seal(cycle), input.split, input.offers, cycleContext), std::invalid_argument);
      auto feedback = cycle; feedback.dependencies.back().operationKind = "TOKEN_FEEDBACK";
      const auto withFeedback = NativePlanProjectionBuilder::build(seal(feedback), input.split, input.offers, context);
      recordProjectionOracle(withFeedback);
      BOOST_CHECK_EQUAL(withFeedback.at(execution.roles.back()).dataflow.mustFetch.size(), 2);
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
    [&](const auto&, const auto&, const auto&, const auto&) { ++publications; return binding; },
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
  const auto selected = planned.corePlan.participantSelector->select(closure.candidates, planned.corePlan.roles);
  BOOST_REQUIRE_EQUAL(selected.size(), 1U);
  BOOST_CHECK_EQUAL(selected.front().provider.toUri(), planned.terminalProvider);
  BOOST_CHECK_EQUAL(selected.front().artifactDataName.toUri(), "/catalog/root");
  BOOST_CHECK(!selected.front().assignmentPayload.empty());
  cancelled->store(true);
  BOOST_CHECK_THROW(planNativeRequest(runtime, {}, input.inspected, encoded, splitter,
    NativePreSplitFirstPlacement(), preparation, admission, closure, control, input.context.deadlineMs, cancelled), std::runtime_error);
  BOOST_CHECK_EQUAL(publications, 1U);
  BOOST_CHECK_EQUAL(grantPublications, 1U);
}

void runPublicClientScenario(int scenario)
{
  using namespace ndn_service_framework;
  class User final : public test::LocalServiceUser {
  public:
    using test::LocalServiceUser::LocalServiceUser;
    // NativeInferenceClient dispatches request work on a detached executor.
    // Keep the externally supplied Face alive with the ServiceUser so a late
    // worker release cannot race the Face's scheduler/reactor destruction.
    std::shared_ptr<ndn::DummyClientFace> faceOwner;
    std::map<std::string, SelectionInputKeyOffer> groupKeys;
    void offer(const ndn::Name& id, const std::string& wire) {
      auto& call = m_pendingCalls.at(id);
      RequestAckMessage ack;
      ack.setStatus(true); ack.setUserToken(call.requestMessage.getUserToken());
      ack.setProviderToken("provider-token");
      ndn::Buffer bytes(wire.begin(), wire.end()); ack.setPayload(bytes, bytes.size());
      const auto decoded = decodeNativeProviderOfferV3(wire);
      if (groupKeys.count(decoded.provider)) ack.setSelectionInputKeyOffer(groupKeys.at(decoded.provider));
      // This supplies the Core authentication boundary as a local fixture.
      // Offer admission still verifies the actual Ed25519 signature below.
      call.requestAcks.push_back({ndn::Name(decoded.provider), call.serviceName, id, ack,
        {decoded.provider, decoded.provider + "/KEY/fixture/issuer/v=1", nativePlanningDigest(wire), true}});
      call.providerTokens[decoded.provider] = "provider-token";
      call.ackWindowExpired = true;
      closeDeferredCollaborationAcks(id, call);
    }
    bool committed(const ndn::Name& id) const {
      const auto it = m_pendingCalls.find(id);
      return it != m_pendingCalls.end() && it->second.collaborationPlanCommitted;
    }
    ResponseHandler responseCallback(const ndn::Name& id) { return m_pendingCalls.at(id).responseHandler; }
    TimeoutHandler timeoutCallback(const ndn::Name& id) { return m_pendingCalls.at(id).timeoutHandler; }
    auto streamCallbacks(const ndn::Name& id) { return m_streamStates.at(id); }
    std::vector<ndn::Name> pendingIds() const {
      std::vector<ndn::Name> result;
      for (const auto& pair : m_pendingCalls) result.push_back(pair.first);
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
  auto ownedInput = std::make_shared<Input>(f, *sample);
  const auto& input = *ownedInput;
  class Splitter final : public NativeModelSplitStrategy {
  public:
    explicit Splitter(NativeSplitCandidate candidate) : value(std::move(candidate)) {}
    NativeStrategyIdentity identity() const override { return value.splitter; }
    std::vector<NativeSplitCandidate> enumerate(const NativeModelDescriptor&, const NativeGraphSnapshot&,
      const NativeCandidateBudget&) const override { return {value}; }
    NativeSplitCandidate value;
  };
  ndn::security::KeyChain keyChain{"pib-memory:", "tpm-memory:"};
  auto faceOwner = std::make_shared<ndn::DummyClientFace>(keyChain);
  auto& face = *faceOwner;
  const auto requesterCert = test::makeRsaIdentity(keyChain, ndn::Name("/requester"));
  const auto authorityCert = test::makeRsaIdentity(keyChain, ndn::Name("/authority"));
  auto user = std::make_shared<User>(face, ndn::Name("/client"),
    requesterCert, authorityCert, "examples/trust-any.conf");
  user->faceOwner = std::move(faceOwner);
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
  if (scenario >= 3) {
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
      value.setField("ndnsfDataV1EndpointPrefix", provider + "/NDNSF-DI/data");
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
  if (scenario >= 3) issuer.recipientPublicKeys.emplace("/provider/b", key('d'));
  issuer.contentKey = [](const auto&, const auto&) { return std::vector<std::uint8_t>(32, 42); };
  std::string authorityPublic(32, '\0'); std::size_t size = authorityPublic.size();
  BOOST_REQUIRE_EQUAL(EVP_PKEY_get_raw_public_key(issuer.authorityPrivateKey.get(),
    reinterpret_cast<unsigned char*>(authorityPublic.data()), &size), 1);
  NativeRequestRuntime runtime;
  runtime.contract = {"/service", "task", input.inspected.descriptor.adapterId,
    input.inspected.descriptor.adapter.descriptorDigest(), nativePlanningDigest("composition"), nativePlanningDigest("task")};
  if (scenario >= 3) runtime.contract.generationMode = "TOKEN_STREAMING";
  runtime.requesterIdentity = "/requester"; runtime.protectionEpoch = issuer.protectionEpoch;
  runtime.security = {nativePlanningDigest("policy"), true}; runtime.budget.maxPolicyMs = 1000;
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
  {
    NativeInferenceClient client(user, registry, runtime, conversations, preparation, admission);
    NativeRequestOptions requestOptions;
    std::atomic<unsigned> accepted{0};
    if (scenario >= 3) {
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
    auto handle = client.request(model, application, std::make_shared<Splitter>(input.split),
      std::make_shared<NativePreSplitFirstPlacement>(), requestOptions);
    std::shared_ptr<std::vector<NativeInferenceEvent>> observedEvents;
    std::shared_ptr<std::promise<void>> observedTerminal;
    std::future<void> observedDone;
    if (scenario >= 3) {
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
    user->postToIo([&, attemptId, wire = nativeCanonicalJson(offer)] { user->offer(attemptId, wire); });
    };
    submitOffer(id, "/provider/a", 1);
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
    if (scenario >= 3) {
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
    ResponseMessage response; response.setStatus(true);
    ndn::Buffer payload{42, 43}; response.setPayload(payload, payload.size());
    response.setAuthenticatedTransportEvidence(scenario == 1 ? "/foreign/response" : "/provider/a/response",
      "/provider/a/KEY/fixture/issuer/v=1", nativePlanningDigest("response-wire"));
    if (scenario == 2) handle.cancel();
    else user->postToIo([&, response] { user->deliver(id, ndn::Name("/provider/a"), response); });
    BOOST_REQUIRE(pumpUntil([&] { return handle.status() != NativeRequestStatus::Pending && !user->hasPendingCall(id); }));
    const auto terminal = handle.status();
    if (scenario == 0) {
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
}

BOOST_AUTO_TEST_CASE(PublicClientCommitsSignedOfferAndIgnoresLateTerminalCallbacks)
{
  for (int scenario = 0; scenario != 3; ++scenario) runPublicClientScenario(scenario);
}

BOOST_AUTO_TEST_CASE(PublicClientConversationCommitsSeededReceiptAndCheckpoint)
{
  runPublicClientScenario(13);
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
        continue;
      }
      const auto core = seal(proposal);
      BOOST_CHECK_EQUAL(core.coreDigest, sample.at("core_digest").get<std::string>());
      if (sample.at("name") == "cpu") {
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
BOOST_AUTO_TEST_SUITE_END()
