#pragma once

#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndn-service-framework/EncryptedLargeDataRangeStore.hpp"

#include <openssl/evp.h>
#include <ndn-cxx/util/random.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

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
      if (m_released.exchange(true))
        return;
      // A same-name replacement belongs to a different transaction.
      try {
        m_repo->removeIfCurrent(m_manifest);
      }
      catch (...) {}
    }
    std::uint64_t size() const noexcept override { return m_manifest.size; }
    std::vector<std::uint8_t> read(std::uint64_t offset,
                                 std::uint64_t length) const override
    {
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
      if (manifest.objectType != "encrypted-large-data-envelope" ||
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
    }
    return found;
  }

  bool supportsDurableRetention() const noexcept override { return true; }

private:
  std::shared_ptr<RepoCore> m_repo;
};

} // namespace ndnsf_distributed_repo
