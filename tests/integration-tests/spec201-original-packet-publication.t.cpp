#include "isolated-test-trust.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"
#include "ndn-service-framework/ProtectedBlobManifest.hpp"
#include "ndn-service-framework/ProducerDataNaming.hpp"
#include "ndnsf-distributed-repo/RepoEncryptedLargeDataStore.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/io.hpp>
#include <ndn-svs/security-options.hpp>
#include <ndn-svs/svspubsub.hpp>
#include <algorithm>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <future>
#include <iomanip>
#include <openssl/sha.h>
#include <sstream>
#include <sys/wait.h>
#include <boost/asio/executor_work_guard.hpp>
#include <thread>
#include <unistd.h>

namespace {
using namespace ndn_service_framework;
using namespace ndn_service_framework::test;
namespace fs = std::filesystem;

std::string keyReferenceDigest(const std::string& text)
{
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(text.data()), text.size(), digest);
  std::ostringstream output;
  output << "sha256:" << std::hex << std::setfill('0');
  for (const auto value : digest) output << std::setw(2) << static_cast<unsigned>(value);
  return output.str();
}

PublishedContentName materialContent(const std::string& label)
{
  PublishedContentName value;
  value.kind = ContentDataKind::Material;
  value.label = label;
  return value;
}

ndn::Name materialManifestSuffix(const std::string& label)
{
  return ndn::Name("/NDNSF-DI/material")
    .append(makeNamingLabelComponent(label)).append("MANIFEST");
}

std::string packetKey(const ndn::Name& name)
{
  const auto& wire = name.wireEncode();
  ndn::util::Sha256 hash;
  hash.update(ndn::span<const std::uint8_t>(wire.data(), wire.size()));
  return hash.toString();
}
std::vector<std::uint8_t> readPacket(const fs::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("fixture packet missing");
  const auto size = fs::file_size(path);
  if (size == 0 || size > ndn::MAX_NDN_PACKET_SIZE) throw std::runtime_error("fixture packet size");
  std::vector<std::uint8_t> wire(size);
  input.read(reinterpret_cast<char*>(wire.data()), wire.size());
  if (!input) throw std::runtime_error("fixture packet short read");
  return wire;
}
class PacketSource final : public EncryptedLargeDataRangeSource {
public:
  PacketSource(fs::path folder, std::uint64_t size) : m_folder(std::move(folder)), m_size(size) {}
  std::uint64_t size() const noexcept override { return m_size; }
  bool isDurable() const noexcept override { return true; }
  std::vector<std::uint8_t> read(std::uint64_t, std::uint64_t) const override
  { throw std::runtime_error("range read forbidden in packet fixture"); }
  std::vector<std::uint8_t> readOriginalDataPacket(const ndn::Name& name) const override
  {
    auto wire = readPacket(m_folder / packetKey(name));
    ndn::Data data(ndn::Block(ndn::span<const std::uint8_t>(wire.data(), wire.size())));
    if (data.getName() != name) throw std::runtime_error("fixture packet wrong name");
    return wire;
  }
private:
  fs::path m_folder;
  std::uint64_t m_size;
};

class RangeOnlyDurableStore final : public EncryptedLargeDataRangeStore {
public:
  bool supportsDurableRetention() const noexcept override { return true; }
  std::shared_ptr<const EncryptedLargeDataRangeSource> commitFile(
    const std::string&, const fs::path&, std::uint64_t, const std::function<void()>&) override
  { ++commits; throw std::runtime_error("range-only commit must not run"); }
  unsigned commits = 0;
};

// Native Core fixture only. Real Repo adapter/network serving is T003.
class PacketStore final : public EncryptedLargeDataRangeStore {
public:
  explicit PacketStore(fs::path root) : m_root(std::move(root)) { fs::create_directories(m_root); }
  bool supportsDurableRetention() const noexcept override { return true; }
  bool supportsOriginalDataPackets() const noexcept override { return true; }
  std::shared_ptr<const EncryptedLargeDataRangeSource> commitFile(
    const std::string&, const fs::path&, std::uint64_t, const std::function<void()>&) override
  { throw std::runtime_error("legacy commit forbidden in packet fixture"); }
  std::shared_ptr<const EncryptedLargeDataRangeSource> commitOriginalDataPackets(
    const ndn::Name& rootName, const EncryptedLargeDataCommitOptions& options,
    const std::function<std::optional<std::pair<ndn::Name, std::vector<std::uint8_t>>>()>& next,
    const std::function<void()>& current) override
  {
    ++commits;
    lastAttemptIdentity = options.publicationIdentity;
    if (options.packetStorageFormat != "original-packet-set") throw std::runtime_error("fixture format");
    const auto destination = m_root / options.publicationIdentity.substr(7);
    const auto temporary = m_root / ("pending-" + std::to_string(::getpid()) + "-" + std::to_string(commits));
    fs::create_directory(temporary);
    try {
      std::optional<ProtectedBlobManifest> manifest;
      std::uint64_t packets = 0, bytes = 0;
      while (const auto item = next()) {
        if (current) current();
        ndn::Data data(ndn::Block(ndn::span<const std::uint8_t>(item->second.data(), item->second.size())));
        if (data.getName() != item->first) throw std::runtime_error("fixture declared name mismatch");
        if (packets == 0) {
          const auto& content = data.getContent();
          const auto producer = ProtectedBlobManifest::inferProducer(rootName);
          if (!producer) throw std::runtime_error("fixture manifest grammar");
          manifest = ProtectedBlobManifest::decode(
            std::vector<std::uint8_t>(content.value(), content.value() + content.value_size()),
            *producer, rootName);
        }
        else if (!manifest || item->first != ndn::Name(manifest->dataPrefix).appendSegment(packets - 1))
          throw std::runtime_error("fixture packet order");
        std::ofstream output(temporary / packetKey(item->first), std::ios::binary);
        output.write(reinterpret_cast<const char*>(item->second.data()), item->second.size());
        output.close();
        if (!output) throw std::runtime_error("fixture packet write");
        bytes += item->second.size();
        ++packets;
        if (afterPacket) afterPacket(packets);
      }
      if (!manifest || packets != manifest->segmentCount + 1 || bytes != options.packetWireBytes)
        throw std::runtime_error("fixture incomplete packet set");
      std::ofstream index(temporary / "index");
      index << std::quoted(options.publicationIdentity) << ' ' << std::quoted(options.protectionEpoch) << ' '
        << std::quoted(options.keyReferenceId) << ' ' << std::quoted(options.keyReferenceVersion) << ' '
        << std::quoted(options.ciphertextManifestDigest) << ' ' << std::quoted(options.servingLocator) << ' '
        << std::quoted(options.contentDigest) << ' ' << options.plaintextSize << ' '
        << std::quoted(options.manifestName) << ' ' << std::quoted(options.dataPrefix) << ' '
        << bytes << ' ' << manifest->envelopeBytes;
      index.close();
      if (!index) throw std::runtime_error("fixture index write");
      if (current) current();
      if (fs::exists(destination)) throw std::runtime_error("fixture duplicate publication");
      fs::rename(temporary, destination);
      return std::make_shared<PacketSource>(destination, manifest->envelopeBytes);
    }
    catch (...) { fs::remove_all(temporary); throw; }
  }
  std::optional<EncryptedLargeDataLookupResult> lookupDurable(
    const std::string& identity, const std::function<void()>& current) const override
  {
    if (current) current();
    const auto folder = m_root / identity.substr(7);
    std::ifstream index(folder / "index");
    if (!index) return std::nullopt;
    EncryptedLargeDataLookupResult value;
    std::uint64_t envelopeSize = 0;
    index >> std::quoted(value.publicationIdentity) >> std::quoted(value.protectionEpoch)
      >> std::quoted(value.keyReferenceId) >> std::quoted(value.keyReferenceVersion)
      >> std::quoted(value.ciphertextManifestDigest) >> std::quoted(value.servingLocator)
      >> std::quoted(value.contentDigest) >> value.plaintextSize >> std::quoted(value.manifestName)
      >> std::quoted(value.dataPrefix) >> value.packetWireBytes >> envelopeSize;
    if (!index) throw std::runtime_error("fixture malformed index");
    value.encryptedName = value.dataPrefix;
    value.packetStorageFormat = "original-packet-set";
    value.source = std::make_shared<PacketSource>(folder, envelopeSize);
    return value;
  }
  unsigned commits = 0;
  std::function<void(std::uint64_t)> afterPacket;
  std::string lastAttemptIdentity;
private:
  fs::path m_root;
};

std::shared_ptr<ndn::svs::SVSPubSub> pubsub(ndn::Face& face, ndn::KeyChain& keys)
{
  ndn::svs::SecurityOptions security(keys);
  security.interestSigner = std::make_shared<ndn::svs::BaseSigner>();
  security.dataSigner->signingInfo = ndn::security::signingWithSha256();
  security.pubSigner->signingInfo = ndn::security::signingWithSha256();
  security.validator = std::make_shared<ndn::svs::BaseValidator>();
  security.encapsulatedDataValidator = std::make_shared<ndn::svs::BaseValidator>();
  ndn::svs::SVSPubSubOptions options;
  options.useTimestamp = false;
  return std::make_shared<ndn::svs::SVSPubSub>(ndn::Name("/spec201/raw/sync"),
    ndn::Name("/spec201/raw/user"), face, [](const auto&) {}, options, security);
}
} // namespace

