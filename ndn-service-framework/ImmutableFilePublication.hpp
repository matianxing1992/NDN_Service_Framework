#pragma once

#include "ProducerDataNaming.hpp"
#include <ndn-cxx/data.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <openssl/sha.h>
#include <array>
#include <algorithm>
#include <vector>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <limits>
#include <chrono>
#include <cstdlib>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

namespace ndn_service_framework {

/** Disk-owned, identity-signed file segments. Construction is streaming;
 * retransmission and reopening never regenerate a published Data packet.
 * The caller supplies an authenticated local producer identity, not a delegate.
 * Access packet() from one serving executor. Cache files are private local state.
 */
class ImmutableFilePublication
{
public:
  ImmutableFilePublication(const ndn::Name& base, const std::string& source,
                           ndn::KeyChain& keyChain, const ndn::Name& identity,
                           size_t segmentBytes, int freshnessMs,
                           const std::filesystem::path& cacheDirectory)
    : m_base(base)
    , m_certificate(keyChain.getPib().getIdentity(identity)
                      .getDefaultKey().getDefaultCertificate())
  {
    if (identity.empty() || base.empty() || !identity.isPrefixOf(base) ||
        segmentBytes == 0 || segmentBytes > 8000 || freshnessMs < 0 ||
        cacheDirectory.empty()) {
      throw std::invalid_argument("invalid immutable file publication owner or geometry");
    }
    checkNamingNameSize(base);
    std::ifstream input(source, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open immutable publication source");
    SHA256_CTX sourceHash;
    SHA256_Init(&sourceHash);
    std::array<char, 65536> block{};
    while (input.read(block.data(), block.size()) || input.gcount() != 0) {
      const auto n = static_cast<size_t>(input.gcount());
      SHA256_Update(&sourceHash, block.data(), n);
      m_size += n;
    }
    if (!input.eof()) throw std::runtime_error("cannot hash immutable publication source");
    std::array<uint8_t, SHA256_DIGEST_LENGTH> digest{};
    SHA256_Final(digest.data(), &sourceHash);
    // C02 callers bind plaintext identity before the representation tail.
    if (base.size() >= 4 && base[-3] == ndn::name::Component("REPRESENTATION") &&
        base[-1] == ndn::name::Component("DATA") &&
        (base[-4].value_size() != digest.size() ||
         !std::equal(digest.begin(), digest.end(), base[-4].value())))
      throw std::runtime_error("file payload does not match its published content identity");
    m_count = std::max<uint64_t>(1, (m_size + segmentBytes - 1) / segmentBytes);
    if (m_count > (static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) - 24) / slotBytes)
      throw std::runtime_error("immutable publication exceeds disk offset bounds");
    const auto& baseWire = base.wireEncode();
    const auto& certificateWire = m_certificate.wireEncode();
    const auto profile = std::to_string(segmentBytes) + ":" + std::to_string(freshnessMs);
    const auto owner = computeNamingDigest("ndnsf-naming/file-publication", {
      {1, 1, {baseWire.begin(), baseWire.end()}},
      {2, 4, {certificateWire.begin(), certificateWire.end()}},
      {3, 4, {digest.begin(), digest.end()}},
      {4, 2, {profile.begin(), profile.end()}}
    });
    std::filesystem::create_directories(cacheDirectory);
    m_path = cacheDirectory / (owner + ".packets");
    // Serialize cache admission as well as creation: concurrent producers cannot
    // each pass a capacity check against the same remaining space.
    FileLock capacityLock(cacheDirectory / ".capacity.lock");
    if (!std::filesystem::exists(m_path)) {
      uint64_t used = 0;
      size_t entries = 0;
      for (const auto& item : std::filesystem::directory_iterator(cacheDirectory)) {
        ++entries;
        if (item.is_regular_file()) used += item.file_size();
      }
      uint64_t limit = uint64_t{4} << 30;
      if (const char* value = std::getenv("NDNSF_FILE_PUBLICATION_CACHE_MAX_BYTES")) {
        const std::string text(value);
        if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
          throw std::invalid_argument("invalid file publication cache capacity");
        limit = std::stoull(text);
      }
      const uint64_t required = 24 + m_count * slotBytes;
      const uint64_t metadata = std::filesystem::exists(cacheDirectory / ".version") ? 0 : 8;
      if (entries > 8192 - 3 || limit == 0 || used > limit ||
          metadata > limit - used || required > limit - used - metadata)
        throw std::runtime_error("immutable file publication cache capacity exceeded");
    }
    FileLock lock(cacheDirectory / (owner + ".lock"));
    if (!std::filesystem::exists(m_path)) {
      const auto temporary = std::filesystem::path(m_path.string() + ".partial");
      try {
        const int fd = ::open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600);
        if (fd < 0) throw std::runtime_error("cannot create immutable publication cache");
        ::close(fd);
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        // The capacity lock also protects a durable version allocator. Different
        // signing contexts must not collide when created in the same millisecond.
        uint64_t previousVersion = 0;
        const auto versionPath = cacheDirectory / ".version";
        if (std::filesystem::exists(versionPath)) {
          std::ifstream previous(versionPath, std::ios::binary);
          previousVersion = readNumber(previous);
          if (previous.peek() != std::char_traits<char>::eof())
            throw std::runtime_error("invalid immutable publication version allocator");
        }
        if (previousVersion == std::numeric_limits<uint64_t>::max())
          throw std::runtime_error("immutable publication version exhausted");
        const auto version = std::max(previousVersion + 1, static_cast<uint64_t>(
          ndn::time::toUnixTimestamp(ndn::time::system_clock::now()).count()));
        const auto versionTemporary = cacheDirectory / ".version.partial";
        { std::ofstream counter(versionTemporary, std::ios::binary | std::ios::trunc);
          writeNumber(counter, version); counter.close();
          if (!counter) throw std::runtime_error("cannot persist immutable publication version"); }
        const int counterFd = ::open(versionTemporary.c_str(), O_RDONLY | O_NOFOLLOW);
        if (counterFd < 0) throw std::runtime_error("cannot sync immutable publication version");
        const auto counterSync = ::fsync(counterFd);
        ::close(counterFd);
        if (counterSync != 0) throw std::runtime_error("cannot sync immutable publication version");
        std::filesystem::rename(versionTemporary, versionPath);
        output.write("NSFFILE1", 8);
        writeNumber(output, version);
        writeNumber(output, m_count);
        m_versioned = base;
        m_versioned.appendVersion(version);
        input.clear();
        input.seekg(0);
        SHA256_Init(&sourceHash);
        std::vector<uint8_t> content(segmentBytes);
        for (uint64_t i = 0; i < m_count; ++i) {
          const auto n = static_cast<size_t>(std::min<uint64_t>(segmentBytes, m_size - i * segmentBytes));
          input.read(reinterpret_cast<char*>(content.data()), n);
          if (static_cast<size_t>(input.gcount()) != n)
            throw std::runtime_error("immutable publication source changed during signing");
          SHA256_Update(&sourceHash, content.data(), n);
          ndn::Name name = m_versioned;
          name.appendSegment(i);
          ndn::Data data(name);
          data.setContent(ndn::span<const uint8_t>(content.data(), n));
          data.setFreshnessPeriod(ndn::time::milliseconds(freshnessMs));
          data.setFinalBlock(ndn::name::Component::fromSegment(m_count - 1));
          const auto signingStarted = std::chrono::steady_clock::now();
          keyChain.sign(data, ndn::security::signingByKey(m_certificate.getKeyName()));
          m_signingNanoseconds += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - signingStarted).count());
          const auto& wire = data.wireEncode();
          if (wire.size() > maxWireBytes) throw std::runtime_error("immutable publication packet exceeds wire bound");
          output.seekp(24 + i * slotBytes);
          writeNumber(output, wire.size());
          output.write(reinterpret_cast<const char*>(wire.data()), wire.size());
          if (!output) throw std::runtime_error("cannot write immutable publication cache");
        }
        std::array<uint8_t, SHA256_DIGEST_LENGTH> finalDigest{};
        SHA256_Final(finalDigest.data(), &sourceHash);
        if (finalDigest != digest || input.peek() != std::char_traits<char>::eof())
          throw std::runtime_error("immutable publication source changed during signing");
        output.close();
        if (!output) throw std::runtime_error("cannot finish immutable publication cache");
        std::filesystem::resize_file(temporary, 24 + m_count * slotBytes);
        const int syncFd = ::open(temporary.c_str(), O_RDONLY | O_NOFOLLOW);
        if (syncFd < 0) throw std::runtime_error("cannot sync immutable publication cache");
        const auto syncResult = ::fsync(syncFd);
        ::close(syncFd);
        if (syncResult != 0) throw std::runtime_error("cannot sync immutable publication cache");
        std::filesystem::rename(temporary, m_path);
        const int dirFd = ::open(cacheDirectory.c_str(), O_DIRECTORY | O_RDONLY);
        if (dirFd < 0) throw std::runtime_error("cannot sync immutable publication directory");
        const auto dirSync = ::fsync(dirFd);
        ::close(dirFd);
        if (dirSync != 0) throw std::runtime_error("cannot sync immutable publication directory");
      }
      catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
      }
    }
    if (std::filesystem::file_size(m_path) != 24 + m_count * slotBytes)
      throw std::runtime_error("invalid immutable publication cache size");
    m_packets.open(m_path, std::ios::binary);
    std::array<char, 8> magic{};
    m_packets.read(magic.data(), magic.size());
    if (std::string(magic.data(), magic.size()) != "NSFFILE1")
      throw std::runtime_error("invalid immutable publication cache header");
    m_versioned = base;
    m_versioned.appendVersion(readNumber(m_packets));
    if (readNumber(m_packets) != m_count)
      throw std::runtime_error("invalid immutable publication cache count");
  }

  const ndn::Name& versionedName() const { return m_versioned; }
  uint64_t fileSize() const { return m_size; }
  uint64_t segmentCount() const { return m_count; }
  uint64_t signingNanoseconds() const { return m_signingNanoseconds; }
  std::vector<uint8_t> publicKeyDer() const
  {
    const auto key = m_certificate.getPublicKey();
    return {key.begin(), key.end()};
  }

  ndn::Data packet(uint64_t segment)
  {
    if (segment >= m_count) throw std::out_of_range("immutable publication segment");
    m_packets.clear();
    m_packets.seekg(24 + segment * slotBytes);
    const auto length = readNumber(m_packets);
    if (length == 0 || length > maxWireBytes) throw std::runtime_error("invalid cached Data wire length");
    std::vector<uint8_t> wire(length);
    m_packets.read(reinterpret_cast<char*>(wire.data()), length);
    if (!m_packets) throw std::runtime_error("truncated cached Data wire");
    ndn::Data data(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
    ndn::Name expected = m_versioned;
    expected.appendSegment(segment);
    if (data.wireEncode().size() != wire.size() || data.getName() != expected ||
        data.getFinalBlock() != ndn::name::Component::fromSegment(m_count - 1) ||
        !ndn::security::verifySignature(data, m_certificate))
      throw std::runtime_error("invalid cached immutable publication Data");
    return data;
  }

private:
  struct FileLock {
    int fd;
    explicit FileLock(const std::filesystem::path& path)
      : fd(::open(path.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600))
    {
      if (fd < 0) throw std::runtime_error("cannot open immutable publication lock");
      if (::flock(fd, LOCK_EX) != 0) { ::close(fd); throw std::runtime_error("cannot lock immutable publication"); }
    }
    ~FileLock() { ::flock(fd, LOCK_UN); ::close(fd); }
  };
  static void writeNumber(std::ostream& out, uint64_t value)
  {
    for (unsigned i = 0; i < 8; ++i) out.put(static_cast<char>((value >> (8 * i)) & 255));
  }
  static uint64_t readNumber(std::istream& in)
  {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
      const int byte = in.get();
      if (byte == std::char_traits<char>::eof()) throw std::runtime_error("truncated immutable publication metadata");
      value |= static_cast<uint64_t>(byte) << (8 * i);
    }
    return value;
  }
  static constexpr uint64_t maxWireBytes = 8800;
  static constexpr uint64_t slotBytes = 8 + maxWireBytes;
  ndn::Name m_base;
  ndn::Name m_versioned;
  ndn::security::Certificate m_certificate;
  std::filesystem::path m_path;
  std::ifstream m_packets;
  uint64_t m_size = 0;
  uint64_t m_count = 0;
  uint64_t m_signingNanoseconds = 0;
};
} // namespace ndn_service_framework
