#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePortableKvCheckpoint.hpp"

#include <boost/test/unit_test.hpp>
#include <openssl/evp.h>
#include <atomic>
#include <fstream>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

using namespace ndnsf::di;

namespace {
struct Directory
{
  Directory() {
    char pattern[] = "/tmp/spec204-private-kv-XXXXXX";
    const auto result = ::mkdtemp(pattern);
    if (!result) throw std::runtime_error("private KV fixture directory failed");
    path = result;
  }
  ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
  std::filesystem::path path;
};

std::string digest(unsigned n) { return "sha256:" + std::string(63, 'a') + "0123456789abcdef"[n % 16]; }

NativePortableKvSnapshotV1 sample()
{
  NativePortableKvSnapshotV1 result;
  auto& id = result.originIdentity;
  id.modelDigest = digest(0); id.graphSemanticDigest = digest(1);
  id.artifactDigest = digest(2); id.adapterDigest = digest(3);
  id.tokenizerDigest = digest(4); id.runnerDigest = digest(5);
  id.roleName = "decoder/key"; id.roleSplitDigest = digest(6);
  id.layerBegin = 2; id.layerEnd = 4; id.prefixDigest = digest(7);
  id.prefixTokenCount = 7; id.positionDigest = digest(8); id.precision = "float32";
  id.layoutDigest = digest(9); id.stateSchemaDigest = digest(10);
  id.stateComponentDigests = {digest(11), digest(12)};
  id.runtimeAbiDigest = digest(13); id.securityDomainDigest = digest(14);
  id.providerIdentity = "/actual/provider/a"; id.providerBootId = "boot\"a";
  id.requestId = "/actual/user/request"; id.generationId = "generation-a";
  id.stateInferenceEpoch = std::numeric_limits<std::uint64_t>::max();
  id.predecessorInferenceEpoch = id.stateInferenceEpoch - 1;
  id.cacheEpoch = id.stateInferenceEpoch; id.attemptEpoch = 2;
  NativePortableKvComponentV1 component;
  component.inputName = "past/key"; component.elementType = TensorElementType::Float32;
  component.shape = {2}; component.payloadBytes = 8;
  component.tensorWire = encodeTensorBundle({NamedTensor{
    component.inputName, component.elementType, component.shape, {0, 0, 128, 63, 0, 0, 0, 64}}});
  unsigned char bytes[EVP_MAX_MD_SIZE]; unsigned length = 0;
  BOOST_REQUIRE_EQUAL(EVP_Digest(component.tensorWire.data(), component.tensorWire.size(),
    bytes, &length, EVP_sha256(), nullptr), 1);
  BOOST_REQUIRE_EQUAL(length, 32U);
  component.contentDigest = "sha256:";
  for (unsigned i = 0; i < length; ++i) {
    component.contentDigest += "0123456789abcdef"[bytes[i] >> 4];
    component.contentDigest += "0123456789abcdef"[bytes[i] & 15];
  }
  result.components = {component}; result.stateIoDigest = digest(15);
  result.logicalBytes = component.payloadBytes; result.encodedBytes = component.tensorWire.size();
  return result;
}

NativePortableKvCheckpointLimitsV1 limits() { return {{64, 1024, {}}, 8192, 16384}; }

std::string read(const std::filesystem::path& path)
{
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}

void write(const std::filesystem::path& path, const std::string& bytes)
{
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  BOOST_REQUIRE(output.write(bytes.data(), bytes.size()));
  output.close();
  BOOST_REQUIRE_EQUAL(::chmod(path.c_str(), 0600), 0);
}

std::uint64_t metadataLength(const std::string& bytes)
{
  BOOST_REQUIRE(bytes.size() >= 16);
  std::uint64_t result = 0;
  for (unsigned i = 8; i < 16; ++i) result = (result << 8) | static_cast<unsigned char>(bytes[i]);
  BOOST_REQUIRE(result <= bytes.size() - 16);
  return result;
}

std::string mutate(const std::string& bytes, const std::function<void(NativeJson&)>& change)
{
  const auto length = metadataLength(bytes);
  auto json = nativeParseJson(bytes.substr(16, length));
  change(json);
  const auto metadata = nativeCanonicalJson(json);
  auto result = bytes.substr(0, 16);
  auto size = static_cast<std::uint64_t>(metadata.size());
  for (int i = 15; i >= 8; --i) { result[i] = size & 0xff; size >>= 8; }
  return result + metadata + bytes.substr(16 + length);
}
} // namespace

