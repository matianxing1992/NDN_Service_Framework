#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-entry-runner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-source-transform.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace ndnsf::di {

namespace {

struct RunnerCacheIdentityRegistry
{
  std::mutex mutex;
  std::unordered_map<const NativeModelRunner*, std::uint64_t> identities;
  std::atomic<std::uint64_t> next{1};
};

RunnerCacheIdentityRegistry&
runnerCacheIdentityRegistry()
{
  static RunnerCacheIdentityRegistry registry;
  return registry;
}

}

NativeModelRunner::NativeModelRunner()
{
  auto& registry = runnerCacheIdentityRegistry();
  const auto identity = registry.next.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(registry.mutex);
  registry.identities.emplace(this, identity);
}

NativeModelRunner::~NativeModelRunner() noexcept
{
  auto& registry = runnerCacheIdentityRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  registry.identities.erase(this);
}

std::shared_ptr<const AutomaticOnnxRequestBinding>
NativeModelRunner::validateAutomaticRequestBinding(
  const NativeSelectionProjectionV3&, const NativeRequestControl& control) const
{
  control.requireActive();
  throw std::invalid_argument("AUTOMATIC_SOURCE_RUNNER_UNSUPPORTED");
}

namespace {

std::vector<NamedTensor>
makeLegacyStateFeedback(const std::vector<NamedTensorView>& raw,
                        const std::vector<std::string>& inputNames,
                        const std::vector<std::string>& outputNames,
                        const RoleExecutionContext& ctx)
{
  if (ctx.automaticBinding)
    throw std::invalid_argument("AUTOMATIC_STATE_FEEDBACK_RUNNER_UNSUPPORTED");
  if (raw.size() != inputNames.size() || inputNames.size() != outputNames.size() ||
      inputNames.empty())
    throw std::invalid_argument("Provider decode-state input/output metadata length mismatch");
  std::vector<NamedTensor> result;
  result.reserve(raw.size());
  for (std::size_t index = 0; index < raw.size(); ++index) {
    if (ctx.executionGuard) ctx.executionGuard();
    validateNamedTensorView(raw[index]);
    if (raw[index].name != outputNames[index])
      throw std::invalid_argument("Provider decode-state output name mismatch");
    result.push_back(NamedTensor{inputNames[index], raw[index].elementType, raw[index].shape,
      std::vector<std::uint8_t>(raw[index].payload.begin(), raw[index].payload.end())});
  }
  if (ctx.executionGuard) ctx.executionGuard();
  return result;
}

} // namespace

std::vector<NamedTensor>
NativeModelRunner::makeStateFeedback(const std::vector<NamedTensorView>& raw,
                                    const std::vector<std::string>& inputNames,
                                    const std::vector<std::string>& outputNames,
                                    const RoleExecutionContext& ctx) const
{
  return makeLegacyStateFeedback(raw, inputNames, outputNames, ctx);
}

TensorBundle
makeNativeProviderDecodeState(const NativeModelRunner* runner,
                              const std::map<std::string, TensorBundle>& outputs,
                              const std::vector<std::string>& inputNames,
                              const std::vector<std::string>& outputNames,
                              const RoleExecutionContext& ctx)
{
  if (inputNames.empty() || inputNames.size() != outputNames.size())
    throw std::invalid_argument("Provider decode-state input/output metadata length mismatch");
  std::vector<std::vector<NamedTensorView>> scopes;
  scopes.reserve(outputs.size());
  for (const auto& output : outputs) {
    if (ctx.executionGuard) ctx.executionGuard();
    // The bundle overload enforces the local/encoded exclusivity and keeps the
    // views borrowed from their owner; a local producer's bytes stay valid for
    // this transaction.
    if (output.second.localTensors || isEncodedTensorBundle(output.second.payload))
      scopes.push_back(decodeTensorBundleViews(output.second, ctx.workStatistics));
  }
  std::vector<NamedTensorView> selected;
  selected.reserve(outputNames.size());
  for (const auto& name : outputNames) {
    if (ctx.executionGuard) ctx.executionGuard();
    bool found = false;
    for (const auto& scope : scopes) {
      const auto tensor = std::find_if(scope.begin(), scope.end(),
        [&] (const auto& value) { return value.name == name; });
      if (tensor == scope.end()) continue;
      selected.push_back(*tensor);
      found = true;
      break;
    }
    if (!found)
      throw std::runtime_error("Provider role is missing decode-state output: " + name);
  }
  auto state = runner ? runner->makeStateFeedback(selected, inputNames, outputNames, ctx) :
    makeLegacyStateFeedback(selected, inputNames, outputNames, ctx);
  if (state.size() != inputNames.size())
    throw std::invalid_argument("Provider decode-state successor set mismatch");
  for (std::size_t index = 0; index < state.size(); ++index) {
    if (state[index].name != inputNames[index])
      throw std::invalid_argument("Provider decode-state successor name mismatch");
    validateNamedTensor(state[index]);
  }
  if (ctx.executionGuard) ctx.executionGuard();
  return makeEncodedTensorBundle("__ndnsf_provider_decode_state", std::move(state));
}

