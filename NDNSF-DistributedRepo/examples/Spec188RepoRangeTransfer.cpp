#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"

#include "ndn-service-framework/LocalServiceRegistry.hpp"

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

void
removeTree(const std::filesystem::path& path)
{
  std::error_code error;
  std::filesystem::remove_all(path, error);
  if (error) {
    throw std::runtime_error("failed to remove range test tree: " + error.message());
  }
}

RepoObjectManifest
makeRangeManifest(const std::string& objectName,
                  const std::vector<uint8_t>& payload)
{
  RepoObjectManifest manifest;
  manifest.objectName = objectName;
  manifest.objectType = "spec188-range-transfer";
  manifest.sha256 = sha256Hex(payload);
  manifest.size = payload.size();
  manifest.segmentCount = 1;
  manifest.generation = 1;
  manifest.policyEpoch = "spec188";
  return manifest;
}

std::vector<uint8_t>
slice(const std::vector<uint8_t>& payload, size_t offset, size_t length)
{
  return std::vector<uint8_t>(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                              payload.begin() + static_cast<std::ptrdiff_t>(offset + length));
}

} // namespace

int
main()
{
  try {
    const auto root = std::filesystem::temp_directory_path() /
      ("ndnsf-spec188-range-transfer-" + std::to_string(::getpid()));
    removeTree(root);

    constexpr size_t chunkBytes = 128 * 1024;
    std::vector<uint8_t> payload(3 * chunkBytes + 17);
    for (size_t i = 0; i < payload.size(); ++i) {
      payload[i] = static_cast<uint8_t>((i * 29U + 11U) & 0xffU);
    }
    const auto manifest = makeRangeManifest("/spec188/range/model", payload);
    auto backend = std::make_shared<FilesystemRepoStoreBackend>(
      root.string(), 256 * 1024, 4096, "spec188-range-transfer");
    RepoNode node(ndn::Name(RepoClient::DEFAULT_SERVICE_NAME),
                  {"/spec188/repo", 16 * 1024 * 1024, 0, 0.0, 1.0,
                   "local", {"model"}},
                  backend);
    ndn_service_framework::LocalServiceRegistry registry;
    node.registerLocalServices(registry);

    const auto prefix = ndn::Name(RepoClient::DEFAULT_SERVICE_NAME);
    require(backend->supportsRange(), "filesystem repo did not advertise range support");
    require(registry.hasService(makeRepoServiceName(prefix, "STORE_RANGE")) &&
              registry.hasService(makeRepoServiceName(prefix, "COMMIT_RANGES")) &&
              registry.hasService(makeRepoServiceName(prefix, "FETCH_RANGE")),
            "range services were not registered");

    for (size_t offset = 0; offset < payload.size(); offset += chunkBytes) {
      const auto length = std::min(chunkBytes, payload.size() - offset);
      const auto bytes = slice(payload, offset, length);
      RepoClient::localPutRange(registry, prefix, manifest,
                                {offset, length}, bytes);
      // Retransmitting an identical verified range is explicitly idempotent.
      RepoClient::localPutRange(registry, prefix, manifest,
                                {offset, length}, bytes);
    }
    require(backend->size() == 0,
            "range writes became visible before COMMIT_RANGES");
    require(std::filesystem::is_empty(root / "manifests"),
            "range writes published a manifest before commit");

    bool precommitReadRejected = false;
    try {
      (void)RepoClient::localGetRange(
        registry, prefix, manifest.objectName, {chunkBytes, chunkBytes});
    }
    catch (const std::runtime_error&) {
      precommitReadRejected = true;
    }
    require(precommitReadRejected,
            "staged range was visible before manifest commit");
    const auto committed = RepoClient::localCommitRanges(registry, prefix, manifest);
    require(committed.objectName == manifest.objectName &&
              committed.sha256 == manifest.sha256 && committed.size == manifest.size,
            "range commit returned the wrong durable manifest");
    require(RepoClient::localGetRange(registry, prefix, manifest.objectName,
                                      {payload.size() - 17, 17}) ==
              slice(payload, payload.size() - 17, 17),
            "committed tail range did not round trip");
    require(backend->fullCopyFallbackCount() == 0,
            "range transfer used a full-copy fallback");

    const auto rangeFallbacks = backend->fullCopyFallbackCount();
    require(rangeFallbacks == 0,
            "range transfer used a full-copy fallback before legacy probe");
    bool fullFetchRejected = false;
    try {
      (void)RepoClient::localGet(registry, prefix, manifest.objectName);
    }
    catch (const std::runtime_error& error) {
      fullFetchRejected = std::string(error.what()).find("vector-path-disabled") !=
                         std::string::npos;
    }
    require(fullFetchRejected,
            "large-object compatibility FETCH was not rejected explicitly");

    bool invalidRangeRejected = false;
    try {
      (void)RepoClient::localGetRange(registry, prefix, manifest.objectName,
                                      {manifest.size, 1});
    }
    catch (const std::runtime_error&) {
      invalidRangeRejected = true;
    }
    require(invalidRangeRejected, "out-of-bounds range read was accepted");

    auto badManifest = manifest;
    badManifest.sha256[0] = badManifest.sha256[0] == '0' ? '1' : '0';
    bool badDigestRejected = false;
    try {
      RepoClient::localPutRange(registry, prefix, badManifest,
                                {0, chunkBytes}, slice(payload, 0, chunkBytes));
    }
    catch (const std::runtime_error&) {
      badDigestRejected = true;
    }
    require(badDigestRejected, "range write accepted a manifest digest mismatch");

    std::vector<uint8_t> empty;
    auto emptyManifest = makeRangeManifest("/spec188/range/empty", empty);
    RepoClient::localPutRange(registry, prefix, emptyManifest, {0, 0}, empty);
    const auto emptyCommitted = RepoClient::localCommitRanges(
      registry, prefix, emptyManifest);
    require(emptyCommitted.size == 0 &&
              RepoClient::localGetRange(registry, prefix, emptyManifest.objectName,
                                        {0, 0}).empty(),
            "zero-length range object did not commit or read");

    const auto abortManifest = makeRangeManifest("/spec188/range/abort", payload);
    RepoClient::localPutRange(registry, prefix, abortManifest,
                              {0, chunkBytes}, slice(payload, 0, chunkBytes));
    RepoClient::localAbortRanges(registry, prefix, abortManifest.objectName);
    bool abortedReadRejected = false;
    try {
      (void)RepoClient::localGetRange(registry, prefix, abortManifest.objectName,
                                      {0, chunkBytes});
    }
    catch (const std::runtime_error&) {
      abortedReadRejected = true;
    }
    require(abortedReadRejected, "aborted range transfer remained readable");

    std::cout << "SPEC188_REPO_RANGE_TRANSFER_OK {\"rangeBytes\":"
              << payload.size() << ",\"chunks\":"
              << ((payload.size() + chunkBytes - 1) / chunkBytes)
              << ",\"fullCopyFallbacks\":"
              << backend->fullCopyFallbackCount() << "}\n";
    removeTree(root);
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "SPEC188_REPO_RANGE_TRANSFER_FAIL " << error.what() << "\n";
    return 1;
  }
}
