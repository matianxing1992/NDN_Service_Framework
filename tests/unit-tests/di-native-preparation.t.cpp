// Spec182Preparation: frozen ports of NativeRequestPreparation against the
// two model task adapters (Qwen pipeline bytes identity, YOLO canonical JSON
// document boundary) and the certified artifact binding checks.  The suite
// mirrors the Python oracle semantics (BytesGenerationTaskAdapter in
// qwen/placement.py, JsonTaskAdapter in adapters/base.py, CanonicalCatalogEnsurer
// in artifact_deployment.py) at the native byte boundary.  Real publication
// behind the artifact port is exercised at T016; here the port is a fake that
// the orchestration layer must never accept an inconsistent binding from.

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeV3Placement.hpp"
#include "tests/fixtures/spec182/native-sealing-fixture.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/test/unit_test.hpp>
#include <openssl/sha.h>

#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace ndnsf::di {
namespace {

std::string digest(const std::string& value)
{
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), hash);
  std::string result = "sha256:";
  for (const auto byte : hash) {
    result += "0123456789abcdef"[byte >> 4];
    result += "0123456789abcdef"[byte & 15];
  }
  return result;
}

std::vector<std::uint8_t> bytes(const std::string& value)
{
  return {value.begin(), value.end()};
}

std::chrono::steady_clock::time_point deadline(std::int64_t msFromNow)
{
  return std::chrono::steady_clock::now() +
    std::chrono::milliseconds(msFromNow);
}

void expectCode(std::function<void()> body, const std::string& code)
{
  try {
    body();
  } catch (const std::runtime_error& error) {
    BOOST_CHECK_EQUAL(std::string(error.what()), code);
    return;
  }
  BOOST_ERROR("expected runtime error " + code);
}

// YOLO JSON document check: the encoding facade (bindings, T012) produces
// canonical JSON with the Python canonical encoder; the native task boundary
// only validates document well-formedness and passes bytes through unchanged
// ("already encoded bytes are not re-encoded", M17).
std::vector<std::uint8_t> jsonValidated(std::vector<std::uint8_t> value)
{
  std::istringstream text({value.begin(), value.end()});
  boost::property_tree::ptree document;
  try {
    boost::property_tree::read_json(text, document);
  } catch (const std::exception&) {
    throw std::runtime_error("YOLO task payload is not a JSON document");
  }
  return value;
}

NativeGraphSnapshot graphFor(const NativeModelDescriptor& model)
{
  NativeGraphSnapshot graph;
  graph.graphDigest = model.graphDigest;
  // Three independent planning nodes support the one/two/three-role binding
  // fixtures without claiming that one node belongs to several roles.
  graph.nodes = {{"node", "Identity", 0}, {"node1", "Identity", 1}, {"node2", "Identity", 2}};
  graph.topologicalOrder = {"node", "node1", "node2"};
  return graph;
}

NativeInspectedModel inspectedFor(const NativeModelDescriptor& model)
{
  return {model, graphFor(model), "/catalog/authenticated/model/42",
          digest("catalog-source-bytes"), digest("manifest"), model.graphDigest};
}

NativeRolePlacementProposalV3 proposalFor(const NativeRequestControl& control,
                                         const NativeInspectedModel& model,
                                         std::vector<std::string> roles)
{
  NativeRolePlacementProposalV3 proposal;
  // Explicit long-lived test context; publication fixtures are not live ACK evidence.
  proposal.context = {control.requestId, control.attempt, "/service",
    model.descriptor.contentDigest, model.graph.graphDigest, 2000000000000ULL};
  proposal.ackClosedDigest = digest("ack");
  proposal.strategy = {"fixture", "1", digest("strategy")};
  for (const auto& name : roles) {
    NativePlanSealingInputs inputs;
    inputs.artifacts.artifactDigestByRole = {{name, digest(name == "role" ? "artifact" : "artifact-" + name)}};
    inputs.artifacts.manifestDigest = model.modelManifestDigest;
    inputs.artifacts.graphDigest = model.graph.graphDigest;
    inputs.artifacts.canonicalGraphDigest = model.canonicalGraphDigest;
    inputs.artifacts.recipeDigest = digest("recipe");
    inputs.protectionEpoch = "protected";
    fixture::assemblies(inputs);
    auto role = inputs.assemblyByRole.at(name);
    role.requiredDeviceMemoryMb = 1;
    role.adapterId = model.descriptor.adapterId;
    role.adapterVersion = model.descriptor.adapterVersion;
    proposal.roles.push_back(role);
    const auto provider = "/provider/" + std::to_string(proposal.roles.size());
    proposal.providerByRole[name] = provider;
    proposal.offerDigestByProvider[provider] = digest(provider);
  }
  return proposal;
}

