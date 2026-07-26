#include "NDNSF-DistributedInference/cpp/ndnsf-di/DistributedExecutionConsistency.hpp"

#include <algorithm>
#include <set>
#include <sstream>

namespace ndnsf::di {
namespace {

std::string
field(const std::map<std::string, std::string>& fields, const char* name)
{
  const auto found = fields.find(name);
  return found == fields.end() ? std::string{} : found->second;
}

uint64_t
unsignedField(const std::map<std::string, std::string>& fields, const char* name)
{
  const auto value = field(fields, name);
  if (value.empty()) {
    return 0;
  }
  try {
    std::size_t consumed = 0;
    const auto parsed = std::stoull(value, &consumed);
    return consumed == value.size() ? parsed : 0;
  }
  catch (const std::exception&) {
    return 0;
  }
}

std::vector<std::string>
splitMembers(const std::string& value)
{
  std::vector<std::string> members;
  std::istringstream input(value);
  std::string member;
  while (std::getline(input, member, ',')) {
    if (!member.empty()) {
      members.push_back(member);
    }
  }
  return members;
}

} // namespace

DistributedExecutionCertificateResult
validateDistributedExecutionCertificate(
  const DistributedExecutionCertificateView& certificate,
  ndn_service_framework::ProviderExecutionLeaseTable& leaseTable,
  const ndn_service_framework::ExecutionLeaseBinding& binding,
  const std::string& leaseId,
  const std::string& providerEpoch,
  uint64_t nowMs)
{
  if (certificate.schema != EXECUTION_COMMIT_CERTIFICATE_SCHEMA) {
    return {false, "DI_CERTIFICATE_SCHEMA_UNSUPPORTED"};
  }
  if (certificate.digest.empty() || certificate.requestId != binding.requestId ||
      certificate.planDigest != binding.planDigest || certificate.attemptEpoch == 0) {
    return {false, "DI_CERTIFICATE_BINDING_MISMATCH"};
  }
  const std::set<std::string> unique(
    certificate.memberKeys.begin(), certificate.memberKeys.end());
  if (certificate.expectedCount == 0 ||
      certificate.expectedCount != certificate.receiptCount ||
      certificate.expectedCount != certificate.memberKeys.size() ||
      unique.size() != certificate.memberKeys.size()) {
    return {false, "DI_CERTIFICATE_RECEIPT_SET_MISMATCH"};
  }
  if (certificate.localMemberKey.empty() ||
      unique.count(certificate.localMemberKey) != 1) {
    return {false, "DI_CERTIFICATE_PROVIDER_NOT_MEMBER"};
  }
  const auto lease = leaseTable.validate(
    leaseId, providerEpoch, binding, nowMs);
  if (!lease.status) {
    return {false, lease.reasonCode};
  }
  return {true, "OK"};
}

DistributedExecutionCertificateView
distributedExecutionCertificateFromFields(
  const std::map<std::string, std::string>& fields)
{
  DistributedExecutionCertificateView result;
  result.schema = field(fields, "executionCertificateSchema");
  result.digest = field(fields, "executionCertificateDigest");
  result.requestId = field(fields, "executionCertificateRequestId");
  if (result.requestId.empty()) {
    result.requestId = field(fields, "executionRequestId");
  }
  if (result.requestId.empty()) {
    result.requestId = field(fields, "executionLeaseTransactionId");
  }
  result.attemptEpoch = unsignedField(fields, "executionCertificateAttemptEpoch");
  if (result.attemptEpoch == 0) {
    result.attemptEpoch = unsignedField(fields, "executionAttemptEpoch");
  }
  result.planDigest = field(fields, "executionCertificatePlanDigest");
  if (result.planDigest.empty()) {
    result.planDigest = field(fields, "executionLeasePlanDigest");
  }
  result.expectedCount = static_cast<std::size_t>(
    unsignedField(fields, "executionCertificateExpectedCount"));
  result.receiptCount = static_cast<std::size_t>(
    unsignedField(fields, "executionCertificateReceiptCount"));
  result.memberKeys = splitMembers(field(fields, "executionCertificateMembers"));
  if (result.expectedCount == 0) {
    result.expectedCount = result.memberKeys.size();
  }
  if (result.receiptCount == 0) {
    result.receiptCount = result.memberKeys.size();
  }
  result.localMemberKey = field(fields, "executionCertificateLocalMember");
  return result;
}

} // namespace ndnsf::di
