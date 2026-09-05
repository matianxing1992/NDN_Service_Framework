#include "tests/boost-test.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProtectedRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedArtifactStore.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>

namespace ndnsf::di::test {
namespace {
std::string unhex(const std::string& text)
{
  std::string result;
  for (std::size_t i = 0; i < text.size(); i += 2) {
    result.push_back(static_cast<char>(std::stoul(text.substr(i, 2), nullptr, 16)));
  }
  return result;
}

struct BoundGrantFixture
{
  ProtectedRuntimeBindingV1 binding;
  NativeProtectedGrantConfig config;
  std::string wire;
  std::string expectedKey;
  std::uint64_t now = 0;
  int fetchCount = 0;

  BoundGrantFixture()
  {
    boost::property_tree::ptree vectors;
    boost::property_tree::read_json("tests/fixtures/spec181/grant-vectors-v1.json", vectors);
    const auto& positive = vectors.get_child("cases").front().second;
    wire = positive.get<std::string>("wire");
    boost::property_tree::ptree grant;
    std::istringstream input(wire);
    boost::property_tree::read_json(input, grant);
    expectedKey = unhex(positive.get<std::string>("expected.contentKey"));
    now = vectors.get<std::uint64_t>("nowMs");
    binding.provider = vectors.get<std::string>("providerIdentity");
    binding.role = "stage0";
    binding.requestId = vectors.get<std::string>("requestId");
    binding.attempt = vectors.get<std::uint64_t>("attempt");
    binding.planCoreDigest = vectors.get<std::string>("planCoreDigest");
    binding.planDigest = "sha256:" + std::string(64, 'a');
    binding.securityPolicySnapshotDigest = "sha256:" + std::string(64, 'b');
    binding.protectionEpoch = vectors.get<std::string>("protectionEpoch");
    binding.grantDigest = grant.get<std::string>("grantDigest");
    binding.providerBootId = "boot-1";
    binding.fencingToken = "fence-1";
    binding.expiresAtMs = grant.get<std::uint64_t>("expiresAtMs") + 1000;
    config.authorityIdentity = grant.get<std::string>("policyAuthority");
    config.modelManifestDigest = vectors.get<std::string>("modelManifestDigest");
    config.authorityPublicKeyRaw = unhex(vectors.get<std::string>("authorityPublicKeyRaw"));
    config.recipientKey.material = unhex(vectors.get<std::string>("recipientSeed"));
    binding.grantName = canonicalNativeGrantName(
      "/requester", binding.provider, binding.requestId, binding.attempt,
      binding.planCoreDigest, config.modelManifestDigest,
      binding.protectionEpoch, binding.grantDigest);
    config.fetchGrant = [this] (const std::string& name) {
      BOOST_CHECK_EQUAL(name, binding.grantName);
      ++fetchCount;
      return wire;
    };
  }
};
}

BOOST_FIXTURE_TEST_CASE(ProtectedRuntimeRealGrantOwnsKeyAndDrains, BoundGrantFixture)
{
  ProtectedRuntime runtime(binding, config);
  runtime.verifyGrant(binding, now);
  BOOST_CHECK_EQUAL(fetchCount, 1);
  BOOST_CHECK(runtime.state() == ProtectedRuntimeState::GrantVerified);
  runtime.withContentKey(now, [&] (const auto& key) {
    BOOST_CHECK_EQUAL(std::string(key.begin(), key.end()), expectedKey);
  });
  std::string plaintext = "host plaintext";
  runtime.registerHostPlaintextLease("model", [&] {
    std::fill(plaintext.begin(), plaintext.end(), 0);
  });
  runtime.complete();
  BOOST_CHECK(runtime.state() == ProtectedRuntimeState::Zeroized);
  BOOST_CHECK(std::all_of(plaintext.begin(), plaintext.end(), [] (char c) { return c == 0; }));
  BOOST_CHECK_THROW(runtime.withContentKey(now, [] (const auto&) {}), std::runtime_error);
}

BOOST_FIXTURE_TEST_CASE(ProtectedRuntimeRejectsSealedGrantSubstitution, BoundGrantFixture)
{
  binding.grantDigest = "sha256:" + std::string(64, 'f');
  binding.grantName = canonicalNativeGrantName(
    "/requester", binding.provider, binding.requestId, binding.attempt,
    binding.planCoreDigest, config.modelManifestDigest,
    binding.protectionEpoch, binding.grantDigest);
  ProtectedRuntime runtime(binding, config);
  BOOST_CHECK_THROW(runtime.verifyGrant(binding, now), std::runtime_error);
  BOOST_CHECK(runtime.state() == ProtectedRuntimeState::FailedClosed);
  BOOST_CHECK_EQUAL(fetchCount, 1);
}

BOOST_FIXTURE_TEST_CASE(ProtectedRuntimeRejectsConfiguredIssuerMismatch, BoundGrantFixture)
{
  config.authorityIdentity = "different-policy-authority";
  ProtectedRuntime runtime(binding, config);
  BOOST_CHECK_THROW(runtime.verifyGrant(binding, now), std::runtime_error);
  BOOST_CHECK(runtime.state() == ProtectedRuntimeState::FailedClosed);
}

BOOST_FIXTURE_TEST_CASE(ProtectedRuntimeRejectsNoncanonicalNameBeforeFetch, BoundGrantFixture)
{
  binding.grantName += "/extra";
  ProtectedRuntime runtime(binding, config);
  BOOST_CHECK_THROW(runtime.verifyGrant(binding, now), std::runtime_error);
  BOOST_CHECK_EQUAL(fetchCount, 0);
}

BOOST_FIXTURE_TEST_CASE(ProtectedRuntimeUsesGrantExpiryBeforeRequestDeadline, BoundGrantFixture)
{
  ProtectedRuntime runtime(binding, config);
  runtime.verifyGrant(binding, now);
  BOOST_CHECK_THROW(runtime.withContentKey(binding.expiresAtMs - 999,
                                          [] (const auto&) {}), std::runtime_error);
  BOOST_CHECK(runtime.state() == ProtectedRuntimeState::Zeroized);
}

BOOST_FIXTURE_TEST_CASE(ProtectedRuntimeCleanupFailureRetainsRetryAndDrainsOthers, BoundGrantFixture)
{
  ProtectedRuntime runtime(binding, config);
  runtime.verifyGrant(binding, now);
  bool fail = true;
  bool otherCleaned = false;
  runtime.registerHostPlaintextLease("retry", [&] {
    if (fail) throw std::runtime_error("cleanup unavailable");
  });
  runtime.registerDevicePlaintextLease("other", [&] { otherCleaned = true; });
  BOOST_CHECK_THROW(runtime.cancel("cancelled"), std::runtime_error);
  BOOST_CHECK(otherCleaned);
  BOOST_CHECK(runtime.state() == ProtectedRuntimeState::FailedClosed);
  fail = false;
  BOOST_CHECK_NO_THROW(runtime.cancel("retry cleanup"));
  BOOST_CHECK(runtime.state() == ProtectedRuntimeState::Zeroized);
}

BOOST_FIXTURE_TEST_CASE(ProtectedRuntimeDestructorDrainsAfterRegistrationFailure, BoundGrantFixture)
{
  bool cleaned = false;
  {
    ProtectedRuntime runtime(binding, config);
    runtime.verifyGrant(binding, now);
    runtime.registerHostPlaintextLease("duplicate", [&] { cleaned = true; });
    BOOST_CHECK_THROW(runtime.registerHostPlaintextLease("duplicate", [] {}), std::runtime_error);
  }
  BOOST_CHECK(cleaned);
}

BOOST_FIXTURE_TEST_CASE(ProtectedRuntimeConsumerFailureClosesAuthority, BoundGrantFixture)
{
  ProtectedRuntime runtime(binding, config);
  runtime.verifyGrant(binding, now);
  BOOST_CHECK_THROW(runtime.withContentKey(now, [] (const auto&) {
    throw std::runtime_error("AEAD consumer failed");
  }), std::runtime_error);
  BOOST_CHECK(runtime.state() == ProtectedRuntimeState::FailedClosed);
  BOOST_CHECK_THROW(runtime.withContentKey(now, [] (const auto&) {}), std::runtime_error);
}

BOOST_FIXTURE_TEST_CASE(ProtectedRuntimeFetchCannotOutliveRequestDeadline, BoundGrantFixture)
{
  binding.expiresAtMs = now + 1;
  config.fetchGrant = [&] (const auto&) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return wire;
  };
  ProtectedRuntime runtime(binding, config);
  BOOST_CHECK_THROW(runtime.verifyGrant(binding, now), std::runtime_error);
  BOOST_CHECK(runtime.state() == ProtectedRuntimeState::FailedClosed);
}

