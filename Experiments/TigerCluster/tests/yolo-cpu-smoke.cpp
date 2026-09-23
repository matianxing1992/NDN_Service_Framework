// Standalone base/ORT acceptance; this does not exercise the NDNSF protocol.
#include <onnxruntime_cxx_api.h>
#ifdef NDNSF_NATIVE_RUNNER
#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include <cstring>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using Row = std::array<float, 6>;

static void require(bool value, const char* reason)
{
  if (!value) throw std::runtime_error(reason);
}

static std::vector<float> input(const char* path)
{
  std::ifstream stream(path);
  std::string magic;
  int width = 0, height = 0, maximum = 0;
  stream >> magic >> width >> height >> maximum;
  require(stream.good() && magic == "P3" && width == 4 && height == 4 && maximum == 255,
          "FIXTURE_FORMAT");
  std::array<float, 48> pixels{};
  for (auto& pixel : pixels) {
    int value = -1;
    require(bool(stream >> value) && value >= 0 && value <= 255, "FIXTURE_PIXEL");
    pixel = float(value) / 255.0f;
  }
  std::string extra;
  require(!(stream >> extra), "FIXTURE_TRAILING_DATA");
  std::vector<float> values(3 * 640 * 640);
  // Match frozen oracle: bilinear resize, align_corners=False, float32 NCHW.
  for (int y = 0; y < 640; ++y) for (int x = 0; x < 640; ++x) {
    const float sy = std::max(0.0f, (float(y) + 0.5f) * (4.0f / 640) - 0.5f);
    const float sx = std::max(0.0f, (float(x) + 0.5f) * (4.0f / 640) - 0.5f);
    const int y0 = int(sy), x0 = int(sx);
    const int y1 = std::min(y0 + 1, 3), x1 = std::min(x0 + 1, 3);
    const float dy = sy - y0, dx = sx - x0;
    for (int c = 0; c < 3; ++c) {
      const float a = (1 - dx) * pixels[(y0 * 4 + x0) * 3 + c] + dx * pixels[(y0 * 4 + x1) * 3 + c];
      const float b = (1 - dx) * pixels[(y1 * 4 + x0) * 3 + c] + dx * pixels[(y1 * 4 + x1) * 3 + c];
      values[c * 640 * 640 + y * 640 + x] = (1 - dy) * a + dy * b;
    }
  }
  return values;
}

static std::vector<Row> oracle(const char* path)
{
  std::ifstream stream(path, std::ios::binary);
  std::array<unsigned char, 10> prefix{};
  stream.read(reinterpret_cast<char*>(prefix.data()), prefix.size());
  require(stream.good() && std::string(reinterpret_cast<char*>(prefix.data()), 6) == "\x93NUMPY"
          && prefix[6] == 1 && prefix[7] == 0, "ORACLE_NPY_VERSION");
  const auto length = unsigned(prefix[8]) + unsigned(prefix[9]) * 256;
  require(length > 0 && length < 4096, "ORACLE_HEADER_LENGTH");
  std::string header(length, '\0');
  stream.read(header.data(), length);
  require(stream.good() && header.find("'<f4'") != std::string::npos &&
          header.find("'fortran_order': False") != std::string::npos &&
          header.find("(1, 50, 6)") != std::string::npos, "ORACLE_HEADER");
  std::vector<Row> rows(50);
  stream.read(reinterpret_cast<char*>(rows.data()), rows.size() * sizeof(Row));
  require(stream.good() && stream.peek() == std::char_traits<char>::eof(), "ORACLE_PAYLOAD");
  for (const auto& row : rows) for (float value : row) require(std::isfinite(value), "ORACLE_NONFINITE");
  return rows;
}

