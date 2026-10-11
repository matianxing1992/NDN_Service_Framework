#pragma once

#include <ndn-cxx/name.hpp>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

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
  ~OriginalPacketFile();
  OriginalPacketFile(const OriginalPacketFile&) = delete;
  OriginalPacketFile& operator=(const OriginalPacketFile&) = delete;
  std::vector<std::uint8_t> read(std::uint64_t segment) const;
  std::uint64_t storageBytes() const noexcept { return m_storageBytes; }

private:
  OriginalPacketFile() = default;
  ndn::Name m_dataPrefix;
  std::uint64_t m_segmentCount = 0;
  std::uint64_t m_storageBytes = 0;
  std::FILE* m_packets = nullptr;
  std::FILE* m_index = nullptr;
  mutable std::mutex m_mutex;
};
} // namespace ndn_service_framework::detail
