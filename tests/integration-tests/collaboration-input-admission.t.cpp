#include "tests/boost-test.hpp"
#include "tests/integration-tests/ndnsf-integration-fixture.hpp"
#include "ndn-service-framework/dependency-transfer.hpp"
#include "ndn-service-framework/detail/finite-packet-layout.hpp"
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/signal/scoped-connection.hpp>
#include <boost/asio/post.hpp>

#include <algorithm>
#include <atomic>
#include <memory>
#include <set>
#include <thread>

namespace ndn_service_framework::test {
namespace {
enum class AdmissionCase { Complete, Missing, Cancelled, WrongScope, Overloaded,
                           GenericLarge, GenericBadTag, GenericMetadataMismatch, DomainRejected,
                           ReusableLarge, RetainedCopy, CancelledDuringValidation, InboxOverload, ReplayQuota, ImmutableGeneric,
                           ConcurrentIndependentObject, WrongDigest, WrongRole, FaceBoundary,
                           RegistrationClosedDuringValidation, CanonicalManifest, BadOriginalSignature,
                           ScopeCredentialMismatch, LegacyReplay,
                           MultiIndex, IndexCycle, OverlappingIndexBudget,
                           IndexDepthFour, IndexDepthFive };

class AdmissionSelector final : public ParticipantSelectionPolicy
{
public:
  ndn::Name source;
  std::vector<ndn::Name> sources;
  ndn::Name consumer;
  std::vector<SelectedParticipant> select(const std::vector<AckCandidate>& candidates,
      const std::vector<CollaborationRoleSpec>&) const override
  {
    std::vector<SelectedParticipant> result;
    for (const auto& candidate : candidates) {
      const bool isSource = candidate.providerName == source ||
        std::find(sources.begin(), sources.end(), candidate.providerName) != sources.end();
      if (isSource) {
        result.push_back({"/source", candidate.serviceName, candidate.providerName,
          {}, false, 0, ndn::Buffer{'a'}, candidate, {}});
      }
      else if (consumer.empty() || candidate.providerName == consumer) {
        result.push_back({"/consumer", candidate.serviceName, candidate.providerName,
          {}, false, 0, ndn::Buffer{'a'}, candidate, {}});
      }
    }
    return result;
  }
};

struct AdmissionObservations
{
  std::atomic<size_t> computes{0};
  std::atomic<size_t> validators{0};
  std::atomic<size_t> rootInterests{0};
  std::atomic<size_t> producerLocationHints{0};
  std::atomic<size_t> selectedRoleLocationHints{0};
  std::atomic<size_t> unselectedLocationHints{0};
  std::atomic<size_t> keyInterests{0};
  std::atomic<size_t> keyControlInterests{0};
  std::atomic<bool> trackKeyReads{false};
  std::atomic<bool> completed{false};
  std::atomic<bool> successful{false};
  std::atomic<bool> timedOut{false};
  std::atomic<bool> committed{false};
  std::atomic<size_t> closedCandidates{0};
  std::string reason;
  std::shared_ptr<InputAdmissionOperation> operation;
  std::atomic<bool> releaseValidator{false};
  std::atomic<bool> lifetimeChecks{false};
  std::atomic<size_t> corruptedPackets{0};
  std::atomic<bool> corruptedSignaturesValid{true};
  std::atomic<bool> originalProducerSignatureValid{false};
  std::atomic<bool> alternateProducerSignatureValid{false};
  std::atomic<size_t> callbacks{0};
  std::string terminalReason;
  std::string postOverloadReason;
  std::atomic<size_t> repairInterests{0};
  std::atomic<size_t> inBandDrops{0};
  std::atomic<size_t> inBandRecovered{0};
  std::atomic<size_t> inBandRepairInterests{0};
  size_t inBandRecoveryAttempts = 0;
  size_t inBandRejectedRepairs = 0;
  size_t inBandArqFallbacks = 0;
  size_t inBandRootControlInterests = 0;
  std::string inBandRecoveryTerminal;
  std::atomic<size_t> resolvers{0};
  std::atomic<size_t> forbiddenIndexInterests{0};
  std::atomic<bool> slotsCorrect{false};
  std::atomic<bool> unknownSlotRejected{false};
  std::atomic<bool> duplicateDataDelivered{false};
  std::mutex locatorMutex;
  std::set<ndn::Name> reachedProducer;
  std::set<ndn::Name> locationInterestNames;
};

void runAuthorizedLocationRotationCase(bool authorizedObject = false)
{
  auto observed = std::make_shared<AdmissionObservations>();
  BootstrapProfile profile;
  profile.providerCount = 4;
  profile.providerRoles = {"/source", "/source", "/source", "/consumer"};
  profile.validateSvsPackets = true;
  profile.serviceName = ndn::Name("/spec226/admission");
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();
  environment.disconnectProviderPeerTransportForTest();

  auto& producer = environment.provider(0);
  auto& selectedRelay = environment.provider(1);
  auto& unselected = environment.provider(2);
  auto& consumer = environment.provider(3);
  const auto producerName = producer.getName();
  const auto relayName = selectedRelay.getName();
  const auto unselectedName = unselected.getName();
  const auto root = ndn::Name(producerName).append("NDNSF").append("INPUT")
    .appendVersion(1);
  ndn::signal::ScopedConnection interests = environment.providerFace(3).onSendInterest.connect(
    [observed, root, producerName, relayName, &environment](const ndn::Interest& interest) {
      if (!root.isPrefixOf(interest.getName())) {
        return;
      }
      const auto& hints = interest.getForwardingHint();
      {
        std::lock_guard<std::mutex> lock(observed->locatorMutex);
        observed->locationInterestNames.insert(interest.getName());
      }
      if (hints.size() != 1) {
        ++observed->unselectedLocationHints;
        return;
      }
      if (hints.front() == producerName) {
        // Model a transient failure on the Producer route. The next attempt
        // must use the Core-derived selected-role route.
        if (observed->producerLocationHints.fetch_add(1) == 0) {
          return;
        }
      }
      else if (hints.front() == relayName) {
        ++observed->selectedRoleLocationHints;
      }
      else {
        ++observed->unselectedLocationHints;
        return;
      }
      {
        std::lock_guard<std::mutex> lock(observed->locatorMutex);
        observed->reachedProducer.insert(interest.getName());
      }
      // The fixture models selectedRelay as a forwarding route to the original
      // Producer. The Interest name remains rooted at producerName.
      environment.providerFace(0).receive(interest);
    });
  ndn::signal::ScopedConnection packets = environment.providerFace(0).onSendData.connect(
    [observed, root, &environment](const ndn::Data& data) {
      if (!root.isPrefixOf(data.getName())) {
        return;
      }
      {
        std::lock_guard<std::mutex> lock(observed->locatorMutex);
        if (!observed->reachedProducer.count(data.getName())) {
          return;
        }
      }
      environment.providerFace(3).receive(data);
    });

  const auto ack = [](const RequestMessage&) {
    ServiceProvider::AckDecision decision;
    decision.status = true;
    decision.message = "finite-input-capable";
    return decision;
  };
  CollaborationAdmissionLimits limits;
  limits.maxInputAcquisitionMs = 1500;
  auto publish = [root](ServiceProvider::CollaborationContext& context,
                        const RequestMessage&) {
    if (!context.publishSignedExactData("input", {{root, ndn::Buffer{3, 7}}})) {
      throw std::runtime_error("authorized-location fixture publication failed");
    }
    context.completeRole();
  };
  auto finishWithoutData = [](ServiceProvider::CollaborationContext& context,
                              const RequestMessage&) {
    context.completeRole();
  };
  auto producerRegistration = producer.addScopedCollaborationHandler(
    profile.serviceName, {"/source"}, ack, publish, limits);
  auto relayRegistration = selectedRelay.addScopedCollaborationHandler(
    profile.serviceName, {"/source"}, ack, finishWithoutData, limits);
  auto unselectedRegistration = unselected.addScopedCollaborationHandler(
    profile.serviceName, {"/source"}, ack, finishWithoutData, limits);

  auto consumerRegistration = consumer.addScopedCollaborationHandler(
    profile.serviceName, {"/consumer"}, ack,
    [root, producerName, service = profile.serviceName, &environment, observed, authorizedObject]
    (ServiceProvider::CollaborationContext& context, const RequestMessage&) {
      CollaborationInputSpec input;
      input.slotId = "producer-object";
      input.producerRole = authorizedObject ? "" : "/source";
      input.provenanceBinding = authorizedObject ? InputProvenanceBinding::AuthorizedObject :
                                                   InputProvenanceBinding::SelectedRole;
      input.object.name = root;
      input.object.producerName = producerName;
      input.object.serviceName = service;
      input.keyScope = "input";
      input.encoding = InputObjectEncoding::SignedExactPackets;
      input.digestKind = InputDigestKind::OrderedWireSha256;
      input.maxBytes = 4096;
      input.maxPackets = 1;
      const auto resolve = [](const CollaborationInputSpec& spec,
                              const ndn::Data& authenticated) {
        if (authenticated.getName() != spec.object.name) {
          throw std::runtime_error("authorized-location Interest name changed");
        }
        DependencyPacketPlan plan;
        plan.rootName = spec.object.name;
        plan.producer = spec.object.producerName;
        plan.packets = {{spec.object.name, {}, {}}};
        plan.maxWireBytes = spec.maxBytes;
        plan.maxPackets = spec.maxPackets;
        return plan;
      };
      const auto validate = [&environment, observed, producerName]
          (const std::vector<VerifiedInputObject>& objects) {
        ++observed->validators;
        if (objects.size() != 1 || objects.front().producer() != producerName ||
            objects.front().packets().size() != 1) {
          throw std::runtime_error("authorized-location object identity changed");
        }
        const auto sourceCertificate = environment.keyChain().getPib()
          .getIdentity(producerName).getDefaultKey().getDefaultCertificate();
        const auto relayCertificate = environment.keyChain().getPib()
          .getIdentity(environment.provider(1).getName())
          .getDefaultKey().getDefaultCertificate();
        const auto& packet = *objects.front().packets().front();
        observed->originalProducerSignatureValid =
          ndn::security::verifySignature(packet, sourceCertificate);
        observed->alternateProducerSignatureValid =
          ndn::security::verifySignature(packet, relayCertificate);
      };
      auto result = context.startInputAdmission({input}, validate, resolve).get();
      observed->reason = result.reasonCode;
      if (result.status == InputAdmissionStatus::Ready && result.inputs &&
          result.inputs->isCurrent()) {
        ++observed->computes;
        observed->successful = true;
        context.publishFinalResponse(ndn::Buffer{10});
      }
      else {
        context.publishFinalFailureResponse({}, result.reasonCode);
      }
    }, limits);

  auto selector = std::make_shared<AdmissionSelector>();
  selector->sources = {producerName, relayName};
  selector->consumer = consumer.getName();
  CollaborationPlan plan;
  plan.ackCollectionTimeMs = 1000;
  plan.timeoutMs = 8000;
  CollaborationRoleSpec sourceRole;
  sourceRole.role = "/source";
  sourceRole.service = profile.serviceName;
  sourceRole.minProviders = 2;
  sourceRole.maxProviders = 2;
  sourceRole.inputAdmissionProfile = "data-admission-v1";
  CollaborationRoleSpec consumerRole;
  consumerRole.role = "/consumer";
  consumerRole.service = profile.serviceName;
  consumerRole.terminalResponseOwner = true;
  consumerRole.inputAdmissionProfile = "data-admission-v1";
  plan.roles = {sourceRole, consumerRole};
  plan.keyScopes.push_back({"input", {"/source", "/consumer"}});
  plan.participantSelector = selector;

  auto& user = environment.user();
  const auto requestId = user.BeginCollaborationWithProviders(
    profile.serviceName, ndn::Buffer{}, 1000, plan.timeoutMs,
    [&user, observed, plan = std::move(plan)]
        (const CollaborationAckClosure& closed) mutable {
      observed->committed = user.CommitCollaborationPlan(
        closed.requestId, closed.digest, std::move(plan));
    },
    [observed](const ResponseMessage& response) {
      observed->successful = observed->successful && response.getStatus() &&
        response.getPayload() == ndn::Buffer{10};
      observed->completed = true;
    },
    [observed](const ndn::Name&) { observed->timedOut = true; },
    {}, {}, RequestCapabilities(), std::nullopt, {}, {}, {},
    {producerName, relayName, unselectedName, consumer.getName()});
  BOOST_REQUIRE(!requestId.empty());

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!observed->completed && !observed->timedOut &&
         std::chrono::steady_clock::now() < deadline) {
    environment.pumpUntil([&] { return observed->completed || observed->timedOut; });
  }
  BOOST_REQUIRE_MESSAGE(observed->completed,
                       "authorized-location read failed: " << observed->reason);
  BOOST_CHECK(!observed->timedOut);
  BOOST_CHECK(observed->committed.load());
  BOOST_CHECK_EQUAL(observed->validators.load(), 1);
  BOOST_CHECK_EQUAL(observed->computes.load(), 1);
  BOOST_CHECK(observed->successful.load());
  BOOST_CHECK_GT(observed->producerLocationHints.load(), 0);
  if (authorizedObject) {
    BOOST_CHECK_GE(observed->producerLocationHints.load(), 2);
    BOOST_CHECK_EQUAL(observed->selectedRoleLocationHints.load(), 0);
  }
  else {
    BOOST_CHECK_GT(observed->selectedRoleLocationHints.load(), 0);
  }
  BOOST_CHECK_EQUAL(observed->unselectedLocationHints.load(), 0);
  BOOST_CHECK(observed->originalProducerSignatureValid.load());
  BOOST_CHECK(!observed->alternateProducerSignatureValid.load());
  {
    std::lock_guard<std::mutex> lock(observed->locatorMutex);
    BOOST_REQUIRE_EQUAL(observed->locationInterestNames.size(), 1);
    BOOST_CHECK_EQUAL(*observed->locationInterestNames.begin(), root);
  }

