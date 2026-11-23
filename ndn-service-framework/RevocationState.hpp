#ifndef NDN_SERVICE_FRAMEWORK_REVOCATION_STATE_HPP
#define NDN_SERVICE_FRAMEWORK_REVOCATION_STATE_HPP

#include "PolicyStatus.hpp"

#include <optional>
#include <set>
#include <string>

namespace ndn::security { class Certificate; }

namespace ndn_service_framework {

enum class ProtectedTransition
{
  DISCOVERY,
  ACK_COLLECTION,
  SELECTION,
  PROVIDER_EXECUTION,
  RESPONSE_DELIVERY,
  STREAM_EVENT,
};

struct AuthorizationSubject
{
  ndn::Name identity;
  std::string certificateDigest;
  ndn::Name serviceName;
  ndn::Name authorizationAttribute;
};

struct RevocationDecision
{
  bool allowed = false;
  std::string reason;
};

struct PolicyLeaseBudget
{
  uint64_t maxLeaseMs;
  uint64_t maxClockSkewMs;
};

/** Fail-closed, service-scoped view of the latest authenticated status. */
class RevocationState
{
public:
  explicit RevocationState(ndn::Name serviceName = {});

  bool acceptStatus(const PolicyStatusData& status, uint64_t nowMs,
                    bool controllerSignatureValid = true, bool freshQuery = false,
                    std::optional<PolicyLeaseBudget> budget = std::nullopt);
  uint64_t remainingLeaseMs(uint64_t nowMs) const;
  /** Fence the exact lease once its local monotonic deadline fires. */
  bool expireStatus(const ControllerVersion& version, uint64_t validUntilMs);
  bool hasCurrentStatus() const;
  const ControllerVersion& currentVersion() const;
  const PolicyStatusData& currentStatus() const;

  RevocationDecision authorize(const AuthorizationSubject& subject,
                               ProtectedTransition transition,
                               uint64_t nowMs) const;

  /** Check both certificates used by a local actor for a protected service operation. */
  RevocationDecision authorizeLocalCredentials(const ndn::security::Certificate& recipient,
                                              const ndn::security::Certificate& signer,
                                              const ndn::Name& service, const ndn::Name& attribute,
                                              ProtectedTransition transition, uint64_t nowMs) const;

  bool cacheInvalidated(const std::string& family) const;
  size_t invalidatedCacheCount() const;
  void clearInvalidationEvidence();

private:
  RevocationDecision evaluate(const AuthorizationSubject& subject, ProtectedTransition transition,
                             uint64_t nowMs, const std::string& signingDigest) const;
  bool isRevoked(const AuthorizationSubject& subject, const std::string& signingDigest) const;
  void invalidateAllFamilies();

  ndn::Name m_serviceName;
  bool m_hasStatus = false;
  bool m_statusExpired = false;
  PolicyStatusData m_status;
  uint64_t m_wallDeadlineMs = 0;
  std::optional<ndn::time::steady_clock::time_point> m_steadyDeadline;
  std::set<std::string> m_invalidatedFamilies;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_REVOCATION_STATE_HPP
