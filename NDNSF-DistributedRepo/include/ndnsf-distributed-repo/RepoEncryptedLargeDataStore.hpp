#pragma once

#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndn-service-framework/EncryptedLargeDataRangeStore.hpp"
#include "ndn-service-framework/ProtectedBlobManifest.hpp"
#include <ndn-cxx/data.hpp>

#include <openssl/evp.h>
#include <ndn-cxx/util/random.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <cstdio>
#include <unistd.h>

namespace ndnsf_distributed_repo {

/** Core ciphertext range-store adapter; no DI or cryptographic key ownership. */
class RepoEncryptedLargeDataStore final
  : public ndn_service_framework::EncryptedLargeDataRangeStore
{
  class Source final : public ndn_service_framework::EncryptedLargeDataRangeSource
  {
  public:
    Source(std::shared_ptr<RepoCore> repo, RepoObjectManifest manifest,
           ndn_service_framework::EncryptedLargeDataRetention retention)
      : m_repo(std::move(repo)), m_manifest(std::move(manifest)), m_retention(retention) {}
    ~Source() override
    {
      if (m_retention == ndn_service_framework::EncryptedLargeDataRetention::Durable)
        return;
      release();
    }
    bool isDurable() const noexcept override
    {
      return m_retention == ndn_service_framework::EncryptedLargeDataRetention::Durable;
    }
    void release() const noexcept override
    {
      // Committed original packets are shared immutable objects. A request
      // lease may stop reading them, but cannot delete another request's copy.
      if (m_manifest.objectType == "original-packet-set" && isDurable())
        return;
      if (m_released.exchange(true))
        return;
      // A same-name replacement belongs to a different transaction.
      try {
        m_repo->removeIfCurrent(m_manifest);
      }
      catch (...) {}
    }
    std::uint64_t size() const noexcept override
    {
      return m_manifest.objectType == "original-packet-set" ? m_manifest.envelopeBytes : m_manifest.size;
    }
    std::vector<std::uint8_t> readOriginalDataPacket(const ndn::Name& name) const override
    {
      if (m_manifest.objectType != "original-packet-set" || m_released.load())
        throw std::runtime_error("ORIGINAL_DATA_PACKET_UNSUPPORTED");
      const auto key = name.toUri(ndn::name::UriFormat::CANONICAL);
      const auto found = std::find(m_manifest.packetNames.begin(), m_manifest.packetNames.end(), key);
      if (found == m_manifest.packetNames.end())
        throw std::out_of_range("original packet is outside source");
      const auto index = static_cast<size_t>(found - m_manifest.packetNames.begin());
      if (m_manifest.packetOffsets.size() != m_manifest.packetNames.size() + 1)
        throw std::runtime_error("DURABLE_METADATA_INVALID");
      const auto wire = m_repo->getRangeIfCurrent(m_manifest,
        {m_manifest.packetOffsets[index], m_manifest.packetOffsets[index + 1] - m_manifest.packetOffsets[index]});
      const ndn::Data data(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
      if (data.getName() != name || data.wireEncode().size() != wire.size())
        throw std::runtime_error("DURABLE_PACKET_NAME_MISMATCH");
      return wire;
    }
    std::vector<std::uint8_t> read(std::uint64_t offset,
                                 std::uint64_t length) const override
    {
      if (m_manifest.objectType == "original-packet-set")
        throw std::runtime_error("ORIGINAL_PACKET_SOURCE_HAS_NO_ENVELOPE_RANGE");
      if (offset > size() || length > size() - offset || length > kWindow)
        throw std::out_of_range("encrypted Repo range exceeds bounds");
      if (length == 0)
        return {};
      return m_repo->getRangeIfCurrent(m_manifest, {offset, length});
    }
  private:
    std::shared_ptr<RepoCore> m_repo;
    RepoObjectManifest m_manifest;
    ndn_service_framework::EncryptedLargeDataRetention m_retention;
    mutable std::atomic<bool> m_released{false};
  };

public:
  static constexpr std::uint64_t kWindow = 1U << 20;
  explicit RepoEncryptedLargeDataStore(std::shared_ptr<RepoCore> repo)
    : m_repo(std::move(repo))
  {
    if (!m_repo)
      throw std::invalid_argument("encrypted Repo store requires RepoCore");
  }

  bool supportsOriginalDataPackets() const noexcept override
  {
    return m_repo->supportsOriginalPacketTransactions();
  }

  std::shared_ptr<const ndn_service_framework::EncryptedLargeDataRangeSource>
  commitOriginalDataPackets(
    const ndn::Name& root, const ndn_service_framework::EncryptedLargeDataCommitOptions& options,
    const std::function<std::optional<std::pair<ndn::Name, std::vector<uint8_t>>>()>& next,
    const std::function<void()>& requireActive = {}) override
  {
    using namespace ndn_service_framework;
    if (!supportsOriginalDataPackets())
      throw std::runtime_error("ORIGINAL_DATA_PACKET_UNSUPPORTED");
    if (options.retention != EncryptedLargeDataRetention::Durable ||
        options.packetStorageFormat != "original-packet-set" || !next ||
        root != ndn::Name(options.manifestName) || options.publicationIdentity.empty() ||
        options.protectionEpoch.empty() || options.keyReferenceId.empty() ||
        options.keyReferenceVersion.empty() || options.ciphertextManifestDigest.empty() ||
        options.servingLocator.empty() || options.contentDigest.empty() || options.plaintextSize == 0)
      throw std::invalid_argument("DURABLE_METADATA_INVALID");
    const auto producer = ProtectedBlobManifest::inferProducer(root);
    if (!producer) throw std::invalid_argument("DURABLE_MANIFEST_NAME_INVALID");
    auto file = std::unique_ptr<std::FILE, decltype(&std::fclose)>(std::tmpfile(), std::fclose);
    if (!file) throw std::runtime_error("cannot create original packet spool");
    auto digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
      throw std::runtime_error("cannot initialize original packet digest");
    RepoObjectManifest record;
    record.objectName = root.toUri(ndn::name::UriFormat::CANONICAL);
    record.objectType = "original-packet-set";
    record.operationId = "packet-set-" + std::to_string(ndn::random::generateSecureWord64()) +
      "-" + std::to_string(ndn::random::generateSecureWord64());
    record.publicationIdentity = options.publicationIdentity;
    record.protectionEpoch = options.protectionEpoch;
    record.keyReferenceId = options.keyReferenceId;
    record.keyReferenceVersion = options.keyReferenceVersion;
    record.ciphertextManifestDigest = options.ciphertextManifestDigest;
    record.servingLocator = options.servingLocator;
    record.contentDigest = options.contentDigest;
    record.plaintextSize = options.plaintextSize;
    record.dataPrefix = ndn::Name(options.dataPrefix).toUri(ndn::name::UriFormat::CANONICAL);
    record.packetOffsets.push_back(0);
    std::optional<ProtectedBlobManifest> manifest;
    while (const auto item = next()) {
      if (requireActive) requireActive();
      if (item->second.empty() || item->second.size() > ndn::MAX_NDN_PACKET_SIZE ||
          record.size > options.packetWireBytes || item->second.size() > options.packetWireBytes - record.size)
        throw std::invalid_argument("DURABLE_PACKET_SIZE_INVALID");
      const ndn::Data data(ndn::Block(ndn::span<const uint8_t>(item->second.data(), item->second.size())));
      if (data.getName() != item->first || data.wireEncode().size() != item->second.size())
        throw std::invalid_argument("DURABLE_PACKET_NAME_MISMATCH");
      if (!manifest) {
        if (item->first != root) throw std::invalid_argument("DURABLE_MANIFEST_ORDER_INVALID");
        const auto& content = data.getContent();
        const std::vector<uint8_t> payload(content.value(), content.value() + content.value_size());
        manifest = ProtectedBlobManifest::decode(payload, *producer, root);
        std::ostringstream plaintextHash;
        for (const auto byte : manifest->plaintextDigest)
          plaintextHash << std::hex << std::setw(2) << std::setfill('0') << unsigned(byte);
        if (manifest->dataPrefix != ndn::Name(record.dataPrefix) ||
            manifest->plaintextBytes != options.plaintextSize ||
            manifest->protectionEpoch != options.protectionEpoch ||
            manifest->keyReferenceId != options.keyReferenceId ||
            "sha256:" + sha256Hex(payload) != options.ciphertextManifestDigest ||
            "sha256:" + plaintextHash.str() != options.contentDigest)
          throw std::invalid_argument("DURABLE_MANIFEST_METADATA_MISMATCH");
        record.envelopeBytes = manifest->envelopeBytes;
      }
      else {
        const auto segment = record.packetNames.size() - 1;
        if (segment >= manifest->segmentCount ||
            item->first != ndn::Name(manifest->dataPrefix).appendSegment(segment) ||
            data.getFinalBlock() != ndn::name::Component::fromSegment(manifest->segmentCount - 1) ||
            data.getContent().value_size() != std::min(manifest->segmentBytes,
              manifest->envelopeBytes - segment * manifest->segmentBytes))
          throw std::invalid_argument("DURABLE_PACKET_GEOMETRY_INVALID");
      }
      if (std::fwrite(item->second.data(), 1, item->second.size(), file.get()) != item->second.size() ||
          EVP_DigestUpdate(digest.get(), item->second.data(), item->second.size()) != 1)
        throw std::runtime_error("original packet spool write failed");
      record.size += item->second.size();
      record.packetNames.push_back(item->first.toUri(ndn::name::UriFormat::CANONICAL));
      record.packetOffsets.push_back(record.size);
    }
    if (!manifest || record.packetNames.size() != manifest->segmentCount + 1 ||
        record.size != options.packetWireBytes || std::fflush(file.get()) != 0)
      throw std::invalid_argument("DURABLE_PACKET_SET_INCOMPLETE");
    std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
    unsigned hashSize = 0;
    if (EVP_DigestFinal_ex(digest.get(), hash.data(), &hashSize) != 1)
      throw std::runtime_error("original packet digest finalization failed");
    std::ostringstream hex;
    for (unsigned i = 0; i < hashSize; ++i)
      hex << std::hex << std::setw(2) << std::setfill('0') << unsigned(hash[i]);
    record.sha256 = hex.str();
    if (record.packetNames.size() > std::numeric_limits<uint32_t>::max())
      throw std::invalid_argument("DURABLE_PACKET_COUNT_INVALID");
    record.segmentCount = static_cast<uint32_t>(record.packetNames.size());
    auto publicationLock = m_repo->acquirePublicationLock();
    const auto committed = m_repo->commitOriginalPacketFile(record,
      "/proc/self/fd/" + std::to_string(::fileno(file.get())), requireActive);
    return std::make_shared<Source>(m_repo, committed, options.retention);
  }

  std::shared_ptr<const ndn_service_framework::EncryptedLargeDataRangeSource>
  commitFile(const std::string& name, const std::filesystem::path& file,
             std::uint64_t size, const std::function<void()>& requireActive = {}) override
  {
    return commitFile(name, file, size,
      ndn_service_framework::EncryptedLargeDataCommitOptions{}, requireActive);
  }

  std::shared_ptr<const ndn_service_framework::EncryptedLargeDataRangeSource>
  commitFile(const std::string& name, const std::filesystem::path& file,
             std::uint64_t size,
             const ndn_service_framework::EncryptedLargeDataCommitOptions& options,
             const std::function<void()>& requireActive = {}) override
  {
    if (options.packetStorageFormat != "encrypted-envelope-bytes")
      throw std::runtime_error("ORIGINAL_PACKET_STORAGE_REQUIRES_PACKET_COMMIT");
    if (requireActive) requireActive();
    if (name.empty() || name.front() != '/' || size == 0 ||
        std::filesystem::file_size(file) != size)
      throw std::invalid_argument("encrypted Repo source identity is invalid");
    if (options.retention == ndn_service_framework::EncryptedLargeDataRetention::Durable &&
        (options.publicationIdentity.empty() || options.protectionEpoch.empty() ||
         options.keyReferenceId.empty() || options.keyReferenceVersion.empty() ||
         options.ciphertextManifestDigest.empty() || options.servingLocator.empty() ||
         options.contentDigest.empty() || options.plaintextSize == 0))
      throw std::invalid_argument("DURABLE_METADATA_INVALID");
    std::ifstream input(file, std::ios::binary);
    if (!input)
      throw std::runtime_error("cannot open encrypted Repo input");
    auto context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(
      EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
      throw std::runtime_error("cannot initialize encrypted Repo digest");
    std::vector<std::uint8_t> buffer(kWindow);
    for (std::uint64_t offset = 0; offset < size;) {
      if (requireActive) requireActive();
      const auto length = std::min(kWindow, size - offset);
      input.read(reinterpret_cast<char*>(buffer.data()), length);
      if (static_cast<std::uint64_t>(input.gcount()) != length ||
          EVP_DigestUpdate(context.get(), buffer.data(), length) != 1)
        throw std::runtime_error("encrypted Repo input read failed");
      offset += length;
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned digestSize = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &digestSize) != 1)
      throw std::runtime_error("encrypted Repo digest finalization failed");
    std::ostringstream hex;
    for (unsigned i = 0; i < digestSize; ++i)
      hex << std::hex << std::setw(2) << std::setfill('0') << unsigned(digest[i]);
    RepoObjectManifest manifest;
    manifest.objectName = name;
    manifest.objectType = "encrypted-large-data-envelope";
    manifest.sha256 = hex.str();
    manifest.size = size;
    manifest.operationId = "encrypted-" + std::to_string(ndn::random::generateSecureWord64()) +
      "-" + std::to_string(ndn::random::generateSecureWord64());
    manifest.publicationIdentity = options.publicationIdentity;
    manifest.protectionEpoch = options.protectionEpoch;
    manifest.keyReferenceId = options.keyReferenceId;
    manifest.keyReferenceVersion = options.keyReferenceVersion;
    manifest.ciphertextManifestDigest = options.ciphertextManifestDigest;
    manifest.servingLocator = options.servingLocator;
    manifest.contentDigest = options.contentDigest;
    manifest.plaintextSize = options.plaintextSize;
    auto lock = m_repo->acquirePublicationLock();
    if (m_repo->has(name))
      throw std::runtime_error("encrypted Repo name already committed");
    try {
      input.clear();
      input.seekg(0);
      for (std::uint64_t offset = 0; offset < size;) {
        if (requireActive) requireActive();
        const auto length = std::min(kWindow, size - offset);
        buffer.resize(length);
        input.read(reinterpret_cast<char*>(buffer.data()), length);
        if (static_cast<std::uint64_t>(input.gcount()) != length)
          throw std::runtime_error("encrypted Repo input changed during commit");
        m_repo->putRangeIfAbsent(manifest, {offset, length}, buffer);
        offset += length;
      }
      if (requireActive) requireActive();
      const auto committed = m_repo->commitRangesIfOwned(manifest);
      if (requireActive) requireActive();
      if (committed.sha256 != manifest.sha256 || committed.size != size ||
          committed.operationId != manifest.operationId)
        throw std::runtime_error("encrypted Repo committed identity mismatch");
      // Exercise the normal authority read path before returning a lease.
      if (m_repo->getRangeIfCurrent(committed, {0, std::min(size, kWindow)}).empty())
        throw std::runtime_error("encrypted Repo commit is not readable");
      return std::make_shared<Source>(m_repo, committed, options.retention);
    }
    catch (...) {
      try { m_repo->abortRangesIfOwned(manifest); } catch (...) {}
      try { m_repo->removeIfCurrent(manifest); } catch (...) {}
      throw;
    }
  }

  std::optional<ndn_service_framework::EncryptedLargeDataLookupResult>
  lookupDurable(const std::string& publicationIdentity,
                const std::function<void()>& requireActive = {}) const override
  {
    if (publicationIdentity.empty())
      return std::nullopt;
    if (requireActive) requireActive();
    auto lock = m_repo->acquirePublicationLock();
    std::optional<ndn_service_framework::EncryptedLargeDataLookupResult> found;
    for (const auto& manifest : m_repo->list()) {
      if (requireActive) requireActive();
      if ((manifest.objectType != "encrypted-large-data-envelope" &&
           manifest.objectType != "original-packet-set") ||
          manifest.publicationIdentity != publicationIdentity)
        continue;
      if (manifest.protectionEpoch.empty() || manifest.keyReferenceId.empty() ||
          manifest.keyReferenceVersion.empty() || manifest.ciphertextManifestDigest.empty() ||
          manifest.servingLocator.empty() || manifest.contentDigest.empty() ||
          manifest.plaintextSize == 0 || manifest.size == 0)
        throw std::runtime_error("DURABLE_METADATA_INVALID");
      if (found)
        throw std::runtime_error("DURABLE_IDENTITY_CONFLICT");
      auto source = std::make_shared<Source>(
        m_repo, manifest, ndn_service_framework::EncryptedLargeDataRetention::Durable);
      found = ndn_service_framework::EncryptedLargeDataLookupResult{
        manifest.objectName, manifest.publicationIdentity, manifest.protectionEpoch,
        manifest.keyReferenceId, manifest.keyReferenceVersion,
        manifest.ciphertextManifestDigest, manifest.servingLocator,
        manifest.contentDigest, manifest.plaintextSize, std::move(source)};
      if (manifest.objectType == "original-packet-set") {
        if (manifest.envelopeBytes == 0 || manifest.dataPrefix.empty() ||
            manifest.packetOffsets.size() != manifest.packetNames.size() + 1 ||
            !m_repo->hasDataPacket(manifest.objectName))
          throw std::runtime_error("DURABLE_METADATA_INVALID");
        found->encryptedName = manifest.dataPrefix;
        found->packetStorageFormat = "original-packet-set";
        found->manifestName = manifest.objectName;
        found->dataPrefix = manifest.dataPrefix;
        found->packetWireBytes = manifest.size;
      }
    }
    return found;
  }

  bool supportsDurableRetention() const noexcept override { return true; }

private:
  std::shared_ptr<RepoCore> m_repo;
};

} // namespace ndnsf_distributed_repo
