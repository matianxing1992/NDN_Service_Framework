#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV3.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/request-work-statistics.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/cuda-device-memory.hpp"
#include "NDNSF-DistributedInference/cpp/vendor/nlohmann/json.hpp"

#include <boost/test/unit_test.hpp>
#include <openssl/sha.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ndnsf::di {
namespace {

std::vector<std::uint8_t>
requestWire()
{
  const std::string wire = R"({"schema":"ndnsf-di-request-envelope-v2",
    "placementProfile":"DI_PLACEMENT_V3","request_id":"/request/1",
    "service":"/service","attempt":1,"plan_deadline_ms":10000,
    "model_identity_hash":
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})";
  return {wire.begin(), wire.end()};
}

NativeProviderOfferV3Config
providerConfig(unsigned& signedOffers)
{
  NativeProviderOfferV3Config config;
  config.provider = "/provider";
  config.service = "/service";
  config.bootEpoch = "boot-1";
  config.signerKeyId = "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  config.acceptedRoles = {"worker"};
  config.backends = {"onnxruntime-cpu"};
  config.signDigest = [&signedOffers] (const std::string&) {
    ++signedOffers;
    return std::string(128, 'a');
  };
  return config;
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec217ProviderOfferGpu)

BOOST_AUTO_TEST_CASE(CpuOfferRemainsAvailableWithoutGpuObservations)
{
  unsigned signedOffers = 0;
  const auto config = providerConfig(signedOffers);
  const auto offer = issueNativeProviderOfferV3(requestWire(), config, 1000);
  BOOST_REQUIRE(offer);
  BOOST_CHECK(offer->status);
  BOOST_CHECK_EQUAL(signedOffers, 1);
}

BOOST_AUTO_TEST_CASE(GpuOfferWithoutMemoryObservationsFailsClosed)
{
  unsigned signedOffers = 0;
  auto config = providerConfig(signedOffers);
  config.backends = {"onnxruntime-cuda"};
  config.devices = {"cuda:0"};
  const auto offer = issueNativeProviderOfferV3(requestWire(), config, 1000);
  BOOST_REQUIRE(offer);
  BOOST_CHECK(!offer->status);
  BOOST_CHECK_EQUAL(signedOffers, 0);
}

BOOST_AUTO_TEST_CASE(OfferWorkObserverClosesBeforeDeliveryAndCannotChangeAdmission)
{
  unsigned signedOffers = 0;
  auto config = providerConfig(signedOffers);
  const auto baseline = issueNativeProviderOfferV3(requestWire(), config, 1000);
  BOOST_REQUIRE(baseline);
  unsigned observations = 0;
  std::optional<NativeWorkStatisticsSnapshot> observedWork;
  std::string observedRequestId;
  config.workObserver = [&] (const std::string& requestId,
                             const NativeWorkStatisticsSnapshot& snapshot) {
    ++observations;
    observedRequestId = requestId;
    observedWork = snapshot;
    throw std::runtime_error("statistics sink unavailable");
  };
  const auto observed = issueNativeProviderOfferV3(requestWire(), config, 1000);
  BOOST_REQUIRE(observed);
  BOOST_CHECK_EQUAL(observed->status, baseline->status);
  BOOST_CHECK_EQUAL(observed->payload, baseline->payload);
  BOOST_CHECK_EQUAL(observed->pendingStateTtlMs, baseline->pendingStateTtlMs);
  BOOST_CHECK_EQUAL(observations, 1U);
  BOOST_REQUIRE(observedWork);
  BOOST_CHECK_EQUAL(observedRequestId, "/request/1");
  BOOST_CHECK(observedWork->complete);
  BOOST_CHECK_EQUAL(observedWork->activeSpans, 0U);
  BOOST_CHECK_EQUAL(observedWork->phases.at("ack-offer-parse").calls, 1U);
  BOOST_CHECK_EQUAL(observedWork->phases.at("ack-offer-sign").calls, 1U);

  config.backends = {"onnxruntime-cuda"};
  config.devices = {"cuda:0"};
  config.workObserver = [&] (const std::string&, const NativeWorkStatisticsSnapshot& snapshot) {
    ++observations;
    observedWork = snapshot;
  };
  observedWork.reset();
  const auto rejected = issueNativeProviderOfferV3(requestWire(), config, 1000);
  BOOST_REQUIRE(rejected);
  BOOST_CHECK(!rejected->status);
  BOOST_CHECK_EQUAL(observations, 2U);
  BOOST_CHECK_EQUAL(signedOffers, 2U);
  BOOST_REQUIRE(observedWork);
  BOOST_CHECK(observedWork->complete);
  BOOST_CHECK_EQUAL(observedWork->activeSpans, 0U);
  BOOST_CHECK_EQUAL(observedWork->phases.at("ack-device-resource").calls, 1U);
  BOOST_CHECK_EQUAL(observedWork->phases.count("ack-offer-sign"), 0U);
}

