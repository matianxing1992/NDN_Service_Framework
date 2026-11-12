#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_STEP_EXECUTOR_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_STEP_EXECUTOR_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/dataflow-types.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ndnsf::di {

/** Drives one unit's sealed step template to completion.
 *
 * A tensor-parallel role is not one model call. Its unit runs a token-local DAG
 * in which CALL_SUBGRAPH steps produce partials and SUM steps reduce admitted
 * contributions. This executor owns the ordering and the slot bookkeeping; it
 * never decides what a step computes, which is the adapter port's job.
 *
 * Ordering rule: a step runs only when every one of its LOCAL_STEP inputs has
 * been produced by an earlier step of the same unit and every REMOTE_STEP input
 * has been admitted from its declared producer. A step whose remote input has
 * not arrived yet is left pending; the caller supplies arrivals through
 * `admit` and calls `advance` again. The executor never invents or waits on a
 * value that no admitted edge can deliver, which is why a plan with a missing
 * remote dependency is rejected at validation time rather than hanging here.
 */
class NativeStepExecutor
{
public:
  using StatisticsSink = std::function<std::shared_ptr<NativeWorkStatistics>()>;

  /** `steps` must be the complete template of exactly `unitId`; other units'
   * steps are ignored so a caller can pass the plan as-is.
   */
  NativeStepExecutor(const NativeExecutionPlan& plan, std::string unitId,
                     StepRunner runner);

  /** Record one admitted remote contribution for `stepId`/`slotId`.
   * Returns false when the pair is not a declared remote input of this unit, so
   * a caller cannot inject a value the contract does not expect.
   */
  bool admit(const std::string& stepId, const std::string& slotId,
             TensorBundle contribution);

  /** Execute every step that has become runnable, in DAG order.
   *
   * `epochInputs` supplies the EPOCH_INPUT slots for the steps that declare
   * them. Returns the outputs produced by the unit's terminal steps, keyed by
   * slot id, and throws when a step the DAG demands cannot run: an unadmitted
   * remote input, a missing epoch input, or an adapter that refused the step.
   */
  StepExecutionResult advance(const std::map<std::string, TensorBundle>& epochInputs,
                              const std::string& sessionId, const std::string& requestId,
                              std::uint64_t attemptEpoch, std::uint64_t inferenceEpoch,
                              const StatisticsSink& statistics = {});

  /** True once every step of this unit has run. */
  bool complete() const;

  /** Steps still waiting for a remote contribution, for diagnostics and for the
   * worker's negative paths. Empty when the unit has finished.
   */
  std::vector<std::string> pendingSteps() const;

private:
  const NativeExecutionStepSpec* findStep(const std::string& stepId) const;

  NativeExecutionPlan m_plan;
  std::string m_unitId;
  StepRunner m_runner;
  std::vector<const NativeExecutionStepSpec*> m_steps;
  std::map<std::string, std::map<std::string, TensorBundle>> m_admitted;
  std::map<std::string, TensorBundle> m_produced;
  std::set<std::string> m_done;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_STEP_EXECUTOR_HPP
