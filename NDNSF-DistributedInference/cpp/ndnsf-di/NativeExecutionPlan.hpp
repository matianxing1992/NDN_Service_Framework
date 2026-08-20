#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_EXECUTION_PLAN_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_EXECUTION_PLAN_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"

#include <ndn-cxx/encoding/buffer.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <ostream>
#include <string>
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
};

/**
 * Return the immutable base name shared by the manifest and every segment of
 * one V3 tensor object. Opaque values are encoded as single reversible
 * components so embedded '/' characters cannot change the grammar.
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

struct NativeReadinessPredicateV3
{
  std::string mode;
  std::vector<std::string> endpointDigests;
  std::size_t quorum = 0;
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

struct NativeExecutionPlan
{
  int version = 1;
  std::string serviceName;
  std::string modelName;
  std::string modelFamily = "generic-onnx";
  std::string modelFormat = "unknown";
  std::string plannerKind = "onnx-dag";
  std::string executionPolicy = "DATA_DRIVEN_V2";
  std::vector<std::string> roles;
  std::vector<NativeDependencySpec> dependencies;
};

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
