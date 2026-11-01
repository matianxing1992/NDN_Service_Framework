#include "tests/fixtures/spec204/owned-kv-publication.hpp"
#include "ndn-service-framework/ProviderDurableLargeDataPublisher.hpp"
#include <fstream>
using namespace spec204_owned_kv;

BOOST_AUTO_TEST_SUITE(Spec201OriginalPacketPublication)

BOOST_AUTO_TEST_CASE(ProviderPublicationDeadlineCancelsQueuedFaceWorkBeforeStackRelease)
{
  OwnedCorePublicationFixture f;
  auto producer = std::make_shared<LocalServiceProvider>(
    f.face, ndn::Name("/spec212/publication-group"), f.actor, f.authority,
    "examples/trust-any.conf");
  ProviderDurableLargeDataPublishRequestV1 request;
  request.serviceName = f.service;
  request.requestId = ndn::Name("/spec212/publication-deadline");
  request.protectedPayload = {1, 2, 3};
  request.objectLabel = "kvcache";
  request.freshness = ndn::time::milliseconds(100);
  request.contentName.kind = ContentDataKind::Blob;
  request.contentName.label = request.objectLabel;
  request.commitOptions.retention = EncryptedLargeDataRetention::Durable;
  request.commitOptions.publicationIdentity = keyReferenceDigest("queued-provider-publication");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  std::atomic<unsigned> grantRequests{0};
  auto work = std::async(std::launch::async,
    [&, producer, request = std::move(request)] () mutable {
      return ProviderDurableLargeDataPublisher::publish(producer, std::move(request), f.store,
        [&] (const ndn::Name&, const std::string&, const std::function<void()>&)
          -> DurableStorageGrantSelector {
          ++grantRequests;
          throw std::runtime_error("unexpected grant after publication deadline");
        }, {}, [deadline] {
          if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("test-provider-publication-deadline");
        });
    });

  // Leave the Face unpumped until the worker's actual deadline has expired.
  // A failing old implementation is still released by the cleanup pump below.
  const bool settledWithoutFace =
    work.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
  for (unsigned round = 0; round < 200 &&
       work.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready; ++round) {
    f.face.processEvents(ndn::time::milliseconds(1));
  }
  BOOST_CHECK(settledWithoutFace);
  BOOST_REQUIRE(work.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
  const auto result = work.get();
  BOOST_CHECK(!result.success);
  BOOST_CHECK_EQUAL(result.errorMessage, "test-provider-publication-deadline");
  BOOST_CHECK_EQUAL(grantRequests.load(), 0U);
  BOOST_CHECK(f.node->core().list().empty());

  // The publication stack is gone. Running its queued callback now must be a
  // no-op, including under ASAN, with no late crypto/grant/storage side effect.
  f.face.processEvents(ndn::time::milliseconds(10));
  BOOST_CHECK_EQUAL(grantRequests.load(), 0U);
  BOOST_CHECK(f.node->core().list().empty());
}

namespace {
enum class RecoveryProbe {
  Success, BadDigest, PlainBudget, EnvelopeBudget, MissingHint, WrongHint,
  WrongSigner, MissingCurrent, ExpiredCaller, ExpiredAssignment, CancelBody, MissingCk,
  NoHintNoOptions, NoHintInlineNoOptions, NoHintMissingCk,
  NoHintCurrentWrapperCancel, NoHintCurrentInlineCancel
};

void checkRepoRecovery(bool locatorWrapper, RecoveryProbe probe)
{
  std::unique_ptr<ScopedEnvironmentValue> fetchTimeout;
  if (probe == RecoveryProbe::NoHintNoOptions || probe == RecoveryProbe::NoHintInlineNoOptions ||
      probe == RecoveryProbe::NoHintMissingCk ||
      probe == RecoveryProbe::NoHintCurrentWrapperCancel ||
      probe == RecoveryProbe::NoHintCurrentInlineCancel)
    fetchTimeout = std::make_unique<ScopedEnvironmentValue>(
      "NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS", "2000");
  OwnedCorePublicationFixture f;
  ndnsf::di::test::protectedKv::Fixture kv;
  const ndn::Name producer("/spec204/recovery-producer");
  const auto owner = kv.certificate.getIdentity().toUri(ndn::name::UriFormat::ALTERNATE);
  BootstrapProfile profile; profile.userIdentity = producer; profile.serviceName = f.service;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap(); environment.enableProductionIngressForTest();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  BOOST_REQUIRE(environment.user().isNacProducerReadyForTest());
  BOOST_REQUIRE(environment.provider().isNacConsumerReadyForTest());
  environment.user().setEncryptedLargeDataRangeStore(f.store);
  kv.state.originIdentity.providerIdentity = producer.toUri();
  ndnsf::di::nativeExportPrivatePortableKvCheckpoint(kv.plain, kv.state, kv.portableLimits);
  const auto protectedFile = kv.protect();
  const auto input = ndnsf::di::nativeLoadProtectedPortableKvCheckpoint(kv.cipher, protectedFile, kv.limits);
  const auto expected = ndnsf::di::test::protectedKv::read(kv.cipher);
  auto options = f.options; options.persistAuxiliaryPackets = true; options.manifestSuffix = ndn::Name{};
  PublishedContentName content; content.kind = ContentDataKind::Blob; content.label = "kvcache";
  options.contentName = content;
  options.resolveDurableStorageGrant = [held = f.journal, repo = f.repoName, owner, producer,
                                        expiry = f.grantExpiresAt]
    (const ndn::Name& name, const std::string& writer, const std::function<void()>& current) {
    if (ndn::Name(writer) != producer || !producer.isPrefixOf(name))
      throw std::runtime_error("recovery actual producer mismatch");
    if (current) current();
    const auto grant = held->grantUserStorage(owner, repo, keyReferenceDigest(name.toUri()), writer,
      name.toUri(ndn::name::UriFormat::ALTERNATE), expiry, OwnedCorePublicationFixture::now());
    return DurableStorageGrantSelector{grant.repoNode, grant.ownerIdentity, grant.grantId, grant.ownerEpoch};
  };
  const auto prepared = environment.user().prepareServiceRequest(f.service.toUri());
  LargeDataPublishResult publication;
  std::optional<LargeDataPublishResult> primerPublication;
  {
    auto& io = environment.userFace().getIoContext(); io.restart();
    auto guard = boost::asio::make_work_guard(io);
    std::thread worker([&] { io.run(); });
    struct Join { boost::asio::io_context& io; std::thread& thread;
      ~Join() { io.stop(); if (thread.joinable()) thread.join(); } } join{io, worker};
    if (locatorWrapper) {
      options.contentName->label = "kvcache-primer";
      primerPublication = environment.user().publishEncryptedLargeDataFromWorker(prepared, input,
        "kvcache-primer", ndn::time::milliseconds(100), options);
      BOOST_REQUIRE_MESSAGE(primerPublication->success, primerPublication->errorMessage);
      options.contentName->label = "kvcache-next";
    }
    publication = environment.user().publishEncryptedLargeDataFromWorker(prepared, input,
      locatorWrapper ? "kvcache-next" : "kvcache", ndn::time::milliseconds(100), options);
  }
  BOOST_REQUIRE_MESSAGE(publication.success, publication.errorMessage);
  const auto source = publication.durableStorageSource; BOOST_REQUIRE(source);

  std::map<ndn::Name, std::vector<uint8_t>> originals;
  std::map<uint64_t, ndn::Buffer> bodySegments;
  std::vector<ndn::Name> storedManifests{publication.manifestName};
  if (primerPublication) {
    BOOST_CHECK(primerPublication->manifestName != publication.manifestName);
    storedManifests.push_back(primerPublication->manifestName);
  }
  for (const auto& manifestName : storedManifests) {
    const auto stored = f.node->core().lookupDataPacketObject(manifestName.toUri());
    BOOST_REQUIRE(stored);
    for (const auto& wire : ndnsf_distributed_repo::RepoClient::getDataPackets(*f.node, *stored)) {
      const ndn::Data packet(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
      const auto inserted = originals.emplace(packet.getName(), wire);
      if (!inserted.second) BOOST_CHECK(inserted.first->second == wire);
      if (publication.dataPrefix.isPrefixOf(packet.getName())) {
        BOOST_REQUIRE_EQUAL(packet.getName().size(), publication.dataPrefix.size() + 1);
        BOOST_REQUIRE(packet.getName()[-1].isSegment());
        const auto& content = packet.getContent();
        BOOST_REQUIRE(bodySegments.emplace(packet.getName()[-1].toSegment(),
          ndn::Buffer(content.value(), content.value_size())).second);
      }
    }
  }
  // The transparent store exposes original packets, not reconstructed ranges.
  // Inspect the actual published body bytes without reproducing its serializer.
  BOOST_REQUIRE(!bodySegments.empty());
  ndn::Buffer envelopeWire;
  uint64_t nextSegment = 0;
  for (const auto& [number, content] : bodySegments) {
    BOOST_REQUIRE_EQUAL(number, nextSegment++);
    envelopeWire.insert(envelopeWire.end(), content.begin(), content.end());
  }
  HybridMessageEnvelope envelope;
  BOOST_REQUIRE(envelope.WireDecode(ndn::Block(
    ndn::span<const uint8_t>(envelopeWire.data(), envelopeWire.size()))));
  BOOST_REQUIRE_EQUAL(envelope.hasWrappedMessageKey(), !locatorWrapper);
  const auto certificate = environment.keyChain().getPib().getIdentity(producer).getDefaultKey().getDefaultCertificate();
  const auto foreign = makeRsaIdentity(environment.keyChain(), ndn::Name("/spec204/foreign-recovery-signer"));
  environment.disconnectProviderTransportForTest(0);
  auto& receiving = environment.providerFace(0);
  ndn::DummyClientFace repoFace(receiving.getIoContext(), environment.keyChain(), {false, true});
  // Serving/filter handles must be released before their referenced Face.
  struct Unregister { std::shared_ptr<ndnsf_distributed_repo::RepoNode>& node;
    ~Unregister() { node.reset(); } } unregister{f.node};
  f.node->registerOriginalDataServing(repoFace);
  const ndn::Name route(f.actor.getIdentity());
  size_t manifestRequests = 0, bodyRequests = 0, primerManifestRequests = 0,
         primerBodyRequests = 0, ckRequests = 0, wrapperRequests = 0;
  size_t originalReplies = 0, producerReplies = 0;
  auto current = std::make_shared<std::atomic<bool>>(true);
  const auto cancelPendingKeyFetch = probe == RecoveryProbe::NoHintCurrentWrapperCancel ||
      probe == RecoveryProbe::NoHintCurrentInlineCancel;
  ndn::signal::ScopedConnection producerObserver = environment.userFace().onSendData.connect(
    [&](const ndn::Data& packet) { if (producer.isPrefixOf(packet.getName())) ++producerReplies; });
  ndn::signal::ScopedConnection outgoing = receiving.onSendInterest.connect([&](const ndn::Interest& interest) {
    if (!producer.isPrefixOf(interest.getName())) return;
    const auto hints = interest.getForwardingHint();
    if (probe == RecoveryProbe::NoHintNoOptions || probe == RecoveryProbe::NoHintInlineNoOptions ||
        probe == RecoveryProbe::NoHintMissingCk ||
        probe == RecoveryProbe::NoHintCurrentWrapperCancel ||
        probe == RecoveryProbe::NoHintCurrentInlineCancel)
      BOOST_CHECK(hints.empty());
    if (interest.getName() == publication.manifestName) ++manifestRequests;
    else if (primerPublication && interest.getName() == primerPublication->manifestName)
      ++primerManifestRequests;
    else if (publication.dataPrefix.isPrefixOf(interest.getName())) {
      ++bodyRequests;
      if (probe == RecoveryProbe::CancelBody) { current->store(false); return; }
    }
    else if (primerPublication && primerPublication->dataPrefix.isPrefixOf(interest.getName()))
      ++primerBodyRequests;
    else {
      bool ck = false;
      for (const auto& part : interest.getName()) if (part == ndn::name::Component("CK")) ck = true;
      if (ck) {
        ++ckRequests;
        if (probe == RecoveryProbe::NoHintCurrentInlineCancel) {
          current->store(false);
          return;
        }
        if (probe == RecoveryProbe::MissingCk || probe == RecoveryProbe::NoHintMissingCk) return;
      }
      else {
        ++wrapperRequests;
        if (probe == RecoveryProbe::NoHintCurrentWrapperCancel) {
          current->store(false);
          return;
        }
      }
    }
    if ((hints.size() == 1 && hints.front() == route) ||
        ((probe == RecoveryProbe::NoHintNoOptions || probe == RecoveryProbe::NoHintInlineNoOptions ||
          probe == RecoveryProbe::NoHintMissingCk ||
          probe == RecoveryProbe::NoHintCurrentWrapperCancel ||
          probe == RecoveryProbe::NoHintCurrentInlineCancel) && hints.empty())) repoFace.receive(interest);
  });
  ndn::signal::ScopedConnection returning = repoFace.onSendData.connect([&](const ndn::Data& packet) {
    if (!producer.isPrefixOf(packet.getName())) return;
    const auto found = originals.find(packet.getName()); BOOST_REQUIRE(found != originals.end());
    const auto& wire = packet.wireEncode();
    BOOST_CHECK(std::vector<uint8_t>(wire.begin(), wire.end()) == found->second);
    BOOST_CHECK(ndn::security::verifySignature(packet, certificate));
    ++originalReplies;
    if (probe == RecoveryProbe::WrongSigner) {
      ndn::Data wrong(packet); environment.keyChain().sign(wrong, ndn::security::signingByCertificate(foreign));
      receiving.receive(wrong);
    }
    else receiving.receive(packet);
  });
  ProtectedBlobFetchOptions fetchOptions;
  fetchOptions.forwardingHint = {route};
  BOOST_REQUIRE(publication.ciphertextManifestDigest.rfind("sha256:", 0) == 0);
  fetchOptions.manifestContentSha256 = publication.ciphertextManifestDigest.substr(7);
  fetchOptions.maxPlaintextBytes = expected.size();
  fetchOptions.maxEnvelopeBytes = envelopeWire.size();
  if (probe == RecoveryProbe::BadDigest) fetchOptions.manifestContentSha256.assign(64, '0');
  if (probe == RecoveryProbe::PlainBudget) --fetchOptions.maxPlaintextBytes;
  if (probe == RecoveryProbe::EnvelopeBudget) --fetchOptions.maxEnvelopeBytes;
  if (probe == RecoveryProbe::MissingHint) fetchOptions.forwardingHint.clear();
  if (probe == RecoveryProbe::WrongHint) fetchOptions.forwardingHint = {ndn::Name("/wrong/repo")};
  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = "restore-probe"; assignment.service = f.service;
  assignment.workDeadline = std::chrono::steady_clock::now() +
      (cancelPendingKeyFetch ? std::chrono::seconds(5) : std::chrono::milliseconds(800));
  if (probe != RecoveryProbe::MissingCurrent) assignment.workCurrent = [current] { return current->load(); };
  auto deadline = assignment.workDeadline;
  if (probe == RecoveryProbe::ExpiredCaller) deadline = std::chrono::steady_clock::now();
  if (probe == RecoveryProbe::ExpiredAssignment) assignment.workDeadline = std::chrono::steady_clock::now();
  ServiceProvider::CollaborationContext context(environment.provider(), producer,
    ndn::Name("/request/repo-recovery"), RequestMessage(), assignment);
  const auto unwraps = environment.provider().getHybridCryptoCounters().nac_abe_key_unwrap_count.load();
  const auto pendingBefore = receiving.getNPendingInterests();
  std::optional<ndn::Buffer> primerResult;
  const auto fetchStarted = std::chrono::steady_clock::now();
  auto work = std::async(std::launch::async, [&] {
    if (probe == RecoveryProbe::NoHintNoOptions || probe == RecoveryProbe::NoHintInlineNoOptions ||
        probe == RecoveryProbe::NoHintMissingCk) {
      auto result = context.fetchEncryptedLargeData(publication.manifestName);
      if (probe == RecoveryProbe::NoHintNoOptions && result && primerPublication)
        primerResult = context.fetchEncryptedLargeData(primerPublication->manifestName);
      return result;
    }
    if (probe == RecoveryProbe::NoHintCurrentWrapperCancel ||
        probe == RecoveryProbe::NoHintCurrentInlineCancel)
      return context.fetchEncryptedLargeDataCurrent(publication.manifestName, deadline);
    return context.fetchEncryptedLargeDataCurrent(publication.manifestName, deadline, fetchOptions);
  });
  environment.pumpUntil([&] { return work.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; });
  const auto result = work.get(); // joins the worker before callback dependencies leave scope
  const auto fetchElapsed = std::chrono::steady_clock::now() - fetchStarted;
  const auto drainUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  environment.pumpUntil([&] { return std::chrono::steady_clock::now() >= drainUntil; });
  if (cancelPendingKeyFetch)
    BOOST_CHECK_LE(receiving.getNPendingInterests(), pendingBefore);
  BOOST_CHECK_EQUAL(producerReplies, 0U);
  if (probe == RecoveryProbe::Success) {
    BOOST_REQUIRE(result); BOOST_CHECK(*result == ndn::Buffer(expected.begin(), expected.end()));
    BOOST_CHECK_GT(manifestRequests, 0U); BOOST_CHECK_GT(bodyRequests, 0U);
    BOOST_CHECK_GT(ckRequests, 0U); BOOST_CHECK_GT(originalReplies, 2U);
    BOOST_CHECK_EQUAL(environment.provider().getHybridCryptoCounters().nac_abe_key_unwrap_count.load(), unwraps + 1);
    if (locatorWrapper) BOOST_CHECK_GT(wrapperRequests, 0U); else BOOST_CHECK_EQUAL(wrapperRequests, 0U);
  }
  else if (probe == RecoveryProbe::NoHintNoOptions) {
    BOOST_REQUIRE(result);
    BOOST_REQUIRE(primerResult);
    BOOST_CHECK(*result == ndn::Buffer(expected.begin(), expected.end()));
    BOOST_CHECK(*primerResult == ndn::Buffer(expected.begin(), expected.end()));
    BOOST_CHECK_GT(manifestRequests, 0U); BOOST_CHECK_GT(bodyRequests, 0U);
    BOOST_CHECK_GT(primerManifestRequests, 0U); BOOST_CHECK_GT(primerBodyRequests, 0U);
    BOOST_CHECK_GT(ckRequests, 0U); BOOST_CHECK_GT(wrapperRequests, 0U);
    BOOST_CHECK_GT(originalReplies, 2U);
    BOOST_CHECK_EQUAL(environment.provider().getHybridCryptoCounters().nac_abe_key_unwrap_count.load(), unwraps + 1);
  }
  else if (probe == RecoveryProbe::NoHintInlineNoOptions) {
    BOOST_REQUIRE(result);
    BOOST_CHECK(*result == ndn::Buffer(expected.begin(), expected.end()));
    BOOST_CHECK_GT(manifestRequests, 0U); BOOST_CHECK_GT(bodyRequests, 0U);
    BOOST_CHECK_GT(ckRequests, 0U); BOOST_CHECK_EQUAL(wrapperRequests, 0U);
    BOOST_CHECK_GT(originalReplies, 2U);
    BOOST_CHECK_EQUAL(environment.provider().getHybridCryptoCounters().nac_abe_key_unwrap_count.load(), unwraps + 1);
  }
  else {
    BOOST_CHECK(!result);
    BOOST_CHECK_EQUAL(environment.provider().getHybridCryptoCounters().nac_abe_key_unwrap_count.load(), unwraps);
    if (probe != RecoveryProbe::CancelBody && probe != RecoveryProbe::MissingCk &&
        probe != RecoveryProbe::NoHintMissingCk &&
        probe != RecoveryProbe::NoHintCurrentWrapperCancel &&
        probe != RecoveryProbe::NoHintCurrentInlineCancel) BOOST_CHECK_EQUAL(bodyRequests, 0U);
    if (probe == RecoveryProbe::MissingCk || probe == RecoveryProbe::NoHintMissingCk ||
        probe == RecoveryProbe::NoHintCurrentInlineCancel)
      BOOST_CHECK_GT(ckRequests, 0U);
    if (probe == RecoveryProbe::NoHintCurrentWrapperCancel) BOOST_CHECK_GT(wrapperRequests, 0U);
    if (probe == RecoveryProbe::NoHintMissingCk ||
        probe == RecoveryProbe::NoHintCurrentWrapperCancel ||
        probe == RecoveryProbe::NoHintCurrentInlineCancel) {
      BOOST_CHECK_GT(bodyRequests, 0U);
      BOOST_CHECK(fetchElapsed < (cancelPendingKeyFetch ? std::chrono::seconds(2) : std::chrono::seconds(5)));
    }
    if (probe == RecoveryProbe::MissingHint || probe == RecoveryProbe::MissingCurrent ||
        probe == RecoveryProbe::ExpiredCaller || probe == RecoveryProbe::ExpiredAssignment)
      BOOST_CHECK_EQUAL(manifestRequests, 0U);
  }
}
} // namespace

BOOST_AUTO_TEST_CASE(RepoHintFetchesProtectedKvAndColdCkAfterProducerDisconnect)
{
  checkRepoRecovery(false, RecoveryProbe::Success);
  checkRepoRecovery(true, RecoveryProbe::Success);
}

BOOST_AUTO_TEST_CASE(RepoHintRejectsManifestDigestAndAllocationBoundsBeforeBody)
{
  for (const auto probe : {RecoveryProbe::BadDigest, RecoveryProbe::PlainBudget,
      RecoveryProbe::EnvelopeBudget, RecoveryProbe::MissingHint, RecoveryProbe::WrongHint,
      RecoveryProbe::WrongSigner}) checkRepoRecovery(false, probe);
}

BOOST_AUTO_TEST_CASE(RepoHintCurrentDispatchCancellationAndMissingCk)
{
  for (const auto probe : {RecoveryProbe::MissingCurrent, RecoveryProbe::ExpiredCaller,
      RecoveryProbe::ExpiredAssignment, RecoveryProbe::CancelBody, RecoveryProbe::MissingCk})
    checkRepoRecovery(false, probe);
  checkRepoRecovery(true, RecoveryProbe::MissingCk);
}

BOOST_AUTO_TEST_CASE(RepoNoHintFetchesTwoProtectedObjectsAndCancelsPendingKeyResolution)
{
  checkRepoRecovery(false, RecoveryProbe::NoHintInlineNoOptions);
  checkRepoRecovery(true, RecoveryProbe::NoHintNoOptions);
  checkRepoRecovery(true, RecoveryProbe::NoHintMissingCk);
  checkRepoRecovery(true, RecoveryProbe::NoHintCurrentWrapperCancel);
  checkRepoRecovery(false, RecoveryProbe::NoHintCurrentInlineCancel);
}

BOOST_AUTO_TEST_CASE(AuxiliaryPersistenceRejectsPlaceholderAndPreservesOlderObject)
{
  OwnedCorePublicationFixture f; f.authorize(f.owner); f.options.persistAuxiliaryPackets = true;
  auto missing = f.publish(); BOOST_CHECK(!missing.success);
  BOOST_CHECK(missing.errorMessage.find("DURABLE_AUXILIARY_PACKETS_MISSING") != std::string::npos);
  BOOST_CHECK(f.node->list().empty()); BOOST_CHECK(f.resolved.empty());
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage(f.owner, f.repoName).committedBytes, 0U);
  f.options.persistAuxiliaryPackets = false;
  const auto old = f.publish(); BOOST_REQUIRE_MESSAGE(old.success, old.errorMessage);
  const auto oldWire = old.durableStorageSource->readOriginalDataPacket(old.manifestName);
  const auto usage = f.journal->userStorageUsage(f.owner, f.repoName).committedBytes;
  f.options.persistAuxiliaryPackets = true; missing = f.publish(); BOOST_CHECK(!missing.success);
  BOOST_CHECK_EQUAL(f.node->list().size(), 1U);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage(f.owner, f.repoName).committedBytes, usage);
  BOOST_CHECK(old.durableStorageSource->readOriginalDataPacket(old.manifestName) == oldWire);
}

