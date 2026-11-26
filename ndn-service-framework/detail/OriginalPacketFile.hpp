#pragma once

#include <ndn-cxx/name.hpp>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace ndn { class Data; }

namespace ndn_service_framework::detail {

/** Transient original packet owner. Wire and offsets live in private files;
 * only one packet is materialized at a time. Does not sign or own keys. */
class OriginalPacketFile
{
public:
  using NextPacket = std::function<std::vector<std::uint8_t>(std::uint64_t)>;
  static std::shared_ptr<const OriginalPacketFile> create(
    const std::filesystem::path& directory, const ndn::Name& dataPrefix,
    std::uint64_t segmentCount, std::uint64_t maxStorageBytes,
    const NextPacket& nextPacket, const std::function<void()>& requireActive = {});
  /** Prepare a private segmented file. It cannot be read until sealed. */
  static std::shared_ptr<OriginalPacketFile> createSegmented(
    const std::filesystem::path& directory, const ndn::Name& dataPrefix,
    std::uint64_t segmentCount, std::uint64_t maxStorageBytes);
  /** Write at most maxSegments and return whether the file is sealed. */
  bool appendSegments(std::uint64_t maxSegments, const NextPacket& nextPacket,
                      const std::function<void()>& requireActive = {});
  /** Create a request-owned file for exact packets without a common FinalBlock.
   * The caller owns name uniqueness, signer admission and the name/ordinal index.
   */
  static std::shared_ptr<OriginalPacketFile> createExact(
    const std::filesystem::path& directory, const ndn::Name& producer,
    std::uint64_t maxStorageBytes);
  /** Append admitted originals and return the first committed ordinal. */
  std::uint64_t appendExact(
    const std::vector<std::shared_ptr<const ndn::Data>>& originals,
    const std::function<void()>& requireActive = {});
  std::vector<std::uint8_t> readExactPacket(std::uint64_t ordinal,
                                           const ndn::Name& expectedName) const;
  ~OriginalPacketFile();
  OriginalPacketFile(const OriginalPacketFile&) = delete;
  OriginalPacketFile& operator=(const OriginalPacketFile&) = delete;
  std::vector<std::uint8_t> read(std::uint64_t segment) const;
  std::uint64_t storageBytes() const;

private:
  OriginalPacketFile() = default;
  std::vector<std::uint8_t> readWire(std::uint64_t ordinal) const;
  ndn::Name m_dataPrefix;
  std::uint64_t m_segmentCount = 0;
  std::uint64_t m_storageBytes = 0;
  std::uint64_t m_expectedSegments = 0;
  std::uint64_t m_segmentedBudget = 0;
  bool m_segmentsSealed = false;
  bool m_segmentsFailed = false;
  std::uint64_t m_exactBudget = 0;
  std::uint64_t m_exactPacketOffset = 0;
  std::FILE* m_packets = nullptr;
  std::FILE* m_index = nullptr;
  mutable std::mutex m_mutex;
};
} // namespace ndn_service_framework::detail
