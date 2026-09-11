#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCatalogModelAdapter.hpp"
#include <openssl/evp.h>
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV3.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"

#include <boost/test/unit_test.hpp>
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"
#include <future>
#include <deque>
#include <limits>
#include <thread>
#include <fstream>
#include <cstdlib>
#include <mutex>

namespace ndnsf::di {
// This friend is defined only in the unit test; no configurable production
// constructor or installed test-clock factory is exposed by the library.
class NativeClientTestAccess {
public:
  using Port = NativeInferenceClient::TestPort;
  static std::unique_ptr<NativeInferenceClient> create(
      const Port& port, std::shared_ptr<ndn_service_framework::ServiceUser> user,
      std::shared_ptr<const NativeAdapterRegistry> adapters,
      std::shared_ptr<NativeRequestPreparation> preparation = nullptr) {
    return std::unique_ptr<NativeInferenceClient>(
      new NativeInferenceClient(port, std::move(user), std::move(adapters),
                                nullptr, nullptr, std::move(preparation)));
  }
  static std::size_t operationEntryCount(const NativeInferenceClient& client)
  {
    std::lock_guard<std::mutex> lock(client.m_mutex);
    return client.m_operations.size();
  }
};
}

using namespace ndnsf::di;

BOOST_AUTO_TEST_SUITE(Spec182NativeInferenceClient)

BOOST_AUTO_TEST_CASE(EmptyHandleFailsClosedAndErrorKeepsStructuredIdentity)
{
  NativeInferenceHandle handle;
  BOOST_CHECK_THROW(handle.status(), NativeDiError);
  BOOST_CHECK_THROW(handle.result(std::chrono::milliseconds(0)), NativeDiError);
  BOOST_CHECK(!handle.conversationCheckpoint().has_value());

  NativeDiError error("INVALID_REQUEST", "local", "request", "bad input",
                      "/NDNSF/DI/REQUEST/1", 1);
  BOOST_CHECK_EQUAL(error.code(), "INVALID_REQUEST");
  BOOST_CHECK_EQUAL(error.domain(), "local");
  BOOST_CHECK_EQUAL(error.boundary(), "request");
  BOOST_CHECK_EQUAL(error.requestId(), "/NDNSF/DI/REQUEST/1");
  BOOST_CHECK_EQUAL(error.attempt(), 1U);
}

BOOST_AUTO_TEST_CASE(ClientRejectsMissingCoreOwner)
{
  auto adapters = std::make_shared<NativeAdapterRegistry>();
  BOOST_CHECK_THROW(
    NativeInferenceClient(nullptr, adapters), NativeDiError);
  BOOST_CHECK_THROW(
    NativeInferenceClient(nullptr, nullptr), NativeDiError);
}

BOOST_AUTO_TEST_SUITE_END()

namespace {
class ClientTestAdapter final : public NativeModelAdapter {
public:
  std::function<void(const std::vector<std::uint8_t>&)> onEncode;
  std::string adapterId() const override { return "client-test"; }
  std::string adapterVersion() const override { return "1"; }
  NativeModelDescriptor inspect(const std::string&, const std::string&) const override
  { return {}; }
  std::vector<std::uint8_t> encodeInput(const std::vector<std::uint8_t>& x) const override
  { if (onEncode) onEncode(x); return x; }
  std::vector<std::uint8_t> decodeResult(const std::vector<std::uint8_t>& x) const override
  { return x; }
};
class ClientTestSplit final : public NativeModelSplitStrategy {
public:
  NativeStrategyIdentity identity() const override { return {"test", "1", nativePlanningDigest("{}")} ; }
  std::vector<NativeSplitCandidate> enumerate(const NativeModelDescriptor&,
      const NativeGraphSnapshot&, const NativeCandidateBudget&) const override { return {}; }
};
struct ClientStateFixture {
  ndn::security::KeyChain keyChain{"pib-memory:", "tpm-memory:"};
  ndn::DummyClientFace face{keyChain};
  std::shared_ptr<ndn_service_framework::test::LocalServiceUser> user;
  std::shared_ptr<NativeAdapterRegistry> adapters = std::make_shared<NativeAdapterRegistry>();
  std::shared_ptr<ClientTestAdapter> clientAdapter;
  std::deque<std::function<void()>> work;
  struct Timer {
    std::chrono::steady_clock::time_point deadline;
    std::function<void()> fire;
    bool cancelled = false;
  };
  std::vector<std::shared_ptr<Timer>> timers;
  std::chrono::steady_clock::time_point now{};
  NativeModelRef model;
  ClientStateFixture() {
    using namespace ndn_service_framework::test;
    auto cert = makeRsaIdentity(keyChain, ndn::Name("/client-test/user"));
    auto aa = makeRsaIdentity(keyChain, ndn::Name("/client-test/aa"));
    user = std::make_shared<LocalServiceUser>(face, ndn::Name("/client-test"), cert, aa,
                                             "examples/trust-any.conf");
    clientAdapter = std::make_shared<ClientTestAdapter>();
    adapters->registerAdapter(clientAdapter);
    adapters->freeze();
    model.modelName = "client-model";
    model.contentDigest = nativePlanningDigest("model");
    model.semanticsDigest = nativePlanningDigest("semantics");
    model.graphDigest = nativePlanningDigest("graph");
    model.modelFormat = "onnx";
    model.precision = "float32";
    model.adapterId = "client-test";
    model.adapterVersion = "1";
    model.adapter = fixture::modelAdapter(model.adapterId, model.adapterVersion, model.modelFormat, model.precision);
  }
  NativeClientTestAccess::Port port() {
    return {[this] { return now; },
      [this](std::function<void()> f) { work.push_back(std::move(f)); },
      [this](std::chrono::steady_clock::time_point deadline, std::function<void()> fire) {
        auto timer = std::make_shared<Timer>(Timer{deadline, std::move(fire)});
        timers.push_back(timer);
        return [timer] { timer->cancelled = true; timer->fire = {}; };
      }};
  }
  NativeInferenceHandle request(NativeInferenceClient& client, NativeRequestOptions options = {}) {
    NativeApplicationInput input;
    input.payload = {1};
    return client.request(model, input, std::make_shared<ClientTestSplit>(),
                          std::make_shared<NativePreSplitFirstPlacement>(), options);
  }
};
}

