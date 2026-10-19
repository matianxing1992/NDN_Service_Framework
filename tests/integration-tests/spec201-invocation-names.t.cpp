#include "isolated-test-trust.hpp"
#include "ndnsf-integration-fixture.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include "ndn-service-framework/utils.hpp"

#include <boost/test/unit_test.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <cstdlib>

namespace {
ndn::Buffer makeFilledBuffer(size_t size, uint8_t value)
{
  ndn::Buffer result(size);
  std::fill(result.begin(), result.end(), value);
  return result;
}
}

namespace ndn_service_framework::test {
struct InvocationNamesOracle
{
  static size_t bindings(const MessageValidator& validator)
  { return validator.m_invocationSegmentSigners.size(); }
  static auto timestamp(const MessageValidator& validator, const ndn::Name& base)
  { return validator.m_invocationSegmentSigners.at(base).second; }
  static void expire(MessageValidator& validator, const ndn::Name& base)
  { validator.m_invocationSegmentSigners.at(base).second -= std::chrono::minutes(6); }
  static void fill(MessageValidator& validator, const ndn::Name& signer)
  {
    validator.m_invocationSegmentSigners.clear();
    for (size_t i = 0; i < 4096; ++i)
      validator.m_invocationSegmentSigners.emplace(ndn::Name("/capacity").appendNumber(i),
        std::make_pair(signer, std::chrono::steady_clock::now()));
  }
  static bool admitVerified(MessageValidator& validator, const ndn::Data& packet)
  { return validator.acceptValidatedInvocationSegmentSigner(packet); }
  static uint64_t ambiguousRequests(const ServiceProvider& provider)
  { return provider.m_ambiguousRequestContextRejections; }
  static bool verifyRequestEnvelope(ServiceUser& user, const ndn::Name& id, const ndn::Name& name,
                                    const ndn::span<const uint8_t>& wire)
  {
    const auto& pending = user.m_pendingCalls.at(id);
    HybridMessageEnvelope envelope;
    if (!envelope.WireDecode(ndn::Block(wire)) || envelope.getMessageType() != "REQUEST") return false;
    const auto key = user.m_hybridMessageCrypto.getOrCreateSendKey(pending.serviceName, user.identity,
      hybridAccessAttributeForName(name, pending.serviceName, user.identity), "REQUEST", user.m_hybridCryptoCounters);
    const auto aad = hybridAssociatedData(name, "REQUEST", id, pending.serviceName, user.identity,
                                         envelope.getKeyId(), envelope.getEpochId());
    ndn::Buffer plaintext;
    RequestMessage decoded;
    return hybridAesGcmDecrypt(key.key, envelope, aad, plaintext) &&
           decoded.WireDecode(ndn::Block(ndn::span<const uint8_t>(plaintext.data(), plaintext.size()))) &&
           decoded.getPayload() == pending.requestMessage.getPayload();
  }
  static void seed(ServiceUser& user, const ndn::Name& requestId, const ndn::Name& service,
                   const std::vector<ndn::Name>& providers, const RequestMessage& request,
                   ServiceUser::ResponseHandler response = {})
  {
    ServiceUser::PendingCall pending;
    pending.serviceName = service;
    pending.providers = providers;
    pending.expectedResponseProviders = providers;
    pending.requestMessage = request;
    pending.requestName = makeRequestNameV2(user.identity, service, requestId);
    pending.requestNameWithoutPrefix = makeRequestNameWithoutPrefixV2(service, requestId);
    pending.timeoutMs = 5000;
    pending.responseHandler = std::move(response);
    user.m_pendingCalls.emplace(requestId, std::move(pending));
  }
  static void launch(ServiceUser& user, const ndn::Name& id)
  {
    auto& pending = user.m_pendingCalls.at(id);
    user.publishHybridMessage(pending.requestName, pending.requestNameWithoutPrefix, pending.requestMessage);
  }
  static void retry(ServiceUser& user, const ndn::Name& id, const ndn::Name& provider)
  { user.PublishServiceSelectionMessageV2(provider, user.m_pendingCalls.at(id).serviceName, id); }
  static const auto& packets(ServiceUser& user, const ndn::Name& id, const ndn::Name& provider)
  { return user.m_pendingCalls.at(id).selectionOriginalPacketsByProvider.at(provider.toUri()); }
  static const auto& prepared(ServiceUser& user, const ndn::Name& id, const ndn::Name& provider)
  { return user.m_pendingCalls.at(id).preparedSelectionsByProvider.at(provider.toUri()); }
  static RequestAckMessage offer(ServiceUser& user, const ndn::Name& id)
  { return user.m_pendingCalls.at(id).requestAcks.front().message; }
  static void restoreOffer(ServiceUser& user, const ndn::Name& id, const RequestAckMessage& offer)
  { user.m_pendingCalls.at(id).requestAcks.front().message = offer; }
  static void changeOffer(ServiceUser& user, const ndn::Name& id)
  {
    auto& ack = user.m_pendingCalls.at(id).requestAcks.front().message;
    SelectionInputKeyOffer changed = ack.hasSelectionInputKeyOffer() ? ack.getSelectionInputKeyOffer() : SelectionInputKeyOffer();
    changed.setField("providerBootId", "different-boot");
    changed.setField("recipientCertName", "/different/KEY/cert");
    ack.setSelectionInputKeyOffer(changed);
  }
  static void changeAttempt(ServiceUser& user, const ndn::Name& id)
  {
    auto& pending = user.m_pendingCalls.at(id);
    StreamRequestOptions options;
    options.generationId.fill(1);
    options.eventKeyCommitment.fill(2);
    options.streamEpoch = 1;
    options.attemptEpoch = 2;
    options.deadlineEpochMs = 2000000000000ULL;
    options.validate();
    pending.streamOptions = options;
  }
  static auto streamOptions(ServiceUser& user, const ndn::Name& id)
  { return user.m_pendingCalls.at(id).streamOptions; }
  static void restoreStreamOptions(ServiceUser& user, const ndn::Name& id,
                                    const std::optional<StreamRequestOptions>& options)
  { user.m_pendingCalls.at(id).streamOptions = options; }
  static void setLargeAssignment(ServiceUser& user, const ndn::Name& id, const ndn::Name& provider)
  {
    // Each opaque item is within the existing inline assignment bound;
    // their valid aggregate still exercises SVS message segmentation.
    std::vector<ndn::Buffer> items(6, makeFilledBuffer(3000, 'x'));
    user.m_pendingCalls.at(id).selectionAssignmentPayloads[provider.toUri()] =
      encodeOpaqueAssignmentSet(items);
  }
  static void setInvalidLargeAssignment(ServiceUser& user, const ndn::Name& id, const ndn::Name& provider)
  { user.m_pendingCalls.at(id).selectionAssignmentPayloads[provider.toUri()] = makeFilledBuffer(5000, 'x'); }
  static bool evaluatePreparationFailure(ServiceUser& user, const ndn::Name& id,
      const std::vector<ndn::Name>& providers, bool custom,
      ServiceUser::SelectionStatusTimeoutHandler timeout)
  {
    auto& pending = user.m_pendingCalls.at(id);
    pending.strategy = tlv::AllSelected;
    pending.trackSelectionStatus = true;
    pending.selectionStatusOptions.enabled = false;
    pending.statusTimeoutHandler = std::move(timeout);
    for (const auto& provider : providers) {
      ServiceUser::StoredAck ack;
      ack.providerName = provider;
      ack.serviceName = pending.serviceName;
      ack.requestId = id;
      ack.message.setStatus(true);
      ack.message.setMessage(provider.toUri(ndn::name::UriFormat::CANONICAL));
      pending.requestAcks.push_back(std::move(ack));
    }
    if (custom) pending.acksHandler = [](const std::vector<RequestAckMessage>& acks) { return acks; };
    return user.evaluateAckSelection(id);
  }
  static void queueSelectionThenFail(ServiceUser& user, const ndn::Name& id,
      const std::vector<ndn::Name>& providers, std::function<void(bool)> completion,
      ServiceUser::SelectionStatusTimeoutHandler timeout)
  {
    auto& pending = user.m_pendingCalls.at(id);
    pending.trackSelectionStatus = true;
    pending.selectionStatusOptions.enabled = false;
    pending.statusTimeoutHandler = std::move(timeout);
    const ndn::Name service = pending.serviceName;
    user.PublishServiceSelectionMessageV2(providers[0], service, id, std::move(completion));
    user.PublishServiceSelectionMessageV2(providers[1], service, id);
  }
  static void fanout(ServiceUser& user, const ndn::Name& id, const std::vector<ndn::Name>& providers,
                     ServiceUser::SelectionStatusTimeoutHandler timeout)
  {
    auto& pending = user.m_pendingCalls.at(id);
    pending.trackSelectionStatus = true;
    pending.selectionStatusOptions.enabled = false;
    pending.statusTimeoutHandler = std::move(timeout);
    std::vector<ServiceUser::StoredAck> acks;
    for (const auto& provider : providers) {
      ServiceUser::StoredAck ack;
      ack.providerName = provider;
      ack.serviceName = pending.serviceName;
      ack.requestId = id;
      ack.message.setStatus(true);
      acks.push_back(ack);
    }
    user.PublishCompactServiceSelectionMessageV2(acks);
  }
  static uint64_t committed(ServiceUser& user, const ndn::Name& id)
  { return user.m_pendingCalls.at(id).selectionPublishedAtUs; }
  static auto publisher(ServiceUser& user) { return user.m_svsps; }
  static void select(ServiceUser& user, const ndn::Name& id, const ndn::Name& provider,
                     std::function<void(bool)> completion)
  { user.PublishServiceSelectionMessageV2(provider, user.m_pendingCalls.at(id).serviceName,
                                        id, std::move(completion)); }
  static void invalidate(ServiceUser& user, const ndn::Name& id, int kind)
  {
    if (kind == 1) user.m_pendingCalls.erase(id);
    else if (kind == 2) user.m_pendingCalls.at(id).timedOut = true;
    else if (kind == 3) changeAttempt(user, id);
    else if (kind == 5) {
      auto version = user.getControllerVersion().value_or(ControllerVersion{});
      ++version.controllerGenerationTimestamp;
      ++version.controllerEpoch;
      user.adoptControllerVersion(version);
    }
    else if (kind == 6) user.m_controllerPrefix = ndn::Name("/configured/controller/without-status");
  }
};
} // namespace ndn_service_framework::test