NativeSplitCandidate candidateFor(const NativeInspectedModel& model,
                                  const NativeRolePlacementProposalV3& proposal)
{
  NativeSplitCandidate candidate;
  candidate.model = model.descriptor; candidate.graphDigest = model.graph.graphDigest;
  candidate.splitter = {"fixture", "1", digest("strategy")};
  candidate.candidateDigest = digest("candidate");
  for (const auto& role : proposal.roles) {
    candidate.executionPlan.roles.push_back(role.role);
    candidate.fragmentsByRole[role.role] = digest("fragment");
    candidate.artifactsByRole[role.role] = {digest(role.role == "role" ? "artifact" : "artifact-" + role.role)};
    candidate.tensorDegreesByRole[role.role] = 1;
    candidate.rankArtifactDigestsByRole[role.role] = candidate.artifactsByRole.at(role.role);
    candidate.requirementsByRole[role.role] = {{"onnxruntime"}, 1, 0, 0, 0, 1.0};
  }
  for (std::size_t i = 0; !candidate.executionPlan.roles.empty() && i < model.graph.nodes.size(); ++i)
    candidate.nodeRoles[model.graph.nodes[i].id] = candidate.executionPlan.roles.at(i % candidate.executionPlan.roles.size());
  return candidate;
}

NativeArtifactBinding ensureArtifacts(const NativeRequestPreparation& preparation,
  const NativeInspectedModel& model, const NativeRolePlacementProposalV3& proposal,
  const NativeRequestControl& control)
{
  return preparation.ensureArtifacts(model, candidateFor(model, proposal), proposal, control);
}

// Parameterized model task adapter: mirrors the frozen Python task adapters
// with one encode/decode pair per model semantics.
class TaskFixtureAdapter : public NativeModelAdapter
{
public:
  TaskFixtureAdapter(std::string adapterId, std::string adapterVersion,
                     std::string modelFormat, std::string precision,
                     std::string semanticsLabel, std::string graphLabel,
                     std::function<std::vector<std::uint8_t>(
                       const std::vector<std::uint8_t>&)> encode,
                     std::function<std::vector<std::uint8_t>(
                       const std::vector<std::uint8_t>&)> decode)
    : m_adapterId(std::move(adapterId)), m_adapterVersion(std::move(adapterVersion)),
      m_modelFormat(std::move(modelFormat)), m_precision(std::move(precision)),
      m_semanticsDigest(digest(semanticsLabel)), m_graphDigest(digest(graphLabel)),
      m_encode(std::move(encode)), m_decode(std::move(decode))
  {}

  std::string adapterId() const override { return m_adapterId; }
  std::string adapterVersion() const override { return m_adapterVersion; }

  NativeModelDescriptor inspect(const std::string& name,
                                const std::string& content) const override
  {
    NativeModelDescriptor model;
    model.modelName = name;
    model.contentDigest = content;
    model.semanticsDigest = m_semanticsDigest;
    model.graphDigest = m_graphDigest;
    model.modelFormat = m_modelFormat;
    model.precision = m_precision;
    model.adapterId = m_adapterId;
    model.adapterVersion = m_adapterVersion;
    return model;
  }

  std::vector<std::uint8_t> encodeInput(
    const std::vector<std::uint8_t>& applicationInput) const override
  {
    return m_encode(applicationInput);
  }

  std::vector<std::uint8_t> decodeResult(
    const std::vector<std::uint8_t>& nativeResult) const override
  {
    return m_decode(nativeResult);
  }

private:
  std::string m_adapterId;
  std::string m_adapterVersion;
  std::string m_modelFormat;
  std::string m_precision;
  std::string m_semanticsDigest;
  std::string m_graphDigest;
  std::function<std::vector<std::uint8_t>(
    const std::vector<std::uint8_t>&)> m_encode;
  std::function<std::vector<std::uint8_t>(
    const std::vector<std::uint8_t>&)> m_decode;
};

std::vector<std::uint8_t> identityBytes(
  const std::vector<std::uint8_t>& value)
{
  return value;
}

// Fixed application-facing descriptor for one registered adapter; the
// semantics/graph digests must be the ones the adapter's inspect answers.
NativeModelDescriptor modelDescriptor(const TaskFixtureAdapter& adapter,
                                      const std::string& modelName = "model",
                                      const std::string& semanticsLabel = "semantics",
                                      const std::string& graphLabel = "graph")
{
  return {modelName, digest("model"), digest(semanticsLabel), digest(graphLabel),
          adapter.inspect(modelName, digest("model")).modelFormat,
          adapter.inspect(modelName, digest("model")).precision, adapter.adapterId(), adapter.adapterVersion()};
}

// Adapter that answers under one registered id but inspects as another model
// identity (a catalog or adapter mix-up the preparation must reject).
class MislabeledIdentityAdapter final : public TaskFixtureAdapter
{
public:
  using TaskFixtureAdapter::TaskFixtureAdapter;

  NativeModelDescriptor inspect(const std::string& name,
                                const std::string& content) const override
  {
    auto model = TaskFixtureAdapter::inspect(name, content);
    model.adapterId = "yolo26n-task";
    model.modelFormat = "onnx";
    return model;
  }
};

} // namespace

BOOST_AUTO_TEST_SUITE(Spec182Preparation)

