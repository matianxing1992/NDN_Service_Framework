#include "NDNSF-DistributedInference/cpp/ndnsf-di/extensions.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeV3Placement.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeAuthenticatedGrantClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeObservedOfferV3.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeQwenPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/yolo/NativeYoloPlanner.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"
#include "tests/fixtures/spec182/native-sealing-fixture.hpp"

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <fstream>
#include <memory>
#include <openssl/evp.h>
#include <thread>
#include <type_traits>

namespace ndnsf::di {
namespace {

class TestAdapter final : public NativeModelAdapter
{
public:
  explicit TestAdapter(std::string id) : m_id(std::move(id)) {}

  std::string adapterId() const override { return m_id; }
  std::string adapterVersion() const override { return "test"; }
  NativeModelDescriptor inspect(const std::string&, const std::string&) const override { return {}; }
  std::vector<std::uint8_t> encodeInput(const std::vector<std::uint8_t>& input) const override
  {
    return input;
  }
  std::vector<std::uint8_t> decodeResult(const std::vector<std::uint8_t>& result) const override
  {
    return result;
  }

private:
  std::string m_id;
};

class StatefulRunner final : public NativeModelRunner
{
public:
  std::map<std::string, TensorBundle> run(const RoleExecutionContext&) override
  {
    ++invocations;
    return {};
  }

  int invocations = 0;
};

class TestCooperativePlacement final : public CooperativePlacementStrategy
{
public:
  explicit TestCooperativePlacement(std::string name) : m_name(std::move(name)) {}

  NativeStrategyIdentity identity() const override
  {
    return {m_name, "1", nativePlanningDigest(m_name)};
  }

  NativeRolePlacementProposalV3 proposeRoles(
    const NativeOfferBindingContext&, const std::string&,
    const std::vector<NativeSelectionRoleV3>&,
    const std::vector<NativeAdmittedOfferV3>&, std::uint64_t,
    const ExtensionControl& control) const override
  {
    control.requireActive();
    return {};
  }

private:
  std::string m_name;
};

class TestCooperativeSplitter final : public CooperativeModelSplitStrategy
{
public:
  TestCooperativeSplitter() = default;
  explicit TestCooperativeSplitter(NativeSplitCandidate candidate,
                                   std::chrono::milliseconds delay = {})
    : m_candidate(std::move(candidate)), m_delay(delay) {}

  NativeStrategyIdentity identity() const override
  {
    return {"test-cooperative-splitter", "1", nativePlanningDigest("test-cooperative-splitter")};
  }

  std::vector<NativeSplitCandidate> enumerate(
    const NativeModelDescriptor&, const NativeGraphSnapshot&, const NativeCandidateBudget&,
    const ExtensionControl& control) const override
  {
    if (m_delay.count() > 0)
      std::this_thread::sleep_for(m_delay);
    control.requireActive();
    return m_candidate ? std::vector<NativeSplitCandidate>{*m_candidate}
                        : std::vector<NativeSplitCandidate>{};
  }

private:
  std::optional<NativeSplitCandidate> m_candidate;
  std::chrono::milliseconds m_delay{};
};

class LateCancelPlacement final : public CooperativePlacementStrategy
{
public:
  explicit LateCancelPlacement(std::shared_ptr<std::atomic<bool>> cancelled)
    : m_cancelled(std::move(cancelled)) {}

  NativeStrategyIdentity identity() const override
  {
    return {"late-cancel-placement", "1", nativePlanningDigest("late-cancel-placement")};
  }

  NativeRolePlacementProposalV3 proposeRoles(
    const NativeOfferBindingContext&, const std::string&,
    const std::vector<NativeSelectionRoleV3>&,
    const std::vector<NativeAdmittedOfferV3>&, std::uint64_t,
    const ExtensionControl& control) const override
  {
    control.requireActive();
    m_cancelled->store(true);
    NativeRolePlacementProposalV3 result;
    result.strategy = identity();
    return result;
  }

private:
  std::shared_ptr<std::atomic<bool>> m_cancelled;
};

std::shared_ptr<EVP_PKEY> testEd25519Key(char value)
{
  const std::string bytes(32, value);
  return std::shared_ptr<EVP_PKEY>(EVP_PKEY_new_raw_private_key(
    EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()),
    EVP_PKEY_free);
}

std::string qwenFixtureGraphDigest(const std::string& modelName,
                                   const std::string& precision,
                                   const std::string& revision,
                                   const std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges)
{
  auto encodedRanges = NativeJson::array();
  for (const auto& range : ranges)
    encodedRanges.push_back(NativeJson::array({range.first, range.second}));
  auto nodes = NativeJson::array({"embedding", "layer-00", "layer-01", "final-norm-head"});
  auto legalCuts = NativeJson::array({"hidden-embedding-to-layer-00",
                                      "hidden-layer-0-to-1",
                                      "hidden-layer-1-to-final"});
  return nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"model", modelName}, {"revision", revision}, {"precision", precision},
    {"decode_mode", "single-token-autoregressive"}, {"modality", "text-only"},
    {"mtp_enabled", false}, {"thinking_mode", "disabled"}, {"layer_ranges", encodedRanges},
    {"nodes", nodes}, {"edges", legalCuts}, {"legal_cuts", legalCuts}}));
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec185ExtensionRegistry)