BOOST_AUTO_TEST_CASE(MeasuredGpuResourcesAreSignedFromOneSnapshot)
{
  unsigned signedOffers = 0;
  auto config = providerConfig(signedOffers);
  config.backends = {"onnxruntime"};
  config.devices = {"cuda:0"};
  unsigned observations = 0;
  config.observeResources = [&observations] (
      std::uint64_t nowMs, const std::shared_ptr<NativeWorkStatistics>&) {
    ++observations;
    NativeDeviceResourceObservationV3 row;
    row.device = "cuda:0";
    row.totalMemoryMb = 24576;
    row.freeMemoryMb = 23000;
    row.capturedAtMs = nowMs;
    return std::vector<NativeDeviceResourceObservationV3>{row};
  };
  std::string signedDigest;
  std::optional<NativeWorkStatisticsSnapshot> observedWork;
  config.workObserver = [&] (const std::string&, const NativeWorkStatisticsSnapshot& work) {
    observedWork = work;
  };
  config.signDigest = [&signedDigest] (const std::string& digest) {
    signedDigest = digest;
    return "signature";
  };
  const auto offer = issueNativeProviderOfferV3(requestWire(), config, 5000);
  BOOST_REQUIRE(offer);
  BOOST_REQUIRE(offer->status);
  BOOST_CHECK_EQUAL(observations, 1);
  BOOST_REQUIRE(observedWork);
  BOOST_CHECK(observedWork->complete);
  BOOST_CHECK_EQUAL(observedWork->activeSpans, 0U);
  BOOST_CHECK(observedWork->phases.at("ack-resource-callback").kind ==
              NativeWorkKind::UNCLASSIFIED);
  BOOST_CHECK(observedWork->phases.at("ack-resource-validate").kind == NativeWorkKind::LOCAL);
  BOOST_CHECK_EQUAL(observedWork->phases.at("ack-resource-validate").calls, 1U);
  BOOST_CHECK_EQUAL(observedWork->counts.at("ack.successfulV3Offers"), 1U);
  BOOST_CHECK_EQUAL(observedWork->counts.at("ack.v3OfferRepresentationBytes"), offer->payload.size());
  auto value = nlohmann::json::parse(offer->payload);
  BOOST_CHECK_EQUAL(value.at("resources").size(), 1);
  BOOST_CHECK_EQUAL(value.at("resources").at(0).at("device"), "cuda:0");
  BOOST_CHECK_EQUAL(value.at("resources").at(0).at("free_memory_mb"), 23000);
  value.erase("signature");
  const auto unsignedWire = value.dump();
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(unsignedWire.data()), unsignedWire.size(), digest);
  std::ostringstream expected;
  expected << "sha256:" << std::hex << std::setfill('0');
  for (const auto byte : digest) expected << std::setw(2) << static_cast<unsigned>(byte);
  BOOST_CHECK_EQUAL(signedDigest, expected.str());
}