BOOST_AUTO_TEST_SUITE(Spec182NativeRequestIdentity)

BOOST_AUTO_TEST_CASE(ProductionRequestIdsCarryProcessOwnerScope)
{
  ClientStateFixture fixture;
  auto first = std::make_unique<NativeInferenceClient>(fixture.user, fixture.adapters);
  auto second = std::make_unique<NativeInferenceClient>(fixture.user, fixture.adapters);

  auto firstHandle = fixture.request(*first);
  auto secondHandle = fixture.request(*second);
  BOOST_CHECK_NE(firstHandle.requestId(), secondHandle.requestId());
  BOOST_CHECK(firstHandle.requestId().find("/NDNSF/DI/REQUEST/") == 0);
  const auto firstSuffix = firstHandle.requestId().substr(std::string("/NDNSF/DI/REQUEST/").size());
  const auto secondSuffix = secondHandle.requestId().substr(std::string("/NDNSF/DI/REQUEST/").size());
  BOOST_REQUIRE(firstSuffix.size() > 33);
  BOOST_REQUIRE(secondSuffix.size() > 33);
  BOOST_CHECK_EQUAL(firstSuffix[32], '-');
  BOOST_CHECK_EQUAL(secondSuffix[32], '-');
  BOOST_CHECK_NE(firstSuffix.substr(0, 32), secondSuffix.substr(0, 32));
  BOOST_CHECK(firstSuffix.substr(0, 32).find_first_not_of("0123456789abcdef") ==
              std::string::npos);
  BOOST_CHECK(firstSuffix.substr(33).find_first_not_of("0123456789") ==
              std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(Spec182ClientState, ClientStateFixture)

BOOST_AUTO_TEST_CASE(RequestEnvelopeMatchesSdkIdentityAndNativeProviderInput)
{
  model.modelName = "模型";
  NativeRequestContract contract{"/di/infer", "task", model.adapterId,
    model.adapter.descriptorDigest(), nativePlanningDigest("composition"), nativePlanningDigest("task")};
  NativeApplicationInput input;
  input.taskName = "task";
  input.inputSchemaDigest = model.adapter.inputSchemaDigest;
  input.optionsSchemaDigest = model.adapter.optionsSchemaDigest;
  input.payload = {0, 1, 255};
  input.options = {123, 125};
  auto encoded = encodeNativeRequestEnvelope(model, input, contract, "/request/1", 1, 10000);
  BOOST_CHECK_EQUAL(encoded.modelIntentDigest,
    "sha256:aae63c9a7d12327b3a5446a2c29130fd1c9779d6635d325593eea0a04dcff0dd");
  BOOST_CHECK_EQUAL(encoded.invocationId, "invocation:6b74ae8ab27493e15debe52670591432");
  BOOST_CHECK(encoded.modelIntentDigest != model.contentDigest);
  auto wire = NativeJson::parse(encoded.wire);
  BOOST_CHECK_EQUAL(wire.at("input_payload_b64").get<std::string>(), "AAH/");
  BOOST_CHECK_EQUAL(wire.at("options_payload_b64").get<std::string>(), "e30=");
  BOOST_CHECK(wire.at("model").at("source_revision").is_null());
  BOOST_CHECK_EQUAL(encoded.requestContractDigest,
    nativePlanningDigest(encoded.wire.data(), encoded.wire.size()));
  NativeProviderOfferV3Config provider;
  provider.provider = "/provider/1"; provider.service = contract.serviceName;
  provider.bootEpoch = "boot-1"; provider.signerKeyId = nativePlanningDigest("key-1");
  provider.acceptedRoles = {"worker"}; provider.backends = {"onnxruntime"};
  provider.devices = {"cpu"};
  unsigned signedOffers = 0;
  provider.signDigest = [&](const auto&) { ++signedOffers; return std::string(128, 'a'); };
  const auto offer = issueNativeProviderOfferV3(encoded.wire, provider, 1000);
  BOOST_REQUIRE(offer);
  BOOST_CHECK(offer->status);
  BOOST_CHECK_EQUAL(signedOffers, 1U);
  BOOST_CHECK_EQUAL(offer->pendingStateTtlMs, 9000U);
  const auto expired = issueNativeProviderOfferV3(encoded.wire, provider, 10000);
  BOOST_REQUIRE(expired);
  BOOST_CHECK(!expired->status);
  BOOST_CHECK_EQUAL(signedOffers, 1U);
  if (const auto path = std::getenv("NDNSF_REQUEST_ENVELOPE_ORACLE_OUTPUT")) {
    std::ofstream out(path);
    BOOST_REQUIRE(out.good());
    out << std::string(encoded.wire.begin(), encoded.wire.end()) << '\n';
  }

  input.transportMode = NativeInputTransportMode::RepositoryReference;
  input.payload.clear();
  input.repositoryReference = nativeCanonicalJson({{"dataName", "/encrypted/input"},
    {"encrypted", true}, {"plaintextSize", 3}, {"authorizationScope", "scope"},
    {"protectionEpoch", "epoch-1"}, {"manifestDigest", nativePlanningDigest("manifest")},
    {"ciphertextDigest", nativePlanningDigest("ciphertext")}});
  encoded = encodeNativeRequestEnvelope(model, input, contract, "/request/1", 1, 10000);
  if (const auto path = std::getenv("NDNSF_REQUEST_ENVELOPE_ORACLE_OUTPUT")) {
    std::ofstream out(path, std::ios::app);
    BOOST_REQUIRE(out.good());
    out << std::string(encoded.wire.begin(), encoded.wire.end()) << '\n';
  }
  wire = NativeJson::parse(encoded.wire);
  BOOST_CHECK_EQUAL(wire.at("input_transport").get<std::string>(), "REPO_REF");
  BOOST_CHECK_EQUAL(wire.at("input_payload_b64").get<std::string>(), "");
  input.payload = {1};
  BOOST_CHECK_THROW(encodeNativeRequestEnvelope(model, input, contract, "/r", 1, 10000), std::invalid_argument);
  input.payload.clear();
  contract.adapterDescriptorDigest = nativePlanningDigest("foreign-adapter");
  BOOST_CHECK_THROW(encodeNativeRequestEnvelope(model, input, contract, "/r", 1, 10000), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ConfiguredClientClosesEmptyAckAndCancelsActualCorePendingCall)
{
  // Exercise the public runtime constructor and worker/Core callbacks. Only
  // model inspection and local Core transport are fixtures; no success or
  // authenticated network response is simulated by this lifecycle test.
  auto configuredAdapters = std::make_shared<NativeAdapterRegistry>();
  configuredAdapters->registerAdapter(std::make_shared<NativeCatalogModelAdapter>(
    std::vector<NativeModelDescriptor>{model}, NativeCatalogModelAdapter::Format::OpaqueBytes, 1024));
  configuredAdapters->freeze();
  std::ifstream file("tests/fixtures/spec182/placement-v3-oracle.json");
  BOOST_REQUIRE(file.good());
  const auto oracle = NativeJson::parse(file);
  auto admission = std::make_shared<NativeOfferAdmission>(oracle.at("policy").dump(),
    std::map<std::string, std::string>{{oracle.at("key_id"), oracle.at("public_pem")}}, oracle.at("candidate"));
  const auto makeKey = [](char seed) {
    const std::string bytes(32, seed);
    return std::shared_ptr<EVP_PKEY>(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
      reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()), EVP_PKEY_free);
  };
  NativeGrantIssuerConfig issuer;
  issuer.authorityIdentity = "/client-test/aa";
  issuer.requesterIdentity = "/client-test/user";
  issuer.protectionEpoch = "test-epoch"; issuer.keyId = "test-key";
  issuer.authorityPrivateKey = makeKey('a'); issuer.requesterPublicKey = makeKey('b');
  issuer.allowedModelManifests = {nativePlanningDigest("manifest")};
  issuer.recipientPublicKeys = {{"/provider", makeKey('c')}};
  issuer.contentKey = [](const auto&, const auto&) { return std::vector<std::uint8_t>(32, 42); };
  std::string authorityPublic(32, '\0'); std::size_t size = authorityPublic.size();
  BOOST_REQUIRE_EQUAL(EVP_PKEY_get_raw_public_key(issuer.authorityPrivateKey.get(),
    reinterpret_cast<unsigned char*>(authorityPublic.data()), &size), 1);
  NativeRequestRuntime runtime;
  runtime.contract = {"/generic/work", "task", model.adapterId, model.adapter.descriptorDigest(),
    nativePlanningDigest("composition"), nativePlanningDigest("task")};
  runtime.requesterIdentity = issuer.requesterIdentity; runtime.protectionEpoch = issuer.protectionEpoch;
  runtime.security = {nativePlanningDigest("policy"), true};
  runtime.grants = std::make_shared<NativeAuthenticatedGrantClient>(issuer.requesterIdentity,
    issuer.requesterPublicKey, issuer.authorityIdentity, authorityPublic,
    std::make_shared<NativeArtifactGrantIssuer>(issuer), user);
  auto preparation = std::make_shared<NativeRequestPreparation>(configuredAdapters,
    [](const NativePreparedInput&, const NativeModelDescriptor& descriptor) {
      NativeGraphSnapshot graph;
      graph.graphDigest = descriptor.graphDigest;
      graph.nodes = {{"node", "Identity", 0}}; graph.topologicalOrder = {"node"};
      return NativeInspectedModel{descriptor, graph, "/fixture/source", nativePlanningDigest("source"),
        nativePlanningDigest("manifest"), nativePlanningDigest("canonical-graph")};
    });
  auto invalidContract = runtime.contract;
  invalidContract.generationMode = "UNSUPPORTED";
  BOOST_CHECK_EXCEPTION(
    NativeInferenceClient(user, configuredAdapters, invalidContract,
                          preparation, admission),
    NativeDiError,
    [](const auto& error) {
      return error.code() == "INVALID_CLIENT_CONFIGURATION" &&
             std::string(error.what()).find("generation mode") != std::string::npos;
    });
  NativeApplicationInput application;
  application.taskName = "task"; application.payload = {1};
  application.inputSchemaDigest = model.adapter.inputSchemaDigest;
  application.optionsSchemaDigest = model.adapter.optionsSchemaDigest;
  NativeRequestOptions diagnosticGeneration;
  diagnosticGeneration.stream = ndn_service_framework::StreamRequestOptions{};
  diagnosticGeneration.generation = NativeGenerationExecutionContractV1{};
  NativeInferenceClient diagnosticClient(user, configuredAdapters, runtime,
                                         preparation, admission);
  BOOST_CHECK_EXCEPTION(
    diagnosticClient.request(model, application,
      std::make_shared<ClientTestSplit>(),
      std::make_shared<NativePreSplitFirstPlacement>(), diagnosticGeneration),
    NativeDiError,
    [](const auto& error) {
      return error.code() == "INVALID_GENERATION_OPTIONS" &&
             std::string(error.what()).find("TOKEN_STREAMING runtime contract") != std::string::npos;
    });
  auto streamingRuntime = runtime;
  streamingRuntime.contract.generationMode = "TOKEN_STREAMING";
  streamingRuntime.contract.tokenizerDigest = nativePlanningDigest("tokenizer");
  NativeInferenceClient streamingClient(user, configuredAdapters, streamingRuntime,
                                        preparation, admission);
  NativeRequestOptions mismatchedGeneration;
  mismatchedGeneration.stream = ndn_service_framework::StreamRequestOptions{};
  mismatchedGeneration.stream->generationId.fill(0x11);
  mismatchedGeneration.generation = NativeGenerationExecutionContractV1{};
  mismatchedGeneration.generation->generationId = std::string(32, '2');
  BOOST_CHECK_EXCEPTION(
    streamingClient.request(model, application,
      std::make_shared<ClientTestSplit>(),
      std::make_shared<NativePreSplitFirstPlacement>(), mismatchedGeneration),
    NativeDiError,
    [](const auto& error) {
      return error.code() == "INVALID_GENERATION_OPTIONS" &&
             std::string(error.what()).find("stream identity") != std::string::npos;
    });
  const auto pumpUntil = [&](const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
      face.getIoContext().restart(); face.getIoContext().poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
  };
  for (const bool cancel : {false, true}) {
    NativeInferenceClient client(user, configuredAdapters, runtime, preparation, admission);
    NativeRequestOptions requestOptions;
    requestOptions.applicationRequestId = "qwen-wire-id";
    auto handle = client.request(model, application, std::make_shared<ClientTestSplit>(),
      std::make_shared<NativePreSplitFirstPlacement>(), requestOptions);
    BOOST_CHECK_EQUAL(handle.applicationRequestId(), "qwen-wire-id");
    const ndn::Name id(handle.requestId());
    BOOST_REQUIRE(pumpUntil([&] { return user->hasPendingCall(id) || handle.status() != NativeRequestStatus::Pending; }));
    if (handle.status() != NativeRequestStatus::Pending) handle.result(std::chrono::milliseconds(0));
    BOOST_REQUIRE(handle.status() == NativeRequestStatus::Pending);
    BOOST_REQUIRE(user->hasPendingCall(id));
    if (cancel) handle.cancel();
    else {
      user->postToIo([&, id] { BOOST_CHECK(user->closeDeferredAcksForTest(id)); });
    }
    BOOST_REQUIRE(pumpUntil([&] { return handle.status() != NativeRequestStatus::Pending && !user->hasPendingCall(id); }));
    if (cancel) {
      BOOST_CHECK(handle.status() == NativeRequestStatus::Cancelled);
      BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
        [](const auto& error) { return error.code() == "CANCELLED"; });
    }
    else {
      BOOST_CHECK(handle.status() == NativeRequestStatus::Failed);
      BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
        [](const auto& error) { return error.code() == "NATIVE_REQUEST_STAGE_FAILED" && error.boundary() == "ACK_CLOSED"; });
    }
    client.close();
    handle.cancel(); // Repeated terminal actions must not recreate Core state.
    face.getIoContext().restart(); face.getIoContext().poll();
    BOOST_CHECK(!user->hasPendingCall(id));
  }
}