BOOST_AUTO_TEST_CASE(ExtensionControlStopsCancellationAndDeadline)
{
  std::atomic<bool> cancelled{false};
  ExtensionControl control{std::chrono::steady_clock::now() + std::chrono::seconds(1),
                           [&cancelled] { return cancelled.load(); }};
  BOOST_CHECK_NO_THROW(control.requireActive());
  cancelled.store(true);
  BOOST_CHECK_THROW(control.requireActive(), std::runtime_error);
  ExtensionControl expired{std::chrono::steady_clock::now() - std::chrono::milliseconds(1), {}};
  BOOST_CHECK_THROW(expired.requireActive(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(BuiltinCooperativeSplitterChecksControlInsideLoops)
{
  using qwen::NativeQwenLayerSplit;
  const auto digest = [](const std::string& value) { return nativePlanningDigest(value); };
  const auto role0 = std::string("/Qwen/Stage/0");
  const auto role1 = std::string("/Qwen/Stage/1");
  const auto graphDigest = qwenFixtureGraphDigest("QwenFixture", "float32", "revision", {{0, 1}, {1, 2}});
  const auto model = fixture::completeModel({"QwenFixture", digest("model"), digest("semantics"),
    graphDigest, "onnx", "float32", "qwen", "1"});
  NativeQwenLayerSplit splitter({{0, 1}, {1, 2}},
    {{role0, digest("artifact-0")}, {role1, digest("artifact-1")}},
    {{role0, 1}, {role1, 1}}, {role0, role1}, {1, 1});
  const auto graph = splitter.inspectGraph(model, "revision", 64);
  std::atomic<int> checks{0};
  ExtensionControl control{std::chrono::steady_clock::now() + std::chrono::seconds(1),
                           [&checks] { return checks.fetch_add(1) >= 2; }};
  BOOST_CHECK_THROW(splitter.enumerate(model, graph, {1, 100, 1}, control), std::runtime_error);
  BOOST_CHECK_GE(checks.load(), 2);
}

BOOST_AUTO_TEST_CASE(BuiltinYoloCooperativeSplitterChecksControlInsideLoops)
{
  const auto digest = [](const std::string& value) { return nativePlanningDigest(value); };
  auto model = fixture::completeModel({"YoloFixture", digest("yolo-model"), digest("semantics"),
    digest("yolo-graph"), "onnx", "float32", "yolo", "1"});
  ndnsf::di::NativeGraphSnapshot graph;
  graph.graphDigest = model.graphDigest;
  graph.nodes = {{"input", "Input", 0}};
  graph.topologicalOrder = {"input"};
  yolo::NativeYoloComponentSpec component;
  component.candidateId = "full";
  component.roles = {"FullModel"};
  component.inputIngressRole = "FullModel";
  component.resultEgressRole = "FullModel";
  component.candidateDigest = digest("registered-yolo-candidate");
  yolo::NativeYoloComponentSplit splitter({component});
  std::atomic<int> checks{0};
  ExtensionControl control{std::chrono::steady_clock::now() + std::chrono::seconds(1),
                           [&checks] { return checks.fetch_add(1) >= 2; }};
  BOOST_CHECK_THROW(splitter.enumerate(model, graph, {1, 100, 1}, control), std::runtime_error);
  BOOST_CHECK_GE(checks.load(), 3);
}

BOOST_AUTO_TEST_CASE(CooperativePlannerRejectsCancelledRequestBeforePublication)
{
  std::ifstream file("tests/fixtures/spec182/placement-v3-oracle.json");
  BOOST_REQUIRE(file.good());
  const auto oracle = NativeJson::parse(file);
  NativeOfferAdmission admission(oracle.at("policy").dump(),
    {{oracle.at("key_id").get<std::string>(), oracle.at("public_pem").get<std::string>()}},
    oracle.at("candidate"));
  NativeRequestPreparation cancelledPreparation(std::make_shared<NativeAdapterRegistry>());
  TestCooperativeSplitter splitter;
  TestCooperativePlacement placement("test-cooperative-placement");
  NativeRequestControl control{"cancelled", 1,
    std::chrono::steady_clock::now() + std::chrono::seconds(1), [] { return true; }};
  const auto wireDeadline = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count() + 60000);
  BOOST_CHECK_THROW(planNativeRequestCooperative(
    NativeRequestRuntime{}, {}, NativeInspectedModel{}, NativeEncodedRequest{}, splitter, placement,
    cancelledPreparation, admission, ndn_service_framework::CollaborationAckClosure{}, control,
    wireDeadline, nullptr), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(CooperativePlannerInvokesStrategyAndStopsBeforeSelection)
{
  std::ifstream file("tests/fixtures/spec182/placement-v3-oracle.json");
  BOOST_REQUIRE(file.good());
  const auto oracle = NativeJson::parse(file);
  const auto& sample = oracle.at("seal_cases").at(0);
  const auto digest = [](const std::string& value) { return nativePlanningDigest(value); };
  NativeOfferAdmission admission(oracle.at("policy").dump(),
    {{oracle.at("key_id").get<std::string>(), oracle.at("public_pem").get<std::string>()}},
    oracle.at("candidate"));
  const auto offerWire = sample.at("offers").at(0).get<std::string>();
  const auto offer = decodeNativeProviderOfferV3(offerWire);
  ndn_service_framework::AckSelectionCandidate ack;
  ack.providerName = ndn::Name(offer.provider);
  ack.serviceName = ndn::Name(offer.service);
  ack.requestId = ndn::Name(offer.requestId);
  ack.ack.setStatus(offer.status);
  ndn::Buffer payload(offerWire.begin(), offerWire.end());
  ack.ack.setPayload(payload, payload.size());
  ack.authenticationEvidence = {offer.provider, offer.provider + "/KEY/fixture/issuer/v=1",
                                "sha256:" + std::string(64, '1'), true};
  const auto role0 = std::string("/Qwen/Stage/0");
  const auto role1 = std::string("/Qwen/Stage/1");
  const auto graphDigest = qwenFixtureGraphDigest("QwenFixture", "float32", "revision", {{0, 1}, {1, 2}});
  auto descriptor = fixture::completeModel({"QwenFixture", digest("model"), digest("semantics"),
    graphDigest, "onnx", "float32", "qwen", "1"});
  qwen::NativeQwenLayerSplit nativeSplitter({{0, 1}, {1, 2}},
    {{role0, digest("artifact-0")}, {role1, digest("artifact-1")}},
    {{role0, 1}, {role1, 1}}, {role0, role1}, {1, 1});
  auto graph = nativeSplitter.inspectGraph(descriptor, "revision", 64);
  descriptor.graphDigest = oracle.at("graph_digest");
  graph.graphDigest = descriptor.graphDigest;
  auto candidate = nativeSplitter.enumerate(descriptor, graph, {1, 1000, 1}).front();
  candidate.splitter = {"test-cooperative-splitter", "1", digest("test-cooperative-splitter"), true};
  candidate.candidateDigest = candidate.computedDigest();
  const auto strictBudgetCandidate = candidate;
  NativePlanSealingInputs roleInputs;
  roleInputs.artifacts.manifestDigest = digest("manifest");
  roleInputs.artifacts.recipeDigest = digest("recipe");
  roleInputs.artifacts.graphDigest = descriptor.graphDigest;
  roleInputs.artifacts.canonicalGraphDigest = descriptor.graphDigest;
  roleInputs.protectionEpoch = "epoch";
  for (const auto& role : candidate.executionPlan.roles)
    roleInputs.artifacts.artifactDigestByRole[role] = candidate.artifactsByRole.at(role).front();
  fixture::assemblies(roleInputs);
  std::vector<NativeSelectionRoleV3> preparedRoles;
  for (auto& item : roleInputs.assemblyByRole) {
    item.second.requiredDeviceMemoryMb = 4096;
    preparedRoles.push_back(item.second);
  }
  NativeInspectedModel inspected{descriptor, graph, "/catalog/model", digest("source"),
    roleInputs.artifacts.manifestDigest, roleInputs.artifacts.canonicalGraphDigest};
  NativeRequestRuntime runtime;
  runtime.contract.serviceName = offer.service;
  runtime.contract.taskName = "task";
  runtime.contract.adapterName = descriptor.adapterId;
  runtime.contract.adapterDescriptorDigest = descriptor.adapter.descriptorDigest();
  runtime.contract.adapterCompositionDigest = digest("composition");
  runtime.contract.taskDescriptorDigest = digest("task");
  runtime.requesterIdentity = "/requester";
  runtime.protectionEpoch = "epoch";
  runtime.security = {digest("runtime-policy"), true};
  runtime.budget = {1, 1000, 1};
  runtime.grants = std::make_shared<NativeAuthenticatedGrantClient>(
    "/requester", testEd25519Key('a'), "/authority", std::string(32, 'b'), "epoch",
    [](const NativeSignedGrantRequest&, const std::string&, std::uint64_t,
       const NativeGrantControl&) { return NativeKeyGrant{}; },
    [](const std::string&, const std::string&, const NativeGrantControl&) { return std::string{}; });
  NativeEncodedRequest encoded;
  encoded.modelIntentDigest = oracle.at("model_digest");
  encoded.requestContractDigest = digest("request-contract");
  ndn_service_framework::CollaborationAckClosure closure;
  closure.requestId = ndn::Name(offer.requestId);
  closure.digest = oracle.at("ack_digest");
  closure.candidates = {ack};
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  NativeRequestControl control{offer.requestId, offer.attempt,
    std::chrono::steady_clock::now() + std::chrono::seconds(5),
    [cancelled] { return cancelled->load(); }};
  std::atomic<int> artifactPublications{0};
  NativeRequestPreparation preparation(std::make_shared<NativeAdapterRegistry>(), {},
    [&artifactPublications](const NativeInspectedModel&, const NativeSplitCandidate&,
                            const std::vector<NativeSelectionRoleV3>&,
                            const NativeRequestControl&) {
      ++artifactPublications;
      return NativeArtifactBinding{};
    },
    [&preparedRoles](const NativeInspectedModel&, const NativeSplitCandidate&,
                     const NativeRequestControl&) { return preparedRoles; });
  TestCooperativeSplitter splitter(std::move(candidate));
  LateCancelPlacement placement(cancelled);
  BOOST_CHECK_THROW(planNativeRequestCooperative(runtime, {}, inspected, encoded, splitter, placement,
    preparation, admission, closure, control, 2000000000000ULL, nullptr), std::runtime_error);
  BOOST_CHECK(cancelled->load());
  BOOST_CHECK_EQUAL(artifactPublications.load(), 0);

  // Keep an independent strict policy-budget counterexample in this selector.
  // The production planner derives the cooperative extension deadline from
  // maxPolicyMs and must stop a slow strategy before it can publish artifacts.
  cancelled->store(false);
  auto strictRuntime = runtime;
  strictRuntime.budget.maxPolicyMs = 1;
  TestCooperativeSplitter slowSplitter(strictBudgetCandidate,
                                       std::chrono::milliseconds(5));
  NativeRequestControl strictControl{offer.requestId, offer.attempt,
    std::chrono::steady_clock::now() + std::chrono::seconds(5),
    [cancelled] { return cancelled->load(); }};
  BOOST_CHECK_EXCEPTION(planNativeRequestCooperative(
    strictRuntime, {}, inspected, encoded, slowSplitter, NativePreSplitFirstPlacement(),
    preparation, admission, closure, strictControl, 2000000000000ULL, nullptr),
    std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()) == "cooperative extension deadline exceeded";
    });
  BOOST_CHECK_EQUAL(artifactPublications.load(), 0);
}

BOOST_AUTO_TEST_CASE(CooperativePlacementRejectsExpiredControlBeforeSelection)
{
  const ExtensionControl expired{std::chrono::steady_clock::now() - std::chrono::milliseconds(1), {}};
  BOOST_CHECK_THROW(NativePreSplitFirstPlacement().proposeRoles(
    {}, "", {}, {}, 0, expired), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(LegacyPlacementVtableCannotEnterCooperativeRuntime)
{
  BOOST_CHECK((!std::is_base_of<CooperativePlacementStrategy, NativePlacementStrategy>::value));
  BOOST_CHECK((!std::is_convertible<NativePlacementStrategy*, CooperativePlacementStrategy*>::value));
}

BOOST_AUTO_TEST_CASE(AdapterRegistryUsesExplicitReplaceBeforeFreeze)
{
  NativeAdapterRegistry registry;
  registry.registerAdapter(std::make_shared<TestAdapter>("test"));
  BOOST_CHECK_THROW(registry.registerAdapter(std::make_shared<TestAdapter>("test")),
                    std::invalid_argument);
  BOOST_CHECK_THROW(registry.replaceAdapter(std::make_shared<TestAdapter>("missing")),
                    std::out_of_range);
  registry.replaceAdapter(std::make_shared<TestAdapter>("test"));
  registry.freeze();
  BOOST_CHECK(registry.frozen());
  BOOST_CHECK(registry.find("test"));
  std::vector<std::future<bool>> readers;
  for (int i = 0; i < 8; ++i) {
    readers.emplace_back(std::async(std::launch::async, [&registry] {
      for (int n = 0; n < 1000; ++n)
        if (!registry.find("test")) return false;
      return true;
    }));
  }
  for (auto& reader : readers) BOOST_CHECK(reader.get());
  BOOST_CHECK_THROW(registry.registerAdapter(std::make_shared<TestAdapter>("late")),
                    std::logic_error);
  BOOST_CHECK_THROW(registry.replaceAdapter(std::make_shared<TestAdapter>("test")),
                    std::logic_error);
}

BOOST_AUTO_TEST_CASE(RunnerFactoryRejectsImplicitOverwriteAndFreezes)
{
  RegistryNativeModelRunnerFactory factory;
  auto creator = [] (const NativeModelRunnerSpec&) {
    return std::make_shared<StatefulRunner>();
  };
  factory.registerBackend("test", creator);
  BOOST_CHECK_THROW(factory.registerBackend("test", creator), std::invalid_argument);
  BOOST_CHECK_THROW(factory.replaceBackend("missing", creator), std::out_of_range);
  factory.replaceBackend("test", creator);
  factory.freeze();
  BOOST_CHECK(factory.frozen());
  auto first = factory.create(NativeModelRunnerSpec{"/r", "kind", "test", "", {}});
  auto second = factory.create(NativeModelRunnerSpec{"/r", "kind", "test", "", {}});
  BOOST_REQUIRE(first);
  BOOST_REQUIRE(second);
  BOOST_CHECK(first.get() != second.get());
  first->run({});
  second->run({});
  BOOST_CHECK_EQUAL(std::dynamic_pointer_cast<StatefulRunner>(first)->invocations, 1);
  BOOST_CHECK_EQUAL(std::dynamic_pointer_cast<StatefulRunner>(second)->invocations, 1);
  std::vector<std::future<bool>> readers;
  for (int i = 0; i < 8; ++i) {
    readers.emplace_back(std::async(std::launch::async, [&factory] {
      for (int n = 0; n < 1000; ++n) {
        if (!factory.hasBackend("test")) return false;
        if (!factory.create(NativeModelRunnerSpec{"/r", "kind", "test", "", {}})) return false;
      }
      return true;
    }));
  }
  for (auto& reader : readers) BOOST_CHECK(reader.get());
  BOOST_CHECK_THROW(factory.registerBackend("late", creator), std::logic_error);
  BOOST_CHECK_THROW(factory.replaceBackend("test", creator), std::logic_error);
}

BOOST_AUTO_TEST_CASE(CooperativePlacementRegistryIsReadOnlyAfterFreeze)
{
  NativePlacementStrategyRegistry registry;
  registry.registerStrategy("primary", std::make_shared<TestCooperativePlacement>("primary"));
  BOOST_CHECK_THROW(registry.registerStrategy("alias", std::make_shared<TestCooperativePlacement>("primary")),
                    std::invalid_argument);
  BOOST_CHECK_THROW(registry.registerStrategy("primary", std::make_shared<TestCooperativePlacement>("duplicate")),
                    std::invalid_argument);
  registry.replaceStrategy("primary", std::make_shared<TestCooperativePlacement>("primary"));
  registry.freeze();
  BOOST_CHECK(registry.frozen());
  auto first = registry.find("primary");
  BOOST_REQUIRE(first);
  BOOST_CHECK_EQUAL(first->identity().name, "primary");

  std::vector<std::future<bool>> readers;
  for (int i = 0; i < 8; ++i) {
    readers.emplace_back(std::async(std::launch::async, [&registry] {
      for (int n = 0; n < 1000; ++n)
        if (!registry.find("primary")) return false;
      return true;
    }));
  }
  for (auto& reader : readers) BOOST_CHECK(reader.get());
  BOOST_CHECK_THROW(registry.registerStrategy("late", std::make_shared<TestCooperativePlacement>("late")),
                    std::logic_error);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di
