#ifndef NDNSF_DI_NATIVE_ENTRY_RUNNER_HPP
#define NDNSF_DI_NATIVE_ENTRY_RUNNER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::di {

/** Byte identity of one model and its optional external initializer in a source program. */
struct NativeSourceEntryFiles
{
  std::string entryId;
  std::string graphDigest;
  std::uint64_t graphBytes = 0;
  std::string initializerDigest;
  std::uint64_t initializerBytes = 0;
};

/** The signed array order defines the file layout. Entry IDs never become path components. */
std::filesystem::path
nativeSourceEntryGraphPath(const std::filesystem::path& root, std::size_t index);

std::string
encodeNativeSourceEntryFiles(const std::vector<NativeSourceEntryFiles>& entries);

std::vector<NativeSourceEntryFiles>
decodeNativeSourceEntryFiles(const std::string& wire);

/** Model-independent graph boundary supplied by an admitted adapter.
 *
 * File identity is optional for legacy planning-only tables. Material admission verifies
 * graph bytes and typed slots before execution; this value validates binding references.
 * Local state is explicit: each input names its exact successor output. Model-specific
 * names, kinds and layer numbers belong to the adapter, not this runtime contract.
 */
struct NativeSubgraphEntryContract
{
  std::string entryId;
  std::string unitId;
  std::vector<std::string> inputNames;
  std::vector<std::string> outputNames;
  std::string relativeGraphPath;
  std::string graphDigest;
  std::map<std::string, std::string> inputSlotByGraphName;
  std::map<std::string, std::string> outputSlotByGraphName;
  std::map<std::string, std::string> passthroughSlotByInputGraphName;
  std::map<std::string, std::string> stateSuccessorByInputGraphName;

  void validate() const;
  bool operator==(const NativeSubgraphEntryContract& other) const;
};

/** Execute one admitted CALL; ProviderRoleWorker owns readiness, SUM and publication.
 *
 * Children are prepared once and owned for this runner's lifetime. Factories are consumed
 * synchronously during construction, never retained. Calls and state transitions follow the
 * existing owner's serialization; this class adds no thread, scheduler or mutable cache.
 * Output views retain their child's storage owner. State transactions and opaque handle
 * spelling are shared with the legacy Qwen adapter.
 */
class NativeEntryRunner : public NativeModelRunner
{
public:
  using ChildFactory = std::function<std::shared_ptr<NativeModelRunner>(
    const NativeModelRunnerSpec&)>;
  using SpecFactory = std::function<NativeModelRunnerSpec(const NativeSubgraphEntryContract&)>;

  NativeEntryRunner(std::vector<NativeSubgraphEntryContract> entries,
                    ChildFactory childFactory, SpecFactory specFactory);

  std::map<std::string, TensorBundle> run(const RoleExecutionContext& ctx) override;
  const std::vector<NativeSubgraphEntryContract>& entryContracts() const { return m_entries; }
  std::string checkTemplate(const std::vector<NativeExecutionStepSpec>& steps,
                            const std::string& unitId) const;
  bool supportsOpaqueStateHandles() const override;
  std::optional<NativeOpaqueStateHandleV1>
  stateHandleSnapshot(const std::string& sessionId) const override;
  void releaseSessionState(const std::string& sessionId) override;
  bool supportsConversationStateTransfer() const override;
  std::optional<NativeConversationStateHandleV1>
  promoteSessionStateToConversation(const std::string& sessionId,
                                    const std::string& conversationKey) override;
  bool restoreConversationState(const NativeConversationStateHandleV1& state,
                                const std::string& sessionId) override;
  bool releaseConversationState(const NativeConversationStateHandleV1& state) override;
  std::optional<ExecutionEvidence> executionEvidenceSnapshot() const override;
  std::optional<NativeRuntimeMetrics> runtimeMetricsSnapshot() const override;

private:
  const NativeSubgraphEntryContract* findEntry(const std::string& entryId) const;

  std::vector<NativeSubgraphEntryContract> m_entries;
  std::map<std::string, std::shared_ptr<NativeModelRunner>> m_children;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_ENTRY_RUNNER_HPP
