#include "../../NDNSF-UAV-APP/shared/UavTrackingWorkerTransaction.hpp"

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

ndn::Buffer
bufferOf(const std::string& value)
{
  return ndn::Buffer(reinterpret_cast<const uint8_t*>(value.data()), value.size());
}

std::string
textOf(const ndn::Buffer& value)
{
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

struct TemporaryDirectory
{
  std::filesystem::path path = std::filesystem::temp_directory_path() /
    ("ndnsf-uav-worker-txn-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));

  TemporaryDirectory()
  {
    std::filesystem::create_directories(path);
  }

  ~TemporaryDirectory()
  {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

constexpr const char* INPUT_DIGEST =
  "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

} // namespace

BOOST_AUTO_TEST_SUITE(UavTrackingWorkerTransactionTests)

BOOST_AUTO_TEST_CASE(CommitBindsImmutableResultAndStateWithManifest)
{
  TemporaryDirectory temporary;
  ndnsf::examples::uav::UavTrackingWorkerTransaction transaction(temporary.path);
  const auto result = bufferOf("verified result bytes");
  const auto state = bufferOf("candidate state bytes");

  BOOST_REQUIRE(transaction.commit("window-0", INPUT_DIGEST, result, state, [] { return true; }));
  const auto restored = transaction.loadCommittedResult("window-0", INPUT_DIGEST);
  BOOST_REQUIRE(restored);
  BOOST_CHECK_EQUAL(textOf(*restored), textOf(result));

  const auto statePath = transaction.committedStatePath("window-0");
  BOOST_REQUIRE(statePath);
  std::ifstream stateInput(*statePath, std::ios::binary);
  const std::string restoredState((std::istreambuf_iterator<char>(stateInput)), {});
  BOOST_CHECK_EQUAL(restoredState, textOf(state));
  BOOST_CHECK_THROW(transaction.loadCommittedResult(
    "window-0", "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"),
    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(RevokedWorkDoesNotCommitCandidatePair)
{
  TemporaryDirectory temporary;
  ndnsf::examples::uav::UavTrackingWorkerTransaction transaction(temporary.path);
  const bool committed = transaction.commit("window-0", INPUT_DIGEST,
    bufferOf("result"), bufferOf("state"), [] { return false; });

  BOOST_CHECK(!committed);
  BOOST_CHECK(!transaction.loadCommittedResult("window-0", INPUT_DIGEST));
  BOOST_CHECK(!transaction.committedStatePath("window-0"));
}

BOOST_AUTO_TEST_CASE(InterruptedCandidateCanBeReplacedAndTamperingIsDetected)
{
  TemporaryDirectory temporary;
  ndnsf::examples::uav::UavTrackingWorkerTransaction transaction(temporary.path);
  const auto partial = temporary.path / "window-0";
  std::filesystem::create_directories(partial);
  std::ofstream(partial / "result.json") << "uncommitted";

  BOOST_REQUIRE(transaction.commit("window-0", INPUT_DIGEST,
    bufferOf("verified result"), bufferOf("verified state"), [] { return true; }));
  const auto state = transaction.committedStatePath("window-0");
  BOOST_REQUIRE(state);
  std::ofstream(*state, std::ios::binary | std::ios::trunc) << "tampered";
  BOOST_CHECK_THROW(transaction.loadCommittedResult("window-0", INPUT_DIGEST),
                    std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()
