#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRunnerReuseCache.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace ndnsf::di {

namespace {

std::uint64_t
providerRunnerReuseNowMs()
{
  return static_cast<std::uint64_t>(std::max<std::int64_t>(0,
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count()));
}

bool
isCandidateArtifactIdentity(const NativeJson& value)
{
  if (!value.is_object() || value.size() != 7)
    return false;
  for (const auto* field : {"artifact_digest", "model_manifest_digest", "graph_digest",
                            "role", "backend", "protection_epoch"}) {
    if (!value.contains(field) || !value.at(field).is_string() ||
        value.at(field).get<std::string>().empty())
      return false;
  }
  const auto& rank = value.at("rank");
  return rank.is_number_unsigned() ||
    (rank.is_number_integer() && rank.get<std::int64_t>() >= 0);
}

std::string
runnerReuseKey(const NativeSelectionProjectionV3& projection,
               const std::string& providerIdentity,
               const std::string& providerBootId,
               const std::shared_ptr<ProtectedRuntime>& protectedRuntime)
{
  std::ostringstream canonical;
  const auto frame = [&canonical] (const std::string& value) {
    canonical << value.size() << ':' << value;
  };
  const auto frameUint = [&frame] (std::uint64_t value) {
    frame(std::to_string(value));
  };
  const auto frameStrings = [&frame, &frameUint] (
    const std::vector<std::string>& values) {
    frameUint(values.size());
    for (const auto& value : values)
      frame(value);
  };
  const auto frameTensors = [&frame, &frameUint] (
    const std::vector<NativeAssemblyTensorContractV3>& tensors) {
    frameUint(tensors.size());
    for (const auto& tensor : tensors) {
      frame(tensor.name);
      frame(tensor.dtype);
      frameUint(tensor.shape.size());
      for (const auto& dimension : tensor.shape) {
        if (std::holds_alternative<std::int64_t>(dimension)) {
          frame("i");
          frame(std::to_string(std::get<std::int64_t>(dimension)));
        }
        else {
          frame("s");
          frame(std::get<std::string>(dimension));
        }
      }
    }
  };
  const auto& role = projection.assembly;
  canonical << "ndnsf-di-live-runner-v2";
  frame(projection.provider.empty() ? providerIdentity : projection.provider);
  frame(providerBootId);
  frame(projection.canonicalArtifactName);
  // These fields identify the immutable runner contract.  Request/attempt
  // admission material is deliberately absent; it is revalidated before
  // lookup and by the protected residency authority.
  frame(role.role);
  frame(role.selectedRole);
  frameUint(role.rank);
  frameUint(role.layerBegin);
  frameUint(role.layerEnd);
  frame(role.backend);
  frameStrings(role.deviceSet);
  frame(role.artifactDigest);
  frame(role.recipeDigest);
  frame(role.roleKind);
  frame(role.adapterId);
  frame(role.adapterVersion);
  frame(role.modelManifestDigest);
  frame(role.artifactProfileDigest);
  frame(role.graphDigest);
  frame(role.canonicalInitializerDigest);
  frame(role.adapterDescriptorDigest);
  frame(role.assemblerDescriptorDigest);
  frame(role.backendAbi);
  frameUint(role.nodeIndices.size());
  for (const auto index : role.nodeIndices)
    frameUint(index);
  frameTensors(role.expectedInputs);
  frameTensors(role.expectedOutputs);
  // A live runner stores output scopes and passthrough tensor names in its
  // NativeModelRunnerSpec. Reusing it across a different request group would
  // publish under the previous Selection's scope. Derive these fields through
  // the same authenticated projection used by assembly, while keeping request
  // and attempt identity out of the key when the runner binding is unchanged.
  frame(projection.dataflow.terminalResponseOwner ? "terminal-output" : "role-output");
  const auto roleSpec = roleSpecFromSelectionProjectionV3(projection, {});
  frameUint(roleSpec.outputs.size());
  for (const auto& output : roleSpec.outputs) {
    frame(output.scope);
    frameStrings(output.bundleTensorNames);
  }
  frame(role.materializedRole ? "true" : "false");
  frame(role.precision);
  frame(role.quantization);
  frame(role.layout);
  frame(role.padding);
  frame(role.protectionEpoch);
  frame(role.mergeKind);
  frame(role.postprocessIdentity);
  frame(role.postprocessOutputName);
  frame(std::to_string(role.postprocessConfidenceThreshold));
  frame(role.postprocessSort);
  frameUint(role.maxSourceBytes);
  frameUint(role.maxAssembledBytes);
  frameUint(role.maxNodes);
  const auto& generation = projection.generationContract;
  frame(generation.enabled ? "true" : "false");
  frame(generation.tokenInputName);
  frameStrings(generation.stateInputNames);
  frameStrings(generation.stateOutputNames);
  frame(generation.stateSuccessorMap);
  frame(generation.positionInputPolicy);
  frame(generation.attentionMaskInputName);
  frame(generation.positionIdsInputName);
  frame(generation.cachePositionInputName);
  // EOS/tokenizer/sampling and prefix controls belong to request execution;
  // they do not alter construction of the ONNX runner/session.
  frame(protectedRuntime ? "protected" : "plaintext");
  return canonical.str();
}

std::optional<ProtectedResidentIdentityV1>
protectedResidentIdentity(const NativeSelectionProjectionV3& projection,
                          const std::string& providerIdentity,
                          const std::string& providerBootId,
                          const std::shared_ptr<ProtectedRuntime>& runtime)
{
  if (!runtime || projection.assembly.mergeKind == "NATIVE_POSTPROCESS")
    return std::nullopt;
  ProtectedResidentIdentityV1 identity;
  identity.provider = projection.provider.empty() ? providerIdentity : projection.provider;
  identity.providerBootId = providerBootId;
  identity.role = projection.assembly.selectedRole;
  identity.modelManifestDigest = projection.assembly.modelManifestDigest;
  identity.graphDigest = projection.assembly.graphDigest;
  identity.initializerDigest = projection.assembly.canonicalInitializerDigest;
  identity.artifactDigest = projection.assembly.artifactDigest;
  identity.recipeDigest = projection.assembly.recipeDigest;
  identity.backend = projection.assembly.backend;
  identity.backendAbi = projection.assembly.backendAbi;
  identity.protectionEpoch = projection.selectedRole.protectionEpoch;
  identity.planCoreDigest = projection.planCoreDigest;
  identity.planDigest = projection.planDigest;
  identity.securityPolicySnapshotDigest = projection.securityPolicySnapshotDigest;
  identity.grantDigest = projection.grantDigest;
  identity.fencingToken = runtime->binding().fencingToken;
  identity.revocationSequence = runtime->binding().revocationSequence;
  return identity;
}

class LeasedRunner final : public NativeModelRunner
{
public:
  LeasedRunner(std::shared_ptr<NativeModelRunner> inner,
               std::shared_ptr<const void> lifetime)
    : m_inner(std::move(inner)), m_lifetime(std::move(lifetime))
  {
    if (!m_inner)
      throw std::invalid_argument("live runner lease requires an inner runner");
  }

