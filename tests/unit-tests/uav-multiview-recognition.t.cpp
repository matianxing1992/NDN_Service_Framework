#include "tests/boost-test.hpp"

#include "NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp"
#include "NDNSF-UAV-APP/shared/UavNames.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <limits>
#include <set>
#include <utility>
#include <vector>

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

BOOST_AUTO_TEST_CASE(ProviderOutputNamesKeepBinaryNumbersSeparateFromEndpointText)
{
  const ndn::Name provider("/provider/gpu");
  std::vector<uint64_t> attempts;
  for (uint64_t attempt = 1; attempt <= 100; ++attempt) attempts.push_back(attempt);
  for (uint64_t attempt : {uint64_t(0x13a), uint64_t(0x3a00), uint64_t(65536),
                           std::numeric_limits<uint64_t>::max()}) attempts.push_back(attempt);
  for (const auto attempt : attempts) {
    for (uint64_t version : {uint64_t(1), uint64_t(58)}) {
      const auto result = makeUavMultiViewResultName(provider, "mission", "job", attempt, version);
      const auto annotation = makeUavMultiViewAnnotationName(
        provider, "mission", "job", attempt, "view-1", version);
      BOOST_CHECK(isUavProviderMultiViewDataName(provider, result, "RESULT", "mission", "job"));
      BOOST_CHECK(isUavProviderMultiViewDataName(provider, annotation, "ANNOTATION", "mission", "job"));
    }
  }
  const ndn::Name binaryColon("/provider/gpu/UAV/MULTIVIEW/MISSION/mission/JOB/job/RESULT/%3A/v=1");
  BOOST_CHECK(binaryColon == makeUavMultiViewResultName(provider, "mission", "job", 58, 1));
  BOOST_CHECK(isUavProviderMultiViewDataName(provider, binaryColon, "RESULT", "mission", "job"));

  for (const auto& kind : {std::string("RESULT"), std::string("ANNOTATION")}) {
    const auto valid = kind == "RESULT" ?
      makeUavMultiViewResultName(provider, "mission", "job", 58, 1) :
      makeUavMultiViewAnnotationName(provider, "mission", "job", 58, "view-1", 1);
    for (const auto& invalid : {ndn::name::Component("not-number"),
                               ndn::name::Component("%00%01%02"),
                               ndn::name::Component("%00%3A"),
                               ndn::name::Component::fromNumber(0),
                               ndn::name::Component::fromSequenceNumber(58),
                               ndn::name::Component("http%3A%2F%2F127.0.0.1")}) {
      auto changed = valid.getPrefix(-2);
      changed.append(invalid).append(valid[-1]);
      BOOST_CHECK(!isUavProviderMultiViewDataName(provider, changed, kind, "mission", "job"));
    }
    for (const auto& invalid : {ndn::name::Component::fromVersion(0),
                               ndn::name::Component::fromNumber(1),
                               ndn::name::Component(std::to_string(ndn::tlv::VersionNameComponent) + "=%00%01"),
                               ndn::name::Component(std::to_string(ndn::tlv::VersionNameComponent) + "=%00%01%02"),
                               ndn::name::Component("not-version")}) {
      auto changed = valid.getPrefix(-1);
      changed.append(invalid);
      BOOST_CHECK(!isUavProviderMultiViewDataName(provider, changed, kind, "mission", "job"));
    }
    BOOST_CHECK(!isUavProviderMultiViewDataName(provider, provider, kind, "mission", "job"));
    for (const auto& ids : {std::pair<std::string, std::string>("socket", "job"),
                            std::pair<std::string, std::string>("mission", "socket")}) {
      const auto named = kind == "RESULT" ?
        makeUavMultiViewResultName(provider, ids.first, ids.second, 58, 1) :
        makeUavMultiViewAnnotationName(provider, ids.first, ids.second, 58, "view-1", 1);
      BOOST_CHECK(!isUavProviderMultiViewDataName(provider, named, kind, ids.first, ids.second));
    }
    const ndn::Name endpointProvider("/provider/socket");
    const auto endpointName = kind == "RESULT" ?
      makeUavMultiViewResultName(endpointProvider, "mission", "job", 58, 1) :
      makeUavMultiViewAnnotationName(endpointProvider, "mission", "job", 58, "view-1", 1);
    BOOST_CHECK(!isUavProviderMultiViewDataName(endpointProvider, endpointName, kind, "mission", "job"));
  }
  const auto endpointView = makeUavMultiViewAnnotationName(provider, "mission", "job", 58, "socket", 1);
  BOOST_CHECK(!isUavProviderMultiViewDataName(provider, endpointView, "ANNOTATION", "mission", "job"));
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace
} // namespace ndn_service_framework::test
