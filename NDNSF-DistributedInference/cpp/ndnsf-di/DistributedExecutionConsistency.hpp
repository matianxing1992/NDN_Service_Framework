#ifndef NDNSF_DI_DISTRIBUTED_EXECUTION_CONSISTENCY_HPP
#define NDNSF_DI_DISTRIBUTED_EXECUTION_CONSISTENCY_HPP

#include "ndn-service-framework/ExecutionLease.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ndnsf::di {

inline constexpr const char* EXECUTION_COMMIT_CERTIFICATE_SCHEMA =
  "ndnsf-di-execution-commit-certificate-v1";

struct DistributedExecutionCertificateView
{
  std::string schema;
  std::string digest;
  std::string requestId;
  uint64_t attemptEpoch = 0;
  std::string planDigest;
  std::size_t expectedCount = 0;
  std::size_t receiptCount = 0;
  std::vector<std::string> memberKeys;
  std::string localMemberKey;
};

struct DistributedExecutionCertificateResult
{
  bool status = false;
  std::string reason;
};

DistributedExecutionCertificateResult
validateDistributedExecutionCertificate(
  const DistributedExecutionCertificateView& certificate,
  ndn_service_framework::ProviderExecutionLeaseTable& leaseTable,
  const ndn_service_framework::ExecutionLeaseBinding& binding,
  const std::string& leaseId,
  const std::string& providerEpoch,
  uint64_t nowMs);

DistributedExecutionCertificateView
distributedExecutionCertificateFromFields(
  const std::map<std::string, std::string>& fields);

} // namespace ndnsf::di

#endif // NDNSF_DI_DISTRIBUTED_EXECUTION_CONSISTENCY_HPP
