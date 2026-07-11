#include "ExecutionLeaseService.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <openssl/evp.h>

#include <sstream>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {
namespace {

std::string
operationName(LeaseOperation operation)
{
  switch (operation) {
    case LeaseOperation::Prepare: return "PREPARE";
    case LeaseOperation::Commit: return "COMMIT";
    case LeaseOperation::Abort: return "ABORT";
    case LeaseOperation::Renew: return "RENEW";
    case LeaseOperation::Release: return "RELEASE";
  }
  throw std::invalid_argument("unknown lease operation");
}

LeaseOperation
parseOperation(const std::string& value)
{
  if (value == "PREPARE") return LeaseOperation::Prepare;
  if (value == "COMMIT") return LeaseOperation::Commit;
  if (value == "ABORT") return LeaseOperation::Abort;
  if (value == "RENEW") return LeaseOperation::Renew;
  if (value == "RELEASE") return LeaseOperation::Release;
  throw std::invalid_argument("invalid lease operation");
}

std::string
escapeJson(const std::string& value)
{
  std::ostringstream output;
  for (unsigned char ch : value) {
    switch (ch) {
      case '"': output << "\\\""; break;
      case '\\': output << "\\\\"; break;
      case '\b': output << "\\b"; break;
      case '\f': output << "\\f"; break;
      case '\n': output << "\\n"; break;
      case '\r': output << "\\r"; break;
      case '\t': output << "\\t"; break;
      default:
        if (ch < 0x20) {
          static constexpr char hex[] = "0123456789abcdef";
          output << "\\u00" << hex[ch >> 4] << hex[ch & 0x0f];
        }
        else {
          output << static_cast<char>(ch);
        }
    }
  }
  return output.str();
}

std::string
quote(const std::string& value)
{
  return "\"" + escapeJson(value) + "\"";
}

std::string
encodeStringArray(const std::vector<std::string>& values)
{
  std::ostringstream output;
  output << '[';
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) output << ',';
    output << quote(values[i]);
  }
  output << ']';
  return output.str();
}

std::string
base64Encode(const ndn::Buffer& value)
{
  if (value.empty()) return {};
  std::string output(4 * ((value.size() + 2) / 3), '\0');
  const auto size = EVP_EncodeBlock(
    reinterpret_cast<unsigned char*>(&output[0]), value.data(),
    static_cast<int>(value.size()));
  output.resize(static_cast<size_t>(size));
  return output;
}

ndn::Buffer
base64Decode(const std::string& value)
{
  if (value.empty()) return {};
  if (value.size() % 4 != 0) {
    throw std::invalid_argument("invalid base64 lease binding");
  }
  ndn::Buffer output((value.size() / 4) * 3);
  const auto size = EVP_DecodeBlock(output.data(),
                                    reinterpret_cast<const unsigned char*>(value.data()),
                                    static_cast<int>(value.size()));
  if (size < 0) {
    throw std::invalid_argument("invalid base64 lease binding");
  }
  size_t adjusted = static_cast<size_t>(size);
  if (!value.empty() && value.back() == '=') --adjusted;
  if (value.size() > 1 && value[value.size() - 2] == '=') --adjusted;
  output.resize(adjusted);
  return output;
}

boost::property_tree::ptree
parseJson(const std::string& wire)
{
  boost::property_tree::ptree root;
  std::istringstream input(wire);
  try {
    boost::property_tree::read_json(input, root);
  }
  catch (const boost::property_tree::json_parser_error& error) {
    throw std::invalid_argument(std::string("malformed lease payload: ") + error.what());
  }
  if (root.get<std::string>("schema", "") != EXECUTION_LEASE_CODEC_SCHEMA) {
    throw std::invalid_argument("unsupported lease operation schema");
  }
  return root;
}

std::vector<std::string>
readStringArray(const boost::property_tree::ptree& root, const std::string& key)
{
  std::vector<std::string> output;
  const auto child = root.get_child_optional(key);
  if (!child) return output;
  for (const auto& item : *child) {
    output.push_back(item.second.get_value<std::string>());
  }
  return output;
}

} // namespace

