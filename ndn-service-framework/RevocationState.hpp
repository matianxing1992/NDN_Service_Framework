#ifndef NDN_SERVICE_FRAMEWORK_REVOCATION_STATE_HPP
#define NDN_SERVICE_FRAMEWORK_REVOCATION_STATE_HPP

#include "PolicyStatus.hpp"

#include <set>
#include <string>

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

/** Fail-closed, service-scoped view of the latest authenticated status. */
class RevocationState
{
public:
  explicit RevocationState(ndn::Name serviceName = {});

  bool acceptStatus(const PolicyStatusData& status, uint64_t nowMs,
                    bool controllerSignatureValid = true);
  bool hasCurrentStatus() const;
  const ControllerVersion& currentVersion() const;
  const PolicyStatusData& currentStatus() const;

  RevocationDecision authorize(const AuthorizationSubject& subject,
                               ProtectedTransition transition,
                               uint64_t nowMs) const;

  bool cacheInvalidated(const std::string& family) const;
  size_t invalidatedCacheCount() const;
  void clearInvalidationEvidence();

private:
  bool isRevoked(const AuthorizationSubject& subject) const;
  void invalidateAllFamilies();

  ndn::Name m_serviceName;
  bool m_hasStatus = false;
  PolicyStatusData m_status;
  std::set<std::string> m_invalidatedFamilies;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_REVOCATION_STATE_HPP
