#pragma once

#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"
#include "tests/integration-tests/ndnsf-integration-fixture.hpp"
#include "ndn-service-framework/ProtectedBlobManifest.hpp"
#include "ndn-service-framework/ProducerDataNaming.hpp"
#include "ndnsf-distributed-repo/RepoEncryptedLargeDataStore.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedKvPublication.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalArtifactPublisher.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "tests/unit-tests/spec204-protected-kv-fixture.hpp"
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/io.hpp>
#include <ndn-svs/security-options.hpp>
#include <ndn-svs/svspubsub.hpp>
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

namespace spec204_owned_kv {
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

class OwnedCorePublicationUser : public LocalServiceUser
{
public:
  using LocalServiceUser::LocalServiceUser;
  void seedOpaqueOriginalAuxiliaries(const ndn::Name& service, ndn::KeyChain& keys)
  {
    // Signed opaque storage fixtures only; real AA/NAC cases below prove real key packets.
    const auto key = prepareHybridSendKeyForTest(service, "REQUEST-LARGE");
    const ndn::Buffer opaque{0x00};
    auto content = prepareHybridMessageKeyPackets(service, getName(), makeAbeServiceAttribute(service, "SERVICE"),
      key.epochId, ndn::span<const uint8_t>(opaque.data(), opaque.size()), keys,
      ndn::security::signingByIdentity(getName()), ndn::time::milliseconds(1000), ndn::tlv::ContentType_Blob);
    auto ck = std::make_shared<ndn::Data>(ndn::Name(getName()).append("KEY").append("fixture-original-ck").appendVersion(1));
    ck->setContent(opaque); keys.sign(*ck, ndn::security::signingByIdentity(getName()));
    m_hybridMessageCrypto.cacheWrappedSendPackets(service, key.keyId, opaque, content, {ck});
  }
};

struct OwnedCorePublicationFixture
{
  fs::path root = fs::temp_directory_path() / ("spec204-core-actor-" + std::to_string(::getpid()));
  std::string spool = (root / "spool").string(), references = (root / "references").string();
  ScopedEnvironmentValue dataDir{"NDNSF_REQUEST_LARGE_DATA_DIR", spool.c_str()};
  ScopedEnvironmentValue keyDir{"NDNSF_DURABLE_KEY_REFERENCE_DIR", references.c_str()};
  ScopedEnvironmentValue retention{"NDNSF_REQUEST_LARGE_DATA_RETENTION_MS", "100"};
  ndn::KeyChain keys{"pib-memory:", "tpm-memory:"};
  ndn::DummyClientFace face{keys};
  ndn::security::Certificate actor = makeRsaIdentity(keys, ndn::Name("/spec204/core-producer"));
  ndn::security::Certificate authority = makeRsaIdentity(keys, ndn::Name("/spec204/core-authority"));
  ndn::Name service{"/spec204/core-model"};
  std::shared_ptr<OwnedCorePublicationUser> user;
  std::shared_ptr<ndnsf_distributed_repo::RepoNode> node;
  std::shared_ptr<ndnsf_distributed_repo::SqliteArtifactMetadataStore> journal;
  std::shared_ptr<EncryptedLargeDataRangeStore> store;
  LargeDataPublishOptions options;
  std::vector<std::uint8_t> input = std::vector<std::uint8_t>(40000, 0x39);
  std::string repoName = "/spec204/core-repo", owner = "/spec204/core-producer";
  const std::uint64_t grantExpiresAt = now() + 60000;
  std::vector<ndn::Name> resolved;

