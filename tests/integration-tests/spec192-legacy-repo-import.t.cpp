#include "NDNSF-DistributedRepo/include/ndnsf-distributed-repo/LegacyRepoImporter.hpp"
#include "NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp"
#include "tests/boost-test.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-info.hpp>
#include <ndn-cxx/util/span.hpp>
#include <openssl/evp.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace ndnsf_distributed_repo::test {
namespace {

namespace fs = std::filesystem;

class TemporaryDirectory
{
public:
  TemporaryDirectory()
  {
    static std::atomic<unsigned> sequence{0};
    m_path = fs::temp_directory_path() /
      ("ndnsf-spec192-import-integration-" + std::to_string(::getpid()) + "-" +
       std::to_string(sequence.fetch_add(1)));
    fs::create_directories(m_path);
  }
  ~TemporaryDirectory()
  {
    std::error_code error;
    fs::remove_all(m_path, error);
  }
  const fs::path& path() const noexcept { return m_path; }

private:
  fs::path m_path;
};

class Database
{
public:
  explicit Database(const fs::path& path)
  {
    const int status = sqlite3_open_v2(path.c_str(), &m_db,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (status != SQLITE_OK) {
      const std::string message = m_db == nullptr ? "sqlite-open-failed" :
        sqlite3_errmsg(m_db);
      if (m_db != nullptr) sqlite3_close(m_db);
      m_db = nullptr;
      throw std::runtime_error(message);
    }
  }
  ~Database() { if (m_db != nullptr) sqlite3_close(m_db); }
  sqlite3* get() const noexcept { return m_db; }

private:
  sqlite3* m_db = nullptr;
};

fs::path
repositoryRoot()
{
  auto current = fs::current_path();
  for (;;) {
    if (fs::exists(current / "tests/fixtures/spec192")) return current;
    const auto parent = current.parent_path();
    if (parent == current) throw std::runtime_error("spec192-fixture-root-not-found");
    current = parent;
  }
}

std::string
readFile(const fs::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("fixture-read-failed: " + path.string());
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

void
execute(sqlite3* db, const std::string& sql)
{
  char* error = nullptr;
  const int status = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error);
  if (status != SQLITE_OK) {
    const std::string message = error == nullptr ? sqlite3_errmsg(db) : error;
    sqlite3_free(error);
    throw std::runtime_error("fixture-sql-failed: " + message);
  }
}

std::string
sha256(const void* bytes, size_t size)
{
  std::array<unsigned char, 32> digest{};
  unsigned int length = 0;
  if (EVP_Digest(bytes, size, digest.data(), &length, EVP_sha256(), nullptr) != 1 ||
      length != digest.size()) {
    throw std::runtime_error("fixture-digest-failed");
  }
  static constexpr char HEX[] = "0123456789abcdef";
  std::string result;
  result.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    result.push_back(HEX[byte >> 4]);
    result.push_back(HEX[byte & 0x0f]);
  }
  return result;
}

std::string
sha256File(const fs::path& path)
{
  const auto bytes = readFile(path);
  return sha256(bytes.data(), bytes.size());
}

std::string
jsonQuote(const std::string& value)
{
  std::string result = "\"";
  for (const auto c : value) {
    if (c == '\\' || c == '"') result.push_back('\\');
    result.push_back(c);
  }
  result.push_back('"');
  return result;
}

using FileHashes = std::map<std::string, std::string>;

FileHashes
sourceBundleHashes(const fs::path& database)
{
  FileHashes result;
  for (const auto* suffix : {"", "-wal", "-shm"}) {
    const auto path = fs::path(database.string() + suffix);
    result.emplace(suffix, fs::exists(path) ? sha256File(path) : "MISSING");
  }
  return result;
}

std::string
sourceBundleDigest(const fs::path& database)
{
  std::string json = "[";
  bool first = true;
  for (const auto* suffix : {"", "-shm", "-wal"}) {
    const auto path = fs::path(database.string() + suffix);
    if (!fs::exists(path)) continue;
    if (!first) json += ',';
    first = false;
    const auto name = std::string(suffix).empty() ? "database" : suffix;
    json += "{\"name\":" + jsonQuote(name) + ",\"sha256\":" +
      jsonQuote(sha256File(path)) + ",\"size\":" +
      std::to_string(fs::file_size(path)) + "}";
  }
  json += ']';
  return sha256(json.data(), json.size());
}

std::string
repoDestinationDigest(const std::vector<RepoObjectManifest>& manifests)
{
  std::vector<std::string> values;
  for (const auto& manifest : manifests) {
    values.push_back("{\"kind\":\"repo-object\",\"manifest\":" +
      jsonQuote(manifest.toJson()) + ",\"name\":" +
      jsonQuote(manifest.objectName) + ",\"sha256\":" +
      jsonQuote(manifest.sha256) + ",\"size\":" +
      std::to_string(manifest.size) + "}");
  }
  std::sort(values.begin(), values.end());
  std::string json = "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) json += ',';
    json += values[i];
  }
  json += ']';
  return sha256(json.data(), json.size());
}