namespace {
using namespace ndn_service_framework;
using test::InvocationNamesOracle;

struct EnvValue
{
  std::string key;
  std::optional<std::string> old;
  EnvValue(std::string name, const char* value) : key(std::move(name))
  {
    if (const auto* previous = std::getenv(key.c_str())) old = previous;
    ::setenv(key.c_str(), value, 1);
  }
  ~EnvValue() { if (old) ::setenv(key.c_str(), old->c_str(), 1); else ::unsetenv(key.c_str()); }
};

test::BootstrapProfile profile()
{
  test::BootstrapProfile result;
  result.validateSvsPackets = true;
  result.userIdentity = ndn::Name("/org/NDNSF/REQUEST/user");
  result.providerIdentity = ndn::Name("/org/NDNSF/ACK/provider");
  result.userNode = ndn::Name(result.userIdentity).append("user");
  result.providerNode = ndn::Name(result.providerIdentity).append("provider");
  result.serviceName = ndn::Name("/service/NDNSF/SELECTION/REQUEST/model");
  return result;
}

BOOST_AUTO_TEST_SUITE(Spec201InvocationNames)

BOOST_AUTO_TEST_CASE(SelectionWaitsForEarlierAsyncPublicationAndRechecksOwner)
{
  EnvValue async("NDNSF_SVS_ASYNC_PUBLISH", "1");
  for (int invalidation = 0; invalidation != 7; ++invalidation) {
    auto config = profile();
    bool released = false, completed = false, accepted = false;
    size_t retries = 0, callbacks = 0;
    test::NdnsfIntegrationEnvironment environment(config);
    environment.bootstrap();
    environment.enableProductionIngressForTest();
    auto& user = environment.user();
    user.setUseTokens(false);
    const auto publisher = InvocationNamesOracle::publisher(user);
    BOOST_REQUIRE(publisher);
    publisher->setPreparedPublicationCommitHookForTest([&](ndn::svs::SeqNo) {
      if (!released) {
        ++retries;
        throw std::runtime_error("injected earlier async commit retry");
      }
    });
    const ndn::Name id("/selection/awaiting/async");
    RequestMessage request;
    request.setPolicyEpoch(user.getCurrentPolicyEpoch(config.serviceName));
    InvocationNamesOracle::seed(user, id, config.serviceName, {config.providerIdentity}, request);
    const auto earlier = ndn::Name(config.userIdentity).append("NDNSF").append("status");
    const ndn::Buffer bytes{1, 2, 3};
    publisher->publishAsync(earlier, ndn::span<const uint8_t>(bytes.data(), bytes.size()));
    BOOST_REQUIRE_GT(publisher->getPublicationPreparationStats().outstanding, 0U);
    const auto encryptions = user.getHybridCryptoCounters().symmetric_encrypt_count.load();
    InvocationNamesOracle::select(user, id, config.providerIdentity, [&](bool ok) {
      ++callbacks;
      accepted = ok;
      completed = true;
      BOOST_CHECK(released);
    });
    ndn::Scheduler scheduler(environment.userFace().getIoContext());
    std::function<void()> invalidateAfterFence;
    if (invalidation == 4) {
      // The User Face is manually pumped: invalidate before its first posted
      // publication continuation, after the real Selection preparation call.
      InvocationNamesOracle::changeAttempt(user, id);
      released = true;
    }
    else {
      invalidateAfterFence = [&] {
        if (user.getHybridCryptoCounters().symmetric_encrypt_count.load() == encryptions) {
          scheduler.schedule(ndn::time::milliseconds(1), invalidateAfterFence);
          return;
        }
        // Only this Selection uses User hybrid encryption after the baseline.
        // Its Face continuation has run and the blocked predecessor prevents
        // acceptance; no wall-time guess determines owner invalidation.
        BOOST_CHECK(!completed);
        InvocationNamesOracle::invalidate(user, id, invalidation);
        released = true;
      };
      scheduler.schedule(ndn::time::milliseconds(1), invalidateAfterFence);
    }
    environment.pumpUntilWithAttributeAuthority([&] {
      return completed && publisher->getPublicationPreparationStats().outstanding == 0;
    });
    if (invalidation != 4) BOOST_CHECK_GT(retries, 0U);
    BOOST_REQUIRE(completed);
    BOOST_CHECK_EQUAL(callbacks, 1U);
    BOOST_CHECK_EQUAL(accepted, invalidation == 0);
    BOOST_CHECK_EQUAL(publisher->getPublicationPreparationStats().outstanding, 0U);
    if (invalidation == 0) {
      const auto& originals = InvocationNamesOracle::packets(user, id, config.providerIdentity);
      BOOST_CHECK(!originals.empty());
    }
    publisher->setPreparedPublicationCommitHookForTest({});
  }
}

BOOST_AUTO_TEST_CASE(ContextKeepsFullActorServiceAndTypedRequestIdentity)
{
  const ndn::Name user("/org/NDNSF/REQUEST/User"), provider("/org/NDNSF/ACK/Provider");
  const ndn::Name service("/REQUEST/NDNSF/SELECTION/model"), id("/arbitrary/a/tail");
  auto typedId = id;
  typedId.appendVersion(73).appendSegment(4);
  const auto request = makeRequestNameV2(user, service, typedId);
  const auto ack = makeRequestAckNameV2(provider, user, service, typedId);
  const auto selection = makeServiceSelectionNameV2(user, provider, service, typedId);
  const auto response = makeResponseNameV2(provider, user, service, typedId);
  const auto encodedUser = ndn::name::Component(user.toUri(ndn::name::UriFormat::CANONICAL));
  const auto encodedProvider = ndn::name::Component(provider.toUri(ndn::name::UriFormat::CANONICAL));
  const auto paperRequest = ndn::Name(user).append("NDNSF").append("REQUEST").append(service).append(typedId);
  const auto paperAck = ndn::Name(provider).append("NDNSF").append("ACK").append(encodedUser).append(service).append(typedId);
  const auto paperSelection = ndn::Name(user).append("NDNSF").append("SELECTION").append(encodedProvider).append(service).append(typedId);
  const auto paperResponse = ndn::Name(provider).append("NDNSF").append("RESPONSE").append(encodedUser).append(service).append(typedId);
  BOOST_CHECK(request.wireEncode() == paperRequest.wireEncode());
  BOOST_CHECK(ack.wireEncode() == paperAck.wireEncode());
  BOOST_CHECK(selection.wireEncode() == paperSelection.wireEncode());
  BOOST_CHECK(response.wireEncode() == paperResponse.wireEncode());
  const auto parsedRequest = parseRequestNameV2(request, user, service);
  const auto parsedAck = parseRequestAckNameV2(ack, provider, service);
  const auto parsedSelection = parseServiceSelectionNameV2(selection, user, service);
  const auto parsedResponse = parseResponseNameV2(response, provider, service);
  BOOST_REQUIRE(parsedRequest && parsedAck && parsedSelection && parsedResponse);
  BOOST_CHECK(parsedRequest->requestId == typedId);
  BOOST_CHECK(parsedAck->requestId == typedId);
  BOOST_CHECK(parsedSelection->requestId == typedId);
  BOOST_CHECK(parsedResponse->requestId == typedId);
  BOOST_CHECK(parsedAck->requesterName == user);
  BOOST_CHECK(parsedSelection->providerName == provider);
  BOOST_CHECK_EQUAL(hybridMessageTypeForName(request, user), "REQUEST");
  BOOST_CHECK_EQUAL(hybridMessageTypeForName(ack, provider), "ACK");
  BOOST_CHECK_EQUAL(hybridMessageTypeForName(selection, user), "SELECTION");
  BOOST_CHECK_EQUAL(hybridMessageTypeForName(response, provider), "RESPONSE");
  BOOST_CHECK(!parseRequestNameV2(request, ndn::Name("/org"), service));
  BOOST_CHECK(!parseRequestNameV2(request, user, ndn::Name("/wrong")));
  BOOST_CHECK(!parseRequestAckNameV2(ack, user, service));
  const auto recipient = ndn::name::Component(user.toUri(ndn::name::UriFormat::CANONICAL));
  BOOST_CHECK(ack[provider.size() + 2] == recipient);
  auto alias = makeRequestAckNameV2(provider, user, service, id);
  alias.set(provider.size() + 2, ndn::name::Component(user.toUri(ndn::name::UriFormat::ALTERNATE)));
  BOOST_CHECK(!parseRequestAckNameV2(alias, provider, service));
  const auto decision = makeServiceSelectionDecisionNameV2(user, provider, service, typedId, 9);
  const auto parsedDecision = parseServiceSelectionDecisionNameV2(decision, user, service);
  BOOST_REQUIRE(parsedDecision);
  BOOST_CHECK(parsedDecision->requestId == typedId);
  BOOST_CHECK_EQUAL(parsedDecision->attempt, 9);
  BOOST_CHECK(!parseServiceSelectionDecisionNameV2(selection, user, service));
  BOOST_CHECK(!parseSvsProducerActor(ndn::Name(user).append("provider").append("3"), "user"));
  BOOST_REQUIRE(parseSvsProducerActor(ndn::Name(user).append("user").append("3"), "user"));
  BOOST_CHECK(*parseSvsProducerActor(ndn::Name(user).append("user").append("3"), "user") == user);
}

BOOST_AUTO_TEST_CASE(AeadUsesExactContextAndExplicitUriProfile)
{
  const ndn::Name actor("/org/ACK/User"), service("/REQUEST/service"), id("/a/tail");
  const auto name = makeRequestNameV2(actor, service, id);
  const auto key = makeFilledBuffer(32, 7);
  const auto plaintext = makeFilledBuffer(127, 13);
  const auto aad = hybridAssociatedData(name, "REQUEST", id, service, actor, "key", "epoch");
  const auto encrypted = hybridAesGcmEncrypt(key, plaintext, aad);
  HybridMessageEnvelope envelope;
  envelope.setNonce(encrypted.nonce);
  envelope.setCipherText(encrypted.ciphertext);
  envelope.setAuthTag(encrypted.tag);
  ndn::Buffer decoded;
  BOOST_CHECK(hybridAesGcmDecrypt(key, envelope, aad, decoded));
  BOOST_CHECK(decoded == plaintext);
  for (const auto& altered : {
       hybridAssociatedData(name, "ACK", id, service, actor, "key", "epoch"),
       hybridAssociatedData(name, "REQUEST", ndn::Name("/b/tail"), service, actor, "key", "epoch"),
       hybridAssociatedData(name, "REQUEST", id, service, ndn::Name("/foreign"), "key", "epoch"),
       hybridAssociatedData(name, "REQUEST", id, service, actor, "key", "next")})
    BOOST_CHECK(!hybridAesGcmDecrypt(key, envelope, altered, decoded));
  const std::string expected = name.toUri(ndn::name::UriFormat::CANONICAL) + "|REQUEST|" +
    id.toUri(ndn::name::UriFormat::CANONICAL) + "|" + service.toUri(ndn::name::UriFormat::CANONICAL) +
    "|" + actor.toUri(ndn::name::UriFormat::CANONICAL) + "|" + hybridCompactKeyId("key") + "|epoch";
  BOOST_CHECK_EQUAL(std::string(reinterpret_cast<const char*>(aad.data()), aad.size()), expected);
  BOOST_CHECK_EQUAL(hybridAccessAttributeForName(name, service, actor), "/SERVICE/REQUEST/service");
}

BOOST_AUTO_TEST_CASE(ValidatedAssemblyBindsEverySignerAndExpiresWithoutEviction)
{
  ndn::KeyChain keys;
  const ndn::Name actor("/spec201/segment-owner"), foreign("/spec201/segment-owner/NDNSF/REQUEST");
  keys.createIdentity(actor);
  keys.createIdentity(foreign);
  ndn_service_framework::test::IsolatedTestTrust fixedTrust1{
    keys.getPib().getIdentity(actor).getDefaultKey().getDefaultCertificate(),
    keys.getPib().getIdentity(foreign).getDefaultKey().getDefaultCertificate()};
  const auto validatorOwner = std::make_shared<MessageValidator>(fixedTrust1.schemaPath());
  auto& validator = *validatorOwner;
  const auto base = makeRequestNameV2(actor, ndn::Name("/service"), ndn::Name("/a/tail")).appendVersion(0);
  auto packet = [&](const ndn::Name& signer, uint64_t segment) {
    ndn::Data data(ndn::Name(base).appendSegment(segment));
    data.setContent("part");
    data.setFinalBlock(ndn::name::Component::fromSegment(2));
    keys.sign(data, ndn::security::signingByIdentity(signer));
    BOOST_REQUIRE(ndn::security::verifySignature(data, keys.getPib().getIdentity(signer).getDefaultKey()));
    return data;
  };
  const auto first = packet(actor, 1); // The first validated packet need not be segment zero.
  bool accepted = false;
  validator.validate(first, [&](const ndn::Data&) { accepted = true; },
                     [](const ndn::Data&, const auto&) { BOOST_FAIL("valid first packet rejected"); });
  BOOST_REQUIRE(accepted);
  BOOST_CHECK_EQUAL(InvocationNamesOracle::bindings(validator), 1);
  const auto before = InvocationNamesOracle::timestamp(validator, base);
  const auto mixed = packet(foreign, 2);
  bool rejected = false;
  validator.validate(mixed, [](const ndn::Data&) { BOOST_FAIL("mixed signer accepted"); },
    [&](const ndn::Data&, const auto&) { rejected = true; });
  BOOST_CHECK(rejected);
  BOOST_CHECK(InvocationNamesOracle::timestamp(validator, base) == before);
  BOOST_CHECK(InvocationNamesOracle::admitVerified(validator, packet(actor, 0)));
  BOOST_CHECK(InvocationNamesOracle::admitVerified(validator, first));
  InvocationNamesOracle::expire(validator, base);
  BOOST_CHECK(InvocationNamesOracle::admitVerified(validator, first));
  BOOST_CHECK_EQUAL(InvocationNamesOracle::bindings(validator), 1);
  InvocationNamesOracle::fill(validator, actor);
  auto extra = packet(actor, 0);
  extra.setName(ndn::Name(base).append("new").appendVersion(1).appendSegment(0));
  keys.sign(extra, ndn::security::signingByIdentity(actor));
  BOOST_REQUIRE(ndn::security::verifySignature(extra, keys.getPib().getIdentity(actor).getDefaultKey()));
  BOOST_CHECK(!InvocationNamesOracle::admitVerified(validator, extra));
  BOOST_CHECK_EQUAL(InvocationNamesOracle::bindings(validator), 4096);
}

BOOST_AUTO_TEST_CASE(InvalidSignatureNeverRefreshesAssemblyBinding)
{
  ndn::KeyChain keys;
  const ndn::Name actor("/spec201/invalid-signature-owner");
  keys.createIdentity(actor);
  boost::asio::io_context io;
  ndn::DummyClientFace face(io, keys);
  ndn_service_framework::test::IsolatedTestTrust fixedTrust2{
    keys.getPib().getIdentity(actor).getDefaultKey().getDefaultCertificate()};
  auto validator = std::make_shared<MessageValidator>(fixedTrust2.schemaPath(), std::nullopt, &face);
  ndn::Data original(makeRequestNameV2(actor, ndn::Name("/service"), ndn::Name("/request"))
                       .appendVersion(0).appendSegment(0));
  original.setContent("valid");
  original.setFinalBlock(ndn::name::Component::fromSegment(0));
  keys.sign(original, ndn::security::signingByIdentity(actor));
  const auto validate = [&](const ndn::Data& data, bool expectedSuccess) {
    bool done = false;
    validator->validate(data,
      [&](const ndn::Data&) { BOOST_CHECK(expectedSuccess); done = true; },
      [&](const ndn::Data&, const auto&) { BOOST_CHECK(!expectedSuccess); done = true; });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done && std::chrono::steady_clock::now() < deadline) {
      io.poll();
      io.restart();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    BOOST_REQUIRE(done);
  };
  validate(original, true);
  const auto base = original.getName().getPrefix(-1);
  const auto before = InvocationNamesOracle::timestamp(*validator, base);
  auto invalid = original;
  invalid.setContent("changed without signature");
  validate(invalid, false);
  BOOST_CHECK(InvocationNamesOracle::timestamp(*validator, base) == before);
}

BOOST_AUTO_TEST_CASE(NestedAuthorizedMarkersAreAmbiguousAndFailClosed)
{
  const ndn::Name ancestor("/spec201/nested"), actor("/spec201/nested/NDNSF/REQUEST/actor");
  ndn::KeyChain keys;
  keys.createIdentity(actor);
  ndn::Data original(makeRequestNameV2(actor, ndn::Name("/service"), ndn::Name("/request"))
                       .appendVersion(0).appendSegment(0));
  original.setContent("valid");
  original.setFinalBlock(ndn::name::Component::fromSegment(0));
  keys.sign(original, ndn::security::signingByIdentity(actor));
  ndn_service_framework::test::IsolatedTestTrust fixedTrust3{
    keys.getPib().getIdentity(actor).getDefaultKey().getDefaultCertificate()};
  const std::filesystem::path trust(fixedTrust3.schemaPath());
  {
    std::ofstream permissions(trust.string() + ".producer-signers");
    permissions << "producer-signer\n{\n producer \"" << ancestor.toUri(ndn::name::UriFormat::CANONICAL)
                << "\"\n signer \"" << actor.toUri(ndn::name::UriFormat::CANONICAL) << "\"\n}\n";
  }
  const auto validatorOwner = std::make_shared<MessageValidator>(trust.string());
  auto& validator = *validatorOwner;
  bool rejected = false;
  validator.validate(original, [](const ndn::Data&) { BOOST_FAIL("ambiguous namespace accepted"); },
    [&](const ndn::Data&, const auto& error) {
      BOOST_CHECK_EQUAL(error.getCode(), ndn::security::ValidationError::POLICY_ERROR);
      rejected = true;
    });
  BOOST_CHECK(rejected);
  BOOST_CHECK_EQUAL(InvocationNamesOracle::bindings(validator), 0);
}

BOOST_AUTO_TEST_CASE(ProviderSpecificFanoutPreservesAcceptedStatusOnLaterFailure)
{
  auto config = profile();
  config.providerCount = 2;
  const ndn::Name id("/fanout/arbitrary/tail");
  const ndn::Name preparationId("/fanout/preparation/tail");
  const ndn::Name queuedId("/queued/preparation/tail");
  std::vector<ndn::Name> providers;
  size_t emitted = 0;
  std::vector<SelectionExecutionStatus> terminal;
  bool queuedCompletion = false;
  bool queuedAccepted = true;
  size_t selectionPackets = 0;
  ndn::Name queuedName;
  test::NdnsfIntegrationEnvironment environment(config);
  environment.bootstrap();
  auto& user = environment.user();
  user.setUseTokens(false);
  providers = {environment.provider(0).getName(), environment.provider(1).getName()};
  RequestMessage request;
  request.setPolicyEpoch(user.getCurrentPolicyEpoch(config.serviceName));
  InvocationNamesOracle::seed(user, id, config.serviceName, providers, request);
  user.setLocalPublicationHandler([&](const ndn::Name& name, const ndn::Buffer& wire) {
    const auto parsed = parseServiceSelectionNameV2(name, config.userIdentity, config.serviceName);
    BOOST_REQUIRE(parsed);
    BOOST_CHECK(parsed->providerName == providers[emitted]);
    BOOST_CHECK(parsed->requestId == id);
    ServiceSelectionMessage decoded;
    BOOST_REQUIRE(decoded.WireDecode(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size()))));
    BOOST_CHECK_EQUAL(decoded.getProviderEntries().size(), 1);
    BOOST_CHECK_EQUAL(InvocationNamesOracle::committed(user, id), 0);
    if (emitted++ == 1) throw std::runtime_error("injected second Provider publication failure");
  });
  InvocationNamesOracle::fanout(user, id, providers,
    [&](const ndn::Name& requestId, const std::vector<SelectionExecutionStatus>& statuses) {
      BOOST_CHECK(requestId == id);
      terminal = statuses;
    });
  environment.pumpUntil([&] { return !terminal.empty(); });
  BOOST_CHECK_EQUAL(emitted, 2);
  BOOST_REQUIRE_EQUAL(terminal.size(), 2);
  BOOST_CHECK(terminal[0].providerName == providers[0]);
  BOOST_CHECK(terminal[0].state == SelectionExecutionState::Unknown);
  BOOST_CHECK(!terminal[0].selectionDigest.empty());
  BOOST_CHECK(terminal[1].providerName == providers[1]);
  BOOST_CHECK(terminal[1].state == SelectionExecutionState::Failed);
  BOOST_CHECK(terminal[1].message.find("injected") != std::string::npos);

  InvocationNamesOracle::seed(user, preparationId, config.serviceName, providers, request);
  InvocationNamesOracle::setInvalidLargeAssignment(user, preparationId, providers[1]);
  emitted = 0;
  terminal.clear();
  user.setLocalPublicationHandler([&](const ndn::Name& name, const ndn::Buffer&) {
    const auto parsed = parseServiceSelectionNameV2(name, config.userIdentity, config.serviceName);
    BOOST_REQUIRE(parsed);
    BOOST_CHECK(parsed->providerName == providers[0]);
    BOOST_CHECK(parsed->requestId == preparationId);
    ++emitted;
  });
  InvocationNamesOracle::fanout(user, preparationId, providers,
    [&](const ndn::Name& requestId, const std::vector<SelectionExecutionStatus>& statuses) {
      BOOST_CHECK(requestId == preparationId);
      terminal = statuses;
    });
  environment.pumpUntil([&] { return !terminal.empty(); });
  BOOST_CHECK_EQUAL(emitted, 1);
  BOOST_REQUIRE_EQUAL(terminal.size(), 2);
  BOOST_CHECK(terminal[0].providerName == providers[0]);
  BOOST_CHECK(terminal[0].state == SelectionExecutionState::Unknown);
  BOOST_CHECK(!terminal[0].selectionDigest.empty());
  BOOST_CHECK(terminal[1].providerName == providers[1]);
  BOOST_CHECK(terminal[1].state == SelectionExecutionState::Failed);
  BOOST_CHECK(terminal[1].message.find("structured envelope") != std::string::npos);

  for (const bool custom : {false, true}) {
    const ndn::Name evaluatorId(custom ? "/custom/preparation/tail" : "/builtin/preparation/tail");
    const std::vector<ndn::Name> candidates = custom ? providers : std::vector<ndn::Name>{providers[0]};
    InvocationNamesOracle::seed(user, evaluatorId, config.serviceName, candidates, request);
    InvocationNamesOracle::setInvalidLargeAssignment(user, evaluatorId, candidates.back());
    terminal.clear();
    emitted = 0;
    user.setLocalPublicationHandler([&, candidates, evaluatorId](const ndn::Name& name, const ndn::Buffer&) {
      const auto parsed = parseServiceSelectionNameV2(name, config.userIdentity, config.serviceName);
      BOOST_REQUIRE(parsed);
      BOOST_CHECK(parsed->providerName == candidates[0]);
      BOOST_CHECK(parsed->requestId == evaluatorId);
      ++emitted;
    });
    BOOST_CHECK(!InvocationNamesOracle::evaluatePreparationFailure(user, evaluatorId, candidates, custom,
      [&, evaluatorId](const ndn::Name& requestId, const std::vector<SelectionExecutionStatus>& statuses) {
        BOOST_CHECK(requestId == evaluatorId);
        terminal = statuses;
      }));
    environment.pumpUntil([&] { return !terminal.empty(); });
    BOOST_REQUIRE_EQUAL(terminal.size(), candidates.size());
    BOOST_CHECK_EQUAL(emitted, custom ? 1 : 0);
    if (custom) {
      BOOST_CHECK(terminal.front().state == SelectionExecutionState::Unknown);
      BOOST_CHECK(!terminal.front().selectionDigest.empty());
    }
    BOOST_CHECK(terminal.back().providerName == candidates.back());
    BOOST_CHECK(terminal.back().state == SelectionExecutionState::Failed);
    BOOST_CHECK(terminal.back().message.find("structured envelope") != std::string::npos);
    user.setLocalPublicationHandler({});
  }

  InvocationNamesOracle::seed(user, queuedId, config.serviceName, providers, request);
  InvocationNamesOracle::setInvalidLargeAssignment(user, queuedId, providers[1]);
  user.setLocalPublicationHandler({});
  environment.enableProductionIngressForTest();
  terminal.clear();
  queuedName = makeServiceSelectionNameV2(config.userIdentity, providers[0], config.serviceName, queuedId);
  ndn::signal::ScopedConnection queuedObserver(environment.userFace().onSendData.connect([&](const ndn::Data& data) {
    if (queuedName.isPrefixOf(data.getName())) ++selectionPackets;
  }));
  const auto beforeSequence = environment.userPubSub().getSVSync().getCore().getSeqNo();
  InvocationNamesOracle::queueSelectionThenFail(user, queuedId, providers,
    [&](bool accepted) { queuedAccepted = accepted; queuedCompletion = true; },
    [&](const ndn::Name& requestId, const std::vector<SelectionExecutionStatus>& statuses) {
      BOOST_CHECK(requestId == queuedId);
      terminal = statuses;
    });
  environment.pumpUntil([&] { return queuedCompletion && !terminal.empty(); });
  BOOST_CHECK(queuedCompletion);
  BOOST_CHECK(!queuedAccepted);
  BOOST_CHECK_EQUAL(selectionPackets, 0);
  BOOST_CHECK_EQUAL(environment.userPubSub().getSVSync().getCore().getSeqNo(), beforeSequence);
  bool failedPreparation = false;
  for (const auto& status : terminal) {
    BOOST_CHECK(status.state == SelectionExecutionState::Failed);
    if (status.providerName == providers[1] && status.message.find("structured envelope") != std::string::npos)
      failedPreparation = true;
  }
  BOOST_CHECK(failedPreparation);
}

