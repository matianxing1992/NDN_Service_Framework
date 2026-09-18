#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeV3Placement.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderArtifactCache.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanSealer.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"
#include "tests/fixtures/spec182/native-sealing-fixture.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace ndnsf::di;

NativeJson oracle()
{
  std::ifstream file("tests/fixtures/spec182/placement-v3-oracle.json");
  if (!file) throw std::runtime_error("missing V3 placement oracle");
  return NativeJson::parse(file);
}

struct V3PlacementFixture
{
  NativeOfferBindingContext context;
  std::string ackClosedDigest;
  std::vector<NativeSelectionRoleV3> roles;
  std::vector<NativeAdmittedOfferV3> offers;

  V3PlacementFixture()
  {
    const auto f = oracle();
    const auto& sample = f.at("cases").at(11); // rank_cover: two roles, two Providers
    context = {"request", 1, "/service", f.at("model_digest").get<std::string>(),
               f.at("graph_digest").get<std::string>(), 900};
    ackClosedDigest = f.at("ack_digest").get<std::string>();

    NativePlanSealingInputs inputs;
    inputs.artifacts.artifactDigestByRole = {{f.at("role").at("role").get<std::string>(),
                                               nativePlanningDigest("spec189-role")}};
    inputs.artifacts.manifestDigest = f.at("role").at("model_manifest_digest").get<std::string>();
    inputs.artifacts.recipeDigest = f.at("role").at("recipe_digest").get<std::string>();
    inputs.artifacts.graphDigest = f.at("role").at("graph_digest").get<std::string>();
    inputs.artifacts.canonicalGraphDigest = f.at("role").at("graph_digest").get<std::string>();
    inputs.protectionEpoch = f.at("role").at("protection_epoch").get<std::string>();
    fixture::assemblies(inputs);
    for (const auto rank : sample.at("ranks")) {
      auto role = inputs.assemblyByRole.begin()->second;
      role.rank = rank.get<std::uint64_t>();
      role.selectedRole = role.role + "#" + std::to_string(role.rank);
      role.layerBegin = role.rank;
      role.layerEnd = role.rank + 1;
      role.artifactDigest = nativePlanningDigest(
        "spec189-layer-" + std::to_string(role.rank));
      role.backend = "onnxruntime-cuda";
      role.deviceSet = {"cuda:0"};
      role.requiredDeviceMemoryMb = 1024;
      roles.push_back(std::move(role));
    }

    NativeOfferAdmission admission(
      f.at("policy").dump(),
      {{f.at("key_id").get<std::string>(), f.at("public_pem").get<std::string>()}},
      f.at("candidate").get<std::string>());
    for (const auto& wireNode : sample.at("offers")) {
      const auto wire = wireNode.get<std::string>();
      const auto offer = decodeNativeProviderOfferV3(wire);
      ndn_service_framework::AckSelectionCandidate ack;
      ack.providerName = ndn::Name(offer.provider);
      ack.serviceName = ndn::Name(offer.service);
      ack.requestId = ndn::Name(offer.requestId);
      ack.ack.setStatus(offer.status);
      ndn::Buffer payload(wire.begin(), wire.end());
      ack.ack.setPayload(payload, payload.size());
      ack.authenticationEvidence = {
        offer.provider, offer.provider + "/KEY/fixture/issuer/v=1",
        "sha256:" + std::string(64, '1'), true};
      offers.push_back(admission.verify(ack, context, 200));
    }
  }
};

NativeSelectionProjectionV3
selectedProjection(const std::string& provider, const NativeSelectionRoleV3& role,
                   const std::string& requestId)
{
  NativeSelectionProjectionV3 projection;
  projection.provider = provider;
  projection.requestId = requestId;
  projection.canonicalArtifactName = "/spec189/repo/qwen/root";
  projection.attempt = 1;
  projection.selectedRole = role;
  projection.assembly = role;
  projection.hasGrantBinding = true;
  // Provider identities are canonical NDN names and already begin with '/'.
  // Join without introducing an empty name component.
  projection.grantName = "/spec189/grant" + provider + "/" + role.selectedRole;
  projection.grantDigest = nativePlanningDigest(provider + role.selectedRole + "-grant");
  return projection;
}

ProviderArtifactKey
keyFor(const NativeSelectionProjectionV3& projection,
       const std::string& candidateDigest, const std::string& sourceDigest,
       const std::string& sourceName)
{
  ProviderArtifactKey key;
  key.sourceDigest = sourceDigest;
  key.canonicalSourceName = sourceName;
  key.canonicalRootName = projection.canonicalArtifactName;
  key.canonicalRootDigest = projection.assembly.modelManifestDigest;
  key.initializerDigest = nativePlanningDigest("spec189-initializer");
  key.canonicalGraphDigest = nativePlanningDigest("spec189-placement-graph");
  key.role = projection.assembly.selectedRole;
  key.candidateDigest = candidateDigest;
  key.recipeDigest = projection.assembly.recipeDigest;
  key.backendAbi = projection.assembly.backendAbi;
  key.device = "cuda:0";
  key.precision = "fp32";
  key.quantization = "none";
  key.layoutDigest = nativePlanningDigest("spec189-layout");
  key.artifactProfile = nativePlanningDigest("spec189-profile");
  key.securityDomain = "spec189";
  key.protectionEpoch = projection.assembly.protectionEpoch;
  if (projection.hasGrantBinding) {
    key.protectionIdentity = projection.provider + "|" + projection.grantName + "|" +
                             projection.grantDigest;
  }
  return key;
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec189PlacementOracle)

