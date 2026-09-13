#include "NDNSF-DistributedInference/cpp/ndnsf-di/ModelPreparationCache.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ndnsf::di {

// Test-only access keeps the production cache owner private while allowing
// deterministic C++ fixtures to hold a job at its source barrier.
struct Spec185PreparationTestAccess
{
  static PreparationHandle prepareAsync(ModelPreparationCache& cache,
                                        const PreparationSpec& spec,
                                        CachePolicy policy = CachePolicy::UseOrFetch,
                                        std::chrono::milliseconds timeout = {})
  {
    return PreparationHandle(cache.prepareAsync(spec, policy, timeout));
  }
};

} // namespace ndnsf::di

namespace {

using namespace ndnsf::di;

struct Fixture
{
  NativeJson oracle;
  NativeCanonicalSource source;
  NativeModelDescriptor descriptor;
  NativeJson catalog;
  PreparationSpec spec;
  std::string canonicalGraphDigest;
  std::string planningGraphDigest;
  std::size_t fetches = 0;

  Fixture()
  {
    std::ifstream file("tests/fixtures/spec182/yolo-semantic-oracle.json");
    BOOST_REQUIRE(file.good());
    file >> oracle;
    const auto hex = oracle.at("model_hex").get<std::string>();
    for (std::size_t i = 0; i < hex.size(); i += 2)
      source.modelBytes.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    std::ifstream graphOracleFile("tests/fixtures/spec182/onnx-planning-graph-oracle.json");
    BOOST_REQUIRE(graphOracleFile.good());
    NativeJson graphOracles;
    graphOracleFile >> graphOracles;
    NativeJson graphOracle;
    for (const auto& item : graphOracles) {
      if (item.at("model_hex") == hex) {
        graphOracle = item;
        break;
      }
    }
    BOOST_REQUIRE(!graphOracle.is_null());
    canonicalGraphDigest = graphOracle.at("canonical_graph_digest").get<std::string>();
    const auto sourceDigest = nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size());
    const auto control = NativeAssemblyControl{
      std::chrono::steady_clock::now() + std::chrono::seconds(30), [] {}, 1 << 20, 1 << 20};
    const auto identity = canonicalOnnxSourceIdentity(source, control);
    descriptor = fixture::completeModel({"yolo26n", nativePlanningDigest("YOLOFixture-content"),
      nativePlanningDigest("YOLOFixture-semantics"), oracle.at("graph_digest"), "onnx", "float32", "YOLOFixture", "1"});
    // The descriptor carries the planning graph identity, while the catalog
    // source field below carries the canonical ONNX source graph identity.
    // Derive the former from the native inspector instead of conflating the
    // two digests in the fixture.
    const auto planning = inspectNativeOnnxSourceGraph(source, descriptor, control);
    descriptor.graphDigest = planning.graph.graphDigest;
    planningGraphDigest = planning.graph.graphDigest;
    BOOST_REQUIRE_EQUAL(identity.graphDigest, canonicalGraphDigest);
    catalog = NativeJson::object();
    catalog["schema"] = "ndnsf-di-native-request-catalog-v1";
    catalog["model"] = nativeParseJson(descriptor.canonicalJson());
    catalog["source"] = NativeJson::object();
    catalog["source"]["data_name"] = "/fixture/source";
    catalog["source"]["digest"] = sourceDigest;
    catalog["source"]["model_manifest_digest"] = nativePlanningDigest("fixture-manifest");
    catalog["source"]["canonical_graph_digest"] = identity.graphDigest;
    catalog["recipe"] = NativeJson::object();
    catalog["recipe"]["artifact_profile_digest"] = nativePlanningDigest("fixture-profile");
    catalog["recipe"]["assembler_descriptor_digest"] = nativePlanningDigest("fixture-assembler-v1");
    catalog["recipe"]["backend_abi"] = "fixture-abi";
    catalog["recipe"]["precision"] = descriptor.precision;
    catalog["recipe"]["quantization"] = "none";
    catalog["recipe"]["layout"] = "NCHW";
    catalog["recipe"]["padding"] = "none";
    catalog["recipe"]["protection_epoch"] = "fixture-epoch";
    catalog["recipe"]["max_source_bytes"] = 1 << 20;
    catalog["recipe"]["max_assembled_bytes"] = 1 << 20;
    catalog["recipe"]["max_nodes"] = 100;
    catalog["publication"] = NativeJson::object();
    catalog["publication"]["artifact_root"] = "/fixture/artifacts";
    catalog["input_format"] = "JSON";
    catalog["max_payload_bytes"] = 32;
    NativeJson component = NativeJson::object();
    component["candidate_id"] = "semantic-v1";
    component["priority"] = 1;
    component["roles"] = {"Front", "Branch", "Merge"};
    component["node_names_by_role"] = NativeJson::object();
    component["input_ingress_role"] = "Front";
    component["result_egress_role"] = "Merge";
    component["merge_kind"] = "NATIVE_POSTPROCESS";
    component["candidate_digest"] = oracle.at("registered_digest");
    component["semantic_partition"] = oracle.at("partition");
    catalog["splitter"] = NativeJson::object();
    catalog["splitter"]["kind"] = "YOLO";
    catalog["splitter"]["components"] = NativeJson::array({component});
    spec.key = "default";
    spec.baseDirectory = ".";
    spec.catalogConfigurationJson = nativeCanonicalJson(catalog);
    spec.taskName = "task";
    spec.taskContractDigest = nativePlanningDigest("task-contract");
    spec.inputLayoutDigest = nativePlanningDigest("input-layout");
    spec.maxSourceBytes = 1 << 20;
    spec.maxAssembledBytes = 1 << 20;
    const NativeJson runtime{
      {"schema", "ndnsf-di-native-requester-v1"},
      {"request", {{"task", spec.taskName}, {"task_descriptor_digest", spec.taskContractDigest},
        {"input_layout_digest", spec.inputLayoutDigest}, {"generation_mode", "TOKEN_DIAGNOSTIC"}}},
      {"catalog", catalog},
      {"limits", {{"max_source_bytes", spec.maxSourceBytes}, {"max_assembled_bytes", spec.maxAssembledBytes}}}};
    spec.configurationJson = nativeCanonicalJson(runtime);
    spec.configurationDigest = nativePlanningDigest(spec.configurationJson);
    spec.loadSource = [this] (const PreparationSpec&, std::chrono::steady_clock::time_point deadline) {
      if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("fixture deadline");
      ++fetches;
      return source;
    };
  }

