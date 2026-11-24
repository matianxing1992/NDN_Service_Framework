#include "RevocationState.hpp"

#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/util/sha256.hpp>
#include <ndn-cxx/util/string-helper.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace ndn_service_framework {
namespace {

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

bool
sameVersionGuards(const PolicyStatusData& candidate, const PolicyStatusData& current)
{
  return candidate.getServiceName() == current.getServiceName() &&
         candidate.getControllerVersion() == current.getControllerVersion() &&
         candidate.getAbePublicParametersName() == current.getAbePublicParametersName() &&
         candidate.getAbePublicParametersDigest() == current.getAbePublicParametersDigest();
}

/** True when `current`'s revocations are all present in `candidate`, i.e. the candidate only adds withdrawals.
 *  Compared field by field rather than by encoded wire: revocations reach this point from decoded statuses whose
 *  encoding is not guaranteed to be reproducible. */
bool
revocationsSuperset(const PolicyStatusData& candidate, const PolicyStatusData& current)
{
  const auto sameTarget = [] (const RevocationTarget& left, const RevocationTarget& right) {
    return left.kind == right.kind && left.targetIdentity == right.targetIdentity &&
           left.serviceName == right.serviceName &&
           sameCertificateDigest(left.certificateDigest, right.certificateDigest) &&
           left.authorizationAttribute == right.authorizationAttribute;
  };
  for (const auto& existing : current.getRevocations()) {
    bool found = false;
    for (const auto& incoming : candidate.getRevocations()) {
      if (sameTarget(existing, incoming)) {
        found = true;
        break;
      }
    }
    if (!found)
      return false;
  }
  return true;
}

} // namespace

RevocationState::RevocationState(ndn::Name serviceName)
  : m_serviceName(std::move(serviceName))
{
}

bool
RevocationState::acceptStatus(const PolicyStatusData& status, uint64_t nowMs,
                              bool controllerSignatureValid, bool freshQuery,
                              std::optional<PolicyLeaseBudget> budget)
{
  if (!controllerSignatureValid || !status.validate(nowMs) ||
      (!m_serviceName.empty() && status.getServiceName() != m_serviceName))
    return false;

  const auto remaining = budget ?
    status.remainingLeaseMs(nowMs, budget->maxLeaseMs, budget->maxClockSkewMs) :
    status.getValidUntilMs() - nowMs;
  if (remaining == 0 || (budget && remaining > 300000))
    return false;
  const auto install = [&] {
    m_status = status;
    m_statusExpired = false;
    m_wallDeadlineMs = nowMs + remaining;
    m_steadyDeadline.reset();
    if (budget) {
      m_steadyDeadline = ndn::time::steady_clock::now() +
        ndn::time::milliseconds(static_cast<int64_t>(remaining));
    }
  };
  if (m_hasStatus) {
    const auto relation = status.getControllerVersion().compare(
        m_status.getControllerVersion());
    if (relation < 0)
      return false;
    if (relation == 0) {
      if (!status.canReplaceSameVersion(m_status, freshQuery))
        return false;
      if (status.getValidUntilMs() == m_status.getValidUntilMs())
        return remainingLeaseMs(nowMs) != 0;
      install();
      return true;
    }
  }

  install();
  m_hasStatus = true;
  invalidateAllFamilies();
  return true;
}

uint64_t
RevocationState::remainingLeaseMs(uint64_t nowMs) const
{
  if (!m_hasStatus || m_statusExpired || nowMs < m_status.getValidFromMs() ||
      nowMs >= m_wallDeadlineMs)
    return 0;
  const auto wallRemaining = m_wallDeadlineMs - nowMs;
  if (!m_steadyDeadline)
    return wallRemaining;

  const auto remaining = ndn::time::duration_cast<ndn::time::milliseconds>(
    *m_steadyDeadline - ndn::time::steady_clock::now()).count();
  return remaining > 0 ? std::min(wallRemaining, static_cast<uint64_t>(remaining)) : 0;
}

bool
RevocationState::expireStatus(const ControllerVersion& version, uint64_t validUntilMs)
{
  if (!m_hasStatus || m_statusExpired || m_status.getControllerVersion() != version ||
      m_status.getValidUntilMs() != validUntilMs)
    return false;
  m_statusExpired = true;
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
RevocationState::isRevoked(const AuthorizationSubject& subject,
                           const std::string& signingDigest) const
{
  for (const auto& target : m_status.getRevocations()) {
    switch (target.kind) {
    case RevocationKind::IDENTITY:
      if (target.targetIdentity == subject.identity)
        return true;
      break;
    case RevocationKind::CERTIFICATE:
      if ((target.targetIdentity.empty() || target.targetIdentity == subject.identity) &&
          (sameCertificateDigest(target.certificateDigest, subject.certificateDigest) ||
           (!signingDigest.empty() && sameCertificateDigest(target.certificateDigest, signingDigest))))
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
  return evaluate(subject, transition, nowMs, {});
}

RevocationDecision
RevocationState::authorizeLocalCredentials(const ndn::security::Certificate& recipient,
                                           const ndn::security::Certificate& signer,
                                           const ndn::Name& service, const ndn::Name& attribute,
                                           ProtectedTransition transition, uint64_t nowMs) const
{
  if (recipient.getIdentity() != signer.getIdentity())
    return {false, "local_credential_identity_mismatch"};
  const auto digest = [] (const ndn::Block& wire) {
    return "sha256:" + ndn::toHex(*ndn::util::Sha256::computeDigest(
      ndn::span<const uint8_t>(wire.data(), wire.size())), false);
  };
  const auto& recipientWire = recipient.wireEncode();
  const auto& signingWire = signer.wireEncode();
  const AuthorizationSubject subject{recipient.getIdentity(), digest(recipientWire),
                                     service, attribute};
  return evaluate(subject, transition, nowMs,
                  recipientWire == signingWire ? std::string{} : digest(signingWire));
}

RevocationDecision
RevocationState::evaluate(const AuthorizationSubject& subject, ProtectedTransition transition,
                          uint64_t nowMs, const std::string& signingDigest) const
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
  if (remainingLeaseMs(nowMs) == 0)
    return {false, "controller_status_expired"};
  if (isRevoked(subject, signingDigest))
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
