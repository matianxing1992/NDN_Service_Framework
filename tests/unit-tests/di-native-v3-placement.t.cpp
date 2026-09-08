#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeV3Placement.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "tests/fixtures/spec182/native-sealing-fixture.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <fstream>

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
      role.backend = "onnxruntime";
      role.requiredDeviceMemoryMb = 1024;
      roles.push_back(role);
    }
    NativeModelDescriptor descriptor{"QwenFixture", context.modelDigest, nativePlanningDigest("semantics"),
      context.graphDigest, "onnx", "fp32", roles.front().adapterId, roles.front().adapterVersion};
    NativeGraphSnapshot graph;
    graph.graphDigest = context.graphDigest; graph.nodes = {{"node", "Identity", 0}};
    graph.topologicalOrder = {"node"};
    inspected = {descriptor, graph, "/catalog/model", nativePlanningDigest("source"),
      r.at("model_manifest_digest"), inputs.artifacts.canonicalGraphDigest};
    split.model = descriptor; split.graphDigest = context.graphDigest;
    split.splitter = {"fixture", "1", nativePlanningDigest("split")};
    split.candidateDigest = nativePlanningDigest("placed-candidate");
    for (const auto& role : roles) {
      if (!split.tensorDegreesByRole.count(role.role)) split.executionPlan.roles.push_back(role.role);
      ++split.tensorDegreesByRole[role.role];
      split.fragmentsByRole[role.role] = nativePlanningDigest("fragment");
      split.artifactsByRole[role.role].push_back(role.artifactDigest);
      split.requirementsByRole[role.role] = {{"onnxruntime"}, 1, 0, 0, 0, 1.0};
    }
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
    }
  }
};
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