  OwnedCorePublicationFixture()
  {
    fs::remove_all(root); fs::create_directories(root);
    ndnsf_distributed_repo::StorageCapability capability;
    capability.repoNode = repoName; capability.repoMode = "persistent";
    capability.freeBytes = 64U << 20;
    const auto database = (root / "metadata.db").string();
    node = std::make_shared<ndnsf_distributed_repo::RepoNode>(ndn::Name("/spec204/core-service"),
      capability, ndnsf_distributed_repo::makeFilesystemRepoStore((root / "objects").string(),
        1U << 20, 1U << 20), repoName, actor.getIdentity().toUri(), database);
    store = node->createEncryptedLargeDataStore();
    journal = std::make_shared<ndnsf_distributed_repo::SqliteArtifactMetadataStore>(database, true, 16, false);
    user = std::make_shared<OwnedCorePublicationUser>(face, ndn::Name("/spec204/core-group"),
      actor, authority, "examples/trust-any.conf");
    user->useSigningKeyChainForSigningOnlyForTest(keys);
    user->attachLocalMockPubSubForTest(pubsub(face, keys));
    user->setEncryptedLargeDataRangeStore(store); user->init();
    face.processEvents(ndn::time::milliseconds(1));
    user->prepareHybridSendKeyForTest(service, "REQUEST-LARGE");
    options.retention = EncryptedLargeDataRetention::Durable;
    options.publicationIdentity = keyReferenceDigest("spec204-core-owned");
    options.contentName = materialContent("owned-material");
    options.manifestSuffix = materialManifestSuffix("owned-material");
  }
  ~OwnedCorePublicationFixture()
  {
    options.resolveDurableStorageGrant = {}; user.reset(); store.reset(); node.reset(); journal.reset();
    std::error_code error; fs::remove_all(root, error);
  }
  static std::uint64_t now()
  {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  }
  void authorize(const std::string& billingOwner)
  {
    owner = billingOwner;
    // This is the trusted fixture grant boundary, not network authentication.
    options.resolveDurableStorageGrant = [this, heldJournal = journal, heldUser = user,
      billingOwner](const ndn::Name& name, const std::string& writer,
                    const std::function<void()>& current) {
      if (face.getIoContext().get_executor().running_in_this_thread())
        throw std::runtime_error("fixture resolver ran on Face I/O");
      if (writer != actor.getIdentity().toUri(ndn::name::UriFormat::ALTERNATE) ||
          !actor.getIdentity().isPrefixOf(name))
        throw std::runtime_error("fixture producer binding mismatch");
      if (current) current();
      resolved.push_back(name);
      const auto grant = heldJournal->grantUserStorage(billingOwner, repoName,
        keyReferenceDigest(name.toUri(ndn::name::UriFormat::ALTERNATE)), writer,
        name.toUri(ndn::name::UriFormat::ALTERNATE), grantExpiresAt, now());
      return DurableStorageGrantSelector{grant.repoNode, grant.ownerIdentity,
        grant.grantId, grant.ownerEpoch};
    };
  }
  LargeDataPublishResult publish(const std::function<void()>& current = {})
  {
    auto& io = face.getIoContext(); io.restart();
    auto guard = boost::asio::make_work_guard(io);
    std::thread thread([&io] { io.run(); });
    struct Join {
      boost::asio::io_context& io; std::thread& thread;
      ~Join() { io.stop(); if (thread.joinable()) thread.join(); }
    } join{io, thread};
    return user->publishEncryptedLargeDataFromWorker(user->prepareServiceRequest(service.toUri()),
      input, "owned-core", ndn::time::milliseconds(100), options, current);
  }
  EncryptedLargeDataCommitOptions retainOptions(const EncryptedLargeDataLookupResult& hit)
  {
    EncryptedLargeDataCommitOptions value;
    value.retention = EncryptedLargeDataRetention::Durable;
    value.publicationIdentity = hit.publicationIdentity; value.protectionEpoch = hit.protectionEpoch;
    value.keyReferenceId = hit.keyReferenceId; value.keyReferenceVersion = hit.keyReferenceVersion;
    value.ciphertextManifestDigest = hit.ciphertextManifestDigest;
    value.servingLocator = hit.servingLocator; value.contentDigest = hit.contentDigest;
    value.plaintextSize = hit.plaintextSize; value.packetStorageFormat = hit.packetStorageFormat;
    value.manifestName = hit.manifestName; value.dataPrefix = hit.dataPrefix;
    value.packetWireBytes = hit.packetWireBytes;
    const auto writer = actor.getIdentity().toUri(ndn::name::UriFormat::ALTERNATE);
    const auto grant = journal->grantUserStorage(owner, repoName,
      keyReferenceDigest(ndn::Name(hit.manifestName).toUri(ndn::name::UriFormat::ALTERNATE)),
      writer, ndn::Name(hit.manifestName).toUri(ndn::name::UriFormat::ALTERNATE), grantExpiresAt, now());
    value.storageActor = DurableStorageActorContext{writer,
      {grant.repoNode, grant.ownerIdentity, grant.grantId, grant.ownerEpoch}};
    return value;
  }
};
} // namespace

