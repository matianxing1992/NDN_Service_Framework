#include "tests/fixtures/spec182/native-sealing-fixture.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include <fstream>
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanSealer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantClient.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeQwenPlanner.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace ndnsf::di;

std::string digest(const std::string& value)
{
  return nativePlanningDigest(value);
}

void checkSha256Format(const std::string& value)
{
  BOOST_REQUIRE_EQUAL(value.size(), 71U);
  BOOST_CHECK_EQUAL(value.substr(0, 7), "sha256:");
}

NativeModelDescriptor model(const std::string& adapter,
                            const std::string& name,
                            const std::string& graphDigest)
{
  return {name, digest(name + "-content"), digest(name + "-semantics"),
          graphDigest, "onnx", "float32", adapter, "1"};
}

NativeGraphSnapshot graph(const std::string& graphDigest,
                          std::vector<std::string> nodeIds)
{
  NativeGraphSnapshot result;
  result.graphDigest = graphDigest;
  for (std::size_t i = 0; i < nodeIds.size(); ++i) {
    result.nodes.push_back({nodeIds[i], "op", static_cast<std::uint64_t>(i)});
  }
  result.topologicalOrder = std::move(nodeIds);
  result.legalCutEdges = {"cut-0", "cut-1"};
  return result;
}

const std::uint64_t GB = 1024ULL * 1024ULL * 1024ULL;

// Builds a validated snapshot + placement proposal for one qwen stage role.
// The snapshot carries the two ACK offers and the proposal deterministically
// binds the role to "provider-a" (provider-a holds one residency digest).
struct OneRolePlan
{
  NativePlanningSnapshot snapshot;
  NativePlacementProposal proposal;
  NativePlanSealingInputs inputs;
  std::string role;
};

OneRolePlan oneRolePlan(const std::string& tag, const std::string& ackDigest,
                        const std::string& offerADigest = "")
{
  const auto graphDigest = digest(tag + "-graph");
  auto modelDescriptor = model("qwen", "QwenFixture", graphDigest);
  auto graphSnapshot = graph(graphDigest,
                             {"embedding", "layer-00", "final-norm-head"});
  OneRolePlan result;
  result.role = "/LLM/Pipeline/Stage/0";
  qwen::NativeQwenLayerSplit splitter(
    {{0, 1}}, {{result.role, digest(tag + "-artifact")}}, {{result.role, 1}},
    {result.role}, {1});
  const auto candidate =
    splitter.enumerate(modelDescriptor, graphSnapshot,
                       NativeCandidateBudget{1, 100, 1}).front();
  result.snapshot.model = modelDescriptor;
  result.snapshot.graph = graphSnapshot;
  result.snapshot.requestId = tag + "-request";
  result.snapshot.attempt = 1;
  result.snapshot.ackClosedDigest = ackDigest;
  result.snapshot.deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(1);
  result.snapshot.offers = {
    {"provider-a", offerADigest.empty() ? digest(tag + "-offer-a") : offerADigest,
     {result.role}, {"onnxruntime"},
     {digest(tag + "-artifact")}, 4 * GB, 1, true, true},
    {"provider-b", digest(tag + "-offer-b"), {result.role}, {"onnxruntime"}, {},
     5 * GB, 1, true, true},
  };
  NativePreSplitFirstPlacement placement;
  result.proposal = placement.propose(result.snapshot, candidate);
  result.inputs.artifacts.sourceByRole = {{result.role, "/canonical/" + tag}};
  result.inputs.artifacts.artifactDigestByRole = {{result.role, digest(tag + "-artifact")}};
  result.inputs.artifacts.manifestDigest = digest(tag + "-manifest");
  result.inputs.artifacts.recipeDigest = digest(tag + "-recipe");
  result.inputs.artifacts.requestId = result.snapshot.requestId;
  result.inputs.artifacts.attempt = result.snapshot.attempt;
  result.inputs.artifacts.modelDigest = result.snapshot.model.contentDigest;
  result.inputs.artifacts.graphDigest = result.snapshot.graph.graphDigest;
  result.inputs.requesterIdentity = "/requester";
  result.inputs.protectionEpoch = "protected-v1";
  result.inputs.expiresAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count() + 1000;
  BOOST_REQUIRE_EQUAL(result.proposal.assignment.providerByRole.at(result.role),
                      "provider-a");
  ndnsf::di::fixture::assemblies(result.inputs);
  return result;
}

