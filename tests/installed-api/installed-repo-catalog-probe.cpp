#include <ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

class Measurement
{
public:
  explicit Measurement(const char* phase)
    : m_phase(phase)
    , m_started(std::chrono::steady_clock::now())
    , m_cpu(std::clock())
  {
  }

  void
  report(std::optional<size_t> count, size_t bytes = 0) const
  {
    const auto wall = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - m_started).count();
    const auto cpu = static_cast<double>(std::clock() - m_cpu) / CLOCKS_PER_SEC;
    std::cout << "{\"phase\":\"" << m_phase << "\",\"wallSeconds\":" << wall
              << ",\"cpuSeconds\":" << cpu << ",\"manifests\":"
              << (count ? std::to_string(*count) : "null")
              << ",\"metadataBytes\":" << bytes << "}" << std::endl;
  }

private:
  const char* m_phase;
  std::chrono::steady_clock::time_point m_started;
  std::clock_t m_cpu;
};

} // namespace

int
main(int argc, char** argv)
{
  try {
    if (argc != 2)
      throw std::invalid_argument("usage: installed-repo-catalog-probe EXISTING_ROOT");
    const std::filesystem::path root(argv[1]);
    if (!root.is_absolute() || std::filesystem::canonical(root) != root ||
        !std::filesystem::is_directory(root / "manifests") ||
        !std::filesystem::is_directory(root / "payloads") ||
        !std::filesystem::is_directory(root / "staging"))
      throw std::invalid_argument("existing canonical Repo directory required");
    Measurement construction("native-backend-construction-and-recovery");
    auto store = ndnsf_distributed_repo::makeFilesystemRepoStore(
      root.string(), 1U << 20, 1U << 20, "catalog-metadata-probe");
    construction.report(std::nullopt);
    Measurement firstRead("first-native-catalog-read");
    auto first = store->listManifests();
    firstRead.report(first.size());
    if (first.empty() || first.size() > 4096)
      throw std::runtime_error("catalog probe requires 1..4096 existing manifests");
    Measurement snapshot("serialize-native-catalog-snapshot");
    std::map<std::string, std::string> encoded;
    size_t bytes = 0;
    for (const auto& manifest : first) {
      auto json = manifest.toJson();
      bytes += json.size();
      if (!encoded.emplace(manifest.objectName, std::move(json)).second)
        throw std::runtime_error("duplicate catalog object name");
    }
    snapshot.report(first.size(), bytes);
    first.clear();
    first.shrink_to_fit();
    Measurement secondRead("cached-native-catalog-read");
    const auto current = store->listManifests();
    secondRead.report(current.size());
    Measurement comparison("serialize-and-compare-native-catalog");
    if (current.size() != encoded.size())
      throw std::runtime_error("catalog changed during probe");
    for (const auto& manifest : current) {
      const auto previous = encoded.find(manifest.objectName);
      if (previous == encoded.end() || previous->second != manifest.toJson())
        throw std::runtime_error("catalog changed during probe");
    }
    comparison.report(current.size(), bytes);
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "CATALOG_PROBE_FAILED: " << error.what() << std::endl;
    return 1;
  }
}
