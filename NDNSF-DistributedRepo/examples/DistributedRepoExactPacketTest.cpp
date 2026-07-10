#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/encoding/block.hpp>
#include <ndn-cxx/security/key-chain.hpp>

#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ndnsf_distributed_repo;

namespace {

void
require(bool condition, const std::string& message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::string
packetName(const std::vector<uint8_t>& wire)
{
  ndn::Block block(ndn::span<const uint8_t>(wire.data(), wire.size()));
  block.parse();
  return ndn::Data(block).getName().toUri();
}

template<typename F>
void
requireThrowsContaining(F&& operation,
                        const std::string& expected,
                        const std::string& message)
{
  try {
    operation();
  }
  catch (const std::exception& e) {
    require(std::string(e.what()).find(expected) != std::string::npos,
            message + ": " + e.what());
    return;
  }
  throw std::runtime_error(message + ": no exception");
}

} // namespace

int
main()
{
  const std::string databasePath = "/tmp/ndnsf-repo-exact-packet-test.sqlite3";
  std::remove(databasePath.c_str());

  StorageCapability capability;
  capability.repoNode = "/repo/exact";
  capability.freeBytes = 8 * 1024 * 1024;
  capability.storageClasses = {"ndn-data"};

  ndn::KeyChain keyChain;
  const ndn::Name identityName("/test/repo/exact-publisher");
  try {
    keyChain.createIdentity(identityName);
  }
  catch (const std::exception&) {
  }
  const ndn::security::SigningInfo signingInfo(
    ndn::security::SigningInfo::SIGNER_TYPE_ID, identityName);
  const auto versionedName = ndn::Name("/data/model/qwen").appendVersion(42).toUri();
  const std::vector<uint8_t> payload(18000, 0x5a);

  RepoObjectManifest storedManifest;
  std::vector<std::vector<uint8_t>> originalWires;
  {
    RepoNode node(ndn::Name("/NDNSF/DistributedRepo"), capability,
                  makeTieredRepoStore(databasePath, 4096));
    const auto status = RepoClient::insertPayload(
      node, versionedName, payload, keyChain, signingInfo, {}, 4096);
    require(status.state == "DONE", "exact packet insertion failed: " + status.message);

    storedManifest = RepoClient::getManifest(node, versionedName);
    require(storedManifest.segmentCount > 1, "test payload was not segmented");
    require(storedManifest.packetNames.size() == storedManifest.segmentCount,
            "manifest does not index every exact Data name");
    for (const auto& name : storedManifest.packetNames) {
      require(name.find(versionedName + "/seg=") == 0,
              "Repo changed original versioned Data name: " + name);
      auto wire = RepoClient::getDataPacket(node, name);
      require(packetName(wire) == name, "stored wire name differs from exact key");
      originalWires.push_back(std::move(wire));
    }
    require(RepoClient::getDataPackets(node, storedManifest) == originalWires,
            "packet-set consumer changed order or wire bytes");

    auto wrongCount = storedManifest;
    ++wrongCount.segmentCount;
    requireThrowsContaining(
      [&] { RepoClient::getDataPackets(node, wrongCount); },
      "repo-packet-index-invalid",
      "packet-set consumer accepted count mismatch");

    auto duplicate = storedManifest;
    duplicate.packetNames.back() = duplicate.packetNames.front();
    requireThrowsContaining(
      [&] { RepoClient::getDataPackets(node, duplicate); },
      "duplicate",
      "packet-set consumer accepted duplicate name");

    auto missing = storedManifest;
    missing.packetNames.back() = versionedName + "/seg=999999";
    requireThrowsContaining(
      [&] { RepoClient::getDataPackets(node, missing); },
      "object not found",
      "packet-set consumer accepted missing packet");

    bool aliasFound = true;
    try {
      node.get(versionedName + "/ndn-data/0");
    }
    catch (const std::out_of_range&) {
      aliasFound = false;
    }
    require(!aliasFound, "packet API created a /ndn-data/N alias");

    RepoDataReference reference;
    reference.objectName = versionedName;
    reference.dataPrefix = versionedName;
    reference.expectedSha256 = sha256Hex([&] {
      std::vector<uint8_t> joined;
      for (const auto& wire : originalWires) {
        joined.insert(joined.end(), wire.begin(), wire.end());
      }
      return joined;
    }());
    const auto repeated = node.insertWirePackets(reference, originalWires);
    require(repeated.state == "DONE", "idempotent packet insertion failed");

    auto conflicting = originalWires;
    conflicting.front().back() ^= 0x01;
    const auto conflict = node.insertWirePackets(reference, conflicting);
    require(conflict.state == "FAILED", "same-name different-wire conflict was accepted");
    require(conflict.message.find("conflict") != std::string::npos,
            "conflict failure did not explain immutable-name conflict");
  }

  {
    RepoNode restarted(ndn::Name("/NDNSF/DistributedRepo"), capability,
                       makeTieredRepoStore(databasePath, 4096));
    const auto manifest = RepoClient::getManifest(restarted, versionedName);
    require(manifest.packetNames == storedManifest.packetNames,
            "packet-name index did not survive SQLite restart");
    require(RepoClient::getDataPackets(restarted, manifest) == originalWires,
            "packet-set consumer changed wire bytes across SQLite restart");
  }

  std::remove(databasePath.c_str());
  std::cout << "EXACT_DATA_PACKET_STORAGE_OK" << std::endl;
  return 0;
}
