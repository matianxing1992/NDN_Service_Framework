// Independent, frozen dataset oracle. Never use these names or dimensions as
// production model-discovery, generation, placement or authorization rules.
#include "NDNSF-DistributedInference/cpp/vendor/nlohmann/json.hpp"

#include <onnxruntime_cxx_api.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

constexpr std::size_t VOCABULARY = 151936;
constexpr std::size_t HEADS = 8;
constexpr std::size_t HEAD_DIM = 128;
constexpr std::size_t ROUNDS = 9; // One prefill and eight actual decode invocations.

struct Stamp
{
  Clock::time_point wall = Clock::now();
  std::clock_t cpu = std::clock();
};

Json
elapsed(const Stamp& start, const Stamp& finish)
{
  return {{"wallSeconds", std::chrono::duration<double>(finish.wall - start.wall).count()},
          {"cpuSeconds", double(finish.cpu - start.cpu) / CLOCKS_PER_SEC}};
}

struct Fixture
{
  std::string row;
  std::size_t layers = 0;
  ONNXTensorElementDataType cacheType = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
};

Fixture
frozenFixture(const std::string& row)
{
  static const std::array<Fixture, 6> FIXTURES = {{
    {"Q06-I4", 28, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT},
    {"Q06-I8", 28, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT},
    {"Q06-F16", 28, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16},
    {"Q06-F32", 28, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT},
    {"Q4-I4", 36, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16},
    {"Q4-F16", 36, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16},
  }};
  const auto found = std::find_if(FIXTURES.begin(), FIXTURES.end(),
    [&](const auto& fixture) { return fixture.row == row; });
  if (found == FIXTURES.end()) throw std::invalid_argument("unknown frozen reference row");
  return *found;
}

std::string
digest(const void* data, std::size_t bytes)
{
  std::array<unsigned char, EVP_MAX_MD_SIZE> output{};
  unsigned int length = 0;
  if (EVP_Digest(data, bytes, output.data(), &length, EVP_sha256(), nullptr) != 1 || length != 32)
    throw std::runtime_error("reference SHA256 failed");
  std::ostringstream encoded;
  encoded << "sha256:" << std::hex << std::setfill('0');
  for (unsigned int i = 0; i < length; ++i) encoded << std::setw(2) << unsigned(output[i]);
  return encoded.str();
}

void
checkpoint(const fs::path& directory, const Json& receipt)
{
  const auto pending = directory / "receipt.pending";
  std::ofstream stream(pending, std::ios::binary | std::ios::trunc);
  stream.exceptions(std::ios::badbit | std::ios::failbit);
  stream << receipt.dump(2) << '\n';
  stream.close();
  fs::rename(pending, directory / "receipt.json");
}

void
requireShape(const std::vector<std::int64_t>& actual,
             const std::vector<std::int64_t>& expected)
{
  if (actual != expected) throw std::runtime_error("reference tensor shape mismatch");
}

std::vector<std::string>
stateNames(const Fixture& fixture, bool input)
{
  std::vector<std::string> result;
  for (std::size_t layer = 0; layer < fixture.layers; ++layer) {
    const auto prefix = (input ? "past_key_values." : "present.") + std::to_string(layer);
    result.push_back(prefix + ".key"); result.push_back(prefix + ".value");
  }
  return result;
}

void
checkSignature(Ort::Session& session, const Fixture& fixture,
                const std::vector<std::string>& stateInputs,
                const std::vector<std::string>& stateOutputs)
{
  const auto checkPorts = [&](bool input) {
    std::set<std::string> remaining;
    for (const auto& name : input ? stateInputs : stateOutputs) remaining.insert(name);
    if (input) for (const auto* name : {"input_ids", "attention_mask", "position_ids"})
      remaining.insert(name);
    else remaining.insert("logits");
    const auto count = input ? session.GetInputCount() : session.GetOutputCount();
    if (count != remaining.size()) throw std::runtime_error("reference I/O count mismatch");
    Ort::AllocatorWithDefaultOptions allocator;
    for (std::size_t index = 0; index < count; ++index) {
      auto allocated = input ? session.GetInputNameAllocated(index, allocator) :
                               session.GetOutputNameAllocated(index, allocator);
      const std::string name(allocated.get());
      if (remaining.erase(name) != 1) throw std::runtime_error("reference I/O name mismatch");
      auto information = input ? session.GetInputTypeInfo(index) : session.GetOutputTypeInfo(index);
      const auto type = information.GetTensorTypeAndShapeInfo();
      const auto shape = type.GetShape();
      const bool state = std::find((input ? stateInputs : stateOutputs).begin(),
                                  (input ? stateInputs : stateOutputs).end(), name) !=
                         (input ? stateInputs : stateOutputs).end();
      if (state) {
        if (type.GetElementType() != fixture.cacheType || shape.size() != 4 ||
            shape[1] != HEADS || shape[3] != HEAD_DIM || (shape[0] != -1 && shape[0] != 1))
          throw std::runtime_error("reference KV dtype/head/rank mismatch");
      }
      else if (input) {
        if (type.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64 || shape.size() != 2)
          throw std::runtime_error("reference ordinary input contract mismatch");
      }
      else if (type.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
               shape.size() != 3 || shape[2] != VOCABULARY)
        throw std::runtime_error("reference logits contract mismatch");
    }
  };
  checkPorts(true); checkPorts(false);
}

