#include "ndnsf-distributed-repo/RepoStoreBackend.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <stdexcept>
#include <sys/file.h>
#include <unistd.h>

namespace ndnsf_distributed_repo {

std::string
toString(ArtifactLifecycleState state)
{
  switch (state) {
  case ArtifactLifecycleState::Absent:
    return "ABSENT";
  case ArtifactLifecycleState::Reserved:
    return "RESERVED";
  case ArtifactLifecycleState::Receiving:
    return "RECEIVING";
  case ArtifactLifecycleState::Verified:
    return "VERIFIED";
  case ArtifactLifecycleState::Committed:
    return "COMMITTED";
  case ArtifactLifecycleState::Active:
    return "ACTIVE";
  case ArtifactLifecycleState::Failed:
    return "FAILED";
  case ArtifactLifecycleState::Expired:
    return "EXPIRED";
  }
  throw std::invalid_argument("repo-lifecycle-invalid-state: unknown state");
}

ArtifactLifecycleState
parseArtifactLifecycleState(const std::string& value)
{
  if (value == "ABSENT") {
    return ArtifactLifecycleState::Absent;
  }
  if (value == "RESERVED") {
    return ArtifactLifecycleState::Reserved;
  }
  if (value == "RECEIVING") {
    return ArtifactLifecycleState::Receiving;
  }
  if (value == "VERIFIED") {
    return ArtifactLifecycleState::Verified;
  }
  if (value == "COMMITTED") {
    return ArtifactLifecycleState::Committed;
  }
  if (value == "ACTIVE") {
    return ArtifactLifecycleState::Active;
  }
  if (value == "FAILED") {
    return ArtifactLifecycleState::Failed;
  }
  if (value == "EXPIRED") {
    return ArtifactLifecycleState::Expired;
  }
  throw std::invalid_argument(
    "repo-lifecycle-invalid-state: unsupported state " + value);
}

bool
isAllowedArtifactTransition(ArtifactLifecycleState from,
                            ArtifactLifecycleState to) noexcept
{
  switch (from) {
  case ArtifactLifecycleState::Absent:
    return to == ArtifactLifecycleState::Reserved;
  case ArtifactLifecycleState::Reserved:
    return to == ArtifactLifecycleState::Receiving ||
           to == ArtifactLifecycleState::Failed ||
           to == ArtifactLifecycleState::Expired;
  case ArtifactLifecycleState::Receiving:
    return to == ArtifactLifecycleState::Verified ||
           to == ArtifactLifecycleState::Failed ||
           to == ArtifactLifecycleState::Expired;
  case ArtifactLifecycleState::Verified:
    return to == ArtifactLifecycleState::Committed ||
           to == ArtifactLifecycleState::Failed ||
           to == ArtifactLifecycleState::Expired;
  case ArtifactLifecycleState::Committed:
    return to == ArtifactLifecycleState::Active;
  case ArtifactLifecycleState::Active:
  case ArtifactLifecycleState::Failed:
  case ArtifactLifecycleState::Expired:
    return false;
  }
  return false;
}

BackendOwnershipLease::BackendOwnershipLease(std::string backendPath,
                                             std::string ownerId)
  : m_ownerId(std::move(ownerId))
  , m_lockPath(std::move(backendPath))
{
  if (m_lockPath.empty() || m_ownerId.empty()) {
    throw std::invalid_argument(
      "repo-persistence-invalid-owner: backend path and owner ID are required");
  }
  if (m_lockPath == ":memory:") {
    m_lockPath.clear();
    return;
  }
  std::error_code pathError;
  const auto canonical = std::filesystem::weakly_canonical(m_lockPath, pathError);
  if (!pathError) {
    m_lockPath = canonical.string();
  }
  else {
    pathError.clear();
    const auto absolute = std::filesystem::absolute(m_lockPath, pathError);
    if (!pathError) {
      m_lockPath = absolute.lexically_normal().string();
    }
  }
  m_lockPath += ".authority.lock";
  m_lockFd = ::open(m_lockPath.c_str(), O_CREAT | O_RDWR, 0600);
  if (m_lockFd < 0) {
    throw std::runtime_error(
      "repo-persistence-lock-open: " + std::string(std::strerror(errno)));
  }
  if (::flock(m_lockFd, LOCK_EX | LOCK_NB) != 0) {
    const auto message = std::string(std::strerror(errno));
    release();
    throw std::runtime_error(
      "repo-persistence-owned: backend already has an authoritative owner: " +
      message);
  }
  const auto written = ::ftruncate(m_lockFd, 0) == 0
    ? ::write(m_lockFd, m_ownerId.data(), m_ownerId.size()) : -1;
  if (written != static_cast<ssize_t>(m_ownerId.size()) ||
      ::fsync(m_lockFd) != 0) {
    const auto message = std::string(std::strerror(errno));
    release();
    throw std::runtime_error(
      "repo-persistence-lock-write: " + message);
  }
}

BackendOwnershipLease::~BackendOwnershipLease()
{
  release();
}

BackendOwnershipLease::BackendOwnershipLease(BackendOwnershipLease&& other) noexcept
  : m_ownerId(std::move(other.m_ownerId))
  , m_lockPath(std::move(other.m_lockPath))
  , m_lockFd(other.m_lockFd)
{
  other.m_lockFd = -1;
}

BackendOwnershipLease&
BackendOwnershipLease::operator=(BackendOwnershipLease&& other) noexcept
{
  if (this != &other) {
    release();
    m_ownerId = std::move(other.m_ownerId);
    m_lockPath = std::move(other.m_lockPath);
    m_lockFd = other.m_lockFd;
    other.m_lockFd = -1;
  }
  return *this;
}

const std::string&
BackendOwnershipLease::ownerId() const noexcept
{
  return m_ownerId;
}

const std::string&
BackendOwnershipLease::lockPath() const noexcept
{
  return m_lockPath;
}

bool
BackendOwnershipLease::ownsBackend() const noexcept
{
  return m_lockPath.empty() || m_lockFd >= 0;
}

void
BackendOwnershipLease::release() noexcept
{
  if (m_lockFd >= 0) {
    ::flock(m_lockFd, LOCK_UN);
    ::close(m_lockFd);
    m_lockFd = -1;
  }
}

RepositoryStoreFacade::RepositoryStoreFacade(
  std::string backendPath, std::string ownerId,
  std::shared_ptr<PayloadStore> payloadStore,
  std::shared_ptr<MetadataStore> metadataStore)
  : m_ownership(std::move(backendPath), std::move(ownerId))
  , m_payloadStore(std::move(payloadStore))
  , m_metadataStore(std::move(metadataStore))
{
  if (m_payloadStore == nullptr || m_metadataStore == nullptr) {
    throw std::invalid_argument(
      "repo-persistence-invalid-facade: payload and metadata stores are required");
  }
}

PayloadStore&
RepositoryStoreFacade::payload()
{
  return *m_payloadStore;
}

MetadataStore&
RepositoryStoreFacade::metadata()
{
  return *m_metadataStore;
}

const BackendOwnershipLease&
RepositoryStoreFacade::ownership() const noexcept
{
  return m_ownership;
}

ArtifactLifecycleEvent
RepositoryStoreFacade::transition(ArtifactLifecycleEvent event)
{
  if (event.eventId.empty() || event.operationId.empty() ||
      event.artifactDigest.empty() || event.eventTimeMs == 0) {
    throw std::invalid_argument(
      "repo-lifecycle-invalid-event: identity and timestamp are required");
  }
  validateIdentifier(
    event.eventId, 256, "eventId", "repo-lifecycle-invalid-event");
  validateIdentifier(
    event.operationId, 256, "operationId", "repo-lifecycle-invalid-event");
  if (event.detail.size() > 16 * 1024) {
    throw std::invalid_argument(
      "repo-lifecycle-invalid-event: detail exceeds 16 KiB");
  }
  validateDigest("sha256", event.artifactDigest, "artifactDigest");
  const auto priorEvents =
    m_metadataStore->lifecycleEvents(event.operationId);
  for (const auto& prior : priorEvents) {
    if (prior.eventId != event.eventId) {
      continue;
    }
    if (prior.operationId != event.operationId ||
        prior.artifactDigest != event.artifactDigest ||
        prior.generation != event.generation ||
        prior.fromState != event.fromState ||
        prior.toState != event.toState ||
        prior.eventTimeMs != event.eventTimeMs ||
        prior.detail != event.detail) {
      throw std::invalid_argument(
        "repo-lifecycle-event-conflict: event ID reused with different content");
    }
    if (!prior.accepted) {
      throw std::invalid_argument(
        "repo-lifecycle-replayed-rejection: lifecycle event was previously rejected");
    }
    return prior;
  }
  for (auto it = priorEvents.rbegin(); it != priorEvents.rend(); ++it) {
    if (!it->accepted) {
      continue;
    }
    if (it->artifactDigest != event.artifactDigest ||
        it->generation != event.generation) {
      event.accepted = false;
      m_metadataStore->appendLifecycleEvent(event);
      throw std::invalid_argument(
        "repo-lifecycle-identity-conflict: operation identity cannot change");
    }
    break;
  }
  const auto current = m_metadataStore->currentLifecycleState(event.operationId);
  if (current != event.fromState) {
    event.accepted = false;
    m_metadataStore->appendLifecycleEvent(event);
    throw std::invalid_argument(
      "repo-lifecycle-state-conflict: expected " + toString(event.fromState) +
      " but authoritative state is " + toString(current));
  }
  if (!isAllowedArtifactTransition(event.fromState, event.toState)) {
    event.accepted = false;
    m_metadataStore->appendLifecycleEvent(event);
    throw std::invalid_argument(
      "repo-lifecycle-illegal-transition: " + toString(event.fromState) +
      " -> " + toString(event.toState));
  }
  event.accepted = true;
  m_metadataStore->appendLifecycleEvent(event);
  return event;
}

} // namespace ndnsf_distributed_repo
