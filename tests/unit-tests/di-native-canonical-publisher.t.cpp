#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalArtifactPublisher.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeV3Placement.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

#include <fstream>
#include <future>
#include <utility>

namespace ndnsf::di {
class NativeCanonicalPublisherTestAccess
{
public:
  using Transport = NativeCanonicalArtifactPublisher::Transport;
  static NativeCanonicalArtifactPublisher create(Transport transport,
    NativeCanonicalPublicationOptions options, NativeCanonicalArtifactPublisher::SourcePort source)
  { return NativeCanonicalArtifactPublisher(std::move(transport), "/service", std::move(options), std::move(source)); }
};
}

namespace {
using namespace ndnsf::di;
using namespace ndn_service_framework;
using Clock = std::chrono::steady_clock;

std::vector<std::uint8_t> unhex(const std::string& text)
{
  std::vector<std::uint8_t> bytes;
  for (std::size_t i = 0; i < text.size(); i += 2)
    bytes.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
  return bytes;
}

struct Input
{
  std::shared_ptr<NativeCanonicalSource> source = std::make_shared<NativeCanonicalSource>();
  NativeInspectedModel model;
  NativeSplitCandidate candidate;
  std::vector<NativeSelectionRoleV3> roles;
  NativeCanonicalPublicationOptions options;
  NativeRequestControl control{"/request", 1, Clock::now() + std::chrono::seconds(10), {}};
  explicit Input(bool external = false)
  {
    std::ifstream stream("tests/fixtures/spec182/dependency-probes/extraction-vectors.json");
    if (!stream) throw std::runtime_error("missing frozen ONNX source fixture");
    const auto vectors = NativeJson::parse(stream);
    const auto& row = vectors.at("cases")[external ? 1 : 0];
    source->modelBytes = unhex(row.at("modelHex"));
    if (external) source->initializerBytes = unhex(row.at("initializerHex"));
    const auto& recipe = row.at("recipe");
    const auto check = validateNativeOnnxWorkerMetadata(NativeJson{
      {"schema", kNativeOnnxAssemblyRequestSchema}, {"recipe", recipe},
      {"recipeDigest", nativePlanningDigest(recipe.dump())},
      {"backend", "onnxruntime-cpu"}, {"adapterId", "fixture"}}.dump());
    if (!check.ok) throw std::runtime_error(check.failureMessage);
    auto role = check.value.recipe;
    role.role = role.selectedRole = "/role"; role.adapterVersion = "1";
    role.artifactDigest = row.at("expectedModelDigest");
    role.recipeDigest = check.value.recipeDigest; role.requiredDeviceMemoryMb = 1;
    role.protectionEpoch = "artifact-policy-epoch";
    roles = {role};
    model.descriptor = {"fixture-model", nativePlanningDigest("model"), nativePlanningDigest("semantics"),
      nativePlanningDigest("planning"), "onnx", "fp32", "fixture", "1"};
    model.descriptor = fixture::completeModel(model.descriptor);
    model.graph.graphDigest = model.descriptor.graphDigest;
    model.graph.nodes = {{"n0", "Identity", 0}, {"n1", "Identity", 1}};
    model.graph.topologicalOrder = {"n0", "n1"};
    model.canonicalSourceName = "/fixture/source";
    model.canonicalSourceDigest = nativePlanningDigest(source->modelBytes.data(), source->modelBytes.size());
    model.canonicalSourceBytes = source->modelBytes.size();
    model.modelManifestDigest = role.modelManifestDigest;
    model.canonicalGraphDigest = role.graphDigest;
    if (external) {
      model.canonicalInitializerBytes = source->initializerBytes->size();
      model.canonicalInitializerObjectDigest = nativePlanningDigest(source->initializerBytes->data(), source->initializerBytes->size());
    }
    candidate.model = model.descriptor; candidate.graphDigest = model.graph.graphDigest;
    candidate.splitter = {"fixture", "1", nativePlanningDigest("splitter")};
    candidate.candidateDigest = nativePlanningDigest("candidate");
    candidate.executionPlan.roles = {role.role}; candidate.tensorDegreesByRole = {{role.role, 1}};
    for (const auto& node : model.graph.nodes) candidate.nodeRoles[node.id] = role.role;
    candidate.artifactsByRole = {{role.role, {role.artifactDigest}}};
    candidate.rankArtifactDigestsByRole = candidate.artifactsByRole;
    candidate.fragmentsByRole = {{role.role, role.artifactDigest}};
    candidate.requirementsByRole = {{role.role, {{"onnxruntime"}, 1, 0, 0, 0, 0, 1.0}}};
    options = {"/fixture/NDNSF/DI/ARTIFACT", model.modelManifestDigest, {role.artifactDigest}};
  }
  NativeCanonicalArtifactPublisher::SourcePort resolver()
  { return [value = source](const auto&, const auto&) { return value; }; }
  NativeRolePlacementProposalV3 proposal() const
  {
    NativeRolePlacementProposalV3 p;
    p.context = {control.requestId, control.attempt, "/service", model.descriptor.contentDigest,
      model.graph.graphDigest, 2000000000000ULL};
    p.ackClosedDigest = nativePlanningDigest("ack"); p.strategy = candidate.splitter;
    p.roles = roles; p.providerByRole = {{roles[0].selectedRole, "/provider"}};
    p.offerDigestByProvider = {{"/provider", nativePlanningDigest("offer")}};
    return p;
  }
};

struct TransportFixture
{
  std::vector<std::vector<std::uint8_t>> payloads;
  std::vector<std::string> requests;
  std::function<void(LargeDataPublishResult&)> mutate;
  NativeCanonicalPublisherTestAccess::Transport transport()
  {
    return {[](auto job) { job(); }, [] { return false; },
      [] { return PreparedServiceRequest{ndn::Name("/service"), ndn::Name("/publication-request")}; },
      [this](const PreparedServiceRequest& request, const std::vector<std::uint8_t>& bytes, const std::string& label) {
        payloads.push_back(bytes); requests.push_back(request.requestId.toUri());
        LargeDataPublishResult result;
        result.success = true; result.encrypted = true; result.objectId = label;
        result.encryptedDataName = ndn::Name("/encrypted").append(label).appendVersion(1);
        result.plaintextSize = bytes.size(); result.contentDigest = nativePlanningDigest(bytes.data(), bytes.size());
        result.manifestDigest = nativePlanningDigest("Core transport metadata");
        result.authorizationScope = "/SERVICE/service"; result.protectionEpoch = "Core epoch";
        if (mutate) mutate(result);
        return result;
      }};
  }
};
}