BOOST_AUTO_TEST_CASE(AmbiguousRegisteredRequestServiceRejectsBeforeCryptoAndState)
{
  const ndn::Name id("/arbitrary/tail");
  for (const bool ambiguous : {true, false}) {
    auto config = profile();
    config.serviceName = ndn::Name("/service/long");
    config.providerAdditionalServices = {ndn::Name("/service")};
    config.userAdditionalServices = {ndn::Name("/service")};
    bool observed = false;
    std::atomic<size_t> executions{0};
    std::atomic<bool> completed{false};
    test::NdnsfIntegrationEnvironment environment(config);
    environment.bootstrap();
    auto& user = environment.user();
    auto& provider = environment.provider();
    user.setUseTokens(false);
    provider.setUseTokens(false);
    const auto handler = ServiceProvider::RequestHandler([&](const auto&, const auto&, const auto&, const auto&, const auto&) {
      ++executions;
      ResponseMessage response;
      response.setStatus(true);
      return response;
    });
    provider.addService(config.serviceName, handler);
    if (ambiguous) provider.addService(ndn::Name("/service"), handler);
    environment.enableProductionIngressForTest();
    RequestMessage request;
    request.setStrategy(tlv::FirstResponding);
    request.setPolicyEpoch(user.getCurrentPolicyEpoch(config.serviceName));
    auto payload = makeFilledBuffer(3, 13);
    request.setPayload(payload, payload.size());
    InvocationNamesOracle::seed(user, id, config.serviceName, {config.providerIdentity}, request,
      [&](const ResponseMessage& response) { BOOST_CHECK(response.getStatus()); completed = true; });
    const auto name = makeRequestNameV2(config.userIdentity, config.serviceName, id);
    const auto before = provider.getHybridCryptoCounters().symmetric_decrypt_count.load();
    environment.providerPubSub().subscribeToProducer(config.userNode,
      [&, name](const ndn::svs::SVSPubSub::SubscriptionData& publication) {
        if (publication.name != name) return;
        BOOST_CHECK(InvocationNamesOracle::verifyRequestEnvelope(user, id, name, publication.data));
        observed = true;
      }, true, false);
    InvocationNamesOracle::launch(user, id);
    if (ambiguous) {
      // The exact admission diagnostic is a deterministic Face boundary;
      // malformed ciphertext and unfinished decryption cannot satisfy it.
      environment.pumpUntilWithAttributeAuthority([&] {
        return observed && InvocationNamesOracle::ambiguousRequests(provider) != 0;
      });
      BOOST_REQUIRE(observed);
      BOOST_CHECK_GT(InvocationNamesOracle::ambiguousRequests(provider), 0);
      BOOST_CHECK_EQUAL(provider.getPendingRequestCountForTesting(), 0);
      BOOST_CHECK_EQUAL(provider.getHybridCryptoCounters().symmetric_decrypt_count.load(), before);
      BOOST_CHECK_EQUAL(executions.load(), 0);
    }
    else {
      environment.pumpUntilWithAttributeAuthority([&] { return observed && completed.load(); });
      BOOST_REQUIRE(observed);
      BOOST_CHECK(completed.load());
      BOOST_CHECK_EQUAL(executions.load(), 1);
      BOOST_CHECK_EQUAL(InvocationNamesOracle::ambiguousRequests(provider), 0);
      BOOST_CHECK_GT(provider.getHybridCryptoCounters().symmetric_decrypt_count.load(), before);
    }
  }
}

