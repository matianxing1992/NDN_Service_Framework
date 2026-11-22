#include "wan-generation.hpp"

#include "wan-unipc.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "ndn-service-framework/RequestConfidentiality.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <stdexcept>

namespace ndnsf::di::wan {
namespace {

constexpr std::size_t TEXT_TOKEN_LIMIT = 512;
constexpr std::size_t CHECK_INTERVAL = 64 * 1024;
constexpr std::chrono::milliseconds WAIT_SLICE{50};

void
requireFinite(const NamedTensorView& tensor, const std::function<void()>& current)
{
  for (std::size_t offset = 0; offset < tensor.payload.size(); offset += sizeof(float)) {
    if (offset % (CHECK_INTERVAL * sizeof(float)) == 0) current();
    float value;
    std::memcpy(&value, tensor.payload.data() + offset, sizeof(value));
    if (!std::isfinite(value))
      throw std::invalid_argument("Wan child returned a non-finite value: " + tensor.name);
  }
}

std::size_t
elementCount(ndn::span<const std::int64_t> shape)
{
  std::size_t count = 1;
  for (const auto extent : shape) {
    if (extent <= 0 || static_cast<std::uint64_t>(extent) >
                         std::numeric_limits<std::size_t>::max() / count)
      throw std::invalid_argument("Wan generation tensor extent is invalid");
    count *= static_cast<std::size_t>(extent);
  }
  return count;
}

template<typename T>
ndn::span<const std::uint8_t>
bytesOf(ndn::span<const T> values)
{
  return {reinterpret_cast<const std::uint8_t*>(values.data()), values.size_bytes()};
}

NamedTensorView
singleFloatOutput(const Result& result, const std::string& name,
                  const std::vector<std::int64_t>& shape)
{
  auto tensors = decodeTensorBundleViews(result.payload);
  if (tensors.size() != 1 || tensors.front().name != name ||
      tensors.front().elementType != TensorElementType::Float32 || tensors.front().shape != shape)
    throw std::invalid_argument("Wan child result does not match its output port: " + name);
  return std::move(tensors.front());
}

std::vector<float>
initialNoise(std::size_t count, std::uint64_t seed, const std::function<void()>& current)
{
  std::mt19937_64 random(seed);
  const auto uniform = [&] {
    return static_cast<double>((random() >> 11) + 1) * 0x1p-53;
  };
  constexpr double twoPi = 6.2831853071795864769;
  std::vector<float> noise(count);
  for (std::size_t i = 0; i < count; i += 2) {
    if (i % CHECK_INTERVAL == 0) current();
    const auto radius = std::sqrt(-2 * std::log(uniform()));
    const auto angle = twoPi * uniform();
    noise[i] = static_cast<float>(radius * std::cos(angle));
    if (i + 1 < count) noise[i + 1] = static_cast<float>(radius * std::sin(angle));
  }
  return noise;
}

void
validatePrompt(const WanPromptTokens& prompt)
{
  if (prompt.validTokens == 0 || prompt.validTokens > prompt.ids.size() ||
      prompt.ids.size() > TEXT_TOKEN_LIMIT ||
      std::any_of(prompt.ids.begin(), prompt.ids.end(), [] (auto id) { return id < 0; }))
    throw std::invalid_argument("Wan generation requires valid padded UMT5 token IDs");
}

std::vector<std::uint8_t>
encodePrompt(const WanPromptTokens& prompt)
{
  const std::vector<std::int64_t> shape{1, static_cast<std::int64_t>(prompt.ids.size())};
  std::vector<std::uint8_t> mask(prompt.ids.size(), 0);
  std::fill_n(mask.begin(), prompt.validTokens, 1);
  return encodeTensorBundleViews({
    {"token-ids", TensorElementType::Int64, shape, bytesOf<std::int64_t>(prompt.ids)},
    {"key-mask", TensorElementType::Bool, shape, mask}});
}

} // namespace

WanClipOutput
planWanClipOutput(const std::array<std::int64_t, 4>& latentShape)
{
  return planWanClipOutput(latentShape, 8);
}

WanClipOutput
planWanClipOutput(const std::array<std::int64_t, 4>& latentShape,
                  std::int64_t vaeSpatialStride)
{
  const auto [channels, frames, height, width] = latentShape;
  constexpr auto limit = std::numeric_limits<std::int64_t>::max();
  if ((vaeSpatialStride != 8 && vaeSpatialStride != 16) ||
      channels <= 0 || frames <= 0 || height <= 0 || width <= 0 ||
      frames > limit / 4 || height > limit / vaeSpatialStride || width > limit / vaeSpatialStride)
    throw std::invalid_argument("Wan generation RGB shape is invalid");
  const std::array<std::int64_t, 5> shape{1, 3, 4 * (frames - 1) + 1,
    vaeSpatialStride * height, vaeSpatialStride * width};
  const auto cells = elementCount(shape);
  if (cells > std::numeric_limits<std::size_t>::max() / sizeof(float))
    throw std::invalid_argument("Wan generation RGB byte count overflows");
  const auto responseBytes = tensorBundleEncodedSize({{3, shape.size(), cells * sizeof(float)}});
  if (responseBytes > ndn_service_framework::MAX_REQUEST_RESPONSE_BYTES)
    throw std::invalid_argument("Wan VAE result exceeds the Core response byte bound");
  return {{shape[2], shape[3], shape[4]}, cells, responseBytes};
}

std::uint64_t
planWanGenerationTraceBytes(const WanGenerationOptions& options,
                            std::size_t positiveTokens, std::size_t negativeTokens)
{
  if (options.traceMaxBytes == 0 || options.traceMaxBytes > WAN_TRACE_HARD_MAX_BYTES ||
      options.textWidth <= 0 || options.steps == 0 || positiveTokens == 0 || negativeTokens == 0 ||
      positiveTokens > TEXT_TOKEN_LIMIT || negativeTokens > TEXT_TOKEN_LIMIT)
    throw std::invalid_argument("Wan fixture trace configuration is outside its bound");
  auto remaining = options.traceMaxBytes;
  const auto charge = [&] (std::uint64_t bytes, std::size_t count = 1) {
    if (bytes == 0 || count > remaining / bytes)
      throw std::invalid_argument("Wan fixture trace exceeds its byte budget");
    remaining -= bytes * count;
  };
  charge(planWanClipOutput(options.latentShape, options.vaeSpatialStride).responseBytes);
  for (const auto valid : {positiveTokens, negativeTokens}) {
    const auto cells = elementCount(std::array<std::int64_t, 2>{
      static_cast<std::int64_t>(valid), options.textWidth});
    if (cells > remaining / sizeof(float))
      throw std::invalid_argument("Wan fixture context exceeds its byte budget");
    charge(tensorBundleEncodedSize({{12, 3, cells * sizeof(float)}}));
  }
  const auto latentCells = elementCount(options.latentShape);
  if (latentCells > remaining / sizeof(float))
    throw std::invalid_argument("Wan fixture latent exceeds its byte budget");
  // Prediction has the longest name among the three observed tensors per step.
  const auto latentRecord = tensorBundleEncodedSize({{10, 5, latentCells * sizeof(float)}});
  for (unsigned record = 0; record < 3; ++record)
    charge(latentRecord, options.steps);
  return options.traceMaxBytes - remaining;
}

void
writeWanRgb24(ndn::span<const std::uint8_t> planarRgb, const WanRgb24Sink& sink,
             const std::function<void()>& requireActive)
{
  if (planarRgb.empty() || planarRgb.size() % (3 * sizeof(float)) != 0 || !sink)
    throw std::invalid_argument("Wan RGB24 conversion requires three FP32 planes and a sink");
  const auto pixels = planarRgb.size() / (3 * sizeof(float));
  std::vector<std::uint8_t> chunk(3 * std::min(pixels, CHECK_INTERVAL));
  for (std::size_t begin = 0; begin < pixels; begin += CHECK_INTERVAL) {
    requireActive();
    const auto count = std::min(CHECK_INTERVAL, pixels - begin);
    for (std::size_t i = 0; i < count; ++i) {
      for (std::size_t channel = 0; channel < 3; ++channel) {
        float value;
        std::memcpy(&value, planarRgb.data() + (channel * pixels + begin + i) * sizeof(float),
                    sizeof(value));
        if (!std::isfinite(value))
          throw std::invalid_argument("Wan VAE returned a non-finite RGB channel");
        const float normalized = (std::clamp(value, -1.0f, 1.0f) + 1.0f) * 0.5f;
        chunk[3 * i + channel] = static_cast<std::uint8_t>(normalized * 255.0f);
      }
    }
    sink({chunk.data(), 3 * count});
  }
  requireActive();
}

WanGenerationResult
runWanGeneration(const User& user, const PreparedModel& textEncoder,
                 const PreparedModel& denoiser, const PreparedModel& decoder,
                 const WanPromptTokens& positive, const WanPromptTokens& negative,
                 const WanGenerationOptions& options,
                 const WanRgb24Sink& sink,
                 const std::function<void()>& requireActive,
                 const WanTensorObserver& observe)
{
  const auto current = [&] {
    requireActive();
    if (options.deadline <= std::chrono::steady_clock::now())
      throw DiError("WAN_GENERATION_TIMEOUT", "local", "wan-generation",
                    "generation deadline expired");
  };
  current();
  validatePrompt(positive);
  validatePrompt(negative);
  const auto [channels, frames, height, width] = options.latentShape;
  const std::vector<std::int64_t> latentShape{1, channels, frames, height, width};
  const auto latentCells = elementCount(latentShape);
  if (height % 2 != 0 || width % 2 != 0 || options.textWidth <= 0 ||
      options.inlineLimit == 0 || options.generationId.empty() ||
      options.generationId.size() > 96 || options.generationId.find('\0') != std::string::npos ||
      positive.ids.size() != negative.ids.size() ||
      !options.childRequest.placement || options.childRequest.stream ||
      options.childRequest.generation || options.childRequest.outputMode != "FULL" ||
      !std::isfinite(options.guidance) || !sink)
    throw std::invalid_argument("invalid Wan generation geometry, identity or request policy");
  const std::array<std::int64_t, 3> videoGrid{frames, height / 2, width / 2};
  const auto videoTokens = elementCount(videoGrid);
  if (options.paddedVideoTokens <= 0 ||
      static_cast<std::uint64_t>(options.paddedVideoTokens) < videoTokens)
    throw std::invalid_argument("Wan generation video token padding is too small");
  if (latentCells > std::numeric_limits<std::size_t>::max() / sizeof(float))
    throw std::invalid_argument("Wan generation geometry overflows its outputs");
  const auto clipOutput = planWanClipOutput(options.latentShape, options.vaeSpatialStride);
  if (clipOutput.responseBytes > decoder.maxPayloadBytes())
    throw std::invalid_argument("Wan VAE result exceeds the verified decoder adapter byte bound");
  (void)tensorBundleEncodedSize({{6, latentShape.size(), latentCells * sizeof(float)},
                                {11, 4, static_cast<std::uint64_t>(options.paddedVideoTokens)}});
  if (observe)
    (void)planWanGenerationTraceBytes(options, positive.validTokens, negative.validTokens);

  auto noise = initialNoise(latentCells, options.seed, current);
  const auto initialBytes = bytesOf<float>(noise);
  WanGenerationResult result;
  result.initialLatentDigest = nativePlanningDigest(initialBytes.data(), initialBytes.size());
  result.framesHeightWidth = clipOutput.framesHeightWidth;
  result.rgb24Bytes = clipOutput.rgb24Bytes;
  WanUniPc sampler(options.steps, options.shift, std::move(noise));

  auto submit = [&](const PreparedModel& model, const std::string& label,
                    std::vector<std::uint8_t> payload) {
    current();
    const auto inputDigest = nativePlanningDigest(payload.data(), payload.size());
    const auto identity = options.generationId + "/" + label + "/" + inputDigest.substr(7);
    auto request = options.childRequest;
    request.maxInlineInputBytes = options.inlineLimit;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      options.deadline - std::chrono::steady_clock::now());
    request.timeout = std::min(request.timeout, remaining);
    if (request.timeout <= request.ackTimeout)
      throw DiError("WAN_GENERATION_TIMEOUT", "local", "wan-generation",
                    "remaining generation deadline cannot cover child admission");
    request.applicationRequestId = identity;
    const auto modelIntent = model.modelIntentDigest();
    auto handle = user.request(model, Input::inlineBytes(std::move(payload)), request);
    try {
      const auto id = handle.id();
      for (;;) {
        current();
        try {
          auto child = handle.result(WAIT_SLICE);
          current();
          if (id.empty() || child.requestId != id ||
              child.modelDigest != modelIntent || child.planDigest.empty())
            throw DiError("WAN_CHILD_RESULT_BINDING", "local", "wan-result",
                          "child result does not match its request and model");
          result.children.push_back({label, child.requestId, child.planDigest, inputDigest});
          return child;
        }
        catch (const DiError& error) {
          if (error.code() != "LOCAL_WAIT_TIMEOUT" && error.code() != "WAIT_TIMEOUT")
            throw;
        }
      }
    }
    catch (...) {
      if (handle.status() == RequestStatus::Pending) handle.cancel();
      throw;
    }
  };

