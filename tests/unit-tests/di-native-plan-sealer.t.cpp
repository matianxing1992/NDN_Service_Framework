// T004-A Canonical Plan Sealing — Spec182PlanSealer/* (case-manifest card
// T004-A; planned suite registered in this file).
//
// Local sealer checks. A8-01 reopened this task: the legacy seven-field
// encoder and custom digest are not production V3 wire or canonical parity.
// Artifact/grant-input checks below exercise repaired production APIs; the
// remaining wire assertions are legacy regressions only, not acceptance.

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
  return result;
}

std::string sealedEncodeBytes(const NativeSealedPlan& sealed,
                              const std::string& provider)
{
  const auto bytes = NativePlanSealer::encode(
    NativePlanSealer::project(sealed, provider));
  return {bytes.begin(), bytes.end()};
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec182PlanSealer)

BOOST_AUTO_TEST_CASE(PlanSealerBindsSnapshotArtifactsAndGrantContext)
{
  // One canonical sealing pipeline over a fixed snapshot/proposal pair: every
  // field of the projection is single-sourced from the sealed plan (M22), the
  // digests are deterministic and the encode bytes are canonical JSON with the
  // legacy fragment layout (not a V3 wire oracle; see A8-01).
  const auto ackDigest = digest("t004-ack");
  auto plan = oneRolePlan("t004-seal", ackDigest);
  const NativeSecurityPolicySnapshot security{digest("t004-security"), true};

  const auto core = NativePlanSealer::sealCore(plan.snapshot, plan.proposal, plan.inputs);
  checkSha256Format(core.coreDigest);
  // Determinism: the same inputs reseal to the same core digest.
  BOOST_CHECK_EQUAL(
    NativePlanSealer::sealCore(plan.snapshot, plan.proposal, plan.inputs).coreDigest,
    core.coreDigest);
  // Offer and artifact maps are bound from the ACK offers and plan roles.
  BOOST_CHECK_EQUAL(core.offerDigestByProvider.at("provider-a"),
                    digest("t004-seal-offer-a"));
  BOOST_CHECK_EQUAL(core.artifactDigestByRole.at(plan.role),
                    digest("t004-seal-artifact"));
  BOOST_CHECK_EQUAL(core.ackClosedDigest, ackDigest);

  const auto view = NativePlanSealer::grantView(core, plan.snapshot.offers[0],
                                                security);
  BOOST_CHECK_EQUAL(view.provider, "provider-a");
  BOOST_CHECK_EQUAL(view.role, plan.role);
  BOOST_CHECK_EQUAL(view.planCoreDigest, core.coreDigest);
  BOOST_CHECK_EQUAL(view.policyDigest, security.policyDigest);
  BOOST_CHECK_EQUAL(view.modelDigest, core.modelDigest);
  BOOST_CHECK_EQUAL(view.graphDigest, core.graphDigest);
  BOOST_CHECK_EQUAL(view.artifactDigest, core.artifactDigestByRole.at(plan.role));
  BOOST_CHECK_EQUAL(view.requestId, plan.snapshot.requestId);
  BOOST_CHECK_EQUAL(view.attempt, plan.snapshot.attempt);
  BOOST_CHECK_EQUAL(view.requesterIdentity, plan.inputs.requesterIdentity);
  BOOST_CHECK_EQUAL(view.modelManifestDigest, plan.inputs.artifacts.manifestDigest);
  BOOST_CHECK_EQUAL(view.protectionEpoch, plan.inputs.protectionEpoch);
  BOOST_CHECK_EQUAL(view.expiresAtMs, plan.inputs.expiresAtMs);

  const NativeGrantBinding grant{view.provider, view.role, "/grant/t004",
                                 digest("t004-grant"), view.provider};
  const auto sealed = NativePlanSealer::finalizeSecurity(core, {grant},
                                                         security);
  checkSha256Format(sealed.planDigest);
  BOOST_CHECK_EQUAL(NativePlanSealer::finalizeSecurity(core, {grant}, security)
                      .planDigest,
                    sealed.planDigest);

  const auto projection = NativePlanSealer::project(sealed, "provider-a");
  BOOST_REQUIRE(projection.hasGrantBinding);
  // Single-sourced fields: each projection field copies the sealed plan value.
  BOOST_CHECK_EQUAL(projection.provider, "provider-a");
  BOOST_CHECK_EQUAL(projection.requestId, core.requestId);
  BOOST_CHECK_EQUAL(projection.attempt, core.attempt);
  BOOST_CHECK_EQUAL(projection.planCoreDigest, core.coreDigest);
  BOOST_CHECK_EQUAL(projection.planDigest, sealed.planDigest);
  BOOST_CHECK_EQUAL(projection.ackClosedDigest, ackDigest);
  BOOST_CHECK_EQUAL(projection.offerDigest,
                    core.offerDigestByProvider.at("provider-a"));
  BOOST_CHECK_EQUAL(projection.securityPolicySnapshotDigest,
                    security.policyDigest);
  BOOST_CHECK_EQUAL(projection.selectedRole.role, plan.role);
  BOOST_CHECK_EQUAL(projection.selectedRole.selectedRole, plan.role);
  BOOST_CHECK_EQUAL(projection.selectedRole.artifactDigest,
                    core.artifactDigestByRole.at(plan.role));
  BOOST_CHECK_EQUAL(projection.selectedRole.graphDigest, core.graphDigest);
  BOOST_CHECK_EQUAL(projection.grantName, grant.grantName);
  BOOST_CHECK_EQUAL(projection.grantDigest, grant.grantDigest);
  BOOST_CHECK_EQUAL(projection.selectedRole.adapterId,
                    core.executionPlan.modelFamily);
  BOOST_CHECK_EQUAL(projection.selectedRole.roleKind,
                    core.executionPlan.modelFamily);
  // The projection is deterministic: re-projecting yields the same bytes.
  BOOST_CHECK_EQUAL(sealedEncodeBytes(sealed, "provider-a"),
                    sealedEncodeBytes(sealed, "provider-a"));

  // The frozen canonical fragment: fixed key order, no whitespace, values
  // quoted canonically, attempt emitted as a bare integer.
  const auto expected =
    std::string("{\"provider\":\"provider-a\",\"request_id\":\"") +
    core.requestId + "\",\"attempt\":" + std::to_string(core.attempt) +
    ",\"plan_digest\":\"" + sealed.planDigest +
    "\",\"plan_core_digest\":\"" + core.coreDigest +
    "\",\"ack_closed_digest\":\"" + ackDigest +
    "\",\"selected_role\":{\"role\":\"" + plan.role + "\"}}";
  const auto wire = sealedEncodeBytes(sealed, "provider-a");
  BOOST_CHECK_EQUAL(wire, expected);

  // Independent grammar oracle: the fragment parses as JSON with exactly the
  // seven top-level keys and faithful values (not generated by encode()).
  boost::property_tree::ptree tree;
  {
    std::istringstream in(wire);
    boost::property_tree::read_json(in, tree);
  }
  std::set<std::string> keys;
  for (const auto& item : tree) keys.insert(item.first);
  BOOST_CHECK_EQUAL(keys.size(), 7U);
  BOOST_CHECK(keys == std::set<std::string>({"provider", "request_id", "attempt",
                                             "plan_digest", "plan_core_digest",
                                             "ack_closed_digest",
                                             "selected_role"}));
  BOOST_CHECK_EQUAL(tree.get<std::string>("provider"), "provider-a");
  BOOST_CHECK_EQUAL(tree.get<std::string>("request_id"), core.requestId);
  BOOST_CHECK_EQUAL(tree.get<std::uint64_t>("attempt"), core.attempt);
  BOOST_CHECK_EQUAL(tree.get<std::string>("plan_digest"), sealed.planDigest);
  BOOST_CHECK_EQUAL(tree.get<std::string>("plan_core_digest"), core.coreDigest);
  BOOST_CHECK_EQUAL(tree.get<std::string>("ack_closed_digest"), ackDigest);
  BOOST_CHECK_EQUAL(tree.get<std::string>("selected_role.role"), plan.role);
}

