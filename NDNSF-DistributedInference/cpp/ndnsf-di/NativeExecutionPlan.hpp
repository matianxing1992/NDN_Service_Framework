#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_EXECUTION_PLAN_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_EXECUTION_PLAN_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/dataflow-types.hpp"

#include <ndn-cxx/encoding/buffer.hpp>
#include <ndn-cxx/name.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

namespace ndnsf::di {

struct ExecutionAttemptKey
{
  std::string requestId;
  std::uint64_t attemptEpoch = 0;

  void validate() const;
  std::string scopedSessionId() const;
  std::map<std::string, std::string> assignmentFields() const;

  bool operator==(const ExecutionAttemptKey& other) const noexcept;
};

enum class ExecutionAttemptAdmission
{
  Accepted,
  Stale,
  Cancelled,
  DuplicateTerminal,
};

const char* toString(ExecutionAttemptAdmission admission) noexcept;
std::ostream& operator<<(std::ostream& os, ExecutionAttemptAdmission admission);

class ExecutionAttemptAuthority
{
public:
  ExecutionAttemptAdmission admit(const ExecutionAttemptKey& key);
  bool cancel(const ExecutionAttemptKey& key);
  bool complete(const ExecutionAttemptKey& key);
  bool isAuthoritative(const ExecutionAttemptKey& key) const;

private:
  struct State
  {
    std::uint64_t currentEpoch = 0;
    bool cancelled = false;
    bool terminal = false;
  };

  mutable std::mutex m_mutex;
  std::map<std::string, State> m_states;
};

struct SegmentNamingSpec
{
  std::string mode = "ndn-segment-component";
  std::size_t staticSegmentCount = 0;
  bool dynamicFallback = true;
};

/** One sealed typed boundary of an execution step.
 *
 * A slot names a tensor the step produces or consumes and fixes the shape,
 * dtype and layout contract the runtime must verify before the step runs. The
 * source distinguishes where a consumed slot comes from, so the executor can
 * admit epoch entry inputs and step dependencies independently instead of
 * waiting for one flat "all role inputs ready" gate.
 */
struct NativeStepSlotSpec
{
  std::string slotId;
  std::string dtype;
  std::vector<std::variant<std::int64_t, std::string>> shape;
  std::string layoutDigest;
  // EPOCH_INPUT | TOKEN_FEEDBACK | LOCAL_STEP | REMOTE_STEP | LOCAL_STATE
  std::string source;
  // Set only for LOCAL_STEP/REMOTE_STEP: the step in this same token template
  // that produces the slot. Empty for every other source.
  std::string producerStepId;

  void validate() const;
};

/** One step of the sealed token-local execution projection.
 *
 * A step belongs to exactly one placement unit and the template as a whole
 * must be a DAG. `operation` selects a subgraph call (which must reference a
 * verified recipe entry) or a local SUM (which must not fabricate an artifact
 * entry or an empty ONNX graph). Both kinds share one dependency admission and
 * lifecycle.
 */
struct NativeExecutionStepSpec
{
  std::string stepId;
  std::string unitId;
  // CALL_SUBGRAPH | SUM
  std::string operation;
  // Required for CALL_SUBGRAPH and rejected for SUM.
  std::string artifactEntry;
  std::vector<NativeStepSlotSpec> inputSlots;
  std::vector<NativeStepSlotSpec> outputSlots;
  std::vector<std::string> localStateSlots;

  void validate() const;
};

/** Step-plan version a plan must declare before its steps may run. */
inline constexpr const char* NATIVE_STEP_PLAN_VERSION_V1 = "step-plan-v1";
/** Capability value a Provider must advertise to accept a step plan. */
inline constexpr const char* NATIVE_STEP_PLAN_CAPABILITY_V1 = "step-plan-v1";
/** Capability value a Provider must advertise to perform a local SUM. */
inline constexpr const char* NATIVE_SUM_CAPABILITY_V1 = "sum-v1";

struct NativeDependencySpec
{
  NativeDependencySpec() = default;

  NativeDependencySpec(std::vector<std::string> producers,
                       std::vector<std::string> consumers,
                       std::string keyScope,
                       std::string topicPrefix,
                       std::string objectNameTemplate,
                       std::size_t expectedSegments = 0,
                       std::size_t expectedBytes = 0,
                       std::vector<std::string> tensors = {});

