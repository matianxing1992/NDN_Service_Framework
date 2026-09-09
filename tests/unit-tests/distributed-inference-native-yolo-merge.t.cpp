#include "tests/boost-test.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeYoloMergeRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <array>
#include <cstring>
#include <limits>

namespace ndnsf::di::test {
namespace {
NativeSelectionProjectionV3 mergeProjection()
{
  NativeSelectionProjectionV3 projection;
  auto& assembly = projection.assembly;
  assembly.selectedRole = "Merge";
  assembly.mergeKind = "NATIVE_POSTPROCESS";
  assembly.postprocessIdentity = "YOLO26n-canonical-detection-rows";
  assembly.postprocessOutputName = "predictions";
  assembly.postprocessConfidenceThreshold = .001;
  assembly.postprocessSort = "confidence-desc,class-asc,xyxy-asc";
  assembly.artifactDigest = "sha256:" + std::string(64, 'c');
  assembly.recipeDigest = "sha256:" + std::string(64, 'd');
  assembly.expectedOutputs = {{"predictions", "float32", {"1", "300", "6"}}};
  return projection;
}

NativeModelRunnerSpec mergeSpec()
{
  auto spec = nativeYoloMergeRunnerSpecFromProjection(mergeProjection());
  for (const auto& entry : std::map<std::string, std::string>{
         {"evidence.providerName", "/provider/merge"},
         {"evidence.providerBootId", "boot-1"},
         {"evidence.epoch", "1"}, {"evidence.createdAtMs", "1"},
         {"evidence.modelDigest", "sha256:" + std::string(64, 'a')},
         {"evidence.planDigest", "sha256:" + std::string(64, 'b')},
         {"evidence.artifactDigest", "sha256:" + std::string(64, 'c')}}) {
    spec.metadata[entry.first] = entry.second;
  }
  return spec;
}

RoleExecutionContext mergeInputs(bool detections)
{
  RoleExecutionContext context;
  const std::array<int, 3> grids{80, 40, 20};
  for (int scale = 0; scale < 3; ++scale) {
    for (int branch = 2; branch <= 3; ++branch) {
      const auto grid = grids[scale];
      const auto channels = branch == 2 ? 4 : 80;
      const auto component = "one2one_cv" + std::to_string(branch) + "." +
        std::to_string(scale);
      const auto name = "/model/model.23/" + component + "/" + component +
        ".2/Conv_output_0";
      const auto scope = "input-" + std::to_string(scale) + "-" + std::to_string(branch);
      std::vector<float> values(channels * grid * grid, branch == 2 ? 1.0f : -100.0f);
      if (detections && branch == 3 && scale == 0) {
        values[0] = 0.0f;
        values[grid * grid] = 0.0f;
        values[1] = 0.0f;
      }
      std::vector<std::uint8_t> bytes(values.size() * sizeof(float));
      std::memcpy(bytes.data(), values.data(), bytes.size());
      auto tensor = makeFloat32Tensor(name, {1, channels, grid, grid}, bytes);
      DependencyEdge edge;
      edge.scope = scope;
      edge.tensors = {name};
      context.inputEdgesByScope[scope] = edge;
      context.inputsByScope[scope] = makeEncodedTensorBundle(scope, {tensor});
    }
  }
  return context;
}

NamedTensor mergeOutput(const NativeModelRunnerSpec& spec, const RoleExecutionContext& context)
{
  auto runner = makeNativeYoloMergeRunner(spec);
  const auto result = runner->run(context);
  const auto tensors = decodeTensorBundle(result.at("final-response").payload);
  BOOST_REQUIRE_EQUAL(tensors.size(), 1);
  BOOST_REQUIRE(runner->executionEvidence());
  BOOST_CHECK(runner->executionEvidence()->runnerKind == RunnerKind::NativeYoloPostprocess);
  return tensors.front();
}
}

BOOST_AUTO_TEST_CASE(NativeYoloMergeComputesCanonicalRowsAndBudget)
{
  auto spec = mergeSpec();
  auto output = mergeOutput(spec, mergeInputs(true));
  BOOST_CHECK(output.shape == std::vector<std::int64_t>({1, 3, 6}));
  const std::vector<float> expected{
    -4, -4, 12, 12, .5f, 0,
     4, -4, 20, 12, .5f, 0,
    -4, -4, 12, 12, .5f, 1};
  BOOST_REQUIRE_EQUAL(output.payload.size(), expected.size() * sizeof(float));
  BOOST_CHECK_EQUAL(std::memcmp(output.payload.data(), expected.data(), output.payload.size()), 0);
  spec.metadata["expectedOutputShape"] = "1,2,6";
  output = mergeOutput(spec, mergeInputs(true));
  BOOST_CHECK(output.shape == std::vector<std::int64_t>({1, 2, 6}));
  BOOST_CHECK_EQUAL(std::memcmp(output.payload.data(), expected.data(), output.payload.size()), 0);
}

BOOST_AUTO_TEST_CASE(NativeYoloMergePreservesEmptyDetectionResult)
{
  const auto output = mergeOutput(mergeSpec(), mergeInputs(false));
  BOOST_CHECK(output.shape == std::vector<std::int64_t>({1, 0, 6}));
  BOOST_CHECK(output.payload.empty());
}

BOOST_AUTO_TEST_CASE(NativeYoloMergeRejectsUnknownPostprocessIdentity)
{
  auto spec = mergeSpec();
  spec.metadata["postprocessIdentity"] = "different-postprocessor";
  BOOST_CHECK_THROW(makeNativeYoloMergeRunner(spec), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeYoloMergeRejectsMalformedNumericMetadata)
{
  for (const auto& value : {"0.001suffix", "nan", "inf", "-0.1", "1.1"}) {
    auto spec = mergeSpec();
    spec.metadata["postprocessConfidenceThreshold"] = value;
    BOOST_CHECK_THROW(makeNativeYoloMergeRunner(spec), std::invalid_argument);
  }
  auto spec = mergeSpec();
  spec.metadata["expectedOutputShape"] = "1,300,6,";
  BOOST_CHECK_THROW(makeNativeYoloMergeRunner(spec), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeYoloMergeRejectsWrongOutputType)
{
  NativeSelectionProjectionV3 projection;
  projection.assembly.selectedRole = "Merge";
  projection.assembly.mergeKind = "NATIVE_POSTPROCESS";
  projection.assembly.postprocessOutputName = "predictions";
  projection.assembly.expectedOutputs = {{"predictions", "int64", {"1", "300", "6"}}};
  BOOST_CHECK_THROW(nativeYoloMergeRunnerSpecFromProjection(projection), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeYoloMergeBindsPreparedOutputBudgetToSelection)
{
  const auto projection = mergeProjection();
  auto spec = mergeSpec();
  BOOST_REQUIRE(!validateNativePreparedRunnerSpec(projection, spec));
  spec.metadata["expectedOutputShape"] = "1,1,6";
  BOOST_CHECK_EQUAL(validateNativePreparedRunnerSpec(projection, spec).value_or(""),
                    "DI_PROVIDER_NATIVE_MERGE_METADATA_MISMATCH");
}

BOOST_AUTO_TEST_CASE(NativeYoloMergeRejectsMissingOrMalformedDependency)
{
  auto runner = makeNativeYoloMergeRunner(mergeSpec());
  auto context = mergeInputs(false);
  context.inputsByScope.erase(context.inputsByScope.begin());
  BOOST_CHECK_THROW(runner->run(context), std::invalid_argument);
  context = mergeInputs(false);
  auto& bundle = context.inputsByScope.begin()->second;
  auto tensors = decodeTensorBundle(bundle.payload);
  tensors.front().shape = {1, 4, 1, 6400};
  bundle = makeEncodedTensorBundle(context.inputEdgesByScope.begin()->first, tensors);
  BOOST_CHECK_THROW(runner->run(context), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeYoloAtomicPostprocessCanonicalizesPredictions)
{
  const std::vector<float> values{
    2, 3, 4, 5, .8f, 1,
    1, 2, 3, 4, .9f, 0,
    9, 9, 9, 9, .0005f, 2,
  };
  std::vector<std::uint8_t> payload(values.size() * sizeof(float));
  std::memcpy(payload.data(), values.data(), payload.size());
  const auto output = nativeYoloCanonicalizePredictions(
    makeFloat32Tensor("predictions", {1, 3, 6}, payload),
    "predictions", .001, 300);
  BOOST_CHECK(output.shape == std::vector<std::int64_t>({1, 2, 6}));
  const std::vector<float> expected{
    1, 2, 3, 4, .9f, 0,
    2, 3, 4, 5, .8f, 1,
  };
  BOOST_REQUIRE_EQUAL(output.payload.size(), expected.size() * sizeof(float));
  BOOST_CHECK_EQUAL(std::memcmp(output.payload.data(), expected.data(),
                                output.payload.size()), 0);

  auto malformed = makeFloat32Tensor("predictions", {1, 1, 6},
                                     float32Payload({1, 2, 3, 4, .5f, .5f}));
  BOOST_CHECK_THROW(nativeYoloCanonicalizePredictions(
    malformed, "predictions", .001, 300), std::invalid_argument);
}
} // namespace ndnsf::di::test