float
cacheValue(const Ort::Value& tensor, ONNXTensorElementDataType type, std::size_t index)
{
  if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) return tensor.GetTensorData<float>()[index];
  return tensor.GetTensorData<Ort::Float16_t>()[index].ToFloat();
}

Json
inspectCache(const Ort::Value& tensor, const Fixture& fixture,
              const std::string& name, std::int64_t sequence)
{
  const auto information = tensor.GetTensorTypeAndShapeInfo();
  const auto shape = information.GetShape();
  requireShape(shape, {1, HEADS, sequence, HEAD_DIM});
  if (information.GetElementType() != fixture.cacheType)
    throw std::runtime_error("reference present dtype changed");
  const auto count = information.GetElementCount();
  const auto expected = HEADS * static_cast<std::size_t>(sequence) * HEAD_DIM;
  if (count != expected) throw std::runtime_error("reference present element count mismatch");
  const auto width = fixture.cacheType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ? 4U : 2U;
  double squareSum = 0;
  float minimum = std::numeric_limits<float>::infinity(), maximum = -minimum;
  for (std::size_t index = 0; index < count; ++index) {
    const auto number = cacheValue(tensor, fixture.cacheType, index);
    if (!std::isfinite(number)) throw std::runtime_error("reference nonfinite KV value");
    squareSum += double(number) * number;
    minimum = std::min(minimum, number); maximum = std::max(maximum, number);
  }
  auto samples = Json::array();
  for (std::size_t sample = 0; sample < 32 && count != 0; ++sample) {
    const auto index = sample * (count - 1) / 31;
    samples.push_back({{"index", index}, {"value", cacheValue(tensor, fixture.cacheType, index)}});
  }
  return {{"name", name}, {"shape", shape}, {"onnxType", int(fixture.cacheType)},
    {"bytes", count * width}, {"sha256", digest(tensor.GetTensorRawData(), count * width)},
    {"minimum", count == 0 ? Json(nullptr) : Json(minimum)},
    {"maximum", count == 0 ? Json(nullptr) : Json(maximum)},
    {"sumSquares", squareSum}, {"samples", samples}};
}

Json
inspectLogits(const Ort::Value& tensor, std::int64_t sequence,
               const fs::path& output, std::size_t request, std::size_t round)
{
  const auto information = tensor.GetTensorTypeAndShapeInfo();
  requireShape(information.GetShape(), {1, sequence, VOCABULARY});
  if (information.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
      information.GetElementCount() != std::size_t(sequence) * VOCABULARY)
    throw std::runtime_error("reference logits dtype/length mismatch");
  const auto* values = tensor.GetTensorData<float>();
  for (std::size_t index = 0; index < information.GetElementCount(); ++index)
    if (!std::isfinite(values[index])) throw std::runtime_error("reference nonfinite logits");
  const auto* row = values + (sequence - 1) * VOCABULARY;
  std::size_t best = 0;
  for (std::size_t index = 1; index < VOCABULARY; ++index)
    if (row[index] > row[best]) best = index; // Strict > preserves lowest-ID ties.
  const auto bytes = VOCABULARY * sizeof(float);
  const auto file = "step-" + std::to_string(round) + "-last-logits.f32le";
  if (request == 0) {
    std::ofstream stream(output / file, std::ios::binary);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream.write(reinterpret_cast<const char*>(row), bytes);
    stream.close();
  }
  return {{"shape", information.GetShape()}, {"onnxType", 1}, {"lastRowBytes", bytes},
    {"lastRowSha256", digest(row, bytes)}, {"lastRowFile", file},
    {"greedyToken", best}, {"greedyValue", row[best]}};
}

} // namespace