BOOST_AUTO_TEST_CASE(InvalidGpuObservationsAreNeverSigned)
{
  for (const std::string scenario : {"empty", "duplicate", "foreign", "stale", "future",
                                     "zero-time", "zero-sequence", "zero-total", "overflow",
                                     "throw"}) {
    BOOST_TEST_CONTEXT(scenario) {
      unsigned signedOffers = 0;
      auto config = providerConfig(signedOffers);
      config.backends = {"onnxruntime-cuda"};
      config.devices = {"cuda:0"};
      config.observeResources = [scenario] (
          std::uint64_t nowMs, const std::shared_ptr<NativeWorkStatistics>&) {
        if (scenario == "throw") throw std::runtime_error("device unavailable");
        NativeDeviceResourceObservationV3 row;
        row.device = scenario == "foreign" ? "cuda:1" : "cuda:0";
        row.totalMemoryMb = scenario == "zero-total" ? 0 : 24576;
        row.freeMemoryMb = scenario == "overflow" ? 24577 : 0;
        row.resourceSequence = scenario == "zero-sequence" ? 0 : 1;
        row.capturedAtMs = scenario == "zero-time" ? 0 :
          scenario == "stale" ? nowMs - 2001 : scenario == "future" ? nowMs + 1 : nowMs;
        std::vector<NativeDeviceResourceObservationV3> rows;
        if (scenario != "empty") rows.push_back(row);
        if (scenario == "duplicate") rows.push_back(row);
        return rows;
      };
      const auto offer = issueNativeProviderOfferV3(requestWire(), config, 5000);
      BOOST_REQUIRE(offer);
      BOOST_CHECK(!offer->status);
      BOOST_CHECK_EQUAL(offer->message, "DI_V3_GPU_RESOURCE_UNAVAILABLE");
      BOOST_CHECK_EQUAL(signedOffers, 0);
    }
  }
}

BOOST_AUTO_TEST_CASE(CudaMemoryUsesRuntimeUuidInsteadOfNvmlIndex)
{
  const auto statistics = makeNativeWorkStatistics();
  const auto memory = queryCudaDeviceMemory(0, 5000, 7, 2, statistics);
  BOOST_CHECK_EQUAL(memory.device, "cuda:0");
  BOOST_CHECK_EQUAL(memory.totalMemoryMb, 24576);
  BOOST_CHECK_EQUAL(memory.freeMemoryMb, 18000);
  BOOST_CHECK_EQUAL(memory.resourceSequence, 7);
  BOOST_CHECK_EQUAL(memory.activeRequests, 2);
  const auto work = statistics->snapshot();
  BOOST_REQUIRE(work.complete);
  BOOST_CHECK_EQUAL(work.activeSpans, 0U);
  const auto& owner = work.phases.at("gpu-resource-query-owner");
  BOOST_CHECK(owner.kind == NativeWorkKind::UNCLASSIFIED);
  BOOST_CHECK_EQUAL(owner.calls, 1U);
  BOOST_REQUIRE(owner.firstStartedSteadyNs && owner.lastFinishedSteadyNs);
  std::uint64_t accounted = 0;
  for (const auto& phase : work.phases) accounted += phase.second.exclusiveWallNs;
  BOOST_CHECK_EQUAL(accounted, *owner.lastFinishedSteadyNs - *owner.firstStartedSteadyNs);
}

BOOST_AUTO_TEST_CASE(CudaMemoryRejectsVendorFailuresAndInvalidSizes)
{
  const char* key = "SPEC217_GPU_FIXTURE_SCENARIO";
  const auto* previous = std::getenv(key);
  const std::string original = previous ? previous : "";
  const bool hadValue = previous != nullptr;
  for (const char* scenario : {"memory-device-missing", "memory-error", "memory-zero-total",
                               "memory-overflow", "memory-bad-used"}) {
    BOOST_TEST_CONTEXT(scenario) {
      BOOST_REQUIRE_EQUAL(::setenv(key, scenario, 1), 0);
      BOOST_CHECK_THROW(queryCudaDeviceMemory(0, 5000, 7, 0), std::runtime_error);
    }
  }
  if (hadValue) ::setenv(key, original.c_str(), 1);
  else ::unsetenv(key);
}

