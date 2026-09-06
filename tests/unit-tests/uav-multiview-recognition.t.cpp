#include "tests/boost-test.hpp"

#include "NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp"
#include "NDNSF-UAV-APP/shared/UavNames.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <set>

namespace ndn_service_framework::test {
namespace {

using namespace ndnsf::examples::uav;

std::string
digest(const ndn::Buffer& content)
{
  ndn::util::Sha256 sha;
  sha << std::string(reinterpret_cast<const char*>(content.data()), content.size());
  return "sha256:" + sha.toString();
}

MultiViewModelProfile
profile()
{
  MultiViewModelProfile result;
  result.profileId = "vehicle-mvcnn-v1";
  result.algorithmId = "detector-guided-mvcnn-pooling/v1";
  result.modelId = "yolo26n";
  result.modelDigest = "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  result.preprocessingProfile = "rgb-resize-224-center-crop/v1";
  return result;
}

MultiViewRecognitionJob
job()
{
  MultiViewRecognitionJob result;
  result.missionSessionId = "mission-177";
  result.jobId = "job-1";
  result.targetId = "car-1";
  result.captureWindowStartMs = 100;
  result.captureWindowEndMs = 200;
  result.modelProfileId = "vehicle-mvcnn-v1";
  for (size_t i = 0; i < 4; ++i) {
    ViewEvidenceReference view;
    view.viewId = "view-" + std::to_string(i + 1);
    view.producerIdentity = ndn::Name(i % 2 == 0 ? "/uav/A" : "/uav/B");
    view.exactDataName = view.producerIdentity;
    view.exactDataName.append("UAV").append("IMAGE").append(view.viewId).appendVersion(1);
    view.contentDigest = "sha256:" + std::string(64, static_cast<char>('a' + i));
    view.captureTimeMs = 100 + i;
    view.targetId = "car-1";
    result.views.push_back(view);
  }
  return result;
}

VerifiedMultiViewInput
inputFor(const ViewEvidenceReference& reference, char value);

MultiViewRecognitionJob
jobWithInputs(std::vector<VerifiedMultiViewInput>* inputs)
{
  auto result = job();
  inputs->clear();
  for (size_t i = 0; i < result.views.size(); ++i) {
    auto input = inputFor(result.views[i], static_cast<char>('a' + i));
    result.views[i].contentDigest = input.reference.contentDigest;
    inputs->push_back(std::move(input));
  }
  return result;
}

VerifiedMultiViewInput
inputFor(const ViewEvidenceReference& reference, char value)
{
  VerifiedMultiViewInput input;
  input.reference = reference;
  input.content = ndn::Buffer{static_cast<uint8_t>(value), static_cast<uint8_t>(value + 1)};
  input.reference.contentDigest = digest(input.content);
  input.signerIdentity = reference.producerIdentity;
  input.nameVerified = true;
  input.signatureVerified = true;
  input.digestVerified = true;
  return input;
}

BOOST_AUTO_TEST_SUITE(UavMultiViewRecognition)

BOOST_AUTO_TEST_CASE(JobWireRoundTripPreservesBoundedViewContract)
{
  auto value = job();
  const auto model = profile();
  std::string reason;
  BOOST_REQUIRE(value.isValid(&model, &reason));
  const auto wire = value.wireEncode();
  BOOST_CHECK_LE(wire.size(), UAV_MULTIVIEW_MAX_WIRE_BYTES);
  const auto decoded = MultiViewRecognitionJob::wireDecode(wire);
  BOOST_CHECK_EQUAL(decoded.jobId, value.jobId);
  BOOST_CHECK_EQUAL(decoded.views.size(), value.views.size());
  BOOST_CHECK_EQUAL(decoded.views[2].exactDataName, value.views[2].exactDataName);
}

BOOST_AUTO_TEST_CASE(DeduplicatedVerifiedViewsChangeJointFusionEvidence)
{
  const auto model = profile();
  std::vector<VerifiedMultiViewInput> inputs;
  auto value = jobWithInputs(&inputs);
  auto first = executeMultiViewJob(value, model, inputs, ndn::Name("/provider/gpu"));
  BOOST_REQUIRE_MESSAGE(first.success, first.detail);
  BOOST_CHECK_EQUAL(first.result.consumedViewCount, 4U);
  BOOST_CHECK_EQUAL(first.result.annotatedViews.size(), 4U);
  auto reducedInputs = inputs;
  reducedInputs.pop_back();
  value.views.pop_back();
  auto second = executeMultiViewJob(value, model, reducedInputs, ndn::Name("/provider/gpu"));
  BOOST_REQUIRE_MESSAGE(second.success, second.detail);
  BOOST_CHECK(first.result.pooledFeatureDigest != second.result.pooledFeatureDigest);
  BOOST_CHECK(first.result.resultManifestName != second.result.resultManifestName ||
              first.result.resultManifestDigest != second.result.resultManifestDigest);
}

BOOST_AUTO_TEST_CASE(InsufficientDistinctProducerAndInvalidSignerFailClosed)
{
  auto value = job();
  value.views[1].producerIdentity = value.views[0].producerIdentity;
  auto model = profile();
  std::vector<VerifiedMultiViewInput> inputs;
  for (size_t i = 0; i < value.views.size(); ++i) inputs.push_back(inputFor(value.views[i], static_cast<char>('a' + i)));
  auto execution = executeMultiViewJob(value, model, inputs, ndn::Name("/provider/gpu"));
  BOOST_CHECK(!execution.success);
  BOOST_CHECK(execution.result.status == MultiViewTerminalStatus::InsufficientViews ||
              execution.result.status == MultiViewTerminalStatus::ValidationFailed);
  std::vector<VerifiedMultiViewInput> validInputs;
  auto validJob = jobWithInputs(&validInputs);
  validInputs[0].signerIdentity = ndn::Name("/attacker");
  execution = executeMultiViewJob(validJob, model, validInputs, ndn::Name("/provider/gpu"));
  BOOST_CHECK(!execution.success);
  BOOST_CHECK(execution.result.status == MultiViewTerminalStatus::ValidationFailed);

  std::vector<VerifiedMultiViewInput> spoofedInputs;
  auto spoofedJob = jobWithInputs(&spoofedInputs);
  spoofedInputs[0].reference.producerIdentity = ndn::Name("/uav");
  spoofedInputs[0].signerIdentity = ndn::Name("/uav");
  execution = executeMultiViewJob(spoofedJob, model, spoofedInputs,
                                   ndn::Name("/provider/gpu"));
  BOOST_CHECK(!execution.success);
  BOOST_CHECK(execution.result.status == MultiViewTerminalStatus::ValidationFailed);
}

BOOST_AUTO_TEST_CASE(ProviderOutputNamesAreContextBound)
{
  const auto provider = ndn::Name("/provider/gpu");
  const auto result = makeUavMultiViewResultName(provider, "mission", "job", 1, 1);
  const auto annotation = makeUavMultiViewAnnotationName(provider, "mission", "job", 1, "view-1", 1);
  BOOST_CHECK(isUavProviderMultiViewDataName(provider, result, "RESULT", "mission", "job"));
  BOOST_CHECK(isUavProviderMultiViewDataName(provider, annotation, "ANNOTATION", "mission", "job"));
  BOOST_CHECK(!isUavProviderMultiViewDataName(ndn::Name("/other"), result, "RESULT", "mission", "job"));
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace
} // namespace ndn_service_framework::test
