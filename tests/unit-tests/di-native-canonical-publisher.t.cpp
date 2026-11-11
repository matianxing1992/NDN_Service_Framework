#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalArtifactPublisher.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-material-fragment-cache.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-filesystem-artifacts.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/FixedAssignmentWork.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"
#include "tests/fixtures/spec182/native-catalog-test-access.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeV3Placement.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-owned-initializer-file.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"
#include "tests/integration-tests/isolated-test-trust.hpp"
#include "tests/fixtures/spec182/native-sealing-fixture.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeYoloMergeRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalRolePreparer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalPreparationCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/model-object-keys.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/protected-preparation-index.hpp"
#include "ndn-service-framework/ProtectedBlobManifest.hpp"

#include <onnx/onnx_pb.h>
#include <onnx/checker.h>
#include <onnx/defs/schema.h>
#include <onnxruntime_cxx_api.h>
#include <ndn-cxx/util/sha256.hpp>

#include <boost/scope_exit.hpp>

#include <fstream>
#include <fcntl.h>
#include <atomic>
#include <cfenv>
#include <future>
#include <thread>
#include <algorithm>
#include <filesystem>
#include <unistd.h>
#include <limits>
#include <random>
#include <utility>

namespace {
using namespace ndnsf::di;
using namespace ndn_service_framework;
using Clock = std::chrono::steady_clock;

class InitializerRangeFixture final : public NativeCanonicalByteRangeSource
{
public:
  std::vector<std::uint8_t> bytes;
  std::uint64_t base = 0;
  mutable std::uint64_t largestRead = 0;
  mutable std::uint64_t reads = 0;
  bool shortRead = false;

  std::uint64_t size() const noexcept override
  {
    return base + bytes.size();
  }

  std::vector<std::uint8_t> read(std::uint64_t offset, std::uint64_t length) const override
  {
    if (length > NativeCanonicalMaterialBundleMaxBytes || offset < base ||
        offset > size() || length > size() - offset)
      throw std::runtime_error("fixture range exceeded declared backing");
    largestRead = std::max(largestRead, length);
    ++reads;
    const auto first = bytes.begin() + static_cast<std::size_t>(offset - base);
    return {first, first + static_cast<std::size_t>(length - (shortRead && length ? 1 : 0))};
  }
};

std::vector<std::uint8_t> unhex(const std::string& text)
{
  std::vector<std::uint8_t> bytes;
  for (std::size_t i = 0; i < text.size(); i += 2)
    bytes.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
  return bytes;
}

struct CanonicalPublisherInput
{
  std::shared_ptr<NativeCanonicalSource> source = std::make_shared<NativeCanonicalSource>();
  NativeInspectedModel model;
  NativeSplitCandidate candidate;
  std::vector<NativeSelectionRoleV3> roles;
  NativeCanonicalPublicationOptions options;
  NativeRequestControl control{"/request", 1, Clock::now() + std::chrono::seconds(10), {}};
  explicit CanonicalPublisherInput(bool external = false)
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
      model.canonicalInitializerDigest = role.canonicalInitializerDigest;
    }
    candidate.model = model.descriptor; candidate.graphDigest = model.graph.graphDigest;
    candidate.splitter = {"fixture", "1", nativePlanningDigest("splitter")};
    candidate.source = "PRE_SPLIT";
    candidate.executionPlan.roles = {role.role}; candidate.tensorDegreesByRole = {{role.role, 1}};
    for (const auto& node : model.graph.nodes) candidate.nodeRoles[node.id] = role.role;
    candidate.artifactsByRole = {{role.role, {role.artifactDigest}}};
    candidate.rankArtifactDigestsByRole = candidate.artifactsByRole;
    candidate.fragmentsByRole = {{role.role, role.artifactDigest}};
    // Frozen inline/external fixture W1 is FP32[2,2], exactly 16 bytes.
    candidate.requirementsByRole = {{role.role, {{"onnxruntime"}, 16, 0, 0, 0, 0, 1.0}}};
    candidate.candidateDigest = candidate.computedDigest();
    options = {"/fixture/NDNSF/DI/ARTIFACT", model.modelManifestDigest, {role.artifactDigest}};
  }
  NativeCanonicalArtifactPublisher::SourcePort resolver()
  { return [value = source](const auto&, const auto&) { return value; }; }
  NativeRolePlacementProposalV3 proposal() const
  {
    NativeRolePlacementProposalV3 p;
    p.context = {control.requestId, control.attempt, "/service", model.descriptor.intentDigest(),
      model.graph.graphDigest, 2000000000000ULL};
    p.ackClosedDigest = nativePlanningDigest("ack"); p.strategy = candidate.splitter;
    p.roles = roles; p.providerByRole = {{roles[0].selectedRole, "/provider"}};
    p.offerDigestByProvider = {{"/provider", nativePlanningDigest("offer")}};
    return p;
  }
};

struct TransportFixture
{
  ndn::Name requestService{ "/service" };
  std::vector<std::vector<std::uint8_t>> payloads;
  std::vector<std::string> requests;
  std::vector<std::string> labels;
  std::vector<ndn_service_framework::PublishedContentName> contentNames;
  std::size_t beginCalls = 0;
  std::size_t abortCalls = 0;
  std::size_t abortedPublications = 0;
  std::function<void(LargeDataPublishResult&)> mutate;
  NativeCanonicalPublisherTestAccess::Transport transport()
  {
    return {[](auto job) { job(); }, [] { return false; },
      [this] {
        ++beginCalls;
        return PreparedServiceRequest{requestService, ndn::Name("/publication-request")};
      },
      [this](const PreparedServiceRequest& request, const std::vector<std::uint8_t>& bytes,
             const std::string& label, const ndn_service_framework::PublishedContentName& contentName,
             const NativeRequestControl&) {
        payloads.push_back(bytes); requests.push_back(request.requestId.toUri());
        labels.push_back(label);
        contentNames.push_back(contentName);
        LargeDataPublishResult result;
        result.success = true; result.encrypted = true; result.objectId = label;
        result.encryptedDataName = ndn::Name("/encrypted").append(label).appendVersion(1);
        result.plaintextSize = bytes.size(); result.contentDigest = nativePlanningDigest(bytes.data(), bytes.size());
        result.manifestDigest = nativePlanningDigest("Core transport metadata");
        result.authorizationScope = "/SERVICE/service"; result.protectionEpoch = "Core epoch";
        if (mutate) mutate(result);
        return result;
      },
      [this](const auto& publications) {
        ++abortCalls;
        abortedPublications += publications.size();
      }};
  }
};
}

BOOST_AUTO_TEST_SUITE(Spec182CanonicalPublisher)

using Input = CanonicalPublisherInput;

