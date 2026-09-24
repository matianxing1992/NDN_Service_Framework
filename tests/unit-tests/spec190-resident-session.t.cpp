#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeSessionCache.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace ndnsf::di::test {
namespace {

using namespace std::chrono_literals;

BOOST_AUTO_TEST_SUITE(Spec190ResidentSession)

BOOST_AUTO_TEST_CASE(CacheSingleFlightAndClose)
{
  auto cache = std::make_shared<OnnxRuntimeSessionCache>(
    OnnxRuntimeSessionCache::Config{50ms, 1});
  std::atomic<int> loads{0};
  const auto loader = [&] {
    ++loads;
    std::this_thread::sleep_for(20ms);
    std::shared_ptr<void> value = std::make_shared<int>(7);
    return value;
  };
  auto first = std::async(std::launch::async, [&] {
    return cache->acquire("identity-a", loader);
  });
  std::this_thread::sleep_for(2ms);
  auto second = std::async(std::launch::async, [&] {
    return cache->acquire("identity-a", loader);
  });
  auto firstLease = first.get();
  auto secondLease = second.get();
  BOOST_REQUIRE(firstLease);
  BOOST_REQUIRE(secondLease);
  BOOST_CHECK_EQUAL(loads.load(), 1);
  BOOST_CHECK_EQUAL(cache->counters().loads, 1);
  BOOST_CHECK_EQUAL(cache->counters().hits, 1);
  BOOST_CHECK_EQUAL(cache->counters().activeLeases, 2);

  firstLease = {};
  secondLease = {};
  BOOST_CHECK_EQUAL(cache->counters().activeLeases, 0);
  BOOST_CHECK(cache->evict("identity-a"));

  auto replacement = cache->acquire("identity-b", loader);
  BOOST_REQUIRE(replacement);
  BOOST_CHECK_EQUAL(loads.load(), 2);
  cache->close();
  BOOST_CHECK_THROW(cache->acquire("identity-c", loader), std::runtime_error);
  replacement = {};
  BOOST_CHECK(cache->drain(50ms));
}

BOOST_AUTO_TEST_CASE(CloseKeepsActiveLeaseSafe)
{
  OnnxRuntimeSessionCache cache;
  auto lease = cache.acquire("identity", [] {
    std::shared_ptr<void> value = std::make_shared<int>(11);
    return value;
  });
  BOOST_REQUIRE(lease);
  cache.close();
  BOOST_CHECK(!cache.drain(0ms));
  BOOST_CHECK_THROW(cache.acquire("identity", [] {
    std::shared_ptr<void> value = std::make_shared<int>(12);
    return value;
  }), std::runtime_error);
  lease = {};
  BOOST_CHECK(cache.drain(50ms));
}

BOOST_AUTO_TEST_CASE(CacheIdleFailureAndWaiterTimeout)
{
  auto cache = std::make_shared<OnnxRuntimeSessionCache>(
    OnnxRuntimeSessionCache::Config{20ms, 1});
  std::atomic<int> loads{0};
  auto loader = [&] {
    ++loads;
    std::shared_ptr<void> value = std::make_shared<int>(13);
    return value;
  };
  auto lease = cache->acquire("idle", loader);
  BOOST_REQUIRE(lease);
  lease = {};
  std::this_thread::sleep_for(30ms);
  cache->evictIdle();
  BOOST_CHECK_EQUAL(cache->counters().residentEntries, 0);

  BOOST_CHECK_THROW(cache->acquire("failure", [] () -> std::shared_ptr<void> {
    throw std::runtime_error("fixture loader failure");
  }), std::runtime_error);
  BOOST_CHECK_EQUAL(cache->counters().residentEntries, 0);

  std::promise<void> loaderStarted;
  auto started = loaderStarted.get_future();
  auto creator = std::async(std::launch::async, [&] {
    return cache->acquire("slow", [&] {
      loaderStarted.set_value();
      std::this_thread::sleep_for(40ms);
      std::shared_ptr<void> value = std::make_shared<int>(17);
      return value;
    });
  });
  started.wait();
  BOOST_CHECK_THROW(cache->acquire(
    "slow", loader, std::chrono::steady_clock::now() + 5ms), std::runtime_error);
  auto creatorLease = creator.get();
  BOOST_REQUIRE(creatorLease);
  creatorLease = {};
  BOOST_CHECK_EQUAL(loads.load(), 1);
}

BOOST_AUTO_TEST_CASE(CloseRejectsLateLoaderPublication)
{
  OnnxRuntimeSessionCache cache;
  std::promise<void> loaderStarted;
  auto started = loaderStarted.get_future();
  std::promise<void> releaseLoader;
  auto release = releaseLoader.get_future().share();
  auto creator = std::async(std::launch::async, [&] {
    return cache.acquire("late", [&] {
      loaderStarted.set_value();
      release.wait();
      std::shared_ptr<void> value = std::make_shared<int>(19);
      return value;
    });
  });
  started.wait();
  cache.close();
  releaseLoader.set_value();
  BOOST_CHECK_THROW(creator.get(), std::runtime_error);
  BOOST_CHECK(cache.drain(100ms));
  BOOST_CHECK_THROW(cache.acquire("late", [] {
    return std::shared_ptr<void>(std::make_shared<int>(23));
  }), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(CloseRejectsCreatorAndWaiter)
{
  auto cache = std::make_shared<OnnxRuntimeSessionCache>();
  std::promise<void> loaderStarted;
  auto started = loaderStarted.get_future();
  std::promise<void> releaseLoader;
  auto release = releaseLoader.get_future().share();
  auto creator = std::async(std::launch::async, [&] {
    return cache->acquire("close-waiters", [&] {
      loaderStarted.set_value();
      release.wait();
      return std::shared_ptr<void>(std::make_shared<int>(41));
    });
  });
  started.wait();
  auto waiterJoined = std::make_shared<std::promise<void>>();
  auto waiterJoinedFuture = waiterJoined->get_future();
  auto waiterCallbackEntered = std::make_shared<std::atomic<bool>>(false);
  auto waiter = std::async(std::launch::async, [&] {
    return cache->acquire(
      "close-waiters", [] {
        return std::shared_ptr<void>(std::make_shared<int>(43));
      }, std::chrono::steady_clock::time_point::max(),
      [waiterJoined, waiterCallbackEntered] {
        if (!waiterCallbackEntered->exchange(true))
          waiterJoined->set_value();
        return false;
      });
  });
  waiterJoinedFuture.wait();
  cache->close();
  releaseLoader.set_value();
  BOOST_CHECK_THROW(creator.get(), std::runtime_error);
  BOOST_CHECK_THROW(waiter.get(), std::runtime_error);
  BOOST_CHECK_EQUAL(cache->counters().residentEntries, 0);
  BOOST_CHECK(cache->drain(100ms));
}

BOOST_AUTO_TEST_CASE(EvictActiveEntryRejectsReplacement)
{
  OnnxRuntimeSessionCache cache(OnnxRuntimeSessionCache::Config{120s, 1});
  auto lease = cache.acquire("active-eviction", [] {
    return std::shared_ptr<void>(std::make_shared<int>(47));
  });
  BOOST_REQUIRE(lease);
  BOOST_CHECK(cache.evict("active-eviction"));
  BOOST_CHECK_THROW(cache.acquire("active-eviction", [] {
    return std::shared_ptr<void>(std::make_shared<int>(53));
  }), std::runtime_error);
  lease = {};
  BOOST_CHECK(!cache.evict("active-eviction"));
  BOOST_CHECK(cache.drain(100ms));
}

BOOST_AUTO_TEST_CASE(CancelledCreatorLeavesResultToValidWaiter)
{
  auto cache = std::make_shared<OnnxRuntimeSessionCache>();
  std::promise<void> loaderStarted;
  auto started = loaderStarted.get_future();
  std::promise<void> releaseLoader;
  auto release = releaseLoader.get_future().share();
  std::atomic<bool> creatorCancelled{false};
  auto creator = std::async(std::launch::async, [&] {
    return cache->acquire("creator-cancel", [&] {
      loaderStarted.set_value();
      release.wait();
      return std::shared_ptr<void>(std::make_shared<int>(29));
    }, std::chrono::steady_clock::time_point::max(), [&] {
      return creatorCancelled.load();
    });
  });
  started.wait();
  auto waiterJoined = std::make_shared<std::promise<void>>();
  auto waiterJoinedFuture = waiterJoined->get_future();
  auto waiterCallbackEntered = std::make_shared<std::atomic<bool>>(false);
  auto waiter = std::async(std::launch::async, [&] {
    return cache->acquire("creator-cancel", [] {
      return std::shared_ptr<void>(std::make_shared<int>(31));
    }, std::chrono::steady_clock::time_point::max(),
    [waiterJoined, waiterCallbackEntered] {
      if (!waiterCallbackEntered->exchange(true))
        waiterJoined->set_value();
      return false;
    });
  });
  waiterJoinedFuture.wait();
  creatorCancelled.store(true);
  releaseLoader.set_value();

  BOOST_CHECK_THROW(creator.get(), std::runtime_error);
  auto waiterLease = waiter.get();
  BOOST_REQUIRE(waiterLease);
  BOOST_CHECK_EQUAL(cache->counters().loads, 1);
  BOOST_CHECK_EQUAL(cache->counters().activeLeases, 1);
  waiterLease = {};
  BOOST_CHECK(cache->drain(100ms));
}

BOOST_AUTO_TEST_CASE(CancelledCreatorDoesNotPublishWithoutWaiter)
{
  auto cache = std::make_shared<OnnxRuntimeSessionCache>();
  std::promise<void> loaderStarted;
  auto started = loaderStarted.get_future();
  std::promise<void> releaseLoader;
  auto release = releaseLoader.get_future().share();
  std::atomic<bool> creatorCancelled{false};
  auto creator = std::async(std::launch::async, [&] {
    return cache->acquire("creator-cancelled-alone", [&] {
      loaderStarted.set_value();
      release.wait();
      return std::shared_ptr<void>(std::make_shared<int>(37));
    }, std::chrono::steady_clock::time_point::max(), [&] {
      return creatorCancelled.load();
    });
  });
  started.wait();
  creatorCancelled.store(true);
  releaseLoader.set_value();
  BOOST_CHECK_THROW(creator.get(), std::runtime_error);
  BOOST_CHECK_EQUAL(cache->counters().residentEntries, 0);
  BOOST_CHECK_EQUAL(cache->counters().loads, 0);
  BOOST_CHECK(cache->drain(100ms));
}

#ifdef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP

std::filesystem::path
writeTinyOnnxFixture()
{
  static constexpr char kHex[] =
    "080a1207737065633138353a4c0a1a0a01581201591a086964656e7469747922084964656e74697479"
    "120474696e795a130a0158120e0a0c080112080a0208010a02080162130a0159120e0a0c080112080a0208010a02080142040a00100d";
  const auto path = std::filesystem::path("/tmp") /
    ("spec190-resident-session-" + std::to_string(static_cast<long long>(::getpid())) + ".onnx");
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("cannot create resident-session ONNX fixture");
  }
  const auto nibble = [] (char c) -> unsigned char {
    if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned char>(c - 'a' + 10);
    throw std::runtime_error("invalid resident-session fixture encoding");
  };
  for (std::size_t i = 0; kHex[i] != '\0'; i += 2) {
    output.put(static_cast<char>((nibble(kHex[i]) << 4) | nibble(kHex[i + 1])));
  }
  return path;
}

NativeModelRunnerSpec
makeResidentSpec(const std::filesystem::path& path,
                 const std::string& modelDigest)
{
  NativeModelRunnerSpec spec;
  spec.role = "/Spec190/Resident/Role";
  spec.kind = "onnxruntime";
  spec.backend = "onnxruntime-cpu";
  spec.path = path.string();
  const auto digest = std::string(64, '1');
  spec.metadata = {
    {"residentSession", "true"},
    {"executionProvider", "cpu"},
    {"assembledModelDigest", modelDigest},
    {"artifactDigest", modelDigest},
    {"graphDigest", digest},
    {"canonicalInitializerDigest", digest},
    {"recipeDigest", digest},
    {"backendAbi", "onnxruntime-cpu-v1"},
    {"artifactProfileDigest", digest},
    {"adapterDescriptorDigest", digest},
    {"assemblerDescriptorDigest", digest},
    {"precision", "float32"},
    {"quantization", "none"},
    {"layout", "row-major"},
    {"inputNames", "X"},
    {"outputNames", "Y"},
    {"evidence.providerName", "/Spec190/Provider"},
    {"evidence.providerBootId", "boot-190"},
    {"evidence.epoch", "1"},
    {"evidence.modelDigest", digest},
    {"evidence.artifactDigest", modelDigest},
    {"evidence.planDigest", digest},
    {"evidence.createdAtMs", "1"},
  };
  return spec;
}

RoleExecutionContext
makeContext(const std::string& sessionId)
{
  float input = 3.5F;
  std::vector<std::uint8_t> bytes(sizeof(input));
  std::memcpy(bytes.data(), &input, bytes.size());
  RoleExecutionContext context;
  context.sessionId = sessionId;
  context.role = "/Spec190/Resident/Role";
  context.requestId = "/Spec190/Request/" + sessionId;
  context.providerBootId = "boot-190";
  context.attemptEpoch = 1;
  context.inputsByScope.emplace(
    "X", makeEncodedTensorBundle(
      "X", {NamedTensor{"X", TensorElementType::Float32, {1, 1}, std::move(bytes)}}));
  return context;
}

BOOST_AUTO_TEST_CASE(ResidentOnnxSessionReusesLoadAndIsolatesRequests)
{
  const auto path = writeTinyOnnxFixture();
  struct Cleanup
  {
    std::filesystem::path path;
    ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); }
  } cleanup{path};
  std::ifstream input(path, std::ios::binary);
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
  const auto modelDigest = sha256TensorBytes(bytes);
  const auto spec = makeResidentSpec(path, modelDigest);
  auto cache = std::make_shared<OnnxRuntimeSessionCache>();
  RegistryNativeModelRunnerFactory factory;
  registerOnnxRuntimeBackend(factory, cache);
  factory.freeze();

