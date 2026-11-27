#ifndef NDNSF_DETAIL_SEGMENT_MANIFEST_FETCH_HPP
#define NDNSF_DETAIL_SEGMENT_MANIFEST_FETCH_HPP

#include "../segment-manifest.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/util/scheduler.hpp>

#include <map>
#include <memory>
#include <string>

namespace ndn_service_framework::detail {

/** Bounded, ordered manifest acquisition on a Face. The invocation owns the operation. */
class SegmentManifestFetch : public std::enable_shared_from_this<SegmentManifestFetch>
{
public:
  struct Group
  {
    SegmentManifestScope scope;
    SegmentManifestLimits limits;
  };

  using GroupAt = std::function<std::optional<Group>(uint32_t)>;
  using Consume = std::function<void(const ndn::Block&)>;
  using Finish = std::function<void(std::string, uint32_t, uint64_t)>;

  SegmentManifestFetch(ndn::Face& face, std::shared_ptr<MessageValidator> validator,
                       ndn::time::milliseconds lifetime, uint32_t window,
                       GroupAt groupAt, Consume consume, Finish finish);

  void start(const ndn::Data* initialRoot = nullptr);
  void expireAfter(ndn::Scheduler& scheduler, ndn::time::milliseconds remaining);
  void stop();

private:
  enum class Phase { ROOT, PAGES, SOURCES };

  void startGroup(const ndn::Data* initialRoot = nullptr);
  void request(const ndn::Name& name, uint32_t index, uint32_t attempt);
  void receive(uint32_t index, const ndn::Data& packet);
  void pump();
  void finish(std::string error);

  ndn::Face& m_face;
  std::shared_ptr<MessageValidator> m_validator;
  ndn::time::milliseconds m_lifetime;
  uint32_t m_window;
  GroupAt m_groupAt;
  Consume m_consume;
  Finish m_finish;
  bool m_stopped = false;
  Phase m_phase = Phase::ROOT;
  uint32_t m_groupId = 0;
  uint32_t m_cursor = 0;
  uint32_t m_next = 0;
  uint64_t m_verifiedMembers = 0;
  std::optional<Group> m_group;
  std::optional<SegmentManifestVerifier> m_verifier;
  std::vector<ValidatedManifestRoot> m_roots;
  ndn::scheduler::ScopedEventId m_expiry;
  std::map<uint32_t, ndn::ScopedPendingInterestHandle> m_pending;
  std::map<uint32_t, ndn::Block> m_ready;
};

} // namespace ndn_service_framework::detail

#endif // NDNSF_DETAIL_SEGMENT_MANIFEST_FETCH_HPP