BOOST_AUTO_TEST_CASE(AuxiliaryPacketSetIsExactChargedAndModeSeparated)
{
  OwnedCorePublicationFixture f; f.authorize(f.owner);
  const auto bodyOnly = f.publish(); BOOST_REQUIRE_MESSAGE(bodyOnly.success, bodyOnly.errorMessage);
  BOOST_CHECK(bodyOnly.durableStorageSource->auxiliaryDataPacketNames().empty());
  f.user->seedOpaqueOriginalAuxiliaries(f.service, f.keys); f.options.persistAuxiliaryPackets = true;
  const auto complete = f.publish(); BOOST_REQUIRE_MESSAGE(complete.success, complete.errorMessage);
  BOOST_CHECK(complete.publicationIdentity != bodyOnly.publicationIdentity);
  BOOST_CHECK(complete.manifestName != bodyOnly.manifestName);
  const auto names = complete.durableStorageSource->auxiliaryDataPacketNames(); BOOST_REQUIRE_EQUAL(names.size(), 2U);
  BOOST_CHECK(names == complete.auxiliaryPacketNames);
  const auto stored = f.node->core().lookupDataPacketObject(complete.manifestName.toUri()); BOOST_REQUIRE(stored);
  const auto wires = ndnsf_distributed_repo::RepoClient::getDataPackets(*f.node, *stored);
  const auto usage = f.journal->userStorageUsage(f.owner, f.repoName).committedBytes;
  const auto repeated = f.publish(); BOOST_REQUIRE_MESSAGE(repeated.success, repeated.errorMessage);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage(f.owner, f.repoName).committedBytes, usage);
  BOOST_CHECK(complete.manifestName == repeated.manifestName);
  BOOST_CHECK(ndnsf_distributed_repo::RepoClient::getDataPackets(*f.node, *stored) == wires);
  EncryptedLargeDataCommitOptions options;
  options.retention = EncryptedLargeDataRetention::Durable; options.publicationIdentity = complete.publicationIdentity;
  options.protectionEpoch = complete.protectionEpoch; options.keyReferenceId = complete.keyReferenceId;
  options.keyReferenceVersion = complete.keyReferenceVersion; options.ciphertextManifestDigest = complete.ciphertextManifestDigest;
  options.servingLocator = complete.servingLocator; options.contentDigest = complete.contentDigest;
  options.plaintextSize = complete.plaintextSize; options.packetStorageFormat = "original-packet-set";
  options.manifestName = complete.manifestName.toUri(); options.dataPrefix = complete.dataPrefix.toUri();
  options.auxiliaryPacketNames = names;
  options.storageActor = DurableStorageActorContext{f.actor.getIdentity().toUri(ndn::name::UriFormat::ALTERNATE),
    f.options.resolveDurableStorageGrant(complete.manifestName,
      f.actor.getIdentity().toUri(ndn::name::UriFormat::ALTERNATE), {})};
  std::vector<std::pair<ndn::Name, std::vector<uint8_t>>> packets;
  for (const auto& wire : wires) {
    const ndn::Data packet(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
    packets.emplace_back(packet.getName(), wire); options.packetWireBytes += wire.size();
  }
  const auto reject = [&](const auto& changedPackets, const auto& changedOptions) {
    size_t next = 0;
    BOOST_CHECK_THROW(f.store->commitOriginalDataPackets(complete.manifestName, changedOptions,
      [&]() -> std::optional<std::pair<ndn::Name, std::vector<uint8_t>>> {
        if (next == changedPackets.size()) return std::nullopt; return changedPackets[next++];
      }), std::invalid_argument);
    BOOST_CHECK_EQUAL(f.journal->userStorageUsage(f.owner, f.repoName).committedBytes, usage);
    BOOST_CHECK(ndnsf_distributed_repo::RepoClient::getDataPackets(*f.node, *stored) == wires);
  };
  auto changed = packets; changed.pop_back(); reject(changed, options);
  changed = packets; std::swap(changed[changed.size() - 1], changed[changed.size() - 2]); reject(changed, options);
  changed = packets; changed.push_back(changed.back()); reject(changed, options);
  auto bad = options; bad.auxiliaryPacketNames.push_back(names.back()); reject(packets, bad);
  bad = options; bad.auxiliaryPacketNames.front() = ndn::Name("/foreign/key"); reject(packets, bad);
  bad = options; bad.auxiliaryPacketNames.assign(EncryptedLargeDataCommitOptions::MaxAuxiliaryPacketCount + 1, names.front());
  reject(packets, bad);
  const auto foreign = makeRsaIdentity(f.keys, ndn::Name("/foreign/key-signer"));
  auto last = ndn::Data(ndn::Block(ndn::span<const uint8_t>(packets.back().second.data(), packets.back().second.size())));
  f.keys.sign(last, ndn::security::signingByCertificate(foreign));
  const auto& foreignWire = last.wireEncode(); changed = packets;
  changed.back().second.assign(foreignWire.begin(), foreignWire.end());
  bad = options; bad.packetWireBytes = 0; for (const auto& packet : changed) bad.packetWireBytes += packet.second.size();
  reject(changed, bad);
  size_t next = 0;
  BOOST_CHECK_THROW(f.store->commitOriginalDataPackets(complete.manifestName, options,
    [&]() -> std::optional<std::pair<ndn::Name, std::vector<uint8_t>>> {
      if (next == packets.size()) return std::nullopt; return packets[next++];
    }, [&] { if (next == packets.size()) throw std::runtime_error("cancel auxiliary packet"); }), std::runtime_error);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage(f.owner, f.repoName).committedBytes, usage);
  BOOST_CHECK(ndnsf_distributed_repo::RepoClient::getDataPackets(*f.node, *stored) == wires);
}