void runActualMessageChain(bool large, bool failDirectDelivery)
{
  EnvValue direct("NDNSF_SELECTION_TARGETED_PREFETCH", failDirectDelivery ? "1" : "0");
  auto config = profile();
  config.deferBridgeDelivery = true;
  auto id = ndn::Name(large ? "/large/arbitrary/tail" : "/small/arbitrary/tail");
  id.appendVersion(7).appendSegment(3);
  std::atomic<bool> executing{false}, released{false}, completed{false};
  std::mutex waitMutex;
  std::condition_variable waitCondition;
  const auto selectionName = makeServiceSelectionNameV2(config.userIdentity, config.providerIdentity,
                                                        config.serviceName, id);
  size_t directFailures = 0, observedOuterPackets = 0;
  bool observedAsyncDeliveryFailure = false;
  // Every worker-captured owner precedes the environment, which drains the
  // Provider pool before any captured state is destroyed on assertion failure.
  test::NdnsfIntegrationEnvironment environment(config);
  environment.bootstrap();
  auto& user = environment.user();
  auto& provider = environment.provider();
  user.setUseTokens(false);
  provider.setUseTokens(false);
  provider.addService(config.serviceName,
    ServiceProvider::RequestHandler([&](const ndn::Name& requester, const ndn::Name& producer,
        const ndn::Name& service, const ndn::Name& requestId, const RequestMessage& request) {
      BOOST_CHECK(requester == config.userIdentity);
      BOOST_CHECK(producer == config.providerIdentity);
      BOOST_CHECK(service == config.serviceName);
      BOOST_CHECK(requestId == id);
      BOOST_CHECK_EQUAL(request.getPayload().size(), 3);
      executing = true;
      std::unique_lock<std::mutex> lock(waitMutex);
      waitCondition.wait_for(lock, std::chrono::seconds(3), [&] { return released.load(); });
      ResponseMessage response;
      response.setStatus(true);
      auto payload = makeFilledBuffer(3, 'r');
      response.setPayload(payload, payload.size());
      return response;
    }));
  environment.enableProductionIngressForTest();
  RequestMessage request;
  request.setPolicyEpoch(user.getCurrentPolicyEpoch(config.serviceName));
  request.setStrategy(tlv::FirstResponding);
  auto payload = makeFilledBuffer(3, 'q');
  request.setPayload(payload, payload.size());
  InvocationNamesOracle::seed(user, id, config.serviceName, {config.providerIdentity}, request,
    [&](const ResponseMessage& response) { BOOST_CHECK(response.getStatus()); completed = true; });
  if (large) InvocationNamesOracle::setLargeAssignment(user, id, config.providerIdentity);
  ndn::signal::ScopedConnection connection(environment.userFace().onSendData.connect([&](const ndn::Data& data) {
    if (data.getContentType() == ndn::tlv::Data) {
      ++observedOuterPackets;
      BOOST_CHECK_LE(data.wireEncode().size(), ndn::MAX_NDN_PACKET_SIZE);
    }
    if (failDirectDelivery && data.getName() == selectionName) {
      ++directFailures;
      throw std::runtime_error("injected post-acceptance direct delivery failure");
    }
  }));
  InvocationNamesOracle::launch(user, id);
  try {
    environment.pumpUntilWithAttributeAuthority([&] {
      return executing.load() && (!failDirectDelivery || directFailures > 0);
    });
  }
  catch (const std::runtime_error& error) {
    // DummyFace emits onSendData from its asynchronous dispatch. Observe
    // this transport failure at that executor boundary, after SVS acceptance.
    if (!failDirectDelivery || directFailures != 1 ||
        std::string(error.what()) != "injected post-acceptance direct delivery failure") throw;
    observedAsyncDeliveryFailure = true;
    connection.disconnect();
  }
  if (!failDirectDelivery) BOOST_REQUIRE(executing.load());
  BOOST_CHECK_GT(observedOuterPackets, 0);
  const auto& originals = InvocationNamesOracle::packets(user, id, config.providerIdentity);
  BOOST_REQUIRE(!originals.empty());
  if (large) BOOST_CHECK_GT(originals.size(), 1);
  else BOOST_CHECK_EQUAL(originals.size(), 1);
  std::vector<ndn::Block> originalWires;
  for (const auto& original : originals) {
    BOOST_CHECK_LE(original.wireEncode().size(), ndn::MAX_NDN_PACKET_SIZE);
    BOOST_CHECK(selectionName.isPrefixOf(original.getName()));
    originalWires.push_back(original.wireEncode());
  }
  auto& sync = environment.userPubSub().getSVSync();
  const auto sequence = sync.getCore().getSeqNo();
  const auto digest = computeSelectionDigest(InvocationNamesOracle::prepared(user, id, config.providerIdentity));
  if (failDirectDelivery) {
    BOOST_CHECK_GT(directFailures, 0);
    BOOST_CHECK(observedAsyncDeliveryFailure);
    connection.disconnect();
  }
  InvocationNamesOracle::retry(user, id, config.providerIdentity);
  BOOST_CHECK_EQUAL(sync.getCore().getSeqNo(), sequence);
  BOOST_CHECK_EQUAL(computeSelectionDigest(InvocationNamesOracle::prepared(user, id, config.providerIdentity)), digest);
  const auto& retried = InvocationNamesOracle::packets(user, id, config.providerIdentity);
  BOOST_REQUIRE_EQUAL(retried.size(), originalWires.size());
  for (size_t index = 0; index < retried.size(); ++index) BOOST_CHECK(retried[index].wireEncode() == originalWires[index]);
  const auto savedOffer = InvocationNamesOracle::offer(user, id);
  InvocationNamesOracle::changeOffer(user, id);
  BOOST_CHECK_THROW(InvocationNamesOracle::retry(user, id, config.providerIdentity), std::runtime_error);
  InvocationNamesOracle::restoreOffer(user, id, savedOffer);
  const auto savedStreamOptions = InvocationNamesOracle::streamOptions(user, id);
  InvocationNamesOracle::changeAttempt(user, id);
  BOOST_CHECK_THROW(InvocationNamesOracle::retry(user, id, config.providerIdentity), std::runtime_error);
  InvocationNamesOracle::restoreStreamOptions(user, id, savedStreamOptions);
  BOOST_CHECK_EQUAL(sync.getCore().getSeqNo(), sequence);
  released = true;
  waitCondition.notify_all();
  // Face::processEvents shuts down its Impl after an escaping handler
  // exception. This fault case proves accepted-packet ownership, not recovery
  // of that closed Face; both ordinary real-chain cases prove final response.
  if (!failDirectDelivery) {
    environment.pumpUntilWithAttributeAuthority([&] { return completed.load(); });
    BOOST_CHECK(completed.load());
  }
}

BOOST_AUTO_TEST_CASE(ActualFourMessageChainKeepsTypedIdAndOriginalRetry)
{ runActualMessageChain(false, false); }

BOOST_AUTO_TEST_CASE(ActualSegmentedSelectionKeepsOriginalWireAndPacketBudget)
{ runActualMessageChain(true, false); }

BOOST_AUTO_TEST_CASE(PostAcceptanceServingFailureKeepsPublishedOriginal)
{ runActualMessageChain(false, true); }

BOOST_AUTO_TEST_SUITE_END()
} // namespace