  PreparationSpec withCatalog(NativeJson value) const
  {
    auto result = spec;
    result.catalogConfigurationJson = nativeCanonicalJson(value);
    auto runtime = nativeParseJson(result.configurationJson);
    runtime["catalog"] = value;
    result.configurationJson = nativeCanonicalJson(runtime);
    result.configurationDigest = nativePlanningDigest(result.configurationJson);
    return result;
  }
};

} // namespace

BOOST_AUTO_TEST_SUITE(Spec185Preparation)

BOOST_AUTO_TEST_CASE(ColdPreparationPublishesOnlyVerifiedImmutablePackage)
{
  Fixture fixture;
  ModelPreparationCache cache(4 << 20, 2, std::chrono::seconds(5));
  auto prepared = cache.prepare(fixture.spec);
  BOOST_CHECK_EQUAL(prepared.manifest().modelName, "yolo26n");
  BOOST_CHECK_EQUAL(prepared.manifest().taskName, "task");
  BOOST_CHECK_EQUAL(prepared.manifest().canonicalGraphDigest, fixture.canonicalGraphDigest);
  BOOST_CHECK_EQUAL(prepared.manifest().planningGraphDigest, fixture.planningGraphDigest);
  BOOST_CHECK(prepared.receipt().origin == PreparationReceipt::Origin::Fetched);
  BOOST_CHECK_EQUAL(prepared.receipt().manifestDigest,
                    fixture.catalog.at("source").at("model_manifest_digest"));
  BOOST_CHECK_EQUAL(cache.parseCount(), 1U);
  BOOST_CHECK_EQUAL(cache.entryCount(), 1U);
  BOOST_CHECK_EQUAL(fixture.fetches, 1U);
  BOOST_CHECK(prepared.capabilities().inputKinds == std::vector<std::string>{"BYTES"});
  BOOST_CHECK(!prepared.capabilities().conversations);

  // A hit returns the same verified package without reading or parsing source.
  auto hit = cache.prepare(fixture.spec);
  BOOST_CHECK(hit.receipt().origin == PreparationReceipt::Origin::CacheHit);
  BOOST_CHECK_EQUAL(hit.manifest().preparationKeyDigest,
                    prepared.manifest().preparationKeyDigest);
  BOOST_CHECK_EQUAL(cache.parseCount(), 1U);
  BOOST_CHECK_EQUAL(fixture.fetches, 1U);
}

