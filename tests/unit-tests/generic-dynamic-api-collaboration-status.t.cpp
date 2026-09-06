#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

#include <set>
#include <future>
#include <thread>

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(GenericDynamicApi)
BOOST_AUTO_TEST_SUITE(CollaborationStatus)

namespace {

class FencedCollaborationProvider : public LocalServiceProvider
{
public:
  using LocalServiceProvider::LocalServiceProvider;

  std::shared_ptr<std::promise<void>> holdFetchWorkers()
  {
    auto release = std::make_shared<std::promise<void>>();
    const auto released = release->get_future().share();
    for (int i = 0; i < 2; ++i) {
      auto entered = std::make_shared<std::promise<void>>();
      auto ready = entered->get_future();
      BOOST_REQUIRE(m_fetchPool.post([entered, released] {
        entered->set_value();
        released.wait();
      }));
      BOOST_REQUIRE(ready.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    }
    return release;
  }

  bool hasScopeKeys(const ndn::Name& requestId)
  {
    std::lock_guard<std::mutex> lock(m_collaborationMutex);
    return m_collaborationScopeKeysByRequest.count(requestId) != 0;
  }

  std::shared_ptr<std::promise<void>> holdHandlerWorker()
  {
    setHandlerThreads(1);
    auto release = std::make_shared<std::promise<void>>();
    const auto released = release->get_future().share();
    auto entered = std::make_shared<std::promise<void>>();
    auto ready = entered->get_future();
    BOOST_REQUIRE(m_handlerPool.post([entered, released] {
      entered->set_value();
      released.wait();
    }));
    BOOST_REQUIRE(ready.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    return release;
  }

  void dispatchForTest(const RequestMessage& request, CollaborationAssignment assignment)
  {
    const auto service = assignment.service;
    BOOST_REQUIRE(dispatchCollaborationExecutionAsync(
        ndn::Name("/user/fenced"), identity, service, ndn::Name("/request/fenced"),
        request, std::move(assignment), "selection-fenced"));
  }

  void drainHandlerWorker()
  {
    auto drained = std::make_shared<std::promise<void>>();
    auto ready = drained->get_future();
    BOOST_REQUIRE(m_handlerPool.post([drained] { drained->set_value(); }));
    BOOST_REQUIRE(ready.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  }

  std::shared_ptr<std::atomic<bool>> stoppingForTest() const
  {
    return m_fetchStopping;
  }
};

class FixedDeferredSelection final : public ParticipantSelectionPolicy
{
public:
  std::vector<SelectedParticipant>
  select(const std::vector<AckCandidate>& candidates,
         const std::vector<CollaborationRoleSpec>& roles) const override
  {
    if (candidates.empty() || roles.empty() || !candidates.front().ack.getStatus()) {
      return {};
    }
    const auto& candidate = candidates.front();
    const auto& role = roles.front();
    std::string assignment = "opaque=" + role.requiredArtifact.toUri();
    ndn::Buffer payload(
      reinterpret_cast<const uint8_t*>(assignment.data()), assignment.size());
    return {{
      role.role,
      candidate.serviceName,
      candidate.providerName,
      role.requiredArtifact,
      false,
      0,
      std::move(payload),
      candidate,
      {},
    }};
  }
};

class ThreeRoleLargeDeferredSelection final : public ParticipantSelectionPolicy
{
public:
  std::vector<SelectedParticipant>
  select(const std::vector<AckCandidate>& candidates,
         const std::vector<CollaborationRoleSpec>& roles) const override
  {
    if (candidates.size() != 3 || roles.size() != 3) {
      return {};
    }
    std::vector<SelectedParticipant> selected;
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (!candidates[i].ack.getStatus()) {
        return {};
      }
      // Keep this above the single-Data safety bound exercised by the real
      // Spec 175 M01 placement projections. The Selection must carry only a
      // bounded reference, never these bytes inline.
      std::string opaque(24 * 1024, static_cast<char>('A' + i));
      opaque.replace(0, roles[i].role.size(), roles[i].role);
      ndn::Buffer payload(
        reinterpret_cast<const uint8_t*>(opaque.data()), opaque.size());
      selected.push_back({
        roles[i].role,
        candidates[i].serviceName,
        candidates[i].providerName,
        roles[i].requiredArtifact,
        false,
        0,
        std::move(payload),
        candidates[i],
        {},
      });
    }
    return selected;
  }
};

CollaborationPlan
makeDeferredPlan(const ndn::Name& artifact = ndn::Name("/artifact/a"))
{
  CollaborationPlan plan;
  plan.ackCollectionTimeMs = 100;
  plan.timeoutMs = 1000;
  CollaborationRoleSpec role;
  role.role = "worker";
  role.service = ndn::Name("/generic/work");
  role.requiredArtifact = artifact;
  plan.roles.push_back(std::move(role));
  plan.participantSelector = std::make_shared<FixedDeferredSelection>();
  return plan;
}

CollaborationPlan
makeThreeRoleLargeDeferredPlan()
{
  CollaborationPlan plan;
  plan.ackCollectionTimeMs = 100;
  plan.timeoutMs = 1000;
  for (size_t i = 0; i < 3; ++i) {
    CollaborationRoleSpec role;
    role.role = "stage-" + std::to_string(i);
    role.service = ndn::Name("/generic/work");
    role.requiredArtifact = ndn::Name("/artifact/stage").appendNumber(i);
    role.minProviders = 1;
    role.maxProviders = 1;
    plan.roles.push_back(std::move(role));
  }
  plan.keyScopes = {
    {"stage-0-to-1", {"stage-0", "stage-1"}},
    {"stage-1-to-2", {"stage-1", "stage-2"}},
  };
  plan.dependencies = {
    {{"stage-0"}, {"stage-1"}, "stage-0-to-1", ndn::Name("/activation"), true},
    {{"stage-1"}, {"stage-2"}, "stage-1-to-2", ndn::Name("/activation"), true},
  };
  const std::string scopeKeyMetadata =
    "scopeKeyData.stage-0-to-1=/key/stage-0-to-1;"
    "scopeKeyData.stage-1-to-2=/key/stage-1-to-2;";
  plan.sharedAssignmentMetadata = ndn::Buffer(
    reinterpret_cast<const uint8_t*>(scopeKeyMetadata.data()),
    scopeKeyMetadata.size());
  plan.participantSelector =
    std::make_shared<ThreeRoleLargeDeferredSelection>();
  return plan;
}

} // namespace

BOOST_AUTO_TEST_CASE(DeferredAckClosureAndPlanCommitAreOneShotAndIdempotent)
{
  ndn::security::KeyChain keyChain(
    "pib-memory:deferred-collab", "tpm-memory:deferred-collab");
  ndn::DummyClientFace face(keyChain);
  auto userCert = makeRsaIdentity(keyChain, ndn::Name("/user/a"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  LocalServiceUser user(
    face, ndn::Name("/test/group"), userCert, aaCert,
    "examples/trust-any.conf");
  const ndn::Name requestId("/request/deferred-1");
  size_t closureCount = 0;
  CollaborationAckClosure closed;
  user.prepareDeferredCollaborationForTest(
    requestId,
    [&](const CollaborationAckClosure& value) {
      ++closureCount;
      closed = value;
    });
  user.addDeferredAckForTest(requestId, ndn::Name("/provider/a"), "worker");

  BOOST_CHECK_THROW(
    user.CommitCollaborationPlan(
      requestId, "sha256:" + std::string(64, '0'), makeDeferredPlan()),
    std::logic_error);
  BOOST_CHECK(user.closeDeferredAcksForTest(requestId));
  BOOST_CHECK(user.closeDeferredAcksForTest(requestId));
  BOOST_CHECK_EQUAL(closureCount, 1);
  BOOST_CHECK_EQUAL(closed.requestId, requestId);
  BOOST_REQUIRE_EQUAL(closed.candidates.size(), 1);
  BOOST_CHECK_EQUAL(closed.candidates.front().providerName, "/provider/a");
  BOOST_CHECK_EQUAL(closed.digest.size(), 71);

  BOOST_CHECK_THROW(
    user.CommitCollaborationPlan(
      requestId, "sha256:" + std::string(64, '1'), makeDeferredPlan()),
    std::invalid_argument);
  BOOST_CHECK(user.CommitCollaborationPlan(
    requestId, closed.digest, makeDeferredPlan()));
  BOOST_CHECK(user.CommitCollaborationPlan(
    requestId, closed.digest, makeDeferredPlan()));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), "/provider/a");
  BOOST_CHECK_THROW(
    user.CommitCollaborationPlan(
      requestId, closed.digest, makeDeferredPlan(ndn::Name("/artifact/b"))),
    std::logic_error);
}

BOOST_AUTO_TEST_CASE(LargeThreeRoleCollaborationPublishesBoundedProviderProjections)
{
  ndn::security::KeyChain keyChain(
    "pib-memory:deferred-large-collab", "tpm-memory:deferred-large-collab");
  ndn::DummyClientFace face(keyChain);
  auto userCert = makeRsaIdentity(keyChain, ndn::Name("/user/large-collab"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  LocalServiceUser user(
    face, ndn::Name("/test/group"), userCert, aaCert,
    "examples/trust-any.conf");
  user.useSigningKeyChainForTest(keyChain);
  user.prepareHybridSendKeyForTest(
    ndn::Name("/generic/work"), "REQUEST-LARGE");
  const ndn::Name requestId("/request/large-collab-1");
  size_t closureCount = 0;
  CollaborationAckClosure closed;
  user.prepareDeferredCollaborationForTest(
    requestId,
    [&](const CollaborationAckClosure& value) {
      ++closureCount;
      closed = value;
    });

  const std::vector<ndn::Name> providers = {
    ndn::Name("/provider/stage-0"),
    ndn::Name("/provider/stage-1"),
    ndn::Name("/provider/stage-2"),
  };
  for (size_t i = 0; i < providers.size(); ++i) {
    user.addDeferredAckForTest(
      requestId, providers[i], "stage-" + std::to_string(i));
  }
  BOOST_CHECK(user.closeDeferredAcksForTest(requestId));
  BOOST_REQUIRE_EQUAL(closed.candidates.size(), 3);
  BOOST_REQUIRE_EQUAL(closureCount, 1);

  const auto plan = makeThreeRoleLargeDeferredPlan();
  BOOST_CHECK(user.CommitCollaborationPlan(requestId, closed.digest, plan));

  const auto published = user.getSelectionPublishedProviders(requestId);
  BOOST_REQUIRE_EQUAL(published.size(), 3);
  const auto digests = user.getSelectionDigestsByProvider(requestId);
  BOOST_REQUIRE_EQUAL(digests.size(), 3);
  std::set<std::string> distinctDigests;
  size_t combinedAssignmentBytes = 0;
  for (const auto& provider : providers) {
    BOOST_CHECK(std::find(published.begin(), published.end(), provider) !=
                published.end());
    const auto found = digests.find(provider.toUri());
    BOOST_REQUIRE(found != digests.end());
    distinctDigests.insert(found->second);
    const auto assignment =
      user.getCollaborationAssignmentForTest(requestId, provider);
    BOOST_REQUIRE(!assignment.empty());
    CollaborationAssignmentEnvelope envelope;
    BOOST_REQUIRE(decodeCollaborationAssignmentEnvelope(assignment, envelope));
    const auto expectedScopeCount =
      provider == providers[0] || provider == providers[2] ? 1 : 2;
    BOOST_CHECK_EQUAL(envelope.scopeKeys.size(), expectedScopeCount);
    BOOST_CHECK_EQUAL(envelope.scopeKeyDataNames.size(), expectedScopeCount);
    if (provider == providers[0]) {
      BOOST_CHECK(envelope.scopeKeys.count("stage-0-to-1") == 1);
      BOOST_CHECK(envelope.scopeKeys.count("stage-1-to-2") == 0);
      BOOST_CHECK(envelope.scopeKeyDataNames.count("stage-0-to-1") == 1);
      BOOST_CHECK(envelope.scopeKeyDataNames.count("stage-1-to-2") == 0);
    }
    else if (provider == providers[1]) {
      BOOST_CHECK(envelope.scopeKeys.count("stage-0-to-1") == 1);
      BOOST_CHECK(envelope.scopeKeys.count("stage-1-to-2") == 1);
      BOOST_CHECK(envelope.scopeKeyDataNames.count("stage-0-to-1") == 1);
      BOOST_CHECK(envelope.scopeKeyDataNames.count("stage-1-to-2") == 1);
    }
    else {
      BOOST_CHECK(envelope.scopeKeys.count("stage-0-to-1") == 0);
      BOOST_CHECK(envelope.scopeKeys.count("stage-1-to-2") == 1);
      BOOST_CHECK(envelope.scopeKeyDataNames.count("stage-0-to-1") == 0);
      BOOST_CHECK(envelope.scopeKeyDataNames.count("stage-1-to-2") == 1);
    }
    BOOST_CHECK_LT(assignment.size(), 4096);
    const auto reference = parseLargeDataReferencePayload(envelope.opaquePayload);
    BOOST_REQUIRE(reference);
    BOOST_CHECK_EQUAL(
      reference->objectType,
      "application/vnd.ndnsf.collaboration-assignment-v1");
    BOOST_CHECK(reference->encrypted);
    BOOST_CHECK_EQUAL(reference->plaintextSize, 24 * 1024);
    BOOST_CHECK_EQUAL(reference->digest.size(), 71);
    BOOST_CHECK_EQUAL(reference->digest.substr(0, 7), "sha256:");
    ndn::Name expectedPrefix("/user/large-collab/NDNSF/LARGE-DATA");
    expectedPrefix.append(ndn::Name("/generic/work")).append(requestId);
    BOOST_CHECK(expectedPrefix.isPrefixOf(reference->dataName));
    BOOST_CHECK(reference->dataName.get(-1).isVersion());
    combinedAssignmentBytes += assignment.size();
  }
  BOOST_CHECK(combinedAssignmentBytes < 12 * 1024);
  BOOST_CHECK_EQUAL(distinctDigests.size(), 3);

  // Recommitting the identical plan is idempotent: it does not reopen ACKs or
  // publish a second set of provider projections.
  BOOST_CHECK(user.CommitCollaborationPlan(requestId, closed.digest, plan));
  BOOST_CHECK_EQUAL(closureCount, 1);
  BOOST_CHECK_EQUAL(user.getSelectionPublishedProviders(requestId).size(), 3);
  BOOST_CHECK(user.getSelectionDigestsByProvider(requestId) == digests);
}

BOOST_AUTO_TEST_CASE(ExternalCollaborationAssignmentReferenceFailsClosedOnInvalidBinding)
{
  ndn::security::KeyChain keyChain(
    "pib-memory:external-collab-reference", "tpm-memory:external-collab-reference");
  ndn::DummyClientFace face(keyChain);
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/provider/stage-0"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  LocalServiceProvider provider(
    face, ndn::Name("/test/group"), providerCert, aaCert,
    "examples/trust-any.conf");

  const ndn::Name requester("/user/large-collab");
  const ndn::Name requestId("/request/external-assignment");
  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = "stage-0";
  assignment.service = ndn::Name("/generic/work");

  LargeDataReference reference;
  reference.dataName = ndn::Name("/attacker/NDNSF/LARGE-DATA/generic/work")
                         .append(requestId)
                         .append("projection")
                         .appendVersion(1);
  reference.objectType =
    "application/vnd.ndnsf.collaboration-assignment-v1";
  reference.objectId = "projection";
  reference.plaintextSize = 1024;
  reference.encrypted = true;
  reference.digest = "sha256:" + std::string(64, '0');
  assignment.assignmentPayload = encodeLargeDataReferencePayload(reference);

  bool callbackCalled = false;
  bool accepted = true;
  std::string error;
  provider.prepareCollaborationAssignmentForTest(
    requester,
    requestId,
    assignment,
    [&](bool ok, std::string reason, ServiceProvider::CollaborationAssignment) {
      callbackCalled = true;
      accepted = ok;
      error = std::move(reason);
    });

  BOOST_CHECK(callbackCalled);
  BOOST_CHECK(!accepted);
  BOOST_CHECK_EQUAL(error,
                    "invalid external collaboration assignment reference");
}

BOOST_AUTO_TEST_CASE(ProviderDestructionCancelsQueuedAndActiveAssignmentFetches)
{
  for (const bool dispatch : {false, true}) {
    ndn::security::KeyChain keyChain("pib-memory:", "tpm-memory:");
    ndn::DummyClientFace face(keyChain);
    const auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/provider/cancel"));
    const auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
    auto provider = std::make_unique<LocalServiceProvider>(
      face, ndn::Name("/test/group"), providerCert, aaCert, "examples/trust-any.conf");
    ServiceProvider::CollaborationAssignment assignment;
    assignment.role = "stage-0";
    assignment.service = ndn::Name("/generic/work");
    for (int i = 0; i < 8; ++i) {
      assignment.scopeKeyDataNames["missing-" + std::to_string(i)] =
        ndn::Name("/missing/key").appendNumber(i);
    }
    bool callbackCalled = false;
    provider->prepareCollaborationAssignmentForTest(
      ndn::Name("/user/cancel"), ndn::Name("/request/cancel"), assignment,
      [&](bool, std::string, ServiceProvider::CollaborationAssignment) {
        callbackCalled = true;
      });
    if (dispatch) {
      face.processEvents(ndn::time::milliseconds(100));
      BOOST_REQUIRE(std::any_of(face.sentInterests.begin(), face.sentInterests.end(),
        [](const auto& interest) {
          return ndn::Name("/missing/key").isPrefixOf(interest.getName());
        }));
    }
    const auto started = std::chrono::steady_clock::now();
    provider.reset();
    BOOST_CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    // Dispatch posted work after destruction, when raw Provider pointers are invalid.
    face.processEvents(ndn::time::milliseconds(100));
    BOOST_CHECK(!callbackCalled);
  }
}

BOOST_AUTO_TEST_CASE(QueuedAssignmentFetchDoesNotResumeAfterAcceptedRevocation)
{
  ndn::security::KeyChain keyChain("pib-memory:", "tpm-memory:");
  ndn::DummyClientFace face(keyChain);
  const auto cert = makeRsaIdentity(keyChain, ndn::Name("/provider/fenced"));
  const auto aa = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  FencedCollaborationProvider provider(face, ndn::Name("/test/group"), cert, aa,
                                       "examples/trust-any.conf");
  const ndn::Name service("/generic/work");
  const ndn::Name requestId("/request/fenced");
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  PolicyStatusData status;
  status.setServiceName(service);
  status.setControllerVersion(ControllerVersion{1000, 1});
  status.setValidity(now - 1, now + 60000);
  status.setPolicyDigest("sha256:" + std::string(64, 'a'));
  status.setControllerCertificate(aa.getName());
  status.setSignature(ndn::Buffer{1});
  BOOST_REQUIRE(provider.installControllerStatus(status));
  auto release = provider.holdFetchWorkers();
  struct ReleaseOnExit {
    std::shared_ptr<std::promise<void>> signal;
    ~ReleaseOnExit() { if (signal) signal->set_value(); }
  } cleanup{release};
  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = "stage-0";
  assignment.service = service;
  for (int i = 0; i < 4; ++i)
    assignment.scopeKeyDataNames["key-" + std::to_string(i)] =
        ndn::Name("/must-not-fetch/revoked").appendNumber(i);
  size_t callbacks = 0;
  bool accepted = true;
  provider.prepareCollaborationAssignmentForTest(
      ndn::Name("/user/fenced"), requestId, assignment,
      [&](bool ready, std::string, ServiceProvider::CollaborationAssignment) {
        ++callbacks;
        accepted = ready;
      });
  RevocationTarget target;
  target.kind = RevocationKind::IDENTITY;
  target.targetIdentity = cert.getIdentity();
  status.setControllerVersion(ControllerVersion{1000, 2});
  status.addRevocation(target);
  BOOST_REQUIRE(provider.installControllerStatus(status));
  release->set_value();
  cleanup.signal.reset();
  face.processEvents(ndn::time::milliseconds(200));
  BOOST_CHECK_EQUAL(callbacks, 1);
  BOOST_CHECK(!accepted);
  BOOST_CHECK(!provider.hasScopeKeys(requestId));
  BOOST_CHECK(std::none_of(face.sentInterests.begin(), face.sentInterests.end(),
      [](const auto& interest) {
        return ndn::Name("/must-not-fetch/revoked").isPrefixOf(interest.getName());
      }));
}

BOOST_AUTO_TEST_CASE(QueuedCollaborationHandlerRejectsChangedControllerVersion)
{
  ndn::security::KeyChain keyChain("pib-memory:", "tpm-memory:");
  ndn::DummyClientFace face(keyChain);
  const auto cert = makeRsaIdentity(keyChain, ndn::Name("/provider/fenced"));
  const auto aa = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  FencedCollaborationProvider provider(face, ndn::Name("/test/group"), cert, aa,
                                       "examples/trust-any.conf");
  const ndn::Name service("/generic/work");
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  PolicyStatusData status;
  status.setServiceName(service);
  status.setControllerVersion(ControllerVersion{1000, 1});
  status.setValidity(now - 1, now + 60000);
  status.setPolicyDigest("sha256:" + std::string(64, 'a'));
  status.setControllerCertificate(aa.getName());
  status.setSignature(ndn::Buffer{1});
  BOOST_REQUIRE(provider.installControllerStatus(status));
  std::atomic<size_t> executions{0};
  provider.addCollaborationHandler(service,
      [&](ServiceProvider::CollaborationContext&, const RequestMessage&) { ++executions; });
  auto release = provider.holdHandlerWorker();
  struct ReleaseOnExit {
    std::shared_ptr<std::promise<void>> signal;
    ~ReleaseOnExit() { if (signal) signal->set_value(); }
  } cleanup{release};
  RequestMessage request;
  request.setControllerVersion(ControllerVersion{1000, 1});
  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = "stage-0";
  assignment.service = service;
  provider.dispatchForTest(request, assignment);
  BOOST_REQUIRE_EQUAL(provider.getHandlerQueueDepth(), 1);
  // Even a grant-only status advance invalidates the old request version.
  status.setControllerVersion(ControllerVersion{1000, 2});
  BOOST_REQUIRE(provider.installControllerStatus(status));
  release->set_value();
  cleanup.signal.reset();
  provider.drainHandlerWorker();
  BOOST_CHECK_EQUAL(executions.load(), 0);
}

BOOST_AUTO_TEST_CASE(ProviderDestructionFencesQueuedActiveAndPostedCollaborationHandlers)
{
  // 0: still queued; 1: running when destruction joins; 2: completed with its
  // Face callback already queued when the Provider is destroyed; 3: rejected
  // on dequeue with its failure publication callback already queued.
  for (int mode = 0; mode < 4; ++mode) {
    ndn::security::KeyChain keyChain("pib-memory:", "tpm-memory:");
    ndn::DummyClientFace face(keyChain);
    const auto cert = makeRsaIdentity(keyChain, ndn::Name("/provider/fenced"));
    const auto aa = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
    auto provider = std::make_unique<FencedCollaborationProvider>(
        face, ndn::Name("/test/group"), cert, aa, "examples/trust-any.conf");
    provider->setHandlerThreads(1);
    const bool queued = mode == 0 || mode == 3;
    auto release = queued ? provider->holdHandlerWorker() :
        std::make_shared<std::promise<void>>();
    struct ReleaseOnExit {
      std::shared_ptr<std::promise<void>> signal;
      ~ReleaseOnExit() {
        try { signal->set_value(); }
        catch (const std::future_error&) {} // The normal release already ran.
      }
    } cleanup{release};
    std::shared_future<void> released;
    if (mode == 1) released = release->get_future().share();
    auto entered = std::make_shared<std::promise<void>>();
    auto started = entered->get_future();
    std::atomic<size_t> executions{0};
    size_t lifecycleCallbacks = 0;
    provider->setProviderRequestLifecycleCallback(
        [&](const auto&) { ++lifecycleCallbacks; });
    const ndn::Name service("/generic/work");
    provider->addCollaborationHandler(service,
        [&, entered, released](ServiceProvider::CollaborationContext&, const RequestMessage&) {
          ++executions;
          entered->set_value();
          if (released.valid()) released.wait();
        });
    ServiceProvider::CollaborationAssignment assignment;
    assignment.role = "stage-0";
    assignment.service = service;
    provider->dispatchForTest(RequestMessage{}, assignment);
    if (queued) {
      BOOST_REQUIRE_EQUAL(provider->getHandlerQueueDepth(), 1);
    }
    else {
      BOOST_REQUIRE(started.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    }
    if (mode == 3) {
      const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
      PolicyStatusData status;
      status.setServiceName(service);
      status.setControllerVersion(ControllerVersion{1000, 1});
      status.setValidity(now - 1, now + 60000);
      status.setPolicyDigest("sha256:" + std::string(64, 'a'));
      status.setControllerCertificate(aa.getName());
      status.setSignature(ndn::Buffer{1});
      BOOST_REQUIRE(provider->installControllerStatus(status));
      release->set_value();
      provider->drainHandlerWorker();
      provider.reset();
    }
    else if (mode == 2) {
      provider->drainHandlerWorker();
      provider.reset();
    }
    else {
      const auto stopping = provider->stoppingForTest();
      std::atomic<bool> observedStop{false};
      std::thread unblock([stopping, release, &observedStop] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!stopping->load() && std::chrono::steady_clock::now() < deadline)
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        observedStop = stopping->load();
        release->set_value();
      });
      provider.reset();
      unblock.join();
      BOOST_CHECK(observedStop.load());
    }
    const auto callbacksBeforePump = lifecycleCallbacks;
    face.processEvents(ndn::time::milliseconds(100));
    BOOST_CHECK_EQUAL(lifecycleCallbacks, callbacksBeforePump);
    BOOST_CHECK_EQUAL(executions.load(), queued ? 0 : 1);
  }
}

BOOST_AUTO_TEST_CASE(DeferredCollaborationTracksAckDecryptBeforeClosure)
{
  ndn::security::KeyChain keyChain(
    "pib-memory:deferred-ack-decrypt", "tpm-memory:deferred-ack-decrypt");
  ndn::DummyClientFace face(keyChain);
  auto userCert = makeRsaIdentity(keyChain, ndn::Name("/user/decrypt"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  LocalServiceUser user(
    face, ndn::Name("/test/group"), userCert, aaCert,
    "examples/trust-any.conf");
  const ndn::Name requestId("/request/deferred-ack-decrypt");
  user.prepareDeferredCollaborationForTest(
    requestId, [](const CollaborationAckClosure&) {});

  // A deferred collaboration owns an immutable ACK_CLOSED snapshot. An ACK
  // observed before the deadline must therefore be tracked while its
  // asynchronous decrypt finishes; otherwise the timer can freeze an empty
  // candidate set even though the Provider already accepted the Request.
  BOOST_CHECK(user.tracksAckDecryptForTest(requestId));
}

BOOST_AUTO_TEST_CASE(OperationStatusCodecRetainsMonotonicAndUnknownProgressFields)
{
  ServiceProvider::ServiceOperationStatus status;
  status.operationId = "prepare:prefill";
  status.operation = "MODEL_PREPARE";
  status.serviceName = ndn::Name("/LLM/Qwen");
  status.providerName = ndn::Name("/provider/a");
  status.requestId = ndn::Name("/request/1");
  status.role = "prefill";
  status.attempt = 2;
  status.epoch = 3;
  status.sequence = 4;
  status.state = "LOADING";
  status.progressKnown = false;
  status.progress = 0.0;
  status.detailsSchema = "ndnsf-di-progress-v1";
  status.detailsPayload = ndn::Buffer{0x00, 0x7f, 0xff};

  const auto wire = ServiceProvider::makeServiceOperationStatusPayload(status);
  const auto decoded = ServiceProvider::parseServiceOperationStatusPayload(wire);
  BOOST_REQUIRE(decoded);
  BOOST_CHECK_EQUAL(decoded->role, "prefill");
  BOOST_CHECK_EQUAL(decoded->attempt, 2);
  BOOST_CHECK_EQUAL(decoded->epoch, 3);
  BOOST_CHECK_EQUAL(decoded->sequence, 4);
  BOOST_CHECK(!decoded->progressKnown);
  BOOST_CHECK_EQUAL_COLLECTIONS(decoded->detailsPayload.begin(),
                                decoded->detailsPayload.end(),
                                status.detailsPayload.begin(),
                                status.detailsPayload.end());
}

BOOST_AUTO_TEST_CASE(SelectionSnapshotRejectsStaleMemberAndKeepsLatest)
{
  ndn::security::KeyChain keyChain("pib-memory:collab-status",
                                   "tpm-memory:collab-status");
  ndn::DummyClientFace face(keyChain);
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/provider/a"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  LocalServiceProvider provider(face, ndn::Name("/test/group"),
                                providerCert, aaCert,
                                "examples/trust-any.conf");
  provider.seedSelectionStatusForTest("sha256:selection",
                                      ndn::Name("/LLM/Qwen"),
                                      ndn::Name("/request/1"));

  ServiceProvider::ServiceOperationStatus status;
  status.operationId = "prepare:decode";
  status.operation = "MODEL_PREPARE";
  status.role = "decode";
  status.attempt = 1;
  status.epoch = 1;
  status.sequence = 1;
  status.state = "FETCHING";
  status.progressKnown = true;
  status.progress = 0.25;
  provider.reportSelectionOperationStatus("sha256:selection", status);

  auto snapshot = provider.getSelectionExecutionStatus("sha256:selection");
  BOOST_REQUIRE(snapshot);
  BOOST_REQUIRE_EQUAL(snapshot->memberStatuses.size(), 1);
  BOOST_CHECK_EQUAL(snapshot->memberStatuses.front().role, "decode");
  BOOST_CHECK_CLOSE(snapshot->memberStatuses.front().progress, 0.25, 0.001);

  BOOST_CHECK_THROW(
    provider.reportSelectionOperationStatus("sha256:selection", status),
    std::invalid_argument);
  status.sequence = 2;
  status.state = "VERIFYING";
  status.progress = 0.5;
  provider.reportSelectionOperationStatus("sha256:selection", status);
  snapshot = provider.getSelectionExecutionStatus("sha256:selection");
  BOOST_REQUIRE(snapshot);
  BOOST_REQUIRE_EQUAL(snapshot->memberStatuses.size(), 1);
  BOOST_CHECK_EQUAL(snapshot->memberStatuses.front().sequence, 2);
}

BOOST_AUTO_TEST_CASE(CollaborationFailureUpdatesSelectionStatus)
{
  ndn::security::KeyChain keyChain("pib-memory:collab-failure",
                                   "tpm-memory:collab-failure");
  ndn::DummyClientFace face(keyChain);
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/provider/a"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  LocalServiceProvider provider(face, ndn::Name("/test/group"),
                                providerCert, aaCert,
                                "examples/trust-any.conf");
  const std::string selectionDigest = "sha256:failed-selection";
  const ndn::Name serviceName("/LLM/Qwen");
  const ndn::Name requestId("/request/failed-1");
  provider.seedSelectionStatusForTest(selectionDigest, serviceName, requestId);

  provider.failCollaborationForTest(selectionDigest,
                                    serviceName,
                                    requestId,
                                    "model fragment verification failed");

  const auto snapshot = provider.getSelectionExecutionStatus(selectionDigest);
  BOOST_REQUIRE(snapshot);
  BOOST_CHECK(snapshot->state == SelectionExecutionState::Failed);
  BOOST_CHECK_EQUAL(snapshot->serviceName, serviceName);
  BOOST_CHECK_EQUAL(snapshot->requestId, requestId);
  BOOST_CHECK_EQUAL(snapshot->message, "model fragment verification failed");
  BOOST_CHECK_NE(snapshot->completedAtUs, 0);
}

BOOST_AUTO_TEST_CASE(R1DecisionReceiptSurvivesSignedStatusPayloadCodec)
{
  SelectionDecisionReceipt receipt;
  receipt.setField("decisionDigest", "sha256:decision");
  receipt.setField("reservationId", "reservation-1");
  receipt.setField("provider", "/provider/a");
  receipt.setField("acceptedState", "RELEASE_ACCEPTED");
  const auto block = receipt.WireEncode();
  SelectionExecutionStatus status;
  status.providerName = ndn::Name("/provider/a");
  status.serviceName = ndn::Name("/Inference/Generic");
  status.requestId = ndn::Name("request-1");
  status.selectionDigest = "sha256:selection";
  status.state = SelectionExecutionState::Completed;
  status.decisionReceipt = ndn::Buffer(block.data(), block.size());
  const auto payload = LocalServiceProvider::encodeSelectionStatusForTest(status);
  ndn::Data data(ndn::Name("/provider/a/status"));
  data.setContent(payload);
  const auto decoded = LocalServiceUser::parseSelectionStatusForTest(
    data, status.providerName, status.selectionDigest);
  BOOST_CHECK_EQUAL_COLLECTIONS(decoded.decisionReceipt.begin(),
                                decoded.decisionReceipt.end(),
                                status.decisionReceipt.begin(),
                                status.decisionReceipt.end());
}

BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