BOOST_AUTO_TEST_SUITE(Spec182CanonicalPublisher)
BOOST_AUTO_TEST_CASE(PublishesOwnedInlineAndExternalSourcesThroughPreparation)
{
  for (bool external : {false, true}) {
    Input input(external); TransportFixture io;
    auto publisher = NativeCanonicalPublisherTestAccess::create(io.transport(), input.options, input.resolver());
    NativeRequestPreparation preparation(std::make_shared<NativeAdapterRegistry>(), {}, publisher.artifactPort());
    const auto result = preparation.ensureArtifacts(input.model, input.candidate, input.proposal(), input.control);
    BOOST_REQUIRE_EQUAL(io.payloads.size(), external ? 3 : 2);
    BOOST_CHECK(io.payloads[0] == input.source->modelBytes);
    if (external) BOOST_CHECK(io.payloads[1] == *input.source->initializerBytes);
    BOOST_CHECK(std::all_of(io.requests.begin(), io.requests.end(), [&](const auto& r) { return r == io.requests.front(); }));
    BOOST_CHECK_EQUAL(result.requestId, input.control.requestId);
    BOOST_CHECK_EQUAL(result.canonicalGraphDigest, input.model.canonicalGraphDigest);
    BOOST_CHECK_NE(result.manifestDigest, nativePlanningDigest("Core transport metadata"));
    BOOST_CHECK_NE(result.manifestDigest, input.model.modelManifestDigest);
    BOOST_CHECK_EQUAL(result.manifestDigest, nativePlanningDigest(io.payloads.back().data(), io.payloads.back().size()));
    const auto root = nativeParseJson(result.canonicalManifestJson);
    BOOST_CHECK_EQUAL(root.at("metadata").at("canonicalSourceDigest").get<std::string>(), input.model.canonicalSourceDigest);
    BOOST_CHECK_EQUAL(root.at("metadata").at("packageManifestDigest").get<std::string>(), input.model.modelManifestDigest);
    BOOST_CHECK_NE(result.sourceByRole.at("/role"), result.artifactNameByRole.at("/role"));
    BOOST_CHECK(result.artifactNameByRole.at("/role").find("//") == std::string::npos);
    const auto certified = NativeRequestPreparation::bindPublishedRoles(input.model, input.candidate, input.roles, result);
    BOOST_CHECK_EQUAL(certified[0].modelManifestDigest, result.manifestDigest);
    BOOST_CHECK_NE(certified[0].recipeDigest, input.roles[0].recipeDigest);
  }
}

