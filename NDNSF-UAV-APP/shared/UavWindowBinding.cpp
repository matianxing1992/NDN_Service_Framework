#include "UavWindowBinding.hpp"

#include "UavDigest.hpp"
#include "uav-person-json.hpp"
#include "UavNames.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ndnsf::examples::uav {
namespace {

using person_json::Json;

void
requireIdentifier(const std::string& value, const char* field)
{
  if (value.empty() || value.size() > 128 ||
      std::any_of(value.begin(), value.end(), [] (unsigned char ch) {
        return ch < 0x20 || ch == 0x7f || ch == '/';
      })) {
    throw std::invalid_argument(std::string("invalid window binding ") + field);
  }
}

void
requireDigest(const std::string& value, const char* field)
{
  if (value.size() != 71 || value.compare(0, 7, "sha256:") != 0 ||
      canonicalSha256Digest(value.substr(7)) != value) {
    throw std::invalid_argument(std::string("invalid window binding ") + field);
  }
}

void
requireObjectRef(const UavBindingObjectRef& ref, const char* field)
{
  if (ref.name.empty() || ref.name.size() > 4096 || ref.name.front() != '/' ||
      ndn::Name(ref.name).toUri() != ref.name || ref.producerIdentity.empty()) {
    throw std::invalid_argument(std::string("invalid window binding ") + field + " name");
  }
  requireDigest(ref.digest, field);
}

Json
objectRefJson(const UavBindingObjectRef& ref)
{
  return Json{{"name", ref.name}, {"digest", ref.digest},
              {"producerIdentity", ref.producerIdentity.toUri()}};
}

UavBindingObjectRef
parseObjectRef(const Json& wire, const char* field)
{
  person_json::exactKeys(wire, {"name", "digest", "producerIdentity"});
  UavBindingObjectRef ref;
  ref.name = person_json::text(wire.at("name"));
  ref.digest = person_json::text(wire.at("digest"));
  ref.producerIdentity = ndn::Name(person_json::text(wire.at("producerIdentity")));
  requireObjectRef(ref, field);
  return ref;
}

} // namespace

ndn::Name
makeUavWindowBindingName(const ndn::Name& producerIdentity,
                         const std::string& runId,
                         const std::string& wireMissionId,
                         const std::string& windowId,
                         const std::string& requestId)
{
  if (producerIdentity.empty()) throw std::invalid_argument("window binding producer is empty");
  requireIdentifier(runId, "runId");
  requireIdentifier(wireMissionId, "wireMissionId");
  requireIdentifier(windowId, "windowId");
  requireIdentifier(requestId, "requestId");
  return ndn::Name(producerIdentity)
    .append("tracking").append("window-binding")
    .append(runId).append(wireMissionId).append(windowId).append(requestId)
    .appendVersion(1);
}

std::string
renderUavSourceDescriptor(const UavSourceDescriptor& descriptor)
{
  if (descriptor.producerIdentity.empty()) {
    throw std::invalid_argument("source descriptor producer is empty");
  }
  requireIdentifier(descriptor.sourceId, "sourceId");
  if (descriptor.cameraId != "UAV1" && descriptor.cameraId != "UAV2" &&
      descriptor.cameraId != "UAV3") {
    throw std::invalid_argument("invalid source descriptor camera");
  }
  if (descriptor.sourceEpoch == 0) {
    throw std::invalid_argument("source descriptor epoch must be positive");
  }
  for (const auto* value : {&descriptor.acquisitionKind, &descriptor.deliveryMode}) {
    if (value->empty() || value->size() > 64 ||
        std::any_of(value->begin(), value->end(), [] (unsigned char ch) {
          return ch < 0x20 || ch > 0x7e;
        })) {
      throw std::invalid_argument("invalid source descriptor mode");
    }
  }
  const Json wire{
    {"schema", "uav-source-descriptor-v1"},
    {"producerIdentity", descriptor.producerIdentity.toUri()},
    {"cameraId", descriptor.cameraId},
    {"sourceId", descriptor.sourceId},
    {"sourceEpoch", descriptor.sourceEpoch},
    {"acquisitionKind", descriptor.acquisitionKind},
    {"deliveryMode", descriptor.deliveryMode}};
  const auto text = wire.dump();
  if (text.size() > MAX_WINDOW_BINDING_BYTES) {
    throw std::invalid_argument("source descriptor exceeds its byte bound");
  }
  return text;
}