BOOST_AUTO_TEST_CASE(ExpiredOperationEntriesAreCompactedForLongLivedClient)
{
  now = std::chrono::steady_clock::now();
  auto client = NativeClientTestAccess::create(
    port(), user, adapters, std::make_shared<NativeRequestPreparation>(adapters));
  for (std::size_t i = 0; i < 128; ++i) {
    {
      auto handle = request(*client);
      BOOST_REQUIRE_EQUAL(work.size(), 1U);
      auto dispatch = std::move(work.front());
      work.pop_front();
      dispatch();
      BOOST_CHECK(handle.status() == NativeRequestStatus::Failed);
    }
  }
  BOOST_CHECK_LE(NativeClientTestAccess::operationEntryCount(*client), 1U);
  client->close();
}

BOOST_AUTO_TEST_CASE(ClientCloseCancelsPendingOperationAfterHandleDrop)
{
  now = std::chrono::steady_clock::now();
  std::atomic<std::size_t> encodeCalls{0};
  clientAdapter->onEncode = [&](const auto&) { ++encodeCalls; };
  auto client = NativeClientTestAccess::create(
    port(), user, adapters, std::make_shared<NativeRequestPreparation>(adapters));
  auto handle = request(*client);
  const auto requestId = handle.requestId();
  handle = NativeInferenceHandle{};

  // The public handle no longer owns the operation, but client close must
  // still cancel it before the queued preparation callback can run.
  client->close();
  BOOST_REQUIRE_EQUAL(work.size(), 1U);
  auto dispatch = std::move(work.front());
  work.pop_front();
  dispatch();
  BOOST_CHECK_EQUAL(encodeCalls.load(), 0U);
  BOOST_CHECK(!user->hasPendingCall(ndn::Name(requestId)));
}