BOOST_AUTO_TEST_CASE(PreparationKeyIgnoresLocalSourceLocator)
{
  Fixture fixture;
  ModelPreparationCache cache(4 << 20, 2, std::chrono::seconds(5));
  auto firstCatalog = fixture.catalog;
  firstCatalog["source"]["file"] = "models/first/model.onnx";
  auto secondCatalog = fixture.catalog;
  secondCatalog["source"]["file"] = "models/second/model.onnx";
  auto first = fixture.withCatalog(firstCatalog);
  auto second = fixture.withCatalog(secondCatalog);

  auto prepared = cache.prepare(first);
  auto hit = cache.prepare(second);
  BOOST_CHECK(hit.receipt().origin == PreparationReceipt::Origin::CacheHit);
  BOOST_CHECK_EQUAL(hit.manifest().preparationKeyDigest,
                    prepared.manifest().preparationKeyDigest);
  BOOST_CHECK_EQUAL(cache.parseCount(), 1U);
  BOOST_CHECK_EQUAL(fixture.fetches, 1U);
}

BOOST_AUTO_TEST_CASE(PreparationRejectsDetachedCatalogAndHonoursCancellation)
{
  Fixture fixture;
  ModelPreparationCache cache(4 << 20, 2, std::chrono::seconds(5));

  auto detached = fixture.spec;
  auto foreignCatalog = fixture.catalog;
  foreignCatalog["source"]["data_name"] = "/foreign/source";
  detached.catalogConfigurationJson = nativeCanonicalJson(foreignCatalog);
  BOOST_CHECK_THROW(cache.prepare(detached), std::exception);
  BOOST_CHECK_EQUAL(fixture.fetches, 0U);

  auto cancelled = fixture.spec;
  cancelled.cancelled = [] { return true; };
  BOOST_CHECK_EXCEPTION(cache.prepare(cancelled), std::runtime_error,
                        [] (const std::runtime_error& error) {
                          return std::string(error.what()).find("CANCELLED") != std::string::npos;
                        });
  BOOST_CHECK_EQUAL(fixture.fetches, 0U);
}

BOOST_AUTO_TEST_CASE(PreparationCancellationRollsBackFetchReservation)
{
  Fixture fixture;
  ModelPreparationCache cache(4 << 20, 2, std::chrono::seconds(5));
  auto lateCancelled = fixture.spec;
  lateCancelled.cancelled = [&fixture] { return fixture.fetches != 0; };
  BOOST_CHECK_EXCEPTION(cache.prepare(lateCancelled), std::runtime_error,
                        [] (const std::runtime_error& error) {
                          return std::string(error.what()).find("CANCELLED") != std::string::npos;
                        });
  BOOST_CHECK_EQUAL(cache.entryCount(), 0U);
  BOOST_CHECK_EQUAL(cache.chargedBytes(), 0U);

  auto prepared = cache.prepare(fixture.spec);
  BOOST_CHECK_EQUAL(cache.entryCount(), 1U);
  BOOST_CHECK(!prepared.manifest().modelName.empty());
}

BOOST_AUTO_TEST_CASE(PreparationRejectsWrongIdentityAndNonOnnxSource)
{
  Fixture fixture;
  ModelPreparationCache cache(4 << 20, 4, std::chrono::seconds(5));

  auto wrongGraph = fixture.catalog;
  wrongGraph["source"]["canonical_graph_digest"] = nativePlanningDigest("foreign-graph");
  BOOST_CHECK_THROW(cache.prepare(fixture.withCatalog(wrongGraph)), std::exception);
  BOOST_CHECK_EQUAL(cache.entryCount(), 0U);

  auto wrongTask = fixture.spec;
  wrongTask.taskName = "foreign-task";
  BOOST_CHECK_THROW(cache.prepare(wrongTask), std::exception);
  BOOST_CHECK_EQUAL(cache.entryCount(), 0U);

  auto nonOnnx = fixture;
  nonOnnx.source.modelBytes = {'{', '}', '\n'};
  nonOnnx.spec.loadSource = [&nonOnnx] (const PreparationSpec&, std::chrono::steady_clock::time_point) {
    return nonOnnx.source;
  };
  auto nonOnnxCatalog = nonOnnx.catalog;
  nonOnnxCatalog["source"]["digest"] = nativePlanningDigest(nonOnnx.source.modelBytes.data(),
                                                               nonOnnx.source.modelBytes.size());
  BOOST_CHECK_THROW(cache.prepare(nonOnnx.withCatalog(nonOnnxCatalog)), std::exception);
  BOOST_CHECK_EQUAL(cache.entryCount(), 0U);

  auto unsupported = fixture.spec;
  unsupported.taskName = "other-task";
  auto unsupportedRuntime = nativeParseJson(unsupported.configurationJson);
  unsupportedRuntime["request"]["task"] = unsupported.taskName;
  unsupported.configurationJson = nativeCanonicalJson(unsupportedRuntime);
  unsupported.configurationDigest = nativePlanningDigest(unsupported.configurationJson);
  BOOST_CHECK_EXCEPTION(cache.prepare(unsupported), std::runtime_error,
                        [] (const std::runtime_error& error) {
                          return std::string(error.what()).find("UNSUPPORTED_CAPABILITY") != std::string::npos;
                        });
  BOOST_CHECK_EQUAL(cache.entryCount(), 0U);
}