BOOST_AUTO_TEST_CASE(ModelObjectKeysBindCurrentInvocationAndRecipient)
{
  Input input;
  NativeSelectionProjectionV3 projection;
  projection.provider = "/provider";
  projection.requestId = "/request/1";
  projection.attempt = 1;
  projection.hasGrantBinding = true;
  projection.grantName = "/controller/grant/1";
  projection.deadlineMs = 2000;
  projection.selectedRole = input.roles.front();
  projection.planCoreDigest = nativePlanningDigest("core");
  projection.planDigest = nativePlanningDigest("plan");
  projection.ackClosedDigest = nativePlanningDigest("ack");
  projection.offerDigest = nativePlanningDigest("offer");
  projection.grantDigest = nativePlanningDigest("grant");
  projection.securityPolicySnapshotDigest = nativePlanningDigest("policy");
  const ndn::Name certificate("/provider/KEY/key/issuer/v=1");
  const auto keyDigest = nativePlanningDigest("recipient public key");
  auto context = [&](const auto& value, const std::string& boot = "boot-1") {
    return nativeModelObjectKeyContext(value, ndn::Name("/requester"),
      ndn::Name("/service"), boot, certificate, keyDigest, 1500, 1000);
  };
  const auto original = context(projection);
  BOOST_CHECK_EQUAL(original.at("modelManifestDigest").get<std::string>(),
                    input.roles.front().modelManifestDigest);
  BOOST_CHECK(context(projection, "boot-2") != original);
  for (unsigned mutation = 0; mutation < 10; ++mutation) {
    auto changed = projection;
    switch (mutation) {
      case 0: changed.requestId = "/request/2"; break;
      case 1: ++changed.attempt; break;
      case 2: changed.grantName = "/controller/grant/2"; break;
      case 3: changed.grantDigest = nativePlanningDigest("other grant"); break;
      case 4: changed.planDigest = nativePlanningDigest("other plan"); break;
      case 5: changed.ackClosedDigest = nativePlanningDigest("other ack"); break;
      case 6: changed.selectedRole.selectedRole = "/other-role"; break;
      case 7: ++changed.selectedRole.rank; break;
      case 8: changed.selectedRole.modelManifestDigest = nativePlanningDigest("other model"); break;
      case 9: changed.securityPolicySnapshotDigest = nativePlanningDigest("other policy"); break;
    }
    BOOST_CHECK(context(changed) != original);
  }
  auto invalid = projection;
  invalid.hasGrantBinding = false;
  BOOST_CHECK_THROW(context(invalid), std::invalid_argument);
  invalid = projection;
  invalid.deadlineMs = 1499;
  BOOST_CHECK_THROW(context(invalid), std::invalid_argument);
  invalid = projection;
  invalid.selectedRole.protectionEpoch = "plaintext-v1";
  BOOST_CHECK_THROW(context(invalid), std::invalid_argument);
  invalid = projection;
  invalid.provider = "/foreign-provider";
  BOOST_CHECK_THROW(context(invalid), std::invalid_argument);
  invalid = projection;
  invalid.offerDigest = "missing digest";
  BOOST_CHECK_THROW(context(invalid), std::invalid_argument);
  BOOST_CHECK_THROW(context(projection, ""), std::invalid_argument);
  BOOST_CHECK_THROW(nativeModelObjectKeyContext(projection, ndn::Name("/requester"),
    ndn::Name("/service"), "boot-1", certificate, keyDigest, 1000, 1000),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ModelObjectKeySelectionRejectsBundlesAcrossProviderAssignments)
{
  const auto canonicalName = [](const std::string& uri) {
    return ndn::Name(uri).toUri(ndn::name::UriFormat::CANONICAL);
  };
  NativeModelObjectKeyIndexV1 index;
  index.providerAssemblyMetadataNames = {canonicalName("/model/root"),
    canonicalName("/model/material"), canonicalName("/model/receipt")};
  index.nonProviderObjectNames = {canonicalName("/model/source"),
    canonicalName("/model/prepared-metadata")};
  index.templatePayloadIds = {"template"};
  index.payloadIdsByNode = {{0, {"node-0", "shared-init"}},
                            {1, {"node-1", "shared-init"}}};
  const auto bundleName = [&canonicalName](const std::string& suffix) {
    return canonicalName("/model/bundle/" + suffix);
  };
  index.payloadIdsByDataName = {{bundleName("template"), {"template"}},
                                {bundleName("node-0"), {"node-0"}},
                                {bundleName("node-1"), {"node-1"}},
                                {bundleName("shared"), {"shared-init"}}};

  const auto first = nativeSelectModelObjectKeyDataNames(index, {0});
  const auto second = nativeSelectModelObjectKeyDataNames(index, {1});
  for (const auto& name : index.providerAssemblyMetadataNames) {
    BOOST_CHECK(first.count(name) == 1);
    BOOST_CHECK(second.count(name) == 1);
  }
  BOOST_CHECK(first.count(bundleName("template")) == 1);
  BOOST_CHECK(first.count(bundleName("node-0")) == 1);
  BOOST_CHECK(first.count(bundleName("shared")) == 1);
  BOOST_CHECK(first.count(bundleName("node-1")) == 0);
  BOOST_CHECK(second.count(bundleName("template")) == 1);
  BOOST_CHECK(second.count(bundleName("node-1")) == 1);
  BOOST_CHECK(second.count(bundleName("shared")) == 1);
  BOOST_CHECK(second.count(bundleName("node-0")) == 0);
  BOOST_CHECK(first.count(canonicalName("/model/source")) == 0);
  BOOST_CHECK(first.count(canonicalName("/model/prepared-metadata")) == 0);

  index.payloadIdsByDataName.erase(bundleName("node-0"));
  index.payloadIdsByDataName.erase(bundleName("node-1"));
  index.payloadIdsByDataName.emplace(bundleName("mixed"),
                                     std::set<std::string>{"node-0", "node-1"});
  BOOST_CHECK_THROW(nativeSelectModelObjectKeyDataNames(index, {0}), std::invalid_argument);
  BOOST_CHECK_THROW(nativeSelectModelObjectKeyDataNames(index, {1}), std::invalid_argument);
  const auto both = nativeSelectModelObjectKeyDataNames(index, {0, 1});
  BOOST_CHECK(both.count(bundleName("mixed")) == 1);
}

BOOST_AUTO_TEST_CASE(DelayedBootstrapPrecedesPublicationAndDoesNotDelayCacheReuse)
{
  for (const bool prepareOnly : {true, false}) {
    Input input;
    TransportFixture io;
    auto transport = io.transport();
    std::atomic<bool> ready{false};
    std::atomic<bool> admitted{false};
    std::atomic<unsigned> probes{0};
    std::promise<void> observed;
    auto observation = observed.get_future();
    transport.bootstrapReady = [&](const NativeRequestControl&) {
      const bool value = ready.load();
      if (value) admitted.store(true);
      if (probes.fetch_add(1) == 0) observed.set_value();
      return value;
    };
    const auto begin = transport.begin;
    transport.begin = [&, begin] {
      if (!admitted.load())
        throw std::runtime_error("publication preceded accepted bootstrap readiness");
      return begin();
    };
    auto publisher = NativeCanonicalPublisherTestAccess::create(
      std::move(transport), input.options, input.resolver());
    const auto publish = [&] {
      if (prepareOnly) publisher.prepare(input.model, input.control);
      else publisher(input.model, input.candidate, input.roles, input.control);
    };
    auto pending = std::async(std::launch::async, publish);
    BOOST_CHECK(observation.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    BOOST_CHECK(pending.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
    ready.store(true);
    pending.get();
    BOOST_CHECK_GE(probes.load(), 2U);
    BOOST_CHECK_EQUAL(io.beginCalls, 1);
    BOOST_CHECK(!io.payloads.empty());
    const auto checked = probes.load();
    ready.store(false);
    publish();
    BOOST_CHECK_EQUAL(probes.load(), checked);
    BOOST_CHECK_EQUAL(io.beginCalls, 1);
  }
}

BOOST_AUTO_TEST_CASE(BootstrapCancellationCannotPublish)
{
  for (const bool prepareOnly : {true, false}) {
    Input input;
    TransportFixture io;
    std::atomic<bool> cancelled{false};
    input.control.cancelled = [&] { return cancelled.load(); };
    auto transport = io.transport();
    transport.bootstrapReady = [&](const NativeRequestControl&) {
      cancelled.store(true);
      return false;
    };
    auto publisher = NativeCanonicalPublisherTestAccess::create(
      std::move(transport), input.options, input.resolver());
    if (prepareOnly)
      BOOST_CHECK_THROW(publisher.prepare(input.model, input.control), std::runtime_error);
    else
      BOOST_CHECK_THROW(publisher(input.model, input.candidate, input.roles, input.control),
                        std::runtime_error);
    BOOST_CHECK_EQUAL(io.beginCalls, 0);
    BOOST_CHECK(io.payloads.empty());
    BOOST_CHECK_EQUAL(io.abortedPublications, 0);
  }
}

BOOST_AUTO_TEST_CASE(BootstrapDeadlineCannotPublish)
{
  for (const bool prepareOnly : {true, false}) {
    Input input;
    TransportFixture io;
    input.control.deadline = Clock::now() + std::chrono::milliseconds(50);
    auto transport = io.transport();
    transport.bootstrapReady = [](const NativeRequestControl&) { return false; };
    auto publisher = NativeCanonicalPublisherTestAccess::create(
      std::move(transport), input.options, input.resolver());
    if (prepareOnly)
      BOOST_CHECK_THROW(publisher.prepare(input.model, input.control), std::runtime_error);
    else
      BOOST_CHECK_THROW(publisher(input.model, input.candidate, input.roles, input.control),
                        std::runtime_error);
    BOOST_CHECK_EQUAL(io.beginCalls, 0);
    BOOST_CHECK(io.payloads.empty());
  }
}

BOOST_AUTO_TEST_CASE(BootstrapFailureCannotPublish)
{
  Input input;
  TransportFixture io;
  auto transport = io.transport();
  transport.bootstrapReady = [](const NativeRequestControl&) -> bool {
    throw std::runtime_error("controlled bootstrap failure");
  };
  auto publisher = NativeCanonicalPublisherTestAccess::create(
    std::move(transport), input.options, input.resolver());
  BOOST_CHECK_THROW(publisher.prepare(input.model, input.control), std::runtime_error);
  BOOST_CHECK_EQUAL(io.beginCalls, 0);
  BOOST_CHECK(io.payloads.empty());
}

BOOST_AUTO_TEST_CASE(PublicationCarriesValidatedModelSemantics)
{
  Input input(true); TransportFixture io;
  auto publisher = NativeCanonicalPublisherTestAccess::create(
    io.transport(), input.options, input.resolver());
  const auto prepared = publisher.prepare(input.model, input.control);
  BOOST_CHECK(!prepared.rootDataName.empty());
  BOOST_REQUIRE_EQUAL(io.contentNames.size(), io.labels.size());
  BOOST_CHECK(!io.contentNames.empty());
  for (size_t i = 0; i < io.labels.size(); ++i) {
    const auto& identity = io.contentNames[i];
    if (io.labels[i] == "di-canonical-source" || io.labels[i] == "di-canonical-root")
      BOOST_CHECK(identity.kind == ndn_service_framework::ContentDataKind::ModelFile);
    else if (io.labels[i] == "di-canonical-initializer")
      BOOST_CHECK(identity.kind == ndn_service_framework::ContentDataKind::Initializer);
    else BOOST_FAIL("unexpected publication payload role");
    BOOST_CHECK_EQUAL(identity.modelName, input.model.descriptor.modelName);
    BOOST_CHECK_EQUAL(identity.modelIdentity, input.model.descriptor.modelDigest().substr(7));
    const auto name = identity.contentPrefix(ndn::Name("/actual/producer"), std::string(64, 'a'));
    BOOST_CHECK(name[2] == ndn::name::Component("NDNSF-DI"));
    BOOST_CHECK(name[3] == ndn::name::Component("model"));
  }
}
BOOST_AUTO_TEST_CASE(LongModelLabelsRetainCompleteSemanticIdentity)
{
  Input left, right;
  left.model.descriptor.modelName = std::string(64, 'm') + "-left";
  right.model.descriptor.modelName = std::string(64, 'm') + "-right";
  BOOST_REQUIRE_EQUAL(left.model.descriptor.contentDigest, right.model.descriptor.contentDigest);
  TransportFixture leftIo, rightIo;
  auto leftPublisher = NativeCanonicalPublisherTestAccess::create(
    leftIo.transport(), left.options, left.resolver());
  auto rightPublisher = NativeCanonicalPublisherTestAccess::create(
    rightIo.transport(), right.options, right.resolver());
  leftPublisher.prepare(left.model, left.control);
  rightPublisher.prepare(right.model, right.control);
  BOOST_REQUIRE(!leftIo.contentNames.empty());
  BOOST_REQUIRE(!rightIo.contentNames.empty());
  const auto& a = leftIo.contentNames.front();
  const auto& b = rightIo.contentNames.front();
  BOOST_CHECK(a.modelIdentity != b.modelIdentity);
  BOOST_CHECK(a.objectIdentity() != b.objectIdentity());
  BOOST_CHECK(a.contentPrefix(ndn::Name("/producer"), std::string(64, 'a')) !=
              b.contentPrefix(ndn::Name("/producer"), std::string(64, 'a')));
}

BOOST_AUTO_TEST_CASE(RequestServiceUsesNameIdentityAndRejectsDifferentService)
{
  Input input;
  TransportFixture io;
  io.requestService = ndn::Name("/8=service");
  BOOST_REQUIRE(io.requestService == ndn::Name("/service"));
  auto publisher = NativeCanonicalPublisherTestAccess::create(
    io.transport(), input.options, input.resolver());
  BOOST_CHECK_NO_THROW(publisher.prepare(input.model, input.control));
  BOOST_CHECK(!io.payloads.empty());
  TransportFixture foreign;
  foreign.requestService = ndn::Name("/different-service");
  auto rejected = NativeCanonicalPublisherTestAccess::create(
    foreign.transport(), input.options, input.resolver());
  BOOST_CHECK_THROW(rejected.prepare(input.model, input.control), std::runtime_error);
  BOOST_CHECK_EQUAL(foreign.beginCalls, 1U);
  BOOST_CHECK(foreign.payloads.empty());
}

BOOST_AUTO_TEST_CASE(PublishedReferencesPreferImmutableManifestNames)
{
  Input input; TransportFixture io;
  io.mutate = [](LargeDataPublishResult& result) {
    result.manifestName = ndn::Name("/producer/NDNSF/RECEIPT")
      .append(ndn::name::Component(std::string(32, 'h')));
  };
  auto publisher = NativeCanonicalPublisherTestAccess::create(
    io.transport(), input.options, input.resolver());
  const auto prepared = publisher.prepare(input.model, input.control);
  const auto expected = ndn::Name("/producer/NDNSF/RECEIPT")
    .append(ndn::name::Component(std::string(32, 'h'))).toUri(ndn::name::UriFormat::CANONICAL);
  BOOST_CHECK_EQUAL(prepared.rootDataName, expected);
  BOOST_CHECK_EQUAL(prepared.sourceDataName, expected);
}
BOOST_AUTO_TEST_CASE(PreparedReceiptRetainsSourceCatalogMetadata)
{
  Input input;
  TransportFixture io;
  input.source->preparedMetadataJson = nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-prepared-metadata-v1"},
    {"sourceDigest", input.model.canonicalSourceDigest},
    {"fixtureGraphFacts", std::string(600000, 'x')}});
  BOOST_REQUIRE_GT(input.source->preparedMetadataJson.size(), 256U * 1024U);
  auto publisher = NativeCanonicalPublisherTestAccess::create(
    io.transport(), input.options, input.resolver());
  const auto prepared = publisher.prepare(input.model, input.control);
  BOOST_CHECK(!prepared.ownerRecoverableObjects);
  BOOST_CHECK_EQUAL(prepared.preparedMetadataJson, input.source->preparedMetadataJson);
  const auto calls = io.payloads.size();
  const auto cached = publisher.prepare(input.model, input.control);
  BOOST_CHECK_EQUAL(cached.preparedMetadataJson, prepared.preparedMetadataJson);
  BOOST_CHECK_EQUAL(io.payloads.size(), calls);
  auto referenceOnly = prepared;
  referenceOnly.preparedMetadataJson.clear();
  referenceOnly.preparedMetadataDataName = "/producer/prepared-metadata";
  referenceOnly.preparedMetadataDigest = nativePlanningDigest(input.source->preparedMetadataJson);
  referenceOnly.preparedMetadataBytes = NativeCanonicalMaterialPublicationMaxBytes + 1;
  BOOST_CHECK_THROW(referenceOnly.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(OwnerRecoverableReceiptRetainsExactCoreReferencesWithoutOperationalState)
{
  Input input;
  TransportFixture io;
  const NativeJson catalog{
    {"model", nativeParseJson(input.model.descriptor.canonicalJson())},
    {"source", {{"digest", input.model.canonicalSourceDigest},
                 {"bytes", input.model.canonicalSourceBytes}}},
    {"recipe", {{"artifact_profile_digest", input.options.artifactProfileDigest}}}};
  input.options.publicationIdentityDigest = nativeCanonicalPublicationIdentity(catalog);
  input.source->preparedMetadataJson = nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-prepared-metadata-v1"}, {"fixtureGraphFacts", "retained"}});
  auto invalid = io.transport();
  invalid.ownerRecoverable = true;
  BOOST_CHECK_THROW(NativeCanonicalPublisherTestAccess::create(
    invalid, input.options, input.resolver()), std::invalid_argument);
  auto transport = io.transport();
  const auto publish = transport.publish;
  transport.ownerRecoverable = true;
  transport.durablePublish = [publish](const auto& request, const auto& bytes,
    const auto& label, const auto& publicationIdentity, const auto& contentName,
    const auto& control) {
    auto result = publish(request, bytes, label, contentName, control);
    result.publicationIdentity = publicationIdentity;
    result.keyReferenceId = nativePlanningDigest("fixture object key reference");
    result.keyReferenceVersion = "v1";
    result.protectionEpoch = std::string(16, 'a');
    result.dataPrefix = contentName.contentPrefix(ndn::Name("/requester"),
      result.contentDigest.substr(7)).append("PROTECTED")
      .append(makeNamingDigestComponent(nativePlanningDigest("fixture protection").substr(7)))
      .append("DATA");
    result.encryptedDataName = result.dataPrefix;
    result.manifestName = ndn::Name(result.dataPrefix.getPrefix(-1)).append("MANIFEST").append(
      makeNamingDigestComponent(result.manifestDigest.substr(7)));
    result.ciphertextManifestDigest = result.manifestDigest;
    result.servingLocator = result.dataPrefix.toUri(ndn::name::UriFormat::CANONICAL);
    result.servingLease = std::make_shared<int>(1);
    result.rollbackDataNames = {result.encryptedDataName.toUri()};
    result.rollbackKeyId = "fixture rollback key";
    result.rollbackServiceName = request.serviceName.toUri();
    return result;
  };
  auto publisher = NativeCanonicalPublisherTestAccess::create(
    std::move(transport), input.options, input.resolver());
  const auto prepared = publisher.prepare(input.model, input.control);
  BOOST_REQUIRE(prepared.ownerRecoverableObjects);
  BOOST_REQUIRE_EQUAL(prepared.ownerRecoverableObjects->size(), io.payloads.size());
  BOOST_CHECK(!prepared.rollbackOwned);
  BOOST_CHECK_EQUAL(prepared.servingLeases.size(), io.payloads.size());
  const auto metadata = nativeParseJson(prepared.preparedMetadataJson);
  BOOST_CHECK_EQUAL(metadata.at("fixtureGraphFacts").get<std::string>(), "retained");
  BOOST_CHECK_EQUAL(metadata.at("protectedObjects").size() + 2, io.payloads.size());
  BOOST_REQUIRE_GE(io.labels.size(), 2U);
  BOOST_CHECK_EQUAL(io.labels.back(), "di-canonical-root");
  BOOST_CHECK_EQUAL(io.labels.at(io.labels.size() - 2), "di-prepared-metadata");
  const auto root = nativeParseJson(prepared.canonicalManifestJson);
  BOOST_CHECK_EQUAL(root.at("metadata").at("preparedMetadataDataName").get<std::string>(),
                    prepared.preparedMetadataDataName);
  BOOST_CHECK_EQUAL(prepared.preparedMetadataDigest,
                    nativePlanningDigest(prepared.preparedMetadataJson));
  for (std::size_t i = 0; i < io.payloads.size(); ++i) {
    const auto& object = prepared.ownerRecoverableObjects->at(i);
    BOOST_CHECK_EQUAL(object.objectId, io.labels.at(i));
    BOOST_CHECK_EQUAL(object.plaintextSize, io.payloads.at(i).size());
    BOOST_CHECK_EQUAL(object.contentDigest, nativePlanningDigest(
      io.payloads.at(i).data(), io.payloads.at(i).size()));
    BOOST_CHECK_EQUAL(object.manifestDigest, nativePlanningDigest("Core transport metadata"));
    BOOST_CHECK_EQUAL(object.keyReferenceId, nativePlanningDigest("fixture object key reference"));
    BOOST_CHECK(!object.publicationIdentity.empty());
    BOOST_CHECK(!object.servingLease);
    BOOST_CHECK(object.rollbackDataNames.empty());
    BOOST_CHECK(object.rollbackKeyId.empty());
    BOOST_CHECK(object.rollbackServiceName.empty());
    const auto reference = nativeProtectedObjectReference(object);
    const auto decoded = nativeProtectedObjectFromReference(reference);
    BOOST_CHECK(decoded.dataPrefix == object.dataPrefix);
    BOOST_CHECK(decoded.manifestName == object.manifestName);
    BOOST_CHECK_EQUAL(decoded.manifestDigest, object.manifestDigest);
    BOOST_CHECK_EQUAL(decoded.publicationIdentity, object.publicationIdentity);
    BOOST_CHECK(!decoded.servingLease);
    BOOST_CHECK(decoded.rollbackDataNames.empty());
    auto changed = reference;
    changed["secret"] = "unexpected";
    BOOST_CHECK_THROW(nativeProtectedObjectFromReference(changed), std::invalid_argument);
    changed = reference;
    changed["plaintextBytes"] = -1;
    BOOST_CHECK_THROW(nativeProtectedObjectFromReference(changed), std::invalid_argument);
    changed = reference;
    changed["protectionEpoch"] = "shared legacy epoch";
    BOOST_CHECK_THROW(nativeProtectedObjectFromReference(changed), std::invalid_argument);
  }

  const auto directory = std::filesystem::temp_directory_path() /
    ("spec220-protected-index-" + std::to_string(::getpid()));
  BOOST_REQUIRE(std::filesystem::create_directory(directory));
  struct Cleanup
  {
    std::filesystem::path path;
    ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
  } cleanup{directory};
  RepositoryPreparedLookupRequest lookup;
  lookup.serviceName = "/service";
  lookup.catalogConfigurationJson = nativeCanonicalJson(catalog);
  lookup.maxPublicationBytes = NativeCanonicalMaterialPublicationMaxBytes;
  lookup.deadline = input.control.deadline;
  lookup.requireActive = [] {};
  std::size_t reads = 0;
  std::size_t restores = 0;
  lookup.readProtected = [&](const auto& object, auto maximum, const auto& consume) {
    ++reads;
    BOOST_CHECK(object.manifestName.toUri(ndn::name::UriFormat::CANONICAL) == prepared.rootDataName ||
      object.manifestName.toUri(ndn::name::UriFormat::CANONICAL) == prepared.preparedMetadataDataName);
    BOOST_CHECK_LE(object.plaintextSize, maximum);
    for (std::size_t i = 0; i < prepared.ownerRecoverableObjects->size(); ++i) {
      if (prepared.ownerRecoverableObjects->at(i).manifestName == object.manifestName) {
        consume(ndn::span<const std::uint8_t>(io.payloads.at(i).data(), io.payloads.at(i).size()));
        return;
      }
    }
    throw std::runtime_error("fixture original object missing");
  };
  lookup.restoreProtected = [&](const auto& object) {
    ++restores;
    auto result = object;
    result.servingLease = std::make_shared<int>(1);
    return result;
  };
  ProtectedPreparationIndex index(directory, ndn::Name("/requester"));
  BOOST_CHECK(!index.lookup(lookup));
  for (const auto& object : *prepared.ownerRecoverableObjects) {
    const auto name = object.manifestName.toUri(ndn::name::UriFormat::CANONICAL);
    const auto producer = ProtectedBlobManifest::inferProducer(object.manifestName);
    BOOST_TEST_MESSAGE("INDEX_REFERENCE label=" << object.objectId
      << " root=" << (name == prepared.rootDataName)
      << " metadata=" << (name == prepared.preparedMetadataDataName)
      << " producer=" << (producer ? producer->toUri() : "unresolved")
      << " bytes=" << object.plaintextSize
      << " rootBytes=" << prepared.canonicalManifestJson.size()
      << " metadataBytes=" << prepared.preparedMetadataBytes
      << " rootDigest=" << (object.contentDigest == prepared.manifestDigest)
      << " metadataDigest=" << (object.contentDigest == prepared.preparedMetadataDigest));
  }
  index.remember(lookup, prepared);
  BOOST_CHECK_EQUAL(reads, 0U);
  ProtectedPreparationIndex restarted(directory, ndn::Name("/requester"));
  const auto hit = restarted.lookup(lookup);
  BOOST_REQUIRE(hit);
  BOOST_CHECK_EQUAL(hit->rootDataName, prepared.rootDataName);
  BOOST_CHECK_EQUAL(hit->canonicalManifestJson, prepared.canonicalManifestJson);
  BOOST_CHECK_EQUAL(hit->preparedMetadataJson, prepared.preparedMetadataJson);
  BOOST_CHECK_EQUAL(hit->publishedBytes, 0U);
  BOOST_CHECK_EQUAL(reads, 2U);
  BOOST_CHECK_EQUAL(restores, prepared.ownerRecoverableObjects->size());
  BOOST_CHECK_EQUAL(hit->servingLeases.size(), restores);
  const auto publishedCount = io.payloads.size();
  index.remember(lookup, prepared);
  BOOST_CHECK_EQUAL(io.payloads.size(), publishedCount);
  const auto durableRoot = directory / "durable";
  BOOST_REQUIRE(std::filesystem::create_directory(durableRoot));
  ProtectedPreparationIndex durable(durableRoot, ndn::Name("/requester"));
  const auto migrated = durable.lookupWithLegacy(lookup, directory);
  BOOST_REQUIRE(migrated);
  BOOST_CHECK_EQUAL(migrated->rootDataName, prepared.rootDataName);
  BOOST_CHECK_EQUAL(migrated->preparedMetadataJson, prepared.preparedMetadataJson);
  BOOST_CHECK_EQUAL(migrated->publishedBytes, 0U);
  BOOST_CHECK_EQUAL(io.payloads.size(), publishedCount);
  // The raw cache may disappear in the next allocation. Its private index is
  // no longer needed once the verified pointer has moved to durable storage.
  const auto withoutRawCache = durable.lookupWithLegacy(lookup, directory / "absent-raw-cache");
  BOOST_REQUIRE(withoutRawCache);
  BOOST_CHECK_EQUAL(withoutRawCache->manifestDigest, prepared.manifestDigest);
  ProtectedPreparationIndex missingParent(directory / "absent-parent", ndn::Name("/requester"));
  BOOST_CHECK_THROW(missingParent.lookupWithLegacy(lookup, directory), RepositorySourceError);
  BOOST_REQUIRE(restarted.lookup(lookup));
  ProtectedPreparationIndex otherOwner(durableRoot, ndn::Name("/other-requester"));
  BOOST_CHECK(!otherOwner.lookupWithLegacy(lookup, directory));
  auto cancelled = lookup;
  cancelled.requireActive = [] { throw std::runtime_error("fixture cancelled"); };
  BOOST_CHECK_THROW(restarted.lookup(cancelled), RepositorySourceError);
  BOOST_CHECK_THROW(durable.lookupWithLegacy(cancelled, directory), RepositorySourceError);
  auto expired = lookup;
  expired.deadline = Clock::now() - std::chrono::seconds(1);
  BOOST_CHECK_THROW(restarted.lookup(expired), RepositorySourceError);
  auto altered = catalog;
  altered["source"]["digest"] = nativePlanningDigest("different source");
  auto different = lookup;
  different.catalogConfigurationJson = nativeCanonicalJson(altered);
  BOOST_CHECK(!restarted.lookup(different));
  auto corruptRead = lookup;
  corruptRead.readProtected = [](const auto&, auto, const auto& consume) {
    const std::vector<std::uint8_t> corrupt{'x'};
    consume(ndn::span<const std::uint8_t>(corrupt.data(), corrupt.size()));
  };
  BOOST_CHECK_THROW(restarted.lookup(corruptRead), RepositorySourceError);
  auto conflict = lookup;
  conflict.restoreProtected = [](const auto& object) {
    auto result = object;
    result.contentDigest = nativePlanningDigest("different object");
    result.servingLease = std::make_shared<int>(1);
    return result;
  };
  BOOST_CHECK_THROW(restarted.lookup(conflict), RepositorySourceError);
  std::filesystem::path pointer;
  for (const auto& entry : std::filesystem::directory_iterator(directory / "prepared-owner-index")) {
    if (entry.path().extension() == ".json") {
      BOOST_REQUIRE(pointer.empty());
      pointer = entry.path();
    }
  }
  BOOST_REQUIRE(!pointer.empty());
  std::filesystem::permissions(pointer, std::filesystem::perms::group_read,
                               std::filesystem::perm_options::add);
  BOOST_CHECK_THROW(restarted.lookup(lookup), RepositorySourceError);
  std::filesystem::permissions(pointer,
    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
  const auto saved = std::filesystem::path(pointer.string() + ".saved");
  std::filesystem::rename(pointer, saved);
  std::filesystem::create_symlink(saved, pointer);
  BOOST_CHECK_THROW(restarted.lookup(lookup), RepositorySourceError);
  std::filesystem::remove(pointer);
  std::filesystem::rename(saved, pointer);
  {
    std::ofstream output(pointer, std::ios::trunc);
    output << "{}";
  }
  BOOST_CHECK_THROW(restarted.lookup(lookup), RepositorySourceError);
  BOOST_REQUIRE(durable.lookupWithLegacy(lookup, directory));
  // A corrupt authoritative pointer must never fall back to the legacy root.
  for (const auto& entry : std::filesystem::directory_iterator(durableRoot / "prepared-owner-index")) {
    if (entry.path().extension() == ".json") {
      std::ofstream output(entry.path(), std::ios::trunc);
      output << "{}";
    }
  }
  BOOST_CHECK_THROW(durable.lookupWithLegacy(lookup, directory / "absent-raw-cache"),
                    RepositorySourceError);
}

BOOST_AUTO_TEST_CASE(PreparedMetadataOversizeFailsBeforePublication)
{
  Input input;
  TransportFixture io;
  input.source->preparedMetadataJson.assign(NativeCanonicalMaterialPublicationMaxBytes + 1, 'x');
  auto publisher = NativeCanonicalPublisherTestAccess::create(
    io.transport(), input.options, input.resolver());
  BOOST_CHECK_THROW(publisher.prepare(input.model, input.control), std::invalid_argument);
  BOOST_CHECK(io.payloads.empty());
  BOOST_CHECK_EQUAL(io.beginCalls, 0U);
}

BOOST_AUTO_TEST_CASE(PreparedReceiptIndexDoesNotRetainServingLeases)
{
  Input input; TransportFixture io;
  std::vector<std::weak_ptr<void>> observed;
  io.mutate = [&](LargeDataPublishResult& result) {
    result.servingLease = std::make_shared<int>(1);
    observed.emplace_back(result.servingLease);
  };
  auto publisher = NativeCanonicalPublisherTestAccess::create(
    io.transport(), input.options, input.resolver());
  auto first = publisher.prepare(input.model, input.control);
  const auto publications = publisher.stats().publicationCalls;
  BOOST_REQUIRE_GT(publications, 0U);
  auto second = publisher.prepare(input.model, input.control);
  BOOST_CHECK_EQUAL(publisher.stats().publicationCalls, publications);
  first.servingLeases.clear();
  for (const auto& pin : observed) BOOST_CHECK(!pin.expired());
  second.servingLeases.clear();
  // prepare() joins its receipt handoff through the returned future; the
  // detached producer may still be releasing its local result briefly.
  const auto until = Clock::now() + std::chrono::seconds(2);
  while (Clock::now() < until &&
         std::any_of(observed.begin(), observed.end(), [](const auto& p) { return !p.expired(); }))
    std::this_thread::yield();
  for (const auto& pin : observed) BOOST_CHECK(pin.expired());
  auto renewed = publisher.prepare(input.model, input.control);
  BOOST_CHECK_EQUAL(publisher.stats().publicationCalls, publications * 2);
  BOOST_CHECK(!renewed.servingLeases.empty());
}

BOOST_AUTO_TEST_CASE(CatalogComposesOwnedInputInspectionRolesAndPublication)
{
  for (bool external : {false, true}) {
    Input input(external); TransportFixture io;
    const auto& original = input.roles.front();
    NativeCanonicalCatalogEntry entry;
    entry.model = input.model; entry.source = *input.source;
    entry.recipe = {original.artifactProfileDigest, original.assemblerDescriptorDigest,
      original.backendAbi, original.precision, original.quantization, original.layout, original.padding,
      original.protectionEpoch, original.maxSourceBytes, original.maxAssembledBytes, original.maxNodes};
    entry.nodes = {{"n0", {0}}, {"n1", {1}}}; entry.publication = input.options;
    entry.format = external ? NativeCatalogModelAdapter::Format::JsonBytes : NativeCatalogModelAdapter::Format::OpaqueBytes;
    entry.maxPayloadBytes = 1024;
    NativeAssemblyControl control{input.control.deadline, [&] { input.control.requireActive(); },
      entry.recipe.maxSourceBytes, entry.recipe.maxAssembledBytes};
    auto alternate = entry;
    alternate.model.descriptor.modelName = "second-model";
    alternate.model.descriptor.contentDigest = nativePlanningDigest("second-model");
    auto catalog = std::make_unique<NativeCanonicalPreparationCatalog>(
      std::vector<NativeCanonicalCatalogEntry>{entry, alternate}, control);
    const auto sourceSnapshot = catalog->sourceRefFor(entry.model.descriptor);
    BOOST_REQUIRE(sourceSnapshot);
    BOOST_REQUIRE(!sourceSnapshot->modelBytes.empty());
    const auto registry = catalog->adapters();
    BOOST_CHECK(registry->frozen());
    const auto preparation = NativeCanonicalCatalogTestAccess::create(*catalog, io.transport());
    auto abstract = input.candidate;
    abstract.roleStateInputsByRole["/role"] = {{"state-in", "float32", {std::string("state")}, std::nullopt}};
    abstract.roleStateOutputsByRole["/role"] = {{"state-out", "float32", {std::string("state")}, std::nullopt}};
    abstract.candidateDigest = abstract.computedDigest();
    const NativeStateTensorMapping stateMap{{{"/role", {{"state-in", {"X"}}}}},
      {{"/role", {{"state-out", {"Y"}}}}}};
    const auto concrete = catalog->bindStateContracts(entry.model, abstract, stateMap, input.control);
    BOOST_CHECK_NE(concrete.candidateDigest, abstract.candidateDigest);
    BOOST_CHECK_EQUAL(concrete.roleStateInputsByRole.at("/role").front().name, "X");
    BOOST_CHECK_EQUAL(*concrete.roleStateInputsByRole.at("/role").front().estimatedBytes, 16);
    BOOST_CHECK_NO_THROW(preparation->prepareRoles(entry.model, concrete, input.control));
    for (const auto& bad : std::vector<NativeStateTensorMapping>{
      {}, {{{"foreign-role", {{"state-in", {"X"}}}}}, stateMap.outputs},
      {{{"/role", {{"foreign-state", {"X"}}}}}, stateMap.outputs},
      {{{"/role", {{"state-in", {"Y"}}}}}, stateMap.outputs},
      {{{"/role", {{"state-in", {"X", "X"}}}}}, stateMap.outputs},
      {{{"/role", {{"state-in", {}}}}}, stateMap.outputs}})
      BOOST_CHECK_THROW(catalog->bindStateContracts(entry.model, abstract, bad, input.control), std::invalid_argument);
    auto wrongDtype = abstract;
    wrongDtype.roleStateInputsByRole["/role"].front().dtype = "int64";
    wrongDtype.candidateDigest = wrongDtype.computedDigest();
    BOOST_CHECK_THROW(catalog->bindStateContracts(entry.model, wrongDtype, stateMap, input.control), std::invalid_argument);
    BOOST_CHECK_EQUAL(abstract.roleStateInputsByRole.at("/role").front().name, "state-in");
    BOOST_CHECK_THROW(catalog->makePreparation(nullptr, "/service"), std::invalid_argument);
    catalog.reset(); // Returned ports own the pinned state, not the factory.
    BOOST_CHECK(!sourceSnapshot->modelBytes.empty());
    input.source->modelBytes.clear(); // Caller mutation must not change the owned publication source.
    const std::vector<std::uint8_t> payload = external ? std::vector<std::uint8_t>{'{', '}'} :
      std::vector<std::uint8_t>{0, 255, 1};
    const auto prepared = preparation->prepareInput(entry.model.descriptor, "inference",
      nativePlanningDigest("input"), nativePlanningDigest("options"), payload, {}, input.control.deadline);
    BOOST_CHECK(prepared.payload == payload);
    const auto inspectedOwner = preparation->inspectModelShared(prepared);
    BOOST_REQUIRE(inspectedOwner);
    const auto& inspected = *inspectedOwner;
    BOOST_CHECK_EQUAL(preparation->inspectModel(prepared).canonicalSourceDigest,
                      inspected.canonicalSourceDigest);
    BOOST_CHECK_EQUAL(inspected.canonicalSourceDigest, entry.model.canonicalSourceDigest);
    const auto secondInput = preparation->prepareInput(alternate.model.descriptor, "inference",
      nativePlanningDigest("input"), nativePlanningDigest("options"), payload, {}, input.control.deadline);
    BOOST_CHECK_EQUAL(preparation->inspectModel(secondInput).descriptor.contentDigest, alternate.model.descriptor.contentDigest);
    BOOST_CHECK(registry->find(inspected.descriptor.adapterId)->decodeResult(payload) == payload);
    auto roles = preparation->prepareRoles(inspected, input.candidate, input.control);
    BOOST_REQUIRE_EQUAL(roles.size(), 1);
    auto proposal = input.proposal(); proposal.roles = roles;
    proposal.roles.front().backend = original.backend;
    auto invalidCopy = inspected;
    invalidCopy.graph.nodes.clear();
    BOOST_CHECK_THROW(preparation->ensureArtifacts(
                        invalidCopy, input.candidate, proposal, input.control),
                      std::invalid_argument);
    auto foreignCandidate = input.candidate;
    foreignCandidate.graphDigest = nativePlanningDigest("foreign-graph");
    BOOST_CHECK_THROW(preparation->ensureArtifacts(
                        inspected, foreignCandidate, proposal, input.control),
                      std::invalid_argument);
    const auto published = preparation->ensureArtifacts(inspected, input.candidate, proposal, input.control);
    const auto rebound = NativeRequestPreparation::bindPublishedRoles(inspected, input.candidate, proposal.roles, published);
    BOOST_CHECK_EQUAL(assembleNativeCertifiedOnnxModel(entry.source, rebound.front(), control).modelDigest, original.artifactDigest);
    BOOST_REQUIRE(!io.payloads.empty());
    BOOST_CHECK_EQUAL(nativePlanningDigest(io.payloads.front().data(), io.payloads.front().size()), entry.model.canonicalSourceDigest);
    auto foreign = prepared;
    foreign.expectedModel.semanticsDigest = nativePlanningDigest("foreign semantics");
    BOOST_CHECK_THROW(preparation->inspectModel(foreign), std::runtime_error);
    auto wrongSource = inspected; wrongSource.canonicalSourceName = "/foreign/source";
    BOOST_CHECK_THROW(preparation->prepareRoles(wrongSource, input.candidate, input.control), std::invalid_argument);
    const auto publishedCount = io.payloads.size();
    auto cancelled = input.control; cancelled.cancelled = [] { return true; };
    BOOST_CHECK_THROW(preparation->ensureArtifacts(inspected, input.candidate, proposal, cancelled), std::runtime_error);
    BOOST_CHECK_EQUAL(io.payloads.size(), publishedCount);
    BOOST_CHECK_THROW(NativeCanonicalPreparationCatalog(std::vector<NativeCanonicalCatalogEntry>{entry, entry}, control), std::invalid_argument);
    auto conflict = entry; conflict.maxPayloadBytes = 2048;
    BOOST_CHECK_THROW(NativeCanonicalPreparationCatalog(std::vector<NativeCanonicalCatalogEntry>{entry, conflict}, control), std::invalid_argument);
    auto corrupt = entry; corrupt.source.modelBytes.front() ^= 1;
    BOOST_CHECK_THROW(NativeCanonicalPreparationCatalog(std::vector<NativeCanonicalCatalogEntry>{corrupt}, control), std::invalid_argument);
    BOOST_CHECK_THROW(NativeCanonicalPreparationCatalog({}, control), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(StateBindingConsumesActualCausalOnnxExport)
{
  Input input;
  std::ifstream file("tests/fixtures/spec175/tiny-causal-lm-v1/one-role/role-0.onnx", std::ios::binary);
  BOOST_REQUIRE(file.good());
  input.source->modelBytes.assign(std::istreambuf_iterator<char>(file), {});
  input.model.canonicalSourceBytes = input.source->modelBytes.size();
  input.model.canonicalSourceDigest = nativePlanningDigest(input.source->modelBytes.data(), input.source->modelBytes.size());
  // Frozen Spec175 manifest; no expected source is generated by the code under test.
  BOOST_CHECK_EQUAL(input.model.canonicalSourceDigest, "sha256:fe41db5c2c8397b62fc8983307b0d755706b7008c56958882cb247ca46594f82");
  NativeAssemblyControl control{input.control.deadline, [&] { input.control.requireActive(); }, 1024 * 1024, 1024 * 1024};
  const auto actual = inspectNativeOnnxSourceGraph(*input.source, input.model.descriptor, control);
  const auto repeated = std::find_if(actual.graph.edges.begin(), actual.graph.edges.end(),
    [](const auto& edge) { return edge.id == "hidden_zero"; });
  BOOST_REQUIRE(repeated != actual.graph.edges.end());
  BOOST_CHECK_EQUAL(repeated->consumers.size(), 2);
  BOOST_CHECK_EQUAL(nativeParseJson(actual.graphMetadataJson).at("tensorConsumers").at("hidden_zero").size(), 7);
  input.model.graph = actual.graph; input.model.descriptor.graphDigest = actual.graph.graphDigest;
  input.model.canonicalGraphDigest = actual.canonicalIdentity.graphDigest;
  input.candidate.model = input.model.descriptor; input.candidate.graphDigest = actual.graph.graphDigest;
  input.candidate.nodeRoles.clear();
  for (const auto& node : actual.graph.nodes) input.candidate.nodeRoles[node.id] = "/role";
  // The frozen causal graph has 13 initializers totaling 504 bytes.
  input.candidate.requirementsByRole.at("/role").weightBytes = 504;
  NativeStateTensorMapping mapping;
  for (const std::string family : {"attention_kv", "recurrent_state", "convolution_state"}) {
    input.candidate.roleStateInputsByRole["/role"].push_back({family + "_in", "float32", {std::string("abstract-state")}, std::nullopt});
    input.candidate.roleStateOutputsByRole["/role"].push_back({family + "_out", "float32", {std::string("abstract-state")}, std::nullopt});
    mapping.inputs["/role"][family + "_in"] = {family + "_in"};
    mapping.outputs["/role"][family + "_out"] = {family + "_out"};
  }
  input.candidate.candidateDigest = input.candidate.computedDigest();
  const auto& original = input.roles.front();
  const NativeRoleRecipeProfile profile{original.artifactProfileDigest, original.assemblerDescriptorDigest,
    original.backendAbi, original.precision, original.quantization, original.layout, original.padding,
    original.protectionEpoch, control.maxSourceBytes, control.maxAssembledBytes, 1024};
  const NativeCanonicalRolePreparer owner(input.model, *input.source, profile, control);
  BOOST_CHECK_THROW(owner.prepare(input.model, input.candidate, input.control), std::invalid_argument);
  const auto bound = owner.bindStateContracts(input.model, input.candidate, mapping, input.control);
  BOOST_CHECK_NE(bound.candidateDigest, input.candidate.candidateDigest);
  const std::vector<std::variant<std::int64_t, std::string>> stateShape = {std::int64_t(4), std::int64_t(8)};
  for (const auto* contracts : {&bound.roleStateInputsByRole, &bound.roleStateOutputsByRole}) {
    BOOST_REQUIRE_EQUAL(contracts->at("/role").size(), 3);
    for (const auto& tensor : contracts->at("/role")) {
      BOOST_CHECK(tensor.shape == stateShape);
      BOOST_REQUIRE(tensor.estimatedBytes);
      BOOST_CHECK_EQUAL(*tensor.estimatedBytes, 128);
    }
  }
  BOOST_CHECK_NO_THROW(owner.prepare(input.model, bound, input.control));
  auto cancelled = input.control; cancelled.cancelled = [] { return true; };
  BOOST_CHECK_THROW(owner.bindStateContracts(input.model, input.candidate, mapping, cancelled), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(SourceDefinedStateBindingPreservesMixedBoundaryTypesAndResourceFloor)
{
  Input input;
  onnx::ModelProto source;
  source.set_ir_version(8);
  source.add_opset_import()->set_version(17);
  auto* graph = source.mutable_graph();
  graph->set_name("spec220-mixed-state");
  const auto addValue = [] (onnx::ValueInfoProto* value, const char* name,
                            int type, std::int64_t count) {
    value->set_name(name);
    auto* tensor = value->mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(type);
    tensor->mutable_shape()->add_dim()->set_dim_value(count);
  };
  addValue(graph->add_input(), "X", onnx::TensorProto::FLOAT, 4);
  addValue(graph->add_output(), "Y", onnx::TensorProto::FLOAT, 4);
  addValue(graph->add_input(), "counter", onnx::TensorProto::INT64, 1);
  addValue(graph->add_output(), "counter_out", onnx::TensorProto::INT64, 1);
  auto* weight = graph->add_initializer();
  weight->set_name("W");
  weight->set_data_type(onnx::TensorProto::FLOAT);
  weight->add_dims(4);
  for (int i = 0; i < 4; ++i) weight->add_float_data(1.0F);
  auto* add = graph->add_node();
  add->set_op_type("Add");
  add->add_input("X");
  add->add_input("W");
  add->add_output("Y");
  auto* identity = graph->add_node();
  identity->set_op_type("Identity");
  identity->add_input("counter");
  identity->add_output("counter_out");
  onnx::checker::check_model(source);
  const auto bytes = source.SerializeAsString();
  input.source->modelBytes.assign(bytes.begin(), bytes.end());
  input.model.canonicalSourceBytes = bytes.size();
  input.model.canonicalSourceDigest = nativePlanningDigest(bytes);
  NativeAssemblyControl control{input.control.deadline,
    [&] { input.control.requireActive(); }, 1U << 20, 1U << 20};
  const auto inspected = inspectNativeOnnxSourceGraph(*input.source, input.model.descriptor, control);
  input.model.graph = inspected.graph;
  input.model.descriptor.graphDigest = inspected.graph.graphDigest;
  input.model.canonicalGraphDigest = inspected.canonicalIdentity.graphDigest;
  input.candidate.model = input.model.descriptor;
  input.candidate.graphDigest = inspected.graph.graphDigest;
  input.candidate.nodeRoles.clear();
  for (const auto& node : inspected.graph.nodes) input.candidate.nodeRoles[node.id] = "/role";
  input.candidate.requirementsByRole.at("/role").weightBytes = 16;
  // Equal count and plausible weight dtype still do not identify actual state.
  input.candidate.roleStateInputsByRole["/role"] = {
    {"old-state-a", "float32", {std::int64_t(4)}, 16},
    {"old-state-b", "float32", {std::int64_t(1)}, 4}};
  input.candidate.roleStateOutputsByRole = input.candidate.roleStateInputsByRole;
  input.candidate.candidateDigest = input.candidate.computedDigest();
  const auto& original = input.roles.front();
  const NativeRoleRecipeProfile profile{original.artifactProfileDigest,
    original.assemblerDescriptorDigest, original.backendAbi, original.precision,
    original.quantization, original.layout, original.padding, original.protectionEpoch,
    control.maxSourceBytes, control.maxAssembledBytes, 16};
  const NativeCanonicalRolePreparer owner(input.model, *input.source, profile, control);
  const NativeStateTensorMapping mapping{
    {{"/role", {{"cache", {"X"}}, {"counter-state", {"counter"}}}}},
    {{"/role", {{"cache", {"Y"}}, {"counter-state", {"counter_out"}}}}}};
  BOOST_CHECK_THROW(owner.bindStateContracts(input.model, input.candidate, mapping, input.control),
                    std::invalid_argument);
  const auto bound = owner.bindStateContracts(input.model, input.candidate, mapping,
    input.control, NativeStateContractBinding::SOURCE);
  using StateShape = std::vector<std::variant<std::int64_t, std::string>>;
  for (const auto* states : {&bound.roleStateInputsByRole, &bound.roleStateOutputsByRole}) {
    BOOST_REQUIRE_EQUAL(states->at("/role").size(), 2U);
    BOOST_CHECK_EQUAL(states->at("/role")[0].dtype, "float32");
    BOOST_CHECK_EQUAL(states->at("/role")[1].dtype, "int64");
    BOOST_CHECK_EQUAL(states->at("/role")[0].estimatedBytes.value(), 16U);
    BOOST_CHECK_EQUAL(states->at("/role")[1].estimatedBytes.value(), 8U);
    BOOST_CHECK(states->at("/role")[0].shape ==
                StateShape{std::int64_t(4)});
    BOOST_CHECK(states->at("/role")[1].shape ==
                StateShape{std::int64_t(1)});
  }
  BOOST_CHECK_EQUAL(bound.roleStateInputsByRole.at("/role")[0].name, "X");
  BOOST_CHECK_EQUAL(bound.roleStateOutputsByRole.at("/role")[1].name, "counter_out");
  BOOST_CHECK_NO_THROW(owner.prepare(input.model, bound, input.control));
  for (int failure = 0; failure != 4; ++failure) {
    auto bad = input.candidate;
    auto& resource = bad.requirementsByRole.at("/role");
    if (failure == 0) resource.weightBytes = 15;
    if (failure == 1) resource.workspaceBytes.reset();
    if (failure == 2) resource.weightBytes = std::numeric_limits<std::uint64_t>::max();
    if (failure == 3) resource.safetyMargin = std::numeric_limits<double>::infinity();
    // Digest serialization itself validates malformed profiles and may reject.
    const auto bindBad = [&] {
      bad.candidateDigest = bad.computedDigest();
      return owner.bindStateContracts(input.model, bad, mapping, input.control,
        NativeStateContractBinding::SOURCE);
    };
    if (failure == 2)
      BOOST_CHECK_THROW(bindBad(), std::overflow_error);
    else
      BOOST_CHECK_THROW(bindBad(), std::invalid_argument);
  }
  for (int failure = 0; failure != 4; ++failure) {
    auto bad = mapping;
    if (failure == 0) bad.inputs.clear();
    if (failure == 1) bad.inputs["/role"]["counter-state"] = {"X"};
    if (failure == 2) bad.inputs["/role"]["counter-state"] = {"foreign-tensor"};
    if (failure == 3) bad.inputs["foreign-role"] = bad.inputs.at("/role");
    BOOST_CHECK_THROW(owner.bindStateContracts(input.model, input.candidate, bad,
      input.control, NativeStateContractBinding::SOURCE), std::invalid_argument);
  }
  auto cancelled = input.control;
  cancelled.cancelled = [] { return true; };
  BOOST_CHECK_THROW(owner.bindStateContracts(input.model, input.candidate, mapping,
    cancelled, NativeStateContractBinding::SOURCE), std::runtime_error);
  BOOST_CHECK_EQUAL(input.candidate.roleStateInputsByRole.at("/role")[1].dtype, "float32");
}

BOOST_AUTO_TEST_CASE(GeneratedMultiLayerModelSplitsAndAssemblesWithCleanup)
{
  // This is intentionally a small deterministic model: it exercises the
  // production source inspector, role preparer, canonical split contracts,
  // and the same certified assembly chain used by the worker without
  // downloading a Qwen-sized artifact in a unit test.
  const auto root = std::filesystem::temp_directory_path() /
                    ("spec189-generated-multilayer-fc-" + std::to_string(::getpid()));
  std::error_code cleanupError;
  std::filesystem::remove_all(root, cleanupError);
  struct Cleanup
  {
    std::filesystem::path path;
    void clear() noexcept
    {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
    ~Cleanup() noexcept
    {
      clear();
    }
  } cleanup{root};

  const auto modelPath = root / "model.onnx";
  if (!std::filesystem::exists(modelPath)) {
    onnx::ModelProto generated;
    generated.set_ir_version(8);
    generated.add_opset_import()->set_version(17);
    generated.mutable_graph()->set_name("spec189-random-multilayer-fc");
    const auto addValue = [] (onnx::ValueInfoProto* value, const std::string& name,
                              std::int64_t width) {
      value->set_name(name);
      auto* tensor = value->mutable_type()->mutable_tensor_type();
      tensor->set_elem_type(onnx::TensorProto::FLOAT);
      tensor->mutable_shape()->add_dim()->set_dim_value(1);
      tensor->mutable_shape()->add_dim()->set_dim_value(width);
    };
    addValue(generated.mutable_graph()->add_input(), "X", 2);
    addValue(generated.mutable_graph()->add_output(), "Y", 2);
    addValue(generated.mutable_graph()->add_value_info(), "H0", 3);
    addValue(generated.mutable_graph()->add_value_info(), "H1", 3);
    addValue(generated.mutable_graph()->add_value_info(), "H2", 3);

    std::mt19937 generator(189U);
    std::uniform_real_distribution<float> distribution(-0.5F, 0.5F);
    const auto addInitializer = [&] (const std::string& name,
                                     std::initializer_list<std::int64_t> shape,
                                     std::size_t count) {
      auto* tensor = generated.mutable_graph()->add_initializer();
      tensor->set_name(name);
      tensor->set_data_type(onnx::TensorProto::FLOAT);
      for (const auto dimension : shape) tensor->add_dims(dimension);
      std::vector<float> values;
      values.reserve(count);
      for (std::size_t i = 0; i < count; ++i) values.push_back(distribution(generator));
      tensor->set_raw_data(reinterpret_cast<const char*>(values.data()),
                           values.size() * sizeof(float));
    };
    addInitializer("W0", {2, 3}, 6);
    addInitializer("B0", {3}, 3);
    addInitializer("W1", {3, 2}, 6);

    auto* matmul0 = generated.mutable_graph()->add_node();
    matmul0->set_name("fc-0"); matmul0->set_op_type("MatMul");
    matmul0->add_input("X"); matmul0->add_input("W0"); matmul0->add_output("H0");
    auto* add0 = generated.mutable_graph()->add_node();
    add0->set_name("bias-0"); add0->set_op_type("Add");
    add0->add_input("H0"); add0->add_input("B0"); add0->add_output("H1");
    auto* relu = generated.mutable_graph()->add_node();
    relu->set_name("activation-0"); relu->set_op_type("Relu");
    relu->add_input("H1"); relu->add_output("H2");
    auto* matmul1 = generated.mutable_graph()->add_node();
    matmul1->set_name("fc-1"); matmul1->set_op_type("MatMul");
    matmul1->add_input("H2"); matmul1->add_input("W1"); matmul1->add_output("Y");

    std::string wire;
    BOOST_REQUIRE(generated.SerializeToString(&wire));
    std::filesystem::create_directories(root);
    std::ofstream output(modelPath, std::ios::binary);
    BOOST_REQUIRE(output.good());
    output.write(wire.data(), static_cast<std::streamsize>(wire.size()));
  }

  NativeCanonicalSource source;
  std::ifstream input(modelPath, std::ios::binary);
  BOOST_REQUIRE(input.good());
  source.modelBytes.assign(std::istreambuf_iterator<char>(input), {});
  BOOST_REQUIRE(!source.modelBytes.empty());

  auto descriptor = fixture::completeModel({
    "spec189-generated-fc", nativePlanningDigest("generated-content"),
    nativePlanningDigest("generated-semantics"), nativePlanningDigest("placeholder-graph"),
    "onnx", "float32", "spec189-generated-adapter", "1"});
  NativeAssemblyControl control{
    Clock::now() + std::chrono::seconds(30), [] {}, 1U << 20, 1U << 20};
  const auto sourceGraph = inspectNativeOnnxSourceGraph(source, descriptor, control);
  const auto identity = canonicalOnnxSourceIdentity(source, control);
  BOOST_REQUIRE_EQUAL(sourceGraph.graph.nodes.size(), 4U);
  BOOST_CHECK_EQUAL(sourceGraph.graph.nodes.front().opType, "MatMul");
  BOOST_CHECK_EQUAL(sourceGraph.graph.nodes.back().opType, "MatMul");
  descriptor.graphDigest = sourceGraph.graph.graphDigest;

  NativeInspectedModel inspected;
  inspected.descriptor = descriptor;
  inspected.graph = sourceGraph.graph;
  inspected.canonicalSourceName = "/spec189/generated-fc/model.onnx";
  inspected.canonicalSourceDigest = nativePlanningDigest(
    source.modelBytes.data(), source.modelBytes.size());
  inspected.canonicalSourceBytes = source.modelBytes.size();
  inspected.modelManifestDigest = nativePlanningDigest("generated-manifest");
  inspected.canonicalGraphDigest = identity.graphDigest;

  const std::string firstRole = "/role/0";
  const std::string secondRole = "/role/1";
  NativeSplitCandidate candidate;
  candidate.source = "PRE_SPLIT";
  candidate.splitter = {"spec189-generated-splitter", "1",
                        nativePlanningDigest("generated-splitter")};
  candidate.model = descriptor;
  candidate.graphDigest = sourceGraph.graph.graphDigest;
  candidate.executionPlan.roles = {firstRole, secondRole};
  candidate.executionPlan.dependencies = {
    NativeDependencySpec{
      {firstRole}, {secondRole}, "generated-h2", "/spec189/generated",
      "/spec189/generated/{sessionId}/{producerRole}/{sequence}", 1, 64, {"H2"}}};
  candidate.fragmentsByRole = {{firstRole, nativePlanningDigest("fragment-0")},
                               {secondRole, nativePlanningDigest("fragment-1")}};
  candidate.artifactsByRole = {
    {firstRole, {nativePlanningDigest("artifact-0")}},
    {secondRole, {nativePlanningDigest("artifact-1")}}};
  candidate.rankArtifactDigestsByRole = candidate.artifactsByRole;
  candidate.roleStateInputsByRole = {
    {firstRole, {{"X", "float32", {std::int64_t{1}, std::int64_t{2}}, std::uint64_t{8}}}},
    {secondRole, {{"H2", "float32", {std::int64_t{1}, std::int64_t{3}}, std::uint64_t{12}}}}};
  candidate.roleStateOutputsByRole = {
    {firstRole, {{"H0", "float32", {std::int64_t{1}, std::int64_t{3}}, std::uint64_t{12}}}},
    {secondRole, {{"Y", "float32", {std::int64_t{1}, std::int64_t{2}}, std::uint64_t{8}}}}};
  candidate.tensorDegreesByRole = {{firstRole, 1}, {secondRole, 1}};
  candidate.requirementsByRole = {
    {firstRole, {{"onnxruntime"}, 36, 0, 0, 0, 0, 1.0}},
    {secondRole, {{"onnxruntime"}, 24, 0, 0, 0, 0, 1.0}}};
  candidate.crossPartitionTensors = {"H2"};
  candidate.inputIngressRole = firstRole;
  candidate.resultEgressRole = secondRole;
  for (std::size_t index = 0; index < sourceGraph.graph.nodes.size(); ++index) {
    candidate.nodeRoles[sourceGraph.graph.nodes[index].id] = index < 3 ? firstRole : secondRole;
  }
  candidate.candidateDigest = candidate.computedDigest();

  const NativeRoleRecipeProfile profile{
    nativePlanningDigest("generated-profile"),
    // The generated inline FLOAT initializers use the current normalization
    // revision, so keep the production descriptor gate explicit in the test.
    "sha256:5291ee00f425c59605f72e26c9b27a73aca43b976421218515fc1a38085c7a89",
    "onnxruntime-cpu-v1", "float32", "none", "row-major", "none",
    "plaintext-v1", 1U << 20, 1U << 20, 8};
  const NativeCanonicalRolePreparer preparer(inspected, source, profile, control);
  NativeRequestControl requestControl{
    "/spec189-generated-model", 1, Clock::now() + std::chrono::seconds(30), {}};
  const auto roles = preparer.prepare(inspected, candidate, requestControl);
  BOOST_REQUIRE_EQUAL(roles.size(), 2U);
  BOOST_REQUIRE_EQUAL(roles[0].nodeIndices.size(), 3U);
  BOOST_REQUIRE_EQUAL(roles[1].nodeIndices.size(), 1U);
  auto foreignState = candidate;
  foreignState.roleStateOutputsByRole[firstRole] = {
    {"Y", "float32", {std::int64_t{1}, std::int64_t{2}}, std::uint64_t{8}}};
  foreignState.candidateDigest = foreignState.computedDigest();
  BOOST_CHECK_THROW(preparer.prepare(inspected, foreignState, requestControl),
                    std::invalid_argument);
  auto wrongStateType = candidate;
  wrongStateType.roleStateOutputsByRole[firstRole].front().dtype = "int64";
  wrongStateType.candidateDigest = wrongStateType.computedDigest();
  BOOST_CHECK_THROW(preparer.prepare(inspected, wrongStateType, requestControl),
                    std::invalid_argument);
  BOOST_CHECK(roles[0].expectedInputs.front().name == "X");
  const auto hasFirstOutput = [&] (const std::string& name) {
    return std::any_of(roles[0].expectedOutputs.begin(), roles[0].expectedOutputs.end(),
      [&] (const auto& item) { return item.name == name; });
  };
  BOOST_CHECK(hasFirstOutput("H0"));
  BOOST_CHECK(hasFirstOutput("H2"));
  BOOST_CHECK(roles[1].expectedInputs.front().name == "H2");
  BOOST_CHECK(roles[1].expectedOutputs.front().name == "Y");
  BOOST_CHECK(roles[0].nodeIndices != roles[1].nodeIndices);

  const auto first = assembleNativeCertifiedOnnxModel(source, roles[0], control);
  const auto second = assembleNativeCertifiedOnnxModel(source, roles[1], control);
  BOOST_CHECK_EQUAL(first.nodeCount, 3U);
  BOOST_CHECK_EQUAL(second.nodeCount, 1U);
  BOOST_CHECK_EQUAL(first.inputNames.front(), "X");
  BOOST_CHECK(std::find(first.outputNames.begin(), first.outputNames.end(), "H0") !=
              first.outputNames.end());
  BOOST_CHECK(std::find(first.outputNames.begin(), first.outputNames.end(), "H2") !=
              first.outputNames.end());
  BOOST_CHECK_EQUAL(second.inputNames.front(), "H2");
  BOOST_CHECK_EQUAL(second.outputNames.front(), "Y");
  BOOST_CHECK(!first.modelBytes.empty());
  BOOST_CHECK(!second.modelBytes.empty());
  BOOST_CHECK_NE(first.modelDigest, second.modelDigest);

  cleanup.clear();
  BOOST_CHECK(!std::filesystem::exists(root));
}

BOOST_AUTO_TEST_CASE(CompletedPublicationLookupReusesFrozenRoleInputsWithoutJson)
{
  Input input; TransportFixture io;
  auto publisher = NativeCanonicalPublisherTestAccess::create(io.transport(), input.options, input.resolver());
  const auto coldBefore = ndnsf::di::detail::fixedAssignmentWork();
  const auto cold = publisher(input.model, input.candidate, input.roles, input.control);
  BOOST_CHECK_GT(ndnsf::di::detail::fixedAssignmentWork().roleEncodes, coldBefore.roleEncodes);
  const auto publications = io.payloads.size();
  const auto warmBefore = ndnsf::di::detail::fixedAssignmentWork();
  const auto warm = publisher(input.model, input.candidate, input.roles, input.control);
  BOOST_CHECK_EQUAL(warm.manifestDigest, cold.manifestDigest);
  BOOST_CHECK_EQUAL(io.payloads.size(), publications);
  BOOST_CHECK_EQUAL(ndnsf::di::detail::fixedAssignmentWork().roleEncodes, warmBefore.roleEncodes);
  BOOST_CHECK_EQUAL(publisher.stats().cacheHits, 1U);

  auto changed = input.candidate;
  ++changed.selectionPriority;
  changed.candidateDigest = changed.computedDigest();
  const auto missBefore = ndnsf::di::detail::fixedAssignmentWork();
  (void)publisher(input.model, changed, input.roles, input.control);
  BOOST_CHECK_GT(io.payloads.size(), publications);
  BOOST_CHECK_GT(ndnsf::di::detail::fixedAssignmentWork().roleEncodes, missBefore.roleEncodes);

  auto invalidRoles = input.roles;
  invalidRoles[0].artifactDigest = nativePlanningDigest("different-role-artifact");
  const auto callsBeforeReject = publisher.stats().publicationCalls;
  BOOST_CHECK_THROW(publisher(input.model, changed, invalidRoles, input.control), std::exception);
  BOOST_CHECK_EQUAL(publisher.stats().publicationCalls, callsBeforeReject);

  auto cancelled = input.control;
  unsigned cancellationChecks = 0;
  cancelled.cancelled = [&] { return ++cancellationChecks > 1; };
  BOOST_CHECK_THROW(publisher(input.model, changed, input.roles, cancelled), std::exception);
  BOOST_CHECK_GE(cancellationChecks, 2U);

  struct RestoreRounding {
    int previous = std::fegetround();
    ~RestoreRounding() { std::fesetround(previous); }
  } restoreRounding;
  for (const auto mode : {FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    BOOST_REQUIRE_EQUAL(std::fesetround(mode), 0);
    const auto before = ndnsf::di::detail::fixedAssignmentWork();
    (void)publisher(input.model, changed, input.roles, input.control);
    BOOST_CHECK_GT(ndnsf::di::detail::fixedAssignmentWork().roleEncodes, before.roleEncodes);
  }
}

BOOST_AUTO_TEST_CASE(PublishesOwnedInlineAndExternalSourcesThroughPreparation)
{
  for (bool external : {false, true}) {
    for (bool explicitRanks : {false, true}) {
    Input input(external); TransportFixture io;
    if (!explicitRanks) {
      input.candidate.tensorDegreesByRole.clear();
      input.candidate.rankArtifactDigestsByRole.clear();
      input.candidate.candidateDigest = input.candidate.computedDigest();
    }
    auto publisher = NativeCanonicalPublisherTestAccess::create(io.transport(), input.options, input.resolver());
    NativeRequestPreparation preparation(std::make_shared<NativeAdapterRegistry>(), {}, publisher.artifactPort());
    const auto result = preparation.ensureArtifacts(input.model, input.candidate, input.proposal(), input.control);
    BOOST_REQUIRE_EQUAL(io.payloads.size(), external ? 3 : 2);
    BOOST_CHECK(io.payloads[0] == input.source->modelBytes);
    if (external) BOOST_CHECK(io.payloads[1] == input.source->initializerBytes->asVector());
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
    BOOST_CHECK(result.artifactNameByRole.at("/role").find("/rank/") == std::string::npos);
    }
  }
}

BOOST_AUTO_TEST_CASE(FilesystemMaterialCacheUsesCheckedExternalRanges)
{
  Input input(true);
  input.options.artifactProfileDigest = input.roles.front().artifactProfileDigest;
  const auto root = std::filesystem::temp_directory_path() /
    ("ndnsf-filesystem-cache-" + std::to_string(::getpid()));
  BOOST_REQUIRE(!std::filesystem::exists(root));
  std::filesystem::create_directories(root);
  BOOST_SCOPE_EXIT_ALL(&root) {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  };
  const auto initializerDigest = input.model.canonicalInitializerObjectDigest;
  const auto initializerBytes = std::vector<std::uint8_t>(input.source->initializerBytes->data(),
    input.source->initializerBytes->data() + input.source->initializerBytes->size());
  NativeMaterialFragmentCache sourceCache(root / "payloads");
  BOOST_REQUIRE(sourceCache.write(input.model.canonicalSourceDigest, input.source->modelBytes));
  BOOST_REQUIRE(sourceCache.write(initializerDigest, initializerBytes));
  const auto initializer = root / "payloads" / "sha256" / initializerDigest.substr(7, 2) /
    initializerDigest.substr(7);
  std::ofstream(root / "cache-identity.json") << nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-model-source-cache-v1"}, {"initializerDigest", initializerDigest}});
  NativeAssemblyControl assemblyControl{
    input.control.deadline, [&] { input.control.requireActive(); }, 1U << 20, 1U << 20};
  RepositorySourceRequest request;
  request.catalogConfigurationJson = nativeCanonicalJson(NativeJson{{"source", {
    {"digest", input.model.canonicalSourceDigest}, {"bytes", input.source->modelBytes.size()},
    {"initializer_digest", initializerDigest}, {"initializer_bytes", initializerBytes.size()}}}});
  request.maxSourceBytes = 1U << 20;
  request.deadline = input.control.deadline;
  const auto loaded = NativeFilesystemSourceProvider(root).load(request, {});
  BOOST_CHECK(loaded.modelBytes == input.source->modelBytes);
  BOOST_CHECK(!loaded.initializerBytes);
  BOOST_REQUIRE(loaded.initializerRangeSource);
  BOOST_CHECK_EQUAL(loaded.initializerRangeSource->size(), initializerBytes.size());
  input.source->initializerRangeSource = openNativeCanonicalInitializerFile(
    initializer.string(), initializerBytes.size(), initializerDigest, assemblyControl);
  input.source->initializerBytes.reset();
  const auto identity = canonicalOnnxSourceIdentity(*input.source, assemblyControl);
  input.model.canonicalGraphDigest = identity.graphDigest;
  input.model.canonicalInitializerDigest = identity.initializerDigest;
  input.source->materialManifest = deriveNativeCanonicalMaterialManifest(*input.source, assemblyControl);
  NativeFilesystemArtifactPublisher publisher(root, ndn::Name("/fixture/user"),
                                               nativePlanningDigest("filesystem-namespace"));
  const auto publication = publisher.publish("default", "/fixture/service", input.model,
    *input.source, input.options, input.control);
  BOOST_CHECK(!publication.artifactPrefetchRequired);
  const ndn::Name rootName(publication.rootDataName);
  BOOST_TEST_MESSAGE("Filesystem cache Name text: " + rootName.toUri());
  BOOST_CHECK(ndn::Name("/fixture/user").isPrefixOf(rootName));
  const auto prefix = rootName.getPrefix(-2);
  const auto rootBytes = readNativeFilesystemArtifact(root, prefix, rootName, 4096);
  BOOST_REQUIRE(rootBytes);
  BOOST_CHECK_EQUAL(std::string(rootBytes->begin(), rootBytes->end()), publication.canonicalManifestJson);
  TransportFixture bindingIo;
  auto bindingPublisher = NativeCanonicalPublisherTestAccess::create(
    bindingIo.transport(), input.options, input.resolver());
  const auto binding = bindingPublisher.bindPrepared(
    input.model, input.candidate, input.roles, publication, input.control);
  BOOST_CHECK_EQUAL(binding.artifactNameByRole.at(input.roles.front().selectedRole),
                    publication.rootDataName);
  BOOST_CHECK(bindingIo.payloads.empty());
  std::optional<ndn::Name> range;
  for (std::size_t i = 0; i < publication.materialDataNames.size(); ++i) {
    const ndn::Name name(publication.materialDataNames[i]);
    const auto payload = readNativeFilesystemArtifact(root, prefix, name, 1U << 20);
    BOOST_REQUIRE(payload);
    BOOST_CHECK_EQUAL(nativePlanningDigest(payload->data(), payload->size()),
                      publication.materialDigests[i]);
    if (name.size() == prefix.size() + 4 && name[-4] == ndn::name::Component("RANGE"))
      range = name;
  }
  BOOST_REQUIRE(range);
  BOOST_CHECK(!readNativeFilesystemArtifact(root, ndn::Name("/foreign"), *range, 1U << 20));
  BOOST_CHECK(!readNativeFilesystemArtifact(root, prefix, *range, 1));
  {
    std::fstream damage(initializer, std::ios::binary | std::ios::in | std::ios::out);
    damage.put(static_cast<char>(initializerBytes.front() ^ 0xff));
  }
  BOOST_CHECK(!readNativeFilesystemArtifact(root, prefix, *range, 1U << 20));
  std::filesystem::remove(initializer);
  std::filesystem::create_symlink("/dev/zero", initializer);
  BOOST_CHECK(!readNativeFilesystemArtifact(root, prefix, *range, 1U << 20));
}

BOOST_AUTO_TEST_CASE(MaterialBackedPublicationOmitsWholeModelAndPreflightsUnionBudget)
{
  Input input;
  // Catalog preparation supplies the recipe's profile to the publication owner.
  input.options.artifactProfileDigest = input.roles.front().artifactProfileDigest;
  NativeAssemblyControl materialControl{
    input.control.deadline, [&] { input.control.requireActive(); }, 1U << 20, 1U << 20};
  const auto identity = canonicalOnnxSourceIdentity(*input.source, materialControl);
  input.model.canonicalGraphDigest = identity.graphDigest;
  input.source->materialManifest = deriveNativeCanonicalMaterialManifest(
    *input.source, materialControl);
  TransportFixture oversizedNameIo;
  auto oversizedNameTransport = oversizedNameIo.transport();
  oversizedNameTransport.maxPublishedDataNameBytes =
    NativeCanonicalMaterialReceiptDataNameMaxBytes + 1;
  BOOST_CHECK_THROW(NativeCanonicalPublisherTestAccess::create(
    std::move(oversizedNameTransport), input.options, input.resolver()), std::invalid_argument);
  BOOST_CHECK_EQUAL(oversizedNameIo.beginCalls, 0U);
  BOOST_CHECK(oversizedNameIo.payloads.empty());
  std::uint64_t materialBytes = input.source->materialManifest->canonicalJson().size();
  for (const auto& payload : input.source->materialManifest->payloads)
    materialBytes += payload.byteSize();
  const auto receiptBudget = NativeCanonicalMaterialReceiptEnvelopeMaxBytes +
    static_cast<std::uint64_t>(input.source->materialManifest->payloads.size()) *
      NativeCanonicalMaterialReceiptRecordMaxBytes;
  constexpr std::uint64_t rootBudget = 16U * 1024U * 1024U;
  BOOST_REQUIRE_LE(materialBytes, std::numeric_limits<std::uint64_t>::max() -
                                  receiptBudget - rootBudget);
  const auto publicationBudget = materialBytes + receiptBudget + rootBudget;
  auto materialOnly = std::make_shared<NativeCanonicalSource>();
  materialOnly->materialManifest = input.source->materialManifest;
  const auto materialResolver = [materialOnly](const auto&, const auto&) {
    return std::shared_ptr<const NativeCanonicalSource>(materialOnly);
  };

  input.options.maxPublicationBytes = publicationBudget - 1;
  TransportFixture rejectedIo;
  auto rejected = NativeCanonicalPublisherTestAccess::create(
    rejectedIo.transport(), input.options, materialResolver);
  BOOST_CHECK_THROW(rejected.prepare(input.model, input.control), std::runtime_error);
  BOOST_CHECK(rejectedIo.payloads.empty());

  input.options.maxPublicationBytes = publicationBudget;
  TransportFixture io;
  auto publisher = NativeCanonicalPublisherTestAccess::create(
    io.transport(), input.options, materialResolver);
  const auto receipt = publisher.prepare(input.model, input.control);
  BOOST_CHECK(receipt.sourceDataName.empty());
  BOOST_CHECK(receipt.initializerDataName.empty());
  BOOST_REQUIRE(!receipt.materialManifestDataName.empty());
  BOOST_REQUIRE(!receipt.materialReceiptDataName.empty());
  BOOST_CHECK_LE(receipt.materialReceiptBytes, receiptBudget);
  BOOST_REQUIRE(!receipt.rootDataName.empty());
  BOOST_CHECK(std::none_of(io.labels.begin(), io.labels.end(), [] (const auto& label) {
    return label == "di-canonical-source" || label == "di-canonical-initializer";
  }));
  const auto root = nativeParseJson(receipt.canonicalManifestJson);
  BOOST_CHECK_EQUAL(root.at("metadata").at("materialBacked").get<bool>(), true);
  BOOST_CHECK(!root.at("metadata").contains("canonicalSourceDataName"));
  BOOST_CHECK(!root.at("metadata").contains("canonicalInitializerDataName"));
  BOOST_CHECK_EQUAL(root.at("metadata").at("canonicalSourceDigest").get<std::string>(),
                    input.model.canonicalSourceDigest);
  BOOST_CHECK_EQUAL(root.at("metadata").at("canonicalSourceBytes").get<std::uint64_t>(),
                    input.model.canonicalSourceBytes);
  BOOST_REQUIRE(!io.labels.empty());
  BOOST_CHECK_EQUAL(io.labels.back(), "di-canonical-root");
  auto referenceOnly = receipt;
  referenceOnly.materialManifest.reset();
  const auto publications = io.payloads.size();
  const auto binding = publisher.bindPrepared(input.model, input.candidate, input.roles,
                                             referenceOnly, input.control);
  BOOST_CHECK(!binding.modelObjectKeyIndex);
  BOOST_CHECK_EQUAL(io.payloads.size(), publications);
}

BOOST_AUTO_TEST_CASE(MaterialPublicationAggregatesSmallReadWindows)
{
  Input input;
  onnx::ModelProto model;
  BOOST_REQUIRE(model.ParseFromArray(input.source->modelBytes.data(),
                                    static_cast<int>(input.source->modelBytes.size())));
  auto* weight = model.mutable_graph()->add_initializer();
  weight->set_name("publication-bulk-weight");
  weight->set_data_type(onnx::TensorProto::UINT8);
  const auto rawBytes = 3 * NativeCanonicalMaterialBundleMaxBytes + 16;
  weight->add_dims(static_cast<std::int64_t>(rawBytes));
  weight->set_raw_data(std::string(rawBytes, '\x5a'));
  std::string wire;
  BOOST_REQUIRE(model.SerializeToString(&wire));
  input.source->modelBytes.assign(wire.begin(), wire.end());
  NativeAssemblyControl control{input.control.deadline, [] {}, 8U << 20, 8U << 20};
  const auto identity = canonicalOnnxSourceIdentity(*input.source, control);
  input.model.canonicalSourceBytes = wire.size();
  input.model.canonicalSourceDigest = nativePlanningDigest(wire);
  input.model.canonicalGraphDigest = identity.graphDigest;
  input.source->materialManifest = deriveNativeCanonicalMaterialManifest(*input.source, control);
  for (const auto& payload : input.source->materialManifest->payloads)
    BOOST_CHECK_LE(payload.byteSize(), NativeCanonicalMaterialBundleMaxBytes);

  TransportFixture io;
  auto publisher = NativeCanonicalPublisherTestAccess::create(
    io.transport(), input.options, input.resolver());
  const auto receipt = publisher.prepare(input.model, input.control);
  receipt.validate();
  std::size_t bundles = 0;
  for (std::size_t i = 0; i < io.labels.size(); ++i) {
    if (io.labels[i].find("di-material-bundle-") != 0)
      continue;
    ++bundles;
    BOOST_CHECK_GT(io.payloads[i].size(), NativeCanonicalMaterialBundleMaxBytes);
    BOOST_CHECK_LE(io.payloads[i].size(), NativeCanonicalMaterialPublicationMaxBytes);
  }
  BOOST_CHECK_EQUAL(bundles, 1U);
  BOOST_CHECK(std::all_of(receipt.materialDataNames.begin(), receipt.materialDataNames.end(),
    [&] (const auto& name) { return name == receipt.materialDataNames.front(); }));
}

BOOST_AUTO_TEST_CASE(ExternalInitializerUsesBoundedChunksAndReassemblesAfterSelection)
{
  NativeCanonicalSource source;
  onnx::ModelProto model;
  model.set_ir_version(8);
  model.mutable_graph()->set_name("spec189-chunked-initializer");
  auto* input = model.mutable_graph()->add_input();
  input->set_name("X");
  input->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::FLOAT16);
  input->mutable_type()->mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
  auto* output = model.mutable_graph()->add_output();
  output->set_name("Y");
  output->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::FLOAT16);
  output->mutable_type()->mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
  auto* node = model.mutable_graph()->add_node();
  node->set_op_type("MatMul");
  node->add_input("X");
  node->add_input("weight");
  node->add_output("Y");
  constexpr std::size_t rawBytes = 3U * 1024U * 1024U + 16U;
  auto* initializer = model.mutable_graph()->add_initializer();
  initializer->set_name("weight");
  initializer->set_data_type(onnx::TensorProto::FLOAT16);
  initializer->add_dims(static_cast<std::int64_t>(rawBytes / 2));
  initializer->set_data_location(onnx::TensorProto::EXTERNAL);
  auto* location = initializer->add_external_data();
  location->set_key("location");
  location->set_value("weights.bin");
  auto* offset = initializer->add_external_data();
  offset->set_key("offset");
  offset->set_value("0");
  auto* length = initializer->add_external_data();
  length->set_key("length");
  length->set_value(std::to_string(rawBytes));
  source.initializerBytes.emplace(rawBytes);
  for (std::size_t i = 0; i < rawBytes; ++i)
    (*source.initializerBytes)[i] = static_cast<std::uint8_t>(i * 17U + 3U);
  std::string wire;
  BOOST_REQUIRE(model.SerializeToString(&wire));
  source.modelBytes.assign(wire.begin(), wire.end());

  const NativeAssemblyControl control{
    Clock::now() + std::chrono::seconds(30), [] {}, 8U * 1024U * 1024U,
    16U * 1024U * 1024U};
  const auto manifest = deriveNativeCanonicalMaterialManifest(source, control);
  BOOST_REQUIRE(manifest);
  validateNativeCanonicalMaterialManifest(source, *manifest, control);
  const auto reference = std::find_if(manifest->references.begin(), manifest->references.end(),
    [] (const auto& item) { return item.kind == "shared-initializer"; });
  BOOST_REQUIRE(reference != manifest->references.end());
  BOOST_REQUIRE_EQUAL(reference->chunkPayloadIds.size(), 4U);
  BOOST_CHECK(reference->bytes < NativeCanonicalMaterialBundleMaxBytes);
  for (const auto& chunkId : reference->chunkPayloadIds) {
    const auto payload = std::find_if(manifest->payloads.begin(), manifest->payloads.end(),
      [&chunkId] (const auto& item) { return item.payloadId == chunkId; });
    BOOST_REQUIRE(payload != manifest->payloads.end());
    BOOST_CHECK_LE(payload->byteSize(), NativeCanonicalMaterialBundleMaxBytes);
    BOOST_CHECK(payload->bytes.empty());
    BOOST_CHECK(payload->backing == source.initializerBytes->shared());
  }

  source.materialManifest = manifest;
  source.materialPayloads = manifest->payloads;
  const auto assembled = materializeNativeCanonicalModel(source, {0}, control);
  onnx::ModelProto roundTrip;
  BOOST_REQUIRE(roundTrip.ParseFromArray(assembled.data(), static_cast<int>(assembled.size())));
  BOOST_REQUIRE_EQUAL(roundTrip.graph().initializer_size(), 1);
  BOOST_CHECK_EQUAL(roundTrip.graph().initializer(0).raw_data().size(), rawBytes);
  BOOST_REQUIRE_EQUAL(roundTrip.graph().initializer(0).raw_data().size(),
                      source.initializerBytes->size());
  BOOST_CHECK(std::equal(roundTrip.graph().initializer(0).raw_data().begin(),
                         roundTrip.graph().initializer(0).raw_data().end(),
                         source.initializerBytes->begin(),
                         [] (const char actual, const std::uint8_t expected) {
                           return static_cast<std::uint8_t>(static_cast<unsigned char>(actual)) == expected;
                         }));
  auto range = std::make_shared<InitializerRangeFixture>();
  range->bytes = source.initializerBytes->asVector();
  NativeCanonicalSource rangeSource;
  rangeSource.modelBytes = source.modelBytes;
  rangeSource.initializerRangeSource = range;
  NativeCanonicalSource::MaterialPayload legacyPayload;
  legacyPayload.rangeSource = range;
  legacyPayload.rangeSize = range->bytes.size();
  BOOST_CHECK(legacyPayload.copyBytes() == range->bytes);
  BOOST_CHECK_EQUAL(range->largestRead, NativeCanonicalMaterialBundleMaxBytes);
  range->reads = 0;
  BOOST_CHECK_THROW(legacyPayload.copyBytes([&] {
    if (range->reads != 0) throw std::runtime_error("fixture cancelled after one window");
  }), std::runtime_error);
  BOOST_CHECK_EQUAL(range->reads, 1U);
  const auto rangeManifest = deriveNativeCanonicalMaterialManifest(rangeSource, control);
  BOOST_CHECK_EQUAL(rangeManifest->canonicalJson(), manifest->canonicalJson());
  validateNativeCanonicalMaterialManifest(rangeSource, *rangeManifest, control);
  rangeSource.materialManifest = rangeManifest;
  rangeSource.materialPayloads = rangeManifest->payloads;
  BOOST_CHECK(materializeNativeCanonicalModel(rangeSource, {0}, control) == assembled);
  BOOST_CHECK_EQUAL(range->largestRead, NativeCanonicalMaterialBundleMaxBytes);
  rangeSource.materialPayloads = rangeManifest->payloads;
  range->reads = 0;
  BOOST_CHECK_THROW(rangeManifest->validate([&] {
    if (range->reads != 0) throw std::runtime_error("fixture cancelled during producer validation");
  }), std::runtime_error);
  BOOST_CHECK_EQUAL(range->reads, 1U);
  auto consumerManifest = std::make_shared<NativeCanonicalSource::MaterialManifest>(*rangeManifest);
  consumerManifest->payloads.clear();
  consumerManifest->payloadsComplete = false;
  rangeSource.materialManifest = std::move(consumerManifest);
  range->reads = 0;
  auto cancelled = control;
  cancelled.requireActive = [&] {
    if (range->reads != 0) throw std::runtime_error("fixture cancelled during material admission");
  };
  BOOST_CHECK_THROW(materializeNativeCanonicalModel(rangeSource, {0}, cancelled),
                    std::runtime_error);
  BOOST_CHECK_EQUAL(range->reads, 1U);
}

BOOST_AUTO_TEST_CASE(RangeOnlyInitializerPreservesMaterialAndAssemblyIdentity)
{
  Input input(true);
  const NativeAssemblyControl control{
    Clock::now() + std::chrono::seconds(30), [] {}, 1U << 20, 1U << 20};
  const auto expected = deriveNativeCanonicalMaterialManifest(*input.source, control);
  const auto expectedIdentity = canonicalOnnxSourceIdentity(*input.source, control);
  auto range = std::make_shared<InitializerRangeFixture>();
  range->bytes = input.source->initializerBytes->asVector();
  NativeCanonicalSource source = *input.source;
  source.initializerBytes.reset();
  source.initializerRangeSource = range;
  const auto actual = deriveNativeCanonicalMaterialManifest(source, control);
  BOOST_CHECK_EQUAL(actual->canonicalJson(), expected->canonicalJson());
  BOOST_CHECK_EQUAL(canonicalOnnxSourceIdentity(source, control).graphDigest,
                    expectedIdentity.graphDigest);
  validateNativeCanonicalMaterialManifest(source, *actual, control);
  for (std::size_t i = 0; i < actual->payloads.size(); ++i)
    BOOST_CHECK(actual->payloads[i].copyBytes() == expected->payloads[i].copyBytes());
  source.materialManifest = actual;
  source.materialPayloads = actual->payloads;
  auto vectorSource = *input.source;
  vectorSource.materialManifest = expected;
  vectorSource.materialPayloads = expected->payloads;
  BOOST_CHECK(materializeNativeCanonicalModel(source, {0, 1}, control) ==
              materializeNativeCanonicalModel(vectorSource, {0, 1}, control));
  BOOST_CHECK_GT(range->reads, 0U);
  BOOST_CHECK_LE(range->largestRead, NativeCanonicalMaterialBundleMaxBytes);

  input.model.canonicalGraphDigest = expectedIdentity.graphDigest;
  *input.source = source;
  TransportFixture io;
  auto publisher = NativeCanonicalPublisherTestAccess::create(
    io.transport(), input.options, input.resolver());
  const auto receipt = publisher.prepare(input.model, input.control);
  BOOST_CHECK(!receipt.materialManifestDataName.empty());
  BOOST_CHECK(receipt.initializerDataName.empty());
  BOOST_CHECK(!receipt.rootDataName.empty());
  BOOST_CHECK(std::find(io.labels.begin(), io.labels.end(), "di-canonical-initializer") ==
              io.labels.end());

  source.materialManifest.reset();
  source.materialPayloads.clear();
  source.initializerBytes = range->bytes;
  range->bytes.front() ^= 1;
  BOOST_CHECK_THROW(nativeCanonicalInitializerDigest(source, control), std::exception);
  source.initializerBytes.reset();
  range->shortRead = true;
  BOOST_CHECK_THROW(deriveNativeCanonicalMaterialManifest(source, control), std::exception);
  range->shortRead = false;
  auto cancelled = control;
  cancelled.requireActive = [] { throw std::runtime_error("fixture cancelled"); };
  BOOST_CHECK_THROW(deriveNativeCanonicalMaterialManifest(source, cancelled), std::exception);
  BOOST_CHECK_THROW(readNativeCanonicalInitializerRange(source, range->size(), 1, control),
                    std::exception);
}

BOOST_AUTO_TEST_CASE(RangeOnlyInitializerSupportsPhysicalOffsetsBeyondProtobufLimit)
{
  Input input(true);
  auto range = std::make_shared<InitializerRangeFixture>();
  range->bytes = input.source->initializerBytes->asVector();
  range->base = static_cast<std::uint64_t>(std::numeric_limits<int>::max()) + 37;
  onnx::ModelProto model;
  BOOST_REQUIRE(model.ParseFromArray(input.source->modelBytes.data(),
                                     static_cast<int>(input.source->modelBytes.size())));
  unsigned externalCount = 0;
  for (auto& tensor : *model.mutable_graph()->mutable_initializer()) {
    if (tensor.data_location() != onnx::TensorProto::EXTERNAL) continue;
    ++externalCount;
    bool found = false;
    for (auto& entry : *tensor.mutable_external_data()) {
      if (entry.key() == "offset") {
        entry.set_value(std::to_string(range->base + std::stoull(entry.value())));
        found = true;
      }
    }
    if (!found) {
      auto* entry = tensor.add_external_data();
      entry->set_key("offset");
      entry->set_value(std::to_string(range->base));
    }
  }
  BOOST_REQUIRE_GT(externalCount, 0U);
  std::string wire;
  BOOST_REQUIRE(model.SerializeToString(&wire));
  NativeCanonicalSource source;
  source.modelBytes.assign(wire.begin(), wire.end());
  source.initializerRangeSource = range;
  const NativeAssemblyControl control{
    Clock::now() + std::chrono::seconds(30), [] {}, range->size(), 1U << 20};
  const auto manifest = deriveNativeCanonicalMaterialManifest(source, control);
  validateNativeCanonicalMaterialManifest(source, *manifest, control);
  BOOST_CHECK_GT(range->reads, 0U);
  BOOST_CHECK_LE(range->largestRead, NativeCanonicalMaterialBundleMaxBytes);
  BOOST_CHECK_EQUAL(nativeCanonicalInitializerByteSize(source), range->size());
}

BOOST_AUTO_TEST_CASE(NBitsExternalWeightsDoNotSupplyShapeValues)
{
  const Input fixture;
  const auto* globalSchema = onnx::OpSchemaRegistry::Instance()->GetSchema(
    "MatMulNBits", 1, "com.microsoft");
  const NativeAssemblyControl control{
    Clock::now() + std::chrono::seconds(30), [] {}, 1U << 20, 1U << 20};
  for (const int bits : {4, 8}) {
    onnx::ModelProto model;
    model.set_ir_version(8);
    model.add_opset_import()->set_version(13);
    auto* contrib = model.add_opset_import();
    contrib->set_domain("com.microsoft");
    contrib->set_version(1);
    auto* graph = model.mutable_graph();
    graph->set_name("spec220-nbits-shape");
    const auto addValue = [&] (bool input, const char* name, std::int64_t last) {
      auto* value = input ? graph->add_input() : graph->add_output();
      value->set_name(name);
      auto* type = value->mutable_type()->mutable_tensor_type();
      type->set_elem_type(onnx::TensorProto::FLOAT);
      type->mutable_shape()->add_dim()->set_dim_value(1);
      type->mutable_shape()->add_dim()->set_dim_value(last);
    };
    addValue(true, "A", 33);
    addValue(false, "Z", 1);
    auto* node = graph->add_node();
    node->set_domain("com.microsoft");
    node->set_op_type("MatMulNBits");
    node->add_input("A");
    node->add_input("B");
    node->add_input("scales");
    node->add_output("Y");
    for (const auto& item : std::map<std::string, int>{
           {"K", 33}, {"N", 2}, {"bits", bits}, {"block_size", 32}}) {
      auto* attribute = node->add_attribute();
      attribute->set_name(item.first);
      attribute->set_type(onnx::AttributeProto::INT);
      attribute->set_i(item.second);
    }
    auto* packed = graph->add_initializer();
    packed->set_name("B");
    packed->set_data_type(onnx::TensorProto::UINT8);
    for (const auto dim : {2, 2, 32 * bits / 8}) packed->add_dims(dim);
    packed->set_raw_data(std::string(4 * 32 * bits / 8, '\x88'));
    auto* scales = graph->add_initializer();
    scales->set_name("scales");
    scales->set_data_type(onnx::TensorProto::FLOAT);
    scales->add_dims(2);
    scales->add_dims(2);
    scales->set_raw_data(std::string(16, '\0'));
    // The split boundary Y has no exporter value_info. Its rank must come
    // from the NBits contract before a following reduction can be extracted.
    auto* reduce = graph->add_node();
    reduce->set_op_type("ReduceMean");
    reduce->add_input("Y");
    reduce->add_output("Z");
    auto* axes = reduce->add_attribute();
    axes->set_name("axes");
    axes->set_type(onnx::AttributeProto::INTS);
    axes->add_ints(-1);
    const auto makeSource = [&] (const onnx::ModelProto& value) {
      std::string wire;
      if (!value.SerializeToString(&wire)) throw std::runtime_error("fixture serialization");
      NativeCanonicalSource source;
      source.modelBytes.assign(wire.begin(), wire.end());
      return source;
    };
    const auto inlined = inspectNativeOnnxSourceGraph(
      makeSource(model), fixture.model.descriptor, control);
    const auto metadata = NativeJson::parse(inlined.graphMetadataJson);
    BOOST_REQUIRE(metadata.at("tensors").contains("Y"));
    BOOST_CHECK(metadata.at("tensors").at("Y").at("shape") == NativeJson::array({1, 2}));
    BOOST_CHECK_EQUAL(metadata.at("tensors").at("Y").at("dtype"), "float32");
    auto batched = model;
    auto* inputShape = batched.mutable_graph()->mutable_input(0)->mutable_type()
      ->mutable_tensor_type()->mutable_shape();
    inputShape->clear_dim();
    inputShape->add_dim()->set_dim_param("batch");
    inputShape->add_dim()->set_dim_param("sequence");
    inputShape->add_dim()->set_dim_value(33);
    auto* outputShape = batched.mutable_graph()->mutable_output(0)->mutable_type()
      ->mutable_tensor_type()->mutable_shape();
    outputShape->clear_dim();
    outputShape->add_dim()->set_dim_param("batch");
    outputShape->add_dim()->set_dim_param("sequence");
    outputShape->add_dim()->set_dim_value(1);
    const auto symbolic = inspectNativeOnnxSourceGraph(
      makeSource(batched), fixture.model.descriptor, control);
    BOOST_CHECK(NativeJson::parse(symbolic.graphMetadataJson).at("tensors").at("Y").at("shape") ==
                NativeJson::array({"batch", "sequence", 2}));
    inputShape->mutable_dim(2)->set_dim_value(34);
    const auto invalidWidth = inspectNativeOnnxSourceGraph(
      makeSource(batched), fixture.model.descriptor, control);
    BOOST_CHECK(!NativeJson::parse(invalidWidth.graphMetadataJson).at("tensors").contains("Y"));
    batched.mutable_graph()->mutable_input(0)->mutable_type()->mutable_tensor_type()->clear_shape();
    const auto unknownRank = inspectNativeOnnxSourceGraph(
      makeSource(batched), fixture.model.descriptor, control);
    BOOST_CHECK(!NativeJson::parse(unknownRank.graphMetadataJson).at("tensors").contains("Y"));

    auto local = model;
    auto* function = local.add_functions();
    function->set_name("MatMulNBits");
    function->set_domain("com.microsoft");
    function->add_opset_import()->set_version(13);
    for (const char* name : {"A", "B", "scales"}) function->add_input(name);
    function->add_output("Y");
    for (const char* name : {"K", "N", "bits", "block_size"}) function->add_attribute(name);
    auto* identity = function->add_node();
    identity->set_op_type("Identity");
    identity->add_input("A");
    identity->add_output("Y");
    const auto localFunction = inspectNativeOnnxSourceGraph(
      makeSource(local), fixture.model.descriptor, control);
    BOOST_CHECK(NativeJson::parse(localFunction.graphMetadataJson).at("tensors").at("Y").at("shape") ==
                NativeJson::array({1, 33}));
    BOOST_CHECK(onnx::OpSchemaRegistry::Instance()->GetSchema("MatMulNBits", 1, "com.microsoft") ==
                globalSchema);
    std::vector<std::uint8_t> backing;
    for (auto& tensor : *graph->mutable_initializer()) {
      const auto raw = tensor.raw_data();
      const auto offset = backing.size();
      backing.insert(backing.end(), raw.begin(), raw.end());
      tensor.clear_raw_data();
      tensor.set_data_location(onnx::TensorProto::EXTERNAL);
      for (const auto& field : std::map<std::string, std::string>{
             {"location", "weights.bin"}, {"offset", std::to_string(offset)},
             {"length", std::to_string(raw.size())}}) {
        auto* entry = tensor.add_external_data();
        entry->set_key(field.first);
        entry->set_value(field.second);
      }
    }
    auto source = makeSource(model);
    source.initializerBytes = backing;
    const auto external = inspectNativeOnnxSourceGraph(source, fixture.model.descriptor, control);
    BOOST_CHECK_EQUAL(external.graphMetadataJson, inlined.graphMetadataJson);
    BOOST_CHECK_EQUAL(external.canonicalIdentity.graphDigest, inlined.canonicalIdentity.graphDigest);
    BOOST_CHECK_EQUAL(external.canonicalIdentity.initializerDigest,
                      inlined.canonicalIdentity.initializerDigest);
    model.mutable_opset_import(1)->set_version(2);
    source.modelBytes = makeSource(model).modelBytes;
    BOOST_CHECK_EXCEPTION(inspectNativeOnnxSourceGraph(source, fixture.model.descriptor, control),
      std::runtime_error, [] (const auto& error) {
        return std::string(error.what()).find("SHAPE_INPUT_UNSUPPORTED") != std::string::npos;
      });
    model.mutable_opset_import(1)->set_version(1);
    model.mutable_opset_import(1)->set_domain("fixture.unknown");
    node->set_domain("fixture.unknown");
    source.modelBytes = makeSource(model).modelBytes;
    BOOST_CHECK_EXCEPTION(inspectNativeOnnxSourceGraph(source, fixture.model.descriptor, control),
      std::runtime_error, [] (const auto& error) {
        return std::string(error.what()).find("SHAPE_INPUT_UNSUPPORTED") != std::string::npos;
      });
  }
}

BOOST_AUTO_TEST_CASE(CausalConvExternalWeightsDoNotSupplyShapeValues)
{
  const Input fixture;
  const NativeAssemblyControl control{
    Clock::now() + std::chrono::seconds(30), [] {}, 1U << 20, 1U << 20};
  onnx::ModelProto model;
  model.set_ir_version(10);
  model.add_opset_import()->set_version(21);
  auto* contrib = model.add_opset_import();
  contrib->set_domain("com.microsoft");
  contrib->set_version(1);
  auto* graph = model.mutable_graph();
  graph->set_name("spec220-causal-conv-shape");
  const auto addValue = [&] (bool input, const char* name,
                             std::initializer_list<std::int64_t> dims) {
    auto* value = input ? graph->add_input() : graph->add_output();
    value->set_name(name);
    auto* type = value->mutable_type()->mutable_tensor_type();
    type->set_elem_type(onnx::TensorProto::FLOAT16);
    for (const auto dim : dims) type->mutable_shape()->add_dim()->set_dim_value(dim);
  };
  addValue(true, "input", {1, 10240, 2});
  addValue(true, "past_state", {1, 10240, 3});
  addValue(false, "output", {1, 10240, 2});
  addValue(false, "present_state", {1, 10240, 3});
  auto* node = graph->add_node();
  node->set_domain("com.microsoft");
  node->set_op_type("CausalConvWithState");
  for (const char* name : {"input", "weight", "bias", "past_state"}) node->add_input(name);
  node->add_output("output");
  node->add_output("present_state");
  auto* ndim = node->add_attribute();
  ndim->set_name("ndim");
  ndim->set_type(onnx::AttributeProto::INT);
  ndim->set_i(1);
  auto* activation = node->add_attribute();
  activation->set_name("activation");
  activation->set_type(onnx::AttributeProto::STRING);
  activation->set_s("silu");
  for (const auto& item : std::map<std::string, std::vector<std::int64_t>>{
         {"weight", {10240, 1, 4}}, {"bias", {10240}}}) {
    auto* tensor = graph->add_initializer();
    tensor->set_name(item.first);
    tensor->set_data_type(onnx::TensorProto::FLOAT16);
    std::size_t bytes = 2;
    for (const auto dim : item.second) {
      tensor->add_dims(dim);
      bytes *= static_cast<std::size_t>(dim);
    }
    std::string raw(bytes, '\0');
    for (std::size_t i = 1; i < bytes; i += 2) raw[i] = '\x3c';
    tensor->set_raw_data(std::move(raw));
  }
  const auto makeSource = [&] {
    std::string wire;
    if (!model.SerializeToString(&wire)) throw std::runtime_error("fixture serialization");
    NativeCanonicalSource source;
    source.modelBytes.assign(wire.begin(), wire.end());
    return source;
  };
  auto descriptor = fixture.model.descriptor;
  descriptor.precision = "fp16";
  descriptor = fixture::completeModel(std::move(descriptor));
  const auto inlined = inspectNativeOnnxSourceGraph(makeSource(), descriptor, control);
  std::vector<std::uint8_t> backing;
  for (auto& tensor : *graph->mutable_initializer()) {
    const auto raw = tensor.raw_data();
    const auto offset = backing.size();
    backing.insert(backing.end(), raw.begin(), raw.end());
    tensor.clear_raw_data();
    tensor.set_data_location(onnx::TensorProto::EXTERNAL);
    for (const auto& field : std::map<std::string, std::string>{
           {"location", "weights.bin"}, {"offset", std::to_string(offset)},
           {"length", std::to_string(raw.size())}}) {
      auto* entry = tensor.add_external_data();
      entry->set_key(field.first);
      entry->set_value(field.second);
    }
  }
  auto source = makeSource();
  source.initializerBytes = backing;
  const auto external = inspectNativeOnnxSourceGraph(source, descriptor, control);
  BOOST_CHECK_EQUAL(external.graphMetadataJson, inlined.graphMetadataJson);
  BOOST_CHECK_EQUAL(external.canonicalIdentity.graphDigest, inlined.canonicalIdentity.graphDigest);
  BOOST_CHECK_EQUAL(external.canonicalIdentity.initializerDigest,
                    inlined.canonicalIdentity.initializerDigest);
  for (const bool foreignDomain : {false, true}) {
    model.mutable_opset_import(1)->set_version(foreignDomain ? 1 : 2);
    if (foreignDomain) {
      model.mutable_opset_import(1)->set_domain("fixture.unknown");
      node->set_domain("fixture.unknown");
    }
    source.modelBytes = makeSource().modelBytes;
    BOOST_CHECK_EXCEPTION(inspectNativeOnnxSourceGraph(source, descriptor, control),
      std::runtime_error, [] (const auto& error) {
        return std::string(error.what()).find("SHAPE_INPUT_UNSUPPORTED") != std::string::npos;
      });
  }
}

BOOST_AUTO_TEST_CASE(HybridAttentionExternalParametersDoNotSupplyShapeValues)
{
  const Input fixture;
  const NativeAssemblyControl control{
    Clock::now() + std::chrono::seconds(30), [] {}, 1U << 20, 1U << 20};
  for (const std::string op : {"LinearAttentionGate", "GatedRMSNorm", "MRotaryEmbedding"}) {
    BOOST_TEST_CONTEXT(op) {
      onnx::ModelProto model;
      model.set_ir_version(10);
      model.add_opset_import()->set_version(22);
      auto* contrib = model.add_opset_import();
      contrib->set_domain("com.microsoft");
      contrib->set_version(1);
      auto* graph = model.mutable_graph();
      graph->set_name("spec220-hybrid-attention-shape");
      auto* node = graph->add_node();
      node->set_domain("com.microsoft");
      node->set_op_type(op);
      const auto addValue = [&] (bool input, const char* name, int dtype,
                                 std::initializer_list<std::int64_t> dims) {
        auto* value = input ? graph->add_input() : graph->add_output();
        value->set_name(name);
        auto* type = value->mutable_type()->mutable_tensor_type();
        type->set_elem_type(dtype);
        for (const auto dim : dims) type->mutable_shape()->add_dim()->set_dim_value(dim);
      };
      const auto parameter = [&] (const char* name, int dtype,
                                  std::initializer_list<std::int64_t> dims) {
        node->add_input(name);
        auto* tensor = graph->add_initializer();
        tensor->set_name(name);
        tensor->set_data_type(dtype);
        std::size_t bytes = dtype == onnx::TensorProto::FLOAT ? 4 : 2;
        for (const auto dim : dims) {
          tensor->add_dims(dim);
          bytes *= static_cast<std::size_t>(dim);
        }
        tensor->set_raw_data(std::string(bytes, '\0'));
      };
      addValue(true, "X", onnx::TensorProto::FLOAT16, {1, 2, 8});
      addValue(false, "Y", onnx::TensorProto::FLOAT16, {1, 2, 8});
      node->add_input("X");
      if (op == "LinearAttentionGate") {
        parameter("dt_bias", onnx::TensorProto::FLOAT, {8});
        parameter("decay_scale", onnx::TensorProto::FLOAT, {8});
      }
      else if (op == "GatedRMSNorm") {
        parameter("scale", onnx::TensorProto::FLOAT16, {4});
        addValue(true, "gate", onnx::TensorProto::FLOAT16, {1, 2, 8});
        node->add_input("gate");
      }
      else {
        addValue(true, "positions", onnx::TensorProto::INT64, {3, 1, 2});
        node->add_input("positions");
        parameter("cos_cache", onnx::TensorProto::FLOAT16, {8, 2});
        parameter("sin_cache", onnx::TensorProto::FLOAT16, {8, 2});
        auto* heads = node->add_attribute();
        heads->set_name("num_heads");
        heads->set_type(onnx::AttributeProto::INT);
        heads->set_i(2);
        auto* section = node->add_attribute();
        section->set_name("mrope_section");
        section->set_type(onnx::AttributeProto::INTS);
        for (const auto value : {1, 1, 0}) section->add_ints(value);
      }
      node->add_output("Y");
      const auto makeSource = [&] {
        std::string wire;
        if (!model.SerializeToString(&wire)) throw std::runtime_error("fixture serialization");
        NativeCanonicalSource source;
        source.modelBytes.assign(wire.begin(), wire.end());
        return source;
      };
      auto descriptor = fixture.model.descriptor;
      descriptor.precision = "fp16";
      descriptor = fixture::completeModel(std::move(descriptor));
      const auto inlined = inspectNativeOnnxSourceGraph(makeSource(), descriptor, control);
      std::vector<std::uint8_t> backing;
      for (auto& tensor : *graph->mutable_initializer()) {
        const auto raw = tensor.raw_data();
        const auto offset = backing.size();
        backing.insert(backing.end(), raw.begin(), raw.end());
        tensor.clear_raw_data();
        tensor.set_data_location(onnx::TensorProto::EXTERNAL);
        for (const auto& field : std::map<std::string, std::string>{
               {"location", "weights.bin"}, {"offset", std::to_string(offset)},
               {"length", std::to_string(raw.size())}}) {
          auto* entry = tensor.add_external_data();
          entry->set_key(field.first);
          entry->set_value(field.second);
        }
      }
      auto source = makeSource();
      source.initializerBytes = backing;
      const auto external = inspectNativeOnnxSourceGraph(source, descriptor, control);
      BOOST_CHECK_EQUAL(external.graphMetadataJson, inlined.graphMetadataJson);
      BOOST_CHECK_EQUAL(external.canonicalIdentity.graphDigest, inlined.canonicalIdentity.graphDigest);
      BOOST_CHECK_EQUAL(external.canonicalIdentity.initializerDigest,
                        inlined.canonicalIdentity.initializerDigest);
      for (const bool foreignDomain : {false, true}) {
        model.mutable_opset_import(1)->set_version(foreignDomain ? 1 : 2);
        if (foreignDomain) {
          model.mutable_opset_import(1)->set_domain("fixture.unknown");
          node->set_domain("fixture.unknown");
        }
        source.modelBytes = makeSource().modelBytes;
        BOOST_CHECK_EXCEPTION(inspectNativeOnnxSourceGraph(source, descriptor, control),
          std::runtime_error, [] (const auto& error) {
            return std::string(error.what()).find("SHAPE_INPUT_UNSUPPORTED") != std::string::npos;
          });
      }
    }
  }
}

BOOST_AUTO_TEST_CASE(SingleInitializerBeyondProtobufLimitStreamsIntoExternalFile)
{
  struct GeneratedRange final : NativeCanonicalByteRangeSource
  {
    mutable std::uint64_t largestRead = 0;
    mutable bool crossedLimit = false;

    std::uint64_t size() const noexcept override
    {
      return (std::uint64_t{1} << 31) + 32;
    }

    std::vector<std::uint8_t> read(std::uint64_t offset, std::uint64_t length) const override
    {
      if (offset > size() || length > size() - offset || length > (1U << 20))
        throw std::runtime_error("generated initializer read outside bounded range");
      largestRead = std::max(largestRead, length);
      crossedLimit = crossedLimit || offset + length > (std::uint64_t{1} << 31);
      std::vector<std::uint8_t> bytes(length, 0);
      const auto tail = std::uint64_t{1} << 31;
      for (auto position = std::max(offset, tail); position < offset + length; ++position)
        bytes[position - offset] = static_cast<std::uint8_t>(position - tail);
      return bytes;
    }
  };
  auto range = std::make_shared<GeneratedRange>();
  std::weak_ptr<const NativeCanonicalByteRangeSource> weak = range;
  NativeCanonicalSource source;
  source.initializerRangeSource = range;
  onnx::ModelProto model;
  model.set_ir_version(8);
  model.add_opset_import()->set_version(13);
  model.mutable_graph()->set_name("spec220-single-large-initializer");
  auto* output = model.mutable_graph()->add_output();
  output->set_name("Y");
  auto* type = output->mutable_type()->mutable_tensor_type();
  type->set_elem_type(onnx::TensorProto::UINT8);
  type->mutable_shape()->add_dim()->set_dim_value(range->size());
  auto* node = model.mutable_graph()->add_node();
  node->set_op_type("Identity");
  node->add_input("weight");
  node->add_output("Y");
  auto* weight = model.mutable_graph()->add_initializer();
  weight->set_name("weight");
  weight->set_data_type(onnx::TensorProto::UINT8);
  weight->add_dims(range->size());
  weight->set_data_location(onnx::TensorProto::EXTERNAL);
  for (const auto& field : std::map<std::string, std::string>{
         {"location", "source.data"}, {"offset", "0"},
         {"length", std::to_string(range->size())}}) {
    auto* entry = weight->add_external_data();
    entry->set_key(field.first);
    entry->set_value(field.second);
  }
  std::string wire;
  BOOST_REQUIRE(model.SerializeToString(&wire));
  source.modelBytes.assign(wire.begin(), wire.end());
  const NativeAssemblyControl control{
    Clock::now() + std::chrono::seconds(300), [] {},
    std::uint64_t{8} << 30, std::uint64_t{8} << 30};
  auto manifest = deriveNativeCanonicalMaterialManifest(source, control);
  BOOST_REQUIRE(manifest);
  source.materialManifest = manifest;
  source.materialPayloads = manifest->payloads;
  ndn::util::Sha256 hash;
  std::uint64_t total = 0;
  std::size_t writes = 0;
  const auto files = materializeNativeCanonicalModelFiles(source, {0}, {}, {}, control,
    [&] (const auto& bytes) {
      BOOST_REQUIRE_LE(bytes.size(), 1U << 20);
      hash.update(ndn::span<const std::uint8_t>(bytes.data(), bytes.size()));
      total += bytes.size();
      ++writes;
    });
  const std::string expected =
    "sha256:8998f71cf2403c396823a210fb680279178083d7f100453b2d82a2757cdc32bd";
  auto digest = hash.toString();
  std::transform(digest.begin(), digest.end(), digest.begin(), [] (unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  BOOST_CHECK_EQUAL("sha256:" + digest, expected);
  BOOST_CHECK_EQUAL(files.initializerDigest, expected);
  BOOST_CHECK_EQUAL(total, range->size());
  BOOST_CHECK_EQUAL(files.initializerBytes, range->size());
  BOOST_CHECK_EQUAL(writes, 2049U);
  BOOST_CHECK(range->crossedLimit);
  BOOST_CHECK_LE(range->largestRead, 1U << 20);
  BOOST_CHECK_LT(files.modelBytes.size(), 4096U);
  onnx::ModelProto external;
  BOOST_REQUIRE(external.ParseFromArray(files.modelBytes.data(),
                                       static_cast<int>(files.modelBytes.size())));
  const auto& result = external.graph().initializer(0);
  BOOST_CHECK(result.raw_data().empty());
  BOOST_CHECK_EQUAL(result.data_location(), onnx::TensorProto::EXTERNAL);
  std::map<std::string, std::string> fields;
  for (const auto& entry : result.external_data()) fields[entry.key()] = entry.value();
  BOOST_CHECK_EQUAL(fields.at("location"), "model.onnx.data");
  BOOST_CHECK_EQUAL(fields.at("offset"), "0");
  BOOST_CHECK_EQUAL(fields.at("length"), std::to_string(total));
  BOOST_CHECK(source.materialPayloads.empty());
  source.initializerRangeSource.reset();
  source.materialManifest.reset();
  manifest.reset();
  range.reset();
  BOOST_CHECK(weak.expired());
}

BOOST_AUTO_TEST_CASE(InitializerFileOwnerSurvivesUnlinkAndRejectsChangedOrInvalidFiles)
{
  char path[] = "/var/tmp/ndnsf-di-initializer-test-XXXXXX";
  const int fd = ::mkstemp(path);
  BOOST_REQUIRE_GE(fd, 0);
  struct Cleanup {
    int fd;
    const char* path;
    ~Cleanup() { ::close(fd); ::unlink(path); }
  } cleanup{fd, path};
  const std::vector<std::uint8_t> bytes{3, 7, 11, 19, 23};
  BOOST_REQUIRE_EQUAL(::write(fd, bytes.data(), bytes.size()), bytes.size());
  const NativeAssemblyControl control{
    Clock::now() + std::chrono::seconds(30), [] {}, 1024, 1024};
  const auto expected = nativePlanningDigest(bytes.data(), bytes.size());
  BOOST_CHECK_THROW(openNativeCanonicalInitializerFile(path, bytes.size() + 1, expected, control),
                    std::exception);
  BOOST_CHECK_THROW(openNativeCanonicalInitializerFile(path, bytes.size(),
                    nativePlanningDigest("wrong"), control), std::exception);
  auto owner = openNativeCanonicalInitializerFile(path, bytes.size(), expected, control);
  BOOST_REQUIRE(owner);
  int ownedFd = -1;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
    std::error_code error;
    if (std::filesystem::read_symlink(entry.path(), error) == path && !error) {
      const int candidate = std::stoi(entry.path().filename().string());
      if (candidate != fd) {
        BOOST_REQUIRE_EQUAL(ownedFd, -1);
        ownedFd = candidate;
      }
    }
  }
  BOOST_REQUIRE_GE(ownedFd, 0);
  BOOST_REQUIRE_EQUAL(::unlink(path), 0);
  BOOST_CHECK(owner->read(0, bytes.size()) == bytes);
  std::weak_ptr<const NativeCanonicalByteRangeSource> weak = owner;
  BOOST_REQUIRE_EQUAL(::ftruncate(fd, bytes.size() - 1), 0);
  BOOST_CHECK_THROW(owner->read(0, bytes.size()), std::exception);
  owner.reset();
  BOOST_CHECK(weak.expired());
  errno = 0;
  BOOST_CHECK_EQUAL(::fcntl(ownedFd, F_GETFD), -1);
  BOOST_CHECK_EQUAL(errno, EBADF);
}

BOOST_AUTO_TEST_CASE(OwnedInitializerSnapshotReusesDigestAndKeepsCancellationAndBounds)
{
  char path[] = "/var/tmp/ndnsf-di-owned-initializer-XXXXXX";
  const int fd = ::mkstemp(path);
  BOOST_REQUIRE_GE(fd, 0);
  const std::vector<std::uint8_t> bytes(2U << 20, 17);
  BOOST_REQUIRE_EQUAL(::write(fd, bytes.data(), bytes.size()), bytes.size());
  BOOST_REQUIRE_EQUAL(::close(fd), 0);
  struct Cleanup {
    const char* path;
    ~Cleanup() { ::unlink(path); }
  } cleanup{path};
  const int probe = ::open(path, O_RDONLY | O_CLOEXEC);
  BOOST_REQUIRE_GE(probe, 0);
  const bool supportsLease = ::fcntl(probe, F_SETLEASE, F_RDLCK) == 0;
  if (supportsLease) BOOST_REQUIRE_EQUAL(::fcntl(probe, F_SETLEASE, F_UNLCK), 0);
  BOOST_REQUIRE_EQUAL(::close(probe), 0);
  bool cancelled = false;
  NativeAssemblyControl control{Clock::now() + std::chrono::seconds(30), [&] {
    if (cancelled) throw std::runtime_error("owned snapshot cancelled");
  }, bytes.size(), bytes.size()};
  const auto expected = nativePlanningDigest(bytes.data(), bytes.size());
  NativeCanonicalSource source;
  source.initializerRangeSource = takeNativeCanonicalInitializerFile(
    path, bytes.size(), expected, control);
  BOOST_CHECK(!std::filesystem::exists(path));
  const auto readCharacters = [] {
    std::ifstream input("/proc/self/io");
    std::string name;
    std::uint64_t count;
    while (input >> name >> count) if (name == "rchar:") return count;
    throw std::runtime_error("native test I/O counters unavailable");
  };
  const auto before = readCharacters();
  for (int i = 0; i < 3; ++i)
    BOOST_CHECK_EQUAL(nativeCanonicalInitializerDigest(source, control), expected);
  const auto delta = readCharacters() - before;
  if (supportsLease) BOOST_CHECK_LT(delta, bytes.size());
  BOOST_TEST_MESSAGE("OWNED_INITIALIZER_DIGEST_IO lease=" << supportsLease
                     << " bytes=" << bytes.size() << " repeat_rchar=" << delta);
  cancelled = true;
  BOOST_CHECK_THROW(nativeCanonicalInitializerDigest(source, control), std::runtime_error);
  cancelled = false;
  control.maxSourceBytes = bytes.size() - 1;
  BOOST_CHECK_THROW(nativeCanonicalInitializerDigest(source, control), std::exception);
}

BOOST_AUTO_TEST_CASE(OwnedInitializerWithOutstandingWriterRetainsByteVerification)
{
  char path[] = "/var/tmp/ndnsf-di-writable-initializer-XXXXXX";
  const int fd = ::mkstemp(path);
  BOOST_REQUIRE_GE(fd, 0);
  struct Cleanup {
    int fd;
    const char* path;
    ~Cleanup() { ::close(fd); ::unlink(path); }
  } cleanup{fd, path};
  const std::vector<std::uint8_t> bytes{3, 7, 11, 19, 23};
  BOOST_REQUIRE_EQUAL(::write(fd, bytes.data(), bytes.size()), bytes.size());
  const NativeAssemblyControl control{
    Clock::now() + std::chrono::seconds(30), [] {}, 1024, 1024};
  const auto expected = nativePlanningDigest(bytes.data(), bytes.size());
  NativeCanonicalSource source;
  source.initializerRangeSource = takeNativeCanonicalInitializerFile(
    path, bytes.size(), expected, control);
  BOOST_CHECK(!std::filesystem::exists(path));
  const std::uint8_t changed = 31;
  BOOST_REQUIRE_EQUAL(::pwrite(fd, &changed, 1, 0), 1);
  BOOST_CHECK_NE(nativeCanonicalInitializerDigest(source, control), expected);
}

BOOST_AUTO_TEST_CASE(RangeFileMutationAfterAdmissionCannotPublishStaleMaterialDigest)
{
  Input input(true);
  const auto bytes = input.source->initializerBytes->asVector();
  char path[] = "/var/tmp/ndnsf-di-material-mutation-XXXXXX";
  const int fd = ::mkstemp(path);
  BOOST_REQUIRE_GE(fd, 0);
  struct Cleanup {
    int fd;
    const char* path;
    ~Cleanup() { ::close(fd); ::unlink(path); }
  } cleanup{fd, path};
  BOOST_REQUIRE_EQUAL(::write(fd, bytes.data(), bytes.size()), bytes.size());
  const NativeAssemblyControl control{
    input.control.deadline, [] {}, 1U << 20, 1U << 20};
  input.source->initializerRangeSource = openNativeCanonicalInitializerFile(
    path, bytes.size(), nativePlanningDigest(bytes.data(), bytes.size()), control);
  input.source->initializerBytes.reset();
  input.source->materialManifest = deriveNativeCanonicalMaterialManifest(*input.source, control);
  input.model.canonicalGraphDigest = input.source->materialManifest->graphDigest;
  BOOST_REQUIRE_EQUAL(::unlink(path), 0);
  TransportFixture io;
  auto transport = io.transport();
  const auto begin = transport.begin;
  transport.begin = [&, begin] {
    const auto request = begin();
    const std::uint8_t changed = bytes.front() ^ 1;
    if (::pwrite(fd, &changed, 1, 0) != 1)
      throw std::runtime_error("fixture same-size mutation failed");
    return request;
  };
  auto publisher = NativeCanonicalPublisherTestAccess::create(
    std::move(transport), input.options, input.resolver());
  BOOST_CHECK_EXCEPTION(publisher.prepare(input.model, input.control), std::runtime_error,
    [] (const auto& error) {
      return std::string(error.what()).find("MATERIAL_DIGEST_MISMATCH") != std::string::npos;
    });
  BOOST_CHECK_EQUAL(io.beginCalls, 1U);
  BOOST_CHECK(io.payloads.empty());
  BOOST_CHECK(io.labels.empty());
}

BOOST_AUTO_TEST_CASE(InlineInitializerUsesBoundedChunksAndReassemblesAfterSelection)
{
  NativeCanonicalSource source;
  onnx::ModelProto model;
  model.set_ir_version(8);
  model.mutable_graph()->set_name("spec190-inline-chunked-initializer");
  auto* input = model.mutable_graph()->add_input();
  input->set_name("X");
  input->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::FLOAT16);
  input->mutable_type()->mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
  auto* output = model.mutable_graph()->add_output();
  output->set_name("Y");
  output->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::FLOAT16);
  output->mutable_type()->mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
  auto* node = model.mutable_graph()->add_node();
  node->set_op_type("MatMul");
  node->add_input("X");
  node->add_input("weight");
  node->add_output("Y");
  constexpr std::size_t rawBytes = 3U * 1024U * 1024U + 16U;
  auto* initializer = model.mutable_graph()->add_initializer();
  initializer->set_name("weight");
  initializer->set_data_type(onnx::TensorProto::FLOAT16);
  initializer->add_dims(static_cast<std::int64_t>(rawBytes / 2));
  std::string raw(rawBytes, '\0');
  for (std::size_t i = 0; i < rawBytes; ++i)
    raw[i] = static_cast<char>(i * 19U + 7U);
  initializer->set_raw_data(raw);
  std::string wire;
  BOOST_REQUIRE(model.SerializeToString(&wire));
  source.modelBytes.assign(wire.begin(), wire.end());

  const NativeAssemblyControl control{
    Clock::now() + std::chrono::seconds(30), [] {}, 8U * 1024U * 1024U,
    16U * 1024U * 1024U};
  const auto manifest = deriveNativeCanonicalMaterialManifest(source, control);
  BOOST_REQUIRE(manifest);
  validateNativeCanonicalMaterialManifest(source, *manifest, control);
  const auto reference = std::find_if(manifest->references.begin(), manifest->references.end(),
    [] (const auto& item) { return item.kind == "shared-initializer"; });
  BOOST_REQUIRE(reference != manifest->references.end());
  BOOST_REQUIRE_EQUAL(reference->chunkPayloadIds.size(), 4U);
  BOOST_CHECK(reference->bytes < NativeCanonicalMaterialBundleMaxBytes);
  for (const auto& chunkId : reference->chunkPayloadIds) {
    const auto payload = std::find_if(manifest->payloads.begin(), manifest->payloads.end(),
      [&chunkId] (const auto& item) { return item.payloadId == chunkId; });
    BOOST_REQUIRE(payload != manifest->payloads.end());
    BOOST_CHECK_LE(payload->byteSize(), NativeCanonicalMaterialBundleMaxBytes);
    BOOST_CHECK(payload->bytes.empty());
    BOOST_CHECK(payload->stringBacking);
  }

  source.materialManifest = manifest;
  source.materialPayloads = manifest->payloads;
  const auto assembled = materializeNativeCanonicalModel(source, {0}, control);
  onnx::ModelProto roundTrip;
  BOOST_REQUIRE(roundTrip.ParseFromArray(assembled.data(), static_cast<int>(assembled.size())));
  BOOST_REQUIRE_EQUAL(roundTrip.graph().initializer_size(), 1);
  BOOST_REQUIRE_EQUAL(roundTrip.graph().initializer(0).raw_data().size(), rawBytes);
  BOOST_CHECK_EQUAL(roundTrip.graph().initializer(0).raw_data(), raw);

  // The production file-pair path must preserve logical initializer identity
  // while keeping the graph small and delivering only bounded raw windows.
  const auto inlineIdentity = canonicalOnnxSourceIdentity(source, control);
  source.materialPayloads = manifest->payloads;
  std::vector<std::uint8_t> sidecar;
  std::size_t writes = 0;
  const auto files = materializeNativeCanonicalModelFiles(source, {0}, {}, {}, control,
    [&] (const auto& bytes) {
      BOOST_CHECK_LE(bytes.size(), NativeCanonicalMaterialBundleMaxBytes);
      sidecar.insert(sidecar.end(), bytes.begin(), bytes.end());
      ++writes;
    });
  BOOST_CHECK_EQUAL(writes, 4U);
  BOOST_CHECK_LT(files.modelBytes.size(), 4096U);
  BOOST_CHECK_EQUAL(files.initializerBytes, rawBytes);
  BOOST_CHECK_EQUAL(files.initializerDigest,
                    nativePlanningDigest(sidecar.data(), sidecar.size()));
  BOOST_REQUIRE_EQUAL(sidecar.size(), raw.size());
  BOOST_CHECK(std::equal(sidecar.begin(), sidecar.end(),
    reinterpret_cast<const std::uint8_t*>(raw.data())));
  BOOST_CHECK(source.materialPayloads.empty());
  onnx::ModelProto external;
  BOOST_REQUIRE(external.ParseFromArray(files.modelBytes.data(),
                                        static_cast<int>(files.modelBytes.size())));
  const auto& weight = external.graph().initializer(0);
  BOOST_CHECK_EQUAL(weight.data_location(), onnx::TensorProto::EXTERNAL);
  BOOST_CHECK(weight.raw_data().empty());
  std::map<std::string, std::string> ranges;
  for (const auto& field : weight.external_data()) ranges[field.key()] = field.value();
  BOOST_CHECK_EQUAL(ranges.at("location"), "model.onnx.data");
  BOOST_CHECK_EQUAL(ranges.at("offset"), "0");
  BOOST_CHECK_EQUAL(ranges.at("length"), std::to_string(rawBytes));
  NativeCanonicalSource externalSource;
  externalSource.modelBytes = files.modelBytes;
  externalSource.initializerBytes = sidecar;
  const auto externalIdentity = canonicalOnnxSourceIdentity(externalSource, control);
  BOOST_CHECK_EQUAL(externalIdentity.graphDigest, inlineIdentity.graphDigest);
  BOOST_CHECK_EQUAL(externalIdentity.initializerDigest, inlineIdentity.initializerDigest);

  source.materialPayloads = manifest->payloads;
  std::size_t failedWrites = 0;
  BOOST_CHECK_EXCEPTION(materializeNativeCanonicalModelFiles(source, {0}, {}, {}, control,
    [&] (const auto&) {
      ++failedWrites;
      throw std::runtime_error("fixture sidecar write failure");
    }), std::runtime_error, [] (const auto& error) {
      return std::string(error.what()) == "fixture sidecar write failure";
    });
  BOOST_CHECK_EQUAL(failedWrites, 1U);
}

BOOST_AUTO_TEST_CASE(InlineInitializerAtBundleBoundaryUsesChunks)
{
  NativeCanonicalSource source;
  onnx::ModelProto model;
  model.set_ir_version(8);
  model.mutable_graph()->set_name("spec190-inline-boundary-initializer");
  auto* input = model.mutable_graph()->add_input();
  input->set_name("X");
  input->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::UINT8);
  input->mutable_type()->mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
  auto* output = model.mutable_graph()->add_output();
  output->set_name("Y");
  output->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::UINT8);
  output->mutable_type()->mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
  auto* node = model.mutable_graph()->add_node();
  node->set_op_type("Identity");
  node->add_input("X");
  node->add_output("Y");
  auto* initializer = model.mutable_graph()->add_initializer();
  initializer->set_name("weight");
  initializer->set_data_type(onnx::TensorProto::UINT8);
  initializer->add_dims(static_cast<std::int64_t>(NativeCanonicalMaterialBundleMaxBytes));
  initializer->set_raw_data(std::string(NativeCanonicalMaterialBundleMaxBytes, '\x5a'));
  std::string wire;
  BOOST_REQUIRE(model.SerializeToString(&wire));
  source.modelBytes.assign(wire.begin(), wire.end());

  const NativeAssemblyControl control{
    Clock::now() + std::chrono::seconds(30), [] {}, 8U * 1024U * 1024U,
    16U * 1024U * 1024U};
  const auto manifest = deriveNativeCanonicalMaterialManifest(source, control);
  BOOST_REQUIRE(manifest);
  validateNativeCanonicalMaterialManifest(source, *manifest, control);
  const auto reference = std::find_if(manifest->references.begin(), manifest->references.end(),
    [] (const auto& item) { return item.kind == "shared-initializer"; });
  BOOST_REQUIRE(reference != manifest->references.end());
  BOOST_REQUIRE_EQUAL(reference->chunkPayloadIds.size(), 1U);
  const auto payload = std::find_if(manifest->payloads.begin(), manifest->payloads.end(),
    [&reference] (const auto& item) { return item.payloadId == reference->chunkPayloadIds.front(); });
  BOOST_REQUIRE(payload != manifest->payloads.end());
  BOOST_CHECK_EQUAL(payload->byteSize(), NativeCanonicalMaterialBundleMaxBytes);
}

BOOST_AUTO_TEST_CASE(ModelIdentityBindsWeightOnlyQuantizationSubtype)
{
  auto model = fixture::completeModel(NativeModelDescriptor{
    "fixture", nativePlanningDigest("content"), nativePlanningDigest("semantics"), nativePlanningDigest("graph"), "onnx", "float32",
    "fixture", "1", {}, "revision"});
  BOOST_CHECK_EQUAL(model.quantizationSubtype, "none");
  const auto legacyJson = model.canonicalJson();
  BOOST_CHECK(legacyJson.find("quantization_subtype") == std::string::npos);

  model.quantizationSubtype = "weight_only_int8";
  const auto quantizedJson = model.canonicalJson();
  BOOST_CHECK(quantizedJson.find("weight_only_int8") != std::string::npos);
  BOOST_CHECK(model.modelDigest() != fixture::completeModel(NativeModelDescriptor{
    "fixture", nativePlanningDigest("content"), nativePlanningDigest("semantics"), nativePlanningDigest("graph"), "onnx", "float32",
    "fixture", "1", {}, "revision"}).modelDigest());

  auto decoded = NativeModelDescriptor::fromCanonicalJson(quantizedJson);
  BOOST_CHECK_EQUAL(decoded.quantizationSubtype, "weight_only_int8");
  const auto int8Digest = model.modelDigest();
  model.quantizationSubtype = "weight_only_int4";
  const auto int4Json = model.canonicalJson();
  BOOST_CHECK(int4Json.find("weight_only_int4") != std::string::npos);
  BOOST_CHECK(model.modelDigest() != int8Digest);
  BOOST_CHECK(int4Json != legacyJson);
  decoded = NativeModelDescriptor::fromCanonicalJson(int4Json);
  BOOST_CHECK_EQUAL(decoded.quantizationSubtype, "weight_only_int4");
  BOOST_CHECK_EQUAL(decoded.modelDigest(), model.modelDigest());
  const auto int4Digest = model.modelDigest();
  model.quantizationSubtype = "dynamic_int8";
  const auto dynamicJson = model.canonicalJson();
  decoded = NativeModelDescriptor::fromCanonicalJson(dynamicJson);
  BOOST_CHECK_EQUAL(decoded.quantizationSubtype, "dynamic_int8");
  BOOST_CHECK_EQUAL(decoded.modelDigest(), model.modelDigest());
  BOOST_CHECK(model.modelDigest() != int8Digest);
  BOOST_CHECK(model.modelDigest() != int4Digest);
  BOOST_CHECK(dynamicJson != legacyJson);
  model.quantizationSubtype = "unsupported";
  BOOST_CHECK_THROW(model.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(MaterializedRoleRebuildsInternalStageBoundary)
{
  NativeCanonicalSource source;
  onnx::ModelProto model;
  model.set_ir_version(8);
  model.add_opset_import()->set_version(17);
  model.mutable_graph()->set_name("spec189-role-boundary");
  auto* input = model.mutable_graph()->add_input();
  input->set_name("x");
  input->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::FLOAT);
  input->mutable_type()->mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
  input->mutable_type()->mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(4);
  auto* output = model.mutable_graph()->add_output();
  output->set_name("z");
  output->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::FLOAT);
  output->mutable_type()->mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
  output->mutable_type()->mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(4);
  auto* first = model.mutable_graph()->add_node();
  first->set_op_type("Identity");
  first->add_input("x");
  first->add_output("hidden_states_out");
  auto* second = model.mutable_graph()->add_node();
  second->set_op_type("Identity");
  second->add_input("hidden_states_out");
  second->add_output("z");
  std::string wire;
  BOOST_REQUIRE(model.SerializeToString(&wire));
  source.modelBytes.assign(wire.begin(), wire.end());

  NativeAssemblyControl control{
    Clock::now() + std::chrono::seconds(10), [] {}, 1U << 20, 1U << 20};
  source.materialManifest = deriveNativeCanonicalMaterialManifest(source, control);
  BOOST_REQUIRE(source.materialManifest);
  source.materialPayloads = source.materialManifest->payloads;
  const std::vector<NativeAssemblyTensorContractV3> inputs{
    {"x", "float32", {std::int64_t(1), std::int64_t(4)}}};
  const std::vector<NativeAssemblyTensorContractV3> outputs{
    {"hidden_states_out", "float32", {std::int64_t(1), std::int64_t(4)}}};

  const auto assembled = materializeNativeCanonicalModel(
    source, {0}, inputs, outputs, control);
  onnx::ModelProto role;
  BOOST_REQUIRE(role.ParseFromArray(assembled.data(), static_cast<int>(assembled.size())));
  BOOST_REQUIRE_NO_THROW(onnx::checker::check_model(role, true));
  BOOST_REQUIRE_EQUAL(role.graph().node_size(), 1);
  BOOST_REQUIRE_EQUAL(role.graph().input_size(), 1);
  BOOST_REQUIRE_EQUAL(role.graph().output_size(), 1);
  BOOST_CHECK_EQUAL(role.graph().input(0).name(), "x");
  BOOST_CHECK_EQUAL(role.graph().output(0).name(), "hidden_states_out");
  BOOST_CHECK_EQUAL(role.graph().output(0).type().tensor_type().elem_type(),
                    onnx::TensorProto::FLOAT);
  BOOST_REQUIRE_EQUAL(role.graph().output(0).type().tensor_type().shape().dim_size(), 2);
  BOOST_CHECK_EQUAL(role.graph().output(0).type().tensor_type().shape().dim(1).dim_value(), 4);
}

BOOST_AUTO_TEST_CASE(MaterializedRoleKeepsUnrankedBoundaryTensor)
{
  // The 4B canonical source records some cross-stage Reshape shape constants
  // in value_info with an empty shape message (declared rank 0) although the
  // producing Constant is 1-D.  A role that consumes such a tensor receives
  // it as a boundary contract whose shape array is empty: the contract has no
  // rank information.  Materializing the empty array as an empty
  // TensorShapeProto would assert a scalar input, and ORT rejects the role at
  // Reshape shape inference ("shape input must be 1D tensor").  Keep the
  // boundary unranked instead; ORT accepts an input without a shape.
  NativeCanonicalSource source;
  onnx::ModelProto model;
  model.set_ir_version(8);
  model.add_opset_import()->set_version(17);
  model.mutable_graph()->set_name("spec189-unranked-boundary");
  auto* input = model.mutable_graph()->add_input();
  input->set_name("x");
  input->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::FLOAT);
  input->mutable_type()->mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
  input->mutable_type()->mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(6);
  auto* output = model.mutable_graph()->add_output();
  output->set_name("z");
  output->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::FLOAT);
  auto* outputShape = output->mutable_type()->mutable_tensor_type()->mutable_shape();
  outputShape->add_dim()->set_dim_value(1);
  outputShape->add_dim()->set_dim_value(3);
  outputShape->add_dim()->set_dim_value(2);
  // A shape constant owned by another role, plus the value_info entry the
  // canonical source carries for its output.
  auto* shapeInfo = model.mutable_graph()->add_value_info();
  shapeInfo->set_name("reshape_shape");
  shapeInfo->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::INT64);
  shapeInfo->mutable_type()->mutable_tensor_type()->mutable_shape();
  auto* constant = model.mutable_graph()->add_node();
  constant->set_op_type("Constant");
  constant->add_output("reshape_shape");
  auto* value = constant->add_attribute();
  value->set_name("value");
  value->set_type(onnx::AttributeProto::TENSOR);
  auto* tensor = value->mutable_t();
  tensor->set_data_type(onnx::TensorProto::INT64);
  tensor->add_dims(3);
  for (const std::int64_t dimension : {1, 3, 2})
    tensor->add_int64_data(dimension);
  auto* reshape = model.mutable_graph()->add_node();
  reshape->set_op_type("Reshape");
  reshape->add_input("x");
  reshape->add_input("reshape_shape");
  reshape->add_output("z");
  std::string wire;
  BOOST_REQUIRE(model.SerializeToString(&wire));
  source.modelBytes.assign(wire.begin(), wire.end());

  const NativeAssemblyControl control{
    Clock::now() + std::chrono::seconds(30), [] {}, 1U << 20, 1U << 20};
  source.materialManifest = deriveNativeCanonicalMaterialManifest(source, control);
  BOOST_REQUIRE(source.materialManifest);
  source.materialPayloads = source.materialManifest->payloads;
  const std::vector<NativeAssemblyTensorContractV3> inputs{
    {"x", "float32", {std::int64_t(1), std::int64_t(6)}},
    {"reshape_shape", "int64", {}}};
  const std::vector<NativeAssemblyTensorContractV3> outputs{
    {"z", "float32", {std::int64_t(1), std::int64_t(3), std::int64_t(2)}}};

  const auto assembled = materializeNativeCanonicalModel(
    source, {1}, inputs, outputs, control);
  onnx::ModelProto role;
  BOOST_REQUIRE(role.ParseFromArray(assembled.data(), static_cast<int>(assembled.size())));
  BOOST_REQUIRE_EQUAL(role.graph().node_size(), 1);
  BOOST_REQUIRE_EQUAL(role.graph().input_size(), 2);
  if (role.graph().input(0).name() == "reshape_shape") {
    BOOST_CHECK(!role.graph().input(0).type().tensor_type().has_shape());
    BOOST_REQUIRE(role.graph().input(1).type().tensor_type().has_shape());
    BOOST_CHECK_EQUAL(role.graph().input(1).type().tensor_type().shape().dim_size(), 2);
  }
  else {
    BOOST_CHECK_EQUAL(role.graph().input(0).name(), "x");
    BOOST_REQUIRE(role.graph().input(0).type().tensor_type().has_shape());
    BOOST_REQUIRE_EQUAL(role.graph().input(1).name(), "reshape_shape");
    BOOST_CHECK(!role.graph().input(1).type().tensor_type().has_shape());
  }
  // The materialized-role certificate path intentionally skips the full ONNX
  // checker (which requires a shape on every graph input) because the source
  // value declaration is not authentic rank information.  ORT remains the
  // authoritative producer-side load check for this role model.
  Ort::Env environment(ORT_LOGGING_LEVEL_ERROR, "spec189-unranked-boundary");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  BOOST_CHECK_NO_THROW(Ort::Session session(environment, assembled.data(),
                                            assembled.size(), options));
}

BOOST_AUTO_TEST_CASE(MaterialFragmentCacheIsContentAddressedAndValidated)
{
  const auto root = std::filesystem::temp_directory_path() /
    ("spec222-material-fragment-cache-" + std::to_string(::getpid()));
  std::error_code cleanupError;
  std::filesystem::remove_all(root, cleanupError);
  struct Cleanup
  {
    std::filesystem::path path;
    ~Cleanup()
    {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{root};
  NativeMaterialFragmentCache cache(root / "material");
  const std::string payload = "native material payload bytes";
  const auto payloadBytes = ndn::span<const std::uint8_t>(
    reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
  ndn::util::Sha256 hasher;
  hasher.update(payloadBytes);
  auto hex = hasher.toString();
  std::transform(hex.begin(), hex.end(), hex.begin(), [] (unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  const auto digest = "sha256:" + hex;
  BOOST_CHECK(cache.write(digest, payloadBytes));
  const auto read = cache.read(digest, payload.size(), 1024);
  BOOST_REQUIRE(read);
  BOOST_CHECK(std::equal(read->begin(), read->end(), payload.begin(), payload.end()));
  BOOST_CHECK(!cache.read(digest, payload.size() + 1, 1024));
  BOOST_CHECK(!cache.read("sha256:" + std::string(64, '0'), 1, 1024));
  BOOST_CHECK(!cache.read("sha256:../" + std::string(61, '0'), 1, 1024));
  // Damaged content is ignored and replaced on the next verified write.
  const auto path = root / "material" / "sha256" / hex.substr(0, 2) / hex;
  {
    std::ofstream output(path, std::ios::trunc | std::ios::binary);
    output << "tampered";
    BOOST_REQUIRE(output.good());
  }
  BOOST_CHECK(!cache.read(digest, payload.size(), 1024));
  BOOST_CHECK(cache.write(digest, payloadBytes));
  BOOST_CHECK(cache.read(digest, payload.size(), 1024));
  // A symlink is never followed.
  std::filesystem::remove(path);
  std::filesystem::create_symlink(root / "material" / "elsewhere", path);
  BOOST_CHECK(!cache.read(digest, payload.size(), 1024));
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

BOOST_AUTO_TEST_CASE(CanonicalRoleProducerMapsSemanticNodesToActualOnnxSource)
{
  for (bool external : {false, true}) {
    Input input(external);
    // One semantic node owns two real ONNX nodes; source index 1 is not a
    // planning ordinal. No recipe or tensor contract is injected into the port.
    input.model.graph.nodes = {{"layer-00", "decoder-layer", 0}};
    input.model.graph.topologicalOrder = {"layer-00"};
    input.candidate.nodeRoles = {{"layer-00", "/role"}};
    input.candidate.candidateDigest = input.candidate.computedDigest();
    const auto& original = input.roles.front();
    NativeRoleRecipeProfile profile{original.artifactProfileDigest, original.assemblerDescriptorDigest,
      original.backendAbi, original.precision, original.quantization, original.layout, original.padding,
      original.protectionEpoch, original.maxSourceBytes, original.maxAssembledBytes, original.maxNodes};
    NativeAssemblyControl control{input.control.deadline, [&] { input.control.requireActive(); },
      profile.maxSourceBytes, profile.maxAssembledBytes};
    const auto sourceGraph = inspectNativeOnnxSourceGraph(*input.source, input.model.descriptor, control);
    BOOST_CHECK_EQUAL(sourceGraph.graph.nodes.size(), 2);
    BOOST_CHECK_NE(sourceGraph.graph.graphDigest, input.model.graph.graphDigest);
    BOOST_CHECK_THROW(inspectNativeOnnxPlanningGraph(*input.source, input.model.descriptor, control), std::invalid_argument);
    BOOST_CHECK_THROW(NativeCanonicalRolePreparer(input.model, *input.source, profile, control), std::invalid_argument);
    const NativeCanonicalRolePreparer owner(input.model, *input.source, profile, control, {{"layer-00", {0, 1}}});
    NativeRequestPreparation preparation(std::make_shared<NativeAdapterRegistry>(), {}, {}, owner.rolePort());
    const auto roles = preparation.prepareRoles(input.model, input.candidate, input.control);
    BOOST_REQUIRE_EQUAL(roles.size(), 1);
    const auto& role = roles.front();
    BOOST_CHECK(role.nodeIndices == std::vector<std::uint64_t>({0, 1}));
    BOOST_CHECK_EQUAL(role.roleKind, "COMPONENT_SET");
    BOOST_CHECK_EQUAL(role.layerBegin, 0); BOOST_CHECK_EQUAL(role.layerEnd, 0);
    BOOST_REQUIRE_EQUAL(role.expectedInputs.size(), 1);
    BOOST_CHECK_EQUAL(role.expectedInputs.front().name, "X");
    BOOST_REQUIRE_EQUAL(role.expectedOutputs.size(), 1);
    BOOST_CHECK_EQUAL(role.expectedOutputs.front().name, "Y");
    BOOST_CHECK_EQUAL(role.adapterDescriptorDigest, input.model.descriptor.adapter.descriptorDigest());
    const auto assembled = assembleNativeCertifiedOnnxModel(*input.source, role, control);
    BOOST_CHECK_EQUAL(assembled.nodeCount, 2);
    BOOST_CHECK_EQUAL(assembled.modelDigest, original.artifactDigest);
    TransportFixture io;
    auto publisher = NativeCanonicalPublisherTestAccess::create(io.transport(), input.options, input.resolver());
    NativeRequestPreparation publishing(std::make_shared<NativeAdapterRegistry>(), {}, publisher.artifactPort(), owner.rolePort());
    auto selected = input.proposal(); selected.roles = roles;
    selected.roles.front().backend = original.backend; // Explicit CPU placement fixture.
    const auto published = publishing.ensureArtifacts(input.model, input.candidate, selected, input.control);
    const auto rebound = NativeRequestPreparation::bindPublishedRoles(input.model, input.candidate, selected.roles, published);
    BOOST_CHECK_EQUAL(rebound.front().modelManifestDigest, published.manifestDigest);
    BOOST_CHECK_EQUAL(assembleNativeCertifiedOnnxModel(*input.source, rebound.front(), control).modelDigest, original.artifactDigest);
    // Resource-bound rejection is separate from exact source-node membership.
    for (const auto& mapping : std::vector<NativeCanonicalRolePreparer::NodeMap>{
           {{"layer-00", {0, 2}}}, {{"layer-00", {0, 0}}}, {{"layer-00", {0}}}, {{"foreign", {0, 1}}}})
      BOOST_CHECK_THROW(NativeCanonicalRolePreparer(input.model, *input.source, profile, control, mapping), std::invalid_argument);
    auto foreign = input.model; foreign.canonicalSourceDigest = nativePlanningDigest("foreign");
    BOOST_CHECK_THROW(NativeCanonicalRolePreparer(foreign, *input.source, profile, control,
      {{"layer-00", {0, 1}}}), std::invalid_argument);
    foreign = input.model; foreign.modelManifestDigest = nativePlanningDigest("foreign");
    BOOST_CHECK_THROW(owner.prepare(foreign, input.candidate, input.control), std::invalid_argument);
    auto wrongState = input.candidate;
    wrongState.roleStateInputsByRole["/role"] = {{"missing-state", "float32", {std::int64_t(1)}, 4}};
    wrongState.candidateDigest = wrongState.computedDigest();
    BOOST_CHECK_THROW(owner.prepare(input.model, wrongState, input.control), std::invalid_argument);
    auto cancelled = input.control; cancelled.cancelled = [] { return true; };
    BOOST_CHECK_THROW(owner.prepare(input.model, input.candidate, cancelled), std::runtime_error);
    // A genuinely identical source/planning graph needs no semantic mapping.
    auto direct = input.model;
    direct.graph = sourceGraph.graph; direct.descriptor.graphDigest = direct.graph.graphDigest;
    auto directCandidate = input.candidate;
    directCandidate.model = direct.descriptor; directCandidate.graphDigest = direct.graph.graphDigest;
    directCandidate.nodeRoles.clear();
    for (const auto& node : direct.graph.nodes) directCandidate.nodeRoles[node.id] = "/role";
    directCandidate.candidateDigest = directCandidate.computedDigest();
    const NativeCanonicalRolePreparer directOwner(direct, *input.source, profile, control);
    BOOST_CHECK(directOwner.prepare(direct, directCandidate, input.control).front().nodeIndices == role.nodeIndices);
    auto aliases = input.model;
    aliases.graph.nodes = {{"layer-00", "layer", 0}, {"layer-0", "layer", 1}};
    aliases.graph.topologicalOrder = {"layer-00", "layer-0"};
    auto aliasesCandidate = input.candidate;
    aliasesCandidate.nodeRoles = {{"layer-00", "/role"}, {"layer-0", "/role"}};
    aliasesCandidate.candidateDigest = aliasesCandidate.computedDigest();
    const NativeCanonicalRolePreparer aliasesOwner(aliases, *input.source, profile, control,
      {{"layer-00", {0}}, {"layer-0", {1}}});
    BOOST_CHECK_THROW(aliasesOwner.prepare(aliases, aliasesCandidate, input.control), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(NativeMergePublishesAndSealsWithoutOnnxRecipe)
{
  for (bool external : {false, true}) {
    Input input(external); TransportFixture io;
    auto merge = input.roles.front();
    merge.role = merge.selectedRole = "/Merge";
    merge.roleKind = "COMPONENT_SET"; merge.layerBegin = merge.layerEnd = 0;
    merge.nodeIndices = {1};
    merge.mergeKind = "NATIVE_POSTPROCESS";
    merge.postprocessIdentity = "YOLO26n-canonical-detection-rows";
    merge.postprocessOutputName = "predictions";
    merge.postprocessConfidenceThreshold = .001;
    merge.postprocessSort = "confidence-desc,class-asc,xyxy-asc";
    merge.expectedOutputs = {{"predictions", "float32", {std::int64_t(1), std::int64_t(300), std::int64_t(6)}}};
    merge.artifactDigest = nativePlanningDigest("native-merge-fragment");
    merge.recipeDigest = nativePlanningDigest("native-merge-candidate-recipe");
    merge.canonicalInitializerDigest.clear(); merge.assemblerDescriptorDigest.clear();
    merge.backendAbi.clear(); merge.precision.clear(); merge.quantization.clear();
    merge.layout.clear(); merge.padding.clear();
    merge.maxSourceBytes = merge.maxAssembledBytes = merge.maxNodes = 0;
    // Native Merge first exercises source-limit selection independently of role order.
    input.roles.insert(input.roles.begin(), merge);
    auto& candidate = input.candidate;
    candidate.executionPlan.roles.insert(candidate.executionPlan.roles.begin(), merge.role);
    candidate.nodeRoles["n1"] = merge.role;
    candidate.artifactsByRole[merge.role] = {merge.artifactDigest};
    candidate.rankArtifactDigestsByRole[merge.role] = {merge.artifactDigest};
    candidate.tensorDegreesByRole[merge.role] = 1;
    candidate.fragmentsByRole[merge.role] = merge.artifactDigest;
    candidate.requirementsByRole[merge.role] = {{"onnxruntime"}, 1, 0, 0, 0, 0, 1.0};
    candidate.inputIngressRole = "/role"; candidate.resultEgressRole = merge.role;
    candidate.mergeKind = merge.mergeKind;
    candidate.postprocessingJson = R"({"identity":"YOLO26n-canonical-detection-rows","outputName":"predictions","confidenceThreshold":0.001,"sort":"confidence-desc,class-asc,xyxy-asc"})";
    candidate.candidateDigest = candidate.computedDigest();
    std::ifstream file("tests/fixtures/spec182/native-merge-offers.json");
    const auto signedOffers = NativeJson::parse(file);
    NativeOfferAdmission admission(signedOffers.at("policy").dump(),
      {{signedOffers.at("key_id"), signedOffers.at("public_pem")}}, signedOffers.at("candidate"));
    const auto context = input.proposal().context;
    std::vector<NativeAdmittedOfferV3> offers;
    for (const auto& item : signedOffers.at("offers")) {
      const auto wire = item.get<std::string>();
      const auto offer = decodeNativeProviderOfferV3(wire);
      AckSelectionCandidate ack;
      ack.providerName = ndn::Name(offer.provider); ack.serviceName = ndn::Name(offer.service);
      ack.requestId = ndn::Name(offer.requestId); ack.ack.setStatus(offer.status);
      ndn::Buffer payload(wire.begin(), wire.end()); ack.ack.setPayload(payload, payload.size());
      // Core authentication evidence fixture; the signature is checked by production admission.
      ack.authenticationEvidence = {offer.provider, offer.provider + "/KEY/fixture/issuer/v=1",
        "sha256:" + std::string(64, '1'), true};
      offers.push_back(admission.verify(ack, context, 200));
    }
    const auto proposal = NativePreSplitFirstPlacement{}.proposeRoles(context,
      nativePlanningDigest("ack"), input.roles, offers, 200);
    auto publisher = NativeCanonicalPublisherTestAccess::create(io.transport(), input.options, input.resolver());
    NativeRequestPreparation preparation(std::make_shared<NativeAdapterRegistry>(), {}, publisher.artifactPort());
    const auto artifacts = preparation.ensureArtifacts(input.model, candidate, proposal, input.control);
    BOOST_REQUIRE_EQUAL(io.payloads.size(), external ? 3 : 2);
    NativePlanSealingInputs inputs;
    inputs.artifacts = artifacts; inputs.requesterIdentity = "/requester";
    inputs.protectionEpoch = merge.protectionEpoch; inputs.expiresAtMs = context.deadlineMs;
    for (const auto& role : input.roles) inputs.assemblyByRole.emplace(role.selectedRole, role);
    auto execution = candidate.executionPlan;
    execution.roles.clear();
    for (const auto& role : proposal.roles) execution.roles.push_back(role.selectedRole);
    execution.serviceName = context.serviceName; execution.modelName = input.model.descriptor.modelName;
    const auto core = NativePlanSealer::sealCore(input.model, candidate, proposal, execution,
      offers, proposal.ackClosedDigest, inputs);
    const auto& native = core.assemblyByRole.at(merge.role);
    BOOST_CHECK_EQUAL(native.modelManifestDigest, artifacts.manifestDigest);
    BOOST_CHECK_NE(native.modelManifestDigest, input.model.modelManifestDigest);
    BOOST_CHECK_EQUAL(native.recipeDigest, merge.recipeDigest);
    BOOST_CHECK(native.canonicalInitializerDigest.empty());
    BOOST_CHECK(native.assemblerDescriptorDigest.empty());
    BOOST_CHECK_EQUAL(native.maxSourceBytes, 0);
    BOOST_CHECK_NE(core.assemblyByRole.at("/role").recipeDigest, input.roles.back().recipeDigest);
    for (const auto& offer : offers)
      BOOST_CHECK_EQUAL(NativePlanSealer::grantView(core, offer,
        {nativePlanningDigest("policy"), true}).modelManifestDigest, artifacts.manifestDigest);
    // Grant transport is a fixture; both bindings retain the protected epoch.
    const auto sealed = NativePlanSealer::finalizeSecurity(core,
      {{"/provider/a", "/role", "/grant/a", nativePlanningDigest("grant-a"), "/provider/a"},
       {"/provider/b", "/Merge", "/grant/b", nativePlanningDigest("grant-b"), "/provider/b"}},
      {nativePlanningDigest("policy"), true});
    const auto projection = NativePlanSealer::project(sealed, "/provider/b",
      fixture::projection(sealed, "/provider/b"));
    const auto spec = nativeYoloMergeRunnerSpecFromProjection(projection);
    BOOST_CHECK(!validateNativePreparedRunnerSpec(projection, spec));
    BOOST_CHECK(spec.path.empty());
    BOOST_CHECK_NO_THROW(NativePlanSealer::encode(projection));
    auto poisoned = core;
    poisoned.assemblyByRole.at(merge.role).modelManifestDigest = input.model.modelManifestDigest;
    BOOST_CHECK_THROW(poisoned.validate(), std::invalid_argument);
    poisoned = core;
    poisoned.assemblyByRole.at(merge.role).postprocessOutputName = "other";
    BOOST_CHECK_THROW(poisoned.validate(), std::invalid_argument);

    for (unsigned poison = 0; poison != 4; ++poison) {
      auto roles = input.roles;
      if (poison == 0) roles.front().modelManifestDigest = nativePlanningDigest("foreign");
      if (poison == 1) roles.front().mergeKind.clear();
      if (poison == 2) roles.front().expectedOutputs.clear();
      if (poison == 3) roles.front().graphDigest = nativePlanningDigest("foreign");
      const auto published = io.payloads.size();
      BOOST_CHECK_THROW(publisher(input.model, candidate, roles, input.control), std::exception);
      BOOST_CHECK_EQUAL(io.payloads.size(), published);
    }
    // Without an ONNX role, publication has no certified source resource bound.
    auto solo = candidate;
    solo.executionPlan.roles = {merge.role}; solo.inputIngressRole = merge.role;
    solo.artifactsByRole.erase("/role"); solo.rankArtifactDigestsByRole.erase("/role");
    solo.tensorDegreesByRole.erase("/role"); solo.fragmentsByRole.erase("/role");
    solo.requirementsByRole.erase("/role"); solo.nodeRoles["n0"] = merge.role;
    solo.candidateDigest = solo.computedDigest();
    BOOST_CHECK_THROW(publisher(input.model, solo, {merge}, input.control), std::invalid_argument);
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
    work();
    const auto releaseDeadline = Clock::now() + std::chrono::seconds(2);
    while (!source.expired() && Clock::now() < releaseDeadline)
      std::this_thread::yield();
    BOOST_CHECK(source.expired());
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
  std::atomic<bool> cancelled{false};
  std::atomic<bool> ownerReleased{false};
  std::atomic<bool> rollbackObserved{false};
  input.control.cancelled = [&] { return cancelled.load(std::memory_order_acquire); };
  auto transport = io.transport();
  auto publish = transport.publish;
  transport.publish = [&](const PreparedServiceRequest& request, const std::vector<std::uint8_t>& bytes,
                          const std::string& label, const ndn_service_framework::PublishedContentName& contentName,
                          const NativeRequestControl& control) {
    auto result = publish(request, bytes, label, contentName, control);
    if (label == "di-canonical-source") {
      cancelled.store(true, std::memory_order_release);
      const auto until = Clock::now() + std::chrono::seconds(2);
      while (!ownerReleased.load(std::memory_order_acquire) && Clock::now() < until)
        std::this_thread::yield();
    }
    return result;
  };
  auto abort = transport.abort;
  transport.abort = [&](const auto& publications) {
    abort(publications);
    rollbackObserved.store(true, std::memory_order_release);
  };
  auto publisher = NativeCanonicalPublisherTestAccess::create(transport, input.options,
    [&](const auto&, const auto&) { return std::exchange(input.source, {}); });
  auto pending = std::async(std::launch::async, [&] {
    return publisher(input.model, input.candidate, input.roles, input.control);
  });
  BOOST_REQUIRE(pending.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  BOOST_CHECK_THROW(pending.get(), std::runtime_error);
  ownerReleased.store(true, std::memory_order_release);
  const auto until = Clock::now() + std::chrono::seconds(2);
  while (!rollbackObserved.load(std::memory_order_acquire) && Clock::now() < until)
    std::this_thread::yield();
  BOOST_REQUIRE(rollbackObserved.load(std::memory_order_acquire));
  BOOST_CHECK_EQUAL(io.payloads.size(), 1);
  BOOST_CHECK_EQUAL(io.abortCalls, 1U);
  BOOST_CHECK_EQUAL(io.abortedPublications, 1U);
}

BOOST_AUTO_TEST_CASE(BootstrapCannotWaitPastDeadlineWhenCoreIoDoesNotProgress)
{
  using namespace ndn_service_framework::test;
  Input input;
  ndn::security::KeyChain keyChain{"pib-memory:", "tpm-memory:"};
  ndn::DummyClientFace face{keyChain};
  auto cert = makeRsaIdentity(keyChain, ndn::Name("/publisher-test/user"));
  auto aa = makeRsaIdentity(keyChain, ndn::Name("/publisher-test/aa"));
  IsolatedTestTrust trust{cert, aa};
  auto user = std::make_shared<LocalServiceUser>(
    face, ndn::Name("/publisher-test"), cert, aa, trust.schemaPath());
  user->useSigningKeyChainForTest(keyChain);
  user->prepareHybridSendKeyForTest(ndn::Name("/service"), "REQUEST-LARGE");
  NativeCanonicalArtifactPublisher publisher(user, "/service", input.options, input.resolver());
  input.control.deadline = Clock::now() + std::chrono::milliseconds(50);
  auto pending = std::async(std::launch::async, [&] {
    return publisher.prepare(input.model, input.control);
  });
  // Leave the posted Core readiness query queued until the preparation owner exits.
  BOOST_REQUIRE(pending.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  BOOST_CHECK_THROW(pending.get(), std::runtime_error);
  face.getIoContext().restart();
  face.getIoContext().poll();
  BOOST_CHECK_EQUAL(user->getLargeDataServingMetricsForTest().publicationCount, 0U);
}

BOOST_AUTO_TEST_CASE(UsesRealCoreIoAndEncryptedPublicationWithLocalMockKey)
{
  using namespace ndn_service_framework::test;
  Input input;
  ndn::security::KeyChain keyChain{"pib-memory:", "tpm-memory:"};
  ndn::DummyClientFace face{keyChain};
  auto cert = makeRsaIdentity(keyChain, ndn::Name("/publisher-test/user"));
  auto aa = makeRsaIdentity(keyChain, ndn::Name("/publisher-test/aa"));
  IsolatedTestTrust trust{cert, aa};
  auto user = std::make_shared<LocalServiceUser>(
    face, ndn::Name("/publisher-test"), cert, aa, trust.schemaPath());
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
  // Worker publication is owner-bound and therefore uses Core's file-backed
  // serving path; the encrypted body is not retained in the IMS.  The
  // publication receipt itself remains the observable ownership contract.
  BOOST_CHECK_EQUAL(user->getLargeDataServingMetricsForTest().publicationCount, 2U);
  BOOST_CHECK_NE(result.manifestDigest, input.model.modelManifestDigest);
  // prepare uses the production worker entry rather than running the model
  // hash/encryption/store inside the posted Face callback.
  input.control.deadline = Clock::now() + std::chrono::seconds(10);
  auto prepared = std::async(std::launch::async, [&] {
    return publisher.prepare(input.model, input.control);
  });
  while (prepared.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
    face.getIoContext().restart(); face.getIoContext().poll();
  }
  const auto receipt = prepared.get();
  BOOST_CHECK(!receipt.servingLeases.empty());
  BOOST_CHECK(!receipt.rootDataName.empty());
  Input materialInput;
  NativeAssemblyControl materialControl{materialInput.control.deadline,
    [&] { materialInput.control.requireActive(); }, 1U << 20, 1U << 20};
  const auto sourceIdentity = canonicalOnnxSourceIdentity(*materialInput.source, materialControl);
  materialInput.model.canonicalGraphDigest = sourceIdentity.graphDigest;
  materialInput.source->materialManifest = deriveNativeCanonicalMaterialManifest(
    *materialInput.source, materialControl);
  NativeCanonicalArtifactPublisher materialPublisher(
    user, "/service", materialInput.options, materialInput.resolver());
  auto materialPending = std::async(std::launch::async, [&] {
    return materialPublisher.prepare(materialInput.model, materialInput.control);
  });
  while (materialPending.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
    face.getIoContext().restart(); face.getIoContext().poll();
  }
  const auto materialReceipt = materialPending.get();
  BOOST_REQUIRE(!materialReceipt.materialReceiptDataName.empty());
  const ndn::Name receiptName(materialReceipt.materialReceiptDataName);
  const auto expectedReceipt = ndn::Name("/publisher-test/user").append("NDNSF-DI")
    .append("receipt").append("assembly")
    .append(ndn::name::Component("name=assembly-receipt-" + materialInput.model.descriptor.modelName));
  BOOST_CHECK(receiptName.getPrefix(-1) == expectedReceipt);
  BOOST_CHECK_EQUAL(receiptName.get(-1).type(), 8U);
  BOOST_CHECK_EQUAL(receiptName.get(-1).value_size(), 32U);
}
BOOST_AUTO_TEST_SUITE_END()