std::string
encodeLeaseOperationRequest(const LeaseOperationRequest& request)
{
  std::ostringstream output;
  output << "{\"expiresAtMs\":" << request.expiresAtMs
         << ",\"idempotencyKey\":" << quote(request.idempotencyKey)
         << ",\"leaseId\":" << quote(request.leaseId)
         << ",\"operation\":" << quote(operationName(request.operation))
         << ",\"planDigest\":" << quote(request.planDigest)
         << ",\"providerEpoch\":" << quote(request.providerEpoch)
         << ",\"requestId\":" << quote(request.requestId)
         << ",\"resourceBindingProof\":" << quote(base64Encode(request.resourceBindingProof))
         << ",\"resourceBindingSchema\":" << quote(request.resourceBindingSchema)
         << ",\"roles\":" << encodeStringArray(request.roles)
         << ",\"schema\":" << quote(EXECUTION_LEASE_CODEC_SCHEMA) << '}';
  auto encoded = output.str();
  encoded.insert(encoded.size() - 1,
                 ",\"targetServiceName\":" + quote(request.targetServiceName));
  return encoded;
}

LeaseOperationRequest
decodeLeaseOperationRequest(const std::string& wire)
{
  const auto root = parseJson(wire);
  LeaseOperationRequest request;
  request.operation = parseOperation(root.get<std::string>("operation"));
  request.requestId = root.get<std::string>("requestId", "");
  request.planDigest = root.get<std::string>("planDigest", "");
  request.idempotencyKey = root.get<std::string>("idempotencyKey", "");
  request.targetServiceName = root.get<std::string>("targetServiceName", "");
  request.leaseId = root.get<std::string>("leaseId", "");
  request.providerEpoch = root.get<std::string>("providerEpoch", "");
  request.resourceBindingSchema = root.get<std::string>(
    "resourceBindingSchema", "ndnsf-di-binding-v1");
  request.resourceBindingProof = base64Decode(
    root.get<std::string>("resourceBindingProof", ""));
  request.roles = readStringArray(root, "roles");
  request.expiresAtMs = root.get<uint64_t>("expiresAtMs", 0);
  if (request.requestId.empty() || request.planDigest.empty() ||
      request.idempotencyKey.empty() || request.targetServiceName.empty()) {
    throw std::invalid_argument("lease request is missing required binding fields");
  }
  return request;
}

std::string
encodeLeaseOperationResponse(const LeaseOperationResponse& response)
{
  std::ostringstream output;
  output << "{\"conflictKeys\":" << encodeStringArray(response.conflictKeys)
         << ",\"executionDeadlineMs\":" << response.executionDeadlineMs
         << ",\"expiresAtMs\":" << response.expiresAtMs
         << ",\"leaseId\":" << quote(response.leaseId)
         << ",\"operation\":" << quote(operationName(response.operation))
         << ",\"providerEpoch\":" << quote(response.providerEpoch)
         << ",\"reasonCode\":" << quote(response.reasonCode)
         << ",\"retryAfterMs\":" << response.retryAfterMs
         << ",\"schema\":" << quote(EXECUTION_LEASE_CODEC_SCHEMA)
         << ",\"state\":" << quote(response.state)
         << ",\"status\":" << (response.status ? "true" : "false") << '}';
  return output.str();
}

LeaseOperationResponse
decodeLeaseOperationResponse(const std::string& wire)
{
  const auto root = parseJson(wire);
  LeaseOperationResponse response;
  response.status = root.get<bool>("status", false);
  response.operation = parseOperation(root.get<std::string>("operation"));
  response.reasonCode = root.get<std::string>("reasonCode", "");
  response.leaseId = root.get<std::string>("leaseId", "");
  response.providerEpoch = root.get<std::string>("providerEpoch", "");
  response.state = root.get<std::string>("state", "");
  response.expiresAtMs = root.get<uint64_t>("expiresAtMs", 0);
  response.executionDeadlineMs = root.get<uint64_t>("executionDeadlineMs", 0);
  response.conflictKeys = readStringArray(root, "conflictKeys");
  response.retryAfterMs = root.get<uint64_t>("retryAfterMs", 0);
  return response;
}