BOOST_AUTO_TEST_CASE(RejectsSourceAndCanonicalIdentityBeforePublication)
{
  for (unsigned poison = 0; poison != 4; ++poison) {
    Input input(true); TransportFixture io;
    if (poison == 0) input.source->modelBytes[0] ^= 1;
    if (poison == 1) (*input.source->initializerBytes)[0] ^= 1;
    if (poison == 2) input.roles[0].canonicalInitializerDigest = nativePlanningDigest("foreign");
    if (poison == 3) {
      input.model.canonicalGraphDigest = input.roles[0].graphDigest = nativePlanningDigest("foreign graph");
    }
    auto publisher = NativeCanonicalPublisherTestAccess::create(io.transport(), input.options, input.resolver());
    BOOST_CHECK_THROW(publisher(input.model, input.candidate, input.roles, input.control), std::invalid_argument);
    BOOST_CHECK(io.payloads.empty());
  }
}

BOOST_AUTO_TEST_CASE(RejectsInvalidCoreReceiptsBeforePublishingRoot)
{
  for (unsigned poison = 0; poison != 7; ++poison) {
    Input input; TransportFixture io;
    io.mutate = [poison](auto& r) {
      if (poison == 0) r.success = false;
      if (poison == 1) r.encrypted = false;
      if (poison == 2) ++r.plaintextSize;
      if (poison == 3) r.contentDigest = nativePlanningDigest("foreign");
      if (poison == 4) r.manifestDigest.clear();
      if (poison == 5) r.authorizationScope = "/SERVICE/foreign";
      if (poison == 6) r.protectionEpoch.clear();
    };
    auto publisher = NativeCanonicalPublisherTestAccess::create(io.transport(), input.options, input.resolver());
    BOOST_CHECK_THROW(publisher(input.model, input.candidate, input.roles, input.control), std::runtime_error);
    BOOST_CHECK_EQUAL(io.payloads.size(), 1);
  }
}