BOOST_FIXTURE_TEST_CASE(ProtectedRuntimeCancellationDuringFetchNeverAuthorizes, BoundGrantFixture)
{
  bool cancelled = false;
  config.shouldCancel = [&] { return cancelled; };
  config.fetchGrant = [&] (const auto&) { cancelled = true; return wire; };
  ProtectedRuntime runtime(binding, config);
  BOOST_CHECK_THROW(runtime.verifyGrant(binding, now), std::runtime_error);
  BOOST_CHECK(runtime.state() == ProtectedRuntimeState::FailedClosed);
}

BOOST_FIXTURE_TEST_CASE(ProtectedRuntimeCancellationBeforeConsumptionDrains, BoundGrantFixture)
{
  bool cancelled = false;
  bool cleaned = false;
  config.shouldCancel = [&] { return cancelled; };
  ProtectedRuntime runtime(binding, config);
  runtime.verifyGrant(binding, now);
  runtime.registerHostPlaintextLease("model", [&] { cleaned = true; });
  cancelled = true;
  BOOST_CHECK_THROW(runtime.withContentKey(now, [] (const auto&) {}), std::runtime_error);
  BOOST_CHECK(cleaned);
  BOOST_CHECK(runtime.state() == ProtectedRuntimeState::Zeroized);
}

