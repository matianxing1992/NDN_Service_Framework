// Print the canonical identity and planning graph of one ONNX source file.
//
// This is fixture preparation tooling: it lets a tracked test driver bind the
// exact digests and node ids the production planner will see, instead of
// duplicating the hash algorithm in the driver.

#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

int
main(int argc, char** argv)
{
  const bool catalogMode = argc == 4 && std::string(argv[1]) == "--catalog";
  if (argc != 2 && !catalogMode) {
    std::cerr << "usage: native-source-inspect model.onnx | "
                 "--catalog catalog.json model.onnx" << std::endl;
    return 2;
  }
  try {
    const char* modelPath = catalogMode ? argv[3] : argv[1];
    std::ifstream input(modelPath, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open the model file");
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
    ndnsf::di::NativeCanonicalSource source;
    source.modelBytes = bytes;
    if (catalogMode) {
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