BOOST_AUTO_TEST_CASE(InspectionPreservesResolvedSourceAndRejectsForeignModel)
{
  auto adapter = std::make_shared<TaskFixtureAdapter>("fixture", "1", "onnx", "float32",
    "semantics", "graph", identityBytes, identityBytes);
  auto registry = std::make_shared<NativeAdapterRegistry>();
  registry->registerAdapter(adapter); registry->freeze();
  const auto model = modelDescriptor(*adapter);
  auto resolved = inspectedFor(model);
  NativeRequestPreparation preparation(registry,
    [&](const NativePreparedInput&, const NativeModelDescriptor&) { return resolved; });
  const auto input = preparation.prepareInput(model, "task", digest("schema"), digest("schema"),
    {1}, {}, deadline(1000));
  auto result = preparation.inspectModel(input);
  BOOST_CHECK_EQUAL(result.canonicalSourceName, "/catalog/authenticated/model/42");
  BOOST_CHECK_EQUAL(result.canonicalSourceDigest, digest("catalog-source-bytes"));
  BOOST_CHECK_EQUAL(result.modelManifestDigest, digest("manifest"));
  resolved.canonicalGraphDigest = digest("canonical-graph");
  result = preparation.inspectModel(input);
  BOOST_CHECK_EQUAL(result.graph.graphDigest, model.graphDigest);
  BOOST_CHECK_EQUAL(result.canonicalGraphDigest, digest("canonical-graph"));
  for (int mutation = 0; mutation < 6; ++mutation) {
    resolved = inspectedFor(model);
    if (mutation == 0) resolved.descriptor.contentDigest = digest("foreign");
    if (mutation == 1) resolved.descriptor.semanticsDigest = digest("foreign");
    if (mutation == 2) resolved.descriptor.modelName = "foreign";
    if (mutation == 3) resolved.canonicalSourceName = "not-an-ndn-name";
    if (mutation == 4) resolved.modelManifestDigest.clear();
    if (mutation == 5) resolved.canonicalGraphDigest.clear();
    BOOST_CHECK_THROW(preparation.inspectModel(input), std::exception);
  }
}

BOOST_AUTO_TEST_CASE(CertifiedRolesBindManifestRankArtifactAndResourceBudget)
{
  auto adapter = std::make_shared<TaskFixtureAdapter>("fixture", "1", "onnx", "float32",
    "semantics", "graph", identityBytes, identityBytes);
  auto registry = std::make_shared<NativeAdapterRegistry>();
  registry->registerAdapter(adapter); registry->freeze();
  const auto model = inspectedFor(modelDescriptor(*adapter));
  NativeSplitCandidate candidate;
  candidate.model = model.descriptor; candidate.graphDigest = model.graph.graphDigest;
  candidate.splitter = {"fixture", "1", digest("strategy")};
  candidate.candidateDigest = digest("candidate");
  candidate.executionPlan.roles = {"role"};
  for (const auto& node : model.graph.nodes) candidate.nodeRoles[node.id] = "role";
  candidate.fragmentsByRole = {{"role", digest("fragment")}};
  candidate.artifactsByRole = {{"role", {digest("artifact")}}};
  candidate.tensorDegreesByRole = {{"role", 1}};
  candidate.rankArtifactDigestsByRole = candidate.artifactsByRole;
  candidate.requirementsByRole = {{"role", {{"onnxruntime"}, 1024 * 1024, 0, 0, 0, 1.0}}};
  NativePlanSealingInputs fixtureInputs;
  fixtureInputs.artifacts.artifactDigestByRole = {{"role", digest("artifact")}};
  fixtureInputs.artifacts.manifestDigest = model.modelManifestDigest;
  fixtureInputs.artifacts.graphDigest = model.graph.graphDigest;
  fixtureInputs.artifacts.recipeDigest = digest("recipe");
  fixtureInputs.protectionEpoch = "protected";
  fixture::assemblies(fixtureInputs);
  auto role = fixtureInputs.assemblyByRole.at("role");
  role.adapterId = adapter->adapterId(); role.requiredDeviceMemoryMb = 1;
  std::vector<NativeSelectionRoleV3> returned{role};
  bool cancelled = false;
  NativeRequestControl control{"/request", 1, deadline(1000), [&] { return cancelled; }};
  NativeRequestPreparation preparation(registry, {}, {},
    [&](const NativeInspectedModel&, const NativeSplitCandidate&, const NativeRequestControl&) { return returned; });
  BOOST_REQUIRE_EQUAL(preparation.prepareRoles(model, candidate, control).size(), 1);
  for (int mutation = 0; mutation < 8; ++mutation) {
    returned = {role};
    if (mutation == 0) returned[0].modelManifestDigest = digest("foreign");
    if (mutation == 1) returned[0].artifactDigest = digest("foreign");
    if (mutation == 2) returned[0].rank = 1;
    if (mutation == 3) returned[0].adapterId = "foreign";
    if (mutation == 4) returned[0].requiredDeviceMemoryMb = 0;
    if (mutation == 5) returned[0].nodeIndices = {100};
    if (mutation == 6) returned.push_back(role);
    if (mutation == 7) returned.clear();
    BOOST_CHECK_THROW(preparation.prepareRoles(model, candidate, control), std::runtime_error);
  }
  returned = {role}; cancelled = true;
  BOOST_CHECK_THROW(preparation.prepareRoles(model, candidate, control), std::runtime_error);
  cancelled = false;
  NativeRequestPreparation missing(registry);
  BOOST_CHECK_THROW(missing.prepareRoles(model, candidate, control), std::runtime_error);
}