BOOST_AUTO_TEST_SUITE(Spec201OriginalPacketPublication)
BOOST_AUTO_TEST_CASE(WorkerPublicationMeasuresOwnerDispatchAndPreservesOriginalPackets)
{
  const auto root = fs::temp_directory_path() /
    ("spec201-worker-measure-" + std::to_string(::getpid()));
  fs::create_directories(root);
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code e; fs::remove_all(path, e); } } cleanup{root};
  const auto spool = (root / "spool").string();
  const auto references = (root / "references").string();
  ScopedEnvironmentValue dataDir("NDNSF_REQUEST_LARGE_DATA_DIR", spool.c_str());
  ScopedEnvironmentValue keyDir("NDNSF_DURABLE_KEY_REFERENCE_DIR", references.c_str());
  for (const bool durable : {false, true}) {
  const bool fileKeys = std::getenv("SPEC201_WORKER_FILE_KEYS") != nullptr;
  const auto keyRoot = root / (durable ? "durable-keys" : "transient-keys");
  ndn::KeyChain keys(fileKeys ? "pib-sqlite3:" + (keyRoot / "pib").string() : "pib-memory:",
                    fileKeys ? "tpm-file:" + (keyRoot / "tpm").string() : "tpm-memory:");
  ndn::DummyClientFace face(keys);
  const auto actor = makeRsaIdentity(keys, ndn::Name("/spec201/worker-owner"));
  const auto authority = makeRsaIdentity(keys, ndn::Name("/spec201/worker-authority"));
  const ndn::Name service("/spec201/model");
  ndn_service_framework::test::IsolatedTestTrust fixedTrust1{
    actor, authority};
  LocalServiceUser user(face, ndn::Name("/spec201/raw"), actor, authority,
                        fixedTrust1.schemaPath());
  user.useSigningKeyChainForSigningOnlyForTest(keys);
  user.attachLocalMockPubSubForTest(pubsub(face, keys));
  user.init();
  face.processEvents(ndn::time::milliseconds(1));
  user.prepareHybridSendKeyForTest(service, "REQUEST-LARGE");
  const std::vector<std::uint8_t> input(7000000, 0x42);
  std::shared_ptr<ndnsf_distributed_repo::RepoEncryptedLargeDataStore> durableStore;
   if (durable) {
    ndnsf_distributed_repo::StorageCapability capability;
    capability.repoNode = "/spec201/worker-repo";
    capability.freeBytes = 256U << 20;
    auto repo = std::make_shared<ndnsf_distributed_repo::RepoCore>(capability,
      ndnsf_distributed_repo::makeFilesystemRepoStore((root / "repo").string(), 1U << 20, 1U << 20));
    durableStore = std::make_shared<ndnsf_distributed_repo::RepoEncryptedLargeDataStore>(repo);
    user.setEncryptedLargeDataRangeStore(durableStore);
   }
  for (const bool worker : {false, true}) {
    const auto request = user.prepareServiceRequest(service.toUri());
    LargeDataPublishOptions options;
    PublishedContentName contentName;
    contentName.kind = ContentDataKind::Blob;
    contentName.label = worker ? "worker" : "inline";
    options.contentName = contentName;
    if (durable) {
      options.retention = EncryptedLargeDataRetention::Durable;
      options.publicationIdentity = keyReferenceDigest(worker ? "worker" : "inline");
    }
    LargeDataPublishResult result;
    const auto started = std::chrono::steady_clock::now();
    if (worker) {
      auto& io = face.getIoContext();
      io.restart();
      auto guard = boost::asio::make_work_guard(io);
      std::thread thread([&io] { io.run(); });
      struct Join {
        boost::asio::io_context& io; std::thread& thread;
        ~Join() { io.stop(); if (thread.joinable()) thread.join(); }
      } join{io, thread};
      result = user.publishEncryptedLargeDataFromWorker(
        request, input, "measurement", ndn::time::milliseconds(1000), options);
    }
    else {
      result = user.publishEncryptedLargeData(request, input, "measurement",
        ndn::time::milliseconds(1000), true, options);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started).count();
    BOOST_REQUIRE_MESSAGE(result.success, result.errorMessage);
    BOOST_REQUIRE(result.fileBacked);
    std::cout << "SPEC201_CORE_WORKER_MEASUREMENT worker=" << worker
              << " fileKeys=" << fileKeys
              << " durable=" << durable
              << " plaintextBytes=" << input.size() << " elapsedUs=" << elapsed << '\n';
    face.getIoContext().restart();
    const auto before = face.sentData.size();
    face.receive(ndn::Interest(result.manifestName));
    face.processEvents(ndn::time::milliseconds(1));
    BOOST_REQUIRE_EQUAL(face.sentData.size(), before + 1);
    const auto& manifestPacket = face.sentData.back();
    BOOST_CHECK(manifestPacket.getName() == result.manifestName);
    BOOST_CHECK(ndn::security::verifySignature(manifestPacket, actor));
    const auto& content = manifestPacket.getContent();
    const auto manifest = ProtectedBlobManifest::decode(
      std::vector<std::uint8_t>(content.value(), content.value() + content.value_size()),
      actor.getIdentity(), result.manifestName);
    BOOST_REQUIRE_GE(manifest.segmentCount, 1000U);
    const auto segmentName = ndn::Name(manifest.dataPrefix).appendSegment(manifest.segmentCount - 1);
    const auto beforeSegment = face.sentData.size();
    face.receive(ndn::Interest(segmentName));
    face.processEvents(ndn::time::milliseconds(1));
    BOOST_REQUIRE_EQUAL(face.sentData.size(), beforeSegment + 1);
    const auto packet = face.sentData.back();
    BOOST_CHECK(packet.getName() == segmentName);
    BOOST_CHECK(ndn::security::verifySignature(packet, actor));
    const auto firstWire = packet.wireEncode();
    const auto beforeRetry = face.sentData.size();
    face.receive(ndn::Interest(segmentName));
    face.processEvents(ndn::time::milliseconds(1));
    BOOST_REQUIRE_EQUAL(face.sentData.size(), beforeRetry + 1);
    BOOST_CHECK(face.sentData.back().wireEncode() == firstWire);
  }
  if (durableStore) {
    const auto measureLookup = [&](unsigned objects) {
      const auto started = std::chrono::steady_clock::now();
      for (unsigned i = 0; i < 10; ++i)
        BOOST_REQUIRE(!durableStore->lookupDurable("sha256:" + std::string(64, 'f')));
      const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
      std::cout << "SPEC201_CORE_CATALOG_MEASUREMENT objects=" << objects
                << " misses=10 elapsedUs=" << elapsed << '\n';
    };
    measureLookup(2);
    std::vector<std::uint8_t> seed(1U << 20, 0x44);
    std::string firstPublicationIdentity;
    // Keep one Core/Repo instance past the observed 122-object catalog failure.
    for (unsigned i = 0; i < 160; ++i) {
      seed.front() = static_cast<std::uint8_t>(i);
      LargeDataPublishOptions options;
      options.retention = EncryptedLargeDataRetention::Durable;
      options.publicationIdentity = keyReferenceDigest("catalog-" + std::to_string(i));
      options.contentName = materialContent("catalog-" + std::to_string(i));
      options.manifestSuffix = materialManifestSuffix("catalog-" + std::to_string(i));
      const auto result = user.publishEncryptedLargeData(
        user.prepareServiceRequest(service.toUri()), seed, "catalog",
        ndn::time::milliseconds(1000), true, options);
      BOOST_REQUIRE_MESSAGE(result.success, result.errorMessage);
      if (i == 0) {
        firstPublicationIdentity = result.publicationIdentity;
        BOOST_REQUIRE(!firstPublicationIdentity.empty());
      }
      if (i == 7 || i == 15 || i == 121 || i == 159) {
        measureLookup(i + 3);
        BOOST_REQUIRE(durableStore->lookupDurable(firstPublicationIdentity));
      }
    }
  }
  }
}
BOOST_AUTO_TEST_CASE(StrictReaderRejectsLegacyAndMalformedNamesBeforeInterest)
{
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  ndn::DummyClientFace face(keys);
  const auto providerCertificate = makeRsaIdentity(keys, ndn::Name("/spec201/strict-provider"));
  const auto authority = makeRsaIdentity(keys, ndn::Name("/spec201/strict-authority"));
  ndn_service_framework::test::IsolatedTestTrust fixedTrust2{
    providerCertificate, authority};
  LocalServiceProvider provider(face, ndn::Name("/spec201/strict"), providerCertificate,
                                authority, fixedTrust2.schemaPath());
  face.processEvents(ndn::time::milliseconds(1));
  const auto before = face.sentInterests.size();
  const std::vector<ndn::Name> rejected{
    ndn::Name("/producer/NDNSF/LARGE-DATA/object"),
    ndn::Name("/producer/NDNSF/BLOB/not-a-digest/PROTECTED/not-a-digest/DATA"),
    ndn::Name("/producer/NDNSF/MATERIAL/MANIFEST/not-a-digest"),
    ndn::Name("/unknown/object")};
  for (const auto& name : rejected) {
    const auto result = provider.fetchAndDecryptLargeData(name, "/spec201/model");
    BOOST_CHECK(!result.success);
    BOOST_CHECK_EQUAL(result.errorMessage, "NONCANONICAL_LARGE_DATA_NAME");
  }
  const auto invalid = provider.fetchAndDecryptLargeData(rejected.front(), "/spec201/model",
                                                        static_cast<DataNameReadMode>(99));
  BOOST_CHECK_EQUAL(invalid.errorMessage, "INVALID_LARGE_DATA_READ_MODE");
  const auto unsupported = provider.fetchAndDecryptLargeData(rejected.back(), "/spec201/model",
                                                            DataNameReadMode::AllowLegacyRead);
  BOOST_CHECK_EQUAL(unsupported.errorMessage, "NONCANONICAL_LARGE_DATA_NAME");
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK_EQUAL(face.sentInterests.size(), before);
}

BOOST_AUTO_TEST_CASE(ExplicitLegacyDecryptsButCanonicalFailureNeverFallsBack)
{
  ScopedEnvironmentValue timeout("NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS", "2000");
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  ndn::DummyClientFace face(keys);
  const auto producer = makeRsaIdentity(keys, ndn::Name("/spec201/legacy-producer"));
  const auto receiver = makeRsaIdentity(keys, ndn::Name("/spec201/legacy-receiver"));
  const auto authority = makeRsaIdentity(keys, ndn::Name("/spec201/legacy-authority"));
  ndn_service_framework::test::IsolatedTestTrust fixedTrust3{
    producer, receiver, authority};
  LocalServiceProvider provider(face, ndn::Name("/spec201/legacy"), receiver, authority,
                                fixedTrust3.schemaPath());
  provider.useSigningKeyChainForTest(keys);
  const ndn::Buffer key(32, 0x11), input{1, 2, 3};
  provider.cacheHybridReceiveKeyForTest("legacy-key", "1", key);
  const ndn::Name service("/spec201/legacy-service");
  auto prefix = ndn::Name(producer.getIdentity()).append("NDNSF").append("LARGE-DATA").append("object");
  const auto aad = prefix.toUri() + "|REQUEST-LARGE|" + service.toUri();
  const auto encrypted = hybridAesGcmEncrypt(key,
    ndn::span<const std::uint8_t>(input.data(), input.size()),
    ndn::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(aad.data()), aad.size()));
  HybridMessageEnvelope envelope;
  envelope.setKeyId("legacy-key"); envelope.setEpochId("1"); envelope.setMessageType("REQUEST-LARGE");
  envelope.setNonce(encrypted.nonce); envelope.setCipherText(encrypted.ciphertext); envelope.setAuthTag(encrypted.tag);
  ndn::Data packet(ndn::Name(prefix).appendSegment(0));
  packet.setContent(envelope.WireEncode());
  packet.setFreshnessPeriod(ndn::time::milliseconds(1000));
  packet.setFinalBlock(ndn::name::Component::fromSegment(0));
  keys.sign(packet, ndn::security::signingByIdentity(producer.getIdentity()));
  ndn::signal::ScopedConnection connection = face.onSendInterest.connect([&](const ndn::Interest& interest) {
    if (interest.getName() == prefix || interest.getName() == packet.getName()) face.receive(packet);
  });
  auto fetch = [&](DataNameReadMode mode) {
    auto pending = std::async(std::launch::async, [&] {
      return provider.fetchAndDecryptLargeData(prefix, service.toUri(), mode);
    });
    while (pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
      face.processEvents(ndn::time::milliseconds(1));
    return pending.get();
  };
  face.processEvents(ndn::time::milliseconds(1));
  const auto beforeStrict = face.sentInterests.size();
  BOOST_CHECK(!fetch(DataNameReadMode::StrictCanonical).success);
  BOOST_CHECK_EQUAL(face.sentInterests.size(), beforeStrict);
  const auto legacy = fetch(DataNameReadMode::AllowLegacyRead);
  BOOST_REQUIRE_MESSAGE(legacy.success, legacy.errorMessage);
  BOOST_CHECK_EQUAL_COLLECTIONS(legacy.plaintext.begin(), legacy.plaintext.end(), input.begin(), input.end());
  const auto hash = ndn::name::Component(std::string(32, 'h'));
  prefix = ndn::Name(producer.getIdentity()).append("NDNSF").append("BLOB").append(hash)
    .append("PROTECTED").append(hash).append("DATA");
  packet.setName(ndn::Name(prefix).appendSegment(0));
  packet.setContent("invalid-envelope");
  keys.sign(packet, ndn::security::signingByIdentity(producer.getIdentity()));
  const auto before = face.sentInterests.size();
  const auto canonical = fetch(DataNameReadMode::AllowLegacyRead);
  BOOST_CHECK(!canonical.success);
  BOOST_CHECK_EQUAL(canonical.errorMessage, "canonical body envelope invalid");
  BOOST_CHECK_EQUAL(face.sentInterests.size(), before + 1);
}

