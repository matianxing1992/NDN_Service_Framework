#ifndef NDNSF_DISTRIBUTED_REPO_FACE_DATA_REFERENCE_FETCHER_HPP
#define NDNSF_DISTRIBUTED_REPO_FACE_DATA_REFERENCE_FETCHER_HPP


#include "ndnsf-distributed-repo/RepoNode.hpp"

#include <ndn-cxx/face.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace ndnsf_distributed_repo::detail {

inline constexpr std::size_t MAX_NAMED_PACKET_COUNT = 4096;
inline constexpr uint64_t MAX_NAMED_PACKET_WIRE_BYTES = 32U << 20;
inline constexpr std::size_t MAX_NAMED_PACKET_NAME_BYTES = 512U << 10;

/** Private, unlinked disk spool for an exact ordered set of original Data wires.
 *
 * The fetch window bounds transient packet buffers. This object keeps only
 * per-packet offsets/lengths and one 64 KiB stdio buffer in memory, storing
 * original wires in tmpfile-backed storage until Node validates and commits.
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
  static constexpr std::size_t STDIO_BUFFER_BYTES = 64U << 10;
  mutable std::mutex m_mutex;
  // FILE must close before its borrowed stdio buffer is destroyed.
  std::array<char, STDIO_BUFFER_BYTES> m_ioBuffer;
  std::unique_ptr<std::FILE, int(*)(std::FILE*)> m_file;
  std::vector<long> m_packetOffsets;
  std::vector<uint64_t> m_logicalOffsets;
  std::vector<uint32_t> m_packetLengths;
  std::vector<bool> m_packetPresent;
  uint64_t m_maxWireBytes = 0;
  uint64_t m_totalWireBytes = 0;
  bool m_finalized = false;
  mutable bool m_readSinceAppend = false;
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
  /** Fetch exact original wires with caller-owned final object admission.
   * Complete canonical zero-based sets whose original FinalBlock agrees use
   * ndn-cxx SegmentFetcher. Its progress timeout and the external total deadline
   * bound retries; its InterestLifetime and RTO are independent. SDK window
   * statistics and a per-packet three-attempt limit are not exposed.
   * Compatibility/bootstrap retries retain at most three expressions per packet,
   * fresh Nonces, a lifetime of at most one second and the original deadline.
   * Zero windowSize lets the compatibility AIMD grow across the
   * admitted packet set without a fixed packet ceiling. A nonzero value is an
   * explicit caller capacity: start with at most 16 and reduce on marks/loss.
   * Retries have priority over new names and share the current congestion
   * window. Each immutable slot owns at most one active or queued attempt.
   * Retry lifetimes back off per packet so other packets' fast ACK samples
   * cannot shorten its recovery; all configured lifetime/deadline caps remain.
   */
  FaceDataReferenceFetcher(ndn::Face& face,
                           std::chrono::milliseconds interestLifetime,
                           std::chrono::milliseconds totalTimeout,
                           std::size_t windowSize = 0,
                           std::shared_ptr<void> lifetime = {});

  std::shared_ptr<FaceDataReferencePacketSpool>
  operator()(const RepoDataReference& reference,
             std::function<void()> requireCurrent = {}) const;

  /** Fetch explicitly named original packets under a wire-byte upper bound.
   * No manifest, digest or exact size is invented. Application admission remains caller-owned.
   * Uses the same window, retries, spool, cancellation and single-operation admission as operator().
   * requireCurrent may run concurrently on the waiting thread and Face strand; it must be
   * thread-safe and must not block on work that requires the Face strand.
   */
  std::shared_ptr<FaceDataReferencePacketSpool>
  fetchNamed(const std::vector<ndn::Name>& names, uint64_t maxWireBytes,
             const ndn::Name& forwardingHint, std::function<void()> requireCurrent = {}) const;

private:
  std::shared_ptr<FaceDataReferencePacketSpool>
  fetch(const RepoDataReference& routing, std::size_t packetCount,
        std::vector<ndn::Name> names, bool exactSize, std::function<void()> requireCurrent) const;

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
