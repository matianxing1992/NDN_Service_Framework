#include "tests/boost-test.hpp"
#include "ndn-service-framework/SecurityProfile.hpp"
#include "ndn-service-framework/ProducerDataNaming.hpp"
#include "ndn-service-framework/common.hpp"
#include "ndn-service-framework/RequestConfidentiality.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/utils.hpp"
#include <ndn-cxx/ims/in-memory-storage-fifo.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>
#include <ndn-cxx/util/io.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/validator-config.hpp>
#include <ndn-cxx/security/validity-period.hpp>
#include <openssl/sha.h>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <thread>
#include <type_traits>
#include <future>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <vector>

namespace ndn_service_framework::test {
// Reuse the existing production friend, without a new public test API.
struct InvocationNamesOracle
{
  template<typename Actor>
  static std::function<void()> blockHybridWorker(Actor& actor, bool ack = false)
  {
    auto& pool = [&]() -> BoundedWorkerPool& {
      if constexpr (std::is_same_v<Actor, ServiceUser>) {
        return ack ? actor.m_ackProcessingPool : actor.m_handlerPool;
      }
      else {
        return actor.m_handlerPool;
      }
    }();
    pool.setThreadCount(1);
    auto release = std::make_shared<std::promise<void>>();
    auto ready = std::make_shared<std::promise<void>>();
    auto readyFuture = ready->get_future();
    auto future = release->get_future().share();
    if (!pool.post([ready, future] { ready->set_value(); future.wait(); }))
      throw std::runtime_error("cannot block hybrid worker");
    readyFuture.get();
    return [release] { release->set_value(); };
  }
  template<typename Actor>
  static bool decrypt(Actor& actor, const ndn::Name& name, const ndn::Block& envelope,
                      const ndn::Name& sender, const ndn::Name& service,
                      const ndn::Name& id, std::function<void(const ndn::Buffer&)> success,
                      std::function<void(const std::string&)> failure)
  {
    return actor.decryptHybridMessage(name, envelope, sender, service, id,
                                      std::move(success), std::move(failure));
  }
  static void seedPublication(ServiceUser& user, const ndn::Name& id, const ndn::Name& service)
  {
    ServiceUser::PendingCall pending;
    pending.serviceName = service;
    user.m_pendingCalls.emplace(id, std::move(pending));
  }
  static std::optional<RequestNameV2>
  publicationContext(const ServiceUser& user, const ndn::Name& name)
  {
    return user.resolvePendingPublicationContext(name);
  }
  static std::optional<ResponseNameV2>
  replyContext(const ServiceUser& user, const ndn::Name& name, const std::string& category,
               const ndn::Name& producer, bool transport = false)
  {
    return user.resolvePendingReplyContext(name, category, producer, transport);
  }
  static void publishCapturedResponse(ServiceProvider& provider, const ndn::Name& name,
                                      ResponseNameV2 context)
  {
    provider.publishHybridEncodedMessageCandidates(name, {ndn::Buffer{'x'}}, std::move(context));
  }
  static void localCertificate(MessageValidator& validator,
                               const ndn::security::Certificate& certificate)
  {
    validator.m_validator.cacheUnverifiedCert(ndn::security::Certificate(certificate));
  }
  static void seed(ServiceUser& user, const ndn::Name& id, const ndn::Name& provider,
                   int timeoutMs, uint64_t deadlineUs, uint64_t nowUs)
  {
    ServiceUser::PendingCall pending;
    pending.serviceName = ndn::Name("/spec205/service");
    pending.providers = {provider};
    pending.timeoutMs = timeoutMs;
    pending.createdAtUs = nowUs;
    pending.requestDeadlineUs = deadlineUs;
    pending.acksHandler = [](const auto&) { return std::vector<RequestAckMessage>{}; };
    pending.ackDecryptsInFlight = 1;
    user.m_pendingCalls.emplace(id, std::move(pending));
  }
  static void seedCollaborationStatuses(ServiceUser& user, const ndn::Name& id,
                                        const ndn::Name& first, const ndn::Name& second)
  {
    ServiceUser::PendingCall pending;
    pending.isCollaboration = true;
    pending.serviceName = ndn::Name("/spec205/service");
    pending.expectedResponseProviders = {first, second};
    pending.selectionDigestsByProvider[first.toUri()] = "first-digest";
    SelectionExecutionStatus rejected;
    rejected.providerName = first;
    rejected.serviceName = pending.serviceName;
    rejected.requestId = id;
    rejected.selectionDigest = "first-digest";
    rejected.state = SelectionExecutionState::Rejected;
    rejected.message = "first provider rejected";
    pending.selectionStatusesByProvider[first.toUri()] = rejected;
    user.m_pendingCalls.emplace(id, std::move(pending));
  }
  static void recordCollaborationStatus(ServiceUser& user, const ndn::Name& id,
                                        const ndn::Name& provider,
                                        SelectionExecutionState state)
  {
    SelectionExecutionStatus status;
    status.providerName = provider;
    status.serviceName = ndn::Name("/spec205/service");
    status.requestId = id;
    status.selectionDigest = "second-digest";
    status.state = state;
    user.m_pendingCalls.at(id).selectionStatusesByProvider[provider.toUri()] = status;
  }
  static size_t uniqueSelectedAckCount(ServiceUser& user)
  {
    ServiceUser::PendingCall pending;
    ServiceUser::StoredAck first;
    first.providerName = ndn::Name("/provider/a");
    first.serviceName = ndn::Name("/spec205/service");
    first.requestId = ndn::Name("/spec215/request");
    auto second = first;
    second.providerName = ndn::Name("/provider/b");
    user.addCustomSelectedAck(pending, first);
    user.addCustomSelectedAck(pending, first);
    user.addCustomSelectedAck(pending, second);
    return pending.customSelectedAcks.size();
  }
  static bool throwingSelectorsFailClosed(ServiceUser& user)
  {
    ServiceUser::PendingCall byCandidate;
    byCandidate.ackCandidatesHandler = [](const auto&)
        -> std::vector<AckSelectionCandidate> {
      throw std::runtime_error("candidate selector failed");
    };
    const bool candidateSelected = user.evaluateCustomAckSelection(byCandidate);

    ServiceUser::PendingCall byMessage;
    byMessage.acksHandler = [](const auto&)
        -> std::vector<RequestAckMessage> {
      throw std::runtime_error("ACK selector failed");
    };
    const bool messageSelected = user.evaluateCustomAckSelection(byMessage);
    return !candidateSelected && byCandidate.timedOut &&
           byCandidate.customSelectedAcks.empty() &&
           !messageSelected && byMessage.timedOut &&
           byMessage.customSelectedAcks.empty();
  }
  static bool failedSelectorIsNotReentered(ServiceUser& user)
  {
    const ndn::Name id("/spec215/throwing-selector");
    ServiceUser::PendingCall pending;
    int invocations = 0;
    pending.ackCandidatesHandler = [&invocations](const auto&)
        -> std::vector<AckSelectionCandidate> {
      ++invocations;
      throw std::runtime_error("candidate selector failed");
    };
    user.m_pendingCalls.emplace(id, std::move(pending));
    const bool first = user.evaluateAckSelection(id);
    const bool repeated = user.evaluateAckSelection(id);
    return !first && !repeated && invocations == 1 &&
           user.m_pendingCalls.at(id).timedOut;
  }
  static void validator(ServiceUser& user, std::shared_ptr<MessageValidator> validator)
  { user.validator = std::move(validator); }
  static void validator(ServiceProvider& provider, std::shared_ptr<MessageValidator> validator)
  { provider.validator = std::move(validator); }
  static void cachePacket(ServiceProvider& provider, const ndn::Data& packet)
  {
    std::lock_guard<std::mutex> lock(provider._cache_mutex);
    provider.m_IMS.insert(packet);
  }
  static void ack(ServiceUser& user, const ndn::Name& id, const ndn::Name& provider,
                  RequestAckMessage message)
  {
    user.finishRequestAckOnEventLoop(provider, ndn::Name("/spec205/service"), id,
                                    std::move(message), {});
  }
  static uint64_t fetches(const ServiceUser& user)
  { return user.m_securityProfiles ? user.m_securityProfiles->fetchCount() : 0; }
  static size_t cacheSize(const ServiceUser& user)
  { return user.m_securityProfiles ? user.m_securityProfiles->size() : 0; }
  static size_t leases(const ServiceUser& user, const ndn::Name& id)
  { return user.m_pendingCalls.at(id).ackDecryptsInFlight; }
  static void seedResponse(ServiceUser& user, const ndn::Name& id, const ndn::Name& provider,
                           int timeoutMs, uint64_t deadlineUs, uint64_t nowUs)
  {
    seed(user, id, provider, timeoutMs, deadlineUs, nowUs);
    auto& pending = user.m_pendingCalls.at(id);
    pending.expectedResponseProviders = {provider};
    pending.responseDecryptProvidersInFlight = {provider};
    RequestCapabilities capabilities;
    capabilities.setField("SecurityProfileReferenceV1", "1");
    pending.requestMessage.setRequestCapabilities(capabilities);
  }
  static void response(ServiceUser& user, const ndn::Name& id, const ndn::Name& provider,
                       ResponseMessage message)
  {
    user.finishDecryptedResponseByName(
      makeResponseNameV2(provider, user.identity, user.m_pendingCalls.at(id).serviceName, id),
      id, std::move(message));
  }
  static size_t responseLeases(const ServiceUser& user, const ndn::Name& id)
  { return user.m_pendingCalls.at(id).responseDecryptProvidersInFlight.size(); }
  static void profileAudience(ServiceUser& user, size_t epoch,
                              const std::vector<ServiceAuthorizationRecord>& records,
                              const std::vector<std::pair<ndn::Name, ndn::Name>>& producerServices = {})
  {
    user.m_currentPolicyEpoch = epoch;
    BOOST_REQUIRE(user.m_authorizations.replacePermissions(tlv::UserPermission, epoch, records));
    user.replaceBasicProfileAudience(producerServices);
  }
  static void observeReader(ServiceUser& user, const ndn::Name& producer, const ndn::Name& service,
                            const RequestAckMessage& ack, const AckAuthenticationEvidence& evidence)
  { user.rememberBasicProfileReader(producer, service, ack, evidence); }
  static bool knownAudience(const ServiceUser& user, const ndn::Name& service)
  { return user.canUseBasicProfileReference(service); }
  static void controllerVersion(ServiceUser& user, const ControllerVersion& version)
  { user.m_controllerVersion = version; }
  static bool unselectedReaderAck(ServiceUser& user, const ndn::Name& id,
                                  const ndn::Name& producer, const RequestAckMessage& ack,
                                  const AckAuthenticationEvidence& evidence)
  {
    user.m_pendingCalls.at(id).requestMessage.setUserToken("pending-user-token");
    return user.handleRequestAckByName(makeRequestAckNameV2(producer, user.identity,
      user.m_pendingCalls.at(id).serviceName, id), ack, evidence);
  }
  static bool selectionUntouched(const ServiceUser& user, const ndn::Name& id)
  {
    const auto& pending = user.m_pendingCalls.at(id);
    return !pending.providerSelected && pending.selectedProvider.empty() && pending.requestAcks.empty();
  }
  static void prepareReaderIngress(ServiceUser& user, const ndn::Name& id, bool selected)
  {
    auto& pending = user.m_pendingCalls.at(id);
    RequestCapabilities capabilities;
    capabilities.setField("SecurityProfileReferenceV1", "1");
    pending.requestMessage.setRequestCapabilities(capabilities);
    pending.requestMessage.setUserToken("pending-user-token");
    pending.requestMessage.setPolicyEpoch(7);
    if (selected) {
      pending.selectedProvider = pending.providers.front();
      pending.providerSelected = true;
      pending.providers.clear(); // Exercise the selected guard independently.
    }
  }
  static void ingress(ServiceUser& user, const ndn::svs::SVSPubSub::SubscriptionData& publication)
  { user.OnRequestAck(publication); }
  static size_t readerTickets(const ServiceUser& user, const ndn::Name& id)
  { return user.m_pendingCalls.at(id).readerObservations.size(); }
  static bool readerSelectionUnchanged(const ServiceUser& user, const ndn::Name& id,
                                       const ndn::Name& selected, bool wasSelected)
  {
    const auto& pending = user.m_pendingCalls.at(id);
    return pending.providerSelected == wasSelected &&
      pending.selectedProvider == (wasSelected ? selected : ndn::Name{}) && pending.requestAcks.empty();
  }
  static void retireReaderCall(ServiceUser& user, const ndn::Name& id)
  { user.m_pendingCalls.at(id).hasResponse = true; }
  static void omitReaderCapability(ServiceUser& user, const ndn::Name& id)
  {
    RequestCapabilities caps;
    caps.setField("application.name", "qwen");
    user.m_pendingCalls.at(id).requestMessage.setRequestCapabilities(caps);
  }
  static void profileOnlyReader(ServiceUser& user, const ndn::Name& id,
                                 const ProducerSecurityProfile& profile)
  {
    omitReaderCapability(user, id);
    user.m_pendingCalls.at(id).requestMessage.setUserSecurityProfile(profile.getName(),
      std::make_shared<const EncryptionCertificateAdvertisement>(profile.advertisement()));
  }
  static bool basicProfile(ServiceUser& user, RequestMessage& message, const ndn::Name& service)
  { return user.prepareBasicProfileReference(message, service); }
  static void signingKeys(ServiceUser& user, ndn::security::KeyChain& keys, const ndn::Name& identity)
  { user.m_testSigningKeyChain = &keys; user.m_signingInfo = ndn::security::signingByIdentity(identity); }
  static void expireReaders(ServiceUser& user)
  { for (auto& [key, observation] : user.m_profileReaderObservations) observation.expiresAtMs = 0; }
  static void invalidateReaders(ServiceUser& user, const ndn::Name& service)
  {
    ControllerVersion version;
    version.controllerEpoch = 7;
    version.controllerGenerationTimestamp = 1;
    user.invalidateControllerScopedCaches(service, version, false, nullptr, false);
  }
};
namespace {
class ScopedProfileSchema
{
public:
  explicit ScopedProfileSchema(const ndn::security::Certificate& anchor)
  {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "ndnsf-profile-trust-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const char* directory = ::mkdtemp(buffer.data());
    if (directory == nullptr) throw std::runtime_error("cannot make profile trust directory");
    m_directory = directory;
    m_schemaPath = m_directory / "profile.conf";
    std::ostringstream encoded;
    ndn::io::save(anchor, encoded);
    auto base64 = encoded.str();
    base64.erase(std::remove(base64.begin(), base64.end(), '\n'), base64.end());
    std::ofstream schema(m_schemaPath);
    schema << "rule { id profile for data checker { type hierarchical "
              "sig-type rsa-sha256 } }\n"
              "trust-anchor { type base64 base64-string \"" << base64 << "\" }\n";
    schema.close();
    if (!schema) throw std::runtime_error("cannot write profile trust schema");
  }