BOOST_AUTO_TEST_CASE(TwoProviderSelectionKeepsPreSelectionEffectsZero)
{
  V3PlacementFixture fixture;
  const auto proposal = NativePreSplitFirstPlacement().proposeRoles(
    fixture.context, fixture.ackClosedDigest, fixture.roles, fixture.offers, 200);
  BOOST_REQUIRE_NO_THROW(validateNativeRolePlacement(
    proposal, fixture.roles, fixture.offers, 200));
  BOOST_REQUIRE_EQUAL(proposal.providerByRole.size(), 2U);
  const std::set<std::string> providers = {
    proposal.providerByRole.at("/LLM/Pipeline/Stage/0#0"),
    proposal.providerByRole.at("/LLM/Pipeline/Stage/0#1")};
  BOOST_REQUIRE_EQUAL(providers.size(), 2U);
  BOOST_CHECK_EQUAL(proposal.offerDigestByProvider.size(), 2U);
  BOOST_CHECK_EQUAL(proposal.roles.size(), 2U);
  for (const auto& role : proposal.roles) {
    BOOST_CHECK_EQUAL(role.layerEnd, role.layerBegin + 1);
    BOOST_CHECK(!role.artifactDigest.empty());
    BOOST_CHECK(!role.modelManifestDigest.empty());
  }

  std::atomic<unsigned> builds{0};
  const auto buildFor = [&builds] (const std::string& provider,
                                   const NativeSelectionRoleV3& role) {
    return [&, provider, role] (const NativeRequestControl&) {
      builds.fetch_add(1, std::memory_order_relaxed);
      auto artifact = std::make_shared<PreparedProviderArtifact>();
      artifact->encryptedObjectName = "/spec189/repo/qwen/" + provider + "/" +
                                      role.selectedRole + "/segment=0";
      artifact->ciphertextDigest = nativePlanningDigest(
        "spec189-ciphertext-" + provider + role.selectedRole);
      artifact->formatVersion = "spec189-provider-artifact-v1";
      artifact->canonicalMetadataJson = "{}";
      artifact->ciphertextBytes = 64;
      return ProviderArtifactCache::BuildResult{std::move(artifact), {}};
    };
  };
  NativeRequestControl control;
  control.requestId = fixture.context.requestId;
  control.attempt = fixture.context.attempt;
  control.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  control.cancelled = [] { return false; };

  std::map<std::string, std::unique_ptr<ProviderArtifactCache>> caches;
  for (const auto& provider : providers)
    caches.emplace(provider, std::make_unique<ProviderArtifactCache>(
      ProviderArtifactCacheConfig{1U << 20, 2, std::chrono::seconds(2)}));

  const auto roleForProvider = [&] (const std::string& provider) -> const NativeSelectionRoleV3& {
    for (const auto& role : proposal.roles)
      if (proposal.providerByRole.at(role.selectedRole) == provider) return role;
    throw std::logic_error("placement did not bind a role to the Provider");
  };
  // A non-empty but incomplete authenticated projection is rejected before
  // the builder. This is a cache-layer subcheck for the post-Selection
  // identity gate; it does not claim to replace a full Core/Provider flow.
  const auto& firstProvider = *providers.begin();
  const auto& firstRole = roleForProvider(firstProvider);
  auto incomplete = selectedProjection(firstProvider, firstRole,
                                       fixture.context.requestId);
  incomplete.grantName.clear();
  incomplete.grantDigest.clear();
  const auto incompleteKey = keyFor(
    incomplete, nativePlanningDigest("spec189-candidate"),
    firstRole.artifactDigest, "/spec189/repo/qwen/source/0");
  BOOST_CHECK_EXCEPTION(
    caches.at(firstProvider)->acquireWithRunner(
      incompleteKey, incomplete, control, buildFor(firstProvider, firstRole)),
    std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find("authenticated projection lacks grant identity") !=
             std::string::npos;
    });
  BOOST_CHECK_EQUAL(builds.load(std::memory_order_relaxed), 0U);

  // Only after the production V3 placement result has supplied a complete
  // grant-bound projection may each Provider build its own exact layer. This
  // remains a cache-layer oracle; a full Core/Provider Selection run must
  // separately prove that no fetch occurs before this projection exists.
  for (const auto& item : proposal.providerByRole) {
    const auto& provider = item.second;
    const auto& role = roleForProvider(provider);
    const auto projection = selectedProjection(
      provider, role, fixture.context.requestId);
    const auto key = keyFor(
      projection, nativePlanningDigest("spec189-candidate"),
      role.artifactDigest, "/spec189/repo/qwen/source/" + std::to_string(role.rank));
    {
      auto lease = caches.at(provider)->acquireWithRunner(
        key, projection, control, buildFor(provider, role));
      BOOST_REQUIRE(lease.valid());
    }
  }
  BOOST_CHECK_EQUAL(builds.load(std::memory_order_relaxed), 2U);
  for (auto& item : caches) item.second->stop();
}

BOOST_AUTO_TEST_SUITE_END()
