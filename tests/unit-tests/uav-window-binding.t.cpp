#include "tests/boost-test.hpp"

#include "NDNSF-UAV-APP/shared/UavNames.hpp"
#include "NDNSF-UAV-APP/shared/UavWindowBinding.hpp"
#include "NDNSF-UAV-APP/shared/uav-person-json.hpp"

#include <string>

namespace ndnsf::test {
namespace {

using namespace examples::uav;

std::string
digest(char digit)
{
  return "sha256:" + std::string(64, digit);
}

UavWindowBinding
validBinding()
{
  const ndn::Name producer("/lab/uav/source-1");
  UavWindowBinding binding;
  binding.runId = "run-1";
  binding.wireMissionId = "tracking-run-1";
  binding.windowId = "window-0";
  binding.windowSequence = 1;
  binding.requestId = "req-1";
  binding.cameraId = "UAV1";
  binding.sourceId = "camera-source-1";
  binding.sourceEpoch = 7;
  binding.captureSequence = 100;
  binding.trackerGeneration = 1;
  binding.descriptorDigest = digest('d');
  binding.commonPtsUs = 123456;
  binding.clockUncertaintyUs = 500;
  binding.frameRef.name = makeUavObservationDataName(producer, "UAV1", 7, 100, "FRAME", 1).toUri();
  binding.frameRef.digest = digest('a');
  binding.frameRef.producerIdentity = producer;
  binding.observationRef.name =
    makeUavObservationDataName(producer, "UAV1", 7, 100, "OBSERVATION", 1).toUri();
  binding.observationRef.digest = digest('b');
  binding.observationRef.producerIdentity = producer;
  return binding;
}

} // namespace

BOOST_AUTO_TEST_SUITE(UavWindowBindingTest)

BOOST_AUTO_TEST_CASE(TerminalFailureRoundtripCannotBeReadAsAFrameBinding)
{
  const auto ready = validBinding();
  UavWindowFailure failure;
  failure.producerIdentity = ready.frameRef.producerIdentity;
  failure.runId = ready.runId;
  failure.wireMissionId = ready.wireMissionId;
  failure.windowId = ready.windowId;
  failure.windowSequence = ready.windowSequence;
  failure.requestId = ready.requestId;
  failure.cameraId = ready.cameraId;
  failure.sourceId = ready.sourceId;
  failure.sourceEpoch = ready.sourceEpoch;
  failure.descriptorDigest = ready.descriptorDigest;
  failure.reason = "injected source produced no frame for the window";
  const auto text = renderUavWindowFailure(failure);
  const auto outcome = parseUavWindowOutcome(text);
  BOOST_REQUIRE(std::holds_alternative<UavWindowFailure>(outcome));
  BOOST_CHECK_EQUAL(renderUavWindowFailure(std::get<UavWindowFailure>(outcome)), text);
  BOOST_CHECK_THROW(parseUavWindowBinding(text), std::invalid_argument);
  BOOST_CHECK(std::holds_alternative<UavWindowBinding>(
    parseUavWindowOutcome(renderUavWindowBinding(ready))));
  const auto wire = person_json::parseBounded(text, MAX_WINDOW_BINDING_BYTES);
  for (const auto* field : {"frameRef", "observationRef", "captureSequence", "trackerGeneration"}) {
    auto mixed = wire;
    mixed[field] = 1;
    BOOST_CHECK_THROW(parseUavWindowOutcome(mixed.dump()), std::exception);
  }
  for (const auto* field : {"sourceEpoch", "windowSequence"}) {
    auto invalid = wire;
    invalid[field] = 0;
    BOOST_CHECK_THROW(parseUavWindowOutcome(invalid.dump()), std::exception);
  }
  auto invalid = wire;
  invalid["failureCode"] = "SUCCESS";
  BOOST_CHECK_THROW(parseUavWindowOutcome(invalid.dump()), std::exception);
  invalid = wire;
  invalid["descriptorDigest"] = "not-a-digest";
  BOOST_CHECK_THROW(parseUavWindowOutcome(invalid.dump()), std::exception);
  invalid = wire;
  invalid["reason"] = std::string(513, 'x');
  BOOST_CHECK_THROW(parseUavWindowOutcome(invalid.dump()), std::exception);
  invalid["reason"] = "a\nsecond line";
  BOOST_CHECK_THROW(parseUavWindowOutcome(invalid.dump()), std::exception);
  BOOST_CHECK_THROW(parseUavWindowOutcome(" " + text), std::invalid_argument);
  BOOST_CHECK_THROW(parseUavWindowOutcome(text, text.size() - 1), std::exception);
}

BOOST_AUTO_TEST_CASE(CanonicalRoundtripAndProducerName)
{
  const auto binding = validBinding();
  const auto text = renderUavWindowBinding(binding);
  const auto parsed = parseUavWindowBinding(text);
  BOOST_CHECK_EQUAL(parsed.captureSequence, 100);
  BOOST_CHECK_EQUAL(parsed.sourceEpoch, 7);
  BOOST_CHECK_EQUAL(renderUavWindowBinding(parsed), text);
  const auto name = makeUavWindowBindingName(binding.frameRef.producerIdentity,
    "run-1", "tracking-run-1", "window-0", "req-1");
  BOOST_CHECK_EQUAL(name.toUri(),
    "/lab/uav/source-1/tracking/window-binding/run-1/tracking-run-1/window-0/req-1/v=1");
}

BOOST_AUTO_TEST_CASE(RejectWrongCameraAndReference)
{
  auto binding = validBinding();
  binding.cameraId = "UAV4";
  BOOST_CHECK_THROW(renderUavWindowBinding(binding), std::invalid_argument);
  binding = validBinding();
  binding.frameRef.name = makeUavObservationDataName(binding.frameRef.producerIdentity,
    "UAV1", 7, 101, "FRAME", 1).toUri();
  BOOST_CHECK_THROW(renderUavWindowBinding(binding), std::invalid_argument);
  binding = validBinding();
  binding.windowId = "window-1";
  BOOST_CHECK_THROW(renderUavWindowBinding(binding), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RejectNonCanonicalAndMalformedBytes)
{
  auto text = renderUavWindowBinding(validBinding());
  auto malformed = text;
  malformed.replace(malformed.find("\"runId\""), 7, "\"runId\" ");
  BOOST_CHECK_THROW(parseUavWindowBinding(malformed), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(SourceDescriptorCanonicalRoundtrip)
{
  const UavSourceDescriptor descriptor{ndn::Name("/lab/uav/source-1"), "UAV1",
    "camera-source-1", 7, "measured", "live-camera"};
  const auto text = renderUavSourceDescriptor(descriptor);
  const auto parsed = parseUavSourceDescriptor(text);
  BOOST_CHECK_EQUAL(parsed.sourceId, "camera-source-1");
  BOOST_CHECK_EQUAL(parsed.sourceEpoch, 7);
  BOOST_CHECK_EQUAL(renderUavSourceDescriptor(parsed), text);
  BOOST_CHECK_EQUAL(uavSourceDescriptorDigest(parsed), uavSourceDescriptorDigest(descriptor));
  auto bad = descriptor;
  bad.deliveryMode = "simulated";
  bad.cameraId = "UAV4";
  BOOST_CHECK_THROW(renderUavSourceDescriptor(bad), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(SourceInfoRequestRoundtrip)
{
  const UavSourceInfoRequest request{"UAV2", "", 0};
  const auto text = renderUavSourceInfoRequest(request);
  const auto parsed = parseUavSourceInfoRequest(text);
  BOOST_CHECK_EQUAL(parsed.cameraId, "UAV2");
  BOOST_CHECK_EQUAL(parsed.sourceId, "");
  auto bad = request;
  bad.cameraId = "UAV4";
  BOOST_CHECK_THROW(renderUavSourceInfoRequest(bad), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(SourceInfoResponseRoundtripAndDigestCheck)
{
  const UavSourceDescriptor descriptor{ndn::Name("/lab/uav/source-1"), "UAV1",
    "camera-source-1", 7, "measured", "live-camera"};
  const UavSourceInfoResponse response{descriptor, uavSourceDescriptorDigest(descriptor)};
  const auto text = renderUavSourceInfoResponse(response);
  const auto parsed = parseUavSourceInfoResponse(text);
  BOOST_CHECK_EQUAL(parsed.descriptor.sourceId, "camera-source-1");
  BOOST_CHECK_EQUAL(parsed.digest, response.digest);
  auto tampered = response;
  tampered.digest = digest('9');
  BOOST_CHECK_THROW(renderUavSourceInfoResponse(tampered), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(SourceDescriptorBindsIdentityAndMode)
{
  const ndn::Name producer("/lab/uav/source-1");
  const auto base = makeUavSourceDescriptorDigest(producer, "UAV1", "camera-source-1", 7,
    "measured", "live-camera");
  BOOST_CHECK_EQUAL(base, makeUavSourceDescriptorDigest(producer, "UAV1", "camera-source-1",
    7, "measured", "live-camera"));
  BOOST_CHECK(base != makeUavSourceDescriptorDigest(producer, "UAV1", "camera-source-1", 8,
    "measured", "live-camera"));
  BOOST_CHECK(base != makeUavSourceDescriptorDigest(producer, "UAV1", "camera-source-1", 7,
    "measured", "video-replay"));
  BOOST_CHECK_THROW(makeUavSourceDescriptorDigest(producer, "UAV1", "camera-source-1", 0,
    "measured", "live-camera"), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::test