ExecutionLeaseService::ExecutionLeaseService(
  std::string providerName, std::string targetServiceName,
  ConflictKeyResolver conflictKeyResolver,
  std::string providerEpoch)
  : m_providerName(std::move(providerName))
  , m_targetServiceName(std::move(targetServiceName))
  , m_conflictKeyResolver(std::move(conflictKeyResolver))
  , m_table(std::move(providerEpoch))
{
  if (m_providerName.empty() || m_targetServiceName.empty() || !m_conflictKeyResolver) {
    throw std::invalid_argument("execution lease service requires provider and resolver");
  }
}

std::string
ExecutionLeaseService::handle(const ExecutionLeaseRequestContext& context,
                              const std::string& payload, uint64_t nowMs)
{
  LeaseOperationRequest request;
  try {
    request = decodeLeaseOperationRequest(payload);
  }
  catch (const std::exception&) {
    LeaseOperationResponse response;
    response.reasonCode = "LEASE_INTERNAL_ERROR";
    return encodeLeaseOperationResponse(response);
  }
  if (context.requesterIdentity.empty() || context.providerName != m_providerName ||
      context.serviceName != EXECUTION_LEASE_SERVICE_NAME ||
      context.requestId.empty()) {
    LeaseOperationResponse response;
    response.operation = request.operation;
    response.reasonCode = "LEASE_BINDING_MISMATCH";
    return encodeLeaseOperationResponse(response);
  }
  if (request.targetServiceName != m_targetServiceName) {
    LeaseOperationResponse response;
    response.operation = request.operation;
    response.reasonCode = "LEASE_SERVICE_MISMATCH";
    return encodeLeaseOperationResponse(response);
  }

  ndn_service_framework::ExecutionLeaseResult result;
  if (request.operation == LeaseOperation::Prepare) {
    std::lock_guard<std::mutex> lock(m_prepareMutex);
    ndn_service_framework::GenericExecutionLease lease;
    lease.providerName = m_providerName;
    lease.requesterName = context.requesterIdentity;
    lease.requestId = request.requestId;
    lease.serviceName = m_targetServiceName;
    lease.planDigest = request.planDigest;
    lease.resourceBindingSchema = request.resourceBindingSchema;
    lease.resourceBindingProof = request.resourceBindingProof;
    lease.conflictKeys = m_conflictKeyResolver(request, context);
    if (lease.conflictKeys.empty()) {
      LeaseOperationResponse response;
      response.operation = request.operation;
      response.reasonCode = "LEASE_CAPACITY_REJECTED";
      return encodeLeaseOperationResponse(response);
    }
    lease.expiresAtMs = request.expiresAtMs;
    lease.idempotencyKey = request.idempotencyKey;
    result = m_table.prepare(std::move(lease), nowMs);
  }
  else if (request.operation == LeaseOperation::Commit) {
    result = m_table.commit(request.leaseId, request.providerEpoch,
                            context.requesterIdentity, request.idempotencyKey, nowMs);
  }
  else if (request.operation == LeaseOperation::Abort) {
    result = m_table.abort(request.leaseId, request.providerEpoch,
                           context.requesterIdentity, request.idempotencyKey, nowMs);
  }
  else if (request.operation == LeaseOperation::Renew) {
    result = m_table.renew(request.leaseId, request.providerEpoch,
                           context.requesterIdentity, request.idempotencyKey,
                           nowMs, request.expiresAtMs);
  }
  else {
    result = m_table.release(request.leaseId, request.providerEpoch,
                             context.requesterIdentity, request.idempotencyKey, nowMs);
  }
  return encodeLeaseOperationResponse(fromCore(request.operation, result));
}

ndn_service_framework::ProviderExecutionLeaseTable&
ExecutionLeaseService::table() noexcept
{
  return m_table;
}

LeaseOperationResponse
ExecutionLeaseService::fromCore(
  LeaseOperation operation,
  const ndn_service_framework::ExecutionLeaseResult& result)
{
  LeaseOperationResponse response;
  response.status = result.status;
  response.operation = operation;
  response.reasonCode = result.reasonCode;
  response.leaseId = result.lease.leaseId;
  response.providerEpoch = result.lease.providerEpoch;
  response.state = ndn_service_framework::toString(result.lease.state);
  response.expiresAtMs = result.lease.expiresAtMs;
  response.executionDeadlineMs = result.lease.executionDeadlineMs;
  response.conflictKeys = result.lease.conflictKeys;
  response.retryAfterMs = result.retryAfterMs;
  return response;
}

} // namespace ndnsf::di
