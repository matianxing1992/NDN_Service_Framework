#ifndef NDNSF_DISTRIBUTED_REPO_FACE_DATA_REFERENCE_FETCHER_HPP
#define NDNSF_DISTRIBUTED_REPO_FACE_DATA_REFERENCE_FETCHER_HPP


#include "ndnsf-distributed-repo/RepoNode.hpp"

#include <ndn-cxx/face.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

namespace ndnsf_distributed_repo::detail {

/** Private, unlinked disk spool for an exact ordered set of original Data wires.
 *
 * The fetch window bounds transient packet buffers. This object keeps only
 * per-packet offsets/lengths in memory and stores the original wire bytes in
 * tmpfile-backed storage until RepoNode validates and commits the set.
 */
class FaceDataReferencePacketSpool final : public DataReferencePacketSource
{
public:
  FaceDataReferencePacketSpool(std::size_t packetCount, uint64_t maxWireBytes);

  FaceDataReferencePacketSpool(const FaceDataReferencePacketSpool&) = delete;
  FaceDataReferencePacketSpool& operator=(const FaceDataReferencePacketSpool&) = delete;

  std::size_t
  packetCount() const noexcept override
  {
    return m_packetLengths.size();
  }

  uint64_t
  totalWireBytes() const override;

  void
  finalize();

  void
  append(std::size_t packetIndex, const std::vector<uint8_t>& wire);

  std::vector<uint8_t>
  readPacket(std::size_t packetIndex) const override;

  std::vector<uint8_t>
  readRange(uint64_t offset, std::size_t length) const override;

private:
  mutable std::mutex m_mutex;
  std::unique_ptr<std::FILE, int(*)(std::FILE*)> m_file;
  std::vector<long> m_packetOffsets;
  std::vector<uint64_t> m_logicalOffsets;
  std::vector<uint32_t> m_packetLengths;
  std::vector<bool> m_packetPresent;
  uint64_t m_maxWireBytes = 0;
  uint64_t m_totalWireBytes = 0;
  bool m_finalized = false;
};

/** Fetch the exact original Data packet set through a shared Face.
 *
 * The caller must be off the Face event thread. Interests are posted onto the
 * Face's io_context; the synchronous caller waits while the owner continues
 * pumping that context. Wire bytes are spooled to an unlinked temporary file;
 * callers read one original Data packet at a time in requested segment order.
 * Copies share single-operation admission: a timed-out operation retains its
 * slot until Face callbacks drain. The Face must outlive those callbacks.
 */
class FaceDataReferenceFetcher
{
public:
  FaceDataReferenceFetcher(ndn::Face& face,
                           std::chrono::milliseconds interestLifetime,
                           std::chrono::milliseconds totalTimeout,
                           std::size_t windowSize = 16,
                           std::shared_ptr<void> lifetime = {});

  std::shared_ptr<FaceDataReferencePacketSpool>
  operator()(const RepoDataReference& reference) const;

private:
  ndn::Face& m_face;
  std::chrono::milliseconds m_interestLifetime;
  std::chrono::milliseconds m_totalTimeout;
  std::size_t m_windowSize;
  // Caller admission remains charged until abandoned callbacks drain.
  std::shared_ptr<void> m_lifetime;
  // Copies share admission until a timed-out Face operation has drained.
  std::shared_ptr<std::atomic<bool>> m_fetchActive = std::make_shared<std::atomic<bool>>(false);
};

} // namespace ndnsf_distributed_repo::detail

#endif // NDNSF_DISTRIBUTED_REPO_FACE_DATA_REFERENCE_FETCHER_HPP
