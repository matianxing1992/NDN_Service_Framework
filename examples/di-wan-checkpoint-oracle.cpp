#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using namespace ndnsf::di;
namespace fs = std::filesystem;

std::vector<std::uint8_t>
readFile(const fs::path& path, std::uint64_t size)
{
  if (size == 0 || size > (512U << 20) || fs::file_size(path) != size)
    throw std::runtime_error("checkpoint oracle file size differs: " + path.string());
  std::ifstream file(path, std::ios::binary);
  std::vector<std::uint8_t> bytes(size);
  if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) ||
      file.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("checkpoint oracle file read failed: " + path.string());
  return bytes;
}

std::uint64_t
tensorBytes(const std::vector<std::int64_t>& shape, std::uint64_t elementBytes = sizeof(float))
{
  std::uint64_t bytes = elementBytes;
  if (shape.empty()) throw std::invalid_argument("checkpoint oracle tensor must have dimensions");
  for (const auto extent : shape) {
    if (extent <= 0 || static_cast<std::uint64_t>(extent) > (512U << 20) / bytes)
      throw std::invalid_argument("checkpoint oracle tensor exceeds its byte bound");
    bytes *= extent;
  }
  return bytes;
}

void
validate(const fs::path& configuration)
{
  const auto configurationBytes = fs::file_size(configuration);
  if (configurationBytes > (1U << 20))
    throw std::invalid_argument("checkpoint oracle configuration exceeds its byte bound");
  const auto bytes = readFile(configuration, configurationBytes);
  const auto config = nativeParseJson(std::string(bytes.begin(), bytes.end()));
  const auto base = configuration.parent_path();
  if (config.at("schema") != "ndnsf-di-wan-checkpoint-oracle-v1")
    throw std::invalid_argument("unsupported checkpoint oracle configuration");
  const auto absolute = config.at("absolute_tolerance").get<double>();
  const auto relative = config.at("relative_tolerance").get<double>();
  if (!std::isfinite(absolute) || !std::isfinite(relative) || absolute < 0 || relative < 0)
    throw std::invalid_argument("invalid checkpoint oracle tolerance");
  RegistryNativeModelRunnerFactory factory;
  registerOnnxRuntimeBackend(factory);
  NativeModelRunnerSpec spec;
  spec.role = "/wan/checkpoint-oracle";
  spec.kind = "onnx-model";
  spec.backend = "onnxruntime";
  spec.path = (base / config.at("model_file").get<std::string>()).string();
  spec.metadata["executionProvider"] = "cpu";
  spec.metadata["outputScope"] = "result";
  spec.metadata["forceOutputBundle"] = "true";
  RoleExecutionContext context;
  context.sessionId = "wan-checkpoint-oracle";
  context.role = spec.role;
  if (!config.at("inputs").is_array() || config.at("inputs").empty() ||
      config.at("inputs").size() > 4)
    throw std::invalid_argument("checkpoint oracle input count is outside its bound");
  std::uint64_t inputBudget = 512U << 20;
  for (const auto& row : config.at("inputs")) {
    NamedTensor tensor;
    tensor.name = row.at("name").get<std::string>();
    const auto dtype = row.value("dtype", std::string("float32"));
    std::uint64_t elementBytes = sizeof(float);
    if (dtype == "float32") tensor.elementType = TensorElementType::Float32;
    else if (dtype == "int64") {
      tensor.elementType = TensorElementType::Int64;
      elementBytes = sizeof(std::int64_t);
    }
    else if (dtype == "bool") {
      tensor.elementType = TensorElementType::Bool;
      elementBytes = 1;
    }
    else throw std::invalid_argument("unsupported checkpoint oracle input dtype");
    tensor.shape = row.at("shape").get<std::vector<std::int64_t>>();
    const auto inputBytes = tensorBytes(tensor.shape, elementBytes);
    if (inputBytes > inputBudget)
      throw std::invalid_argument("checkpoint oracle inputs exceed their aggregate byte bound");
    inputBudget -= inputBytes;
    tensor.payload = readFile(base / row.at("file").get<std::string>(),
                              inputBytes);
    if (nativePlanningDigest(tensor.payload.data(), tensor.payload.size()) != row.at("digest"))
      throw std::runtime_error("checkpoint oracle input identity differs");
    const auto name = tensor.name;
    if (!context.inputsByScope.emplace(
          name, makeEncodedTensorBundle(name, {std::move(tensor)})).second)
      throw std::invalid_argument("checkpoint oracle duplicate input");
  }
  const auto& reference = config.at("reference");
  const auto shape = reference.at("shape").get<std::vector<std::int64_t>>();
  const auto expected = readFile(base / reference.at("file").get<std::string>(),
                                 tensorBytes(shape));
  if (nativePlanningDigest(expected.data(), expected.size()) != reference.at("digest"))
    throw std::runtime_error("checkpoint oracle reference identity differs");
  const auto creationStarted = std::chrono::steady_clock::now();
  const auto runner = factory.create(spec);
  const auto created = std::chrono::steady_clock::now();
  const auto output = runner->run(context);
  const auto executed = std::chrono::steady_clock::now();
  if (output.size() != 1 || output.count("result") != 1)
    throw std::runtime_error("checkpoint oracle expected one output bundle");
  const auto tensors = decodeTensorBundleViews(output.at("result"));
  if (tensors.size() != 1 || tensors.front().name != reference.at("name") ||
      tensors.front().elementType != TensorElementType::Float32 ||
      tensors.front().shape != shape || tensors.front().payload.size() != expected.size())
    throw std::runtime_error("checkpoint oracle incomplete output");
  double maximum = 0;
  std::size_t failures = 0;
  for (std::size_t at = 0; at < expected.size(); at += sizeof(float)) {
    float actualValue, expectedValue;
    std::memcpy(&actualValue, tensors.front().payload.data() + at, sizeof(float));
    std::memcpy(&expectedValue, expected.data() + at, sizeof(float));
    const auto error = std::abs(static_cast<double>(actualValue) - expectedValue);
    if (!std::isfinite(actualValue) || !std::isfinite(expectedValue) ||
        error > absolute + relative * std::abs(expectedValue))
      ++failures;
    maximum = std::max(maximum, error);
  }
  std::cout << "WAN_CHECKPOINT_ORACLE elements=" << expected.size() / sizeof(float)
            << " max_abs=" << maximum << " failures=" << failures
            << " factory_create_s="
            << std::chrono::duration<double>(created - creationStarted).count()
            << " execute_s=" << std::chrono::duration<double>(executed - created).count() << '\n';
  if (failures != 0) throw std::runtime_error("checkpoint oracle numerical mismatch");
}

} // namespace

int
main(int argc, char** argv)
try {
  if (argc != 2) throw std::invalid_argument("usage: di-wan-checkpoint-oracle CONFIG.json");
  validate(fs::absolute(argv[1]));
  return 0;
}
catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