BOOST_AUTO_TEST_CASE(PreparationRejectsMissingInitializerAndBudgetBeforePublication)
{
  Fixture fixture;
  ModelPreparationCache cache(64, 1, std::chrono::seconds(5));
  BOOST_CHECK_THROW(cache.prepare(fixture.spec), std::exception);
  BOOST_CHECK_EQUAL(cache.entryCount(), 0U);

  Fixture missingInitializer;
  auto catalog = missingInitializer.catalog;
  catalog["source"]["initializer_digest"] = nativePlanningDigest("missing-initializer");
  BOOST_CHECK_THROW(cache.prepare(missingInitializer.withCatalog(catalog)), std::exception);
  BOOST_CHECK_EQUAL(cache.entryCount(), 0U);
}

BOOST_AUTO_TEST_CASE(RequireReadyAndUseOrWaitNeverPerformAnImplicitFetch)
{
  Fixture fixture;
  ModelPreparationCache cache(4 << 20, 1, std::chrono::seconds(5));
  BOOST_CHECK_THROW(cache.prepare(fixture.spec, CachePolicy::RequireReady), std::exception);
  BOOST_CHECK_EXCEPTION(cache.prepare(fixture.spec, CachePolicy::UseOrWait), std::runtime_error,
                        [] (const std::runtime_error& error) {
                          return std::string(error.what()).find("PREPARATION_NOT_IN_FLIGHT") != std::string::npos;
                        });
  BOOST_CHECK_EQUAL(fixture.fetches, 0U);
  BOOST_CHECK_EQUAL(cache.parseCount(), 0U);
}

BOOST_AUTO_TEST_CASE(RefreshKeepsOldLeaseBytesUntilTheOldObjectIsReleased)
{
  Fixture fixture;
  // Refresh reserves model and optional initializer staging while retaining
  // the old leased package until commit; allow both generations in this
  // focused budget test.
  ModelPreparationCache cache(8 << 20, 2, std::chrono::seconds(5));
  std::size_t refreshedCharge = 0;
  {
    auto prepared = cache.prepare(fixture.spec);
    auto refreshed = cache.prepare(fixture.spec, CachePolicy::Refresh);
    BOOST_CHECK(refreshed.receipt().origin == PreparationReceipt::Origin::Refreshed);
    refreshedCharge = cache.chargedBytes();
    BOOST_CHECK(refreshedCharge > refreshed.manifest().modelName.size());
    BOOST_CHECK_EQUAL(prepared.manifest().modelName, "yolo26n");
  }
  BOOST_CHECK(cache.chargedBytes() < refreshedCharge);
  BOOST_CHECK_EQUAL(cache.entryCount(), 1U);
}