BOOST_FIXTURE_TEST_CASE(NativeProtectedStoreSealsAndAuthenticatesAllEntryKinds, BoundGrantFixture)
{
  ProtectedRuntime runtime(binding, config);
  runtime.verifyGrant(binding, now);
  for (const auto& kind : {"MODEL_PROTO", "EXTERNAL_DATA"}) {
    const NativeAssembledEntryContext context{
      config.modelManifestDigest, binding.planDigest, binding.securityPolicySnapshotDigest, kind};
    const std::vector<std::uint8_t> plain{'m', 'o', 'd', 'e', 'l'};
    runtime.withContentKey(now, [&] (const auto& key) {
      auto wire = sealNativeAssembledEntry(key, plain, context);
      BOOST_CHECK(openNativeAssembledEntry(key, wire, context, plain.size()) == plain);
      auto wrongKey = key;
      wrongKey.front() ^= 1;
      BOOST_CHECK_THROW(openNativeAssembledEntry(wrongKey, wire, context, plain.size()), std::runtime_error);
      BOOST_CHECK_THROW(openNativeAssembledEntry(key, wire, context, plain.size() - 1), std::runtime_error);
      wire.back() ^= 1;
      BOOST_CHECK_THROW(openNativeAssembledEntry(key, wire, context, plain.size()), std::runtime_error);
    });
  }
}

BOOST_FIXTURE_TEST_CASE(NativeProtectedDirectoryCleansOriginalAndPreservesReplacement, BoundGrantFixture)
{
  char pattern[] = "/tmp/spec181-native-lease-XXXXXX";
  const auto root = std::filesystem::path(::mkdtemp(pattern));
  const auto staging = root / "staging";
  std::filesystem::create_directory(staging);
  std::filesystem::permissions(staging, std::filesystem::perms::owner_all);
  ProtectedRuntime runtime(binding, config);
  runtime.verifyGrant(binding, now);
  registerNativePlaintextDirectory(runtime, staging, "assembly");
  std::ofstream(root / "canonical") << "shared model";
  std::ofstream(staging / "model.onnx") << "owned plaintext";
  std::filesystem::create_symlink(root / "canonical", staging / "source-link");
  std::filesystem::rename(staging, root / "moved");
  std::filesystem::create_directory(staging);
  std::ofstream(staging / "replacement") << "keep";
  BOOST_CHECK_THROW(runtime.cancel("cancelled"), std::runtime_error);
  BOOST_CHECK(!std::filesystem::exists(root / "moved/model.onnx"));
  BOOST_CHECK(std::filesystem::exists(staging / "replacement"));
  BOOST_CHECK_EQUAL(std::filesystem::file_size(root / "canonical"), 12);
  std::filesystem::remove_all(root);
}
}