  std::map<std::string, TensorBundle>
  run(const RoleExecutionContext& context) final { return m_inner->run(context); }

  std::optional<std::map<std::string, TensorBundle>>
  runStreamed(const RoleExecutionContext& context) final
  { return m_inner->runStreamed(context); }

  const std::optional<ExecutionEvidence>&
  executionEvidence() const final { return m_inner->executionEvidence(); }

  std::optional<ExecutionEvidence>
  executionEvidenceSnapshot() const final
  { return m_inner->executionEvidenceSnapshot(); }

  std::optional<NativeRuntimeMetrics>
  runtimeMetricsSnapshot() const final
  { return m_inner->runtimeMetricsSnapshot(); }

  bool supportsOpaqueStateHandles() const final
  { return m_inner->supportsOpaqueStateHandles(); }

  std::optional<NativeOpaqueStateHandleV1>
  stateHandleSnapshot(const std::string& sessionId) const final
  { return m_inner->stateHandleSnapshot(sessionId); }

  void releaseSessionState(const std::string& sessionId) final
  { m_inner->releaseSessionState(sessionId); }

  bool supportsConversationStateTransfer() const final
  { return m_inner->supportsConversationStateTransfer(); }

  std::optional<NativeConversationStateHandleV1>
  promoteSessionStateToConversation(const std::string& sessionId,
                                    const std::string& conversationKey) final
  { return m_inner->promoteSessionStateToConversation(sessionId, conversationKey); }

