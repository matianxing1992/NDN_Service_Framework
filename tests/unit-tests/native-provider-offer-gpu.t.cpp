#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV3.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/cuda-device-memory.hpp"
#include "NDNSF-DistributedInference/cpp/vendor/nlohmann/json.hpp"

#include <boost/test/unit_test.hpp>
#include <openssl/sha.h>

#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>
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

BOOST_AUTO_TEST_CASE(MeasuredGpuResourcesAreSignedFromOneSnapshot)
{
  unsigned signedOffers = 0;
  auto config = providerConfig(signedOffers);
  config.backends = {"onnxruntime"};
  config.devices = {"cuda:0"};
  unsigned observations = 0;
  config.observeResources = [&observations] (std::uint64_t nowMs) {
    ++observations;
    NativeDeviceResourceObservationV3 row;
    row.device = "cuda:0";
    row.totalMemoryMb = 24576;
    row.freeMemoryMb = 23000;
    row.capturedAtMs = nowMs;
    return std::vector<NativeDeviceResourceObservationV3>{row};
  };
  std::string signedDigest;
  config.signDigest = [&signedDigest] (const std::string& digest) {
    signedDigest = digest;
    return "signature";
  };
  const auto offer = issueNativeProviderOfferV3(requestWire(), config, 5000);
  BOOST_REQUIRE(offer);
  BOOST_REQUIRE(offer->status);
  BOOST_CHECK_EQUAL(observations, 1);
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
      config.observeResources = [scenario] (std::uint64_t nowMs) {
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
  const auto memory = queryCudaDeviceMemory(0, 5000, 7, 2);
  BOOST_CHECK_EQUAL(memory.device, "cuda:0");
  BOOST_CHECK_EQUAL(memory.totalMemoryMb, 24576);
  BOOST_CHECK_EQUAL(memory.freeMemoryMb, 18000);
  BOOST_CHECK_EQUAL(memory.resourceSequence, 7);
  BOOST_CHECK_EQUAL(memory.activeRequests, 2);
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

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di
