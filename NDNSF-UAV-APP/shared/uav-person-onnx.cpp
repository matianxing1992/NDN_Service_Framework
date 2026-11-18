#include "uav-person-onnx.hpp"

#include "UavDigest.hpp"

#include <jpeglib.h>
#include <ndn-cxx/util/sha256.hpp>
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace ndnsf::examples::uav {
namespace {

std::string
sha256File(const std::filesystem::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::invalid_argument("cannot open person ReID model");
  ndn::util::Sha256 hash;
  std::array<char, 1024 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto read = input.gcount();
    if (read > 0) hash << std::string_view(buffer.data(), static_cast<size_t>(read));
  }
  if (!input.eof()) throw std::runtime_error("person ReID model read failed");
  return canonicalSha256Digest(hash.toString());
}

std::string
allocatedString(Ort::AllocatorWithDefaultOptions& allocator, Ort::Session& session, bool input)
{
  auto value = input ? session.GetInputNameAllocated(0, allocator)
                     : session.GetOutputNameAllocated(0, allocator);
  return std::string(value.get());
}

std::vector<int64_t>
checkedShape(const std::vector<int64_t>& shape, const char* role)
{
  if (shape.size() != 4 || shape[0] != -1 || shape[1] != 3 ||
      shape[2] != static_cast<int64_t>(256) || shape[3] != static_cast<int64_t>(128)) {
    throw std::invalid_argument(std::string("unexpected person ReID ") + role + " shape");
  }
  return shape;
}

} // namespace

std::string
personOnnxRuntimeId()
{
  return "onnxruntime-" + std::string(OrtGetApiBase()->GetVersionString());
}

struct UavPersonReidSession::Impl
{
  Ort::Env env;
  Ort::SessionOptions options;
  Ort::Session session;
  std::string inputName;
  std::string outputName;

  static Ort::SessionOptions
  makeOptions()
  {
    Ort::SessionOptions value;
    value.SetIntraOpNumThreads(1);
    value.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);
    return value;
  }

  explicit Impl(const std::filesystem::path& model)
    : env(ORT_LOGGING_LEVEL_WARNING, "ndnsf-person-reid")
    , options(makeOptions())
    , session(env, model.c_str(), options)
  {
    Ort::AllocatorWithDefaultOptions allocator;
    inputName = allocatedString(allocator, session, true);
    outputName = allocatedString(allocator, session, false);
    if (session.GetInputCount() != 1 || session.GetOutputCount() != 1 ||
        session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetElementType() !=
          ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
        session.GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetElementType() !=
          ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
      throw std::invalid_argument("person ReID model must have one float input and output");
    }
    checkedShape(session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape(), "input");
    const auto outputShape =
      session.GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    if (outputShape.size() != 2 || outputShape[0] != -1 ||
        outputShape[1] != static_cast<int64_t>(FEATURE_DIMENSION)) {
      throw std::invalid_argument("person ReID model output must be [batch, 512]");
    }
  }
};

UavPersonReidSession::UavPersonReidSession(const std::filesystem::path& model,
                                           size_t maximumBatch)
  : m_impl(std::make_unique<Impl>(model))
  , m_maximumBatch(maximumBatch)
  , m_modelDigest(sha256File(model))
{
  if (maximumBatch == 0 || maximumBatch > 32) {
    throw std::invalid_argument("person ReID batch bound must be 1..32");
  }
}

UavPersonReidSession::~UavPersonReidSession() = default;