void runOriginalSignedPacketExecRestart(bool legacyV2, bool wrongManifest = false)
{
  const char* mode = std::getenv("NDNSF_SPEC201_EXEC_CHILD");
  const bool child = mode && std::string(mode) == "1";
  const char* inheritedRoot = std::getenv("NDNSF_SPEC201_EXEC_ROOT");
  BOOST_REQUIRE(!child || (inheritedRoot && *inheritedRoot));
  const auto root = child ? fs::path(inheritedRoot) :
    fs::temp_directory_path() / ("spec201-exec-" + std::to_string(::getpid()));
  fs::create_directories(root);
  struct Cleanup {
    fs::path path; bool owns;
    ~Cleanup() { if (owns) { std::error_code e; fs::remove_all(path, e); } }
  } cleanup{root, !child};
  const auto spool = (root / "spool").string();
  const auto references = (root / "references").string();
  ScopedEnvironmentValue dataDir("NDNSF_REQUEST_LARGE_DATA_DIR", spool.c_str());
  ScopedEnvironmentValue keyDir("NDNSF_DURABLE_KEY_REFERENCE_DIR", references.c_str());
  // All Core owners are scoped inside this call and destroyed before fork.
  const auto exercise = [&] {
    auto store = std::make_shared<PacketStore>(root / "store");
    ndn::KeyChain keys("pib-memory:", "tpm-memory:");
    ndn::DummyClientFace face(keys);
    const auto actor = makeRsaIdentity(keys, ndn::Name("/spec201/NDNSF/exec-owner"));
    const auto authority = makeRsaIdentity(keys, ndn::Name("/spec201/exec-authority"));
    ndn_service_framework::test::IsolatedTestTrust fixedTrust4{
      actor, authority};
    LocalServiceUser user(face, ndn::Name("/spec201/raw"), actor, authority, fixedTrust4.schemaPath());
    user.useSigningKeyChainForSigningOnlyForTest(keys);
    user.attachLocalMockPubSubForTest(pubsub(face, keys));
    user.setEncryptedLargeDataRangeStore(store);
    user.init();
    face.processEvents(ndn::time::milliseconds(1));
    const ndn::Name service("/spec201/exec-model");
    if (legacyV2) {
      const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
      PolicyStatusData status;
      status.setServiceName(service);
      status.setControllerVersion(ControllerVersion{2010, 7});
      status.setValidity(now - 1000, now + 60000);
      status.setPolicyDigest("sha256:" + std::string(64, '0'));
      status.setControllerCertificate(ndn::Name("/controller/spec201"));
      BOOST_REQUIRE(user.installControllerStatus(status));
    }
    user.prepareHybridSendKeyForTest(service, "REQUEST-LARGE");
    LargeDataPublishOptions options;
    options.retention = EncryptedLargeDataRetention::Durable;
    options.publicationIdentity = "sha256:" + std::string(64, 'e');
    options.contentName = materialContent("assembly-receipt");
    options.manifestSuffix = ndn::Name("/NDNSF-DI/receipt/assembly")
      .append(makeNamingLabelComponent(options.contentName->label));
    const auto publication = user.publishEncryptedLargeData(user.prepareServiceRequest(service.toUri()),
      std::vector<std::uint8_t>(40000, 0x61), "exec", ndn::time::milliseconds(1), true, options);
    if (child && wrongManifest) {
      BOOST_CHECK(!publication.success);
      BOOST_CHECK(publication.errorMessage.find("DURABLE_REFERENCE_MANIFEST_MISMATCH") != std::string::npos);
      BOOST_CHECK_EQUAL(store->commits, 0U);
      BOOST_CHECK_EQUAL(user.getLargeDataServingMetricsForTest().publicationCount, 0U);
      const auto oracle = readPacket(root / "oracle-root");
      const ndn::Data original(ndn::Block(ndn::span<const std::uint8_t>(oracle.data(), oracle.size())));
      const auto count = face.sentData.size();
      face.receive(ndn::Interest(original.getName()));
      face.processEvents(ndn::time::milliseconds(1));
      BOOST_CHECK_EQUAL(face.sentData.size(), count);
      return;
    }
    BOOST_REQUIRE_MESSAGE(publication.success, publication.errorMessage);
    BOOST_CHECK_EQUAL(store->commits, child ? 0U : 1U);
    for (const auto& item : std::vector<std::pair<ndn::Name, std::string>>{
           {publication.manifestName, "oracle-root"},
           {ndn::Name(publication.dataPrefix).appendSegment(0), "oracle-segment"}}) {
      const auto count = face.sentData.size();
      face.receive(ndn::Interest(item.first));
      face.processEvents(ndn::time::milliseconds(1));
      BOOST_REQUIRE_EQUAL(face.sentData.size(), count + 1);
      const auto& block = face.sentData.back().wireEncode();
      const std::vector<std::uint8_t> wire(block.begin(), block.end());
      if (child) BOOST_CHECK(wire == readPacket(root / item.second));
      else {
        std::ofstream oracle(root / item.second, std::ios::binary);
        oracle.write(reinterpret_cast<const char*>(wire.data()), wire.size());
        oracle.close();
        BOOST_REQUIRE(oracle.good());
      }
    }
  };
  exercise();
  if (child) return;
  // Rewrite only URI spelling of the persisted service and locator fields.
  // Its complete Name wire and all crypto/packet metadata remain unchanged.
  unsigned referencesChanged = 0;
  for (const auto& entry : fs::directory_iterator(root / "references")) {
    if (entry.path().filename().string().find(".v3.dkr") == std::string::npos ||
        entry.path().extension() != ".dkr") continue;
    BOOST_CHECK_EQUAL(entry.path().filename().string().size(), 76U);
    std::ifstream input(entry.path(), std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    std::size_t offset = sizeof("NDNSF-DURABLE-LARGE-KEYREF-V2") - 1;
    constexpr char currentMagic[] = "NDNSF-DURABLE-LARGE-KEYREF-V3";
    BOOST_REQUIRE(bytes.size() >= sizeof(currentMagic) - 1);
    BOOST_REQUIRE(std::equal(currentMagic, currentMagic + sizeof(currentMagic) - 1, bytes.begin()));
    const auto integer = [&] {
      BOOST_REQUIRE(offset + sizeof(std::uint64_t) <= bytes.size());
      std::uint64_t value = 0;
      std::memcpy(&value, bytes.data() + offset, sizeof(value));
      offset += sizeof(value);
      return value;
    };
    BOOST_REQUIRE_EQUAL(integer(), 1U);
    const auto publicationBytes = integer();
    BOOST_REQUIRE(publicationBytes <= bytes.size() - offset);
    offset += publicationBytes;
    const auto fieldStart = offset;
    const auto serviceBytes = integer();
    BOOST_REQUIRE(serviceBytes <= bytes.size() - offset);
    const std::string original(bytes.begin() + offset, bytes.begin() + offset + serviceBytes);
    const ndn::Name service(original);
    const auto canonical = service.toUri(ndn::name::UriFormat::CANONICAL);
    const auto replacement = original == canonical ? service.toUri(ndn::name::UriFormat::ALTERNATE) : canonical;
    BOOST_REQUIRE(original != replacement);
    BOOST_REQUIRE(ndn::Name(replacement) == service);
    const std::uint64_t size = replacement.size();
    std::vector<std::uint8_t> rewritten(bytes.begin(), bytes.begin() + fieldStart);
    const auto* length = reinterpret_cast<const std::uint8_t*>(&size);
    rewritten.insert(rewritten.end(), length, length + sizeof(size));
    rewritten.insert(rewritten.end(), replacement.begin(), replacement.end());
    const auto serviceEnd = offset + serviceBytes;
    offset = serviceEnd;
    const auto skipString = [&] {
      const auto count = integer();
      BOOST_REQUIRE(count <= bytes.size() - offset);
      offset += count;
    };
    skipString(); // encryptedDataName
    skipString(); // contentDigest
    BOOST_REQUIRE(offset + 3 * sizeof(std::uint64_t) <= bytes.size());
    offset += 3 * sizeof(std::uint64_t); // size and Controller version
    for (unsigned field = 0; field < 4; ++field) skipString();
    const auto locatorStart = offset;
    const auto locatorBytes = integer();
    BOOST_REQUIRE(locatorBytes <= bytes.size() - offset);
    const auto extensionStart = offset + locatorBytes;
    const std::string locator(bytes.begin() + offset, bytes.begin() + extensionStart);
    const ndn::Name locatorName(locator);
    const auto canonicalLocator = locatorName.toUri(ndn::name::UriFormat::CANONICAL);
    const auto newLocator = locator == canonicalLocator ?
      locatorName.toUri(ndn::name::UriFormat::ALTERNATE) : canonicalLocator;
    BOOST_REQUIRE(locator != newLocator);
    BOOST_REQUIRE(ndn::Name(newLocator) == locatorName);
    rewritten.insert(rewritten.end(), bytes.begin() + serviceEnd, bytes.begin() + locatorStart);
    const std::uint64_t locatorSize = newLocator.size();
    const auto* locatorLength = reinterpret_cast<const std::uint8_t*>(&locatorSize);
    rewritten.insert(rewritten.end(), locatorLength, locatorLength + sizeof(locatorSize));
    rewritten.insert(rewritten.end(), newLocator.begin(), newLocator.end());
    if (legacyV2) {
      constexpr char oldMagic[] = "NDNSF-DURABLE-LARGE-KEYREF-V2";
      std::copy(oldMagic, oldMagic + sizeof(oldMagic) - 1, rewritten.begin());
    }
    else if (wrongManifest) {
      offset = extensionStart;
      const auto nameBytes = integer();
      BOOST_REQUIRE(nameBytes <= bytes.size() - offset);
      const std::string original(bytes.begin() + offset, bytes.begin() + offset + nameBytes);
      ndn::Name wrong(original);
      const auto& digest = wrong.get(-1);
      std::vector<std::uint8_t> value(digest.value(), digest.value() + digest.value_size());
      BOOST_REQUIRE_EQUAL(value.size(), 32U);
      value.front() ^= 1;
      wrong.set(-1, ndn::name::Component(ndn::span<const std::uint8_t>(value.data(), value.size())));
      const auto replacement = wrong.toUri(ndn::name::UriFormat::CANONICAL);
      const std::uint64_t nameSize = replacement.size();
      const auto* length = reinterpret_cast<const std::uint8_t*>(&nameSize);
      rewritten.insert(rewritten.end(), length, length + sizeof(nameSize));
      rewritten.insert(rewritten.end(), replacement.begin(), replacement.end());
      rewritten.insert(rewritten.end(), bytes.begin() + offset + nameBytes, bytes.end());
    }
    else rewritten.insert(rewritten.end(), bytes.begin() + extensionStart, bytes.end());
    std::ofstream output(entry.path(), std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(rewritten.data()), rewritten.size());
    output.close();
    BOOST_REQUIRE(output.good());
    if (legacyV2) {
      // Independent implementation of the historical URI-escaped location.
      const auto uri = ndn::Name("/spec201/NDNSF/exec-owner").toUri(ndn::name::UriFormat::ALTERNATE);
      std::string escaped;
      constexpr char hex[] = "0123456789abcdef";
      for (const unsigned char value : uri) {
        if (std::isalnum(value) || value == '-' || value == '_' || value == '.') escaped += value;
        else { escaped += '_'; escaped += hex[value >> 4]; escaped += hex[value & 15]; }
      }
      fs::rename(entry.path(), entry.path().parent_path() / ("user-" + escaped + ".v2.dkr"));
    }
    ++referencesChanged;
  }
  BOOST_REQUIRE_EQUAL(referencesChanged, 1U);
  const auto pid = ::fork();
  BOOST_REQUIRE(pid >= 0);
  if (pid == 0) {
    const char* parentUri = std::getenv("NDN_NAME_ALT_URI");
    const char* childUri = parentUri && std::strcmp(parentUri, "0") == 0 ? "1" : "0";
    if (::setenv("NDNSF_SPEC201_EXEC_CHILD", "1", 1) != 0 ||
        ::setenv("NDNSF_SPEC201_EXEC_ROOT", root.c_str(), 1) != 0 ||
        ::setenv("NDN_NAME_ALT_URI", childUri, 1) != 0) ::_exit(126);
    ::execl("/proc/self/exe", "spec201-original-packet-publication",
      wrongManifest ? "--run_test=Spec201OriginalPacketPublication/WrongManifestReferenceFailsBeforeServing" :
      legacyV2 ? "--run_test=Spec201OriginalPacketPublication/LegacyV2ReferenceUpgradesAfterExec" :
        "--run_test=Spec201OriginalPacketPublication/OriginalSignedPacketsSurviveRealExecRestart",
      "--log_level=test_suite", static_cast<char*>(nullptr));
    ::_exit(127);
  }
  int status = 0;
  BOOST_REQUIRE_EQUAL(::waitpid(pid, &status, 0), pid);
  BOOST_REQUIRE(WIFEXITED(status));
  BOOST_CHECK_EQUAL(WEXITSTATUS(status), 0);
  if (wrongManifest) return;
  unsigned currentRecords = 0, oldRecords = 0;
  for (const auto& entry : fs::directory_iterator(root / "references")) {
    if (entry.path().extension() != ".dkr") continue;
    if (entry.path().filename().string().find(".v2.dkr") != std::string::npos) {
      ++oldRecords;
      std::ifstream old(entry.path(), std::ios::binary);
      std::string magic(sizeof("NDNSF-DURABLE-LARGE-KEYREF-V2") - 1, '\0');
      old.read(magic.data(), magic.size());
      BOOST_CHECK_EQUAL(magic, "NDNSF-DURABLE-LARGE-KEYREF-V2");
      continue;
    }
    ++currentRecords;
    BOOST_CHECK_EQUAL(entry.path().filename().string().size(), 76U);
    std::ifstream record(entry.path(), std::ios::binary);
    std::string magic(sizeof("NDNSF-DURABLE-LARGE-KEYREF-V3") - 1, '\0');
    record.read(magic.data(), magic.size());
    BOOST_REQUIRE_EQUAL(magic, "NDNSF-DURABLE-LARGE-KEYREF-V3");
    const auto number = [&] {
      std::uint64_t value = 0;
      record.read(reinterpret_cast<char*>(&value), sizeof(value));
      BOOST_REQUIRE(record.good());
      return value;
    };
    const auto text = [&] {
      const auto size = number();
      BOOST_REQUIRE(size <= (1U << 20));
      std::string value(size, '\0');
      record.read(value.data(), value.size());
      BOOST_REQUIRE(record.good());
      return value;
    };
    BOOST_REQUIRE_EQUAL(number(), 1U);
    for (unsigned field = 0; field < 4; ++field) text();
    BOOST_CHECK_EQUAL(number(), 40000U);
    BOOST_CHECK_EQUAL(number(), legacyV2 ? 7U : 0U);
    BOOST_CHECK_EQUAL(number(), legacyV2 ? 2010U : 0U);
    for (unsigned field = 0; field < 5; ++field) text();
    const auto manifestName = text();
    BOOST_CHECK_EQUAL(text(), "original-packet-set");
    const auto oracle = readPacket(root / "oracle-root");
    const ndn::Data original(ndn::Block(ndn::span<const std::uint8_t>(oracle.data(), oracle.size())));
    BOOST_CHECK(ndn::Name(manifestName) == original.getName());
    BOOST_CHECK_EQUAL(record.peek(), std::char_traits<char>::eof());
  }
  BOOST_CHECK_EQUAL(currentRecords, 1U);
  BOOST_CHECK_EQUAL(oldRecords, legacyV2 ? 1U : 0U);
}
BOOST_AUTO_TEST_CASE(OriginalSignedPacketsSurviveRealExecRestart)
{ runOriginalSignedPacketExecRestart(false); }
BOOST_AUTO_TEST_CASE(LegacyV2ReferenceUpgradesAfterExec)
{ runOriginalSignedPacketExecRestart(true); }
BOOST_AUTO_TEST_CASE(WrongManifestReferenceFailsBeforeServing)
{ runOriginalSignedPacketExecRestart(false, true); }
BOOST_AUTO_TEST_CASE(SemanticManifestCategoriesKeepOriginalBlobPackets)
{
  ScopedEnvironmentValue timeout("NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS", "2000");
  ScopedEnvironmentValue fileMode("NDNSF_REQUEST_LARGE_FILE_BACKED", "0");
  const auto root = fs::temp_directory_path() / ("spec201-semantic-" + std::to_string(::getpid()));
  fs::create_directories(root);
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code e; fs::remove_all(path, e); } } cleanup{root};
  const auto spool = (root / "spool").string();
  const auto references = (root / "references").string();
  ScopedEnvironmentValue dataDir("NDNSF_REQUEST_LARGE_DATA_DIR", spool.c_str());
  ScopedEnvironmentValue keyDir("NDNSF_DURABLE_KEY_REFERENCE_DIR", references.c_str());
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  ndn::DummyClientFace face(keys);
  const auto actor = makeRsaIdentity(keys, ndn::Name("/spec201/NDNSF/MODEL/owner"));
  const auto authority = makeRsaIdentity(keys, ndn::Name("/spec201/authority"));
  const ndn::Name service("/spec201/model");
  ndn_service_framework::test::IsolatedTestTrust fixedTrust5{
    actor, authority};
  LocalServiceUser user(face, ndn::Name("/spec201/raw"), actor, authority, fixedTrust5.schemaPath());
  user.useSigningKeyChainForSigningOnlyForTest(keys);
  user.attachLocalMockPubSubForTest(pubsub(face, keys));
  user.init();
  face.processEvents(ndn::time::milliseconds(1));
  const auto sendKey = user.prepareHybridSendKeyForTest(service, "REQUEST-LARGE");
  const auto providerCertificate = makeRsaIdentity(keys, ndn::Name("/spec201/semantic-provider"));
  ndn_service_framework::test::IsolatedTestTrust fixedTrust6{
    actor, authority, providerCertificate};
  LocalServiceProvider provider(face, ndn::Name("/spec201/raw"), providerCertificate, authority,
                                fixedTrust6.schemaPath());
  provider.useSigningKeyChainForTest(keys);
  // Key injection isolates publication/fetch/decrypt from AA bootstrap.
  provider.cacheHybridReceiveKeyForTest(sendKey.keyId, sendKey.epochId, sendKey.key);
  const std::vector<std::uint8_t> plaintext(40000, 0x42);
  PublishedContentName blob;
  blob.kind = ContentDataKind::Blob;
  blob.label = "semantic";
  PublishedContentName model;
  model.kind = ContentDataKind::ModelFile;
  model.label = "model.onnx";
  model.modelName = "fixture-model";
  model.modelIdentity = std::string(64, 'a');
  PublishedContentName assignment;
  assignment.kind = ContentDataKind::Assignment;
  assignment.label = "S0R0";
  assignment.modelName = model.modelName;
  assignment.modelIdentity = model.modelIdentity;
  PublishedContentName applicationInput;
  applicationInput.kind = ContentDataKind::Input;
  applicationInput.label = "image";
  applicationInput.inputKind = "application";
  const auto modelSuffix = ndn::Name("/NDNSF-DI/model")
    .append(makeNamingLabelComponent(model.modelName))
    .append(makeNamingDigestComponent(model.modelIdentity)).append("MANIFEST");
  const std::vector<std::pair<ndn::Name, PublishedContentName>> suffixes{
    {ndn::Name(), blob}, {modelSuffix, model}, {ndn::Name(), assignment}, {ndn::Name(), applicationInput},
    {materialManifestSuffix("material"), materialContent("material")},
    {ndn::Name("/NDNSF-DI/receipt/assembly")
       .append(makeNamingLabelComponent("assembly-receipt")), materialContent("assembly-receipt")}};
  for (const bool fileBacked : {false, true}) {
   for (const auto& item : suffixes) {
    const auto& suffix = item.first;
    LargeDataPublishOptions options;
    options.manifestSuffix = suffix;
    options.contentName = item.second;
    const auto publication = user.publishEncryptedLargeData(user.prepareServiceRequest(service.toUri()),
      plaintext, "semantic", ndn::time::milliseconds(1000), fileBacked, options);
    BOOST_REQUIRE_MESSAGE(publication.success, publication.errorMessage);
    BOOST_CHECK_EQUAL(publication.fileBacked, fileBacked);
    if (suffix.empty()) {
      BOOST_CHECK(publication.manifestName.getPrefix(-2) == publication.dataPrefix.getPrefix(-1));
    }
    else {
      BOOST_CHECK(publication.manifestName.getPrefix(-1) == ndn::Name(actor.getIdentity()).append(suffix));
    }
    // IMS publication queues active-put packets on the Face. Drain those
    // before measuring the response to this exact manifest Interest.
    face.processEvents(ndn::time::milliseconds(1));
    const auto before = face.sentData.size();
    face.receive(ndn::Interest(publication.manifestName));
    face.processEvents(ndn::time::milliseconds(1));
    BOOST_REQUIRE_EQUAL(face.sentData.size(), before + 1);
    const auto packet = face.sentData.back();
    const auto& content = packet.getContent();
    const std::vector<std::uint8_t> payload(content.value(), content.value() + content.value_size());
    const auto descriptor = ProtectedBlobManifest::decode(
      payload, actor.getIdentity(), publication.manifestName);
    BOOST_CHECK_EQUAL(descriptor.keyReferenceId, keyReferenceDigest(
      sendKey.keyId + "|" + sendKey.epochId + "|" +
      service.toUri(ndn::name::UriFormat::CANONICAL) + "|" + descriptor.authorizationScope));
    BOOST_CHECK(descriptor.manifestSuffix == suffix);
    BOOST_CHECK(descriptor.dataPrefix == publication.dataPrefix);
    BOOST_REQUIRE(descriptor.contentName.has_value());
    BOOST_CHECK(descriptor.contentName->encode() == item.second.encode());
    if (suffix.size() == 4 && suffix[1] == ndn::name::Component("receipt")) {
      BOOST_CHECK(suffix[3] == ndn::name::Component("name=assembly-receipt"));
      BOOST_CHECK(ProtectedBlobManifest::inferProducer(publication.manifestName) ==
                  std::optional<ndn::Name>(actor.getIdentity()));
      auto wrongLabel = descriptor;
      wrongLabel.manifestSuffix = ndn::Name("/NDNSF-DI/receipt/assembly")
        .append(ndn::name::Component("name=another-object"));
      BOOST_CHECK_THROW(wrongLabel.encode(), std::invalid_argument);
      auto historical = descriptor;
      historical.manifestSuffix = ndn::Name("/NDNSF-DI/receipt/assembly");
      BOOST_CHECK_THROW(historical.encode(), std::invalid_argument);
      auto oldWriterOptions = options;
      oldWriterOptions.manifestSuffix = historical.manifestSuffix;
      const auto publicationsBefore = user.getLargeDataServingMetricsForTest().publicationCount;
      const auto oldWriter = user.publishEncryptedLargeData(
        user.prepareServiceRequest(service.toUri()), plaintext, "rejected-old-receipt",
        ndn::time::milliseconds(1000), fileBacked, oldWriterOptions);
      BOOST_CHECK(!oldWriter.success);
      BOOST_CHECK(oldWriter.errorMessage.find("UNSUPPORTED_PROTECTED_MANIFEST_SUFFIX") != std::string::npos);
      BOOST_CHECK_EQUAL(user.getLargeDataServingMetricsForTest().publicationCount, publicationsBefore);
      const auto oldName = historical.getName(DataNameReadMode::AllowLegacyRead);
      const auto oldPayload = historical.encode(DataNameReadMode::AllowLegacyRead);
      BOOST_CHECK(!ProtectedBlobManifest::inferProducer(oldName).has_value());
      BOOST_CHECK(ProtectedBlobManifest::inferProducer(oldName, DataNameReadMode::AllowLegacyRead) ==
                  std::optional<ndn::Name>(actor.getIdentity()));
      const auto restored = ProtectedBlobManifest::decode(oldPayload, actor.getIdentity(), oldName,
        DataNameReadMode::AllowLegacyRead);
      BOOST_CHECK(restored.encode(DataNameReadMode::AllowLegacyRead) == oldPayload);
      BOOST_CHECK(restored.dataPrefix == descriptor.dataPrefix);
      BOOST_CHECK_THROW(ProtectedBlobManifest::decode(oldPayload, actor.getIdentity(), oldName),
                        std::invalid_argument);
      BOOST_CHECK_THROW(ProtectedBlobManifest::validateSuffix(
        ndn::Name("/NDNSF-DI/receipt/assembly").append(ndn::name::Component("name="))),
        std::invalid_argument);
    }
    const auto producer = ProtectedBlobManifest::inferDataProducer(publication.dataPrefix);
    BOOST_REQUIRE(producer.has_value());
    BOOST_CHECK(*producer == actor.getIdentity());
    BOOST_CHECK(publication.dataPrefix.getPrefix(-3) ==
                item.second.contentPrefix(actor.getIdentity(), publication.contentDigest.substr(7)));
    if (const char* oraclePath = std::getenv("SPEC201_PUBLICATION_BINDING_ORACLE")) {
      std::ofstream oracle(oraclePath, std::ios::app);
      BOOST_REQUIRE(oracle.good());
      oracle << "manifest_name=" << publication.manifestName.toUri(ndn::name::UriFormat::CANONICAL)
             << "\ndata_prefix=" << publication.dataPrefix.toUri(ndn::name::UriFormat::CANONICAL)
             << "\nencrypted_data_name=" << publication.encryptedDataName.toUri(ndn::name::UriFormat::CANONICAL)
             << "\nobject_id=" << publication.objectId
             << "\nplaintext_size=" << publication.plaintextSize
             << "\ncontent_digest=" << publication.contentDigest
             << "\nmanifest_digest=" << publication.manifestDigest
             << "\nauthorization_scope=" << publication.authorizationScope
             << "\nprotection_epoch=" << publication.protectionEpoch
             << "\nfile_backed=" << publication.fileBacked << "\n\n";
      oracle.close();
      BOOST_REQUIRE(oracle.good());
    }
    if (const char* packetDirectory = std::getenv("SPEC201_PUBLICATION_PACKET_ORACLE")) {
      const auto category = size_t(&item - suffixes.data()) + (fileBacked ? suffixes.size() : 0);
      const auto destination = fs::path(packetDirectory) / std::to_string(category);
      fs::create_directories(destination);
      const auto write = [&](const char* label, const ndn::Data& original) {
        const auto& wire = original.wireEncode();
        std::ofstream output(destination / label, std::ios::binary);
        output.write(reinterpret_cast<const char*>(wire.data()), wire.size());
        output.close();
        BOOST_REQUIRE(output.good());
      };
      write("root.wire", packet);
      for (uint64_t segment = 0; segment < descriptor.segmentCount; ++segment) {
        const auto count = face.sentData.size();
        face.receive(ndn::Interest(ndn::Name(descriptor.dataPrefix).appendSegment(segment)));
        face.processEvents(ndn::time::milliseconds(1));
        BOOST_REQUIRE_EQUAL(face.sentData.size(), count + 1);
        const auto filename = std::to_string(segment) + ".wire";
        write(filename.c_str(), face.sentData.back());
        if (segment == 0) {
          auto wrongFinal = face.sentData.back();
          wrongFinal.setFinalBlock(ndn::name::Component::fromSegment(descriptor.segmentCount));
          keys.sign(wrongFinal, ndn::security::signingByIdentity(actor.getIdentity()));
          write("invalid-final.wire", wrongFinal);
          auto wrongSize = face.sentData.back();
          const auto& bytes = wrongSize.getContent();
          BOOST_REQUIRE_GT(bytes.value_size(), 0);
          wrongSize.setContent(ndn::span<const uint8_t>(bytes.value(), bytes.value_size() - 1));
          keys.sign(wrongSize, ndn::security::signingByIdentity(actor.getIdentity()));
          write("invalid-size.wire", wrongSize);
        }
      }
      auto wrongRoot = packet;
      wrongRoot.setContent(ndn::span<const uint8_t>(payload.data(), payload.size() - 1));
      keys.sign(wrongRoot, ndn::security::signingByIdentity(actor.getIdentity()));
      write("invalid-root.wire", wrongRoot);
    }
    BOOST_CHECK_THROW(ProtectedBlobManifest::decode(payload, providerCertificate.getIdentity(),
      publication.manifestName), std::invalid_argument);
    auto wrongName = publication.manifestName.getPrefix(-1);
    wrongName.append(ndn::name::Component(std::string(32, 'x')));
    BOOST_CHECK_THROW(ProtectedBlobManifest::decode(payload, actor.getIdentity(), wrongName),
                      std::invalid_argument);
    auto broken = payload;
    broken.pop_back();
    BOOST_CHECK_THROW(ProtectedBlobManifest::decode(broken, actor.getIdentity(), publication.manifestName),
                      std::exception);
    face.receive(ndn::Interest(publication.manifestName));
    face.processEvents(ndn::time::milliseconds(1));
    BOOST_CHECK(face.sentData.back().wireEncode() == packet.wireEncode());
    auto pending = std::async(std::launch::async, [&] {
      return provider.fetchAndDecryptLargeData(publication.manifestName, service.toUri());
    });
    while (pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
      face.processEvents(ndn::time::milliseconds(1));
    const auto fetched = pending.get();
    BOOST_REQUIRE_MESSAGE(fetched.success, fetched.errorMessage);
    BOOST_CHECK_EQUAL_COLLECTIONS(fetched.plaintext.begin(), fetched.plaintext.end(),
                                  plaintext.begin(), plaintext.end());
    if (!fileBacked && item.second.kind == ContentDataKind::Blob && suffix.empty()) {
      // An independently composed same-schema original uses the historical
      // ALTERNATE reference transcript and genuine signed encrypted segments.
      auto historical = descriptor;
      historical.keyReferenceId = keyReferenceDigest(sendKey.keyId + "|" + sendKey.epochId + "|" +
        service.toUri(ndn::name::UriFormat::ALTERNATE) + "|" + descriptor.authorizationScope);
      const auto position = historical.protectionSpec.find(descriptor.keyReferenceId);
      BOOST_REQUIRE(position != std::string::npos);
      historical.protectionSpec.replace(position, descriptor.keyReferenceId.size(), historical.keyReferenceId);
      const auto protection = computeNamingDigest("ndnsf-naming/protection",
        {{1, 5, std::vector<std::uint8_t>(historical.representationSpec.begin(), historical.representationSpec.end())},
         {2, 5, std::vector<std::uint8_t>(historical.protectionSpec.begin(), historical.protectionSpec.end())}});
      historical.dataPrefix = item.second.contentPrefix(actor.getIdentity(), publication.contentDigest.substr(7));
      historical.dataPrefix.append("PROTECTED").append(makeNamingDigestComponent(protection)).append("DATA");
      const auto aad = historical.dataPrefix.toUri(ndn::name::UriFormat::CANONICAL) +
        "|REQUEST-LARGE|" + service.toUri(ndn::name::UriFormat::CANONICAL);
      const auto encrypted = hybridAesGcmEncrypt(sendKey.key,
        ndn::span<const std::uint8_t>(plaintext.data(), plaintext.size()),
        ndn::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(aad.data()), aad.size()));
      HybridMessageEnvelope envelope;
      envelope.setKeyId(sendKey.keyId); envelope.setEpochId(sendKey.epochId);
      envelope.setMessageType("REQUEST-LARGE"); envelope.setNonce(encrypted.nonce);
      envelope.setCipherText(encrypted.ciphertext); envelope.setAuthTag(encrypted.tag);
      const auto wire = envelope.WireEncode();
      historical.envelopeBytes = wire.size();
      historical.segmentCount = 1 + (wire.size() - 1) / historical.segmentBytes;
      std::vector<ndn::Data> originals;
      for (std::uint64_t segment = 0; segment < historical.segmentCount; ++segment) {
        ndn::Data data(ndn::Name(historical.dataPrefix).appendSegment(segment));
        const auto offset = segment * historical.segmentBytes;
        data.setContent(ndn::span<const std::uint8_t>(wire.data() + offset,
                        std::min<std::uint64_t>(historical.segmentBytes, wire.size() - offset)));
        data.setFreshnessPeriod(ndn::time::milliseconds(1000));
        data.setFinalBlock(ndn::name::Component::fromSegment(historical.segmentCount - 1));
        keys.sign(data, ndn::security::signingByIdentity(actor.getIdentity()));
        originals.push_back(std::move(data));
      }
      for (int variant = 0; variant < 4; ++variant) {
        auto candidate = historical;
        if (variant == 1) candidate.keyReferenceId.back() = candidate.keyReferenceId.back() == '0' ? '1' : '0';
        if (variant == 2) candidate.authorizationScope = "/SERVICE/spec201/other";
        if (variant == 3) candidate.protectionEpoch = sendKey.epochId + "-wrong";
        const auto body = candidate.encode();
        ndn::Data rootPacket(candidate.getName());
        rootPacket.setContent(ndn::span<const std::uint8_t>(body.data(), body.size()));
        rootPacket.setFreshnessPeriod(ndn::time::milliseconds(1000));
        keys.sign(rootPacket, ndn::security::signingByIdentity(actor.getIdentity()));
        const auto rootWire = rootPacket.wireEncode();
        ndn::signal::ScopedConnection replay = face.onSendInterest.connect([&](const ndn::Interest& interest) {
          if (interest.matchesData(rootPacket)) face.receive(rootPacket);
          for (const auto& data : originals) if (interest.matchesData(data)) face.receive(data);
        });
        auto pendingHistorical = std::async(std::launch::async, [&] {
          return provider.fetchAndDecryptLargeData(rootPacket.getName(),
            service.toUri(ndn::name::UriFormat::CANONICAL));
        });
        while (pendingHistorical.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
          face.processEvents(ndn::time::milliseconds(1));
        const auto restored = pendingHistorical.get();
        if (variant == 0) {
          BOOST_REQUIRE_MESSAGE(restored.success, restored.errorMessage);
          BOOST_CHECK_EQUAL_COLLECTIONS(restored.plaintext.begin(), restored.plaintext.end(),
                                        plaintext.begin(), plaintext.end());
          BOOST_CHECK(rootPacket.wireEncode() == rootWire);
        }
        else {
          BOOST_CHECK(!restored.success);
          BOOST_CHECK_EQUAL(restored.errorMessage, variant == 1 ? "manifest key reference mismatch" :
            variant == 2 ? "manifest service scope mismatch" : "manifest protection epoch mismatch");
        }
      }
    }
   }
  }
}
BOOST_AUTO_TEST_CASE(DelegatedProducerSignaturesAreCheckedForRootAndEverySegment)
{
  ScopedEnvironmentValue timeout("NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS", "2000");
  const auto root = fs::temp_directory_path() / ("spec201-delegated-" + std::to_string(::getpid()));
  fs::create_directories(root);
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code e; fs::remove_all(path, e); } } cleanup{root};
  const auto spool = (root / "spool").string();
  const auto references = (root / "references").string();
  ScopedEnvironmentValue dataDir("NDNSF_REQUEST_LARGE_DATA_DIR", spool.c_str());
  ScopedEnvironmentValue keyDir("NDNSF_DURABLE_KEY_REFERENCE_DIR", references.c_str());
  const auto schema = (root / "trust.conf").string();
  {
    std::ofstream out(schema);
    // Both identities are cryptographically trusted using explicit certificate
    // anchors below; only the delegate has permission for the Producer namespace.
    out << "rule { id delegated-fixture for data checker { type customized "
           "sig-type rsa-sha256 key-locator { type name regex \"^<>*$\" } } }\n";
    out.close();
    BOOST_REQUIRE(out.good());
    std::ofstream permissions(schema + ".producer-signers");
    permissions << "producer-signer { producer /spec201/delegated-owner "
                   "signer /spec201/delegate }\n";
    permissions.close();
    BOOST_REQUIRE(permissions.good());
  }
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  ndn::DummyClientFace publishingFace(keys), receivingFace(keys);
  const auto owner = makeRsaIdentity(keys, ndn::Name("/spec201/delegated-owner"));
  const auto delegate = makeRsaIdentity(keys, ndn::Name("/spec201/delegate"));
  const auto foreign = makeRsaIdentity(keys, ndn::Name("/spec201/foreign"));
  const auto receiver = makeRsaIdentity(keys, ndn::Name("/spec201/delegated-receiver"));
  const auto authority = makeRsaIdentity(keys, ndn::Name("/spec201/delegated-authority"));
  {
    const auto delegateAnchor = (root / "delegate.cert").string();
    const auto foreignAnchor = (root / "foreign.cert").string();
    ndn::io::save(delegate, delegateAnchor);
    ndn::io::save(foreign, foreignAnchor);
    std::ofstream out(schema, std::ios::app);
    out << "trust-anchor { type file file-name \"" << delegateAnchor << "\" }\n"
        << "trust-anchor { type file file-name \"" << foreignAnchor << "\" }\n";
    out.close();
    BOOST_REQUIRE(out.good());
  }
  auto store = std::make_shared<PacketStore>(root / "store");
  ndn_service_framework::test::IsolatedTestTrust fixedTrust7{
    owner, delegate, foreign, receiver, authority};
  LocalServiceUser user(publishingFace, ndn::Name("/spec201/delegated"), owner, authority,
                        fixedTrust7.schemaPath());
  user.useSigningKeyChainForSigningOnlyForTest(keys);
  user.attachLocalMockPubSubForTest(pubsub(publishingFace, keys));
  user.setEncryptedLargeDataRangeStore(store);
  user.init();
  publishingFace.processEvents(ndn::time::milliseconds(1));
  const ndn::Name service("/spec201/delegated-model");
  const auto sendKey = user.prepareHybridSendKeyForTest(service, "REQUEST-LARGE");
  LargeDataPublishOptions options;
  options.retention = EncryptedLargeDataRetention::Durable;
  options.contentName = materialContent("delegated-material");
  options.manifestSuffix = materialManifestSuffix("delegated-material");
  options.publicationIdentity = "sha256:" + std::string(64, 'd');
  const std::vector<std::uint8_t> plaintext(40000, 0x51);
  const auto publication = user.publishEncryptedLargeData(user.prepareServiceRequest(service.toUri()),
    plaintext, "delegated", ndn::time::milliseconds(1000), true, options);
  BOOST_REQUIRE_MESSAGE(publication.success, publication.errorMessage);
  const auto stored = store->lookupDurable(publication.publicationIdentity, {});
  BOOST_REQUIRE(stored.has_value());
  const auto source = stored->source;
  BOOST_REQUIRE(source);
  LocalServiceProvider provider(receivingFace, ndn::Name("/spec201/delegated"), receiver,
                                authority, schema);
  provider.useSigningKeyChainForTest(keys);
  provider.cacheHybridReceiveKeyForTest(sendKey.keyId, sendKey.epochId, sendKey.key);
  enum class SigningCase { Delegate, ForeignRoot, ForeignSegment, InvalidRootSignature };
  SigningCase signingCase = SigningCase::Delegate;
  std::size_t servedRoots = 0, servedSegments = 0;
  ndn::signal::ScopedConnection connection = receivingFace.onSendInterest.connect(
    [&](const ndn::Interest& interest) {
      const bool isRoot = interest.getName() == publication.manifestName;
      if (isRoot || publication.dataPrefix.isPrefixOf(interest.getName())) {
        const auto packetName = interest.getName() == publication.dataPrefix ?
          ndn::Name(publication.dataPrefix).appendSegment(0) : interest.getName();
        const auto wire = source->readOriginalDataPacket(packetName);
        ndn::Data packet(ndn::Block(ndn::span<const std::uint8_t>(wire.data(), wire.size())));
        BOOST_REQUIRE(interest.matchesData(packet));
        const auto& signer = (isRoot && signingCase == SigningCase::ForeignRoot) ||
                             (!isRoot && signingCase == SigningCase::ForeignSegment) ? foreign : delegate;
        keys.sign(packet, ndn::security::signingByIdentity(signer.getIdentity()));
        BOOST_REQUIRE(ndn::security::verifySignature(packet,
          keys.getPib().getIdentity(signer.getIdentity()).getDefaultKey()));
        if (isRoot && signingCase == SigningCase::InvalidRootSignature) {
          packet.setContent("tampered-root");
          BOOST_REQUIRE(!ndn::security::verifySignature(packet,
            keys.getPib().getIdentity(signer.getIdentity()).getDefaultKey()));
        }
        if (isRoot) ++servedRoots; else ++servedSegments;
        receivingFace.receive(packet);
      }
      else {
        for (const auto& certificate : {delegate, foreign}) {
          if (interest.matchesData(certificate)) receivingFace.receive(certificate);
        }
      }
    });
  for (const auto candidate : {SigningCase::Delegate, SigningCase::ForeignRoot,
                               SigningCase::ForeignSegment, SigningCase::InvalidRootSignature}) {
    signingCase = candidate;
    servedRoots = servedSegments = 0;
    auto pending = std::async(std::launch::async, [&] {
      return provider.fetchAndDecryptLargeData(publication.manifestName, service.toUri());
    });
    while (pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
      receivingFace.processEvents(ndn::time::milliseconds(1));
    const auto result = pending.get();
    BOOST_CHECK_EQUAL(servedRoots, 1);
    if (candidate == SigningCase::Delegate) {
      BOOST_REQUIRE_MESSAGE(result.success, result.errorMessage);
      BOOST_CHECK_EQUAL_COLLECTIONS(result.plaintext.begin(), result.plaintext.end(),
                                    plaintext.begin(), plaintext.end());
      BOOST_CHECK_GT(servedSegments, 1);
    }
    else if (candidate == SigningCase::ForeignRoot) {
      BOOST_CHECK(!result.success);
      BOOST_CHECK_EQUAL(result.errorMessage, "manifest signer is not authorized for its Producer");
      BOOST_CHECK_EQUAL(servedSegments, 0);
    }
    else if (candidate == SigningCase::ForeignSegment) {
      BOOST_CHECK(!result.success);
      BOOST_CHECK_EQUAL(result.errorMessage, "segment Producer signature or manifest geometry mismatch");
      BOOST_CHECK_GT(servedSegments, 0);
    }
    else {
      BOOST_CHECK(!result.success);
      BOOST_CHECK_EQUAL(result.errorMessage.find("manifest validation failed:"), 0);
      BOOST_CHECK_EQUAL(servedSegments, 0);
    }
  }
}