BOOST_AUTO_TEST_CASE(SubmissionOwnsInputsAndStrategiesBeforeWorkerPreparation)
{
  now = std::chrono::steady_clock::now();
  auto adapter = std::make_shared<ClientTestAdapter>();
  auto registry = std::make_shared<NativeAdapterRegistry>();
  registry->registerAdapter(adapter);
  registry->freeze();
  std::vector<std::uint8_t> encoded;
  adapter->onEncode = [&](const auto& bytes) { encoded = bytes; };
  auto client = NativeClientTestAccess::create(port(), user, registry,
    std::make_shared<NativeRequestPreparation>(registry));
  NativeApplicationInput input;
  input.taskName = "inference";
  input.inputSchemaDigest = nativePlanningDigest("input-schema");
  input.optionsSchemaDigest = nativePlanningDigest("options-schema");
  input.payload = {1, 2, 3};
  NativeRequestOptions options;
  options.taskName = input.taskName;
  auto split = std::make_shared<ClientTestSplit>();
  std::weak_ptr<const NativeModelSplitStrategy> retained = split;
  auto handle = client->request(model, input, split,
    std::make_shared<NativePreSplitFirstPlacement>(), options);
  split.reset();
  input.payload = {9};
  input.taskName = "changed";
  options.taskName = "different";
  model.adapterVersion = "invalid-after-submit";
  BOOST_CHECK(!retained.expired());
  BOOST_CHECK(encoded.empty());
  BOOST_REQUIRE_EQUAL(work.size(), 1U);
  auto dispatch = std::move(work.front()); work.pop_front(); dispatch();
  const std::vector<std::uint8_t> expected{1, 2, 3};
  BOOST_CHECK_EQUAL_COLLECTIONS(encoded.begin(), encoded.end(), expected.begin(), expected.end());
  // The remaining pipeline is still incomplete; preparation is not success.
  BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
    [](const auto& e) { return e.code() == "NATIVE_REQUEST_PIPELINE_NOT_READY"; });
}