  ~ScopedProfileSchema()
  {
    std::error_code ignored;
    std::filesystem::remove_all(m_directory, ignored);
  }

  std::string path() const { return m_schemaPath.string(); }

private:
  std::filesystem::path m_directory;
  std::filesystem::path m_schemaPath;
};

std::string hash(ndn::span<const uint8_t> bytes)
{
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(bytes.data(), bytes.size(), digest);
  std::ostringstream result;
  result << "sha256:" << std::hex << std::setfill('0');
  for (const auto byte : digest) result << std::setw(2) << static_cast<unsigned>(byte);
  return result.str();
}

struct ProfileFixture
{
  ndn::security::KeyChain keys{"pib-memory:spec205-profile", "tpm-memory:spec205-profile"};
  ndn::DummyClientFace face{keys};
  ndn::Name producer{"/spec205/profile/producer"};
  ProducerSecurityProfile profile;
  std::unique_ptr<ScopedProfileSchema> schema;
  std::shared_ptr<MessageValidator> validator;

  ProfileFixture()
  {
    const auto identity = keys.createIdentity(producer, ndn::RsaKeyParams(2048));
    profile.producer = producer;
    profile.certificate = identity.getDefaultKey().getDefaultCertificate();
    profile.capabilities = ProducerSecurityProfile::SUPPORTED_CAPABILITIES;
    keys.createIdentity(ndn::Name("/spec205"), ndn::RsaKeyParams(2048));
    schema = std::make_unique<ScopedProfileSchema>(profile.certificate);
    validator = std::make_shared<MessageValidator>(schema->path(), std::nullopt, &face);
  }