std::uint64_t
NativeModelRunner::cacheIdentity() const
{
  auto& registry = runnerCacheIdentityRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  const auto found = registry.identities.find(this);
  if (found == registry.identities.end()) {
    throw std::logic_error("NativeModelRunner cache identity is unavailable");
  }
  return found->second;
}

void
NativeOpaqueStateHandleV1::validate() const
{
  if (providerIdentity.empty() || providerBootId.empty() || sessionId.empty() ||
      role.empty() || token.empty()) {
    throw std::invalid_argument("opaque Provider state handle is incomplete");
  }
  if (token.rfind("ndnsf-device-state-v1:", 0) != 0 &&
      token.rfind("ndnsf-resident-state-v1:", 0) != 0) {
    throw std::invalid_argument("opaque Provider state handle has invalid schema");
  }
}

void
NativeConversationStateHandleV1::validate() const
{
  opaque.validate();
  if (conversationKey.empty() || conversationKey.find('\n') != std::string::npos ||
      logicalBytes == 0) {
    throw std::invalid_argument(
      "Provider conversation state handle is incomplete");
  }
}

const std::optional<ExecutionEvidence>&
NativeModelRunner::executionEvidence() const
{
  static const std::optional<ExecutionEvidence> none;
  return none;
}

std::optional<std::map<std::string, TensorBundle>>
NativeModelRunner::runStreamed(const RoleExecutionContext&)
{
  return std::nullopt;
}

std::optional<ExecutionEvidence>
NativeModelRunner::executionEvidenceSnapshot() const
{
  return executionEvidence();
}

std::optional<NativeRuntimeMetrics>
NativeModelRunner::runtimeMetricsSnapshot() const
{
  return std::nullopt;
}

bool
NativeModelRunner::supportsOpaqueStateHandles() const
{
  return false;
}

std::optional<NativeOpaqueStateHandleV1>
NativeModelRunner::stateHandleSnapshot(const std::string&) const
{
  return std::nullopt;
}

void
NativeModelRunner::releaseSessionState(const std::string&)
{
}

bool
NativeModelRunner::supportsConversationStateTransfer() const
{
  return false;
}

std::optional<NativeConversationStateHandleV1>
NativeModelRunner::promoteSessionStateToConversation(
  const std::string&, const std::string&)
{
  return std::nullopt;
}

bool
NativeModelRunner::supportsPortableConversationState() const
{
  return false;
}

NativePortableKvSnapshotV1
NativeModelRunner::exportConversationState(
  const NativeConversationStateHandleV1&, const DecodeStateIdentityV1&,
  const NativePortableKvLimitsV1&)
{
  throw std::runtime_error("native-portable-kv-unsupported");
}

NativeConversationStateHandleV1
NativeModelRunner::importConversationState(
  const NativePortableKvSnapshotV1&, const DecodeStateIdentityV1&,
  const std::string&, const NativePortableKvLimitsV1&)
{
  throw std::runtime_error("native-portable-kv-unsupported");
}

bool
NativeModelRunner::restoreConversationState(
  const NativeConversationStateHandleV1&, const std::string&)
{
  return false;
}

bool
NativeModelRunner::pauseConversationStateToHost(
  const NativeConversationStateHandleV1&)
{
  return false;
}

std::future<bool>
NativeModelRunner::prefetchConversationStateToGpu(
  const NativeConversationStateHandleV1&)
{
  std::promise<bool> promise;
  promise.set_value(false);
  return promise.get_future();
}

bool
NativeModelRunner::cancelConversationStatePrefetch(
  const NativeConversationStateHandleV1&)
{
  return false;
}

bool
NativeModelRunner::releaseConversationState(
  const NativeConversationStateHandleV1&)
{
  return false;
}

