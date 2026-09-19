#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoCore.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/encoding/tlv.hpp>

#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using namespace ndnsf_distributed_repo;

struct Fixture
{
  std::filesystem::path root = std::filesystem::temp_directory_path() /
    ("spec189-quota-" + std::to_string(::getpid()));

  Fixture()
  {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    if (error)
      throw std::runtime_error("unable to create quota fixture: " + error.message());
  }

  ~Fixture()
  {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::remove(root.string() + ".authority.lock", error);
  }
};

std::vector<uint8_t>
bytes(size_t size, uint8_t value)
{
  return std::vector<uint8_t>(size, value);
}

RepoObjectManifest
rangeManifest(const std::string& name, const std::vector<uint8_t>& payload)
{
  RepoObjectManifest manifest;
  manifest.objectName = name;
  manifest.objectType = "canonical-layer";
  manifest.sha256 = sha256Hex(payload);
  manifest.size = payload.size();
  manifest.segmentCount = 1;
  manifest.generation = 1;
  manifest.operationId = "spec189-quota-range";
  return manifest;
}

std::vector<uint8_t>
makeDataWire(const std::string& name, size_t contentSize)
{
  ndn::Data data{ndn::Name(name)};
  const auto content = bytes(contentSize, 0x5a);
  data.setContent(ndn::span<const uint8_t>(content.data(), content.size()));
  data.setSignatureInfo(ndn::SignatureInfo(ndn::tlv::DigestSha256));
  const std::vector<uint8_t> signature(32, 0x33);
  data.setSignatureValue(ndn::span<const uint8_t>(signature.data(), signature.size()));
  const auto wire = data.wireEncode();
  return {wire.begin(), wire.end()};
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec189RepoQuota)

BOOST_AUTO_TEST_CASE(RangeReservationBlocksVectorAdmissionUntilAbort)
{
  Fixture fixture;
  StorageCapability capability;
  capability.repoNode = "/spec189/quota/vector";
  capability.freeBytes = 1024;
  auto store = makeFilesystemRepoStore((fixture.root / "vector").string(),
                                       1U << 20, 1U << 20);
  RepoCore repo(capability, std::move(store));

  const auto staged = bytes(800, 0x11);
  const auto manifest = rangeManifest("/spec189/quota/range", staged);
  repo.putRange(manifest, RepoByteRange{0, 32}, bytes(32, 0x11));

  bool rejected = false;
  try {
    (void) repo.put("/spec189/quota/vector-object", bytes(300, 0x22));
  }
  catch (const std::runtime_error& error) {
    rejected = std::string(error.what()).find("insufficient free space") !=
               std::string::npos;
  }
  BOOST_REQUIRE_MESSAGE(rejected,
                        "vector admission ignored the outstanding range reservation");

  repo.abortRanges(manifest.objectName);
  BOOST_CHECK_EQUAL(repo.put("/spec189/quota/vector-object", bytes(300, 0x22)).size, 300U);
}

BOOST_AUTO_TEST_CASE(RangeReservationBlocksExactDataPacketAdmissionUntilAbort)
{
  Fixture fixture;
  StorageCapability capability;
  capability.repoNode = "/spec189/quota/data";
  capability.freeBytes = 1024;
  auto store = makeFilesystemRepoStore((fixture.root / "data").string(),
                                       1U << 20, 1U << 20);
  RepoCore repo(capability, std::move(store));

  const auto staged = bytes(800, 0x44);
  const auto manifest = rangeManifest("/spec189/quota/range-data", staged);
  repo.putRange(manifest, RepoByteRange{0, 32}, bytes(32, 0x44));
  const auto dataName = std::string("/spec189/quota/data-packet");
  const auto wire = makeDataWire(dataName, 512);
  BOOST_REQUIRE_GT(wire.size(), 224U);

  bool rejected = false;
  try {
    (void) repo.putDataPacket(dataName, wire);
  }
  catch (const std::runtime_error& error) {
    rejected = std::string(error.what()).find("insufficient free space") !=
               std::string::npos;
  }
  BOOST_REQUIRE_MESSAGE(rejected,
                        "exact Data packet admission ignored the range reservation");

  repo.abortRanges(manifest.objectName);
  BOOST_CHECK_EQUAL(repo.putDataPacket(dataName, wire).size, wire.size());
}

BOOST_AUTO_TEST_SUITE_END()
