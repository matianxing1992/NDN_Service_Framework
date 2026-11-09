#include "uav-person-published-frame.hpp"

#include "UavDigest.hpp"
#include "UavNames.hpp"
#include "ndn-service-framework/strict-json.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <stdexcept>
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
  auto candidate = std::make_unique<Publication>(Publication{fields, requesterIdentity, bytes});
  std::lock_guard<std::mutex> lock(m_mutex);
  m_publication.swap(candidate);
}

ndn::Buffer
UavPersonPublishedFrameStore::read(const std::map<std::string, std::string>& fields,
                                  const ndn::Name& authenticatedRequester) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_publication || authenticatedRequester.empty() ||
      authenticatedRequester != m_publication->requester || fields != m_publication->fields) {
    throw std::invalid_argument("source frame publication is absent, evicted or context-mismatched");
  }
  return m_publication->bytes;
}

} // namespace ndnsf::examples::uav