BOOST_AUTO_TEST_CASE(RepositoryReferenceReachesNativePreparationBoundary)
{
  now = std::chrono::steady_clock::now();
  auto client = NativeClientTestAccess::create(port(), user, adapters,
    std::make_shared<NativeRequestPreparation>(adapters));
  NativeApplicationInput input;
  input.taskName = "task";
  input.inputSchemaDigest = model.adapter.inputSchemaDigest;
  input.optionsSchemaDigest = model.adapter.optionsSchemaDigest;
  input.transportMode = NativeInputTransportMode::RepositoryReference;
  input.repositoryReference = nativeCanonicalJson({
    {"authorizationScope", "/SERVICE/di"},
    {"ciphertextDigest", nativePlanningDigest("ciphertext")},
    {"dataName", "/user/NDNSF/DI/DATA/request/object"},
    {"encrypted", true},
    {"manifestDigest", nativePlanningDigest("manifest")},
    {"plaintextSize", 17},
    {"protectionEpoch", "epoch-1"},
  });
  NativeRequestOptions options;
  options.taskName = input.taskName;
  auto handle = client->request(model, input, std::make_shared<ClientTestSplit>(),
    std::make_shared<NativePreSplitFirstPlacement>(), options);
  BOOST_REQUIRE_EQUAL(work.size(), 1U);
  auto dispatch = std::move(work.front());
  work.pop_front();
  dispatch();
  // With no runtime configured this remains a deliberate not-ready boundary;
  // the repository reference must reach preparation instead of failing with
  // the old "resolution is not linked" sentinel.
  BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
    [](const auto& error) {
      return error.code() == "NATIVE_REQUEST_PIPELINE_NOT_READY";
    });
}

BOOST_AUTO_TEST_CASE(DeadlineDuringAdapterEncodingDoesNotRequireTimerDelivery)
{
  now = std::chrono::steady_clock::now();
  auto adapter = std::make_shared<ClientTestAdapter>();
  auto registry = std::make_shared<NativeAdapterRegistry>();
  registry->registerAdapter(adapter);
  registry->freeze();
  auto client = NativeClientTestAccess::create(port(), user, registry,
    std::make_shared<NativeRequestPreparation>(registry));
  NativeApplicationInput input;
  input.taskName = "inference";
  input.inputSchemaDigest = nativePlanningDigest("input-schema");
  input.optionsSchemaDigest = nativePlanningDigest("options-schema");
  input.payload = {1};
  auto handle = client->request(model, input, std::make_shared<ClientTestSplit>(),
    std::make_shared<NativePreSplitFirstPlacement>(), {});
  adapter->onEncode = [&](const auto&) { now += std::chrono::seconds(31); };
  BOOST_REQUIRE_EQUAL(work.size(), 1U);
  auto dispatch = std::move(work.front()); work.pop_front(); dispatch();
  BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
    [](const auto& e) { return e.code() == "NATIVE_REQUEST_TIMEOUT"; });
  BOOST_REQUIRE_EQUAL(timers.size(), 1U);
  BOOST_CHECK(timers.front()->cancelled);
  adapter->onEncode = {};
}

