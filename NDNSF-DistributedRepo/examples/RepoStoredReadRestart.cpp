/** Repo stored-read restart test.
 *
 * Reproduces the production read path for a stored model object: a Repo process
 * writes original packet sets, exits, and a fresh Repo opens the same store. Each
 * packet is then requested by exact name through RepoCore::getDataPacket, the same
 * call the network handler uses. A fresh process has no in-memory packet index, so
 * this is the case that silently fell back to scanning every manifest.
 */
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/name.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>

#include <chrono>
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

constexpr std::size_t OBJECTS = 200;
constexpr std::size_t PACKETS = 1200;

ndn::Name
objectName(std::size_t index)
{
  ndn::Name name("/ndnsf/di/probe/requester/NDNSF/DI/STORED-MODEL");
  name.append("objects").append("bundle-" + std::to_string(index)).appendVersion(0);
  return name;
}

ndn::Name
packetName(const ndn::Name& object, std::size_t index)
{
  ndn::Name name(object);
  name.appendSegment(index);
  return name;
}

} // namespace

int
main()
{
  const auto root = std::filesystem::temp_directory_path() /
    ("ndnsf-repo-stored-read-" + std::to_string(::getpid()));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  const auto store = root / "store";

  StorageCapability capability;
  capability.repoNode = "/ndnsf/di/probe/repo";
  capability.freeBytes = 1ULL << 30;
  capability.availabilityScore = 1.0;
  capability.repoMode = "persistent";

  ndn::KeyChain keyChain("pib-memory:", "tpm-memory:");
  const auto identity = keyChain.createIdentity(ndn::Name("/ndnsf/di/probe/signer"));

  ndn::Name firstTarget;
  ndn::Name splitTarget;
  {
    auto backend = makeFilesystemRepoStore(store.string(), 1U << 20, 1U << 20);
    RepoCore core(capability, backend);
    const auto writePacketSet = [&](const ndn::Name& name, std::size_t first,
                                    std::size_t last, ndn::Name* firstPacketName) {
      std::vector<std::shared_ptr<ndn::Data>> packets;
      for (std::size_t packet = first; packet < last; ++packet) {
        auto data = std::make_shared<ndn::Data>(packetName(name, packet));
        data->setContent(std::vector<uint8_t>(48, 0x5a));
        keyChain.sign(*data, ndn::security::signingWithSha256());
        packets.push_back(std::move(data));
      }
      std::vector<uint8_t> payload;
      RepoObjectManifest manifest;
      // The object name is the first packet name, and manifests store ALTERNATE URIs.
      manifest.objectName = packets.front()->getName().toUri(ndn::name::UriFormat::ALTERNATE);
      manifest.objectType = "original-packet-set";
      manifest.segmentCount = packets.size();
      manifest.generation = 1;
      manifest.replicationFactor = 1;
      manifest.writeConsistency = "ALL";
      manifest.requiredWriteAcks = 1;
      manifest.lifecycleState = "COMMITTED";
      for (const auto& packet : packets) {
        const auto& wire = packet->wireEncode();
        manifest.packetNames.push_back(packet->getName().toUri(ndn::name::UriFormat::ALTERNATE));
        manifest.packetOffsets.push_back(payload.size());
        payload.insert(payload.end(), wire.begin(), wire.end());
      }
      manifest.packetOffsets.push_back(payload.size());
      manifest.size = payload.size();
      manifest.sha256 = sha256Hex(payload);
      backend->put(manifest, payload);
      if (firstPacketName) *firstPacketName = packets.front()->getName();
    };
    for (std::size_t object = 0; object < OBJECTS; ++object) {
      writePacketSet(objectName(object), 0, PACKETS,
                     object == 0 ? &firstTarget : nullptr);
    }
    // Split one object into two committed groups whose first packets are
    // segment 0 and segment 600. Production stored-model objects use exactly
    // this shape, and a fresh Repo must serve segment 700 without scanning.
    const auto splitName = objectName(OBJECTS);
    writePacketSet(splitName, 0, PACKETS / 2, nullptr);
    writePacketSet(splitName, PACKETS / 2, PACKETS, nullptr);
    splitTarget = packetName(splitName, PACKETS / 2 + 100);
  }

  auto reopened = makeFilesystemRepoStore(store.string(), 1U << 20, 1U << 20);
  RepoCore fresh(capability, reopened);
  const auto& target = firstTarget;
  const auto accept = [](const RepoObjectManifest& manifest) {
    return manifest.objectType == "original-packet-set";
  };

  std::size_t served = 0;
  const auto begin = std::chrono::steady_clock::now();
  for (const std::size_t index : {std::size_t(0), std::size_t(1), std::size_t(2)}) {
    ndn::Interest interest(packetName(target.getPrefix(-1), index));
    interest.setCanBePrefix(false);
    interest.setMustBeFresh(false);
    const auto wire = fresh.getDataPacket(interest, accept);
    ndn::Data data(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
    require(data.getName() == interest.getName(), "served packet name mismatch");
    ++served;
  }
  {
    // A non-zero first segment is the production shape that previously forced
    // the linear manifest scan and timed out.
    ndn::Interest interest(splitTarget);
    interest.setCanBePrefix(false);
    interest.setMustBeFresh(false);
    const auto wire = fresh.getDataPacket(interest, accept);
    ndn::Data data(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
    require(data.getName() == interest.getName(), "served split-group packet name mismatch");
    ++served;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - begin).count();
  std::cout << "objects=" << OBJECTS + 1 << " packetsPerObject=" << PACKETS
            << " served=" << served << " elapsedMs=" << elapsed << "\n";
  require(elapsed < 1000, "exact packet lookup must not scan the whole store");
  std::filesystem::remove_all(root, ignored);
  std::cout << "REPO_STORED_READ_RESTART_OK\n";
  return 0;
}
