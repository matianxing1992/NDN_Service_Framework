#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-generation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
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
prepareInitialLatent(const fs::path& configuration, const fs::path& output)
{
  static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                "initial latent reference requires IEEE754 FP32");
  const std::uint16_t endian = 1;
  if (*reinterpret_cast<const std::uint8_t*>(&endian) != 1)
    throw std::runtime_error("initial latent reference requires a little-endian host");
  const auto configurationBytes = fs::file_size(configuration);
  if (configurationBytes > (1U << 20))
    throw std::invalid_argument("initial latent configuration exceeds its byte bound");
  const auto wire = readFile(configuration, configurationBytes);
  const auto config = nativeParseJson(std::string(wire.begin(), wire.end()));
  const auto& options = config.at("generation");
  if (!options.at("latent_shape").is_array() || options.at("latent_shape").size() != 4 ||
      !options.at("seed").is_number_unsigned() ||
      !options.at("vae_spatial_stride").is_number_unsigned() ||
      options.at("vae_spatial_stride").get<std::uint64_t>() > 16)
    throw std::invalid_argument("invalid initial latent geometry or seed");
  for (const auto& extent : options.at("latent_shape")) {
    if (!extent.is_number_integer())
      throw std::invalid_argument("initial latent dimensions must be integers");
  }
  const auto shape = options.at("latent_shape").get<std::array<std::int64_t, 4>>();
  const auto stride = options.at("vae_spatial_stride").get<std::uint32_t>();
  (void)wan::planWanClipOutput(shape, stride);
  const auto count = tensorBytes({1, shape[0], shape[1], shape[2], shape[3]}) / sizeof(float);
  const auto seed = options.at("seed").get<std::uint64_t>();
  std::mt19937_64 engine(seed);
  const auto uniform = [&] {
    return static_cast<double>((engine() >> 11) + 1) * 0x1p-53;
  };
  constexpr double TWO_PI = 6.2831853071795864769;
  std::vector<float> latent(count);
  for (std::size_t i = 0; i < count; i += 2) {
    const auto radius = std::sqrt(-2 * std::log(uniform()));
    const auto angle = TWO_PI * uniform();
    latent[i] = static_cast<float>(radius * std::cos(angle));
    if (i + 1 < count) latent[i + 1] = static_cast<float>(radius * std::sin(angle));
  }
  std::unique_ptr<std::FILE, decltype(&std::fclose)> file(
    std::fopen(output.c_str(), "wbx"), &std::fclose);
  if (!file) throw std::runtime_error("initial latent output cannot be exclusively created");
  if (std::fwrite(latent.data(), sizeof(float), count, file.get()) != count ||
      std::fflush(file.get()) != 0)
    throw std::runtime_error("initial latent output write failed");
  if (std::fclose(file.release()) != 0)
    throw std::runtime_error("initial latent output close failed");
  std::cout << nativeCanonicalJson(NativeJson{
    {"shape", {1, shape[0], shape[1], shape[2], shape[3]}}, {"seed", seed},
    {"bytes", latent.size() * sizeof(float)}, {"dtype", "float32"}, {"byte_order", "little"},
    {"digest", nativePlanningDigest(reinterpret_cast<const std::uint8_t*>(latent.data()),
                                    latent.size() * sizeof(float))}}) << '\n';
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
  if (argc == 4 && std::string(argv[1]) == "--initial-latent") {
    prepareInitialLatent(fs::absolute(argv[2]), fs::absolute(argv[3]));
    return 0;
  }
  if (argc != 2) throw std::invalid_argument("usage: di-wan-checkpoint-oracle CONFIG.json");
  validate(fs::absolute(argv[1]));
  return 0;
}
catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
