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
};

struct NativeExecutionPlan
{
  int version = 1;
  std::string serviceName;
  std::string modelName;
  std::string modelFamily = "generic-onnx";
  std::string modelFormat = "unknown";
  std::string plannerKind = "onnx-dag";
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
