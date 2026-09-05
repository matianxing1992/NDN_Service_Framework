#include "tests/boost-test.hpp"

#include "ndn-service-framework/RuntimeStatusStore.hpp"

#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace ndn_service_framework::test {

namespace {

RuntimeStatusStore::Record
makeRecord(const std::string& service, uint64_t timestamp, uint64_t epoch)
{
  RuntimeStatusStore::Record record;
  record.serviceName = ndn::Name(service);
  record.controllerVersion = ControllerVersion{timestamp, epoch};
  record.installTimeMs = 1700000000000ULL + epoch;
  record.abePublicParametersName =
      ndn::Name("/spec179/controller/params").appendVersion(1);
  record.abePublicParametersDigest =
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  PolicyStatusData status;
  status.setServiceName(ndn::Name(service));
  status.setControllerVersion(record.controllerVersion);
  status.setValidity(900, 3000);
  status.setPolicyDigest(record.abePublicParametersDigest);
  status.setControllerCertificate(ndn::Name("/controller/KEY/signing/v=1"));
  status.setSignature(ndn::Buffer(reinterpret_cast<const uint8_t*>("sig"), 3));
  const auto statusWire = status.wireEncode();
  record.policyStatusWire =
      ndn::Buffer(statusWire.data(), statusWire.size());
  // The signed Data wire is opaque to the store; the runtime re-verifies it
  // against the configured trust anchor on restore.  Any non-empty bytes
  // round-trip.
  const std::string dataBytes =
      "signed-data-wire-for-" + service + "-" + std::to_string(epoch);
  record.statusDataWire = ndn::Buffer(
      reinterpret_cast<const uint8_t*>(dataBytes.data()), dataBytes.size());
  return record;
}

std::filesystem::path
tempStorePath(const char* tag)
{
  const auto path = std::filesystem::temp_directory_path() /
                    (std::string("ndnsf-runtime-status-") + tag + "-" +
                     std::to_string(::getpid()) + ".rts");
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".tmp." + std::to_string(::getpid()));
  return path;
}

void
writeRaw(const std::filesystem::path& path, const std::string& bytes)
{
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

BOOST_AUTO_TEST_SUITE(RuntimeStatusStorePersistence)

BOOST_AUTO_TEST_CASE(RoundTripPersistsEveryAcceptedField)
{
  const auto path = tempStorePath("roundtrip");
  const auto first = makeRecord("/ObjectDetection/YOLOv8", 1, 1);
  const auto second = makeRecord("/ObjectDetection/SSD", 1, 2);

  {
    RuntimeStatusStore store(path);
    BOOST_REQUIRE(store.persist({first, second}));
    BOOST_CHECK(std::filesystem::exists(path));
  }

  std::vector<RuntimeStatusStore::Record> loaded;
  {
    RuntimeStatusStore restarted(path);
    BOOST_REQUIRE(restarted.load(loaded));
  }
  BOOST_REQUIRE_EQUAL(loaded.size(), 2U);
  const auto byService = [&loaded] (const char* uri) -> const RuntimeStatusStore::Record* {
    for (const auto& record : loaded) {
      if (record.serviceName == ndn::Name(uri))
        return &record;
    }
    return nullptr;
  };
  const auto* firstLoaded = byService("/ObjectDetection/YOLOv8");
  const auto* secondLoaded = byService("/ObjectDetection/SSD");
  BOOST_REQUIRE(firstLoaded != nullptr);
  BOOST_REQUIRE(secondLoaded != nullptr);
  BOOST_CHECK(firstLoaded->controllerVersion == (ControllerVersion{1, 1}));
  BOOST_CHECK_EQUAL(firstLoaded->installTimeMs, 1700000000001ULL);
  BOOST_CHECK_EQUAL(firstLoaded->abePublicParametersName,
                    secondLoaded->abePublicParametersName);
  BOOST_CHECK_EQUAL(firstLoaded->abePublicParametersDigest,
                    secondLoaded->abePublicParametersDigest);
  BOOST_CHECK(std::equal(firstLoaded->policyStatusWire.begin(),
                         firstLoaded->policyStatusWire.end(),
                         first.policyStatusWire.begin()));
  BOOST_CHECK(std::equal(firstLoaded->statusDataWire.begin(),
                         firstLoaded->statusDataWire.end(),
                         first.statusDataWire.begin()));
  BOOST_CHECK(std::equal(secondLoaded->statusDataWire.begin(),
                         secondLoaded->statusDataWire.end(),
                         second.statusDataWire.begin()));

  std::filesystem::remove(path);
}

BOOST_AUTO_TEST_CASE(PersistReplacesWholeStoreAtomically)
{
  const auto path = tempStorePath("overwrite");
  RuntimeStatusStore store(path);
  BOOST_REQUIRE(store.persist({makeRecord("/HELLO", 1, 1)}));

  std::vector<RuntimeStatusStore::Record> loaded;
  BOOST_REQUIRE(store.load(loaded));
  BOOST_REQUIRE_EQUAL(loaded.size(), 1U);

  const auto replacement = makeRecord("/HELLO", 2, 1);
  BOOST_REQUIRE(store.persist({replacement}));
  BOOST_REQUIRE(store.load(loaded));
  BOOST_REQUIRE_EQUAL(loaded.size(), 1U);
  BOOST_CHECK(loaded.front().controllerVersion == (ControllerVersion{2, 1}));

  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(
           path.parent_path(), ec)) {
    const auto name = entry.path().filename().string();
    if (name.find("ndnsf-runtime-status-overwrite-") != std::string::npos &&
        name.find(".tmp.") != std::string::npos) {
      BOOST_FAIL("temporary file left behind after persist: " + name);
    }
  }
  std::filesystem::remove(path);
}