BOOST_AUTO_TEST_CASE(PlanSealerDigestAndEncodeBytesAreTamperSensitivePerDimension)
{
  // Every sealing input dimension is bound into the plan digest and the
  // encode bytes: a canonical-but-different value on any single dimension
  // changes the affected digest, and dimensions outside its canonical scope
  // leave that digest unchanged.
  const auto ackDigest = digest("t004-ack");
  auto plan = oneRolePlan("t004-tamper", ackDigest);
  const NativeSecurityPolicySnapshot security{digest("t004-security"), true};
  const auto core = NativePlanSealer::sealCore(plan.snapshot, plan.proposal, plan.inputs);
  const auto view = NativePlanSealer::grantView(core, plan.snapshot.offers[0],
                                                security);
  const NativeGrantBinding grant{view.provider, view.role, "/grant/t004",
                                 digest("t004-grant"), view.provider};
  const auto sealed = NativePlanSealer::finalizeSecurity(core, {grant},
                                                         security);
  const auto baselineCore = core.coreDigest;
  const auto baselinePlan = sealed.planDigest;
  const auto baselineWire = sealedEncodeBytes(sealed, "provider-a");
  BOOST_REQUIRE(baselineWire.find(baselinePlan) != std::string::npos);
  BOOST_REQUIRE(baselineWire.find(baselineCore) != std::string::npos);
  BOOST_REQUIRE(baselineWire.find(ackDigest) != std::string::npos);

  // 1. Wrong (well-formed) ACK digest: bound into the core canonical, the
  // plan digest and the wire.
  {
    auto other = oneRolePlan("t004-tamper", digest("t004-ack-tampered"));
    const auto otherCore = NativePlanSealer::sealCore(other.snapshot, other.proposal, other.inputs);
    BOOST_REQUIRE_NE(otherCore.coreDigest, baselineCore);
    const auto otherView =
      NativePlanSealer::grantView(otherCore, other.snapshot.offers[0],
                                  security);
    const NativeGrantBinding otherGrant{otherView.provider, otherView.role,
                                        "/grant/t004", digest("t004-grant"),
                                        otherView.provider};
    const auto otherSealed = NativePlanSealer::finalizeSecurity(
      otherCore, {otherGrant}, security);
    BOOST_REQUIRE_NE(otherSealed.planDigest, baselinePlan);
    const auto otherWire = sealedEncodeBytes(otherSealed, "provider-a");
    BOOST_REQUIRE_NE(otherWire, baselineWire);
    checkSha256Format(otherCore.coreDigest);
    checkSha256Format(otherSealed.planDigest);
  }

  // 2. Wrong ACK-offer digest (provider-a stays assigned; only the bound
  // offer digest changes): core digest and downstream bytes change.
  {
    auto other = oneRolePlan("t004-tamper", ackDigest,
                             digest("t004-offer-a-tampered"));
    const auto otherCore = NativePlanSealer::sealCore(other.snapshot, other.proposal, other.inputs);
    BOOST_REQUIRE_NE(otherCore.coreDigest, baselineCore);
    BOOST_REQUIRE_EQUAL(otherCore.offerDigestByProvider.at("provider-a"),
                        digest("t004-offer-a-tampered"));
    // provider-b is not assigned, so its (untouched) offer digest does not
    // enter the core canonical at all.
    BOOST_REQUIRE_EQUAL(otherCore.offerDigestByProvider.size(), 1U);
  }

  // 3. Wrong security-policy digest: plan digest and wire change; the core
  // canonical does not include the policy, so the core digest stays stable.
  {
    const NativeSecurityPolicySnapshot otherPolicy{digest("t004-security-x"),
                                                   true};
    const auto otherCore = NativePlanSealer::sealCore(plan.snapshot, plan.proposal, plan.inputs);
    BOOST_REQUIRE_EQUAL(otherCore.coreDigest, baselineCore);
    const auto otherView = NativePlanSealer::grantView(otherCore,
                                                       plan.snapshot.offers[0],
                                                       otherPolicy);
    const NativeGrantBinding otherGrant{otherView.provider, otherView.role,
                                        "/grant/t004", digest("t004-grant"),
                                        otherView.provider};
    const auto otherSealed = NativePlanSealer::finalizeSecurity(
      otherCore, {otherGrant}, otherPolicy);
    BOOST_REQUIRE_NE(otherSealed.planDigest, baselinePlan);
    BOOST_REQUIRE_NE(sealedEncodeBytes(otherSealed, "provider-a"),
                     baselineWire);
    checkSha256Format(otherSealed.planDigest);
  }

  // 4. Grant dimensions (name/digest/recipient): plan digest and wire change
  // with the core canonical untouched.
  {
    const NativeGrantBinding nameGrant{view.provider, view.role,
                                       "/grant/t004-renamed",
                                       digest("t004-grant"), view.provider};
    BOOST_REQUIRE_NE(NativePlanSealer::finalizeSecurity(core, {nameGrant},
                                                        security).planDigest,
                     baselinePlan);
    const NativeGrantBinding digestGrant{view.provider, view.role,
                                         "/grant/t004",
                                         digest("t004-grant-tampered"),
                                         view.provider};
    BOOST_REQUIRE_NE(NativePlanSealer::finalizeSecurity(core, {digestGrant},
                                                        security).planDigest,
                     baselinePlan);
    const NativeGrantBinding recipientGrant{view.provider, view.role,
                                            "/grant/t004", digest("t004-grant"),
                                            "provider-b"};
    BOOST_REQUIRE_NE(NativePlanSealer::finalizeSecurity(core, {recipientGrant},
                                                        security).planDigest,
                     baselinePlan);
  }
}