// Existing case (T008-A manifest existingCases[0]): the fixture adapter and
// graph port are bound into one preparation that encodes, inspects and then
// ensures an artifact binding covering the placed plan roles.
BOOST_AUTO_TEST_CASE(NativePreparationBindsAdapterAndGraphPort)
{
  auto adapter = std::make_shared<TaskFixtureAdapter>(
    "fixture", "1", "fixture", "float32", "semantics", "graph",
    identityBytes, identityBytes);
  auto registry = std::make_shared<NativeAdapterRegistry>();
  registry->registerAdapter(adapter);
  registry->freeze();

  std::size_t artifactCalls = 0;
  NativeRequestPreparation preparation(
    registry,
    [] (const NativePreparedInput&, const NativeModelDescriptor& model) {
      return inspectedFor(model);
    },
    [&artifactCalls] (const NativeInspectedModel&, const NativeSplitCandidate&, const std::vector<NativeSelectionRoleV3>&,
                      const NativeRequestControl&) {
      ++artifactCalls;
      return NativeArtifactBinding{{{"role", "/ndnsf/catalog/root/1"}},
                                   {{"role", digest("artifact")}},
                                   digest("manifest"), digest("recipe")};
    });

  const auto input = preparation.prepareInput(
    modelDescriptor(*adapter), "task", digest("schema"), digest("schema"),
    {1, 2, 3}, {}, deadline(1000));
  BOOST_CHECK(input.encoded);
  BOOST_CHECK(input.payload == std::vector<std::uint8_t>({1, 2, 3}));

  const auto inspected = preparation.inspectModel(input);
  BOOST_CHECK_EQUAL(inspected.graph.nodes.size(), 3u);
  BOOST_CHECK_EQUAL(inspected.canonicalSourceName,
                    "/catalog/authenticated/model/42");

  NativeRequestControl control{"/request/1", 1, deadline(1000), {}};
  const auto binding = ensureArtifacts(preparation,
    inspected, proposalFor(control, inspected, {"role"}), control);
  BOOST_CHECK_EQUAL(artifactCalls, 1u);
  BOOST_CHECK_EQUAL(binding.sourceByRole.at("role"), "/ndnsf/catalog/root/1");
  BOOST_CHECK_EQUAL(binding.artifactDigestByRole.at("role"), digest("artifact"));
}

// Qwen pipeline bytes are encoded exactly once and passed through unchanged
// (BytesGenerationTaskAdapter identity semantics); decodeResult is the same
// byte identity for the native final result.
BOOST_AUTO_TEST_CASE(QwenPipelineBytesIdentityMappingThroughPreparation)
{
  auto adapter = std::make_shared<TaskFixtureAdapter>(
    "qwen-three-stage-pipeline", "1", "onnx", "float32", "qwen-semantics",
    "qwen-graph", identityBytes, identityBytes);
  auto registry = std::make_shared<NativeAdapterRegistry>();
  registry->registerAdapter(adapter);
  registry->freeze();

  NativeRequestPreparation preparation(
    registry,
    [] (const NativePreparedInput&, const NativeModelDescriptor& model) {
      return inspectedFor(model);
    },
    {});

  const std::vector<std::uint8_t> pipeline = {0x00, 0x01, 0x7f, 0x80, 0xff};
  const auto input = preparation.prepareInput(
    modelDescriptor(*adapter, "Qwen/Qwen2.5-1.5B", "qwen-semantics", "qwen-graph"),
    "text-generation", digest("schema"), digest("schema"), pipeline, {},
    deadline(1000));
  BOOST_CHECK(input.encoded);
  BOOST_CHECK(input.payload == pipeline);          // one native encoding, identity
  BOOST_CHECK_EQUAL(input.adapterId, "qwen-three-stage-pipeline");

  // decodeResult is exercised at native final-result construction (T010);
  // this pins the frozen adapter mapping now.
  const auto decoded = adapter->decodeResult(pipeline);
  BOOST_CHECK(decoded == pipeline);
}

// The Qwen three-stage plan binds one canonical artifact source per stage
// role; the exact role cover is what the preparation accepts and returns.
BOOST_AUTO_TEST_CASE(QwenStageRolesCoveredByCanonicalBinding)
{
  auto adapter = std::make_shared<TaskFixtureAdapter>(
    "qwen-three-stage-pipeline", "1", "onnx", "float32", "qwen-semantics",
    "qwen-graph", identityBytes, identityBytes);
  auto registry = std::make_shared<NativeAdapterRegistry>();
  registry->registerAdapter(adapter);
  registry->freeze();

  const std::vector<std::string> roles = {
    "/LLM/Pipeline/Stage/0", "/LLM/Pipeline/Stage/1", "/LLM/Pipeline/Stage/2"};
  NativeRequestPreparation preparation(
    registry,
    [] (const NativePreparedInput&, const NativeModelDescriptor& model) {
      return inspectedFor(model);
    },
    [&roles] (const NativeInspectedModel&, const NativeSplitCandidate&, const std::vector<NativeSelectionRoleV3>&,
              const NativeRequestControl&) {
      NativeArtifactBinding binding;
      for (const auto& role : roles) {
        binding.sourceByRole[role] = "/ndnsf/catalog/qwen/root/" +
          role.substr(role.find_last_of('/') + 1);
        binding.artifactDigestByRole[role] = digest("artifact-" + role);
      }
      binding.manifestDigest = digest("manifest");
      binding.recipeDigest = digest("recipe");
      return binding;
    });

  const auto input = preparation.prepareInput(
    modelDescriptor(*adapter, "Qwen/Qwen2.5-1.5B", "qwen-semantics", "qwen-graph"),
    "text-generation", digest("schema"), digest("schema"), {1, 2, 3}, {},
    deadline(1000));
  const auto inspected = preparation.inspectModel(input);
  NativeRequestControl control{"/request/qwen", 1, deadline(1000), {}};
  const auto binding = ensureArtifacts(preparation,
    inspected, proposalFor(control, inspected, roles), control);
  BOOST_CHECK_EQUAL(binding.sourceByRole.size(), roles.size());
  BOOST_CHECK_EQUAL(binding.artifactDigestByRole.at(roles[1]),
                    digest("artifact-/LLM/Pipeline/Stage/1"));
}

