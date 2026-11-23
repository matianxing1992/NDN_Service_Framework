#include "uav-person-onnx.hpp"
#include "config.hpp"

#include "UavDigest.hpp"
#include "uav-person-json.hpp"

#include <jpeglib.h>
#include <ndn-cxx/util/sha256.hpp>
#include <onnxruntime_cxx_api.h>

#ifdef HAVE_UAV_OPENCV
#include <opencv2/imgproc.hpp>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
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

std::array<int, 4>
integerPersonBox(const std::array<double, 4>& box, uint32_t width, uint32_t height)
{
  if (width == 0 || height == 0 || width > static_cast<uint32_t>(INT_MAX) ||
      height > static_cast<uint32_t>(INT_MAX)) {
    throw std::invalid_argument("invalid person image dimensions");
  }
  std::array<int, 4> result{};
  for (size_t i = 0; i < box.size(); ++i) {
    if (!std::isfinite(box[i])) {
      throw std::invalid_argument("non-finite person box");
    }
    const double value = std::clamp(box[i], 0.0, static_cast<double>(i % 2 ? height : width));
    const double lower = std::floor(value);
    const double fraction = value - lower;
    result[i] = static_cast<int>(lower);
    if (fraction > 0.5 || (fraction == 0.5 && result[i] % 2 != 0)) ++result[i];
  }
  return result;
}