BOOST_AUTO_TEST_CASE(RestartReplaysOriginalManifestAndEvictedSegments)
{
  const auto root = fs::temp_directory_path() / ("spec201-raw-" + std::to_string(::getpid()));
  fs::create_directories(root);
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code e; fs::remove_all(path, e); } } cleanup{root};
  const auto spool = (root / "spool").string();
  const auto references = (root / "references").string();
  ScopedEnvironmentValue dataDir("NDNSF_REQUEST_LARGE_DATA_DIR", spool.c_str());
  ScopedEnvironmentValue keyDir("NDNSF_DURABLE_KEY_REFERENCE_DIR", references.c_str());
  ScopedEnvironmentValue window("NDNSF_REQUEST_LARGE_WINDOW_SEGMENTS", "2");
  ScopedEnvironmentValue retention("NDNSF_REQUEST_LARGE_DATA_RETENTION_MS", "1000");
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  ndn::DummyClientFace face(keys);
  const auto actor = makeRsaIdentity(keys, ndn::Name("/spec201/NDNSF/BLOB/owner"));
  const auto authority = makeRsaIdentity(keys, ndn::Name("/spec201/authority"));
  const ndn::Name service("/spec201/model");
  const std::vector<std::uint8_t> input(40000, 0x31);
  LargeDataPublishOptions options;
  options.retention = EncryptedLargeDataRetention::Durable;
  options.publicationIdentity = "sha256:" + std::string(64, 'a');
  options.contentName = materialContent("material");
  options.manifestSuffix = materialManifestSuffix("material");
  LargeDataPublishResult first;
  std::vector<std::uint8_t> rootWire, firstWire;
  {
    auto store = std::make_shared<PacketStore>(root / "store");
    ndn_service_framework::test::IsolatedTestTrust fixedTrust8{
      actor, authority};
    LocalServiceUser user(face, ndn::Name("/spec201/raw"), actor, authority, fixedTrust8.schemaPath());
    user.useSigningKeyChainForSigningOnlyForTest(keys);
    user.attachLocalMockPubSubForTest(pubsub(face, keys));
    user.setEncryptedLargeDataRangeStore(store);
    user.init();
    face.processEvents(ndn::time::milliseconds(1));
    user.prepareHybridSendKeyForTest(service, "REQUEST-LARGE");
    first = user.publishEncryptedLargeData(user.prepareServiceRequest(service.toUri()), input,
      "model", ndn::time::milliseconds(1000), true, options);
    BOOST_REQUIRE_MESSAGE(first.success, first.errorMessage);
    BOOST_CHECK(first.manifestName.getPrefix(-1) ==
      ndn::Name(actor.getIdentity()).append(options.manifestSuffix));
    BOOST_CHECK_EQUAL(store->commits, 1);
    auto same = user.publishEncryptedLargeData(user.prepareServiceRequest(service.toUri()), input,
      "model", ndn::time::milliseconds(1000), true, options);
    BOOST_REQUIRE_MESSAGE(same.success, same.errorMessage);
    BOOST_CHECK(same.manifestName == first.manifestName);
    BOOST_CHECK(same.dataPrefix == first.dataPrefix);
    BOOST_CHECK_EQUAL(store->commits, 1);
    BOOST_REQUIRE(first.servingLease);
    BOOST_REQUIRE(same.servingLease);
    face.receive(ndn::Interest(first.manifestName));
    face.processEvents(ndn::time::milliseconds(1));
    const auto& wire = face.sentData.back().wireEncode();
    rootWire.assign(wire.begin(), wire.end());
    const auto fetch = [&](unsigned segment) {
      const auto requested = ndn::Name(first.dataPrefix).appendSegment(segment);
      const auto previous = face.sentData.size();
      face.receive(ndn::Interest(requested));
      face.processEvents(ndn::time::milliseconds(1));
      BOOST_REQUIRE_EQUAL(face.sentData.size(), previous + 1);
      BOOST_REQUIRE(face.sentData.back().getName() == requested);
      const auto& packet = face.sentData.back().wireEncode();
      return std::vector<std::uint8_t>(packet.begin(), packet.end());
    };
    firstWire = fetch(0);
    fetch(1); fetch(2); fetch(3);
    BOOST_CHECK(fetch(0) == firstWire);
    // One request releases its pin; the other must still serve the original
    // packets after expiration, rather than merely preserving stored bytes.
    first.servingLease.reset();
    face.processEvents(ndn::time::milliseconds(1100));
    BOOST_REQUIRE_EQUAL(user.getLargeDataServingMetricsForTest().publicationCount, 1U);
    const auto previous = face.sentData.size();
    face.receive(ndn::Interest(same.manifestName));
    face.processEvents(ndn::time::milliseconds(1));
    BOOST_REQUIRE_EQUAL(face.sentData.size(), previous + 1);
    BOOST_REQUIRE(face.sentData.back().getName() == same.manifestName);
    const auto& survivingRoot = face.sentData.back().wireEncode();
    BOOST_CHECK_EQUAL_COLLECTIONS(rootWire.begin(), rootWire.end(),
                                 survivingRoot.begin(), survivingRoot.end());
    BOOST_CHECK(fetch(0) == firstWire);
  }
  face.processEvents(ndn::time::milliseconds(1));
  {
    auto store = std::make_shared<PacketStore>(root / "store");
    ndn_service_framework::test::IsolatedTestTrust fixedTrust9{
      actor, authority};
    LocalServiceUser restored(face, ndn::Name("/spec201/raw"), actor, authority, fixedTrust9.schemaPath());
    restored.useSigningKeyChainForSigningOnlyForTest(keys);
    restored.attachLocalMockPubSubForTest(pubsub(face, keys));
    restored.setEncryptedLargeDataRangeStore(store);
    restored.init();
    face.processEvents(ndn::time::milliseconds(1));
    const auto reused = restored.publishEncryptedLargeData(restored.prepareServiceRequest(service.toUri()),
      input, "model", ndn::time::milliseconds(1000), true, options);
    BOOST_REQUIRE_MESSAGE(reused.success, reused.errorMessage);
    BOOST_CHECK_EQUAL(store->commits, 0);
    BOOST_CHECK(reused.manifestName == first.manifestName);
    BOOST_CHECK(reused.dataPrefix == first.dataPrefix);
    face.receive(ndn::Interest(reused.manifestName));
    face.processEvents(ndn::time::milliseconds(1));
    const auto& rootPacket = face.sentData.back().wireEncode();
    BOOST_CHECK_EQUAL_COLLECTIONS(rootWire.begin(), rootWire.end(), rootPacket.begin(), rootPacket.end());
    face.receive(ndn::Interest(ndn::Name(reused.dataPrefix).appendSegment(0)));
    face.processEvents(ndn::time::milliseconds(1));
    const auto& packet = face.sentData.back().wireEncode();
    BOOST_CHECK_EQUAL_COLLECTIONS(firstWire.begin(), firstWire.end(), packet.begin(), packet.end());
  }
}
BOOST_AUTO_TEST_CASE(LegacyAndMalformedReferencesUseActualCoreRestore)
{
  // Independent byte mutations of the record emitted by the actual writer.
  // No production reader/serializer is used to construct these counterexamples.
  for (const std::string mode : {"v1", "v1-duplicate", "v1-tail", "v1-truncated",
                                 "v1-count", "v1-field", "mixed", "duplicate",
                                 "tail", "truncated", "count", "field", "precedence",
                                 "warm-service", "warm-locator", "warm-manifest", "warm-version"}) {
    BOOST_TEST_CONTEXT(mode) {
      const auto root = fs::temp_directory_path() /
        ("spec201-record-" + std::to_string(::getpid()) + "-" + mode);
      fs::create_directories(root);
      struct Cleanup { fs::path path; ~Cleanup() { std::error_code e; fs::remove_all(path, e); } } cleanup{root};
      const auto refs = (root / "references").string();
      const auto spool = (root / "spool").string();
      ScopedEnvironmentValue keyDir("NDNSF_DURABLE_KEY_REFERENCE_DIR", refs.c_str());
      ScopedEnvironmentValue dataDir("NDNSF_REQUEST_LARGE_DATA_DIR", spool.c_str());
      ndn::KeyChain keys("pib-memory:", "tpm-memory:");
      ndn::DummyClientFace face(keys);
      const auto actor = makeRsaIdentity(keys, ndn::Name("/spec201/record-owner"));
      const auto authority = makeRsaIdentity(keys, ndn::Name("/spec201/authority"));
      const ndn::Name service("/spec201/model");
      auto store = std::make_shared<PacketStore>(root / "store");
      LargeDataPublishOptions options;
      options.retention = EncryptedLargeDataRetention::Durable;
      options.publicationIdentity = "sha256:" + std::string(64, 'f');
      options.contentName = materialContent("record-proof");
      options.manifestSuffix = materialManifestSuffix("record-proof");
      const std::vector<std::uint8_t> payload(40000, 0x32);
      const auto publish = [&](LocalServiceUser& user) {
        return user.publishEncryptedLargeData(user.prepareServiceRequest(service.toUri()),
          payload, "record-proof", ndn::time::milliseconds(1000), true, options);
      };
      LargeDataPublishResult original;
      {
        ndn_service_framework::test::IsolatedTestTrust fixedTrust10{
          actor, authority};
        LocalServiceUser user(face, ndn::Name("/spec201/records"), actor, authority, fixedTrust10.schemaPath());
        user.useSigningKeyChainForSigningOnlyForTest(keys);
        user.attachLocalMockPubSubForTest(pubsub(face, keys));
        user.setEncryptedLargeDataRangeStore(store);
        user.init(); face.processEvents(ndn::time::milliseconds(1));
        user.prepareHybridSendKeyForTest(service, "REQUEST-LARGE");
        original = publish(user);
        BOOST_REQUIRE_MESSAGE(original.success, original.errorMessage);
      }
      face.processEvents(ndn::time::milliseconds(1));
      // Load the actual table twice in fresh owners in this process before
      // mutating it.  The second restore must exercise positive syntax memo
      // reuse; subsequent malformed/external records must still be rejected.
      for (unsigned replay = 0; replay < 2; ++replay) {
        ndn_service_framework::test::IsolatedTestTrust fixedTrust11{
          actor, authority};
        LocalServiceUser warm(face, ndn::Name("/spec201/records"), actor, authority,
                              fixedTrust11.schemaPath());
        warm.useSigningKeyChainForSigningOnlyForTest(keys);
        warm.attachLocalMockPubSubForTest(pubsub(face, keys));
        warm.setEncryptedLargeDataRangeStore(store);
        warm.init(); face.processEvents(ndn::time::milliseconds(1));
        warm.prepareHybridSendKeyForTest(service, "REQUEST-LARGE");
        const auto reused = publish(warm);
        BOOST_REQUIRE_MESSAGE(reused.success, reused.errorMessage);
        BOOST_REQUIRE(reused.manifestName == original.manifestName);
        BOOST_CHECK_EQUAL(store->commits, 1);
      }
      const auto committed = store->lookupDurable(original.publicationIdentity, {});
      BOOST_REQUIRE(committed);
      const auto originalWire = committed->source->readOriginalDataPacket(original.manifestName);
      fs::path current;
      for (const auto& entry : fs::directory_iterator(refs))
        if (entry.path().extension() == ".dkr") { BOOST_REQUIRE(current.empty()); current = entry.path(); }
      BOOST_REQUIRE(!current.empty());
      std::ifstream file(current, std::ios::binary);
      std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
      constexpr std::size_t header = sizeof("NDNSF-DURABLE-LARGE-KEYREF-V3") - 1;
      std::size_t offset = header + sizeof(std::uint64_t);
      std::vector<std::vector<std::uint8_t>> fields;
      for (unsigned i = 0; i < 14; ++i) {
        const auto start = offset;
        BOOST_REQUIRE(offset + sizeof(std::uint64_t) <= bytes.size());
        std::uint64_t size = 0;
        std::memcpy(&size, bytes.data() + offset, sizeof(size));
        offset += sizeof(size);
        if (i < 4 || i > 6) { BOOST_REQUIRE(size <= bytes.size() - offset); offset += size; }
        fields.emplace_back(bytes.begin() + start, bytes.begin() + offset);
      }
      BOOST_REQUIRE_EQUAL(offset, bytes.size());
      const auto appendInteger = [](std::vector<std::uint8_t>& out, std::uint64_t value) {
        const auto* raw = reinterpret_cast<const std::uint8_t*>(&value);
        out.insert(out.end(), raw, raw + sizeof(value));
      };
      const auto write = [](const fs::path& path, const std::vector<std::uint8_t>& out) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(out.data()), out.size());
        file.close(); BOOST_REQUIRE(file.good());
      };
      std::string escaped;
      constexpr char hex[] = "0123456789abcdef";
      for (const unsigned char c : actor.getIdentity().toUri()) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') escaped += c;
        else { escaped += '_'; escaped += hex[c >> 4]; escaped += hex[c & 15]; }
      }
      const auto legacy = current.parent_path() / ("user-" + escaped + ".dkr");
      const bool v1 = mode.rfind("v1", 0) == 0;
      if (mode.rfind("warm-", 0) == 0) {
        const auto index = mode == "warm-service" ? 1U : mode == "warm-locator" ? 11U :
                           mode == "warm-manifest" ? 12U : 9U;
        const std::string changed = mode == "warm-service" ? "/" :
          mode == "warm-locator" ? "/other/valid/locator" :
          mode == "warm-manifest" ? "/other/invalid/manifest" : "v2";
        fields[index].clear(); appendInteger(fields[index], changed.size());
        fields[index].insert(fields[index].end(), changed.begin(), changed.end());
      }
      std::vector<std::uint8_t> record;
      for (unsigned i = 0; i < (v1 ? 12U : 14U); ++i)
        if (!v1 || i != 6) record.insert(record.end(), fields[i].begin(), fields[i].end());
      std::vector<std::uint8_t> mutated(bytes.begin(), bytes.begin() + header);
      if (v1) mutated.back() = '1';
      appendInteger(mutated, (mode == "v1-duplicate" || mode == "duplicate" || mode == "mixed") ? 2 : 1);
      mutated.insert(mutated.end(), record.begin(), record.end());
      if (mode == "v1-duplicate" || mode == "duplicate")
        mutated.insert(mutated.end(), record.begin(), record.end());
      if (mode == "mixed") {
        auto historical = fields;
        historical[0].back() = historical[0].back() == 'a' ? 'b' : 'a';
        historical[12].clear(); appendInteger(historical[12], 0);
        const std::string format = "legacy-reference-v2";
        historical[13].clear(); appendInteger(historical[13], format.size());
        historical[13].insert(historical[13].end(), format.begin(), format.end());
        for (const auto& part : historical) mutated.insert(mutated.end(), part.begin(), part.end());
      }
      if (mode == "v1-tail" || mode == "tail") mutated.push_back(0x99);
      if (mode == "v1-truncated" || mode == "truncated") mutated.pop_back();
      if (mode == "v1-count" || mode == "count") {
        const std::uint64_t huge = UINT64_MAX;
        std::memcpy(mutated.data() + header, &huge, sizeof(huge));
      }
      if (mode == "v1-field" || mode == "field") {
        const std::uint64_t huge = UINT64_MAX;
        std::memcpy(mutated.data() + header + sizeof(huge), &huge, sizeof(huge));
      }
      if (v1) { write(legacy, mutated); fs::remove(current); }
      else if (mode == "precedence") {
        auto validLegacy = bytes;
        validLegacy[header - 1] = '2';
        validLegacy.resize(header + sizeof(std::uint64_t));
        for (unsigned i = 0; i < 12; ++i)
          validLegacy.insert(validLegacy.end(), fields[i].begin(), fields[i].end());
        write(current.parent_path() / ("user-" + escaped + ".v2.dkr"), validLegacy);
        mutated.push_back(0x99); write(current, mutated);
      }
      else write(current, mutated);
      const auto before = store->commits;
      {
        ndn_service_framework::test::IsolatedTestTrust fixedTrust12{
          actor, authority};
        LocalServiceUser restored(face, ndn::Name("/spec201/records"), actor, authority, fixedTrust12.schemaPath());
        restored.useSigningKeyChainForSigningOnlyForTest(keys);
        restored.attachLocalMockPubSubForTest(pubsub(face, keys));
        restored.setEncryptedLargeDataRangeStore(store);
        restored.init(); face.processEvents(ndn::time::milliseconds(1));
        if (mode != "v1" && mode != "mixed")
          restored.prepareHybridSendKeyForTest(service, "REQUEST-LARGE");
        const auto result = publish(restored);
        if (mode == "v1" || mode == "mixed") {
          BOOST_REQUIRE_MESSAGE(result.success, result.errorMessage);
          BOOST_CHECK_EQUAL(store->commits, before);
          BOOST_REQUIRE(result.manifestName == original.manifestName);
          const auto count = face.sentData.size();
          face.receive(ndn::Interest(result.manifestName)); face.processEvents(ndn::time::milliseconds(1));
          BOOST_REQUIRE_EQUAL(face.sentData.size(), count + 1);
          const auto& served = face.sentData.back().wireEncode();
          BOOST_CHECK_EQUAL_COLLECTIONS(originalWire.begin(), originalWire.end(), served.begin(), served.end());
        }
        else {
          if (v1) {
            BOOST_REQUIRE_MESSAGE(result.success, result.errorMessage);
            BOOST_CHECK(result.manifestName != original.manifestName);
          }
          else {
            BOOST_CHECK(!result.success);
            BOOST_CHECK_EQUAL(result.errorMessage, "DURABLE_KEY_REFERENCE_STORE_UNAVAILABLE");
          }
          const auto count = face.sentData.size();
          face.receive(ndn::Interest(original.manifestName)); face.processEvents(ndn::time::milliseconds(1));
          BOOST_CHECK_EQUAL(face.sentData.size(), count);
        }
      }
      BOOST_CHECK(committed->source->readOriginalDataPacket(original.manifestName) == originalWire);
      if (v1) BOOST_CHECK(fs::exists(legacy));
    }
  }
}

