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

std::vector<uint8_t>
dataWire(const std::string& name, uint8_t content)
{
  ndn::Data data{ndn::Name(name)};
  data.setContent(ndn::span<const uint8_t>(&content, 1));
  data.setSignatureInfo(ndn::SignatureInfo(ndn::tlv::DigestSha256));
  const std::vector<uint8_t> signature(32, content);
  data.setSignatureValue(ndn::span<const uint8_t>(signature.data(), signature.size()));
  const auto wire = data.wireEncode();
  return {wire.begin(), wire.end()};
}

StorageCapability
capability()
{
  StorageCapability value;
  value.repoNode = "/repo/one";
  value.freeBytes = 1U << 20;
  return value;
}

void
checkReceiptLifecycle(const std::string& path, bool sqlite)
{
  const std::string name = "/producer/original";
  const auto original = dataWire(name, 0x41);
  const auto replacement = dataWire(name, 0x42);
  const auto digest = sha256Hex(original);
  const std::string legacyName = "/producer/legacy";
  const auto legacyWire = dataWire(legacyName, 0x43);
  const auto legacyDigest = sha256Hex(legacyWire);
  std::string receiptId;
  auto openStore = [&] () {
    return sqlite ? makeSqliteRepoStore(path) :
      makeFilesystemRepoStore(path, 1U << 20, 1U << 20);
  };
  {
    RepoCore repo(capability(), openStore());
    const auto manifest = repo.putDataPacket(name, original, "/repo/one", "/repo/provider/one");
    BOOST_REQUIRE(!manifest.dataReceiptId.empty());
    receiptId = manifest.dataReceiptId;
    BOOST_CHECK_EQUAL(manifest.dataReceiptRepoNode, "/repo/one");
    BOOST_CHECK_EQUAL(manifest.dataReceiptProviderIdentity, "/repo/provider/one");
    BOOST_CHECK_EQUAL(repo.putDataPacket(name, original, "/repo/one", "/repo/provider/one")
                        .dataReceiptId, receiptId);
    BOOST_CHECK_THROW(repo.putManifest(manifest), std::invalid_argument);
    BOOST_CHECK_THROW(repo.putDataPacket(name, replacement, "/repo/one",
                                        "/repo/provider/one"), std::runtime_error);
    const auto receipt = repo.getDataPacketReceipt(name, digest);
    BOOST_CHECK_EQUAL(receipt.operationId, receiptId);
    BOOST_CHECK_EQUAL(receipt.digest, digest);
    BOOST_CHECK(repo.remove(name));
    BOOST_CHECK(!repo.hasDataPacket(name));
    BOOST_CHECK_EQUAL(repo.getDataPacketReceipt(name, digest).operationId, receiptId);
  }
  {
    RepoCore repo(capability(), openStore());
    BOOST_CHECK(!repo.hasDataPacket(name));
    BOOST_CHECK_EQUAL(repo.getDataPacketReceipt(name, digest).operationId, receiptId);
    BOOST_CHECK_THROW(repo.getDataPacketReceipt(name, sha256Hex(replacement)),
                      std::out_of_range);
    const auto next = repo.putDataPacket(name, replacement, "/repo/one", "/repo/provider/one");
    BOOST_CHECK_NE(next.dataReceiptId, receiptId);
    BOOST_CHECK_EQUAL(repo.getDataPacketReceipt(name, digest).operationId, receiptId);
    BOOST_CHECK_EQUAL(repo.getDataPacketReceipt(name, sha256Hex(replacement)).operationId,
                      next.dataReceiptId);
    BOOST_CHECK(repo.putDataPacket(legacyName, legacyWire).dataReceiptId.empty());
    BOOST_CHECK_THROW(repo.getDataPacketReceipt(legacyName, legacyDigest),
                      std::out_of_range);
  }
  std::string importId;
  {
    RepoCore repo(capability(), openStore());
    BOOST_CHECK(repo.getDataPacket(legacyName) == legacyWire);
    BOOST_CHECK_THROW(repo.importLegacyDataPacketReceipt(
      legacyName, "/repo/other", "/repo/provider/one"), std::invalid_argument);
    BOOST_CHECK_THROW(repo.getDataPacketReceipt(legacyName, legacyDigest),
                      std::out_of_range);
    const auto imported = repo.importLegacyDataPacketReceipt(
      legacyName, "/repo/one", "/repo/provider/one");
    importId = imported.operationId;
    BOOST_CHECK(importId.rfind("repo-import-data-", 0) == 0);
    BOOST_CHECK_EQUAL(imported.digest, legacyDigest);
    BOOST_CHECK_EQUAL(repo.importLegacyDataPacketReceipt(
      legacyName, "/repo/one", "/repo/provider/one").operationId, importId);
    BOOST_CHECK_THROW(repo.importLegacyDataPacketReceipt(
      legacyName, "/repo/one", "/repo/provider/other"), std::runtime_error);
    BOOST_CHECK(repo.getDataPacket(legacyName) == legacyWire);
  }
  {
    RepoCore repo(capability(), openStore());
    BOOST_CHECK_EQUAL(repo.getDataPacketReceipt(legacyName, legacyDigest).operationId,
                      importId);
    BOOST_CHECK_EQUAL(repo.importLegacyDataPacketReceipt(
      legacyName, "/repo/one", "/repo/provider/one").operationId, importId);
    BOOST_CHECK(repo.getDataPacket(legacyName) == legacyWire);
    BOOST_CHECK(repo.remove(legacyName));
    BOOST_CHECK_EQUAL(repo.getDataPacketReceipt(legacyName, legacyDigest).operationId,
                      importId);
  }
}