BOOST_AUTO_TEST_CASE(MissingStoreIsColdStartAndDisabledModeStoresNothing)
{
  const auto path = tempStorePath("coldstart");
  {
    RuntimeStatusStore store(path);
    std::vector<RuntimeStatusStore::Record> loaded;
    BOOST_CHECK(!store.load(loaded)); // missing file: cold start, no restore
    BOOST_CHECK(loaded.empty());
  }
  std::filesystem::remove(path);
}

BOOST_AUTO_TEST_CASE(CorruptOrTruncatedStoreFailsClosed)
{
  const auto path = tempStorePath("corrupt");
  RuntimeStatusStore valid(path);
  BOOST_REQUIRE(valid.persist({makeRecord("/HELLO", 1, 1)}));

  {
    // Truncated header.
    writeRaw(path, "NDNS");
    RuntimeStatusStore store(path);
    std::vector<RuntimeStatusStore::Record> loaded;
    BOOST_CHECK(!store.load(loaded));
    BOOST_CHECK(loaded.empty());
  }
  {
    // Wrong magic.
    writeRaw(path, "XXXXXXXXGARBAGE");
    RuntimeStatusStore store(path);
    std::vector<RuntimeStatusStore::Record> loaded;
    BOOST_CHECK(!store.load(loaded));
  }
  {
    // Record count above the sanity bound.
    writeRaw(path, "NDNSFRS1" + std::string("\xff\xff\xff\xff\xff\xff\xff\xff", 8));
    RuntimeStatusStore store(path);
    std::vector<RuntimeStatusStore::Record> loaded;
    BOOST_CHECK(!store.load(loaded));
  }
  {
    // Truncated record body (count 1 then EOF).
    writeRaw(path, std::string("NDNSFRS1") +
                   std::string("\x00\x00\x00\x00\x00\x00\x00\x01", 8));
    RuntimeStatusStore store(path);
    std::vector<RuntimeStatusStore::Record> loaded;
    BOOST_CHECK(!store.load(loaded));
  }
  {
    // Valid record followed by trailing garbage.  The earlier sub-cases
    // overwrote the path with corrupt bytes, so rebuild the valid store.
    BOOST_REQUIRE(valid.persist({makeRecord("/HELLO", 1, 1)}));
    {
      std::ofstream tail(path, std::ios::binary | std::ios::app);
      tail.put('X');
    }
    RuntimeStatusStore store(path);
    std::vector<RuntimeStatusStore::Record> loaded;
    BOOST_CHECK(!store.load(loaded));
  }
  std::filesystem::remove(path);
}

BOOST_AUTO_TEST_CASE(StoreRefusesInvalidInputAndUnwritableLocation)
{
  const auto path = tempStorePath("invalid");
  {
    // Records with empty service name or empty signed wire are refused.
    RuntimeStatusStore store(path);
    auto emptyService = makeRecord("/HELLO", 1, 1);
    emptyService.serviceName = ndn::Name();
    BOOST_CHECK(!store.persist({emptyService}));
    auto emptyWire = makeRecord("/HELLO", 1, 1);
    emptyWire.statusDataWire = ndn::Buffer();
    BOOST_CHECK(!store.persist({emptyWire}));
    BOOST_CHECK(!std::filesystem::exists(path));
  }
  {
    // A path whose parent cannot become a directory reports failure instead
    // of throwing (e.g. the parent is a regular file).
    const auto blocked = std::filesystem::temp_directory_path() /
                         ("ndnsf-runtime-status-blocked-" +
                          std::to_string(::getpid()) + ".rts");
    writeRaw(blocked, "occupied");
    const auto nested = blocked / "nested.rts";
    RuntimeStatusStore store(nested);
    BOOST_CHECK(!store.persist({makeRecord("/HELLO", 1, 1)}));
    std::filesystem::remove(blocked);
  }
  std::filesystem::remove(path);
}

BOOST_AUTO_TEST_CASE(EnabledGateRequiresTruthyOptIn)
{
  const char* saved = std::getenv("NDNSF_PERSIST_RUNTIME_STATE");
  ::unsetenv("NDNSF_PERSIST_RUNTIME_STATE");
  BOOST_CHECK(!RuntimeStatusStore::enabled());
  ::setenv("NDNSF_PERSIST_RUNTIME_STATE", "1", 1);
  BOOST_CHECK(RuntimeStatusStore::enabled());
  ::setenv("NDNSF_PERSIST_RUNTIME_STATE", "false", 1);
  BOOST_CHECK(!RuntimeStatusStore::enabled());
  if (saved != nullptr)
    ::setenv("NDNSF_PERSIST_RUNTIME_STATE", saved, 1);
  else
    ::unsetenv("NDNSF_PERSIST_RUNTIME_STATE");
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