BOOST_AUTO_TEST_CASE(PlanSealerRejectsProviderOutsidePlanEndpoint)
{
  // Wrong endpoint: projection and grant view refuse providers the sealed
  // plan never assigned, while the assigned provider keeps working.
  const auto ackDigest = digest("t004-ack");
  auto plan = oneRolePlan("t004-endpoint", ackDigest);
  const NativeSecurityPolicySnapshot security{digest("t004-security"), true};
  const auto core = NativePlanSealer::sealCore(plan.snapshot, plan.proposal, plan.inputs);
  const auto view = NativePlanSealer::grantView(core, plan.snapshot.offers[0],
                                                security);
  const NativeGrantBinding grant{view.provider, view.role, "/grant/t004",
                                 digest("t004-grant"), view.provider};
  const auto sealed = NativePlanSealer::finalizeSecurity(core, {grant},
                                                         security);

  BOOST_CHECK_THROW(NativePlanSealer::project(sealed, "provider-ghost"),
                    std::invalid_argument);
  // The unknown provider offer is fully valid by itself (canonical digests,
  // accepted role, backends, resources) yet is not assigned by the plan.
  const NativeProviderPlanningView ghost{
    "provider-ghost", digest("t004-offer-ghost"), {plan.role}, {"onnxruntime"},
    {}, 4 * GB, 1, true, true};
  BOOST_CHECK_THROW(NativePlanSealer::grantView(core, ghost, security),
                    std::invalid_argument);
  // First-boundary semantics: after the rejections the canonical endpoint
  // still projects and encodes.
  BOOST_CHECK(!NativePlanSealer::encode(
                NativePlanSealer::project(sealed, "provider-a")).empty());
  // A sealed plan whose core assigns no provider to the queried role cannot
  // produce a projection either (empty provider string is an invalid
  // endpoint as well).
  BOOST_CHECK_THROW(NativePlanSealer::project(sealed, ""),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(PlanSealerRejectsIncompleteSubstitutedOrOutsideGrantCover)
{
  // Missing grant / substituted cover: a protected plan is rejected unless
  // every role has exactly one canonical (provider, role) grant.
  const auto ackDigest = digest("t004-ack");
  auto plan = oneRolePlan("t004-grant", ackDigest);
  const NativeSecurityPolicySnapshot security{digest("t004-security"), true};
  const auto core = NativePlanSealer::sealCore(plan.snapshot, plan.proposal, plan.inputs);
  const auto view = NativePlanSealer::grantView(core, plan.snapshot.offers[0],
                                                security);

  // No grant at all, and a grant for the unassigned offer, both leave the
  // protected cover incomplete.
  BOOST_CHECK_THROW(NativePlanSealer::finalizeSecurity(core, {}, security),
                    std::invalid_argument);
  const NativeGrantBinding foreignGrant{"provider-b", plan.role, "/grant/t004",
                                        digest("t004-grant"), "provider-b"};
  BOOST_CHECK_THROW(NativePlanSealer::finalizeSecurity(core, {foreignGrant},
                                                       security),
                    std::invalid_argument);
  // A role that the plan never assigned is outside the assignment.
  const NativeGrantBinding phantomRole{"provider-a", "/LLM/Pipeline/Stage/7",
                                       "/grant/t004", digest("t004-grant"),
                                       "provider-a"};
  BOOST_CHECK_THROW(NativePlanSealer::finalizeSecurity(core, {phantomRole},
                                                       security),
                    std::invalid_argument);
  // Substituted duplicates and malformed bindings are rejected.
  const NativeGrantBinding grant{view.provider, view.role, "/grant/t004",
                                 digest("t004-grant"), view.provider};
  BOOST_CHECK_THROW(NativePlanSealer::finalizeSecurity(core, {grant, grant},
                                                       security),
                    std::invalid_argument);
  const NativeGrantBinding emptyRecipient{view.provider, view.role,
                                          "/grant/t004", digest("t004-grant"),
                                          ""};
  BOOST_CHECK_THROW(NativePlanSealer::finalizeSecurity(core, {emptyRecipient},
                                                       security),
                    std::invalid_argument);
  const NativeGrantBinding junkDigest{view.provider, view.role, "/grant/t004",
                                      "not-a-digest", view.provider};
  BOOST_CHECK_THROW(NativePlanSealer::finalizeSecurity(core, {junkDigest},
                                                       security),
                    std::invalid_argument);
  // After all rejections the exact canonical cover still seals.
  const auto sealed = NativePlanSealer::finalizeSecurity(core, {grant},
                                                         security);
  checkSha256Format(sealed.planDigest);
  BOOST_CHECK(!NativePlanSealer::encode(
                NativePlanSealer::project(sealed, "provider-a")).empty());
}

BOOST_AUTO_TEST_CASE(PlanSealerRejectsNonCanonicalAckDigestAtSealBoundary)
{
  // Wrong ACK digest, format level: a non-canonical ACK digest cannot enter a
  // sealed core.  The placement proposal above is well-formed; the sealer
  // refuses to mint an identity over a non-canonical digest string.
  auto plan = oneRolePlan("t004-ack-junk", "not-a-digest");
  BOOST_CHECK_THROW(NativePlanSealer::sealCore(plan.snapshot, plan.proposal, plan.inputs),
                    std::invalid_argument);
  // The same guard holds on the sealed-core object: tampering the ACK field
  // into a non-canonical string invalidates the core identity.
  const auto ackDigest = digest("t004-ack");
  auto valid = oneRolePlan("t004-ack-boundary", ackDigest);
  auto core = NativePlanSealer::sealCore(valid.snapshot, valid.proposal, valid.inputs);
  core.ackClosedDigest = "not-a-digest";
  BOOST_CHECK_THROW(core.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(PlanSealerPlaintextPolicyNeedsNoGrantCover)
{
  // A plaintext policy (requireProtectedArtifacts=false) is the frozen
  // plaintext-v1 analog: no protected role needs a grant, the projection is
  // still single-sourced and carries no grant binding.
  const auto ackDigest = digest("t004-ack");
  auto plan = oneRolePlan("t004-plaintext", ackDigest);
  plan.inputs.protectionEpoch = "plaintext-v1";
  const NativeSecurityPolicySnapshot plaintext{digest("t004-security"), false};
  const auto core = NativePlanSealer::sealCore(plan.snapshot, plan.proposal, plan.inputs);
  const auto sealed = NativePlanSealer::finalizeSecurity(core, {}, plaintext);
  checkSha256Format(sealed.planDigest);
  const auto projection = NativePlanSealer::project(sealed, "provider-a");
  BOOST_CHECK(!projection.hasGrantBinding);
  BOOST_CHECK_EQUAL(projection.securityPolicySnapshotDigest,
                    plaintext.policyDigest);
  BOOST_CHECK(!NativePlanSealer::encode(projection).empty());
}

BOOST_AUTO_TEST_CASE(PlanSealerEncodeRejectsIncompleteProjectionsAndEscapesCanonically)
{
  // encode() is the canonical fragment boundary: incomplete projections are
  // rejected at the first boundary, and string values are JSON-escaped
  // canonically so the fragment round-trips through an independent parser.
  NativeSelectionProjectionV3 incomplete;
  incomplete.provider = "provider-a";
  incomplete.requestId = "request";
  incomplete.attempt = 1;
  incomplete.planDigest = digest("t004-encode");
  incomplete.selectedRole.role = "/LLM/Pipeline/Stage/0";
  BOOST_CHECK(!NativePlanSealer::encode(incomplete).empty());

  auto copy = incomplete;
  copy.provider.clear();
  BOOST_CHECK_THROW(NativePlanSealer::encode(copy), std::invalid_argument);
  copy.provider = "provider-a";
  copy.requestId.clear();
  BOOST_CHECK_THROW(NativePlanSealer::encode(copy), std::invalid_argument);
  copy.requestId = "request";
  copy.attempt = 0;
  BOOST_CHECK_THROW(NativePlanSealer::encode(copy), std::invalid_argument);
  copy.attempt = 1;
  copy.planDigest = "not-a-digest";
  BOOST_CHECK_THROW(NativePlanSealer::encode(copy), std::invalid_argument);
  copy.planDigest = digest("t004-encode");
  copy.selectedRole.role.clear();
  BOOST_CHECK_THROW(NativePlanSealer::encode(copy), std::invalid_argument);

  // Canonical escaping: quotes and backslashes inside string values survive a
  // JSON parse round-trip unchanged; the fragment never drops keys even when
  // optional digest fields are absent.
  NativeSelectionProjectionV3 escaping;
  escaping.provider = "pro\"vider\\a";
  escaping.requestId = "req\t004";
  escaping.attempt = 3;
  escaping.planDigest = digest("t004-escape");
  escaping.selectedRole.role = "/LLM/Pipeline/Stage/0";
  const auto bytes = NativePlanSealer::encode(escaping);
  const std::string wire(bytes.begin(), bytes.end());
  boost::property_tree::ptree tree;
  {
    std::istringstream in(wire);
    boost::property_tree::read_json(in, tree);
  }
  BOOST_CHECK_EQUAL(tree.get<std::string>("provider"), escaping.provider);
  BOOST_CHECK_EQUAL(tree.get<std::string>("request_id"), escaping.requestId);
  BOOST_CHECK_EQUAL(tree.get<std::uint64_t>("attempt"), escaping.attempt);
  BOOST_CHECK_EQUAL(tree.get<std::string>("plan_digest"), escaping.planDigest);
  // plan_core_digest/ack_closed_digest are absent on this projection yet the
  // fragment still carries them as empty strings (fixed canonical layout).
  BOOST_CHECK_EQUAL(tree.get<std::string>("plan_core_digest", ""), "");
  BOOST_CHECK_EQUAL(tree.get<std::string>("ack_closed_digest", ""), "");
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
  BOOST_CHECK_EXCEPTION(NativePlanSealer::sealCore(plan.snapshot, plan.proposal, plan.inputs),
                        std::invalid_argument, [](const std::invalid_argument& e) {
                          return std::string(e.what()) == "native plan requires one role per Provider";
                        });
}

BOOST_AUTO_TEST_SUITE_END()