  bool restoreConversationState(const NativeConversationStateHandleV1& state,
                                const std::string& sessionId) final
  { return m_inner->restoreConversationState(state, sessionId); }

  bool pauseConversationStateToHost(const NativeConversationStateHandleV1& state) final
  { return m_inner->pauseConversationStateToHost(state); }

  std::future<bool>
  prefetchConversationStateToGpu(const NativeConversationStateHandleV1& state) final
  { return m_inner->prefetchConversationStateToGpu(state); }

  bool cancelConversationStatePrefetch(const NativeConversationStateHandleV1& state) final
  { return m_inner->cancelConversationStatePrefetch(state); }

  bool releaseConversationState(const NativeConversationStateHandleV1& state) final
  { return m_inner->releaseConversationState(state); }

private:
  std::shared_ptr<NativeModelRunner> m_inner;
  std::shared_ptr<const void> m_lifetime;
};

} // namespace

struct NativeProviderRunnerReuseCache::State
{
  struct Entry
  {
    std::shared_ptr<NativeModelRunner> runner;
    std::string protectedIdentity;
    std::shared_ptr<const void> residentLease;
    std::optional<NativeProviderResidencyIdentityV4> residencyIdentity;
    std::uint64_t lastUse = 0;
  };

  explicit State(std::size_t maxEntries)
    : maxEntries(std::max<std::size_t>(1, maxEntries))
  {
  }

  const std::size_t maxEntries;
  std::mutex mutex;
  std::uint64_t sequence = 0;
  std::map<std::string, Entry> entries;
  std::weak_ptr<ProtectedResidentAuthority> authority;
};

NativeProviderRunnerReuseCache::NativeProviderRunnerReuseCache(std::size_t maxEntries)
  : m_state(std::make_shared<State>(maxEntries))
{
}

std::shared_ptr<NativeModelRunner>
NativeProviderRunnerReuseCache::lookup(
  const NativeSelectionProjectionV3& projection,
  const std::string& providerIdentity,
  const std::string& providerBootId,
  const std::shared_ptr<ProtectedRuntime>& runtime)
{
  const auto key = runnerReuseKey(projection, providerIdentity, providerBootId, runtime);
  std::shared_ptr<NativeModelRunner> runner;
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    const auto found = m_state->entries.find(key);
    if (found == m_state->entries.end())
      return {};
    found->second.lastUse = ++m_state->sequence;
    runner = found->second.runner;
  }
  if (const auto identity = protectedResidentIdentity(
        projection, providerIdentity, providerBootId, runtime)) {
    const auto authority = m_state->authority.lock();
    if (!authority)
      return {};
    auto use = authority->acquire(*identity, *runtime, providerRunnerReuseNowMs());
    auto lease = std::make_shared<ProtectedResidentAuthority::Use>(std::move(use));
    return std::make_shared<LeasedRunner>(std::move(runner), std::move(lease));
  }
  return runner;
}

void
NativeProviderRunnerReuseCache::publish(
  const NativeSelectionProjectionV3& projection,
  const std::string& providerIdentity,
  const std::string& providerBootId,
  const std::shared_ptr<ProtectedRuntime>& runtime,
  const std::shared_ptr<NativeModelRunner>& runner)
{
  if (!runner)
    return;
  const auto key = runnerReuseKey(projection, providerIdentity, providerBootId, runtime);
  const auto keyDigest = sha256TensorBytes(
    std::vector<std::uint8_t>(key.begin(), key.end()));
  const auto identity = protectedResidentIdentity(
    projection, providerIdentity, providerBootId, runtime);
  std::shared_ptr<const void> residentLease;
  std::string protectedIdentityKey;
  if (identity) {
    const auto authority = m_state->authority.lock();
    if (!authority)
      return;
    auto use = authority->acquire(*identity, *runtime, providerRunnerReuseNowMs());
    protectedIdentityKey = use.identity();
    residentLease = std::make_shared<ProtectedResidentAuthority::Use>(std::move(use));
  }
  auto deviceSet = projection.assembly.deviceSet;
  std::sort(deviceSet.begin(), deviceSet.end());
  NativeProviderResidencyIdentityV4 placementIdentity{
    projection.assembly.artifactDigest,
    projection.assembly.modelManifestDigest,
    projection.assembly.graphDigest,
    projection.assembly.role.empty() ? projection.assembly.selectedRole
                                     : projection.assembly.role,
    projection.assembly.rank,
    projection.assembly.backend,
    projection.assembly.protectionEpoch,
    std::move(deviceSet)};
  std::optional<NativeProviderResidencyIdentityV4> storedIdentity;
  if (placementIdentity.isValid())
    storedIdentity = std::move(placementIdentity);
  std::lock_guard<std::mutex> lock(m_state->mutex);
  if (m_state->entries.count(key) != 0)
    return;
  while (m_state->entries.size() >= m_state->maxEntries) {
    auto victim = m_state->entries.begin();
    for (auto it = m_state->entries.begin(); it != m_state->entries.end(); ++it) {
      if (it->second.lastUse < victim->second.lastUse)
        victim = it;
    }
    m_state->entries.erase(victim);
  }
  m_state->entries.emplace(key, State::Entry{
    runner, std::move(protectedIdentityKey), std::move(residentLease),
    std::move(storedIdentity), ++m_state->sequence});
  logRuntimeEvidence(
    std::string("NDNSF_DI_PROVIDER_PREPARATION phase=RUNNER_REUSE_PUBLISHED") +
    " requestId=" + projection.requestId +
    " provider=" + (projection.provider.empty() ? providerIdentity : projection.provider) +
    " role=" + projection.assembly.selectedRole + " keyDigest=" + keyDigest +
    " detail=live-runner");
}