BOOST_AUTO_TEST_CASE(RangeOnlyDurableBackendIsRejectedBeforeCommit)
{
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  ndn::DummyClientFace face(keys);
  const auto actor = makeRsaIdentity(keys, ndn::Name("/spec201/reject/user"));
  const auto authority = makeRsaIdentity(keys, ndn::Name("/spec201/reject/authority"));
  auto store = std::make_shared<RangeOnlyDurableStore>();
  ndn_service_framework::test::IsolatedTestTrust fixedTrust13{
    actor, authority};
  LocalServiceUser user(face, ndn::Name("/spec201/raw"), actor, authority, fixedTrust13.schemaPath());
  user.useSigningKeyChainForSigningOnlyForTest(keys);
  user.attachLocalMockPubSubForTest(pubsub(face, keys));
  user.setEncryptedLargeDataRangeStore(store);
  user.init();
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK(!user.supportsDurableEncryptedLargeData());
  LargeDataPublishOptions options;
  options.retention = EncryptedLargeDataRetention::Durable;
  options.publicationIdentity = "sha256:" + std::string(64, 'b');
  const auto published = user.publishEncryptedLargeData(
    user.prepareServiceRequest("/spec201/model"), std::vector<std::uint8_t>(100, 42),
    "model", ndn::time::milliseconds(1000), true, options);
  BOOST_CHECK(!published.success);
  BOOST_CHECK_EQUAL(published.errorMessage, "ORIGINAL_DATA_PACKET_UNSUPPORTED");
  BOOST_CHECK_EQUAL(store->commits, 0);
  BOOST_CHECK(published.manifestName.empty());
  BOOST_CHECK_EQUAL(user.getLargeDataServingMetricsForTest().publicationCount, 0);
}
BOOST_AUTO_TEST_CASE(CancelledPacketTransactionPreservesEarlierPublication)
{
  const auto root = fs::temp_directory_path() / ("spec201-cancel-" + std::to_string(::getpid()));
  fs::create_directories(root);
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code e; fs::remove_all(path, e); } } cleanup{root};
  const auto spool = (root / "spool").string();
  const auto references = (root / "references").string();
  ScopedEnvironmentValue dataDir("NDNSF_REQUEST_LARGE_DATA_DIR", spool.c_str());
  ScopedEnvironmentValue keyDir("NDNSF_DURABLE_KEY_REFERENCE_DIR", references.c_str());
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  ndn::DummyClientFace face(keys);
  const auto actor = makeRsaIdentity(keys, ndn::Name("/spec201/cancel/user"));
  const auto authority = makeRsaIdentity(keys, ndn::Name("/spec201/cancel/authority"));
  auto store = std::make_shared<PacketStore>(root / "store");
  ndn_service_framework::test::IsolatedTestTrust fixedTrust14{
    actor, authority};
  LocalServiceUser user(face, ndn::Name("/spec201/raw"), actor, authority, fixedTrust14.schemaPath());
  user.useSigningKeyChainForSigningOnlyForTest(keys);
  user.attachLocalMockPubSubForTest(pubsub(face, keys));
  user.setEncryptedLargeDataRangeStore(store);
  user.init();
  face.processEvents(ndn::time::milliseconds(1));
  const ndn::Name service("/spec201/model");
  user.prepareHybridSendKeyForTest(service, "REQUEST-LARGE");
  LargeDataPublishOptions options;
  options.retention = EncryptedLargeDataRetention::Durable;
  options.publicationIdentity = "sha256:" + std::string(64, 'c');
  const std::vector<std::uint8_t> input(40000, 0x21);
  const auto publish = [&](const LargeDataPublishOptions& retention, const std::function<void()>& current) {
    return user.publishEncryptedLargeData(user.prepareServiceRequest(service.toUri()), input,
      "model", ndn::time::milliseconds(1000), true, retention, current);
  };
  const auto earlier = publish(options, {});
  BOOST_REQUIRE_MESSAGE(earlier.success, earlier.errorMessage);
  const auto committed = store->lookupDurable(earlier.publicationIdentity, {});
  BOOST_REQUIRE(committed);
  const auto oldRootWire = committed->source->readOriginalDataPacket(earlier.manifestName);
  const auto conflict = user.publishEncryptedLargeData(user.prepareServiceRequest(service.toUri()),
    std::vector<std::uint8_t>(input.size(), 0x22), "model", ndn::time::milliseconds(1000), true, options);
  BOOST_CHECK(!conflict.success);
  BOOST_CHECK_EQUAL(store->commits, 1);
  BOOST_CHECK(committed->source->readOriginalDataPacket(earlier.manifestName) == oldRootWire);
  options.publicationIdentity = "sha256:" + std::string(64, 'd');
  bool cancelled = false;
  store->afterPacket = [&](std::uint64_t count) { if (count == 2) cancelled = true; };
  const auto failed = publish(options, [&] {
    if (cancelled) throw std::runtime_error("injected packet transaction cancellation");
  });
  BOOST_CHECK(!failed.success);
  BOOST_CHECK(failed.errorMessage.find("injected packet transaction cancellation") != std::string::npos);
  BOOST_REQUIRE(!store->lastAttemptIdentity.empty());
  BOOST_CHECK(!store->lookupDurable(store->lastAttemptIdentity, {}));
  BOOST_CHECK_EQUAL(user.getLargeDataServingMetricsForTest().publicationCount, 1);
  BOOST_CHECK(committed->source->readOriginalDataPacket(earlier.manifestName) == oldRootWire);
  for (const auto& entry : fs::directory_iterator(root / "store"))
    BOOST_CHECK(entry.path().filename().string().find("pending-") != 0);
  const auto before = face.sentData.size();
  face.receive(ndn::Interest(failed.manifestName));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK_EQUAL(face.sentData.size(), before);
  cancelled = false;
  store->afterPacket = {};
  const auto retry = publish(options, {});
  BOOST_REQUIRE_MESSAGE(retry.success, retry.errorMessage);
  BOOST_CHECK(store->lookupDurable(retry.publicationIdentity, {}).has_value());
  BOOST_CHECK_EQUAL(user.getLargeDataServingMetricsForTest().publicationCount, 2);
  BOOST_CHECK(committed->source->readOriginalDataPacket(earlier.manifestName) == oldRootWire);
}
BOOST_AUTO_TEST_CASE(RealCorePublicationUsesAtomicRepoBackendAndRestoresOriginalPackets)
{
  using namespace ndnsf_distributed_repo;
  const auto root = fs::temp_directory_path() / ("spec201-core-repo-" + std::to_string(::getpid()));
  fs::create_directories(root);
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code e; fs::remove_all(path, e); } } cleanup{root};
  const auto spool = (root / "spool").string();
  const auto references = (root / "references").string();
  ScopedEnvironmentValue dataDir("NDNSF_REQUEST_LARGE_DATA_DIR", spool.c_str());
  ScopedEnvironmentValue keyDir("NDNSF_DURABLE_KEY_REFERENCE_DIR", references.c_str());
  ScopedEnvironmentValue window("NDNSF_REQUEST_LARGE_WINDOW_SEGMENTS", "2");
  const auto openRepo = [&] {
    StorageCapability capability;
    capability.repoNode = "/spec201/core-repo";
    capability.freeBytes = 1U << 20;
    return std::make_shared<RepoCore>(capability,
      makeFilesystemRepoStore((root / "repo").string(), 1U << 20, 1U << 20));
  };
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  ndn::DummyClientFace face(keys);
  const auto actor = makeRsaIdentity(keys, ndn::Name("/spec201/NDNSF/actual-repo-producer"));
  const auto authority = makeRsaIdentity(keys, ndn::Name("/spec201/core-repo-authority"));
  const ndn::Name service("/spec201/model");
  const std::vector<uint8_t> input(40000, 0x31);
  LargeDataPublishOptions options;
  options.retention = EncryptedLargeDataRetention::Durable;
  options.publicationIdentity = "sha256:" + std::string(64, 'b');
  options.contentName = materialContent("material");
  options.manifestSuffix = materialManifestSuffix("material");
  LargeDataPublishResult first;
  std::vector<uint8_t> firstWire, rootWire;
  {
    auto repo = openRepo();
    auto backend = std::make_shared<RepoEncryptedLargeDataStore>(repo);
    ndn_service_framework::test::IsolatedTestTrust fixedTrust15{
      actor, authority};
    LocalServiceUser user(face, ndn::Name("/spec201/raw"), actor, authority, fixedTrust15.schemaPath());
    user.useSigningKeyChainForSigningOnlyForTest(keys);
    user.attachLocalMockPubSubForTest(pubsub(face, keys));
    user.setEncryptedLargeDataRangeStore(backend);
    user.init();
    face.processEvents(ndn::time::milliseconds(1));
    user.prepareHybridSendKeyForTest(service, "REQUEST-LARGE");
    first = user.publishEncryptedLargeData(user.prepareServiceRequest(service.toUri()), input,
      "model", ndn::time::milliseconds(1000), true, options);
    BOOST_REQUIRE_MESSAGE(first.success, first.errorMessage);
    BOOST_REQUIRE_EQUAL(repo->list().size(), 1U);
    BOOST_CHECK_EQUAL(repo->list().front().objectType, "original-packet-set");
    rootWire = repo->getDataPacket(first.manifestName.toUri());
    firstWire = repo->getDataPacket(ndn::Name(first.dataPrefix).appendSegment(0).toUri());
    const auto stored = backend->lookupDurable(first.publicationIdentity);
    BOOST_REQUIRE(stored);
    EncryptedLargeDataCommitOptions copy;
    copy.retention = EncryptedLargeDataRetention::Durable;
    copy.publicationIdentity = stored->publicationIdentity;
    copy.protectionEpoch = stored->protectionEpoch;
    copy.keyReferenceId = stored->keyReferenceId;
    copy.keyReferenceVersion = stored->keyReferenceVersion;
    copy.ciphertextManifestDigest = stored->ciphertextManifestDigest;
    copy.servingLocator = stored->servingLocator;
    copy.contentDigest = stored->contentDigest;
    copy.plaintextSize = stored->plaintextSize;
    copy.packetStorageFormat = stored->packetStorageFormat;
    copy.manifestName = stored->manifestName;
    copy.dataPrefix = stored->dataPrefix;
    copy.packetWireBytes = stored->packetWireBytes;
    StorageCapability replicaCapability;
    replicaCapability.repoNode = "/spec201/second-repo";
    replicaCapability.freeBytes = 1U << 20;
    auto replica = std::make_shared<RepoCore>(replicaCapability,
      makeFilesystemRepoStore((root / "replica").string(), 1U << 20, 1U << 20));
    RepoEncryptedLargeDataStore replicaBackend(replica);
    const auto names = repo->list().front().packetNames;
    size_t nextIndex = 0;
    const auto replicaSource = replicaBackend.commitOriginalDataPackets(first.manifestName, copy,
      [&]() -> std::optional<std::pair<ndn::Name, std::vector<uint8_t>>> {
        if (nextIndex == names.size()) return std::nullopt;
        const ndn::Name name(names[nextIndex++]);
        return std::make_pair(name, stored->source->readOriginalDataPacket(name));
      });
    for (const auto& name : names) {
      BOOST_CHECK(replicaSource->readOriginalDataPacket(ndn::Name(name)) == repo->getDataPacket(name));
      BOOST_CHECK(replica->getDataPacket(name) == repo->getDataPacket(name));
    }
    for (uint64_t i = 0; i + 1 < repo->list().front().packetNames.size(); ++i) {
      const ndn::Name name = ndn::Name(first.dataPrefix).appendSegment(i);
      face.receive(ndn::Interest(name));
      face.processEvents(ndn::time::milliseconds(1));
      const auto& wire = face.sentData.back().wireEncode();
      BOOST_CHECK(std::vector<uint8_t>(wire.begin(), wire.end()) == repo->getDataPacket(name.toUri()));
    }
  }
  {
    auto repo = openRepo();
    ndn_service_framework::test::IsolatedTestTrust fixedTrust16{
      actor, authority};
    LocalServiceUser user(face, ndn::Name("/spec201/raw"), actor, authority, fixedTrust16.schemaPath());
    user.useSigningKeyChainForSigningOnlyForTest(keys);
    user.attachLocalMockPubSubForTest(pubsub(face, keys));
    user.setEncryptedLargeDataRangeStore(std::make_shared<RepoEncryptedLargeDataStore>(repo));
    user.init();
    face.processEvents(ndn::time::milliseconds(1));
    const auto reused = user.publishEncryptedLargeData(user.prepareServiceRequest(service.toUri()), input,
      "model", ndn::time::milliseconds(1000), true, options);
    BOOST_REQUIRE_MESSAGE(reused.success, reused.errorMessage);
    BOOST_CHECK(reused.manifestName == first.manifestName);
    BOOST_CHECK(repo->getDataPacket(reused.manifestName.toUri()) == rootWire);
    BOOST_CHECK(repo->getDataPacket(ndn::Name(reused.dataPrefix).appendSegment(0).toUri()) == firstWire);
    BOOST_CHECK_EQUAL(repo->list().size(), 1U);
  }
}