void
bindText(sqlite3_stmt* statement, int index, const std::string& value)
{
  if (sqlite3_bind_text(statement, index, value.data(),
                        static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
    throw std::runtime_error("fixture-bind-text-failed");
  }
}

void
bindBlob(sqlite3_stmt* statement, int index, const std::vector<uint8_t>& value)
{
  if (sqlite3_bind_blob(statement, index, value.data(),
                        static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
    throw std::runtime_error("fixture-bind-blob-failed");
  }
}

void
stepDone(sqlite3_stmt* statement)
{
  if (sqlite3_step(statement) != SQLITE_DONE) {
    throw std::runtime_error("fixture-insert-failed");
  }
}

struct SourceObject
{
  std::string objectName = "/legacy/large-model";
  std::string payload;
  std::string dataName = "/spec192/import/large-model/seg=0";
  std::vector<uint8_t> dataWire;
  RepoObjectManifest manifest;
};

SourceObject
seedRepoFixture(sqlite3* db, const std::string& objectType = "artifact")
{
  execute(db, readFile(repositoryRoot() /
    "tests/fixtures/spec192/legacy-python-repo-v12.sql"));
  execute(db, "PRAGMA journal_mode=WAL");

  SourceObject source;
  source.payload.resize(16391);
  for (size_t i = 0; i < source.payload.size(); ++i) {
    source.payload[i] = static_cast<char>((i * 37 + 19) & 0xff);
  }
  source.manifest.objectName = source.objectName;
  source.manifest.objectType = objectType;
  source.manifest.sha256 = sha256(source.payload.data(), source.payload.size());
  source.manifest.size = source.payload.size();
  source.manifest.segmentCount = 1;
  source.manifest.replicationFactor = 3;
  source.manifest.replicaNodes = {"legacy-replica-a", "legacy-replica-b"};
  source.manifest.confirmedReplicaNodes = {"legacy-replica-a"};
  source.manifest.packetNames = {source.dataName};
  source.manifest.policyEpoch = "unverified-old-policy";
  source.manifest.publicationIdentity = "old-operation-claim";
  source.manifest.protectionEpoch = "old-protection-claim";
  source.manifest.generation = 9;
  source.manifest.parentGeneration = 4;
  source.manifest.writeConsistency = "QUORUM";
  source.manifest.requiredWriteAcks = 2;
  source.manifest.operationId = "old-operation-claim";
  source.manifest.lifecycleState = "ACTIVE";

  ndn::security::KeyChain keyChain("pib-memory:", "tpm-memory:");
  ndn::Data packet{ndn::Name(source.dataName)};
  packet.setContent(ndn::span<const uint8_t>(
    reinterpret_cast<const uint8_t*>(source.payload.data()), source.payload.size()));
  keyChain.sign(packet, ndn::security::SigningInfo(
    ndn::security::SigningInfo::SIGNER_TYPE_SHA256));
  const auto& wireBlock = packet.wireEncode();
  source.dataWire.assign(wireBlock.begin(), wireBlock.end());
  const auto wireDigest = sha256(source.dataWire.data(), source.dataWire.size());

  sqlite3_stmt* data = nullptr;
  const char* dataSql =
    "INSERT INTO data_packets(data_name,wire,wire_sha256,wire_size,updated_at,hit_count)"
    " VALUES(?,?,?,?,1,0)";
  if (sqlite3_prepare_v2(db, dataSql, -1, &data, nullptr) != SQLITE_OK) {
    throw std::runtime_error("fixture-data-packet-prepare-failed");
  }
  bindText(data, 1, source.dataName);
  bindBlob(data, 2, source.dataWire);
  bindText(data, 3, wireDigest);
  sqlite3_bind_int64(data, 4, static_cast<sqlite3_int64>(source.dataWire.size()));
  stepDone(data);
  sqlite3_finalize(data);

  sqlite3_stmt* object = nullptr;
  const char* objectSql =
    "INSERT INTO objects(object_name,manifest_json,payload,payload_size,sha256,"
    "object_type,updated_at,hit_count) VALUES(?,?,?,?,?,?,1,0)";
  if (sqlite3_prepare_v2(db, objectSql, -1, &object, nullptr) != SQLITE_OK) {
    throw std::runtime_error("fixture-object-prepare-failed");
  }
  bindText(object, 1, source.objectName);
  bindText(object, 2, source.manifest.toJson());
  std::vector<uint8_t> objectPayload(source.payload.begin(), source.payload.end());
  bindBlob(object, 3, objectPayload);
  sqlite3_bind_int64(object, 4, static_cast<sqlite3_int64>(source.payload.size()));
  bindText(object, 5, source.manifest.sha256);
  bindText(object, 6, source.manifest.objectType);
  stepDone(object);
  sqlite3_finalize(object);

  sqlite3_stmt* packetRef = nullptr;
  if (sqlite3_prepare_v2(db,
      "INSERT INTO object_packet_refs(object_name,ordinal,segment_no,data_name)"
      " VALUES(?,0,0,?)", -1, &packetRef, nullptr) != SQLITE_OK) {
    throw std::runtime_error("fixture-packet-ref-prepare-failed");
  }
  bindText(packetRef, 1, source.objectName);
  bindText(packetRef, 2, source.dataName);
  stepDone(packetRef);
  sqlite3_finalize(packetRef);

  sqlite3_stmt* serving = nullptr;
  const char* servingSql =
    "INSERT INTO serving_packets(data_name,object_name,generation,wire,wire_sha256,wire_size)"
    " VALUES(?,?,9,?,?,?)";
  if (sqlite3_prepare_v2(db, servingSql, -1, &serving, nullptr) != SQLITE_OK) {
    throw std::runtime_error("fixture-serving-packet-prepare-failed");
  }
  bindText(serving, 1, source.dataName);
  bindText(serving, 2, source.objectName);
  bindBlob(serving, 3, source.dataWire);
  bindText(serving, 4, wireDigest);
  sqlite3_bind_int64(serving, 5, static_cast<sqlite3_int64>(source.dataWire.size()));
  stepDone(serving);
  sqlite3_finalize(serving);
  return source;
}

std::vector<RepoObjectManifest>
verifyRepoObject(const fs::path& root, const std::string& importId,
                 const SourceObject& source)
{
  FilesystemRepoStoreBackend store((root / "repo-core").string(), 1024, 1024,
                                  "legacy-import-" + importId);
  const auto objectManifest = store.getManifest(source.objectName);
  BOOST_CHECK_EQUAL(objectManifest.sha256, source.manifest.sha256);
  BOOST_CHECK_EQUAL(objectManifest.size, source.payload.size());
  BOOST_CHECK_EQUAL(objectManifest.generation, 9);
  BOOST_CHECK_EQUAL(objectManifest.segmentCount, 1U);
  BOOST_CHECK(objectManifest.packetNames.empty());
  BOOST_CHECK_EQUAL(objectManifest.objectType,
    source.manifest.objectType == "ndn-segmented-data" ? "object" : source.manifest.objectType);
  BOOST_CHECK_EQUAL(objectManifest.replicationFactor, 1);
  BOOST_CHECK(objectManifest.replicaNodes.empty());
  BOOST_CHECK(objectManifest.confirmedReplicaNodes.empty());
  BOOST_CHECK(objectManifest.policyEpoch.empty());
  BOOST_CHECK(objectManifest.publicationIdentity.empty());
  BOOST_CHECK(objectManifest.protectionEpoch.empty());
  BOOST_CHECK(objectManifest.keyReferenceId.empty());
  BOOST_CHECK(objectManifest.keyReferenceVersion.empty());
  BOOST_CHECK(objectManifest.ciphertextManifestDigest.empty());
  BOOST_CHECK(objectManifest.servingLocator.empty());
  BOOST_CHECK(objectManifest.operationId.empty());
  BOOST_CHECK_EQUAL(objectManifest.lifecycleState, "COMMITTED");

  std::string copied;
  copied.reserve(source.payload.size());
  uint64_t offset = 0;
  while (offset < source.payload.size()) {
    const auto length = static_cast<size_t>(std::min<uint64_t>(1024,
      source.payload.size() - offset));
    const auto range = store.getRange(source.objectName, RepoByteRange{offset, length});
    copied.append(reinterpret_cast<const char*>(range.data()), range.size());
    offset += length;
  }
  BOOST_CHECK_EQUAL(copied, source.payload);

  const auto packetManifest = store.getManifest(source.dataName);
  BOOST_CHECK_EQUAL(packetManifest.objectType, "ndn-data-wire");
  BOOST_REQUIRE_EQUAL(packetManifest.packetNames.size(), 1);
  BOOST_CHECK_EQUAL(packetManifest.packetNames.front(), source.dataName);
  std::vector<uint8_t> copiedWire;
  uint64_t wireOffset = 0;
  while (wireOffset < source.dataWire.size()) {
    const auto length = static_cast<size_t>(std::min<uint64_t>(1024,
      source.dataWire.size() - wireOffset));
    const auto range = store.getRange(source.dataName,
      RepoByteRange{wireOffset, length});
    copiedWire.insert(copiedWire.end(), range.begin(), range.end());
    wireOffset += length;
  }
  BOOST_CHECK(copiedWire == source.dataWire);
  return {objectManifest, packetManifest};
}

int
runImporterChild(const fs::path& log, std::vector<std::string> arguments)
{
  const auto binary = fs::read_symlink("/proc/self/exe").parent_path() /
    "NDNSF-DistributedRepo/ndnsf-repo-import";
  arguments.insert(arguments.begin(), binary.string());
  std::vector<char*> argv;
  for (auto& value : arguments) argv.push_back(value.data());
  argv.push_back(nullptr);
  const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) throw std::runtime_error("spec202-cli-log-open-failed");
  const auto child = ::fork();
  if (child == 0) {
    if (::dup2(fd, STDOUT_FILENO) < 0 || ::dup2(fd, STDERR_FILENO) < 0) ::_exit(126);
    ::close(fd);
    ::execv(argv.front(), argv.data());
    ::_exit(127);
  }
  ::close(fd);
  if (child < 0) throw std::runtime_error("spec202-cli-fork-failed");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  int status = 0;
  for (;;) {
    const auto waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child) {
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
    if (waited < 0 && errno == EINTR) continue;
    if (waited < 0 || std::chrono::steady_clock::now() >= deadline) {
      ::kill(child, SIGKILL);
      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
      throw std::runtime_error("spec202-cli-wait-failed: " + log.string());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec192LegacyRepoImportIntegration)

BOOST_AUTO_TEST_CASE(productionCliMatchesNativeImportAndReplayWithoutSourceMutation)
{
  TemporaryDirectory temporary;
  const auto sourcePath = temporary.path() / "legacy.sqlite3";
  const auto payloadRoot = temporary.path() / "source-payloads";
  fs::create_directories(payloadRoot);
  Database sourceDb(sourcePath);
  const auto source = seedRepoFixture(sourceDb.get(), "ndn-segmented-data");
  const auto sourceHashes = sourceBundleHashes(sourcePath);
  LegacyRepoImportConfig config;
  config.sourceDatabase = sourcePath;
  config.sourcePayloadRoot = payloadRoot;
  config.destinationRoot = temporary.path() / "library-target";
  config.importId = "spec202-cli-parity";
  config.maxRangeBytes = 1024;
  const auto direct = LegacyRepoImporter::import(config);
  config.destinationRoot = temporary.path() / "cli-target";
  const std::vector<std::string> arguments{
    "--source-db", sourcePath.string(), "--source-payload-root", payloadRoot.string(),
    "--destination", config.destinationRoot.string(), "--import-id", config.importId,
    "--max-range-bytes", "1024",
  };
  const auto log = temporary.path() / "cli.log";
  BOOST_REQUIRE_EQUAL(runImporterChild(log, arguments), 0);
  const auto replay = LegacyRepoImporter::import(config);
  BOOST_CHECK_EQUAL(readFile(log), replay.toJson() + "\n");
  BOOST_CHECK_EQUAL(replay.state, "COMMITTED");
  BOOST_CHECK_EQUAL(replay.destinationInventoryDigest, direct.destinationInventoryDigest);
  BOOST_CHECK_EQUAL(replay.sourceSnapshotDigest, direct.sourceSnapshotDigest);
  BOOST_CHECK_EQUAL(replay.sourcePayloadInventoryDigest, direct.sourcePayloadInventoryDigest);
  BOOST_REQUIRE_EQUAL(replay.items.size(), direct.items.size());
  for (size_t index = 0; index < replay.items.size(); ++index) {
    BOOST_CHECK_EQUAL(replay.items[index].sourceManifestSha256, direct.items[index].sourceManifestSha256);
    BOOST_CHECK(replay.items[index].packetNames == direct.items[index].packetNames);
    BOOST_CHECK(replay.items[index].packetSha256 == direct.items[index].packetSha256);
  }
  (void)verifyRepoObject(config.destinationRoot, config.importId, source);
  const auto manifestBefore = readFile(config.destinationRoot / "LegacyImportManifest.json");
  BOOST_REQUIRE_EQUAL(runImporterChild(log, arguments), 0);
  BOOST_CHECK_EQUAL(readFile(log), replay.toJson() + "\n");
  BOOST_CHECK_EQUAL(readFile(config.destinationRoot / "LegacyImportManifest.json"), manifestBefore);
  auto invalid = arguments;
  invalid[5] = (temporary.path() / "invalid-target").string();
  invalid.back() = "-1";
  BOOST_CHECK_EQUAL(runImporterChild(log, invalid), 2);
  BOOST_CHECK(!fs::exists(temporary.path() / "invalid-target"));
  BOOST_CHECK(sourceBundleHashes(sourcePath) == sourceHashes);
}

BOOST_AUTO_TEST_CASE(importsSegmentedLegacyObjectAsVerifiedBytesWithPacketProvenance)
{
  TemporaryDirectory temporary;
  const auto sourcePath = temporary.path() / "legacy-repo.sqlite3";
  const auto payloadRoot = temporary.path() / "source-payloads";
  const auto destination = temporary.path() / "native-repo";
  fs::create_directories(payloadRoot);
  Database sourceDb(sourcePath);
  const auto source = seedRepoFixture(sourceDb.get(), "ndn-segmented-data");
  const auto sourceHashes = sourceBundleHashes(sourcePath);
  LegacyRepoImportConfig config;
  config.sourceDatabase = sourcePath;
  config.sourcePayloadRoot = payloadRoot;
  config.destinationRoot = destination;
  config.importId = "spec202-segmented-materialization";
  config.maxRangeBytes = 1024;
  const auto imported = LegacyRepoImporter::import(config);
  BOOST_CHECK_EQUAL(imported.state, "COMMITTED");
  const auto item = std::find_if(imported.items.begin(), imported.items.end(),
    [] (const LegacyImportItem& value) { return value.sourceKind == "python-repo-object"; });
  BOOST_REQUIRE(item != imported.items.end());
  BOOST_CHECK(item->packetNames == source.manifest.packetNames);
  BOOST_REQUIRE_EQUAL(item->packetSha256.size(), 1U);
  BOOST_CHECK_EQUAL(item->packetSha256.front(),
                    sha256(source.dataWire.data(), source.dataWire.size()));
  const auto sourceManifestWire = source.manifest.toJson();
  BOOST_CHECK_EQUAL(item->sourceManifestSha256,
                    sha256(sourceManifestWire.data(), sourceManifestWire.size()));
  const auto inventory = verifyRepoObject(destination, config.importId, source);
  BOOST_CHECK_EQUAL(imported.destinationInventoryDigest, repoDestinationDigest(inventory));
  BOOST_CHECK_EQUAL(LegacyRepoImporter::import(config).destinationInventoryDigest,
                    imported.destinationInventoryDigest);

  // Persisted-input migration probe, derived from a real interrupted import.
  auto stageConfig = config;
  stageConfig.destinationRoot = temporary.path() / "old-layout-target";
  stageConfig.importId = "spec202-old-layout";
  stageConfig.onItemCommitted = [] (size_t index, const LegacyImportItem&) {
    if (index == 0) throw std::runtime_error("spec202-migration-interruption");
  };
  BOOST_CHECK_EXCEPTION(LegacyRepoImporter::import(stageConfig), std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()) == "spec202-migration-interruption";
    });
  const auto staging = temporary.path() /
    ".old-layout-target.ndnsf-import-spec202-old-layout.staging";
  auto oldInventory = inventory;
  oldInventory.front().objectType = source.manifest.objectType;
  oldInventory.front().segmentCount = source.manifest.segmentCount;
  oldInventory.front().packetNames = source.manifest.packetNames;
  const auto oldDigest = repoDestinationDigest(oldInventory);
  BOOST_REQUIRE(oldDigest != imported.destinationInventoryDigest);
  const auto stagedManifest = staging / "LegacyImportManifest.json";
  auto stagedWire = readFile(stagedManifest);
  const auto digestField = "\"destinationInventoryDigest\":\"" +
                           imported.destinationInventoryDigest + "\"";
  const auto digestPosition = stagedWire.find(digestField);
  BOOST_REQUIRE(digestPosition != std::string::npos);
  stagedWire.replace(digestPosition, digestField.size(),
                    "\"destinationInventoryDigest\":\"" + oldDigest + "\"");
  {
    std::ofstream output(stagedManifest, std::ios::binary | std::ios::trunc);
    output << stagedWire;
    BOOST_REQUIRE(output.good());
  }
  const auto objectKey = sha256(source.objectName.data(), source.objectName.size());
  {
    std::ofstream output(staging / "repo-core/manifests" / (objectKey + ".json"),
                         std::ios::binary | std::ios::trunc);
    output << oldInventory.front().toJson();
    BOOST_REQUIRE(output.good());
  }
  const auto stageHashes = [&] {
    FileHashes hashes;
    for (const auto& entry : fs::recursive_directory_iterator(staging)) {
      if (entry.is_regular_file()) hashes.emplace(
        fs::relative(entry.path(), staging).generic_string(), sha256File(entry.path()));
    }
    return hashes;
  };
  const auto beforeRejectedReplay = stageHashes();
  stageConfig.onItemCommitted = {};
  BOOST_CHECK_EXCEPTION(LegacyRepoImporter::import(stageConfig), LegacyRepoImportError,
    [] (const LegacyRepoImportError& error) {
      return std::string(error.what()) == "legacy-import-existing-state-conflict";
    });
  BOOST_CHECK(stageHashes() == beforeRejectedReplay);
  BOOST_CHECK(!fs::exists(stageConfig.destinationRoot));
  BOOST_CHECK(sourceBundleHashes(sourcePath) == sourceHashes);
}

BOOST_AUTO_TEST_CASE(importsExactRepoProfileInBoundedRangesAndResumesIncompleteStage)
{
  TemporaryDirectory temporary;
  const auto sourcePath = temporary.path() / "legacy-repo.sqlite3";
  const auto payloadRoot = temporary.path() / "source-payloads";
  const auto destination = temporary.path() / "native-repo";
  fs::create_directories(payloadRoot);

  Database sourceDb(sourcePath);
  const auto source = seedRepoFixture(sourceDb.get());
  const auto walPath = fs::path(sourcePath.string() + "-wal");
  BOOST_REQUIRE(fs::exists(walPath));
  BOOST_REQUIRE_GT(fs::file_size(walPath), 0);
  const auto sourceHashes = sourceBundleHashes(sourcePath);
  const auto expectedSnapshotDigest = sourceBundleDigest(sourcePath);
  const auto expectedPayloadDigest = sha256("[]", 2);

  LegacyRepoImportConfig config;
  config.sourceDatabase = sourcePath;
  config.sourcePayloadRoot = payloadRoot;
  config.destinationRoot = destination;
  config.importId = "repo-profile-wal-import";
  config.maxRangeBytes = 1024;
  config.onItemCommitted = [](size_t index, const LegacyImportItem&) {
    if (index == 0) throw std::runtime_error("simulated-import-interruption");
  };
  std::string importError;
  try {
    (void)LegacyRepoImporter::import(config);
  }
  catch (const std::exception& error) {
    importError = error.what();
  }
  BOOST_CHECK_MESSAGE(importError == "simulated-import-interruption",
                      "unexpected import error: " << importError);
  BOOST_CHECK(!fs::exists(destination));
  const auto staging = destination.parent_path() /
    ("." + destination.filename().string() + ".ndnsf-import-" +
     config.importId + ".staging");
  BOOST_REQUIRE(fs::exists(staging / "LegacyImportManifest.json"));
  const auto stagingPermissions = fs::status(staging).permissions();
  BOOST_CHECK((stagingPermissions &
    (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
  const auto incomplete = readFile(staging / "LegacyImportManifest.json");
  BOOST_CHECK(incomplete.find("\"state\":\"INCOMPLETE\"") != std::string::npos);
  BOOST_CHECK(incomplete.find("\"state\":\"COMMITTED\"") != std::string::npos);
  BOOST_CHECK(sourceBundleHashes(sourcePath) == sourceHashes);

  config.onItemCommitted = {};
  const auto imported = LegacyRepoImporter::import(config);
  BOOST_CHECK_EQUAL(imported.state, "COMMITTED");
  BOOST_CHECK_EQUAL(imported.sourceSchemaId, "python-repo-v12-exact-packets");
  BOOST_CHECK_EQUAL(imported.sourceSchemaFingerprint,
    "56dc41b33bd3b67c4c0cd863482b4355beda11585b141122946e28093c974682");
  BOOST_CHECK_EQUAL(imported.sourceSnapshotDigest, expectedSnapshotDigest);
  BOOST_CHECK_EQUAL(imported.sourcePayloadInventoryDigest, expectedPayloadDigest);
  BOOST_REQUIRE_EQUAL(imported.items.size(), 2);
  BOOST_CHECK_EQUAL(imported.items.front().sourceGeneration, 9);
  BOOST_CHECK_EQUAL(imported.items.front().nativeGeneration, 9);
  BOOST_CHECK_EQUAL(imported.items.back().nativeGeneration, 1);
  BOOST_CHECK(std::any_of(imported.legacyClaims.begin(), imported.legacyClaims.end(),
    [](const LegacyImportClaim& claim) {
      return claim.table == "repo_meta" && claim.sourceKey.find("schema_version") !=
        std::string::npos;
    }));
  BOOST_CHECK_EQUAL(imported.sourceSnapshotDigest.size(), 64);
  BOOST_CHECK_EQUAL(imported.sourcePayloadInventoryDigest.size(), 64);
  BOOST_CHECK_EQUAL(imported.destinationInventoryDigest.size(), 64);
  BOOST_CHECK_EQUAL(imported.itemCount, 2);
  BOOST_CHECK_EQUAL(imported.byteCount, source.payload.size() + source.dataWire.size());
  BOOST_CHECK(sourceBundleHashes(sourcePath) == sourceHashes);

  const auto nativeInventory = verifyRepoObject(destination, config.importId, source);
  BOOST_CHECK_EQUAL(imported.destinationInventoryDigest,
                    repoDestinationDigest(nativeInventory));
  const auto repeated = LegacyRepoImporter::import(config);
  BOOST_CHECK_EQUAL(repeated.state, "COMMITTED");
  BOOST_CHECK_EQUAL(repeated.destinationInventoryDigest,
                    imported.destinationInventoryDigest);
  BOOST_CHECK(sourceBundleHashes(sourcePath) == sourceHashes);

  const auto manifestPath = destination / "LegacyImportManifest.json";
  auto tamperedManifest = readFile(manifestPath);
  const auto items = tamperedManifest.find("\"items\":[");
  BOOST_REQUIRE_NE(items, std::string::npos);
  const auto itemState = tamperedManifest.find("\"state\":\"COMMITTED\"", items);
  BOOST_REQUIRE_NE(itemState, std::string::npos);
  tamperedManifest.replace(itemState, std::string("\"state\":\"COMMITTED\"").size(),
                           "\"state\":\"PREPARING\"");
  {
    std::ofstream output(manifestPath, std::ios::binary | std::ios::trunc);
    BOOST_REQUIRE(output);
    output.write(tamperedManifest.data(),
                 static_cast<std::streamsize>(tamperedManifest.size()));
    BOOST_REQUIRE(output);
  }
  BOOST_CHECK_THROW(LegacyRepoImporter::import(config), LegacyRepoImportError);
}

BOOST_AUTO_TEST_CASE(rejectsNonemptyLegacySegmentsBeforePublish)
{
  TemporaryDirectory temporary;
  const auto sourcePath = temporary.path() / "legacy.sqlite3";
  const auto payloadRoot = temporary.path() / "payloads";
  const auto destination = temporary.path() / "native";
  fs::create_directories(payloadRoot);
  Database sourceDb(sourcePath);
  (void)seedRepoFixture(sourceDb.get());
  execute(sourceDb.get(),
    "INSERT INTO data_segments(object_name,segment_no,data_name,wire,wire_size,updated_at,hit_count)"
    " VALUES('/legacy/old',0,'/legacy/old/seg=0',X'00',1,1,0)");

  LegacyRepoImportConfig config;
  config.sourceDatabase = sourcePath;
  config.sourcePayloadRoot = payloadRoot;
  config.destinationRoot = destination;
  config.importId = "reject-nonempty-segments";
  BOOST_CHECK_THROW(LegacyRepoImporter::import(config), LegacyRepoImportError);
  BOOST_CHECK(!fs::exists(destination));
}

BOOST_AUTO_TEST_CASE(rejectsInvalidPacketWireBeforePublish)
{
  TemporaryDirectory temporary;
  const auto sourcePath = temporary.path() / "legacy.sqlite3";
  const auto payloadRoot = temporary.path() / "payloads";
  const auto destination = temporary.path() / "native";
  fs::create_directories(payloadRoot);
  Database sourceDb(sourcePath);
  execute(sourceDb.get(), readFile(repositoryRoot() /
    "tests/fixtures/spec192/legacy-python-repo-v12.sql"));
  execute(sourceDb.get(),
    "INSERT INTO serving_packets(data_name,object_name,generation,wire,wire_sha256,wire_size)"
    " VALUES('/invalid/wire','/legacy/invalid',1,X'00',"
    "'6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d',1)");

  LegacyRepoImportConfig config;
  config.sourceDatabase = sourcePath;
  config.sourcePayloadRoot = payloadRoot;
  config.destinationRoot = destination;
  config.importId = "reject-invalid-wire";
  BOOST_CHECK_THROW(LegacyRepoImporter::import(config), LegacyRepoImportError);
  BOOST_CHECK(!fs::exists(destination));
}

BOOST_AUTO_TEST_CASE(rejectsPacketDigestMismatchBeforePublish)
{
  TemporaryDirectory temporary;
  const auto sourcePath = temporary.path() / "legacy.sqlite3";
  const auto payloadRoot = temporary.path() / "payloads";
  const auto destination = temporary.path() / "native";
  fs::create_directories(payloadRoot);
  Database sourceDb(sourcePath);
  execute(sourceDb.get(), readFile(repositoryRoot() /
    "tests/fixtures/spec192/legacy-python-repo-v12.sql"));
  execute(sourceDb.get(),
    "INSERT INTO serving_packets(data_name,object_name,generation,wire,wire_sha256,wire_size)"
    " VALUES('/invalid/digest','/legacy/invalid',1,X'00',"
    "'0000000000000000000000000000000000000000000000000000000000000000',1)");

  LegacyRepoImportConfig config;
  config.sourceDatabase = sourcePath;
  config.sourcePayloadRoot = payloadRoot;
  config.destinationRoot = destination;
  config.importId = "reject-packet-digest-mismatch";
  BOOST_CHECK_THROW(LegacyRepoImporter::import(config), LegacyRepoImportError);
  BOOST_CHECK(!fs::exists(destination));
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf_distributed_repo::test