  auto packet() { return profile.makeData(keys, ndn::security::signingByIdentity(producer), ndn::time::seconds(60)); }
  // Keep the bounded processEvents timer alive across an empty-PIT callback;
  // invalidation can empty the PIT before the replacement Interest is posted.
  void pump() { face.processEvents(ndn::time::milliseconds(30), true); }
};

struct PlaintextAckScope
{
  std::optional<std::string> crypto, ack;
  PlaintextAckScope()
  {
    if (const auto* value = std::getenv("NDNSF_CRYPTO_DIAG")) crypto = value;
    if (const auto* value = std::getenv("NDNSF_DIAG_PLAINTEXT_ACK")) ack = value;
    setenv("NDNSF_CRYPTO_DIAG", "1", 1);
    setenv("NDNSF_DIAG_PLAINTEXT_ACK", "1", 1);
  }
  ~PlaintextAckScope()
  {
    if (crypto) setenv("NDNSF_CRYPTO_DIAG", crypto->c_str(), 1); else unsetenv("NDNSF_CRYPTO_DIAG");
    if (ack) setenv("NDNSF_DIAG_PLAINTEXT_ACK", ack->c_str(), 1); else unsetenv("NDNSF_DIAG_PLAINTEXT_ACK");
  }
};
} // namespace

BOOST_AUTO_TEST_SUITE(Spec205SecurityProfile)

BOOST_AUTO_TEST_CASE(CollaborationSnapshotIncludesUnreportedSelectedProvider)
{
  ProfileFixture f;
  ServiceUser user(ServiceUser::LocalMockTag{}, f.face, ndn::Name("/spec205/group"),
                   f.profile.certificate, f.profile.certificate, f.profile.certificate,
                   f.schema->path());
  const ndn::Name id("/spec215/request"), first("/provider/a"), second("/provider/b");
  InvocationNamesOracle::seedCollaborationStatuses(user, id, first, second);
  auto snapshot = user.GetCollaborationStatusSnapshot(id);
  BOOST_REQUIRE_EQUAL(snapshot.size(), 2);
  BOOST_CHECK(snapshot[0].state == SelectionExecutionState::Rejected);
  BOOST_CHECK(snapshot[1].state == SelectionExecutionState::Unknown);
  BOOST_CHECK(snapshot[1].providerName.equals(second));

  InvocationNamesOracle::recordCollaborationStatus(user, id, second,
                                                    SelectionExecutionState::Running);
  snapshot = user.GetCollaborationStatusSnapshot(id);
  BOOST_REQUIRE_EQUAL(snapshot.size(), 2);
  BOOST_CHECK(snapshot[1].state == SelectionExecutionState::Running);
}

BOOST_AUTO_TEST_CASE(CustomAckSelectionDeduplicatesProviderTuple)
{
  ProfileFixture f;
  ServiceUser user(ServiceUser::LocalMockTag{}, f.face, ndn::Name("/spec205/group"),
                   f.profile.certificate, f.profile.certificate, f.profile.certificate,
                   f.schema->path());
  BOOST_CHECK_EQUAL(InvocationNamesOracle::uniqueSelectedAckCount(user), 2);
}

BOOST_AUTO_TEST_CASE(ApplicationAckSelectorExceptionsFailClosed)
{
  ProfileFixture f;
  ServiceUser user(ServiceUser::LocalMockTag{}, f.face, ndn::Name("/spec205/group"),
                   f.profile.certificate, f.profile.certificate, f.profile.certificate,
                   f.schema->path());
  BOOST_CHECK(InvocationNamesOracle::throwingSelectorsFailClosed(user));
  BOOST_CHECK(InvocationNamesOracle::failedSelectorIsNotReentered(user));
}

BOOST_FIXTURE_TEST_CASE(BoundedHybridDecryptDoesNotWaitForApplicationWorker, ProfileFixture)
{
  const ndn::Name sender("/spec205/sender"), service("/service"), id("/request");
  const ndn::Buffer key(32, 0x5a);
  const std::string keyId = "bounded-key", epoch = "bounded-epoch";
  auto check = [&](auto& actor, const std::string& type, const ndn::Name& name) {
    actor.cacheHybridReceiveKeyForTest(keyId, epoch, key);
    struct Release
    {
      std::function<void()> function;
      ~Release() { if (function) function(); }
      void now() { auto callback = std::move(function); callback(); }
    } release{InvocationNamesOracle::blockHybridWorker(actor, type == "ACK")};
    auto envelope = [&](size_t size, bool damaged = false) {
      ndn::Buffer plaintext(size);
      std::fill(plaintext.begin(), plaintext.end(), 0x42);
      const auto ad = hybridAssociatedData(name, type, id, service, sender, keyId, epoch);
      const auto encrypted = hybridAesGcmEncrypt(key, plaintext, ad);
      HybridMessageEnvelope result;
      result.setKeyId(keyId); result.setEpochId(epoch); result.setMessageType(type);
      result.setNonce(encrypted.nonce); result.setAuthTag(encrypted.tag);
      auto ciphertext = encrypted.ciphertext;
      if (damaged) ciphertext.front() ^= 1;
      result.setCipherText(ciphertext);
      return result.WireEncode();
    };
    size_t successes = 0, failures = 0;
    bool inside = false;
    auto submit = [&](size_t size, bool damaged = false, bool wrongContext = false) {
      boost::asio::post(face.getIoContext(), [&, size, damaged, wrongContext] {
        inside = true;
        BOOST_CHECK(InvocationNamesOracle::decrypt(actor, name, envelope(size, damaged),
          sender, service, wrongContext ? ndn::Name("/another-request") : id,
          [&, size](const ndn::Buffer& plaintext) {
            BOOST_CHECK(!inside);
            BOOST_CHECK(face.getIoContext().get_executor().running_in_this_thread());
            BOOST_CHECK_EQUAL(plaintext.size(), size);
            BOOST_CHECK(std::all_of(plaintext.begin(), plaintext.end(),
                                    [](uint8_t value) { return value == 0x42; }));
            ++successes;
          }, [&](const std::string&) { BOOST_CHECK(!inside); ++failures; }));
        inside = false;
      });
      pump();
    };
    submit(4096);
    BOOST_CHECK_EQUAL(successes, 1);
    submit(64, true);
    submit(64, false, true);
    BOOST_CHECK_EQUAL(failures, 2);
    submit(4097);
    BOOST_CHECK_EQUAL(successes, 1);
    release.now();
    for (size_t attempt = 0; attempt < 30 && successes != 2; ++attempt) pump();
    BOOST_CHECK_EQUAL(successes, 2);
    BOOST_CHECK_EQUAL(failures, 2);
  };
  {
    ServiceUser user(ServiceUser::LocalMockTag{}, face, ndn::Name("/spec205/group"),
      profile.certificate, profile.certificate, profile.certificate, schema->path());
    check(user, "ACK", makeRequestAckNameV2(sender, producer, service, id));
    check(user, "RESPONSE", makeResponseNameV2(sender, producer, service, id));
  }
  {
    ServiceProvider provider(ServiceProvider::LocalMockTag{}, face, ndn::Name("/spec205/group"),
      profile.certificate, profile.certificate, schema->path());
    check(provider, "REQUEST", makeRequestNameV2(sender, service, id));
  }
}


BOOST_FIXTURE_TEST_CASE(ImsLowerBoundPreservesFullNameRange, ProfileFixture)
{
  for (const bool reverseInsertion : {false, true}) {
    ndn::InMemoryStorageFifo cache(32);
    const ndn::InMemoryStorage& storage = cache;
    const ndn::Name name("/range/item");
    BOOST_CHECK(storage.lowerBound(name) == storage.end());
    const auto makePacket = [&](const ndn::Name& packetName, uint8_t value) {
      auto packet = std::make_shared<ndn::Data>(packetName);
      packet->setContent(ndn::Buffer{value});
      keys.sign(*packet, ndn::security::signingByIdentity(producer));
      return packet;
    };
    const ndn::Buffer digest(32, 0x5a);
    auto typedChild = ndn::Name(name).appendImplicitSha256Digest(
      ndn::span<const uint8_t>(digest.data(), digest.size())).append("child");
    std::vector<std::shared_ptr<ndn::Data>> packets{
      makePacket(ndn::Name("/range"), 0), makePacket(name, 1), makePacket(name, 2),
      makePacket(ndn::Name(name).append("child"), 3), makePacket(typedChild, 4),
      makePacket(ndn::Name(name).appendSegment(0), 5), makePacket(ndn::Name("/range/next"), 6)};
    if (reverseInsertion) std::reverse(packets.begin(), packets.end());
    std::vector<ndn::Name> fullNames;
    for (const auto& packet : packets) {
      cache.insert(*packet);
      fullNames.push_back(packet->getFullName());
    }
    std::sort(fullNames.begin(), fullNames.end());
    auto queries = fullNames;
    queries.insert(queries.end(), {ndn::Name(), name, ndn::Name(name).append("absent"),
                                   typedChild, ndn::Name(fullNames.back()).append("after")});
    for (const auto& query : queries) {
      const auto expected = std::lower_bound(fullNames.begin(), fullNames.end(), query);
      const auto found = storage.lowerBound(query);
      if (expected == fullNames.end()) {
        BOOST_CHECK(found == storage.end());
      }
      else {
        BOOST_REQUIRE(found != storage.end());
        BOOST_CHECK_EQUAL(found->getFullName(), *expected);
      }
    }
    size_t exact = 0, descendants = 0;
    for (auto entry = storage.lowerBound(name);
         entry != storage.end() && name.isPrefixOf(entry->getFullName()); ++entry) {
      ++descendants;
      if (entry->getName() == name) ++exact;
    }
    BOOST_CHECK_EQUAL(exact, 2);
    BOOST_CHECK_EQUAL(descendants, 5);
  }
}

BOOST_FIXTURE_TEST_CASE(SignedAppReplayPreservesWireAndRejectsAllConflicts, ProfileFixture)
{
  ServiceUser user(ServiceUser::LocalMockTag{}, face, ndn::Name("/spec205/group"),
                   profile.certificate, profile.certificate, profile.certificate,
                   schema->path());
  InvocationNamesOracle::validator(user, validator);
  InvocationNamesOracle::signingKeys(user, keys, producer);
  const ndn::Buffer payload{0x41, 0x42};
  const auto freshness = ndn::time::milliseconds(60000);
  const auto makePacket = [&](const ndn::Name& name) {
    auto packet = std::make_shared<ndn::Data>(name);
    packet->setContent(payload);
    packet->setFreshnessPeriod(freshness);
    keys.sign(*packet, ndn::security::signingByIdentity(producer));
    return packet;
  };
  const auto name = ndn::Name(producer).append("NDNSF-DI").append("input").append("replay");
  for (const auto& neighbor : {name.getPrefix(-1), ndn::Name(name).append("child"),
       ndn::Name(producer).append("a"), ndn::Name(producer).append("zzzzzzzzzzzzzzzzzzzz")}) {
    user.cacheDataForTest(*makePacket(neighbor), freshness);
  }
  BOOST_REQUIRE_NO_THROW(user.publishSignedAppData(name, payload, freshness));
  pump();
  BOOST_REQUIRE(!face.sentData.empty());
  const auto original = face.sentData.back().wireEncode();
  const auto sent = face.sentData.size();
  BOOST_REQUIRE_NO_THROW(user.publishSignedAppData(name, payload, freshness));
  pump();
  BOOST_REQUIRE_EQUAL(face.sentData.size(), sent + 1);
  BOOST_CHECK(face.sentData.back().wireEncode() == original);
  const auto immutableConflict = [](const std::invalid_argument& error) {
    return std::string(error.what()) == "immutable signed APP Data name conflict";
  };
  BOOST_CHECK_EXCEPTION(user.publishSignedAppData(name, ndn::Buffer{0x43}, freshness),
                        std::invalid_argument, immutableConflict);
  BOOST_CHECK_EXCEPTION(user.publishSignedAppData(name, payload, freshness + ndn::time::seconds(1)),
                        std::invalid_argument, immutableConflict);
  for (const bool reverseInsertion : {false, true}) {
    const auto variantName = ndn::Name(name).append(reverseInsertion ? "reverse" : "forward");
    auto first = makePacket(variantName);
    auto second = std::make_shared<ndn::Data>(*first);
    auto info = second->getSignatureInfo();
    const ndn::Buffer nonce(8, 0x37);
    info.setNonce(ndn::span<const uint8_t>(nonce.data(), nonce.size()));
    auto signing = ndn::security::signingByIdentity(producer);
    signing.setSignatureInfo(info);
    keys.sign(*second, signing);
    BOOST_REQUIRE(first->getFullName() != second->getFullName());
    BOOST_REQUIRE(first->getContent() == second->getContent());
    BOOST_REQUIRE(first->getMetaInfo().wireEncode() == second->getMetaInfo().wireEncode());
    BOOST_REQUIRE(validator->isProducerSignerAuthorized(*second, producer));
    user.cacheDataForTest(*(reverseInsertion ? second : first), freshness);
    user.cacheDataForTest(*(reverseInsertion ? first : second), freshness);
    BOOST_CHECK_EXCEPTION(user.publishSignedAppData(variantName, payload, freshness),
                          std::invalid_argument, immutableConflict);
  }
  const auto unauthorizedName = ndn::Name(name).append("wrong-signer");
  auto unauthorized = makePacket(unauthorizedName);
  keys.sign(*unauthorized, ndn::security::signingByIdentity(ndn::Name("/spec205")));
  user.cacheDataForTest(*unauthorized, freshness);
  BOOST_CHECK_EXCEPTION(user.publishSignedAppData(unauthorizedName, payload, freshness),
                        std::invalid_argument, immutableConflict);
  pump();
  BOOST_CHECK_EQUAL(face.sentData.size(), sent + 1);
  BOOST_CHECK(face.sentData.back().wireEncode() == original);
}

BOOST_FIXTURE_TEST_CASE(PublicationContextResolvesRequestSelectionAndDecision, ProfileFixture)
{
  ServiceUser user(ServiceUser::LocalMockTag{}, face, ndn::Name("/spec205/group"),
                   profile.certificate, profile.certificate, profile.certificate,
                   schema->path());
  const ndn::Name service("/spec205/service"), provider("/spec205/provider");
  for (unsigned index = 0; index < 64; ++index) {
    InvocationNamesOracle::seedPublication(user,
      ndn::Name("/unrelated").appendNumber(index), ndn::Name("/other/service"));
  }
  for (const ndn::Name id : {ndn::Name("/17"), ndn::Name("/arbitrary/NDNSF/REQUEST/tail")}) {
    InvocationNamesOracle::seedPublication(user, id, service);
    for (const auto& name : {
           makeRequestNameV2(producer, service, id),
           makeServiceSelectionNameV2(producer, provider, service, id),
           makeServiceSelectionDecisionNameV2(producer, provider, service, id, 1)}) {
      const auto context = InvocationNamesOracle::publicationContext(user, name);
      BOOST_REQUIRE(context.has_value());
      BOOST_CHECK_EQUAL(context->requesterName, producer);
      BOOST_CHECK_EQUAL(context->serviceName, service);
      BOOST_CHECK_EQUAL(context->requestId, id);
    }
  }
  const auto request = makeRequestNameV2(producer, service, ndn::Name("/17"));
  BOOST_CHECK(!InvocationNamesOracle::publicationContext(user,
    makeRequestNameV2(ndn::Name("/other/actor"), service, ndn::Name("/17"))));
  BOOST_CHECK(!InvocationNamesOracle::publicationContext(user,
    makeRequestNameV2(producer, ndn::Name("/other/service"), ndn::Name("/17"))));
  BOOST_CHECK(!InvocationNamesOracle::publicationContext(user,
    makeRequestNameV2(producer, service, ndn::Name("/missing"))));
  BOOST_CHECK(!InvocationNamesOracle::publicationContext(user, request.getPrefix(-1)));
  const auto selection = makeServiceSelectionNameV2(producer, provider, service, ndn::Name("/17"));
  for (const char* invalid : {"0", "01", "-1", "18446744073709551616"}) {
    BOOST_CHECK(!InvocationNamesOracle::publicationContext(user, ndn::Name(selection).append(invalid)));
  }
  BOOST_CHECK(!InvocationNamesOracle::publicationContext(user,
    ndn::Name(selection).appendSequenceNumber(1)));
  const auto malformedRecipient = ndn::Name(producer).append("NDNSF").append("SELECTION")
    .append("not-an-encoded-name").append(service).append("17");
  BOOST_CHECK(!InvocationNamesOracle::publicationContext(user, malformedRecipient));
}

BOOST_FIXTURE_TEST_CASE(PublicationContextRejectsAmbiguousServiceAndDecisionBoundaries, ProfileFixture)
{
  ServiceUser user(ServiceUser::LocalMockTag{}, face, ndn::Name("/spec205/group"),
                   profile.certificate, profile.certificate, profile.certificate,
                   schema->path());
  const ndn::Name service("/service"), longerService("/service/long");
  const ndn::Name provider("/spec205/provider"), id("/tail");
  InvocationNamesOracle::seedPublication(user, id, longerService);
  InvocationNamesOracle::seedPublication(user, ndn::Name("/long/tail"), service);
  for (const auto& name : {
         makeRequestNameV2(producer, longerService, id),
         makeServiceSelectionNameV2(producer, provider, longerService, id),
         makeServiceSelectionDecisionNameV2(producer, provider, longerService, id, 1)}) {
    BOOST_CHECK_EXCEPTION(InvocationNamesOracle::publicationContext(user, name), std::runtime_error,
      [](const std::runtime_error& error) {
        return std::string(error.what()) == "ambiguous hybrid publication context";
      });
  }
  const ndn::Name decisionId("/numeric/17");
  InvocationNamesOracle::seedPublication(user, decisionId, service);
  const auto decision = makeServiceSelectionDecisionNameV2(producer, provider, service, decisionId, 1);
  const auto context = InvocationNamesOracle::publicationContext(user, decision);
  BOOST_REQUIRE(context.has_value());
  BOOST_CHECK_EQUAL(context->requestId, decisionId);
  InvocationNamesOracle::seedPublication(user, ndn::Name(decisionId).append("1"), service);
  BOOST_CHECK_EXCEPTION(InvocationNamesOracle::publicationContext(user, decision), std::runtime_error,
    [](const std::runtime_error& error) {
      return std::string(error.what()) == "ambiguous hybrid publication context";
    });
}

BOOST_FIXTURE_TEST_CASE(ReplyContextResolvesOnlyExactRecipientServiceAndProducer, ProfileFixture)
{
  ServiceUser user(ServiceUser::LocalMockTag{}, face, ndn::Name("/spec205/group"),
                   profile.certificate, profile.certificate, profile.certificate,
                   schema->path());
  const ndn::Name service("/service/NDNSF/ACK/model");
  const ndn::Name provider("/provider/NDNSF/RESPONSE/actor");
  for (unsigned index = 0; index < 64; ++index) {
    InvocationNamesOracle::seedPublication(user,
      ndn::Name("/unrelated").appendNumber(index), ndn::Name("/other/service"));
  }
  for (const auto& id : {ndn::Name("/17"), ndn::Name("/arbitrary/NDNSF/RESPONSE/tail")}) {
    InvocationNamesOracle::seedPublication(user, id, service);
    const auto ackName = makeRequestAckNameV2(provider, producer, service, id);
    const auto responseName = makeResponseNameV2(provider, producer, service, id);
    const auto ack = user.parsePendingAckName(ackName);
    const auto response = user.parsePendingResponseName(responseName);
    BOOST_REQUIRE(ack.has_value());
    BOOST_REQUIRE(response.has_value());
    BOOST_CHECK_EQUAL(ack->providerName, provider);
    BOOST_CHECK_EQUAL(ack->requesterName, producer);
    BOOST_CHECK_EQUAL(ack->serviceName, service);
    BOOST_CHECK_EQUAL(ack->requestId, id);
    BOOST_CHECK_EQUAL(response->providerName, provider);
    BOOST_CHECK_EQUAL(response->requesterName, producer);
    BOOST_CHECK_EQUAL(response->serviceName, service);
    BOOST_CHECK_EQUAL(response->requestId, id);
    BOOST_CHECK(InvocationNamesOracle::replyContext(user, ackName, "ACK", provider));
    BOOST_CHECK(InvocationNamesOracle::replyContext(user, responseName, "RESPONSE", provider));
    BOOST_CHECK(!InvocationNamesOracle::replyContext(user, ackName, "ACK", ndn::Name("/wrong")));
    BOOST_CHECK(!InvocationNamesOracle::replyContext(user, responseName, "RESPONSE", ndn::Name("/wrong")));
    BOOST_CHECK(!user.parsePendingAckName(makeRequestAckNameV2(
      provider, ndn::Name("/other/user"), service, id)));
    BOOST_CHECK(!user.parsePendingResponseName(makeResponseNameV2(
      provider, ndn::Name("/other/user"), service, id)));
    BOOST_CHECK(!user.parsePendingAckName(makeRequestAckNameV2(
      provider, producer, ndn::Name("/other/service"), id)));
    BOOST_CHECK(!user.parsePendingResponseName(makeResponseNameV2(
      provider, producer, ndn::Name("/other/service"), id)));
  }
}

BOOST_FIXTURE_TEST_CASE(ReplyContextRejectsAmbiguousServiceAndTransportBoundaries, ProfileFixture)
{
  ServiceUser user(ServiceUser::LocalMockTag{}, face, ndn::Name("/spec205/group"),
                   profile.certificate, profile.certificate, profile.certificate,
                   schema->path());
  const ndn::Name service("/service"), longerService("/service/long");
  const ndn::Name provider("/provider"), id("/tail");
  InvocationNamesOracle::seedPublication(user, id, longerService);
  InvocationNamesOracle::seedPublication(user, ndn::Name("/long/tail"), service);
  const auto ack = makeRequestAckNameV2(provider, producer, longerService, id);
  const auto response = makeResponseNameV2(provider, producer, longerService, id);
  BOOST_CHECK(!user.parsePendingAckName(ack));
  BOOST_CHECK(!user.parsePendingResponseName(response));
  BOOST_CHECK(!InvocationNamesOracle::replyContext(user, ack, "ACK", provider));
  BOOST_CHECK(!InvocationNamesOracle::replyContext(user, response, "RESPONSE", provider));

  const ndn::Name transportId("/transport/request");
  InvocationNamesOracle::seedPublication(user, transportId, service);
  const auto transported = makeResponseNameV2(provider, producer, service, transportId)
    .appendVersion(7).appendSegment(0);
  BOOST_CHECK(!user.parsePendingResponseName(transported));
  const auto context = InvocationNamesOracle::replyContext(user, transported, "RESPONSE", provider, true);
  BOOST_REQUIRE(context.has_value());
  BOOST_CHECK_EQUAL(context->requestId, transportId);
  BOOST_CHECK(!InvocationNamesOracle::replyContext(user, transported, "RESPONSE", ndn::Name("/wrong"), true));
  const auto typedId = ndn::Name(transportId).appendVersion(7).appendSegment(0);
  InvocationNamesOracle::seedPublication(user, typedId, service);
  const auto exact = user.parsePendingResponseName(transported);
  BOOST_REQUIRE(exact.has_value());
  BOOST_CHECK_EQUAL(exact->requestId, typedId);
  BOOST_CHECK(!InvocationNamesOracle::replyContext(user, transported, "RESPONSE", provider, true));
}

BOOST_FIXTURE_TEST_CASE(WrappedKeyReplayChecksAllVariantsAmongUnrelatedCachedPackets, ProfileFixture)
{
  ServiceUser user(ServiceUser::LocalMockTag{}, face, ndn::Name("/spec205/group"),
                   profile.certificate, profile.certificate, profile.certificate,
                   schema->path());
  ServiceProvider provider(ServiceProvider::LocalMockTag{}, face, ndn::Name("/spec205/group"),
                           profile.certificate, profile.certificate, profile.certificate,
                           schema->path());
  InvocationNamesOracle::validator(user, validator);
  InvocationNamesOracle::validator(provider, validator);
  const auto exercise = [&](auto& actor, auto insert) {
    const auto makePacket = [&](const ndn::Name& name, std::uint8_t value) {
      auto packet = std::make_shared<ndn::Data>(name);
      packet->setContent(ndn::Buffer{value});
      keys.sign(*packet, ndn::security::signingByIdentity(producer));
      return packet;
    };
    // Include both unequal component counts and equally sized unrelated names.
    for (unsigned index = 0; index < 24; ++index) {
      auto name = ndn::Name(producer).append("NDNSF").append("cache-fixture");
      for (unsigned component = 0; component < index % 4; ++component) {
        name.append("part");
      }
      name.appendNumber(index);
      insert(*makePacket(name, 0x31));
    }
    ndn::nacabe::SPtrVector<ndn::Data> noKeys;
    for (unsigned variant = 0; variant < 3; ++variant) {
      const bool originalFirst = variant != 1;
      auto name = ndn::Name(producer).append("NDNSF").append("wrapped-key")
        .append(originalFirst ? "original-first" : "conflict-first");
      if (variant == 2) {
        const ndn::Buffer digest(32, 0x61);
        name.appendImplicitSha256Digest(ndn::span<const uint8_t>(digest.data(), digest.size()));
      }
      const auto original = makePacket(name, 0x41);
      const auto conflicting = makePacket(name, 0x42);
      const auto before = makePacket(ndn::Name(producer).append("NDNSF").append("aa"), 0x54);
      const auto after = makePacket(ndn::Name(producer).append("NDNSF")
        .append("zzzzzzzzzzzzzzzzzzzz"), 0x55);
      BOOST_REQUIRE(before->getFullName() < name);
      BOOST_REQUIRE(original->getFullName() < after->getFullName());
      insert(*before);
      insert(*after);
      // Prefix neighbors must neither conflict nor hide the exact-name variants.
      insert(*makePacket(name.getPrefix(-1), 0x51));
      insert(*makePacket(ndn::Name(name).append("child"), 0x52));
      insert(*makePacket(ndn::Name(name.getPrefix(-1)).append("neighbor"), 0x53));
      BOOST_REQUIRE(original->getFullName() != conflicting->getFullName());
      ndn::nacabe::SPtrVector<ndn::Data> packets{original};
      if (originalFirst) {
        BOOST_REQUIRE_NO_THROW(actor.serveDataWithIMS(packets, noKeys));
        BOOST_REQUIRE_NO_THROW(actor.serveDataWithIMS(packets, noKeys));
        insert(*conflicting);
      }
      else {
        insert(*conflicting);
        insert(*original);
      }
      // Both digest variants coexist. A single arbitrary find(Name) is insufficient.
      BOOST_CHECK_EXCEPTION(actor.serveDataWithIMS(packets, noKeys), std::invalid_argument,
        [](const std::invalid_argument& error) {
          return std::string(error.what()) == "immutable wrapped key original conflict";
        });
    }
  };
  exercise(user, [&](const ndn::Data& packet) {
    user.cacheDataForTest(packet, ndn::time::milliseconds(60000));
  });
  exercise(provider, [&](const ndn::Data& packet) {
    InvocationNamesOracle::cachePacket(provider, packet);
  });
}

BOOST_FIXTURE_TEST_CASE(CanonicalProducerNameAndOriginalCertificate, ProfileFixture)
{
  const auto wire = profile.wireEncode();
  const auto decoded = ProducerSecurityProfile::wireDecode(wire);
  BOOST_CHECK(decoded.certificate.wireEncode() == profile.certificate.wireEncode());
  BOOST_CHECK_EQUAL(decoded.producer, producer);
  BOOST_CHECK_EQUAL(decoded.getName(), profile.getName());
  const auto parsed = parseProducerDataName(profile.getName(), producer);
  BOOST_REQUIRE(parsed.has_value());
  BOOST_CHECK_EQUAL(parsed->category, "SECURITY");
  BOOST_CHECK_EQUAL(ndn::readString(parsed->suffix[0]), "name=encryption-profile");
  BOOST_CHECK_EQUAL(parsed->suffix[0].toUri(), "name%3Dencryption-profile");
  BOOST_CHECK_EQUAL(parsed->suffix[1].value_size(), 32);
  const auto cert = profile.certificate.wireEncode();
  BOOST_CHECK_EQUAL(profile.advertisement().certificateDigest,
    hash(ndn::span<const uint8_t>(cert.data(), cert.size())));
  const auto key = profile.certificate.getPublicKey();
  BOOST_CHECK_EQUAL(profile.publicKeyDigest(), hash(ndn::span<const uint8_t>(key.data(), key.size())));
  auto changed = profile;
  changed.capabilities = 0;
  BOOST_CHECK(changed.getName() != profile.getName());
  changed.producer = ndn::Name("/other");
  BOOST_CHECK_THROW(changed.wireEncode(), std::exception);
  changed = profile;
  changed.capabilities = 8;
  BOOST_CHECK_THROW(changed.wireEncode(), std::exception);
}

BOOST_FIXTURE_TEST_CASE(CanonicalParserRejectsReorderDuplicateAndUnknownSchema, ProfileFixture)
{
  const auto wire = profile.wireEncode();
  wire.parse();
  auto fields = wire.elements();
  for (int variant = 0; variant < 3; ++variant) {
    ndn::Block invalid(ProducerSecurityProfile::TYPE);
    if (variant == 0) std::swap(fields[1], fields[2]);
    for (size_t i = 0; i < fields.size(); ++i) {
      if (variant == 2 && i == 0) invalid.push_back(ndn::makeNonNegativeIntegerBlock(0xF851, 2));
      else invalid.push_back(fields[i]);
    }
    if (variant == 1) invalid.push_back(fields.back());
    invalid.encode();
    BOOST_CHECK_THROW(ProducerSecurityProfile::wireDecode(invalid), std::exception);
    fields = wire.elements();
  }
}

BOOST_FIXTURE_TEST_CASE(SingleFlightAndWarmHitUseSameValidatedObjectWithoutInterest, ProfileFixture)
{
  SecurityProfileStore store(face, validator);
  const auto data = packet();
  std::shared_ptr<const SecurityProfileStore::Record> first, second, warm;
  size_t successes = 0;
  const auto callback = [&](auto& target) {
    return [&](auto record, const std::string& error) {
      BOOST_CHECK_MESSAGE(error.empty(), error);
      target = record;
      if (record) ++successes;
    };
  };
  store.resolve(data->getName(), producer, ndn::time::seconds(1), callback(first));
  store.resolve(data->getName(), producer, ndn::time::seconds(1), callback(second));
  BOOST_CHECK_EQUAL(store.fetchCount(), 1);
  pump();
  BOOST_REQUIRE_EQUAL(face.sentInterests.size(), 1);
  BOOST_CHECK(!face.sentInterests.back().getCanBePrefix());
  BOOST_CHECK(!face.sentInterests.back().getMustBeFresh());
  face.receive(*data);
  pump();
  BOOST_REQUIRE_EQUAL(successes, 2);
  BOOST_REQUIRE(first != nullptr);
  BOOST_CHECK(first == second);
  BOOST_CHECK(first->packet->wireEncode() == data->wireEncode());
  const auto interests = face.sentInterests.size();
  store.resolve(data->getName(), producer, ndn::time::seconds(1), callback(warm));
  BOOST_REQUIRE(warm != nullptr);
  BOOST_CHECK(warm == first);
  BOOST_CHECK_EQUAL(store.fetchCount(), 1);
  BOOST_CHECK_EQUAL(face.sentInterests.size(), interests);
  store.invalidate();
  BOOST_CHECK_EQUAL(store.size(), 0);
}

BOOST_FIXTURE_TEST_CASE(RejectSameNameChangedContentAndUnauthorizedSigner, ProfileFixture)
{
  for (int variant = 0; variant < 2; ++variant) {
    SecurityProfileStore store(face, validator);
    auto data = std::make_shared<ndn::Data>(*packet());
    if (variant == 0) {
      auto changed = profile;
      changed.capabilities = 0;
      const auto content = changed.wireEncode();
      data->setContent(ndn::span<const uint8_t>(content.data(), content.size()));
      keys.sign(*data, ndn::security::signingByIdentity(producer));
    }
    else keys.sign(*data, ndn::security::signingByIdentity(ndn::Name("/spec205")));
    size_t rejected = 0;
    store.resolve(data->getName(), producer, ndn::time::seconds(1),
      [&](auto result, const std::string& error) { if (!result && !error.empty()) ++rejected; });
    pump();
    size_t nextInterest = face.sentInterests.size();
    face.receive(*data);
    pump();
    if (variant == 1) {
      const auto foreign = keys.getPib().getIdentity(ndn::Name("/spec205"))
        .getDefaultKey().getDefaultCertificate();
      bool certificateRequested = false;
      for (int attempt = 0; attempt < 12 && rejected == 0; ++attempt) {
        while (nextInterest < face.sentInterests.size()) {
          const auto interest = face.sentInterests[nextInterest++];
          if (interest.matchesData(foreign)) {
            certificateRequested = true;
            face.receive(foreign);
          }
        }
        pump();
      }
      BOOST_CHECK(certificateRequested);
    }
    BOOST_CHECK_EQUAL(rejected, 1);
    BOOST_CHECK_EQUAL(store.size(), 0);
  }
}

BOOST_FIXTURE_TEST_CASE(TimeoutInvalidateAndCloseHaveNoLateOwnerCallbacks, ProfileFixture)
{
  const auto data = packet();
  SecurityProfileStore store(face, validator);
  size_t failures = 0;
  store.resolve(data->getName(), producer, ndn::time::milliseconds(5),
    [&](auto record, const std::string& error) { if (!record && !error.empty()) ++failures; });
  pump();
  BOOST_CHECK_EQUAL(failures, 1);
  store.resolve(data->getName(), producer, ndn::time::seconds(1),
    [&](auto record, const std::string& error) { if (!record && !error.empty()) ++failures; });
  store.invalidate();
  BOOST_CHECK_EQUAL(failures, 2);
  store.resolve(data->getName(), producer, ndn::time::seconds(1),
    [&](auto, const auto&) { ++failures; });
  store.close();
  face.receive(*data);
  pump();
  BOOST_CHECK_EQUAL(failures, 2);
  BOOST_CHECK_THROW(store.resolve(data->getName(), producer, ndn::time::seconds(1),
    [](auto, const auto&) {}), std::logic_error);
}

BOOST_FIXTURE_TEST_CASE(RejectExpiredEncryptionCertificateAndDamagedSignature, ProfileFixture)
{
  for (int variant = 0; variant < 2; ++variant) {
    auto candidate = profile;
    if (variant == 0) {
      auto info = candidate.certificate.getSignatureInfo();
      const auto now = ndn::time::system_clock::now();
      info.setValidityPeriod(ndn::security::ValidityPeriod(now - ndn::time::hours(2),
                                                         now - ndn::time::hours(1)));
      auto signing = ndn::security::signingByIdentity(producer);
      signing.setSignatureInfo(info);
      keys.sign(candidate.certificate, signing);
    }
    auto data = std::make_shared<ndn::Data>(*candidate.makeData(keys,
      ndn::security::signingByIdentity(producer), ndn::time::seconds(60)));
    if (variant == 1) {
      const auto signature = data->getSignatureValue();
      ndn::Buffer damaged(signature.value(), signature.value() + signature.value_size());
      damaged[0] ^= 1;
      data->setSignatureValue(ndn::span<const uint8_t>(damaged.data(), damaged.size()));
    }
    SecurityProfileStore store(face, validator);
    size_t rejected = 0;
    store.resolve(data->getName(), producer, ndn::time::seconds(1),
      [&](auto record, const auto& error) { if (!record && !error.empty()) ++rejected; });
    pump();
    face.receive(*data);
    pump();
    BOOST_CHECK_EQUAL(rejected, 1);
    BOOST_CHECK_EQUAL(store.size(), 0);
  }
}

BOOST_FIXTURE_TEST_CASE(CloseDrainsDeliveryAndInvalidationDoesNotAcceptLatePacket, ProfileFixture)
{
  SecurityProfileStore store(face, validator);
  const auto data = packet();
  size_t callbacks = 0;
  store.resolve(data->getName(), producer, ndn::time::seconds(1),
    [&](auto record, const auto& error) { BOOST_REQUIRE(record != nullptr); ++callbacks; });
  pump();
  face.receive(*data);
  pump();
  BOOST_REQUIRE_EQUAL(callbacks, 1);
  std::future<void> closing;
  store.resolve(data->getName(), producer, ndn::time::seconds(1),
    [&](auto record, const auto&) {
      BOOST_REQUIRE(record != nullptr);
      std::promise<void> started;
      auto ready = started.get_future();
      closing = std::async(std::launch::async, [&] {
        started.set_value();
        store.close();
      });
      ready.wait();
      // close cannot finish while this owner callback is executing.
      BOOST_CHECK(closing.wait_for(std::chrono::milliseconds(5)) == std::future_status::timeout);
      ++callbacks;
    });
  closing.get();
  BOOST_CHECK_EQUAL(callbacks, 2);
  BOOST_CHECK_EQUAL(store.size(), 0);
  face.receive(*data);
  pump();
  BOOST_CHECK_EQUAL(callbacks, 2);
}

BOOST_FIXTURE_TEST_CASE(CallbackCanCloseWithoutDeadlockOrDeliveringRemainingWaiters, ProfileFixture)
{
  SecurityProfileStore store(face, validator);
  const auto data = packet();
  size_t callbacks = 0;
  store.resolve(data->getName(), producer, ndn::time::seconds(1),
    [&](auto record, const auto&) {
      BOOST_REQUIRE(record != nullptr);
      ++callbacks;
      store.close();
    });
  store.resolve(data->getName(), producer, ndn::time::seconds(1),
    [&](auto, const auto&) { ++callbacks; });
  pump();
  face.receive(*data);
  pump();
  BOOST_CHECK_EQUAL(callbacks, 1);
  BOOST_CHECK_EQUAL(store.size(), 0);
  store.close();
}

BOOST_FIXTURE_TEST_CASE(OldValidationCannotCompleteReplacementFetchOfSameName, ProfileFixture)
{
  SecurityProfileStore store(face, validator);
  const auto data = packet();
  size_t invalidated = 0, replacement = 0;
  store.resolve(data->getName(), producer, ndn::time::seconds(1),
    [&](auto record, const auto& error) {
      BOOST_CHECK(record == nullptr);
      BOOST_CHECK(!error.empty());
      ++invalidated;
    });
  pump();
  // DummyClientFace delivers Data synchronously, but MessageValidator posts
  // validation to this Face. Replace the pending entry before that work runs.
  face.receive(*data);
  BOOST_CHECK_EQUAL(invalidated, 0);
  store.invalidate();
  BOOST_REQUIRE_EQUAL(invalidated, 1);
  store.resolve(data->getName(), producer, ndn::time::seconds(1),
    [&](auto record, const auto& error) {
      BOOST_REQUIRE_MESSAGE(record != nullptr, error);
      BOOST_CHECK(error.empty());
      ++replacement;
    });
  pump();
  BOOST_CHECK_EQUAL(store.fetchCount(), 2);
  BOOST_CHECK_EQUAL(store.size(), 0);
  BOOST_CHECK_EQUAL(replacement, 0);
  face.receive(*data);
  pump();
  BOOST_CHECK_EQUAL(replacement, 1);
  BOOST_CHECK_EQUAL(invalidated, 1);
  BOOST_CHECK_EQUAL(store.size(), 1);
}

BOOST_FIXTURE_TEST_CASE(RequestReferenceResolutionNeverChangesAuthenticatedWire, ProfileFixture)
{
  const auto advertisement = std::make_shared<const EncryptionCertificateAdvertisement>(profile.advertisement());
  RequestMessage request;
  request.setUserSecurityProfile(profile.getName(), advertisement);
  ndn::Buffer payload(64, 0x61);
  request.setPayload(payload, payload.size());
  const auto wire = request.WireEncode();
  RequestMessage decoded;
  BOOST_REQUIRE(decoded.WireDecode(wire));
  BOOST_CHECK(decoded.hasUserSecurityProfile());
  BOOST_CHECK(!decoded.hasUserEncryptionCertificate());
  BOOST_CHECK_THROW(decoded.getUserEncryptionCertificate(), std::logic_error);
  decoded.resolveUserSecurityProfile(advertisement);
  BOOST_CHECK(decoded.WireEncode() == wire);
  RequestMessage copy(decoded);
  BOOST_CHECK(&copy.getUserEncryptionCertificate() == advertisement.get());
  BOOST_CHECK(copy.WireEncode() == wire);
  copy.Clear();
  BOOST_CHECK(!copy.hasUserSecurityProfile());
  BOOST_CHECK(!copy.hasUserEncryptionCertificate());
  RequestMessage legacy;
  legacy.setUserEncryptionCertificate(*advertisement);
  legacy.setPayload(payload, payload.size());
  BOOST_CHECK_LT(wire.size(), legacy.WireEncode().size());
}

BOOST_FIXTURE_TEST_CASE(CompactAckReconstructsStableOfferWithoutChangingWire, ProfileFixture)
{
  const auto advertisement = std::make_shared<const EncryptionCertificateAdvertisement>(profile.advertisement());
  SelectionInputKeyOffer full;
  full.setField("schemaVersion", "1");
  full.setField("recipient", producer.toUri());
  full.setField("providerBootEpoch", "test-boot-1");
  full.setField("recipientCertName", profile.certificate.getName().toUri());
  full.setField("recipientCertDigest", profile.publicKeyDigest());
  full.setField("recipientPublicKey", selectionGatedHex(profile.certificate.getPublicKey()));
  RequestAckMessage legacy;
  legacy.setStatus(true);
  legacy.setProviderEncryptionCertificate(*advertisement);
  legacy.setSelectionInputKeyOffer(full);
  RequestAckMessage compact;
  compact.setStatus(true);
  compact.setProviderSecurityProfile(profile.getName());
  SelectionInputKeyOffer dynamic;
  for (const auto& [field, value] : full.getFields()) {
    if (field != "recipientCertName" && field != "recipientCertDigest" && field != "recipientPublicKey")
      dynamic.setField(field, value);
  }
  compact.setSelectionInputKeyOffer(dynamic);
  const auto wire = compact.WireEncode();
  BOOST_CHECK_GT(legacy.WireEncode().size(), wire.size() + 500);
  RequestAckMessage decoded;
  BOOST_REQUIRE(decoded.WireDecode(wire));
  BOOST_CHECK(!decoded.hasProviderEncryptionCertificate());
  BOOST_CHECK_THROW(decoded.resolveSelectionInputKeyOffer(full), std::logic_error);
  decoded.resolveProviderSecurityProfile(advertisement);
  decoded.resolveSelectionInputKeyOffer(full);
  BOOST_CHECK(decoded.WireEncode() == wire);
  BOOST_CHECK(decoded.getSelectionInputKeyOffer().WireEncode() == full.WireEncode());
  RequestAckMessage copy(decoded);
  BOOST_CHECK(&copy.getProviderEncryptionCertificate() == advertisement.get());
  BOOST_CHECK(&copy.getSelectionInputKeyOffer() == &decoded.getSelectionInputKeyOffer());
  BOOST_CHECK(copy.WireEncode() == wire);
  copy.Clear();
  BOOST_CHECK(!copy.hasProviderSecurityProfile());
  BOOST_CHECK(!copy.hasProviderEncryptionCertificate());
}

BOOST_FIXTURE_TEST_CASE(ReferenceRejectsDuplicateMixedAndMalformedFields, ProfileFixture)
{
  const auto reference = SecurityProfileReference{profile.getName(), {}}.wireEncode();
  for (const auto root : {tlv::RequestMessageType, tlv::RequestAckMessageType}) {
    for (int variant = 0; variant < 3; ++variant) {
      ndn::Block wire(root);
      wire.push_back(reference);
      if (variant == 0) wire.push_back(reference);
      if (variant == 1) wire.push_back(profile.advertisement().WireEncode());
      if (variant == 2) {
        ndn::Block bad(SecurityProfileReference::TYPE);
        bad.push_back(profile.getName().wireEncode());
        bad.push_back(ndn::makeNonNegativeIntegerBlock(0xF854, 1));
        bad.encode();
        wire = ndn::Block(root);
        wire.push_back(bad);
      }
      wire.encode();
      if (root == tlv::RequestMessageType) {
        RequestMessage message;
        BOOST_CHECK(!message.WireDecode(wire));
      }
      else {
        RequestAckMessage message;
        BOOST_CHECK(!message.WireDecode(wire));
      }
    }
  }
  BOOST_CHECK_THROW((SecurityProfileReference{ndn::Name(), {}}.wireEncode()), std::invalid_argument);
}

BOOST_FIXTURE_TEST_CASE(ActorAllowsUnboundedInvocationAndCleansExpiredAckLease, ProfileFixture)
{
  ServiceUser user(ServiceUser::LocalMockTag{}, face, ndn::Name("/spec205/group"),
                   profile.certificate, profile.certificate, profile.certificate,
                   schema->path());
  InvocationNamesOracle::validator(user, validator);
  const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  const ndn::Name unbounded("/unbounded"), expired("/expired"), negative("/negative-timeout");
  InvocationNamesOracle::seed(user, unbounded, producer, 0, 0, now);
  InvocationNamesOracle::seed(user, expired, producer, 1000, now - 1, now);
  InvocationNamesOracle::seed(user, negative, producer, -1, 0, now);
  RequestAckMessage ack;
  ack.setStatus(true);
  ack.setProviderSecurityProfile(profile.getName());
  InvocationNamesOracle::ack(user, expired, producer, ack);
  BOOST_CHECK_EQUAL(InvocationNamesOracle::leases(user, expired), 0);
  BOOST_CHECK_EQUAL(InvocationNamesOracle::fetches(user), 0);
  InvocationNamesOracle::ack(user, unbounded, producer, ack);
  BOOST_CHECK_EQUAL(InvocationNamesOracle::fetches(user), 1);
  pump();
  const auto profileName = profile.getName();
  // LocalMock also starts real NAC public-parameter retrieval on this Face.
  const auto matching = [&](const ndn::Interest& interest) { return interest.getName() == profileName; };
  BOOST_REQUIRE_EQUAL(std::count_if(face.sentInterests.begin(), face.sentInterests.end(), matching), 1);
  const auto profileInterest = std::find_if(face.sentInterests.begin(), face.sentInterests.end(), matching);
  BOOST_CHECK(profileInterest->getInterestLifetime() == ndn::time::seconds(5));
  face.receive(*packet());
  pump();
  BOOST_CHECK_EQUAL(InvocationNamesOracle::cacheSize(user), 1);
  InvocationNamesOracle::ack(user, negative, producer, ack);
  BOOST_CHECK_EQUAL(InvocationNamesOracle::fetches(user), 1);
  BOOST_CHECK_EQUAL(InvocationNamesOracle::cacheSize(user), 1);
  // This oracle covers metadata/deadline and lease cleanup only. Empty ACK
  // authentication evidence cannot qualify secure candidate admission.
}

BOOST_FIXTURE_TEST_CASE(InlineSelectionRetainsOriginalDataAndBindsEncodingModes, ProfileFixture)
{
  const auto data = packet();
  ServiceSelectionMessage selection;
  selection.setRequestIDs({"/request-1"});
  selection.setOptionalInlineSignedInput(true);
  selection.setCompactKeyEnvelopeMode(true);
  const auto normalizedDigest = computeSelectionDigestWithoutKeyEnvelope(selection);
  selection.setInlineSignedInputData(data->wireEncode());
  BOOST_CHECK_EQUAL(computeSelectionDigestWithoutKeyEnvelope(selection), normalizedDigest);
  ServiceSelectionMessage decoded;
  BOOST_REQUIRE(decoded.WireDecode(selection.WireEncode()));
  BOOST_CHECK(decoded.getInlineSignedInputData() == data->wireEncode());
  BOOST_CHECK_EQUAL(computeSelectionDigestWithoutKeyEnvelope(decoded), normalizedDigest);
  ServiceSelectionMessage copy(decoded);
  BOOST_CHECK(&copy.getInlineSignedInputData() == &decoded.getInlineSignedInputData());
  decoded.setCompactKeyEnvelopeMode(false);
  BOOST_CHECK(computeSelectionDigestWithoutKeyEnvelope(decoded) != normalizedDigest);
  decoded = selection;
  decoded.setOptionalInlineSignedInput(false);
  BOOST_CHECK(!decoded.hasInlineSignedInputData());
  BOOST_CHECK(computeSelectionDigestWithoutKeyEnvelope(decoded) != normalizedDigest);
  copy.clearInlineSignedInputData();
  BOOST_CHECK(copy.hasOptionalInlineSignedInput());
  BOOST_REQUIRE(decoded.WireDecode(copy.WireEncode()));
  BOOST_CHECK(!decoded.hasInlineSignedInputData());
  copy.Clear();
  BOOST_CHECK(!copy.hasOptionalInlineSignedInput());
  BOOST_CHECK(!copy.hasCompactKeyEnvelopeMode());
}

BOOST_FIXTURE_TEST_CASE(InlineSelectionRejectsWrongSchemaDuplicateAndOversizeData, ProfileFixture)
{
  for (int variant = 0; variant < 3; ++variant) {
    ndn::Block inlineData(0xF85A);
    inlineData.push_back(ndn::makeNonNegativeIntegerBlock(0xF85B, variant == 0 ? 2 : 1));
    if (variant == 1) inlineData.push_back(ndn::makeNonNegativeIntegerBlock(0xF854, 1));
    inlineData.encode();
    ndn::Block wire(tlv::ServiceSelectionMessageType);
    wire.push_back(inlineData);
    if (variant == 2) wire.push_back(inlineData);
    wire.encode();
    ServiceSelectionMessage decoded;
    BOOST_CHECK(!decoded.WireDecode(wire));
  }
  auto data = std::make_shared<ndn::Data>(*packet());
  data->setContent(ndn::Buffer(2048, 1));
  keys.sign(*data, ndn::security::signingByIdentity(producer));
  ServiceSelectionMessage selection;
  selection.setOptionalInlineSignedInput(true);
  BOOST_CHECK_THROW(selection.setInlineSignedInputData(data->wireEncode()), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(FrameworkSvsPackingDefaultsRespectExplicitBounds)
{
  struct Restore
  {
    std::map<std::string, std::optional<std::string>> values;
    Restore() {
      for (const auto* name : {"NDNSF_SVS_MAX_APP_PARAMS_BYTES", "NDNSF_SVS_MAX_PIGGYDATA_BYTES"}) {
        const auto* value = std::getenv(name);
        values[name] = value ? std::optional<std::string>(value) : std::nullopt;
        ::unsetenv(name);
      }
    }
    ~Restore() {
      for (const auto& [name, value] : values) {
        if (value) ::setenv(name.c_str(), value->c_str(), 1);
        else ::unsetenv(name.c_str());
      }
    }
  } restore;
  ndn::svs::SVSPubSubOptions options;
  configureSvsPubSubOptionsFromEnvironment(options);
  BOOST_CHECK_EQUAL(options.maxApplicationParametersSize, 6144);
  BOOST_CHECK_EQUAL(options.maxPiggyDataSize, 4096);
  BOOST_CHECK_LT(options.maxApplicationParametersSize, ndn::MAX_NDN_PACKET_SIZE);
  options.maxApplicationParametersSize = 2048;
  options.maxPiggyDataSize = 512;
  configureSvsPubSubOptionsFromEnvironment(options);
  BOOST_CHECK_EQUAL(options.maxApplicationParametersSize, 2048);
  BOOST_CHECK_EQUAL(options.maxPiggyDataSize, 512);
  ::setenv("NDNSF_SVS_MAX_APP_PARAMS_BYTES", "3072", 1);
  ::setenv("NDNSF_SVS_MAX_PIGGYDATA_BYTES", "1024", 1);
  configureSvsPubSubOptionsFromEnvironment(options);
  BOOST_CHECK_EQUAL(options.maxApplicationParametersSize, 3072);
  BOOST_CHECK_EQUAL(options.maxPiggyDataSize, 1024);
}

BOOST_FIXTURE_TEST_CASE(CompactResponseRecipientMetadataRetainsOriginalWire, ProfileFixture)
{
  const std::map<std::string, std::string> stable{
    {"targeted.recipientPublicKey", selectionGatedHex(profile.certificate.getPublicKey())},
    {"targeted.recipientCertName", profile.certificate.getName().toUri()},
    {"targeted.recipientCertDigest", profile.publicKeyDigest()},
    {"targeted.inlineSignedInputV1", "1"}, {"targeted.binaryKeyEnvelopeV1", "1"},
    {"targeted.compactKeyEnvelopeV1", "1"}, {"targeted.securityProfileReferenceV1", "1"}};
  const std::map<std::string, std::string> dynamic{
    {"targeted.count", "1"}, {"targeted.0.provider", "fresh-provider-token"},
    {"targeted.0.user", "fresh-user-token"}, {"targeted.providerBootEpoch", "boot-1"},
    {"targeted.scopedKeyV1", "1"}, {"targeted.expiresAtMs", "123456"}};
  ResponseMessage compact, legacy;
  compact.setStatus(true);
  compact.setTokens(dynamic);
  compact.setProviderSecurityProfile(profile.getName());
  legacy.setStatus(true);
  auto full = dynamic;
  full.insert(stable.begin(), stable.end());
  legacy.setTokens(full);
  const auto original = compact.WireEncode();
  BOOST_CHECK_GT(legacy.WireEncode().size(), original.size() + 500);
  ResponseMessage decoded;
  BOOST_REQUIRE(decoded.WireDecode(original));
  BOOST_CHECK(!decoded.hasResolvedProviderSecurityProfile());
  BOOST_CHECK(decoded.getTokens() == dynamic);
  const auto advertisement = std::make_shared<const EncryptionCertificateAdvertisement>(profile.advertisement());
  auto contradictory = stable;
  contradictory["targeted.recipientCertName"] = "/wrong/KEY/key/self/v=1";
  BOOST_CHECK_THROW(decoded.resolveProviderSecurityProfile(advertisement, contradictory), std::invalid_argument);
  BOOST_CHECK(!decoded.hasResolvedProviderSecurityProfile());
  decoded.resolveProviderSecurityProfile(advertisement, stable);
  BOOST_CHECK(decoded.hasResolvedProviderSecurityProfile());
  BOOST_CHECK(decoded.getTokens() == full);
  BOOST_CHECK(decoded.WireEncode() == original);
  ResponseMessage copied(decoded);
  BOOST_CHECK(&copied.getTokens() == &decoded.getTokens());
  copied.clearStreamCompletion();
  BOOST_CHECK(copied.hasResolvedProviderSecurityProfile());
  copied.setTokens(dynamic);
  BOOST_CHECK(!copied.hasResolvedProviderSecurityProfile());
  BOOST_CHECK(copied.WireEncode() == original);
  copied.Clear();
  BOOST_CHECK(!copied.hasProviderSecurityProfile());
}

BOOST_FIXTURE_TEST_CASE(CompactResponseUsesValidatedWarmRecordWithoutRefetch, ProfileFixture)
{
  SecurityProfileStore store(face, validator);
  const auto data = packet();
  std::shared_ptr<const SecurityProfileStore::Record> cold;
  store.resolve(data->getName(), producer, ndn::time::seconds(1),
    [&](auto record, const auto& error) { BOOST_CHECK(error.empty()); cold = record; });
  pump();
  face.receive(*data);
  pump();
  BOOST_REQUIRE(cold != nullptr);
  const auto interests = face.sentInterests.size();
  ResponseMessage response;
  response.setTokens({{"targeted.count", "1"}, {"targeted.0.provider", "fresh"}});
  response.setProviderSecurityProfile(data->getName());
  const auto wire = response.WireEncode();
  bool resolved = false;
  store.resolve(response.getProviderSecurityProfile(), producer, ndn::time::seconds(1),
    [&](auto record, const auto& error) {
      BOOST_REQUIRE(record != nullptr);
      BOOST_CHECK(error.empty());
      BOOST_CHECK(record == cold);
      response.resolveProviderSecurityProfile(
        std::shared_ptr<const EncryptionCertificateAdvertisement>(record, &record->advertisement),
        {{"targeted.recipientPublicKey", record->publicKeyHex},
         {"targeted.recipientCertName", record->profile.certificate.getName().toUri()},
         {"targeted.recipientCertDigest", record->publicKeyDigest},
         {"targeted.inlineSignedInputV1", "1"}, {"targeted.binaryKeyEnvelopeV1", "1"},
         {"targeted.compactKeyEnvelopeV1", "1"}, {"targeted.securityProfileReferenceV1", "1"}});
      resolved = true;
    });
  BOOST_CHECK(resolved);
  BOOST_CHECK_EQUAL(store.fetchCount(), 1);
  BOOST_CHECK_EQUAL(face.sentInterests.size(), interests);
  BOOST_CHECK(response.hasResolvedProviderSecurityProfile());
  BOOST_CHECK(response.WireEncode() == wire);
}

BOOST_FIXTURE_TEST_CASE(CompactResponseRejectsMixedDuplicateAndIncompleteMetadata, ProfileFixture)
{
  ResponseMessage compact;
  compact.setProviderSecurityProfile(profile.getName());
  auto duplicate = compact.WireEncode();
  duplicate.push_back(SecurityProfileReference{profile.getName(), {}}.wireEncode());
  duplicate.encode();
  ResponseMessage decoded;
  BOOST_CHECK(!decoded.WireDecode(duplicate));
  auto mixed = compact.WireEncode();
  mixed.push_back(ndn::makeStringBlock(tlv::TokenType, "targeted.recipientPublicKey=forged"));
  mixed.encode();
  BOOST_CHECK(!decoded.WireDecode(mixed));
  const auto advertisement = std::make_shared<const EncryptionCertificateAdvertisement>(profile.advertisement());
  BOOST_CHECK_THROW(compact.resolveProviderSecurityProfile(advertisement, {}), std::invalid_argument);
  BOOST_CHECK(!compact.hasResolvedProviderSecurityProfile());
  compact.setTokens({{"targeted.recipientPublicKey", "forged"}});
  BOOST_CHECK_THROW(compact.WireEncode(), std::invalid_argument);
}

BOOST_FIXTURE_TEST_CASE(ResponseProfileActorRejectsWrongProducerMalformedAndExpired, ProfileFixture)
{
  ServiceUser user(ServiceUser::LocalMockTag{}, face, ndn::Name("/spec205/group"),
                   profile.certificate, profile.certificate, profile.certificate,
                   schema->path());
  InvocationNamesOracle::validator(user, validator);
  const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  ndn::Name wrong("/wrong");
  wrong.append(profile.getName().getSubName(producer.size()));
  for (int variant = 0; variant < 3; ++variant) {
    const auto id = ndn::Name("/response-profile-negative").appendNumber(variant);
    InvocationNamesOracle::seedResponse(user, id, producer, 1000,
                                       variant == 2 ? now - 1 : now + 1000000, now);
    ResponseMessage message;
    message.setProviderSecurityProfile(variant == 0 ? wrong :
      variant == 1 ? ndn::Name(producer).append("not-a-profile") : profile.getName());
    boost::asio::post(face.getIoContext(), [&, id, message] {
      BOOST_CHECK_NO_THROW(InvocationNamesOracle::response(user, id, producer, message));
      BOOST_CHECK_EQUAL(InvocationNamesOracle::responseLeases(user, id), 0);
      BOOST_CHECK_EQUAL(InvocationNamesOracle::fetches(user), 0);
    });
    pump();
  }
}

BOOST_FIXTURE_TEST_CASE(ResponseProfileActorUnboundedCallsRetainBoundedMetadataInterest, ProfileFixture)
{
  ServiceUser user(ServiceUser::LocalMockTag{}, face, ndn::Name("/spec205/group"),
                   profile.certificate, profile.certificate, profile.certificate,
                   schema->path());
  InvocationNamesOracle::validator(user, validator);
  const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  for (int timeout : {0, -1}) {
    const auto id = ndn::Name("/unbounded-response").append(std::to_string(timeout));
    InvocationNamesOracle::seedResponse(user, id, producer, timeout, 0, now);
    ResponseMessage message;
    message.setProviderSecurityProfile(profile.getName());
    boost::asio::post(face.getIoContext(), [&, id, message] {
      BOOST_CHECK_NO_THROW(InvocationNamesOracle::response(user, id, producer, message));
      BOOST_CHECK_EQUAL(InvocationNamesOracle::responseLeases(user, id), 1);
    });
    pump();
  }
  BOOST_CHECK_EQUAL(InvocationNamesOracle::fetches(user), 1);
  size_t matching = 0;
  for (const auto& interest : face.sentInterests) {
    if (interest.getName() == profile.getName()) {
      ++matching;
      BOOST_CHECK(interest.getInterestLifetime() == ndn::time::milliseconds(5000));
    }
  }
  BOOST_CHECK_EQUAL(matching, 1);
}

BOOST_FIXTURE_TEST_CASE(BasicProfileRequiresEveryAuthorizedAuthenticatedReader, ProfileFixture)
{
  ServiceUser user(ServiceUser::LocalMockTag{}, face, ndn::Name("/spec205/group"),
                   profile.certificate, profile.certificate, profile.certificate,
                   schema->path());
  const ndn::Name service("/spec205/service"), other("/spec205/other-service");
  auto peer = profile;
  peer.producer = ndn::Name("/spec205/another-provider");
  peer.certificate = keys.createIdentity(peer.producer, ndn::RsaKeyParams(2048)).getDefaultKey().getDefaultCertificate();
  const auto permission = [&](const ndn::Name& p) {
    return ServiceAuthorizationRecord{ndn::Name(p).append(service).toUri(), service.toUri(), tlv::UserPermission, 7};
  };
  InvocationNamesOracle::profileAudience(user, 7, {permission(producer), permission(peer.producer)},
    {{producer, service}, {peer.producer, service}});
  const auto ack = [&](const ProducerSecurityProfile& p) {
    RequestAckMessage result;
    result.setStatus(true);
    result.setPolicyEpoch(7);
    result.setProviderSecurityProfile(p.getName(),
      std::make_shared<const EncryptionCertificateAdvertisement>(p.advertisement()));
    return result;
  };
  const auto provenance = [&](const ProducerSecurityProfile& p, bool trusted) {
    // Only the observation predicate is under test; production creates these
    // fields after validated SVS delivery. This is not a wire trust oracle.
    return AckAuthenticationEvidence{p.producer.toUri(), p.certificate.getName().toUri(), "signed-wire", trusted};
  };
  BOOST_CHECK(!InvocationNamesOracle::knownAudience(user, service));
  InvocationNamesOracle::observeReader(user, producer, service, ack(profile), provenance(profile, false));
  BOOST_CHECK(!InvocationNamesOracle::knownAudience(user, service));
  InvocationNamesOracle::observeReader(user, producer, service, ack(profile), provenance(profile, true));
  BOOST_CHECK(!InvocationNamesOracle::knownAudience(user, service));
  auto wrongSigner = provenance(peer, true);
  wrongSigner.signerIdentity = producer.toUri();
  InvocationNamesOracle::observeReader(user, peer.producer, service, ack(peer), wrongSigner);
  BOOST_CHECK(!InvocationNamesOracle::knownAudience(user, service));
  InvocationNamesOracle::observeReader(user, peer.producer, service, ack(peer), provenance(peer, true));
  BOOST_CHECK(InvocationNamesOracle::knownAudience(user, service));
  InvocationNamesOracle::profileAudience(user, 7, {
    permission(producer), permission(peer.producer),
    {"/new/provider/spec205/service", service.toUri(), tlv::UserPermission, 7}},
    {{producer, service}, {peer.producer, service}, {ndn::Name("/new/provider"), service}});
  BOOST_CHECK(!InvocationNamesOracle::knownAudience(user, service));
  InvocationNamesOracle::profileAudience(user, 7, {permission(producer), permission(peer.producer)},
    {{producer, service}, {peer.producer, service}});
  BOOST_CHECK(InvocationNamesOracle::knownAudience(user, service));
  BOOST_CHECK(!InvocationNamesOracle::knownAudience(user, other));
  InvocationNamesOracle::expireReaders(user);
  BOOST_CHECK(!InvocationNamesOracle::knownAudience(user, service));
  InvocationNamesOracle::observeReader(user, producer, service, ack(profile), provenance(profile, true));
  InvocationNamesOracle::observeReader(user, peer.producer, service, ack(peer), provenance(peer, true));
  auto negative = ack(peer);
  negative.setStatus(false);
  InvocationNamesOracle::observeReader(user, peer.producer, service, negative, provenance(peer, true));
  BOOST_CHECK(!InvocationNamesOracle::knownAudience(user, service));
  InvocationNamesOracle::observeReader(user, peer.producer, service, ack(peer), provenance(peer, true));
  BOOST_CHECK(InvocationNamesOracle::knownAudience(user, service));
  InvocationNamesOracle::profileAudience(user, 8, {
    {ndn::Name(producer).append(service).toUri(), service.toUri(), tlv::UserPermission, 8}}, {{producer, service}});
  BOOST_CHECK(!InvocationNamesOracle::knownAudience(user, service));
  InvocationNamesOracle::profileAudience(user, 8, {});
  BOOST_CHECK(!InvocationNamesOracle::knownAudience(user, service));
}

BOOST_FIXTURE_TEST_CASE(UnselectedAuthorizedAckLearnsReaderWithoutSelection, ProfileFixture)
{
  ServiceUser user(ServiceUser::LocalMockTag{}, face, ndn::Name("/spec205/group"),
                   profile.certificate, profile.certificate, profile.certificate,
                   schema->path());
  user.setUseTokens(true);
  const ndn::Name service("/spec205/service"), id("/reader-observation");
  auto peer = profile;
  peer.producer = ndn::Name("/spec205/unselected-provider");
  peer.certificate = keys.createIdentity(peer.producer, ndn::RsaKeyParams(2048)).getDefaultKey().getDefaultCertificate();
  const auto permission = [&](const ndn::Name& p) {
    return ServiceAuthorizationRecord{ndn::Name(p).append(service).toUri(), service.toUri(), tlv::UserPermission, 7};
  };
  InvocationNamesOracle::profileAudience(user, 7, {permission(producer), permission(peer.producer)},
    {{producer, service}, {peer.producer, service}});
  RequestAckMessage ack;
  ControllerVersion version;
  version.controllerGenerationTimestamp = 1234; version.controllerEpoch = 7;
  InvocationNamesOracle::controllerVersion(user, version);
  ack.setControllerVersion(version);
  ack.setStatus(true); ack.setPolicyEpoch(7);
  ack.setProviderSecurityProfile(profile.getName(),
    std::make_shared<const EncryptionCertificateAdvertisement>(profile.advertisement()));
  InvocationNamesOracle::observeReader(user, producer, service, ack,
    {producer.toUri(), profile.certificate.getName().toUri(), "signed-wire", true});
  InvocationNamesOracle::seed(user, id, producer, 5000, 0, 0);
  ack.setProviderSecurityProfile(peer.getName(),
    std::make_shared<const EncryptionCertificateAdvertisement>(peer.advertisement()));
  ack.setUserToken("wrong-user-token"); ack.setProviderToken("peer-provider-token");
  const AckAuthenticationEvidence evidence{peer.producer.toUri(), peer.certificate.getName().toUri(), "signed-peer-wire", true};
  const auto reject = [&](const RequestAckMessage& candidate, const AckAuthenticationEvidence& provenance) {
    BOOST_CHECK(!InvocationNamesOracle::unselectedReaderAck(user, id, peer.producer, candidate, provenance));
    BOOST_CHECK(InvocationNamesOracle::selectionUntouched(user, id));
  };
  reject(ack, evidence);
  BOOST_CHECK(!InvocationNamesOracle::knownAudience(user, service));
  ack.setUserToken("pending-user-token"); ack.setProviderToken("");
  reject(ack, evidence);
  BOOST_CHECK(!InvocationNamesOracle::knownAudience(user, service));
  ack.setProviderToken("peer-provider-token"); ack.setPolicyEpoch(6);
  reject(ack, evidence);
  BOOST_CHECK(!InvocationNamesOracle::knownAudience(user, service));
  ack.setPolicyEpoch(7);
  auto staleVersion = version; staleVersion.controllerEpoch = 6;
  ack.setControllerVersion(staleVersion);
  reject(ack, evidence);
  BOOST_CHECK(!InvocationNamesOracle::knownAudience(user, service));
  ack.setControllerVersion(version);
  auto untrusted = evidence; untrusted.trustSchemaValidated = false;
  reject(ack, untrusted);
  BOOST_CHECK(!InvocationNamesOracle::knownAudience(user, service));
  auto wrongSigner = evidence; wrongSigner.signerIdentity = producer.toUri();
  reject(ack, wrongSigner);
  BOOST_CHECK(!InvocationNamesOracle::knownAudience(user, service));
  reject(ack, evidence);
  BOOST_CHECK(InvocationNamesOracle::knownAudience(user, service));
  // This is a native post-decryption call-path oracle, not signature verification.
}

BOOST_AUTO_TEST_CASE(PublicAckIngressObservesExcludedReaderWithoutSelectionLease)
{
  PlaintextAckScope diagnostic;
  for (const bool alreadySelected : {false, true}) {
    ProfileFixture f;
    ServiceUser user(ServiceUser::LocalMockTag{}, f.face, ndn::Name("/spec205/group"),
                     f.profile.certificate, f.profile.certificate, f.profile.certificate,
                     f.schema->path());
    user.setUseTokens(true);
    InvocationNamesOracle::validator(user, f.validator);
    const ndn::Name service("/spec205/service"), id("/reader-ingress"), selected("/spec205/selected");
    const auto permission = [&](const ndn::Name& p) {
      return ServiceAuthorizationRecord{ndn::Name(p).append(service).toUri(), service.toUri(), tlv::UserPermission, 7};
    };
    InvocationNamesOracle::profileAudience(user, 7, {permission(f.producer)}, {{f.producer, service}});
    InvocationNamesOracle::seed(user, id, selected, 0, 0, 0);
    InvocationNamesOracle::prepareReaderIngress(user, id, alreadySelected);
    RequestAckMessage ack;
    ack.setStatus(true); ack.setPolicyEpoch(7);
    ack.setUserToken("pending-user-token"); ack.setProviderToken("reader-provider-token");
    ack.setProviderSecurityProfile(f.profile.getName());
    const auto block = ack.WireEncode();
    const ndn::Buffer bytes(block.data(), block.size());
    ndn::Data data(makeRequestAckNameV2(f.producer, f.producer, service, id));
    data.setContent(bytes);
    f.keys.sign(data, ndn::security::signingByIdentity(f.producer));
    bool validated = false;
    f.validator->validateData(data, [&](const auto&) { validated = true; },
      [&](const auto&, const auto&) { BOOST_FAIL("original ACK signature rejected"); });
    f.pump();
    BOOST_REQUIRE(validated);
    const ndn::Name producerNode = ndn::Name(f.producer).append("provider").append("1");
    const std::optional<ndn::Data> originalPacket{data};
    BOOST_REQUIRE(f.validator->isProducerSignerAuthorized(data, f.producer));
    BOOST_REQUIRE(parseSvsProducerActor(producerNode, "provider") == f.producer);
    const ndn::svs::SVSPubSub::SubscriptionData publication{
      data.getName(), ndn::span<const uint8_t>(bytes.data(), bytes.size()),
      producerNode, 1, originalPacket};
    InvocationNamesOracle::ingress(user, publication);
    InvocationNamesOracle::ingress(user, publication);
    BOOST_CHECK_EQUAL(InvocationNamesOracle::readerTickets(user, id), 1);
    f.pump();
    BOOST_CHECK_EQUAL(InvocationNamesOracle::fetches(user), 1);
    f.face.receive(*f.packet());
    f.pump();
    BOOST_CHECK(InvocationNamesOracle::knownAudience(user, service));
    BOOST_CHECK_EQUAL(InvocationNamesOracle::readerTickets(user, id), 0);
    BOOST_CHECK_EQUAL(InvocationNamesOracle::leases(user, id), 1);
    BOOST_CHECK(InvocationNamesOracle::readerSelectionUnchanged(user, id, selected, alreadySelected));
    InvocationNamesOracle::ingress(user, publication);
    f.pump();
    BOOST_CHECK_EQUAL(InvocationNamesOracle::fetches(user), 1);
    BOOST_CHECK_EQUAL(InvocationNamesOracle::readerTickets(user, id), 0);
    // Compact requests omit the explicit redundant reader flag. The reference
    // itself still negotiates support, including renewal from a warm record.
    InvocationNamesOracle::profileOnlyReader(user, id, f.profile);
    InvocationNamesOracle::expireReaders(user);
    InvocationNamesOracle::ingress(user, publication);
    f.pump();
    BOOST_CHECK(InvocationNamesOracle::knownAudience(user, service));
    BOOST_CHECK_EQUAL(InvocationNamesOracle::fetches(user), 1);
    BOOST_CHECK_EQUAL(InvocationNamesOracle::readerTickets(user, id), 0);
    BOOST_CHECK_EQUAL(InvocationNamesOracle::leases(user, id), 1);
  }
  // The original signed ingress and profile validation are real; encryption is
  // bypassed only by the existing explicit diagnostic seam. MiniNDN covers it.
}

BOOST_AUTO_TEST_CASE(PublicReaderAckRejectsFailuresAndLateCompletionWithoutMainLease)
{
  for (const std::string mode : {"malformed", "bad-inner", "wrong-token", "retired", "stale-version", "missing-capability"}) {
    ProfileFixture f;
    ServiceUser user(ServiceUser::LocalMockTag{}, f.face, ndn::Name("/spec205/group"),
                     f.profile.certificate, f.profile.certificate, f.profile.certificate,
                     f.schema->path());
    user.setUseTokens(true);
    InvocationNamesOracle::validator(user, f.validator);
    const ndn::Name service("/spec205/service"), id("/reader-failure"), selected("/spec205/selected");
    const ServiceAuthorizationRecord permission{ndn::Name(f.producer).append(service).toUri(),
      service.toUri(), tlv::UserPermission, 7};
    InvocationNamesOracle::profileAudience(user, 7, {permission}, {{f.producer, service}});
    InvocationNamesOracle::seed(user, id, selected, 0, 0, 0);
    InvocationNamesOracle::prepareReaderIngress(user, id, false);
    if (mode == "missing-capability") InvocationNamesOracle::omitReaderCapability(user, id);
    ControllerVersion version;
    version.controllerEpoch = 7; version.controllerGenerationTimestamp = 1234;
    InvocationNamesOracle::controllerVersion(user, version);
    RequestAckMessage ack;
    ack.setStatus(true); ack.setPolicyEpoch(7); ack.setControllerVersion(version);
    ack.setUserToken(mode == "wrong-token" ? "wrong" : "pending-user-token");
    ack.setProviderToken("reader-provider-token");
    ack.setProviderSecurityProfile(f.profile.getName());
    const auto block = ack.WireEncode();
    ndn::Buffer bytes = mode == "malformed" ? ndn::Buffer{0xfd} :
      ndn::Buffer(block.data(), block.size());
    if (mode == "bad-inner") {
      const ndn::Buffer truncated{0xfd};
      ndn::Block bad(tlv::HybridMessageEnvelopeType,
        std::make_shared<ndn::Buffer>(truncated));
      bad.encode();
      bytes.assign(bad.data(), bad.data() + bad.size());
    }
    ndn::Data data(makeRequestAckNameV2(f.producer, f.producer, service, id));
    data.setContent(bytes);
    f.keys.sign(data, ndn::security::signingByIdentity(f.producer));
    bool validated = false;
    f.validator->validateData(data, [&](const auto&) { validated = true; },
      [&](const auto&, const auto&) { BOOST_FAIL("signed negative ACK rejected by trust schema"); });
    f.pump();
    BOOST_REQUIRE(validated);
    const ndn::Name producerNode = ndn::Name(f.producer).append("provider").append("1");
    const std::optional<ndn::Data> originalPacket{data};
    BOOST_REQUIRE(f.validator->isProducerSignerAuthorized(data, f.producer));
    BOOST_REQUIRE(parseSvsProducerActor(producerNode, "provider") == f.producer);
    const ndn::svs::SVSPubSub::SubscriptionData publication{
      data.getName(), ndn::span<const uint8_t>(bytes.data(), bytes.size()),
      producerNode, 1, originalPacket};
    std::unique_ptr<PlaintextAckScope> diagnostic;
    if (mode != "malformed" && mode != "bad-inner") diagnostic = std::make_unique<PlaintextAckScope>();
    BOOST_CHECK_NO_THROW(InvocationNamesOracle::ingress(user, publication));
    f.pump();
    if (mode == "retired" || mode == "stale-version") {
      BOOST_REQUIRE_EQUAL(InvocationNamesOracle::fetches(user), 1);
      if (mode == "retired") InvocationNamesOracle::retireReaderCall(user, id);
      else {
        ++version.controllerEpoch;
        InvocationNamesOracle::controllerVersion(user, version);
      }
      f.face.receive(*f.packet());
      f.pump();
    }
    else BOOST_CHECK_EQUAL(InvocationNamesOracle::fetches(user), 0);
    BOOST_CHECK(!InvocationNamesOracle::knownAudience(user, service));
    BOOST_CHECK_EQUAL(InvocationNamesOracle::readerTickets(user, id), 0);
    BOOST_CHECK_EQUAL(InvocationNamesOracle::leases(user, id), 1);
    BOOST_CHECK(InvocationNamesOracle::readerSelectionUnchanged(user, id, selected, false));
  }
}

BOOST_FIXTURE_TEST_CASE(BasicProfilePreservesExactProducersForQualifiedServiceAliases, ProfileFixture)
{
  ServiceUser user(ServiceUser::LocalMockTag{}, face, ndn::Name("/spec205/group"),
                   profile.certificate, profile.certificate, profile.certificate,
                   schema->path());
  auto peer = profile;
  peer.producer = ndn::Name(producer).append("nested-peer");
  peer.certificate = keys.createIdentity(peer.producer, ndn::RsaKeyParams(2048)).getDefaultKey().getDefaultCertificate();
  const auto service = ndn::Name(peer.producer).append("invoke");
  const ServiceAuthorizationRecord row{service.toUri(), service.toUri(), tlv::UserPermission, 7};
  // Both exact Producers legitimately project to the same qualified key.
  // One observed peer must never be mistaken for the entire signed audience.
  InvocationNamesOracle::profileAudience(user, 7, {row, row}, {{producer, service}, {peer.producer, service}});
  const auto observe = [&](const ProducerSecurityProfile& p) {
    RequestAckMessage ack;
    ack.setStatus(true);
    ack.setPolicyEpoch(7);
    ack.setProviderSecurityProfile(p.getName(),
      std::make_shared<const EncryptionCertificateAdvertisement>(p.advertisement()));
    InvocationNamesOracle::observeReader(user, p.producer, service, ack,
      AckAuthenticationEvidence{p.producer.toUri(), p.certificate.getName().toUri(), "signed-wire", true});
  };
  observe(profile);
  BOOST_CHECK(!InvocationNamesOracle::knownAudience(user, service));
  observe(peer);
  BOOST_CHECK(InvocationNamesOracle::knownAudience(user, service));
  InvocationNamesOracle::profileAudience(user, 7, {row});
  BOOST_CHECK(!InvocationNamesOracle::knownAudience(user, service));
}

BOOST_FIXTURE_TEST_CASE(BasicProfileRepresentationPreservesCapabilitiesAndInvalidates, ProfileFixture)
{
  ServiceUser user(ServiceUser::LocalMockTag{}, face, ndn::Name("/spec205/group"),
                   profile.certificate, profile.certificate, profile.certificate,
                   schema->path());
  InvocationNamesOracle::validator(user, validator);
  InvocationNamesOracle::signingKeys(user, keys, producer);
  const ndn::Name service("/spec205/service");
  InvocationNamesOracle::profileAudience(user, 7, {
    {ndn::Name(producer).append(service).toUri(), service.toUri(), tlv::UserPermission, 7}}, {{producer, service}});
  RequestMessage message;
  message.setUserEncryptionCertificate(profile.advertisement());
  RequestCapabilities caps;
  caps.setField("SecurityProfileReferenceV1", "1");
  caps.setField("RequestScopedConfidentialityV1", "required");
  caps.setField("application.name", "qwen");
  message.setRequestCapabilities(caps);
  const auto legacy = message.WireEncode();
  BOOST_CHECK(!InvocationNamesOracle::basicProfile(user, message, service));
  BOOST_CHECK(message.WireEncode() == legacy);
  RequestAckMessage ack;
  ack.setStatus(true);
  ack.setPolicyEpoch(7);
  ack.setProviderSecurityProfile(profile.getName(),
    std::make_shared<const EncryptionCertificateAdvertisement>(profile.advertisement()));
  InvocationNamesOracle::observeReader(user, producer, service, ack,
    AckAuthenticationEvidence{producer.toUri(), profile.certificate.getName().toUri(), "signed-wire", true});
  BOOST_REQUIRE(InvocationNamesOracle::basicProfile(user, message, service));
  BOOST_CHECK(message.hasUserSecurityProfile());
  BOOST_CHECK(message.getUserSecurityProfile() == profile.getName());
  BOOST_CHECK(!message.getRequestCapabilities().hasField("SecurityProfileReferenceV1"));
  BOOST_CHECK_EQUAL(message.getRequestCapabilities().getField("RequestScopedConfidentialityV1"), "required");
  BOOST_CHECK_EQUAL(message.getRequestCapabilities().getField("application.name"), "qwen");
  BOOST_CHECK_LT(message.WireEncode().size(), legacy.size());
  InvocationNamesOracle::invalidateReaders(user, service);
  BOOST_CHECK(!InvocationNamesOracle::knownAudience(user, service));
  auto fresh = RequestMessage{};
  fresh.setUserEncryptionCertificate(profile.advertisement());
  fresh.setRequestCapabilities(caps);
  BOOST_CHECK(!InvocationNamesOracle::basicProfile(user, fresh, service));
  BOOST_CHECK(!fresh.hasUserSecurityProfile());
}

BOOST_AUTO_TEST_CASE(CachedEncapsulatedDataReadyInlineAndGenericOrderingPreserved)
{
  ProfileFixture f;
  InvocationNamesOracle::localCertificate(*f.validator, f.profile.certificate);
  auto adapter = f.validator->makeEncapsulatedValidator();
  auto packet = f.packet();
  bool inside = false, inlineSuccess = false, genericSuccess = false;
  size_t failures = 0;
  boost::asio::post(f.face.getIoContext(), [&] {
    inside = true;
    adapter->validate(*packet, [&](const auto&) {
      BOOST_CHECK(inside);
      BOOST_CHECK(f.face.getIoContext().get_executor().running_in_this_thread());
      inlineSuccess = true;
    }, [&](const auto&, const auto&) { ++failures; });
    BOOST_CHECK(inlineSuccess);
    f.validator->validateData(*packet, [&](const auto&) {
      BOOST_CHECK(!inside);
      genericSuccess = true;
    }, [&](const auto&, const auto&) { ++failures; });
    BOOST_CHECK(!genericSuccess);
    inside = false;
  });
  f.pump();
  BOOST_CHECK(inlineSuccess);
  BOOST_CHECK(genericSuccess);
  BOOST_CHECK_EQUAL(failures, 0);
}

BOOST_AUTO_TEST_CASE(EncapsulatedBadSignatureRejectsWithoutAdmission)
{
  ProfileFixture f;
  InvocationNamesOracle::localCertificate(*f.validator, f.profile.certificate);
  auto adapter = f.validator->makeEncapsulatedValidator();
  auto packet = *f.packet();
  const ndn::Buffer changed{0x42};
  packet.setContent(ndn::span<const uint8_t>(changed.data(), changed.size()));
  bool inside = false;
  size_t accepted = 0, rejected = 0;
  boost::asio::post(f.face.getIoContext(), [&] {
    inside = true;
    adapter->validate(packet, [&](const auto&) { ++accepted; },
      [&](const auto&, const auto& error) {
        BOOST_CHECK(inside);
        BOOST_CHECK_EQUAL(error.getCode(), ndn::security::ValidationError::INVALID_SIGNATURE);
        ++rejected;
      });
    inside = false;
  });
  f.pump();
  BOOST_CHECK_EQUAL(accepted, 0);
  BOOST_CHECK_EQUAL(rejected, 1);
}

BOOST_AUTO_TEST_CASE(EncapsulatedThrowingCallbacksDoNotRestartValidation)
{
  ProfileFixture f;
  InvocationNamesOracle::localCertificate(*f.validator, f.profile.certificate);
  auto adapter = f.validator->makeEncapsulatedValidator();
  auto valid = f.packet();
  auto invalid = *valid;
  const ndn::Buffer changed{0x42};
  invalid.setContent(ndn::span<const uint8_t>(changed.data(), changed.size()));
  size_t successes = 0, failures = 0, unexpected = 0;
  boost::asio::post(f.face.getIoContext(), [&] {
    adapter->validate(*valid, [&](const auto&) {
      ++successes;
      throw std::runtime_error("success callback");
    }, [&](const auto&, const auto&) { ++unexpected; });
    adapter->validate(invalid, [&](const auto&) { ++unexpected; },
      [&](const auto&, const auto& error) {
        BOOST_CHECK_EQUAL(error.getCode(), ndn::security::ValidationError::INVALID_SIGNATURE);
        ++failures;
        throw std::runtime_error("failure callback");
      });
  });
  f.pump();
  BOOST_CHECK_EQUAL(successes, 1);
  BOOST_CHECK_EQUAL(failures, 1);
  BOOST_CHECK_EQUAL(unexpected, 0);
}

BOOST_AUTO_TEST_CASE(EncapsulatedLargeAndOffFaceRetainAsyncOwnership)
{
  ProfileFixture f;
  InvocationNamesOracle::localCertificate(*f.validator, f.profile.certificate);
  auto adapter = f.validator->makeEncapsulatedValidator();
  auto small = f.packet();
  ndn::Data large(ndn::Name(f.producer).append("large"));
  const ndn::Buffer content(5000, 0x42);
  large.setContent(ndn::span<const uint8_t>(content.data(), content.size()));
  f.keys.sign(large, ndn::security::signingByIdentity(f.producer));
  BOOST_REQUIRE_GT(large.wireEncode().size(), 4096);
  bool inside = false, largeSuccess = false, workerSuccess = false;
  size_t failures = 0;
  boost::asio::post(f.face.getIoContext(), [&] {
    inside = true;
    adapter->validate(large, [&](const auto&) {
      BOOST_CHECK(!inside);
      BOOST_CHECK(f.face.getIoContext().get_executor().running_in_this_thread());
      largeSuccess = true;
    }, [&](const auto&, const auto&) { ++failures; });
    BOOST_CHECK(!largeSuccess);
    inside = false;
  });
  const auto faceThread = std::this_thread::get_id();
  std::thread worker([&] {
    adapter->validate(*small, [&](const auto&) {
      BOOST_CHECK(std::this_thread::get_id() == faceThread);
      BOOST_CHECK(f.face.getIoContext().get_executor().running_in_this_thread());
      workerSuccess = true;
    }, [&](const auto&, const auto&) { ++failures; });
  });
  worker.join();
  BOOST_CHECK(!workerSuccess);
  f.pump();
  BOOST_CHECK(largeSuccess);
  BOOST_CHECK(workerSuccess);
  BOOST_CHECK_EQUAL(failures, 0);
}

BOOST_FIXTURE_TEST_CASE(CapturedResponseContextRejectsForeignMissingAndMismatchedNames, ProfileFixture)
{
  ServiceProvider provider(ServiceProvider::LocalMockTag{}, face, ndn::Name("/spec205/group"),
                           profile.certificate, profile.certificate, profile.certificate,
                           schema->path());
  const ResponseNameV2 context{producer, ndn::Name("/spec205/requester"),
                              ndn::Name("/spec205/service"), ndn::Name("/request")};
  const auto name = makeResponseNameV2(context.providerName, context.requesterName,
                                      context.serviceName, context.requestId);
  const auto sentBefore = face.sentData.size();
  auto foreign = context;
  foreign.providerName = ndn::Name("/foreign");
  const auto foreignName = makeResponseNameV2(foreign.providerName, foreign.requesterName,
                                             foreign.serviceName, foreign.requestId);
  BOOST_CHECK_THROW(InvocationNamesOracle::publishCapturedResponse(provider, foreignName, foreign),
                    std::invalid_argument);
  auto missing = context;
  missing.requesterName.clear();
  BOOST_CHECK_THROW(InvocationNamesOracle::publishCapturedResponse(provider, name, missing),
                    std::invalid_argument);
  auto changed = context;
  changed.serviceName = ndn::Name("/other-service");
  BOOST_CHECK_THROW(InvocationNamesOracle::publishCapturedResponse(provider, name, changed),
                    std::invalid_argument);
  BOOST_CHECK_THROW(InvocationNamesOracle::publishCapturedResponse(provider, name.getPrefix(-1), context),
                    std::invalid_argument);
  BOOST_CHECK_EQUAL(face.sentData.size(), sentBefore);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndn_service_framework::test