  std::vector<std::string> producers;
  std::vector<std::string> consumers;
  std::string keyScope;
  std::string topicPrefix;
  std::string objectNameTemplate;
  std::size_t expectedSegments = 0;
  std::size_t expectedBytes = 0;
  std::vector<std::string> tensors;
  // Internal request-scoped control edges (for example TOKEN_FEEDBACK) are
  // still ordinary signed Data dependencies, but they do not change which
  // role owns the user-facing final response.
  std::string operationKind;
  SegmentNamingSpec segmentNaming;
  // Cross-Provider dependencies may opt into the authenticated,
  // request-scoped NDNSF_DATA_V1 transport. Ordinary dependencies retain the
  // existing COLLAB-LARGE path.
  bool useNdnsfDataV1 = false;
  std::uint64_t collectiveOperationIndex = 0;
  std::string collectiveProducerRank;
  std::string collectiveSourceLayoutDigest;
  std::string collectiveTargetLayoutDigest;
  std::string collectiveTensorDigest;
  std::vector<RedistributionSpec> redistributions;
  // Explicit dependency identity, unique within one plan. It reuses the
  // existing keyScope value rather than introducing a second identity space;
  // groupId stays a security/membership label and is never used to look a
  // dependency up.
  std::string dependencyId;
  // Step binding for tensor-parallel plans: the producing step/output slot and
  // the consuming step/input slot. Empty for legacy pipeline dependencies.
  std::string producerStepId;
  std::string producerSlotId;
  std::string consumerStepId;
  std::string consumerSlotId;
};

/** One complete request-scoped execution unit. Tensor-parallel ranks are
 * represented as separate roles; a role is never spread across Providers.
 */
struct NativeExecutionRoleV3
{
  std::string roleId;
  std::string stageId;
  std::uint64_t rank = 0;
  std::uint64_t layerBegin = 0;
  std::uint64_t layerEnd = 0;
  std::string backend;
  std::string adapterId;
  std::string adapterVersion;
};

/** Exact named tensor object authorized by one sealed role dataflow contract. */
struct NativeTensorEndpointV3
{
  std::string producerNamespace;
  std::string requester;
  std::string requestId;
  std::uint64_t attempt = 0;
  std::string planDigest;
  std::string groupId;
  std::string groupEpoch;
  std::string operation;
  std::uint64_t round = 0;
  std::string sourceKind;
  std::string producerRole;
  std::uint64_t producerRank = 0;
  std::string consumerRole;
  std::vector<std::string> consumerRoles;
  std::string tensorId;
  // Concrete members carried by one logical transport tensor.  The signed
  // tensorId remains the adapter/transport identity; these names bind the
  // runtime bundle to the producer/consumer ONNX boundary without aliasing
  // several concrete tensors to one name.
  std::vector<std::string> bundleTensorNames;
  std::string tensorDigest;
  std::string layoutDigest;
  std::string targetLayoutDigest;
  std::uint64_t microbatch = 0;
  std::size_t segmentCount = 0;
  std::string manifestDigest;
  std::string securityProfile;
  std::uint64_t noProgressDeadlineMs = 0;
  std::uint64_t hardDeadlineMs = 0;
  std::string endpointDigest;
  // Optional signed prefetch hints; actual manifest/content remain authority.
  std::uint64_t prefetchBundleBytes = 0;
  std::uint64_t prefetchSegmentBytes = 0;
  // Signed endpoint opt-in: dynamic manifest and segment0 share one Data.
  bool inlineFirstSegment = false;
  // Fixed, signed generation projection: zero for application input and
  // non-generation roles; otherwise the generation contract's operation stride.
  std::uint64_t namingOperationStride = 0;
};

/**
 * Return the immutable base name shared by the manifest and every segment of
 * one V3 tensor object. Readable labels occupy one bounded Generic component;
 * their complete identity remains in the signed endpoint and tensor key.
 */
std::string
tensorObjectNamePrefix(const NativeTensorEndpointV3& endpoint);

/** Exact signed TensorObjectManifest Data name declared by the endpoint. */
std::string
tensorObjectManifestName(const NativeTensorEndpointV3& endpoint);

/** Exact segment Data name. The final component is an NDN Segment component. */
std::string
tensorObjectSegmentName(const NativeTensorEndpointV3& endpoint,
                        std::size_t segmentNo);

/** Complete object contract identity, independent of unknown runtime bytes. */
std::string
computeTensorExecutionObjectDigest(const NativeTensorEndpointV3& endpoint);

/** The same typed segment suffix used by endpoint and validated runtime edge. */
ndn::Name
makeTensorSegmentDataName(const ndn::Name& objectPrefix, std::uint64_t segmentNo);

struct NativeReadinessPredicateV3
{
  std::string mode;
  std::vector<std::string> endpointDigests;
  std::size_t quorum = 0;
};

struct NativeGenerationEndpointRangeV1
{
  std::uint64_t epochCount = 0;
  std::uint64_t operationStride = 0;
};

struct NativeRoleDataflowContractV3
{
  std::string requestId;
  std::uint64_t attempt = 0;
  std::string planDigest;
  std::string role;
  std::vector<NativeTensorEndpointV3> mayPublish;
  std::vector<NativeTensorEndpointV3> mustFetch;
  std::vector<NativeReadinessPredicateV3> waitFor;
  bool terminalResponseOwner = false;
  std::string dataflowDigest;
  // Present only for a V3 invocation; endpoint vectors contain base slots.
  std::optional<NativeGenerationEndpointRangeV1> endpointRange;
};

/** A Provider-local CPU or exactly-one-device binding for one role. */
struct NativeDeviceBindingV3
{
  std::string mode;
  std::string provider;
  std::string role;
  std::string offerDigest;
  std::string topologyProfileDigest;
  std::string resourceSnapshotDigest;
  std::uint64_t resourceSequence = 0;
  std::string offerScopedDeviceHandle;
  std::string sharingPolicy;
};

/** Communication a sealed step plan must move between units, derived from the
 * step slots rather than from provider-local measurements.
 *
 * Unknown is never zero: a slot whose dtype or any dimension is symbolic cannot
 * be costed, so it is counted in unknownTransferTensors instead of contributing
 * a made-up byte count. A caller that sums only knownTransferBytes would then
 * understate the real cost whenever unknownTransferTensors is non-zero.
 */
struct NativeStepCommunicationCost
{
  std::uint64_t remoteEdges = 0;
  std::uint64_t knownTransferBytes = 0;
  std::uint64_t unknownTransferTensors = 0;
};

struct NativeExecutionPlan
{
  int version = 1;
  std::string serviceName;
  std::string modelName;
  std::string modelFamily = "generic-onnx";
  std::string modelFormat = "unknown";
  std::string plannerKind = "onnx-dag";
  std::string executionPolicy = "DATA_DRIVEN_V2";
  // Signed stride for request-scoped streamed DATA_V1 operation indexes.
  // Zero means the plan is not authorized for multi-epoch DATA_V1 execution.
  std::uint64_t streamingOperationStride = 0;
  std::vector<std::string> roles;
  std::vector<NativeDependencySpec> dependencies;
  // Sealed token-local step template. Empty for every legacy pipeline plan,
  // which then keeps its original whole-role run path unchanged.
  std::vector<NativeExecutionStepSpec> steps;
  // Step-plan version this plan requires. Empty means the plan carries no step
  // semantics; a non-empty value must equal NATIVE_STEP_PLAN_VERSION_V1 and
  // imply a non-empty `steps` template.
  std::string stepPlanVersion;
  // Tensor-parallel degree this plan was sealed for. 1 means pipeline; the
  // value is part of the sealed contract and is never chosen by a Provider.
  std::uint64_t tensorDegree = 1;