void
checkInvalidLegacyImport(const std::string& path, bool sqlite)
{
  const std::string name = "/producer/malformed";
  const std::vector<uint8_t> malformed{0x01, 0x02, 0x03};
  auto openStore = [&] () {
    return sqlite ? makeSqliteRepoStore(path) :
      makeFilesystemRepoStore(path, 1U << 20, 1U << 20);
  };
  {
    auto store = openStore();
    RepoObjectManifest manifest;
    manifest.objectName = name;
    manifest.objectType = "ndn-data-wire";
    manifest.sha256 = sha256Hex(malformed);
    manifest.size = malformed.size();
    manifest.segmentCount = 1;
    manifest.packetNames = {name};
    manifest.generation = 1;
    store->put(manifest, malformed);
  }
  {
    RepoCore repo(capability(), openStore());
    BOOST_CHECK_THROW(repo.importLegacyDataPacketReceipt(
      name, "/repo/one", "/repo/provider/one"), std::runtime_error);
    BOOST_CHECK_THROW(repo.getDataPacketReceipt(name, sha256Hex(malformed)),
                      std::out_of_range);
    BOOST_CHECK(repo.getDataPacket(name) == malformed);
  }
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec193OriginalDataReceipt)

BOOST_AUTO_TEST_CASE(FilesystemRetainsReceiptAcrossDeleteAndRestart)
{
  const auto root = std::filesystem::temp_directory_path() /
    ("spec193-original-receipt-file-" + std::to_string(::getpid()));
  std::error_code error;
  std::filesystem::remove_all(root, error);
  checkReceiptLifecycle(root.string(), false);
  std::filesystem::remove_all(root, error);
  std::filesystem::remove(root.string() + ".authority.lock", error);
}

BOOST_AUTO_TEST_CASE(SqliteRetainsReceiptAcrossDeleteAndRestart)
{
  const auto path = (std::filesystem::temp_directory_path() /
    ("spec193-original-receipt-sqlite-" + std::to_string(::getpid()) + ".db")).string();
  std::error_code error;
  std::filesystem::remove(path, error);
  checkReceiptLifecycle(path, true);
  std::filesystem::remove(path, error);
  std::filesystem::remove(path + "-wal", error);
  std::filesystem::remove(path + "-shm", error);
  std::filesystem::remove(path + ".authority.lock", error);
}