BOOST_AUTO_TEST_CASE(AuxiliaryRealNacOriginalsSurviveProducerExitAndFreshOwnerRetention)
{
  OwnedCorePublicationFixture f; ndnsf::di::test::protectedKv::Fixture kv;
  const auto owner = kv.certificate.getIdentity().toUri(ndn::name::UriFormat::ALTERNATE);
  const ndn::Name producer("/spec204/real-key-producer");
  ndn::Name manifestName; std::string publicationIdentity;
  std::vector<ndn::Name> auxiliaryNames;
  std::map<std::string, std::vector<uint8_t>> originals;
  std::uint64_t charge = 0;
  {
    BootstrapProfile profile; profile.userIdentity = producer; profile.serviceName = f.service;
    NdnsfIntegrationEnvironment environment(profile); environment.bootstrap(); environment.enableProductionIngressForTest();
    BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
    BOOST_REQUIRE(environment.user().isNacProducerReadyForTest());
    environment.user().setEncryptedLargeDataRangeStore(f.store);
    kv.state.originIdentity.providerIdentity = producer.toUri();
    ndnsf::di::nativeExportPrivatePortableKvCheckpoint(kv.plain, kv.state, kv.portableLimits);
    const auto protectedFile = kv.protect();
    const auto input = ndnsf::di::nativeLoadProtectedPortableKvCheckpoint(kv.cipher, protectedFile, kv.limits);
    auto options = f.options; options.persistAuxiliaryPackets = true; options.manifestSuffix = ndn::Name{};
    PublishedContentName content; content.kind = ContentDataKind::Blob; content.label = "kvcache"; options.contentName = content;
    options.resolveDurableStorageGrant = [held = f.journal, repo = f.repoName, owner, producer, expiry = f.grantExpiresAt]
      (const ndn::Name& name, const std::string& writer, const std::function<void()>& current) {
      if (ndn::Name(writer) != producer || !producer.isPrefixOf(name)) throw std::runtime_error("actual key producer mismatch");
      if (current) current();
      const auto grant = held->grantUserStorage(owner, repo, keyReferenceDigest(name.toUri()), writer,
        name.toUri(ndn::name::UriFormat::ALTERNATE), expiry, OwnedCorePublicationFixture::now());
      return DurableStorageGrantSelector{grant.repoNode, grant.ownerIdentity, grant.grantId, grant.ownerEpoch};
    };
    const auto prepared = environment.user().prepareServiceRequest(f.service.toUri());
    const auto wrapsBefore = environment.user().getHybridCryptoCounters().nac_abe_key_wrap_count.load();
    LargeDataPublishResult result;
    {
      auto& io = environment.userFace().getIoContext(); io.restart(); auto guard = boost::asio::make_work_guard(io);
      std::thread thread([&] { io.run(); });
      struct Join { boost::asio::io_context& io; std::thread& thread;
        ~Join() { io.stop(); if (thread.joinable()) thread.join(); } } join{io, thread};
      result = environment.user().publishEncryptedLargeDataFromWorker(prepared, input, "kvcache",
        ndn::time::milliseconds(100), options);
    }
    BOOST_REQUIRE_MESSAGE(result.success, result.errorMessage);
    BOOST_CHECK(environment.user().getHybridCryptoCounters().nac_abe_key_wrap_count.load() > wrapsBefore);
    manifestName = result.manifestName; publicationIdentity = result.publicationIdentity;
    auxiliaryNames = result.auxiliaryPacketNames; BOOST_REQUIRE(auxiliaryNames.size() >= 2U);
    BOOST_CHECK(auxiliaryNames == result.durableStorageSource->auxiliaryDataPacketNames());
    const auto certificate = environment.keyChain().getPib().getIdentity(producer).getDefaultKey().getDefaultCertificate();
    for (const auto& name : auxiliaryNames) {
      const auto wire = result.durableStorageSource->readOriginalDataPacket(name);
      const ndn::Data packet(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
      BOOST_CHECK(producer.isPrefixOf(packet.getName())); BOOST_CHECK(ndn::security::verifySignature(packet, certificate));
      originals.emplace(name.toUri(), wire);
    }
    const auto stored = f.node->core().lookupDataPacketObject(manifestName.toUri()); BOOST_REQUIRE(stored);
    const auto all = ndnsf_distributed_repo::RepoClient::getDataPackets(*f.node, *stored);
    std::uint64_t wireBytes = 0; for (const auto& wire : all) wireBytes += wire.size();
    const auto measurement = f.node->core().probeObjectMetadata(stored->objectName);
    charge = f.journal->userStorageUsage(owner, f.repoName).committedBytes;
    BOOST_CHECK_EQUAL(charge, wireBytes + measurement.manifestWireBytes);
    BOOST_CHECK_EQUAL(f.journal->userStorageUsage(owner, f.repoName).reservedBytes, 0U);
  }
  // No Producer, Face/IMS, NAC cache or original Source survives this point.
  f.options.resolveDurableStorageGrant = {}; f.user.reset(); f.store.reset(); f.node.reset(); f.journal.reset();
  ndnsf_distributed_repo::StorageCapability capability;
  capability.repoNode = f.repoName; capability.repoMode = "persistent"; capability.freeBytes = 64U << 20;
  auto reopened = std::make_shared<ndnsf_distributed_repo::RepoNode>(ndn::Name("/spec204/core-service"), capability,
    ndnsf_distributed_repo::makeFilesystemRepoStore((f.root / "objects").string(), 1U << 20, 1U << 20),
    f.repoName, f.actor.getIdentity().toUri(), (f.root / "metadata.db").string());
  auto store = reopened->createEncryptedLargeDataStore(); const auto hit = store->lookupDurable(publicationIdentity); BOOST_REQUIRE(hit);
  BOOST_CHECK_THROW(hit->source->auxiliaryDataPacketNames(), std::runtime_error);
  auto journal = std::make_shared<ndnsf_distributed_repo::SqliteArtifactMetadataStore>((f.root / "metadata.db").string(), true, 16, false);
  const std::string nextActor = "/spec204/new-key-reader";
  const auto grant = journal->grantUserStorage(owner, f.repoName, "fresh-original-key-reader", nextActor,
    manifestName.toUri(ndn::name::UriFormat::ALTERNATE), f.now() + 60000, f.now());
  EncryptedLargeDataCommitOptions retain;
  retain.retention = EncryptedLargeDataRetention::Durable; retain.publicationIdentity = hit->publicationIdentity;
  retain.protectionEpoch = hit->protectionEpoch; retain.keyReferenceId = hit->keyReferenceId;
  retain.keyReferenceVersion = hit->keyReferenceVersion; retain.ciphertextManifestDigest = hit->ciphertextManifestDigest;
  retain.servingLocator = hit->servingLocator; retain.contentDigest = hit->contentDigest; retain.plaintextSize = hit->plaintextSize;
  retain.packetStorageFormat = hit->packetStorageFormat; retain.manifestName = hit->manifestName;
  retain.dataPrefix = hit->dataPrefix; retain.packetWireBytes = hit->packetWireBytes;
  retain.storageActor = DurableStorageActorContext{nextActor, {grant.repoNode, grant.ownerIdentity, grant.grantId, grant.ownerEpoch}};
  const auto source = store->retainDurableSource(*hit, retain); BOOST_REQUIRE(source);
  BOOST_CHECK(source->auxiliaryDataPacketNames() == auxiliaryNames);
  for (const auto& name : auxiliaryNames) BOOST_CHECK(source->readOriginalDataPacket(name) == originals.at(name.toUri()));
  BOOST_CHECK_EQUAL(journal->userStorageUsage(owner, f.repoName).committedBytes, charge);
  const auto claims = journal->listUserStorage(owner, f.repoName, "", 10); BOOST_REQUIRE_EQUAL(claims.size(), 1U);
  journal->prepareUserStorageCleanup(owner, f.repoName, "remove-real-key-set", claims.front().object.objectName,
    claims.front().ownerEpoch, f.now());
  BOOST_CHECK_THROW(source->auxiliaryDataPacketNames(), std::runtime_error);
  BOOST_CHECK_THROW(source->readOriginalDataPacket(auxiliaryNames.front()), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(KvOwnerCompletePromotionIsAtomicIdempotentAndReopens)
{
  OwnedCorePublicationFixture f; ndnsf::di::test::protectedKv::Fixture kv;
  f.authorize(kv.certificate.getIdentity().toUri()); OwnerKvConversationFixture owner(f, f.owner);
  auto publications = owner.publish(kv);
  BOOST_REQUIRE(!owner.coordinator->find(owner.id())->protectedKvReferences);
  const auto promoted = owner.coordinator->promoteProtectedKvSet(owner.id(), owner.digest(), publications);
  BOOST_REQUIRE(promoted.protectedKvReferences); BOOST_CHECK_EQUAL(promoted.protectedKvReferences->size(), 2U);
  for (const auto& [role, publication] : publications) {
    const auto& reference = promoted.protectedKvReferences->at(role);
    BOOST_CHECK_EQUAL(reference.at("version").get<std::uint64_t>(), 2U);
    BOOST_CHECK_EQUAL(reference.at("keyEnvelope").get<std::string>(), publication.checkpoint.keyEnvelope);
  }
  const auto logSize = fs::file_size(owner.log());
  const auto repeated = owner.coordinator->promoteProtectedKvSet(owner.id(), owner.digest(), publications);
  BOOST_CHECK(repeated.protectedKvReferences == promoted.protectedKvReferences);
  BOOST_CHECK_EQUAL(fs::file_size(owner.log()), logSize);
  BOOST_CHECK_EQUAL(owner.config.journal->readConversations(owner.time).size(), 1U);
  const auto wire = ndnsf::di::test::protectedKv::read(owner.log());
  BOOST_CHECK(wire.find("checkpointIdentityDigest") == std::string::npos);
  BOOST_CHECK(wire.find("keyReferenceId") == std::string::npos);
  BOOST_CHECK(wire.find("wrappedKey") == std::string::npos);
  publications.clear(); f.options.resolveDurableStorageGrant = {}; f.user.reset(); f.store.reset(); f.node.reset(); f.journal.reset();
  owner.reopen(); const auto restored = owner.coordinator->find(owner.id()); BOOST_REQUIRE(restored);
  BOOST_CHECK(restored->protectedKvReferences == promoted.protectedKvReferences);
  BOOST_CHECK_EQUAL(restored->checkpoint.successorContextEpoch, 1U);
}

BOOST_AUTO_TEST_CASE(KvActiveTurnRecoveryReferenceUsesOwnedParent)
{
  using namespace ndnsf::di;
  OwnedCorePublicationFixture producer; ndnsf::di::test::protectedKv::Fixture kv;
  producer.authorize(kv.certificate.getIdentity().toUri());
  OwnerKvConversationFixture owner(producer, producer.owner);
  const auto continuation = owner.continuation();
  auto turn = owner.coordinator->beginTurn(continuation, "opaque-unsaved-turn");
  BOOST_CHECK(!owner.coordinator->protectedKvForTurn(turn));
  owner.coordinator->abortTurn(turn, NativeDiError("CANCELLED", "local", "test", "controlled abort"));
  auto publications = owner.publish(kv);
  const auto promoted = owner.coordinator->promoteProtectedKvSet(owner.id(), owner.digest(), publications);
  BOOST_REQUIRE(promoted.protectedKvReferences);
  publications.clear(); producer.user.reset(); producer.store.reset(); producer.node.reset(); producer.journal.reset();
  owner.reopen();
  turn = owner.coordinator->beginTurn(continuation, "opaque-active-turn");
  BOOST_CHECK(owner.coordinator->protectedKvForTurn(turn) == promoted.protectedKvReferences);
  auto recovery = owner.coordinator->protectedKvRecoveryForTurn(turn);
  BOOST_REQUIRE(recovery);
  BOOST_CHECK(recovery->references == *promoted.protectedKvReferences);
  BOOST_CHECK(recovery->checkpoint == owner.checkpoint);
  BOOST_REQUIRE_EQUAL(recovery->originReceipts.size(), owner.placement.size());
  const auto ownedRecord = owner.coordinator->find(owner.id());
  BOOST_REQUIRE(ownedRecord);
  for (const auto& encoded : ownedRecord->checkpoint.transcript.at("providerRoleReceipts")) {
    const auto raw = nativeConversationBase64Decode(encoded.get<std::string>());
    const auto receipt = nativeReadProviderConversationStateReceipt(nativeParseJson(raw));
    BOOST_CHECK_EQUAL(recovery->originReceipts.at(receipt.roleName), raw);
    BOOST_CHECK_EQUAL(receipt.computedDigest(),
      recovery->checkpoint.at("roleReceiptDigests").at(receipt.roleName).get<std::string>());
    BOOST_CHECK(ndn::Name(receipt.providerIdentity) == ndn::Name(owner.placement.at(receipt.roleName)));
  }
  const auto expectedRecovery = *recovery;
  recovery->checkpoint["expiresAtMs"] = 0U;
  recovery->references.clear(); recovery->originReceipts.clear();
  BOOST_CHECK(*owner.coordinator->protectedKvRecoveryForTurn(turn) == expectedRecovery);
  auto copy = owner.coordinator->protectedKvForTurn(turn);
  copy->erase("/role/A");
  BOOST_CHECK(owner.coordinator->protectedKvForTurn(turn) == promoted.protectedKvReferences);
  for (const auto& mutate : std::vector<std::function<void(NativeConversationTurn&)>>{
    [](auto& value) { value.ticket += "forged"; },
    [](auto& value) { value.executionRequestId += "other"; },
    [](auto& value) { value.parent.conversationId += "other"; },
    [](auto& value) { ++value.parent.parentContextEpoch; },
    [](auto& value) { value.parent.parentCheckpointDigest = keyReferenceDigest("other parent"); },
    [](auto& value) { value.parent.requestContractDigest = keyReferenceDigest("other contract"); },
    [](auto& value) { value.parent.planRoleMapDigest = keyReferenceDigest("other placement"); },
    [](auto& value) { value.parent.expectedRoles.clear(); },
    [](auto& value) { ++value.parent.retentionDeadlineMs; }}) {
    auto invalid = turn; mutate(invalid);
    BOOST_CHECK_THROW(owner.coordinator->protectedKvForTurn(invalid), std::runtime_error);
    BOOST_CHECK_THROW(owner.coordinator->protectedKvRecoveryForTurn(invalid), std::runtime_error);
  }
  auto replacement = owner.coordinator->replaceAttempt(turn, "opaque-replacement-turn");
  BOOST_CHECK_THROW(owner.coordinator->protectedKvForTurn(turn), std::runtime_error);
  BOOST_CHECK(owner.coordinator->protectedKvForTurn(replacement) == promoted.protectedKvReferences);
  const auto remapped = owner.coordinator->bindAttemptPlanRoleMap(replacement,
    {{"/role/A", "/fresh/provider/b"}, {"/role/B", "/fresh/provider/c"}});
  // New execution placement must not change the saved parent's Producer map.
  BOOST_CHECK(owner.coordinator->protectedKvForTurn(remapped) == promoted.protectedKvReferences);
  BOOST_CHECK(*owner.coordinator->protectedKvRecoveryForTurn(remapped) == expectedRecovery);
  owner.time = owner.expiry;
  BOOST_CHECK_THROW(owner.coordinator->protectedKvForTurn(remapped), std::runtime_error);
  --owner.time;
  owner.coordinator->abortTurn(remapped, NativeDiError("CANCELLED", "local", "test", "controlled abort"));
  BOOST_CHECK_THROW(owner.coordinator->protectedKvForTurn(remapped), std::runtime_error);
  BOOST_CHECK(owner.coordinator->find(owner.id())->protectedKvReferences == promoted.protectedKvReferences);
}

BOOST_AUTO_TEST_CASE(KvOwnerLongKeyEnvelopePassesActualPromotionAndReopen)
{
  OwnedCorePublicationFixture f; ndnsf::di::test::protectedKv::Fixture kv;
  kv.certificate = kv.ownerKeys.createIdentity(ndn::Name("/" + std::string(1900, 'o')),
    ndn::RsaKeyParams(2048)).getDefaultKey().getDefaultCertificate();
  f.authorize(kv.certificate.getIdentity().toUri()); OwnerKvConversationFixture owner(f, f.owner);
  auto publications = owner.publish(kv);
  for (const auto& [role, publication] : publications) {
    BOOST_REQUIRE_GT(publication.checkpoint.keyEnvelope.size(), 4096U);
    BOOST_REQUIRE_LE(publication.checkpoint.keyEnvelope.size(), 16384U);
  }
  const auto saved = owner.coordinator->promoteProtectedKvSet(owner.id(), owner.digest(), publications);
  BOOST_REQUIRE(saved.protectedKvReferences);
  publications.clear(); f.options.resolveDurableStorageGrant = {}; f.user.reset();
  f.store.reset(); f.node.reset(); f.journal.reset();
  owner.reopen(); const auto restored = owner.coordinator->find(owner.id()); BOOST_REQUIRE(restored);
  BOOST_CHECK(restored->protectedKvReferences == saved.protectedKvReferences);
}

BOOST_AUTO_TEST_CASE(KvOwnerLegacyReferencesReopenWithoutInventingKeyEnvelope)
{
  OwnedCorePublicationFixture f; ndnsf::di::test::protectedKv::Fixture kv;
  f.authorize(kv.certificate.getIdentity().toUri()); OwnerKvConversationFixture owner(f, f.owner);
  const auto publications = owner.publish(kv);
  ndnsf::di::NativeJson legacy = ndnsf::di::NativeJson::object();
  for (const auto& [role, publication] : publications) {
    auto reference = ndnsf::di::nativeProtectedKvPublicationReference(publication);
    reference["version"] = 1U; reference.erase("keyEnvelope"); legacy[role] = std::move(reference);
  }
  BOOST_CHECK_NO_THROW(ndnsf::di::nativeValidateProtectedKvReferences(legacy, owner.checkpoint, owner.placement));
  auto mixed = legacy;
  mixed["/role/A"] = ndnsf::di::nativeProtectedKvPublicationReference(publications.at("/role/A"));
  BOOST_CHECK_NO_THROW(ndnsf::di::nativeValidateProtectedKvReferences(mixed, owner.checkpoint, owner.placement));
  owner.config.journal->appendProtectedKvSet(owner.checkpointWire, legacy, owner.time);
  owner.reopen(); const auto restored = owner.coordinator->find(owner.id()); BOOST_REQUIRE(restored);
  BOOST_REQUIRE(restored->protectedKvReferences);
  BOOST_CHECK(*restored->protectedKvReferences == legacy);
  for (const auto& reference : restored->protectedKvReferences->items())
    BOOST_CHECK(!reference.value().contains("keyEnvelope"));
  const auto size = fs::file_size(owner.log());
  BOOST_CHECK_THROW(owner.coordinator->promoteProtectedKvSet(owner.id(), owner.digest(), publications), std::runtime_error);
  BOOST_CHECK_EQUAL(fs::file_size(owner.log()), size);
  BOOST_CHECK(owner.coordinator->find(owner.id())->protectedKvReferences == restored->protectedKvReferences);
}

BOOST_AUTO_TEST_CASE(KvOwnerMissingAliasStaleAndDeletedReferencesRejectWithoutVisibility)
{
  OwnedCorePublicationFixture f; ndnsf::di::test::protectedKv::Fixture kv;
  f.authorize(kv.certificate.getIdentity().toUri()); OwnerKvConversationFixture owner(f, f.owner);
  const auto publications = owner.publish(kv); const auto initialSize = fs::file_size(owner.log());
  auto invalid = publications; invalid.erase("/role/B");
  BOOST_CHECK_THROW(owner.coordinator->promoteProtectedKvSet(owner.id(), owner.digest(), invalid), std::runtime_error);
  invalid = publications; invalid["/role/B"] = publications.at("/role/A");
  BOOST_CHECK_THROW(owner.coordinator->promoteProtectedKvSet(owner.id(), owner.digest(), invalid), std::runtime_error);
  invalid = publications; invalid["/role/A"].checkpoint.ownerIdentity = ndn::Name("/other-owner");
  BOOST_CHECK_THROW(owner.coordinator->promoteProtectedKvSet(owner.id(), owner.digest(), invalid), std::runtime_error);
  invalid = publications; invalid["/role/A"].checkpoint.producerPrefix = ndn::Name("/other-producer");
  BOOST_CHECK_THROW(owner.coordinator->promoteProtectedKvSet(owner.id(), owner.digest(), invalid), std::runtime_error);
  invalid = publications; invalid["/role/A"].transport.keyReferenceVersion = "forged-version";
  BOOST_CHECK_THROW(invalid["/role/A"].requireCurrent(), std::runtime_error);
  BOOST_CHECK_THROW(owner.coordinator->promoteProtectedKvSet(owner.id(), owner.digest(), invalid), std::runtime_error);
  invalid = publications; invalid["/role/A"].checkpoint.keyEnvelope += " ";
  BOOST_CHECK_THROW(invalid["/role/A"].requireCurrent(), std::runtime_error);
  BOOST_CHECK_THROW(owner.coordinator->promoteProtectedKvSet(owner.id(), owner.digest(), invalid), std::runtime_error);
  BOOST_CHECK_THROW(owner.coordinator->promoteProtectedKvSet(owner.id(), keyReferenceDigest("stale"), publications), std::runtime_error);
  BOOST_CHECK_EQUAL(fs::file_size(owner.log()), initialSize);
  BOOST_CHECK(!owner.coordinator->find(owner.id())->protectedKvReferences);
  ndnsf::di::NativeConversationContinuation continuation;
  continuation.mode = "APPEND_DELTA"; continuation.conversationId = owner.id();
  continuation.parentContextEpoch = 1; continuation.serviceName = owner.config.serviceName;
  continuation.planRoleMapDigest = owner.checkpoint.at("planRoleMapDigest").get<std::string>();
  continuation.parentCheckpointWire = owner.checkpointWire; continuation.parentCheckpointDigest = owner.digest();
  continuation.requestContractDigest = keyReferenceDigest("pending-request"); continuation.retentionDeadlineMs = owner.expiry;
  continuation.generationId = std::string(32, '4'); continuation.expectedRoles = {"/role/A", "/role/B"};
  continuation.canonicalTokenIds = owner.coordinator->find(owner.id())->checkpoint.transcript.at("canonicalTokenIds")
    .get<std::vector<std::int64_t>>(); continuation.canonicalTokenIds.push_back(99);
  owner.coordinator->beginTurn(continuation, "pending-turn");
  BOOST_CHECK_THROW(owner.coordinator->promoteProtectedKvSet(owner.id(), owner.digest(), publications), std::runtime_error);
  BOOST_CHECK_EQUAL(fs::file_size(owner.log()), initialSize); owner.reopen();
  const auto promoted = owner.coordinator->promoteProtectedKvSet(owner.id(), owner.digest(), publications);
  const auto claims = f.journal->listUserStorage(f.owner, f.repoName, "", 10); BOOST_REQUIRE_EQUAL(claims.size(), 2U);
  f.journal->prepareUserStorageCleanup(f.owner, f.repoName, "kv-owner-delete", claims.front().object.objectName,
    claims.front().ownerEpoch, f.now());
  const auto size = fs::file_size(owner.log());
  BOOST_CHECK_THROW(owner.coordinator->promoteProtectedKvSet(owner.id(), owner.digest(), publications), std::runtime_error);
  BOOST_CHECK_EQUAL(fs::file_size(owner.log()), size);
  BOOST_CHECK(owner.coordinator->find(owner.id())->protectedKvReferences == promoted.protectedKvReferences);
}

BOOST_AUTO_TEST_CASE(KvOwnerConflictingActualPublicationAndQuotaFailureKeepOldState)
{
  OwnedCorePublicationFixture f; ndnsf::di::test::protectedKv::Fixture kv;
  f.authorize(kv.certificate.getIdentity().toUri()); OwnerKvConversationFixture owner(f, f.owner);
  const auto publications = owner.publish(kv); const auto initialSize = fs::file_size(owner.log());
  owner.reopen(initialSize + 1);
  BOOST_CHECK_THROW(owner.coordinator->promoteProtectedKvSet(owner.id(), owner.digest(), publications), std::runtime_error);
  BOOST_CHECK_EQUAL(fs::file_size(owner.log()), initialSize);
  BOOST_CHECK(!owner.coordinator->find(owner.id())->protectedKvReferences);
  owner.reopen(); const auto saved = owner.coordinator->promoteProtectedKvSet(owner.id(), owner.digest(), publications);
  const auto other = owner.publish(kv); const auto size = fs::file_size(owner.log());
  BOOST_CHECK_THROW(owner.coordinator->promoteProtectedKvSet(owner.id(), owner.digest(), other), std::runtime_error);
  BOOST_CHECK_EQUAL(fs::file_size(owner.log()), size);
  BOOST_CHECK(owner.coordinator->find(owner.id())->protectedKvReferences == saved.protectedKvReferences);
}

BOOST_AUTO_TEST_CASE(KvOwnerSuccessorAndExpiryDoNotInheritReferences)
{
  OwnedCorePublicationFixture f; ndnsf::di::test::protectedKv::Fixture kv;
  f.authorize(kv.certificate.getIdentity().toUri()); OwnerKvConversationFixture owner(f, f.owner);
  const auto publications = owner.publish(kv);
  owner.coordinator->promoteProtectedKvSet(owner.id(), owner.digest(), publications);
  const auto oldDigest = owner.digest(); owner.seed(2); owner.coordinator->restore();
  BOOST_REQUIRE(owner.coordinator->find(owner.id()));
  BOOST_CHECK(!owner.coordinator->find(owner.id())->protectedKvReferences);
  BOOST_CHECK_EQUAL(owner.coordinator->find(owner.id())->checkpoint.successorContextEpoch, 2U);
  BOOST_CHECK_THROW(owner.coordinator->promoteProtectedKvSet(owner.id(), oldDigest, publications), std::runtime_error);
  owner.time = owner.expiry; owner.coordinator->restore();
  BOOST_CHECK(!owner.coordinator->find(owner.id()));
  BOOST_CHECK(owner.config.journal->readConversations(owner.time).empty());
}

BOOST_AUTO_TEST_CASE(KvOwnerDescriptorValidationRejectsMalformedTypesNamesAndRoleBinding)
{
  OwnedCorePublicationFixture f; ndnsf::di::test::protectedKv::Fixture kv;
  f.authorize(kv.certificate.getIdentity().toUri()); OwnerKvConversationFixture owner(f, f.owner);
  const auto publications = owner.publish(kv);
  ndnsf::di::NativeJson references = ndnsf::di::NativeJson::object();
  for (const auto& [role, publication] : publications)
    references[role] = ndnsf::di::nativeProtectedKvPublicationReference(publication);
  const auto validate = [&](const auto& value) {
    ndnsf::di::nativeValidateProtectedKvReferences(value, owner.checkpoint, owner.placement);
  };
  BOOST_CHECK_NO_THROW(validate(references));
  auto missing = references; missing["/role/A"].erase("keyEnvelope");
  BOOST_CHECK_THROW(validate(missing), std::exception);
  auto wrongVersion = references; wrongVersion["/role/A"]["version"] = 1U;
  BOOST_CHECK_THROW(validate(wrongVersion), std::exception);
  wrongVersion = references; wrongVersion["/role/A"]["version"] = 3U;
  BOOST_CHECK_THROW(validate(wrongVersion), std::exception);
  auto malformedEnvelope = references; malformedEnvelope["/role/A"]["keyEnvelope"] = std::string(16385, 'x');
  BOOST_CHECK_THROW(validate(malformedEnvelope), std::exception);
  malformedEnvelope = references; malformedEnvelope["/role/A"]["keyEnvelope"] = "{}";
  BOOST_CHECK_THROW(validate(malformedEnvelope), std::exception);
  for (const auto& value : {ndnsf::di::NativeJson(false), ndnsf::di::NativeJson(-1), ndnsf::di::NativeJson(1.0)}) {
    auto invalid = references; invalid["/role/A"]["wireBytes"] = value;
    BOOST_CHECK_THROW(validate(invalid), std::exception);
    invalid = references; invalid["/role/A"]["version"] = value;
    BOOST_CHECK_THROW(validate(invalid), std::exception);
  }
  for (const auto& field : {"ownerCertificateName", "manifestName", "dataPrefix"}) {
    auto invalid = references; invalid["/role/A"][field] = "/8=unrelated/8=object";
    BOOST_CHECK_THROW(validate(invalid), std::exception);
  }
  auto invalid = references; invalid["/role/A"]["extra"] = "unrecognized";
  BOOST_CHECK_THROW(validate(invalid), std::exception);
  invalid = references; invalid["/role/A"]["keyReferenceVersion"] = std::string(4097, 'x');
  BOOST_CHECK_THROW(validate(invalid), std::exception);
  invalid = references; invalid["/role/A"] = references.at("/role/B");
  BOOST_CHECK_THROW(validate(invalid), std::exception);
  BOOST_CHECK(!owner.coordinator->find(owner.id())->protectedKvReferences);
}

BOOST_AUTO_TEST_CASE(KvPublicationPersistsOwnedOriginalWireAndReusesStableInput)
{
  OwnedCorePublicationFixture f;
  f.user->seedOpaqueOriginalAuxiliaries(f.service, f.keys);
  ndnsf::di::test::protectedKv::Fixture kv;
  kv.state.originIdentity.providerIdentity = f.actor.getIdentity().toUri();
  ndnsf::di::nativeExportPrivatePortableKvCheckpoint(kv.plain, kv.state, kv.portableLimits);
  const auto checkpoint = kv.protect(); f.authorize(kv.certificate.getIdentity().toUri(ndn::name::UriFormat::ALTERNATE));
  std::mutex challengeMutex;
  std::vector<std::string> grantEvents;
  std::vector<ndn_service_framework::DurableStorageGrantChallengeV1> challenges;
  bool repoEmptyAtChallenge = true;
  const auto resolveGrant = f.options.resolveDurableStorageGrant;
  auto owner = ndnsf::di::NativeDurableStorageOwner{f.owner,
    [&, resolveGrant](const ndn::Name& name, const std::string& writer,
                      const std::function<void()>& active) {
      {
        std::lock_guard<std::mutex> lock(challengeMutex);
        grantEvents.emplace_back("resolver");
      }
      return resolveGrant(name, writer, active);
    },
    [&](const auto& challenge) {
      const auto usage = f.journal->userStorageUsage(f.owner, f.repoName);
      const bool emptyBeforeGrant = f.node->list().empty() && usage.committedBytes == 0 &&
                                    usage.reservedBytes == 0;
      std::lock_guard<std::mutex> lock(challengeMutex);
      grantEvents.emplace_back("challenge"); challenges.push_back(challenge);
      repoEmptyAtChallenge = repoEmptyAtChallenge && emptyBeforeGrant;
    }};
  const auto publish = [&] {
    const auto request = f.user->prepareServiceRequest(f.service.toUri());
    auto& io = f.face.getIoContext(); io.restart(); auto guard = boost::asio::make_work_guard(io);
    std::thread thread([&] { io.run(); });
    struct Join { boost::asio::io_context& io; std::thread& thread;
      ~Join() { io.stop(); if (thread.joinable()) thread.join(); } } join{io, thread};
    return ndnsf::di::nativePublishProtectedPortableKvCheckpoint(f.user, request, kv.cipher, checkpoint,
      owner, kv.limits);
  };
  auto first = publish(); BOOST_CHECK_NO_THROW(first.requireCurrent());
  {
    std::lock_guard<std::mutex> lock(challengeMutex);
    BOOST_REQUIRE_EQUAL(grantEvents.size(), challenges.size() * 2U);
    for (std::size_t i = 0; i < grantEvents.size(); i += 2U) {
      BOOST_CHECK_EQUAL(grantEvents[i], "challenge");
      BOOST_CHECK_EQUAL(grantEvents[i + 1U], "resolver");
    }
    BOOST_REQUIRE(!challenges.empty());
    const auto& challenge = challenges.front();
    BOOST_CHECK_EQUAL(challenge.manifestName, first.transport.manifestName);
    BOOST_CHECK_EQUAL(challenge.writerIdentity,
      f.user->getName().toUri(ndn::name::UriFormat::ALTERNATE));
    BOOST_CHECK_EQUAL(challenge.publicationIdentity, first.transport.publicationIdentity);
    BOOST_CHECK(challenge.dataPrefix == first.transport.dataPrefix);
    BOOST_CHECK(challenge.packetWireBytes > checkpoint.wireBytes);
    BOOST_CHECK(challenge.auxiliaryPacketNames == first.transport.auxiliaryPacketNames);
    BOOST_CHECK(repoEmptyAtChallenge);
  }
  BOOST_REQUIRE(first.transport.auxiliaryPacketNames.size() >= 2U);
  BOOST_CHECK(first.transport.auxiliaryPacketNames == first.transport.durableStorageSource->auxiliaryDataPacketNames());
  BOOST_CHECK_EQUAL(first.inputPublicationIdentity().size(), 71U);
  BOOST_CHECK(first.transport.publicationIdentity != first.inputPublicationIdentity());
  auto alteredReceipt = first; alteredReceipt.checkpoint.checkpointIdentityDigest = keyReferenceDigest("other-checkpoint");
  BOOST_CHECK_THROW(alteredReceipt.requireCurrent(), std::runtime_error);
  alteredReceipt = first; alteredReceipt.transport.publicationIdentity = keyReferenceDigest("other-core-publication");
  BOOST_CHECK_THROW(alteredReceipt.requireCurrent(), std::runtime_error);
  alteredReceipt = first; alteredReceipt.transport.auxiliaryPacketNames.clear();
  BOOST_CHECK_THROW(alteredReceipt.requireCurrent(), std::runtime_error);
  auto otherSource = f.store->lookupDurable(first.transport.publicationIdentity); BOOST_REQUIRE(otherSource);
  BOOST_REQUIRE(otherSource->source != first.transport.durableStorageSource);
  alteredReceipt = first; alteredReceipt.transport.durableStorageSource = otherSource->source;
  BOOST_CHECK_THROW(alteredReceipt.requireCurrent(), std::runtime_error);
  BOOST_CHECK_THROW(ndnsf::di::NativeProtectedKvPublicationV1{}.requireCurrent(), std::exception);
  BOOST_CHECK(first.transport.manifestName != checkpoint.objectName());
  BOOST_CHECK(f.actor.getIdentity().isPrefixOf(first.transport.dataPrefix));
  const auto wire = first.transport.durableStorageSource->readOriginalDataPacket(first.transport.manifestName);
  const ndn::Data packet(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
  BOOST_CHECK(ndn::security::verifySignature(packet, f.actor));
  const auto usage = f.journal->userStorageUsage(f.owner, f.repoName);
  BOOST_CHECK(usage.committedBytes > checkpoint.wireBytes); BOOST_CHECK_EQUAL(usage.reservedBytes, 0U);
  const auto stored = f.node->core().lookupDataPacketObject(first.transport.manifestName.toUri());
  BOOST_REQUIRE(stored); const auto allWires = ndnsf_distributed_repo::RepoClient::getDataPackets(*f.node, *stored);
  BOOST_REQUIRE(allWires.size() > 1);
  for (const auto& bytes : allWires) {
    const ndn::Data original(ndn::Block(ndn::span<const uint8_t>(bytes.data(), bytes.size())));
    BOOST_CHECK(f.actor.getIdentity().isPrefixOf(original.getName()));
    BOOST_CHECK(ndn::security::verifySignature(original, f.actor));
  }
  auto repeated = publish(); BOOST_CHECK_NO_THROW(repeated.requireCurrent());
  BOOST_CHECK(first.transport.manifestName == repeated.transport.manifestName);
  BOOST_CHECK(repeated.transport.durableStorageSource->readOriginalDataPacket(repeated.transport.manifestName) == wire);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage(f.owner, f.repoName).committedBytes, usage.committedBytes);
  BOOST_CHECK_EQUAL(f.node->list().size(), 1U);
  const auto claims = f.journal->listUserStorage(f.owner, f.repoName, "", 10); BOOST_REQUIRE_EQUAL(claims.size(), 1U);
  f.journal->prepareUserStorageCleanup(f.owner, f.repoName, "kv-prepare-cleanup",
    claims.front().object.objectName, claims.front().ownerEpoch, f.now());
  BOOST_CHECK_THROW(first.requireCurrent(), std::runtime_error);
  BOOST_CHECK_THROW(repeated.requireCurrent(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(KvPublicationReopensExactPacketsAfterProducerAndReceiptsRelease)
{
  OwnedCorePublicationFixture f; ndnsf::di::test::protectedKv::Fixture kv;
  f.user->seedOpaqueOriginalAuxiliaries(f.service, f.keys);
  kv.state.originIdentity.providerIdentity = f.actor.getIdentity().toUri();
  ndnsf::di::nativeExportPrivatePortableKvCheckpoint(kv.plain, kv.state, kv.portableLimits);
  const auto checkpoint = kv.protect(); f.authorize(kv.certificate.getIdentity().toUri(ndn::name::UriFormat::ALTERNATE));
  ndnsf::di::NativeProtectedKvPublicationV1 receipt;
  {
    const auto request = f.user->prepareServiceRequest(f.service.toUri());
    auto& io = f.face.getIoContext(); io.restart(); auto guard = boost::asio::make_work_guard(io);
    std::thread thread([&] { io.run(); });
    struct Join { boost::asio::io_context& io; std::thread& thread;
      ~Join() { io.stop(); if (thread.joinable()) thread.join(); } } join{io, thread};
    receipt = ndnsf::di::nativePublishProtectedPortableKvCheckpoint(f.user, request, kv.cipher, checkpoint,
      ndnsf::di::NativeDurableStorageOwner{f.owner, f.options.resolveDurableStorageGrant}, kv.limits);
  }
  const auto claims = f.journal->listUserStorage(f.owner, f.repoName, "", 10);
  BOOST_REQUIRE_EQUAL(claims.size(), 1U);
  const auto manifestName = receipt.transport.manifestName;
  const auto physical = f.node->core().lookupDataPacketObject(manifestName.toUri()); BOOST_REQUIRE(physical);
  const auto original = ndnsf_distributed_repo::RepoClient::getDataPackets(*f.node, *physical);
  BOOST_CHECK_NO_THROW(receipt.requireCurrent());
  receipt = {}; f.options.resolveDurableStorageGrant = {}; f.user.reset(); f.store.reset(); f.node.reset(); f.journal.reset();
  ndnsf_distributed_repo::StorageCapability capability;
  capability.repoNode = f.repoName; capability.repoMode = "persistent"; capability.freeBytes = 64U << 20;
  auto reopened = std::make_shared<ndnsf_distributed_repo::RepoNode>(ndn::Name("/spec204/core-service"), capability,
    ndnsf_distributed_repo::makeFilesystemRepoStore((f.root / "objects").string(), 1U << 20, 1U << 20),
    f.repoName, f.actor.getIdentity().toUri(), (f.root / "metadata.db").string());
  const auto restored = reopened->core().lookupDataPacketObject(manifestName.toUri()); BOOST_REQUIRE(restored);
  BOOST_CHECK(ndnsf_distributed_repo::RepoClient::getDataPackets(*reopened, *restored) == original);
  BOOST_CHECK(reopened->getDataPacket(manifestName.toUri()) == original.front());
  BOOST_REQUIRE(!original.empty());
}

BOOST_AUTO_TEST_CASE(KvPublicationFailurePreflightAndOwnerQuotaRemainHonest)
{
  OwnedCorePublicationFixture f; ndnsf::di::test::protectedKv::Fixture kv;
  f.user->seedOpaqueOriginalAuxiliaries(f.service, f.keys);
  kv.state.originIdentity.providerIdentity = f.actor.getIdentity().toUri();
  ndnsf::di::nativeExportPrivatePortableKvCheckpoint(kv.plain, kv.state, kv.portableLimits);
  const auto checkpoint = kv.protect(); f.authorize(kv.certificate.getIdentity().toUri(ndn::name::UriFormat::ALTERNATE));
  const auto request = f.user->prepareServiceRequest(f.service.toUri());
  auto owner = ndnsf::di::NativeDurableStorageOwner{f.owner, f.options.resolveDurableStorageGrant};
  BOOST_CHECK(!f.actor.getIdentity().isPrefixOf(request.requestId));
  auto unconfigured = std::make_shared<LocalServiceUser>(f.face, ndn::Name("/spec204/core-group"),
    f.actor, f.authority, "examples/trust-any.conf");
  BOOST_CHECK_THROW(ndnsf::di::nativePublishProtectedPortableKvCheckpoint(unconfigured, request, kv.cipher,
    checkpoint, owner, kv.limits), std::runtime_error);
  const auto publish = [&](const auto& reference, const auto& storageOwner, const auto& limits) {
    return ndnsf::di::nativePublishProtectedPortableKvCheckpoint(f.user, request, kv.cipher,
      reference, storageOwner, limits);
  };
  auto wrong = checkpoint; wrong.producerPrefix.append("other");
  BOOST_CHECK_THROW(publish(wrong, owner, kv.limits), std::runtime_error);
  auto missing = owner; missing.resolveGrant = {};
  BOOST_CHECK_THROW(publish(checkpoint, missing, kv.limits), std::invalid_argument);
  auto other = owner; other.ownerIdentity = "/other/owner";
  BOOST_CHECK_THROW(publish(checkpoint, other, kv.limits), std::runtime_error);
  auto cancelled = kv.limits; cancelled.cancelled = [] { return true; };
  BOOST_CHECK_THROW(publish(checkpoint, owner, cancelled), std::runtime_error);
  auto changed = ndnsf::di::test::protectedKv::read(kv.cipher); changed.back() ^= 1;
  ndnsf::di::test::protectedKv::write(kv.cipher, changed);
  BOOST_CHECK_THROW(publish(checkpoint, owner, kv.limits), std::invalid_argument);
  BOOST_CHECK(f.node->list().empty()); BOOST_CHECK(f.resolved.empty());
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage(f.owner, f.repoName).committedBytes, 0U);
  f.face.getIoContext().restart(); bool faceRejected = false;
  f.user->postToIo([&] {
    try { (void)publish(checkpoint, owner, kv.limits); }
    catch (const std::runtime_error&) { faceRejected = true; }
  });
  f.face.processEvents(ndn::time::milliseconds(1)); BOOST_CHECK(faceRejected);
}

BOOST_AUTO_TEST_CASE(KvPublicationRejectsActualFullOwnerReservationWithoutRefund)
{
  OwnedCorePublicationFixture f; ndnsf::di::test::protectedKv::Fixture kv;
  f.user->seedOpaqueOriginalAuxiliaries(f.service, f.keys);
  kv.state.originIdentity.providerIdentity = f.actor.getIdentity().toUri();
  ndnsf::di::nativeExportPrivatePortableKvCheckpoint(kv.plain, kv.state, kv.portableLimits);
  const auto checkpoint = kv.protect(); f.authorize(kv.certificate.getIdentity().toUri(ndn::name::UriFormat::ALTERNATE));
  const auto writer = f.actor.getIdentity().toUri(ndn::name::UriFormat::ALTERNATE);
  BOOST_REQUIRE(f.node->core().list().empty());
  BOOST_REQUIRE(f.journal->reconcileUserStorageInventory(f.repoName, {}, f.now()).ready);
  ndnsf_distributed_repo::RepoUserStorageObject reserved{
    ndn::Name(f.actor.getIdentity()).append("capacity-reservation").toUri(ndn::name::UriFormat::ALTERNATE),
    std::string(64, 'a'), 1, 999999999, 1};
  const auto grant = f.journal->grantUserStorage(f.owner, f.repoName, "kv-full-owner", writer,
    reserved.objectName, f.grantExpiresAt, f.now());
  (void)f.journal->reserveUserStorage(writer, f.repoName, f.owner, grant.grantId, "kv-full-reservation", reserved, f.now());
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage(f.owner, f.repoName).reservedBytes, 1000000000U);
  const auto request = f.user->prepareServiceRequest(f.service.toUri());
  auto& io = f.face.getIoContext(); io.restart(); auto guard = boost::asio::make_work_guard(io);
  std::thread thread([&] { io.run(); });
  struct Join { boost::asio::io_context& io; std::thread& thread;
    ~Join() { io.stop(); if (thread.joinable()) thread.join(); } } join{io, thread};
  BOOST_CHECK_THROW(ndnsf::di::nativePublishProtectedPortableKvCheckpoint(f.user, request, kv.cipher, checkpoint,
    ndnsf::di::NativeDurableStorageOwner{f.owner, f.options.resolveDurableStorageGrant}, kv.limits), std::runtime_error);
  const auto usage = f.journal->userStorageUsage(f.owner, f.repoName);
  BOOST_CHECK_EQUAL(usage.limitBytes, 1000000000U); BOOST_CHECK_EQUAL(usage.reservedBytes, 1000000000U);
  BOOST_CHECK_EQUAL(usage.committedBytes, 0U); BOOST_CHECK(f.node->list().empty());
  BOOST_CHECK_EQUAL(ndnsf::di::test::protectedKv::read(kv.plain).substr(0, 8), "NDKV0001");
}

BOOST_AUTO_TEST_CASE(OwnedCoreRequiresWorkerAndResolverBeforeCrypto)
{
  OwnedCorePublicationFixture f;
  const auto missing = f.publish();
  BOOST_CHECK(!missing.success);
  BOOST_CHECK_EQUAL(missing.errorMessage, "DURABLE_STORAGE_GRANT_RESOLVER_REQUIRED");
  BOOST_CHECK(f.node->list().empty());
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage(f.owner, f.repoName).committedBytes, 0U);
  f.authorize(f.owner);
  const auto inlineResult = f.user->publishEncryptedLargeData(
    f.user->prepareServiceRequest(f.service.toUri()), f.input, "owned-core",
    ndn::time::milliseconds(100), true, f.options);
  BOOST_CHECK(!inlineResult.success);
  BOOST_CHECK_EQUAL(inlineResult.errorMessage, "OWNED_DURABLE_REQUIRES_WORKER");
  BOOST_CHECK(f.resolved.empty());
  BOOST_CHECK(f.node->list().empty());
  BOOST_CHECK_EQUAL(f.user->getLargeDataServingMetricsForTest().publicationCount, 0U);
}

BOOST_AUTO_TEST_CASE(OwnedCoreWorkerPreservesProducerWireAndExactCharge)
{
  OwnedCorePublicationFixture f; f.authorize(f.owner);
  const auto first = f.publish();
  BOOST_REQUIRE_MESSAGE(first.success, first.errorMessage);
  BOOST_REQUIRE(first.durableStorageSource);
  BOOST_CHECK_NO_THROW(first.durableStorageSource->requireCurrentDurableStorageOwner(first.manifestName, f.owner));
  BOOST_REQUIRE_EQUAL(f.resolved.size(), 1U);
  BOOST_CHECK(f.resolved.front() == first.manifestName);
  BOOST_CHECK(f.actor.getIdentity().isPrefixOf(first.manifestName));
  const auto physical = f.node->list();
  BOOST_REQUIRE_EQUAL(physical.size(), 1U);
  const auto usage = f.journal->userStorageUsage(f.owner, f.repoName);
  const auto stored = f.node->core().probeObjectMetadata(physical.front().objectName);
  BOOST_CHECK_EQUAL(usage.committedBytes, physical.front().size + stored.manifestWireBytes);
  BOOST_CHECK_EQUAL(usage.reservedBytes, 0U);
  f.face.getIoContext().restart();
  const auto before = f.face.sentData.size();
  f.face.receive(ndn::Interest(first.manifestName)); f.face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(f.face.sentData.size(), before + 1);
  const auto& packet = f.face.sentData.back();
  BOOST_CHECK(packet.getName() == first.manifestName);
  BOOST_CHECK(ndn::security::verifySignature(packet, f.actor));
  const auto originalWire = packet.wireEncode();
  const auto storedWire = first.durableStorageSource->readOriginalDataPacket(first.manifestName);
  BOOST_CHECK(storedWire == std::vector<uint8_t>(originalWire.begin(), originalWire.end()));
  const auto same = f.publish();
  BOOST_REQUIRE_MESSAGE(same.success, same.errorMessage);
  BOOST_REQUIRE(same.durableStorageSource);
  BOOST_CHECK_NO_THROW(same.durableStorageSource->requireCurrentDurableStorageOwner(same.manifestName, f.owner));
  BOOST_CHECK(same.durableStorageSource->readOriginalDataPacket(same.manifestName) == storedWire);
  BOOST_REQUIRE_EQUAL(f.resolved.size(), 2U);
  BOOST_CHECK(same.manifestName == first.manifestName);
  BOOST_CHECK(first.servingLease != same.servingLease);
  BOOST_CHECK_EQUAL(f.node->list().size(), 1U);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage(f.owner, f.repoName).committedBytes, usage.committedBytes);
  f.face.getIoContext().restart(); f.face.receive(ndn::Interest(same.manifestName));
  f.face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK(f.face.sentData.back().wireEncode() == originalWire);
}

BOOST_AUTO_TEST_CASE(OwnedCoreCachedHitInstallsNewOwnerSourceAndKeepsExpiryIndependent)
{
  OwnedCorePublicationFixture f; f.authorize(f.owner);
  auto first = f.publish(); BOOST_REQUIRE_MESSAGE(first.success, first.errorMessage);
  BOOST_REQUIRE(first.durableStorageSource);
  const auto firstOwner = f.owner;
  BOOST_CHECK_NO_THROW(first.durableStorageSource->requireCurrentDurableStorageOwner(first.manifestName, firstOwner));
  const auto hit = f.store->lookupDurable(first.publicationIdentity);
  BOOST_REQUIRE(hit);
  const auto retain = f.retainOptions(*hit);
  auto oldSource = f.store->retainDurableSource(*hit, retain);
  BOOST_REQUIRE(oldSource);
  const auto charge = f.journal->userStorageUsage(f.owner, f.repoName).committedBytes;
  (void)f.journal->prepareUserStorageCleanup(f.owner, f.repoName, "prepare-old-core-owner",
    first.manifestName.toUri(ndn::name::UriFormat::ALTERNATE),
    retain.storageActor->grant.ownerEpoch, f.now());
  BOOST_CHECK_THROW(oldSource->readOriginalDataPacket(first.manifestName), std::runtime_error);
  BOOST_CHECK_THROW(first.durableStorageSource->requireCurrentDurableStorageOwner(first.manifestName, firstOwner), std::runtime_error);
  f.authorize("/spec204/core-owner-v");
  auto next = f.publish(); BOOST_REQUIRE_MESSAGE(next.success, next.errorMessage);
  BOOST_REQUIRE(next.durableStorageSource);
  BOOST_CHECK_NO_THROW(next.durableStorageSource->requireCurrentDurableStorageOwner(next.manifestName, f.owner));
  BOOST_CHECK_THROW(first.durableStorageSource->requireCurrentDurableStorageOwner(first.manifestName, f.owner), std::runtime_error);
  BOOST_CHECK(next.manifestName == first.manifestName);
  BOOST_CHECK(first.servingLease != next.servingLease);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage(f.owner, f.repoName).committedBytes, charge);
  first.servingLease.reset();
  f.face.getIoContext().restart(); f.face.processEvents(ndn::time::milliseconds(150));
  BOOST_CHECK_EQUAL(f.user->getLargeDataServingMetricsForTest().publicationCount, 1U);
  const auto before = f.face.sentData.size();
  const auto segment = ndn::Name(next.dataPrefix).appendSegment(3);
  f.face.receive(ndn::Interest(segment)); f.face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(f.face.sentData.size(), before + 1);
  BOOST_CHECK(f.face.sentData.back().getName() == segment);
  BOOST_CHECK(ndn::security::verifySignature(f.face.sentData.back(), f.actor));
  BOOST_CHECK_THROW(oldSource->readOriginalDataPacket(first.manifestName), std::runtime_error);
  next.servingLease.reset(); f.face.processEvents(ndn::time::milliseconds(1200));
  BOOST_CHECK_EQUAL(f.user->getLargeDataServingMetricsForTest().publicationCount, 0U);
  BOOST_CHECK_EQUAL(f.node->list().size(), 1U);
}

BOOST_AUTO_TEST_CASE(OwnedCoreResolverFailureAndCancellationCannotCommit)
{
  OwnedCorePublicationFixture f;
  f.options.resolveDurableStorageGrant = [](const ndn::Name&, const std::string&,
    const std::function<void()>&) -> DurableStorageGrantSelector {
      throw std::runtime_error("OWNER_UNAVAILABLE");
    };
  const auto failed = f.publish();
  BOOST_CHECK(!failed.success); BOOST_CHECK_EQUAL(failed.errorMessage, "OWNER_UNAVAILABLE");
  BOOST_CHECK(!failed.durableStorageSource);
  BOOST_CHECK(f.node->list().empty());
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage(f.owner, f.repoName).reservedBytes, 0U);
  f.authorize(f.owner);
  const auto resolver = f.options.resolveDurableStorageGrant;
  bool resolved = false;
  f.options.resolveDurableStorageGrant = [resolver, &resolved](const ndn::Name& name,
    const std::string& writer, const std::function<void()>& current) {
      const auto grant = resolver(name, writer, current); resolved = true; return grant;
    };
  const auto cancelled = f.publish([&] { if (resolved) throw std::runtime_error("CANCELLED_AFTER_GRANT"); });
  BOOST_CHECK(!cancelled.success); BOOST_CHECK_EQUAL(cancelled.errorMessage, "CANCELLED_AFTER_GRANT");
  BOOST_CHECK(!cancelled.durableStorageSource);
  BOOST_CHECK(f.node->list().empty());
  const auto usage = f.journal->userStorageUsage(f.owner, f.repoName);
  BOOST_CHECK_EQUAL(usage.reservedBytes, 0U); BOOST_CHECK_EQUAL(usage.committedBytes, 0U);
  BOOST_CHECK_EQUAL(f.user->getLargeDataServingMetricsForTest().publicationCount, 0U);
}

