#pragma once

#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndnsf-distributed-repo/FilesystemArtifactStore.hpp"
#include "ndn-service-framework/EncryptedLargeDataRangeStore.hpp"
#include "ndn-service-framework/ProtectedBlobManifest.hpp"
#include <ndn-cxx/data.hpp>

#include <openssl/evp.h>
#include <ndn-cxx/util/random.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <set>
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
           ndn_service_framework::EncryptedLargeDataRetention retention,
           std::shared_ptr<SqliteArtifactMetadataStore> journal = {},
           std::shared_ptr<std::mutex> writerGate = {}, std::string repoNode = {},
           std::optional<RepoUserStorageClaim> claim = {})
      : m_repo(std::move(repo)), m_manifest(std::move(manifest)), m_retention(retention)
      , m_journal(std::move(journal)), m_writerGate(std::move(writerGate))
      , m_repoNode(std::move(repoNode)), m_claim(std::move(claim)) {}
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
    void requireCurrentDurableStorageOwner(const ndn::Name& manifestName,
                                          const std::string& expectedOwnerIdentity) const override
    {
      requireOwnerBinding(manifestName, expectedOwnerIdentity);
      auto gate = lockWriterGate(m_writerGate);
      requireCurrentOwnerUnlocked();
    }
    void withCurrentDurableStorageOwner(const ndn::Name& manifestName,
                                        const std::string& expectedOwnerIdentity,
                                        const std::function<void()>& enqueue) const override
    {
      if (!enqueue) throw std::invalid_argument("DURABLE_ENQUEUE_REQUIRED");
      requireOwnerBinding(manifestName, expectedOwnerIdentity);
      auto gate = lockWriterGate(m_writerGate);
      requireCurrentOwnerUnlocked();
      enqueue();
    }
    void release() const noexcept override
    {
      // Releasing a serving lease cannot delete a durable object. Persistent
      // cleanup belongs to the explicitly authorized storage owner.
      if (isDurable())
        return;
      if (m_released.exchange(true))
        return;
      // A same-name replacement belongs to a different transaction.
      try {
        auto gate = lockWriterGate(m_writerGate);
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
      auto gate = lockWriterGate(m_writerGate);
      requireCurrentOwnerUnlocked();
      return readOriginalDataPacketUnlocked(name);
    }
    std::vector<ndn::Name> auxiliaryDataPacketNames() const override
    {
      auto gate = lockWriterGate(m_writerGate);
      requireCurrentOwnerUnlocked();
      if (m_manifest.objectType != "original-packet-set") return {};
      const ndn::Name root(m_manifest.objectName);
      const auto producer = ndn_service_framework::ProtectedBlobManifest::inferProducer(root);
      if (!producer) throw std::runtime_error("DURABLE_MANIFEST_NAME_INVALID");
      const auto wire = readOriginalDataPacketUnlocked(root);
      const ndn::Data data(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
      const auto& content = data.getContent();
      const auto manifest = ndn_service_framework::ProtectedBlobManifest::decode(
        std::vector<uint8_t>(content.value(), content.value() + content.value_size()), *producer, root);
      if (manifest.segmentCount >= m_manifest.packetNames.size() ||
          m_manifest.packetNames.size() - manifest.segmentCount - 1 >
            ndn_service_framework::EncryptedLargeDataCommitOptions::MaxAuxiliaryPacketCount)
        throw std::runtime_error("DURABLE_AUXILIARY_PACKET_INDEX_INVALID");
      const auto bodyCount = static_cast<size_t>(manifest.segmentCount) + 1;
      if (m_manifest.packetOffsets[bodyCount] > m_manifest.size ||
          m_manifest.size - m_manifest.packetOffsets[bodyCount] >
            ndn_service_framework::EncryptedLargeDataCommitOptions::MaxAuxiliaryPacketWireBytes)
        throw std::runtime_error("DURABLE_AUXILIARY_PACKET_INDEX_INVALID");
      std::vector<ndn::Name> names;
      std::set<ndn::Name> unique;
      for (size_t i = bodyCount; i < m_manifest.packetNames.size(); ++i) {
        ndn::Name name(m_manifest.packetNames[i]);
        if (!producer->isPrefixOf(name) || name.size() <= producer->size() || name == root ||
            manifest.dataPrefix.isPrefixOf(name) || !unique.insert(name).second)
          throw std::runtime_error("DURABLE_AUXILIARY_PACKET_INDEX_INVALID");
        names.push_back(std::move(name));
      }
      return names;
    }
    std::vector<std::uint8_t> read(std::uint64_t offset,
                                 std::uint64_t length) const override
    {
      auto gate = lockWriterGate(m_writerGate);
      requireCurrentOwnerUnlocked();
      if (m_manifest.objectType == "original-packet-set")
        throw std::runtime_error("ORIGINAL_PACKET_SOURCE_HAS_NO_ENVELOPE_RANGE");
      if (offset > size() || length > size() - offset || length > kWindow)
        throw std::out_of_range("encrypted Repo range exceeds bounds");
      if (length == 0)
        return {};
      return m_repo->getRangeIfCurrent(m_manifest, {offset, length});
    }
  private:
    // The caller already holds the writer gate and has checked the owner claim.
    std::vector<std::uint8_t> readOriginalDataPacketUnlocked(const ndn::Name& name) const
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
    void requireOwnerBinding(const ndn::Name& manifestName,
                             const std::string& expectedOwnerIdentity) const
    {
      if (manifestName.empty() || expectedOwnerIdentity.empty() ||
          !isDurable() || !m_journal || !m_writerGate || !m_claim)
        throw std::runtime_error("DURABLE_OWNER_BINDING_REQUIRED");
      const ndn::Name owner(expectedOwnerIdentity);
      if (owner.empty() || owner.toUri(ndn::name::UriFormat::ALTERNATE) != expectedOwnerIdentity ||
          expectedOwnerIdentity != m_claim->ownerIdentity ||
          manifestName != ndn::Name(m_manifest.objectName))
        throw std::runtime_error("DURABLE_OWNER_BINDING_MISMATCH");
    }
    void requireCurrentOwnerUnlocked() const
    {
      if (!m_journal || !isDurable()) return;
      if (!m_claim) throw std::runtime_error("DURABLE_OWNER_BINDING_REQUIRED");
      const auto current = m_journal->lookupCommittedUserStorageClaim(
        m_claim->ownerIdentity, m_repoNode, m_claim->object.objectName, m_claim->ownerEpoch);
      if (!current || !sameObject(current->object, m_claim->object) ||
          !sameObject(measureOwnedObject(*m_repo, m_manifest, false), m_claim->object))
        throw std::runtime_error("DURABLE_OWNER_CLAIM_UNAVAILABLE");
    }
    std::shared_ptr<RepoCore> m_repo;
    RepoObjectManifest m_manifest;
    ndn_service_framework::EncryptedLargeDataRetention m_retention;
    mutable std::atomic<bool> m_released{false};
    std::shared_ptr<SqliteArtifactMetadataStore> m_journal;
    std::shared_ptr<std::mutex> m_writerGate;
    std::string m_repoNode;
    std::optional<RepoUserStorageClaim> m_claim;
  };

public:
  static constexpr std::uint64_t kWindow = 1U << 20;
  explicit RepoEncryptedLargeDataStore(std::shared_ptr<RepoCore> repo)
    : m_repo(std::move(repo))
  {
    if (!m_repo)
      throw std::invalid_argument("encrypted Repo store requires RepoCore");
  }

  RepoEncryptedLargeDataStore(std::shared_ptr<RepoCore> repo,
    std::shared_ptr<SqliteArtifactMetadataStore> journal, std::string repoNode,
    std::shared_ptr<std::mutex> physicalWriterGate)
    : m_repo(std::move(repo)), m_journal(std::move(journal))
    , m_repoNode(std::move(repoNode)), m_writerGate(std::move(physicalWriterGate))
  {
    if (!m_repo || !m_journal || !m_writerGate || m_repoNode.empty() ||
        ndn::Name(m_repoNode).toUri(ndn::name::UriFormat::ALTERNATE) != m_repoNode ||
        m_repo->catalogStatus().repoNode != m_repoNode ||
        m_journal->schemaGeneration() != 16 || !m_journal->artifactWritesEnabled())
      throw std::invalid_argument("repo-user-storage-adapter-resources-invalid");
  }

  bool supportsOriginalDataPackets() const noexcept override
  {
    return m_repo->supportsOriginalPacketTransactions();
  }
  bool supportsAuxiliaryDataPackets() const noexcept override
  {
    return supportsOriginalDataPackets();
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
    if (options.auxiliaryPacketNames.size() > EncryptedLargeDataCommitOptions::MaxAuxiliaryPacketCount)
      throw std::invalid_argument("DURABLE_AUXILIARY_PACKET_LIMIT");
    std::set<ndn::Name> auxiliaryNames;
    for (const auto& name : options.auxiliaryPacketNames) {
      if (!producer->isPrefixOf(name) || name.size() <= producer->size() || name == root ||
          ndn::Name(options.dataPrefix).isPrefixOf(name) || name.wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE ||
          !auxiliaryNames.insert(name).second)
        throw std::invalid_argument("DURABLE_AUXILIARY_PACKET_NAME_INVALID");
    }
    if (options.storageActor && !m_journal)
      throw std::runtime_error("DURABLE_OWNER_JOURNAL_REQUIRED");
    if (m_journal) requireOwnedGrant(options, root.toUri(ndn::name::UriFormat::ALTERNATE));
    auto file = std::unique_ptr<std::FILE, decltype(&std::fclose)>(std::tmpfile(), std::fclose);
    if (!file) throw std::runtime_error("cannot create original packet spool");
    auto digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
      throw std::runtime_error("cannot initialize original packet digest");
    RepoObjectManifest record;
    record.objectName = root.toUri(m_journal ? ndn::name::UriFormat::ALTERNATE :
                                             ndn::name::UriFormat::CANONICAL);
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
    std::uint64_t auxiliaryWireBytes = 0;
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
        if (segment < manifest->segmentCount) {
          if (item->first != ndn::Name(manifest->dataPrefix).appendSegment(segment) ||
            data.getFinalBlock() != ndn::name::Component::fromSegment(manifest->segmentCount - 1) ||
            data.getContent().value_size() != std::min(manifest->segmentBytes,
              manifest->envelopeBytes - segment * manifest->segmentBytes))
            throw std::invalid_argument("DURABLE_PACKET_GEOMETRY_INVALID");
        }
        else {
          const auto index = segment - manifest->segmentCount;
          if (index >= options.auxiliaryPacketNames.size() || item->first != options.auxiliaryPacketNames[index] ||
              !data.getSignatureInfo().hasKeyLocator() ||
              data.getSignatureInfo().getKeyLocator().getType() != ndn::tlv::Name ||
              !producer->isPrefixOf(data.getSignatureInfo().getKeyLocator().getName()) ||
              item->second.size() > EncryptedLargeDataCommitOptions::MaxAuxiliaryPacketWireBytes - auxiliaryWireBytes)
            throw std::invalid_argument("DURABLE_AUXILIARY_PACKET_INVALID");
          auxiliaryWireBytes += item->second.size();
        }
      }
      if (std::fwrite(item->second.data(), 1, item->second.size(), file.get()) != item->second.size() ||
          EVP_DigestUpdate(digest.get(), item->second.data(), item->second.size()) != 1)
        throw std::runtime_error("original packet spool write failed");
      record.size += item->second.size();
      record.packetNames.push_back(item->first.toUri(ndn::name::UriFormat::CANONICAL));
      record.packetOffsets.push_back(record.size);
    }
    if (!manifest || record.packetNames.size() - 1 < manifest->segmentCount ||
        record.packetNames.size() - 1 - manifest->segmentCount != options.auxiliaryPacketNames.size() ||
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
    auto writerLock = acquireWriterLock();
    auto publicationLock = m_repo->acquirePublicationLock();
    std::optional<RepoUserStorageWrite> admitted;
    if (m_journal) admitted = admitOwnedCommit(record, options);
    const auto check = [&] {
      if (requireActive) requireActive();
      if (m_journal) requireOwnedGrant(options, record.objectName);
    };
    try {
      const auto committed = m_repo->commitOriginalPacketFile(record,
        "/proc/self/fd/" + std::to_string(::fileno(file.get())), check);
      if (admitted) return finishOwnedCommit(committed, *admitted, options, requireActive);
      return std::make_shared<Source>(m_repo, committed, options.retention);
    }
    catch (...) {
      if (admitted) rollbackOwnedCommit(record, *admitted);
      throw;
    }
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
    if (options.retention == ndn_service_framework::EncryptedLargeDataRetention::Durable &&
        options.storageActor && !m_journal)
      throw std::runtime_error("DURABLE_OWNER_JOURNAL_REQUIRED");
    const bool owned = m_journal &&
      options.retention == ndn_service_framework::EncryptedLargeDataRetention::Durable;
    if (owned) requireOwnedGrant(options, ndn::Name(name).toUri(ndn::name::UriFormat::ALTERNATE));
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
    manifest.objectName = owned ? ndn::Name(name).toUri(ndn::name::UriFormat::ALTERNATE) : name;
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
    auto writerLock = acquireWriterLock();
    auto lock = m_repo->acquirePublicationLock();
    std::optional<RepoUserStorageWrite> admitted;
    if (owned) admitted = admitOwnedCommit(manifest, options);
    if (!owned && m_repo->has(name))
      throw std::runtime_error("encrypted Repo name already committed");
    const auto check = [&] {
      if (requireActive) requireActive();
      if (owned) requireOwnedGrant(options, manifest.objectName);
    };
    try {
      if (owned && m_repo->probeObjectMetadata(manifest.objectName).manifestPresent) {
        check();
        const auto current = m_repo->confirmDurablePublicationIfCurrent(manifest);
        return finishOwnedCommit(current, *admitted, options, requireActive);
      }
      input.clear();
      input.seekg(0);
      for (std::uint64_t offset = 0; offset < size;) {
        check();
        const auto length = std::min(kWindow, size - offset);
        buffer.resize(length);
        input.read(reinterpret_cast<char*>(buffer.data()), length);
        if (static_cast<std::uint64_t>(input.gcount()) != length)
          throw std::runtime_error("encrypted Repo input changed during commit");
        m_repo->putRangeIfAbsent(manifest, {offset, length}, buffer);
        offset += length;
      }
      check();
      const auto committed = m_repo->commitRangesIfOwned(manifest);
      check();
      if (committed.sha256 != manifest.sha256 || committed.size != size ||
          committed.operationId != manifest.operationId)
        throw std::runtime_error("encrypted Repo committed identity mismatch");
      // Exercise the normal authority read path before returning a lease.
      if (m_repo->getRangeIfCurrent(committed, {0, std::min(size, kWindow)}).empty())
        throw std::runtime_error("encrypted Repo commit is not readable");
      if (admitted) return finishOwnedCommit(committed, *admitted, options, requireActive);
      return std::make_shared<Source>(m_repo, committed, options.retention,
        m_journal, m_writerGate, m_repoNode);
    }
    catch (...) {
      if (admitted) rollbackOwnedCommit(manifest, *admitted);
      else {
        try { m_repo->abortRangesIfOwned(manifest); } catch (...) {}
        try { m_repo->removeIfCurrent(manifest); } catch (...) {}
      }
      throw;
    }
  }

  std::optional<ndn_service_framework::EncryptedLargeDataLookupResult>
  lookupDurable(const std::string& publicationIdentity,
                const std::function<void()>& requireActive = {}) const override
  {
    auto writerLock = acquireWriterLock();
    auto lock = m_repo->acquirePublicationLock();
    return lookupDurableUnlocked(publicationIdentity, requireActive);
  }

  std::shared_ptr<const ndn_service_framework::EncryptedLargeDataRangeSource>
  retainDurableSource(const ndn_service_framework::EncryptedLargeDataLookupResult& candidate,
    const ndn_service_framework::EncryptedLargeDataCommitOptions& options,
    const std::function<void()>& requireActive = {}) override
  {
    if (options.retention == ndn_service_framework::EncryptedLargeDataRetention::Durable &&
        options.storageActor && !m_journal)
      throw std::runtime_error("DURABLE_OWNER_JOURNAL_REQUIRED");
    if (!m_journal) {
      if (requireActive) requireActive();
      return candidate.source;
    }
    auto writerLock = acquireWriterLock();
    auto lock = m_repo->acquirePublicationLock();
    if (options.retention != ndn_service_framework::EncryptedLargeDataRetention::Durable)
      throw std::invalid_argument("DURABLE_METADATA_INVALID");
    const auto current = lookupDurableUnlocked(options.publicationIdentity, requireActive);
    if (!current) throw std::runtime_error("DURABLE_PUBLICATION_NOT_FOUND");
    const auto objectName = ndn::Name(current->packetStorageFormat == "original-packet-set" ?
      current->manifestName : current->encryptedName).toUri(ndn::name::UriFormat::ALTERNATE);
    auto manifest = m_repo->probeObjectMetadata(objectName).manifest;
    if (manifest.publicationIdentity != options.publicationIdentity ||
        manifest.protectionEpoch != options.protectionEpoch ||
        manifest.keyReferenceId != options.keyReferenceId ||
        manifest.keyReferenceVersion != options.keyReferenceVersion ||
        manifest.ciphertextManifestDigest != options.ciphertextManifestDigest ||
        manifest.servingLocator != options.servingLocator ||
        manifest.contentDigest != options.contentDigest || manifest.plaintextSize != options.plaintextSize ||
        (manifest.objectType == "original-packet-set") != (options.packetStorageFormat == "original-packet-set") ||
        (manifest.objectType == "original-packet-set" &&
          (ndn::Name(options.manifestName) != ndn::Name(manifest.objectName) ||
           ndn::Name(options.dataPrefix) != ndn::Name(manifest.dataPrefix) ||
           options.packetWireBytes != manifest.size)))
      throw std::runtime_error("DURABLE_METADATA_INVALID");
    const auto admitted = admitOwnedCommit(manifest, options);
    try {
      if (requireActive) requireActive();
      requireOwnedGrant(options, manifest.objectName);
      const auto confirmed = m_repo->confirmDurablePublicationIfCurrent(manifest);
      return finishOwnedCommit(confirmed, admitted, options, requireActive);
    }
    catch (...) {
      rollbackOwnedCommit(manifest, admitted);
      throw;
    }
  }

  bool supportsDurableRetention() const noexcept override { return true; }
  bool requiresDurableStorageActor() const noexcept override { return !!m_journal; }