// YOLO task documents cross the boundary well-formed and byte-identical in
// both directions; malformed documents are rejected at the adapter boundary
// (mirror of JsonTaskAdapter json.loads on decode, canonical facade on encode).
BOOST_AUTO_TEST_CASE(YoloJsonTaskMappingValidatesAndRoundTrips)
{
  auto adapter = std::make_shared<TaskFixtureAdapter>(
    "yolo26n-task", "1", "onnx", "float32", "yolo-semantics", "yolo-graph",
    jsonValidated, jsonValidated);
  auto registry = std::make_shared<NativeAdapterRegistry>();
  registry->registerAdapter(adapter);
  registry->freeze();

  NativeRequestPreparation preparation(registry, {}, {});

  // Well-formed object; key order intentionally non-sorted to prove that the
  // native boundary does not re-encode already encoded bytes.
  const auto document = bytes(R"({"image":"aGVsbG8=","meta":{"j":2,"k":1}})");
  const auto input = preparation.prepareInput(
    modelDescriptor(*adapter, "yolo26n-onnx", "yolo-semantics", "yolo-graph"),
    "object-detection", digest("schema"), digest("schema"), document, {},
    deadline(1000));
  BOOST_CHECK(input.payload == document);
  BOOST_CHECK(adapter->decodeResult(document) == document);

  // Malformed JSON never becomes a prepared input.
  BOOST_CHECK_THROW(preparation.prepareInput(
    modelDescriptor(*adapter, "yolo26n-onnx", "yolo-semantics", "yolo-graph"),
    "object-detection", digest("schema"), digest("schema"), bytes("{\"image\":"),
    {}, deadline(1000)), std::runtime_error);
  BOOST_CHECK_THROW(adapter->decodeResult(bytes("{\"image\":")),
                    std::runtime_error);
}

// ensureArtifacts is bound to the very request/attempt/model that was
// inspected; a foreign proposal never reaches the catalog port.
BOOST_AUTO_TEST_CASE(PreparationRejectsForeignRequestOrModelProposal)
{
  auto adapter = std::make_shared<TaskFixtureAdapter>(
    "fixture", "1", "fixture", "float32", "semantics", "graph",
    identityBytes, identityBytes);
  auto registry = std::make_shared<NativeAdapterRegistry>();
  registry->registerAdapter(adapter);
  registry->freeze();

  std::size_t artifactCalls = 0;
  NativeRequestPreparation preparation(
    registry,
    [] (const NativePreparedInput&, const NativeModelDescriptor& model) {
      return inspectedFor(model);
    },
    [&artifactCalls] (const NativeInspectedModel&, const NativeSplitCandidate&, const std::vector<NativeSelectionRoleV3>&,
                      const NativeRequestControl&) {
      ++artifactCalls;
      return NativeArtifactBinding{{{"role", "/ndnsf/catalog/root/1"}},
                                   {{"role", digest("artifact")}},
                                   digest("manifest"), digest("recipe")};
    });

  const auto input = preparation.prepareInput(
    modelDescriptor(*adapter), "task", digest("schema"), digest("schema"),
    {1}, {}, deadline(1000));
  const auto inspected = preparation.inspectModel(input);
  NativeRequestControl control{"/request/1", 1, deadline(1000), {}};
  const auto valid = proposalFor(control, inspected, {"role"});

  auto foreign = valid;
  foreign.context.requestId = "/request/other";
  BOOST_CHECK_THROW(ensureArtifacts(preparation, inspected, foreign, control),
                    std::runtime_error);
  foreign = valid;
  foreign.context.attempt = 2;
  BOOST_CHECK_THROW(ensureArtifacts(preparation, inspected, foreign, control),
                    std::runtime_error);
  foreign = valid;
  foreign.context.modelDigest = digest("other-model");
  BOOST_CHECK_THROW(ensureArtifacts(preparation, inspected, foreign, control),
                    std::runtime_error);
  foreign = valid;
  foreign.context.graphDigest = digest("other-graph");
  BOOST_CHECK_THROW(ensureArtifacts(preparation, inspected, foreign, control),
                    std::runtime_error);
  BOOST_CHECK_EQUAL(artifactCalls, 0u);   // rejected before the port was reached
}

