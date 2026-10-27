#include <ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp>
#include <ndnsf-distributed-repo/RepoCore.hpp>
#include <ndnsf-distributed-repo/RepoProtocol.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace ndnsf_distributed_repo;
using Clock = std::chrono::steady_clock;

struct Counter
{
  size_t calls = 0;
  double wall = 0;
  double cpu = 0;
};

struct Span
{
  Counter& counter;
  Clock::time_point start = Clock::now();
  std::clock_t cpu = std::clock();

  ~Span()
  {
    ++counter.calls;
    counter.wall += std::chrono::duration<double>(Clock::now() - start).count();
    counter.cpu += static_cast<double>(std::clock() - cpu) / CLOCKS_PER_SEC;
  }
};

// Forward every operation used by Core admission to the actual selected backend.
// Timers cover disjoint interface calls, including failures; there is no storage logic here.
class TimedStore final : public RepoStoreBackend
{
public:
  explicit TimedStore(std::shared_ptr<RepoStoreBackend> store)
    : m_store(std::move(store))
  {
  }

  template<typename F>
  auto
  call(const char* name, F&& function) const -> decltype(function())
  {
    Span span{m_counters[name]};
    return function();
  }

  void put(const RepoObjectManifest& m, std::vector<uint8_t> p) override
  { call("put", [&] { m_store->put(m, std::move(p)); }); }
  void putManifest(const RepoObjectManifest& m) override
  { call("putManifest", [&] { m_store->putManifest(m); }); }
  StoredObject get(const std::string& n) const override
  { return call("get", [&] { return m_store->get(n); }); }
  bool has(const std::string& n) const override
  { return call("has", [&] { return m_store->has(n); }); }
  bool erase(const std::string& n) override
  { return call("erase", [&] { return m_store->erase(n); }); }
  size_t size() const override
  { return call("size", [&] { return m_store->size(); }); }
  uint64_t usedBytes() const override
  { return call("usedBytes", [&] { return m_store->usedBytes(); }); }
  std::vector<RepoObjectManifest> listManifests() const override
  { return call("listManifests", [&] { return m_store->listManifests(); }); }
  void putRange(const RepoObjectManifest& m, RepoByteRange r,
                const std::vector<uint8_t>& p) override
  { call("putRange", [&] { m_store->putRange(m, r, p); }); }
  void commitRanges(const RepoObjectManifest& m) override
  { call("commitRanges", [&] { m_store->commitRanges(m); }); }
  std::vector<uint8_t> getRange(const std::string& n, RepoByteRange r) const override
  { return call("getRange", [&] { return m_store->getRange(n, r); }); }
  RepoObjectManifest getManifest(const std::string& n) const override
  { return call("getManifest", [&] { return m_store->getManifest(n); }); }
  void abortRanges(const std::string& n) override
  { call("abortRanges", [&] { m_store->abortRanges(n); }); }
  bool supportsRange() const noexcept override { return m_store->supportsRange(); }
  bool supportsManifestLookup() const noexcept override
  { return m_store->supportsManifestLookup(); }

  void
  reportAndReset(const char* phase, const Counter& total)
  {
    std::cout << "{\"phase\":\"" << phase << "\",\"wallSeconds\":" << total.wall
              << ",\"cpuSeconds\":" << total.cpu << ",\"backend\":[";
    bool first = true;
    for (const auto& [name, counter] : m_counters) {
      if (!first) std::cout << ',';
      first = false;
      std::cout << "{\"method\":\"" << name << "\",\"calls\":" << counter.calls
                << ",\"wallSeconds\":" << counter.wall
                << ",\"cpuSeconds\":" << counter.cpu << '}';
    }
    std::cout << "]}" << std::endl;
    m_counters.clear();
  }

private:
  std::shared_ptr<RepoStoreBackend> m_store;
  mutable std::map<std::string, Counter> m_counters;
};

} // namespace

int
main(int argc, char** argv)
{
  try {
    if (argc != 4)
      throw std::invalid_argument("usage: installed-repo-packet-cost-probe ROOT MANIFEST WIRE");
    const std::filesystem::path root(argv[1]);
    if (!root.is_absolute() || std::filesystem::canonical(root) != root)
      throw std::invalid_argument("owned canonical snapshot root required");
    const std::filesystem::path manifestPath(argv[2]), wire(argv[3]);
    if (!std::filesystem::is_regular_file(manifestPath) ||
        std::filesystem::file_size(manifestPath) > 1024 * 1024)
      throw std::invalid_argument("bounded manifest input required");
    std::ifstream input(manifestPath);
    const std::string encoded((std::istreambuf_iterator<char>(input)), {});
    const auto manifest = parseManifestJson(encoded);
    if (manifest.objectType != "original-packet-set" || manifest.packetNames.size() > 256 ||
        manifest.size > (1U << 20) || std::filesystem::file_size(wire) != manifest.size)
      throw std::invalid_argument("bounded original packet set required");
    auto store = makeFilesystemRepoStore(root.string(), 1U << 20, 1U << 20, "packet-cost-probe");
    if (store->has(manifest.objectName))
      throw std::invalid_argument("selected object must be omitted from snapshot");
    auto timed = std::make_shared<TimedStore>(std::move(store));
    StorageCapability capability;
    capability.repoNode = "/spec217/packet-cost-probe";
    capability.freeBytes = 10ULL << 30;
    Counter construction;
    std::unique_ptr<RepoCore> repo;
    { Span span{construction}; repo = std::make_unique<RepoCore>(capability, timed); }
    timed->reportAndReset("core-construction", construction);
    for (const auto* phase : {"new-original-packet-commit", "reuse-original-packet-commit"}) {
      std::cout << "{\"starting\":\"" << phase << "\"}" << std::endl;
      Counter total;
      { Span span{total}; repo->commitOriginalPacketFile(manifest, wire); }
      timed->reportAndReset(phase, total);
    }
    if (repo->getManifest(manifest.objectName).sha256 != manifest.sha256)
      throw std::runtime_error("committed packet set digest mismatch");
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "PACKET_COST_PROBE_FAILED: " << error.what() << std::endl;
    return 1;
  }
}
