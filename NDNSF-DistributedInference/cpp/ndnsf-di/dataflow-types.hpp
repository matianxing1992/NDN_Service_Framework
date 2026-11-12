#ifndef NDNSF_DISTRIBUTED_INFERENCE_DATAFLOW_TYPES_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_DATAFLOW_TYPES_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ConversationStateBinding.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/DecodeStateIdentity.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/GenerationEpochLineage.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ndnsf::di {

// Sealed step template types live in NativeExecutionPlan.hpp. A role only needs
// to carry a borrowed view of them, so they are declared here rather than
// including the planner from this shared value header.
struct NativeExecutionStepSpec;

struct TensorBundle
{
  std::string name;
  std::vector<uint8_t> payload;
  std::size_t expectedSegments = 0;
  std::size_t expectedBytes = 0;
  // Runtime-only observation shared between the worker result and the
  // dependency transport. It is never serialized into the tensor bundle.
  mutable std::shared_ptr<StageTransferObservation> transferObservation;
};

/** Adapter-certified tensor-layout transition at one pipeline boundary. */
struct RedistributionSpec
{
  std::vector<std::uint64_t> producerRanks;
  std::vector<std::uint64_t> consumerRanks;
  std::string tensor;
  std::string operation;
  std::string epoch;
  std::string integrityDigest;
  std::string sourceLayoutDigest;
  std::string targetLayoutDigest;
  std::int64_t axis = -1;
  std::size_t temporaryMemoryBytes = 0;
  bool completeOutput = false;
};

struct DependencyEdge
{
  DependencyEdge() = default;

  DependencyEdge(std::string scope,
                 std::string producerRole,
                 std::string consumerRole,
                 std::string plannedDataName,
                 std::size_t expectedSegments = 0,
                 std::size_t expectedBytes = 0,
                 std::vector<std::string> tensors = {});

  std::string scope;
  std::string producerRole;
  std::string consumerRole;
  std::vector<std::string> consumerRoles;
  std::string plannedDataName;
  std::size_t expectedSegments = 0;
  std::size_t expectedBytes = 0;
  std::vector<std::string> tensors;
  // Concrete names selected from one logical transport bundle.  Empty keeps
  // the legacy one-name selection contract.
  std::vector<std::string> bundleTensorNames;
  std::string requestId;
  std::uint64_t attemptEpoch = 0;
  // Cross-Provider edges opt into the authenticated NDNSF_DATA_V1 profile.
  // Ordinary pipeline dependencies remain on the existing COLLAB-LARGE path.
  bool useNdnsfDataV1 = false;
  std::uint64_t collectiveOperationIndex = 0;
  std::string collectiveProducerRank;
  std::string collectiveSourceLayoutDigest;
  std::string collectiveTargetLayoutDigest;
  std::string collectiveTensorDigest;
  std::vector<RedistributionSpec> redistributions;
  // Rank identifiers from the adapter-certified redistribution edge for this
  // exact producer/consumer role projection.
  std::optional<std::uint64_t> redistributionProducerRank;
  std::optional<std::uint64_t> redistributionConsumerRank;
  // Request-scoped V3 projections use the ordinary Collaboration naming
  // contract when no explicit plannedDataName is sealed.
  std::string transportScope;
  std::string producerProvider;
  std::string topicPrefix;
  // Exact Placement V3 tensor-object authority. `expectedSegments` is the
  // concrete count only after the signed manifest is verified; maxSegments is
  // the plan-time allocation/fetch bound.
  bool declaredByV3 = false;
  // Advisory early transport sizing, separate from expected byte/count checks.
  std::uint64_t prefetchBundleBytes = 0;
  std::uint64_t prefetchSegmentBytes = 0;
  bool inlineFirstSegment = false;
  std::string manifestDataName;
  std::size_t maxSegments = 0;
  std::string endpointDigest;
  std::string executionObjectDigest;
  std::string planDigest;
  std::string manifestContractDigest;
  std::string tensorDigest;
  std::string layoutDigest;
  std::string securityProfile;
  std::string operationKind;
  std::uint64_t round = 0;
  std::uint64_t namingOperationStride = 0;
  std::uint64_t microbatch = 0;
  std::uint64_t noProgressDeadlineMs = 0;
  std::uint64_t hardDeadlineMs = 0;
  // Step binding for a sealed tensor-parallel edge. The worker matches a step's
  // remote input to exactly this edge through these fields; matching on
  // tensors[0] or a shared slot name would be the guess C4 forbids. All four
  // stay empty for an ordinary pipeline edge, so its behaviour is unchanged.
  std::string producerStepId;
  std::string producerSlotId;
  std::string consumerStepId;
  std::string consumerSlotId;
};