// A placement must name the roles it places; an empty or duplicate role list
// cannot drive a certified binding.
BOOST_AUTO_TEST_CASE(PreparationRejectsEmptyOrDuplicatePlanRoles)
{
  auto adapter = std::make_shared<TaskFixtureAdapter>(
    "fixture", "1", "fixture", "float32", "semantics", "graph",
    identityBytes, identityBytes);
  auto registry = std::make_shared<NativeAdapterRegistry>();
  registry->registerAdapter(adapter);
  registry->freeze();

  std::size_t artifactCalls = 0;
  NativeRequestPreparation preparation(
    registry,
    [] (const NativePreparedInput&, const NativeModelDescriptor& model) {
      return inspectedFor(model);
    },
    [&artifactCalls] (const NativeInspectedModel&, const NativeSplitCandidate&, const std::vector<NativeSelectionRoleV3>&,
                      const NativeRequestControl&) {
      ++artifactCalls;
      return NativeArtifactBinding{};
    });

  const auto input = preparation.prepareInput(
    modelDescriptor(*adapter), "task", digest("schema"), digest("schema"),
    {1}, {}, deadline(1000));
  const auto inspected = preparation.inspectModel(input);
  NativeRequestControl control{"/request/1", 1, deadline(1000), {}};

  expectCode([&] {
    ensureArtifacts(preparation,
      inspected, proposalFor(control, inspected, {}), control);
  }, "DI_NATIVE_ARTIFACT_BINDING_MISMATCH");
  expectCode([&] {
    ensureArtifacts(preparation,
      inspected, proposalFor(control, inspected, {"role", "role"}), control);
  }, "DI_NATIVE_ARTIFACT_BINDING_MISMATCH");
  BOOST_CHECK_EQUAL(artifactCalls, 0u);
}

// The returned binding must cover exactly the plan roles: gaps, extra roles
// and foreign role sets are all rejected before the binding can be used.
BOOST_AUTO_TEST_CASE(PreparationRejectsBindingRoleGapExtraForeignRoles)
{
  auto adapter = std::make_shared<TaskFixtureAdapter>(
    "fixture", "1", "fixture", "float32", "semantics", "graph",
    identityBytes, identityBytes);
  auto registry = std::make_shared<NativeAdapterRegistry>();
  registry->registerAdapter(adapter);
  registry->freeze();

  NativeRequestPreparation preparation(
    registry,
    [] (const NativePreparedInput&, const NativeModelDescriptor& model) {
      return inspectedFor(model);
    },
    [] (const NativeInspectedModel&, const NativeSplitCandidate&, const std::vector<NativeSelectionRoleV3>& selectedRoles,
        const NativeRequestControl&) {
      std::vector<std::string> roles;
      for (const auto& role : selectedRoles) roles.push_back(role.selectedRole);
      NativeArtifactBinding binding;
      for (const auto& role : roles) {
        binding.sourceByRole[role] = "/ndnsf/catalog/root/" + role;
        binding.artifactDigestByRole[role] = digest("artifact-" + role);
      }
      // Gapped: one role carries no binding.
      if (roles.size() == 2u) {
        binding.sourceByRole.erase(roles[1]);
        binding.artifactDigestByRole.erase(roles[1]);
      }
      // Extra role beyond the plan.
      if (roles.size() == 1u) {
        binding.sourceByRole["/Foreign/Role"] = "/ndnsf/catalog/root/foreign";
        binding.artifactDigestByRole["/Foreign/Role"] = digest("artifact-foreign");
      }
      // Same-size foreign role set.
      if (roles.size() == 3u) {
        binding.sourceByRole.clear();
        binding.artifactDigestByRole.clear();
        for (const auto& role : std::vector<std::string>{
               "/Foreign/A", "/Foreign/B", "/Foreign/C"}) {
          binding.sourceByRole[role] = "/ndnsf/catalog" + role;
          binding.artifactDigestByRole[role] = digest("artifact-" + role);
        }
      }
      binding.manifestDigest = digest("manifest");
      binding.recipeDigest = digest("recipe");
      return binding;
    });

  const auto input = preparation.prepareInput(
    modelDescriptor(*adapter), "task", digest("schema"), digest("schema"),
    {1}, {}, deadline(1000));
  const auto inspected = preparation.inspectModel(input);

  for (const auto& roles : std::vector<std::vector<std::string>>{
         {"role-a", "role-b"}, {"role-a"}, {"a", "b", "c"}}) {
    NativeRequestControl control{"/request/1", 1, deadline(1000), {}};
    expectCode([&] {
      ensureArtifacts(preparation,
        inspected, proposalFor(control, inspected, roles), control);
    }, "DI_NATIVE_ARTIFACT_BINDING_MISMATCH");
  }
}

