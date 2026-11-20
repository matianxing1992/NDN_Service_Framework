/** Independent C++ ONNX Runtime oracle for the Spec 261 detection profiles.
 *
 * The oracle loads the frozen graph, runs the frozen input tensor, and compares
 * the detected rows against the frozen prediction fixture. It never calls the
 * DI splitter, the role merger or any application adapter, so a matching result
 * cannot be produced by the path under test.
 */

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Array
{
  std::vector<std::int64_t> shape;
  std::vector<float> values;
};

std::string
readFile(const std::string& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open " + path);
  return std::string(std::istreambuf_iterator<char>(input), {});
}

/** Parse the small endian-independent subset of .npy this oracle writes. */
Array
readNpy(const std::string& path)
{
  const auto raw = readFile(path);
  if (raw.size() < 128 || raw.compare(0, 6, "\x93NUMPY") != 0)
    throw std::runtime_error("not an npy file: " + path);
  const auto* header = reinterpret_cast<const unsigned char*>(raw.data());
  const auto major = header[6];
  std::size_t offset = 0, length = 0;
  if (major == 1) {
    std::uint16_t value = 0;
    std::memcpy(&value, header + 8, sizeof(value));
    offset = 10;
    length = value;
  }
  else {
    std::uint32_t value = 0;
    std::memcpy(&value, header + 8, sizeof(value));
    offset = 12;
    length = value;
  }
  const auto text = raw.substr(offset, length);
  Array result;
  const auto open = text.find('(');
  const auto close = text.find(')', open);
  if (open == std::string::npos || close == std::string::npos)
    throw std::runtime_error("npy header has no shape");
  std::size_t index = open + 1;
  while (index < close) {
    while (index < close && (text[index] == ' ' || text[index] == ',')) ++index;
    if (index >= close) break;
    std::size_t end = index;
    while (end < close && text[end] >= '0' && text[end] <= '9') ++end;
    if (end == index) throw std::runtime_error("npy shape is not integral");
    result.shape.push_back(std::stoll(text.substr(index, end - index)));
    index = end;
  }
  if (text.find("'<f4'") == std::string::npos)
    throw std::runtime_error("npy fixture is not little-endian float32");
  std::size_t count = 1;
  for (const auto size : result.shape) count *= static_cast<std::size_t>(size);
  const auto dataOffset = offset + length;
  if (raw.size() != dataOffset + count * sizeof(float))
    throw std::runtime_error("npy payload length disagrees with its shape");
  result.values.resize(count);
  std::memcpy(result.values.data(), raw.data() + dataOffset, count * sizeof(float));
  return result;
}

} // namespace

int
main(int argc, char** argv)
{
  std::string graph, input, predictions, inputName;
  std::vector<std::string> outputNames;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (i + 1 >= argc) throw std::runtime_error("missing value for " + flag);
    const std::string value = argv[++i];
    if (flag == "--graph") graph = value;
    else if (flag == "--input") input = value;
    else if (flag == "--predictions") predictions = value;
    else if (flag == "--input-name") inputName = value;
    else if (flag == "--output-name") outputNames.push_back(value);
    else throw std::runtime_error("unknown flag " + flag);
  }
  if (graph.empty() || input.empty() || predictions.empty())
    throw std::runtime_error("oracle requires --graph, --input and --predictions");
  // The frozen graph declares its own terminal output name; a profile whose
  // export names it differently must not force the caller to remember it. Use
  // exactly one output and bind it by the graph's actual name unless asked.
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "spec261-oracle");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  Ort::Session session(env, graph.c_str(), options);
  Ort::AllocatorWithDefaultOptions allocator;
  if (session.GetOutputCount() != 1)
    throw std::runtime_error("oracle expects exactly one graph output");
  auto outputNameValue = session.GetOutputNameAllocated(0, allocator);
  if (outputNames.empty()) outputNames.push_back(outputNameValue.get());
  else if (outputNames.front() != outputNameValue.get())
    throw std::runtime_error("requested output name differs from the graph: " +
                             std::string(outputNameValue.get()));

  const auto frozen = readNpy(input);
  if (frozen.shape.size() != 4 || frozen.shape[0] != 1)
    throw std::runtime_error("frozen input is not a batch-one tensor");
  const std::size_t elements = frozen.values.size();
  const std::array<std::int64_t, 4> shape{
    frozen.shape[0], frozen.shape[1], frozen.shape[2], frozen.shape[3]};

  std::vector<Ort::Value> inputs;
  std::vector<const char*> inputNames;
  const auto inputCount = session.GetInputCount();
  if (inputCount != 1) throw std::runtime_error("oracle supports exactly one input");
  auto inputNameValue = session.GetInputNameAllocated(0, allocator);
  if (inputName.empty()) inputName = inputNameValue.get();
  else if (inputName != inputNameValue.get())
    throw std::runtime_error(std::string("graph input is ") + inputNameValue.get());
  auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  inputs.push_back(Ort::Value::CreateTensor<float>(memory, const_cast<float*>(frozen.values.data()), elements,
                                                 shape.data(), shape.size()));
  inputNames.push_back(inputName.c_str());

  std::vector<const char*> outputNameValues;
  for (const auto& name : outputNames) outputNameValues.push_back(name.c_str());
  auto outputs = session.Run(Ort::RunOptions{nullptr}, inputNames.data(), inputs.data(),
                             inputs.size(), outputNameValues.data(), outputNameValues.size());
  if (outputs.size() != 1) throw std::runtime_error("oracle expects one output tensor");
  const auto info = outputs.front().GetTensorTypeAndShapeInfo();
  const auto shapeValue = info.GetShape();
  const auto values = outputs.front().GetTensorData<float>();
  const auto size = info.GetElementCount();

  const auto expected = readNpy(predictions);
  // Shape equality is part of the ABI: two tensors with the same element count
  // but a different layout would otherwise compare as equal element-wise.
  if (expected.shape.size() != shapeValue.size() ||
      !std::equal(expected.shape.begin(), expected.shape.end(), shapeValue.begin()))
    throw std::runtime_error("prediction shape differs from the frozen fixture");
  if (expected.values.size() != size)
    throw std::runtime_error("prediction row count differs from the frozen fixture");
  double maxAbsolute = 0.0;
  std::size_t failing = 0;
  std::size_t nonFinite = 0;
  for (std::size_t i = 0; i < size; ++i) {
    const double actual = values[i];
    const double reference = expected.values[i];
    if (!std::isfinite(actual) || !std::isfinite(reference)) {
      ++nonFinite;
      continue;
    }
    const double difference = std::abs(actual - reference);
    maxAbsolute = std::max(maxAbsolute, difference);
    if (difference > 1e-4 + 1e-4 * std::abs(reference)) ++failing;
  }
  if (nonFinite != 0) {
    std::cout << "SPEC261_CPP_ORT_ORACLE non_finite=" << nonFinite << std::endl;
    return 3;
  }
  std::cout << "SPEC261_CPP_ORT_ORACLE graph=" << graph << " elements=" << size
            << " max_abs_error=" << maxAbsolute << " failing=" << failing
            << " finite=1 shape=";
  for (std::size_t i = 0; i < shapeValue.size(); ++i)
    std::cout << (i ? "x" : "") << shapeValue[i];
  std::cout << std::endl;
  if (failing != 0) return 2;
  std::cout << "SPEC261_CPP_ORT_ORACLE_PASS" << std::endl;
  return 0;
}