BOOST_AUTO_TEST_CASE(OwnedSourceEnqueueSerializesActualNodeCleanup)
{
  OwnedCorePublicationFixture f; f.authorize(f.owner);
  const auto publication = f.publish(); BOOST_REQUIRE_MESSAGE(publication.success, publication.errorMessage);
  const auto source = publication.durableStorageSource; BOOST_REQUIRE(source);
  unsigned calls = 0;
  BOOST_CHECK_THROW(source->withCurrentDurableStorageOwner(publication.manifestName, f.owner, {}), std::invalid_argument);
  BOOST_CHECK_THROW(source->withCurrentDurableStorageOwner(publication.manifestName, "/wrong-owner", [&] { ++calls; }), std::runtime_error);
  BOOST_CHECK_THROW(source->withCurrentDurableStorageOwner(ndn::Name("/wrong-manifest"), f.owner, [&] { ++calls; }), std::runtime_error);
  BOOST_CHECK_EQUAL(calls, 0U);
  BOOST_CHECK_THROW(source->withCurrentDurableStorageOwner(publication.manifestName, f.owner,
    [] { throw std::runtime_error("enqueue failed"); }), std::runtime_error);
  BOOST_CHECK_NO_THROW(source->requireCurrentDurableStorageOwner(publication.manifestName, f.owner));
  const auto claims = f.journal->listUserStorage(f.owner, f.repoName, "", 10);
  BOOST_REQUIRE_EQUAL(claims.size(), 1U);
  ndnsf_distributed_repo::RepoUserStorageDeleteRequest request;
  request.cleanupId = "enqueue-cleanup"; request.objectName = claims.front().object.objectName;
  request.ownerEpoch = claims.front().ownerEpoch;
  const auto json = request.toJson(); const std::vector<uint8_t> bytes(json.begin(), json.end());
  std::promise<void> entered, release, cleanupStarted;
  auto enteredFuture = entered.get_future(); auto releaseFuture = release.get_future();
  auto cleanupStartedFuture = cleanupStarted.get_future();
  auto enqueue = std::async(std::launch::async, [&] {
    source->withCurrentDurableStorageOwner(publication.manifestName, f.owner, [&] {
      entered.set_value();
      if (releaseFuture.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
        throw std::runtime_error("fixture enqueue barrier timed out");
      ++calls;
    });
  });
  const auto enteredState = enteredFuture.wait_for(std::chrono::seconds(2));
  auto cleanup = std::async(std::launch::async, [&] {
    cleanupStarted.set_value(); return f.node->handleUserStorageDelete(bytes, f.owner);
  });
  const auto startedState = cleanupStartedFuture.wait_for(std::chrono::seconds(2));
  const auto blockedState = cleanup.wait_for(std::chrono::milliseconds(50));
  release.set_value(); enqueue.get(); const auto reply = cleanup.get();
  BOOST_CHECK(enteredState == std::future_status::ready);
  BOOST_CHECK(startedState == std::future_status::ready);
  BOOST_CHECK(blockedState == std::future_status::timeout);
  BOOST_CHECK_EQUAL(calls, 1U);
  BOOST_CHECK_EQUAL(ndnsf_distributed_repo::RepoUserStorageDeleteReply::fromJson(
    std::string(reply.begin(), reply.end())).state, "DELETED");
  BOOST_CHECK_THROW(source->withCurrentDurableStorageOwner(publication.manifestName, f.owner, [&] { ++calls; }), std::runtime_error);
  BOOST_CHECK_EQUAL(calls, 1U);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage(f.owner, f.repoName).committedBytes, 0U);
}

