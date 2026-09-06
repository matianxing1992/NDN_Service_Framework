#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeYoloMergeRunner.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ndnsf::di {
namespace {

const std::vector<std::string>&
canonicalInputNames()
{
  static const std::vector<std::string> names{
    "/model/model.23/one2one_cv2.0/one2one_cv2.0.2/Conv_output_0",
    "/model/model.23/one2one_cv2.1/one2one_cv2.1.2/Conv_output_0",
    "/model/model.23/one2one_cv2.2/one2one_cv2.2.2/Conv_output_0",
    "/model/model.23/one2one_cv3.0/one2one_cv3.0.2/Conv_output_0",
    "/model/model.23/one2one_cv3.1/one2one_cv3.1.2/Conv_output_0",
    "/model/model.23/one2one_cv3.2/one2one_cv3.2.2/Conv_output_0",
  };
  return names;
}

struct ScaleInputs
{
  const NamedTensor* boxes = nullptr;
  const NamedTensor* scores = nullptr;
  std::size_t grid = 0;
  float stride = 0.0f;
};

std::string
metadataValue(const NativeModelRunnerSpec& spec, const std::string& key)
{
  const auto found = spec.metadata.find(key);
  return found == spec.metadata.end() ? std::string() : found->second;
}

std::vector<std::int64_t>
parseShape(const std::string& value)
{
  if (value.empty() || value.back() == ',') {
    throw std::invalid_argument("YOLO Merge output shape is invalid");
  }
  std::vector<std::int64_t> shape;
  std::size_t start = 0;
  while (start < value.size()) {
    const auto end = value.find(',', start);
    const auto part = value.substr(start, end == std::string::npos
                                            ? std::string::npos : end - start);
    if (part.empty()) {
      throw std::invalid_argument("YOLO Merge output shape is invalid");
    }
    std::size_t consumed = 0;
    const auto dimension = std::stoll(part, &consumed);
    if (consumed != part.size() || dimension <= 0) {
      throw std::invalid_argument("YOLO Merge output shape is invalid");
    }
    shape.push_back(dimension);
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return shape;
}

float
readFloat(const NamedTensor& tensor, std::size_t index)
{
  float value = 0.0f;
  std::memcpy(&value, tensor.payload.data() + index * sizeof(float), sizeof(float));
  if (!std::isfinite(value)) {
    throw std::invalid_argument("YOLO Merge input contains a non-finite value");
  }
  return value;
}

float
sigmoid(float value)
{
  if (value >= 0.0f) {
    return 1.0f / (1.0f + std::exp(-value));
  }
  const auto exponential = std::exp(value);
  return exponential / (1.0f + exponential);
}

struct Detection
{
  float x1 = 0.0f;
  float y1 = 0.0f;
  float x2 = 0.0f;
  float y2 = 0.0f;
  float confidence = 0.0f;
  std::size_t classId = 0;
  std::size_t ordinal = 0;
};

bool
canonicalOrder(const Detection& left, const Detection& right)
{
  if (left.confidence != right.confidence) {
    return left.confidence > right.confidence;
  }
  if (left.classId != right.classId) {
    return left.classId < right.classId;
  }
  if (left.x1 != right.x1) return left.x1 < right.x1;
  if (left.y1 != right.y1) return left.y1 < right.y1;
  if (left.x2 != right.x2) return left.x2 < right.x2;
  if (left.y2 != right.y2) return left.y2 < right.y2;
  return left.ordinal < right.ordinal;
}

class NativeYoloMergeRunner final : public NativeModelRunner
{
public:
  explicit NativeYoloMergeRunner(NativeModelRunnerSpec spec)
    : m_spec(std::move(spec))
  {
    if (m_spec.kind != "native-yolo-postprocess" ||
        m_spec.backend != "native-yolo-postprocess" || !m_spec.path.empty()) {
      throw std::invalid_argument("YOLO Merge runner must be native and pathless");
    }
    if (metadataValue(m_spec, "mergeKind") != "NATIVE_POSTPROCESS" ||
        metadataValue(m_spec, "postprocessIdentity") != "YOLO26n-canonical-detection-rows" ||
        metadataValue(m_spec, "postprocessOutputName").empty() ||
        metadataValue(m_spec, "postprocessSort") !=
          "confidence-desc,class-asc,xyxy-asc") {
      throw std::invalid_argument("YOLO Merge postprocessing contract is incomplete");
    }
    m_outputName = metadataValue(m_spec, "postprocessOutputName");
    m_outputShape = parseShape(metadataValue(m_spec, "expectedOutputShape"));
    if (m_outputShape.size() != 3 || m_outputShape[0] != 1 ||
        m_outputShape[2] != 6) {
      throw std::invalid_argument("YOLO Merge output contract must be [1,K,6]");
    }
    const auto threshold = metadataValue(m_spec, "postprocessConfidenceThreshold");
    std::size_t consumed = 0;
    m_confidenceThreshold = std::stod(threshold, &consumed);
    if (consumed != threshold.size() || !std::isfinite(m_confidenceThreshold) ||
        m_confidenceThreshold < 0.0 || m_confidenceThreshold > 1.0) {
      throw std::invalid_argument("YOLO Merge confidence threshold is invalid");
    }
    m_evidence = executionEvidenceFromRunnerSpec(
      m_spec, RunnerKind::NativeYoloPostprocess,
      "native-yolo-postprocess-v1", "cpu", "cpu");
  }

  std::map<std::string, TensorBundle>
  run(const RoleExecutionContext& ctx) final
  {
    std::map<std::string, NamedTensor> tensors;
    if (ctx.inputEdgesByScope.size() != canonicalInputNames().size()) {
      throw std::invalid_argument("YOLO Merge requires exactly six dependency inputs");
    }
    for (const auto& item : ctx.inputEdgesByScope) {
      const auto input = ctx.inputsByScope.find(item.first);
      if (input == ctx.inputsByScope.end() ||
          !isEncodedTensorBundle(input->second.payload)) {
        throw std::invalid_argument("YOLO Merge dependency is not an encoded tensor bundle");
      }
      const auto decoded = decodeTensorBundle(input->second.payload);
      if (decoded.size() != 1 || item.second.tensors.size() != 1 ||
          decoded.front().name != item.second.tensors.front()) {
        throw std::invalid_argument("YOLO Merge dependency tensor selection is ambiguous");
      }
      if (!tensors.emplace(decoded.front().name, decoded.front()).second) {
        throw std::invalid_argument("YOLO Merge dependency tensor is duplicated");
      }
    }
    if (tensors.size() != canonicalInputNames().size()) {
      throw std::invalid_argument("YOLO Merge dependency tensor set is incomplete");
    }
    for (const auto& name : canonicalInputNames()) {
      if (tensors.find(name) == tensors.end()) {
        throw std::invalid_argument("YOLO Merge dependency tensor is not canonical: " + name);
      }
    }

    const std::vector<std::pair<std::string, std::string>> scaleNames{
      {canonicalInputNames()[0], canonicalInputNames()[3]},
      {canonicalInputNames()[1], canonicalInputNames()[4]},
      {canonicalInputNames()[2], canonicalInputNames()[5]},
    };
    const std::array<std::size_t, 3> grids{80, 40, 20};
    const std::array<float, 3> strides{8.0f, 16.0f, 32.0f};
    std::vector<Detection> detections;
    std::size_t ordinal = 0;
    for (std::size_t scale = 0; scale < scaleNames.size(); ++scale) {
      const auto& boxes = tensors.at(scaleNames[scale].first);
      const auto& scores = tensors.at(scaleNames[scale].second);
      if (boxes.elementType != TensorElementType::Float32 ||
          scores.elementType != TensorElementType::Float32 ||
          boxes.shape != std::vector<std::int64_t>{1, 4,
            static_cast<std::int64_t>(grids[scale]),
            static_cast<std::int64_t>(grids[scale])} ||
          scores.shape != std::vector<std::int64_t>{1, 80,
            static_cast<std::int64_t>(grids[scale]),
            static_cast<std::int64_t>(grids[scale])}) {
        throw std::invalid_argument("YOLO Merge dependency shape/dtype contract mismatch");
      }
      for (std::size_t y = 0; y < grids[scale]; ++y) {
        for (std::size_t x = 0; x < grids[scale]; ++x) {
          const auto cell = y * grids[scale] + x;
          float maxConfidence = 0.0f;
          std::size_t bestClass = 0;
          std::vector<float> confidences(80);
          for (std::size_t classId = 0; classId < 80; ++classId) {
            const auto value = sigmoid(readFloat(
              scores, classId * grids[scale] * grids[scale] + cell));
            confidences[classId] = value;
            if (value > maxConfidence) {
              maxConfidence = value;
              bestClass = classId;
            }
          }
          if (maxConfidence < m_confidenceThreshold) {
            continue;
          }
          const auto left = readFloat(boxes, cell);
          const auto top = readFloat(boxes, grids[scale] * grids[scale] + cell);
          const auto right = readFloat(boxes, 2 * grids[scale] * grids[scale] + cell);
          const auto bottom = readFloat(boxes, 3 * grids[scale] * grids[scale] + cell);
          const auto centerX = (static_cast<float>(x) + 0.5f) * strides[scale];
          const auto centerY = (static_cast<float>(y) + 0.5f) * strides[scale];
          const auto x1 = centerX - left * strides[scale];
          const auto y1 = centerY - top * strides[scale];
          const auto x2 = centerX + right * strides[scale];
          const auto y2 = centerY + bottom * strides[scale];
          if (!std::isfinite(x1) || !std::isfinite(y1) ||
              !std::isfinite(x2) || !std::isfinite(y2)) {
            throw std::invalid_argument("YOLO Merge decoded box is non-finite");
          }
          (void)bestClass; // maxConfidence only bounds the candidate set.
          for (std::size_t classId = 0; classId < confidences.size(); ++classId) {
            if (confidences[classId] >= m_confidenceThreshold) {
              detections.push_back({x1, y1, x2, y2, confidences[classId],
                                    classId, ordinal++});
            }
          }
        }
      }
    }
    std::sort(detections.begin(), detections.end(), canonicalOrder);
    // The graph interface declares the maximum detection budget (K=300 for
    // YOLO26n), while the numerical contract publishes the confidence-filtered
    // rows.  The fixed fixture therefore legitimately produces [1,50,6], not
    // a padded [1,300,6].  Keep at most the declared budget but never invent
    // detections merely to satisfy the graph's maximum shape.
    const auto maxCount = static_cast<std::size_t>(m_outputShape[1]);
    if (detections.size() > maxCount) {
      detections.resize(maxCount);
    }
    const auto count = detections.size();
    std::vector<float> output;
    output.reserve(count * 6);
    for (const auto& detection : detections) {
      output.insert(output.end(), {detection.x1, detection.y1, detection.x2,
                                   detection.y2, detection.confidence,
                                   static_cast<float>(detection.classId)});
    }
    std::vector<std::uint8_t> outputPayload(output.size() * sizeof(float));
    std::memcpy(outputPayload.data(), output.data(), outputPayload.size());
    auto tensor = makeFloat32Tensor(
      m_outputName, {1, static_cast<std::int64_t>(count), 6},
      outputPayload);
    const auto outputScope = metadataValue(m_spec, "outputScope").empty()
      ? std::string("final-response") : metadataValue(m_spec, "outputScope");
    return {{outputScope, makeEncodedTensorBundle(outputScope, {tensor})}};
  }

  const std::optional<ExecutionEvidence>&
  executionEvidence() const final
  {
    return m_evidence;
  }

private:
  NativeModelRunnerSpec m_spec;
  std::string m_outputName;
  std::vector<std::int64_t> m_outputShape;
  double m_confidenceThreshold = 0.0;
  std::optional<ExecutionEvidence> m_evidence;
};

} // namespace

NativeModelRunnerSpec
nativeYoloMergeRunnerSpecFromProjection(
  const NativeSelectionProjectionV3& projection)
{
  const auto& assembly = projection.assembly;
  if (assembly.mergeKind != "NATIVE_POSTPROCESS" ||
      assembly.selectedRole.empty() || assembly.expectedOutputs.size() != 1) {
    throw std::invalid_argument("projection does not declare a native YOLO Merge");
  }
  const auto output = std::find_if(
    assembly.expectedOutputs.begin(), assembly.expectedOutputs.end(),
    [&assembly] (const auto& item) {
      return item.name == assembly.postprocessOutputName;
    });
  if (output == assembly.expectedOutputs.end() || output->dtype != "float32") {
    throw std::invalid_argument("YOLO Merge contract requires one float32 detection output");
  }
  NativeModelRunnerSpec spec;
  spec.role = assembly.selectedRole;
  spec.kind = "native-yolo-postprocess";
  spec.backend = "native-yolo-postprocess";
  spec.metadata = {
    {"fragmentDigest", assembly.artifactDigest},
    {"recipeDigest", assembly.recipeDigest},
    {"mergeKind", assembly.mergeKind},
    {"postprocessIdentity", assembly.postprocessIdentity},
    {"postprocessOutputName", assembly.postprocessOutputName},
    {"postprocessConfidenceThreshold",
     std::to_string(assembly.postprocessConfidenceThreshold)},
    {"postprocessSort", assembly.postprocessSort},
    {"expectedOutputShape", [&output] {
      std::string value;
      for (std::size_t i = 0; i < output->shape.size(); ++i) {
        if (i != 0) value += ',';
        value += output->shape[i];
      }
      return value;
    }()},
    {"outputScope", "final-response"},
    {"evidence.artifactDigest", assembly.artifactDigest},
  };
  return spec;
}

std::shared_ptr<NativeModelRunner>
makeNativeYoloMergeRunner(const NativeModelRunnerSpec& spec)
{
  return std::make_shared<NativeYoloMergeRunner>(spec);
}

} // namespace ndnsf::di