std::vector<float>
UavPersonReidSession::embed(const std::vector<float>& input, size_t count,
                            const std::function<bool()>& stillAuthorized)
{
  if (count == 0 || count > m_maximumBatch ||
      input.size() != count * 3 * INPUT_HEIGHT * INPUT_WIDTH) {
    throw std::invalid_argument("person ReID input size does not match its batch");
  }
  if (!std::all_of(input.begin(), input.end(), [] (float value) {
        return std::isfinite(value);
      })) {
    throw std::invalid_argument("person ReID input contains a non-finite value");
  }
  if (stillAuthorized && !stillAuthorized()) {
    throw std::runtime_error("person ReID cancelled before inference");
  }
  const std::array<int64_t, 4> inputShape{static_cast<int64_t>(count), 3,
    static_cast<int64_t>(INPUT_HEIGHT), static_cast<int64_t>(INPUT_WIDTH)};
  auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  auto tensor = Ort::Value::CreateTensor<float>(memory,
    const_cast<float*>(input.data()), input.size(), inputShape.data(), inputShape.size());
  const char* inputNames[] = {m_impl->inputName.c_str()};
  const char* outputNames[] = {m_impl->outputName.c_str()};
  Ort::RunOptions runOptions;
  std::atomic<bool> watch{true};
  std::thread watchdog;
  if (stillAuthorized) {
    watchdog = std::thread([&] {
      while (watch.load(std::memory_order_relaxed)) {
        if (!stillAuthorized()) {
          runOptions.SetTerminate();
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
  }
  std::vector<Ort::Value> outputs;
  try {
    outputs = m_impl->session.Run(runOptions, inputNames, &tensor, 1, outputNames, 1);
  }
  catch (...) {
    watch = false;
    if (watchdog.joinable()) watchdog.join();
    throw;
  }
  watch = false;
  if (watchdog.joinable()) watchdog.join();
  if (stillAuthorized && !stillAuthorized()) {
    throw std::runtime_error("person ReID cancelled after inference");
  }
  if (outputs.size() != 1 || !outputs.front().IsTensor()) {
    throw std::runtime_error("person ReID returned no tensor");
  }
  const auto info = outputs.front().GetTensorTypeAndShapeInfo();
  const auto actualShape = info.GetShape();
  if (actualShape.size() != 2 || actualShape[0] != static_cast<int64_t>(count) ||
      actualShape[1] != static_cast<int64_t>(FEATURE_DIMENSION) ||
      info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
    throw std::runtime_error("person ReID returned an unexpected output shape");
  }
  const float* data = outputs.front().GetTensorData<float>();
  std::vector<float> result(data, data + count * FEATURE_DIMENSION);
  for (size_t row = 0; row < count; ++row) {
    float* feature = result.data() + row * FEATURE_DIMENSION;
    double norm = 0;
    for (size_t index = 0; index < FEATURE_DIMENSION; ++index) {
      if (!std::isfinite(feature[index])) {
        throw std::runtime_error("person ReID returned a non-finite embedding");
      }
      norm += static_cast<double>(feature[index]) * feature[index];
    }
    norm = std::sqrt(norm);
    if (!std::isfinite(norm) || norm <= 1e-12) {
      throw std::runtime_error("person ReID returned a zero embedding");
    }
    for (size_t index = 0; index < FEATURE_DIMENSION; ++index) {
      feature[index] = static_cast<float>(feature[index] / norm);
    }
  }
  return result;
}



namespace {

struct JpegErrorManager
{
  jpeg_error_mgr base;
  std::jmp_buf jump;
};

void
jpegFailure(j_common_ptr info)
{
  auto* manager = reinterpret_cast<JpegErrorManager*>(info->err);
  std::longjmp(manager->jump, 1);
}

} // namespace

namespace {

struct RawJpeg
{
  unsigned char* pixels = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
};

RawJpeg
decodeJpegRaw(const ndn::Buffer& jpeg)
{
  if (jpeg.empty() || jpeg.size() > 8U * 1024U * 1024U) {
    throw std::invalid_argument("admitted JPEG is empty or exceeds its byte bound");
  }
  jpeg_decompress_struct info{};
  JpegErrorManager error{};
  info.err = jpeg_std_error(&error.base);
  error.base.error_exit = jpegFailure;
  RawJpeg raw;
  if (setjmp(error.jump) != 0) {
    jpeg_destroy_decompress(&info);
    std::free(raw.pixels);
    throw std::runtime_error("admitted JPEG decode failed");
  }
  jpeg_create_decompress(&info);
  jpeg_mem_src(&info, const_cast<unsigned char*>(jpeg.data()), jpeg.size());
  jpeg_read_header(&info, TRUE);
  info.out_color_space = JCS_RGB;
  const uint32_t width = info.image_width;
  const uint32_t height = info.image_height;
  if (width == 0 || height == 0 || width > 8192 || height > 8192 ||
      static_cast<uint64_t>(width) * height > 16U * 1024U * 1024U) {
    jpeg_destroy_decompress(&info);
    throw std::invalid_argument("admitted JPEG dimensions are invalid");
  }
  jpeg_start_decompress(&info);
  if (info.output_components != 3) {
    jpeg_destroy_decompress(&info);
    throw std::invalid_argument("admitted JPEG is not RGB decodable");
  }
  const size_t bytes = static_cast<size_t>(width) * height * 3;
  raw.pixels = static_cast<unsigned char*>(std::malloc(bytes));
  if (raw.pixels == nullptr) {
    jpeg_destroy_decompress(&info);
    throw std::bad_alloc();
  }
  while (info.output_scanline < height) {
    JSAMPROW row = raw.pixels + info.output_scanline * static_cast<size_t>(width) * 3;
    jpeg_read_scanlines(&info, &row, 1);
  }
  jpeg_finish_decompress(&info);
  jpeg_destroy_decompress(&info);
  raw.width = width;
  raw.height = height;
  return raw;
}

} // namespace

UavRgbImage
decodePersonJpeg(const ndn::Buffer& jpeg)
{
  auto raw = decodeJpegRaw(jpeg);
  UavRgbImage result;
  result.width = raw.width;
  result.height = raw.height;
  result.rgb.assign(raw.pixels, raw.pixels +
    static_cast<size_t>(raw.width) * raw.height * 3);
  std::free(raw.pixels);
  return result;
}

UavLetterboxedImage
letterboxPersonImage(const UavRgbImage& image)
{
  constexpr int size = 640;
  if (image.width == 0 || image.height == 0 ||
      image.rgb.size() != static_cast<size_t>(image.width) * image.height * 3) {
    throw std::invalid_argument("RGB image is not initialized");
  }
  const double scale = std::min(static_cast<double>(size) / image.width,
                                static_cast<double>(size) / image.height);
  const int newWidth = static_cast<int>(std::lround(image.width * scale));
  const int newHeight = static_cast<int>(std::lround(image.height * scale));
  if (newWidth <= 0 || newHeight <= 0 || newWidth > size || newHeight > size) {
    throw std::invalid_argument("letterbox target is empty or oversized");
  }
  const int padX = static_cast<int>(std::lround((size - newWidth) / 2.0 - 0.1));
  const int padY = static_cast<int>(std::lround((size - newHeight) / 2.0 - 0.1));
  UavLetterboxedImage result;
  result.scale = scale;
  result.padX = padX;
  result.padY = padY;
  result.sourceWidth = image.width;
  result.sourceHeight = image.height;
  result.tensor.assign(3 * size * size, 114.0F / 255.0F);
  for (int dy = 0; dy < newHeight; ++dy) {
    const double sourceY = (dy + 0.5) / scale - 0.5;
    const int y0 = static_cast<int>(std::floor(std::clamp(sourceY, 0.0,
      static_cast<double>(image.height - 1))));
    const int y1 = std::min<int>(y0 + 1, image.height - 1);
    const double wy = std::clamp(sourceY - y0, 0.0, 1.0);
    for (int dx = 0; dx < newWidth; ++dx) {
      const double sourceX = (dx + 0.5) / scale - 0.5;
      const int x0 = static_cast<int>(std::floor(std::clamp(sourceX, 0.0,
        static_cast<double>(image.width - 1))));
      const int x1 = std::min<int>(x0 + 1, image.width - 1);
      const double wx = std::clamp(sourceX - x0, 0.0, 1.0);
      const auto at = [&] (int x, int y, int channel) {
        return static_cast<double>(image.rgb[(static_cast<size_t>(y) * image.width + x) * 3 + channel]);
      };
      for (int channel = 0; channel < 3; ++channel) {
        const double top = at(x0, y0, channel) * (1 - wx) + at(x1, y0, channel) * wx;
        const double bottom = at(x0, y1, channel) * (1 - wx) + at(x1, y1, channel) * wx;
        const double value = (top * (1 - wy) + bottom * wy) / 255.0;
        result.tensor[static_cast<size_t>(channel) * size * size +
                      static_cast<size_t>(dy + padY) * size + (dx + padX)] =
          static_cast<float>(value);
      }
    }
  }
  return result;
}

struct UavPersonDetectorSession::Impl
{
  Ort::Env env;
  Ort::SessionOptions options;
  Ort::Session session;
  std::string inputName;
  std::string outputName;

  static Ort::SessionOptions
  makeOptions()
  {
    Ort::SessionOptions value;
    value.SetIntraOpNumThreads(1);
    value.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);
    return value;
  }

  explicit Impl(const std::filesystem::path& model)
    : env(ORT_LOGGING_LEVEL_WARNING, "ndnsf-person-detector")
    , options(makeOptions())
    , session(env, model.c_str(), options)
  {
    Ort::AllocatorWithDefaultOptions allocator;
    inputName = allocatedString(allocator, session, true);
    outputName = allocatedString(allocator, session, false);
    if (session.GetInputCount() != 1 || session.GetOutputCount() != 1) {
      throw std::invalid_argument("person detector must have one input and one output");
    }
    const auto inputShape = session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    if (inputShape.size() != 4 || inputShape[0] != 1 || inputShape[1] != 3 ||
        inputShape[2] != static_cast<int64_t>(UavPersonDetectorSession::INPUT_SIZE) ||
        inputShape[3] != static_cast<int64_t>(UavPersonDetectorSession::INPUT_SIZE)) {
      throw std::invalid_argument("person detector input must be [1,3,640,640]");
    }
    const auto outputShape = session.GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    if (outputShape.size() != 3 || outputShape[0] != 1 ||
        outputShape[1] < 5 || outputShape[2] <= 0) {
      throw std::invalid_argument("person detector output must be [1,classes,anchors]");
    }
  }
};

UavPersonDetectorSession::UavPersonDetectorSession(const std::filesystem::path& model,
                                                   float confidenceThreshold,
                                                   float nmsIou,
                                                   size_t maximumDetections)
  : m_impl(std::make_unique<Impl>(model))
  , m_confidenceThreshold(confidenceThreshold)
  , m_nmsIou(nmsIou)
  , m_maximumDetections(maximumDetections)
  , m_modelDigest(sha256File(model))
{
  if (!(confidenceThreshold > 0 && confidenceThreshold <= 1) ||
      !(nmsIou > 0 && nmsIou <= 1) || maximumDetections == 0 ||
      maximumDetections > MAX_DETECTIONS) {
    throw std::invalid_argument("invalid person detector thresholds/bounds");
  }
}

UavPersonDetectorSession::~UavPersonDetectorSession() = default;

std::vector<UavPersonBoxDetection>
UavPersonDetectorSession::detect(const std::vector<float>& input,
                                 const std::function<bool()>& stillAuthorized)
{
  constexpr size_t expected = 3 * INPUT_SIZE * INPUT_SIZE;
  if (input.size() != expected ||
      !std::all_of(input.begin(), input.end(), [] (float value) { return std::isfinite(value); })) {
    throw std::invalid_argument("person detector input must be 3x640x640 finite floats");
  }
  if (stillAuthorized && !stillAuthorized()) {
    throw std::runtime_error("person detector cancelled before inference");
  }
  const std::array<int64_t, 4> inputShape{1, 3, static_cast<int64_t>(INPUT_SIZE),
    static_cast<int64_t>(INPUT_SIZE)};
  auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  auto tensor = Ort::Value::CreateTensor<float>(memory,
    const_cast<float*>(input.data()), input.size(), inputShape.data(), inputShape.size());
  const char* inputNames[] = {m_impl->inputName.c_str()};
  const char* outputNames[] = {m_impl->outputName.c_str()};
  Ort::RunOptions runOptions;
  std::atomic<bool> watch{true};
  std::thread watchdog;
  if (stillAuthorized) {
    watchdog = std::thread([&] {
      while (watch.load(std::memory_order_relaxed)) {
        if (!stillAuthorized()) {
          runOptions.SetTerminate();
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
  }
  std::vector<Ort::Value> outputs;
  try {
    outputs = m_impl->session.Run(runOptions, inputNames, &tensor, 1, outputNames, 1);
  }
  catch (...) {
    watch = false;
    if (watchdog.joinable()) watchdog.join();
    throw;
  }
  watch = false;
  if (watchdog.joinable()) watchdog.join();
  if (stillAuthorized && !stillAuthorized()) {
    throw std::runtime_error("person detector cancelled after inference");
  }
  if (outputs.size() != 1 || !outputs.front().IsTensor()) {
    throw std::runtime_error("person detector returned no tensor");
  }
  const auto info = outputs.front().GetTensorTypeAndShapeInfo();
  const auto shape = info.GetShape();
  if (shape.size() != 3 || shape[0] != 1 || shape[1] < 5 || shape[2] <= 0) {
    throw std::runtime_error("person detector returned an unexpected shape");
  }
  const size_t classes = static_cast<size_t>(shape[1]);
  const size_t anchors = static_cast<size_t>(shape[2]);
  const float* data = outputs.front().GetTensorData<float>();
  const size_t scoreOffset = 4 * anchors; // person class is class 0
  if (classes <= PERSON_CLASS) throw std::runtime_error("person class missing");
  std::vector<UavPersonBoxDetection> candidates;
  candidates.reserve(64);
  for (size_t anchor = 0; anchor < anchors; ++anchor) {
    const float score = data[scoreOffset + anchor];
    if (!std::isfinite(score) || score < m_confidenceThreshold) continue;
    const float cx = data[anchor];
    const float cy = data[anchors + anchor];
    const float width = data[2 * anchors + anchor];
    const float height = data[3 * anchors + anchor];
    if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(width) ||
        !std::isfinite(height) || width <= 0 || height <= 0) continue;
    candidates.push_back({{{cx - width / 2, cy - height / 2,
                            cx + width / 2, cy + height / 2}}, score});
  }
  std::sort(candidates.begin(), candidates.end(),
    [] (const auto& lhs, const auto& rhs) { return lhs.confidence > rhs.confidence; });
  std::vector<UavPersonBoxDetection> selected;
  selected.reserve(std::min(candidates.size(), m_maximumDetections));
  for (const auto& candidate : candidates) {
    bool suppressed = false;
    for (const auto& kept : selected) {
      const float x1 = std::max(candidate.box[0], kept.box[0]);
      const float y1 = std::max(candidate.box[1], kept.box[1]);
      const float x2 = std::min(candidate.box[2], kept.box[2]);
      const float y2 = std::min(candidate.box[3], kept.box[3]);
      const float intersection = std::max(0.0F, x2 - x1) * std::max(0.0F, y2 - y1);
      const float areaCandidate = (candidate.box[2] - candidate.box[0]) *
                                  (candidate.box[3] - candidate.box[1]);
      const float areaKept = (kept.box[2] - kept.box[0]) * (kept.box[3] - kept.box[1]);
      const float overlap = intersection / (areaCandidate + areaKept - intersection + 1e-9F);
      if (overlap > m_nmsIou) { suppressed = true; break; }
    }
    if (!suppressed) {
      selected.push_back(candidate);
      if (selected.size() >= m_maximumDetections) break;
    }
  }
  return selected;
}

} // namespace ndnsf::examples::uav
