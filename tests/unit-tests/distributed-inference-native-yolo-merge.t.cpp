#include "tests/boost-test.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeYoloMergeRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>

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

RoleExecutionContext mergeInputs(bool detections, std::array<int, 3> grids = {80, 40, 20},
                                 std::size_t detectionScale = 0, bool groupByBranch = false)
{
  RoleExecutionContext context;
  std::array<std::vector<NamedTensor>, 2> groupedTensors;
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
      if (detections && branch == 3 && static_cast<std::size_t>(scale) == detectionScale) {
        values[0] = 0.0f;
        values[grid * grid] = 0.0f;
        values[1] = 0.0f;
      }
      std::vector<std::uint8_t> bytes(values.size() * sizeof(float));
      std::memcpy(bytes.data(), values.data(), bytes.size());
      auto tensor = makeFloat32Tensor(name, {1, channels, grid, grid}, bytes);
      if (groupByBranch) {
        groupedTensors[static_cast<std::size_t>(branch - 2)].push_back(std::move(tensor));
      }
      else {
        DependencyEdge edge;
        edge.scope = scope;
        edge.tensors = {name};
        context.inputEdgesByScope[scope] = edge;
        context.inputsByScope[scope] = makeEncodedTensorBundle(scope, {tensor});
      }
    }
  }
  if (groupByBranch) {
    for (int branch = 2; branch <= 3; ++branch) {
      const auto scope = "input-group-" + std::to_string(branch);
      const auto& bundleTensors = groupedTensors[static_cast<std::size_t>(branch - 2)];
      DependencyEdge edge;
      edge.scope = scope;
      for (const auto& tensor : bundleTensors) edge.tensors.push_back(tensor.name);
      context.inputEdgesByScope[scope] = std::move(edge);
      context.inputsByScope[scope] = makeEncodedTensorBundle(scope, bundleTensors);
    }
  }
  return context;
}

