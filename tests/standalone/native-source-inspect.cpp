// Print the canonical identity and planning graph of one ONNX source file.
//
// This is fixture preparation tooling: it lets a tracked test driver bind the
// exact digests and node ids the production planner will see, instead of
// duplicating the hash algorithm in the driver.

#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

int
main(int argc, char** argv)
{
  const bool catalogMode = (argc == 4 || argc == 5) && std::string(argv[1]) == "--catalog";
  const bool initializerMode = argc == 4 && std::string(argv[1]) == "--initializer";
  const bool planningMode = (argc >= 4 && argc <= 6) && std::string(argv[1]) == "--planning";
  const bool preflightMode = argc == 6 && std::string(argv[1]) == "--preflight";
  const bool convMode = (argc == 4 || argc == 5) && std::string(argv[1]) == "--conv";
  if (argc != 2 && !catalogMode && !initializerMode && !planningMode && !convMode &&
      !preflightMode) {
    std::cerr << "usage: native-source-inspect model.onnx | "
                 "--catalog catalog.json model.onnx | "
                 "--planning descriptor.json model.onnx [sidecar [sha256:...]] | "
                 "--preflight catalog.json model.onnx sidecar sha256:..." << std::endl;
    return 2;
  }
  try {
    const char* modelPath =
      (catalogMode || initializerMode || planningMode || convMode || preflightMode) ? argv[3]
                                                                                   : argv[1];
    std::ifstream input(modelPath, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open the model file");
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
    ndnsf::di::NativeCanonicalSource source;
    source.modelBytes = bytes;
    const auto rangeControl = ndnsf::di::NativeAssemblyControl{
      std::chrono::steady_clock::now() + std::chrono::seconds(180), [] {}, 2ULL << 30, 2ULL << 30};
    if (argc == 6) {
      source.initializerRangeSource = ndnsf::di::openNativeCanonicalInitializerFile(
        argv[4], std::filesystem::file_size(argv[4]), argv[5], rangeControl);
    }
    if (preflightMode) {
      using namespace ndnsf::di;
      std::ifstream catalogStream(argv[2]);
      if (!catalogStream) throw std::runtime_error("cannot open preflight catalog");
      const std::string wire{std::istreambuf_iterator<char>(catalogStream), {}};
      const auto catalog = NativeRequestCatalog::load(wire, std::move(source), rangeControl);
      const auto candidates = catalog.splitter->enumerate(catalog.model.descriptor,
                                                          catalog.model.graph, {});
      if (candidates.size() != 1)
        throw std::invalid_argument("source preflight requires one exact catalog candidate");
      const auto& candidate = candidates.front();
      const NativeRequestControl request{"/fixture/preflight", 1, rangeControl.deadline, {}};
      const auto roles = catalog.preparation->prepareRoles(catalog.model, candidate, request);
      auto facts = NativeJson::array();
      for (const auto& role : roles) {
        auto& record = facts.emplace_back(NativeJson{
          {"role", role.role}, {"nodes", role.nodeIndices.size()},
          {"memory_mib", role.requiredDeviceMemoryMb}, {"entries", 0}});
        if (!role.sourceTransformV1.empty()) {
          const auto recipe = nativeParseJson(role.sourceTransformV1);
          if (recipe.contains("entries")) record["entries"] = recipe.at("entries").size();
        }
      }
      const auto peers = nativeDistinctProviderPairs(candidate);
      std::cout << nativeCanonicalJson(NativeJson{
        {"roles", std::move(facts)}, {"source_nodes", catalog.model.graph.nodes.size()},
        {"steps", candidate.executionPlan.steps.size()},
        {"dependencies", candidate.executionPlan.dependencies.size()}, {"peer_roles", peers}})
        << std::endl;
      return 0;
    }
    if (convMode) {
      if (argc == 5) {
        std::ifstream weights(argv[4], std::ios::binary);
        if (!weights) throw std::runtime_error("cannot open the initializer file");
        source.initializerBytes = ndnsf::di::NativeCanonicalByteBuffer{
          std::vector<std::uint8_t>{std::istreambuf_iterator<char>(weights), {}}};
      }
      // Reuse the protected-source parser so facts come from the same bytes the
      // planner will inspect, including external initializers.
      const auto control = ndnsf::di::NativeAssemblyControl{
        std::chrono::steady_clock::now() + std::chrono::seconds(60), [] {}, 1 << 28, 1 << 28};
      const auto index = static_cast<std::size_t>(std::stoul(argv[2]));
      const auto facts = ndnsf::di::inspectOnnxNodeFacts(source, index, control);
      std::cout << facts << std::endl;
      return 0;
    }
    if (planningMode) {
      if (argc == 5) {
        std::ifstream weights(argv[4], std::ios::binary);
        if (!weights) throw std::runtime_error("cannot open the initializer file");
        source.initializerBytes = ndnsf::di::NativeCanonicalByteBuffer{
          std::vector<std::uint8_t>{std::istreambuf_iterator<char>(weights), {}}};
      }
      std::ifstream descriptorStream(argv[2]);
      if (!descriptorStream) throw std::runtime_error("cannot open the descriptor");
      const auto descriptorJson = ndnsf::di::nativeParseJson(
        std::string{std::istreambuf_iterator<char>(descriptorStream), {}});
      const auto declared = ndnsf::di::NativeModelDescriptor::fromCanonicalJson(
        ndnsf::di::nativeCanonicalJson(descriptorJson.at("model")));
      const auto control = argc == 6 ? rangeControl : ndnsf::di::NativeAssemblyControl{
        std::chrono::steady_clock::now() + std::chrono::seconds(60), [] {}, 1 << 28, 1 << 28};
      const auto graph = ndnsf::di::inspectNativeOnnxSourceGraph(source, declared, control);
      const auto identity = ndnsf::di::canonicalOnnxSourceIdentity(source, control);
      std::cout << ndnsf::di::nativeCanonicalJson(ndnsf::di::NativeJson{
        {"declared_graph_digest", declared.graphDigest},
        {"planning_graph_digest", graph.graph.graphDigest},
        {"canonical_graph_digest", identity.graphDigest},
        {"initializer_digest", identity.initializerDigest},
        {"initializer_object_digest",
          (source.initializerBytes || source.initializerRangeSource)
            ? ndnsf::di::nativeCanonicalInitializerDigest(source, control)
            : std::string{}}}) << std::endl;
      return 0;
    }
    if (initializerMode) {
      std::ifstream weights(argv[2], std::ios::binary);
      if (!weights) throw std::runtime_error("cannot open the initializer file");
      const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(weights), {}};
      source.initializerBytes = ndnsf::di::NativeCanonicalByteBuffer{bytes};
      const auto control = ndnsf::di::NativeAssemblyControl{
        std::chrono::steady_clock::now() + std::chrono::seconds(30), [] {}, 1 << 26, 1 << 26};
      const auto identity = ndnsf::di::canonicalOnnxSourceIdentity(source, control);
      auto result = ndnsf::di::NativeJson{
        {"source_bytes", source.modelBytes.size()},
        {"initializer_bytes", ndnsf::di::nativeCanonicalInitializerByteSize(source)},
        {"initializer_object_digest", ndnsf::di::nativeCanonicalInitializerDigest(source, control)},
        {"initializer_digest", identity.initializerDigest},
        {"graph_digest", identity.graphDigest}};
      std::cout << ndnsf::di::nativeCanonicalJson(result) << std::endl;
      return 0;
    }
    if (catalogMode) {
      if (argc == 5) {
        std::ifstream weights(argv[4], std::ios::binary);
        if (!weights) throw std::runtime_error("cannot open the initializer file");
        source.initializerBytes = ndnsf::di::NativeCanonicalByteBuffer{
          std::vector<std::uint8_t>{std::istreambuf_iterator<char>(weights), {}}};
      }
      std::ifstream catalogStream(argv[2]);
      if (!catalogStream) throw std::runtime_error("cannot open the catalog");
      const std::string catalogJson{std::istreambuf_iterator<char>(catalogStream), {}};
      const auto control = ndnsf::di::NativeAssemblyControl{
        std::chrono::steady_clock::now() + std::chrono::seconds(30), [] {}, 1 << 26, 1 << 26};
      const auto catalogValue = ndnsf::di::nativeParseJson(catalogJson);
      const auto declared = ndnsf::di::NativeModelDescriptor::fromCanonicalJson(
        ndnsf::di::nativeCanonicalJson(catalogValue.at("model")));
      std::cerr << "declared graph_digest=" << declared.graphDigest << std::endl;
      const auto sourceInspection = ndnsf::di::inspectNativeOnnxSourceGraph(source, declared, control);
      const auto identity = ndnsf::di::canonicalOnnxSourceIdentity(source, control);
      std::cout << ndnsf::di::nativeCanonicalJson(ndnsf::di::NativeJson{
        {"declared_graph_digest", declared.graphDigest},
        {"planning_graph_digest", sourceInspection.graph.graphDigest},
        {"canonical_graph_digest", identity.graphDigest},
        {"initializer_digest", identity.initializerDigest}}) << std::endl;
      return 0;
    }
    const auto control = ndnsf::di::NativeAssemblyControl{
      std::chrono::steady_clock::now() + std::chrono::seconds(30), [] {}, 1 << 26, 1 << 26};
    const std::string zero = "sha256:" + std::string(64, '0');
    ndnsf::di::NativeModelDescriptor model;
    model.modelName = "inspect";
    model.contentDigest = zero;
    model.semanticsDigest = zero;
    model.graphDigest = zero;
    model.modelFormat = "onnx";
    model.precision = "float32";
    model.adapterId = "fixture";
    model.adapterVersion = "1";
    model.adapter.name = "fixture";
    model.adapter.version = "1";
    model.adapter.stateDigest = zero;
    model.adapter.abi = "fixture-abi-v1";
    model.adapter.modelFormats = {"onnx"};
    model.adapter.tasks = {"task"};
    model.adapter.backends = {"onnxruntime"};
    model.adapter.precisions = {"float32"};
    model.adapter.inputSchemaDigest = zero;
    model.adapter.optionsSchemaDigest = zero;
    model.adapter.resultSchemaDigest = zero;
    model.adapter.graphSchemaDigest = zero;
    model.adapter.splitSchemaDigest = zero;
    model.adapter.stateSchemaDigest = zero;
    model.adapter.graphInspectable = true;
    model.adapter.splittable = true;
    model.adapter.deterministicAnalysis = true;
    const auto identity = ndnsf::di::canonicalOnnxSourceIdentity(source, control);
    const auto graph = ndnsf::di::inspectNativeOnnxSourceGraph(source, model, control);
    model.graphDigest = graph.graph.graphDigest;
    try {
      (void)ndnsf::di::inspectNativeOnnxPlanningGraph(source, model, control);
    }
    catch (const std::exception& error) {
      std::cerr << "planning graph rejected: " << error.what() << std::endl;
      return 3;
    }
    auto nodes = ndnsf::di::NativeJson::array();
    for (const auto& node : graph.graph.nodes)
      nodes.push_back({{"id", node.id}, {"op", node.opType}});
    auto edges = ndnsf::di::NativeJson::array();
    for (const auto& edge : graph.graph.edges)
      edges.push_back({{"id", edge.id}, {"producer", edge.producer}});
    auto inputs = ndnsf::di::NativeJson::array();
    for (const auto& value : graph.graph.modelInputs)
      inputs.push_back(value.name);
    auto outputs = ndnsf::di::NativeJson::array();
    for (const auto& value : graph.graph.modelOutputs)
      outputs.push_back(value.name);
    auto result = ndnsf::di::NativeJson{{"graph_digest", graph.graph.graphDigest},
      {"canonical_graph_digest", identity.graphDigest},
      {"initializer_digest", identity.initializerDigest},
      {"nodes", nodes}, {"edges", edges},
      {"model_inputs", inputs}, {"model_outputs", outputs}};
    std::cout << ndnsf::di::nativeCanonicalJson(result) << std::endl;
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "native-source-inspect: " << error.what() << std::endl;
    return 1;
  }
}
