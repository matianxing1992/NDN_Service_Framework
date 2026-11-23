#ifndef NDNSF_DISTRIBUTED_REPO_FACE_DATA_REFERENCE_FETCHER_HPP
#define NDNSF_DISTRIBUTED_REPO_FACE_DATA_REFERENCE_FETCHER_HPP


#include "ndnsf-distributed-repo/RepoNode.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/util/span.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace ndnsf_distributed_repo::detail {

inline constexpr std::size_t MAX_NAMED_PACKET_COUNT = 4096;
inline constexpr uint64_t MAX_NAMED_PACKET_WIRE_BYTES = 32U << 20;
inline constexpr std::size_t MAX_NAMED_PACKET_NAME_BYTES = 512U << 10;

/** Trace the ordinary manifest root along the exact Interest path. */
void traceManifestRoot(const ndn::Name& name, const char* stage) noexcept;

/** Sample manifest DATA across the Producer and fetching Face callbacks. */
bool traceManifestSource(const ndn::Name& name, const char* stage) noexcept;

/** Shared admission for authenticated source Interests on one Repo Face. */
class ManifestSourceWindow
{
public:
  explicit ManifestSourceWindow(std::size_t capacity);

  // A wake callback posts work to the waiting fetch's strand.
  bool tryAcquire(const void* owner, const std::function<void()>& wake);
  void release(std::size_t count, const void* cancelledOwner = nullptr);
  std::size_t peak() const;
  std::size_t capacity() const { return m_capacity; }

private:
  void wakeNext() noexcept;

  struct Waiter
  {
    const void* owner;
    std::function<void()> wake;
    bool notified = false;
  };

  const std::size_t m_capacity;
  mutable std::mutex m_mutex;
  std::size_t m_pending = 0;
  std::size_t m_peak = 0;
  std::deque<Waiter> m_waiters;
};

/** Bounded temporary storage for an exact ordered set of original Data wires.
 *
 * The default keeps offsets/lengths and a 64 KiB stdio buffer in memory.
 * Manifest readers returning owned wires can keep a small authenticated set
 * in memory and transfer each slot after finalization.
 */
class FaceDataReferencePacketSpool final : public DataReferencePacketSource
{
public:
  FaceDataReferencePacketSpool(std::size_t packetCount, uint64_t maxWireBytes,
                             const std::vector<uint32_t>& plannedWireLengths = {},
                             bool materializeWires = false,
                             std::shared_ptr<void> spoolLease = {});

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
  append(std::size_t packetIndex, ndn::span<const uint8_t> wire);

  std::vector<uint8_t>
  readPacket(std::size_t packetIndex) const override;

  /** Transfer an owned wire when the fetch was bounded for a materialized result. */
  std::vector<uint8_t>
  takePacket(std::size_t packetIndex);

  std::vector<uint8_t>
  readRange(uint64_t offset, std::size_t length) const override;

private:
  void
  readAt(long offset, uint8_t* output, std::size_t length) const;

  static constexpr std::size_t STDIO_BUFFER_BYTES = 64U << 10;
  // Release the reservation after the retained wires and pending reads are gone.
  std::shared_ptr<void> m_spoolLease;
  mutable std::mutex m_mutex;
  // FILE must close before its borrowed stdio buffer is destroyed.
  std::array<char, STDIO_BUFFER_BYTES> m_ioBuffer;
  std::unique_ptr<std::FILE, int(*)(std::FILE*)> m_file;
  std::vector<long> m_packetOffsets;
  std::vector<uint64_t> m_logicalOffsets;
  std::vector<uint32_t> m_packetLengths;
  std::vector<bool> m_packetPresent;
  std::vector<std::vector<uint8_t>> m_materializedWires;
  uint64_t m_maxWireBytes = 0;
  uint64_t m_totalWireBytes = 0;
  bool m_finalized = false;
  bool m_positioned = false;
  mutable bool m_readSinceAppend = false;
  mutable long m_nextReadOffset = -1;
};

/** Move materialized manifest wires, or read from file-backed sources. */
std::vector<uint8_t>
takeFetchedPacket(DataReferencePacketSource& source, std::size_t index);

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
   * With an admission callback, full digest names use Interest matching and
   * support only the exact path. Admission runs on the Face strand before append.
   * A supplied absolute deadline caps this fetch, including retries.
   */
  AuthenticatedManifestPackets
  fetchManifest(const RepoDataReference& reference,
                const ndn_service_framework::SegmentManifestScope& scope,
                std::chrono::steady_clock::time_point deadline,
                const ManifestRootValidator& validateRoot,
                std::function<void()> requireCurrent = {},
                std::size_t maxPacketCount = MAX_NAMED_PACKET_COUNT,
                std::shared_ptr<ManifestSourceWindow> sourceWindow = {},
                bool materializeWires = false,
                std::shared_ptr<void> spoolLease = {}) const;

  using PacketAdmission = std::function<void(std::size_t, const ndn::Data&)>;

  std::shared_ptr<FaceDataReferencePacketSpool>
  fetchNamed(const std::vector<ndn::Name>& names, uint64_t maxWireBytes,
             const ndn::Name& forwardingHint, std::function<void()> requireCurrent = {},
             std::chrono::steady_clock::time_point deadline =
               std::chrono::steady_clock::time_point::max(),
             PacketAdmission admit = {}) const;

private:
  using PacketName = std::function<ndn::Name(std::size_t)>;

  std::shared_ptr<FaceDataReferencePacketSpool>
  fetch(const RepoDataReference& routing, std::size_t packetCount,
        PacketName nameAt, bool exactSize, std::function<void()> requireCurrent,
        std::chrono::steady_clock::time_point deadline =
          std::chrono::steady_clock::time_point::max(),
        PacketAdmission admit = {}, std::shared_ptr<ManifestSourceWindow> sourceWindow = {},
        const std::vector<uint32_t>& plannedWireLengths = {},
        bool materializeWires = false, std::shared_ptr<void> spoolLease = {}) const;

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
