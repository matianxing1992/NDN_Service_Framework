#include "tests/boost-test.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRunnerReuseCache.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactPolicyAuthority.hpp"

#include <openssl/sha.h>
#include <openssl/evp.h>
#include <atomic>
#include <chrono>
#include <future>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace ndnsf::di {

struct ConversationStateStoreTestAccess
{
  static bool finalize(ConversationStateStore& store, const ConversationStateBinding& binding)
  {
    return store.finalizeSaveEligibility(binding);
  }
};

} // namespace ndnsf::di

namespace ndnsf::di::tests {
namespace {

std::string digest(char c) { return "sha256:" + std::string(64, c); }

DecodeStateIdentityV1 identity()
{
  DecodeStateIdentityV1 id;
  id.modelDigest = digest('1'); id.graphSemanticDigest = digest('2');
  id.artifactDigest = digest('3'); id.adapterDigest = digest('4');
  id.tokenizerDigest = digest('5'); id.runnerDigest = digest('6');
  id.roleName = "/Stage/0"; id.roleSplitDigest = digest('7'); id.layerEnd = 4;
  id.prefixDigest = digest('8'); id.prefixTokenCount = 3; id.positionDigest = digest('9');
  id.precision = "fp32"; id.layoutDigest = digest('a'); id.stateSchemaDigest = digest('b');
  id.stateComponentDigests = {digest('c')}; id.runtimeAbiDigest = digest('e');
  id.securityDomainDigest = digest('f'); id.providerIdentity = "/provider/B";
  id.providerBootId = "boot-b"; id.cacheEpoch = 7; id.requestId = "request-b";
  id.generationId = "generation-b";
  id.validate();
  return id;
}

// Refuses inference: this fixture isolates admission and lifetime behavior.
class ReservationRunner final : public NativeModelRunner
{
public:
  std::atomic<unsigned> imports{0}, releases{0};
  unsigned exports = 0;
  std::optional<NativePortableKvSnapshotV1> exportSnapshot;
  bool throwImport = false, badSize = false;
  bool releaseSucceeds = true;
  std::string wrongKey, failReleaseKey;
  std::function<void()> beforeReturn, beforeRelease;
  std::map<std::string, NativeConversationStateHandleV1> live;
  std::mutex mutex;

  std::map<std::string, TensorBundle> run(const RoleExecutionContext&) override
  { throw std::logic_error("reservation fixture must not run inference"); }
  bool supportsPortableConversationState() const override { return true; }
  NativePortableKvSnapshotV1 exportConversationState(
    const NativeConversationStateHandleV1&, const DecodeStateIdentityV1& id,
    const NativePortableKvLimitsV1& limits) override
  {
    ++exports;
    if (limits.cancelled && limits.cancelled()) throw std::runtime_error("fixture-export-cancelled");
    if (!exportSnapshot) throw std::logic_error("fixture-export-not-configured");
    auto result = *exportSnapshot;
    result.originIdentity = id;
    validateNativePortableKvSnapshot(result, limits);
    return result;
  }
  NativeConversationStateHandleV1 importConversationState(
    const NativePortableKvSnapshotV1& snapshot, const DecodeStateIdentityV1& id,
    const std::string& key, const NativePortableKvLimitsV1&) override
  {
    ++imports;
    if (throwImport) throw std::runtime_error("fixture-import-failure");
    NativeConversationStateHandleV1 state;
    state.opaque.providerIdentity = id.providerIdentity;
    state.opaque.providerBootId = id.providerBootId; state.opaque.role = id.roleName;
    state.opaque.sessionId = id.requestId; state.opaque.stateInferenceEpoch = id.stateInferenceEpoch;
    state.opaque.token = "ndnsf-resident-state-v1:" + std::to_string(imports.load());
    state.conversationKey = key; state.logicalBytes = snapshot.logicalBytes;
    { std::lock_guard<std::mutex> lock(mutex); live.emplace(key, state); }
    try { if (beforeReturn) beforeReturn(); }
    catch (...) { std::lock_guard<std::mutex> lock(mutex); live.erase(key); throw; }
    if (badSize) ++state.logicalBytes;
    if (!wrongKey.empty()) state.conversationKey = wrongKey;
    return state;
  }
  bool releaseConversationState(const NativeConversationStateHandleV1& state) override
  {
    ++releases;
    if (beforeRelease) beforeRelease();
    if (!releaseSucceeds || state.conversationKey == failReleaseKey) return false;
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = live.find(state.conversationKey);
    if (found == live.end() || found->second.opaque.token != state.opaque.token) return false;
    live.erase(found);
    return true;
  }
  std::size_t liveCount() { std::lock_guard<std::mutex> lock(mutex); return live.size(); }
};

struct Fixture
{
  NativeProviderRuntime runtime;
  std::shared_ptr<ReservationRunner> runner = std::make_shared<ReservationRunner>();
  NativeModelRunnerSpec spec;
  ConversationStateBinding binding;
  NativePortableKvSnapshotV1 snapshot;
  NativePortableKvLimitsV1 limits{1024, 2048, {}};