BOOST_AUTO_TEST_CASE(QueuedCancellationAndTimeoutReleaseSourceAndSuppressLateWork)
{
  for (bool timeout : {false, true}) {
    Input input; TransportFixture io;
    std::atomic<bool> cancelled{false};
    input.control.cancelled = [&] { return cancelled.load(); };
    if (timeout) input.control.deadline = Clock::now() + std::chrono::milliseconds(150);
    std::promise<std::function<void()>> queued;
    auto ready = queued.get_future();
    auto transport = io.transport();
    transport.post = [&](auto work) { queued.set_value(std::move(work)); };
    const std::weak_ptr<NativeCanonicalSource> source = input.source;
    auto publisher = NativeCanonicalPublisherTestAccess::create(transport, input.options,
      [&](const auto&, const auto&) { return std::exchange(input.source, {}); });
    auto pending = std::async(std::launch::async, [&] {
      return publisher(input.model, input.candidate, input.roles, input.control);
    });
    BOOST_REQUIRE(ready.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    auto work = ready.get();
    if (!timeout) cancelled = true;
    BOOST_REQUIRE(pending.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    BOOST_CHECK_THROW(pending.get(), std::runtime_error);
    BOOST_CHECK(source.expired());
    work();
    BOOST_CHECK(io.payloads.empty());
  }
}

BOOST_AUTO_TEST_CASE(PreservesCoreFailureBoundary)
{
  Input input; TransportFixture io;
  io.mutate = [](auto& result) { result.success = false; result.errorMessage = "fixture missing wrapped key"; };
  auto publisher = NativeCanonicalPublisherTestAccess::create(io.transport(), input.options, input.resolver());
  try {
    publisher(input.model, input.candidate, input.roles, input.control);
    BOOST_FAIL("expected Core publication failure");
  }
  catch (const std::runtime_error& error) {
    BOOST_CHECK_EQUAL(error.what(), "DI_NATIVE_ENCRYPTED_PUBLICATION_FAILED: fixture missing wrapped key");
  }
  BOOST_CHECK_EQUAL(io.payloads.size(), 1);
}

BOOST_AUTO_TEST_CASE(CancellationAfterSourceSuppressesInitializerAndRoot)
{
  Input input(true); TransportFixture io;
  bool cancelled = false;
  input.control.cancelled = [&] { return cancelled; };
  io.mutate = [&](auto&) { cancelled = true; };
  auto publisher = NativeCanonicalPublisherTestAccess::create(io.transport(), input.options, input.resolver());
  BOOST_CHECK_THROW(publisher(input.model, input.candidate, input.roles, input.control), std::runtime_error);
  BOOST_CHECK_EQUAL(io.payloads.size(), 1);
}

BOOST_AUTO_TEST_CASE(UsesRealCoreIoAndEncryptedPublicationWithLocalMockKey)
{
  using namespace ndn_service_framework::test;
  Input input;
  ndn::security::KeyChain keyChain{"pib-memory:", "tpm-memory:"};
  ndn::DummyClientFace face{keyChain};
  auto cert = makeRsaIdentity(keyChain, ndn::Name("/publisher-test/user"));
  auto aa = makeRsaIdentity(keyChain, ndn::Name("/publisher-test/aa"));
  auto user = std::make_shared<LocalServiceUser>(face, ndn::Name("/publisher-test"), cert, aa, "examples/trust-any.conf");
  user->useSigningKeyChainForTest(keyChain);
  // Unit fixture only: seed Core's existing LocalMock wrapped-key state. The
  // actual publish API still encrypts, signs and stores its segmented envelope.
  user->prepareHybridSendKeyForTest(ndn::Name("/service"), "REQUEST-LARGE");
  NativeCanonicalArtifactPublisher publisher(user, "/service", input.options, input.resolver());
  bool ioRejected = false;
  user->postToIo([&] {
    try { publisher(input.model, input.candidate, input.roles, input.control); }
    catch (const std::runtime_error& e) { ioRejected = std::string(e.what()) == "DI_NATIVE_PUBLICATION_IO_WAIT_FORBIDDEN"; }
  });
  face.getIoContext().poll();
  BOOST_CHECK(ioRejected);
  auto pending = std::async(std::launch::async, [&] {
    return publisher(input.model, input.candidate, input.roles, input.control);
  });
  while (pending.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
    face.getIoContext().restart(); face.getIoContext().poll();
  }
  const auto result = pending.get();
  const auto name = ndn::Name(result.sourceByRole.at("/role"));
  BOOST_CHECK(user->hasCachedDataForTest(name));
  BOOST_CHECK_NE(user->getCachedDataContentForTest(name),
    ndn::Buffer(result.canonicalManifestJson.begin(), result.canonicalManifestJson.end()));
  BOOST_CHECK_NE(result.manifestDigest, input.model.modelManifestDigest);
}
BOOST_AUTO_TEST_SUITE_END()
