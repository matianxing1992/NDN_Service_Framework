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
      r.at("model_manifest_digest")};
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
}
BOOST_AUTO_TEST_SUITE(Spec182V3Placement)
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
      const auto seal = [&](const auto& value) { return NativePlanSealer::sealCore(input.inspected,
        input.split, value, execution, input.offers, input.ackDigest, inputs); };
      const auto core = seal(proposal);
      BOOST_CHECK_EQUAL(core.coreDigest, sample.at("core_digest").get<std::string>());
      for (const auto& admitted : input.offers) {
        if (!core.offerDigestByProvider.count(admitted.observation().provider)) continue;
        const auto grant = NativePlanSealer::grantView(core, admitted, {nativePlanningDigest("policy"), true});
        BOOST_CHECK_EQUAL(grant.provider, admitted.observation().provider);
        BOOST_CHECK_EQUAL(grant.modelManifestDigest, input.inspected.modelManifestDigest);
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
      NativePreSplitFirstPlacement strategy;
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