  auto first = factory.create(spec);
  auto second = factory.create(spec);
  BOOST_REQUIRE(first);
  BOOST_REQUIRE(second);
  const auto counters = cache->counters();
  BOOST_CHECK_EQUAL(counters.loads, 1);
  BOOST_CHECK_EQUAL(counters.hits, 1);
  BOOST_CHECK_EQUAL(counters.activeLeases, 2);

  const auto firstOutputs = first->run(makeContext("request-1"));
  const auto secondOutputs = second->run(makeContext("request-2"));
  const auto& firstTensor = firstOutputs.at("Y");
  const auto& secondTensor = secondOutputs.at("Y");
  BOOST_REQUIRE_EQUAL(firstTensor.payload.size(), sizeof(float));
  BOOST_REQUIRE_EQUAL(secondTensor.payload.size(), sizeof(float));
  float firstValue = 0;
  float secondValue = 0;
  std::memcpy(&firstValue, firstTensor.payload.data(), sizeof(firstValue));
  std::memcpy(&secondValue, secondTensor.payload.data(), sizeof(secondValue));
  BOOST_CHECK_CLOSE(firstValue, 3.5F, 0.001);
  BOOST_CHECK_CLOSE(secondValue, 3.5F, 0.001);

  first.reset();
  second.reset();
  auto changed = spec;
  changed.metadata["graphDigest"] = std::string(64, '2');
  auto third = factory.create(changed);
  BOOST_REQUIRE(third);
  BOOST_CHECK_EQUAL(cache->counters().loads, 2);
  cache->close();
  BOOST_CHECK_THROW(factory.create(spec), std::runtime_error);
  third.reset();
  BOOST_CHECK(cache->drain(100ms));
}