LambdaModelRunner::LambdaModelRunner(RoleRunner runner,
                                     std::optional<ExecutionEvidence> evidence)
  : m_runner(std::move(runner))
  , m_evidence(std::move(evidence))
{
  if (!m_runner) {
    throw std::invalid_argument("LambdaModelRunner requires a runner");
  }
}

const std::optional<ExecutionEvidence>&
LambdaModelRunner::executionEvidence() const
{
  return m_evidence;
}

std::map<std::string, TensorBundle>
LambdaModelRunner::run(const RoleExecutionContext& ctx)
{
  return m_runner(ctx);
}

std::shared_ptr<NativeModelRunner>
makeNativeModelRunner(RoleRunner runner, ExecutionEvidence evidence)
{
  evidence.validate();
  return std::make_shared<LambdaModelRunner>(std::move(runner), std::move(evidence));
}

std::shared_ptr<NativeModelRunner>
makeNativeModelRunner(RoleRunner runner)
{
  return std::make_shared<LambdaModelRunner>(std::move(runner));
}

void
RegistryNativeModelRunnerFactory::registerBackend(std::string backend, Creator creator)
{
  if (m_frozen) {
    throw std::logic_error("NativeModelRunner backend registry is frozen");
  }
  if (backend.empty()) {
    throw std::invalid_argument("NativeModelRunner backend must not be empty");
  }
  if (!creator) {
    throw std::invalid_argument("NativeModelRunner creator must not be empty");
  }
  if (!m_creators.emplace(std::move(backend), std::move(creator)).second) {
    throw std::invalid_argument("NativeModelRunner backend is already registered");
  }
}

void
RegistryNativeModelRunnerFactory::replaceBackend(std::string backend, Creator creator)
{
  if (m_frozen) {
    throw std::logic_error("NativeModelRunner backend registry is frozen");
  }
  if (backend.empty()) {
    throw std::invalid_argument("NativeModelRunner backend must not be empty");
  }
  if (!creator) {
    throw std::invalid_argument("NativeModelRunner creator must not be empty");
  }
  const auto it = m_creators.find(backend);
  if (it == m_creators.end()) {
    throw std::out_of_range("NativeModelRunner backend is not registered: " + backend);
  }
  it->second = std::move(creator);
}

void
RegistryNativeModelRunnerFactory::freeze()
{
  m_frozen = true;
}

bool
RegistryNativeModelRunnerFactory::hasBackend(const std::string& backend) const
{
  return m_creators.find(backend) != m_creators.end();
}