BOOST_AUTO_TEST_SUITE(Spec204PrivateKvCheckpoint)

BOOST_AUTO_TEST_CASE(CompleteOriginAndExactNativeWireRoundTrip)
{
  Directory directory; const auto file = directory.path / "role.kv";
  const auto original = sample();
  nativeExportPrivatePortableKvCheckpoint(file, original, limits());
  struct stat info{}; BOOST_REQUIRE_EQUAL(::lstat(file.c_str(), &info), 0);
  BOOST_CHECK_EQUAL(info.st_mode & 0777, 0600);
  const auto bytes = read(file);
  BOOST_CHECK_EQUAL(bytes.substr(0, 8), "NDKV0001");
  const auto length = metadataLength(bytes);
  BOOST_CHECK_EQUAL(bytes.size(), 16 + length + original.encodedBytes);
  const auto json = nativeParseJson(bytes.substr(16, length));
  BOOST_CHECK_EQUAL(nativeCanonicalJson(json), bytes.substr(16, length));
  BOOST_CHECK_EQUAL(json.at("originIdentity").size(), 27U);
  const auto restored = nativeReadPrivatePortableKvCheckpoint(file, limits());
  BOOST_CHECK(restored.originIdentity == original.originIdentity);
  BOOST_CHECK_EQUAL(restored.stateIoDigest, original.stateIoDigest);
  BOOST_CHECK_EQUAL(restored.logicalBytes, original.logicalBytes);
  BOOST_CHECK_EQUAL(restored.encodedBytes, original.encodedBytes);
  BOOST_REQUIRE_EQUAL(restored.components.size(), original.components.size());
  const auto& component = restored.components.front();
  BOOST_CHECK_EQUAL(component.inputName, original.components.front().inputName);
  BOOST_CHECK(component.elementType == original.components.front().elementType);
  BOOST_CHECK(component.shape == original.components.front().shape);
  BOOST_CHECK_EQUAL(component.contentDigest, original.components.front().contentDigest);
  BOOST_CHECK(component.tensorWire == original.components.front().tensorWire);
}

