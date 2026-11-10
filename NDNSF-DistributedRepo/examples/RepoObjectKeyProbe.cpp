/** Probe: stored object lookup must use the ALTERNATE URI of the object name.
 *
 * A Repo that just started has no in-process packet index, so it resolves a packet
 * name by reading the containing object manifest from the store. That store keys
 * manifests by sha256 of the exact object-name string it was given at insert time,
 * which is the ALTERNATE URI. Looking it up with the CANONICAL URI therefore misses.
 * This program reproduces the store-level fact without a model or a network.
 */
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"

#include <ndn-cxx/name.hpp>

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
  if (!condition) throw std::runtime_error(message);
}

} // namespace

int
main()
{
  const auto root = std::filesystem::temp_directory_path() /
    ("ndnsf-repo-key-probe-" + std::to_string(::getpid()));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);

  ndn::Name objectName("/ndnsf/di/probe/requester/NDNSF/DI/STORED-MODEL/objects/bundle-0");
  objectName.appendVersion(0);
  const auto canonical = objectName.toUri(ndn::name::UriFormat::CANONICAL);
  const auto alternate = objectName.toUri(ndn::name::UriFormat::ALTERNATE);
  require(canonical != alternate, "canonical and alternate URIs must differ");

  std::vector<uint8_t> payload(8192, 0x5a);
  RepoObjectManifest manifest;
  manifest.objectName = alternate;
  manifest.objectType = "original-packet-set";
  manifest.sha256 = sha256Hex(payload);
  manifest.size = payload.size();
  manifest.segmentCount = 2;
  manifest.generation = 1;
  manifest.replicationFactor = 1;
  manifest.writeConsistency = "ALL";
  manifest.requiredWriteAcks = 1;
  manifest.lifecycleState = "COMMITTED";
  manifest.packetNames = {canonical + "/seg=0", canonical + "/seg=1"};
  manifest.packetOffsets = {0, payload.size() / 2, payload.size()};

  {
    FilesystemRepoStoreBackend backend(root.string(), 1U << 20, 1U << 20);
    backend.put(manifest, payload);
    require(backend.has(alternate), "alternate key must exist after insert");
    std::cout << "inserted alternate=" << alternate << "\n";
  }

  FilesystemRepoStoreBackend reopened(root.string(), 1U << 20, 1U << 20);
  const bool canonicalHit = reopened.has(canonical);
  const bool alternateHit = reopened.has(alternate);
  std::cout << "reopened canonicalHit=" << canonicalHit
            << " alternateHit=" << alternateHit << "\n";
  require(alternateHit, "reopened store must find the object by its alternate name");
  require(!canonicalHit,
          "canonical lookup must miss; callers must use the alternate object name");
  const auto bytes = reopened.getRange(alternate, {0, 16});
  require(bytes.size() == 16 && bytes.front() == 0x5a, "range read failed");

  std::filesystem::remove_all(root, ignored);
  std::cout << "REPO_OBJECT_KEY_PROBE_OK: manifests are keyed by the ALTERNATE URI\n";
  return 0;
}
