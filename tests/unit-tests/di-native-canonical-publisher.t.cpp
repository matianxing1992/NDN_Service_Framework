#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalArtifactPublisher.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeV3Placement.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"
#include "tests/fixtures/spec182/native-sealing-fixture.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeYoloMergeRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalRolePreparer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalPreparationCatalog.hpp"

#include <fstream>
#include <future>
#include <utility>

namespace ndnsf::di {
class NativeCanonicalPublisherTestAccess
{
public:
  using Transport = NativeCanonicalArtifactPublisher::Transport;
  static NativeCanonicalArtifactPublisher create(Transport transport,
    NativeCanonicalPublicationOptions options, NativeCanonicalArtifactPublisher::SourcePort source)
  { return NativeCanonicalArtifactPublisher(std::move(transport), "/service", std::move(options), std::move(source)); }
};
class NativeCanonicalCatalogTestAccess
{
public:
  static std::shared_ptr<NativeRequestPreparation> create(
    const NativeCanonicalPreparationCatalog& catalog, NativeCanonicalPublisherTestAccess::Transport transport)
  {
    return catalog.makePreparation([transport](auto options, auto source) {
      return NativeCanonicalPublisherTestAccess::create(transport, std::move(options), std::move(source));
    });
  }
};
}

namespace {
using namespace ndnsf::di;
using namespace ndn_service_framework;
using Clock = std::chrono::steady_clock;

std::vector<std::uint8_t> unhex(const std::string& text)
{
  std::vector<std::uint8_t> bytes;
  for (std::size_t i = 0; i < text.size(); i += 2)
    bytes.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
  return bytes;
}

struct Input
{
  std::shared_ptr<NativeCanonicalSource> source = std::make_shared<NativeCanonicalSource>();
  NativeInspectedModel model;
  NativeSplitCandidate candidate;
  std::vector<NativeSelectionRoleV3> roles;
  NativeCanonicalPublicationOptions options;
  NativeRequestControl control{"/request", 1, Clock::now() + std::chrono::seconds(10), {}};
  explicit Input(bool external = false)
  {
    std::ifstream stream("tests/fixtures/spec182/dependency-probes/extraction-vectors.json");
    if (!stream) throw std::runtime_error("missing frozen ONNX source fixture");
    const auto vectors = NativeJson::parse(stream);
    const auto& row = vectors.at("cases")[external ? 1 : 0];
    source->modelBytes = unhex(row.at("modelHex"));
    if (external) source->initializerBytes = unhex(row.at("initializerHex"));
    const auto& recipe = row.at("recipe");
    const auto check = validateNativeOnnxWorkerMetadata(NativeJson{
      {"schema", kNativeOnnxAssemblyRequestSchema}, {"recipe", recipe},
      {"recipeDigest", nativePlanningDigest(recipe.dump())},
      {"backend", "onnxruntime-cpu"}, {"adapterId", "fixture"}}.dump());
    if (!check.ok) throw std::runtime_error(check.failureMessage);
    auto role = check.value.recipe;
    role.role = role.selectedRole = "/role"; role.adapterVersion = "1";
    role.artifactDigest = row.at("expectedModelDigest");
    role.recipeDigest = check.value.recipeDigest; role.requiredDeviceMemoryMb = 1;
    role.protectionEpoch = "artifact-policy-epoch";
    roles = {role};
    model.descriptor = {"fixture-model", nativePlanningDigest("model"), nativePlanningDigest("semantics"),
      nativePlanningDigest("planning"), "onnx", "fp32", "fixture", "1"};
    model.descriptor = fixture::completeModel(model.descriptor);
    model.graph.graphDigest = model.descriptor.graphDigest;
    model.graph.nodes = {{"n0", "Identity", 0}, {"n1", "Identity", 1}};
    model.graph.topologicalOrder = {"n0", "n1"};
    model.canonicalSourceName = "/fixture/source";
    model.canonicalSourceDigest = nativePlanningDigest(source->modelBytes.data(), source->modelBytes.size());
    model.canonicalSourceBytes = source->modelBytes.size();
    model.modelManifestDigest = role.modelManifestDigest;
    model.canonicalGraphDigest = role.graphDigest;
    if (external) {
      model.canonicalInitializerBytes = source->initializerBytes->size();
      model.canonicalInitializerObjectDigest = nativePlanningDigest(source->initializerBytes->data(), source->initializerBytes->size());
    }
    candidate.model = model.descriptor; candidate.graphDigest = model.graph.graphDigest;
    candidate.splitter = {"fixture", "1", nativePlanningDigest("splitter")};
    candidate.source = "PRE_SPLIT";
    candidate.executionPlan.roles = {role.role}; candidate.tensorDegreesByRole = {{role.role, 1}};
    for (const auto& node : model.graph.nodes) candidate.nodeRoles[node.id] = role.role;
    candidate.artifactsByRole = {{role.role, {role.artifactDigest}}};
    candidate.rankArtifactDigestsByRole = candidate.artifactsByRole;
    candidate.fragmentsByRole = {{role.role, role.artifactDigest}};
    candidate.requirementsByRole = {{role.role, {{"onnxruntime"}, 1, 0, 0, 0, 0, 1.0}}};
    candidate.candidateDigest = candidate.computedDigest();
    options = {"/fixture/NDNSF/DI/ARTIFACT", model.modelManifestDigest, {role.artifactDigest}};
  }
  NativeCanonicalArtifactPublisher::SourcePort resolver()
  { return [value = source](const auto&, const auto&) { return value; }; }
  NativeRolePlacementProposalV3 proposal() const
  {
    NativeRolePlacementProposalV3 p;
    p.context = {control.requestId, control.attempt, "/service", model.descriptor.intentDigest(),
      model.graph.graphDigest, 2000000000000ULL};
    p.ackClosedDigest = nativePlanningDigest("ack"); p.strategy = candidate.splitter;
    p.roles = roles; p.providerByRole = {{roles[0].selectedRole, "/provider"}};
    p.offerDigestByProvider = {{"/provider", nativePlanningDigest("offer")}};
    return p;
  }
};

struct TransportFixture
{
  std::vector<std::vector<std::uint8_t>> payloads;
  std::vector<std::string> requests;
  std::function<void(LargeDataPublishResult&)> mutate;
  NativeCanonicalPublisherTestAccess::Transport transport()
  {
    return {[](auto job) { job(); }, [] { return false; },
      [] { return PreparedServiceRequest{ndn::Name("/service"), ndn::Name("/publication-request")}; },
      [this](const PreparedServiceRequest& request, const std::vector<std::uint8_t>& bytes, const std::string& label) {
        payloads.push_back(bytes); requests.push_back(request.requestId.toUri());
        LargeDataPublishResult result;
        result.success = true; result.encrypted = true; result.objectId = label;
        result.encryptedDataName = ndn::Name("/encrypted").append(label).appendVersion(1);
        result.plaintextSize = bytes.size(); result.contentDigest = nativePlanningDigest(bytes.data(), bytes.size());
        result.manifestDigest = nativePlanningDigest("Core transport metadata");
        result.authorizationScope = "/SERVICE/service"; result.protectionEpoch = "Core epoch";
        if (mutate) mutate(result);
        return result;
      }};
  }
};
}