private:
  static std::vector<ndn::Name> provisionalAuxiliaryPacketNames(
    const RepoObjectManifest& manifest)
  {
    using namespace ndn_service_framework;
    const ndn::Name root(manifest.objectName);
    const ndn::Name dataPrefix(manifest.dataPrefix);
    const auto producer = ProtectedBlobManifest::inferProducer(root);
    if (!producer || !producer->isPrefixOf(dataPrefix) || dataPrefix == root ||
        dataPrefix.toUri(ndn::name::UriFormat::CANONICAL) != manifest.dataPrefix ||
        manifest.objectType != "original-packet-set" || manifest.packetNames.size() < 2 ||
        manifest.packetOffsets.size() != manifest.packetNames.size() + 1 ||
        manifest.packetOffsets.front() != 0 || manifest.packetOffsets.back() != manifest.size)
      throw std::runtime_error("DURABLE_AUXILIARY_PACKET_INDEX_INVALID");

    std::vector<ndn::Name> names;
    std::set<ndn::Name> unique;
    std::uint64_t expectedSegment = 0;
    std::uint64_t auxiliaryWireBytes = 0;
    bool sawAuxiliary = false;
    for (std::size_t i = 0; i < manifest.packetNames.size(); ++i) {
      const ndn::Name name(manifest.packetNames[i]);
      if (manifest.packetOffsets[i + 1] <= manifest.packetOffsets[i] ||
          name.empty() || name.toUri(ndn::name::UriFormat::CANONICAL) != manifest.packetNames[i] ||
          !producer->isPrefixOf(name) || name.size() <= producer->size() ||
          !unique.insert(name).second)
        throw std::runtime_error("DURABLE_AUXILIARY_PACKET_INDEX_INVALID");
      const auto packetBytes = manifest.packetOffsets[i + 1] - manifest.packetOffsets[i];
      if (packetBytes > ndn::MAX_NDN_PACKET_SIZE)
        throw std::runtime_error("DURABLE_AUXILIARY_PACKET_INDEX_INVALID");

      if (i == 0) {
        if (name != root)
          throw std::runtime_error("DURABLE_AUXILIARY_PACKET_INDEX_INVALID");
        continue;
      }

      if (dataPrefix.isPrefixOf(name)) {
        if (sawAuxiliary)
          throw std::runtime_error("DURABLE_AUXILIARY_PACKET_INDEX_INVALID");
        auto expectedName = dataPrefix;
        expectedName.appendSegment(expectedSegment++);
        if (name != expectedName)
          throw std::runtime_error("DURABLE_AUXILIARY_PACKET_INDEX_INVALID");
        continue;
      }

      sawAuxiliary = true;
      if (name == root || names.size() >= EncryptedLargeDataCommitOptions::MaxAuxiliaryPacketCount ||
          packetBytes > EncryptedLargeDataCommitOptions::MaxAuxiliaryPacketWireBytes - auxiliaryWireBytes)
        throw std::runtime_error("DURABLE_AUXILIARY_PACKET_INDEX_INVALID");
      auxiliaryWireBytes += packetBytes;
      names.push_back(name);
    }
    // Ordinary durable objects may contain only a root and payload segments.
    if (expectedSegment == 0)
      throw std::runtime_error("DURABLE_AUXILIARY_PACKET_INDEX_INVALID");
    return names;
  }

  std::optional<ndn_service_framework::EncryptedLargeDataLookupResult>
  lookupDurableUnlocked(const std::string& publicationIdentity,
    const std::function<void()>& requireActive) const
  {
    if (publicationIdentity.empty())
      return std::nullopt;
    if (requireActive) requireActive();
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
        m_repo, manifest, ndn_service_framework::EncryptedLargeDataRetention::Durable,
        m_journal, m_writerGate, m_repoNode);
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
        found->auxiliaryPacketNames = provisionalAuxiliaryPacketNames(manifest);
      }
    }
    return found;
  }

  static std::unique_lock<std::mutex> lockWriterGate(const std::shared_ptr<std::mutex>& gate)
  {
    return gate ? std::unique_lock<std::mutex>(*gate) : std::unique_lock<std::mutex>();
  }
  std::unique_lock<std::mutex> acquireWriterLock() const { return lockWriterGate(m_writerGate); }
  static uint64_t now()
  {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  }
  static bool sameObject(const RepoUserStorageObject& a, const RepoUserStorageObject& b)
  {
    return a.objectName == b.objectName && a.wireSha256 == b.wireSha256 &&
      a.generation == b.generation && a.payloadWireBytes == b.payloadWireBytes &&
      a.manifestBytes == b.manifestBytes;
  }
  void requireOwnedGrant(const ndn_service_framework::EncryptedLargeDataCommitOptions& options,
                         const std::string& objectName) const
  {
    if (!options.storageActor || options.storageActor->grant.repoNode != m_repoNode ||
        options.storageActor->authenticatedWriterIdentity.empty() ||
        ndn::Name(options.storageActor->authenticatedWriterIdentity).toUri(ndn::name::UriFormat::ALTERNATE) !=
          options.storageActor->authenticatedWriterIdentity)
      throw std::runtime_error("repo-user-storage-unauthorized");
    const auto& actor = *options.storageActor;
    (void)m_journal->validateUserStorageGrant(actor.authenticatedWriterIdentity, m_repoNode,
      actor.grant.ownerIdentity, actor.grant.grantId, objectName, actor.grant.ownerEpoch, now());
  }
  static RepoUserStorageObject measureOwnedObject(const RepoCore& repo,
    const RepoObjectManifest& expected, bool verifyPayload)
  {
    const auto probe = repo.probeObjectMetadata(expected.objectName);
    if (expected.generation == 0 || expected.operationId.empty() ||
        !probe.manifestPresent || !probe.manifestMeasurementAvailable ||
        !probe.payloadMetadataAvailable || !probe.payloadPresent || !probe.payloadRepresentationValid ||
        probe.payloadSize != expected.size || probe.manifest.toJson() != expected.toJson())
      throw std::runtime_error("repo-user-storage-inventory-conflict: physical tuple unavailable");
    if (verifyPayload) {
      auto digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(), EVP_MD_CTX_free);
      if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("cannot initialize owned Repo digest");
      for (uint64_t offset = 0; offset < expected.size;) {
        const auto bytes = repo.getRangeIfCurrent(expected, {offset, std::min(kWindow, expected.size - offset)});
        if (bytes.empty() || EVP_DigestUpdate(digest.get(), bytes.data(), bytes.size()) != 1)
          throw std::runtime_error("repo-user-storage-inventory-conflict: payload read failed");
        offset += bytes.size();
      }
      std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
      unsigned size = 0;
      if (EVP_DigestFinal_ex(digest.get(), hash.data(), &size) != 1)
        throw std::runtime_error("cannot finalize owned Repo digest");
      std::ostringstream encoded;
      for (unsigned i = 0; i < size; ++i)
        encoded << std::hex << std::setw(2) << std::setfill('0') << unsigned(hash[i]);
      if (encoded.str() != expected.sha256)
        throw std::runtime_error("repo-user-storage-inventory-conflict: payload digest mismatch");
    }
    return {expected.objectName, expected.sha256, expected.generation,
      probe.payloadSize, probe.manifestWireBytes};
  }
  RepoUserStorageWrite admitOwnedCommit(RepoObjectManifest& candidate,
    const ndn_service_framework::EncryptedLargeDataCommitOptions& options)
  {
    requireOwnedGrant(options, candidate.objectName);
    const auto& actor = *options.storageActor;
    std::string identity;
    for (const auto& value : {m_repoNode, actor.grant.ownerIdentity, actor.grant.grantId,
      std::to_string(actor.grant.ownerEpoch), actor.authenticatedWriterIdentity,
      options.publicationIdentity, candidate.objectName, candidate.sha256})
      identity += std::to_string(value.size()) + ":" + value;
    const auto operation = "live-store-" + sha256Hex(toBytes(identity));
    const auto probe = m_repo->probeObjectMetadata(candidate.objectName);
    RepoUserStorageObject object;
    if (probe.manifestPresent) {
      auto normalized = candidate;
      normalized.operationId = probe.manifest.operationId;
      normalized.generation = probe.manifest.generation;
      if (normalized.toJson() != probe.manifest.toJson())
        throw std::runtime_error("repo-data-wire-conflict");
      candidate = probe.manifest;
      object = measureOwnedObject(*m_repo, candidate, true);
      if (object.manifestBytes != candidate.toJson().size())
        throw std::runtime_error("repo-user-storage-inventory-conflict: noncanonical manifest bytes");
      for (const auto& holder : m_journal->liveUserStorageClaimsForObject(m_repoNode, candidate.objectName))
        if (!sameObject(holder.object, object))
          throw std::runtime_error("repo-user-storage-inventory-conflict: shared holder measurement drift");
    }
    else {
      if (m_repo->has(candidate.objectName))
        throw std::runtime_error("repo-user-storage-reconciliation-required: private object alias");
      candidate.operationId = operation;
      candidate.generation = 1;
      object = {candidate.objectName, candidate.sha256, candidate.generation,
        candidate.size, candidate.toJson().size()};
    }
    auto admitted = m_journal->reserveUserStorage(actor.authenticatedWriterIdentity, m_repoNode,
      actor.grant.ownerIdentity, actor.grant.grantId, operation, object, now());
    if (admitted.state == RepoUserStorageWriteState::Cancelled)
      throw std::runtime_error("repo-user-storage-operation-conflict: cancelled write cannot restart");
    return admitted;
  }
  std::shared_ptr<const ndn_service_framework::EncryptedLargeDataRangeSource>
  finishOwnedCommit(const RepoObjectManifest& actual, const RepoUserStorageWrite& admitted,
    const ndn_service_framework::EncryptedLargeDataCommitOptions& options,
    const std::function<void()>& requireActive)
  {
    if (requireActive) requireActive();
    const auto measured = measureOwnedObject(*m_repo, actual, true);
    if (!sameObject(measured, admitted.object))
      throw std::runtime_error("repo-user-storage-inventory-conflict: committed measurement changed");
    requireOwnedGrant(options, actual.objectName);
    if (admitted.state == RepoUserStorageWriteState::Reserved) {
      (void)m_journal->settleUserStorageReservation(m_repoNode, admitted.grant.ownerIdentity,
        admitted.operationId, measured.manifestBytes, now());
      (void)m_journal->commitUserStorage(m_repoNode, admitted.grant.ownerIdentity, admitted.operationId, now());
    }
    else if (admitted.state != RepoUserStorageWriteState::Committed)
      throw std::runtime_error("repo-user-storage-operation-conflict");
    const auto claim = m_journal->lookupCommittedUserStorageClaim(admitted.grant.ownerIdentity,
      m_repoNode, actual.objectName, admitted.grant.ownerEpoch);
    if (!claim || !sameObject(claim->object, measured))
      throw std::runtime_error("DURABLE_OWNER_CLAIM_UNAVAILABLE");
    return std::make_shared<Source>(m_repo, actual, options.retention,
      m_journal, m_writerGate, m_repoNode, *claim);
  }
  void rollbackOwnedCommit(const RepoObjectManifest& expected, const RepoUserStorageWrite& admitted) noexcept
  {
    try {
      if (admitted.state == RepoUserStorageWriteState::Reserved &&
          m_journal->userStorageReconciliation(m_repoNode).ready &&
          m_repo->abortFailedRangesIfOwned(expected))
        (void)m_journal->cancelUserStorage(m_repoNode, admitted.grant.ownerIdentity, admitted.operationId, now());
    }
    catch (...) {} // Preserve the original failure and any unconfirmed charge.
  }
  std::shared_ptr<RepoCore> m_repo;
  std::shared_ptr<SqliteArtifactMetadataStore> m_journal;
  std::string m_repoNode;
  std::shared_ptr<std::mutex> m_writerGate;
};

} // namespace ndnsf_distributed_repo