  const auto textShape = [&] (const WanPromptTokens& prompt) {
    return std::vector<std::int64_t>{1, static_cast<std::int64_t>(prompt.validTokens),
                                     options.textWidth};
  };
  const auto observeChild = [&] (const NamedTensorView& value) {
    if (observe) {
      const auto& receipt = result.children.back();
      observe(receipt.label, value, &receipt, {});
    }
  };
  const auto positiveText = submit(textEncoder, "text/positive", encodePrompt(positive));
  const auto positiveContext = singleFloatOutput(positiveText, "text-context", textShape(positive));
  requireFinite(positiveContext, current);
  observeChild(positiveContext);
  const auto negativeText = submit(textEncoder, "text/negative", encodePrompt(negative));
  const auto negativeContext = singleFloatOutput(negativeText, "text-context", textShape(negative));
  requireFinite(negativeContext, current);
  observeChild(negativeContext);
  std::vector<std::uint8_t> videoMask(options.paddedVideoTokens, 0);
  std::fill_n(videoMask.begin(), videoTokens, 1);
  while (!sampler.finished()) {
    const auto step = sampler.stepIndex();
    const std::array<std::int64_t, 1> timestep{sampler.timestep()};
    const auto predict = [&] (const NamedTensorView& context, const std::string& branch) {
      current();
      auto prediction = submit(denoiser, "denoise/" + std::to_string(step) + "/" + branch,
        encodeTensorBundleViews({
          {"latent", TensorElementType::Float32, latentShape, bytesOf<float>(sampler.latent())},
          {"context", TensorElementType::Float32, context.shape, context.payload},
          {"timestep", TensorElementType::Int64, {1}, bytesOf<std::int64_t>(timestep)},
          {"video-valid", TensorElementType::Bool, {1, 1, 1, options.paddedVideoTokens}, videoMask}}));
      const auto tensor = singleFloatOutput(prediction, "prediction", latentShape);
      requireFinite(tensor, current);
      observeChild(tensor);
      std::vector<float> values(latentCells);
      std::memcpy(values.data(), tensor.payload.data(), tensor.payload.size());
      return values;
    };
    const auto conditioned = predict(positiveContext, "positive");
    const auto unconditioned = predict(negativeContext, "negative");
    const auto order = sampler.advance(step, conditioned, unconditioned, options.guidance, current);
    if (observe) {
      observe("latent/" + std::to_string(step),
              {"latent", TensorElementType::Float32, latentShape, bytesOf<float>(sampler.latent())},
              nullptr, WanSamplingReceipt{timestep.front(), order.predictorOrder,
                                          order.correctorOrder});
    }
  }

  const auto clip = submit(decoder, "decode", encodeTensorBundleViews({
    {"latent-clip", TensorElementType::Float32, latentShape, bytesOf<float>(sampler.latent())}}));
  const auto [rgbFrames, rgbHeight, rgbWidth] = clipOutput.framesHeightWidth;
  const auto rgb = singleFloatOutput(clip, "rgb", {1, 3, rgbFrames, rgbHeight, rgbWidth});
  if (observe) {
    requireFinite(rgb, current);
    observeChild(rgb);
  }
  writeWanRgb24(rgb.payload, sink, current);
  return result;
}

} // namespace ndnsf::di::wan
