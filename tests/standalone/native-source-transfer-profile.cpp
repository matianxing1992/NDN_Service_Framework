#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <variant>

using namespace ndnsf::di;

int
main(int argc, char** argv)
{
  try {
    const bool metadataOnly = argc == 3 && std::string(argv[2]) == "--metadata-only";
    if (argc != 2 && !metadataOnly)
      throw std::invalid_argument(
        "usage: native-source-transfer-profile deployment.json [--metadata-only]");
    std::ifstream configuration(argv[1]);
    if (!configuration)
      throw std::runtime_error("cannot read deployment");
    const auto deployment = NativeJson::parse(configuration);
    const auto& catalogJson = deployment.at("requester").at("catalog");
    const auto& sourceJson = catalogJson.at("source");
    NativeAssemblyControl control{std::chrono::steady_clock::now() + std::chrono::seconds(120),
      [] {}, catalogJson.at("recipe").at("max_source_bytes").get<std::uint64_t>(),
      catalogJson.at("recipe").at("max_assembled_bytes").get<std::uint64_t>()};
    const auto graphPath = sourceJson.at("file").get<std::string>();
    const auto graphBytes = sourceJson.at("bytes").get<std::uint64_t>();
    if (graphBytes == 0 || graphBytes > control.maxSourceBytes ||
        std::filesystem::file_size(graphPath) != graphBytes)
      throw std::runtime_error("source graph exceeds its declared bound");
    std::ifstream graph(graphPath, std::ios::binary);
    if (!graph)
      throw std::runtime_error("cannot read pinned source graph");
    NativeCanonicalSource source;
    source.modelBytes.assign(std::istreambuf_iterator<char>(graph), {});
    if (source.modelBytes.size() != sourceJson.at("bytes").get<std::size_t>())
      throw std::runtime_error("source graph size mismatch");
    source.initializerRangeSource = openNativeCanonicalInitializerFile(
      sourceJson.at("initializer_file").get<std::string>(),
      sourceJson.at("initializer_bytes").get<std::uint64_t>(),
      sourceJson.at("initializer_digest").get<std::string>(), control);
    const auto catalog = NativeRequestCatalog::load(nativeCanonicalJson(catalogJson),
                                                   std::move(source), control);
    const auto preparedSource = catalog.preparation->sourceRefFor(catalog.model.descriptor);
    if (!preparedSource)
      throw std::runtime_error("verified preparation source is unavailable");
    if (metadataOnly) {
      std::cout << NativeJson{{"sourceDigest", catalog.model.canonicalSourceDigest},
        {"preparedMetadataBytes", preparedSource->preparedMetadataJson.size()},
        {"graphNodes", catalog.model.graph.nodes.size()}}.dump() << '\n';
      return 0;
    }
    const auto& profile = catalog.planningCache->requestTransferProfiles.at(0);
    NativeJson result = NativeJson::array();
    for (const auto& prepared : catalog.planningCache->candidates) {
      auto roles = NativeJson::array();
      for (const auto& role : prepared.roles) {
        const auto contracts = [] (const auto& tensors) {
          auto values = NativeJson::array();
          for (const auto& tensor : tensors) {
            auto shape = NativeJson::array();
            for (const auto& dimension : tensor.shape)
              std::visit([&](const auto& value) { shape.push_back(value); }, dimension);
            values.push_back({{"name", tensor.name}, {"dtype", tensor.dtype}, {"shape", shape}});
          }
          return values;
        };
        roles.push_back({{"role", role.selectedRole}, {"recipeDigest", role.recipeDigest},
                         {"modelManifestDigest", role.modelManifestDigest},
                         {"inputs", contracts(role.expectedInputs)},
                         {"outputs", contracts(role.expectedOutputs)}});
      }
      auto edges = NativeJson::array();
      for (const auto& edge : prepared.candidate.executionPlan.dependencies) {
        const auto estimate = estimateNativeRequestTransferEncodedBytesV1(profile,
          catalog.model.canonicalSourceDigest, profile.at("input_digest").get<std::string>(),
          edge, prepared.roles);
        edges.push_back({{"operation", edge.operationKind}, {"producers", edge.producers},
          {"consumers", edge.consumers}, {"tensors", edge.tensors},
          {"redistributions", edge.redistributions.size()},
          {"estimatedBytes", estimate ? NativeJson(*estimate) : NativeJson(nullptr)}});
      }
      result.push_back({{"roles", roles}, {"dependencies", edges}});
    }
    std::cout << NativeJson{{"sourceDigest", catalog.model.canonicalSourceDigest},
      {"preparedMetadataBytes", preparedSource->preparedMetadataJson.size()},
      {"profileSourceDigest", profile.at("source_digest")},
      {"profileInputDigest", profile.at("input_digest")}, {"candidates", result}}.dump(2) << '\n';
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
