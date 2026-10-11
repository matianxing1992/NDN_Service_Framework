#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndnsf-distributed-repo/RepoPacketServer.hpp"

#include <boost/asio/signal_set.hpp>
#include <algorithm>
#include <csignal>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

// Serve a durable replica directory after the writer has released its ownership
// lease. The Actor's original names and signed wires remain authoritative.
int main(int argc, char** argv)
{
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::cout << "Usage: NDNSF_RepoPacketServer STORE_DIR REPO_PREFIX CAPACITY_BYTES\n";
    return 0;
  }
  if (argc != 4) {
    std::cerr << "Usage: NDNSF_RepoPacketServer STORE_DIR REPO_PREFIX CAPACITY_BYTES\n";
    return 2;
  }
  try {
    const ndn::Name replica(argv[2]);
    const std::string declaredCapacity(argv[3]);
    if (replica.empty() || declaredCapacity.empty() ||
        !std::all_of(declaredCapacity.begin(), declaredCapacity.end(),
                     [](unsigned char c) { return c >= '0' && c <= '9'; }))
      throw std::invalid_argument("Repo prefix and positive capacity are required");
    const auto capacity = std::stoull(declaredCapacity);
    if (capacity == 0)
      throw std::invalid_argument("Repo capacity must be positive");
    ndnsf_distributed_repo::StorageCapability capability;
    capability.repoNode = replica.toUri(ndn::name::UriFormat::CANONICAL);
    capability.freeBytes = capacity;
    auto backend = std::make_shared<ndnsf_distributed_repo::FilesystemRepoStoreBackend>(argv[1]);
    auto store = std::make_shared<ndnsf_distributed_repo::RepoCore>(capability, backend);
    std::set<ndn::Name> prefixes;
    for (const auto& record : store->list()) {
      if (record.objectType != "ndn-data-wire" && record.objectType != "original-packet-set")
        continue;
      for (const auto& packet : record.packetNames) {
        const ndn::Name name(packet);
        // An exact root packet can be served directly; segmented discovery
        // also needs its existing original segment prefix.
        prefixes.insert(name.get(-1).isSegment() ? name.getPrefix(-1) : name);
      }
    }
    if (prefixes.empty())
      throw std::runtime_error("No committed original Data packets in the replica directory");
    ndn::Face face;
    ndnsf_distributed_repo::RepoPacketServer server(face, store);
    boost::asio::signal_set signals(face.getIoContext(), SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code& error, int) {
      if (!error) {
        server.close();
        face.shutdown();
      }
    });
    size_t remaining = prefixes.size();
    bool failed = false;
    for (const auto& prefix : prefixes) {
      server.servePrefix(prefix, [&] {
        if (--remaining == 0)
          std::cout << "REPO_ORIGINAL_PACKETS_READY " << prefixes.size() << std::endl;
      }, [&](const std::string& reason) {
        failed = true;
        std::cerr << "Repo original-prefix registration failed: " << reason << std::endl;
        server.close();
        face.shutdown();
      });
    }
    face.processEvents();
    server.close();
    return failed ? 1 : 0;
  }
  catch (const std::exception& error) {
    std::cerr << "Repo packet server: " << error.what() << std::endl;
    return 1;
  }
}
