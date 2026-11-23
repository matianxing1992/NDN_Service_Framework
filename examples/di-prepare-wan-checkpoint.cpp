#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-preparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-filesystem-artifacts.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string_view>

namespace {

std::string
readConfiguration(const std::filesystem::path& path, std::size_t limit)
{
  const auto size = std::filesystem::file_size(path);
  if (size == 0 || size > limit)
    throw std::invalid_argument("Wan preparation configuration size exceeds its bound");
  std::ifstream input(path, std::ios::binary);
  std::string bytes(size, '\0');
  if (!input.read(bytes.data(), bytes.size()) || input.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("Wan preparation configuration could not be read");
  return bytes;
}

void
writeAbsoluteRoleCatalog(const std::filesystem::path& input,
                         const std::filesystem::path& cache,
                         const std::filesystem::path& outputDirectory)
{
  using namespace ndnsf::di;
  if (std::filesystem::exists(outputDirectory))
    throw std::invalid_argument("Wan role catalog requires a fresh output directory");
  auto catalog = nativeParseJson(readConfiguration(input, NATIVE_REQUEST_CATALOG_MAX_BYTES));
  auto& splitter = catalog.at("splitter");
  auto& plan = splitter.at("step_template_v2");
  std::map<std::string, std::string> names;
  for (auto& stage : plan.at("stage_layout")) {
    for (auto& role : stage.at("unit_role_ids")) {
      const auto original = role.get<std::string>();
      const auto absolute = ndn::Name(original).toUri(ndn::name::UriFormat::ALTERNATE);
      if (original.empty() || absolute == "/" || !names.emplace(original, absolute).second)
        throw std::invalid_argument("Wan role catalog has an invalid role cover");
      role = absolute;
    }
  }
  const auto translate = [&names] (NativeJson& role) {
    role = names.at(role.get<std::string>());
  };
  for (auto& step : plan.at("steps")) translate(step.at("unit_id"));
  for (auto& edge : splitter.at("dependencies")) {
    for (auto& role : edge.at("producers")) translate(role);
    for (auto& role : edge.at("consumers")) translate(role);
  }
  auto resources = NativeJson::object();
  for (auto& [role, budget] : splitter.at("resources_by_role").items()) {
    const auto& absolute = names.at(role);
    if (resources.contains(absolute))
      throw std::invalid_argument("Wan roles resolve to the same absolute Name");
    resources[absolute] = std::move(budget);
  }
  splitter["resources_by_role"] = std::move(resources);
  auto facts = catalog.at("preparation_facts");
  catalog.erase("preparation_facts");
  auto& source = catalog.at("source");
  for (const auto* key : {"file", "initializer_file"}) {
    if (source.contains(key))
      source[key] = std::filesystem::canonical(input.parent_path() /
        source.at(key).get<std::string>()).string();
  }
  const auto configuration = nativeCanonicalJson(catalog);
  const auto& recipe = catalog.at("recipe");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::hours(2);
  const NativeAssemblyControl control{deadline, [deadline] {
    if (std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error("Wan role catalog preparation deadline expired");
  }, recipe.at("max_source_bytes"), recipe.at("max_assembled_bytes")};
  RepositorySourceRequest request;
  request.catalogConfigurationJson = configuration;
  request.maxSourceBytes = control.maxSourceBytes;
  request.deadline = deadline;
  auto loaded = NativeRequestCatalog::load(configuration,
    NativeFilesystemSourceProvider(std::filesystem::canonical(cache)).load(request, {}), control);
  if (!loaded.planningCache || loaded.planningCache->candidates.size() != 1)
    throw std::runtime_error("Wan role catalog requires one native planning candidate");
  facts["candidate_digest"] = loaded.planningCache->candidates.front().candidate.candidateDigest;
  catalog["preparation_facts"] = std::move(facts);
  if (!std::filesystem::create_directory(outputDirectory))
    throw std::runtime_error("Wan role catalog output could not be reserved");
  const auto output = outputDirectory / "catalog.json";
  std::ofstream file(output, std::ios::binary);
  file << nativeCanonicalJson(catalog);
  file.close();
  if (!file) throw std::runtime_error("Wan role catalog could not be written");
  std::cout << output.string() << '\n';
}

} // namespace

int
main(int argc, char** argv)
try {
  constexpr std::string_view USAGE =
    "usage: di-prepare-wan-checkpoint REQUEST.json OUTPUT_DIR\n"
    "       di-prepare-wan-checkpoint --absolute-roles CATALOG.json CACHE_DIR OUTPUT_DIR";
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << USAGE << '\n';
    return 0;
  }
  if (argc == 5 && std::string_view(argv[1]) == "--absolute-roles") {
    writeAbsoluteRoleCatalog(std::filesystem::absolute(argv[2]), argv[3], argv[4]);
    return 0;
  }
  if (argc != 3)
    throw std::invalid_argument(std::string(USAGE));
  const auto config = std::filesystem::absolute(argv[1]);
  auto request = ndnsf::di::nativeParseJson(readConfiguration(config, 8U << 20));
  const auto& recipe = request.at("catalog").at("recipe");
  const auto timeout = request.at("timeout_ms").get<std::int64_t>();
  if (timeout <= 0 || timeout > 86'400'000)
    throw std::invalid_argument("Wan preparation timeout_ms must be within one day");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
  const ndnsf::di::NativeAssemblyControl control{deadline, [deadline] {
    if (std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error("Wan checkpoint preparation deadline expired");
  }, recipe.at("max_source_bytes"), recipe.at("max_assembled_bytes")};
  const auto catalog = ndnsf::di::wan::prepareWanCheckpointComponent(
    std::move(request), config.parent_path(), argv[2], control);
  std::cout << catalog.string() << '\n';
  return 0;
}
catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
