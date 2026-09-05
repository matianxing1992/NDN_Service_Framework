#include "RevocationState.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace ndn_service_framework {
namespace {

bool
sameWire(const PolicyStatusData& left, const PolicyStatusData& right)
{
  try {
    const auto lhs = left.wireEncode();
    const auto rhs = right.wireEncode();
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin());
  }
  catch (const std::exception&) {
    return false;
  }
}

const char*
transitionName(ProtectedTransition transition)
{
  switch (transition) {
  case ProtectedTransition::DISCOVERY: return "discovery";
  case ProtectedTransition::ACK_COLLECTION: return "ack_collection";
  case ProtectedTransition::SELECTION: return "selection";
  case ProtectedTransition::PROVIDER_EXECUTION: return "provider_execution";
  case ProtectedTransition::RESPONSE_DELIVERY: return "response_delivery";
  case ProtectedTransition::STREAM_EVENT: return "stream_event";
  }
  return "unknown";
}

bool
hasCanonicalServiceAttribute(const ndn::Name& serviceName,
                             const ndn::Name& attribute)
{
  if (serviceName.empty() || attribute.empty())
    return false;
  ndn::Name permission("/PERMISSION");
  permission.append(serviceName);
  ndn::Name service("/SERVICE");
  service.append(serviceName);
  return attribute == permission || attribute == service;
}

} // namespace

RevocationState::RevocationState(ndn::Name serviceName)
  : m_serviceName(std::move(serviceName))
{
}

bool
RevocationState::acceptStatus(const PolicyStatusData& status, uint64_t nowMs,
                              bool controllerSignatureValid)
{
  if (!controllerSignatureValid || !status.validate(nowMs) ||
      (!m_serviceName.empty() && status.getServiceName() != m_serviceName))
    return false;

  if (m_hasStatus) {
    const auto relation = status.getControllerVersion().compare(
        m_status.getControllerVersion());
    if (relation < 0)
      return false;
    if (relation == 0)
      return sameWire(status, m_status);
  }

  m_status = status;
  m_hasStatus = true;
  invalidateAllFamilies();
  return true;
}

bool
RevocationState::hasCurrentStatus() const
{
  return m_hasStatus;
}

const ControllerVersion&
RevocationState::currentVersion() const
{
  static const ControllerVersion invalid;
  return m_hasStatus ? m_status.getControllerVersion() : invalid;
}

const PolicyStatusData&
RevocationState::currentStatus() const
{
  return m_status;
}

bool
RevocationState::isRevoked(const AuthorizationSubject& subject) const
{
  for (const auto& target : m_status.getRevocations()) {
    switch (target.kind) {
    case RevocationKind::IDENTITY:
      if (target.targetIdentity == subject.identity)
        return true;
      break;
    case RevocationKind::CERTIFICATE:
      if (target.certificateDigest == subject.certificateDigest &&
          (target.targetIdentity.empty() ||
           target.targetIdentity == subject.identity))
        return true;
      break;
    case RevocationKind::SERVICE_AUTHORIZATION:
      if (target.targetIdentity == subject.identity &&
          target.serviceName == subject.serviceName &&
          target.authorizationAttribute == subject.authorizationAttribute)
        return true;
      break;
    }
  }
  return false;
}

RevocationDecision
RevocationState::authorize(const AuthorizationSubject& subject,
                           ProtectedTransition transition,
                           uint64_t nowMs) const
{
  if (!m_hasStatus)
    return {false, "no_authenticated_controller_status"};
  if (subject.identity.empty() || subject.certificateDigest.empty())
    return {false, "incomplete_authorization_subject"};
  if (subject.serviceName != m_status.getServiceName())
    return {false, "service_scope_mismatch"};
  const bool statusHasServiceTarget = std::any_of(
      m_status.getRevocations().begin(), m_status.getRevocations().end(),
      [] (const RevocationTarget& target) {
        return target.kind == RevocationKind::SERVICE_AUTHORIZATION;
      });
  if (statusHasServiceTarget &&
      !hasCanonicalServiceAttribute(subject.serviceName,
                                    subject.authorizationAttribute))
    return {false, "incomplete_authorization_attribute"};
  if (nowMs < m_status.getValidFromMs() ||
      nowMs >= m_status.getValidUntilMs())
    return {false, "controller_status_expired"};
  if (isRevoked(subject))
    return {false, std::string("revoked_before_") + transitionName(transition)};
  return {true, "authorized"};
}

bool
RevocationState::cacheInvalidated(const std::string& family) const
{
  return m_invalidatedFamilies.count(family) != 0;
}

size_t
RevocationState::invalidatedCacheCount() const
{
  return m_invalidatedFamilies.size();
}

void
RevocationState::clearInvalidationEvidence()
{
  m_invalidatedFamilies.clear();
}

void
RevocationState::invalidateAllFamilies()
{
  // A complete ControllerVersion change invalidates all material that can
  // authorize a protected transition.  The state itself is service-scoped,
  // so this does not invalidate caches belonging to another service.
  for (const auto& family : {"abe", "message-key", "targeted-token",
                             "selection-binding", "nonce", "replay"})
    m_invalidatedFamilies.emplace(family);
}

} // namespace ndn_service_framework