BOOST_AUTO_TEST_CASE(CancellationDuringAdapterEncodingDiscardsLatePreparation)
{
  now = std::chrono::steady_clock::now();
  auto adapter = std::make_shared<ClientTestAdapter>();
  auto registry = std::make_shared<NativeAdapterRegistry>();
  registry->registerAdapter(adapter);
  registry->freeze();
  auto client = NativeClientTestAccess::create(port(), user, registry,
    std::make_shared<NativeRequestPreparation>(registry));
  NativeApplicationInput input;
  input.taskName = "inference";
  input.inputSchemaDigest = nativePlanningDigest("input-schema");
  input.optionsSchemaDigest = nativePlanningDigest("options-schema");
  input.payload = {1};
  auto handle = client->request(model, input, std::make_shared<ClientTestSplit>(),
    std::make_shared<NativePreSplitFirstPlacement>(), {});
  unsigned calls = 0;
  adapter->onEncode = [&](const auto&) { ++calls; handle.cancel(); };
  BOOST_REQUIRE_EQUAL(work.size(), 1U);
  auto dispatch = std::move(work.front()); work.pop_front(); dispatch();
  BOOST_CHECK_EQUAL(calls, 1U);
  BOOST_CHECK(handle.status() == NativeRequestStatus::Cancelled);
  BOOST_REQUIRE_EQUAL(timers.size(), 1U);
  BOOST_CHECK(timers.front()->cancelled);
  adapter->onEncode = {};
}

BOOST_AUTO_TEST_CASE(SlowObserverDoesNotBlockCancelAndLateReplaySurvivesClientClose)
{
  auto client = NativeClientTestAccess::create(port(), user, adapters);
  auto handle = request(*client);
  std::promise<void> entered, release;
  auto enteredFuture = entered.get_future();
  auto gate = release.get_future().share();
  handle.observe([&](const NativeInferenceEvent&) { entered.set_value(); gate.wait(); });
  auto cancelling = std::async(std::launch::async, [&] { handle.cancel(); });
  auto enteredState = enteredFuture.wait_for(std::chrono::seconds(2));
  auto cancelState = cancelling.wait_for(std::chrono::milliseconds(200));
  release.set_value();
  cancelling.get();
  BOOST_CHECK(enteredState == std::future_status::ready);
  BOOST_CHECK(cancelState == std::future_status::ready);
  BOOST_CHECK(handle.status() == NativeRequestStatus::Cancelled);
  client.reset();
  auto replay = std::make_shared<std::promise<std::string>>();
  auto replayed = replay->get_future();
  handle.observe([replay](const NativeInferenceEvent& e) { replay->set_value(e.requestId); });
  BOOST_REQUIRE(replayed.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  BOOST_CHECK_EQUAL(replayed.get(), handle.requestId());
  while (!work.empty()) { auto f = std::move(work.front()); work.pop_front(); f(); }
  BOOST_CHECK(handle.status() == NativeRequestStatus::Cancelled);
  BOOST_CHECK(!handle.conversationCheckpoint().has_value());
}

BOOST_AUTO_TEST_CASE(LocalWaitDoesNotTerminateRequestAndExpiredDispatchFails)
{
  auto client = NativeClientTestAccess::create(port(), user, adapters);
  auto handle = request(*client);
  BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
                        [](const NativeDiError& e) { return e.code() == "LOCAL_WAIT_TIMEOUT"; });
  BOOST_CHECK(handle.status() == NativeRequestStatus::Pending);
  now += std::chrono::seconds(31);
  work.front()();
  BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
                        [](const NativeDiError& e) { return e.code() == "NATIVE_REQUEST_TIMEOUT"; });
  handle.cancel();
  BOOST_CHECK(handle.status() == NativeRequestStatus::Failed);
}

BOOST_AUTO_TEST_CASE(SubmissionFailureReturnsFailedHandle)
{
  auto throwingPort = port();
  throwingPort.submitHook = [](std::function<void()>) { throw std::runtime_error("queue unavailable"); };
  auto client = NativeClientTestAccess::create(throwingPort, user, adapters);
  auto handle = request(*client);
  BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
                        [](const NativeDiError& e) { return e.code() == "NATIVE_REQUEST_DISPATCH_FAILED"; });
  BOOST_REQUIRE_EQUAL(timers.size(), 1U);
  BOOST_CHECK(timers.front()->cancelled);
}

BOOST_AUTO_TEST_CASE(DeadlineFiresWithoutDispatchAndCannotBeRearmedByWait)
{
  auto client = NativeClientTestAccess::create(port(), user, adapters);
  auto handle = request(*client);
  BOOST_REQUIRE_EQUAL(timers.size(), 1U);
  BOOST_CHECK(timers.front()->deadline == now + std::chrono::seconds(30));
  BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
                        [](const NativeDiError& e) { return e.code() == "LOCAL_WAIT_TIMEOUT"; });
  auto lateDeadline = timers.front()->fire;
  now += std::chrono::seconds(30);
  lateDeadline();
  BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
                        [](const NativeDiError& e) { return e.code() == "NATIVE_REQUEST_TIMEOUT"; });
  BOOST_CHECK(timers.front()->cancelled);
  BOOST_CHECK_EQUAL(timers.size(), 1U);
  // Neither the overdue queued request nor an already-dispatched timer may
  // resurrect this operation or replace its first terminal error.
  work.front()();
  lateDeadline();
  handle.cancel();
  BOOST_CHECK(handle.status() == NativeRequestStatus::Failed);
}

