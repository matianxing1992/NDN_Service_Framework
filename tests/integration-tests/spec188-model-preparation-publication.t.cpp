#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalArtifactPublisher.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/name.hpp>

#include <boost/test/unit_test.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>

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
  std::vector<std::uint8_t> result;
  result.reserve(text.size() / 2);
  for (std::size_t i = 0; i < text.size(); i += 2)
    result.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
  return result;
}

struct Fixture
{
  std::shared_ptr<NativeCanonicalSource> source = std::make_shared<NativeCanonicalSource>();
  NativeInspectedModel model;
  NativeSplitCandidate candidate;
  std::vector<NativeSelectionRoleV3> roles;
  NativeCanonicalPublicationOptions options;
  NativeRequestControl control{"/spec188/prepare", 1, Clock::now() + std::chrono::seconds(30), {}};

  Fixture()
  {
    std::ifstream stream;
    const std::filesystem::path relative = "tests/fixtures/spec182/dependency-probes/extraction-vectors.json";
    for (auto directory = std::filesystem::current_path(); !directory.empty(); directory = directory.parent_path()) {
      const auto candidate = directory / relative;
      stream.open(candidate);
      if (stream.good()) break;
      stream.clear();
      if (directory == directory.parent_path()) break;
    }
    if (!stream) throw std::runtime_error("missing frozen ONNX source fixture");
    const auto vectors = NativeJson::parse(stream);
    const auto& row = vectors.at("cases").at(0);
    source->modelBytes = unhex(row.at("modelHex"));
    const auto recipe = row.at("recipe");
    const auto check = validateNativeOnnxWorkerMetadata(NativeJson{
      {"schema", kNativeOnnxAssemblyRequestSchema}, {"recipe", recipe},
      {"recipeDigest", nativePlanningDigest(recipe.dump())},
      {"backend", "onnxruntime-cpu"}, {"adapterId", "fixture"}}.dump());
    if (!check.ok) throw std::runtime_error(check.failureMessage);
    auto role = check.value.recipe;
    role.role = role.selectedRole = "/role";
    role.adapterVersion = "1";
    role.artifactDigest = row.at("expectedModelDigest");
    role.recipeDigest = check.value.recipeDigest;
    role.requiredDeviceMemoryMb = 1;
    role.protectionEpoch = "artifact-policy-epoch";
    roles = {role};
    model.descriptor = fixture::completeModel({"fixture-model", nativePlanningDigest("model"),
      nativePlanningDigest("semantics"), nativePlanningDigest("planning"), "onnx", "fp32",
      "fixture", "1"});
    model.graph.graphDigest = model.descriptor.graphDigest;
    model.graph.nodes = {{"n0", "Identity", 0}, {"n1", "Identity", 1}};
    model.graph.topologicalOrder = {"n0", "n1"};
    model.canonicalSourceName = "/fixture/source";
    model.canonicalSourceDigest = nativePlanningDigest(source->modelBytes.data(), source->modelBytes.size());
    model.canonicalSourceBytes = source->modelBytes.size();
    model.modelManifestDigest = role.modelManifestDigest;
    model.canonicalGraphDigest = role.graphDigest;
    candidate.model = model.descriptor;
    candidate.graphDigest = model.graph.graphDigest;
    candidate.splitter = {"fixture", "1", nativePlanningDigest("splitter")};
    candidate.source = "PRE_SPLIT";
    candidate.executionPlan.roles = {role.role};
    candidate.tensorDegreesByRole = {{role.role, 1}};
    for (const auto& node : model.graph.nodes)
      candidate.nodeRoles[node.id] = role.role;
    candidate.artifactsByRole = {{role.role, {role.artifactDigest}}};
    candidate.rankArtifactDigestsByRole = candidate.artifactsByRole;
    candidate.fragmentsByRole = {{role.role, role.artifactDigest}};
    candidate.requirementsByRole = {{role.role, {{"onnxruntime"}, 1, 0, 0, 0, 0, 1.0}}};
    candidate.candidateDigest = candidate.computedDigest();
    options = {"/fixture/NDNSF/DI/ARTIFACT", model.modelManifestDigest, {role.artifactDigest},
               role.artifactProfileDigest};
  }