UavSourceDescriptor
parseUavSourceDescriptor(const std::string& json, size_t maximumBytes)
{
  const auto wire = person_json::parseBounded(json,
    std::min(maximumBytes, MAX_WINDOW_BINDING_BYTES));
  person_json::exactKeys(wire, {"schema", "producerIdentity", "cameraId", "sourceId",
    "sourceEpoch", "acquisitionKind", "deliveryMode"});
  if (person_json::text(wire.at("schema")) != "uav-source-descriptor-v1") {
    throw std::invalid_argument("unsupported source descriptor schema");
  }
  UavSourceDescriptor result;
  result.producerIdentity = ndn::Name(person_json::text(wire.at("producerIdentity")));
  result.cameraId = person_json::text(wire.at("cameraId"));
  result.sourceId = person_json::text(wire.at("sourceId"));
  result.sourceEpoch = person_json::unsignedNumber(wire.at("sourceEpoch"));
  result.acquisitionKind = person_json::text(wire.at("acquisitionKind"));
  result.deliveryMode = person_json::text(wire.at("deliveryMode"));
  if (renderUavSourceDescriptor(result) != json) {
    throw std::invalid_argument("source descriptor is not canonical");
  }
  return result;
}

std::string
uavSourceDescriptorDigest(const UavSourceDescriptor& descriptor)
{
  ndn::util::Sha256 hash;
  hash << renderUavSourceDescriptor(descriptor);
  return canonicalSha256Digest(hash.toString());
}

std::string
renderUavSourceInfoRequest(const UavSourceInfoRequest& request)
{
  if (request.cameraId != "UAV1" && request.cameraId != "UAV2" &&
      request.cameraId != "UAV3") {
    throw std::invalid_argument("invalid source-info request camera");
  }
  if (!request.sourceId.empty()) requireIdentifier(request.sourceId, "sourceId");
  const Json wire{
    {"schema", "uav-tracking-source-info-v1"},
    {"camera_id", request.cameraId},
    {"source_id", request.sourceId},
    {"source_epoch", request.sourceEpoch}};
  const auto text = wire.dump();
  if (text.size() > MAX_WINDOW_BINDING_BYTES) {
    throw std::invalid_argument("source-info request exceeds its byte bound");
  }
  return text;
}

UavSourceInfoRequest
parseUavSourceInfoRequest(const std::string& json, size_t maximumBytes)
{
  const auto wire = person_json::parseBounded(json,
    std::min(maximumBytes, MAX_WINDOW_BINDING_BYTES));
  person_json::exactKeys(wire, {"schema", "camera_id", "source_id", "source_epoch"});
  if (person_json::text(wire.at("schema")) != "uav-tracking-source-info-v1") {
    throw std::invalid_argument("unsupported source-info request schema");
  }
  UavSourceInfoRequest request;
  request.cameraId = person_json::text(wire.at("camera_id"));
  request.sourceId = person_json::text(wire.at("source_id"));
  request.sourceEpoch = person_json::unsignedNumber(wire.at("source_epoch"));
  if (renderUavSourceInfoRequest(request) != json) {
    throw std::invalid_argument("source-info request is not canonical");
  }
  return request;
}