RoleExecutionContext mergeInputsWithRepeatedProviderBundles(bool detections)
{
  auto individualInputs = mergeInputs(detections);
  std::map<std::string, NamedTensor> tensorsByName;
  for (const auto& item : individualInputs.inputsByScope) {
    for (auto& tensor : decodeTensorBundle(item.second.payload)) {
      tensorsByName.emplace(tensor.name, std::move(tensor));
    }
  }

  std::array<std::vector<std::string>, 2> namesByProvider;
  for (const auto& item : tensorsByName) {
    const auto provider = item.first.find("one2one_cv2.0/") != std::string::npos ||
                          item.first.find("one2one_cv3.0/") != std::string::npos ? 0 : 1;
    namesByProvider[provider].push_back(item.first);
  }

  RoleExecutionContext context;
  std::size_t scopeIndex = 0;
  for (const auto& item : tensorsByName) {
    const auto provider = item.first.find("one2one_cv2.0/") != std::string::npos ||
                          item.first.find("one2one_cv3.0/") != std::string::npos ? 0 : 1;
    const auto scope = "provider-bundle-" + std::to_string(scopeIndex++);
    DependencyEdge edge;
    edge.scope = scope;
    edge.tensors = {item.first};
    edge.bundleTensorNames = namesByProvider[provider];
    std::vector<NamedTensor> bundleTensors;
    for (const auto& name : edge.bundleTensorNames) {
      bundleTensors.push_back(tensorsByName.at(name));
    }
    context.inputEdgesByScope[scope] = std::move(edge);
    context.inputsByScope[scope] = makeEncodedTensorBundle(scope, bundleTensors);
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
  output = mergeOutput(mergeSpec(), mergeInputs(true, {80, 40, 20}, 0, true));
  BOOST_CHECK(output.shape == std::vector<std::int64_t>({1, 3, 6}));
  BOOST_REQUIRE_EQUAL(output.payload.size(), expected.size() * sizeof(float));
  BOOST_CHECK_EQUAL(std::memcmp(output.payload.data(), expected.data(), output.payload.size()), 0);
  output = mergeOutput(mergeSpec(), mergeInputs(true, {4, 2, 1}));
  BOOST_CHECK(output.shape == std::vector<std::int64_t>({1, 3, 6}));
  BOOST_REQUIRE_EQUAL(output.payload.size(), expected.size() * sizeof(float));
  BOOST_CHECK_EQUAL(std::memcmp(output.payload.data(), expected.data(), output.payload.size()), 0);
  output = mergeOutput(mergeSpec(), mergeInputs(true, {4, 2, 1}, 2));
  const std::vector<float> coarsestScaleExpected{
    -16, -16, 48, 48, .5f, 0,
    -16, -16, 48, 48, .5f, 1};
  BOOST_CHECK(output.shape == std::vector<std::int64_t>({1, 2, 6}));
  BOOST_REQUIRE_EQUAL(output.payload.size(), coarsestScaleExpected.size() * sizeof(float));
  BOOST_CHECK_EQUAL(std::memcmp(output.payload.data(), coarsestScaleExpected.data(),
                                output.payload.size()), 0);
  BOOST_CHECK_THROW(mergeOutput(mergeSpec(), mergeInputs(false, {4, 3, 1})),
                    std::invalid_argument);
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

BOOST_AUTO_TEST_CASE(NativeYoloMergeAcceptsRepeatedCompleteProviderBundles)
{
  const auto output = mergeOutput(mergeSpec(), mergeInputs(true));
  const auto bundledOutput = mergeOutput(
    mergeSpec(), mergeInputsWithRepeatedProviderBundles(true));
  BOOST_CHECK_EQUAL(bundledOutput.name, output.name);
  BOOST_CHECK(bundledOutput.shape == output.shape);
  BOOST_CHECK(bundledOutput.payload == output.payload);

  auto conflicting = mergeInputsWithRepeatedProviderBundles(true);
  auto& bundle = conflicting.inputsByScope.begin()->second;
  auto tensors = decodeTensorBundle(bundle.payload);
  tensors.front().payload.front() ^= 1;
  bundle = makeEncodedTensorBundle(bundle.name, tensors);
  BOOST_CHECK_THROW(makeNativeYoloMergeRunner(mergeSpec())->run(conflicting),
                    std::invalid_argument);

  auto omittedEndpoint = mergeInputsWithRepeatedProviderBundles(true);
  auto edge = omittedEndpoint.inputEdgesByScope.begin();
  const auto endpointName = edge->second.tensors.front();
  edge->second.bundleTensorNames.erase(
    std::remove(edge->second.bundleTensorNames.begin(),
                edge->second.bundleTensorNames.end(), endpointName),
    edge->second.bundleTensorNames.end());
  auto& omittedBundle = omittedEndpoint.inputsByScope.at(edge->first);
  auto incompleteBundle = decodeTensorBundle(omittedBundle.payload);
  incompleteBundle.erase(
    std::remove_if(incompleteBundle.begin(), incompleteBundle.end(),
                   [&] (const auto& tensor) { return tensor.name == endpointName; }),
    incompleteBundle.end());
  omittedBundle = makeEncodedTensorBundle(omittedBundle.name, incompleteBundle);
  BOOST_CHECK_THROW(makeNativeYoloMergeRunner(mergeSpec())->run(omittedEndpoint),
                    std::invalid_argument);
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
  const auto scope = context.inputEdgesByScope.begin()->first;
  auto selectedTensors = decodeTensorBundle(context.inputsByScope.at(scope).payload);
  auto extra = makeFloat32Tensor("/foreign", {1}, std::vector<std::uint8_t>(sizeof(float)));
  context.inputEdgesByScope.at(scope).tensors.push_back(extra.name);
  selectedTensors.push_back(std::move(extra));
  context.inputsByScope[scope] = makeEncodedTensorBundle(scope, selectedTensors);
  BOOST_CHECK_THROW(runner->run(context), std::invalid_argument);

  context = mergeInputs(false);
  auto& bundle = context.inputsByScope.begin()->second;
  auto tensors = decodeTensorBundle(bundle.payload);
  tensors.front().shape = {1, 4, 1, 6400};
  bundle = makeEncodedTensorBundle(context.inputEdgesByScope.begin()->first, tensors);
  BOOST_CHECK_THROW(runner->run(context), std::invalid_argument);
}
} // namespace ndnsf::di::test
