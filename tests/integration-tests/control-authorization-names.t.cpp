#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include "ndn-service-framework/ProducerDataNaming.hpp"
#include "ndn-service-framework/ServiceAuthorizationTable.hpp"
#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/CertificateBootstrap.hpp"
#include "ndn-service-framework/ServiceController.hpp"
#include "ndn-service-framework/ProducerSignerPolicy.hpp"
#include "NativeGrantVerifier.hpp"

#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/io.hpp>
#include <ndn-cxx/lp/nack.hpp>
#include <openssl/sha.h>
#include <fstream>
#include <filesystem>
#include <thread>
#include <array>
#include <atomic>
#include <algorithm>
#include <sstream>
#include <cstdlib>
#include <unistd.h>

namespace ndn_service_framework::test {
// Existing private oracle friendship: no production layout or public API.
struct InvocationNamesOracle {
  static std::optional<ndn::Interest> policyStatusQuery(ServiceUser& user,
      const ndn::Name& controller, const ndn::Name& service)
  { return user.prepareControllerControlQuery(controller, "POLICY-STATUS",
      encodeControlQueryParameters({{}, service, 0, 0}), 1); }
  static std::optional<ndn::Interest> policyStatusQuery(ServiceProvider& provider,
      const ndn::Name& controller, const ndn::Name& service)
  { return provider.prepareControllerControlQuery(controller, "POLICY-STATUS",
      encodeControlQueryParameters({{}, service, 0, 0}), 1); }
  static void policyStatus(ServiceUser& user, const ndn::Interest& query, const ndn::Data& packet)
  { user.onPolicyStatusData(query, packet, 1); }
  static void policyStatus(ServiceProvider& provider, const ndn::Interest& query, const ndn::Data& packet)
  { provider.onPolicyStatusData(query, packet, 1); }
  static bool ready(ServiceUser& user, const ndn::Interest& interest)
  { return user.handleProviderReadyInterest(interest); }
  static bool activate(ServiceProvider& provider, const ndn::Interest& interest)
  { return provider.handleExecutionActivateInterest(interest); }
  static bool statusReply(ServiceProvider& provider, const ndn::Interest& interest)
  { return provider.replySelectionExecutionStatus(interest); }
  static void bindConfiguredValidator(ServiceUser& user, ndn::Face& face,
                                      const ndn::Name& group, const std::string& schema)
  { user.validator = std::make_shared<MessageValidator>(schema, group, &face); }
  static void status(ServiceProvider& provider, const ndn::Name& actor, const ndn::Name& service,
                     const std::string& digest, SelectionExecutionState state)
  { provider.updateSelectionExecutionStatus(digest, state, actor, service, ndn::Name("/status/request")); }
  static void retireActivation(ServiceProvider& provider, const std::string& selection)
  { provider.m_preparedDeployments.erase(selection); }

  static DeploymentPlan prepareReadyOwner(ServiceUser& user, const ndn::Name& id,
                                         const ndn::Name& provider, const std::string& handle)
  {
    DeploymentPlan plan;
    plan.setField("member.0.provider", provider.toUri());
    plan.setField("member.0.role", "/role");
    plan.setField("member.0.statusHandle", handle);
    // Keep another real declared member unready; this test observes readiness
    // admission and ACK retry without starting an unrelated activation.
    plan.setField("member.1.provider", provider.toUri());
    plan.setField("member.1.role", "/waiting-role");
    plan.setField("member.1.statusHandle", handle);
    auto& pending = user.m_pendingCalls[id];
    pending.serviceName = ndn::Name("/service");
    pending.deploymentPlan = plan;
    pending.selectionDigestsByProvider[provider.toUri()] = "selection";
    return plan;
  }
  static size_t readyCount(const ServiceUser& user, const ndn::Name& id)
  { return user.m_pendingCalls.at(id).deploymentReadyByMember.size(); }
  static void terminal(ServiceUser& user, const ndn::Name& id, bool value)
  { user.m_pendingCalls.at(id).timedOut = value; }
  static void revokeReadyOwner(ServiceUser& user)
  {
    const ndn::Name service("/service");
    const auto now = static_cast<uint64_t>(ndn::time::duration_cast<ndn::time::milliseconds>(
        ndn::time::system_clock::now().time_since_epoch()).count());
    PolicyStatusData status;
    status.setServiceName(service);
    status.setControllerVersion(ControllerVersion{1, 1});
    status.setValidity(now - 1, now + 60000);
    status.setPolicyDigest("sha256:" + std::string(64, 'a'));
    status.setControllerCertificate(ndn::Name("/controller/KEY/key/issuer").appendVersion(1));
    ndn::Buffer signature(1); signature[0] = 1; status.setSignature(signature);
    RevocationTarget target;
    target.kind = RevocationKind::IDENTITY; target.targetIdentity = user.identity;
    status.addRevocation(target);
    user.m_controllerPrefix = ndn::Name("/controller");
    auto inserted = user.m_revocationStates.emplace(service.toUri(), RevocationState(service));
    BOOST_REQUIRE(inserted.first->second.acceptStatus(status, now));
  }
};
}

