// File-backed integration port for the real native assembler. No network or
// substitute assembler is used; the production helper is executed normally.
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using boost::property_tree::ptree;
using namespace ndnsf::di;

ndn::Buffer decodeHex(const std::string& hex)
{
  if (hex.size() % 2 != 0) throw std::runtime_error("invalid fixture hex");
  ndn::Buffer result;
  for (std::size_t i = 0; i < hex.size(); i += 2)
    result.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  return result;
}

std::vector<NativeAssemblyTensorContractV3> contracts(const ptree& values)
{
  std::vector<NativeAssemblyTensorContractV3> result;
  for (const auto& child : values) {
    NativeAssemblyTensorContractV3 item;
    item.name = child.second.get<std::string>("name");
    item.dtype = child.second.get<std::string>("dtype");
    for (const auto& dimension : child.second.get_child("shape"))
      item.shape.push_back(dimension.second.get_value<std::string>());
    result.push_back(std::move(item));
  }
  return result;
}

int main(int argc, char** argv)
{
  ptree result;
  try {
    if (argc != 4) throw std::runtime_error("usage: driver case.json cache python");
    ptree row;
    boost::property_tree::read_json(argv[1], row);
    const auto& input = row.get_child("role");
    NativeSelectionProjectionV3 projection;
    projection.provider = "/spec181/provider";
    projection.requestId = "/spec181/assembly/request";
    projection.canonicalArtifactName = "/spec181/assembly/root";
    projection.plan.serviceName = "/spec181/assembly/service";
    auto& role = projection.assembly;
#define COPY_STRING(native, field) role.native = input.get<std::string>(field)
    COPY_STRING(role, "role");
    COPY_STRING(adapterId, "adapter_id");
    COPY_STRING(adapterVersion, "adapter_version");
    COPY_STRING(artifactDigest, "artifact_digest");
    COPY_STRING(backend, "backend");
    COPY_STRING(backendAbi, "backend_abi");
    COPY_STRING(roleKind, "role_kind");
    COPY_STRING(modelManifestDigest, "model_manifest_digest");
    COPY_STRING(artifactProfileDigest, "artifact_profile_digest");
    COPY_STRING(graphDigest, "graph_digest");
    COPY_STRING(canonicalInitializerDigest, "canonical_initializer_digest");
    COPY_STRING(adapterDescriptorDigest, "adapter_descriptor_digest");
    COPY_STRING(assemblerDescriptorDigest, "assembler_descriptor_digest");
    COPY_STRING(recipeDigest, "recipe_digest");
    COPY_STRING(precision, "precision");
    COPY_STRING(quantization, "quantization");
    COPY_STRING(layout, "layout");
    COPY_STRING(padding, "padding");
    COPY_STRING(protectionEpoch, "protection_epoch");
#undef COPY_STRING
    role.selectedRole = role.role;
    role.rank = input.get<std::uint32_t>("rank");
    role.layerBegin = input.get<std::uint32_t>("layer_begin");
    role.layerEnd = input.get<std::uint32_t>("layer_end");
    role.maxSourceBytes = input.get<std::uint64_t>("resource_envelope.maxSourceBytes");
    role.maxAssembledBytes = input.get<std::uint64_t>("resource_envelope.maxAssembledBytes");
    role.maxNodes = input.get<std::uint64_t>("resource_envelope.maxNodes");
    for (const auto& value : input.get_child("node_indices"))
      role.nodeIndices.push_back(value.second.get_value<std::uint64_t>());
    for (const auto& value : input.get_child("device_set"))
      role.deviceSet.push_back(value.second.get_value<std::string>());
    role.expectedInputs = contracts(input.get_child("expected_inputs"));
    role.expectedOutputs = contracts(input.get_child("expected_outputs"));
    const auto root = row.get<std::string>("rootManifest");
    NativeCanonicalOnnxFetchers fetchers;
    fetchers.getArtifact = [&] (const ndn::Name& name) -> std::optional<ndn::Buffer> {
      if (name.toUri() != projection.canonicalArtifactName)
        throw std::runtime_error("unexpected root fixture fetch");
      return ndn::Buffer(root.begin(), root.end());
    };
    fetchers.fetchEncryptedLargeData = [&] (const ndn::Name& name, const ndn::Name& service)
      -> std::optional<ndn::Buffer> {
      if (service.toUri() != projection.plan.serviceName)
        throw std::runtime_error("unexpected service fixture fetch");
      if (name.toUri() == "/spec181/assembly/source")
        return decodeHex(row.get<std::string>("canonicalModelHex"));
      if (name.toUri() == "/spec181/assembly/initializers")
        return decodeHex(row.get<std::string>("initializerHex"));
      throw std::runtime_error("unexpected canonical fixture fetch");
    };
    NativeCanonicalOnnxAssemblerOptions options;
    options.cacheDir = argv[2];
    options.pythonExecutable = argv[3];
    options.providerIdentity = projection.provider;
    options.signManifest = [] (const std::string&) { return "fixture-signature"; };
    const auto prepared = prepareNativeCanonicalOnnxRole(fetchers, projection, options);
    result.put("status", "ASSEMBLED");
    result.put("modelPath", prepared.path);
    result.put("modelDigest", prepared.metadata.at("assembledModelDigest"));
    boost::property_tree::write_json(std::cout, result, false);
    return 0;
  }
  catch (const std::exception& error) {
    result.put("status", "REJECTED");
    result.put("reason", error.what());
    boost::property_tree::write_json(std::cout, result, false);
    return 2;
  }
}