BOOST_AUTO_TEST_CASE(ResidentOnnxSessionBypassesProtectedAndProfiling)
{
  const auto path = writeTinyOnnxFixture();
  struct Cleanup
  {
    std::filesystem::path path;
    ~Cleanup()
    {
      std::error_code error;
      std::filesystem::remove(path, error);
      std::filesystem::remove(path.string() + ".profile", error);
    }
  } cleanup{path};
  std::ifstream input(path, std::ios::binary);
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
  const auto modelDigest = sha256TensorBytes(bytes);
  const auto spec = makeResidentSpec(path, modelDigest);
  auto cache = std::make_shared<OnnxRuntimeSessionCache>();
  RegistryNativeModelRunnerFactory factory;
  registerOnnxRuntimeBackend(factory, cache);
  factory.freeze();

  auto ordinary = factory.create(spec);
  BOOST_REQUIRE(ordinary);
  ordinary.reset();

  auto protectedSpec = spec;
  protectedSpec.metadata["encryptedArtifactPath"] = path.string();
  auto protectedRunner = factory.create(protectedSpec);
  BOOST_REQUIRE(protectedRunner);
  protectedRunner.reset();

  auto profiledSpec = spec;
  profiledSpec.metadata["providerProfilePrefix"] = path.string() + ".profile";
  auto profiledRunner = factory.create(profiledSpec);
  BOOST_REQUIRE(profiledRunner);
  profiledRunner.reset();

  const auto counters = cache->counters();
  BOOST_CHECK_EQUAL(counters.loads, 1);
  BOOST_CHECK_EQUAL(counters.hits, 0);
  BOOST_CHECK_EQUAL(counters.residentEntries, 1);
  cache->close();
  BOOST_CHECK(cache->drain(100ms));
}

#else

BOOST_AUTO_TEST_CASE(ResidentOnnxSessionRequiresEnabledOrt)
{
  BOOST_TEST(true);
}

#endif

BOOST_AUTO_TEST_SUITE_END()

} // namespace
} // namespace ndnsf::di::test