  producer.setHandlerThreads(0);
  selectedRelay.setHandlerThreads(0);
  unselected.setHandlerThreads(0);
  consumer.setHandlerThreads(0);
}

void runAdmissionCase(AdmissionCase mode, bool inBandXor = false)
{
  auto observed = std::make_shared<AdmissionObservations>();
  auto payload = std::make_shared<ndn::Buffer>(
    inBandXor && mode == AdmissionCase::ImmutableGeneric ? 24000 :
    inBandXor && mode == AdmissionCase::ReusableLarge ? 2400 : 2);
  std::fill(payload->begin(), payload->end(), 3);
  (*payload)[1] = 7;
  const std::shared_ptr<const ndn::Buffer> immutablePayload = payload;
  BootstrapProfile profile;
  profile.providerCount = 2;
  profile.providerRoles = {"/source", "/consumer"};
  profile.validateSvsPackets = true;
  profile.serviceName = ndn::Name("/spec226/admission");
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();
  const bool notificationCase = mode == AdmissionCase::InboxOverload || mode == AdmissionCase::ReplayQuota ||
    mode == AdmissionCase::ImmutableGeneric || mode == AdmissionCase::CanonicalManifest || mode == AdmissionCase::LegacyReplay;
  if (!notificationCase) environment.disconnectProviderPeerTransportForTest();
  auto& source = environment.provider(0);
  auto& consumer = environment.provider(1);
  const ndn::Name root = ndn::Name(source.getName()).append("NDNSF").append("INPUT").appendVersion(1);
  const ndn::Name a = ndn::Name(root).append("a");
  const ndn::Name b = ndn::Name(root).append("b");
  std::vector<ndn::signal::ScopedConnection> inBandLinks;
  if (inBandXor) {
    environment.disconnectProviderPeerTransportForTest();
    for (size_t sender = 0; sender < 2; ++sender) {
      auto* destination = &environment.providerFace(1 - sender);
      inBandLinks.emplace_back(environment.providerFace(sender).onSendInterest.connect(
        [destination](const ndn::Interest& interest) { destination->receive(interest); }));
      inBandLinks.emplace_back(environment.providerFace(sender).onSendData.connect(
        [destination, sender, prefix = root.getPrefix(-1), a, mode, observed](const ndn::Data& data) {
          const bool missing = mode == AdmissionCase::Complete || mode == AdmissionCase::RetainedCopy ?
            data.getName() == a :
            prefix.isPrefixOf(data.getName()) && data.getName().at(-1).isSegment() &&
            data.getName().at(-1).toSegment() == 1;
          if (sender == 0 && missing) {
            ++observed->inBandDrops;
            return;
          }
          destination->receive(data);
        }));
    }
  }
  std::optional<LargeDataPublishResult> canonicalPublication;
  ServiceProvider::DataProductReference canonicalReference;
  if (mode == AdmissionCase::CanonicalManifest) {
    const auto publicationContext = environment.user().prepareServiceRequest(profile.serviceName.toUri());
    canonicalPublication = environment.user().publishEncryptedLargeData(publicationContext,
      ndn::Buffer{3, 7}, "spec226-canonical-input", ndn::time::milliseconds(60000), true);
    BOOST_REQUIRE_MESSAGE(canonicalPublication->success, canonicalPublication->errorMessage);
    BOOST_REQUIRE(!canonicalPublication->manifestName.empty());
    canonicalReference.name = canonicalPublication->manifestName;
    canonicalReference.producerName = environment.user().getName();
    canonicalReference.serviceName = profile.serviceName;
    canonicalReference.sizeBytes = 2;
  }
  ndn::signal::ScopedConnection interests = environment.providerFace(1).onSendInterest.connect(
    [&](const ndn::Interest& interest) {
      if (mode == AdmissionCase::ImmutableGeneric && observed->trackKeyReads.load() &&
          (ndn::Name(source.getName()).append("NDNSF").append("KEY").isPrefixOf(interest.getName()) ||
           ndn::Name(source.getName()).append("CK").isPrefixOf(interest.getName())))
        ++observed->keyInterests;
      if (notificationCase || inBandXor) return;
      if (source.getName().isPrefixOf(interest.getName())) {
        if (mode == AdmissionCase::IndexDepthFive && interest.getName() == ndn::Name(root).append("index").appendNumber(4))
          ++observed->forbiddenIndexInterests;
        if (interest.getName().toUri().find("DEPENDENCY-REPAIR") != std::string::npos)
          ++observed->repairInterests;
        if (root.isPrefixOf(interest.getName())) ++observed->rootInterests;
        environment.providerFace(0).receive(interest);
      }
    });
  ndn::signal::ScopedConnection packets = environment.providerFace(0).onSendData.connect(
    [&](const ndn::Data& data) {
      if (notificationCase || inBandXor) return;
      if (!source.getName().isPrefixOf(data.getName())) return;
      if (root.isPrefixOf(data.getName()) && mode == AdmissionCase::BadOriginalSignature) {
        ndn::Data corrupted(data);
        ndn::Buffer bytes(data.getContent().value_begin(), data.getContent().value_end());
        if (bytes.empty()) throw std::runtime_error("fixture original has no content");
        bytes[0] ^= 1;
        corrupted.setContent(bytes);
        ++observed->corruptedPackets;
        if (ndn::security::verifySignature(corrupted, environment.keyChain().getPib().getIdentity(
              source.getName()).getDefaultKey().getDefaultCertificate()))
          observed->corruptedSignaturesValid = false;
        environment.providerFace(1).receive(corrupted);
        return;
      }
      if (root.isPrefixOf(data.getName()) &&
          (mode == AdmissionCase::GenericBadTag || mode == AdmissionCase::GenericMetadataMismatch)) {
        ndn::Data corrupted(data);
        HybridMessageEnvelope envelope;
        if (!envelope.WireDecode(data.getContent().blockFromValue()))
          throw std::runtime_error("fixture envelope decoding failed");
        if (mode == AdmissionCase::GenericBadTag) {
          auto cipher = envelope.getCipherText();
          if (cipher.empty()) throw std::runtime_error("fixture has no ciphertext");
          cipher[0] ^= 1;
          envelope.setCipherText(cipher);
        }
        else envelope.setMessageType("WRONG-PROTECTION");
        const auto wire = envelope.WireEncode();
        corrupted.setContent(wire);
        environment.keyChain().sign(corrupted, ndn::security::signingByIdentity(source.getName()));
        ++observed->corruptedPackets;
        if (!ndn::security::verifySignature(corrupted, environment.keyChain().getPib().getIdentity(
              source.getName()).getDefaultKey().getDefaultCertificate()))
          observed->corruptedSignaturesValid = false;
        environment.providerFace(1).receive(corrupted);
      }
      else environment.providerFace(1).receive(data);
    });
  auto ack = [](const RequestMessage&) {
    ServiceProvider::AckDecision decision;
    decision.status = true;
    decision.message = "finite-input-capable";
    return decision;
  };
  CollaborationAdmissionLimits limits;
  if (mode == AdmissionCase::OverlappingIndexBudget) limits.maxRetainedBytes = 256 * 1024;
  limits.maxInputAcquisitionMs = mode == AdmissionCase::ImmutableGeneric ||
      mode == AdmissionCase::ReusableLarge || mode == AdmissionCase::CanonicalManifest ? 5000 : 300;
  auto sourceRegistration = source.addScopedCollaborationHandler(profile.serviceName, {"/source"}, ack,
    [root, a, b, mode, inBandXor, observed, immutablePayload, &environment](ServiceProvider::CollaborationContext& context, const RequestMessage&) {
      if (mode == AdmissionCase::CanonicalManifest) { context.completeRole(); return; }
      if (mode == AdmissionCase::ImmutableGeneric) {
        const auto published = context.publishImmutableProtectedData(
          root.getPrefix(-1), *immutablePayload);
        if (!published.success) throw std::runtime_error(published.errorMessage);
        const auto reused = context.publishImmutableProtectedData(
          root.getPrefix(-1), *immutablePayload);
        if (!reused.success || reused.dataName != published.dataName ||
            reused.contentSha256 != published.contentSha256)
          throw std::runtime_error("immutable publication identity/reuse failed");
        ServiceProvider::DataProductReference reference;
        reference.name = published.dataName;
        reference.producerName = context.localProvider();
        reference.serviceName = context.assignment().service;
        reference.sizeBytes = immutablePayload->size();
        reference.digest = published.contentSha256;
        context.publish("input", ndn::Name("/immutable-ref"),
          ServiceProvider::makeDataProductReferencePayload(reference));
        context.completeRole();
        return;
      }
      if (mode == AdmissionCase::InboxOverload || mode == AdmissionCase::ReplayQuota || mode == AdmissionCase::LegacyReplay) {
        context.publish("input", ndn::Name("/finite"), ndn::Buffer{3, 7});
        context.publish("input", ndn::Name("/finite"), ndn::Buffer{5, 9});
        context.completeRole();
        return;
      }
      if (mode == AdmissionCase::GenericLarge || mode == AdmissionCase::GenericBadTag ||
          mode == AdmissionCase::GenericMetadataMismatch) {
        if (context.publishLargeNamed("input", root, *immutablePayload,
            mode == AdmissionCase::GenericLarge ? 64 : 7000).empty())
          throw std::runtime_error("fixture protected publication failed");
        context.completeRole();
        return;
      }
      if (mode == AdmissionCase::ReusableLarge) {
        if (context.publishReusableLargeNamed("input", root, *immutablePayload, 700).empty())
          throw std::runtime_error("fixture reusable object publication failed");
        const auto secondObject = ndn::Name(context.localProvider()).append("NDNSF")
          .append("INPUT").append("SECOND").appendVersion(1);
        if (context.publishReusableLargeNamed("input", secondObject, *immutablePayload, 700).empty())
          throw std::runtime_error("fixture second reusable object publication failed");
        context.completeRole();
        return;
      }
      std::vector<std::pair<ndn::Name, ndn::Buffer>> originals{
        {root, ndn::Buffer{2}}, {a, ndn::Buffer{3}}};
      if (mode != AdmissionCase::Missing && mode != AdmissionCase::Cancelled &&
          mode != AdmissionCase::ConcurrentIndependentObject)
        originals.push_back({b, ndn::Buffer{7}});
      if (mode == AdmissionCase::ConcurrentIndependentObject)
        originals.push_back({ndn::Name(context.localProvider()).append("NDNSF").append("CONTROL-PROGRESS").appendVersion(1),
          ndn::Buffer{9}});
      if (mode == AdmissionCase::OverlappingIndexBudget)
        for (size_t i = 0; i < 12; ++i)
          originals.push_back({ndn::Name(root).append("index").appendNumber(i), ndn::Buffer{4}});
      if (mode == AdmissionCase::IndexDepthFour || mode == AdmissionCase::IndexDepthFive)
        for (size_t i = 0; i < 5; ++i)
          originals.push_back({ndn::Name(root).append("index").appendNumber(i), ndn::Buffer{4}});
      if (!context.publishSignedExactData("input", originals))
        throw std::runtime_error("fixture publication failed");
      context.completeRole();
    }, limits);
  if (mode == AdmissionCase::Overloaded || mode == AdmissionCase::InboxOverload) limits.maxRetainedBytes = 1;
  if (mode == AdmissionCase::ReplayQuota) limits.maxQueuedCallbacks = 1;
  if (mode == AdmissionCase::RetainedCopy) limits.maxRetainedObjects = 1;
  const auto producer = source.getName();
  auto consumerRegistration = consumer.addScopedCollaborationHandler(profile.serviceName, {"/consumer"}, ack,
    [root, a, b, producer, mode, observed, canonicalReference, immutablePayload, face = &environment.providerFace(1), service = profile.serviceName]
    (ServiceProvider::CollaborationContext& context, const RequestMessage&) {
      if (mode == AdmissionCase::LegacyReplay) {
        context.allowData("input", ndn::Name("/finite"));
        const auto history = context.waitFor("input", ndn::Name("/finite"), 2, 1000);
        if (history.size() != 2) {
          context.publishFinalFailureResponse({}, "fixture legacy history incomplete");
          return;
        }
        observed->lifetimeChecks = true;
        context.subscribe("input", ndn::Name("/finite"),
          [observed, face](ServiceProvider::CollaborationContext& callbackContext,
                           const ServiceProvider::CollaborationData&) {
            if (face->getIoContext().get_executor().running_in_this_thread()) observed->lifetimeChecks = false;
            if (observed->callbacks.fetch_add(1) + 1 == 2) {
              ++observed->computes;
              callbackContext.publishFinalResponse(ndn::Buffer{10});
            }
          });
        return;
      }
      if (mode == AdmissionCase::InboxOverload || mode == AdmissionCase::ReplayQuota) {
        context.allowData("input", ndn::Name("/finite"));
        if (mode == AdmissionCase::ReplayQuota) {
          context.waitFor("input", ndn::Name("/finite"), 2, 1000);
          context.subscribe("input", ndn::Name("/finite"),
            [observed](const ServiceProvider::CollaborationData&) { ++observed->callbacks; });
        }
        else context.waitFor("input", ndn::Name("/finite"), 2, 1000);
        return;
      }
      CollaborationInputSpec input;
      input.slotId = "finite-pair";
      input.producerRole = "/source";
      if (mode == AdmissionCase::WrongRole) input.producerRole = "/consumer";
      input.object.name = root;
      input.object.producerName = producer;
      input.object.serviceName = service;
      if (mode == AdmissionCase::WrongDigest) input.object.digest = "sha256:" + std::string(64, '0');
      input.keyScope = mode == AdmissionCase::WrongScope ? "ungranted" : "input";
      input.encoding = InputObjectEncoding::SignedExactPackets;
      input.digestKind = InputDigestKind::OrderedWireSha256;
      input.maxBytes = 4096;
      input.maxPackets = 3;
      if (mode == AdmissionCase::IndexDepthFour || mode == AdmissionCase::IndexDepthFive) {
        input.maxBytes = 16 * 1024;
        input.maxPackets = 8;
      }
      if (mode == AdmissionCase::OverlappingIndexBudget) {
        input.maxBytes = 16 * 1024;
        input.maxPackets = 15;
      }
      if (mode == AdmissionCase::CanonicalManifest) {
        input.object = canonicalReference;
        input.provenanceBinding = InputProvenanceBinding::AuthorizedObject;
        input.producerRole.clear();
        input.encoding = InputObjectEncoding::ImmutableProtected;
        input.digestKind = InputDigestKind::PlaintextSha256;
        input.maxBytes = 64 * 1024;
        input.maxPackets = 32;
      }
      if (mode == AdmissionCase::ImmutableGeneric) {
        context.allowData("input", ndn::Name("/immutable-ref"));
        const auto notification = context.waitOne("input", ndn::Name("/immutable-ref"), 1000);
        if (!notification) {
          context.publishFinalFailureResponse({}, "fixture immutable reference missing");
          return;
        }
        const auto reference = ServiceProvider::parseDataProductReferencePayload(notification->payload);
        if (!reference) throw std::runtime_error("fixture immutable reference malformed");
        input.object = *reference;
        input.encoding = InputObjectEncoding::ImmutableProtected;
        input.digestKind = InputDigestKind::PlaintextSha256;
        input.maxBytes = 64 * 1024;
        input.maxPackets = 32;
      }
      if (mode == AdmissionCase::GenericLarge || mode == AdmissionCase::GenericBadTag ||
          mode == AdmissionCase::GenericMetadataMismatch) {
        input.encoding = InputObjectEncoding::CollaborationLarge;
        input.digestKind = InputDigestKind::PlaintextSha256;
        input.maxPackets = 16;
        if (immutablePayload->size() > 4096) input.maxBytes = 64 * 1024;
      }
      if (mode == AdmissionCase::ReusableLarge) {
        input.encoding = InputObjectEncoding::ReusableCollaborationObject;
        input.digestKind = InputDigestKind::PlaintextSha256;
        input.maxBytes = 64 * 1024;
        input.maxPackets = 16;
      }
      auto resolver = [root, a, b, producer, mode, observed](const CollaborationInputSpec& spec, const ndn::Data& authenticated) {
        ++observed->resolvers;
        if (mode == AdmissionCase::IndexDepthFour || mode == AdmissionCase::IndexDepthFive) {
          DependencyPacketPlan plan;
          plan.rootName = root;
          plan.producer = producer;
          plan.packets = {{authenticated.getName(), {}, {}}};
          plan.maxWireBytes = spec.maxBytes;
          plan.maxPackets = spec.maxPackets;
          const auto depth = authenticated.getName() == root ? 0 : authenticated.getName().at(-1).toNumber() + 1;
          const size_t maximum = mode == AdmissionCase::IndexDepthFour ? 4 : 5;
          if (depth < maximum)
            plan.indexDataNames.push_back(ndn::Name(root).append("index").appendNumber(depth));
          else {
            plan.packets.push_back({a, {}, {}});
            plan.packets.push_back({b, {}, {}});
          }
          return plan;
        }
        if (mode == AdmissionCase::OverlappingIndexBudget) {
          if (authenticated.getContent().value_size() != 1)
            throw std::runtime_error("bad overlapping index");
          DependencyPacketPlan plan;
          plan.rootName = root;
          plan.producer = producer;
          plan.packets = {{root, {}, {}}, {a, {}, {}}, {b, {}, {}}};
          for (size_t i = 0; i < 12; ++i) {
            const auto index = ndn::Name(root).append("index").appendNumber(i);
            plan.packets.push_back({index, {}, {}});
            if (authenticated.getName() == root) plan.indexDataNames.push_back(index);
          }
          plan.maxWireBytes = spec.maxBytes;
          plan.maxPackets = spec.maxPackets;
          return plan;
        }
        const bool nested = mode == AdmissionCase::MultiIndex || mode == AdmissionCase::IndexCycle;
        if ((!nested && authenticated.getName() != root) || authenticated.getContent().value_size() != 1 ||
            (authenticated.getName() == root ? authenticated.getContent().value()[0] != 2 :
             authenticated.getName() != a || authenticated.getContent().value()[0] != 3))
          throw std::runtime_error("bad finite index");
        DependencyPacketPlan plan;
        plan.rootName = root;
        plan.producer = producer;
        plan.packets = {{root, {}, {}}, {a, {}, {}}, {b, {}, {}}};
        plan.maxWireBytes = spec.maxBytes;
        plan.maxPlaintextBytes = spec.maxBytes;
        plan.maxPackets = spec.maxPackets;
        if (nested) {
          if (authenticated.getName() == root) {
            plan.packets = {{root, {}, {}}};
            plan.indexDataNames = {a};
          }
          else {
            plan.packets = {{a, {}, {}}, {b, {}, {}}};
            if (mode == AdmissionCase::IndexCycle) plan.indexDataNames = {root};
          }
        }
        return plan;
      };
      auto escaped = std::make_shared<std::vector<VerifiedInputObject>>();
      auto validate = [a, b, observed, mode, escaped, immutablePayload](const std::vector<VerifiedInputObject>& objects) {
        ++observed->validators;
        if (objects.size() != 1) throw std::runtime_error("incomplete object set");
        if (mode == AdmissionCase::DomainRejected) throw std::runtime_error("private domain diagnostic");
        if (mode == AdmissionCase::CancelledDuringValidation || mode == AdmissionCase::RegistrationClosedDuringValidation) {
          *escaped = objects;
          const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
          while (!observed->releaseValidator.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (mode == AdmissionCase::GenericLarge || mode == AdmissionCase::ReusableLarge ||
            mode == AdmissionCase::ImmutableGeneric || mode == AdmissionCase::CanonicalManifest) {
          const auto& expected = mode == AdmissionCase::CanonicalManifest ? ndn::Buffer{3, 7} : *immutablePayload;
          if (objects[0].content() != expected) throw std::runtime_error("incorrect plaintext");
          return;
        }
        bool first = false, second = false;
        for (const auto& packet : objects[0].packets()) {
          first |= packet->getName() == a && packet->getContent().value_size() == 1 && packet->getContent().value()[0] == 3;
          second |= packet->getName() == b && packet->getContent().value_size() == 1 && packet->getContent().value()[0] == 7;
        }
        if (!first || !second) throw std::runtime_error("missing finite source");
      };
      if (mode == AdmissionCase::ScopeCredentialMismatch) {
        auto read = context.startVerifiedSignedExactDataBatch("input", {root, a, b}, producer, 300,
          {}, {}, ServiceProvider::ExactDataFetchPhase::AwaitPublication, {}, 16, 4096);
        auto verified = read.get();
        if (!verified) throw std::runtime_error("fixture scoped read failed");
        const auto before = observed->rootInterests.load();
        input.keyScope = "other";
        const auto result = context.admitResolvedInputs({input}, {verified->object}, validate);
        observed->lifetimeChecks = result.status == InputAdmissionStatus::Rejected &&
          observed->rootInterests.load() == before;
        observed->reason = result.reasonCode;
        context.publishFinalFailureResponse({}, result.reasonCode);
        return;
      }
      if (mode == AdmissionCase::ConcurrentIndependentObject) {
        auto first = context.startInputAdmission({input}, validate, resolver);
        auto second = context.startInputAdmission({input}, validate, resolver);
        auto independent = input;
        independent.slotId = "independent-object";
        independent.object.name = ndn::Name(producer).append("NDNSF").append("CONTROL-PROGRESS").appendVersion(1);
        independent.maxPackets = 1;
        auto independentResolver = [](const CollaborationInputSpec& spec, const ndn::Data& root) {
          if (root.getName() != spec.object.name || root.getContent().value_size() != 1 ||
              root.getContent().value()[0] != 9) throw std::runtime_error("wrong independent root");
          DependencyPacketPlan plan;
          plan.rootName = spec.object.name;
          plan.producer = spec.object.producerName;
          plan.packets = {{spec.object.name, {}, {}}};
          plan.maxPackets = 1;
          plan.maxWireBytes = spec.maxBytes;
          return plan;
        };
        auto independentValidator = [observed, name = independent.object.name](const auto& objects) {
          ++observed->validators;
          if (objects.size() != 1 || objects.front().name() != name || objects.front().packets().size() != 1 ||
              objects.front().packets().front()->getContent().value_size() != 1 ||
              objects.front().packets().front()->getContent().value()[0] != 9)
            throw std::runtime_error("independent input mismatch");
        };
        auto third = context.startInputAdmission({independent}, independentValidator, independentResolver);
        auto result = third.get();
        first.cancel();
        second.cancel();
        const auto firstResult = first.get();
        const auto secondResult = second.get();
        observed->lifetimeChecks = result.status == InputAdmissionStatus::Ready &&
          firstResult.status == InputAdmissionStatus::Cancelled && secondResult.status == InputAdmissionStatus::Cancelled &&
          result.inputs && result.inputs->isCurrent();
        if (result.status == InputAdmissionStatus::Ready) {
          ++observed->computes;
          context.publishFinalResponse(ndn::Buffer{10});
        }
        else context.publishFinalFailureResponse({}, result.reasonCode);
        return;
      }
      if (mode == AdmissionCase::ImmutableGeneric) observed->trackKeyReads = true;
      auto operation = std::make_shared<InputAdmissionOperation>(context.startInputAdmission({input}, validate, resolver));
      std::atomic_store(&observed->operation, operation);
      if (mode == AdmissionCase::FaceBoundary) {
        auto checked = std::make_shared<std::promise<bool>>();
        auto checkedFuture = checked->get_future();
        boost::asio::post(face->getIoContext(), [operation, checked, &context, validate] {
          bool getRejected = false, waitRejected = false;
          try { operation->get(); } catch (const std::logic_error&) { getRejected = true; }
          try { operation->wait_for(std::chrono::milliseconds(1)); }
          catch (const std::logic_error&) { waitRejected = true; }
          const auto resolved = context.admitResolvedInputs({}, {}, validate);
          checked->set_value(getRejected && waitRejected && resolved.status == InputAdmissionStatus::Rejected &&
            resolved.reasonCode == "INPUT_ADMISSION_REQUIRES_WORKER");
        });
        observed->lifetimeChecks = checkedFuture.get();
      }
      if (mode == AdmissionCase::Cancelled) operation->cancel();
      auto result = operation->get();
      observed->inBandRecovered = result.metrics.fecRecoveredPackets;
      observed->inBandRepairInterests = result.metrics.repairInterests;
      observed->inBandRecoveryAttempts = result.metrics.fecRecoveryAttempts;
      observed->inBandRejectedRepairs = result.metrics.fecRejectedRepairs;
      observed->inBandArqFallbacks = result.metrics.arqFallbacks;
      observed->inBandRootControlInterests = result.metrics.rootControlInterests;
      if (!result.metrics.recovery.empty())
        observed->inBandRecoveryTerminal = result.metrics.recovery.back().terminalState;
      observed->trackKeyReads = false;
      if (mode == AdmissionCase::ImmutableGeneric)
        observed->keyControlInterests = result.metrics.rootControlInterests;
      if (mode == AdmissionCase::OverlappingIndexBudget) {
        auto independent = input;
        independent.slotId = "after-projection-overload";
        independent.object.name = a;
        independent.maxPackets = 1;
        // Leave 17 KiB for the projection; a leaked original 16 KiB lease
        // leaves only 1 KiB and cannot accommodate this projection.
        independent.maxBytes = 239 * 1024;
        auto onePacket = [](const CollaborationInputSpec& spec, const ndn::Data&) {
          DependencyPacketPlan plan;
          plan.rootName = spec.object.name;
          plan.producer = spec.object.producerName;
          plan.packets = {{spec.object.name, {}, {}}};
          plan.maxPackets = 1;
          plan.maxWireBytes = spec.maxBytes;
          return plan;
        };
        auto checkOne = [a](const std::vector<VerifiedInputObject>& objects) {
          if (objects.size() != 1 || objects.front().packets().size() != 1 ||
              objects.front().packets().front()->getName() != a)
            throw std::runtime_error("post-overload independent object mismatch");
        };
        const auto resumed = context.startInputAdmission({independent}, checkOne, onePacket).get();
        observed->lifetimeChecks = resumed.status == InputAdmissionStatus::Ready;
        observed->postOverloadReason = resumed.reasonCode;
      }
      if (mode == AdmissionCase::Complete) {
        operation->cancel();
        observed->lifetimeChecks = result.inputs && result.inputs->isCurrent();
      }
      if (mode == AdmissionCase::RetainedCopy && result.inputs) {
        std::optional<VerifiedInputObject> retained = result.inputs->objects().front();
        result.inputs.reset();
        auto blocked = context.startInputAdmission({input}, validate, resolver).get();
        retained.reset();
        auto resumed = context.startInputAdmission({input}, validate, resolver).get();
        observed->lifetimeChecks = blocked.status == InputAdmissionStatus::Overloaded &&
          resumed.status == InputAdmissionStatus::Ready;
        result = std::move(resumed);
      }
      if (mode == AdmissionCase::CancelledDuringValidation || mode == AdmissionCase::RegistrationClosedDuringValidation) {
        auto reused = context.admitResolvedInputs({input}, *escaped, validate);
        observed->lifetimeChecks = result.status == (mode == AdmissionCase::CancelledDuringValidation ?
            InputAdmissionStatus::Cancelled : InputAdmissionStatus::Rejected) &&
          reused.status == InputAdmissionStatus::Rejected;
      }
      observed->reason = result.reasonCode;
      if (result.status == InputAdmissionStatus::Ready) {
        ++observed->computes;
        context.publishFinalResponse(ndn::Buffer{10});
      }
      else context.publishFinalFailureResponse(ndn::Buffer{}, result.reasonCode);
    }, limits);
  auto selector = std::make_shared<AdmissionSelector>();
  selector->source = source.getName();
  environment.user().BeginCollaborationWithProviders(profile.serviceName, {}, 1000, 5000,
    [selector, observed, mode, inBandXor, service = profile.serviceName, user = &environment.user()]
    (const CollaborationAckClosure& closed) {
      observed->closedCandidates = closed.candidates.size();
      CollaborationPlan plan;
      if (inBandXor) plan.dependencyTransport.redundancy = DependencyRedundancy::Xor;
      plan.ackCollectionTimeMs = 1000;
      plan.timeoutMs = 5000;
      for (const auto& name : {"/source", "/consumer"}) {
        CollaborationRoleSpec role;
        role.role = name;
        role.service = service;
        if (mode != AdmissionCase::LegacyReplay) role.inputAdmissionProfile = "data-admission-v1";
        role.terminalResponseOwner = role.role == "/consumer";
        plan.roles.push_back(std::move(role));
      }
      plan.keyScopes.push_back({"input", {"/source", "/consumer"}});
      if (mode == AdmissionCase::ScopeCredentialMismatch) plan.keyScopes.push_back({"other", {"/consumer"}});
      plan.participantSelector = selector;
      observed->committed = user->CommitCollaborationPlan(closed.requestId, closed.digest, std::move(plan));
    },
    [observed](const ResponseMessage& response) {
      observed->successful = response.getStatus() && response.getPayload() == ndn::Buffer{10};
      observed->terminalReason = response.getErrorInfo();
      observed->completed = true;
    },
    [observed](const ndn::Name&) { observed->timedOut = true; },
    {}, [first = source.getName(), second = consumer.getName()](const std::vector<AckCandidate>& candidates) {
      const auto present = [&](const ndn::Name& name) {
        return std::any_of(candidates.begin(), candidates.end(), [&](const auto& candidate) {
          return candidate.providerName == name && candidate.ack.getStatus();
        });
      };
      return present(first) && present(second);
    }, {}, std::nullopt, {}, {}, {}, {source.getName(), consumer.getName()});
  if (mode == AdmissionCase::CancelledDuringValidation || mode == AdmissionCase::RegistrationClosedDuringValidation) {
    environment.pumpUntil([&] { return (observed->validators.load() && std::atomic_load(&observed->operation)) ||
      observed->completed || observed->timedOut; });
    auto operation = std::atomic_load(&observed->operation);
    BOOST_REQUIRE(operation);
    if (mode == AdmissionCase::CancelledDuringValidation) operation->cancel();
    else consumerRegistration.close();
    observed->releaseValidator = true;
  }
  const auto terminalDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
  do {
    environment.pumpUntil([&] { return observed->completed || observed->timedOut; });
  } while (!observed->completed && !observed->timedOut && std::chrono::steady_clock::now() < terminalDeadline);
  BOOST_REQUIRE_MESSAGE(observed->committed, "ACK_CLOSED candidates=" << observed->closedCandidates.load());
  BOOST_REQUIRE_MESSAGE(observed->completed, "no terminal response; reason=" << observed->terminalReason);
  BOOST_CHECK(!observed->timedOut);
  if (inBandXor) {
    BOOST_CHECK_GT(observed->inBandDrops.load(), 0);
    BOOST_CHECK_EQUAL(observed->inBandRecovered.load(), 1);
    if (mode == AdmissionCase::GenericLarge || mode == AdmissionCase::ReusableLarge)
      BOOST_CHECK_EQUAL(observed->inBandRepairInterests.load(), 1);
  }
  if (mode == AdmissionCase::ImmutableGeneric) {
    BOOST_CHECK_GE(observed->keyInterests.load(), 2);
    // Every outbound wrapper/CK Interest must belong to the Core owner. A
    // direct SDK acquisition would not be included in these control metrics.
    // The single content segment zero is also a control read.
    BOOST_CHECK_EQUAL(observed->keyControlInterests.load(), observed->keyInterests.load() + 1);
  }
  const bool ready = mode == AdmissionCase::Complete || mode == AdmissionCase::GenericLarge ||
    mode == AdmissionCase::ReusableLarge ||
    mode == AdmissionCase::RetainedCopy || mode == AdmissionCase::ImmutableGeneric ||
    mode == AdmissionCase::ConcurrentIndependentObject || mode == AdmissionCase::FaceBoundary ||
    mode == AdmissionCase::CanonicalManifest || mode == AdmissionCase::LegacyReplay ||
    mode == AdmissionCase::MultiIndex ||
    mode == AdmissionCase::IndexDepthFour;
  BOOST_CHECK_MESSAGE(observed->computes.load() == (ready ? 1 : 0),
    "input admission reason=" << observed->reason << " terminal=" << observed->terminalReason
    << " recoveryAttempts=" << observed->inBandRecoveryAttempts
    << " repairInterests=" << observed->inBandRepairInterests.load()
    << " rejectedRepairs=" << observed->inBandRejectedRepairs
    << " arqFallbacks=" << observed->inBandArqFallbacks
    << " rootControlInterests=" << observed->inBandRootControlInterests
    << " recoveryTerminal=" << observed->inBandRecoveryTerminal);
  const size_t validators = mode == AdmissionCase::LegacyReplay ? 0 : mode == AdmissionCase::RetainedCopy ? 2 :
    (ready || mode == AdmissionCase::DomainRejected || mode == AdmissionCase::CancelledDuringValidation ||
     mode == AdmissionCase::RegistrationClosedDuringValidation ? 1 : 0);
  BOOST_CHECK_EQUAL(observed->validators.load(), validators);
  BOOST_CHECK_EQUAL(observed->successful.load(), ready);
  if (mode == AdmissionCase::DomainRejected) BOOST_CHECK_EQUAL(observed->reason, "INPUT_DOMAIN_REJECTED");
  if (mode == AdmissionCase::WrongDigest) BOOST_CHECK_EQUAL(observed->reason, "INPUT_DIGEST_MISMATCH");
  if (mode == AdmissionCase::IndexCycle) BOOST_CHECK_EQUAL(observed->reason, "INPUT_PACKET_PLAN_INVALID");
  if (mode == AdmissionCase::IndexDepthFive) BOOST_CHECK_EQUAL(observed->reason, "INPUT_PACKET_PLAN_INVALID");
  if (mode == AdmissionCase::IndexDepthFour) BOOST_CHECK_EQUAL(observed->resolvers.load(), 5);
  if (mode == AdmissionCase::IndexDepthFive) {
    BOOST_CHECK_EQUAL(observed->resolvers.load(), 5);
    BOOST_CHECK_EQUAL(observed->forbiddenIndexInterests.load(), 0);
  }
  if (mode == AdmissionCase::OverlappingIndexBudget) {
    BOOST_CHECK_EQUAL(observed->reason, "INPUT_OVERLOADED");
    BOOST_CHECK_GT(observed->resolvers.load(), 1);
    BOOST_CHECK_MESSAGE(observed->lifetimeChecks.load(), "post-overload admission: " << observed->postOverloadReason);
  }
  if (mode == AdmissionCase::WrongRole) BOOST_CHECK_EQUAL(observed->reason, "INPUT_PRODUCER_MISMATCH");
  if (mode == AdmissionCase::BadOriginalSignature) BOOST_CHECK_EQUAL(observed->reason, "INPUT_SIGNATURE_REJECTED");
  if (mode == AdmissionCase::ScopeCredentialMismatch) BOOST_CHECK_EQUAL(observed->reason, "INPUT_PROVENANCE_MISMATCH");
  if (mode == AdmissionCase::RegistrationClosedDuringValidation)
    BOOST_CHECK_EQUAL(observed->reason, "INPUT_AUTHORITY_EXPIRED");
  if (mode == AdmissionCase::Complete || mode == AdmissionCase::RetainedCopy || mode == AdmissionCase::CancelledDuringValidation ||
      mode == AdmissionCase::ConcurrentIndependentObject || mode == AdmissionCase::FaceBoundary ||
      mode == AdmissionCase::RegistrationClosedDuringValidation || mode == AdmissionCase::ScopeCredentialMismatch)
    BOOST_CHECK(observed->lifetimeChecks.load());
  if (mode == AdmissionCase::GenericBadTag || mode == AdmissionCase::GenericMetadataMismatch ||
      mode == AdmissionCase::BadOriginalSignature) {
    BOOST_CHECK_GT(observed->corruptedPackets.load(), 0);
    BOOST_CHECK(observed->corruptedSignaturesValid.load());
  }
  if (mode == AdmissionCase::InboxOverload || mode == AdmissionCase::ReplayQuota) {
    BOOST_CHECK_EQUAL(observed->callbacks.load(), 0);
    BOOST_CHECK_MESSAGE(observed->terminalReason.find("INPUT_OVERLOADED") != std::string::npos,
      "terminal reason=" << observed->terminalReason);
  }
  if (mode == AdmissionCase::LegacyReplay) {
    BOOST_CHECK_EQUAL(observed->callbacks.load(), 2);
    BOOST_CHECK(observed->lifetimeChecks.load());
  }
  if (mode == AdmissionCase::WrongScope || mode == AdmissionCase::Overloaded || mode == AdmissionCase::WrongRole)
    BOOST_CHECK_EQUAL(observed->rootInterests.load(), 0);
  source.setHandlerThreads(0);
  consumer.setHandlerThreads(0);
}

void runTwoSourceAdmissionCase(bool omitSecondSource, bool useRequireInputs = false,
                               bool duplicateSlot = false, bool rejectDomain = false)
{
  auto observed = std::make_shared<AdmissionObservations>();
  BootstrapProfile profile;
  profile.providerCount = 3;
  profile.providerRoles = {"/source", "/source", "/consumer"};
  profile.validateSvsPackets = true;
  profile.serviceName = ndn::Name("/spec226/admission");
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();
  environment.disconnectProviderPeerTransportForTest();

  auto& sourceA = environment.provider(0);
  auto& sourceB = environment.provider(1);
  auto& consumer = environment.provider(2);
  const auto rootA = ndn::Name(sourceA.getName()).append("NDNSF").append("INPUT")
    .appendVersion(1);
  const auto rootB = ndn::Name(sourceB.getName()).append("NDNSF").append("INPUT")
    .appendVersion(1);

  std::atomic<size_t> sourceAInterests{0};
  std::atomic<bool> sourceADelivered{false};
  std::atomic<bool> sourceBDelivered{false};
  std::atomic<bool> reordered{false};
  ndn::signal::ScopedConnection interests = environment.providerFace(2).onSendInterest.connect(
    [&](const ndn::Interest& interest) {
      if (rootA.isPrefixOf(interest.getName())) {
        const auto number = sourceAInterests.fetch_add(1);
        if (!omitSecondSource && number == 0) {
          return; // Lose the first Interest; admission must retry the exact name.
        }
        environment.providerFace(0).receive(interest);
      }
      else if (rootB.isPrefixOf(interest.getName())) {
        environment.providerFace(1).receive(interest);
      }
    });
  ndn::signal::ScopedConnection sourceAPackets = environment.providerFace(0).onSendData.connect(
    [&](const ndn::Data& data) {
      if (!rootA.isPrefixOf(data.getName())) {
        return;
      }
      if (sourceBDelivered.load()) {
        reordered = true;
      }
      sourceADelivered = true;
      environment.providerFace(2).receive(data);
    });
  ndn::signal::ScopedConnection sourceBPackets = environment.providerFace(1).onSendData.connect(
    [&](const ndn::Data& data) {
      if (!rootB.isPrefixOf(data.getName())) {
        return;
      }
      if (!sourceADelivered.load()) {
        reordered = true;
      }
      sourceBDelivered = true;
      environment.providerFace(2).receive(data);
      if (useRequireInputs) {
        observed->duplicateDataDelivered = true;
        environment.providerFace(2).receive(data);
      }
    });

  const auto ack = [] (const RequestMessage&) {
    ServiceProvider::AckDecision decision;
    decision.status = true;
    decision.message = "finite-input-capable";
    return decision;
  };
  CollaborationAdmissionLimits limits;
  limits.maxInputAcquisitionMs = omitSecondSource ? 700 : 4000;
  auto sourceHandler = [] (const ndn::Name& root, uint8_t value, bool delay,
                           bool omit) {
    return [root, value, delay, omit](ServiceProvider::CollaborationContext& context,
                                     const RequestMessage&) {
      if (delay) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
      }
      if (!omit) {
        if (!context.publishSignedExactData(
              "input", {{root, ndn::Buffer{value}}})) {
          throw std::runtime_error("two-source fixture publication failed");
        }
      }
      context.completeRole();
    };
  };
  auto sourceARegistration = sourceA.addScopedCollaborationHandler(
    profile.serviceName, {"/source"}, ack,
    sourceHandler(rootA, 3, !omitSecondSource, false), limits);
  auto sourceBRegistration = sourceB.addScopedCollaborationHandler(
    profile.serviceName, {"/source"}, ack,
    sourceHandler(rootB, 7, false, omitSecondSource), limits);

  auto consumerRegistration = consumer.addScopedCollaborationHandler(
    profile.serviceName, {"/consumer"}, ack,
    [rootA, rootB, sourceAName = sourceA.getName(), sourceBName = sourceB.getName(),
     service = profile.serviceName, observed, useRequireInputs, duplicateSlot, rejectDomain]
    (ServiceProvider::CollaborationContext& context, const RequestMessage&) {
      const auto makeInput = [&service](const ndn::Name& root,
                                        const ndn::Name& producer,
                                        const std::string& slot) {
        CollaborationInputSpec input;
        input.slotId = slot;
        input.producerRole = "/source";
        input.object.name = root;
        input.object.producerName = producer;
        input.object.serviceName = service;
        input.object.segmentCount = 1;
        input.keyScope = "input";
        input.encoding = InputObjectEncoding::SignedExactPackets;
        input.digestKind = InputDigestKind::OrderedWireSha256;
        input.maxBytes = 4096;
        input.maxPackets = 1;
        return input;
      };
      std::vector<CollaborationInputSpec> inputs{
        makeInput(rootB, sourceBName, "source-b"),
        makeInput(rootA, sourceAName, "source-a"),
      };
      if (duplicateSlot) inputs[1].slotId = inputs[0].slotId;
      const auto resolve = [] (const CollaborationInputSpec& input,
                               const ndn::Data& authenticated) {
        if (authenticated.getName() != input.object.name) {
          throw std::runtime_error("two-source fixture root name mismatch");
        }
        DependencyPacketPlan plan;
        plan.rootName = input.object.name;
        plan.producer = input.object.producerName;
        plan.packets = {{input.object.name, {}, {}}};
        plan.maxWireBytes = input.maxBytes;
        plan.maxPackets = input.maxPackets;
        return plan;
      };
      const auto validate = [rootA, rootB, observed, rejectDomain](
          const std::vector<VerifiedInputObject>& objects) {
        ++observed->validators;
        if (objects.size() != 2) {
          throw std::runtime_error("two-source input set is incomplete");
        }
        const auto hasContent = [&objects] (const ndn::Name& name, uint8_t value) {
          return std::any_of(objects.begin(), objects.end(),
            [&name, value] (const VerifiedInputObject& object) {
              return object.name() == name && object.packets().size() == 1 &&
                object.packets().front()->getContent().value_size() == 1 &&
                object.packets().front()->getContent().value()[0] == value;
            });
        };
        if (!hasContent(rootA, 3) || !hasContent(rootB, 7)) {
          throw std::runtime_error("two-source packet set differs");
        }
        if (rejectDomain) throw std::runtime_error("two-source domain rejected");
      };
      if (useRequireInputs) {
        try {
          auto admitted = context.requireInputs(inputs, validate);
          const auto& sourceBInput = admitted.at("source-b");
          const auto& sourceAInput = admitted.at("source-a");
          observed->slotsCorrect = sourceBInput.name() == rootB &&
            sourceAInput.name() == rootA &&
            sourceBInput.packets().front()->getContent().value()[0] == 7 &&
            sourceAInput.packets().front()->getContent().value()[0] == 3;
          try {
            (void)admitted.at("missing-slot");
          }
          catch (const std::out_of_range&) {
            observed->unknownSlotRejected = true;
          }
          ++observed->computes;
          context.publishFinalResponse(ndn::Buffer{10});
        }
        catch (const std::runtime_error& error) {
          observed->reason = error.what();
          context.publishFinalFailureResponse({}, error.what());
        }
        return;
      }
      auto operation = context.startInputAdmission(inputs, validate, resolve);
      const auto result = operation.get();
      observed->reason = result.reasonCode;
      if (result.status == InputAdmissionStatus::Ready && result.inputs &&
          result.inputs->isCurrent()) {
        ++observed->computes;
        context.publishFinalResponse(ndn::Buffer{10});
      }
      else {
        context.publishFinalFailureResponse({}, result.reasonCode);
      }
    }, limits);

  auto selector = std::make_shared<AdmissionSelector>();
  selector->sources = {sourceA.getName(), sourceB.getName()};
  selector->consumer = consumer.getName();
  CollaborationPlan plan;
  plan.ackCollectionTimeMs = 1000;
  plan.timeoutMs = 12000;
  CollaborationRoleSpec sourceRole;
  sourceRole.role = "/source";
  sourceRole.service = profile.serviceName;
  sourceRole.minProviders = 2;
  sourceRole.maxProviders = 2;
  sourceRole.inputAdmissionProfile = "data-admission-v1";
  CollaborationRoleSpec consumerRole;
  consumerRole.role = "/consumer";
  consumerRole.service = profile.serviceName;
  consumerRole.terminalResponseOwner = true;
  consumerRole.inputAdmissionProfile = "data-admission-v1";
  plan.roles = {sourceRole, consumerRole};
  plan.keyScopes.push_back({"input", {"/source", "/consumer"}});
  plan.participantSelector = selector;

  auto& user = environment.user();
  std::atomic<size_t> responses{0};
  const auto requestId = user.BeginCollaborationWithProviders(
    profile.serviceName, ndn::Buffer{}, 1000, plan.timeoutMs,
    [&user, observed, plan = std::move(plan)](const CollaborationAckClosure& closed) mutable {
      observed->closedCandidates = closed.candidates.size();
      observed->committed = user.CommitCollaborationPlan(
        closed.requestId, closed.digest, std::move(plan));
    },
    [observed, &responses](const ResponseMessage& response) {
      ++responses;
      observed->successful = response.getStatus() &&
        response.getPayload() == ndn::Buffer{10};
      observed->terminalReason = response.getErrorInfo();
      observed->completed = true;
    },
    [observed](const ndn::Name&) { observed->timedOut = true; },
    {}, {}, RequestCapabilities(), std::nullopt, {}, {}, {},
    {sourceA.getName(), sourceB.getName(), consumer.getName()});
  BOOST_REQUIRE(!requestId.empty());

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
  while (!observed->completed && !observed->timedOut &&
         std::chrono::steady_clock::now() < deadline) {
    environment.pumpUntil([&] { return observed->completed || observed->timedOut; });
  }
  BOOST_REQUIRE_MESSAGE(observed->committed.load(),
                        "ACK_CLOSED candidates=" << observed->closedCandidates.load());
  BOOST_REQUIRE_MESSAGE(observed->completed,
                        "no terminal response; admission=" << observed->reason
                        << " response=" << observed->terminalReason);
  BOOST_CHECK(!observed->timedOut);
  BOOST_CHECK(observed->committed.load());
  BOOST_CHECK_EQUAL(responses.load(), 1);
  const bool expectSuccess = !omitSecondSource && !duplicateSlot && !rejectDomain;
  BOOST_CHECK_EQUAL(observed->computes.load(), expectSuccess ? 1 : 0);
  BOOST_CHECK_EQUAL(observed->successful.load(), expectSuccess);
  if (omitSecondSource || duplicateSlot) {
    BOOST_CHECK_EQUAL(observed->validators.load(), 0);
  }
  else {
    BOOST_CHECK_EQUAL(observed->validators.load(), 1);
    BOOST_CHECK_GE(sourceAInterests.load(), 2);
    BOOST_CHECK(reordered.load());
    if (useRequireInputs && !rejectDomain) {
      BOOST_CHECK(observed->slotsCorrect.load());
      BOOST_CHECK(observed->unknownSlotRejected.load());
      BOOST_CHECK(observed->duplicateDataDelivered.load());
    }
  }
  if (duplicateSlot) {
    BOOST_CHECK_EQUAL(observed->computes.load(), 0);
    BOOST_CHECK_EQUAL(observed->validators.load(), 0);
    BOOST_CHECK_EQUAL(sourceAInterests.load(), 0);
    BOOST_CHECK_EQUAL(observed->reason, "INPUT_SPEC_INVALID");
  }

  sourceA.setHandlerThreads(0);
  sourceB.setHandlerThreads(0);
  consumer.setHandlerThreads(0);
}
}

BOOST_AUTO_TEST_SUITE(Spec226InputAdmission)


BOOST_AUTO_TEST_CASE(CompleteFiniteDependencies) { runAdmissionCase(AdmissionCase::Complete); }
BOOST_AUTO_TEST_CASE(MissingDependencyNeverComputes) { runAdmissionCase(AdmissionCase::Missing); }
BOOST_AUTO_TEST_CASE(CancelledAdmissionNeverComputes) { runAdmissionCase(AdmissionCase::Cancelled); }
BOOST_AUTO_TEST_CASE(UngrantedScopeRejectedBeforeRead) { runAdmissionCase(AdmissionCase::WrongScope); }
BOOST_AUTO_TEST_CASE(AdmissionBudgetRejectedBeforeRead) { runAdmissionCase(AdmissionCase::Overloaded); }
BOOST_AUTO_TEST_CASE(ProtectedGenericObjectIsDecodedByCore) { runAdmissionCase(AdmissionCase::GenericLarge); }
BOOST_AUTO_TEST_CASE(ValidOriginalSignatureDoesNotReplaceAead) { runAdmissionCase(AdmissionCase::GenericBadTag); }
BOOST_AUTO_TEST_CASE(ProtectionMetadataMismatchNeverReachesDomain) { runAdmissionCase(AdmissionCase::GenericMetadataMismatch); }
BOOST_AUTO_TEST_CASE(DomainFailureDoesNotExposeExceptionAsProtocol) { runAdmissionCase(AdmissionCase::DomainRejected); }
BOOST_AUTO_TEST_CASE(CopiedCredentialRetainsAndReleasesAdmissionBudget) { runAdmissionCase(AdmissionCase::RetainedCopy); }
BOOST_AUTO_TEST_CASE(InBandCopiedCredentialRetainsAndReleasesObjectAdmissionBudget)
{ runAdmissionCase(AdmissionCase::RetainedCopy, true); }
BOOST_AUTO_TEST_CASE(CancellationDuringDomainValidationInvalidatesEscapedCredential) { runAdmissionCase(AdmissionCase::CancelledDuringValidation); }
BOOST_AUTO_TEST_CASE(InboxByteLimitProducesExplicitTerminalFailure) { runAdmissionCase(AdmissionCase::InboxOverload); }
BOOST_AUTO_TEST_CASE(HistoryReplayQuotaNeverFallsBackToInlineCallback) { runAdmissionCase(AdmissionCase::ReplayQuota); }
BOOST_AUTO_TEST_CASE(ImmutableObjectUsesOriginalNacAuthorizationAndDecoder) { runAdmissionCase(AdmissionCase::ImmutableGeneric); }
BOOST_AUTO_TEST_CASE(ImmutableInBandRecoveryUsesOriginalNacAuthorizationAndDecoder)
{ runAdmissionCase(AdmissionCase::ImmutableGeneric, true); }
BOOST_AUTO_TEST_CASE(SignedExactInBandRecoveryUsesThePublicCollaborationPublisher)
{ runAdmissionCase(AdmissionCase::Complete, true); }
BOOST_AUTO_TEST_CASE(GenericLargeInBandRecoveryUsesThePublicCollaborationPublisher)
{ runAdmissionCase(AdmissionCase::GenericLarge, true); }
BOOST_AUTO_TEST_CASE(ReusableCollaborationObjectUsesCoreGrantAndInBandXorRecovery)
{ runAdmissionCase(AdmissionCase::ReusableLarge, true); }
BOOST_AUTO_TEST_CASE(ConcurrentMissingInputsDoNotBlockIndependentCoreRead) { runAdmissionCase(AdmissionCase::ConcurrentIndependentObject); }
BOOST_AUTO_TEST_CASE(WrongWireDigestNeverEntersDomainValidator) { runAdmissionCase(AdmissionCase::WrongDigest); }
BOOST_AUTO_TEST_CASE(WrongProducerRoleRejectsBeforeNetworkRead) { runAdmissionCase(AdmissionCase::WrongRole); }
BOOST_AUTO_TEST_CASE(FaceExecutorCannotBlockOrRunDomainValidator) { runAdmissionCase(AdmissionCase::FaceBoundary); }
BOOST_AUTO_TEST_CASE(RegistrationClosureInvalidatesInputsBeforeCompute) { runAdmissionCase(AdmissionCase::RegistrationClosedDuringValidation); }
BOOST_AUTO_TEST_CASE(CanonicalManifestAdmitsAuthorizedObjectFromUnselectedProducer) { runAdmissionCase(AdmissionCase::CanonicalManifest); }
BOOST_AUTO_TEST_CASE(OriginalSignatureFailureHasStableAdmissionReason) { runAdmissionCase(AdmissionCase::BadOriginalSignature); }
BOOST_AUTO_TEST_CASE(ResolvedCredentialCannotChangeItsReadScope) { runAdmissionCase(AdmissionCase::ScopeCredentialMismatch); }
BOOST_AUTO_TEST_CASE(LegacyNotificationReplayStillCompletesOnWorker) { runAdmissionCase(AdmissionCase::LegacyReplay); }
BOOST_AUTO_TEST_CASE(AuthorizedLocationRotationUsesSelectedRoleAndPreservesProducerIdentity)
{
  runAuthorizedLocationRotationCase();
}
BOOST_AUTO_TEST_CASE(AuthorizedObjectDoesNotUseSelectedRoleFallback)
{
  runAuthorizedLocationRotationCase(true);
}
BOOST_AUTO_TEST_CASE(CoreExpandsAuthenticatedIndexesWithoutDomainIo) { runAdmissionCase(AdmissionCase::MultiIndex); }
BOOST_AUTO_TEST_CASE(IndexCycleIsRejectedBeforeDomainCompute) { runAdmissionCase(AdmissionCase::IndexCycle); }
BOOST_AUTO_TEST_CASE(OverlappingIndexProjectionsShareProviderByteBudget) { runAdmissionCase(AdmissionCase::OverlappingIndexBudget); }
BOOST_AUTO_TEST_CASE(FourIndexLevelsAreExpandedOnceEach) { runAdmissionCase(AdmissionCase::IndexDepthFour); }
BOOST_AUTO_TEST_CASE(FifthIndexLevelRejectsBeforeReadingOrParsing) { runAdmissionCase(AdmissionCase::IndexDepthFive); }
BOOST_AUTO_TEST_CASE(TwoSelectedProducersShareOneRoleAndAdmitOnceAfterRetry) { runTwoSourceAdmissionCase(false); }
BOOST_AUTO_TEST_CASE(MissingSelectedProducerInputNeverComputes) { runTwoSourceAdmissionCase(true); }
BOOST_AUTO_TEST_CASE(RequireInputsUsesBuiltInSinglePacketCodecAndSlots)
{ runTwoSourceAdmissionCase(false, true); }
BOOST_AUTO_TEST_CASE(RequireInputsRejectsMissingAndDuplicateSlots)
{
  runTwoSourceAdmissionCase(true, true);
  runTwoSourceAdmissionCase(false, true, true);
}
BOOST_AUTO_TEST_CASE(RequireInputsStopsAfterDomainRejection)
{ runTwoSourceAdmissionCase(false, true, false, true); }
BOOST_AUTO_TEST_SUITE_END()
} // namespace ndn_service_framework::test