BOOST_AUTO_TEST_CASE(RejectsMalformedMetadataAndFramingBeforePayloadAllocation)
{
  Directory directory; const auto file = directory.path / "role.kv";
  nativeExportPrivatePortableKvCheckpoint(file, sample(), limits());
  const auto valid = read(file);
  const std::vector<std::function<void(NativeJson&)>> changes = {
    [](auto& j) { j["version"] = 2; },
    [](auto& j) { j["extra"] = 1; },
    [](auto& j) { j.erase("stateIoDigest"); },
    [](auto& j) { j["logicalBytes"] = -1; },
    [](auto& j) { j["encodedBytes"] = 1.0; },
    [](auto& j) { j["originIdentity"]["cacheEpoch"] = true; },
    [](auto& j) { j["originIdentity"]["layerBegin"] = 1ULL << 32; },
    [](auto& j) { j["originIdentity"]["generationId"] = std::string(4097, 'x'); },
    [](auto& j) { j["originIdentity"]["stateComponentDigests"] = NativeJson::array(); },
    [](auto& j) { j["components"][0]["wireBytes"] = 1ULL << 40; },
    [](auto& j) { j["components"][0]["shape"] = {-1}; },
    [](auto& j) { j["components"][0]["elementType"] = 99; },
    [](auto& j) { j["components"][0]["payloadBytes"] = 7; },
    [](auto& j) { j["components"][0]["contentDigest"] = digest(0); },
  };
  for (const auto& change : changes) {
    write(file, mutate(valid, change));
    BOOST_CHECK_THROW(nativeReadPrivatePortableKvCheckpoint(file, limits()), std::invalid_argument);
  }
  for (unsigned mode = 0; mode < 4; ++mode) {
    auto bytes = valid;
    if (mode == 0) bytes[0] = '!';
    if (mode == 1) bytes.pop_back();
    if (mode == 2) bytes += 'x';
    if (mode == 3) bytes.back() ^= 1;
    write(file, bytes);
    BOOST_CHECK_THROW(nativeReadPrivatePortableKvCheckpoint(file, limits()), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(PrivateFilesAndBudgetsRejectBeforeStateOrOutputChanges)
{
  Directory directory; const auto file = directory.path / "role.kv";
  nativeExportPrivatePortableKvCheckpoint(file, sample(), limits());
  const auto valid = read(file);
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto budget = limits();
    if (mode == 0) budget.maxMetadataBytes = 0;
    if (mode == 1) budget.maxCheckpointBytes = 16;
    if (mode == 2) budget.portable.maxEncodedBytes = 8;
    BOOST_CHECK_THROW(nativeExportPrivatePortableKvCheckpoint(file, sample(), budget), std::invalid_argument);
    BOOST_CHECK_EQUAL(read(file), valid);
    BOOST_CHECK_THROW(nativeReadPrivatePortableKvCheckpoint(file, budget), std::invalid_argument);
  }
  const auto alias = directory.path / "alias";
  BOOST_REQUIRE_EQUAL(::symlink(file.c_str(), alias.c_str()), 0);
  BOOST_CHECK_THROW(nativeReadPrivatePortableKvCheckpoint(alias, limits()), std::exception);
  BOOST_REQUIRE_EQUAL(::unlink(alias.c_str()), 0);
  BOOST_REQUIRE_EQUAL(::link(file.c_str(), alias.c_str()), 0);
  BOOST_CHECK_THROW(nativeReadPrivatePortableKvCheckpoint(file, limits()), std::invalid_argument);
  BOOST_REQUIRE_EQUAL(::unlink(alias.c_str()), 0);
  BOOST_REQUIRE_EQUAL(::chmod(file.c_str(), 0644), 0);
  BOOST_CHECK_THROW(nativeReadPrivatePortableKvCheckpoint(file, limits()), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(AtomicFaultsAndCancellationPreservePreviousFile)
{
  Directory directory; const auto file = directory.path / "role.kv";
  for (const auto stage : {NativeCheckpointExportStage::BeforeWrite,
       NativeCheckpointExportStage::BeforeFileSync, NativeCheckpointExportStage::BeforeRename,
       NativeCheckpointExportStage::BeforeDirectorySync}) {
    write(file, "previous");
    NativeCheckpointExportOptions options;
    options.beforeStage = [stage](auto actual) { if (actual == stage) throw std::runtime_error("injected"); };
    BOOST_CHECK_THROW(nativeExportPrivatePortableKvCheckpoint(file, sample(), limits(), options), std::exception);
    BOOST_CHECK_EQUAL(read(file), "previous");
    BOOST_CHECK_EQUAL(std::distance(std::filesystem::directory_iterator(directory.path),
      std::filesystem::directory_iterator{}), 1);
  }
  bool stop = false; auto budget = limits(); budget.portable.cancelled = [&] { return stop; };
  NativeCheckpointExportOptions options;
  options.beforeStage = [&](auto stage) { if (stage == NativeCheckpointExportStage::BeforeWrite) stop = true; };
  BOOST_CHECK_THROW(nativeExportPrivatePortableKvCheckpoint(file, sample(), budget, options), std::runtime_error);
  BOOST_CHECK_EQUAL(read(file), "previous");
}

BOOST_AUTO_TEST_CASE(FailedRestoreRetainsOnlyPriorBackupAndLeavesForeignDirectory)
{
  Directory directory; const auto file = directory.path / "role.kv";
  write(file, "previous");
  NativeCheckpointExportOptions options;
  options.beforeStage = [&](auto stage) {
    if (stage == NativeCheckpointExportStage::BeforeDirectorySync) {
      BOOST_REQUIRE_EQUAL(::unlink(file.c_str()), 0);
      BOOST_REQUIRE_EQUAL(::mkdir(file.c_str(), 0700), 0);
      throw std::runtime_error("injected destination replacement");
    }
  };
  bool caught = false;
  try { nativeExportPrivatePortableKvCheckpoint(file, sample(), limits(), options); }
  catch (const NativeCheckpointRollbackUnconfirmed& error) {
    caught = true;
    BOOST_CHECK_EQUAL(error.what(), "native checkpoint rollback unconfirmed");
    BOOST_CHECK(!error.retainedBackupPath().empty());
    BOOST_CHECK_EQUAL(read(error.retainedBackupPath()), "previous");
    BOOST_CHECK(std::filesystem::is_directory(file));
  }
  BOOST_CHECK(caught);
}

BOOST_AUTO_TEST_CASE(RecoveryDirectorySyncFailureIsUnconfirmedWithVisiblePriorFile)
{
  Directory directory; const auto file = directory.path / "role.kv";
  write(file, "previous");
  struct stat parent{}; BOOST_REQUIRE_EQUAL(::stat(directory.path.c_str(), &parent), 0);
  NativeCheckpointExportOptions options;
  options.beforeStage = [&](auto stage) {
    if (stage != NativeCheckpointExportStage::BeforeDirectorySync) return;
    unsigned closed = 0;
    // Only the exporter's fd for this uniquely owned fixture directory is closed.
    for (int fd = 3; fd < 256; ++fd) {
      struct stat info{};
      if (::fstat(fd, &info) == 0 && S_ISDIR(info.st_mode) &&
          info.st_dev == parent.st_dev && info.st_ino == parent.st_ino) {
        BOOST_REQUIRE_EQUAL(::close(fd), 0); ++closed;
      }
    }
    BOOST_REQUIRE_EQUAL(closed, 1U);
  };
  bool caught = false;
  try { nativeExportPrivatePortableKvCheckpoint(file, sample(), limits(), options); }
  catch (const NativeCheckpointRollbackUnconfirmed& error) {
    caught = true; BOOST_CHECK(error.retainedBackupPath().empty());
    BOOST_CHECK_EQUAL(read(file), "previous");
  }
  BOOST_CHECK(caught);
}

BOOST_AUTO_TEST_CASE(NoPriorBackupDoesNotRemoveForeignDirectory)
{
  Directory directory; const auto file = directory.path / "role.kv";
  NativeCheckpointExportOptions options;
  options.beforeStage = [&](auto stage) {
    if (stage == NativeCheckpointExportStage::BeforeDirectorySync) {
      BOOST_REQUIRE_EQUAL(::unlink(file.c_str()), 0);
      BOOST_REQUIRE_EQUAL(::mkdir(file.c_str(), 0700), 0);
      throw std::runtime_error("injected new output replacement");
    }
  };
  bool caught = false;
  try { nativeExportPrivatePortableKvCheckpoint(file, sample(), limits(), options); }
  catch (const NativeCheckpointRollbackUnconfirmed& error) {
    caught = true; BOOST_CHECK(error.retainedBackupPath().empty());
    BOOST_CHECK(std::filesystem::is_directory(file));
  }
  BOOST_CHECK(caught);
}

BOOST_AUTO_TEST_CASE(MalformedMetadataDoesNotEchoPrivateBytes)
{
  Directory directory; const auto file = directory.path / "role.kv";
  nativeExportPrivatePortableKvCheckpoint(file, sample(), limits());
  auto bytes = read(file);
  const auto length = metadataLength(bytes);
  bytes.replace(16, length, std::string(length, 'x'));
  write(file, bytes);
  bool caught = false;
  try { (void)nativeReadPrivatePortableKvCheckpoint(file, limits()); }
  catch (const std::invalid_argument& error) {
    caught = true; BOOST_CHECK_EQUAL(error.what(), "native portable checkpoint invalid");
  }
  BOOST_CHECK(caught);
}

BOOST_AUTO_TEST_CASE(FifoRejectsWithoutWaitingForAnotherProcess)
{
  Directory directory; const auto file = directory.path / "role.fifo";
  BOOST_REQUIRE_EQUAL(::mkfifo(file.c_str(), 0600), 0);
  BOOST_CHECK_THROW(nativeReadPrivatePortableKvCheckpoint(file, limits()), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(DuplicateNameRejectsBeforePayloadPhaseCancellationProbe)
{
  Directory directory; const auto file = directory.path / "role.kv";
  nativeExportPrivatePortableKvCheckpoint(file, sample(), limits());
  const auto valid = read(file);
  const auto length = metadataLength(valid);
  auto bytes = mutate(valid, [](auto& j) {
    j["components"].push_back(j["components"][0]);
    j["logicalBytes"] = j["logicalBytes"].template get<std::uint64_t>() * 2;
    j["encodedBytes"] = j["encodedBytes"].template get<std::uint64_t>() * 2;
  });
  bytes += valid.substr(16 + length);
  write(file, bytes);
  std::atomic<unsigned> probes{0}; auto budget = limits();
  // Small metadata uses exactly the three preflight/header/metadata probes.
  // Any entry into the payload phase would cancel instead of accepting allocation.
  budget.portable.cancelled = [&] { return ++probes > 3; };
  BOOST_CHECK_THROW(nativeReadPrivatePortableKvCheckpoint(file, budget), std::invalid_argument);
  BOOST_CHECK_EQUAL(probes.load(), 3U);
}

BOOST_AUTO_TEST_SUITE_END()