NativeProviderResidencyOwnerResultV4
NativeProviderRunnerReuseCache::queryPlacementResidencyV4(
  const std::vector<NativeJson>& candidateArtifacts,
  const std::string& processEpoch,
  const std::uint64_t capturedAtMs,
  const std::uint64_t expiresAtMs) const
{
  NativeProviderResidencyOwnerResultV4 result{
    NativeProviderResidencyOwnerV4::RunnerReuseCache, false, {}};
  if (candidateArtifacts.empty() ||
      candidateArtifacts.size() > MAX_NATIVE_PLACEMENT_CANDIDATE_ARTIFACTS ||
      processEpoch.empty() || capturedAtMs == 0 || expiresAtMs <= capturedAtMs ||
      !std::all_of(candidateArtifacts.begin(), candidateArtifacts.end(),
                   isCandidateArtifactIdentity))
    return result;

  const auto state = m_state;
  if (!state)
    return result;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->entries.size() > state->maxEntries)
    return result;
  result.complete = true;
  for (const auto& item : state->entries) {
    const auto& entry = item.second;
    if (!entry.runner || !entry.residencyIdentity) {
      result.complete = false;
      result.rows.clear();
      return result;
    }
    const auto matches = std::any_of(candidateArtifacts.begin(),
      candidateArtifacts.end(), [&entry] (const NativeJson& candidate) {
        return entry.residencyIdentity->matchesCandidateArtifact(candidate);
      });
    if (!matches)
      continue;
    const auto runtimeGeneration = processEpoch + ":" +
      std::to_string(entry.runner->cacheIdentity());
    result.rows.push_back(entry.residencyIdentity->makeRow(
      "EXACT_LOADED", processEpoch, capturedAtMs, expiresAtMs,
      std::uint64_t{0}, 0.0, 0.0, runtimeGeneration));
  }
  std::sort(result.rows.begin(), result.rows.end(), [] (
    const NativeJson& left, const NativeJson& right) {
    return nativeCanonicalJson(left) < nativeCanonicalJson(right);
  });
  result.rows.erase(std::unique(result.rows.begin(), result.rows.end()),
                    result.rows.end());
  return result;
}

void
NativeProviderRunnerReuseCache::setAuthority(
  const std::shared_ptr<ProtectedResidentAuthority>& authority) noexcept
{
  m_state->authority = authority;
}

void
NativeProviderRunnerReuseCache::evictProtectedIdentity(
  const std::string& protectedIdentity) noexcept
{
  if (protectedIdentity.empty())
    return;
  std::lock_guard<std::mutex> lock(m_state->mutex);
  for (auto it = m_state->entries.begin(); it != m_state->entries.end();) {
    if (it->second.protectedIdentity == protectedIdentity)
      it = m_state->entries.erase(it);
    else
      ++it;
  }
}

void
NativeProviderRunnerReuseCache::clear() noexcept
{
  std::lock_guard<std::mutex> lock(m_state->mutex);
  m_state->entries.clear();
}

} // namespace ndnsf::di
