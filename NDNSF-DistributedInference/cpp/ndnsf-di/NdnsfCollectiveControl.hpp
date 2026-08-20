#ifndef NDNSF_DISTRIBUTED_INFERENCE_NDNSF_COLLECTIVE_CONTROL_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NDNSF_COLLECTIVE_CONTROL_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ndnsf::di {

inline constexpr char NDNSF_DATA_V1[] = "NDNSF_DATA_V1";

/**
 * The immutable identity and bounds of one cross-Provider Data segment.
 *
 * This is deliberately independent of the model adapter.  Provider-group
 * planning owns the values; the transport only authenticates and bounds them.
 */
struct CollectiveSegmentDescriptor
{
  std::string requestId;
  std::string attemptId;
  std::string planDigest;
  std::string groupId;
  std::uint64_t epoch = 0;
  std::uint64_t operationIndex = 0;
  std::string operationKind;
  std::string producerRank;
  std::string tensorDigest;
  std::string manifestDigest;
  std::uint64_t segmentNo = 0;
  std::uint64_t segmentCount = 0;
  std::uint64_t totalBytes = 0;
  std::uint64_t segmentSize = 0;
  std::uint64_t noProgressMs = 0;
  std::uint64_t hardDeadlineMs = 0;

  void validate() const;

  /** Canonical associated-data bytes, including the complete Data name. */
  std::vector<std::uint8_t>
  associatedData(const std::string& dataName) const;
};

struct NdnsfDataV1Segment
{
  std::string magic = NDNSF_DATA_V1;
  CollectiveSegmentDescriptor descriptor;
  std::string dataName;
  std::vector<std::uint8_t> nonce;
  std::vector<std::uint8_t> ciphertext;
  std::vector<std::uint8_t> authTag;
  std::vector<std::uint8_t> hmac;

  void validate() const;

  std::vector<std::uint8_t>
  wireEncode() const;

  static NdnsfDataV1Segment
  wireDecode(const std::vector<std::uint8_t>& wire);
};

class NdnsfCollectiveControl
{
public:
  static NdnsfDataV1Segment
  seal(const CollectiveSegmentDescriptor& descriptor,
       const std::string& dataName,
       const std::vector<std::uint8_t>& epochKey,
       const std::vector<std::uint8_t>& plaintext);

  /**
   * Seal with a coordinator-derived operation key and deterministic nonce.
   * The legacy seal() remains random-nonce compatible for existing callers;
   * cross-Provider NDNSF_DATA_V1 must use this overload.
   */
  static NdnsfDataV1Segment
  sealWithNonce(const CollectiveSegmentDescriptor& descriptor,
                const std::string& dataName,
                const std::vector<std::uint8_t>& operationKey,
                const std::vector<std::uint8_t>& nonce,
                const std::vector<std::uint8_t>& plaintext);

  static std::vector<std::uint8_t>
  open(const NdnsfDataV1Segment& segment,
       const std::vector<std::uint8_t>& epochKey,
       const std::string& expectedDataName = {});
};

/**
 * Bounded receiver-side duplicate/replay state for one operation epoch.
 * `accept` authenticates and decrypts before changing the window.
 */
class DataSegmentReplayWindow
{
public:
  enum class Result
  {
    Accepted,
    Duplicate,
  };

  DataSegmentReplayWindow(std::size_t maxSegments, std::size_t maxBytes);

  Result
  accept(const NdnsfDataV1Segment& segment,
         const std::vector<std::uint8_t>& epochKey,
         const std::string& expectedDataName = {});

  bool complete() const;
  std::size_t acceptedSegments() const noexcept;
  std::size_t acceptedBytes() const noexcept;

private:
  std::size_t m_maxSegments;
  std::size_t m_maxBytes;
  bool m_bound = false;
  std::uint64_t m_epoch = 0;
  std::uint64_t m_operationIndex = 0;
  std::uint64_t m_segmentCount = 0;
  std::uint64_t m_totalBytes = 0;
  std::size_t m_acceptedBytes = 0;
  std::map<std::uint64_t, std::string> m_seenDigests;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NDNSF_COLLECTIVE_CONTROL_HPP