int
main(int argc, char** argv)
{
  if (argc != 4) return 2;
  const Stamp start;
  fs::path directory;
  Json receipt;
  std::string boundary = "fixture";
  try {
    const auto fixture = frozenFixture(argv[1]);
    const std::uint16_t endian = 1;
    if (*reinterpret_cast<const std::uint8_t*>(&endian) != 1)
      throw std::runtime_error("reference requires declared little-endian float storage");
    directory = fs::absolute(argv[3]);
    if (!fs::create_directory(directory)) throw std::runtime_error("reference output already exists");
    fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace);
    receipt = {{"schema", "spec233-uncut-cpu-reference-v1"}, {"row", fixture.row},
      {"model", fs::absolute(argv[2]).string()}, {"runtime", Ort::GetVersionString()},
      {"provider", "CPUExecutionProvider"}, {"threads", {{"intra", 1}, {"inter", 1}}},
      {"optimization", "ORT_ENABLE_ALL"}, {"prompt", {1, 2, 3}}, {"requests", 2},
      {"decodeCallsPerRequest", 8}, {"roundsPerRequest", ROUNDS},
      {"kvHeads", HEADS}, {"headDim", HEAD_DIM}, {"layers", fixture.layers},
      {"tolerance", {{"absolute", fixture.cacheType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ? 1e-4 : 5e-3},
                      {"relative", fixture.cacheType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ? 1e-4 : 5e-3},
                      {"scope", "identical artifact and matched EP/runtime"}}},
      {"rounds", Json::array()}, {"status", "RUNNING"},
      {"scope", "independent uncut reference, NOT distributed qualification"}};
    boundary = "session-construction"; receipt["boundary"] = boundary; checkpoint(directory, receipt);
    Ort::Env environment(ORT_LOGGING_LEVEL_ERROR, "spec233-uncut-reference");
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(1); options.SetInterOpNumThreads(1);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    const Stamp sessionStart;
    Ort::Session session(environment, argv[2], options);
    const Stamp sessionEnd;
    receipt["sessionConstruction"] = elapsed(sessionStart, sessionEnd);
    const auto stateInputs = stateNames(fixture, true), stateOutputs = stateNames(fixture, false);
    boundary = "source-signature";
    checkSignature(session, fixture, stateInputs, stateOutputs);
    std::vector<std::string> inputNames{"input_ids", "attention_mask", "position_ids"};
    inputNames.insert(inputNames.end(), stateInputs.begin(), stateInputs.end());
    std::vector<std::string> outputNames{"logits"};
    outputNames.insert(outputNames.end(), stateOutputs.begin(), stateOutputs.end());
    std::vector<const char*> inputs, outputs;
    for (const auto& name : inputNames) inputs.push_back(name.c_str());
    for (const auto& name : outputNames) outputs.push_back(name.c_str());
    Ort::AllocatorWithDefaultOptions allocator;
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    double measuredWall = receipt["sessionConstruction"]["wallSeconds"].get<double>();
    double measuredCpu = receipt["sessionConstruction"]["cpuSeconds"].get<double>();
    for (std::size_t request = 0; request < 2; ++request) {
      // Each independent request starts with freshly allocated zero-length past.
      std::vector<Ort::Value> past;
      const std::array<std::int64_t, 4> emptyShape{1, HEADS, 0, HEAD_DIM};
      for (std::size_t i = 0; i < stateInputs.size(); ++i)
        past.push_back(Ort::Value::CreateTensor(allocator, emptyShape.data(), emptyShape.size(),
                                               fixture.cacheType));
      std::vector<std::int64_t> tokens{1, 2, 3};
      std::int64_t acceptedPast = 0;
      for (std::size_t round = 0; round < ROUNDS; ++round) {
        const Stamp roundStart;
        receipt["rounds"].push_back({{"request", request}, {"round", round},
          {"phase", round == 0 ? "prefill" : "decode"}, {"pastLength", acceptedPast},
          {"inputTokens", tokens}, {"status", "RUNNING"}});
        auto& record = receipt["rounds"].back();
        boundary = "input-preparation"; receipt["boundary"] = boundary;
        const auto sequence = static_cast<std::int64_t>(tokens.size());
        const auto total = acceptedPast + sequence;
        std::vector<std::int64_t> mask(total, 1), positions(sequence);
        for (std::int64_t i = 0; i < sequence; ++i) positions[i] = acceptedPast + i;
        const std::array<std::int64_t, 2> tokenShape{1, sequence}, maskShape{1, total};
        std::vector<Ort::Value> tensors;
        tensors.push_back(Ort::Value::CreateTensor<std::int64_t>(memory, tokens.data(), tokens.size(),
          tokenShape.data(), tokenShape.size()));
        tensors.push_back(Ort::Value::CreateTensor<std::int64_t>(memory, mask.data(), mask.size(),
          maskShape.data(), maskShape.size()));
        tensors.push_back(Ort::Value::CreateTensor<std::int64_t>(memory, positions.data(), positions.size(),
          tokenShape.data(), tokenShape.size()));
        for (std::size_t state = 0; state < past.size(); ++state) {
          requireShape(past[state].GetTensorTypeAndShapeInfo().GetShape(),
                       {1, HEADS, acceptedPast, HEAD_DIM});
          tensors.push_back(std::move(past[state]));
        }
        past.clear();
        boundary = "ORT-run"; receipt["boundary"] = boundary; checkpoint(directory, receipt);
        const Stamp beforeRun;
        auto result = session.Run(Ort::RunOptions{nullptr}, inputs.data(), tensors.data(), tensors.size(),
                                  outputs.data(), outputs.size());
        const Stamp afterRun;
        record["preparation"] = elapsed(roundStart, beforeRun);
        record["execution"] = elapsed(beforeRun, afterRun);
        boundary = "output-state-validation"; receipt["boundary"] = boundary;
        if (result.size() != outputNames.size()) throw std::runtime_error("reference output cover changed");
        record["logits"] = inspectLogits(result[0], sequence, directory, request, round);
        auto states = Json::array();
        for (std::size_t state = 0; state < stateOutputs.size(); ++state)
          states.push_back(inspectCache(result[state + 1], fixture, stateOutputs[state], total));
        record["states"] = std::move(states);
        if (request == 1) {
          const auto& cold = receipt["rounds"].at(round);
          if (record["logits"] != cold["logits"] || record["states"] != cold["states"] ||
              record["inputTokens"] != cold["inputTokens"])
            throw std::runtime_error("REFERENCE_REQUEST_RESET_MISMATCH");
        }
        acceptedPast = total;
        for (std::size_t state = 0; state < stateOutputs.size(); ++state)
          past.push_back(std::move(result[state + 1])); // Full present replaces past; never append it.
        tokens = {record["logits"]["greedyToken"].get<std::int64_t>()};
        record["presentLength"] = acceptedPast;
        record["validationAndOutput"] = elapsed(afterRun, Stamp{});
        record["status"] = "PASS";
        for (const auto* phase : {"preparation", "execution", "validationAndOutput"}) {
          measuredWall += record[phase]["wallSeconds"].get<double>();
          measuredCpu += record[phase]["cpuSeconds"].get<double>();
        }
        checkpoint(directory, receipt);
        std::cout << "REFERENCE_ROUND_PASS request=" << request << " round=" << round
                  << " past=" << record["pastLength"] << " present=" << acceptedPast
                  << " token=" << tokens.front() << std::endl;
      }
    }
    receipt["status"] = "PASS"; receipt["boundary"] = "complete";
    receipt["total"] = elapsed(start, Stamp{});
    receipt["otherHarness"] = {{"wallSeconds", receipt["total"]["wallSeconds"].get<double>() - measuredWall},
                               {"cpuSeconds", receipt["total"]["cpuSeconds"].get<double>() - measuredCpu}};
    receipt["timingNote"] = "Add sessionConstruction, every round preparation/execution/validationAndOutput, and otherHarness. Final receipt/session destruction is outside total. No network time is inferred.";
    checkpoint(directory, receipt);
    std::cout << "ONNX_UNCUT_REFERENCE_PASS " << fixture.row << std::endl;
    return 0;
  }
  catch (const std::exception& error) {
    if (!receipt.is_null()) {
      receipt["status"] = "FAILED"; receipt["boundary"] = boundary;
      receipt["error"] = error.what(); receipt["elapsedAtFailure"] = elapsed(start, Stamp{});
      if (!receipt["rounds"].empty() && receipt["rounds"].back()["status"] == "RUNNING")
        receipt["rounds"].back()["status"] = "FAILED";
      try { checkpoint(directory, receipt); } catch (...) { }
    }
    std::cerr << "ONNX_UNCUT_REFERENCE_FAILED boundary=" << boundary << ": " << error.what()
              << std::endl;
    return 1;
  }
}
