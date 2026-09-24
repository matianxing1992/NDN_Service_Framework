#pragma once

#include "UavFrameIngress.hpp"
#include "UavNames.hpp"
#include "UavProtocol.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::examples::uav {

struct PublishedFrame
{
  UavEvidenceReference reference;
  /** Complete source bytes retained for the Core segmented publication call. */
  ndn::Buffer payload;
  std::vector<ndn::Buffer> segments;
};

class UavTrackingEvidenceStore
{
public:
  explicit UavTrackingEvidenceStore(size_t maxSegmentBytes = 1400,
                                    size_t maxRetainedBytes = 16 * 1024 * 1024);

  std::optional<PublishedFrame>
  stage(const ndn::Name& producer, const std::string& missionId,
        const std::string& windowId, const FrameEnvelope& frame,
        std::string* reason = nullptr);

  /**
   * Publish a staged frame through the existing collaboration primitive.
   * The application owns the exact producer name; Core owns signing,
   * segmentation, and retrieval semantics.
   */
  ndn::Name publish(ndn_service_framework::ServiceProvider::CollaborationContext& context,
                    const std::string& keyScope, const PublishedFrame& frame,
                    int freshnessMs = 60000) const;
  void clear();
  size_t retainedBytes() const noexcept { return m_retainedBytes; }

private:
  size_t m_maxSegmentBytes;
  size_t m_maxRetainedBytes;
  size_t m_retainedBytes = 0;
  uint64_t m_version = 1;
};

} // namespace ndnsf::examples::uav