BOOST_AUTO_TEST_CASE(OwnedCudaQueryReadsFreshValuesAndDrainsItsSession)
{
  const auto closeLibrary = [] (void* handle) { if (handle) dlclose(handle); };
  std::unique_ptr<void, decltype(closeLibrary)> library(
    dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL), closeLibrary);
  BOOST_REQUIRE(library);
  const auto reset = reinterpret_cast<void (*)()>(dlsym(library.get(), "spec217GpuQueryReset"));
  const auto count = reinterpret_cast<unsigned (*)(unsigned)>(
    dlsym(library.get(), "spec217GpuQueryCount"));
  BOOST_REQUIRE(reset);
  BOOST_REQUIRE(count);
  const char* key = "SPEC217_GPU_FIXTURE_SCENARIO";
  const auto* previous = std::getenv(key);
  const std::optional<std::string> original = previous ? std::optional<std::string>(previous) :
                                                       std::nullopt;
  const auto restore = [key, original] (void*) {
    if (original) ::setenv(key, original->c_str(), 1);
    else ::unsetenv(key);
  };
  const std::unique_ptr<void, decltype(restore)> environment(library.get(), restore);
  BOOST_REQUIRE_EQUAL(::unsetenv(key), 0);
  reset();
  auto owner = std::make_shared<CudaDeviceMemoryQuery>(0);
  auto statistics = makeNativeWorkStatistics();
  BOOST_REQUIRE(statistics);
  const std::weak_ptr<NativeWorkStatistics> statisticsOwner = statistics;
  BOOST_CHECK_EQUAL(count(0), 0U);
  const auto first = owner->query(100, 1, 0, statistics);
  BOOST_CHECK_EQUAL(first.freeMemoryMb, 18000U);
  BOOST_CHECK_EQUAL(count(0), 1U);
  BOOST_CHECK_EQUAL(count(2), 1U);
  const auto firstWork = statistics->snapshot();
  const auto& wrapper = firstWork.phases.at("gpu-resource-wrapper");
  BOOST_CHECK(wrapper.kind == NativeWorkKind::LOCAL);
  BOOST_REQUIRE(wrapper.firstStartedSteadyNs && wrapper.lastFinishedSteadyNs);
  std::uint64_t accounted = 0;
  for (const auto& phase : firstWork.phases) accounted += phase.second.exclusiveWallNs;
  BOOST_CHECK_EQUAL(accounted,
                   *wrapper.lastFinishedSteadyNs - *wrapper.firstStartedSteadyNs);
  BOOST_REQUIRE_EQUAL(::setenv(key, "memory-low", 1), 0);
  const auto next = owner->query(101, 2, 3, statistics);
  BOOST_CHECK_EQUAL(next.freeMemoryMb, 50U);
  BOOST_CHECK_EQUAL(next.capturedAtMs, 101U);
  BOOST_CHECK_EQUAL(next.resourceSequence, 2U);
  BOOST_CHECK_EQUAL(next.activeRequests, 3U);
  const auto work = statistics->snapshot();
  BOOST_REQUIRE(work.complete);
  BOOST_CHECK_EQUAL(work.activeSpans, 0U);
  BOOST_CHECK(work.phases.at("gpu-resource-lock").kind == NativeWorkKind::WAIT);
  BOOST_CHECK_EQUAL(work.phases.at("gpu-resource-lock").calls, 2U);
  BOOST_CHECK(work.phases.at("gpu-resource-initialize").kind == NativeWorkKind::UNCLASSIFIED);
  BOOST_CHECK_EQUAL(work.phases.at("gpu-resource-initialize").calls, 1U);
  BOOST_CHECK(work.phases.at("gpu-resource-query").kind == NativeWorkKind::UNCLASSIFIED);
  BOOST_CHECK_EQUAL(work.phases.at("gpu-resource-query").calls, 2U);
  BOOST_CHECK(work.phases.at("gpu-resource-fields").kind == NativeWorkKind::LOCAL);
  BOOST_CHECK_EQUAL(work.phases.at("gpu-resource-fields").calls, 2U);
  BOOST_CHECK_EQUAL(work.phases.at("gpu-resource-wrapper").calls, 2U);
  statistics.reset();
  BOOST_CHECK(statisticsOwner.expired());
  unsigned signedOffers = 0;
  auto config = providerConfig(signedOffers);
  config.backends = {"onnxruntime-cuda"};
  config.devices = {"cuda:0"};
  config.observeResources = [owner] (
      std::uint64_t now, const std::shared_ptr<NativeWorkStatistics>& observed) {
    return std::vector<NativeDeviceResourceObservationV3>{owner->query(now, 3, 0, observed)};
  };
  std::optional<NativeWorkStatisticsSnapshot> rejectedWork;
  config.workObserver = [&] (const std::string&, const NativeWorkStatisticsSnapshot& value) {
    rejectedWork = value;
  };
  BOOST_REQUIRE_EQUAL(::setenv(key, "memory-error", 1), 0);
  const auto rejected = issueNativeProviderOfferV3(requestWire(), config, 5000);
  BOOST_REQUIRE(rejected);
  BOOST_CHECK(!rejected->status);
  BOOST_CHECK_EQUAL(signedOffers, 0U);
  BOOST_REQUIRE(rejectedWork);
  BOOST_CHECK(rejectedWork->complete);
  BOOST_CHECK_EQUAL(rejectedWork->activeSpans, 0U);
  BOOST_CHECK_EQUAL(rejectedWork->phases.at("gpu-resource-query").calls, 1U);
  BOOST_CHECK_EQUAL(rejectedWork->phases.count("ack-resource-validate"), 0U);
  BOOST_CHECK(rejectedWork->counts.empty());
  BOOST_REQUIRE_EQUAL(::unsetenv(key), 0);
  std::atomic<bool> correct{true};
  std::vector<std::thread> workers;
  for (unsigned worker = 0; worker < 4; ++worker) {
    workers.emplace_back([owner, &correct, worker] {
      const auto work = makeNativeWorkStatistics();
      if (!work) {
        correct = false;
        return;
      }
      for (unsigned call = 0; call < 16; ++call) {
        try {
          const auto value = owner->query(1000 + call, worker * 16 + call + 4, worker, work);
          if (value.freeMemoryMb != 18000 || value.activeRequests != worker ||
              value.capturedAtMs != 1000 + call) correct = false;
        }
        catch (...) { correct = false; }
      }
      const auto observed = work->snapshot();
      const auto lockPhase = observed.phases.find("gpu-resource-lock");
      if (!observed.complete || observed.activeSpans != 0 ||
          lockPhase == observed.phases.end() || lockPhase->second.calls != 16)
        correct = false;
    });
  }
  for (auto& worker : workers) worker.join();
  BOOST_CHECK(correct.load());
  BOOST_CHECK_EQUAL(count(0), 1U);
  BOOST_CHECK_EQUAL(count(2), 1U);
  BOOST_CHECK_EQUAL(count(3), 67U);
  owner.reset();
  BOOST_CHECK_EQUAL(count(1), 0U);
  config.observeResources = {};
  BOOST_CHECK_EQUAL(count(1), 1U);
  reset();
  const auto recoveryWork = makeNativeWorkStatistics();
  BOOST_REQUIRE(recoveryWork);
  BOOST_REQUIRE_EQUAL(::setenv(key, "memory-device-missing", 1), 0);
  CudaDeviceMemoryQuery recovering(0);
  BOOST_CHECK_THROW(recovering.query(200, 1, 0, recoveryWork), std::runtime_error);
  BOOST_CHECK_EQUAL(count(0), 1U);
  BOOST_CHECK_EQUAL(count(1), 1U);
  BOOST_REQUIRE_EQUAL(::unsetenv(key), 0);
  BOOST_CHECK_EQUAL(recovering.query(201, 2, 0, recoveryWork).freeMemoryMb, 18000U);
  BOOST_CHECK_EQUAL(count(0), 2U);
  BOOST_REQUIRE_EQUAL(::setenv(key, "init-error", 1), 0);
  CudaDeviceMemoryQuery retryInit(0);
  BOOST_CHECK_THROW(retryInit.query(300, 1, 0, recoveryWork), std::runtime_error);
  BOOST_CHECK_EQUAL(count(0), 3U);
  BOOST_CHECK_EQUAL(count(1), 1U);
  BOOST_REQUIRE_EQUAL(::unsetenv(key), 0);
  BOOST_CHECK_EQUAL(retryInit.query(301, 2, 0, recoveryWork).freeMemoryMb, 18000U);
  BOOST_CHECK_EQUAL(count(0), 4U);
  const auto recovered = recoveryWork->snapshot();
  BOOST_CHECK(recovered.complete);
  BOOST_CHECK_EQUAL(recovered.activeSpans, 0U);
  BOOST_CHECK_EQUAL(recovered.phases.at("gpu-resource-initialize").calls, 4U);
  BOOST_CHECK_EQUAL(recovered.phases.at("gpu-resource-query").calls, 2U);
}