std::shared_ptr<NativeModelRunner>
RegistryNativeModelRunnerFactory::create(const NativeModelRunnerSpec& spec) const
{
  if (spec.backend.empty()) {
    throw std::invalid_argument("NativeModelRunnerSpec.backend must not be empty");
  }
  const auto found = m_creators.find(spec.backend);
  if (found == m_creators.end()) {
    throw std::out_of_range("no NativeModelRunner backend registered: " +
                            spec.backend);
  }
  std::shared_ptr<NativeModelRunner> runner;
  const bool sourceProgram = spec.metadata.count("sourceEntryFiles") != 0 ||
                             spec.metadata.count("sourceEntryProgram") != 0;
  if (spec.entryContract || sourceProgram) {
    std::vector<NativeSubgraphEntryContract> entries;
    const auto root = std::filesystem::path(spec.path).parent_path();
    if (sourceProgram) {
      if (spec.entryContract || std::filesystem::path(spec.path).filename() != "model.onnx")
        throw std::invalid_argument("source program requires its verified model anchor");
      const auto program = NativeOnnxSourceEntryProgramV1::parse(
        spec.metadata.at("sourceEntryProgram"));
      const auto files = decodeNativeSourceEntryFiles(spec.metadata.at("sourceEntryFiles"));
      if (program.entries.size() != files.size() ||
          files.front().graphDigest != spec.metadata.at("assembledModelDigest"))
        throw std::invalid_argument("source program file set differs from its admitted entries");
      for (std::size_t index = 0; index < files.size(); ++index) {
        const auto& recipe = program.entries[index];
        const auto& file = files[index];
        if (file.entryId != recipe.entryId)
          throw std::invalid_argument("source program file order differs from its admitted entries");
        NativeSubgraphEntryContract entry;
        entry.entryId = recipe.entryId;
        entry.unitId = spec.role;
        entry.relativeGraphPath =
          nativeSourceEntryGraphPath(root, index).lexically_relative(root).string();
        entry.graphDigest = file.graphDigest;
        entry.inputNames = recipe.transform.isAttention()
          ? recipe.inputPorts : std::vector<std::string>{recipe.transform.input};
        entry.outputNames = recipe.transform.isAttention()
          ? recipe.outputPorts : std::vector<std::string>{recipe.transform.output};
        for (const auto& name : entry.inputNames) entry.inputSlotByGraphName.emplace(name, name);
        for (const auto& name : entry.outputNames) entry.outputSlotByGraphName.emplace(name, name);
        entries.push_back(std::move(entry));
      }
    }
    else {
      const auto& entry = *spec.entryContract;
      entry.validate();
      if (entry.unitId != spec.role || entry.relativeGraphPath != "model.onnx" ||
          std::filesystem::path(spec.path).filename() != entry.relativeGraphPath ||
          entry.graphDigest != spec.metadata.at("assembledModelDigest"))
        throw std::invalid_argument("whole-region entry differs from its assembled artifact");
      entries.push_back(entry);
    }
    const auto makeChild = [&] (const NativeSubgraphEntryContract& entry) {
      auto child = spec;
      child.entryContract.reset();
      child.role = entry.entryId;
      if (sourceProgram) {
        child.path = (root / entry.relativeGraphPath).string();
        for (auto metadata = child.metadata.begin(); metadata != child.metadata.end();) {
          if (metadata->first.rfind("inputScope.", 0) == 0 ||
              metadata->first.rfind("outputScope.", 0) == 0)
            metadata = child.metadata.erase(metadata);
          else
            ++metadata;
        }
        for (const auto* key : {"sourceEntryFiles", "sourceEntryProgram", "assembledCachePath",
                                "protectedResidentIdentity"})
          child.metadata.erase(key);
        // ORT derives a profile filename from the prefix and timestamp.
        // Several children can start in the same millisecond.
        for (const auto* key : {"providerProfilePrefix", "provider_profile_prefix"}) {
          const auto profile = child.metadata.find(key);
          if (profile != child.metadata.end() && !profile->second.empty())
            profile->second += "-entry-" + nativePlanningDigest(entry.entryId).substr(7);
        }
        child.metadata["assembledModelDigest"] = entry.graphDigest;
        child.protectedResidentUse.reset();
        child.residencyIdentity.reset();
      }
      child.metadata["forceOutputBundle"] = "true";
      for (std::size_t i = 0; i < entry.inputNames.size(); ++i) {
        child.metadata["inputScope." + entry.inputNames[i]] = entry.inputNames[i];
        child.metadata["inputScope." + std::to_string(i)] = entry.inputNames[i];
      }
      for (std::size_t i = 0; i < entry.outputNames.size(); ++i) {
        child.metadata["outputScope." + entry.outputNames[i]] = entry.outputNames[i];
        child.metadata["outputScope." + std::to_string(i)] = entry.outputNames[i];
      }
      if (!entry.stateSuccessorByInputGraphName.empty()) {
        std::string inputs;
        std::string outputs;
        std::string successors;
        for (const auto& pair : entry.stateSuccessorByInputGraphName) {
          if (!inputs.empty()) {
            inputs += ',';
            outputs += ',';
            successors += ',';
          }
          inputs += pair.first;
          outputs += pair.second;
          successors += pair.first + '=' + pair.second;
        }
        child.metadata["statefulModel"] = "true";
        child.metadata["stateInputNames"] = std::move(inputs);
        child.metadata["stateOutputNames"] = std::move(outputs);
        child.metadata["stateSuccessorMap"] = std::move(successors);
      }
      else {
        for (const auto* key : {"statefulModel", "stateInputNames", "stateOutputNames",
                                "stateSuccessorMap", "kvTensorMap"})
          child.metadata.erase(key);
      }
      return child;
    };
    runner = std::make_shared<NativeEntryRunner>(
      std::move(entries),
      [creator = found->second] (const NativeModelRunnerSpec& childSpec) {
        auto childRunner = creator(childSpec);
        if (!childRunner)
          throw std::logic_error("NativeModelRunner backend returned null: " + childSpec.backend);
        return childRunner;
      },
      makeChild);
  }
  else {
    runner = found->second(spec);
  }
  if (!runner) {
    throw std::logic_error("NativeModelRunner backend returned null: " +
                           spec.backend);
  }
  return runner;
}

} // namespace ndnsf::di