std::string sealedEncodeBytes(const NativeSealedPlan& sealed,
                              const std::string& provider)
{
  const auto bytes = NativePlanSealer::encode(
    NativePlanSealer::project(sealed, provider, ndnsf::di::fixture::projection(sealed, provider)));
  return {bytes.begin(), bytes.end()};
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec182PlanSealer)

BOOST_AUTO_TEST_CASE(PlanSealerBindsSnapshotArtifactsAndGrantContext)
{
  const auto plan = oneRolePlan("complete", digest("ack"));
  const auto core = NativePlanSealer::sealCore(plan.snapshot, plan.proposal, plan.inputs);
  const NativeSecurityPolicySnapshot policy{digest("policy"), true};
  const auto view = NativePlanSealer::grantView(core, plan.snapshot.offers.front(), policy);
  BOOST_CHECK_EQUAL(view.artifactDigest, plan.inputs.artifacts.artifactDigestByRole.at(plan.role));
  BOOST_CHECK_EQUAL(view.modelManifestDigest, plan.inputs.artifacts.manifestDigest);
  BOOST_CHECK_EQUAL(view.requesterIdentity, plan.inputs.requesterIdentity);
  BOOST_CHECK_EQUAL(view.requestId, plan.snapshot.requestId);
  BOOST_CHECK_EQUAL(view.expiresAtMs, plan.inputs.expiresAtMs);
  const auto sealed = NativePlanSealer::finalizeSecurity(core,
    {{"provider-a", plan.role, "/grant/1", digest("grant"), "provider-a"}}, policy);
  const auto wire = sealedEncodeBytes(sealed, "provider-a");
  std::istringstream input(wire);
  const auto parsed = nativeSelectionProjectionV3FromJson(input, plan.role);
  BOOST_CHECK_EQUAL(parsed.planCoreDigest, core.coreDigest);
  BOOST_CHECK_EQUAL(parsed.planDigest, sealed.planDigest);
  BOOST_CHECK_EQUAL(parsed.selectedRole.adapterId, "qwen");
  BOOST_CHECK_EQUAL(parsed.selectedRole.roleKind, "PIPELINE_RANGE");
  BOOST_CHECK_EQUAL(parsed.deadlineMs, plan.inputs.expiresAtMs);
  BOOST_CHECK_EQUAL(parsed.grantName, "/grant/1");
  BOOST_CHECK_EQUAL(parsed.deviceBinding.offerDigest, plan.snapshot.offers.front().offerDigest);
  BOOST_CHECK_EQUAL(nativeSelectionProjectionV3ToJson(parsed), wire);
  BOOST_CHECK(nativeParseJson(wire).contains("roles"));
  BOOST_CHECK(!nativeParseJson(wire).contains("selected_role"));
}

BOOST_AUTO_TEST_CASE(PlanSealerMatchesFrozenPythonCoreAndSecurityDigests)
{
  auto plan = oneRolePlan("oracle", digest("oracle-ack"));
  plan.proposal.candidateDigest = digest("candidate");
  plan.proposal.strategy = {"test-placement", "1", digest("strategy")};
  const auto core = NativePlanSealer::sealCore(plan.snapshot, plan.proposal, plan.inputs);
  const auto sealed = NativePlanSealer::finalizeSecurity(core,
    {{"provider-a", plan.role, "/grant/1", digest("grant"), "provider-a"}}, {digest("policy"), true});
  std::ifstream file("tests/fixtures/spec182/sealer-python-oracle.json");
  BOOST_REQUIRE(file.good());
  const auto oracle = NativeJson::parse(file);
  BOOST_CHECK_EQUAL(core.coreDigest, oracle.at("core_digest").get<std::string>());
  BOOST_CHECK_EQUAL(sealed.planDigest, oracle.at("plan_digest").get<std::string>());
}

