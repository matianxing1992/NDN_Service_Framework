#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactStaging.hpp"

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace ndnsf::di;

namespace {

void
makeOld(const std::filesystem::path& path)
{
  const auto old = std::filesystem::file_time_type::clock::now() -
                   std::chrono::hours(2);
  std::filesystem::last_write_time(path, old);
}

} // namespace

BOOST_AUTO_TEST_CASE(Spec189ProviderStagingCleanupRemovesOnlyOrphans)
{
  const auto root = std::filesystem::temp_directory_path() /
    ("spec189-provider-staging-" + std::to_string(::getpid()));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root / ".staging");
  std::filesystem::create_directories(root / "protected" / "role");

  const auto stale = root / ".staging" / "assembly-stale";
  const auto live = root / ".staging" / "assembly-live";
  const auto protectedStale = root / "protected" / "role" / "assembly-stale";
  const auto plaintextStale = root / ".staging" / "protected-plaintext-stale";
  const auto valid = root / "assembled" / "role" / "digest";
  std::filesystem::create_directories(stale);
  std::filesystem::create_directories(live);
  std::filesystem::create_directories(protectedStale);
  std::filesystem::create_directories(plaintextStale);
  std::filesystem::create_directories(valid);
  std::ofstream(root / ".staging" / "assembly-live.ndnsf-di-provider-lease")
    << "pid " << ::getpid() << "\n";
  std::ofstream(stale / "payload") << "stale";
  std::ofstream(protectedStale / "payload") << "stale";
  std::ofstream(plaintextStale / "model.onnx") << "stale";
  makeOld(stale);
  makeOld(protectedStale);
  makeOld(plaintextStale);

  const auto removed = cleanupNativeArtifactStaging(root, std::chrono::minutes(10));
  BOOST_CHECK_EQUAL(removed, 3U);
  BOOST_CHECK(!std::filesystem::exists(stale));
  BOOST_CHECK(!std::filesystem::exists(protectedStale));
  BOOST_CHECK(!std::filesystem::exists(plaintextStale));
  BOOST_CHECK(std::filesystem::exists(live));
  BOOST_CHECK(std::filesystem::exists(valid));

  std::filesystem::remove_all(root, ignored);
}