/** Fail closed unless a dependency bundle is complete for its sealed edge.
 * V3 uses the runtime manifest's concrete segment/byte counts, while legacy
 * edges may retain zero as an unspecified count. */
void
validateTensorBundleForEdge(const DependencyEdge& edge,
                            const TensorBundle& bundle,
                            bool requireReconstructed = true);

struct RoleSpec
{
  RoleSpec() = default;

  RoleSpec(std::string role,
           std::vector<DependencyEdge> inputs,
           std::vector<DependencyEdge> outputs,
           std::string requestId = {},
           std::uint64_t attemptEpoch = 0)
    : role(std::move(role))
    , inputs(std::move(inputs))
    , outputs(std::move(outputs))
    , requestId(std::move(requestId))
    , attemptEpoch(attemptEpoch)
  {
  }

  std::string role;
  std::vector<DependencyEdge> inputs;
  std::vector<DependencyEdge> outputs;
  std::string requestId;
  std::uint64_t attemptEpoch = 0;
  // Request-scoped inference epoch. Epoch zero is prefill; every later epoch
  // must consume the Provider-owned state committed by its direct predecessor.
  std::uint64_t inferenceEpoch = 0;
  // Observation owner only; never serialized or included in execution/cache identity.
  std::shared_ptr<class NativeWorkStatistics> workStatistics;
  // Canonical model-token lineage shared by every role in this inference
  // epoch. The worker binds edge-local producer/consumer fields before
  // publishing it inside the encrypted dependency payload.
  std::optional<GenerationEpochLineageV1> generationLineage;
  std::vector<std::string> stateInputNames;
  std::vector<std::string> stateOutputNames;
  // Exact adapter-certified state identities for this transition. Prefill has
  // no predecessor; every decode epoch carries both values. The Provider
  // runtime compares the predecessor before invoking the runner and stages the
  // candidate only after execution succeeds.
  std::optional<DecodeStateIdentityV1> predecessorDecodeStateIdentity;
  std::optional<DecodeStateIdentityV1> candidateDecodeStateIdentity;
  // A resumed conversation Request may explicitly restore one exact
  // Provider-local state entry.  This is separate from the request-local
  // predecessor/candidate transition above: the binding names the retained
  // prior state, while the candidate identity must belong to this fresh
  // request/generation.  The runtime injects the restored bundle only after
  // exact lookup succeeds; callers may not provide the internal scope.
  std::optional<ConversationStateBinding> conversationStateBinding;
  std::uint64_t conversationStateLookupNowMs = 0;
  // The ordinary role path commits state after worker publication.  The epoch
  // coordinator defers this commit until its token/feedback publication has
  // also been accepted.
  bool deferStateCommit = false;
  // Request-scoped native streamed execution. CUDA adapters retain state in
  // their own device bindings and expose only an opaque Provider-local handle;
  // CPU adapters continue to use the ordinary host-tensor representation.
  bool streamingStateExecution = false;
  // Sealed token-local step template this role must execute. Empty keeps the
  // legacy whole-role path, which is why every existing role is unaffected.
  // A role that carries steps cannot be executed by a runner that only knows
  // how to run a whole role, so the worker must not silently fall back.
  //
  // Per C4 the worker matches a step's remote input to exactly one declared
  // role input by consumer step and slot; the sealed dependency records that
  // binding, which is what the worker reads at readiness instead of matching
  // tensors or slot names.
  std::shared_ptr<const std::vector<NativeExecutionStepSpec>> steps;
  std::string stepPlanVersion;
  std::uint64_t tensorDegree = 1;
};