BOOST_AUTO_TEST_CASE(PlanSealerRejectsTamperedFrozenCoreAndSecurity)
{
  const auto plan = oneRolePlan("tamper", digest("ack"));
  const auto core = NativePlanSealer::sealCore(plan.snapshot, plan.proposal, plan.inputs);
  const std::vector<std::function<void(NativePlacementPlanCore&)>> mutations = {
    [](auto& c) { c.requestId += "foreign"; },
    [](auto& c) { c.candidateDigest = digest("foreign"); },
    [](auto& c) { c.assemblyByRole.begin()->second.adapterVersion = "2"; },
    [](auto& c) { c.assemblyByRole.begin()->second.expectedInputs.front().shape[0] = std::string("1"); },
    [](auto& c) { c.requestContractDigest = digest("contract"); },
  };
  for (const auto& mutate : mutations) {
    auto changed = core;
    mutate(changed);
    BOOST_CHECK_THROW(changed.validate(), std::invalid_argument);
  }
  auto sealed = NativePlanSealer::finalizeSecurity(core,
    {{"provider-a", plan.role, "/grant/1", digest("grant"), "provider-a"}}, {digest("policy"), true});
  sealed.grants.front().grantDigest = digest("substitute");
  BOOST_CHECK_THROW(sealed.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(PlanSealerRejectsIncompleteSubstitutedOrOutsideGrantCover)
{
  const auto plan = oneRolePlan("grants", digest("ack"));
  const auto core = NativePlanSealer::sealCore(plan.snapshot, plan.proposal, plan.inputs);
  const NativeSecurityPolicySnapshot policy{digest("policy"), true};
  const NativeGrantBinding grant{"provider-a", plan.role, "/grant/1", digest("grant"), "provider-a"};
  BOOST_CHECK_THROW(NativePlanSealer::finalizeSecurity(core, {}, policy), std::invalid_argument);
  BOOST_CHECK_THROW(NativePlanSealer::finalizeSecurity(core, {grant, grant}, policy), std::invalid_argument);
  auto foreign = grant;
  foreign.recipient = "another-provider";
  BOOST_CHECK_THROW(NativePlanSealer::finalizeSecurity(core, {foreign}, policy), std::invalid_argument);
  foreign = grant;
  foreign.provider = foreign.recipient = "another-provider";
  BOOST_CHECK_THROW(NativePlanSealer::finalizeSecurity(core, {foreign}, policy), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(PlanSealerProjectionRejectsForeignRoleDeviceOrDataflow)
{
  const auto plan = oneRolePlan("project", digest("ack"));
  const auto core = NativePlanSealer::sealCore(plan.snapshot, plan.proposal, plan.inputs);
  const auto sealed = NativePlanSealer::finalizeSecurity(core,
    {{"provider-a", plan.role, "/grant/1", digest("grant"), "provider-a"}}, {digest("policy"), true});
  const auto valid = ndnsf::di::fixture::projection(sealed, "provider-a");
  BOOST_CHECK_THROW(NativePlanSealer::project(sealed, "foreign", valid), std::invalid_argument);
  auto changed = valid;
  changed.deviceBinding.offerDigest = digest("foreign");
  BOOST_CHECK_THROW(NativePlanSealer::project(sealed, "provider-a", changed), std::invalid_argument);
  changed = valid;
  changed.executionRole.backend = "onnxruntime-cuda";
  BOOST_CHECK_THROW(NativePlanSealer::project(sealed, "provider-a", changed), std::invalid_argument);
  changed = valid;
  changed.dataflow.attempt += 1;
  BOOST_CHECK_THROW(NativePlanSealer::project(sealed, "provider-a", changed), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(PlanSealerPlaintextPolicyNeedsNoGrantCover)
{
  auto plan = oneRolePlan("plain", digest("ack"));
  plan.inputs.protectionEpoch = "plaintext-v1";
  ndnsf::di::fixture::assemblies(plan.inputs);
  const auto core = NativePlanSealer::sealCore(plan.snapshot, plan.proposal, plan.inputs);
  const auto sealed = NativePlanSealer::finalizeSecurity(core, {}, {digest("policy"), false});
  const auto wire = sealedEncodeBytes(sealed, "provider-a");
  BOOST_CHECK(!nativeParseJson(wire).contains("grant_binding"));
  BOOST_CHECK_THROW(NativePlanSealer::finalizeSecurity(core,
    {{"provider-a", plan.role, "/grant", digest("grant"), "provider-a"}}, {digest("policy"), false}),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(PlanSealerEncodeRejectsIncompleteProjections)
{
  NativeSelectionProjectionV3 incomplete;
  incomplete.provider = "provider-a";
  incomplete.requestId = "request";
  incomplete.attempt = 1;
  incomplete.planDigest = incomplete.planCoreDigest = incomplete.ackClosedDigest = digest("fragment");
  incomplete.selectedRole.role = "/role";
  BOOST_CHECK_THROW(NativePlanSealer::encode(incomplete), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(PreparedArtifactsReachGrantAcquisitionWithoutBackfill)
{
  auto plan = oneRolePlan("t004-prepared", digest("ack"));
  auto registry = std::make_shared<NativeAdapterRegistry>();
  registry->freeze();
  auto published = plan.inputs.artifacts;
  published.requestId = "untrusted-port-claim";
  published.attempt = 99;
  unsigned publications = 0, issues = 0;
  NativeRequestPreparation preparation(registry, {},
    [&](const NativeInspectedModel&, const NativePlacementProposal&,
        const NativeRequestControl&) { ++publications; return published; });
  NativeInspectedModel inspected{plan.snapshot.model, plan.snapshot.graph,
                                  "/canonical/model", plan.snapshot.model.contentDigest};
  NativeRequestControl control{plan.snapshot.requestId, plan.snapshot.attempt,
                                plan.snapshot.deadline, {}};
  plan.inputs.artifacts = preparation.ensureArtifacts(inspected, plan.proposal, control);
  const auto core = NativePlanSealer::sealCore(plan.snapshot, plan.proposal, plan.inputs);
  const NativeSecurityPolicySnapshot security{digest("policy"), true};
  const auto view = NativePlanSealer::grantView(core, plan.snapshot.offers.front(), security);
  auto authority = std::make_shared<NativeArtifactPolicyAuthority>(
    [&](const NativeGrantRequest& request) {
      ++issues;
      BOOST_CHECK_EQUAL(request.requestId, control.requestId);
      BOOST_CHECK_EQUAL(request.attempt, control.attempt);
      BOOST_CHECK_EQUAL(request.requesterIdentity, "/requester");
      BOOST_CHECK_EQUAL(request.modelManifestDigest, published.manifestDigest);
      BOOST_CHECK_EQUAL(request.artifactDigest, published.artifactDigestByRole.at(plan.role));
      BOOST_CHECK_EQUAL(request.expiresAtMs, plan.inputs.expiresAtMs);
      return NativeKeyGrant{"/issued/grant", digest("issued"), request.providerIdentity,
                             "test-issuer-envelope", request.expiresAtMs};
    });
  NativeGrantClient client("/requester", authority,
    [](const std::string& name, const std::string&) { return name; });
  // No caller repairs the grant view between the real preparation, sealer
  // and acquire boundaries. Crypto/actual network publication remain T005/T016.
  const auto grant = client.acquire(view,
    std::chrono::system_clock::time_point(std::chrono::milliseconds(view.expiresAtMs)));
  BOOST_CHECK_EQUAL(publications, 1U);
  BOOST_CHECK_EQUAL(issues, 1U);
  BOOST_CHECK_EQUAL(grant.expiresAtMs, view.expiresAtMs);
  BOOST_CHECK_EQUAL(grant.provider, view.provider);
}

BOOST_AUTO_TEST_CASE(SealCoreRejectsForeignArtifactsAndInexactCover)
{
  const auto plan = oneRolePlan("t004-binding", digest("ack"));
  const std::vector<std::function<void(NativePlanSealingInputs&)>> mutations = {
    [](auto& x) { x.artifacts.requestId = "foreign"; },
    [](auto& x) { ++x.artifacts.attempt; },
    [](auto& x) { x.artifacts.modelDigest = digest("foreign-model"); },
    [](auto& x) { x.artifacts.graphDigest = digest("foreign-graph"); },
    [](auto& x) { x.artifacts.artifactDigestByRole.clear(); },
    [](auto& x) { x.artifacts.sourceByRole.clear(); },
    [](auto& x) { x.artifacts.sourceByRole.emplace("extra", "/canonical/extra");
                  x.artifacts.artifactDigestByRole.emplace("extra", digest("extra")); },
    [](auto& x) { x.requesterIdentity.clear(); },
    [](auto& x) { x.protectionEpoch.clear(); },
    [](auto& x) { x.expiresAtMs = 1; },
  };
  for (const auto& mutate : mutations) {
    auto changed = plan.inputs;
    mutate(changed);
    BOOST_CHECK_THROW(NativePlanSealer::sealCore(plan.snapshot, plan.proposal, changed),
                      std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(RealArtifactIdentityChangesCoreWithoutChangingRole)
{
  const auto plan = oneRolePlan("t004-artifact", digest("ack"));
  const auto before = NativePlanSealer::sealCore(plan.snapshot, plan.proposal, plan.inputs);
  auto revised = plan.inputs;
  revised.artifacts.artifactDigestByRole.at(plan.role) = digest("different-certified-artifact");
  ndnsf::di::fixture::assemblies(revised);
  const auto after = NativePlanSealer::sealCore(plan.snapshot, plan.proposal, revised);
  BOOST_CHECK_NE(before.coreDigest, after.coreDigest);
  BOOST_CHECK_EQUAL(after.artifactDigestByRole.at(plan.role), digest("different-certified-artifact"));
}

BOOST_AUTO_TEST_CASE(GrantViewRejectsChangedOfferAndProtectionPolicy)
{
  auto plan = oneRolePlan("t004-offer", digest("ack"));
  const auto core = NativePlanSealer::sealCore(plan.snapshot, plan.proposal, plan.inputs);
  const NativeSecurityPolicySnapshot security{digest("policy"), true};
  auto changed = plan.snapshot.offers.front();
  changed.offerDigest = digest("changed-offer");
  BOOST_CHECK_THROW(NativePlanSealer::grantView(core, changed, security), std::invalid_argument);
  BOOST_CHECK_THROW(NativePlanSealer::grantView(core, plan.snapshot.offers.front(),
                    NativeSecurityPolicySnapshot{digest("policy"), false}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(SealCoreRejectsProviderReusedAcrossRoles)
{
  auto plan = oneRolePlan("t004-ownership", digest("ack"));
  plan.proposal.executionPlan.roles.push_back("/other-role");
  plan.proposal.assignment.providerByRole.emplace("/other-role", "provider-a");
  plan.inputs.artifacts.sourceByRole.emplace("/other-role", "/canonical/other");
  plan.inputs.artifacts.artifactDigestByRole.emplace("/other-role", digest("other-artifact"));
  ndnsf::di::fixture::assemblies(plan.inputs);
  BOOST_CHECK_EXCEPTION(NativePlanSealer::sealCore(plan.snapshot, plan.proposal, plan.inputs),
                        std::invalid_argument, [](const std::invalid_argument& e) {
                          return std::string(e.what()) == "native plan requires one role per Provider";
                        });
}

BOOST_AUTO_TEST_SUITE_END()
