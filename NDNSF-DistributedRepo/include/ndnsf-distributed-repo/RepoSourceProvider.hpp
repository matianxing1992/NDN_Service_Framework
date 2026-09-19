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
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
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

  struct MaterialSelectionResult
  {
    std::shared_ptr<ndnsf::di::NativeCanonicalSource::MaterialManifest> manifest;
    std::vector<ndnsf::di::NativeCanonicalSource::MaterialPayload> payloads;
    std::uint64_t bytesRead = 0;
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
    auto repoPublicationLock = m_repo->acquirePublicationLock();
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
    if (source.materialManifest) {
      ndnsf::di::validateNativeCanonicalMaterialManifest(
        source, *source.materialManifest, ndnsf::di::NativeAssemblyControl{
          control.deadline, [&control] { control.requireActive(); },
          std::max(model.canonicalSourceBytes, model.canonicalInitializerBytes),
          options.maxPublicationBytes != 0 ? options.maxPublicationBytes
                                           : std::numeric_limits<std::uint64_t>::max()});
      if (source.materialManifest->sourceDigest != model.canonicalSourceDigest ||
          source.materialManifest->graphDigest != model.canonicalGraphDigest ||
          (!model.canonicalInitializerDigest.empty() &&
           source.materialManifest->initializerDigest != model.canonicalInitializerDigest))
        throw ndnsf::di::RepositorySourceError(
          ndnsf::di::RepositorySourceError::Kind::Unavailable,
          "repository material manifest differs from inspected identity");
    }
    if (options.layerManifestDigests.size() != source.layerPayloads.size())
      throw ndnsf::di::RepositorySourceError(
        ndnsf::di::RepositorySourceError::Kind::Unavailable,
        "repository layer manifest and payload counts differ");

    std::set<std::uint64_t> stages;
    std::uint64_t previousEnd = 0;
    for (std::size_t i = 0; i < source.layerPayloads.size(); ++i) {
      const auto& layer = source.layerPayloads[i];
      if (layer.bytes.empty() || layer.layerBegin >= layer.layerEnd ||
          !stages.insert(layer.stageIndex).second ||
          (i != 0 && layer.layerBegin != previousEnd) ||
          layer.digest != options.layerManifestDigests[i] ||
          ndnsf::di::nativePlanningDigest(layer.bytes.data(), layer.bytes.size()) != layer.digest)
        throw ndnsf::di::RepositorySourceError(
          ndnsf::di::RepositorySourceError::Kind::Unavailable,
          "repository layer payload identity is invalid");
      previousEnd = layer.layerEnd;
    }

    const auto suffix = model.canonicalSourceDigest.substr(7);
    const auto root = options.artifactRoot + "/prepared/" + suffix;
    const auto sourceName = root + "/source";
    const auto initializerName = root + "/initializer";
    const auto rootName = root + "/manifest";
    const auto materialManifestName = root + "/material-manifest";
    std::vector<std::string> materialPayloadIds;
    std::vector<std::string> materialNames;
    std::vector<std::string> materialDigests;
    if (source.materialManifest) {
      materialPayloadIds.reserve(source.materialManifest->payloads.size());
      materialNames.reserve(source.materialManifest->payloads.size());
      materialDigests.reserve(source.materialManifest->payloads.size());
      for (const auto& payload : source.materialManifest->payloads) {
        materialPayloadIds.push_back(payload.payloadId);
        materialNames.push_back(root + "/materials/" + payload.payloadId);
        materialDigests.push_back(payload.digest);
      }
    }
    const auto checkPublicationBudget = [&] (std::uint64_t rootBytes) {
      if (options.maxPublicationBytes == 0)
        return;
      std::uint64_t total = rootBytes;
      const auto add = [&] (std::uint64_t bytes) {
        if (bytes > options.maxPublicationBytes - std::min(total, options.maxPublicationBytes))
          throw ndnsf::di::RepositorySourceError(
            ndnsf::di::RepositorySourceError::Kind::Unavailable,
            "repository publication exceeds the configured material budget");
        total += bytes;
      };
      add(source.modelBytes.size());
      if (source.initializerBytes) add(source.initializerBytes->size());
      for (const auto& layer : source.layerPayloads) add(layer.bytes.size());
      if (source.materialManifest) {
        for (const auto& payload : source.materialManifest->payloads) add(payload.bytes.size());
        add(source.materialManifest->canonicalJson().size());
      }
    };
    ++m_publicationCalls;

    const auto makeReceipt = [&] (const std::string& manifestJson) {
      ndnsf::di::NativePreparedCanonicalPublication receipt;
      receipt.sourceDataName = sourceName;
      receipt.initializerDataName = source.initializerBytes ? initializerName : std::string{};
      receipt.rootDataName = rootName;
      receipt.materialManifestDataName = source.materialManifest ? materialManifestName : std::string{};
      receipt.materialManifestDigest = source.materialManifest
        ? ndnsf::di::nativePlanningDigest(source.materialManifest->canonicalJson()) : std::string{};
      receipt.materialManifestBytes = source.materialManifest
        ? source.materialManifest->canonicalJson().size() : 0;
      receipt.materialPayloadIds = materialPayloadIds;
      receipt.materialDataNames = materialNames;
      receipt.materialDigests = materialDigests;
      receipt.canonicalManifestJson = manifestJson;
      receipt.manifestDigest = ndnsf::di::nativePlanningDigest(manifestJson);
      receipt.artifactProfileDigest = options.artifactProfileDigest;
      receipt.layerManifestDigests = options.layerManifestDigests;
      for (const auto& layer : source.layerPayloads) {
        const auto layerName = root + "/layers/" + std::to_string(layer.stageIndex) + "-" +
          std::to_string(layer.layerBegin) + "-" + std::to_string(layer.layerEnd);
        receipt.layerDataNames.push_back(layerName);
      }
      receipt.rollbackOwned = false;
      receipt.rollbackDataNames = {sourceName};
      if (source.initializerBytes)
        receipt.rollbackDataNames.push_back(initializerName);
      receipt.rollbackDataNames.insert(receipt.rollbackDataNames.end(),
                                       receipt.layerDataNames.begin(), receipt.layerDataNames.end());
      receipt.rollbackDataNames.insert(receipt.rollbackDataNames.end(),
                                       receipt.materialDataNames.begin(), receipt.materialDataNames.end());
      if (!receipt.materialManifestDataName.empty())
        receipt.rollbackDataNames.push_back(receipt.materialManifestDataName);
      receipt.rollbackDataNames.push_back(rootName);
      receipt.validate();
      return receipt;
    };

    if (m_repo->has(rootName)) {
      const auto rootBytes = m_repo->get(rootName);
      const auto manifestJson = std::string(rootBytes.begin(), rootBytes.end());
      checkPublicationBudget(manifestJson.size());
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
            metadata.value("materialManifestDataName", std::string{}) !=
              (source.materialManifest ? materialManifestName : std::string{}) ||
            metadata.value("materialManifestDigest", std::string{}) !=
              (source.materialManifest
                 ? ndnsf::di::nativePlanningDigest(source.materialManifest->canonicalJson())
                 : std::string{}) ||
            metadata.value("materialManifestBytes", std::uint64_t{0}) !=
              (source.materialManifest ? source.materialManifest->canonicalJson().size() : 0) ||
            metadata.value("materialObjects", ndnsf::di::NativeJson::array()).size() !=
              materialPayloadIds.size() ||
            metadata.value("packageManifestDigest", std::string{}) !=
              options.packageManifestDigest ||
            layerDigests != options.layerManifestDigests ||
            rootJson.value("layerReferences", ndnsf::di::NativeJson::array()).size() !=
              source.layerPayloads.size())
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
        if (!source.layerPayloads.empty()) {
          const auto& layerReferences = rootJson.at("layerReferences");
          for (std::size_t i = 0; i < source.layerPayloads.size(); ++i) {
            const auto& layer = source.layerPayloads[i];
            const auto& reference = layerReferences.at(i);
            const auto layerName = root + "/layers/" + std::to_string(layer.stageIndex) + "-" +
              std::to_string(layer.layerBegin) + "-" + std::to_string(layer.layerEnd);
            if (reference.value("stageIndex", std::uint64_t{~0U}) != layer.stageIndex ||
                reference.value("layerBegin", std::uint64_t{~0U}) != layer.layerBegin ||
                reference.value("layerEnd", std::uint64_t{~0U}) != layer.layerEnd ||
                reference.value("dataName", std::string{}) != layerName ||
                reference.value("digest", std::string{}) != layer.digest ||
                reference.value("bytes", std::uint64_t{0}) != layer.bytes.size())
              rejectConflict();
            checkObject(layerName, layer.digest, layer.bytes.size());
          }
        }
        if (source.materialManifest) {
          checkObject(materialManifestName,
            ndnsf::di::nativePlanningDigest(source.materialManifest->canonicalJson()),
            source.materialManifest->canonicalJson().size());
          const auto& materialObjects = metadata.at("materialObjects");
          if (!materialObjects.is_array() || materialObjects.size() != materialPayloadIds.size())
            rejectConflict();
          for (std::size_t i = 0; i < materialPayloadIds.size(); ++i) {
            const auto& object = materialObjects.at(i);
            if (object.value("payloadId", std::string{}) != materialPayloadIds[i] ||
                object.value("dataName", std::string{}) != materialNames[i] ||
                object.value("digest", std::string{}) != materialDigests[i] ||
                object.value("bytes", std::uint64_t{0}) != source.materialManifest->payloads[i].bytes.size())
              rejectConflict();
            checkObject(materialNames[i], materialDigests[i], source.materialManifest->payloads[i].bytes.size());
          }
        }
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

    std::vector<RepoObjectManifest> ownedManifests;
    std::random_device random;
    const auto operationPrefix = std::string("ndnsf-di-") + suffix + "-" +
      std::to_string(random()) + "-" + std::to_string(random()) + "-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::uint64_t publicationBytes = 0;
    const auto putRanges = [&] (const std::string& objectName,
                                const std::vector<std::uint8_t>& bytes,
                                const std::string& objectType) {
      if (options.maxPublicationBytes != 0 &&
          (bytes.size() > options.maxPublicationBytes -
             std::min<std::uint64_t>(publicationBytes, options.maxPublicationBytes)))
        throw ndnsf::di::RepositorySourceError(
          ndnsf::di::RepositorySourceError::Kind::Unavailable,
          "repository publication exceeds the configured material budget");
      publicationBytes += bytes.size();
      ndnsf_distributed_repo::RepoObjectManifest manifest;
      manifest.objectName = objectName;
      manifest.objectType = objectType;
      manifest.sha256 = ndnsf_distributed_repo::sha256Hex(bytes);
      manifest.size = bytes.size();
      manifest.segmentCount = 1;
      manifest.generation = 0;
      manifest.operationId = operationPrefix + "-" + std::to_string(ownedManifests.size());
      if (m_repo->has(objectName)) {
        const auto current = m_repo->getManifest(objectName);
        if (current.objectName != manifest.objectName || current.objectType != manifest.objectType ||
            current.sha256 != manifest.sha256 || current.size != manifest.size ||
            current.segmentCount != manifest.segmentCount)
          throw ndnsf::di::RepositorySourceError(
            ndnsf::di::RepositorySourceError::Kind::Unavailable,
            "repository publication object conflicts with an existing object");
        std::vector<std::uint8_t> existing;
        existing.reserve(static_cast<std::size_t>(current.size));
        constexpr std::uint64_t readWindow = 1U << 20;
        for (std::uint64_t offset = 0; offset < current.size;) {
          control.requireActive();
          const auto length = std::min(readWindow, current.size - offset);
          const auto part = m_repo->getRange(objectName, {offset, length});
          if (part.size() != length)
            throw ndnsf::di::RepositorySourceError(
              ndnsf::di::RepositorySourceError::Kind::Unavailable,
              "repository existing object range is incomplete");
          existing.insert(existing.end(), part.begin(), part.end());
          offset += length;
        }
        if (existing.size() != bytes.size() ||
            ndnsf::di::nativePlanningDigest(existing.data(), existing.size()) !=
              "sha256:" + current.sha256)
          throw ndnsf::di::RepositorySourceError(
            ndnsf::di::RepositorySourceError::Kind::Unavailable,
            "repository existing object content differs from its manifest");
        return;
      }
      constexpr std::uint64_t window = 1U << 20;
      try {
        bool firstRange = true;
        for (std::uint64_t offset = 0; offset < manifest.size;) {
          control.requireActive();
          const auto length = std::min(window, manifest.size - offset);
          std::vector<std::uint8_t> part(
            bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
          if (firstRange) {
            m_repo->putRangeIfAbsent(manifest, {offset, length}, part);
            firstRange = false;
          }
          else {
            m_repo->putRange(manifest, {offset, length}, part);
          }
          offset += length;
        }
        control.requireActive();
        const auto committed = m_repo->commitRangesIfOwned(manifest);
        ownedManifests.push_back(committed);
        if (committed.objectName != manifest.objectName ||
            committed.sha256 != manifest.sha256 || committed.size != manifest.size ||
            committed.segmentCount != manifest.segmentCount ||
            committed.operationId != manifest.operationId)
          throw ndnsf::di::RepositorySourceError(
            ndnsf::di::RepositorySourceError::Kind::Unavailable,
            "repository commit returned an unexpected manifest");
      }
      catch (...) {
        try { m_repo->abortRangesIfOwned(manifest); } catch (...) {}
        throw;
      }
    };

    try {
      putRanges(sourceName, source.modelBytes, "ndnsf-di-canonical-source");
      if (source.initializerBytes)
        putRanges(initializerName, *source.initializerBytes, "ndnsf-di-canonical-initializer");
      std::vector<std::string> layerNames;
      layerNames.reserve(source.layerPayloads.size());
      for (const auto& layer : source.layerPayloads) {
        const auto layerName = root + "/layers/" + std::to_string(layer.stageIndex) + "-" +
          std::to_string(layer.layerBegin) + "-" + std::to_string(layer.layerEnd);
        putRanges(layerName, layer.bytes, "ndnsf-di-canonical-layer");
        layerNames.push_back(layerName);
      }
      if (source.materialManifest) {
        for (std::size_t i = 0; i < source.materialManifest->payloads.size(); ++i)
          putRanges(materialNames[i], source.materialManifest->payloads[i].bytes,
                    "ndnsf-di-canonical-material");
        const auto materialManifestJson = source.materialManifest->canonicalJson();
        const std::vector<std::uint8_t> materialManifestBytes(
          materialManifestJson.begin(), materialManifestJson.end());
        putRanges(materialManifestName, materialManifestBytes,
                  "ndnsf-di-canonical-material-manifest");
      }
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
      if (source.materialManifest) {
        metadata["materialManifestDataName"] = materialManifestName;
        metadata["materialManifestDigest"] =
          ndnsf::di::nativePlanningDigest(source.materialManifest->canonicalJson());
        metadata["materialManifestBytes"] = source.materialManifest->canonicalJson().size();
        metadata["materialIdentityDigest"] = source.materialManifest->manifestDigest;
        auto materialObjects = ndnsf::di::NativeJson::array();
        for (std::size_t i = 0; i < materialPayloadIds.size(); ++i)
          materialObjects.push_back({{"payloadId", materialPayloadIds[i]},
                                     {"dataName", materialNames[i]},
                                     {"digest", materialDigests[i]},
                                     {"bytes", source.materialManifest->payloads[i].bytes.size()}});
        metadata["materialObjects"] = std::move(materialObjects);
      }
      ndnsf::di::NativeJson rootJson{
        {"schema", "ndnsf-di-canonical-model-manifest-v1"}, {"state", "ACTIVE"},
        {"artifactProfileDigest", options.artifactProfileDigest},
        {"modelIdentityDigest", model.descriptor.contentDigest},
        {"modelName", model.descriptor.modelName}, {"metadata", std::move(metadata)}};
      if (!options.layerManifestDigests.empty()) {
        rootJson["layerManifestDigests"] = options.layerManifestDigests;
        auto references = ndnsf::di::NativeJson::array();
        for (std::size_t i = 0; i < source.layerPayloads.size(); ++i) {
          const auto& layer = source.layerPayloads[i];
          references.push_back({
            {"stageIndex", layer.stageIndex}, {"layerBegin", layer.layerBegin},
            {"layerEnd", layer.layerEnd}, {"dataName", layerNames[i]},
            {"digest", layer.digest}, {"bytes", layer.bytes.size()}});
        }
        rootJson["layerReferences"] = std::move(references);
      }
      const auto manifestJson = ndnsf::di::nativeCanonicalJson(rootJson);
      const std::vector<std::uint8_t> rootBytes(manifestJson.begin(), manifestJson.end());
      putRanges(rootName, rootBytes, "ndnsf-di-canonical-manifest");
      return makeReceipt(manifestJson);
    }
    catch (...) {
      for (const auto& manifest : ownedManifests) {
        try { m_repo->removeIfCurrent(manifest); } catch (...) {}
      }
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

  /** Read the authenticated material index and only the node/shared payloads
   * selected after ACK.  This is the Repo-side consumer seam; it never falls
   * back to the complete canonical source or external initializer. */
  MaterialSelectionResult loadMaterialSelection(
    const std::string& rootDataName,
    const std::string& expectedRootDigest,
    const std::vector<std::uint64_t>& nodeIndices,
    std::uint64_t maxBytes,
    std::chrono::steady_clock::time_point deadline,
    const std::function<void()>& ownerFence = {}) const
  {
    if (rootDataName.empty() || expectedRootDigest.empty() || nodeIndices.empty() || maxBytes == 0)
      throw ndnsf::di::RepositorySourceError(
        ndnsf::di::RepositorySourceError::Kind::Unavailable,
        "material selection request is incomplete");
    const auto requireDeadline = [&] {
      if (ownerFence)
        ownerFence();
      if (std::chrono::steady_clock::now() >= deadline)
        throw ndnsf::di::RepositorySourceError(
          ndnsf::di::RepositorySourceError::Kind::Timeout,
          "material selection deadline expired");
    };
    std::uint64_t bytesRead = 0;
    const auto readObject = [&] (const std::string& name,
                                 const std::string& digest,
                                 std::uint64_t expectedSize) {
      requireDeadline();
      const auto manifest = m_repo->getManifest(name);
      if (!digest.empty() && "sha256:" + manifest.sha256 != digest)
        throw ndnsf::di::RepositorySourceError(
          ndnsf::di::RepositorySourceError::Kind::Unavailable,
          "material object digest differs from root receipt");
      if (expectedSize != 0 && manifest.size != expectedSize)
        throw ndnsf::di::RepositorySourceError(
          ndnsf::di::RepositorySourceError::Kind::Unavailable,
          "material object size differs from root receipt");
      if (manifest.size > maxBytes || bytesRead > maxBytes - manifest.size)
        throw ndnsf::di::RepositorySourceError(
          ndnsf::di::RepositorySourceError::Kind::Unavailable,
          "material selection exceeds read budget");
      std::vector<std::uint8_t> bytes;
      bytes.reserve(static_cast<std::size_t>(manifest.size));
      constexpr std::uint64_t kReadWindow = 1U << 20;
      for (std::uint64_t offset = 0; offset < manifest.size;) {
        requireDeadline();
        const auto length = std::min(kReadWindow, manifest.size - offset);
        const auto part = m_repo->getRange(name, {offset, length});
        if (part.size() != length)
          throw ndnsf::di::RepositorySourceError(
            ndnsf::di::RepositorySourceError::Kind::Unavailable,
            "material range read returned an unexpected size");
        bytes.insert(bytes.end(), part.begin(), part.end());
        offset += length;
      }
      if (ndnsf::di::nativePlanningDigest(bytes.data(), bytes.size()) != digest)
        throw ndnsf::di::RepositorySourceError(
          ndnsf::di::RepositorySourceError::Kind::Unavailable,
          "material object payload digest differs from receipt");
      bytesRead += manifest.size;
      return bytes;
    };

    const auto rootBytes = readObject(rootDataName, expectedRootDigest, 0);
    const auto root = ndnsf::di::nativeParseJson(
      std::string(rootBytes.begin(), rootBytes.end()));
    if (root.value("schema", std::string{}) != "ndnsf-di-canonical-model-manifest-v1" ||
        root.value("state", std::string{}) != "ACTIVE" || !root.contains("metadata"))
      throw ndnsf::di::RepositorySourceError(
        ndnsf::di::RepositorySourceError::Kind::Unavailable,
        "material root schema is invalid");
    const auto& metadata = root.at("metadata");
    const auto manifestName = metadata.value("materialManifestDataName", std::string{});
    const auto manifestDigest = metadata.value("materialManifestDigest", std::string{});
    const auto identityDigest = metadata.value("materialIdentityDigest", std::string{});
    const auto objects = metadata.value("materialObjects", ndnsf::di::NativeJson::array());
    if (manifestName.empty() || manifestDigest.empty() || identityDigest.empty() ||
        !objects.is_array())
      throw ndnsf::di::RepositorySourceError(
        ndnsf::di::RepositorySourceError::Kind::Unavailable,
        "material root references are incomplete");
    std::map<std::string, ndnsf::di::NativeJson> objectById;
    for (const auto& object : objects) {
      const auto id = object.value("payloadId", std::string{});
      if (id.empty() || objectById.count(id) != 0)
        throw ndnsf::di::RepositorySourceError(
          ndnsf::di::RepositorySourceError::Kind::Unavailable,
          "material root payload identity is invalid");
      objectById.emplace(id, object);
    }
    const auto manifestBytes = readObject(manifestName, manifestDigest, 0);
    auto resultManifest = ndnsf::di::parseNativeCanonicalMaterialManifest(manifestBytes);
    if (resultManifest->manifestDigest != identityDigest)
      throw ndnsf::di::RepositorySourceError(
        ndnsf::di::RepositorySourceError::Kind::Unavailable,
        "material manifest identity differs from root receipt");

    std::set<std::string> selectedIds{resultManifest->templatePayloadId};
    std::set<std::string> selectedNodes;
    std::set<std::string> selectedDependencies;
    std::optional<std::uint64_t> previousNodeIndex;
    for (const auto nodeIndex : nodeIndices) {
      if (previousNodeIndex && nodeIndex <= *previousNodeIndex)
        throw ndnsf::di::RepositorySourceError(
          ndnsf::di::RepositorySourceError::Kind::Unavailable,
          "selected material nodes are not in canonical order");
      previousNodeIndex = nodeIndex;
      const auto logicalName = "node/" + std::to_string(nodeIndex);
      const auto* match = static_cast<const ndnsf::di::NativeCanonicalSource::MaterialReference*>(nullptr);
      for (const auto& reference : resultManifest->references)
        if (reference.kind == "graph-node" && reference.logicalName == logicalName) {
          match = &reference;
          break;
        }
      if (match == nullptr || !selectedNodes.insert(logicalName).second)
        throw ndnsf::di::RepositorySourceError(
          ndnsf::di::RepositorySourceError::Kind::Unavailable,
          "selected material node is absent or duplicated");
      selectedIds.insert(match->payloadId);
      selectedDependencies.insert(match->dependencies.begin(), match->dependencies.end());
    }
    for (const auto& dependency : selectedDependencies) {
      const ndnsf::di::NativeCanonicalSource::MaterialReference* match = nullptr;
      for (const auto& reference : resultManifest->references) {
        if (reference.kind == "shared-initializer" &&
            reference.logicalName == dependency) {
          if (match != nullptr)
            throw ndnsf::di::RepositorySourceError(
              ndnsf::di::RepositorySourceError::Kind::Unavailable,
              "selected material dependency has duplicate references");
          match = &reference;
        }
      }
      if (match == nullptr)
        throw ndnsf::di::RepositorySourceError(
          ndnsf::di::RepositorySourceError::Kind::Unavailable,
          "selected material dependency is absent from manifest");
      selectedIds.insert(match->payloadId);
    }
    for (const auto& reference : resultManifest->references) {
      if (reference.kind == "shared-initializer" &&
          selectedDependencies.count(reference.logicalName) != 0)
        selectedIds.insert(reference.payloadId);
    }
    for (const auto& payloadId : selectedIds) {
      requireDeadline();
      const auto object = objectById.find(payloadId);
      if (object == objectById.end())
        throw ndnsf::di::RepositorySourceError(
          ndnsf::di::RepositorySourceError::Kind::Unavailable,
          "selected material payload is absent from root");
      const auto dataName = object->second.value("dataName", std::string{});
      const auto digest = object->second.value("digest", std::string{});
      const auto expectedSize = object->second.value("bytes", std::uint64_t{0});
      const auto* reference = static_cast<const ndnsf::di::NativeCanonicalSource::MaterialReference*>(nullptr);
      for (const auto& candidate : resultManifest->references) {
        if (candidate.payloadId == payloadId) {
          if (reference != nullptr &&
              (candidate.digest != reference->digest || candidate.bytes != reference->bytes))
            throw ndnsf::di::RepositorySourceError(
              ndnsf::di::RepositorySourceError::Kind::Unavailable,
              "material payload references disagree");
          if (reference == nullptr)
            reference = &candidate;
        }
      }
      if (dataName.empty() || digest.empty() || expectedSize == 0)
        throw ndnsf::di::RepositorySourceError(
          ndnsf::di::RepositorySourceError::Kind::Unavailable,
          "selected material payload receipt is incomplete");
      if (reference == nullptr || reference->digest != digest ||
          reference->bytes != expectedSize)
        throw ndnsf::di::RepositorySourceError(
          ndnsf::di::RepositorySourceError::Kind::Unavailable,
          "selected material payload differs from manifest reference");
      auto bytes = readObject(dataName, digest, expectedSize);
      resultManifest->payloads.push_back({payloadId, digest, std::move(bytes)});
    }
    resultManifest->payloadsComplete = false;
    resultManifest->validate();
    auto selectedPayloads = std::move(resultManifest->payloads);
    resultManifest->payloads.clear();
    return {std::move(resultManifest), std::move(selectedPayloads), bytesRead};
  }

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
