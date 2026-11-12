#ifndef NDNSF_DI_QWEN_NATIVE_TENSOR_RUNNER_HPP
#define NDNSF_DI_QWEN_NATIVE_TENSOR_RUNNER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/native-qwen-tensor-split.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ndnsf::di::qwen {

/** Parent runner for one tensor-parallel unit.
 *
 * C4 fixes the division of responsibility. The worker owns dependency
 * admission, readiness, publication and unit completion; this runner executes
 * exactly the one step the context names. It does not wait for a peer, does not
 * decide whether a SUM's members are complete, and does not return a scheduling
 * result for the whole DAG. Those decisions already exist in the worker, and
 * duplicating them here is the second scheduler the contract forbids.
 *
 * It reaches the existing ONNX port rather than a new one: each entry becomes an
 * ordinary OnnxRuntimeModelRunner child built from a spec, so the multi-entry
 * case needs no new execution interface. Children are held by entry id for the
 * life of this parent, which is what makes warm reuse of the whole group
 * possible without raising the default session-cache capacity.
 */
class NativeQwenTensorRunner final : public NativeModelRunner
{
public:
  using ChildFactory = std::function<std::shared_ptr<NativeModelRunner>(
    const NativeModelRunnerSpec& spec)>;

  /** `entries` are the verified contracts this unit may execute; `specFactory`
   * turns one entry into the child spec, so the caller owns path and identity
   * decisions. Every entry is prepared once, before the first step runs.
   */
  NativeQwenTensorRunner(std::vector<QwenTpEntryContract> entries,
                         ChildFactory childFactory,
                         std::function<NativeModelRunnerSpec(
                           const QwenTpEntryContract&)> specFactory);

  std::map<std::string, TensorBundle>
  run(const RoleExecutionContext& ctx) override;

  /** The step plan this runner was built for, for the worker's consistency
   * checks. Empty means the runner was built without a template.
   */
  const std::vector<QwenTpEntryContract>& entries() const { return m_entries; }

  /** Whether this runner can execute the given template.
   *
   * A unit is only loadable when every CALL_SUBGRAPH step names an entry this
   * runner holds and every held entry belongs to the unit. Checking it before the
   * first step means a mismatched table fails at admission rather than half-way
   * through a sequence that already published partials.
   *
   * @returns Empty when the template is executable, otherwise the first problem,
   *          phrased for the operator who has to fix the table.
   */
  std::string checkTemplate(const std::vector<NativeExecutionStepSpec>& steps,
                            const std::string& unitId) const;

  /** Whether this unit retains KV state behind an opaque handle.
   *
   * Only the attention entries hold KV: INIT has no session state, MLP is a pure
   * function of its inputs, and HEAD samples. C6 states the consequence
   * explicitly -- the parent must not report "no state support" merely because
   * those children cannot answer a state question.
   */
  bool supportsOpaqueStateHandles() const override;

  /** The unit's state handle, aggregated from the attention children.
   *
   * The handle names this unit, not one child, because a caller restores the whole
   * rank at once. The token carries each child's own handle so a later restore can
   * reach the exact sub-session that owns each layer's KV.
   *
   * @returns Nothing when the unit holds no attention state for this session, or
   *          when any attention child is missing its handle: a partial aggregate
   *          would let a checkpoint omit layers and still look complete.
   */
  std::optional<NativeOpaqueStateHandleV1>
  stateHandleSnapshot(const std::string& sessionId) const override;

  /** Release the session's state in every child.
   *
   * Releasing only some children would leave KV resident for a session the caller
   * considers terminal, which is the leak the bounded-lifecycle rule forbids.
   */
  void releaseSessionState(const std::string& sessionId) override;

  /** Whether the whole rank can be retained without a tensor bundle.
   *
   * True only when every attention child supports it; reporting support from one
   * child would promise a transfer that fails half way.
   */
  bool supportsConversationStateTransfer() const override;

  /** Promote every KV-holding child's request state under one unit handle.
   *
   * C6 requires the parent transaction to commit only when all children succeed and
   * to release the uncommitted branch otherwise. A child that could not promote
   * therefore cannot leave the unit half-committed: the already-promoted children
   * are released before this returns, and no handle is produced.
   *
   * The unit's conversation key is suffixed per child, because two attention
   * children would otherwise claim the same key and the second promotion would be
   * refused by the adapter as a duplicate -- which reads as a failure rather than
   * as the collision it is.
   *
   * @returns The unit handle on success, nothing on any child failure.
   */
  std::optional<NativeConversationStateHandleV1>
  promoteSessionStateToConversation(const std::string& sessionId,
                                    const std::string& conversationKey) override;

  /** Restore a unit handle into every KV-holding child.
   *
   * All-or-nothing: a child that fails is rolled back along with the ones already
   * restored, so an exception cannot leave the unit holding state from two
   * different checkpoints.
   */
  bool
  restoreConversationState(const NativeConversationStateHandleV1& state,
                           const std::string& sessionId) override;

  /** Release the unit's conversation state in every KV-holding child. */
  bool
  releaseConversationState(const NativeConversationStateHandleV1& state) override;

private:
  const QwenTpEntryContract* findEntry(const std::string& entryId) const;

  std::vector<QwenTpEntryContract> m_entries;
  ChildFactory m_childFactory;
  std::function<NativeModelRunnerSpec(const QwenTpEntryContract&)> m_specFactory;
  std::map<std::string, std::shared_ptr<NativeModelRunner>> m_children;
};

/** Backend name this parent runner registers under.
 *
 * A distinct backend keeps the ordinary ONNX creators untouched: a role whose
 * spec says "native-qwen-tensor" is dispatched here, and every other role keeps
 * its existing one-session runner.
 */
inline constexpr const char* QWEN_TENSOR_RUNNER_BACKEND = "native-qwen-tensor";

/** Register the parent runner on an existing registry.
 *
 * Reuses the registry rather than introducing a second factory: the spec picks
 * this backend, and the creator below builds the children through the same
 * registry's ordinary ONNX creator with an empty session cache, exactly as C4
 * describes. That keeps one dispatch path and one place where backends are
 * registered.
 *
 * @param factory Registry that must already hold the ONNX backend, because the
 *                children are created through it. Freezing happens in the
 *                caller's existing startup sequence, not here.
 * @throws std::invalid_argument when the ONNX backend is not yet registered.
 */
void
registerQwenTensorBackend(RegistryNativeModelRunnerFactory& factory);

} // namespace ndnsf::di::qwen

#endif // NDNSF_DI_QWEN_NATIVE_TENSOR_RUNNER_HPP
