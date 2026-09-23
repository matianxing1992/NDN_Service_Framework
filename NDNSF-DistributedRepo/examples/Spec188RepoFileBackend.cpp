#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoCore.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

using namespace ndnsf_distributed_repo;

namespace {

void
require(bool condition, const std::string& message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

RepoObjectManifest
manifestFor(const std::string& name, const std::vector<uint8_t>& payload,
            uint64_t generation = 1)
{
  RepoObjectManifest manifest;
  manifest.objectName = name;
  manifest.objectType = "spec188-file-backend";
  manifest.sha256 = sha256Hex(payload);
  manifest.size = payload.size();
  manifest.segmentCount = 1;
  manifest.generation = generation;
  manifest.policyEpoch = "spec188";
  return manifest;
}

void
removeTree(const std::filesystem::path& path)
{
  std::error_code error;
  std::filesystem::remove_all(path, error);
  if (error) {
    throw std::runtime_error("failed to remove test tree: " + error.message());
  }
}

} // namespace

int
main()
{
  try {
    const auto root = std::filesystem::temp_directory_path() /
      ("ndnsf-spec188-file-backend-" + std::to_string(::getpid()));
    removeTree(root);

    const std::vector<uint8_t> smallPayload{0x01, 0x02, 0x03, 0x04};
    const auto smallManifest = manifestFor("/spec188/small", smallPayload);
    {
      FilesystemRepoStoreBackend backend(root.string(), 64, 64);
      backend.put(smallManifest, smallPayload);
      require(backend.has(smallManifest.objectName),
              "small vector object was not committed");
      require(backend.get(smallManifest.objectName).payload == smallPayload,
              "small vector round trip failed");
      require(backend.getRange(smallManifest.objectName, {1, 2}) ==
                std::vector<uint8_t>({0x02, 0x03}),
              "small range read failed");
      // Manifest-only updates are reserved for segmented/metadata objects;
      // ordinary payload identities must use put() or range commit.  Exercise
      // the generation conflict on the supported metadata path.
      auto metadataManifest = smallManifest;
      metadataManifest.objectName = "/spec188/metadata";
      metadataManifest.objectType = "ndn-segmented-data";
      metadataManifest.sha256.clear();
      metadataManifest.size = 0;
      metadataManifest.segmentCount = 2;
      backend.putManifest(metadataManifest);
      auto sameGenerationMutation = metadataManifest;
      sameGenerationMutation.parentGeneration = 7;
      bool mutationRejected = false;
      try {
        backend.putManifest(sameGenerationMutation);
      }
      catch (const std::runtime_error& error) {
        mutationRejected = std::string(error.what()).find(
                             "repo-generation-conflict") != std::string::npos;
      }
      require(mutationRejected,
              "same-generation manifest identity mutation was accepted");
      require(backend.erase(metadataManifest.objectName),
              "metadata manifest erase failed");

      const std::vector<uint8_t> emptyPayload;
      const auto emptyManifest = manifestFor("/spec188/empty", emptyPayload);
      backend.put(emptyManifest, emptyPayload);
      require(backend.has(emptyManifest.objectName) &&
                backend.get(emptyManifest.objectName).payload.empty() &&
                backend.getRange(emptyManifest.objectName, {0, 0}).empty(),
              "empty vector object did not survive commit");
      require(backend.erase(smallManifest.objectName),
              "small object erase failed");
    }
    {
      RepoCore core({"/spec188/core", 1024 * 1024, 0, 0.0, 1.0,
                     "embedded", {"object"}},
                    makeFilesystemRepoStore((root / "core").string(), 64, 64));
      const auto committed = core.put("/spec188/core/object", smallPayload,
                                      "spec188-core");
      require(committed.objectName == "/spec188/core/object" &&
                core.get("/spec188/core/object") == smallPayload,
              "RepoCore did not use the filesystem authority factory");
    }

    std::vector<uint8_t> largePayload(1200 * 1024);
    for (size_t index = 0; index < largePayload.size(); ++index) {
      largePayload[index] = static_cast<uint8_t>((index * 17U + 3U) & 0xffU);
    }
    const auto largeManifest = manifestFor("/spec188/large", largePayload, 9);
    {
      FilesystemRepoStoreBackend backend(root.string(), 512 * 1024, 1024);
      const size_t firstLength = 400 * 1024;
      const size_t middleLength = 400 * 1024;
      const size_t lastOffset = firstLength + middleLength;
      backend.putRange(largeManifest, {0, firstLength},
                       std::vector<uint8_t>(largePayload.begin(),
                                            largePayload.begin() + firstLength));
      require(!backend.has(largeManifest.objectName),
              "staging payload became visible before commit");
      backend.putRange(largeManifest, {firstLength, middleLength},
                       std::vector<uint8_t>(largePayload.begin() + firstLength,
                                            largePayload.begin() + lastOffset));
      backend.putRange(largeManifest,
                       {lastOffset, largePayload.size() - lastOffset},
                       std::vector<uint8_t>(largePayload.begin() + lastOffset,
                                            largePayload.end()));
      backend.commitRanges(largeManifest);
      require(backend.has(largeManifest.objectName),
              "large range object was not committed");
      require(backend.getRange(largeManifest.objectName, {firstLength, 32}) ==
                std::vector<uint8_t>(largePayload.begin() + firstLength,
                                     largePayload.begin() + firstLength + 32),
              "middle range read failed");
      require(backend.getRange(largeManifest.objectName,
                               {largePayload.size() - 32, 32}) ==
                std::vector<uint8_t>(largePayload.end() - 32,
                                     largePayload.end()),
              "last range read failed");
      bool fullReadRejected = false;
      try {
        (void)backend.get(largeManifest.objectName);
      }
      catch (const std::runtime_error& error) {
        fullReadRejected = std::string(error.what()).find(
                             "repo-large-object-vector-path-disabled") !=
                           std::string::npos;
      }
      require(fullReadRejected && backend.fullCopyFallbackCount() == 1,
              "large vector compatibility path was not explicitly rejected");

      bool outOfRangeRejected = false;
      try {
        (void)backend.getRange(largeManifest.objectName,
                               {largePayload.size() - 1, 2});
      }
      catch (const std::out_of_range&) {
        outOfRangeRejected = true;
      }
      require(outOfRangeRejected, "out-of-range read was accepted");
    }

    const auto orphanManifest = manifestFor("/spec188/orphan", largePayload, 11);
    {
      FilesystemRepoStoreBackend backend(root.string(), 512 * 1024, 1024);
      backend.putRange(orphanManifest, {0, 128},
                       std::vector<uint8_t>(largePayload.begin(),
                                            largePayload.begin() + 128));
      require(!backend.has(orphanManifest.objectName),
              "orphan staging object became visible");
    }
    {
      FilesystemRepoStoreBackend restarted(root.string(), 512 * 1024, 1024);
      require(restarted.recoverOrphans() == 0,
              "orphan recovery was not idempotent");
      require(!restarted.has(orphanManifest.objectName),
              "orphan object survived restart recovery");
      require(restarted.getManifest(largeManifest.objectName).sha256 ==
                largeManifest.sha256,
              "durable manifest did not survive restart");
      require(restarted.has("/spec188/empty") &&
                restarted.get("/spec188/empty").payload.empty(),
              "empty object did not survive restart");
      require(restarted.usedBytes() == largeManifest.size,
              "disk usage does not reflect committed payload only");
      require(restarted.erase(largeManifest.objectName),
              "committed object erase failed");
      require(!restarted.has(largeManifest.objectName),
              "erased object remained visible");
      require(restarted.erase("/spec188/empty"),
              "empty object erase failed");
    }

    const auto cancelRoot = std::filesystem::temp_directory_path() /
      ("ndnsf-spec188-file-cancel-" + std::to_string(::getpid()));
    removeTree(cancelRoot);
    {
      FilesystemRepoStoreBackend backend(cancelRoot.string(), 512 * 1024, 1024,
                                         "spec188-cancel");
      const auto cancelManifest = manifestFor(
        "/spec188/cancel/retry", largePayload, 13);
      backend.putRange(cancelManifest, {0, 128},
                       std::vector<uint8_t>(largePayload.begin(),
                                            largePayload.begin() + 128));
      require(!backend.has(cancelManifest.objectName),
              "cancelled staging object became visible");
      backend.abortRanges(cancelManifest.objectName);
      require(!backend.has(cancelManifest.objectName) &&
                backend.usedBytes() == 0 &&
                std::filesystem::is_empty(cancelRoot / "staging"),
              "range cancellation left visible bytes or staging state");

      // A cancelled operation releases its reservation and can be retried
      // from a clean generation without relying on process restart recovery.
      constexpr size_t retryRangeBytes = 512 * 1024;
      for (size_t offset = 0; offset < largePayload.size();
           offset += retryRangeBytes) {
        const auto length = std::min(retryRangeBytes,
                                     largePayload.size() - offset);
        backend.putRange(
          cancelManifest, {offset, length},
          std::vector<uint8_t>(largePayload.begin() + offset,
                               largePayload.begin() + offset + length));
      }
      backend.commitRanges(cancelManifest);
      require(backend.has(cancelManifest.objectName) &&
                backend.getRange(cancelManifest.objectName,
                                 {largePayload.size() - 32, 32}) ==
                  std::vector<uint8_t>(largePayload.end() - 32,
                                       largePayload.end()),
              "cancelled range operation could not be retried cleanly");
    }
    removeTree(cancelRoot);

    const auto faultRoot = std::filesystem::temp_directory_path() /
      ("ndnsf-spec188-file-faults-" + std::to_string(::getpid()));
    removeTree(faultRoot);
    {
      FilesystemRepoStoreBackend backend(faultRoot.string(), 64, 64,
                                         "spec188-faults");
      RepoObjectManifest diskManifest = manifestFor(
        "/spec188/fault/disk-reservation", {}, 17);
      // Stay below the artifact hard limit so the Repo reservation check is
      // the first boundary; no payload allocation is needed for this probe.
      diskManifest.size = 1ULL << 50;
      bool diskRejected = false;
      try {
        backend.putRange(diskManifest, {0, 0}, {});
      }
      catch (const std::runtime_error& error) {
        diskRejected = std::string(error.what()).find(
                         "repo-file-disk-reservation-failed") !=
                       std::string::npos;
      }
      require(diskRejected && !backend.has(diskManifest.objectName),
              "insufficient disk reservation was not rejected atomically");

      if (::geteuid() == 0)
        throw std::runtime_error(
          "permission fault injection requires non-root execution");
      struct PermissionRestore
      {
        std::filesystem::path path;

        ~PermissionRestore()
        {
          std::error_code error;
          std::filesystem::permissions(
            path, std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace, error);
        }
      } permissionRestore{faultRoot / "manifests"};
      std::error_code permissionError;
      std::filesystem::permissions(
        faultRoot / "manifests", std::filesystem::perms::owner_read |
          std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace, permissionError);
      require(!permissionError, "failed to remove manifest write permission");
      const std::vector<uint8_t> permissionPayload{0x5a, 0x6b};
      const auto permissionManifest = manifestFor(
        "/spec188/fault/manifest-permission", permissionPayload, 19);
      bool permissionRejected = false;
      try {
        backend.put(permissionManifest, permissionPayload);
      }
      catch (const std::runtime_error& error) {
        permissionRejected = std::string(error.what()).find(
                              "repo-file-metadata-write-failed") !=
                            std::string::npos;
      }
      require(permissionRejected,
              "manifest permission failure was not surfaced by atomic commit");
      require(!backend.has(permissionManifest.objectName) &&
                backend.recoverOrphans() == 0 && backend.usedBytes() == 0 &&
                std::filesystem::is_empty(faultRoot / "staging"),
              "permission failure left durable payload or reservation state");
    }
    removeTree(faultRoot);

    removeTree(root);
    std::cout << "SPEC188_REPO_FILE_BACKEND_OK" << std::endl;
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "Spec188RepoFileBackend failed: " << error.what() << std::endl;
    return 1;
  }
}
