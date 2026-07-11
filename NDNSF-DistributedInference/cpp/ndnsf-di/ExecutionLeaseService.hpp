#ifndef NDNSF_DI_EXECUTION_LEASE_SERVICE_HPP
#define NDNSF_DI_EXECUTION_LEASE_SERVICE_HPP

#include "ndn-service-framework/ExecutionLease.hpp"

#include <cstdint>
#include <functional>
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

class ExecutionLeaseService
{
public:
  using ConflictKeyResolver = std::function<std::vector<std::string>(
    const LeaseOperationRequest&, const ExecutionLeaseRequestContext&)>;

  ExecutionLeaseService(std::string providerName,
                        std::string targetServiceName,
                        ConflictKeyResolver conflictKeyResolver,
                        std::string providerEpoch = {});

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
  ndn_service_framework::ProviderExecutionLeaseTable m_table;
  std::mutex m_prepareMutex;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_EXECUTION_LEASE_SERVICE_HPP
