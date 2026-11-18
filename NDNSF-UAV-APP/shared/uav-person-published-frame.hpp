#ifndef NDNSF_EXAMPLES_UAV_PERSON_PUBLISHED_FRAME_HPP
#define NDNSF_EXAMPLES_UAV_PERSON_PUBLISHED_FRAME_HPP

#include <ndn-cxx/encoding/buffer.hpp>
#include <ndn-cxx/name.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace ndnsf::examples::uav {

struct UavPersonFrameReadBinding
{
  std::string runId;
  std::string missionId;
  std::string windowId;
  std::string cameraId;
  std::string sourceId;
  std::string frameName;
  std::string inputDigest;
  uint64_t sourceEpoch = 0;
  uint64_t sequence = 0;
  std::string windowBindingName;
  std::string windowBindingDigest;
};

constexpr size_t MAX_PERSON_PUBLISHED_FRAME_BYTES = 4 * 1024 * 1024;

/** Produce the exact source-frame read request; Core separately authenticates the requester. */
std::map<std::string, std::string>
makeUavPersonFrameReadFields(const UavPersonFrameReadBinding& binding);

/** Canonical Source-verified context for one exact frame. */
std::string
renderUavPersonFrameContext(const UavPersonFrameReadBinding& binding);

UavPersonFrameReadBinding
parseUavPersonFrameContext(const std::string& json, size_t maximumBytes = 16384);

/** One source-owned publication, available only to its original admitted requester. */
class UavPersonPublishedFrameStore
{
public:
  UavPersonPublishedFrameStore(ndn::Name producer, std::string cameraId);

  /** Call only after the source's actual Core publication succeeds; failure preserves the slot. */
  void retainPublished(const UavPersonFrameReadBinding& binding, const ndn::Buffer& bytes,
                       const ndn::Name& requesterIdentity);

  /** Bind the exact window-binding object reference to the retained publication. */
  void retainBindingRef(const std::string& name, const std::string& digest);

  /** Copy the matching publication under the lock; never substitute an absent/evicted frame. */
  ndn::Buffer read(const std::map<std::string, std::string>& fields,
                   const ndn::Name& authenticatedRequester) const;

  /** Source-verified exact context for the same publication, as canonical JSON. */
  std::string readContext(const std::map<std::string, std::string>& fields,
                          const ndn::Name& authenticatedRequester) const;

private:
  struct Publication
  {
    std::map<std::string, std::string> fields;
    ndn::Name requester;
    ndn::Buffer bytes;
    std::string bindingName;
    std::string bindingDigest;
  };

  ndn::Name m_producer;
  std::string m_cameraId;
  mutable std::mutex m_mutex;
  std::map<std::string, Publication> m_publications;
  std::deque<std::string> m_order;
};

} // namespace ndnsf::examples::uav

#endif // NDNSF_EXAMPLES_UAV_PERSON_PUBLISHED_FRAME_HPP