BOOST_AUTO_TEST_CASE(DurableReferenceCapacityRetiresOnlyObsoleteServiceVersions)
{
  for (const std::string mode : {"retire", "same-full", "stale-owner", "write-fail"}) {
    BOOST_TEST_CONTEXT(mode) {
      const auto root = fs::temp_directory_path() /
        ("spec217-reference-capacity-" + std::to_string(::getpid()) + "-" + mode);
      fs::create_directories(root);
      struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
      } cleanup{root};
      const auto spool = (root / "spool").string();
      const auto references = (root / "references").string();
      ScopedEnvironmentValue dataDir("NDNSF_REQUEST_LARGE_DATA_DIR", spool.c_str());
      ScopedEnvironmentValue keyDir("NDNSF_DURABLE_KEY_REFERENCE_DIR", references.c_str());
      ndn::KeyChain keys("pib-memory:", "tpm-memory:");
      ndn::DummyClientFace face(keys);
      const auto actor = makeRsaIdentity(keys, ndn::Name("/spec217/reference-owner"));
      const auto authority = makeRsaIdentity(keys, ndn::Name("/spec217/reference-authority"));
      IsolatedTestTrust trust{actor, authority};
      const ndn::Name service("/spec217/reference-model");
      auto store = std::make_shared<PacketStore>(root / "store");
      const std::vector<std::uint8_t> payload(40000, 0x37);
      const auto publish = [&] (std::uint64_t epoch, const std::string& label) {
        LocalServiceUser user(face, ndn::Name("/spec217/references"), actor,
                              authority, trust.schemaPath());
        user.useSigningKeyChainForSigningOnlyForTest(keys);
        user.attachLocalMockPubSubForTest(pubsub(face, keys));
        user.setEncryptedLargeDataRangeStore(store);
        user.init();
        face.processEvents(ndn::time::milliseconds(1));
        const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<
          std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        PolicyStatusData status;
        status.setServiceName(service);
        status.setControllerVersion(ControllerVersion{2170, epoch});
        status.setValidity(now - 1000, now + 60000);
        status.setPolicyDigest(keyReferenceDigest("policy"));
        status.setControllerCertificate(ndn::Name("/controller/spec217"));
        BOOST_REQUIRE(user.installControllerStatus(status));
        user.prepareHybridSendKeyForTest(service, "REQUEST-LARGE");
        LargeDataPublishOptions options;
        options.retention = EncryptedLargeDataRetention::Durable;
        options.publicationIdentity = keyReferenceDigest(label);
        options.contentName = materialContent(label);
        options.manifestSuffix = materialManifestSuffix(label);
        return user.publishEncryptedLargeData(user.prepareServiceRequest(service.toUri()),
          payload, label, ndn::time::milliseconds(1000), true, options);
      };
      const auto seed = publish(1, "reference-seed");
      BOOST_REQUIRE_MESSAGE(seed.success, seed.errorMessage);
      const auto original = store->lookupDurable(seed.publicationIdentity, {});
      BOOST_REQUIRE(original);
      const auto originalWire = original->source->readOriginalDataPacket(seed.manifestName);
      fs::path table;
      for (const auto& entry : fs::directory_iterator(references)) {
        if (entry.path().extension() == ".dkr") {
          BOOST_REQUIRE(table.empty());
          table = entry.path();
        }
      }
      BOOST_REQUIRE(!table.empty());
      const auto read = [] (const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(input)), {});
      };
      const auto bytes = read(table);
      constexpr std::size_t header = sizeof("NDNSF-DURABLE-LARGE-KEYREF-V3") - 1;
      std::size_t offset = header + sizeof(std::uint64_t);
      std::vector<std::vector<std::uint8_t>> fields;
      for (unsigned i = 0; i < 14; ++i) {
        const auto begin = offset;
        BOOST_REQUIRE(offset + sizeof(std::uint64_t) <= bytes.size());
        std::uint64_t size = 0;
        std::memcpy(&size, bytes.data() + offset, sizeof(size));
        offset += sizeof(size);
        if (i < 4 || i > 6) {
          BOOST_REQUIRE(size <= bytes.size() - offset);
          offset += size;
        }
        fields.emplace_back(bytes.begin() + begin, bytes.begin() + offset);
      }
      BOOST_REQUIRE_EQUAL(offset, bytes.size());
      const auto integer = [] (std::uint64_t value) {
        const auto* first = reinterpret_cast<const std::uint8_t*>(&value);
        return std::vector<std::uint8_t>(first, first + sizeof(value));
      };
      const auto string = [&] (const std::string& value) {
        auto result = integer(value.size());
        result.insert(result.end(), value.begin(), value.end());
        return result;
      };
      const auto count = mode == "stale-owner" ? 2U : 4096U;
      std::vector<std::uint8_t> seeded(bytes.begin(), bytes.begin() + header);
      const auto countBytes = integer(count);
      seeded.insert(seeded.end(), countBytes.begin(), countBytes.end());
      std::vector<std::uint8_t> preserved;
      for (unsigned i = 0; i < count; ++i) {
        auto record = fields;
        record[0] = string(keyReferenceDigest("historical-" + std::to_string(i)));
        if (mode == "stale-owner" && i == count - 1) {
          record[5] = integer(3);
        }
        if ((mode == "retire" || mode == "write-fail") && i >= count - 3) {
          if (i == count - 3) {
            record[5] = integer(0);
            record[6] = integer(0);
            record[12] = string("");
            record[13] = string("legacy-reference-v2");
          }
          else if (i == count - 2) {
            record[1] = string(ndn::Name("/spec217/other-model").toUri(
              ndn::name::UriFormat::CANONICAL));
          }
          else {
            record[5] = integer(2);
          }
          for (const auto& part : record)
            preserved.insert(preserved.end(), part.begin(), part.end());
        }
        for (const auto& part : record)
          seeded.insert(seeded.end(), part.begin(), part.end());
      }
      {
        std::ofstream output(table, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(seeded.data()), seeded.size());
        output.close();
        BOOST_REQUIRE(output.good());
      }
      if (mode == "write-fail")
        fs::create_directory(table.string() + ".tmp." + std::to_string(::getpid()));
      const auto result = publish(mode == "same-full" ? 1 : 2, "reference-current");
      const auto observed = read(table);
      if (mode == "retire") {
        BOOST_REQUIRE_MESSAGE(result.success, result.errorMessage);
        std::uint64_t actualCount = 0;
        BOOST_REQUIRE(observed.size() >= header + sizeof(actualCount) + preserved.size());
        std::memcpy(&actualCount, observed.data() + header, sizeof(actualCount));
        BOOST_CHECK_EQUAL(actualCount, 4U);
        BOOST_CHECK(std::equal(preserved.begin(), preserved.end(),
                              observed.begin() + header + sizeof(actualCount)));
      }
      else {
        BOOST_CHECK(!result.success);
        BOOST_CHECK_EQUAL(result.errorMessage, "DURABLE_KEY_REFERENCE_STORE_UNAVAILABLE");
        BOOST_CHECK(observed == seeded);
      }
      BOOST_CHECK(original->source->readOriginalDataPacket(seed.manifestName) == originalWire);
    }
  }
}

BOOST_AUTO_TEST_SUITE_END()