// Catalog data names are absolute NDN names; any other source reference is
// rejected as an invalid binding.
BOOST_AUTO_TEST_CASE(PreparationRejectsNonNdnBindingSourceNames)
{
  auto adapter = std::make_shared<TaskFixtureAdapter>(
    "fixture", "1", "fixture", "float32", "semantics", "graph",
    identityBytes, identityBytes);
  auto registry = std::make_shared<NativeAdapterRegistry>();
  registry->registerAdapter(adapter);
  registry->freeze();

  NativeRequestPreparation preparation(
    registry,
    [] (const NativePreparedInput&, const NativeModelDescriptor& model) {
      return inspectedFor(model);
    },
    [] (const NativeInspectedModel&, const NativeSplitCandidate&, const std::vector<NativeSelectionRoleV3>&,
        const NativeRequestControl&) {
      return NativeArtifactBinding{{{"role", "catalog-root-without-slash"}},
                                   {{"role", digest("artifact")}},
                                   digest("manifest"), digest("recipe")};
    });

  const auto input = preparation.prepareInput(
    modelDescriptor(*adapter), "task", digest("schema"), digest("schema"),
    {1}, {}, deadline(1000));
  const auto inspected = preparation.inspectModel(input);
  NativeRequestControl control{"/request/1", 1, deadline(1000), {}};
  const auto proposal = proposalFor(control, inspected, {"role"});

  for (const auto& source : std::vector<std::string>{
         "catalog-root-without-slash", "/a//b", "/trailing/", "/a\tb",
         std::string("/a\0b", 4)}) {
    auto binding = NativeArtifactBinding{{{"role", source}},
                                         {{"role", digest("artifact")}},
                                         digest("manifest"), digest("recipe")};
    NativeRequestPreparation bound(registry, {}, [&binding] (
      const NativeInspectedModel&, const NativeSplitCandidate&, const std::vector<NativeSelectionRoleV3>&,
      const NativeRequestControl&) { return binding; });
    BOOST_CHECK_THROW(ensureArtifacts(bound, inspected, proposal, control),
                      std::invalid_argument);
  }
}

// Canonical digest shape is enforced on every binding reference: role artifact
// digests and the manifest/recipe digests must be authenticated digests.
BOOST_AUTO_TEST_CASE(PreparationRejectsMalformedBindingDigests)
{
  auto adapter = std::make_shared<TaskFixtureAdapter>(
    "fixture", "1", "fixture", "float32", "semantics", "graph",
    identityBytes, identityBytes);
  auto registry = std::make_shared<NativeAdapterRegistry>();
  registry->registerAdapter(adapter);
  registry->freeze();

  NativeRequestPreparation preparation(
    registry,
    [] (const NativePreparedInput&, const NativeModelDescriptor& model) {
      return inspectedFor(model);
    },
    {});
  const auto input = preparation.prepareInput(
    modelDescriptor(*adapter), "task", digest("schema"), digest("schema"),
    {1}, {}, deadline(1000));
  const auto inspected = preparation.inspectModel(input);
  NativeRequestControl control{"/request/1", 1, deadline(1000), {}};
  const auto proposal = proposalFor(control, inspected, {"role"});

  const auto badDigest = std::string("sha256:xyz");
  const auto variants = std::vector<std::function<NativeArtifactBinding()>>{
    [&] { return NativeArtifactBinding{{{"role", "/ndnsf/catalog/root/1"}},
                                       {{"role", badDigest}},
                                       digest("manifest"), digest("recipe")}; },
    [&] { return NativeArtifactBinding{{{"role", "/ndnsf/catalog/root/1"}},
                                       {{"role", digest("artifact")}},
                                       badDigest, digest("recipe")}; },
    [&] { return NativeArtifactBinding{{{"role", "/ndnsf/catalog/root/1"}},
                                       {{"role", digest("artifact")}},
                                       digest("manifest"), "not-a-digest"}; },
    [&] { return NativeArtifactBinding{{{"role", "/ndnsf/catalog/root/1"}},
                                       {}, digest("manifest"), digest("recipe")}; },
  };
  for (const auto& make : variants) {
    NativeRequestPreparation bound(registry, {}, [&make] (
      const NativeInspectedModel&, const NativeSplitCandidate&, const std::vector<NativeSelectionRoleV3>&,
      const NativeRequestControl&) { return make(); });
    BOOST_CHECK_THROW(ensureArtifacts(bound, inspected, proposal, control),
                      std::invalid_argument);
  }
}

// Preparation owns the I/O ports; an unconfigured graph or catalog port is a
// configuration error at the point of use, not a silent no-op.
BOOST_AUTO_TEST_CASE(PreparationFailsClosedWithoutConfiguredPorts)
{
  auto adapter = std::make_shared<TaskFixtureAdapter>(
    "fixture", "1", "fixture", "float32", "semantics", "graph",
    identityBytes, identityBytes);
  auto registry = std::make_shared<NativeAdapterRegistry>();
  registry->registerAdapter(adapter);
  registry->freeze();

  NativeRequestPreparation preparation(registry, {}, {});
  const auto input = preparation.prepareInput(
    modelDescriptor(*adapter), "task", digest("schema"), digest("schema"),
    {1}, {}, deadline(1000));
  expectCode([&] { preparation.inspectModel(input); },
             "DI_NATIVE_MODEL_GRAPH_PORT_NOT_CONFIGURED");

  NativeRequestPreparation graphOnly(
    registry,
    [] (const NativePreparedInput&, const NativeModelDescriptor& model) {
      return inspectedFor(model);
    },
    {});
  const auto inspected = graphOnly.inspectModel(input);
  NativeRequestControl control{"/request/1", 1, deadline(1000), {}};
  expectCode([&] {
    ensureArtifacts(graphOnly, inspected, proposalFor(control, inspected, {"role"}),
                              control);
  }, "DI_NATIVE_ARTIFACT_PORT_NOT_CONFIGURED");
}

