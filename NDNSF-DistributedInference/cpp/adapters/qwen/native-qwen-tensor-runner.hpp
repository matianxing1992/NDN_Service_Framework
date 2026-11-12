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

private:
  const QwenTpEntryContract* findEntry(const std::string& entryId) const;

  std::vector<QwenTpEntryContract> m_entries;
  ChildFactory m_childFactory;
  std::function<NativeModelRunnerSpec(const QwenTpEntryContract&)> m_specFactory;
  std::map<std::string, std::shared_ptr<NativeModelRunner>> m_children;
};

} // namespace ndnsf::di::qwen

#endif // NDNSF_DI_QWEN_NATIVE_TENSOR_RUNNER_HPP
