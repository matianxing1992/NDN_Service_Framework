#ifndef NDNSF_DISTRIBUTED_REPO_SOURCE_PROVIDER_HPP
#define NDNSF_DISTRIBUTED_REPO_SOURCE_PROVIDER_HPP

#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalArtifactPublisher.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ndnsf_distributed_repo {

/**
 * RepoCore-backed Runtime source owner. The object owns the Repo authority,
 * performs manifest-first lookup, ingests a missing canonical object at the
 * prepare boundary, and returns verified bytes. RepoCore itself remains
 * independent of DI Runtime; this adapter is the application integration
 * point.
 */
class RepoSourceProvider final : public ndnsf::di::RepositorySourceProvider,
                                 public ndnsf::di::RepositoryArtifactPublisher
{
public:
  using Fallback = ndnsf::di::RepositorySourceProvider::Fallback;

  struct Stats
  {
    std::size_t lookups = 0;
    std::size_t missIngests = 0;
    std::size_t publicationCalls = 0;
    std::size_t publicationHits = 0;
  };

  explicit RepoSourceProvider(std::shared_ptr<RepoCore> repo, Fallback fallback = {})
    : m_repo(std::move(repo)), m_fallback(std::move(fallback))
  {
    if (!m_repo)
      throw std::invalid_argument("RepoSourceProvider requires RepoCore");
  }

  ndnsf::di::NativeCanonicalSource load(
    const ndnsf::di::RepositorySourceRequest& request,
    const Fallback& fallback) const override
  {
    const auto& sourceFallback = m_fallback ? m_fallback : fallback;
    if (std::chrono::steady_clock::now() >= request.deadline)
      throw ndnsf::di::RepositorySourceError(
        ndnsf::di::RepositorySourceError::Kind::Timeout,
        "repository source deadline expired");
    const auto root = ndnsf::di::nativeParseJson(request.catalogConfigurationJson);
    const auto& source = root.at("source");
    const auto objectName = source.at("data_name").get<std::string>();
    const auto expectedDigest = source.at("digest").get<std::string>();
    if (objectName.empty() || expectedDigest.empty())
      throw ndnsf::di::RepositorySourceError(
        ndnsf::di::RepositorySourceError::Kind::Unavailable,
        "repository source identity is incomplete");
    const auto initializerName = source.value(
      "initializer_data_name", objectName + "/initializer");
    const auto initializerDigest = source.value("initializer_digest", std::string{});
    const auto sourceSize = source.value("bytes", std::uint64_t{0});
    ++m_lookups;

    const auto readObject = [&] (const std::string& name,
                                 const std::string& digest,
                                 std::uint64_t expectedSize) {
      const auto manifest = m_repo->getManifest(name);
      if ((!digest.empty() && "sha256:" + manifest.sha256 != digest) ||
          (expectedSize != 0 && manifest.size != expectedSize) ||
          manifest.size > request.maxSourceBytes)
        throw ndnsf::di::RepositorySourceError(
          ndnsf::di::RepositorySourceError::Kind::Unavailable,
          "repository manifest does not match pinned source");
      std::vector<std::uint8_t> bytes;
      bytes.reserve(static_cast<std::size_t>(manifest.size));
      constexpr std::uint64_t kReadWindow = 1U << 20;
      for (std::uint64_t offset = 0; offset < manifest.size;) {
        if (std::chrono::steady_clock::now() >= request.deadline)
          throw ndnsf::di::RepositorySourceError(
            ndnsf::di::RepositorySourceError::Kind::Timeout,
            "repository source range read deadline expired");
        const auto length = std::min(kReadWindow, manifest.size - offset);
        auto part = m_repo->getRange(name, {offset, length});
        if (part.size() != length)
          throw ndnsf::di::RepositorySourceError(
            ndnsf::di::RepositorySourceError::Kind::Unavailable,
            "repository range read returned an unexpected size");
        bytes.insert(bytes.end(), part.begin(), part.end());
        offset += length;
      }
      if (bytes.size() != manifest.size ||
          ndnsf::di::nativePlanningDigest(bytes.data(), bytes.size()) != digest)
        throw ndnsf::di::RepositorySourceError(
          ndnsf::di::RepositorySourceError::Kind::Unavailable,
          "repository payload digest or size differs from manifest");
      return bytes;
    };

    const auto makeManifest = [&] (const std::string& name,
                                   const std::vector<std::uint8_t>& bytes,
                                   const std::string& objectType) {
      RepoObjectManifest manifest;
      manifest.objectName = name;
      manifest.objectType = objectType;
      manifest.sha256 = sha256Hex(bytes);
      manifest.size = bytes.size();
      manifest.segmentCount = 1;
      manifest.generation = 0;
      return manifest;
    };
    const auto ingestObject = [&] (const std::string& name,
                                   const std::vector<std::uint8_t>& bytes,
                                   const std::string& objectType) {
      const auto manifest = makeManifest(name, bytes, objectType);
      constexpr std::uint64_t kWriteWindow = 1U << 20;
      const auto requireDeadline = [&] {
        if (std::chrono::steady_clock::now() >= request.deadline)
          throw ndnsf::di::RepositorySourceError(
            ndnsf::di::RepositorySourceError::Kind::Timeout,
            "repository source ingest deadline expired");
      };
      try {
        for (std::uint64_t offset = 0; offset < manifest.size;) {
          requireDeadline();
          const auto length = std::min(kWriteWindow, manifest.size - offset);
          std::vector<std::uint8_t> part(
            bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
          m_repo->putRange(manifest, {offset, length}, part);
          offset += length;
        }
        requireDeadline();
        m_repo->commitRanges(manifest);
      }
      catch (...) {
        try { m_repo->abortRanges(name); }
        catch (...) {}
        throw;
      }
    };

    ndnsf::di::NativeCanonicalSource sourceValue;
    if (m_repo->has(objectName)) {
      sourceValue.modelBytes = readObject(objectName, expectedDigest, sourceSize);
    }
    else {
      if (!sourceFallback)
        throw ndnsf::di::RepositorySourceError(
          ndnsf::di::RepositorySourceError::Kind::Unavailable,
          "repository source is absent and no fallback is configured");
      ++m_missIngests;
      sourceValue = sourceFallback(request);
      if (sourceValue.modelBytes.empty() ||
          sourceValue.modelBytes.size() > request.maxSourceBytes ||
          (sourceSize != 0 && sourceValue.modelBytes.size() != sourceSize) ||
          ndnsf::di::nativePlanningDigest(sourceValue.modelBytes.data(),
                                           sourceValue.modelBytes.size()) != expectedDigest)
        throw ndnsf::di::RepositorySourceError(
          ndnsf::di::RepositorySourceError::Kind::Unavailable,
          "fallback source does not match pinned digest or size");
      if (sourceValue.initializerBytes && initializerDigest.empty())
        throw ndnsf::di::RepositorySourceError(
          ndnsf::di::RepositorySourceError::Kind::Unavailable,
          "fallback initializer is not pinned by the catalog");
      if (sourceValue.initializerBytes &&
          (sourceValue.initializerBytes->empty() ||
           sourceValue.initializerBytes->size() > request.maxSourceBytes ||
           ndnsf::di::nativePlanningDigest(sourceValue.initializerBytes->data(),
                                            sourceValue.initializerBytes->size()) != initializerDigest))
        throw ndnsf::di::RepositorySourceError(
          ndnsf::di::RepositorySourceError::Kind::Unavailable,
          "fallback initializer does not match pinned digest or size");
      // Validate all fallback material before either object is persisted. If a
      // later range write fails, RepoCore aborts only the staging reservation;
      // a successfully committed canonical object is safe durable material
      // for the next idempotent prepare and is never advertised as READY here.
      ingestObject(objectName, sourceValue.modelBytes, "canonical-model-source");
      sourceValue.modelBytes = readObject(objectName, expectedDigest,
                                          sourceValue.modelBytes.size());
    }

    if (!initializerDigest.empty()) {
      if (!m_repo->has(initializerName)) {
        if (!sourceFallback)
          throw ndnsf::di::RepositorySourceError(
            ndnsf::di::RepositorySourceError::Kind::Unavailable,
            "repository initializer is absent and no fallback is configured");
        const auto fallbackValue = sourceFallback(request);
        if (!fallbackValue.initializerBytes || fallbackValue.initializerBytes->empty() ||
            fallbackValue.initializerBytes->size() > request.maxSourceBytes ||
            ndnsf::di::nativePlanningDigest(fallbackValue.initializerBytes->data(),
                                             fallbackValue.initializerBytes->size()) != initializerDigest)
          throw ndnsf::di::RepositorySourceError(
            ndnsf::di::RepositorySourceError::Kind::Unavailable,
            "fallback initializer does not match pinned digest or size");
        ingestObject(initializerName, *fallbackValue.initializerBytes, "canonical-initializer");
      }
      sourceValue.initializerBytes = readObject(initializerName, initializerDigest, 0);
    }
    return sourceValue;
  }

  ndnsf::di::NativePreparedCanonicalPublication publish(
    const std::string& modelKey,
    const std::string& serviceName,
    const ndnsf::di::NativeInspectedModel& model,
    const ndnsf::di::NativeCanonicalSource& source,
    const ndnsf::di::NativeCanonicalPublicationOptions& options,
    const ndnsf::di::NativeRequestControl& control) const override
  {
    control.requireActive();
    std::lock_guard<std::mutex> publicationLock(m_publicationMutex);
    if (modelKey.empty() || serviceName.empty() || source.modelBytes.empty())
      throw ndnsf::di::RepositorySourceError(
        ndnsf::di::RepositorySourceError::Kind::Unavailable,
        "repository artifact publication identity is incomplete");
    if (source.modelBytes.size() != model.canonicalSourceBytes ||
        ndnsf::di::nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size()) !=
          model.canonicalSourceDigest ||
        source.initializerBytes.has_value() != (model.canonicalInitializerBytes != 0) ||
        (source.initializerBytes &&
         (source.initializerBytes->size() != model.canonicalInitializerBytes ||
          ndnsf::di::nativePlanningDigest(source.initializerBytes->data(),
                                          source.initializerBytes->size()) !=
            model.canonicalInitializerObjectDigest)))
      throw ndnsf::di::RepositorySourceError(
        ndnsf::di::RepositorySourceError::Kind::Unavailable,
        "repository artifact source differs from inspected identity");
    if (!options.layerManifestDigests.empty())
      throw ndnsf::di::RepositorySourceError(
        ndnsf::di::RepositorySourceError::Kind::Unavailable,
        "repository layer publication is not connected to a durable layer payload owner");

    const auto suffix = model.canonicalSourceDigest.substr(7);
    const auto root = options.artifactRoot + "/prepared/" + suffix;
    const auto sourceName = root + "/source";
    const auto initializerName = root + "/initializer";
    const auto rootName = root + "/manifest";
    ++m_publicationCalls;

    const auto makeReceipt = [&] (const std::string& manifestJson) {
      ndnsf::di::NativePreparedCanonicalPublication receipt;
      receipt.sourceDataName = sourceName;
      receipt.initializerDataName = source.initializerBytes ? initializerName : std::string{};
      receipt.rootDataName = rootName;
      receipt.canonicalManifestJson = manifestJson;
      receipt.manifestDigest = ndnsf::di::nativePlanningDigest(manifestJson);
      receipt.artifactProfileDigest = options.artifactProfileDigest;
      receipt.rollbackOwned = false;
      receipt.rollbackDataNames = {sourceName};
      if (source.initializerBytes)
        receipt.rollbackDataNames.push_back(initializerName);
      receipt.rollbackDataNames.push_back(rootName);
      receipt.validate();
      return receipt;
    };

    if (m_repo->has(rootName)) {
      const auto rootBytes = m_repo->get(rootName);
      const auto manifestJson = std::string(rootBytes.begin(), rootBytes.end());
      const auto rejectConflict = [] {
        throw ndnsf::di::RepositorySourceError(
          ndnsf::di::RepositorySourceError::Kind::Unavailable,
          "repository prepared artifact identity conflicts with the requested model");
      };
      try {
        const auto rootJson = ndnsf::di::nativeParseJson(manifestJson);
        const auto metadata = rootJson.at("metadata");
        const auto layerDigests = rootJson.value(
          "layerManifestDigests", std::vector<std::string>{});
        if (rootJson.value("schema", std::string{}) !=
              "ndnsf-di-canonical-model-manifest-v1" ||
            rootJson.value("state", std::string{}) != "ACTIVE" ||
            rootJson.value("artifactProfileDigest", std::string{}) !=
              options.artifactProfileDigest ||
            rootJson.value("modelIdentityDigest", std::string{}) !=
              model.descriptor.contentDigest ||
            rootJson.value("modelName", std::string{}) != model.descriptor.modelName ||
            metadata.value("modelKey", std::string{}) != modelKey ||
            metadata.value("serviceName", std::string{}) != serviceName ||
            metadata.value("canonicalSourceDataName", std::string{}) != sourceName ||
            metadata.value("canonicalSourceDigest", std::string{}) !=
              model.canonicalSourceDigest ||
            metadata.value("canonicalSourceBytes", std::uint64_t{0}) !=
              model.canonicalSourceBytes ||
            metadata.value("canonicalInitializerDataName", std::string{}) !=
              (source.initializerBytes ? initializerName : std::string{}) ||
            metadata.value("canonicalInitializerObjectDigest", std::string{}) !=
              model.canonicalInitializerObjectDigest ||
            metadata.value("canonicalInitializerBytes", std::uint64_t{0}) !=
              model.canonicalInitializerBytes ||
            metadata.value("canonicalGraphDigest", std::string{}) !=
              model.canonicalGraphDigest ||
            metadata.value("packageManifestDigest", std::string{}) !=
              options.packageManifestDigest ||
            layerDigests != options.layerManifestDigests)
          rejectConflict();

        const auto checkObject = [&] (const std::string& name,
                                      const std::string& expectedDigest,
                                      std::uint64_t expectedBytes) {
          const auto manifest = m_repo->getManifest(name);
          if (expectedDigest.size() < 7 || expectedDigest.compare(0, 7, "sha256:") != 0 ||
              manifest.objectName != name || manifest.size != expectedBytes ||
              manifest.sha256 != expectedDigest.substr(7))
            rejectConflict();

          std::vector<std::uint8_t> bytes;
          bytes.reserve(static_cast<std::size_t>(manifest.size));
          constexpr std::uint64_t window = 1U << 20;
          for (std::uint64_t offset = 0; offset < manifest.size;) {
            control.requireActive();
            const auto length = std::min(window, manifest.size - offset);
            const auto part = m_repo->getRange(name, {offset, length});
            if (part.size() != length)
              rejectConflict();
            bytes.insert(bytes.end(), part.begin(), part.end());
            offset += length;
          }
          if (ndnsf::di::nativePlanningDigest(bytes.data(), bytes.size()) != expectedDigest)
            rejectConflict();
        };
        checkObject(sourceName, model.canonicalSourceDigest, model.canonicalSourceBytes);
        if (source.initializerBytes)
          checkObject(initializerName, model.canonicalInitializerObjectDigest,
                      model.canonicalInitializerBytes);
        checkObject(rootName, ndnsf::di::nativePlanningDigest(manifestJson),
                    manifestJson.size());
      }
      catch (const ndnsf::di::RepositorySourceError&) {
        throw;
      }
      catch (...) {
        rejectConflict();
      }
      const auto receipt = makeReceipt(manifestJson);
      ++m_publicationHits;
      return receipt;
    }

    const auto putRanges = [&] (const std::string& objectName,
                                const std::vector<std::uint8_t>& bytes,
                                const std::string& objectType) {
      ndnsf_distributed_repo::RepoObjectManifest manifest;
      manifest.objectName = objectName;
      manifest.objectType = objectType;
      manifest.sha256 = ndnsf_distributed_repo::sha256Hex(bytes);
      manifest.size = bytes.size();
      manifest.segmentCount = 1;
      manifest.generation = 0;
      constexpr std::uint64_t window = 1U << 20;
      for (std::uint64_t offset = 0; offset < manifest.size;) {
        control.requireActive();
        const auto length = std::min(window, manifest.size - offset);
        std::vector<std::uint8_t> part(
          bytes.begin() + static_cast<std::ptrdiff_t>(offset),
          bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
        m_repo->putRange(manifest, {offset, length}, part);
        offset += length;
      }
      control.requireActive();
      m_repo->commitRanges(manifest);
    };

    try {
      putRanges(sourceName, source.modelBytes, "ndnsf-di-canonical-source");
      if (source.initializerBytes)
        putRanges(initializerName, *source.initializerBytes, "ndnsf-di-canonical-initializer");
      ndnsf::di::NativeJson metadata{
        {"modelKey", modelKey}, {"serviceName", serviceName},
        {"canonicalSourceDataName", sourceName},
        {"canonicalSourceDigest", model.canonicalSourceDigest},
        {"canonicalSourceBytes", model.canonicalSourceBytes},
        {"canonicalInitializerDataName", source.initializerBytes ? initializerName : ""},
        {"canonicalInitializerObjectDigest", model.canonicalInitializerObjectDigest},
        {"canonicalInitializerBytes", model.canonicalInitializerBytes},
        {"canonicalGraphDigest", model.canonicalGraphDigest},
        {"packageManifestDigest", options.packageManifestDigest}};
      ndnsf::di::NativeJson rootJson{
        {"schema", "ndnsf-di-canonical-model-manifest-v1"}, {"state", "ACTIVE"},
        {"artifactProfileDigest", options.artifactProfileDigest},
        {"modelIdentityDigest", model.descriptor.contentDigest},
        {"modelName", model.descriptor.modelName}, {"metadata", std::move(metadata)}};
      if (!options.layerManifestDigests.empty())
        rootJson["layerManifestDigests"] = options.layerManifestDigests;
      const auto manifestJson = ndnsf::di::nativeCanonicalJson(rootJson);
      const std::vector<std::uint8_t> rootBytes(manifestJson.begin(), manifestJson.end());
      putRanges(rootName, rootBytes, "ndnsf-di-canonical-manifest");
      return makeReceipt(manifestJson);
    }
    catch (...) {
      try { m_repo->abortRanges(sourceName); } catch (...) {}
      try { m_repo->abortRanges(initializerName); } catch (...) {}
      try { m_repo->abortRanges(rootName); } catch (...) {}
      try { m_repo->remove(sourceName); } catch (...) {}
      try { m_repo->remove(initializerName); } catch (...) {}
      try { m_repo->remove(rootName); } catch (...) {}
      throw;
    }
  }

  void rollback(const ndnsf::di::NativePreparedCanonicalPublication& publication) const noexcept override
  {
    if (!publication.rollbackOwned)
      return;
    for (const auto& name : publication.rollbackDataNames) {
      try { m_repo->remove(name); } catch (...) {}
    }
    for (const auto& name : {publication.sourceDataName, publication.initializerDataName,
                             publication.rootDataName}) {
      if (!name.empty()) {
        try { m_repo->remove(name); } catch (...) {}
      }
    }
  }

  ndnsf::di::NativeCanonicalSource load(
    const ndnsf::di::RepositorySourceRequest& request) const
  { return load(request, m_fallback); }

  Stats stats() const noexcept
  {
    return {m_lookups.load(std::memory_order_relaxed),
            m_missIngests.load(std::memory_order_relaxed),
            m_publicationCalls.load(std::memory_order_relaxed),
            m_publicationHits.load(std::memory_order_relaxed)};
  }

private:
  std::shared_ptr<RepoCore> m_repo;
  Fallback m_fallback;
  mutable std::atomic<std::size_t> m_lookups{0};
  mutable std::atomic<std::size_t> m_missIngests{0};
  mutable std::atomic<std::size_t> m_publicationCalls{0};
  mutable std::atomic<std::size_t> m_publicationHits{0};
  mutable std::mutex m_publicationMutex;
};

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_SOURCE_PROVIDER_HPP
