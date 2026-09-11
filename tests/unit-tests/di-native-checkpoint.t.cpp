#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCheckpointExport.hpp"

#include <boost/test/unit_test.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

class TempDirectory
{
public:
  TempDirectory()
  {
    char pattern[] = "/tmp/spec184-checkpoint-XXXXXX";
    const auto value = ::mkdtemp(pattern);
    if (!value) throw std::runtime_error("checkpoint test temporary directory unavailable");
    m_path = value;
  }
  ~TempDirectory() { std::error_code error; std::filesystem::remove_all(m_path, error); }
  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;
  const std::string& path() const noexcept { return m_path; }

private:
  std::string m_path;
};

std::string readFile(const std::string& path)
{
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void writeFile(const std::string& path, const std::string& value)
{
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output || !(output << value)) throw std::runtime_error("checkpoint test seed write failed");
}

std::size_t temporaryFileCount(const std::string& directory, const std::string& stem)
{
  std::size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().filename().string().rfind(stem + ".tmp-", 0) == 0)
      ++count;
  }
  return count;
}

ndnsf::di::NativeJson sampleState(const std::string& wire = "sha256:checkpoint")
{
  return ndnsf::di::NativeJson{
    {"schema", "ndnsf-di-native-conversation-state-v1"},
    {"checkpoint_wire", wire},
    {"transcript", {
      {"canonicalTokenIds", {1, 2, 3}},
      {"generationId", "generation-1"},
    }},
  };
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec184NativeCheckpoint)

BOOST_AUTO_TEST_CASE(Spec184CheckpointExport)
{
  TempDirectory directory;
  const auto destination = directory.path() + "/conversation.state";
  const auto state = sampleState();

  ndnsf::di::nativeExportPrivateCheckpoint(destination, state);

  struct stat value{};
  BOOST_REQUIRE_EQUAL(::lstat(destination.c_str(), &value), 0);
  BOOST_CHECK(S_ISREG(value.st_mode));
  BOOST_CHECK_EQUAL(value.st_mode & 0777, 0600);
  BOOST_CHECK_EQUAL(readFile(destination), ndnsf::di::nativeCanonicalJson(state));
  BOOST_CHECK_EQUAL(temporaryFileCount(directory.path(), "conversation.state"), 0U);
}

BOOST_AUTO_TEST_CASE(Spec184CheckpointExportRejectsSymlink)
{
  TempDirectory directory;
  const auto target = directory.path() + "/target.state";
  const auto destination = directory.path() + "/conversation.state";
  writeFile(target, "old-checkpoint");
  BOOST_REQUIRE_EQUAL(::symlink(target.c_str(), destination.c_str()), 0);

  BOOST_CHECK_THROW(
    ndnsf::di::nativeExportPrivateCheckpoint(destination, sampleState()), std::exception);
  BOOST_CHECK_EQUAL(readFile(target), "old-checkpoint");
  struct stat value{};
  BOOST_REQUIRE_EQUAL(::lstat(destination.c_str(), &value), 0);
  BOOST_CHECK(S_ISLNK(value.st_mode));
  BOOST_CHECK_EQUAL(temporaryFileCount(directory.path(), "conversation.state"), 0U);
}

BOOST_AUTO_TEST_CASE(Spec184CheckpointExportPreservesOldOnFailure)
{
  TempDirectory directory;
  const auto destination = directory.path() + "/conversation.state";
  writeFile(destination, "old-checkpoint");
  ndnsf::di::NativeCheckpointExportOptions options;
  options.beforeStage = [](ndnsf::di::NativeCheckpointExportStage stage) {
    if (stage == ndnsf::di::NativeCheckpointExportStage::BeforeRename)
      throw std::runtime_error("injected rename failure");
  };

  BOOST_CHECK_THROW(
    ndnsf::di::nativeExportPrivateCheckpoint(destination, sampleState("new-checkpoint"), options),
    std::exception);
  BOOST_CHECK_EQUAL(readFile(destination), "old-checkpoint");
  BOOST_CHECK_EQUAL(temporaryFileCount(directory.path(), "conversation.state"), 0U);
}

BOOST_AUTO_TEST_SUITE_END()