BOOST_AUTO_TEST_SUITE(Spec182CanonicalPublisher)
BOOST_AUTO_TEST_CASE(CatalogComposesOwnedInputInspectionRolesAndPublication)
{
  for (bool external : {false, true}) {
    Input input(external); TransportFixture io;
    const auto& original = input.roles.front();
    NativeCanonicalCatalogEntry entry;
    entry.model = input.model; entry.source = *input.source;
    entry.recipe = {original.artifactProfileDigest, original.assemblerDescriptorDigest,
      original.backendAbi, original.precision, original.quantization, original.layout, original.padding,
      original.protectionEpoch, original.maxSourceBytes, original.maxAssembledBytes, original.maxNodes};
    entry.nodes = {{"n0", {0}}, {"n1", {1}}}; entry.publication = input.options;
    entry.format = external ? NativeCatalogModelAdapter::Format::JsonBytes : NativeCatalogModelAdapter::Format::OpaqueBytes;
    entry.maxPayloadBytes = 1024;
    NativeAssemblyControl control{input.control.deadline, [&] { input.control.requireActive(); },
      entry.recipe.maxSourceBytes, entry.recipe.maxAssembledBytes};
    auto alternate = entry;
    alternate.model.descriptor.modelName = "second-model";
    alternate.model.descriptor.contentDigest = nativePlanningDigest("second-model");
    auto catalog = std::make_unique<NativeCanonicalPreparationCatalog>(
      std::vector<NativeCanonicalCatalogEntry>{entry, alternate}, control);
    const auto registry = catalog->adapters();
    BOOST_CHECK(registry->frozen());
    const auto preparation = NativeCanonicalCatalogTestAccess::create(*catalog, io.transport());
    auto abstract = input.candidate;
    abstract.roleStateInputsByRole["/role"] = {{"state-in", "float32", {std::string("state")}, std::nullopt}};
    abstract.roleStateOutputsByRole["/role"] = {{"state-out", "float32", {std::string("state")}, std::nullopt}};
    abstract.candidateDigest = abstract.computedDigest();
    const NativeStateTensorMapping stateMap{{{"/role", {{"state-in", {"X"}}}}},
      {{"/role", {{"state-out", {"Y"}}}}}};
    const auto concrete = catalog->bindStateContracts(entry.model, abstract, stateMap, input.control);
    BOOST_CHECK_NE(concrete.candidateDigest, abstract.candidateDigest);
    BOOST_CHECK_EQUAL(concrete.roleStateInputsByRole.at("/role").front().name, "X");
    BOOST_CHECK_EQUAL(*concrete.roleStateInputsByRole.at("/role").front().estimatedBytes, 16);
    BOOST_CHECK_NO_THROW(preparation->prepareRoles(entry.model, concrete, input.control));
    for (const auto& bad : std::vector<NativeStateTensorMapping>{
      {}, {{{"foreign-role", {{"state-in", {"X"}}}}}, stateMap.outputs},
      {{{"/role", {{"foreign-state", {"X"}}}}}, stateMap.outputs},
      {{{"/role", {{"state-in", {"Y"}}}}}, stateMap.outputs},
      {{{"/role", {{"state-in", {"X", "X"}}}}}, stateMap.outputs},
      {{{"/role", {{"state-in", {}}}}}, stateMap.outputs}})
      BOOST_CHECK_THROW(catalog->bindStateContracts(entry.model, abstract, bad, input.control), std::invalid_argument);
    auto wrongDtype = abstract;
    wrongDtype.roleStateInputsByRole["/role"].front().dtype = "int64";
    wrongDtype.candidateDigest = wrongDtype.computedDigest();
    BOOST_CHECK_THROW(catalog->bindStateContracts(entry.model, wrongDtype, stateMap, input.control), std::invalid_argument);
    BOOST_CHECK_EQUAL(abstract.roleStateInputsByRole.at("/role").front().name, "state-in");
    BOOST_CHECK_THROW(catalog->makePreparation(nullptr, "/service"), std::invalid_argument);
    catalog.reset(); // Returned ports own the pinned state, not the factory.
    input.source->modelBytes.clear(); // Caller mutation must not change the owned publication source.
    const std::vector<std::uint8_t> payload = external ? std::vector<std::uint8_t>{'{', '}'} :
      std::vector<std::uint8_t>{0, 255, 1};
    const auto prepared = preparation->prepareInput(entry.model.descriptor, "inference",
      nativePlanningDigest("input"), nativePlanningDigest("options"), payload, {}, input.control.deadline);
    BOOST_CHECK(prepared.payload == payload);
    const auto inspected = preparation->inspectModel(prepared);
    BOOST_CHECK_EQUAL(inspected.canonicalSourceDigest, entry.model.canonicalSourceDigest);
    const auto secondInput = preparation->prepareInput(alternate.model.descriptor, "inference",
      nativePlanningDigest("input"), nativePlanningDigest("options"), payload, {}, input.control.deadline);
    BOOST_CHECK_EQUAL(preparation->inspectModel(secondInput).descriptor.contentDigest, alternate.model.descriptor.contentDigest);
    BOOST_CHECK(registry->find(inspected.descriptor.adapterId)->decodeResult(payload) == payload);
    auto roles = preparation->prepareRoles(inspected, input.candidate, input.control);
    BOOST_REQUIRE_EQUAL(roles.size(), 1);
    auto proposal = input.proposal(); proposal.roles = roles;
    proposal.roles.front().backend = original.backend;
    const auto published = preparation->ensureArtifacts(inspected, input.candidate, proposal, input.control);
    const auto rebound = NativeRequestPreparation::bindPublishedRoles(inspected, input.candidate, proposal.roles, published);
    BOOST_CHECK_EQUAL(assembleNativeCertifiedOnnxModel(entry.source, rebound.front(), control).modelDigest, original.artifactDigest);
    BOOST_REQUIRE(!io.payloads.empty());
    BOOST_CHECK_EQUAL(nativePlanningDigest(io.payloads.front().data(), io.payloads.front().size()), entry.model.canonicalSourceDigest);
    auto foreign = prepared;
    foreign.expectedModel.semanticsDigest = nativePlanningDigest("foreign semantics");
    BOOST_CHECK_THROW(preparation->inspectModel(foreign), std::runtime_error);
    auto wrongSource = inspected; wrongSource.canonicalSourceName = "/foreign/source";
    BOOST_CHECK_THROW(preparation->prepareRoles(wrongSource, input.candidate, input.control), std::invalid_argument);
    const auto publishedCount = io.payloads.size();
    auto cancelled = input.control; cancelled.cancelled = [] { return true; };
    BOOST_CHECK_THROW(preparation->ensureArtifacts(inspected, input.candidate, proposal, cancelled), std::runtime_error);
    BOOST_CHECK_EQUAL(io.payloads.size(), publishedCount);
    BOOST_CHECK_THROW(NativeCanonicalPreparationCatalog(std::vector<NativeCanonicalCatalogEntry>{entry, entry}, control), std::invalid_argument);
    auto conflict = entry; conflict.maxPayloadBytes = 2048;
    BOOST_CHECK_THROW(NativeCanonicalPreparationCatalog(std::vector<NativeCanonicalCatalogEntry>{entry, conflict}, control), std::invalid_argument);
    auto corrupt = entry; corrupt.source.modelBytes.front() ^= 1;
    BOOST_CHECK_THROW(NativeCanonicalPreparationCatalog(std::vector<NativeCanonicalCatalogEntry>{corrupt}, control), std::invalid_argument);
    BOOST_CHECK_THROW(NativeCanonicalPreparationCatalog({}, control), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(StateBindingConsumesActualCausalOnnxExport)
{
  Input input;
  std::ifstream file("tests/fixtures/spec175/tiny-causal-lm-v1/one-role/role-0.onnx", std::ios::binary);
  BOOST_REQUIRE(file.good());
  input.source->modelBytes.assign(std::istreambuf_iterator<char>(file), {});
  input.model.canonicalSourceBytes = input.source->modelBytes.size();
  input.model.canonicalSourceDigest = nativePlanningDigest(input.source->modelBytes.data(), input.source->modelBytes.size());
  // Frozen Spec175 manifest; no expected source is generated by the code under test.
  BOOST_CHECK_EQUAL(input.model.canonicalSourceDigest, "sha256:fe41db5c2c8397b62fc8983307b0d755706b7008c56958882cb247ca46594f82");
  NativeAssemblyControl control{input.control.deadline, [&] { input.control.requireActive(); }, 1024 * 1024, 1024 * 1024};
  const auto actual = inspectNativeOnnxSourceGraph(*input.source, input.model.descriptor, control);
  const auto repeated = std::find_if(actual.graph.edges.begin(), actual.graph.edges.end(),
    [](const auto& edge) { return edge.id == "hidden_zero"; });
  BOOST_REQUIRE(repeated != actual.graph.edges.end());
  BOOST_CHECK_EQUAL(repeated->consumers.size(), 2);
  BOOST_CHECK_EQUAL(nativeParseJson(actual.graphMetadataJson).at("tensorConsumers").at("hidden_zero").size(), 7);
  input.model.graph = actual.graph; input.model.descriptor.graphDigest = actual.graph.graphDigest;
  input.model.canonicalGraphDigest = actual.canonicalIdentity.graphDigest;
  input.candidate.model = input.model.descriptor; input.candidate.graphDigest = actual.graph.graphDigest;
  input.candidate.nodeRoles.clear();
  for (const auto& node : actual.graph.nodes) input.candidate.nodeRoles[node.id] = "/role";
  NativeStateTensorMapping mapping;
  for (const std::string family : {"attention_kv", "recurrent_state", "convolution_state"}) {
    input.candidate.roleStateInputsByRole["/role"].push_back({family + "_in", "float32", {std::string("abstract-state")}, std::nullopt});
    input.candidate.roleStateOutputsByRole["/role"].push_back({family + "_out", "float32", {std::string("abstract-state")}, std::nullopt});
    mapping.inputs["/role"][family + "_in"] = {family + "_in"};
    mapping.outputs["/role"][family + "_out"] = {family + "_out"};
  }
  input.candidate.candidateDigest = input.candidate.computedDigest();
  const auto& original = input.roles.front();
  const NativeRoleRecipeProfile profile{original.artifactProfileDigest, original.assemblerDescriptorDigest,
    original.backendAbi, original.precision, original.quantization, original.layout, original.padding,
    original.protectionEpoch, control.maxSourceBytes, control.maxAssembledBytes, 1024};
  const NativeCanonicalRolePreparer owner(input.model, *input.source, profile, control);
  BOOST_CHECK_THROW(owner.prepare(input.model, input.candidate, input.control), std::invalid_argument);
  const auto bound = owner.bindStateContracts(input.model, input.candidate, mapping, input.control);
  BOOST_CHECK_NE(bound.candidateDigest, input.candidate.candidateDigest);
  const std::vector<std::variant<std::int64_t, std::string>> stateShape = {std::int64_t(4), std::int64_t(8)};
  for (const auto* contracts : {&bound.roleStateInputsByRole, &bound.roleStateOutputsByRole}) {
    BOOST_REQUIRE_EQUAL(contracts->at("/role").size(), 3);
    for (const auto& tensor : contracts->at("/role")) {
      BOOST_CHECK(tensor.shape == stateShape);
      BOOST_REQUIRE(tensor.estimatedBytes);
      BOOST_CHECK_EQUAL(*tensor.estimatedBytes, 128);
    }
  }
  BOOST_CHECK_NO_THROW(owner.prepare(input.model, bound, input.control));
  auto cancelled = input.control; cancelled.cancelled = [] { return true; };
  BOOST_CHECK_THROW(owner.bindStateContracts(input.model, input.candidate, mapping, cancelled), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(PublishesOwnedInlineAndExternalSourcesThroughPreparation)
{
  for (bool external : {false, true}) {
    for (bool explicitRanks : {false, true}) {
    Input input(external); TransportFixture io;
    if (!explicitRanks) {
      input.candidate.tensorDegreesByRole.clear();
      input.candidate.rankArtifactDigestsByRole.clear();
      input.candidate.candidateDigest = input.candidate.computedDigest();
    }
    auto publisher = NativeCanonicalPublisherTestAccess::create(io.transport(), input.options, input.resolver());
    NativeRequestPreparation preparation(std::make_shared<NativeAdapterRegistry>(), {}, publisher.artifactPort());
    const auto result = preparation.ensureArtifacts(input.model, input.candidate, input.proposal(), input.control);
    BOOST_REQUIRE_EQUAL(io.payloads.size(), external ? 3 : 2);
    BOOST_CHECK(io.payloads[0] == input.source->modelBytes);
    if (external) BOOST_CHECK(io.payloads[1] == *input.source->initializerBytes);
    BOOST_CHECK(std::all_of(io.requests.begin(), io.requests.end(), [&](const auto& r) { return r == io.requests.front(); }));
    BOOST_CHECK_EQUAL(result.requestId, input.control.requestId);
    BOOST_CHECK_EQUAL(result.canonicalGraphDigest, input.model.canonicalGraphDigest);
    BOOST_CHECK_NE(result.manifestDigest, nativePlanningDigest("Core transport metadata"));
    BOOST_CHECK_NE(result.manifestDigest, input.model.modelManifestDigest);
    BOOST_CHECK_EQUAL(result.manifestDigest, nativePlanningDigest(io.payloads.back().data(), io.payloads.back().size()));
    const auto root = nativeParseJson(result.canonicalManifestJson);
    BOOST_CHECK_EQUAL(root.at("metadata").at("canonicalSourceDigest").get<std::string>(), input.model.canonicalSourceDigest);
    BOOST_CHECK_EQUAL(root.at("metadata").at("packageManifestDigest").get<std::string>(), input.model.modelManifestDigest);
    BOOST_CHECK_NE(result.sourceByRole.at("/role"), result.artifactNameByRole.at("/role"));
    BOOST_CHECK(result.artifactNameByRole.at("/role").find("//") == std::string::npos);
    const auto certified = NativeRequestPreparation::bindPublishedRoles(input.model, input.candidate, input.roles, result);
    BOOST_CHECK_EQUAL(certified[0].modelManifestDigest, result.manifestDigest);
    BOOST_CHECK_NE(certified[0].recipeDigest, input.roles[0].recipeDigest);
    BOOST_CHECK(result.artifactNameByRole.at("/role").find("/rank/") == std::string::npos);
    }
  }
}

BOOST_AUTO_TEST_CASE(RejectsSourceAndCanonicalIdentityBeforePublication)
{
  for (unsigned poison = 0; poison != 4; ++poison) {
    Input input(true); TransportFixture io;
    if (poison == 0) input.source->modelBytes[0] ^= 1;
    if (poison == 1) (*input.source->initializerBytes)[0] ^= 1;
    if (poison == 2) input.roles[0].canonicalInitializerDigest = nativePlanningDigest("foreign");
    if (poison == 3) {
      input.model.canonicalGraphDigest = input.roles[0].graphDigest = nativePlanningDigest("foreign graph");
    }
    auto publisher = NativeCanonicalPublisherTestAccess::create(io.transport(), input.options, input.resolver());
    BOOST_CHECK_THROW(publisher(input.model, input.candidate, input.roles, input.control), std::invalid_argument);
    BOOST_CHECK(io.payloads.empty());
  }
}

BOOST_AUTO_TEST_CASE(CanonicalRoleProducerMapsSemanticNodesToActualOnnxSource)
{
  for (bool external : {false, true}) {
    Input input(external);
    // One semantic node owns two real ONNX nodes; source index 1 is not a
    // planning ordinal. No recipe or tensor contract is injected into the port.
    input.model.graph.nodes = {{"layer-00", "decoder-layer", 0}};
    input.model.graph.topologicalOrder = {"layer-00"};
    input.candidate.nodeRoles = {{"layer-00", "/role"}};
    input.candidate.candidateDigest = input.candidate.computedDigest();
    const auto& original = input.roles.front();
    NativeRoleRecipeProfile profile{original.artifactProfileDigest, original.assemblerDescriptorDigest,
      original.backendAbi, original.precision, original.quantization, original.layout, original.padding,
      original.protectionEpoch, original.maxSourceBytes, original.maxAssembledBytes, original.maxNodes};
    NativeAssemblyControl control{input.control.deadline, [&] { input.control.requireActive(); },
      profile.maxSourceBytes, profile.maxAssembledBytes};
    const auto sourceGraph = inspectNativeOnnxSourceGraph(*input.source, input.model.descriptor, control);
    BOOST_CHECK_EQUAL(sourceGraph.graph.nodes.size(), 2);
    BOOST_CHECK_NE(sourceGraph.graph.graphDigest, input.model.graph.graphDigest);
    BOOST_CHECK_THROW(inspectNativeOnnxPlanningGraph(*input.source, input.model.descriptor, control), std::invalid_argument);
    BOOST_CHECK_THROW(NativeCanonicalRolePreparer(input.model, *input.source, profile, control), std::invalid_argument);
    const NativeCanonicalRolePreparer owner(input.model, *input.source, profile, control, {{"layer-00", {0, 1}}});
    NativeRequestPreparation preparation(std::make_shared<NativeAdapterRegistry>(), {}, {}, owner.rolePort());
    const auto roles = preparation.prepareRoles(input.model, input.candidate, input.control);
    BOOST_REQUIRE_EQUAL(roles.size(), 1);
    const auto& role = roles.front();
    BOOST_CHECK(role.nodeIndices == std::vector<std::uint64_t>({0, 1}));
    BOOST_CHECK_EQUAL(role.roleKind, "COMPONENT_SET");
    BOOST_CHECK_EQUAL(role.layerBegin, 0); BOOST_CHECK_EQUAL(role.layerEnd, 0);
    BOOST_REQUIRE_EQUAL(role.expectedInputs.size(), 1);
    BOOST_CHECK_EQUAL(role.expectedInputs.front().name, "X");
    BOOST_REQUIRE_EQUAL(role.expectedOutputs.size(), 1);
    BOOST_CHECK_EQUAL(role.expectedOutputs.front().name, "Y");
    BOOST_CHECK_EQUAL(role.adapterDescriptorDigest, input.model.descriptor.adapter.descriptorDigest());
    const auto assembled = assembleNativeCertifiedOnnxModel(*input.source, role, control);
    BOOST_CHECK_EQUAL(assembled.nodeCount, 2);
    BOOST_CHECK_EQUAL(assembled.modelDigest, original.artifactDigest);
    TransportFixture io;
    auto publisher = NativeCanonicalPublisherTestAccess::create(io.transport(), input.options, input.resolver());
    NativeRequestPreparation publishing(std::make_shared<NativeAdapterRegistry>(), {}, publisher.artifactPort(), owner.rolePort());
    auto selected = input.proposal(); selected.roles = roles;
    selected.roles.front().backend = original.backend; // Explicit CPU placement fixture.
    const auto published = publishing.ensureArtifacts(input.model, input.candidate, selected, input.control);
    const auto rebound = NativeRequestPreparation::bindPublishedRoles(input.model, input.candidate, selected.roles, published);
    BOOST_CHECK_EQUAL(rebound.front().modelManifestDigest, published.manifestDigest);
    BOOST_CHECK_EQUAL(assembleNativeCertifiedOnnxModel(*input.source, rebound.front(), control).modelDigest, original.artifactDigest);
    // Resource-bound rejection is separate from exact source-node membership.
    for (const auto& mapping : std::vector<NativeCanonicalRolePreparer::NodeMap>{
           {{"layer-00", {0, 2}}}, {{"layer-00", {0, 0}}}, {{"layer-00", {0}}}, {{"foreign", {0, 1}}}})
      BOOST_CHECK_THROW(NativeCanonicalRolePreparer(input.model, *input.source, profile, control, mapping), std::invalid_argument);
    auto foreign = input.model; foreign.canonicalSourceDigest = nativePlanningDigest("foreign");
    BOOST_CHECK_THROW(NativeCanonicalRolePreparer(foreign, *input.source, profile, control,
      {{"layer-00", {0, 1}}}), std::invalid_argument);
    foreign = input.model; foreign.modelManifestDigest = nativePlanningDigest("foreign");
    BOOST_CHECK_THROW(owner.prepare(foreign, input.candidate, input.control), std::invalid_argument);
    auto wrongState = input.candidate;
    wrongState.roleStateInputsByRole["/role"] = {{"missing-state", "float32", {std::int64_t(1)}, 4}};
    wrongState.candidateDigest = wrongState.computedDigest();
    BOOST_CHECK_THROW(owner.prepare(input.model, wrongState, input.control), std::invalid_argument);
    auto cancelled = input.control; cancelled.cancelled = [] { return true; };
    BOOST_CHECK_THROW(owner.prepare(input.model, input.candidate, cancelled), std::runtime_error);
    // A genuinely identical source/planning graph needs no semantic mapping.
    auto direct = input.model;
    direct.graph = sourceGraph.graph; direct.descriptor.graphDigest = direct.graph.graphDigest;
    auto directCandidate = input.candidate;
    directCandidate.model = direct.descriptor; directCandidate.graphDigest = direct.graph.graphDigest;
    directCandidate.nodeRoles.clear();
    for (const auto& node : direct.graph.nodes) directCandidate.nodeRoles[node.id] = "/role";
    directCandidate.candidateDigest = directCandidate.computedDigest();
    const NativeCanonicalRolePreparer directOwner(direct, *input.source, profile, control);
    BOOST_CHECK(directOwner.prepare(direct, directCandidate, input.control).front().nodeIndices == role.nodeIndices);
    auto aliases = input.model;
    aliases.graph.nodes = {{"layer-00", "layer", 0}, {"layer-0", "layer", 1}};
    aliases.graph.topologicalOrder = {"layer-00", "layer-0"};
    auto aliasesCandidate = input.candidate;
    aliasesCandidate.nodeRoles = {{"layer-00", "/role"}, {"layer-0", "/role"}};
    aliasesCandidate.candidateDigest = aliasesCandidate.computedDigest();
    const NativeCanonicalRolePreparer aliasesOwner(aliases, *input.source, profile, control,
      {{"layer-00", {0}}, {"layer-0", {1}}});
    BOOST_CHECK_THROW(aliasesOwner.prepare(aliases, aliasesCandidate, input.control), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(NativeMergePublishesAndSealsWithoutOnnxRecipe)
{
  for (bool external : {false, true}) {
    Input input(external); TransportFixture io;
    auto merge = input.roles.front();
    merge.role = merge.selectedRole = "/Merge";
    merge.roleKind = "COMPONENT_SET"; merge.layerBegin = merge.layerEnd = 0;
    merge.nodeIndices = {1};
    merge.mergeKind = "NATIVE_POSTPROCESS";
    merge.postprocessIdentity = "YOLO26n-canonical-detection-rows";
    merge.postprocessOutputName = "predictions";
    merge.postprocessConfidenceThreshold = .001;
    merge.postprocessSort = "confidence-desc,class-asc,xyxy-asc";
    merge.expectedOutputs = {{"predictions", "float32", {std::int64_t(1), std::int64_t(300), std::int64_t(6)}}};
    merge.artifactDigest = nativePlanningDigest("native-merge-fragment");
    merge.recipeDigest = nativePlanningDigest("native-merge-candidate-recipe");
    merge.canonicalInitializerDigest.clear(); merge.assemblerDescriptorDigest.clear();
    merge.backendAbi.clear(); merge.precision.clear(); merge.quantization.clear();
    merge.layout.clear(); merge.padding.clear();
    merge.maxSourceBytes = merge.maxAssembledBytes = merge.maxNodes = 0;
    // Native Merge first exercises source-limit selection independently of role order.
    input.roles.insert(input.roles.begin(), merge);
    auto& candidate = input.candidate;
    candidate.executionPlan.roles.insert(candidate.executionPlan.roles.begin(), merge.role);
    candidate.nodeRoles["n1"] = merge.role;
    candidate.artifactsByRole[merge.role] = {merge.artifactDigest};
    candidate.rankArtifactDigestsByRole[merge.role] = {merge.artifactDigest};
    candidate.tensorDegreesByRole[merge.role] = 1;
    candidate.fragmentsByRole[merge.role] = merge.artifactDigest;
    candidate.requirementsByRole[merge.role] = {{"onnxruntime"}, 1, 0, 0, 0, 0, 1.0};
    candidate.inputIngressRole = "/role"; candidate.resultEgressRole = merge.role;
    candidate.mergeKind = merge.mergeKind;
    candidate.postprocessingJson = R"({"identity":"YOLO26n-canonical-detection-rows","outputName":"predictions","confidenceThreshold":0.001,"sort":"confidence-desc,class-asc,xyxy-asc"})";
    candidate.candidateDigest = candidate.computedDigest();
    std::ifstream file("tests/fixtures/spec182/native-merge-offers.json");
    const auto signedOffers = NativeJson::parse(file);
    NativeOfferAdmission admission(signedOffers.at("policy").dump(),
      {{signedOffers.at("key_id"), signedOffers.at("public_pem")}}, signedOffers.at("candidate"));
    const auto context = input.proposal().context;
    std::vector<NativeAdmittedOfferV3> offers;
    for (const auto& item : signedOffers.at("offers")) {
      const auto wire = item.get<std::string>();
      const auto offer = decodeNativeProviderOfferV3(wire);
      AckSelectionCandidate ack;
      ack.providerName = ndn::Name(offer.provider); ack.serviceName = ndn::Name(offer.service);
      ack.requestId = ndn::Name(offer.requestId); ack.ack.setStatus(offer.status);
      ndn::Buffer payload(wire.begin(), wire.end()); ack.ack.setPayload(payload, payload.size());
      // Core authentication evidence fixture; the signature is checked by production admission.
      ack.authenticationEvidence = {offer.provider, offer.provider + "/KEY/fixture/issuer/v=1",
        "sha256:" + std::string(64, '1'), true};
      offers.push_back(admission.verify(ack, context, 200));
    }
    const auto proposal = NativePreSplitFirstPlacement{}.proposeRoles(context,
      nativePlanningDigest("ack"), input.roles, offers, 200);
    auto publisher = NativeCanonicalPublisherTestAccess::create(io.transport(), input.options, input.resolver());
    NativeRequestPreparation preparation(std::make_shared<NativeAdapterRegistry>(), {}, publisher.artifactPort());
    const auto artifacts = preparation.ensureArtifacts(input.model, candidate, proposal, input.control);
    BOOST_REQUIRE_EQUAL(io.payloads.size(), external ? 3 : 2);
    NativePlanSealingInputs inputs;
    inputs.artifacts = artifacts; inputs.requesterIdentity = "/requester";
    inputs.protectionEpoch = merge.protectionEpoch; inputs.expiresAtMs = context.deadlineMs;
    for (const auto& role : input.roles) inputs.assemblyByRole.emplace(role.selectedRole, role);
    auto execution = candidate.executionPlan;
    execution.roles.clear();
    for (const auto& role : proposal.roles) execution.roles.push_back(role.selectedRole);
    execution.serviceName = context.serviceName; execution.modelName = input.model.descriptor.modelName;
    const auto core = NativePlanSealer::sealCore(input.model, candidate, proposal, execution,
      offers, proposal.ackClosedDigest, inputs);
    const auto& native = core.assemblyByRole.at(merge.role);
    BOOST_CHECK_EQUAL(native.modelManifestDigest, artifacts.manifestDigest);
    BOOST_CHECK_NE(native.modelManifestDigest, input.model.modelManifestDigest);
    BOOST_CHECK_EQUAL(native.recipeDigest, merge.recipeDigest);
    BOOST_CHECK(native.canonicalInitializerDigest.empty());
    BOOST_CHECK(native.assemblerDescriptorDigest.empty());
    BOOST_CHECK_EQUAL(native.maxSourceBytes, 0);
    BOOST_CHECK_NE(core.assemblyByRole.at("/role").recipeDigest, input.roles.back().recipeDigest);
    for (const auto& offer : offers)
      BOOST_CHECK_EQUAL(NativePlanSealer::grantView(core, offer,
        {nativePlanningDigest("policy"), true}).modelManifestDigest, artifacts.manifestDigest);
    // Grant transport is a fixture; both bindings retain the protected epoch.
    const auto sealed = NativePlanSealer::finalizeSecurity(core,
      {{"/provider/a", "/role", "/grant/a", nativePlanningDigest("grant-a"), "/provider/a"},
       {"/provider/b", "/Merge", "/grant/b", nativePlanningDigest("grant-b"), "/provider/b"}},
      {nativePlanningDigest("policy"), true});
    const auto projection = NativePlanSealer::project(sealed, "/provider/b",
      fixture::projection(sealed, "/provider/b"));
    const auto spec = nativeYoloMergeRunnerSpecFromProjection(projection);
    BOOST_CHECK(!validateNativePreparedRunnerSpec(projection, spec));
    BOOST_CHECK(spec.path.empty());
    BOOST_CHECK_NO_THROW(NativePlanSealer::encode(projection));
    auto poisoned = core;
    poisoned.assemblyByRole.at(merge.role).modelManifestDigest = input.model.modelManifestDigest;
    BOOST_CHECK_THROW(poisoned.validate(), std::invalid_argument);
    poisoned = core;
    poisoned.assemblyByRole.at(merge.role).postprocessOutputName = "other";
    BOOST_CHECK_THROW(poisoned.validate(), std::invalid_argument);

    for (unsigned poison = 0; poison != 4; ++poison) {
      auto roles = input.roles;
      if (poison == 0) roles.front().modelManifestDigest = nativePlanningDigest("foreign");
      if (poison == 1) roles.front().mergeKind.clear();
      if (poison == 2) roles.front().expectedOutputs.clear();
      if (poison == 3) roles.front().graphDigest = nativePlanningDigest("foreign");
      const auto published = io.payloads.size();
      BOOST_CHECK_THROW(publisher(input.model, candidate, roles, input.control), std::exception);
      BOOST_CHECK_EQUAL(io.payloads.size(), published);
    }
    // Without an ONNX role, publication has no certified source resource bound.
    auto solo = candidate;
    solo.executionPlan.roles = {merge.role}; solo.inputIngressRole = merge.role;
    solo.artifactsByRole.erase("/role"); solo.rankArtifactDigestsByRole.erase("/role");
    solo.tensorDegreesByRole.erase("/role"); solo.fragmentsByRole.erase("/role");
    solo.requirementsByRole.erase("/role"); solo.nodeRoles["n0"] = merge.role;
    solo.candidateDigest = solo.computedDigest();
    BOOST_CHECK_THROW(publisher(input.model, solo, {merge}, input.control), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(RejectsInvalidCoreReceiptsBeforePublishingRoot)
{
  for (unsigned poison = 0; poison != 7; ++poison) {
    Input input; TransportFixture io;
    io.mutate = [poison](auto& r) {
      if (poison == 0) r.success = false;
      if (poison == 1) r.encrypted = false;
      if (poison == 2) ++r.plaintextSize;
      if (poison == 3) r.contentDigest = nativePlanningDigest("foreign");
      if (poison == 4) r.manifestDigest.clear();
      if (poison == 5) r.authorizationScope = "/SERVICE/foreign";
      if (poison == 6) r.protectionEpoch.clear();
    };
    auto publisher = NativeCanonicalPublisherTestAccess::create(io.transport(), input.options, input.resolver());
    BOOST_CHECK_THROW(publisher(input.model, input.candidate, input.roles, input.control), std::runtime_error);
    BOOST_CHECK_EQUAL(io.payloads.size(), 1);
  }
}

BOOST_AUTO_TEST_CASE(QueuedCancellationAndTimeoutReleaseSourceAndSuppressLateWork)
{
  for (bool timeout : {false, true}) {
    Input input; TransportFixture io;
    std::atomic<bool> cancelled{false};
    input.control.cancelled = [&] { return cancelled.load(); };
    if (timeout) input.control.deadline = Clock::now() + std::chrono::milliseconds(150);
    std::promise<std::function<void()>> queued;
    auto ready = queued.get_future();
    auto transport = io.transport();
    transport.post = [&](auto work) { queued.set_value(std::move(work)); };
    const std::weak_ptr<NativeCanonicalSource> source = input.source;
    auto publisher = NativeCanonicalPublisherTestAccess::create(transport, input.options,
      [&](const auto&, const auto&) { return std::exchange(input.source, {}); });
    auto pending = std::async(std::launch::async, [&] {
      return publisher(input.model, input.candidate, input.roles, input.control);
    });
    BOOST_REQUIRE(ready.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    auto work = ready.get();
    if (!timeout) cancelled = true;
    BOOST_REQUIRE(pending.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    BOOST_CHECK_THROW(pending.get(), std::runtime_error);
    BOOST_CHECK(source.expired());
    work();
    BOOST_CHECK(io.payloads.empty());
  }
}

BOOST_AUTO_TEST_CASE(PreservesCoreFailureBoundary)
{
  Input input; TransportFixture io;
  io.mutate = [](auto& result) { result.success = false; result.errorMessage = "fixture missing wrapped key"; };
  auto publisher = NativeCanonicalPublisherTestAccess::create(io.transport(), input.options, input.resolver());
  try {
    publisher(input.model, input.candidate, input.roles, input.control);
    BOOST_FAIL("expected Core publication failure");
  }
  catch (const std::runtime_error& error) {
    BOOST_CHECK_EQUAL(error.what(), "DI_NATIVE_ENCRYPTED_PUBLICATION_FAILED: fixture missing wrapped key");
  }
  BOOST_CHECK_EQUAL(io.payloads.size(), 1);
}

BOOST_AUTO_TEST_CASE(CancellationAfterSourceSuppressesInitializerAndRoot)
{
  Input input(true); TransportFixture io;
  bool cancelled = false;
  input.control.cancelled = [&] { return cancelled; };
  io.mutate = [&](auto&) { cancelled = true; };
  auto publisher = NativeCanonicalPublisherTestAccess::create(io.transport(), input.options, input.resolver());
  BOOST_CHECK_THROW(publisher(input.model, input.candidate, input.roles, input.control), std::runtime_error);
  BOOST_CHECK_EQUAL(io.payloads.size(), 1);
}

BOOST_AUTO_TEST_CASE(UsesRealCoreIoAndEncryptedPublicationWithLocalMockKey)
{
  using namespace ndn_service_framework::test;
  Input input;
  ndn::security::KeyChain keyChain{"pib-memory:", "tpm-memory:"};
  ndn::DummyClientFace face{keyChain};
  auto cert = makeRsaIdentity(keyChain, ndn::Name("/publisher-test/user"));
  auto aa = makeRsaIdentity(keyChain, ndn::Name("/publisher-test/aa"));
  auto user = std::make_shared<LocalServiceUser>(face, ndn::Name("/publisher-test"), cert, aa, "examples/trust-any.conf");
  user->useSigningKeyChainForTest(keyChain);
  // Unit fixture only: seed Core's existing LocalMock wrapped-key state. The
  // actual publish API still encrypts, signs and stores its segmented envelope.
  user->prepareHybridSendKeyForTest(ndn::Name("/service"), "REQUEST-LARGE");
  NativeCanonicalArtifactPublisher publisher(user, "/service", input.options, input.resolver());
  bool ioRejected = false;
  user->postToIo([&] {
    try { publisher(input.model, input.candidate, input.roles, input.control); }
    catch (const std::runtime_error& e) { ioRejected = std::string(e.what()) == "DI_NATIVE_PUBLICATION_IO_WAIT_FORBIDDEN"; }
  });
  face.getIoContext().poll();
  BOOST_CHECK(ioRejected);
  auto pending = std::async(std::launch::async, [&] {
    return publisher(input.model, input.candidate, input.roles, input.control);
  });
  while (pending.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
    face.getIoContext().restart(); face.getIoContext().poll();
  }
  const auto result = pending.get();
  const auto name = ndn::Name(result.sourceByRole.at("/role"));
  BOOST_CHECK(user->hasCachedDataForTest(name));
  BOOST_CHECK_NE(user->getCachedDataContentForTest(name),
    ndn::Buffer(result.canonicalManifestJson.begin(), result.canonicalManifestJson.end()));
  BOOST_CHECK_NE(result.manifestDigest, input.model.modelManifestDigest);
}
BOOST_AUTO_TEST_SUITE_END()
