#include "uav-person-published-frame.hpp"

#include "uav-person-json.hpp"

#include "UavDigest.hpp"
#include "UavNames.hpp"
#include "ndn-service-framework/strict-json.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <string_view>
#include <utility>

namespace ndnsf::examples::uav {
namespace {

void
checkIdentifier(const std::string& text)
{
  if (text.empty() || text.size() > 128 || !ndn_service_framework::json::isValidUtf8(text) ||
      std::any_of(text.begin(), text.end(), [] (unsigned char ch) { return ch < 0x20; })) {
    throw std::invalid_argument("invalid source frame read identifier");
  }
}

void
checkCamera(const std::string& camera)
{
  if (camera != "UAV1" && camera != "UAV2" && camera != "UAV3") {
    throw std::invalid_argument("invalid source frame read camera");
  }
}

} // namespace

std::map<std::string, std::string>
makeUavPersonFrameReadFields(const UavPersonFrameReadBinding& binding)
{
  for (const auto* text : {&binding.runId, &binding.missionId, &binding.windowId, &binding.sourceId}) {
    checkIdentifier(*text);
  }
  checkCamera(binding.cameraId);
  if (binding.sourceEpoch == 0 || binding.sequence == 0 || binding.frameName.size() > 4096 ||
      binding.frameName.empty() || binding.frameName.front() != '/' ||
      ndn::Name(binding.frameName).toUri() != binding.frameName || binding.inputDigest.size() != 71 ||
      binding.inputDigest.compare(0, 7, "sha256:") != 0 ||
      canonicalSha256Digest(binding.inputDigest.substr(7)) != binding.inputDigest) {
    throw std::invalid_argument("invalid source frame read identity/digest");
  }
  return {{"schema", "uav-person-source-frame-read-v1"}, {"run_id", binding.runId},
          {"mission_id", binding.missionId}, {"window_id", binding.windowId},
          {"camera_id", binding.cameraId}, {"source_id", binding.sourceId},
          {"source_epoch", std::to_string(binding.sourceEpoch)},
          {"sequence", std::to_string(binding.sequence)}, {"frame_name", binding.frameName},
          {"input_digest", binding.inputDigest}};
}

UavPersonPublishedFrameStore::UavPersonPublishedFrameStore(ndn::Name producer, std::string cameraId)
  : m_producer(std::move(producer))
  , m_cameraId(std::move(cameraId))
{
  if (m_producer.empty()) throw std::invalid_argument("source frame store requires a producer");
  checkCamera(m_cameraId);
}

void
UavPersonPublishedFrameStore::retainPublished(const UavPersonFrameReadBinding& binding,
                                             const ndn::Buffer& bytes,
                                             const ndn::Name& requesterIdentity)
{
  const auto fields = makeUavPersonFrameReadFields(binding);
  if (binding.cameraId != m_cameraId || requesterIdentity.empty() || bytes.empty() ||
      bytes.size() > MAX_PERSON_PUBLISHED_FRAME_BYTES ||
      makeUavObservationDataName(m_producer, m_cameraId, binding.sourceEpoch, binding.sequence,
                                "FRAME", 1).toUri() != binding.frameName) {
    throw std::invalid_argument("source publication does not match its owner/bounds");
  }
  ndn::util::Sha256 hash;
  hash << std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (canonicalSha256Digest(hash.toString()) != binding.inputDigest) {
    throw std::invalid_argument("source publication bytes do not match its digest");
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto frameName = binding.frameName;
  if (m_publications.find(frameName) == m_publications.end()) {
    m_order.push_back(frameName);
  }
  m_publications[frameName] = Publication{fields, requesterIdentity, bytes};
  constexpr size_t MAX_RETAINED_FRAMES = 8;
  while (m_order.size() > MAX_RETAINED_FRAMES) {
    m_publications.erase(m_order.front());
    m_order.pop_front();
  }
}

ndn::Buffer
UavPersonPublishedFrameStore::read(const std::map<std::string, std::string>& fields,
                                  const ndn::Name& authenticatedRequester) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto frameNameIt = fields.find("frame_name");
  const auto publication = frameNameIt == fields.end()
    ? m_publications.end() : m_publications.find(frameNameIt->second);
  if (publication == m_publications.end() || authenticatedRequester.empty() ||
      authenticatedRequester != publication->second.requester ||
      fields != publication->second.fields) {
    throw std::invalid_argument("source frame publication is absent, evicted or context-mismatched");
  }
  return publication->second.bytes;
}

std::string
renderUavPersonFrameContext(const UavPersonFrameReadBinding& binding)
{
  const uav::person_json::Json wire{
    {"runId", binding.runId}, {"missionId", binding.missionId},
    {"windowId", binding.windowId}, {"cameraId", binding.cameraId},
    {"sourceId", binding.sourceId}, {"sourceEpoch", binding.sourceEpoch},
    {"sequence", binding.sequence}, {"frameName", binding.frameName},
    {"inputDigest", binding.inputDigest},
    {"windowBindingName", binding.windowBindingName},
    {"windowBindingDigest", binding.windowBindingDigest}};
  return wire.dump();
}

UavPersonFrameReadBinding
parseUavPersonFrameContext(const std::string& json, size_t maximumBytes)
{
  const auto wire = uav::person_json::parseBounded(json, maximumBytes);
  uav::person_json::exactKeys(wire, {"runId", "missionId", "windowId", "cameraId",
    "sourceId", "sourceEpoch", "sequence", "frameName", "inputDigest",
    "windowBindingName", "windowBindingDigest"});
  UavPersonFrameReadBinding binding;
  binding.runId = uav::person_json::text(wire.at("runId"));
  binding.missionId = uav::person_json::text(wire.at("missionId"));
  binding.windowId = uav::person_json::text(wire.at("windowId"));
  binding.cameraId = uav::person_json::text(wire.at("cameraId"));
  binding.sourceId = uav::person_json::text(wire.at("sourceId"));
  binding.sourceEpoch = uav::person_json::unsignedNumber(wire.at("sourceEpoch"));
  binding.sequence = uav::person_json::unsignedNumber(wire.at("sequence"));
  binding.frameName = uav::person_json::text(wire.at("frameName"));
  binding.inputDigest = uav::person_json::text(wire.at("inputDigest"));
  binding.windowBindingName = uav::person_json::text(wire.at("windowBindingName"));
  binding.windowBindingDigest = uav::person_json::text(wire.at("windowBindingDigest"));
  if (renderUavPersonFrameContext(binding) != json) {
    throw std::invalid_argument("source frame context is not canonical");
  }
  return binding;
}

void
UavPersonPublishedFrameStore::retainBindingRef(const std::string& name,
                                               const std::string& digest)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_order.empty()) throw std::invalid_argument("no publication for binding ref");
  auto& publication = m_publications.at(m_order.back());
  publication.bindingName = name;
  publication.bindingDigest = digest;
}