std::string
renderUavSourceInfoResponse(const UavSourceInfoResponse& response)
{
  const auto descriptorText = renderUavSourceDescriptor(response.descriptor);
  if (response.digest != uavSourceDescriptorDigest(response.descriptor)) {
    throw std::invalid_argument("source-info response digest disagrees with its descriptor");
  }
  const Json wire{
    {"schema", "uav-tracking-source-info-response-v1"},
    {"descriptor", person_json::parseBounded(descriptorText, MAX_WINDOW_BINDING_BYTES)},
    {"digest", response.digest}};
  const auto text = wire.dump();
  if (text.size() > MAX_WINDOW_BINDING_BYTES) {
    throw std::invalid_argument("source-info response exceeds its byte bound");
  }
  return text;
}

UavSourceInfoResponse
parseUavSourceInfoResponse(const std::string& json, size_t maximumBytes)
{
  const auto wire = person_json::parseBounded(json,
    std::min(maximumBytes, MAX_WINDOW_BINDING_BYTES));
  person_json::exactKeys(wire, {"schema", "descriptor", "digest"});
  if (person_json::text(wire.at("schema")) != "uav-tracking-source-info-response-v1") {
    throw std::invalid_argument("unsupported source-info response schema");
  }
  UavSourceInfoResponse response;
  response.descriptor = parseUavSourceDescriptor(wire.at("descriptor").dump(),
    MAX_WINDOW_BINDING_BYTES);
  response.digest = person_json::text(wire.at("digest"));
  if (response.digest != uavSourceDescriptorDigest(response.descriptor)) {
    throw std::invalid_argument("source-info response digest mismatch");
  }
  if (renderUavSourceInfoResponse(response) != json) {
    throw std::invalid_argument("source-info response is not canonical");
  }
  return response;
}

std::string
makeUavSourceDescriptorDigest(const ndn::Name& producerIdentity,
                              const std::string& cameraId,
                              const std::string& sourceId,
                              uint64_t sourceEpoch,
                              const std::string& acquisitionKind,
                              const std::string& deliveryMode)
{
  return uavSourceDescriptorDigest({producerIdentity, cameraId, sourceId, sourceEpoch,
                                    acquisitionKind, deliveryMode});
}

void
validateUavWindowBinding(const UavWindowBinding& binding)
{
  requireIdentifier(binding.runId, "runId");
  requireIdentifier(binding.wireMissionId, "wireMissionId");
  requireIdentifier(binding.windowId, "windowId");
  requireIdentifier(binding.requestId, "requestId");
  requireIdentifier(binding.sourceId, "sourceId");
  if (binding.cameraId != "UAV1" && binding.cameraId != "UAV2" &&
      binding.cameraId != "UAV3") {
    throw std::invalid_argument("invalid window binding camera");
  }
  if (binding.windowSequence == 0 || binding.sourceEpoch == 0 ||
      binding.captureSequence == 0 || binding.trackerGeneration == 0 ||
      binding.commonPtsUs < 0) {
    throw std::invalid_argument("invalid window binding counters");
  }
  if (binding.windowId != "window-" + std::to_string(binding.windowSequence - 1)) {
    throw std::invalid_argument("window binding windowId/sequence mismatch");
  }
  requireDigest(binding.descriptorDigest, "descriptorDigest");
  requireObjectRef(binding.frameRef, "frameRef");
  requireObjectRef(binding.observationRef, "observationRef");
  if (binding.frameRef.producerIdentity != binding.observationRef.producerIdentity) {
    throw std::invalid_argument("window binding reference producers disagree");
  }
  const auto expectedFrame = makeUavObservationDataName(binding.frameRef.producerIdentity,
    binding.cameraId, binding.sourceEpoch, binding.captureSequence, "FRAME", 1).toUri();
  const auto expectedObservation = makeUavObservationDataName(
    binding.observationRef.producerIdentity, binding.cameraId, binding.sourceEpoch,
    binding.captureSequence, "OBSERVATION", 1).toUri();
  if (binding.frameRef.name != expectedFrame ||
      binding.observationRef.name != expectedObservation) {
    throw std::invalid_argument("window binding reference name does not match its context");
  }
}