struct RoleExecutionContext
{
  using StreamEventSink = std::function<bool(const std::vector<std::uint8_t>&)>;

  std::shared_ptr<class NativeWorkStatistics> workStatistics;

  std::string sessionId;
  std::string role;
  std::string requestId;
  // Stable Provider boot identity propagated from the selected decode-state
  // binding into adapter-owned phase records.  It is metadata only; no model
  // or state bytes cross this boundary.
  std::string providerBootId;
  std::uint64_t attemptEpoch = 0;
  std::uint64_t inferenceEpoch = 0;
  // Set only while a stateful adapter owns an entire streamed decode loop.
  // CUDA state outputs remain in the adapter's persistent device bindings for
  // intermediate epochs; the adapter exports one final state bundle only
  // after the stream has reached a terminal token so Provider state
  // bookkeeping can commit the request atomically.
  bool streamingStateExecution = false;
  // Authenticated logical token lineage for this exact role/epoch.  Adapter
  // runners use it only to materialize graph-declared positional inputs; it
  // is copied from the sealed RoleSpec and never inferred from activation
  // tensor shape or application-controlled metadata.
  std::optional<GenerationEpochLineageV1> generationLineage;
  // Number of newly admitted logical tokens represented by this invocation.
  // This is derived from exact predecessor/candidate state identities by the
  // Provider worker.  It lets downstream stages create position/mask inputs
  // even though they receive activations rather than input_ids.
  std::uint32_t generationInputTokenCount = 0;
  std::map<std::string, TensorBundle> inputsByScope;
  // Exact request-scoped dependency and redistribution contracts corresponding
  // to inputsByScope. Adapter runners use these to apply GATHER/SCATTER/RESHARD
  // rather than guessing layout semantics from tensor names.
  std::map<std::string, DependencyEdge> inputEdgesByScope;
  // Set only for the request's terminal streamed role.  Native adapters may
  // publish an already serialized application event through this sink while
  // executing incremental decode; the worker/handler owns Core admission and
  // terminal End/Response, and dependency transport never carries events.
  StreamEventSink streamEventSink;
  // The one sealed step this invocation is executing, when the role carries a
  // step template. Null keeps the ordinary whole-role call, which is why every
  // existing runner is unaffected.
  //
  // C4 fixes the execution shape: `NativeModelRunner::run(ctx)` stays the only
  // port, the context gains this optional reference, and a composite runner
  // resolves it against its sealed entry table before calling the ordinary ONNX
  // runner. There is deliberately no `runStep` virtual and no second scheduler,
  // so an adapter that does not understand steps can still be told the plan is
  // step-based by the presence of `roleSteps`.
  std::shared_ptr<const NativeExecutionStepSpec> activeStep;
  // The full template for this role, so a composite runner can resolve local
  // producer steps and identify which remote slots it still owes. Null on the
  // legacy path.
  std::shared_ptr<const std::vector<NativeExecutionStepSpec>> roleSteps;
  // Slot-resolved values for the active step: local slots already produced in
  // this unit, remote slots already admitted. Keyed by sealed slot id.
  std::map<std::string, TensorBundle> stepInputsBySlot;
};

using RoleRunner = std::function<std::map<std::string, TensorBundle>(
  const RoleExecutionContext&)>;

struct RoleTiming
{
  std::string role;
  std::chrono::steady_clock::time_point queuedAt;
  std::chrono::steady_clock::time_point workerStartedAt;
  std::chrono::steady_clock::time_point startedAt;
  std::chrono::steady_clock::time_point finishedAt;
};

struct DataflowResult
{
  std::map<std::string, TensorBundle> outputsByScope;
  std::vector<RoleTiming> roleTimings;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_DATAFLOW_TYPES_HPP