std::vector<float>
reidPersonCrop(const UavRgbImage& image, const std::array<int, 4>& box)
{
  constexpr int width = static_cast<int>(UavPersonReidSession::INPUT_WIDTH);
  constexpr int height = static_cast<int>(UavPersonReidSession::INPUT_HEIGHT);
  constexpr int precision = 22;
  constexpr int64_t rounding = int64_t{1} << (precision - 1);
  constexpr float mean[] = {0.485F, 0.456F, 0.406F};
  constexpr float stdv[] = {0.229F, 0.224F, 0.225F};
  if (image.width == 0 || image.height == 0 ||
      image.width > static_cast<uint32_t>(INT_MAX) ||
      image.height > static_cast<uint32_t>(INT_MAX) ||
      image.rgb.size() != uint64_t{3} * image.width * image.height ||
      box[0] < 0 || box[1] < 0 || box[2] > static_cast<int>(image.width) ||
      box[3] > static_cast<int>(image.height) || box[0] >= box[2] || box[1] >= box[3]) {
    throw std::invalid_argument("invalid admitted ReID crop");
  }
  const int sourceWidth = box[2] - box[0];
  const int sourceHeight = box[3] - box[1];
  if (sourceWidth < 16 || sourceHeight < 32) return {};

  // Match Pillow 10.4's RGB bilinear filter: scale the triangle on reduction,
  // quantize positive coefficients to 22 bits, and round each uint8 pass.
  // Reference: Pillow/10.4.0/src/libImaging/Resample.c.
  struct Weights
  {
    int first;
    std::vector<int32_t> values;
  };
  const auto weights = [] (int source, int target) {
    std::vector<Weights> result;
    result.reserve(target);
    const double scale = static_cast<double>(source) / target;
    const double support = std::max(1.0, scale);
    const double inverseSupport = 1.0 / support;
    for (int destination = 0; destination < target; ++destination) {
      const double center = (destination + 0.5) * scale;
      const int first = std::max(0, static_cast<int>(center - support + 0.5));
      const int last = std::min(source, static_cast<int>(center + support + 0.5));
      std::vector<double> raw;
      raw.reserve(last - first);
      double total = 0;
      for (int i = first; i < last; ++i) {
        const double weight = std::max(0.0, 1.0 - std::abs((i - center + 0.5) * inverseSupport));
        raw.push_back(weight);
        total += weight;
      }
      Weights item{first, {}};
      item.values.reserve(raw.size());
      for (const auto weight : raw) {
        item.values.push_back(static_cast<int32_t>(weight / total * (1 << precision) + 0.5));
      }
      result.push_back(std::move(item));
    }
    return result;
  };
  const auto horizontal = weights(sourceWidth, width);
  const auto vertical = weights(sourceHeight, height);
  std::vector<uint8_t> rows(static_cast<size_t>(sourceHeight) * width * 3);
  for (int y = 0; y < sourceHeight; ++y) {
    for (int x = 0; x < width; ++x) {
      const auto& filter = horizontal[x];
      for (int channel = 0; channel < 3; ++channel) {
        int64_t value = rounding;
        for (size_t i = 0; i < filter.values.size(); ++i) {
          const auto offset = (static_cast<size_t>(box[1] + y) * image.width +
                               box[0] + filter.first + i) * 3 + channel;
          value += static_cast<int64_t>(image.rgb[offset]) * filter.values[i];
        }
        rows[(static_cast<size_t>(y) * width + x) * 3 + channel] =
          static_cast<uint8_t>(std::clamp<int64_t>(value >> precision, 0, 255));
      }
    }
  }
  std::vector<float> result(3 * width * height);
  for (int y = 0; y < height; ++y) {
    const auto& filter = vertical[y];
    for (int x = 0; x < width; ++x) {
      for (int channel = 0; channel < 3; ++channel) {
        int64_t value = rounding;
        for (size_t i = 0; i < filter.values.size(); ++i) {
          const auto offset = ((filter.first + i) * width + x) * 3 + channel;
          value += static_cast<int64_t>(rows[offset]) * filter.values[i];
        }
        const auto pixel = std::clamp<int64_t>(value >> precision, 0, 255);
        const float unit = static_cast<float>(pixel) / 255.0F;
        result[static_cast<size_t>(channel) * width * height + y * width + x] =
          (unit - mean[channel]) / stdv[channel];
      }
    }
  }
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

UavRectLetterboxedImage
letterboxRectangularImage(const UavRgbImage& image)
{
#ifdef HAVE_UAV_OPENCV
  constexpr int bound = 640;
  constexpr int stride = 32;
  if (image.width == 0 || image.height == 0 || image.width > 8192 || image.height > 8192 ||
      image.rgb.size() != static_cast<size_t>(image.width) * image.height * 3) {
    throw std::invalid_argument("invalid rectangular detector RGB image");
  }
  const auto scale = std::min(static_cast<double>(bound) / image.width,
                              static_cast<double>(bound) / image.height);
  const auto width = static_cast<int>(std::nearbyint(image.width * scale));
  const auto height = static_cast<int>(std::nearbyint(image.height * scale));
  if (width <= 0 || height <= 0 || width > bound || height > bound) {
    throw std::invalid_argument("rectangular letterbox dimensions are outside the supported range");
  }
  const int extraWidth = (bound - width) % stride;
  const int extraHeight = (bound - height) % stride;
  const int left = extraWidth / 2;
  const int top = extraHeight / 2;

  // resize reads the RGB source, which remains owned by the admitted frame.
  cv::Mat source(image.height, image.width, CV_8UC3, const_cast<uint8_t*>(image.rgb.data()));
  cv::Mat scaled;
  cv::resize(source, scaled, cv::Size(width, height), 0, 0, cv::INTER_LINEAR);
  cv::Mat padded;
  cv::copyMakeBorder(scaled, padded, top, extraHeight - top, left, extraWidth - left,
                     cv::BORDER_CONSTANT, cv::Scalar(114, 114, 114));

  UavRectLetterboxedImage output;
  output.tensorWidth = padded.cols;
  output.tensorHeight = padded.rows;
  auto& result = output.image;
  result.scale = scale;
  result.padX = left;
  result.padY = top;
  result.sourceWidth = image.width;
  result.sourceHeight = image.height;
  const auto plane = static_cast<size_t>(padded.cols) * padded.rows;
  result.tensor.resize(3 * plane);
  for (int y = 0; y < padded.rows; ++y) {
    const auto* row = padded.ptr<cv::Vec3b>(y);
    for (int x = 0; x < padded.cols; ++x) {
      const auto offset = static_cast<size_t>(y) * padded.cols + x;
      for (size_t channel = 0; channel < 3; ++channel) {
        result.tensor[channel * plane + offset] = row[x][channel] / 255.0F;
      }
    }
  }
  return output;
#else
  static_cast<void>(image);
  throw std::runtime_error("native rectangular preprocessing requires OpenCV core and imgproc");
#endif
}

struct UavPersonDetectorSession::Impl
{
  ModelFormat format;
  Ort::Env env;
  Ort::SessionOptions options;
  Ort::Session session;
  std::string inputName;
  std::string outputName;
  std::vector<std::string> classNames;
  bool endToEnd = false;
  size_t personClass = PERSON_CLASS;

  static Ort::SessionOptions
  makeOptions()
  {
    Ort::SessionOptions value;
    value.SetIntraOpNumThreads(1);
    value.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);
    return value;
  }

  explicit Impl(const std::filesystem::path& model, ModelFormat modelFormat)
    : format(modelFormat)
    , env(ORT_LOGGING_LEVEL_WARNING, format == ModelFormat::VEHICLE_RAW ?
                                    "ndnsf-vehicle-detector" : "ndnsf-person-detector")
    , options(makeOptions())
    , session(env, model.c_str(), options)
  {
    Ort::AllocatorWithDefaultOptions allocator;
    inputName = allocatedString(allocator, session, true);
    outputName = allocatedString(allocator, session, false);
    if (session.GetInputCount() != 1 || session.GetOutputCount() != 1) {
      throw std::invalid_argument("person detector must have one input and one output");
    }
    const auto inputType = session.GetInputTypeInfo(0);
    const auto outputType = session.GetOutputTypeInfo(0);
    const auto inputInfo = inputType.GetTensorTypeAndShapeInfo();
    const auto outputInfo = outputType.GetTensorTypeAndShapeInfo();
    const auto inputShape = inputInfo.GetShape();
    const auto outputShape = outputInfo.GetShape();
    if (format == ModelFormat::PERSON_STOCK && outputShape.size() == 3 &&
        (outputShape[1] == MAX_DETECTIONS || outputShape[1] == -1) &&
        (outputShape[2] == 6 || outputShape[2] == -1)) {
      if (inputShape.size() != 4 || (inputShape[0] != 1 && inputShape[0] != -1) ||
          inputShape[1] != 3 || inputShape[2] != -1 || inputShape[3] != -1 ||
          outputShape.size() != 3 || (outputShape[0] != 1 && outputShape[0] != -1) ||
          (outputShape[1] != MAX_DETECTIONS && outputShape[1] != -1) ||
          (outputShape[2] != 6 && outputShape[2] != -1) ||
          inputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
          outputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        throw std::invalid_argument("person detector requires dynamic input and [1,300,6] output");
      }
      endToEnd = true;
      return;
    }
    if (format == ModelFormat::VEHICLE_RAW || format == ModelFormat::PERSON_STOCK) {
      const auto metadata = session.GetModelMetadata();
      const auto names = metadata.LookupCustomMetadataMapAllocated(
        "ndnsf.uav.class-names.v1", allocator);
      if (!names) {
        throw std::invalid_argument("raw detector is missing the prepared class index");
      }
      const auto index = person_json::parseBounded(names.get(), 64 * 1024);
      if (!index.is_array() || index.empty() || index.size() > 256) {
        throw std::invalid_argument("raw detector class index outside supported bounds");
      }
      for (const auto& item : index) {
        auto name = person_json::text(item);
        if (name.empty() || name.size() > 256 ||
            std::any_of(name.begin(), name.end(),
                        [] (unsigned char ch) { return ch < 0x20 || ch == 0x7f; })) {
          throw std::invalid_argument("raw detector contains an invalid class label");
        }
        classNames.push_back(std::move(name));
      }
      if (format == ModelFormat::PERSON_STOCK) {
        if (std::count(classNames.begin(), classNames.end(), "person") != 1) {
          throw std::invalid_argument("stock person detector needs one person class");
        }
        personClass = static_cast<size_t>(
          std::find(classNames.begin(), classNames.end(), "person") - classNames.begin());
      }
      const auto channels = static_cast<int64_t>(4 + classNames.size());
      if (inputShape.size() != 4 || (inputShape[0] != 1 && inputShape[0] != -1) ||
          inputShape[1] != 3 || inputShape[2] != -1 || inputShape[3] != -1 ||
          outputShape.size() != 3 || (outputShape[0] != 1 && outputShape[0] != -1) ||
          (outputShape[1] != channels &&
           (format == ModelFormat::PERSON_STOCK || outputShape[1] != -1)) ||
          outputShape[2] != -1 ||
          inputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
          outputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        throw std::invalid_argument("raw detector requires a dynamic YOLO head");
      }
      return;
    }
    if (inputShape.size() != 4 || inputShape[0] != 1 || inputShape[1] != 3 ||
        inputShape[2] != static_cast<int64_t>(UavPersonDetectorSession::INPUT_SIZE) ||
        inputShape[3] != static_cast<int64_t>(UavPersonDetectorSession::INPUT_SIZE)) {
      throw std::invalid_argument("person detector input must be [1,3,640,640]");
    }
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
  : UavPersonDetectorSession(model, confidenceThreshold, nmsIou, maximumDetections,
                             ModelFormat::PERSON_RAW)
{
}

UavPersonDetectorSession::UavPersonDetectorSession(const std::filesystem::path& model,
                                                   float confidenceThreshold,
                                                   float nmsIou,
                                                   size_t maximumDetections, ModelFormat format)
  : m_impl(std::make_unique<Impl>(model, format))
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
  return detectPersons(input, INPUT_SIZE, INPUT_SIZE, stillAuthorized);
}

std::vector<UavPersonBoxDetection>
UavPersonDetectorSession::detect(const UavRectLetterboxedImage& input,
                                 const std::function<bool()>& stillAuthorized)
{
  return detectPersons(input.image.tensor, input.tensorWidth, input.tensorHeight, stillAuthorized);
}

std::vector<UavPersonBoxDetection>
UavPersonDetectorSession::detectPersons(const std::vector<float>& input, size_t width, size_t height,
                                        const std::function<bool()>& stillAuthorized)
{
  const auto boxes = detectBoxes(input, width, height, stillAuthorized);
  std::vector<UavPersonBoxDetection> result;
  result.reserve(boxes.size());
  for (const auto& box : boxes) {
    result.push_back({box.box, box.confidence});
  }
  return result;
}

std::vector<UavVehicleBoxDetection>
UavPersonDetectorSession::detectBoxes(const std::vector<float>& input, size_t width, size_t height,
                                      const std::function<bool()>& stillAuthorized)
{
  const bool vehicle = m_impl->format == ModelFormat::VEHICLE_RAW;
  const bool stock = m_impl->format == ModelFormat::PERSON_STOCK;
  const bool endToEnd = m_impl->endToEnd;
  const bool dynamicRaw = vehicle || (stock && !endToEnd);
  if (width == 0 || width > INPUT_SIZE || width % 32 != 0 ||
      height == 0 || height > INPUT_SIZE || height % 32 != 0 ||
      (!vehicle && !stock && (width != INPUT_SIZE || height != INPUT_SIZE)) ||
      input.size() != 3 * width * height ||
      !std::all_of(input.begin(), input.end(), [] (float value) { return std::isfinite(value); })) {
    throw std::invalid_argument("detector input must match the admitted CHW geometry");
  }
  if (stillAuthorized && !stillAuthorized()) {
    throw std::runtime_error(vehicle ? "vehicle detector cancelled before inference" :
                                              "person detector cancelled before inference");
  }
  const std::array<int64_t, 4> inputShape{1, 3, static_cast<int64_t>(height),
    static_cast<int64_t>(width)};
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
    throw std::runtime_error(vehicle ? "vehicle detector cancelled after inference" :
                                              "person detector cancelled after inference");
  }
  if (outputs.size() != 1 || !outputs.front().IsTensor()) {
    throw std::runtime_error("person detector returned no tensor");
  }
  const auto info = outputs.front().GetTensorTypeAndShapeInfo();
  const auto shape = info.GetShape();
  if (endToEnd) {
    if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
        shape != std::vector<int64_t>{1, MAX_DETECTIONS, 6}) {
      throw std::runtime_error("end-to-end person detector returned an unexpected tensor");
    }
    const float* data = outputs.front().GetTensorData<float>();
    std::vector<UavVehicleBoxDetection> selected;
    selected.reserve(m_maximumDetections);
    for (size_t index = 0; index < MAX_DETECTIONS; ++index) {
      const auto* row = data + 6 * index;
      if (!std::all_of(row, row + 6, [] (float value) { return std::isfinite(value); })) {
        throw std::runtime_error("end-to-end person detector returned non-finite values");
      }
      if (row[4] <= m_confidenceThreshold || row[5] != PERSON_CLASS) continue;
      if (row[2] <= row[0] || row[3] <= row[1]) continue;
      selected.push_back({{{row[0], row[1], row[2], row[3]}}, row[4], PERSON_CLASS});
      if (selected.size() == m_maximumDetections) break;
    }
    return selected;
  }
  if (shape.size() != 3 || shape[0] != 1 || shape[1] < 5 || shape[2] <= 0) {
    throw std::runtime_error("person detector returned an unexpected shape");
  }
  if (dynamicRaw) {
    const auto expectedAnchors = (width / 8) * (height / 8) +
                                 (width / 16) * (height / 16) +
                                 (width / 32) * (height / 32);
    if (static_cast<size_t>(shape[1]) != 4 + m_impl->classNames.size() ||
        static_cast<size_t>(shape[2]) != expectedAnchors) {
      throw std::runtime_error("raw detector returned an unexpected YOLO head");
    }
  }
  const size_t classCount = static_cast<size_t>(shape[1]) - 4;
  const size_t anchors = static_cast<size_t>(shape[2]);
  const float* data = outputs.front().GetTensorData<float>();
  std::vector<UavVehicleBoxDetection> candidates;
  candidates.reserve(64);
  for (size_t anchor = 0; anchor < anchors; ++anchor) {
    size_t classId = PERSON_CLASS;
    float score = data[4 * anchors + anchor];
    if (dynamicRaw) {
      for (size_t id = 1; id < classCount; ++id) {
        const auto candidateScore = data[(4 + id) * anchors + anchor];
        if (candidateScore > score) {
          score = candidateScore;
          classId = id;
        }
      }
    }
    if (!std::isfinite(score) || score < m_confidenceThreshold ||
        (dynamicRaw && score == m_confidenceThreshold) ||
        (stock && classId != m_impl->personClass)) continue;
    const float cx = data[anchor];
    const float cy = data[anchors + anchor];
    const float width = data[2 * anchors + anchor];
    const float height = data[3 * anchors + anchor];
    if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(width) ||
        !std::isfinite(height) || width <= 0 || height <= 0) continue;
    candidates.push_back({{{cx - width / 2, cy - height / 2,
                            cx + width / 2, cy + height / 2}}, score, classId});
  }
  const auto descendingScore = [] (const auto& lhs, const auto& rhs) {
    return lhs.confidence > rhs.confidence;
  };
  if (dynamicRaw) {
    std::stable_sort(candidates.begin(), candidates.end(), descendingScore);
  }
  else {
    std::sort(candidates.begin(), candidates.end(), descendingScore);
  }
  std::vector<UavVehicleBoxDetection> selected;
  selected.reserve(std::min(candidates.size(), m_maximumDetections));
  for (const auto& candidate : candidates) {
    bool suppressed = false;
    for (const auto& kept : selected) {
      if (candidate.classId != kept.classId) continue;
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

UavVehicleDetectorSession::UavVehicleDetectorSession(const std::filesystem::path& model,
                                                     float confidenceThreshold)
  : m_detector(model, confidenceThreshold, 0.5F, UavPersonDetectorSession::MAX_DETECTIONS,
               UavPersonDetectorSession::ModelFormat::VEHICLE_RAW)
{
}

std::vector<UavVehicleBoxDetection>
UavVehicleDetectorSession::detect(const UavRectLetterboxedImage& input,
                                  const std::function<bool()>& stillAuthorized)
{
  return m_detector.detectBoxes(input.image.tensor, input.tensorWidth, input.tensorHeight,
                                stillAuthorized);
}

const std::vector<std::string>&
UavVehicleDetectorSession::modelClassNames() const noexcept
{
  return m_detector.m_impl->classNames;
}

} // namespace ndnsf::examples::uav