// An adapter must answer under the identity it is registered under; a catalog
// or adapter answering for another model identity is rejected, as is a
// request referencing an adapter version that is not registered.
BOOST_AUTO_TEST_CASE(PreparationRejectsAdapterIdentityAndVersionMismatch)
{
  auto mislabeled = std::make_shared<MislabeledIdentityAdapter>(
    "qwen-three-stage-pipeline", "1", "onnx", "float32", "qwen-semantics",
    "qwen-graph", identityBytes, identityBytes);
  auto registry = std::make_shared<NativeAdapterRegistry>();
  registry->registerAdapter(mislabeled);
  registry->freeze();

  // inspect claims a different adapter than the one the request resolved.
  NativeRequestPreparation preparation(registry, {}, {});
  const auto input = preparation.prepareInput(
    modelDescriptor(*mislabeled, "Qwen/Qwen2.5-1.5B", "qwen-semantics",
                    "qwen-graph"),
    "object-detection", digest("schema"), digest("schema"),
    {1}, {}, deadline(1000));
  expectCode([&] { preparation.inspectModel(input); },
             "DI_NATIVE_MODEL_ADAPTER_IDENTITY_MISMATCH");

  // A version that the frozen registry does not hold is refused up front.
  auto unknownVersion = modelDescriptor(*mislabeled, "Qwen/Qwen2.5-1.5B");
  unknownVersion.adapterVersion = "2";
  BOOST_CHECK_THROW(preparation.prepareInput(
    unknownVersion, "text-generation", digest("schema"), digest("schema"),
    {1}, {}, deadline(1000)), std::invalid_argument);
}

// Failure and cancellation release the request's preparation state: the
// control is re-checked before and after the port call, a rejected request
// leaves no binding behind, and a later fresh request succeeds.
BOOST_AUTO_TEST_CASE(PreparationCleanupBoundaryReleasesRequestState)
{
  auto adapter = std::make_shared<TaskFixtureAdapter>(
    "fixture", "1", "fixture", "float32", "semantics", "graph",
    identityBytes, identityBytes);
  auto registry = std::make_shared<NativeAdapterRegistry>();
  registry->registerAdapter(adapter);
  registry->freeze();

  std::size_t artifactCalls = 0;
  bool cancelled = false;
  bool cancelAtPort = false;
  NativeRequestPreparation preparation(
    registry,
    [] (const NativePreparedInput&, const NativeModelDescriptor& model) {
      return inspectedFor(model);
    },
    [&artifactCalls, &cancelled, &cancelAtPort] (const NativeInspectedModel&,
                                                 const NativeSplitCandidate&, const std::vector<NativeSelectionRoleV3>&,
                                                 const NativeRequestControl&) {
      ++artifactCalls;
      if (cancelAtPort) cancelled = true;   // cancel lands while the port runs
      return NativeArtifactBinding{{{"role", "/ndnsf/catalog/root/1"}},
                                   {{"role", digest("artifact")}},
                                   digest("manifest"), digest("recipe")};
    });

  const auto input = preparation.prepareInput(
    modelDescriptor(*adapter), "task", digest("schema"), digest("schema"),
    {1}, {}, deadline(1000));
  const auto inspected = preparation.inspectModel(input);

  // Expired or empty controls never reach the catalog port.
  NativeRequestControl expired{"/request/1", 1, deadline(-1000), {}};
  expectCode([&] {
    ensureArtifacts(preparation,
      inspected, proposalFor(expired, inspected, {"role"}), expired);
  }, "DI_NATIVE_REQUEST_CANCELLED_OR_EXPIRED");
  BOOST_CHECK_EQUAL(artifactCalls, 0u);

  NativeRequestControl cancelledControl{"/request/1", 1, deadline(1000),
    [&cancelled] { return cancelled; }};
  cancelled = true;
  expectCode([&] {
    ensureArtifacts(preparation,
      inspected, proposalFor(cancelledControl, inspected, {"role"}),
      cancelledControl);
  }, "DI_NATIVE_REQUEST_CANCELLED_OR_EXPIRED");
  BOOST_CHECK_EQUAL(artifactCalls, 0u);

  // Cancellation lands between the pre-check and the post-check: the binding
  // the port just produced is discarded before it can be returned.
  cancelled = false;
  cancelAtPort = true;
  expectCode([&] {
    ensureArtifacts(preparation,
      inspected, proposalFor(cancelledControl, inspected, {"role"}),
      cancelledControl);
  }, "DI_NATIVE_REQUEST_CANCELLED_OR_EXPIRED");
  BOOST_CHECK_EQUAL(artifactCalls, 1u);

  // Control identity itself must be complete.
  cancelled = false;
  NativeRequestControl nameless{"", 1, deadline(1000), {}};
  expectCode([&] {
    ensureArtifacts(preparation,
      inspected, proposalFor(nameless, inspected, {"role"}), nameless);
  }, "DI_NATIVE_REQUEST_CANCELLED_OR_EXPIRED");

  // A fresh request on the same preparation succeeds: no lease or partial
  // binding survived the rejected attempts.
  cancelAtPort = false;
  NativeRequestControl fresh{"/request/2", 1, deadline(1000), {}};
  const auto binding = ensureArtifacts(preparation,
    inspected, proposalFor(fresh, inspected, {"role"}), fresh);
  BOOST_CHECK_EQUAL(binding.sourceByRole.at("role"), "/ndnsf/catalog/root/1");
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di