BOOST_AUTO_TEST_CASE(FilesystemRejectsMalformedLegacyImport)
{
  const auto root = std::filesystem::temp_directory_path() /
    ("spec193-original-import-bad-file-" + std::to_string(::getpid()));
  std::error_code error;
  std::filesystem::remove_all(root, error);
  checkInvalidLegacyImport(root.string(), false);
  std::filesystem::remove_all(root, error);
  std::filesystem::remove(root.string() + ".authority.lock", error);
}

BOOST_AUTO_TEST_CASE(SqliteRejectsMalformedLegacyImport)
{
  const auto path = (std::filesystem::temp_directory_path() /
    ("spec193-original-import-bad-sqlite-" + std::to_string(::getpid()) + ".db")).string();
  std::error_code error;
  std::filesystem::remove(path, error);
  checkInvalidLegacyImport(path, true);
  std::filesystem::remove(path, error);
  std::filesystem::remove(path + "-wal", error);
  std::filesystem::remove(path + "-shm", error);
  std::filesystem::remove(path + ".authority.lock", error);
}

BOOST_AUTO_TEST_CASE(LocationParserRejectsUnboundOrUnhintedClaims)
{
  const RepoDataPacketQuery query{1, "/producer/original", std::string(64, 'a')};
  BOOST_CHECK_EQUAL(RepoDataPacketQuery::fromJson(query.toJson()).dataName,
                    query.dataName);
  BOOST_CHECK_THROW(RepoDataPacketQuery::fromJson(
    "{\"schemaVersion\":2,\"dataName\":\"/producer/original\","
    "\"expectedWireSha256\":\"" + std::string(64, 'a') + "\"}"),
    std::invalid_argument);

  RepoDataPacketLocation location;
  location.receiptId = "receipt-one";
  location.repoNode = "/repo/one";
  location.providerIdentity = "/repo/provider/one";
  location.dataName = query.dataName;
  location.wireSha256 = query.expectedWireSha256;
  location.state = "PRESENT";
  location.forwardingHint = location.providerIdentity;
  location.observedAtMs = 100;
  location.expiresAtMs = 200;
  BOOST_CHECK_EQUAL(RepoDataPacketLocation::fromJson(location.toJson()).forwardingHint,
                    location.providerIdentity);
  location.forwardingHint.clear();
  BOOST_CHECK_THROW(RepoDataPacketLocation::fromJson(location.toJson()),
                    std::invalid_argument);
  location.forwardingHint = "/repo/provider/other";
  BOOST_CHECK_THROW(RepoDataPacketLocation::fromJson(location.toJson()),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(FilesystemReceiptRemainsQueryableWhenPayloadIsCorrupt)
{
  const auto root = std::filesystem::temp_directory_path() /
    ("spec193-original-receipt-corrupt-" + std::to_string(::getpid()));
  std::error_code error;
  std::filesystem::remove_all(root, error);
  {
    RepoCore repo(capability(), makeFilesystemRepoStore(root.string(), 1U << 20, 1U << 20));
    const std::string name = "/producer/corrupt";
    const auto wire = dataWire(name, 0x44);
    const auto digest = sha256Hex(wire);
    const auto manifest = repo.putDataPacket(name, wire, "/repo/one", "/repo/provider/one");
    const auto payload = root / "payloads" / "sha256" / digest.substr(0, 2) / digest;
    BOOST_REQUIRE(std::filesystem::is_regular_file(payload));
    BOOST_REQUIRE(std::filesystem::remove(payload));
    BOOST_CHECK_EQUAL(repo.getDataPacketReceipt(name, digest).operationId,
                      manifest.dataReceiptId);
    BOOST_CHECK_THROW(repo.getDataPacket(name), std::exception);
  }
  std::filesystem::remove_all(root, error);
  std::filesystem::remove(root.string() + ".authority.lock", error);
}

BOOST_AUTO_TEST_SUITE_END()
