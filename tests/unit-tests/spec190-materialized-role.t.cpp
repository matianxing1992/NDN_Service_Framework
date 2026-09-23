// Spec190 T003 regression: a role graph rebuilt from authenticated material
// is reused by the worker assembly chain without a second shape-inference or
// reachability-extraction protobuf graph.  The final checker, contract check,
// deterministic wire and real ORT load remain in the production chain.

#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndnsf::di {
namespace {

boost::property_tree::ptree
loadVector()
{
  const std::string paths[] = {
    "tests/fixtures/spec182/dependency-probes/extraction-vectors.json",
    "../tests/fixtures/spec182/dependency-probes/extraction-vectors.json",
  };
  for (const auto& path : paths) {
    std::ifstream input(path);
    if (!input)
      continue;
    boost::property_tree::ptree root;
    boost::property_tree::read_json(input, root);
    for (const auto& item : root.get_child("cases")) {
      if (item.second.get<std::string>("expectedModelDigest", "") ==
          "sha256:59269bc795b453dcc15fcb34dd6c9b1625070bc094f32361fcd236c3b6600383")
        return item.second;
    }
  }
  throw std::runtime_error("Spec190 materialized-role vector is unavailable");
}

std::vector<std::uint8_t>
fromHex(const std::string& value)
{
  if (value.size() % 2 != 0)
    throw std::runtime_error("invalid vector hex");
  std::vector<std::uint8_t> result;
  result.reserve(value.size() / 2);
  for (std::size_t i = 0; i < value.size(); i += 2)
    result.push_back(static_cast<std::uint8_t>(std::stoul(value.substr(i, 2), nullptr, 16)));
  return result;
}

NativeCertifiedRecipe
recipeFromVector(const boost::property_tree::ptree& row)
{
  const auto& json = row.get_child("recipe");
  NativeCertifiedRecipe recipe;
  recipe.adapterId = "onnx";
  recipe.adapterVersion = "1";
  recipe.backend = "onnxruntime";
  recipe.role = "/role/0";
  recipe.selectedRole = recipe.role;
  recipe.backendAbi = json.get<std::string>("backendAbi");
  recipe.roleKind = json.get<std::string>("roleKind");
  recipe.modelManifestDigest = json.get<std::string>("modelManifestDigest");
  recipe.artifactProfileDigest = json.get<std::string>("artifactProfileDigest");
  recipe.graphDigest = json.get<std::string>("graphDigest");
  recipe.canonicalInitializerDigest = json.get<std::string>("canonicalInitializerDigest");
  recipe.adapterDescriptorDigest = json.get<std::string>("adapterDescriptorDigest");
  recipe.assemblerDescriptorDigest = json.get<std::string>("assemblerDescriptorDigest");
  recipe.layerBegin = json.get<std::uint64_t>("layerBegin");
  recipe.layerEnd = json.get<std::uint64_t>("layerEnd");
  for (const auto& item : json.get_child("nodeIndices"))
    recipe.nodeIndices.push_back(std::stoull(item.second.get_value<std::string>()));
  const auto readContracts = [] (const boost::property_tree::ptree& values) {
    std::vector<NativeAssemblyTensorContractV3> result;
    for (const auto& item : values) {
      NativeAssemblyTensorContractV3 contract;
      contract.name = item.second.get<std::string>("name");
      contract.dtype = item.second.get<std::string>("dtype");
      for (const auto& dimension : item.second.get_child("shape"))
        contract.shape.emplace_back(dimension.second.get_value<std::string>());
      result.push_back(std::move(contract));
    }
    return result;
  };
  recipe.expectedInputs = readContracts(json.get_child("expectedInputs"));
  recipe.expectedOutputs = readContracts(json.get_child("expectedOutputs"));
  recipe.precision = json.get<std::string>("precision");
  recipe.quantization = json.get<std::string>("quantization");
  recipe.layout = json.get<std::string>("layout");
  recipe.padding = json.get<std::string>("padding");
  recipe.maxSourceBytes = json.get<std::uint64_t>("maxSourceBytes");
  recipe.maxAssembledBytes = json.get<std::uint64_t>("maxAssembledBytes");
  recipe.maxNodes = json.get<std::uint64_t>("maxNodes");
  return recipe;
}

NativeAssemblyControl
control()
{
  return NativeAssemblyControl{
    std::chrono::steady_clock::now() + std::chrono::seconds(30), [] {},
    8 * 1024 * 1024, 8 * 1024 * 1024};
}

} // namespace

BOOST_AUTO_TEST_CASE(MaterializedRoleReusesCertifiedGraph)
{
  const auto row = loadVector();
  const NativeCanonicalSource source{fromHex(row.get<std::string>("modelHex")), std::nullopt};
  const auto normalRecipe = recipeFromVector(row);
  const auto normal = assembleNativeCertifiedOnnxModel(source, normalRecipe, control());
  BOOST_CHECK_EQUAL(normal.modelDigest, row.get<std::string>("expectedModelDigest"));

  auto materializedRecipe = normalRecipe;
  materializedRecipe.materializedRole = true;
  const auto materialized = assembleNativeCertifiedOnnxModel(
    source, materializedRecipe, control());
  const auto repeated = assembleNativeCertifiedOnnxModel(
    source, materializedRecipe, control());

  // The materialized path is intentionally the already-authenticated role
  // graph, not the canonical-source extraction wire.  Its exact bytes must
  // remain stable and match the role source that production hands to OA02.
  BOOST_CHECK(materialized.modelBytes == source.modelBytes);
  BOOST_CHECK(materialized.modelBytes == repeated.modelBytes);
  BOOST_CHECK_EQUAL(materialized.modelDigest, repeated.modelDigest);
  BOOST_CHECK_EQUAL(materialized.nodeCount, normal.nodeCount);
  BOOST_CHECK(materialized.inputNames == normal.inputNames);
  BOOST_CHECK(materialized.outputNames == normal.outputNames);
}

} // namespace ndnsf::di