std::string
renderUavWindowBinding(const UavWindowBinding& binding)
{
  validateUavWindowBinding(binding);
  const Json wire{
    {"schema", "uav-tracking-window-binding-v1"},
    {"runId", binding.runId},
    {"wireMissionId", binding.wireMissionId},
    {"windowId", binding.windowId},
    {"windowSequence", binding.windowSequence},
    {"requestId", binding.requestId},
    {"cameraId", binding.cameraId},
    {"sourceId", binding.sourceId},
    {"sourceEpoch", binding.sourceEpoch},
    {"captureSequence", binding.captureSequence},
    {"trackerGeneration", binding.trackerGeneration},
    {"descriptorDigest", binding.descriptorDigest},
    {"commonPtsUs", binding.commonPtsUs},
    {"clockUncertaintyUs", binding.clockUncertaintyUs},
    {"frameRef", objectRefJson(binding.frameRef)},
    {"observationRef", objectRefJson(binding.observationRef)}};
  const auto text = wire.dump();
  if (text.size() > MAX_WINDOW_BINDING_BYTES) {
    throw std::invalid_argument("window binding exceeds its byte bound");
  }
  return text;
}

namespace {

UavWindowBinding
parseWindowBinding(const Json& wire, const std::string& json)
{
  person_json::exactKeys(wire, {"schema", "runId", "wireMissionId", "windowId",
    "windowSequence", "requestId", "cameraId", "sourceId", "sourceEpoch",
    "captureSequence", "trackerGeneration", "descriptorDigest", "commonPtsUs",
    "clockUncertaintyUs", "frameRef", "observationRef"});
  if (person_json::text(wire.at("schema")) != "uav-tracking-window-binding-v1") {
    throw std::invalid_argument("unsupported window binding schema");
  }
  UavWindowBinding binding;
  binding.runId = person_json::text(wire.at("runId"));
  binding.wireMissionId = person_json::text(wire.at("wireMissionId"));
  binding.windowId = person_json::text(wire.at("windowId"));
  binding.windowSequence = person_json::unsignedNumber(wire.at("windowSequence"));
  binding.requestId = person_json::text(wire.at("requestId"));
  binding.cameraId = person_json::text(wire.at("cameraId"));
  binding.sourceId = person_json::text(wire.at("sourceId"));
  binding.sourceEpoch = person_json::unsignedNumber(wire.at("sourceEpoch"));
  binding.captureSequence = person_json::unsignedNumber(wire.at("captureSequence"));
  binding.trackerGeneration = person_json::unsignedNumber(wire.at("trackerGeneration"));
  binding.descriptorDigest = person_json::text(wire.at("descriptorDigest"));
  binding.commonPtsUs = static_cast<int64_t>(person_json::unsignedNumber(
    wire.at("commonPtsUs"), std::numeric_limits<int64_t>::max()));
  binding.clockUncertaintyUs = person_json::unsignedNumber(wire.at("clockUncertaintyUs"));
  binding.frameRef = parseObjectRef(wire.at("frameRef"), "frameRef");
  binding.observationRef = parseObjectRef(wire.at("observationRef"), "observationRef");
  validateUavWindowBinding(binding);
  if (renderUavWindowBinding(binding) != json) {
    throw std::invalid_argument("window binding is not canonical");
  }
  return binding;
}

void
validateWindowFailure(const UavWindowFailure& failure)
{
  requireIdentifier(failure.runId, "runId");
  requireIdentifier(failure.wireMissionId, "wireMissionId");
  requireIdentifier(failure.windowId, "windowId");
  requireIdentifier(failure.requestId, "requestId");
  requireIdentifier(failure.sourceId, "sourceId");
  requireDigest(failure.descriptorDigest, "descriptorDigest");
  if (failure.producerIdentity.empty() || failure.windowSequence == 0 ||
      failure.sourceEpoch == 0 ||
      failure.windowId != "window-" + std::to_string(failure.windowSequence - 1) ||
      (failure.cameraId != "UAV1" && failure.cameraId != "UAV2" && failure.cameraId != "UAV3")) {
    throw std::invalid_argument("invalid window failure context");
  }
  if (failure.reason.empty() || failure.reason.size() > 512 ||
      std::any_of(failure.reason.begin(), failure.reason.end(), [] (unsigned char ch) {
        return ch < 0x20 || ch == 0x7f;
      })) {
    throw std::invalid_argument("invalid window failure reason");
  }
}

UavWindowFailure
parseWindowFailure(const Json& wire, const std::string& json)
{
  person_json::exactKeys(wire, {"schema", "producerIdentity", "runId", "wireMissionId",
    "windowId", "windowSequence", "requestId", "cameraId", "sourceId", "sourceEpoch",
    "descriptorDigest", "failureCode", "reason"});
  if (person_json::text(wire.at("failureCode")) != "FRAME_UNAVAILABLE") {
    throw std::invalid_argument("unsupported window failure code");
  }
  UavWindowFailure failure;
  failure.producerIdentity = ndn::Name(person_json::text(wire.at("producerIdentity")));
  failure.runId = person_json::text(wire.at("runId"));
  failure.wireMissionId = person_json::text(wire.at("wireMissionId"));
  failure.windowId = person_json::text(wire.at("windowId"));
  failure.windowSequence = person_json::unsignedNumber(wire.at("windowSequence"));
  failure.requestId = person_json::text(wire.at("requestId"));
  failure.cameraId = person_json::text(wire.at("cameraId"));
  failure.sourceId = person_json::text(wire.at("sourceId"));
  failure.sourceEpoch = person_json::unsignedNumber(wire.at("sourceEpoch"));
  failure.descriptorDigest = person_json::text(wire.at("descriptorDigest"));
  failure.reason = person_json::text(wire.at("reason"));
  if (renderUavWindowFailure(failure) != json) {
    throw std::invalid_argument("window failure is not canonical");
  }
  return failure;
}

} // namespace

