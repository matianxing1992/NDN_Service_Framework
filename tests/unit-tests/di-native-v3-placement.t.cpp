#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeV3Placement.hpp"
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
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include <ndn-cxx/security/key-params.hpp>

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
    NativeModelDescriptor descriptor{"QwenFixture", context.modelDigest, nativePlanningDigest("semantics"),
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
    const auto proposal = NativePreSplitFirstPlacement().proposeRoles(input.context, input.ackDigest, input.roles, input.offers, now);
    auto execution = input.split.executionPlan;
    execution.serviceName = input.context.serviceName; execution.modelName = input.inspected.descriptor.modelName;
    execution.roles.clear();
    NativePlanSealingInputs sealing;
    sealing.requesterIdentity = "/requester"; sealing.protectionEpoch = input.roles.front().protectionEpoch;
    sealing.expiresAtMs = input.context.deadlineMs;
    auto& artifacts = sealing.artifacts;
    artifacts.requestId = input.context.requestId; artifacts.attempt = input.context.attempt;
    artifacts.modelDigest = input.context.modelDigest; artifacts.graphDigest = input.context.graphDigest;
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
      inputs.artifacts.modelDigest = input.context.modelDigest; inputs.artifacts.graphDigest = input.context.graphDigest;
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