  NativeCanonicalArtifactPublisher::SourcePort resolver() const
  { return [value = source](const auto&, const auto&) { return value; }; }
};

struct AsyncTransport
{
  std::mutex mutex;
  std::vector<std::thread> workers;
  std::vector<std::string> labels;
  std::vector<std::string> aborted;
  std::string failLabel;
  std::chrono::milliseconds delay{0};
  std::atomic<bool> block{false};
  std::atomic<bool> entered{false};
  std::promise<void> unblock;
  std::shared_future<void> gate;

  AsyncTransport()
    : gate(unblock.get_future().share())
  {
  }

  void release() noexcept
  {
    try {
      unblock.set_value();
    }
    catch (const std::future_error&) {
    }
  }

  NativeCanonicalPublisherTestAccess::Transport make()
  {
    return {
      [this](auto work) {
        std::lock_guard<std::mutex> lock(mutex);
        workers.emplace_back([work = std::move(work)] { work(); });
      },
      [] { return false; },
      [] { return PreparedServiceRequest{ndn::Name("/service"), ndn::Name("/publication-request")}; },
      [this](const PreparedServiceRequest&, const std::vector<std::uint8_t>& bytes,
             const std::string& label, const NativeRequestControl&) {
        if (block.load(std::memory_order_acquire)) {
          entered.store(true, std::memory_order_release);
          gate.wait();
        }
        if (delay.count() != 0) std::this_thread::sleep_for(delay);
        {
          std::lock_guard<std::mutex> lock(mutex);
          labels.push_back(label);
        }
      LargeDataPublishResult result;
        if (!failLabel.empty() && label == failLabel) {
          result.errorMessage = "injected publication failure";
          return result;
        }
        result.success = true;
        result.encrypted = true;
        result.objectId = label;
        result.encryptedDataName = ndn::Name("/encrypted").append(label).appendVersion(1);
        result.plaintextSize = bytes.size();
        result.contentDigest = nativePlanningDigest(bytes.data(), bytes.size());
        result.manifestDigest = nativePlanningDigest("Core transport metadata");
        result.authorizationScope = "/SERVICE/service";
        result.protectionEpoch = "Core epoch";
        return result;
      },
      [this](const std::vector<LargeDataPublishResult>& publications) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& publication : publications)
          aborted.push_back(publication.encryptedDataName.toUri());
      }};
  }

  ~AsyncTransport()
  {
    release();
    for (auto& worker : workers)
      if (worker.joinable()) worker.join();
  }
};

struct GateRelease
{
  AsyncTransport& transport;
  ~GateRelease() { transport.release(); }
};
}

BOOST_AUTO_TEST_SUITE(Spec188PreparationPublication)

BOOST_AUTO_TEST_CASE(PrepareReceiptBindsRequestsWithoutRepublishing)
{
  Fixture fixture;
  AsyncTransport io;
  auto publisher = NativeCanonicalPublisherTestAccess::create(
    io.make(), fixture.options, fixture.resolver());

  const auto receipt = publisher.prepare(fixture.model, fixture.control);
  BOOST_CHECK(!receipt.sourceDataName.empty());
  BOOST_CHECK(receipt.initializerDataName.empty());
  BOOST_CHECK(!receipt.rootDataName.empty());
  BOOST_CHECK_EQUAL(publisher.stats().publicationCalls, 2U);
  BOOST_CHECK_EQUAL(io.labels.size(), 2U);
  BOOST_CHECK_EQUAL(io.labels.at(0), "di-canonical-source");
  BOOST_CHECK_EQUAL(io.labels.at(1), "di-canonical-root");

  const auto first = publisher.bindPrepared(
    fixture.model, fixture.candidate, fixture.roles, receipt, fixture.control);
  BOOST_REQUIRE_EQUAL(first.sourceByRole.size(), 1U);
  BOOST_CHECK_EQUAL(first.sourceByRole.at("/role"), receipt.rootDataName);
  BOOST_CHECK_EQUAL(publisher.stats().publicationCalls, 2U);

  auto replacement = fixture.candidate;
  replacement.selectionPriority = 1;
  replacement.candidateDigest = replacement.computedDigest();
  const auto second = publisher.bindPrepared(
    fixture.model, replacement, fixture.roles, receipt, fixture.control);
  BOOST_CHECK_NE(second.artifactNameByRole.at("/role"),
                 first.artifactNameByRole.at("/role"));
  BOOST_CHECK_EQUAL(publisher.stats().publicationCalls, 2U);

  auto mismatched = receipt;
  mismatched.manifestDigest = nativePlanningDigest("foreign receipt");
  BOOST_CHECK_THROW(publisher.bindPrepared(
    fixture.model, fixture.candidate, fixture.roles, mismatched, fixture.control),
    std::exception);
}