BOOST_AUTO_TEST_CASE(CancelAndCloseRemoveTimersAndIgnoreLateExpiry)
{
  auto client = NativeClientTestAccess::create(port(), user, adapters);
  auto cancelled = request(*client);
  auto closing = request(*client);
  auto lateFirst = timers.at(0)->fire;
  auto lateSecond = timers.at(1)->fire;
  cancelled.cancel();
  client.reset();
  BOOST_CHECK(timers.at(0)->cancelled);
  BOOST_CHECK(timers.at(1)->cancelled);
  lateFirst();
  lateSecond();
  for (auto& task : work) task();
  BOOST_CHECK(cancelled.status() == NativeRequestStatus::Cancelled);
  BOOST_CHECK(closing.status() == NativeRequestStatus::Cancelled);
}

BOOST_AUTO_TEST_CASE(RealDeadlineDoesNotWaitForWorkOrSlowObserver)
{
  auto realPort = port();
  realPort.now = {};
  realPort.scheduleHook = {};
  auto client = NativeClientTestAccess::create(realPort, user, adapters);
  auto first = request(*client);
  std::promise<void> entered, release;
  auto enteredFuture = entered.get_future();
  auto gate = release.get_future().share();
  first.observe([&](const NativeInferenceEvent&) { entered.set_value(); gate.wait(); });
  first.cancel();
  const auto enteredState = enteredFuture.wait_for(std::chrono::seconds(2));
  NativeRequestOptions options;
  options.timeoutMs = 50;
  options.ackTimeoutMs = 10;
  auto pending = request(*client, options);
  std::string errorCode;
  try { pending.result(std::chrono::seconds(2)); }
  catch (const NativeDiError& error) { errorCode = error.code(); }
  // Always release the worker before any fatal test assertion/unwinding.
  release.set_value();
  BOOST_CHECK(enteredState == std::future_status::ready);
  BOOST_CHECK_EQUAL(errorCode, "NATIVE_REQUEST_TIMEOUT");
  BOOST_CHECK(pending.status() == NativeRequestStatus::Failed);
  BOOST_CHECK_EQUAL(work.size(), 2U); // No DI work was pumped to cause expiry.
  // Queue a barrier to ensure the callback's referenced promises are no
  // longer used when the test returns.
  auto barrier = std::make_shared<std::promise<void>>();
  auto done = barrier->get_future();
  pending.observe([barrier](const NativeInferenceEvent&) { barrier->set_value(); });
  BOOST_REQUIRE(done.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
}

BOOST_AUTO_TEST_CASE(InvalidDeadlinesAndNegativeWaitAreRejectedWithoutSubmission)
{
  auto client = NativeClientTestAccess::create(port(), user, adapters);
  NativeRequestOptions options;
  options.timeoutMs = options.ackTimeoutMs;
  BOOST_CHECK_EXCEPTION(request(*client, options), NativeDiError,
                        [](const NativeDiError& e) { return e.code() == "INVALID_REQUEST"; });
  options.timeoutMs = std::numeric_limits<std::uint64_t>::max();
  BOOST_CHECK_EXCEPTION(request(*client, options), NativeDiError,
                        [](const NativeDiError& e) { return e.code() == "INVALID_REQUEST"; });
  BOOST_CHECK(work.empty());
  BOOST_CHECK(timers.empty());
  auto handle = request(*client);
  BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(-1)), NativeDiError,
                        [](const NativeDiError& e) { return e.code() == "INVALID_WAIT_TIMEOUT"; });
  BOOST_CHECK(handle.status() == NativeRequestStatus::Pending);
}

BOOST_AUTO_TEST_CASE(DispatchExceptionIsContainedAndRemovesTimer)
{
  auto failingClock = port();
  unsigned calls = 0;
  failingClock.now = [&] {
    if (calls++ > 0) throw std::runtime_error("private diagnostic detail");
    return now;
  };
  auto client = NativeClientTestAccess::create(failingClock, user, adapters);
  auto handle = request(*client);
  BOOST_CHECK_NO_THROW(work.front()());
  BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
                        [](const NativeDiError& e) {
                          return e.code() == "NATIVE_REQUEST_DISPATCH_FAILED" &&
                            std::string(e.what()).find("private diagnostic") == std::string::npos;
                        });
  BOOST_CHECK(timers.at(0)->cancelled);
}

BOOST_AUTO_TEST_CASE(CorePostUsesSharedFaceAndNeverRunsInline)
{
  BOOST_CHECK(!user->isOnIoThread());
  BOOST_CHECK_THROW(user->postToIo({}), std::invalid_argument);
  bool outerRan = false, outerReturned = false, innerRan = false;
  const auto ioThread = std::this_thread::get_id();
  auto submitter = std::async(std::launch::async, [&] {
    user->postToIo([&] {
      outerRan = true;
      BOOST_CHECK(user->isOnIoThread());
      BOOST_CHECK(std::this_thread::get_id() == ioThread);
      user->postToIo([&] {
        innerRan = true;
        BOOST_CHECK(outerReturned);
        BOOST_CHECK(user->isOnIoThread());
      });
      BOOST_CHECK(!innerRan);
      outerReturned = true;
    });
  });
  submitter.get();
  BOOST_CHECK(!outerRan);
  face.getIoContext().restart();
  face.getIoContext().poll();
  BOOST_CHECK(outerRan);
  BOOST_CHECK(innerRan);
  BOOST_CHECK(!user->isOnIoThread());
}

