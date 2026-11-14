// Writes original Data records into a filesystem Repo store so NDNSF_RepoPacketServer can
// serve them. Evidence tool for the Spec 240 restart acceptance; not a production path.
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/repo-client-types.hpp"
#include "ndnsf-distributed-repo/repo-store-api.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace repo = ndnsf_distributed_repo;

int
main(int argc, char** argv)
{
  if (argc != 5) {
    std::fprintf(stderr, "usage: spec240-seed-store STORE_DIR NODE_NAME PREFIX SOURCE\n");
    return 2;
  }
  const std::string storeDir = argv[1];
  const std::string nodeName = argv[2];
  const ndn::Name prefix(argv[3]);
  std::ifstream input(argv[4], std::ios::binary);
  if (!input) {
    std::fprintf(stderr, "cannot read %s\n", argv[4]);
    return 1;
  }
  const std::vector<uint8_t> payload{std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>()};

  repo::StorageCapability capability;
  capability.repoNode = nodeName;
  capability.repoMode = "persistent";
  capability.freeBytes = 64ULL * 1024 * 1024;
  auto node = std::make_shared<repo::RepoNode>(
    prefix, capability,
    std::make_shared<repo::FilesystemRepoStoreBackend>(storeDir, 1 << 20, 4096,
                                                       "spec240-accept"),
    nodeName, nodeName, storeDir + "/seed.sqlite");
  ndn::KeyChain keyChain("pib-memory:spec240-accept", "tpm-memory:spec240-accept");

  for (uint32_t segment = 0; segment < 2; ++segment) {
    const ndn::Name name = ndn::Name(prefix).append("object").appendSegment(segment);
    ndn::Data packet(name);
    packet.setContent(ndn::span<const uint8_t>(payload.data(), payload.size()));
    packet.setFreshnessPeriod(ndn::time::seconds(3600));
    keyChain.sign(packet, ndn::security::signingWithSha256());
    const auto wire = packet.wireEncode();
    const std::vector<uint8_t> bytes(wire.begin(), wire.end());
    const auto manifest = repo::RepoStoreApi::putDataPacket(*node, bytes);
    std::printf("STORED %s object=%s\n", name.toUri().c_str(),
                manifest.objectName.c_str());
  }
  return 0;
}
