#ifndef NDNSF_DI_EXECUTION_LEASE_SERVICE_HPP
#define NDNSF_DI_EXECUTION_LEASE_SERVICE_HPP

#include "ndn-service-framework/ExecutionLease.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ndnsf::di {

inline constexpr char EXECUTION_LEASE_SERVICE_NAME[] = "/Inference/Control/Lease";
inline constexpr char EXECUTION_LEASE_CODEC_SCHEMA[] =
  "ndnsf-di-execution-lease-operation-v1";

enum class LeaseOperation
{
  Prepare,
  Commit,
  Abort,
  Renew,
  Release,
};

struct LeaseOperationRequest
{
  LeaseOperation operation = LeaseOperation::Prepare;
  std::string requestId;
  std::string planDigest;
  std::string idempotencyKey;
  std::string targetServiceName;
  std::string leaseId;
  std::string providerEpoch;
  std::string resourceBindingSchema = "ndnsf-di-binding-v1";
  ndn::Buffer resourceBindingProof;
  std::vector<std::string> roles;
  uint64_t expiresAtMs = 0;
};

struct LeaseOperationResponse
{
  bool status = false;
  LeaseOperation operation = LeaseOperation::Prepare;
  std::string reasonCode;
  std::string leaseId;
  std::string providerEpoch;
  std::string state;
  uint64_t expiresAtMs = 0;
  uint64_t executionDeadlineMs = 0;
  std::vector<std::string> conflictKeys;
  uint64_t retryAfterMs = 0;
};

struct ExecutionLeaseRequestContext
{
  std::string requesterIdentity;
  std::string providerName;
  std::string serviceName;
  std::string requestId;
};

std::string
encodeLeaseOperationRequest(const LeaseOperationRequest& request);

LeaseOperationRequest
decodeLeaseOperationRequest(const std::string& wire);

std::string
encodeLeaseOperationResponse(const LeaseOperationResponse& response);

LeaseOperationResponse
decodeLeaseOperationResponse(const std::string& wire);

/// Per-host shared lease state: one Core table and one prepare mutex shared
/// by every target ExecutionLeaseService instance of the same provider host.
/// All targets serialize slot selection through the same mutex so one target
/// cannot bypass reservations made by another; the table epoch is the host
/// boot epoch and is not resettable per target.
class SharedExecutionLeaseState
{
public:
  explicit
  SharedExecutionLeaseState(std::string providerEpoch = {});

  ndn_service_framework::ProviderExecutionLeaseTable table;
  std::mutex prepareMutex;
};

class ExecutionLeaseService
{
public:
  using ConflictKeyResolver = std::function<std::vector<std::string>(
    const LeaseOperationRequest&, const ExecutionLeaseRequestContext&)>;

  /// Retained legacy constructor: owns a private shared state, so every
  /// instance keeps the exact single-target semantics of the pre-T009-B API.
  ExecutionLeaseService(std::string providerName,
                        std::string targetServiceName,
                        ConflictKeyResolver conflictKeyResolver,
                        std::string providerEpoch = {});

  /// Host constructor: routes this target through the host-wide shared
  /// state (table + prepare mutex) instead of a private one.
  ExecutionLeaseService(std::string providerName,
                        std::string targetServiceName,
                        ConflictKeyResolver conflictKeyResolver,
                        std::shared_ptr<SharedExecutionLeaseState> sharedState);

  std::string
  handle(const ExecutionLeaseRequestContext& context,
         const std::string& payload,
         uint64_t nowMs);

  ndn_service_framework::ProviderExecutionLeaseTable&
  table() noexcept;

private:
  static LeaseOperationResponse
  fromCore(LeaseOperation operation,
           const ndn_service_framework::ExecutionLeaseResult& result);

private:
  std::string m_providerName;
  std::string m_targetServiceName;
  ConflictKeyResolver m_conflictKeyResolver;
  std::shared_ptr<SharedExecutionLeaseState> m_sharedState;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_EXECUTION_LEASE_SERVICE_HPP