BOOST_AUTO_TEST_CASE(CoreIoRejectsBlockingResultButAllowsPollAndCancel)
{
  auto client = NativeClientTestAccess::create(port(), user, adapters);
  auto handle = request(*client);
  bool checked = false;
  user->postToIo([&] {
    checked = true;
    BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(1)), NativeDiError,
                          [&](const NativeDiError& e) {
                            return e.code() == "CORE_IO_WAIT_FORBIDDEN" &&
                              e.requestId() == handle.requestId();
                          });
    BOOST_CHECK(handle.status() == NativeRequestStatus::Pending);
    BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
                          [](const NativeDiError& e) { return e.code() == "LOCAL_WAIT_TIMEOUT"; });
    handle.cancel();
    BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
                          [](const NativeDiError& e) { return e.code() == "CANCELLED"; });
  });
  face.getIoContext().restart();
  face.getIoContext().poll();
  BOOST_CHECK(checked);
  BOOST_CHECK(handle.status() == NativeRequestStatus::Cancelled);
}

BOOST_AUTO_TEST_CASE(Spec184TurnPublicationRace)
{
  NativeConversationConfig config;
  config.authenticationKeys = {std::vector<std::uint8_t>(32, 0x5a)};
  config.requesterIdentity = "/client-test/user";
  config.serviceName = "/client-test/conversation";
  config.securityDomainDigest = nativePlanningDigest("spec184-security");
  config.nowMs = [] { return std::uint64_t{2'000'000'000'001ULL}; };
  NativeConversationCoordinator owner(config);

  const auto makeContinuation = [&] (const std::string& id, bool mapped) {
    NativeConversationContinuation continuation;
    continuation.conversationId = id;
    continuation.serviceName = config.serviceName;
    continuation.requestContractDigest = nativePlanningDigest("spec184-request");
    continuation.retentionDeadlineMs = 2'000'000'060'000ULL;
    continuation.generationId = std::string(32, '1');
    continuation.canonicalTokenIds = {1, 2};
    if (mapped) {
      continuation.planRoleMapDigest = nativePlanningDigest(
        nativeCanonicalJson(NativeJson::array({NativeJson::array({"/role/A", "/provider/A"})})));
      continuation.expectedRoles = {"/role/A"};
    }
    return continuation;
  };
  const NativeDiError cancelled("CANCELLED", "conversation", "test", "cancel");

  // The planner-side publication and terminal cleanup contend on one ticket.
  // Whichever operation wins, the other must observe the same ticket as stale;
  // no half-bound turn may remain available for token acceptance.
  const auto initial = owner.beginTurn(makeContinuation(
    "spec184-initial-race", false), "/request/spec184-initial");
  std::promise<void> releaseInitial;
  auto initialGate = releaseInitial.get_future().share();
  std::atomic<bool> initialBound{false};
  std::atomic<bool> initialBindFailed{false};
  auto initialBinder = std::async(std::launch::async, [&] {
    initialGate.wait();
    try {
      (void) owner.bindInitialPlanRoleMap(initial,
        {{"/role/A", "/provider/A"}});
      initialBound.store(true);
    }
    catch (const std::exception&) {
      initialBindFailed.store(true);
    }
  });
  auto initialAborter = std::async(std::launch::async, [&] {
    initialGate.wait();
    owner.abortTurn(initial, cancelled);
  });
  releaseInitial.set_value();
  initialBinder.get();
  initialAborter.get();
  BOOST_CHECK_NO_THROW(owner.abortTurn(initial, cancelled));
  BOOST_CHECK_THROW(owner.acceptTokenPrefix(initial, {3}), std::runtime_error);

  const auto replacementParent = owner.beginTurn(makeContinuation(
    "spec184-replacement-race", true), "/request/spec184-replacement");
  const auto replacement = owner.replaceAttempt(
    replacementParent, "/request/spec184-replacement/attempt-2");
  std::promise<void> releaseReplacement;
  auto replacementGate = releaseReplacement.get_future().share();
  std::atomic<bool> replacementBound{false};
  std::atomic<bool> replacementBindFailed{false};
  auto replacementBinder = std::async(std::launch::async, [&] {
    replacementGate.wait();
    try {
      (void) owner.bindAttemptPlanRoleMap(replacement,
        {{"/role/A", "/provider/replacement"}});
      replacementBound.store(true);
    }
    catch (const std::exception&) {
      replacementBindFailed.store(true);
    }
  });
  auto replacementAborter = std::async(std::launch::async, [&] {
    replacementGate.wait();
    owner.abortTurn(replacement, cancelled);
  });
  releaseReplacement.set_value();
  replacementBinder.get();
  replacementAborter.get();
  BOOST_CHECK_NO_THROW(owner.abortTurn(replacement, cancelled));
  BOOST_CHECK_THROW(owner.acceptTokenPrefix(replacement, {4}), std::runtime_error);
  BOOST_CHECK(initialBound.load() || initialBindFailed.load());
  BOOST_CHECK(replacementBound.load() || replacementBindFailed.load());
}

BOOST_AUTO_TEST_CASE(HandleRetainsCoreOwnerAfterClientClose)
{
  auto client = NativeClientTestAccess::create(port(), user, adapters);
  auto handle = request(*client);
  std::weak_ptr<ndn_service_framework::ServiceUser> weakUser = user;
  client.reset();
  user.reset();
  BOOST_CHECK(!weakUser.expired());
  BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(1)), NativeDiError,
                        [](const NativeDiError& e) { return e.code() == "CANCELLED"; });
  work.clear();
  handle = NativeInferenceHandle{};
  BOOST_CHECK(weakUser.expired());
}

BOOST_AUTO_TEST_SUITE_END()
