#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-preparation.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

int
main(int argc, char** argv)
try {
  constexpr std::string_view USAGE = "usage: di-prepare-wan-checkpoint REQUEST.json OUTPUT_DIR";
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << USAGE << '\n';
    return 0;
  }
  if (argc != 3)
    throw std::invalid_argument(std::string(USAGE));
  const auto config = std::filesystem::absolute(argv[1]);
  const auto size = std::filesystem::file_size(config);
  if (size == 0 || size > (8U << 20))
    throw std::invalid_argument("Wan preparation configuration size exceeds its bound");
  std::ifstream input(config, std::ios::binary);
  std::string bytes(size, '\0');
  if (!input.read(bytes.data(), bytes.size()) || input.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("Wan preparation configuration could not be read");
  auto request = ndnsf::di::nativeParseJson(bytes);
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
