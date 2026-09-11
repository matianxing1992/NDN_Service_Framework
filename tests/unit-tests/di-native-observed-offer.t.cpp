#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeObservedOfferV3.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <fstream>
#include <random>

namespace {
using namespace ndnsf::di;
NativeJson vectors()
{
  std::ifstream stream("tests/fixtures/spec182/offer-python-oracle.json");
  if (!stream) throw std::runtime_error("missing frozen SDK offer oracle");
  return NativeJson::parse(stream);
}
}

BOOST_AUTO_TEST_SUITE(Spec182ObservedOffer)
BOOST_AUTO_TEST_CASE(RealSdkIdentityAndObservations)
{
  for (const auto& sample : vectors()) {
    BOOST_TEST_CONTEXT(sample.at("name").get<std::string>()) {
      const auto offer = decodeNativeProviderOfferV3(sample.at("wire").get<std::string>());
      BOOST_CHECK_EQUAL(offer.offerDigest, sample.at("digest").get<std::string>());
      BOOST_CHECK_EQUAL(offer.topologyDigest, sample.at("topology_digest").get<std::string>());
      BOOST_CHECK_EQUAL(offer.provider, "/provider/a");
      BOOST_CHECK(offer.hasModel);
      if (sample.at("name") == "cuda") {
        BOOST_REQUIRE_EQUAL(offer.resources.size(), 1);
        BOOST_CHECK_EQUAL(offer.resources[0].freeMemoryMb, 9000);
        BOOST_CHECK_EQUAL(offer.resources[0].resourceSequence, 3);
        BOOST_REQUIRE_EQUAL(offer.residency.size(), 1);
        const auto& proof = offer.residency[0];
        BOOST_CHECK_EQUAL(proof.runtimeGeneration, 7);
        BOOST_CHECK_EQUAL(proof.fencingToken, "fence");
        BOOST_CHECK_EQUAL(proof.estimatedAssemblyMs, 1.5);
        BOOST_CHECK_EQUAL(proof.missingVerifiedBytes, 11);
      }
      else {
        BOOST_CHECK(offer.devices.empty());
        BOOST_CHECK(offer.resources.empty());
        BOOST_CHECK(offer.residency.empty());
      }
    }
  }
}

BOOST_AUTO_TEST_CASE(RejectMalformedContractBeforeObservation)
{
  const auto samples = vectors();
  const auto base = nativeParseJson(samples.at(0).at("wire").get<std::string>());
  const auto reject = [](const NativeJson& value) {
    BOOST_CHECK_THROW(decodeNativeProviderOfferV3(nativeCanonicalJson(value)), std::invalid_argument);
  };
  for (const auto* key : {"status", "preparation_accepted", "attempt", "topology"}) {
    auto value = base; value.erase(key); reject(value);
  }
  for (const auto* key : {"attempt", "queue_depth", "captured_at_ms"}) {
    auto value = base; value[key] = -1; reject(value);
    value[key] = true; reject(value);
  }
  auto value = base; value["preparation_accepted"] = false; reject(value);
  value = base; value["ack_reservation"] = true; reject(value);
  value = base; value["topology"]["devices"] = {"cpu"}; reject(value);
  value = base; value["unknown"] = 0; reject(value);
  value = base; value["expires_at_ms"] = 100; reject(value);
  const auto gpu = nativeParseJson(samples.at(1).at("wire").get<std::string>());
  value = gpu; value["resources"][0]["free_memory_mb"] = 13000; reject(value);
  value = gpu; value["resources"][0].erase("total_memory_mb"); reject(value);
  value = gpu; value["residency"][0].erase("rank"); reject(value);
  value = gpu; value["resources"].push_back(value["resources"][0]); reject(value);
  value = gpu; value["resources"][0]["device"] = "cuda:1"; reject(value);
  const auto wire = nativeCanonicalJson(base);
  BOOST_CHECK_THROW(decodeNativeProviderOfferV3(" " + wire), std::invalid_argument);
  BOOST_CHECK_THROW(decodeNativeProviderOfferV3("{\"attempt\":1," + wire.substr(1)), std::invalid_argument);
  BOOST_CHECK_THROW(decodeNativeProviderOfferV3(std::string(1024 * 1024 + 1, ' ')), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(Spec184NativeParserFuzz)
{
  const auto samples = vectors();
  std::mt19937 generator(0x184F00D);
  std::uniform_int_distribution<std::size_t> sampleDistribution(0, samples.size() - 1);
  std::uniform_int_distribution<std::size_t> mutationDistribution(0, 5);
  std::uniform_int_distribution<unsigned int> byteDistribution(0, 255);
  std::size_t rejected = 0;
  std::size_t accepted = 0;

  // This is a bounded deterministic parser campaign.  It exercises the
  // production decoder with truncation, bit flips, suffix/prefix noise and
  // structural byte replacement while keeping each input below the decoder
  // wire limit.  Business meaning remains owned by the decoder's contract
  // checks; the test only requires a bounded reject or a valid decode.
  for (std::size_t iteration = 0; iteration < 512; ++iteration) {
    const auto& sample = samples.at(sampleDistribution(generator));
    auto mutated = sample.at("wire").get<std::string>();
    switch (mutationDistribution(generator)) {
    case 0:
      mutated.resize(iteration % (mutated.size() + 1));
      break;
    case 1:
      mutated.at(iteration % mutated.size()) =
        static_cast<char>(byteDistribution(generator));
      break;
    case 2:
      mutated.insert(mutated.begin() + static_cast<std::ptrdiff_t>(
        iteration % (mutated.size() + 1)),
        static_cast<char>(byteDistribution(generator)));
      break;
    case 3:
      mutated.append(iteration % 17, static_cast<char>(byteDistribution(generator)));
      break;
    case 4:
      mutated = "{" + mutated;
      break;
    case 5:
      mutated.push_back('}');
      break;
    }
    try {
      const auto decoded = decodeNativeProviderOfferV3(mutated);
      BOOST_CHECK(!decoded.provider.empty());
      ++accepted;
    }
    catch (const std::exception&) {
      ++rejected;
    }
  }
  BOOST_CHECK_GT(rejected, 0U);
  BOOST_CHECK_GT(accepted, 0U);
}
BOOST_AUTO_TEST_SUITE_END()