namespace {
using namespace ndn_service_framework;
using namespace ndn_service_framework::test;
using Bytes = std::vector<uint8_t>;

Bytes wire(const ndn::Name& name)
{
  const auto& value = name.wireEncode();
  return Bytes(value.data(), value.data() + value.size());
}
Bytes utf8(const std::string& value) { return Bytes(value.begin(), value.end()); }
Bytes uint64(uint64_t value)
{
  Bytes output;
  for (int shift = 56; shift >= 0; shift -= 8) output.push_back(value >> shift);
  return output;
}
ndn::name::Component rawHash(const Bytes& value)
{
  std::array<uint8_t, SHA256_DIGEST_LENGTH> digest{};
  SHA256(value.data(), value.size(), digest.data());
  return ndn::name::Component(ndn::span<const uint8_t>(digest.data(), digest.size()));
}
// Independent C01 transcript; no production naming codec constructs expected Names.
ndn::name::Component commitment(const std::string& domain,
                               const std::vector<NamingDigestField>& fields)
{
  Bytes value;
  const auto integer = [&](uint64_t number, size_t width) {
    for (size_t i = width; i > 0; --i) value.push_back(number >> (8 * (i - 1)));
  };
  integer(domain.size(), 2); value.insert(value.end(), domain.begin(), domain.end());
  integer(2, 2); integer(fields.size(), 2);
  for (const auto& field : fields) {
    integer(field.tag, 2); value.push_back(field.kind); integer(field.value.size(), 8);
    value.insert(value.end(), field.value.begin(), field.value.end());
  }
  return rawHash(value);
}
ndn::name::Component repeatedDigest(uint8_t byte)
{
  const Bytes value(32, byte);
  return ndn::name::Component(ndn::span<const uint8_t>(value.data(), value.size()));
}
std::string digestText(char character) { return "sha256:" + std::string(64, character); }
void pump(boost::asio::io_context& io, const std::function<bool()>& finished,
          std::chrono::seconds budget = std::chrono::seconds(3))
{
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!finished() && std::chrono::steady_clock::now() < deadline) {
    io.poll(); io.restart(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  BOOST_REQUIRE(finished());
}
}

BOOST_AUTO_TEST_SUITE(Spec201ControlAuthorization)

BOOST_AUTO_TEST_CASE(UnconfiguredStatusRepliesRequireExactPreparedOwnerAndSigner)
{
  struct GenerationState {
    std::optional<std::string> previous;
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("spec201-status-query-owner-" + std::to_string(::getpid()) + ".bin");
    GenerationState() {
      if (const auto* value = ::getenv("NDNSF_CONTROLLER_GENERATION_STATE")) previous = value;
      ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", path.c_str(), 1);
    }
    ~GenerationState() {
      if (previous) ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", previous->c_str(), 1);
      else ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
      std::filesystem::remove(path.string() + ".lock", ignored);
    }
  } state;
  ndn::KeyChain keys;
  // Reserved labels inside the Producer prefix must retain their boundary.
  const auto issuer = makeRsaIdentity(keys, ndn::Name("/spec201/query-owner/NDNSF/RPC/controller"));
  const auto foreign = makeRsaIdentity(keys, ndn::Name("/spec201/query-owner/foreign"));
  const auto userCert = makeRsaIdentity(keys, ndn::Name("/spec201/query-owner/user"));
  const auto providerCert = makeRsaIdentity(keys, ndn::Name("/spec201/query-owner/provider"));
  ndn::io::save(issuer, "spec201-query-owner-controller.cert");
  ndn::io::save(foreign, "spec201-query-owner-foreign.cert");
  {
    std::ofstream schema("spec201-query-owner.conf");
    schema << "rule { id status for data checker { type customized sig-type rsa-sha256 "
              "key-locator { type name regex ^<>*$ } } }\n"
              "trust-anchor { type file file-name spec201-query-owner-controller.cert }\n"
              "trust-anchor { type file file-name spec201-query-owner-foreign.cert }\n";
  }
  ndn::DummyClientFace face(keys);
  ndn::ValidatorConfig validator(face);
  ServiceController controller(face, issuer, validator, "examples/hello.policies");
  const ndn::Name service("/HELLO"), producer = issuer.getIdentity();
  const auto status = controller.getPolicyStatus(service);
  ServiceUser user(ServiceUser::LocalMockTag{}, face, ndn::Name("/spec201/query-owner"),
                   userCert, issuer, "spec201-query-owner.conf");
  ServiceProvider provider(ServiceProvider::LocalMockTag{}, face, ndn::Name("/spec201/query-owner"),
                           providerCert, issuer, "spec201-query-owner.conf");
  const auto packet = [&](const ndn::Interest& query, const ndn::security::Certificate& signer) {
    ndn::Data data(query.getName());
    data.setContent(status.wireEncode());
    keys.sign(data, ndn::security::signingByCertificate(signer));
    return data;
  };
  const auto exercise = [&](auto& runtime) {
    const auto parameters = encodeControlQueryParameters({{}, service, 0, 0});
    ndn::Interest unowned(makeControlRpcName(producer, "POLICY-STATUS", parameters, makeControlQueryNonce()));
    unowned.setApplicationParameters(parameters);
    InvocationNamesOracle::policyStatus(runtime, unowned, packet(unowned, issuer));
    BOOST_REQUIRE(!runtime.getControllerVersion());
    const auto prepared = InvocationNamesOracle::policyStatusQuery(runtime, producer, service);
    BOOST_REQUIRE(prepared);
    // Same call identity with a different nonce is still unowned.
    InvocationNamesOracle::policyStatus(runtime, unowned, packet(unowned, issuer));
    BOOST_REQUIRE(!runtime.getControllerVersion());
    // This foreign signer is explicitly anchored, so rejection must retain
    // the exact requested Controller binding after cryptographic validation.
    InvocationNamesOracle::policyStatus(runtime, *prepared, packet(*prepared, foreign));
    BOOST_REQUIRE(!runtime.getControllerVersion());
    InvocationNamesOracle::policyStatus(runtime, *prepared, packet(*prepared, issuer));
    BOOST_REQUIRE(runtime.getControllerVersion());
    BOOST_CHECK(*runtime.getControllerVersion() == status.getControllerVersion());
  };
  exercise(user);
  exercise(provider);
}

BOOST_AUTO_TEST_CASE(LegacyHybridMessageCategoriesUseComponentValuesInBothUriProfiles)
{
  for (const auto* type : {"REQUEST", "ACK", "SELECTION", "RESPONSE", "EVENT"}) {
    const auto name = ndn::Name("/producer").append("NDNSF").append(type).append("tail");
    BOOST_CHECK_EQUAL(hybridMessageTypeForName(name), type);
    BOOST_CHECK_EQUAL(hybridMessageTypeForName(
      ndn::Name(name.toUri(ndn::name::UriFormat::CANONICAL))), type);
    BOOST_CHECK_EQUAL(hybridMessageTypeForName(
      ndn::Name(name.toUri(ndn::name::UriFormat::ALTERNATE))), type);
  }
}

BOOST_AUTO_TEST_CASE(DeploymentControlCarriersContainExactlyOneCanonicalMessage)
{
  ProviderReadyMessage original;
  original.setField("requestId", "/request");
  original.setField("providerIdentity", "/provider");
  const auto encoded = original.WireEncode();
  ndn::Data packet(ndn::Name("/provider/control"));
  packet.setContent(encoded);
  ndn::Interest query(ndn::Name("/user/control"));
  query.setApplicationParameters(encoded);
  for (const auto& carrier : {encoded, packet.getContent(), query.getApplicationParameters()}) {
    ProviderReadyMessage decoded;
    BOOST_REQUIRE(decoded.WireDecode(carrier));
    BOOST_CHECK(decoded.WireEncode() == encoded);
    BOOST_CHECK_EQUAL(decoded.computeDigest(), original.computeDigest());
  }
  ndn::Block siblings(ndn::tlv::Content);
  siblings.push_back(encoded); siblings.push_back(encoded); siblings.encode();
  ndn::Block nested(ndn::tlv::Content);
  nested.push_back(query.getApplicationParameters()); nested.encode();
  ndn::Block wrong(ndn::tlv::InterestSignatureInfo);
  wrong.push_back(encoded); wrong.encode();
  Bytes trailingBytes(encoded.begin(), encoded.end()); trailingBytes.push_back(0);
  const auto trailing = ndn::makeBinaryBlock(ndn::tlv::Content, trailingBytes.begin(), trailingBytes.end());
  Bytes oversized(DeploymentControlMessage::MAX_WIRE_SIZE + 1, 0);
  const auto tooLarge = ndn::makeBinaryBlock(ndn::tlv::Content, oversized.begin(), oversized.end());
  for (const auto& invalid : {siblings, nested, wrong, trailing, tooLarge}) {
    ProviderReadyMessage decoded;
    BOOST_REQUIRE(decoded.WireDecode(encoded));
    BOOST_CHECK(!decoded.WireDecode(invalid));
    BOOST_CHECK(decoded.WireEncode() == encoded);
  }
}

BOOST_AUTO_TEST_CASE(CurrentRpcNamesBindExactParameterWireAndActor)
{
  const ndn::Name producer("/spec201/NDNSF/POLICY-MANIFEST/controller");
  ControlQueryParameters query{ndn::Name("/org/user"), ndn::Name(), 0, 0};
  const auto parameters = encodeControlQueryParameters(query);
  ndn::Interest normalized; normalized.setApplicationParameters(parameters);
  const auto& parameterWire = normalized.getApplicationParameters();
  auto expected = producer;
  expected.append("NDNSF").append("RPC").append("USER-PERMISSIONS")
    .append(commitment("ndnsf-naming/control-call", {
      {1, 2, utf8("USER-PERMISSIONS")},
      {2, 4, {parameterWire.data(), parameterWire.data() + parameterWire.size()}}}))
    .append("independent-query");
  const auto logical = makeControlRpcName(producer, "USER-PERMISSIONS", parameters, "independent-query");
  BOOST_CHECK(logical.wireEncode() == expected.wireEncode());
  ndn::Interest interest(logical); interest.setApplicationParameters(parameters);
  BOOST_REQUIRE(parseControlRpcName(interest, producer, "USER-PERMISSIONS"));
  BOOST_CHECK(!parseControlRpcName(interest, ndn::Name("/spec201"), "USER-PERMISSIONS"));
  BOOST_CHECK(!parseControlRpcName(interest, producer, "PROVIDER-PERMISSIONS"));
  auto changed = interest;
  changed.setApplicationParameters(encodeControlQueryParameters({ndn::Name("/foreign"), {}, 0, 0}));
  BOOST_CHECK(!parseControlRpcName(changed, producer, "USER-PERMISSIONS"));
  BOOST_REQUIRE(decodeControlQueryParameters(interest.getApplicationParameters()));
  BOOST_CHECK(decodeControlQueryParameters(interest.getApplicationParameters())->subject == query.subject);
}

BOOST_AUTO_TEST_CASE(AuthorityReadinessRpcBindsOriginalChallengeAndFinalParametersDigest)
{
  const ndn::Name actor("/authority/complete");
  const std::string challenge = "/8=authority/8=PUBPARAMS|sha256:" + std::string(64, 'a');
  const auto parameters = ndn::makeStringBlock(ndn::tlv::Content, challenge);
  ndn::Interest query(makeControlRpcName(actor, "AA-READY", parameters, "challenge"));
  query.setApplicationParameters(parameters); query.setHopLimit(1); query.setCanBePrefix(false);
  auto expected = actor;
  const auto app = query.getApplicationParameters();
  expected.append("NDNSF").append("RPC").append("AA-READY")
    .append(commitment("ndnsf-naming/control-call", {
      {1, 2, utf8("AA-READY")}, {2, 4, Bytes(app.data(), app.data() + app.size())}}))
    .append("challenge").append(query.getName()[-1]);
  BOOST_CHECK(expected.wireEncode() == query.getName().wireEncode());
  BOOST_REQUIRE(parseControlRpcName(query, actor, "AA-READY"));
  BOOST_CHECK(query.isParametersDigestValid());
  auto changed = query; changed.setApplicationParameters(ndn::makeStringBlock(ndn::tlv::Content, "changed"));
  changed.setName(query.getName()); BOOST_CHECK(!parseControlRpcName(changed, actor, "AA-READY"));
}

BOOST_AUTO_TEST_CASE(GrantAndDeploymentMatchIndependentPaperComponentWire)
{
  const ndn::Name producer("/org/NDNSF/GRANT/user"), recipient("/org/KEY/provider");
  const ndn::Name request = ndn::Name("/request/tail").appendVersion(257).appendSegment(0);
  const Bytes plan(32, 0xaa), model(32, 0xbb);
  auto expected = producer;
  expected.append("NDNSF").append("GRANT").append(ndn::name::Component("kind=KEY-GRANT"))
    .append("RECIPIENT").append(commitment("ndnsf-naming/opaque/recipient", {{1, 4, wire(recipient)}}))
    .append("SCOPE").append("MODEL")
    .append(commitment("ndnsf-naming/grant-scope", {
      {1, 1, wire(producer)}, {2, 1, wire(request)}, {3, 3, uint64(7)},
      {4, 4, plan}, {5, 4, model}}))
    .append("EPOCH").append(commitment("ndnsf-naming/opaque/protection-epoch", {{1, 4, utf8("epoch/typed=1")}}))
    .append("OBJECT").append(repeatedDigest(0xcc));
  const ndn::Name actual(ndnsf::di::canonicalNativeGrantName(
      producer.toUri(ndn::name::UriFormat::CANONICAL), recipient.toUri(), request.toUri(),
      7, digestText('a'), digestText('b'), "epoch/typed=1", digestText('c')));
  BOOST_CHECK(actual.wireEncode() == expected.wireEncode());
  BOOST_CHECK(ndn::Name(ndnsf::di::canonicalNativeGrantName(producer.toUri(ndn::name::UriFormat::ALTERNATE),
      recipient.toUri(), request.toUri(), 7, digestText('a'), digestText('b'),
      "epoch/typed=1", digestText('c'))) == actual);
  const ndn::Name application("/app/NDNSF/DEPLOYMENT/demo");
  auto definition = producer;
  definition.append("NDNSF").append("DEPLOYMENT")
    .append(ndn::name::Component("name=" + application.toUri(ndn::name::UriFormat::CANONICAL)))
    .append(commitment("ndnsf-naming/deployment", {{1, 1, wire(application)}, {2, 2, utf8("deployment/one")}}));
  auto activation = definition;
  activation.append("REVISION").append(repeatedDigest(0xbb)).append("ACTIVATION").append(repeatedDigest(0xcc));
  definition.append("DEFINITION").append(repeatedDigest(0xcc));
  BOOST_CHECK(makeDeploymentRecordName(producer, application, "deployment/one", "DEFINITION", "", 0,
                                      digestText('c')).wireEncode() == definition.wireEncode());
  BOOST_CHECK(makeDeploymentRecordName(producer, application, "deployment/one", "ACTIVATION", digestText('b'), 0,
                                      digestText('c')).wireEncode() == activation.wireEncode());
  auto revocation = activation.getPrefix(-2);
  revocation.append("REVOCATION").append("EPOCH").append("257").append(repeatedDigest(0xcc));
  BOOST_CHECK(makeDeploymentRecordName(producer, application, "deployment/one", "REVOCATION", digestText('b'), 257,
                                      digestText('c')).wireEncode() == revocation.wireEncode());
  BOOST_CHECK_THROW(makeDeploymentRecordName(producer, application, "deployment/one", "ACTIVATION", digestText('b'), 1,
                                           digestText('c')), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(PermissionTableUsesCompleteNameWireAndKeepsEpochFence)
{
  ServiceAuthorizationTable table;
  const ndn::Name providerService("/org/provider/SERVICE/service"), service("/service");
  BOOST_REQUIRE(table.replacePermissions(tlv::UserPermission, 7, {{
    providerService.toUri(ndn::name::UriFormat::ALTERNATE),
    service.toUri(ndn::name::UriFormat::CANONICAL), tlv::UserPermission, 7}}));
  BOOST_CHECK(table.contains(providerService.toUri(ndn::name::UriFormat::CANONICAL),
                            service.toUri(ndn::name::UriFormat::ALTERNATE), tlv::UserPermission));
  BOOST_CHECK(!table.upsert({providerService.toUri(), service.toUri(), tlv::UserPermission, 6}));
  BOOST_CHECK(!table.replacePermissions(tlv::UserPermission, 8, {
    {providerService.toUri(ndn::name::UriFormat::CANONICAL), service.toUri(), tlv::UserPermission, 8},
    {providerService.toUri(ndn::name::UriFormat::ALTERNATE), "/foreign", tlv::UserPermission, 8}}));
  BOOST_REQUIRE(table.find(providerService.toUri()));
  BOOST_CHECK_EQUAL(table.find(providerService.toUri())->policyEpoch, 7);
  BOOST_CHECK(!table.find("/"));
  BOOST_CHECK(!table.contains(providerService.toUri(), "/", tlv::UserPermission));
}

BOOST_AUTO_TEST_CASE(PrefixOnlyControllerBootstrapCannotAdmitForgedData)
{
  ndn::KeyChain keys;
  const ndn::Name controller("/spec201/control/controller"), foreign("/spec201/control/user");
  const ndn::Name root("/spec201/control");
  const auto rootIdentity = keys.createIdentity(root, ndn::RsaKeyParams(2048));
  const auto controllerKey = keys.createIdentity(controller, ndn::RsaKeyParams(2048)).getDefaultKey();
  const auto foreignKey = keys.createIdentity(foreign, ndn::RsaKeyParams(2048)).getDefaultKey();
  for (const auto& key : {controllerKey, foreignKey}) {
    const auto issued = keys.makeCertificate(key, ndn::security::signingByIdentity(root));
    keys.addCertificate(key, issued); keys.setDefaultCertificate(key, issued);
    BOOST_REQUIRE(ndn::security::verifySignature(issued, rootIdentity.getDefaultKey()));
  }
  ndn::io::save(rootIdentity.getDefaultKey().getDefaultCertificate(), "spec201-control-root.cert");
  const auto schema = std::getenv("SPEC201_CONTROL_SCHEMA");
  BOOST_REQUIRE_MESSAGE(schema != nullptr, "actual generated anchored schema is required");
  boost::asio::io_context io;
  ndn::DummyClientFace face(io, keys);
  auto validator = std::make_shared<MessageValidator>(schema, std::nullopt, &face);
  ndn::Data original(ndn::Name(controller).append("NDNSF").append("POLICY-MANIFEST"));
  original.setContent("original"); keys.sign(original, ndn::security::signingByIdentity(controller));
  auto validate = [&](const ndn::Data& packet, bool success) {
    bool done = false;
    validator->validate(packet,
      [&](const ndn::Data&) { BOOST_CHECK(success); done = true; },
      [&](const ndn::Data&, const auto&) { BOOST_CHECK(!success); done = true; });
    pump(io, [&] { return done; });
  };
  validate(original, true);
  auto tampered = original; tampered.setContent("forged without signature");
  validate(tampered, false);
  auto invalidSignature = original;
  const auto& signature = original.getSignatureValue();
  Bytes damaged(signature.value(), signature.value() + signature.value_size());
  BOOST_REQUIRE(!damaged.empty()); damaged.front() ^= 1;
  invalidSignature.setSignatureValue(ndn::span<const uint8_t>(damaged.data(), damaged.size()));
  validate(invalidSignature, false);
  auto wrongActor = original;
  keys.sign(wrongActor, ndn::security::signingByIdentity(foreign));
  BOOST_REQUIRE(ndn::security::verifySignature(wrongActor,
      keys.getPib().getIdentity(foreign).getDefaultKey()));
  // Keep test assertions outside the asynchronous validator callback stack.
  auto certificateRequests = std::make_shared<std::vector<std::string>>();
  auto certificateReplies = std::make_shared<size_t>(0);
  const std::vector<ndn::security::Certificate> originalCertificates{
    controllerKey.getDefaultCertificate(), foreignKey.getDefaultCertificate()};
  ndn::signal::ScopedConnection observeRequests = face.onSendInterest.connect(
    [certificateRequests, certificateReplies, originalCertificates, &face](const ndn::Interest& interest) {
      certificateRequests->push_back(interest.getName().toUri(ndn::name::UriFormat::CANONICAL) +
        " CanBePrefix=" + (interest.getCanBePrefix() ? "true" : "false"));
      for (const auto& certificate : originalCertificates) {
        if (interest.matchesData(certificate)) {
          ++*certificateReplies; face.receive(certificate); return;
        }
      }
    });
  auto validateConfigured = [&](const ndn::Data& packet, bool expected) {
    struct Result { bool done = false; bool accepted = false; std::string error; };
    auto result = std::make_shared<Result>();
    validator->validateWithConfiguredTrustSchema(packet,
      [result](const ndn::Data&) { result->accepted = true; result->done = true; },
      [result](const ndn::Data&, const ndn::security::ValidationError& error) {
        std::ostringstream text; text << error;
        result->error = text.str(); result->done = true;
      });
    pump(io, [result] { return result->done; });
    std::ostringstream diagnostic;
    diagnostic << "configured result=" << result->accepted << " error=" << result->error;
    for (const auto& request : *certificateRequests) diagnostic << " certificate Interest=" << request;
    BOOST_REQUIRE_MESSAGE(result->accepted == expected, diagnostic.str());
  };
  validateConfigured(original, true);
  BOOST_CHECK_GT(*certificateReplies, 0U);
  validateConfigured(tampered, false);
  validateConfigured(invalidSignature, false);
  validateConfigured(wrongActor, false);
}

BOOST_AUTO_TEST_CASE(AsyncValidatorOwnerSurvivesCallerReleaseAndTerminates)
{
  const auto schema = std::getenv("SPEC201_CONTROL_SCHEMA");
  BOOST_REQUIRE(schema != nullptr);
  for (bool configured : {false, true}) {
    for (unsigned mode : {0U, 1U, 2U, 3U, 4U}) {
      // The configured path additionally observes the installed 4x4s retry
      // timeout. Both entry points observe completion and Face cancellation.
      if (!configured && mode >= 2) continue;
      ndn::KeyChain keys("pib-memory:", "tpm-memory:");
      const ndn::Name root("/spec201/control");
      const auto rootIdentity = keys.createIdentity(root, ndn::RsaKeyParams(2048));
      auto actor = root; actor.append("retained-owner")
        .append(configured ? "configured" : "ordinary").append(std::to_string(mode).c_str());
      const auto actorKey = keys.createIdentity(actor, ndn::RsaKeyParams(2048)).getDefaultKey();
      const auto certificate = keys.makeCertificate(actorKey, ndn::security::signingByIdentity(root));
      keys.addCertificate(actorKey, certificate); keys.setDefaultCertificate(actorKey, certificate);
      ndn::io::save(rootIdentity.getDefaultKey().getDefaultCertificate(), "spec201-control-root.cert");
      boost::asio::io_context io;
      ndn::DummyClientFace face(io, keys);
      const auto parameters = ndn::makeStringBlock(ndn::tlv::Content, "owner-lifetime");
      ndn::Interest query(makeControlRpcName(actor, "POLICY-MANIFEST", parameters,
          makeControlQueryNonce()));
      query.setApplicationParameters(parameters);
      ndn::Data packet(query.getName());
      packet.setContent("original"); keys.sign(packet, ndn::security::signingByIdentity(actor));
      struct Result {
        bool requested = false; bool matchesOriginal = false; bool done = false;
        size_t success = 0; size_t failure = 0; size_t interestCount = 0; std::string error;
        std::optional<ndn::Interest> certificateInterest;
      };
      auto result = std::make_shared<Result>();
      ndn::signal::ScopedConnection observe = face.onSendInterest.connect(
        [result, certificate, &face, mode](const ndn::Interest& interest) {
          result->requested = true; result->matchesOriginal = interest.matchesData(certificate);
          result->certificateInterest = interest;
          ++result->interestCount;
          if (mode == 4 && result->interestCount == 2 && result->matchesOriginal)
            face.receive(certificate);
        });
      auto validator = std::make_shared<MessageValidator>(schema, std::nullopt, &face);
      const std::weak_ptr<MessageValidator> weakOwner(validator);
      auto success = [result](const ndn::Data&) { ++result->success; result->done = true; };
      auto failure = [result](const ndn::Data&, const ndn::security::ValidationError& error) {
        ++result->failure; result->done = true;
        std::ostringstream text; text << error; result->error = text.str();
      };
      if (configured) validator->validateWithConfiguredTrustSchema(packet, success, failure);
      else validator->validate(packet, success, failure);
      pump(io, [result] { return result->requested || result->done; });
      BOOST_REQUIRE_MESSAGE(result->requested && !result->done, result->error);
      BOOST_REQUIRE(result->matchesOriginal);
      BOOST_REQUIRE_GT(face.getNPendingInterests(), 0U);
      validator.reset();
      BOOST_CHECK(!weakOwner.expired());
      if (mode == 0) face.receive(certificate);
      else if (mode == 1) face.shutdown();
      else if (mode == 3 || mode == 4) {
        // Face.shutdown does not cancel a separate Scheduler. Keep the
        // borrowed Face alive while the original Nack retry reaches its PIT;
        // cancellation of that pending Face operation must release the owner.
        BOOST_REQUIRE(result->certificateInterest.has_value());
        ndn::lp::Nack nack(*result->certificateInterest);
        nack.setReason(ndn::lp::NackReason::NO_ROUTE);
        face.receive(nack);
        if (mode == 3) {
          face.shutdown();
          pump(io, [result] { return result->interestCount == 2; }, std::chrono::seconds(3));
          BOOST_REQUIRE(!result->done);
          BOOST_REQUIRE(!weakOwner.expired());
          BOOST_REQUIRE_GT(face.getNPendingInterests(), 0U);
          face.shutdown();
        }
      }
      pump(io, [result, weakOwner] { return result->done && weakOwner.expired(); },
           mode == 2 ? std::chrono::seconds(20) : std::chrono::seconds(3));
      const bool completesSuccessfully = mode == 0 || mode == 4;
      BOOST_CHECK_EQUAL(result->success, completesSuccessfully ? 1U : 0U);
      BOOST_CHECK_EQUAL(result->failure, completesSuccessfully ? 0U : 1U);
      if (mode == 2) BOOST_CHECK_MESSAGE(result->error.find("Timeout after exhausting") != std::string::npos,
                                       result->error);
      if (mode == 3 || mode == 4) BOOST_CHECK_EQUAL(result->interestCount, 2U);
      BOOST_CHECK_EQUAL(face.getNPendingInterests(), 0U);
      BOOST_CHECK(weakOwner.expired());
    }
  }
}

BOOST_AUTO_TEST_CASE(StackValidatorRejectsEscapingCertificateValidation)
{
  const auto schema = std::getenv("SPEC201_CONTROL_SCHEMA");
  BOOST_REQUIRE(schema != nullptr);
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  const ndn::Name root("/spec201/control"), actor("/spec201/control/stack-owner");
  const auto rootIdentity = keys.createIdentity(root, ndn::RsaKeyParams(2048));
  const auto key = keys.createIdentity(actor, ndn::RsaKeyParams(2048)).getDefaultKey();
  const auto certificate = keys.makeCertificate(key, ndn::security::signingByIdentity(root));
  keys.addCertificate(key, certificate); keys.setDefaultCertificate(key, certificate);
  ndn::io::save(rootIdentity.getDefaultKey().getDefaultCertificate(), "spec201-control-root.cert");
  boost::asio::io_context io; ndn::DummyClientFace face(io, keys);
  MessageValidator validator(schema, std::nullopt, &face);
  ndn::Data packet(ndn::Name(actor).append("NDNSF").append("POLICY-MANIFEST"));
  packet.setContent("original"); keys.sign(packet, ndn::security::signingByIdentity(actor));
  size_t successes = 0, failures = 0;
  auto success = [&](const ndn::Data&) { ++successes; };
  auto failure = [&](const ndn::Data&, const ndn::security::ValidationError& error) {
    ++failures; BOOST_CHECK_EQUAL(error.getCode(), ndn::security::ValidationError::IMPLEMENTATION_ERROR);
  };
  validator.validateWithConfiguredTrustSchema(packet, success, failure);
  validator.validate(packet, success, failure);
  BOOST_CHECK_EQUAL(successes, 0U); BOOST_CHECK_EQUAL(failures, 2U);
  BOOST_CHECK_EQUAL(face.getNPendingInterests(), 0U);
}

BOOST_AUTO_TEST_CASE(ActualUserGrantPublisherKeepsOriginalWireAndRejectsConflict)
{
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  ndn::DummyClientFace face(keys);
  const auto owner = makeRsaIdentity(keys, ndn::Name("/spec201/owner/NDNSF/GRANT"));
  const auto authority = makeRsaIdentity(keys, ndn::Name("/spec201/authority"));
  LocalServiceUser user(face, ndn::Name("/spec201/group"), owner, authority, "examples/trust-any.conf");
  user.useSigningKeyChainForTest(keys);
  const ndn::Name name(ndnsf::di::canonicalNativeGrantName(owner.getIdentity().toUri(), "/recipient", "/request", 1,
       digestText('a'), digestText('b'), "epoch", digestText('c')));
  ndn::Buffer payload(5); std::fill(payload.begin(), payload.end(), 0x71);
  user.publishSignedAppData(name, payload, ndn::time::milliseconds(1000));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE(!face.sentData.empty());
  const auto original = face.sentData.back().wireEncode();
  user.publishSignedAppData(name, payload, ndn::time::milliseconds(1000));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK(face.sentData.back().wireEncode() == original);
  payload[0] ^= 1;
  BOOST_CHECK_THROW(user.publishSignedAppData(name, payload, ndn::time::milliseconds(1000)), std::invalid_argument);
  BOOST_CHECK(face.sentData.back().wireEncode() == original);
  BOOST_CHECK_THROW(user.publishSignedAppData(ndn::Name("/foreign/NDNSF/GRANT/object"), payload,
                                            ndn::time::milliseconds(1000)), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ActualSelectionStatusQueryPreservesOriginalAndRefreshesCurrentState)
{
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  boost::asio::io_context io;
  ndn::DummyClientFace providerFace(io, keys), userFace(io, keys);
  const ndn::Name root("/spec201/control");
  const auto rootIdentity = keys.createIdentity(root, ndn::RsaKeyParams(2048));
  const auto issue = [&](const ndn::Name& name) {
    const auto key = keys.createIdentity(name, ndn::RsaKeyParams(2048)).getDefaultKey();
    const auto certificate = keys.makeCertificate(key, ndn::security::signingByIdentity(root));
    keys.addCertificate(key, certificate); keys.setDefaultCertificate(key, certificate);
    BOOST_REQUIRE(ndn::security::verifySignature(certificate, rootIdentity.getDefaultKey()));
    return certificate;
  };
  const auto actor = issue(ndn::Name("/spec201/control/NDNSF/SELECTION-STATUS/provider"));
  const auto requester = issue(ndn::Name("/spec201/control/status/user"));
  const auto aa = issue(ndn::Name("/spec201/control/status/authority"));
  const auto foreign = issue(ndn::Name("/spec201/control/status/foreign"));
  ndn::io::save(rootIdentity.getDefaultKey().getDefaultCertificate(), "spec201-control-root.cert");
  const auto schema = std::getenv("SPEC201_CONTROL_SCHEMA");
  BOOST_REQUIRE_MESSAGE(schema != nullptr, "actual generated anchored schema is required");
  const ndn::Name service("/service/NDNSF/REQUEST/tail");
  const std::string digest = "selection-digest/full";
  LocalServiceProvider provider(providerFace, ndn::Name("/spec201/status/group"), actor, aa,
                                schema);
  LocalServiceUser user(userFace, ndn::Name("/spec201/status/group"), requester, aa,
                        schema);
  provider.useSigningKeyChainForTest(keys);
  user.useSigningKeyChainForSigningOnlyForTest(keys);
  InvocationNamesOracle::bindConfiguredValidator(user, userFace,
      ndn::Name("/spec201/status/group"), schema);
  provider.setSelectionStatusQueryable(service, true);
  InvocationNamesOracle::status(provider, actor.getIdentity(), service, digest, SelectionExecutionState::Received);
  enum class Fault { None, Signature, ForeignSigner };
  Fault fault = Fault::None;
  size_t originalCertificateReplies = 0;
  std::vector<ndn::Interest> queries;
  std::vector<ndn::Data> replies;
  ndn::signal::ScopedConnection toProvider = userFace.onSendInterest.connect([&](const ndn::Interest& interest) {
    for (const auto& certificate : {actor, requester, aa, foreign,
                                   rootIdentity.getDefaultKey().getDefaultCertificate()}) {
      if (interest.matchesData(certificate)) {
        ++originalCertificateReplies;
        userFace.receive(certificate);
        return;
      }
    }
    if (!parseSelectionStatusQueryName(interest, actor.getIdentity())) return;
    queries.push_back(interest);
    BOOST_REQUIRE(InvocationNamesOracle::statusReply(provider, interest));
  });
  ndn::signal::ScopedConnection toUser = providerFace.onSendData.connect([&](const ndn::Data& data) {
    if (!actor.getIdentity().isPrefixOf(data.getName())) return;
    replies.push_back(data);
    auto delivered = data;
    if (fault == Fault::Signature) {
      const auto& originalSignature = data.getSignatureValue();
      Bytes damaged(originalSignature.value(), originalSignature.value() + originalSignature.value_size());
      BOOST_REQUIRE(!damaged.empty()); damaged.front() ^= 1;
      delivered.setSignatureValue(ndn::span<const uint8_t>(damaged.data(), damaged.size()));
    }
    else if (fault == Fault::ForeignSigner) {
      keys.sign(delivered, ndn::security::signingByCertificate(foreign));
      BOOST_REQUIRE(ndn::security::verifySignature(delivered, foreign));
    }
    userFace.receive(delivered);
  });
  auto query = [&] (SelectionExecutionState expected) {
    bool complete = false;
    user.QuerySelectionStatus(actor.getIdentity(), service, digest,
      [&](const SelectionExecutionStatus& status) { BOOST_CHECK(status.state == expected); complete = true; },
      [](const ndn::Name&) { BOOST_FAIL("actual status query timed out or failed validation"); }, 1000);
    pump(io, [&] { return complete; });
  };
  query(SelectionExecutionState::Received);
  BOOST_REQUIRE_EQUAL(queries.size(), 1);
  BOOST_REQUIRE_EQUAL(replies.size(), 1);
  BOOST_CHECK_GT(originalCertificateReplies, 0);
  const auto original = replies.front().wireEncode();
  BOOST_CHECK(ndn::security::verifySignature(replies.front(), actor));
  InvocationNamesOracle::status(provider, actor.getIdentity(), service, digest, SelectionExecutionState::Completed);
  BOOST_REQUIRE(InvocationNamesOracle::statusReply(provider, queries.front()));
  io.restart(); io.poll();
  BOOST_REQUIRE_EQUAL(replies.size(), 2);
  BOOST_CHECK(replies.back().wireEncode() == original);
  query(SelectionExecutionState::Completed);
  BOOST_REQUIRE_EQUAL(queries.size(), 2);
  BOOST_CHECK(queries.front().getName() != queries.back().getName());
  BOOST_CHECK(!(replies.back().wireEncode() == original));
  auto changed = queries.front();
  changed.setApplicationParameters(encodeSelectionStatusQueryParameters(service, digest + "-changed"));
  changed.setName(queries.front().getName());
  BOOST_CHECK(!InvocationNamesOracle::statusReply(provider, changed));
  const auto params = encodeSelectionStatusQueryParameters(service, digest);
  const auto oldMs = static_cast<uint64_t>(ndn::time::duration_cast<ndn::time::milliseconds>(
    ndn::time::system_clock::now().time_since_epoch()).count()) - 300001;
  ndn::Interest expired(makeControlRpcName(actor.getIdentity(), "SELECTION-STATUS", params,
    std::to_string(oldMs) + "-" + makeOpaqueControlHandle(16)));
  expired.setApplicationParameters(params);
  BOOST_CHECK(!InvocationNamesOracle::statusReply(provider, expired));
  io.restart(); io.poll();
  BOOST_CHECK_EQUAL(replies.size(), 3);
  for (const auto negative : {Fault::Signature, Fault::ForeignSigner}) {
    fault = negative;
    bool rejected = false;
    size_t accepted = 0;
    user.QuerySelectionStatus(actor.getIdentity(), service, digest,
      [&](const SelectionExecutionStatus&) { ++accepted; },
      [&](const ndn::Name&) { rejected = true; }, 10000);
    pump(io, [&] { return rejected; });
    BOOST_CHECK_EQUAL(accepted, 0);
  }
}

BOOST_AUTO_TEST_CASE(WrappedKeySingleFetchRejectsSignerGeometryAndMalformedBlock)
{
  enum class Fault { None, Signer, FinalBlock, Block, TrailingBlock, Timeout };
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  const auto owner = makeRsaIdentity(keys, ndn::Name("/spec201/wrapper/owner"));
  const auto foreign = makeRsaIdentity(keys, ndn::Name("/spec201/wrapper/foreign"));
  ndn::io::save(owner, "spec201-wrapper-owner.cert");
  ndn::io::save(foreign, "spec201-wrapper-foreign.cert");
  {
    std::ofstream schema("spec201-wrapper.conf");
    schema << "rule { id wrapper for data checker { type customized sig-type rsa-sha256 "
              "key-locator { type name regex ^<>*$ } } }\n"
              "trust-anchor { type file file-name spec201-wrapper-owner.cert }\n"
              "trust-anchor { type file file-name spec201-wrapper-foreign.cert }\n";
  }
  const ndn::Name service("/service"), producer = owner.getIdentity();
  const auto attribute = makeAbeServiceAttribute(service, "SERVICE");
  const auto locator = makeHybridMessageKeyDataName(service, producer, attribute, "epoch");
  auto expectedLocator = producer;
  expectedLocator.append("NDNSF").append("KEY").append("kind=HYBRID-MESSAGE")
    .append("SCOPE").append("SERVICE")
    .append(commitment("ndnsf-naming/hybrid-key-scope", {
      {1, 1, wire(service)}, {2, 1, wire(producer)}, {3, 2, utf8(attribute)}}))
    .append("EPOCH").append(commitment("ndnsf-naming/opaque/protection-epoch", {{1, 4, utf8("epoch")}}))
    .append("OBJECT");
  BOOST_CHECK(locator.wireEncode() == expectedLocator.wireEncode());
  for (const auto fault : {Fault::None, Fault::Signer, Fault::FinalBlock, Fault::Block, Fault::TrailingBlock, Fault::Timeout}) {
    bool completed = false, succeeded = false;
    std::string failure;
    size_t outgoing = 0;
    std::vector<ndn::Name> expressedNames;
    boost::asio::io_context io;
    ndn::DummyClientFace face(io, keys);
    ndn::ValidatorConfig validator(face);
    validator.load("spec201-wrapper.conf");
    Bytes plaintext(9000, 0x33);
    ndn::Block envelope(128);
    envelope.push_back(ndn::makeBinaryBlock(129, plaintext.begin(), plaintext.end()));
    envelope.encode();
    Bytes cipher(envelope.data(), envelope.data() + envelope.size());
    if (fault == Fault::Block) cipher = {0x80, 0x03, 0x01};
    if (fault == Fault::TrailingBlock) cipher.push_back(0);
    auto packets = prepareHybridMessageKeyPackets(service, producer, attribute, "epoch",
        ndn::span<const uint8_t>(cipher.data(), cipher.size()), keys,
        ndn::security::signingByIdentity(producer), ndn::time::milliseconds(1000), ndn::tlv::ContentType_Blob);
    BOOST_REQUIRE(!packets.empty());
    BOOST_CHECK(packets.front()->getName().getPrefix(-1) == ndn::Name(locator).append(rawHash(cipher)));
    if (fault == Fault::FinalBlock) {
      BOOST_REQUIRE_EQUAL(packets.size(), 2);
      packets.back()->setFinalBlock(ndn::name::Component::fromSegment(2));
      keys.sign(*packets.back(), ndn::security::signingByIdentity(producer));
    }
    if (fault == Fault::Signer)
      for (auto& packet : packets) keys.sign(*packet, ndn::security::signingByIdentity(foreign.getIdentity()));
    ndn::signal::ScopedConnection connection = face.onSendInterest.connect([&](const ndn::Interest& interest) {
      ++outgoing; expressedNames.push_back(interest.getName());
      if (fault == Fault::Timeout) return;
      if (interest.getName() == locator) face.receive(*packets.front());
      else for (const auto& packet : packets)
        if (interest.getName() == packet->getName()) { face.receive(*packet); break; }
    });
    fetchHybridMessageKeyWrapper(face, validator, locator,
      [producer](const ndn::Data& packet) { return ProducerSignerPolicy().allowsValidatedSigner(packet, producer); },
      [&](const ndn::Name& name, const ndn::Block& block) {
        succeeded = true; completed = true;
        BOOST_CHECK(name == ndn::Name(locator).append(rawHash(cipher)));
        BOOST_CHECK(block == envelope);
      },
      [&](const std::string& error) { failure = error; completed = true; });
    pump(io, [&] { return completed; }, fault == Fault::Timeout ?
        std::chrono::seconds(65) : std::chrono::seconds(3));
    // Failure acknowledgement follows stop's posted self release. This
    // dedicated Face has no SVS or NAC background Interests to confound it.
    io.poll(); io.restart();
    BOOST_CHECK_EQUAL(face.getNPendingInterests(), 0);
    BOOST_CHECK_EQUAL(succeeded, fault == Fault::None);
    if (fault == Fault::FinalBlock) {
      // SegmentFetcher continues its signal stack after stop() posts self
      // release. The inconsistent last packet advertises one additional
      // segment; require that exact same-object request and final pending0.
      BOOST_REQUIRE_EQUAL(expressedNames.size(), packets.size() + 1);
      BOOST_CHECK(expressedNames.back() == ndn::Name(packets.back()->getName().getPrefix(-1)).appendSegment(2));
    }
    else if (fault != Fault::Timeout) BOOST_CHECK_MESSAGE(outgoing <= packets.size(),
      "single-fetch packet bound: fault=" << static_cast<unsigned>(fault)
      << " Interests=" << outgoing << " packets=" << packets.size());
    else { BOOST_CHECK_GT(outgoing, 0); BOOST_CHECK(failure.find("Timeout exceeded") != std::string::npos); }
    if (fault == Fault::Signer) BOOST_CHECK(failure.find("signer is not authorized for Producer") != std::string::npos);
    if (fault == Fault::FinalBlock) BOOST_CHECK(failure.find("FinalBlock") != std::string::npos);
    if (fault == Fault::Block || fault == Fault::TrailingBlock) BOOST_CHECK(failure.find("valid NAC Block") != std::string::npos);
  }
}

BOOST_AUTO_TEST_CASE(ActualReadyHandlerReplaysAuthenticatedQueryWithoutNonceRevalidation)
{
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  const auto owner = makeRsaIdentity(keys, ndn::Name("/spec201/ack/user"));
  const auto provider = makeRsaIdentity(keys, ndn::Name("/spec201/ack/provider"));
  const auto authority = makeRsaIdentity(keys, ndn::Name("/spec201/ack/authority"));
  ndn::io::save(owner, "spec201-ack-user.cert");
  ndn::io::save(provider, "spec201-ack-provider.cert");
  {
    std::ofstream schema("spec201-ack.conf");
    schema << "rule { id signed-call for interest checker { type customized sig-type rsa-sha256 "
              "key-locator { type name regex ^<>*$ } } }\n"
              "rule { id reply for data checker { type hierarchical sig-type rsa-sha256 } }\n"
              "trust-anchor { type file file-name spec201-ack-user.cert }\n"
              "trust-anchor { type file file-name spec201-ack-provider.cert }\n";
  }
  boost::asio::io_context io;
  ndn::DummyClientFace face(io, keys);
  LocalServiceUser user(face, ndn::Name("/spec201/ack/group"), owner, authority, "spec201-ack.conf");
  user.useSigningKeyChainForTest(keys);
  const ndn::Name id = ndn::Name("/request/tail").appendVersion(257);
  const auto handle = makeOpaqueControlHandle(16);
  const auto plan = InvocationNamesOracle::prepareReadyOwner(user, id, provider.getIdentity(), handle);
  const auto now = static_cast<uint64_t>(ndn::time::duration_cast<ndn::time::microseconds>(
      ndn::time::system_clock::now().time_since_epoch()).count());
  ProviderReadyMessage ready;
  for (const auto& field : std::vector<std::pair<std::string, std::string>>{
      {"requestId", id.toUri()}, {"attempt", "1"}, {"selectionDigest", "selection"},
      {"deploymentPlanDigest", plan.computeDigest()}, {"providerIdentity", provider.getIdentity().toUri(ndn::name::UriFormat::ALTERNATE)},
      {"providerBootEpoch", "boot"}, {"role", "/role"}, {"deploymentInstanceId", "deployment"},
      {"operationId", "operation"}, {"readySequence", "1"}, {"issuedAtUs", std::to_string(now)},
      {"expiresAtUs", std::to_string(now + 60000000)}}) ready.setField(field.first, field.second);
  ndn::security::InterestSigner signer(keys);
  const auto prepare = [&](const ProviderReadyMessage& message, const ndn::Name& caller) {
    const auto parameters = message.WireEncode();
    ndn::Interest request(makeControlRpcName(owner.getIdentity(), "PROVIDER-READY", parameters, handle));
    request.setApplicationParameters(parameters);
    signer.makeSignedInterest(request, ndn::security::signingByIdentity(caller));
    return request;
  };
  const auto request = prepare(ready, provider.getIdentity());
  BOOST_REQUIRE(parseControlRpcName(request, owner.getIdentity(), "PROVIDER-READY"));
  BOOST_REQUIRE(InvocationNamesOracle::ready(user, request));
  pump(io, [&] { return !face.sentData.empty(); });
  BOOST_REQUIRE_EQUAL(InvocationNamesOracle::readyCount(user, id), 1);
  const auto first = face.sentData.back();
  BOOST_CHECK(first.getName() == request.getName());
  BOOST_REQUIRE(ndn::security::verifySignature(first, owner));
  ReadyAcknowledgement acknowledgement;
  BOOST_REQUIRE(acknowledgement.WireDecode(first.getContent()));
  BOOST_CHECK_EQUAL(acknowledgement.getField("accepted"), "true");
  const auto count = face.sentData.size();
  auto retry = request; retry.refreshNonce(); retry.setInterestLifetime(ndn::time::milliseconds(700));
  BOOST_REQUIRE(InvocationNamesOracle::ready(user, retry));
  pump(io, [&] { return face.sentData.size() == count + 1; });
  BOOST_CHECK(face.sentData.back().wireEncode() == first.wireEncode());
  BOOST_CHECK_EQUAL(InvocationNamesOracle::readyCount(user, id), 1);
  const auto noReply = [&] (const ndn::Interest& invalid) {
    const auto before = face.sentData.size();
    InvocationNamesOracle::ready(user, invalid);
    io.poll(); io.restart();
    BOOST_CHECK_EQUAL(face.sentData.size(), before);
    BOOST_CHECK_EQUAL(InvocationNamesOracle::readyCount(user, id), 1);
  };
  auto changed = request;
  changed.setApplicationParameters(encodeControlQueryParameters({ndn::Name("/changed"), {}, 0, 0}));
  changed.setName(request.getName()); // Neither complete digest nor call commitment matches.
  noReply(changed);
  auto badSignature = request;
  const auto signature = request.getSignatureValue();
  Bytes damaged(signature.value(), signature.value() + signature.value_size());
  BOOST_REQUIRE(!damaged.empty()); damaged.front() ^= 1;
  badSignature.setSignatureValue(ndn::span<const uint8_t>(damaged.data(), damaged.size()));
  noReply(badSignature);
  noReply(prepare(ready, owner.getIdentity())); // Crypto-valid, wrong caller.
  auto expired = ready; expired.setField("expiresAtUs", std::to_string(now - 1));
  noReply(prepare(expired, provider.getIdentity()));
  InvocationNamesOracle::terminal(user, id, true);
  noReply(request);
  InvocationNamesOracle::terminal(user, id, false);
  InvocationNamesOracle::revokeReadyOwner(user);
  noReply(request);
}


BOOST_AUTO_TEST_CASE(ActualActivationHandlerExecutesOnceAndReplaysOriginalAuthenticatedAck)
{
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  const auto user = makeRsaIdentity(keys, ndn::Name("/spec201/activate/user"));
  const auto actor = makeRsaIdentity(keys, ndn::Name("/spec201/activate/provider"));
  const auto aa = makeRsaIdentity(keys, ndn::Name("/spec201/activate/authority"));
  ndn::io::save(user, "spec201-activate-user.cert");
  ndn::io::save(actor, "spec201-activate-provider.cert");
  {
    std::ofstream schema("spec201-activate.conf");
    schema << "rule { id call for interest checker { type customized sig-type rsa-sha256 "
              "key-locator { type name regex ^<>*$ } } }\n"
              "rule { id reply for data checker { type hierarchical sig-type rsa-sha256 } }\n"
              "trust-anchor { type file file-name spec201-activate-user.cert }\n"
              "trust-anchor { type file file-name spec201-activate-provider.cert }\n";
  }
  boost::asio::io_context io;
  ndn::DummyClientFace face(io, keys);
  const ndn::Name service("/service"), id("/activate/request/tail");
  size_t prepared = 0, ready = 0;
  std::atomic<size_t> executed{0};
  LocalServiceProvider provider(face, ndn::Name("/spec201/activate/group"), actor, aa,
                                "spec201-activate.conf");
  provider.useSigningKeyChainForTest(keys);
  provider.setUseTokens(false);
  provider.addService(service, ServiceProvider::RequestHandler(
    [&](const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
        const RequestMessage&) { ++executed; ResponseMessage response; response.setStatus(true); return response; }));
  provider.setDeploymentPrepareHandler(
    [&](const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
        const RequestMessage&, const DeploymentPlan&, const std::string&) {
      ++prepared; ProviderReadyMessage result;
      result.setField("artifactDigest", digestText('a'));
      result.setField("deploymentInstanceId", "instance"); result.setField("operationId", "prepare");
      return result;
    });
  provider.setProviderReadyPublisher([&](const ndn::Name&, const ProviderReadyMessage&) { ++ready; });
  DeploymentIntent intent; intent.setField("artifactDigest", digestText('a'));
  RequestMessage request; request.setDeploymentIntent(intent);
  provider.addPendingRequestForTokenTest(user.getIdentity(), service, id, request, "");
  DeploymentPlan plan;
  plan.setField("requestId", id.toUri()); plan.setField("attempt", "1");
  plan.setField("requesterIdentity", user.getIdentity().toUri());
  plan.setField("intentDigest", intent.computeDigest());
  plan.setField("member.0.provider", actor.getIdentity().toUri()); plan.setField("member.0.role", "worker");
  ServiceSelectionMessage selection; selection.setRequestIDs({id.toUri()}); selection.setDeploymentPlan(plan);
  const auto selectionBlock = selection.WireEncode();
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(user.getIdentity(), actor.getIdentity(),
      service, id, ndn::Buffer(selectionBlock.data(), selectionBlock.size()));
  BOOST_REQUIRE_EQUAL(prepared, 1); BOOST_REQUIRE_EQUAL(ready, 1); BOOST_CHECK_EQUAL(executed.load(), 0);
  ExecutionActivateMessage activation;
  const auto now = static_cast<uint64_t>(ndn::time::duration_cast<ndn::time::microseconds>(
      ndn::time::system_clock::now().time_since_epoch()).count());
  for (const auto& field : std::vector<std::pair<std::string, std::string>>{
      {"requestId", id.toUri(ndn::name::UriFormat::ALTERNATE)}, {"selectionDigest", computeSelectionDigest(selection)},
      {"deploymentPlanDigest", plan.computeDigest()}, {"readySetDigest", "ready-set"},
      {"memberSetDigest", "members"}, {"requesterIdentity", user.getIdentity().toUri(ndn::name::UriFormat::ALTERNATE)},
      {"activationSequence", "1"}, {"expiresAtUs", std::to_string(now + 60000000)}})
    activation.setField(field.first, field.second);
  ndn::security::InterestSigner signer(keys);
  const auto handle = makeOpaqueControlHandle(16);
  const auto prepare = [&](const ExecutionActivateMessage& value, const ndn::Name& caller) {
    const auto params = value.WireEncode();
    ndn::Interest call(makeControlRpcName(actor.getIdentity(), "EXECUTION-ACTIVATE", params, handle));
    call.setApplicationParameters(params); signer.makeSignedInterest(call, ndn::security::signingByIdentity(caller));
    return call;
  };
  const auto call = prepare(activation, user.getIdentity());
  BOOST_REQUIRE(InvocationNamesOracle::activate(provider, call));
  pump(io, [&] { return std::any_of(face.sentData.begin(), face.sentData.end(),
    [&](const ndn::Data& data) { return data.getName() == call.getName(); }); });
  const auto findAck = [&]() -> ndn::Data {
    for (auto it = face.sentData.rbegin(); it != face.sentData.rend(); ++it)
      if (it->getName() == call.getName()) return *it;
    throw std::runtime_error("activation ACK absent");
  };
  const auto original = findAck();
  BOOST_REQUIRE(ndn::security::verifySignature(original, actor));
  ReadyAcknowledgement ack; BOOST_REQUIRE(ack.WireDecode(original.getContent()));
  BOOST_CHECK_EQUAL(ack.getField("accepted"), "true");
  pump(io, [&] { return executed.load() == 1; });
  auto retry = call; retry.refreshNonce(); retry.setInterestLifetime(ndn::time::milliseconds(500));
  const auto acknowledgementCount = [&] {
    return std::count_if(face.sentData.begin(), face.sentData.end(), [&](const ndn::Data& packet) {
      const auto parsed = parseProducerDataName(packet.getName(), actor.getIdentity());
      return parsed && parsed->category == "RPC" && !parsed->suffix.empty() &&
        parsed->suffix[0] == ndn::name::Component("EXECUTION-ACTIVATE");
    });
  };
  const auto count = acknowledgementCount();
  BOOST_REQUIRE(InvocationNamesOracle::activate(provider, retry));
  pump(io, [&] { return acknowledgementCount() == count + 1; });
  BOOST_CHECK(findAck().wireEncode() == original.wireEncode()); BOOST_CHECK_EQUAL(executed.load(), 1);
  const auto noAck = [&](const ndn::Interest& invalid) {
    const auto before = acknowledgementCount();
    InvocationNamesOracle::activate(provider, invalid); io.poll(); io.restart();
    BOOST_CHECK_EQUAL(acknowledgementCount(), before); BOOST_CHECK_EQUAL(executed.load(), 1);
  };
  auto changed = call; changed.setApplicationParameters(encodeControlQueryParameters({ndn::Name("/changed"), {}, 0, 0}));
  changed.setName(call.getName()); noAck(changed);
  auto bad = call; const auto signature = bad.getSignatureValue();
  Bytes damaged(signature.value(), signature.value() + signature.value_size());
  BOOST_REQUIRE(!damaged.empty()); damaged.front() ^= 1;
  bad.setSignatureValue(ndn::span<const uint8_t>(damaged.data(), damaged.size())); noAck(bad);
  noAck(prepare(activation, actor.getIdentity()));
  auto expired = activation; expired.setField("expiresAtUs", std::to_string(now - 1)); noAck(prepare(expired, user.getIdentity()));
  InvocationNamesOracle::retireActivation(provider, computeSelectionDigest(selection)); noAck(call);
}


BOOST_AUTO_TEST_CASE(CertificatePublisherServesOriginalControllerIssuedSubjectWire)
{
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  const auto controller = keys.createIdentity(ndn::Name("/spec201/cert/controller"), ndn::RsaKeyParams(2048));
  const auto subject = keys.createIdentity(ndn::Name("/spec201/cert/provider"), ndn::RsaKeyParams(2048));
  const auto foreign = keys.createIdentity(ndn::Name("/spec201/cert/foreign"), ndn::RsaKeyParams(2048));
  const auto certificate = keys.makeCertificate(subject.getDefaultKey(),
      ndn::security::signingByIdentity(controller.getName()));
  keys.addCertificate(subject.getDefaultKey(), certificate);
  keys.setDefaultCertificate(subject.getDefaultKey(), certificate);
  BOOST_REQUIRE(isCertificateSignedByIdentity(certificate, controller.getName()));
  BOOST_REQUIRE(ndn::security::verifySignature(certificate, controller.getDefaultKey()));
  const auto wrong = keys.makeCertificate(subject.getDefaultKey(),
      ndn::security::signingByIdentity(foreign.getName()));
  BOOST_CHECK(!isCertificateSignedByIdentity(wrong, controller.getName()));
  BOOST_CHECK(!ndn::security::verifySignature(wrong, controller.getDefaultKey()));
  boost::asio::io_context io;
  ndn::DummyClientFace::Options options; options.enableRegistrationReply = true;
  ndn::DummyClientFace publisherFace(io, keys, options), consumerFace(io, keys, options);
  publisherFace.linkTo(consumerFace);
  ndn::io::save(controller.getDefaultKey().getDefaultCertificate(), "spec201-certificate-controller.cert");
  ndn::io::save(foreign.getDefaultKey().getDefaultCertificate(), "spec201-certificate-foreign.cert");
  {
    std::ofstream schema("spec201-issued-certificate.conf");
    schema << "rule { id issued-certificate for data filter { type name name \""
           << certificate.getName().toUri(ndn::name::UriFormat::CANONICAL)
           << "\" relation equal } checker { type customized sig-type rsa-sha256 "
              "key-locator { type name name \""
           << controller.getName().toUri(ndn::name::UriFormat::CANONICAL)
           << "\" relation equal } } }\n"
              "trust-anchor { type file file-name spec201-certificate-controller.cert }\n"
              "trust-anchor { type file file-name spec201-certificate-foreign.cert }\n";
  }
  auto configured = std::make_shared<MessageValidator>("spec201-issued-certificate.conf",
                                                       std::nullopt, &consumerFace);
  size_t rejected = 0;
  auto forged = certificate;
  keys.sign(forged, ndn::security::signingByIdentity(foreign.getName()));
  BOOST_REQUIRE(ndn::security::verifySignature(forged, foreign.getDefaultKey()));
  configured->validateWithConfiguredTrustSchema(forged,
      [](const ndn::Data&) { BOOST_FAIL("foreign signer admitted for issued subject certificate"); },
      [&](const ndn::Data&, const ndn::security::ValidationError&) { ++rejected; });
  pump(io, [&] { return rejected == 1; });
  CertificatePublisher publisher(publisherFace, keys, certificate.getName());
  // Drain the actual DummyFace registration reply before sending the first
  // consumer Interest; the broadcast link has no NFD PIT to retain it.
  io.poll(); io.restart();
  size_t received = 0;
  const auto fetch = [&] {
    consumerFace.expressInterest(ndn::Interest(certificate.getName()).setCanBePrefix(false),
      [&](const ndn::Interest&, const ndn::Data& packet) {
        configured->validateWithConfiguredTrustSchema(packet,
          [&](const ndn::Data& validated) {
            BOOST_CHECK(validated.wireEncode() == certificate.wireEncode()); ++received;
          }, [](const ndn::Data&, const ndn::security::ValidationError& error) {
            BOOST_FAIL("original issued certificate validation failed: " << error);
          });
      }, [](const ndn::Interest&, const ndn::lp::Nack&) { BOOST_FAIL("certificate Nack"); },
      [](const ndn::Interest&) { BOOST_FAIL("certificate timeout"); });
  };
  fetch(); pump(io, [&] { return received == 1; });
  fetch(); pump(io, [&] { return received == 2; });
  BOOST_CHECK(publisher.getRegisteredPrefix() == certificate.getKeyName());
}

BOOST_AUTO_TEST_SUITE_END()