BOOST_AUTO_TEST_CASE(OwnedCoreRootWindowAndDiscoveryRejectCleanedOwner)
{
  OwnedCorePublicationFixture f; f.authorize(f.owner);
  auto first = f.publish(); BOOST_REQUIRE_MESSAGE(first.success, first.errorMessage);
  const auto serve = [&](const ndn::Name& name, bool prefix = false) {
    f.face.getIoContext().restart();
    const auto before = f.face.sentData.size();
    ndn::Interest interest(name); interest.setCanBePrefix(prefix);
    f.face.receive(interest); f.face.processEvents(ndn::time::milliseconds(1));
    return f.face.sentData.size() - before;
  };
  BOOST_REQUIRE_EQUAL(serve(first.manifestName), 1U);
  const auto root = f.face.sentData.back().wireEncode();
  const auto segment = ndn::Name(first.dataPrefix).appendSegment(0);
  BOOST_REQUIRE_EQUAL(serve(segment), 1U);
  const auto wire = f.face.sentData.back().wireEncode();
  BOOST_REQUIRE_EQUAL(serve(segment), 1U);
  BOOST_CHECK(f.face.sentData.back().wireEncode() == wire);
  const auto hits = f.user->getLargeDataServingMetricsForTest().retransmissionHitCount;
  BOOST_CHECK_GT(hits, 0U);
  const auto claims = f.journal->listUserStorage(f.owner, f.repoName, "", 10);
  BOOST_REQUIRE_EQUAL(claims.size(), 1U);
  // Sequential epoch fence retains bytes for an independently charged new
  // owner. The concurrent gate invariant uses actual Node cleanup above.
  f.journal->prepareUserStorageCleanup(f.owner, f.repoName, "cached-owner-fence",
    claims.front().object.objectName, claims.front().ownerEpoch, f.now());
  BOOST_CHECK_EQUAL(serve(first.manifestName), 0U);
  BOOST_CHECK_EQUAL(serve(segment), 0U);
  BOOST_CHECK_EQUAL(serve(first.dataPrefix, true), 0U);
  BOOST_CHECK_EQUAL(serve(ndn::Name(first.dataPrefix).appendSegment(3)), 0U);
  f.authorize("/spec204/current-cached-owner");
  auto next = f.publish(); BOOST_REQUIRE_MESSAGE(next.success, next.errorMessage);
  BOOST_CHECK(next.manifestName == first.manifestName);
  BOOST_REQUIRE_EQUAL(serve(next.manifestName), 1U);
  BOOST_CHECK(f.face.sentData.back().wireEncode() == root);
  BOOST_REQUIRE_EQUAL(serve(segment), 1U);
  BOOST_CHECK(f.face.sentData.back().wireEncode() == wire);
  BOOST_CHECK_THROW(first.durableStorageSource->withCurrentDurableStorageOwner(
    first.manifestName, claims.front().ownerIdentity, [] {}), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(OwnedCoreControllerRebindResolvesOnlyTheNewExactRoot)
{
  OwnedCorePublicationFixture f; f.authorize(f.owner);
  const auto first = f.publish(); BOOST_REQUIRE_MESSAGE(first.success, first.errorMessage);
  PolicyStatusData status; status.setServiceName(f.service);
  status.setControllerVersion(ControllerVersion{2040, 7});
  status.setValidity(f.now() - 1000, f.now() + 60000);
  status.setPolicyDigest("sha256:" + std::string(64, '0'));
  status.setControllerCertificate(ndn::Name("/controller/spec204"));
  BOOST_REQUIRE(f.user->installControllerStatus(status));
  f.user->prepareHybridSendKeyForTest(f.service, "REQUEST-LARGE");
  const auto rebound = f.publish(); BOOST_REQUIRE_MESSAGE(rebound.success, rebound.errorMessage);
  BOOST_REQUIRE(rebound.durableStorageSource);
  BOOST_CHECK_NO_THROW(rebound.durableStorageSource->requireCurrentDurableStorageOwner(rebound.manifestName, f.owner));
  BOOST_REQUIRE_EQUAL(f.resolved.size(), 2U);
  BOOST_CHECK(rebound.manifestName != first.manifestName);
  BOOST_CHECK(f.resolved.back() == rebound.manifestName);
  BOOST_CHECK(f.actor.getIdentity().isPrefixOf(rebound.manifestName));
  BOOST_CHECK_EQUAL(f.node->list().size(), 2U);
  const auto same = f.publish(); BOOST_REQUIRE_MESSAGE(same.success, same.errorMessage);
  BOOST_REQUIRE_EQUAL(f.resolved.size(), 3U);
  BOOST_CHECK(f.resolved.back() == rebound.manifestName);
  BOOST_CHECK_EQUAL(f.node->list().size(), 2U);
}

BOOST_AUTO_TEST_CASE(OwnedCoreReplacementPreservesActualWrappedControlCleanup)
{
  OwnedCorePublicationFixture f;
  BootstrapProfile profile; profile.userIdentity = ndn::Name("/spec204/control-producer");
  profile.serviceName = f.service;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  environment.enableProductionIngressForTest();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  BOOST_REQUIRE(environment.user().isNacProducerReadyForTest());
  environment.user().setEncryptedLargeDataRangeStore(f.store);
  auto options = f.options;
  options.resolveDurableStorageGrant = [heldJournal = f.journal, repo = f.repoName,
    writerIdentity = profile.userIdentity, grantExpiresAt = f.grantExpiresAt](const ndn::Name& name, const std::string& writer,
      const std::function<void()>& current) {
      if (writer != writerIdentity.toUri(ndn::name::UriFormat::ALTERNATE) ||
          !writerIdentity.isPrefixOf(name)) throw std::runtime_error("control producer mismatch");
      if (current) current();
      const auto grant = heldJournal->grantUserStorage(writer, repo,
        keyReferenceDigest(name.toUri(ndn::name::UriFormat::ALTERNATE)), writer,
        name.toUri(ndn::name::UriFormat::ALTERNATE), grantExpiresAt,
        OwnedCorePublicationFixture::now());
      return DurableStorageGrantSelector{grant.repoNode, grant.ownerIdentity, grant.grantId, grant.ownerEpoch};
    };
  auto& face = environment.userFace(); auto& io = face.getIoContext();
  const auto publish = [&] {
    io.restart(); auto guard = boost::asio::make_work_guard(io);
    std::thread thread([&io] { io.run(); });
    struct Join { boost::asio::io_context& io; std::thread& thread;
      ~Join() { io.stop(); if (thread.joinable()) thread.join(); } } join{io, thread};
    return environment.user().publishEncryptedLargeDataFromWorker(
      environment.user().prepareServiceRequest(f.service.toUri()), f.input, "owned-control",
      ndn::time::milliseconds(100), options);
  };
  auto first = publish(); BOOST_REQUIRE_MESSAGE(first.success, first.errorMessage);
  BOOST_REQUIRE(!first.rollbackDataNames.empty());
  auto next = publish(); BOOST_REQUIRE_MESSAGE(next.success, next.errorMessage);
  BOOST_CHECK(first.manifestName == next.manifestName);
  BOOST_CHECK(first.servingLease != next.servingLease);
  first.servingLease.reset(); io.restart(); face.processEvents(ndn::time::milliseconds(150));
  BOOST_CHECK_EQUAL(environment.user().getLargeDataServingMetricsForTest().publicationCount, 1U);
  const auto servedControl = [&](const std::string& fullName) {
    const auto before = face.sentData.size();
    face.receive(ndn::Interest(ndn::Name(fullName))); face.processEvents(ndn::time::milliseconds(1));
    return std::any_of(face.sentData.begin() + before, face.sentData.end(), [&](const ndn::Data& data) {
      return data.getFullName() == ndn::Name(fullName);
    });
  };
  for (const auto& name : first.rollbackDataNames) BOOST_CHECK(servedControl(name));
  next.servingLease.reset(); face.processEvents(ndn::time::milliseconds(1200));
  BOOST_CHECK_EQUAL(environment.user().getLargeDataServingMetricsForTest().publicationCount, 0U);
  for (const auto& name : first.rollbackDataNames) BOOST_CHECK(!servedControl(name));
  BOOST_CHECK_EQUAL(f.node->list().size(), 1U);
}

BOOST_AUTO_TEST_CASE(OwnedCoreSharedWrappedControlRequiresOneLiveOwner)
{
  OwnedCorePublicationFixture f;
  BootstrapProfile profile; profile.userIdentity = ndn::Name("/spec204/shared-control-producer");
  profile.serviceName = f.service;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap(); environment.enableProductionIngressForTest();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  BOOST_REQUIRE(environment.user().isNacProducerReadyForTest());
  environment.user().setEncryptedLargeDataRangeStore(f.store);
  auto options = f.options;
  options.resolveDurableStorageGrant = [heldJournal = f.journal, repo = f.repoName,
    writerIdentity = profile.userIdentity, grantExpiresAt = f.grantExpiresAt](const ndn::Name& name,
      const std::string& writer, const std::function<void()>& current) {
      if (writer != writerIdentity.toUri(ndn::name::UriFormat::ALTERNATE) ||
          !writerIdentity.isPrefixOf(name)) throw std::runtime_error("shared control producer mismatch");
      if (current) current();
      const auto grant = heldJournal->grantUserStorage(writer, repo,
        keyReferenceDigest(name.toUri(ndn::name::UriFormat::ALTERNATE)), writer,
        name.toUri(ndn::name::UriFormat::ALTERNATE), grantExpiresAt, OwnedCorePublicationFixture::now());
      return DurableStorageGrantSelector{grant.repoNode, grant.ownerIdentity, grant.grantId, grant.ownerEpoch};
    };
  auto& face = environment.userFace(); auto& io = face.getIoContext();
  const auto publish = [&] {
    io.restart(); auto guard = boost::asio::make_work_guard(io);
    std::thread thread([&io] { io.run(); });
    struct Join { boost::asio::io_context& io; std::thread& thread;
      ~Join() { io.stop(); if (thread.joinable()) thread.join(); } } join{io, thread};
    return environment.user().publishEncryptedLargeDataFromWorker(
      environment.user().prepareServiceRequest(f.service.toUri()), f.input, "shared-control",
      ndn::time::milliseconds(100), options);
  };
  auto first = publish(); BOOST_REQUIRE_MESSAGE(first.success, first.errorMessage);
  BOOST_REQUIRE(!first.rollbackDataNames.empty());
  options.publicationIdentity = keyReferenceDigest("spec204-second-shared-control");
  options.contentName = materialContent("second-control");
  options.manifestSuffix = materialManifestSuffix("second-control");
  f.input.front() ^= 1;
  auto second = publish(); BOOST_REQUIRE_MESSAGE(second.success, second.errorMessage);
  BOOST_REQUIRE(first.manifestName != second.manifestName);
  BOOST_REQUIRE_EQUAL(first.keyReferenceId, second.keyReferenceId);
  BOOST_REQUIRE_EQUAL(first.protectionEpoch, second.protectionEpoch);
  BOOST_REQUIRE(!second.rollbackDataNames.empty());
  std::vector<std::string> shared;
  for (const auto& name : first.rollbackDataNames)
    if (std::find(second.rollbackDataNames.begin(), second.rollbackDataNames.end(), name) !=
        second.rollbackDataNames.end()) shared.push_back(name);
  BOOST_REQUIRE(!shared.empty());
  const auto serves = [&](const std::string& fullName) {
    io.restart(); const auto before = face.sentData.size();
    face.receive(ndn::Interest(ndn::Name(fullName))); face.processEvents(ndn::time::milliseconds(1));
    return std::count_if(face.sentData.begin() + before, face.sentData.end(), [&](const ndn::Data& data) {
      return data.getFullName() == ndn::Name(fullName);
    });
  };
  for (const auto& name : shared) BOOST_CHECK_EQUAL(serves(name), 1);
  const auto owner = profile.userIdentity.toUri(ndn::name::UriFormat::ALTERNATE);
  const auto claims = f.journal->listUserStorage(owner, f.repoName, "", 10);
  BOOST_REQUIRE_EQUAL(claims.size(), 2U);
  const auto fence = [&](const ndn::Name& manifest, const std::string& cleanupId) {
    const auto claim = std::find_if(claims.begin(), claims.end(), [&](const auto& candidate) {
      return ndn::Name(candidate.object.objectName) == manifest;
    });
    BOOST_REQUIRE(claim != claims.end());
    f.journal->prepareUserStorageCleanup(owner, f.repoName, cleanupId,
      claim->object.objectName, claim->ownerEpoch, f.now());
  };
  fence(first.manifestName, "shared-first");
  for (const auto& name : shared) BOOST_CHECK_EQUAL(serves(name), 1);
  fence(second.manifestName, "shared-second");
  for (const auto& name : shared) BOOST_CHECK_EQUAL(serves(name), 0);
  BOOST_CHECK_EQUAL(environment.user().getLargeDataServingMetricsForTest().publicationCount, 2U);
  first.servingLease.reset(); second.servingLease.reset();
  io.restart(); face.processEvents(ndn::time::milliseconds(1200));
  BOOST_CHECK_EQUAL(environment.user().getLargeDataServingMetricsForTest().publicationCount, 0U);
}

BOOST_AUTO_TEST_CASE(OwnedCoreMixedControlWitnessesSurviveIndependentCleanup)
{
  for (const auto mode : {0, 1, 2}) {
    OwnedCorePublicationFixture f;
    BootstrapProfile profile; profile.userIdentity = ndn::Name("/spec204/mixed-control-producer");
    profile.serviceName = f.service;
    NdnsfIntegrationEnvironment environment(profile);
    environment.bootstrap(); environment.enableProductionIngressForTest();
    BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
    BOOST_REQUIRE(environment.user().isNacProducerReadyForTest());
    environment.user().setEncryptedLargeDataRangeStore(f.store);
    auto options = f.options;
    options.retention = EncryptedLargeDataRetention::Transient;
    auto& face = environment.userFace(); auto& io = face.getIoContext();
    const auto publish = [&](const std::function<void()>& current = {}) {
      io.restart(); auto guard = boost::asio::make_work_guard(io);
      std::thread thread([&io] { io.run(); });
      struct Join { boost::asio::io_context& io; std::thread& thread;
        ~Join() { io.stop(); if (thread.joinable()) thread.join(); } } join{io, thread};
      return environment.user().publishEncryptedLargeDataFromWorker(
        environment.user().prepareServiceRequest(f.service.toUri()), f.input, "mixed-control",
        ndn::time::milliseconds(100), options, current);
    };
    auto transient = publish(); BOOST_REQUIRE_MESSAGE(transient.success, transient.errorMessage);
    BOOST_REQUIRE(transient.fileBacked);
    BOOST_REQUIRE(!transient.rollbackDataNames.empty());
    const auto serves = [&](const std::string& fullName) {
      io.restart(); const auto before = face.sentData.size();
      face.receive(ndn::Interest(ndn::Name(fullName))); face.processEvents(ndn::time::milliseconds(1));
      return std::count_if(face.sentData.begin() + before, face.sentData.end(), [&](const ndn::Data& data) {
        return data.getFullName() == ndn::Name(fullName);
      });
    };
    for (const auto& name : transient.rollbackDataNames) BOOST_CHECK_EQUAL(serves(name), 1);
    options.retention = EncryptedLargeDataRetention::Durable;
    options.publicationIdentity = keyReferenceDigest("spec204-mixed-durable");
    options.contentName = materialContent("mixed-durable");
    options.manifestSuffix = materialManifestSuffix("mixed-durable");
    const auto owner = profile.userIdentity.toUri(ndn::name::UriFormat::ALTERNATE);
    options.resolveDurableStorageGrant = [heldJournal = f.journal, repo = f.repoName,
      producer = profile.userIdentity, grantExpiresAt = f.grantExpiresAt](const ndn::Name& name,
        const std::string& writer, const std::function<void()>& current) {
      if (writer != producer.toUri(ndn::name::UriFormat::ALTERNATE) || !producer.isPrefixOf(name))
        throw std::runtime_error("mixed control producer mismatch");
      if (current) current();
      const auto grant = heldJournal->grantUserStorage(writer, repo,
        keyReferenceDigest(name.toUri(ndn::name::UriFormat::ALTERNATE)), writer,
        name.toUri(ndn::name::UriFormat::ALTERNATE), grantExpiresAt, OwnedCorePublicationFixture::now());
      return DurableStorageGrantSelector{grant.repoNode, grant.ownerIdentity, grant.grantId, grant.ownerEpoch};
    };
    f.input.front() ^= 1;
    bool retiredPending = false;
    const auto current = [&] {
      // The Repo claim becomes committed before Core's final pending check.
      // This exercises real status retirement in that registration gap.
      if (mode != 2 || retiredPending || f.journal->listUserStorage(owner, f.repoName, "", 10).empty())
        return;
      PolicyStatusData status; status.setServiceName(f.service);
      status.setControllerVersion(ControllerVersion{2041, 8});
      status.setValidity(f.now() - 1000, f.now() + 60000);
      status.setPolicyDigest("sha256:" + std::string(64, '0'));
      status.setControllerCertificate(ndn::Name("/controller/spec204"));
      if (!environment.user().installControllerStatus(status))
        throw std::runtime_error("fixture controller status rejected");
      retiredPending = true;
    };
    auto durable = publish(current);
    if (mode == 2) {
      BOOST_REQUIRE(retiredPending);
      BOOST_CHECK(!durable.success);
      BOOST_CHECK_EQUAL(durable.errorMessage, "DURABLE_STALE_CONTROLLER_VERSION");
      BOOST_CHECK_EQUAL(environment.user().getLargeDataServingMetricsForTest().publicationCount, 1U);
    }
    else {
      BOOST_REQUIRE_MESSAGE(durable.success, durable.errorMessage);
      BOOST_REQUIRE_EQUAL(transient.rollbackKeyId, durable.rollbackKeyId);
      BOOST_REQUIRE_EQUAL(transient.protectionEpoch, durable.protectionEpoch);
      BOOST_REQUIRE_EQUAL(transient.rollbackDataNames.size(), durable.rollbackDataNames.size());
      for (const auto& name : transient.rollbackDataNames)
        BOOST_REQUIRE(std::find(durable.rollbackDataNames.begin(), durable.rollbackDataNames.end(), name) !=
                      durable.rollbackDataNames.end());
      if (mode == 1) {
        environment.user().abortLargeDataPublications({transient});
        BOOST_CHECK_EQUAL(environment.user().getLargeDataServingMetricsForTest().publicationCount, 1U);
        for (const auto& name : transient.rollbackDataNames) BOOST_CHECK_EQUAL(serves(name), 1);
      }
      const auto claims = f.journal->listUserStorage(owner, f.repoName, "", 10);
      BOOST_REQUIRE_EQUAL(claims.size(), 1U);
      f.journal->prepareUserStorageCleanup(owner, f.repoName, "mixed-cleanup",
        claims.front().object.objectName, claims.front().ownerEpoch, f.now());
    }
    for (const auto& name : transient.rollbackDataNames) BOOST_CHECK_EQUAL(serves(name), mode == 1 ? 0 : 1);
    transient.servingLease.reset(); durable.servingLease.reset();
    io.restart(); face.processEvents(ndn::time::milliseconds(1200));
    BOOST_CHECK_EQUAL(environment.user().getLargeDataServingMetricsForTest().publicationCount, 0U);
    for (const auto& name : transient.rollbackDataNames) BOOST_CHECK_EQUAL(serves(name), 0);
  }
}

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
  LocalServiceUser user(face, ndn::Name("/spec201/raw"), actor, authority,
                        "examples/trust-any.conf");
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
    capability.freeBytes = 128U << 20;
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
    for (unsigned i = 0; i < 16; ++i) {
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
      if (i == 7 || i == 15) measureLookup(i + 3);
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
  LocalServiceProvider provider(face, ndn::Name("/spec201/strict"), providerCertificate,
                                authority, "examples/trust-any.conf");
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
  LocalServiceProvider provider(face, ndn::Name("/spec201/legacy"), receiver, authority,
                                "examples/trust-any.conf");
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
    LocalServiceUser user(face, ndn::Name("/spec201/raw"), actor, authority, "examples/trust-any.conf");
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
  LocalServiceUser user(face, ndn::Name("/spec201/raw"), actor, authority, "examples/trust-any.conf");
  user.useSigningKeyChainForSigningOnlyForTest(keys);
  user.attachLocalMockPubSubForTest(pubsub(face, keys));
  user.init();
  face.processEvents(ndn::time::milliseconds(1));
  const auto sendKey = user.prepareHybridSendKeyForTest(service, "REQUEST-LARGE");
  const auto providerCertificate = makeRsaIdentity(keys, ndn::Name("/spec201/semantic-provider"));
  LocalServiceProvider provider(face, ndn::Name("/spec201/raw"), providerCertificate, authority,
                                "examples/trust-any.conf");
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
  LocalServiceUser user(publishingFace, ndn::Name("/spec201/delegated"), owner, authority,
                        "examples/trust-any.conf");
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
    LocalServiceUser user(face, ndn::Name("/spec201/raw"), actor, authority, "examples/trust-any.conf");
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
    LocalServiceUser restored(face, ndn::Name("/spec201/raw"), actor, authority, "examples/trust-any.conf");
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
        LocalServiceUser user(face, ndn::Name("/spec201/records"), actor, authority, "examples/trust-any.conf");
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
        LocalServiceUser warm(face, ndn::Name("/spec201/records"), actor, authority,
                              "examples/trust-any.conf");
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
        LocalServiceUser restored(face, ndn::Name("/spec201/records"), actor, authority, "examples/trust-any.conf");
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
  LocalServiceUser user(face, ndn::Name("/spec201/raw"), actor, authority, "examples/trust-any.conf");
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

BOOST_AUTO_TEST_CASE(OwnerBoundDurableOperationsRequireRepoJournal)
{
  using namespace ndnsf_distributed_repo;
  OwnedCorePublicationFixture fixture;
  fixture.authorize(fixture.owner);
  const auto publication = fixture.publish();
  BOOST_REQUIRE_MESSAGE(publication.success, publication.errorMessage);
  const auto candidate = fixture.store->lookupDurable(publication.publicationIdentity);
  BOOST_REQUIRE(candidate);
  const auto ownerOptions = fixture.retainOptions(*candidate);

  StorageCapability capability;
  capability.repoNode = "/spec204/unowned-repo";
  capability.freeBytes = 1U << 20;
  auto repo = std::make_shared<RepoCore>(capability,
    makeFilesystemRepoStore((fixture.root / "unowned-repo").string(), 1U << 20, 1U << 20));
  RepoEncryptedLargeDataStore unowned(repo);
  const auto missingJournal = [](const std::runtime_error& error) {
    return std::string(error.what()) == "DURABLE_OWNER_JOURNAL_REQUIRED";
  };

  auto packetOptions = ownerOptions;
  packetOptions.packetStorageFormat = "original-packet-set";
  std::size_t packetReads = 0;
  BOOST_CHECK_EXCEPTION(unowned.commitOriginalDataPackets(ndn::Name(candidate->manifestName), packetOptions,
    [&]() -> std::optional<std::pair<ndn::Name, std::vector<uint8_t>>> {
      ++packetReads;
      return std::nullopt;
    }), std::runtime_error, missingJournal);
  BOOST_CHECK_EQUAL(packetReads, 0U);
  BOOST_CHECK(repo->list().empty());

  auto envelopeOptions = ownerOptions;
  envelopeOptions.packetStorageFormat = "encrypted-envelope-bytes";
  const auto envelope = fixture.root / "owner-bound-envelope.bin";
  std::ofstream output(envelope, std::ios::binary);
  BOOST_REQUIRE(output.good());
  output.put('x');
  output.close();
  BOOST_REQUIRE(output.good());
  BOOST_CHECK_EXCEPTION(unowned.commitFile(candidate->manifestName, envelope, 1, envelopeOptions),
    std::runtime_error, missingJournal);
  BOOST_CHECK(repo->list().empty());

  BOOST_CHECK_EXCEPTION(unowned.retainDurableSource(*candidate, ownerOptions),
    std::runtime_error, missingJournal);
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
  LocalServiceUser user(face, ndn::Name("/spec201/raw"), actor, authority, "examples/trust-any.conf");
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
    LocalServiceUser user(face, ndn::Name("/spec201/raw"), actor, authority, "examples/trust-any.conf");
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
  BOOST_REQUIRE(first.durableStorageSource);
  BOOST_CHECK_THROW(openRepo(), std::runtime_error);
  // A process-local storage proof owns its backend until the process releases it.
  first.durableStorageSource.reset();
  {
    auto repo = openRepo();
    LocalServiceUser user(face, ndn::Name("/spec201/raw"), actor, authority, "examples/trust-any.conf");
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

namespace {
struct RecipientConsumptionFixture
{
  NdnsfIntegrationEnvironment environment;
  ndn::security::Certificate certificate;
  ndn::Buffer plaintext = ndn::Buffer(32, 0x5a);
  ndn::Buffer aad{0x31, 0x32, 0x33};
  RecipientEncryptedAssignment capsule;
  std::string publicDigest;

  explicit RecipientConsumptionFixture(BootstrapProfile profile = {})
    : environment(std::move(profile)) {
    environment.bootstrap();
    BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
    certificate = environment.keyChain().getPib()
      .getIdentity(environment.profile().providerIdentity).getDefaultKey().getDefaultCertificate();
    const auto publicKey = certificate.getPublicKey();
    publicDigest = ndnsf::di::nativePlanningDigest(publicKey.data(), publicKey.size());
    capsule = encryptRecipientAssignment(plaintext, publicKey, certificate.getIdentity(),
      certificate.getName(), aad);
  }

  struct Outcome {
    bool accepted = false, threw = false, onWorker = false;
    std::size_t calls = 0;
    ndn::Buffer bytes;
  };

  Outcome run(const RecipientEncryptedAssignment& input, const ndn::Name& cert,
              const std::string& digest, const ndn::Buffer& ad,
              bool authority = true, bool expired = false, bool throwConsumer = false,
              bool expiredCaller = false) {
    ServiceProvider::CollaborationAssignment assignment;
    assignment.role = "recipient-probe"; assignment.service = ndn::Name("/spec204/model");
    assignment.workDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    if (authority) assignment.workCurrent = [] { return true; };
    if (expired) assignment.workDeadline = std::chrono::steady_clock::now();
    ServiceProvider::CollaborationContext context(environment.provider(),
      environment.profile().userIdentity, ndn::Name("/request/recipient-probe"), {}, assignment);
    auto& io = environment.providerFace().getIoContext(); io.restart();
    auto guard = boost::asio::make_work_guard(io);
    auto work = std::async(std::launch::async, [&] {
      Outcome out;
      const auto worker = std::this_thread::get_id();
      try {
        const auto deadline = std::chrono::steady_clock::now() +
          (expiredCaller ? std::chrono::seconds(0) : std::chrono::seconds(2));
        out.accepted = context.consumeRecipientAssignmentCurrent(input, cert, digest, ad,
          deadline, [&](const ndn::Buffer& bytes) {
            ++out.calls; out.bytes = bytes; out.onWorker = std::this_thread::get_id() == worker;
            if (throwConsumer) throw std::runtime_error("consumer sentinel");
          });
      }
      catch (const std::runtime_error& error) {
        if (std::string(error.what()) != "consumer sentinel") throw;
        out.threw = true;
      }
      return out;
    });
    environment.pumpUntil([&] { return work.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; });
    return work.get();
  }
};
}

BOOST_AUTO_TEST_CASE(RecipientCurrentConsumesOnWorkerAndPreservesThrowingCleanup)
{
  RecipientConsumptionFixture f;
  const auto success = f.run(f.capsule, f.certificate.getName(), f.publicDigest, f.aad);
  BOOST_REQUIRE(success.accepted); BOOST_CHECK_EQUAL(success.calls, 1U);
  BOOST_CHECK(success.onWorker); BOOST_CHECK(success.bytes == f.plaintext);
  const auto throwing = f.run(f.capsule, f.certificate.getName(), f.publicDigest, f.aad, true, false, true);
  BOOST_CHECK(throwing.threw); BOOST_CHECK(!throwing.accepted);
  BOOST_CHECK_EQUAL(throwing.calls, 1U); BOOST_CHECK(throwing.onWorker);
  const ndn::Buffer maximum(2048, 0x6b);
  const ndn::Buffer maximumAad(65536, 0x31);
  const auto maximumCapsule = encryptRecipientAssignment(maximum, f.certificate.getPublicKey(),
    f.certificate.getIdentity(), f.certificate.getName(), maximumAad);
  const auto bounded = f.run(maximumCapsule, f.certificate.getName(), f.publicDigest, maximumAad);
  BOOST_CHECK(bounded.accepted); BOOST_CHECK_EQUAL(bounded.calls, 1U);
  BOOST_CHECK(bounded.bytes == maximum);
}

BOOST_AUTO_TEST_CASE(RecipientCurrentRejectsMalformedOrReboundCapsulesBeforeConsumption)
{
  RecipientConsumptionFixture f;
  auto changedAad = f.aad; changedAad.push_back(0x34);
  auto wrongCert = f.certificate.getName(); wrongCert.appendVersion(9);
  auto wrongCertCapsule = f.capsule;
  wrongCertCapsule.setField("recipientCertName", wrongCert.toUri(ndn::name::UriFormat::CANONICAL));
  const auto rejected = [&](const auto& capsule, const auto& cert, const auto& digest, const auto& aad) {
    const auto result = f.run(capsule, cert, digest, aad);
    BOOST_CHECK(!result.accepted); BOOST_CHECK_EQUAL(result.calls, 0U);
  };
  rejected(wrongCertCapsule, wrongCert, f.publicDigest, f.aad);
  rejected(f.capsule, f.certificate.getName(), "sha256:" + std::string(64, '0'), f.aad);
  rejected(f.capsule, f.certificate.getName(), f.publicDigest, changedAad);
  rejected(f.capsule, f.certificate.getName(), f.publicDigest, ndn::Buffer(65537, 0x31));
  for (const auto& edit : std::vector<std::function<void(RecipientEncryptedAssignment&)>>{
    [](auto& value) { value.setVersion(2); },
    [](auto& value) { value.setField("schemaVersion", "2"); },
    [](auto& value) { value.setField("unknown", "x"); },
    [](auto& value) { value.setField("nonce", "0"); },
    [](auto& value) { auto bytes = value.getField("ciphertext"); bytes[0] = bytes[0] == '0' ? '1' : '0'; value.setField("ciphertext", bytes); },
    [](auto& value) { value.setField("assignmentDigest", std::string(64, '0')); }
  }) {
    auto capsule = f.capsule; edit(capsule);
    rejected(capsule, f.certificate.getName(), f.publicDigest, f.aad);
  }
  const auto missing = f.run(f.capsule, f.certificate.getName(), f.publicDigest, f.aad, false);
  BOOST_CHECK(!missing.accepted); BOOST_CHECK_EQUAL(missing.calls, 0U);
  const auto expired = f.run(f.capsule, f.certificate.getName(), f.publicDigest, f.aad, true, true);
  BOOST_CHECK(!expired.accepted); BOOST_CHECK_EQUAL(expired.calls, 0U);
  const auto callerExpired = f.run(f.capsule, f.certificate.getName(), f.publicDigest,
    f.aad, true, false, false, true);
  BOOST_CHECK(!callerExpired.accepted); BOOST_CHECK_EQUAL(callerExpired.calls, 0U);
}

BOOST_AUTO_TEST_CASE(RecipientCurrentCancelsQueuedTpmBeforeIoDrain)
{
  RecipientConsumptionFixture f;
  auto current = std::make_shared<std::atomic<bool>>(true);
  auto probes = std::make_shared<std::atomic<std::size_t>>(0);
  std::atomic<std::size_t> calls{0};
  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = "recipient-cancel"; assignment.service = ndn::Name("/spec204/model");
  assignment.workDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  assignment.workCurrent = [current, probes] { ++*probes; return current->load(); };
  ServiceProvider::CollaborationContext context(f.environment.provider(),
    f.environment.profile().userIdentity, ndn::Name("/request/recipient-cancel"), {}, assignment);
  auto& io = f.environment.providerFace().getIoContext(); io.restart();
  auto guard = boost::asio::make_work_guard(io);
  auto work = std::async(std::launch::async, [&] {
    return context.consumeRecipientAssignmentCurrent(f.capsule, f.certificate.getName(),
      f.publicDigest, f.aad, assignment.workDeadline, [&](const auto&) { ++calls; });
  });
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (probes->load() < 2 && std::chrono::steady_clock::now() < until)
    std::this_thread::yield();
  const bool queued = probes->load() >= 2;
  current->store(false);
  const bool cancelled = work.get(); // join before draining the queued callback
  BOOST_CHECK(queued); BOOST_CHECK(!cancelled); BOOST_CHECK_EQUAL(calls.load(), 0U);
  bool drained = false; boost::asio::post(io, [&] { drained = true; });
  f.environment.pumpUntil([&] { return drained; });
  BOOST_CHECK_EQUAL(calls.load(), 0U);
}

BOOST_AUTO_TEST_CASE(RecipientCurrentRecoversCompletePrivatePortableCheckpoint)
{
  namespace kv = ndnsf::di::test::protectedKv;
  kv::Fixture source;
  const auto reference = source.protect();
  BootstrapProfile profile; profile.userIdentity = source.certificate.getIdentity();
  RecipientConsumptionFixture f(profile);
  ndnsf::di::NativeProtectedKvRecipientBindingV1 binding;
  binding.requesterIdentity = source.certificate.getIdentity();
  binding.recipientIdentity = f.certificate.getIdentity();
  binding.recipientCertificateName = f.certificate.getName();
  binding.recipientPublicKeyDigest = f.publicDigest;
  binding.serviceName = ndn::Name("/spec204/model");
  binding.requestId = ndn::Name("/request/recipient-file");
  binding.authorizationBinding = "current-native-grant-commitment";
  binding.planDigest = kv::hash("recipient-file-plan");
  // Exercise the crypto binding's legal limit while retaining a valid portable
  // origin. This is an AAD/file oracle, not Handler role admission or import.
  binding.roleName.assign(4096, '\x01');
  binding.attemptEpoch = 1; binding.expiresAtMs = std::numeric_limits<std::uint64_t>::max();
  const auto capsule = ndnsf::di::nativeWrapProtectedKvKeyForRecipient(reference,
    source.certificate, source.ownerKeys, f.certificate.getPublicKey(), binding, [] {});
  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = binding.roleName; assignment.service = binding.serviceName;
  assignment.workDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  auto active = std::make_shared<std::atomic<bool>>(true);
  assignment.workCurrent = [active] { return active->load(); };
  ServiceProvider::CollaborationContext context(f.environment.provider(), binding.requesterIdentity,
    binding.requestId, {}, assignment);
  auto& io = f.environment.providerFace().getIoContext(); io.restart();
  auto guard = boost::asio::make_work_guard(io);
  std::size_t observedAadBytes = 0;
  auto work = std::async(std::launch::async, [&] {
    ndnsf::di::NativeProtectedKvRecipientConsumerV1 consume = [&](const auto& encrypted,
      const auto& certificateName, const auto& publicDigest, const auto& associatedData, const auto& deliver) {
      observedAadBytes = associatedData.size();
      return context.consumeRecipientAssignmentCurrent(encrypted, certificateName, publicDigest,
        associatedData, assignment.workDeadline, deliver);
    };
    ndnsf::di::nativeRecoverProtectedPortableKvCheckpointForRecipient(source.cipher,
      source.restored, reference, capsule, binding, source.limits, consume, [&] {
        if (!active->load() || std::chrono::steady_clock::now() >= assignment.workDeadline)
          throw std::runtime_error("recipient file authority expired");
      });
  });
  f.environment.pumpUntil([&] { return work.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; });
  work.get(); // all borrowed Context and file owners remain live through recovery
  BOOST_CHECK_GT(observedAadBytes, 16384U);
  BOOST_CHECK_LE(observedAadBytes, 65536U);
  BOOST_CHECK_EQUAL(kv::read(source.restored), kv::read(source.plain));
  const auto recovered = ndnsf::di::nativeReadPrivatePortableKvCheckpoint(source.restored, source.portableLimits);
  BOOST_CHECK(recovered.originIdentity == source.state.originIdentity);
  BOOST_CHECK_EQUAL(recovered.stateIoDigest, source.state.stateIoDigest);
  BOOST_REQUIRE_EQUAL(recovered.components.size(), source.state.components.size());
  for (std::size_t i = 0; i < recovered.components.size(); ++i)
    BOOST_CHECK(recovered.components[i].tensorWire == source.state.components[i].tensorWire);
}

BOOST_AUTO_TEST_SUITE_END()