BOOST_AUTO_TEST_CASE(EightConcurrentPreparationsUseOneFetchAndInspection)
{
  Fixture fixture;
  ModelPreparationCache cache(16 << 20, 2, std::chrono::seconds(5));
  std::atomic<unsigned> loads{0};
  std::mutex gateMutex;
  std::condition_variable gate;
  bool release = false;
  auto makeSpec = [&] {
    auto spec = fixture.spec;
    spec.loadSource = [&] (const PreparationSpec&, std::chrono::steady_clock::time_point) {
      const auto count = loads.fetch_add(1, std::memory_order_acq_rel) + 1;
      if (count == 1)
        gate.notify_all();
      std::unique_lock<std::mutex> lock(gateMutex);
      gate.wait(lock, [&] { return release; });
      return fixture.source;
    };
    return spec;
  };
  std::vector<std::thread> threads;
  std::atomic<unsigned> successes{0};
  std::vector<std::exception_ptr> errors(8);
  for (unsigned i = 0; i < 8; ++i) {
    threads.emplace_back([&, i, spec = makeSpec()] {
      try {
        (void)cache.prepare(spec);
        successes.fetch_add(1, std::memory_order_relaxed);
      }
      catch (...) { errors[i] = std::current_exception(); }
    });
  }
  bool bothLoaded = false;
  {
    std::unique_lock<std::mutex> lock(gateMutex);
    bothLoaded = gate.wait_for(lock, std::chrono::seconds(2), [&] {
      return loads.load(std::memory_order_acquire) >= 1;
    });
    release = true;
  }
  gate.notify_all();
  for (auto& thread : threads)
    thread.join();
  BOOST_REQUIRE(bothLoaded);
  BOOST_CHECK_EQUAL(successes.load(std::memory_order_relaxed), 8U);
  for (const auto& error : errors)
    BOOST_CHECK(!error);
  BOOST_CHECK_EQUAL(cache.entryCount(), 1U);
  BOOST_CHECK_EQUAL(cache.parseCount(), 1U);
  BOOST_CHECK_EQUAL(fixture.fetches, 0U);
}

BOOST_AUTO_TEST_SUITE(Spec185PreparationT004)

BOOST_AUTO_TEST_CASE(RefreshFailureRetainsThePreviouslyPublishedReadyPackage)
{
  Fixture fixture;
  ModelPreparationCache cache(8 << 20, 2, std::chrono::seconds(5));
  auto prepared = cache.prepare(fixture.spec);
  auto failing = fixture.spec;
  failing.loadSource = [] (const PreparationSpec&, std::chrono::steady_clock::time_point) {
    throw std::runtime_error("refresh source failed");
    return NativeCanonicalSource{};
  };
  BOOST_CHECK_THROW(cache.prepare(failing, CachePolicy::Refresh), std::exception);
  auto ready = cache.prepare(fixture.spec, CachePolicy::RequireReady);
  BOOST_CHECK(ready.receipt().origin == PreparationReceipt::Origin::CacheHit);
  BOOST_CHECK_EQUAL(ready.manifest().preparationKeyDigest,
                    prepared.manifest().preparationKeyDigest);
}

BOOST_AUTO_TEST_CASE(PinnedEntryBlocksEvictionUntilItsLeaseIsReleased)
{
  Fixture fixture;
  auto otherCatalog = fixture.catalog;
  otherCatalog["source"]["data_name"] = "/fixture/other-source";
  auto other = fixture.withCatalog(otherCatalog);
  ModelPreparationCache cache(8 << 20, 1, std::chrono::seconds(5));
  {
    auto pinned = cache.prepare(fixture.spec);
    BOOST_CHECK_EXCEPTION(cache.prepare(other), std::runtime_error,
                          [] (const std::runtime_error& error) {
                            return std::string(error.what()).find("BUDGET_EXCEEDED") != std::string::npos;
                          });
  }
  auto replacement = cache.prepare(other);
  BOOST_CHECK_EQUAL(cache.entryCount(), 1U);
  BOOST_CHECK(!replacement.manifest().preparationKeyDigest.empty());
}

