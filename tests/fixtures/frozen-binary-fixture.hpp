#ifndef NDNSF_TESTS_FROZEN_BINARY_FIXTURE_HPP
#define NDNSF_TESTS_FROZEN_BINARY_FIXTURE_HPP

#include "tests/fixtures/test-resource-paths.hpp"

#include <ndn-cxx/util/string-helper.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace ndnsf::test {

/** Materialize a small tracked hex fixture into an exclusively owned temporary file. */
class FrozenBinaryFixture
{
public:
  explicit FrozenBinaryFixture(const std::string& resource)
  {
    const auto source = testFixturePath(resource);
    if (std::filesystem::file_size(source) > 64 * 1024)
      throw std::runtime_error("frozen binary fixture exceeds its size bound");
    std::ifstream input(source);
    if (!input) throw std::runtime_error("cannot read frozen binary fixture");
    std::string hex(64 * 1024 + 1, '\0');
    input.read(hex.data(), hex.size());
    const auto count = static_cast<std::size_t>(input.gcount());
    if (input.bad() || count > 64 * 1024)
      throw std::runtime_error("cannot read bounded frozen binary fixture");
    hex.resize(count);
    hex.erase(std::remove_if(hex.begin(), hex.end(), [] (unsigned char c) {
      return std::isspace(c);
    }), hex.end());
    const auto bytes = ndn::fromHex(hex);
    if (bytes->empty()) throw std::runtime_error("frozen binary fixture is empty");
    auto pattern = (std::filesystem::temp_directory_path() / "ndnsf-fixture-XXXXXX").string();
    const auto* directory = ::mkdtemp(pattern.data());
    if (!directory) throw std::runtime_error("cannot create frozen fixture directory");
    m_root = directory;
    try {
      std::ofstream output(path(), std::ios::binary);
      output.write(reinterpret_cast<const char*>(bytes->data()), bytes->size());
      output.close();
      if (!output) throw std::runtime_error("cannot write frozen binary fixture");
    }
    catch (...) {
      std::error_code error;
      std::filesystem::remove_all(m_root, error);
      throw;
    }
  }

  ~FrozenBinaryFixture()
  {
    std::error_code error;
    std::filesystem::remove_all(m_root, error);
  }

  FrozenBinaryFixture(const FrozenBinaryFixture&) = delete;
  FrozenBinaryFixture& operator=(const FrozenBinaryFixture&) = delete;

  std::string path() const { return (m_root / "payload.bin").string(); }

private:
  std::filesystem::path m_root;
};

} // namespace ndnsf::test

#endif // NDNSF_TESTS_FROZEN_BINARY_FIXTURE_HPP