std::string
UavPersonPublishedFrameStore::readContext(const std::map<std::string, std::string>& fields,
                                          const ndn::Name& authenticatedRequester) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto frameNameIt = fields.find("frame_name");
  const auto publication = frameNameIt == fields.end()
    ? m_publications.end() : m_publications.find(frameNameIt->second);
  if (publication == m_publications.end() || authenticatedRequester.empty() ||
      authenticatedRequester != publication->second.requester ||
      fields != publication->second.fields) {
    throw std::invalid_argument("source frame publication is absent, evicted or context-mismatched");
  }
  UavPersonFrameReadBinding binding;
  binding.runId = fields.at("run_id");
  binding.missionId = fields.at("mission_id");
  binding.windowId = fields.at("window_id");
  binding.cameraId = fields.at("camera_id");
  binding.sourceId = fields.at("source_id");
  binding.sourceEpoch = std::stoull(fields.at("source_epoch"));
  binding.sequence = std::stoull(fields.at("sequence"));
  binding.frameName = fields.at("frame_name");
  binding.inputDigest = fields.at("input_digest");
  binding.windowBindingName = publication->second.bindingName;
  binding.windowBindingDigest = publication->second.bindingDigest;
  return renderUavPersonFrameContext(binding);
}

} // namespace ndnsf::examples::uav
