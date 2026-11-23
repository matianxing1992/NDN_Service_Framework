#ifndef NDN_SERVICE_FRAMEWORK_DETAIL_HISTORICAL_POLICY_STATUS_HPP
#define NDN_SERVICE_FRAMEWORK_DETAIL_HISTORICAL_POLICY_STATUS_HPP

#include "../PolicyStatus.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/certificate.hpp>

#include <chrono>
#include <optional>

namespace ndn_service_framework {
struct HistoricalReadRequest;
}

namespace ndn_service_framework::detail {

struct AdmittedPolicyStatus
{
  PolicyStatusData data;
  std::chrono::steady_clock::time_point deadline;
};

/** Verify a fresh exact-name status against the pinned signer, floor and local lease budget. */
std::optional<AdmittedPolicyStatus>
fetchCurrentPolicyStatus(ndn::Face& face, const ndn::Name& controller, const ndn::Name& service,
                         const ndn::security::Certificate& certificate, uint32_t timeoutMs);

/** Read with an ordinary, current identity DKEY and the deployed Validator. */
std::optional<std::string>
readCurrentNacObject(ndn::Face& face, ndn::KeyChain& keys, const HistoricalReadRequest& request,
                     const std::string& trustSchema);

} // namespace ndn_service_framework::detail

#endif // NDN_SERVICE_FRAMEWORK_DETAIL_HISTORICAL_POLICY_STATUS_HPP
