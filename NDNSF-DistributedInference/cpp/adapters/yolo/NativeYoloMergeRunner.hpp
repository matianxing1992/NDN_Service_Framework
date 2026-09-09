#ifndef NDNSF_DI_ADAPTERS_YOLO_NATIVE_MERGE_RUNNER_HPP
#define NDNSF_DI_ADAPTERS_YOLO_NATIVE_MERGE_RUNNER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <cstddef>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <stdexcept>
#include <vector>

namespace ndnsf::di {

/** Build the request-scoped native consumer spec for a declared YOLO Merge. */
NativeModelRunnerSpec
nativeYoloMergeRunnerSpecFromProjection(const NativeSelectionProjectionV3& projection);

/** Construct the CPU-only YOLO postprocessing runner. */
std::shared_ptr<NativeModelRunner>
makeNativeYoloMergeRunner(const NativeModelRunnerSpec& spec);

/**
 * Apply the Spec180 canonical detection-row contract to one ONNX output.
 *
 * This is used by the atomic FullModel path after real ONNX execution. The
 * dependency-only Merge runner has a different input contract, but both
 * terminal paths must publish the same filtered and stably ordered rows.
 */
inline NamedTensor
nativeYoloCanonicalizePredictions(const NamedTensor& input,
                                  std::string outputName,
                                  double confidenceThreshold,
                                  std::size_t maxRows)
{
  if (input.elementType != TensorElementType::Float32 ||
      input.shape.size() != 3 || input.shape[0] != 1 || input.shape[2] != 6 ||
      input.shape[1] < 0 || !std::isfinite(confidenceThreshold) ||
      confidenceThreshold < 0.0 || confidenceThreshold > 1.0 || maxRows == 0) {
    throw std::invalid_argument("YOLO prediction postprocessing contract mismatch");
  }
  const auto rowCount = static_cast<std::size_t>(input.shape[1]);
  if (rowCount > std::numeric_limits<std::size_t>::max() / 6 ||
      input.payload.size() != rowCount * 6 * sizeof(float)) {
    throw std::invalid_argument(
      "YOLO prediction tensor payload size does not match [1,N,6]");
  }
  struct Detection {
    float x1, y1, x2, y2, confidence;
    std::size_t classId, ordinal;
  };
  auto read = [&input] (std::size_t index) {
    float value = 0.0f;
    std::memcpy(&value, input.payload.data() + index * sizeof(float), sizeof(float));
    if (!std::isfinite(value)) {
      throw std::invalid_argument("YOLO prediction contains a non-finite value");
    }
    return value;
  };
  std::vector<Detection> detections;
  detections.reserve(std::min(rowCount, maxRows));
  for (std::size_t row = 0; row < rowCount; ++row) {
    const auto offset = row * 6;
    const auto confidence = read(offset + 4);
    const auto classValue = read(offset + 5);
    if (confidence < 0.0f || confidence > 1.0f || classValue < 0.0f ||
        classValue >= 80.0f || std::floor(classValue) != classValue) {
      throw std::invalid_argument("YOLO prediction row is outside canonical bounds");
    }
    if (confidence >= static_cast<float>(confidenceThreshold)) {
      detections.push_back({read(offset), read(offset + 1), read(offset + 2),
                            read(offset + 3), confidence,
                            static_cast<std::size_t>(classValue), row});
    }
  }
  std::sort(detections.begin(), detections.end(), [] (const Detection& left,
                                                       const Detection& right) {
    if (left.confidence != right.confidence) return left.confidence > right.confidence;
    if (left.classId != right.classId) return left.classId < right.classId;
    if (left.x1 != right.x1) return left.x1 < right.x1;
    if (left.y1 != right.y1) return left.y1 < right.y1;
    if (left.x2 != right.x2) return left.x2 < right.x2;
    if (left.y2 != right.y2) return left.y2 < right.y2;
    return left.ordinal < right.ordinal;
  });
  if (detections.size() > maxRows) detections.resize(maxRows);
  std::vector<float> values;
  values.reserve(detections.size() * 6);
  for (const auto& detection : detections) {
    values.insert(values.end(), {detection.x1, detection.y1, detection.x2,
                                 detection.y2, detection.confidence,
                                 static_cast<float>(detection.classId)});
  }
  std::vector<std::uint8_t> payload(values.size() * sizeof(float));
  if (!payload.empty()) std::memcpy(payload.data(), values.data(), payload.size());
  return makeFloat32Tensor(outputName.empty() ? input.name : outputName,
                           {1, static_cast<std::int64_t>(detections.size()), 6},
                           payload);
}

} // namespace ndnsf::di

#endif
