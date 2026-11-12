#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-model.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-generation.hpp"

#include <chrono>
#include <ctime>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

int
main(int argc, char** argv)
{
  const bool attention = argc == 3 && std::string(argv[1]) == "--attention";
  const bool inspect = attention || (argc == 3 && std::string(argv[1]) == "--inspect");
  if (argc != 2 && !inspect) return 2;
  const auto start = std::chrono::steady_clock::now();
  const auto cpu = std::clock();
  try {
    ndnsf::di::NativeAssemblyControl control{
      start + std::chrono::minutes(5), [] {}, 64ULL << 30, 64ULL << 30};
    {
      ndnsf::di::OpenedAutomaticOnnxSource opened;
      auto facts = ndnsf::di::NativeJson::object();
      if (inspect) {
        auto model = ndnsf::di::inspectAutomaticOnnxModel({argv[2]}, control);
        const auto contractJson = model.contract.canonicalJson(control);
        const auto restored = ndnsf::di::AutomaticOnnxContract::fromCanonicalJson(
          contractJson, control);
        const auto contractDigest = ndnsf::di::nativePlanningDigest(contractJson);
        if (restored.digest(control) != contractDigest)
          throw std::runtime_error("automatic contract did not round trip");
        facts = ndnsf::di::NativeJson{{"factsDigest", model.contract.factsDigest},
          {"contractDigest", contractDigest}, {"contractBytes", contractJson.size()},
          {"shapeRelations", model.contract.relations.size()},
          {"descriptor", ndnsf::di::nativeParseJson(model.descriptor.canonicalJson())},
          {"operators", model.contract.operators.size()}, {"quantizedOperators", model.contract.quantization.size()},
          {"canonicalGraphDigest", model.inspection.canonicalIdentity.graphDigest},
          {"normalizedInitializerDigest", model.inspection.canonicalIdentity.initializerDigest}};
        facts["tensorCount"] = model.contract.tensors.size();
        facts["integerLiterals"] = model.contract.integerLiterals;
        facts["generationSourceProof"] = ndnsf::di::automaticOnnxGenerationJson(
          model.contract.generation);
        facts["generationDiagnostic"] = model.generationDiagnostic;
        if (attention) {
          facts["attentionStates"] = ndnsf::di::NativeJson::array();
          for (const auto& state : ndnsf::di::deriveAutomaticOnnxAttentionStates(
                 model.contract, control)) {
            facts["attentionStates"].push_back({
              {"role", state.role == ndnsf::di::AutomaticAttentionState::Role::KEY ? "key" : "value"},
              {"operatorIndex", state.operatorIndex}, {"input", state.input},
              {"output", state.output}, {"query", state.query}, {"key", state.key},
              {"value", state.value}, {"sequenceLengths", state.sequenceLengths},
              {"totalLength", state.totalLength}, {"inputAxes", state.inputAxes},
              {"outputAxes", state.outputAxes}, {"heads", state.heads},
              {"headSize", state.headSize},
              {"capacity", "max(past_capacity,total_length)"}});
          }
          facts["generationQualified"] = false;
        }
        facts["boundaryTensors"] = ndnsf::di::NativeJson::array();
        std::set<std::string> boundary;
        for (const auto& input : model.inspection.graph.modelInputs)
          boundary.insert("graph/" + input.name);
        for (const auto& output : model.inspection.graph.modelOutputs)
          boundary.insert("graph/" + output.name);
        for (const auto& tensor : model.contract.tensors) {
          if (boundary.count(tensor.name) == 0) continue;
          ndnsf::di::NativeJson dimensions = nullptr;
          if (tensor.dimensions) {
            dimensions = ndnsf::di::NativeJson::array();
            for (const auto& axis : *tensor.dimensions) {
              if (axis.kind == ndnsf::di::DimensionConstraint::Kind::FIXED)
                dimensions.push_back(axis.fixed);
              else if (axis.kind == ndnsf::di::DimensionConstraint::Kind::SYMBOL)
                dimensions.push_back(axis.symbol);
              else dimensions.push_back(nullptr);
            }
          }
          facts["boundaryTensors"].push_back({{"name", tensor.name},
            {"onnxElementType", tensor.onnxElementType}, {"dimensions", std::move(dimensions)},
            {"wireTransportable", tensor.wireElementType.has_value()},
            {"provenance", tensor.provenance}});
        }
        opened = std::move(model.opened);
      }
      else {
        opened = ndnsf::di::openAutomaticOnnxSource(argv[1], control);
      }
      auto files = ndnsf::di::NativeJson::array();
      for (const auto& file : opened.externalFiles) {
        files.push_back(ndnsf::di::NativeJson{
          {"location", file.location}, {"bytes", file.bytes}, {"digest", file.digest}});
      }
      std::cout << (inspect ? "ONNX_SOURCE_FACTS_PASS " : "ONNX_SOURCE_OPEN_PASS ")
        << ndnsf::di::nativeCanonicalJson(
        ndnsf::di::NativeJson{{"scope", inspect ? "descriptor/source facts only" :
                                               "source opening/identity only"},
                             {"sourceDigest", opened.sourceDigest},
                             {"modelDigest", opened.modelDigest},
                             {"externalFiles", std::move(files)}, {"facts", std::move(facts)}})
        << std::endl;
    }
    std::cout << "ONNX_SOURCE_OPEN_RESOURCES wallSeconds="
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
              << " cpuSeconds=" << static_cast<double>(std::clock() - cpu) / CLOCKS_PER_SEC
              << std::endl;
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "ONNX_SOURCE_OPEN_FAILED: " << error.what() << std::endl;
    return 1;
  }
}