  explicit Fixture(std::size_t capacity = 8)
    : runtime(1, 8, 1024, 8, capacity, 8, capacity, 8)
  {
    spec.role = "/Stage/0"; spec.metadata["evidence.providerName"] = "/provider/B";
    spec.metadata["providerBootId"] = "boot-b"; spec.metadata["state.cacheEpoch"] = "7";
    runtime.registerRunner(spec, runner);
    binding.conversationId = "reservation-conversation"; binding.contextEpoch = 1;
    binding.serviceName = "/LLM/Pipeline/Generate"; binding.planRoleMapDigest = digest('0');
    binding.receiptDigest = digest('9'); binding.expiresAtMs = 10'000; binding.identity = identity();
    snapshot.originIdentity = binding.identity;
    snapshot.originIdentity.providerIdentity = "/provider/A";
    snapshot.originIdentity.providerBootId = "boot-a";
    snapshot.stateIoDigest = digest('8');
    NativePortableKvComponentV1 component;
    component.inputName = "state_in"; component.elementType = TensorElementType::UInt8;
    component.shape = {4}; component.payloadBytes = 4;
    component.tensorWire = encodeTensorBundle({NamedTensor{"state_in", TensorElementType::UInt8, {4}, {1, 2, 3, 4}}});
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(component.tensorWire.data(), component.tensorWire.size(), hash);
    std::ostringstream out; out << "sha256:" << std::hex << std::setfill('0');
    for (auto byte : hash) out << std::setw(2) << unsigned(byte);
    component.contentDigest = out.str();
    snapshot.logicalBytes = 4; snapshot.encodedBytes = component.tensorWire.size();
    snapshot.components.push_back(std::move(component));
  }
  ~Fixture() { runner->beforeReturn = {}; runner->beforeRelease = {}; }
  bool import(const ConversationStateBinding& target)
  { return runtime.importConversationState(snapshot, target, runner, limits, 100); }
  ConversationStateBinding second() const
  {
    auto result = binding; result.contextEpoch = 2;
    result.identity.requestId = "second-request"; result.identity.generationId = "second-generation";
    return result;
  }
  ConversationStateBinding commit(const ConversationStateBinding& staged)
  {
    BOOST_REQUIRE(runtime.commitStagedDecodeStatePromotion(staged, digest('a')));
    auto result = staged; result.checkpointDigest = digest('a'); return result;
  }
};

} // namespace

BOOST_AUTO_TEST_SUITE(Spec221KvStateLifecycle)

BOOST_AUTO_TEST_CASE(ScopeControlsAreBoundedWithoutPreventingAFullStoreFence)
{
  auto store = std::make_shared<ConversationStateStore>(16, 2, 16, 0, 1000);
  auto first = store->tryAcquireConversationExecution("/owner", "/service", "a");
  auto second = store->tryAcquireConversationExecution("/owner", "/service", "b");
  BOOST_REQUIRE(first);
  BOOST_REQUIRE(second);
  BOOST_CHECK(!store->tryAcquireConversationExecution("/owner", "/service", "c"));
  auto parent = store->beginProviderWithdrawalSaveFence("withdrawal", 1);
  BOOST_REQUIRE(parent);
  BOOST_CHECK(!store->isSaveFenceQuiescent(*parent));
  first.reset(); second.reset();
  BOOST_CHECK(store->isSaveFenceQuiescent(*parent));
  parent.reset();
  std::vector<ConversationStateStore::SaveFence> fences;
  for (size_t i = 0; i < 5; ++i) {
    auto fence = store->beginConversationSaveFence("/owner", "/service", std::to_string(i));
    BOOST_REQUIRE(fence);
    fences.push_back(std::move(*fence));
  }
  BOOST_CHECK(!store->beginConversationSaveFence("/owner", "/service", "overflow"));
  fences.clear();
  BOOST_CHECK(store->tryAcquireConversationExecution("/owner", "/service", "c"));
  BOOST_CHECK_THROW(store->tryAcquireConversationExecution(
    "/owner", "/service", std::string(4097, 'a')), std::invalid_argument);
  BOOST_CHECK_THROW(store->beginConversationSaveFence(
    "/owner", "/" + std::string(4097, 's'), "a"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(CommittedMetadataKeepsOwnerIsolationAfterExecutionAdmissionEnds)
{
  Fixture fixture;
  auto binding = fixture.binding;
  auto& id = binding.identity;
  ConversationSaveMetadataV1 metadata;
  metadata.authenticatedRequester = "/owner/A";
  auto& receipt = metadata.receipt;
  receipt.requesterIdentity = metadata.authenticatedRequester;
  receipt.conversationId = binding.conversationId;
  receipt.parentContextEpoch = binding.contextEpoch - 1;
  receipt.successorContextEpoch = binding.contextEpoch;
  receipt.originRequestId = id.requestId;
  receipt.originGenerationId = id.generationId;
  receipt.serviceName = binding.serviceName;
  receipt.securityDomainDigest = id.securityDomainDigest;
  receipt.modelDigest = id.modelDigest;
  receipt.graphSemanticDigest = id.graphSemanticDigest;
  receipt.adapterDigest = id.adapterDigest;
  receipt.roleName = id.roleName;
  receipt.roleSplitDigest = id.roleSplitDigest;
  receipt.layoutDigest = id.layoutDigest;
  receipt.planRoleMapDigest = binding.planRoleMapDigest;
  receipt.providerIdentity = id.providerIdentity;
  receipt.providerBootId = id.providerBootId;
  receipt.cacheEpoch = id.cacheEpoch;
  receipt.prefixDigest = id.prefixDigest;
  receipt.prefixTokenCount = id.prefixTokenCount;
  receipt.positionDigest = id.positionDigest;
  receipt.stateSchemaDigest = id.stateSchemaDigest;
  receipt.stateComponentDigests = id.stateComponentDigests;
  receipt.expiresAtMs = binding.expiresAtMs;
  binding.receiptDigest = receipt.computedDigest();
  auto store = std::make_shared<ConversationStateStore>(16, 4, 16, 4, 10000);
  store->setProviderBinding(id.providerIdentity, id.providerBootId, id.cacheEpoch);
  auto admission = store->tryAcquireConversationExecution(metadata.authenticatedRequester,
    binding.serviceName, binding.conversationId);
  BOOST_REQUIRE(admission);
  const auto handle = fixture.runner->importConversationState(fixture.snapshot, id,
    "committed-source-key", fixture.limits);
  fixture.runner->exportSnapshot = fixture.snapshot;
  BOOST_REQUIRE(store->stageAdapterPromotion(id.requestId, id.roleName, binding, handle,
    fixture.runner, 100, metadata, &*admission));
  ConversationStateReferenceV1 reference{binding.conversationId, binding.contextEpoch,
    binding.serviceName, binding.planRoleMapDigest, digest('a'), id.roleName,
    binding.receiptDigest, binding.expiresAtMs};
  BOOST_CHECK(store->resolveStaged(reference, 101, "/owner/A"));
  BOOST_CHECK(!store->resolveStaged(reference, 101, "/owner/B"));
  BOOST_REQUIRE(store->commitStagedPromotion(binding, digest('a')));
  binding.checkpointDigest = digest('a');
  BOOST_REQUIRE(ConversationStateStoreTestAccess::finalize(*store, binding));
  admission.reset();
  BOOST_CHECK(store->resolve(reference, 101, "/owner/A"));
  BOOST_CHECK(!store->resolve(reference, 101, "/owner/B"));
  auto foreign = store->beginConversationSaveFence("/owner/B", binding.serviceName,
    binding.conversationId);
  BOOST_REQUIRE(foreign);
  auto denied = store->tryAcquireCommittedSaveBatch(*foreign, {4, 65536, 16}, 101);
  BOOST_CHECK(denied.status != ConversationSaveBatchStatusV1::Ready);
  BOOST_CHECK(!denied.batch);
  BOOST_CHECK_EQUAL(fixture.runner->exports, 0U);
  foreign.reset();
  auto fence = store->beginConversationSaveFence("/owner/A", binding.serviceName,
    binding.conversationId);
  BOOST_REQUIRE(fence);
  auto small = store->tryAcquireCommittedSaveBatch(*fence, {4, 65536, 16}, 101);
  BOOST_CHECK(small.status == ConversationSaveBatchStatusV1::BoundsExceeded);
  BOOST_CHECK(!small.batch);
  auto saved = store->tryAcquireCommittedSaveBatch(*fence, {4, 65536, 65536}, 101);
  BOOST_REQUIRE(saved.status == ConversationSaveBatchStatusV1::Ready);
  BOOST_REQUIRE(saved.batch);
  BOOST_REQUIRE_EQUAL(saved.batch->size(), 1U);
  BOOST_CHECK_EQUAL(saved.batch->rows().front().metadata.authenticatedRequester, "/owner/A");
  saved.batch.reset(); fence.reset();
  store->clear();
  BOOST_CHECK_EQUAL(fixture.runner->liveCount(), 0U);
}

BOOST_AUTO_TEST_CASE(ImportedOwnerSurvivesCommitAndAdmissionRelease)
{
  Fixture fixture;
  auto admission = fixture.runtime.tryAcquireConversationExecution("/owner/A",
    fixture.binding.serviceName, fixture.binding.conversationId);
  BOOST_REQUIRE(admission);
  auto owned = fixture.runtime.importConversationStateOwned(fixture.snapshot,
    fixture.binding, fixture.runner, fixture.limits, 100, "/owner/A", &*admission);
  BOOST_REQUIRE(owned);
  BOOST_REQUIRE(fixture.runtime.commitImportedConversationState(*owned, digest('a')));
  admission.reset();
  const auto& binding = fixture.binding;
  ConversationStateReferenceV1 reference{binding.conversationId, binding.contextEpoch,
    binding.serviceName, binding.planRoleMapDigest, digest('a'), binding.identity.roleName,
    binding.receiptDigest, binding.expiresAtMs};
  BOOST_CHECK(fixture.runtime.resolveConversationState(reference, 101, "/owner/A"));
  BOOST_CHECK(!fixture.runtime.resolveConversationState(reference, 101, "/owner/B"));
  BOOST_CHECK(fixture.runtime.releaseImportedConversationState(*owned) ==
    ImportedStateReleaseResult::Released);
  BOOST_CHECK_EQUAL(fixture.runner->liveCount(), 0U);
}

BOOST_AUTO_TEST_CASE(ProtectedRunnerReusePreservesPortableKvAndOwnedLease)
{
  Fixture f;
  const auto key = [](char seed) {
    const std::string bytes(32, seed);
    auto result = std::shared_ptr<EVP_PKEY>(EVP_PKEY_new_raw_private_key(
      EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()),
      EVP_PKEY_free);
    BOOST_REQUIRE(result);
    return result;
  };
  const auto authorityKey = key('a'), requesterKey = key('b'), providerKey = key('c');
  const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count());
  NativeGrantIssuerConfig policy;
  policy.authorityIdentity = "/authority"; policy.requesterIdentity = "/requester";
  policy.protectionEpoch = "portable-wrapper-v1"; policy.keyId = "portable-wrapper-key";
  policy.authorityPrivateKey = authorityKey; policy.requesterPublicKey = requesterKey;
  policy.allowedModelManifests = {f.binding.identity.modelDigest};
  policy.recipientPublicKeys = {{"/provider/B", providerKey}};
  policy.contentKey = [](const auto&, const auto&) { return std::vector<std::uint8_t>(32, 7); };
  NativeSignedGrantRequest request;
  request.providerIdentity = "/provider/B"; request.requesterIdentity = "/requester";
  request.requestId = "/NDNSF/DI/REQUEST/portable-wrapper"; request.attempt = 1;
  request.planCoreDigest = digest('1'); request.grantViewDigest = digest('2');
  request.modelManifestDigest = f.binding.identity.modelDigest;
  request.protectionEpoch = policy.protectionEpoch; request.issuedAtMs = now;
  const auto grant = NativeArtifactGrantIssuer(policy).issue(request.sign(*requesterKey), now, now + 60000);
  ProtectedRuntimeBindingV1 binding;
  binding.provider = "/provider/B"; binding.providerBootId = "boot-b";
  binding.role = f.spec.role; binding.requestId = request.requestId; binding.attempt = 1;
  binding.planCoreDigest = request.planCoreDigest; binding.planDigest = digest('3');
  binding.securityPolicySnapshotDigest = f.binding.identity.securityDomainDigest;
  binding.protectionEpoch = policy.protectionEpoch; binding.grantName = grant.grantName;
  binding.grantDigest = grant.grantDigest; binding.fencingToken = "portable-wrapper-fence";
  binding.revocationSequence = 1;
  binding.expiresAtMs = now + 60000;
  NativeProtectedGrantConfig config;
  config.authorityIdentity = policy.authorityIdentity; config.publicationIdentity = "/requester";
  config.modelManifestDigest = request.modelManifestDigest;
  config.authorityPublicKeyRaw.resize(32);
  std::size_t publicSize = config.authorityPublicKeyRaw.size();
  BOOST_REQUIRE_EQUAL(EVP_PKEY_get_raw_public_key(authorityKey.get(),
    reinterpret_cast<unsigned char*>(&config.authorityPublicKeyRaw[0]), &publicSize), 1);
  config.recipientKey.material = std::string(32, 'c');
  config.fetchGrant = [wire = grant.wireJson](const auto&) { return wire; };
  auto protectedRuntime = std::make_shared<ProtectedRuntime>(binding, config);
  protectedRuntime->verifyGrant(binding, now);
  NativeSelectionProjectionV3 projection;
  projection.provider = binding.provider; projection.requestId = binding.requestId;
  projection.attempt = 1; projection.planCoreDigest = binding.planCoreDigest;
  projection.planDigest = binding.planDigest; projection.grantDigest = binding.grantDigest;
  projection.securityPolicySnapshotDigest = binding.securityPolicySnapshotDigest;
  projection.executionRole.roleId = binding.role;
  projection.dataflow.requestId = projection.requestId; projection.dataflow.attempt = 1;
  projection.dataflow.planDigest = projection.planDigest; projection.dataflow.role = binding.role;
  projection.dataflow.terminalResponseOwner = true; projection.canonicalArtifactName = "/model/portable-wrapper";
  auto& assembly = projection.assembly;
  assembly.role = binding.role; assembly.selectedRole = binding.role;
  assembly.modelManifestDigest = request.modelManifestDigest; assembly.graphDigest = digest('2');
  assembly.canonicalInitializerDigest = digest('3'); assembly.artifactDigest = digest('4');
  assembly.recipeDigest = digest('5'); assembly.backend = "onnxruntime";
  assembly.backendAbi = digest('6'); assembly.protectionEpoch = binding.protectionEpoch;
  projection.selectedRole = assembly;
  auto authority = std::make_shared<ProtectedResidentAuthority>();
  NativeProviderRunnerReuseCache cache(2);
  cache.setAuthority(authority);
  f.runner->exportSnapshot = f.snapshot;
  cache.publish(projection, binding.provider, binding.providerBootId, protectedRuntime, f.runner);
  auto wrapped = cache.lookup(projection, binding.provider, binding.providerBootId, protectedRuntime);
  BOOST_REQUIRE(wrapped);
  BOOST_CHECK(wrapped != f.runner);
  BOOST_CHECK(wrapped->supportsPortableConversationState());
  auto owned = f.runtime.importConversationStateOwned(f.snapshot, f.binding, wrapped, f.limits, 100);
  BOOST_REQUIRE(owned);
  BOOST_REQUIRE(f.runtime.commitImportedConversationState(*owned, digest('a')));
  auto committed = f.binding; committed.checkpointDigest = digest('a');
  const auto exported = f.runtime.exportConversationState(committed, f.limits, 101);
  BOOST_CHECK(exported.components.front().tensorWire == f.snapshot.components.front().tensorWire);
  BOOST_CHECK(exported.originIdentity == committed.identity);
  BOOST_CHECK_EQUAL(f.runner->imports.load(), 1U);
  BOOST_CHECK_EQUAL(f.runner->exports, 1U);
  auto cancelled = f.limits; cancelled.cancelled = [] { return true; };
  BOOST_CHECK_THROW(f.runtime.exportConversationState(committed, cancelled, 102), std::runtime_error);
  authority->retireAll(); cache.clear(); wrapped.reset();
  BOOST_CHECK(!authority->drain(std::chrono::milliseconds(1)));
  BOOST_CHECK(f.runtime.releaseImportedConversationState(*owned) == ImportedStateReleaseResult::Released);
  BOOST_CHECK(authority->drain(std::chrono::milliseconds(100)));
  BOOST_CHECK_EQUAL(authority->counters().activeUses, 0U);
  BOOST_CHECK_EQUAL(f.runner->liveCount(), 0U);
}

BOOST_AUTO_TEST_CASE(LazyHostBindingAdmitsOnlyItsOwnIncarnationWithoutRunnerRegistration)
{
  Fixture f;
  NativeProviderRuntime lazy;
  BOOST_CHECK(!lazy.hasRunner(f.spec.role));
  BOOST_CHECK(!lazy.importConversationStateOwned(f.snapshot, f.binding, f.runner, f.limits, 100));
  BOOST_CHECK_EQUAL(f.runner->imports.load(), 0U);
  BOOST_CHECK_THROW(lazy.bindConversationStateProvider("", "boot-b", 7), std::invalid_argument);
  BOOST_CHECK_THROW(lazy.bindConversationStateProvider("/provider/B", "", 7), std::invalid_argument);
  BOOST_CHECK_THROW(lazy.bindConversationStateProvider("/provider/B", "boot-b", 0), std::invalid_argument);
  lazy.bindConversationStateProvider("/provider/B", "boot-b", 7);
  auto foreign = f.binding;
  foreign.identity.providerBootId = "foreign-boot";
  BOOST_CHECK(!lazy.importConversationStateOwned(f.snapshot, foreign, f.runner, f.limits, 100));
  foreign = f.binding;
  foreign.identity.providerIdentity = "/provider/C";
  BOOST_CHECK(!lazy.importConversationStateOwned(f.snapshot, foreign, f.runner, f.limits, 100));
  foreign = f.binding;
  ++foreign.identity.cacheEpoch;
  BOOST_CHECK(!lazy.importConversationStateOwned(f.snapshot, foreign, f.runner, f.limits, 100));
  BOOST_CHECK_EQUAL(f.runner->imports.load(), 0U);
  auto owned = lazy.importConversationStateOwned(f.snapshot, f.binding, f.runner, f.limits, 100);
  BOOST_REQUIRE(owned);
  BOOST_CHECK_EQUAL(f.runner->imports.load(), 1U);
  BOOST_REQUIRE(lazy.commitImportedConversationState(*owned, digest('a')));
  BOOST_CHECK(!lazy.hasRunner(f.spec.role));
  BOOST_CHECK(lazy.releaseImportedConversationState(*owned) == ImportedStateReleaseResult::Released);
  BOOST_CHECK_EQUAL(f.runner->liveCount(), 0U);
  BOOST_CHECK_EQUAL(lazy.conversationStateSnapshot().gpuBytes, 0U);
}

BOOST_AUTO_TEST_CASE(ImportTicketCannotCrossAuthenticatedRequesterFence)
{
  Fixture f;
  auto requesterAdmission = f.runtime.tryAcquireConversationExecution(
    "/requester/A", f.binding.serviceName, f.binding.conversationId);
  BOOST_REQUIRE(requesterAdmission);
  auto requesterFence = f.runtime.beginConversationSaveFence(
    "/requester/A", f.binding.serviceName, f.binding.conversationId);
  BOOST_REQUIRE(requesterFence);

  // The already admitted A operation may finish under its fence, but its
  // ticket cannot be replayed to authorize B's import for the same NDN scope.
  BOOST_CHECK(!f.runtime.importConversationStateOwned(
    f.snapshot, f.binding, f.runner, f.limits, 100, "/requester/B",
    &*requesterAdmission));
  BOOST_CHECK_EQUAL(f.runner->imports.load(), 0U);
  BOOST_CHECK(!f.runtime.isSaveFenceQuiescent(*requesterFence));
  requesterAdmission.reset();
  BOOST_CHECK(f.runtime.isSaveFenceQuiescent(*requesterFence));
}

BOOST_AUTO_TEST_CASE(OwnedAdmissionSurvivesCommitAndDefeatsIdenticalBindingAba)
{
  Fixture f, foreign;
  auto ownership = f.runtime.importConversationStateOwned(f.snapshot, f.binding, f.runner, f.limits, 100);
  BOOST_REQUIRE(ownership);
  BOOST_CHECK(!foreign.runtime.commitImportedConversationState(*ownership, digest('a')));
  BOOST_CHECK_THROW(foreign.runtime.releaseImportedConversationState(*ownership), std::invalid_argument);
  BOOST_REQUIRE(f.runtime.commitImportedConversationState(*ownership, digest('a')));
  BOOST_CHECK(!f.runtime.commitImportedConversationState(*ownership, digest('a')));
  BOOST_CHECK(f.runtime.releaseImportedConversationState(*ownership) == ImportedStateReleaseResult::Released);
  BOOST_CHECK(f.runtime.releaseImportedConversationState(*ownership) == ImportedStateReleaseResult::AlreadyGone);
  BOOST_CHECK_EQUAL(f.runtime.conversationStateSnapshot().gpuBytes, 0U);

  // Recreate exactly the same logical key and binding. Old cleanup/commit must
  // neither erase this entry nor erase its Runtime staged side index.
  auto next = f.runtime.importConversationStateOwned(f.snapshot, f.binding, f.runner, f.limits, 100);
  BOOST_REQUIRE(next);
  BOOST_CHECK(!f.runtime.commitImportedConversationState(*ownership, digest('a')));
  BOOST_CHECK(f.runtime.releaseImportedConversationState(*ownership) == ImportedStateReleaseResult::AlreadyGone);
  BOOST_CHECK_EQUAL(f.runner->liveCount(), 1U);
  BOOST_CHECK_EQUAL(f.runtime.conversationStateSnapshot().gpuBytes, 4U);
  BOOST_REQUIRE(f.runtime.commitImportedConversationState(*next, digest('b')));
  BOOST_CHECK(f.runtime.releaseImportedConversationState(*next) == ImportedStateReleaseResult::Released);
  BOOST_CHECK_EQUAL(f.runner->imports.load(), 2U);
  BOOST_CHECK_EQUAL(f.runner->releases.load(), 2U);
}

BOOST_AUTO_TEST_CASE(OwnedCleanupWaitsForUseAndRetainsFailedReleaseCharge)
{
  Fixture f;
  BOOST_REQUIRE(f.import(f.binding));
  const auto earlier = f.commit(f.binding);
  auto binding = f.second();
  auto ownership = f.runtime.importConversationStateOwned(f.snapshot, binding, f.runner, f.limits, 100);
  BOOST_REQUIRE(ownership);
  BOOST_REQUIRE(f.runtime.commitImportedConversationState(*ownership, digest('b')));
  binding.checkpointDigest = digest('b');
  auto use = f.runtime.acquireConversationUse(binding, 101);
  BOOST_REQUIRE(use);
  BOOST_CHECK(f.runtime.releaseImportedConversationState(*ownership) == ImportedStateReleaseResult::Busy);
  BOOST_CHECK_EQUAL(f.runner->releases.load(), 0U);
  use.reset();
  f.runner->releaseSucceeds = false;
  BOOST_CHECK(f.runtime.releaseImportedConversationState(*ownership) == ImportedStateReleaseResult::CleanupUnconfirmed);
  BOOST_CHECK_EQUAL(f.runner->liveCount(), 2U);
  BOOST_CHECK_EQUAL(f.runtime.conversationStateSnapshot().gpuBytes, 8U);
  BOOST_CHECK(f.runtime.releaseImportedConversationState(*ownership) == ImportedStateReleaseResult::CleanupUnconfirmed);
  BOOST_CHECK_EQUAL(f.runner->releases.load(), 1U);
  f.runner->releaseSucceeds = true;
  BOOST_REQUIRE(f.runtime.retryConversationStateCleanup());
  BOOST_CHECK(f.runtime.releaseImportedConversationState(*ownership) == ImportedStateReleaseResult::AlreadyGone);
  BOOST_CHECK_EQUAL(f.runner->liveCount(), 1U);
  BOOST_CHECK_EQUAL(f.runtime.conversationStateSnapshot().gpuBytes, 4U);
  BOOST_REQUIRE(f.runtime.releaseConversationState(earlier));
}

BOOST_AUTO_TEST_CASE(OwnedStagedCleanupAndDroppedOwnershipKeepExplicitSemantics)
{
  Fixture f;
  auto ownership = f.runtime.importConversationStateOwned(f.snapshot, f.binding, f.runner, f.limits, 100);
  BOOST_REQUIRE(ownership);
  BOOST_CHECK(f.runtime.releaseImportedConversationState(*ownership) == ImportedStateReleaseResult::Released);
  BOOST_CHECK_EQUAL(f.runtime.conversationStateSnapshot().gpuBytes, 0U);
  auto next = f.runtime.importConversationStateOwned(f.snapshot, f.binding, f.runner, f.limits, 100);
  BOOST_REQUIRE(next);
  BOOST_REQUIRE(f.runtime.commitImportedConversationState(*next, digest('a')));
  next.reset();
  BOOST_CHECK_EQUAL(f.runner->liveCount(), 1U);
  BOOST_CHECK_EQUAL(f.runtime.conversationStateSnapshot().gpuBytes, 4U);
  auto committed = f.binding; committed.checkpointDigest = digest('a');
  BOOST_REQUIRE(f.runtime.releaseConversationState(committed));
}

BOOST_AUTO_TEST_CASE(ImportedOriginIsExactAndCurrentContinuationUsesFreshDestination)
{
  Fixture f;
  f.snapshot.originIdentity.requestId = "actual-earlier-request";
  f.snapshot.originIdentity.generationId = "actual-earlier-generation";
  auto binding = f.binding;
  binding.importedOriginIdentity = f.snapshot.originIdentity;
  auto changed = binding;
  changed.importedOriginIdentity->providerBootId = "other-origin-boot";
  BOOST_CHECK(!(binding == changed));
  BOOST_CHECK(!f.runtime.importConversationStateOwned(f.snapshot, changed, f.runner, f.limits, 100));
  BOOST_CHECK_EQUAL(f.runner->imports.load(), 0U);
  auto ownership = f.runtime.importConversationStateOwned(f.snapshot, binding, f.runner, f.limits, 100);
  BOOST_REQUIRE(ownership);
  BOOST_REQUIRE(f.runtime.commitImportedConversationState(*ownership, digest('a')));
  binding.checkpointDigest = digest('a');
  changed.checkpointDigest = binding.checkpointDigest;
  auto use = f.runtime.acquireConversationUse(changed, 101);
  BOOST_CHECK(!use);
  auto current = binding.identity;
  current.prefixDigest.clear(); current.positionDigest.clear(); current.prefixTokenCount = 0;
  current.stateInferenceEpoch = 0; current.predecessorInferenceEpoch.reset();
  BOOST_CHECK(nativeConversationStateAllowsContinuation(binding, current));
  auto wrong = current; wrong.providerBootId = "wrong-destination-boot";
  BOOST_CHECK(!nativeConversationStateAllowsContinuation(binding, wrong));
  wrong = current; wrong.modelDigest = digest('0');
  BOOST_CHECK(!nativeConversationStateAllowsContinuation(binding, wrong));
  wrong = current; wrong.requestId = "mixed-request";
  BOOST_CHECK(!nativeConversationStateAllowsContinuation(binding, wrong));
  auto later = current; later.requestId = "later-request"; later.generationId = "later-generation";
  BOOST_CHECK(nativeConversationStateAllowsContinuation(binding, later));
  auto ordinary = binding; ordinary.importedOriginIdentity.reset();
  BOOST_CHECK(!nativeConversationStateAllowsContinuation(ordinary, current));
  BOOST_CHECK(nativeConversationStateAllowsContinuation(ordinary, later));
  BOOST_CHECK(f.runtime.releaseImportedConversationState(*ownership) == ImportedStateReleaseResult::Released);
}

BOOST_AUTO_TEST_CASE(CapacityAndDestinationRejectBeforeBackend)
{
  Fixture f(4);
  BOOST_REQUIRE(f.import(f.binding));
  const auto committed = f.commit(f.binding);
  BOOST_CHECK(!f.import(f.second()));
  BOOST_CHECK(!f.import(f.binding));
  auto bad = f.second(); bad.identity.providerBootId = "wrong-boot";
  BOOST_CHECK(!f.import(bad));
  BOOST_CHECK_EQUAL(f.runner->imports.load(), 1U);
  BOOST_CHECK_EQUAL(f.runtime.conversationStateSnapshot().gpuBytes, 4U);
  BOOST_CHECK_EQUAL(f.runner->liveCount(), 1U);
  BOOST_REQUIRE(f.runtime.releaseConversationState(committed));
}

BOOST_AUTO_TEST_CASE(BackendThrowRefundsOnlyPlaceholder)
{
  Fixture f;
  BOOST_REQUIRE(f.import(f.binding));
  const auto committed = f.commit(f.binding);
  f.runner->throwImport = true;
  BOOST_CHECK_THROW(f.import(f.second()), std::runtime_error);
  BOOST_CHECK_EQUAL(f.runner->releases.load(), 0U);
  BOOST_CHECK_EQUAL(f.runner->liveCount(), 1U);
  BOOST_CHECK_EQUAL(f.runtime.conversationStateSnapshot().gpuBytes, 4U);
  BOOST_CHECK_EQUAL(f.runtime.conversationStateDrainSnapshot().activeTransitions, 0U);
  f.runner->throwImport = false;
  BOOST_CHECK_EQUAL(f.runtime.conversationStateSnapshot().stagedPromotions, 2U);
  BOOST_CHECK_EQUAL(f.runtime.conversationStateSnapshot().promotionRollbacks, 1U);
  BOOST_REQUIRE(f.import(f.second()));
  BOOST_REQUIRE(f.runtime.rollbackStagedDecodeStatePromotion(f.second()));
  BOOST_REQUIRE(f.runtime.releaseConversationState(committed));
}

BOOST_AUTO_TEST_CASE(CancelAfterAllocationReleasesCandidate)
{
  Fixture f;
  std::atomic<bool> cancelled{false};
  f.limits.cancelled = [&] { return cancelled.load(); };
  f.runner->beforeReturn = [&] { cancelled = true; };
  BOOST_CHECK(!f.import(f.binding));
  BOOST_CHECK_EQUAL(f.runner->imports.load(), 1U);
  BOOST_CHECK_EQUAL(f.runner->releases.load(), 1U);
  BOOST_CHECK_EQUAL(f.runner->liveCount(), 0U);
  BOOST_CHECK_EQUAL(f.runtime.conversationStateSnapshot().gpuBytes, 0U);
  BOOST_CHECK(f.runtime.conversationStateDrainSnapshot().complete);
}

BOOST_AUTO_TEST_CASE(FailedCleanupRetainsChargeAndPreservesCommittedWinner)
{
  Fixture f;
  BOOST_REQUIRE(f.import(f.binding));
  const auto committed = f.commit(f.binding);
  f.runner->badSize = true; f.runner->releaseSucceeds = false;
  BOOST_CHECK(!f.import(f.second()));
  auto drain = f.runtime.conversationStateDrainSnapshot();
  BOOST_CHECK(!drain.complete); BOOST_CHECK(drain.closing);
  BOOST_CHECK_EQUAL(drain.cleanupFailures, 1U);
  BOOST_CHECK_EQUAL(drain.unverifiedOccupancyEntries, 1U);
  BOOST_CHECK_EQUAL(drain.retainedGpuBytes, 8U);
  BOOST_CHECK_EQUAL(drain.activeTransitions, 0U);
  BOOST_CHECK_EQUAL(f.runner->liveCount(), 2U);
  BOOST_CHECK_EQUAL(f.runner->releases.load(), 1U);
  f.runner->badSize = false; f.runner->releaseSucceeds = true;
  BOOST_REQUIRE(f.runtime.retryConversationStateCleanup());
  BOOST_CHECK_EQUAL(f.runner->liveCount(), 1U);
  BOOST_CHECK_EQUAL(f.runtime.conversationStateSnapshot().gpuBytes, 4U);
  BOOST_CHECK(f.runtime.pinConversationState(committed, 101));
  BOOST_CHECK(f.runtime.unpinConversationState(committed));
  BOOST_REQUIRE(f.runtime.releaseConversationState(committed));
}

BOOST_AUTO_TEST_CASE(CancellationProbeExceptionStillCleansOwnedAllocation)
{
  Fixture f;
  bool allocated = false;
  f.limits.cancelled = [&] {
    if (allocated) throw std::runtime_error("fixture-cancellation-probe-failure");
    return false;
  };
  f.runner->beforeReturn = [&] { allocated = true; };
  BOOST_CHECK_THROW(f.import(f.binding), std::runtime_error);
  BOOST_CHECK_EQUAL(f.runner->releases.load(), 1U);
  BOOST_CHECK_EQUAL(f.runner->liveCount(), 0U);
  BOOST_CHECK_EQUAL(f.runtime.conversationStateSnapshot().gpuBytes, 0U);
  BOOST_CHECK_EQUAL(f.runtime.conversationStateDrainSnapshot().activeTransitions, 0U);
}

BOOST_AUTO_TEST_CASE(RebindDuringSelectiveRetryCompletesTheRequestedFullDrain)
{
  Fixture f;
  BOOST_REQUIRE(f.import(f.binding));
  f.commit(f.binding);
  f.runner->badSize = true; f.runner->releaseSucceeds = false;
  BOOST_CHECK(!f.import(f.second()));
  f.runner->badSize = false; f.runner->releaseSucceeds = true;
  bool rebound = false;
  f.runner->beforeRelease = [&] {
    if (rebound) return;
    rebound = true;
    f.spec.metadata["providerBootId"] = "boot-new";
    f.spec.metadata["state.cacheEpoch"] = "8";
    f.runtime.registerRunner(f.spec, f.runner);
  };
  BOOST_REQUIRE(f.runtime.retryConversationStateCleanup());
  BOOST_CHECK(rebound);
  BOOST_CHECK_EQUAL(f.runtime.conversationStateSnapshot().entries, 0U);
  BOOST_CHECK_EQUAL(f.runner->liveCount(), 0U);
  BOOST_CHECK_EQUAL(f.runtime.conversationStateDrainSnapshot().activeTransitions, 0U);
}

BOOST_AUTO_TEST_CASE(RebindDuringFailedRetryDoesNotRetryTheSameReleaseAgain)
{
  Fixture f;
  BOOST_REQUIRE(f.import(f.binding));
  f.commit(f.binding);
  std::string winnerKey;
  { std::lock_guard<std::mutex> lock(f.runner->mutex); winnerKey = f.runner->live.begin()->first; }
  f.runner->badSize = true; f.runner->releaseSucceeds = false;
  BOOST_CHECK(!f.import(f.second()));
  f.runner->badSize = false; f.runner->releaseSucceeds = true;
  {
    std::lock_guard<std::mutex> lock(f.runner->mutex);
    for (const auto& item : f.runner->live)
      if (item.first != winnerKey) f.runner->failReleaseKey = item.first;
  }
  bool rebound = false;
  f.runner->beforeRelease = [&] {
    if (rebound) return;
    rebound = true;
    f.spec.metadata["providerBootId"] = "boot-new"; f.spec.metadata["state.cacheEpoch"] = "8";
    f.runtime.registerRunner(f.spec, f.runner);
  };
  BOOST_CHECK(!f.runtime.retryConversationStateCleanup());
  BOOST_CHECK(rebound);
  // Initial failed cleanup, one explicit retry, one safe-winner full drain.
  BOOST_CHECK_EQUAL(f.runner->releases.load(), 3U);
  BOOST_CHECK_EQUAL(f.runner->liveCount(), 1U);
  BOOST_CHECK_EQUAL(f.runtime.conversationStateSnapshot().gpuBytes, 4U);
  f.runner->failReleaseKey.clear();
  BOOST_REQUIRE(f.runtime.retryConversationStateCleanup());
  BOOST_CHECK_EQUAL(f.runner->releases.load(), 4U);
  BOOST_CHECK_EQUAL(f.runner->liveCount(), 0U);
}

BOOST_AUTO_TEST_CASE(MalformedReturnedKeyCannotReleaseAnEarlierState)
{
  Fixture f;
  BOOST_REQUIRE(f.import(f.binding));
  const auto committed = f.commit(f.binding);
  { std::lock_guard<std::mutex> lock(f.runner->mutex); f.runner->wrongKey = f.runner->live.begin()->first; }
  BOOST_CHECK(!f.import(f.second()));
  BOOST_CHECK_EQUAL(f.runner->releases.load(), 0U);
  BOOST_CHECK_EQUAL(f.runner->liveCount(), 2U);
  auto drain = f.runtime.conversationStateDrainSnapshot();
  BOOST_CHECK_EQUAL(drain.unsafeCleanupEntries, 1U);
  BOOST_CHECK(!f.runtime.retryConversationStateCleanup());
  BOOST_CHECK_EQUAL(f.runner->releases.load(), 0U);
  BOOST_CHECK_EQUAL(f.runtime.conversationStateSnapshot().gpuBytes, 8U);
  BOOST_CHECK(!f.runtime.releaseConversationState(committed));
  // An explicit rebind requests full drain, but still cannot release the
  // unsafe candidate. The previously committed safe winner is cleaned once.
  f.spec.metadata["providerBootId"] = "boot-new"; f.spec.metadata["state.cacheEpoch"] = "8";
  f.runtime.registerRunner(f.spec, f.runner);
  BOOST_CHECK_EQUAL(f.runner->releases.load(), 1U);
  BOOST_CHECK_EQUAL(f.runner->liveCount(), 1U);
  BOOST_CHECK_EQUAL(f.runtime.conversationStateDrainSnapshot().unsafeCleanupEntries, 1U);
  BOOST_CHECK(!f.runtime.conversationStateDrainSnapshot().complete);
}

BOOST_AUTO_TEST_CASE(RebindWaitsForImportToSettleBeforeCleanup)
{
  Fixture f;
  std::promise<void> entered, proceed;
  auto permit = proceed.get_future().share();
  f.runner->beforeReturn = [&] { entered.set_value(); permit.wait(); };
  auto work = std::async(std::launch::async, [&] { return f.import(f.binding); });
  struct Unblock { std::promise<void>& promise; ~Unblock() { try { promise.set_value(); } catch (...) {} } } unblock{proceed};
  BOOST_REQUIRE(entered.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  f.spec.metadata["providerBootId"] = "boot-new"; f.spec.metadata["state.cacheEpoch"] = "8";
  f.runtime.registerRunner(f.spec, f.runner);
  const auto pending = f.runtime.conversationStateDrainSnapshot();
  BOOST_CHECK(!pending.complete); BOOST_CHECK_EQUAL(pending.activeTransitions, 1U);
  BOOST_CHECK_EQUAL(f.runner->releases.load(), 0U);
  BOOST_CHECK_EQUAL(pending.retainedGpuBytes, 4U);
  proceed.set_value();
  BOOST_CHECK(!work.get());
  BOOST_CHECK_EQUAL(f.runner->releases.load(), 1U);
  BOOST_CHECK_EQUAL(f.runner->liveCount(), 0U);
  BOOST_CHECK(f.runtime.conversationStateDrainSnapshot().complete);
  BOOST_CHECK_EQUAL(f.runtime.conversationStateSnapshot().entries, 0U);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