namespace {
struct OwnerKvConversationFixture
{
  using Json = ndnsf::di::NativeJson;
  OwnedCorePublicationFixture& producer;
  ndnsf::di::NativeConversationConfig config;
  std::unique_ptr<ndnsf::di::NativeConversationCoordinator> coordinator;
  std::string checkpointWire;
  Json checkpoint;
  std::uint64_t time, expiry;
  std::vector<std::uint8_t> key = std::vector<std::uint8_t>(32, 0x47);
  std::map<std::string, std::string> placement;

  OwnerKvConversationFixture(OwnedCorePublicationFixture& f, const std::string& owner,
    std::map<std::string, std::string> rolePlacement = {})
    : producer(f), time(f.now()), expiry(time + 60000)
  {
    f.user->seedOpaqueOriginalAuxiliaries(f.service, f.keys);
    config.requesterIdentity = owner; config.serviceName = f.service.toUri();
    config.securityDomainDigest = keyReferenceDigest("security"); config.authenticationKeys = {key};
    config.nowMs = [this] { return time; };
    placement = rolePlacement.empty()
      ? std::map<std::string, std::string>{{"/role/A", f.actor.getIdentity().toUri()}, {"/role/B", f.actor.getIdentity().toUri()}}
      : std::move(rolePlacement);
    reopen(); seed(1); coordinator->restore();
  }
  void reopen(std::size_t quota = 64 * 1024 * 1024)
  {
    coordinator.reset(); config.journal.reset();
    config.journal = std::make_shared<ndnsf::di::NativeConversationJournal>(
      ndnsf::di::NativeConversationJournalConfig{producer.root / "conversation", "owner", {{"key", key}}, quota, true});
    coordinator = std::make_unique<ndnsf::di::NativeConversationCoordinator>(config);
  }
  void seed(std::uint64_t epoch)
  {
    // Trusted owner fixture data, not network receipt authentication or inference.
    std::ifstream input("tests/fixtures/spec182/conversation-oracle.json");
    const auto oracle = Json::parse(input).at("cases").at(2);
    auto cp = ndnsf::di::nativeParseJson(oracle.at("checkpointWire").get<std::string>());
    auto transcript = oracle.at("transcript");
    Json roles = Json::array(); for (const auto& [role, provider] : placement) roles.push_back(Json::array({role, provider}));
    const auto mapDigest = ndnsf::di::nativePlanningDigest(ndnsf::di::nativeCanonicalJson(roles));
    for (auto* value : {&cp, &transcript}) {
      (*value)["requesterIdentity"] = config.requesterIdentity; (*value)["serviceName"] = config.serviceName;
      (*value)["securityDomainDigest"] = config.securityDomainDigest; (*value)["planRoleMapDigest"] = mapDigest;
      (*value)["contextEpoch"] = epoch; (*value)["expiresAtMs"] = expiry;
    }
    cp["parentContextEpoch"] = epoch - 1; cp["issuedAtMs"] = time;
    transcript["createdAtMs"] = time;
    Json receipts = Json::array(), digests = Json::object();
    auto selectedRole = placement.begin();
    for (const auto& encoded : transcript.at("providerRoleReceipts")) {
      if (selectedRole == placement.end()) break;
      auto receipt = ndnsf::di::nativeParseJson(ndnsf::di::nativeConversationBase64Decode(encoded.get<std::string>()));
      for (const auto field : {"requesterIdentity", "serviceName", "securityDomainDigest", "planRoleMapDigest", "expiresAtMs"})
        receipt[field] = cp.at(field);
      if (!placement.count(receipt.at("roleName").get<std::string>())) receipt["roleName"] = selectedRole->first;
      receipt["providerIdentity"] = placement.at(receipt.at("roleName").get<std::string>()); ++selectedRole;
      receipt["parentContextEpoch"] = epoch - 1; receipt["successorContextEpoch"] = epoch;
      receipt.erase("receiptDigest"); receipt.erase("signature");
      const auto digest = ndnsf::di::nativePlanningDigest(ndnsf::di::nativeConversationCanonicalJson(receipt));
      digests[receipt.at("roleName").get<std::string>()] = digest;
      // Native receipts carry no inner signature; this is trusted owner fixture
      // metadata, not evidence of a newly verified Provider Data packet.
      receipt["receiptDigest"] = digest; receipt["signature"] = "";
      receipts.push_back(ndnsf::di::nativeConversationBase64Encode(ndnsf::di::nativeConversationCanonicalJson(receipt)));
    }
    cp["roleReceiptDigests"] = digests; cp.erase("checkpointDigest"); cp.erase("signature");
    checkpointWire = ndnsf::di::nativeSignConversationCheckpoint(cp, key);
    checkpoint = ndnsf::di::nativeReadConversationCheckpoint(checkpointWire, {key}, time);
    transcript["checkpointDigest"] = checkpoint.at("checkpointDigest"); transcript["providerRoleReceipts"] = receipts;
    ndnsf::di::nativeValidateConversationTranscript(transcript, checkpoint, 1);
    config.journal->appendConversation(checkpointWire, transcript, time, 1, placement);
  }
  std::string id() const { return checkpoint.at("conversationId").get<std::string>(); }
  std::string digest() const { return checkpoint.at("checkpointDigest").get<std::string>(); }
  fs::path log() const { return producer.root / "conversation" / "owner" / "journal.jsonl"; }
  ndnsf::di::NativeConversationContinuation continuation() const
  {
    ndnsf::di::NativeConversationContinuation value;
    value.mode = "APPEND_DELTA"; value.conversationId = id(); value.parentContextEpoch = 1;
    value.serviceName = config.serviceName;
    value.planRoleMapDigest = checkpoint.at("planRoleMapDigest").get<std::string>();
    value.parentCheckpointWire = checkpointWire; value.parentCheckpointDigest = digest();
    value.requestContractDigest = keyReferenceDigest("pending-request"); value.retentionDeadlineMs = expiry;
    value.generationId = std::string(32, '4');
    for (const auto& [role, provider] : placement) { (void)provider; value.expectedRoles.push_back(role); }
    value.canonicalTokenIds = coordinator->find(id())->checkpoint.transcript.at("canonicalTokenIds")
      .get<std::vector<std::int64_t>>(); value.canonicalTokenIds.push_back(99);
    return value;
  }
  auto publish(ndnsf::di::test::protectedKv::Fixture& kv)
  {
    std::map<std::string, ndnsf::di::NativeProtectedKvPublicationV1> result;
    for (const auto& [role, provider] : placement) {
      kv.state.originIdentity.providerIdentity = provider; kv.state.originIdentity.roleName = role;
      ndnsf::di::nativeExportPrivatePortableKvCheckpoint(kv.plain, kv.state, kv.portableLimits);
      const auto reference = ndnsf::di::nativeProtectPortableKvCheckpoint(kv.plain, kv.cipher, kv.certificate,
        ndn::Name(provider), ndnsf::di::nativeProtectedKvRoleBinding(checkpoint, role), kv.limits);
      const auto prepared = producer.user->prepareServiceRequest(producer.service.toUri());
      auto& io = producer.face.getIoContext(); io.restart(); auto guard = boost::asio::make_work_guard(io);
      std::thread thread([&] { io.run(); });
      struct Join { boost::asio::io_context& io; std::thread& thread;
        ~Join() { io.stop(); if (thread.joinable()) thread.join(); } } join{io, thread};
      result.emplace(role, ndnsf::di::nativePublishProtectedPortableKvCheckpoint(producer.user, prepared,
        kv.cipher, reference, ndnsf::di::NativeDurableStorageOwner{
          producer.owner, producer.options.resolveDurableStorageGrant}, kv.limits));
    }
    return result;
  }
};
} // namespace
} // namespace spec204_owned_kv