// Explicit source-call observation on an allocated real GPU, outside the
// ordinary fake-vendor regression suite. This is not invocation qualification.
BOOST_AUTO_TEST_CASE(ObserveRealCudaResourceQueryCost,
                     *boost::unit_test::disabled())
{
  const auto* enabled = std::getenv("SPEC220_REAL_CUDA_OBSERVE");
  BOOST_REQUIRE(enabled && std::string(enabled) == "1");
  BOOST_REQUIRE(std::getenv("SPEC217_GPU_FIXTURE_SCENARIO") == nullptr);
  unsigned signedOffers = 0;
  auto config = providerConfig(signedOffers);
  config.backends = {"onnxruntime-cuda"};
  config.devices = {"cuda:0"};
  const auto query = std::make_shared<CudaDeviceMemoryQuery>(0);
  for (std::uint64_t index = 0; index < 16; ++index) {
    const auto nowMs = static_cast<std::uint64_t>(std::chrono::duration_cast<
      std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    auto request = nlohmann::json::parse(requestWire());
    request["request_id"] = "/request/real-resource/" + std::to_string(index);
    request["plan_deadline_ms"] = nowMs + 60'000;
    const auto wire = request.dump();
    std::optional<NativeWorkStatisticsSnapshot> observed;
    config.workObserver = [&](const std::string&, const NativeWorkStatisticsSnapshot& snapshot) {
      observed = snapshot;
    };
    config.observeResources = [index, query] (
        std::uint64_t capturedAtMs, const std::shared_ptr<NativeWorkStatistics>& statistics) {
      return std::vector<NativeDeviceResourceObservationV3>{
        query->query(capturedAtMs, index + 1, 0, statistics)};
    };
    const auto offer = issueNativeProviderOfferV3({wire.begin(), wire.end()}, config, nowMs);
    BOOST_REQUIRE(offer);
    BOOST_REQUIRE(offer->status);
    BOOST_REQUIRE(observed);
    BOOST_REQUIRE(observed->complete);
    BOOST_REQUIRE_EQUAL(observed->activeSpans, 0U);
    BOOST_REQUIRE_EQUAL(observed->phases.at("ack-device-resource").calls, 1U);
    const auto value = nlohmann::json::parse(offer->payload);
    BOOST_REQUIRE_EQUAL(value.at("resources").size(), 1U);
    BOOST_CHECK_EQUAL(value.at("resources").at(0).at("resource_sequence"), index + 1);
    BOOST_CHECK_EQUAL(value.at("resources").at(0).at("captured_at_ms"), nowMs);
    std::cout << nlohmann::json{
      {"schema", "spec220-real-resource-observation-v1"}, {"sample", index},
      {"scope", "source-call; synthetic signer; no model or invocation"},
      {"resources", value.at("resources")},
      {"work", nlohmann::json::parse(nativeWorkStatisticsToJson(*observed))}}
      .dump() << '\n';
  }
  BOOST_CHECK_EQUAL(signedOffers, 16U);
  // Preserve any surviving vendor mappings; container/device provenance must
  // also come from the bound job record because query handles are closed.
  std::ifstream maps("/proc/self/maps");
  BOOST_REQUIRE(maps.good());
  std::set<std::string> libraries;
  for (std::string line; std::getline(maps, line);) {
    if (line.find("libcuda") == std::string::npos &&
        line.find("libnvidia-ml") == std::string::npos) continue;
    const auto path = line.find('/');
    if (path != std::string::npos) libraries.insert(line.substr(path));
  }
  // Query functions close their dynamic-library handles. Empty maps are
  // explicitly possible and are not evidence of a specific loaded path.
  const auto domain = nativeWorkClockDomain();
  std::cout << nlohmann::json{{"schema", "spec220-real-resource-library-maps-v1"},
    {"retainedMappings", libraries},
    {"clockDomain", domain ? nlohmann::json(*domain) : nlohmann::json(nullptr)}}
    .dump() << '\n';
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di