int main(int argc, char** argv)
try {
  require(argc == 4, "usage: yolo-cpu-smoke MODEL FIXTURE ORACLE");
  const auto reference = oracle(argv[3]);
  auto pixels = input(argv[2]);
#ifdef NDNSF_NATIVE_RUNNER
  using namespace ndnsf::di;
  const auto start = std::chrono::steady_clock::now();
  NativeModelRunnerSpec spec{"YOLO", "onnx", "onnxruntime", argv[1],
                             {{"output_tensor", "predictions"}}};
  OnnxRuntimeModelRunner session(spec);
  NamedTensor tensor;
  tensor.name = "images";
  tensor.shape = {1, 3, 640, 640};
  tensor.payload.resize(pixels.size() * sizeof(float));
  std::memcpy(tensor.payload.data(), pixels.data(), tensor.payload.size());
  RoleExecutionContext context;
  context.role = "YOLO";
  context.sessionId = "candidate-yolo-cpu";
  context.requestId = "candidate-yolo-cpu";
  context.inputsByScope["images"] = TensorBundle{"images", encodeTensorBundle({tensor})};
#else
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "yolo-cpu-smoke");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(4);
  options.SetInterOpNumThreads(1);
  // No accelerator is appended: ORT's default CPUExecutionProvider owns this session.
  const auto start = std::chrono::steady_clock::now();
  Ort::Session session(env, argv[1], options);
  require(session.GetInputCount() == 1 && session.GetOutputCount() == 1, "MODEL_IO_COUNT");
  const std::array<int64_t, 4> shape{1, 3, 640, 640};
  auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  auto tensor = Ort::Value::CreateTensor<float>(memory, pixels.data(), pixels.size(), shape.data(), shape.size());
  const char* inputs[] = {"images"};
  const char* outputs[] = {"predictions"};
#endif
  std::cout << "session_ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() << '\n';
  for (int run = 0; run < 3; ++run) {
    const auto begin = std::chrono::steady_clock::now();
#ifdef NDNSF_NATIVE_RUNNER
    auto result = session.run(context);
    require(result.size() == 1, "MODEL_OUTPUT_COUNT");
    const auto tensors = decodeTensorBundle(result.begin()->second.payload);
    const auto& output = findTensor(tensors, "predictions");
    const auto dims = output.shape;
    require(output.elementType == TensorElementType::Float32 && dims.size() == 3 &&
            dims[0] == 1 && dims[1] > 0 && dims[1] <= 300 && dims[2] == 6, "OUTPUT_SHAPE");
    require(output.payload.size() == std::size_t(dims[1] * 6) * sizeof(float), "OUTPUT_BYTES");
    std::vector<float> values(output.payload.size() / sizeof(float));
    std::memcpy(values.data(), output.payload.data(), output.payload.size());
    const auto* data = values.data();
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
#else
    auto result = session.Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    auto info = result.at(0).GetTensorTypeAndShapeInfo();
    auto dims = info.GetShape();
    require(info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT && dims.size() == 3 &&
            dims[0] == 1 && dims[1] > 0 && dims[1] <= 300 && dims[2] == 6, "OUTPUT_SHAPE");
    const auto* data = result[0].GetTensorData<float>();
#endif
    std::vector<Row> rows;
    for (int64_t i = 0; i < dims[1]; ++i) {
      Row row{};
      std::copy_n(data + i * 6, 6, row.begin());
      for (float value : row) require(std::isfinite(value), "OUTPUT_NONFINITE");
      if (row[4] >= 0.001f) rows.push_back(row);
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
      if (a[4] != b[4]) return a[4] > b[4];
      for (int k : {5, 0, 1, 2, 3}) if (a[k] != b[k]) return a[k] < b[k];
      return false;
    });
    require(rows.size() == reference.size(), "DETECTION_COUNT_MISMATCH");
    float error = 0;
    for (size_t i = 0; i < rows.size(); ++i) for (size_t j = 0; j < 6; ++j) {
      const auto delta = std::abs(rows[i][j] - reference[i][j]);
      error = std::max(error, delta);
      require(delta <= 1e-3f + 1e-4f * std::abs(reference[i][j]), "ORACLE_VALUE_MISMATCH");
    }
    std::cout << "run=" << run << " inference_ms=" << elapsed << " rows=" << rows.size()
              << " max_abs_error=" << error << '\n';
  }
#ifdef NDNSF_NATIVE_RUNNER
  std::cout << "YOLO_CPU_NATIVE_RUNNER_PASS ort=" << OrtGetApiBase()->GetVersionString() << '\n';
#else
  std::cout << "YOLO_CPU_MODEL_SMOKE_PASS ort=" << OrtGetApiBase()->GetVersionString() << '\n';
#endif
  return 0;
}
catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