std::string
renderUavWindowFailure(const UavWindowFailure& failure)
{
  validateWindowFailure(failure);
  const auto text = Json{
    {"schema", "uav-tracking-window-failure-v1"},
    {"producerIdentity", failure.producerIdentity.toUri()},
    {"runId", failure.runId}, {"wireMissionId", failure.wireMissionId},
    {"windowId", failure.windowId}, {"windowSequence", failure.windowSequence},
    {"requestId", failure.requestId}, {"cameraId", failure.cameraId},
    {"sourceId", failure.sourceId}, {"sourceEpoch", failure.sourceEpoch},
    {"descriptorDigest", failure.descriptorDigest},
    {"failureCode", "FRAME_UNAVAILABLE"}, {"reason", failure.reason}}.dump();
  if (text.size() > MAX_WINDOW_BINDING_BYTES) {
    throw std::invalid_argument("window failure exceeds its byte bound");
  }
  return text;
}

UavWindowOutcome
parseUavWindowOutcome(const std::string& json, size_t maximumBytes)
{
  const auto wire = person_json::parseBounded(json,
    std::min(maximumBytes, MAX_WINDOW_BINDING_BYTES));
  const auto schema = person_json::text(wire.at("schema"));
  if (schema == "uav-tracking-window-binding-v1") return parseWindowBinding(wire, json);
  if (schema == "uav-tracking-window-failure-v1") return parseWindowFailure(wire, json);
  throw std::invalid_argument("unsupported window outcome schema");
}

UavWindowBinding
parseUavWindowBinding(const std::string& json, size_t maximumBytes)
{
  auto outcome = parseUavWindowOutcome(json, maximumBytes);
  if (auto* binding = std::get_if<UavWindowBinding>(&outcome)) return std::move(*binding);
  throw std::invalid_argument("a window failure is not a ready binding");
}

} // namespace ndnsf::examples::uav