  /** Reject malformed step templates, dependency bindings and version skew. */
  void validateSteps() const;
};

/** Sum every REMOTE_STEP input of a sealed step plan. A plan without steps has
 * no inter-unit step traffic and yields an all-zero cost.
 */
NativeStepCommunicationCost
nativeStepCommunicationCost(const NativeExecutionPlan& plan);

/** Provider-local R1 reservation policy. This class deliberately lives in DI:
 * generic NDNSF applications are not required to reserve exclusive resources.
 */
struct DiReservationPolicy
{
  std::size_t globalLimit = 1;
  std::size_t requesterLimit = 1;
  std::size_t serviceLimit = 1;
  std::uint64_t tentativeLeaseMs = 5000;
};

struct DiReservationRequest
{
  std::string providerName;
  std::string requesterName;
  std::string requestId;
  std::string serviceName;
  std::string planDigest;
  ndn::Buffer resourceBindingProof;
  std::vector<std::string> conflictKeys;
  bool authorized = false;
};

enum class DiReservationState { Tentative, Committed, Released, Expired };

struct DiReservationLease
{
  std::string reservationId;
  std::string providerName;
  std::string providerBootId;
  std::string requesterName;
  std::string requestId;
  std::string serviceName;
  std::string planDigest;
  ndn::Buffer resourceBindingProof;
  std::vector<std::string> conflictKeys;
  std::uint64_t expiresAtMs = 0;
  DiReservationState state = DiReservationState::Tentative;
};

struct DiReservationResult
{
  bool status = false;
  std::string reasonCode;
  DiReservationLease lease;
  bool idempotentReplay = false;
};

/** Authorization-first, bounded and idempotent reservation authority for
 * DIReservationSelectionV1 positive ACK generation. No model lifecycle work
 * is performed by this authority.
 */
class DiReservationAuthority
{
public:
  DiReservationAuthority(std::string providerBootId,
                         DiReservationPolicy policy = {});
  ~DiReservationAuthority();