BOOST_AUTO_TEST_CASE(AsyncWaiterTimeoutUsesASeparateGateAndAllowsReentrantCancel)
{
  Fixture fixture;
  ModelPreparationCache cache(8 << 20, 2, std::chrono::seconds(5));
  std::mutex sourceMutex;
  std::condition_variable sourceCondition;
  bool releaseSource = false;
  std::function<void()> fireTimer;
  auto spec = fixture.spec;
  spec.loadSource = [&] (const PreparationSpec&, std::chrono::steady_clock::time_point) {
    std::unique_lock<std::mutex> lock(sourceMutex);
    sourceCondition.wait(lock, [&] { return releaseSource; });
    return fixture.source;
  };
  spec.schedule = [&fireTimer] (std::chrono::steady_clock::time_point,
                                std::function<void()> task) {
    fireTimer = std::move(task);
    return [&fireTimer] { fireTimer = {}; };
  };
  auto handle = Spec185PreparationTestAccess::prepareAsync(cache, spec);
  std::promise<std::string> timeoutCode;
  auto future = timeoutCode.get_future();
  auto subscription = handle.resultAsync(std::chrono::milliseconds(1),
    [&] (std::exception_ptr error, std::optional<PreparedModel>) {
      std::string message = "NONE";
      try {
        if (error)
          std::rethrow_exception(error);
      }
      catch (const std::exception& value) { message = value.what(); }
      timeoutCode.set_value(message);
      // This callback re-enters the owning handle; the timer must not hold
      // the job commit mutex while invoking it.
      handle.cancel();
    });
  BOOST_REQUIRE(static_cast<bool>(fireTimer));
  auto trigger = fireTimer;
  trigger();
  BOOST_REQUIRE(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK_EQUAL(future.get(), "DI_NATIVE_PREPARATION_RESULT_TIMEOUT");
  subscription.cancel();
  {
    std::lock_guard<std::mutex> lock(sourceMutex);
    releaseSource = true;
  }
  sourceCondition.notify_all();
}

BOOST_AUTO_TEST_CASE(AsyncCompletionCapacityIsExactlyBoundedAtSixtyFour)
{
  Fixture fixture;
  ModelPreparationCache cache(8 << 20, 2, std::chrono::seconds(5));
  std::mutex sourceMutex;
  std::condition_variable sourceCondition;
  bool releaseSource = false;
  auto spec = fixture.spec;
  spec.loadSource = [&] (const PreparationSpec&, std::chrono::steady_clock::time_point) {
    std::unique_lock<std::mutex> lock(sourceMutex);
    sourceCondition.wait(lock, [&] { return releaseSource; });
    return fixture.source;
  };
  auto handle = Spec185PreparationTestAccess::prepareAsync(cache, spec);
  std::vector<Subscription> subscriptions;
  subscriptions.reserve(64);
  for (unsigned i = 0; i < 64; ++i)
    subscriptions.emplace_back(handle.onCompletion(
      [] (std::exception_ptr, std::optional<PreparedModel>) {}));
  BOOST_CHECK_EXCEPTION(handle.onCompletion(
      [] (std::exception_ptr, std::optional<PreparedModel>) {}), std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find("SUBSCRIPTION_LIMIT") != std::string::npos;
    });
  for (auto& subscription : subscriptions)
    subscription.cancel();
  handle.cancel();
  {
    std::lock_guard<std::mutex> lock(sourceMutex);
    releaseSource = true;
  }
  sourceCondition.notify_all();
}

BOOST_AUTO_TEST_CASE(DispatchQueueThenThrowStillDeliversExactlyOnce)
{
  Fixture fixture;
  ModelPreparationCache cache(8 << 20, 2, std::chrono::seconds(5));
  std::mutex sourceMutex;
  std::condition_variable sourceCondition;
  bool releaseSource = false;
  std::mutex dispatchMutex;
  std::condition_variable dispatchCondition;
  std::vector<std::function<void()>> queued;
  auto spec = fixture.spec;
  spec.loadSource = [&] (const PreparationSpec&, std::chrono::steady_clock::time_point) {
    std::unique_lock<std::mutex> lock(sourceMutex);
    sourceCondition.wait(lock, [&] { return releaseSource; });
    return fixture.source;
  };
  spec.dispatch = [&] (std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(dispatchMutex);
      queued.push_back(std::move(task));
    }
    dispatchCondition.notify_one();
    throw std::runtime_error("dispatch failure after queue");
  };
  auto handle = Spec185PreparationTestAccess::prepareAsync(cache, spec);
  std::atomic<unsigned> callbackCount{0};
  std::promise<std::string> callbackError;
  auto subscription = handle.onCompletion(
    [&] (std::exception_ptr error, std::optional<PreparedModel>) {
      callbackCount.fetch_add(1, std::memory_order_acq_rel);
      std::string message = "NONE";
      try {
        if (error)
          std::rethrow_exception(error);
      }
      catch (const std::exception& value) {
        message = value.what();
      }
      callbackError.set_value(std::move(message));
    });
  {
    std::lock_guard<std::mutex> lock(sourceMutex);
    releaseSource = true;
  }
  sourceCondition.notify_all();
  auto future = callbackError.get_future();
  BOOST_REQUIRE(future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  BOOST_CHECK_EQUAL(future.get(), "dispatch failure after queue");
  std::function<void()> queuedTask;
  {
    std::unique_lock<std::mutex> lock(dispatchMutex);
    BOOST_REQUIRE(dispatchCondition.wait_for(lock, std::chrono::seconds(2),
      [&] { return !queued.empty(); }));
    queuedTask = std::move(queued.front());
  }
  queuedTask();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  BOOST_CHECK_EQUAL(callbackCount.load(std::memory_order_acquire), 1U);
  subscription.cancel();
  handle.cancel();
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()