BOOST_AUTO_TEST_CASE(PrepareRollsBackPartialPublication)
{
  Fixture fixture;
  AsyncTransport io;
  io.failLabel = "di-canonical-root";
  auto publisher = NativeCanonicalPublisherTestAccess::create(
    io.make(), fixture.options, fixture.resolver());

  BOOST_CHECK_THROW(publisher.prepare(fixture.model, fixture.control), std::exception);
  BOOST_REQUIRE_EQUAL(publisher.stats().publicationCalls, 2U);
  std::lock_guard<std::mutex> lock(io.mutex);
  BOOST_REQUIRE_EQUAL(io.aborted.size(), 1U);
  BOOST_CHECK(io.aborted.front().find("/encrypted/di-canonical-source") == 0);
}

BOOST_AUTO_TEST_CASE(CancelledPrepareOwnerRollsBackCompletedPublication)
{
  Fixture fixture;
  AsyncTransport io;
  io.block.store(true, std::memory_order_release);
  io.delay = std::chrono::milliseconds(50);
  std::atomic<bool> cancelled{false};
  fixture.control.cancelled = [&] { return cancelled.load(std::memory_order_acquire); };
  auto publisher = NativeCanonicalPublisherTestAccess::create(
    io.make(), fixture.options, fixture.resolver());
  auto owner = std::async(std::launch::async, [&] {
    return publisher.prepare(fixture.model, fixture.control);
  });
  for (int i = 0; i < 200 && !io.entered.load(std::memory_order_acquire); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  BOOST_REQUIRE(io.entered.load(std::memory_order_acquire));
  cancelled.store(true, std::memory_order_release);
  io.release();
  BOOST_CHECK_THROW(owner.get(), std::exception);
  for (int i = 0; i < 200; ++i) {
    {
      std::lock_guard<std::mutex> lock(io.mutex);
      if (!io.aborted.empty())
        break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::lock_guard<std::mutex> lock(io.mutex);
  BOOST_CHECK(!io.aborted.empty());
}

BOOST_AUTO_TEST_CASE(ColdHotAndConcurrentPublicationAreSingleFlight)
{
  Fixture fixture;
  AsyncTransport io;
  io.block.store(true, std::memory_order_release);
  auto publisher = NativeCanonicalPublisherTestAccess::create(io.make(), fixture.options, fixture.resolver());

  auto coldFuture = std::async(std::launch::async, [&] {
    return publisher(fixture.model, fixture.candidate, fixture.roles, fixture.control);
  });
  const GateRelease release{io};
  for (int i = 0; i < 200 && !io.entered.load(std::memory_order_acquire); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if (!io.entered.load(std::memory_order_acquire) &&
      coldFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
    try {
      (void)coldFuture.get();
      BOOST_FAIL("publication worker completed before transport without an error");
    }
    catch (const std::exception& error) {
      BOOST_FAIL(std::string("publication worker rejected fixture: ") + error.what());
    }
  }
  if (!io.entered.load(std::memory_order_acquire))
    io.release();
  BOOST_REQUIRE(io.entered.load(std::memory_order_acquire));
  std::vector<std::future<NativeArtifactBinding>> waiters;
  for (int i = 0; i < 8; ++i)
    waiters.emplace_back(std::async(std::launch::async, [&] {
      return publisher(fixture.model, fixture.candidate, fixture.roles, fixture.control);
    }));
  for (int i = 0; i < 200 && publisher.stats().sharedWaiters < 8; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if (publisher.stats().sharedWaiters < 8U)
    io.release();
  BOOST_REQUIRE_GE(publisher.stats().sharedWaiters, 8U);
  io.release();
  const auto cold = coldFuture.get();
  for (auto& waiter : waiters)
    BOOST_CHECK_EQUAL(waiter.get().manifestDigest, cold.manifestDigest);
  BOOST_CHECK(!cold.manifestDigest.empty());
  auto stats = publisher.stats();
  BOOST_CHECK_EQUAL(stats.sourceVerifications, 1U);
  BOOST_CHECK_EQUAL(stats.publicationCalls, 2U);

  const auto hot = publisher(fixture.model, fixture.candidate, fixture.roles, fixture.control);
  BOOST_CHECK_EQUAL(hot.manifestDigest, cold.manifestDigest);
  stats = publisher.stats();
  BOOST_CHECK_EQUAL(stats.sourceVerifications, 1U);
  BOOST_CHECK_EQUAL(stats.publicationCalls, 2U);
  BOOST_CHECK_EQUAL(stats.cacheHits, 1U);

  stats = publisher.stats();
  BOOST_CHECK_EQUAL(stats.publicationCalls, 2U);
  BOOST_CHECK(stats.sharedWaiters >= 8U);
  BOOST_CHECK_EQUAL(stats.cacheHits, 1U);

  auto replacement = fixture.candidate;
  replacement.selectionPriority = 1;
  replacement.candidateDigest = replacement.computedDigest();
  const auto replaced = publisher(fixture.model, replacement, fixture.roles, fixture.control);
  BOOST_CHECK_NE(replaced.artifactNameByRole.at("/role"), cold.artifactNameByRole.at("/role"));
  BOOST_CHECK_EQUAL(publisher.stats().publicationCalls, 4U);
}

BOOST_AUTO_TEST_CASE(CancelledWaiterDoesNotAbortSharedPublication)
{
  Fixture fixture;
  AsyncTransport io;
  io.block.store(true, std::memory_order_release);
  auto publisher = NativeCanonicalPublisherTestAccess::create(io.make(), fixture.options, fixture.resolver());
  auto owner = std::async(std::launch::async, [&] {
    return publisher(fixture.model, fixture.candidate, fixture.roles, fixture.control);
  });
  const GateRelease release{io};
  for (int i = 0; i < 200 && !io.entered.load(std::memory_order_acquire); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if (!io.entered.load(std::memory_order_acquire))
    io.release();
  BOOST_REQUIRE(io.entered.load(std::memory_order_acquire));
  auto cancelled = fixture.control;
  std::atomic<bool> waiterCancelled{false};
  cancelled.cancelled = [&] { return waiterCancelled.load(std::memory_order_acquire); };
  auto waiter = std::async(std::launch::async, [&] {
    return publisher(fixture.model, fixture.candidate, fixture.roles, cancelled);
  });
  for (int i = 0; i < 200 && publisher.stats().sharedWaiters < 1; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if (publisher.stats().sharedWaiters < 1U)
    io.release();
  BOOST_REQUIRE_GE(publisher.stats().sharedWaiters, 1U);
  waiterCancelled.store(true, std::memory_order_release);
  BOOST_CHECK_THROW(waiter.get(), std::runtime_error);
  io.release();
  BOOST_CHECK_NO_THROW(owner.get());
  BOOST_CHECK_EQUAL(publisher.stats().publicationCalls, 2U);
  BOOST_CHECK_EQUAL(publisher.stats().sourceVerifications, 1U);
  BOOST_CHECK(publisher.stats().sharedWaiters >= 1U);
}

BOOST_AUTO_TEST_SUITE_END()