  DiReservationResult
  reserve(const DiReservationRequest& request, std::uint64_t nowMs);

  DiReservationResult commit(const std::string& reservationId,
                             std::uint64_t nowMs);
  bool release(const std::string& reservationId, const std::string& cause,
               std::uint64_t nowMs);

  std::size_t cleanupExpired(std::uint64_t nowMs);
  void releaseAll(std::uint64_t nowMs, const std::string& cause = "PROVIDER_SHUTDOWN");

private:
  struct Record
  {
    DiReservationLease lease;
    std::string releaseCause;
  };

  std::string keyFor(const DiReservationRequest& request) const;
  bool withinQuota(const DiReservationRequest& request, std::uint64_t nowMs);

private:
  std::string m_providerBootId;
  DiReservationPolicy m_policy;
  std::mutex m_reservationMutex;
  std::map<std::string, Record> m_records;
  std::map<std::string, std::string> m_keyByReservation;
  std::uint64_t m_nextReservationId = 1;
};

struct NativeProviderAssignment
{
  std::map<std::string, std::string> providerByRole;
};

struct NativePlanSession
{
  std::string sessionId;
  NativeExecutionPlan plan;
  NativeProviderAssignment assignment;
  std::map<std::string, RoleSpec> rolesByName;
};

std::string
trimSlashes(std::string value);

std::string
replaceAll(std::string value, const std::string& from, const std::string& to);

std::string
plannedDataNameFromTemplate(const std::string& objectNameTemplate,
                            const std::string& sessionId,
                            const std::string& keyScope,
                            const std::string& producerRole,
                            const std::string& consumerRole,
                            const std::string& topicPrefix,
                            const std::string& producerProvider,
                            std::size_t sequence = 0);

std::string
plannedSegmentName(const std::string& plannedDataName, std::size_t segmentNo);

std::vector<std::string>
plannedSegmentNamesForEdge(const DependencyEdge& edge);

bool
hasStaticSegmentPlan(const NativeDependencySpec& dependency);

std::string
providerForRole(const NativeProviderAssignment& assignment,
                const std::string& role,
                const std::string& fallbackProvider = "");

RoleSpec
roleSpecFor(const NativeExecutionPlan& plan,
            const std::string& role,
            const std::string& sessionId,
            const NativeProviderAssignment& assignment,
            const std::string& localProvider = "");

/**
 * Build one request-scoped role projection for a streamed inference epoch.
 * `sequence` is substituted into `{sequence}` in every dependency name and
 * is recorded as the edge round.  The legacy overload above remains the
 * sequence-zero projection used by unary and pre-streaming callers.
 */
RoleSpec
roleSpecFor(const NativeExecutionPlan& plan,
            const std::string& role,
            const std::string& sessionId,
            const NativeProviderAssignment& assignment,
            const std::string& localProvider,
            std::size_t sequence);

RoleSpec
roleSpecFor(const NativeExecutionPlan& plan,
            const std::string& role,
            const ExecutionAttemptKey& attempt,
            const NativeProviderAssignment& assignment,
            const std::string& localProvider = "");

NativePlanSession
deployNativePlanSession(NativeExecutionPlan plan,
                        std::string sessionId,
                        NativeProviderAssignment assignment);

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_EXECUTION_PLAN_HPP
