#ifndef NDNSF_DISTRIBUTED_REPO_REPO_STORE_BACKEND_HPP
#define NDNSF_DISTRIBUTED_REPO_REPO_STORE_BACKEND_HPP

#include "ndnsf-distributed-repo/ArtifactTypes.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ndnsf_distributed_repo {

struct ArtifactByteRange
{
  uint64_t offsetBytes = 0;
  uint64_t lengthBytes = 0;
};

enum class ArtifactLifecycleState
{
  Absent,
  Reserved,
  Receiving,
  Verified,
  Committed,
  Active,
  Failed,
  Expired,
};

std::string
toString(ArtifactLifecycleState state);

ArtifactLifecycleState
parseArtifactLifecycleState(const std::string& value);

bool
isAllowedArtifactTransition(ArtifactLifecycleState from,
                            ArtifactLifecycleState to) noexcept;

struct ArtifactLifecycleEvent
{
  std::string eventId;
  std::string operationId;
  std::string artifactDigest;
  uint64_t generation = 0;
  ArtifactLifecycleState fromState = ArtifactLifecycleState::Absent;
  ArtifactLifecycleState toState = ArtifactLifecycleState::Absent;
  uint64_t eventTimeMs = 0;
  bool accepted = true;
  std::string detail;
};

class PayloadStore
{
public:
  virtual ~PayloadStore() = default;

  virtual void begin(const ArtifactReference& artifact, uint64_t generation) = 0;
  virtual void writeRange(const ArtifactReference& artifact, uint64_t generation,
                          ArtifactByteRange range,
                          const std::vector<uint8_t>& bytes) = 0;
  virtual std::vector<uint8_t> readRange(const ArtifactReference& artifact,
                                         uint64_t generation,
                                         ArtifactByteRange range) const = 0;
  virtual void markVerified(const ArtifactReference& artifact, uint64_t generation,
                            ArtifactByteRange range) = 0;
  virtual std::vector<ArtifactByteRange>
  verifiedRanges(const ArtifactReference& artifact, uint64_t generation) const = 0;
  virtual void flush(const ArtifactReference& artifact, uint64_t generation) = 0;
  virtual void finalize(const ArtifactReference& artifact, uint64_t generation) = 0;
  virtual bool isCommitted(const ArtifactReference& artifact,
                           uint64_t generation) const = 0;
  virtual void abort(const ArtifactReference& artifact, uint64_t generation) = 0;
};

class MetadataStore
{
public:
  virtual ~MetadataStore() = default;

  virtual uint64_t schemaGeneration() const = 0;
  virtual void appendLifecycleEvent(const ArtifactLifecycleEvent& event) = 0;
  virtual std::vector<ArtifactLifecycleEvent>
  lifecycleEvents(const std::string& operationId) const = 0;
  virtual ArtifactLifecycleState
  currentLifecycleState(const std::string& operationId) const = 0;
};

class BackendOwnershipLease
{
public:
  BackendOwnershipLease(std::string backendPath, std::string ownerId);
  ~BackendOwnershipLease();

  BackendOwnershipLease(const BackendOwnershipLease&) = delete;
  BackendOwnershipLease& operator=(const BackendOwnershipLease&) = delete;

  BackendOwnershipLease(BackendOwnershipLease&& other) noexcept;
  BackendOwnershipLease& operator=(BackendOwnershipLease&& other) noexcept;

  const std::string& ownerId() const noexcept;
  const std::string& lockPath() const noexcept;
  bool ownsBackend() const noexcept;

private:
  void release() noexcept;

private:
  std::string m_ownerId;
  std::string m_lockPath;
  int m_lockFd = -1;
};

class RepositoryStoreFacade
{
public:
  RepositoryStoreFacade(std::string backendPath, std::string ownerId,
                        std::shared_ptr<PayloadStore> payloadStore,
                        std::shared_ptr<MetadataStore> metadataStore);

  PayloadStore& payload();
  MetadataStore& metadata();
  const BackendOwnershipLease& ownership() const noexcept;

  ArtifactLifecycleEvent transition(ArtifactLifecycleEvent event);

private:
  BackendOwnershipLease m_ownership;
  std::shared_ptr<PayloadStore> m_payloadStore;
  std::shared_ptr<MetadataStore> m_metadataStore;
};

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_REPO_STORE_BACKEND_HPP
