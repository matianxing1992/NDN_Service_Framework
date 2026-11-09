#ifndef NDN_SERVICE_FRAMEWORK_CONTROLLER_GENERATION_STORE_HPP
#define NDN_SERVICE_FRAMEWORK_CONTROLLER_GENERATION_STORE_HPP

#include "ControllerVersion.hpp"
#include "PolicyStatus.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndn_service_framework {

/** Small durable fenced writer store for one Controller identity. */
class ControllerGenerationStore
{
public:
  class CommitUncertain : public std::runtime_error
  {
  public:
    using std::runtime_error::runtime_error;
  };

  /** Private ABE pair bound to this writer and its effective policy. */
  struct AbeMaterial
  {
    std::string owner;
    std::string signingKeyDigest;
    std::string policyDigest;
    std::string publicParameters;
    std::string masterSecret;
    bool rotationRequired = false;

    AbeMaterial() = default;
    AbeMaterial(const AbeMaterial&) = default;
    AbeMaterial& operator=(const AbeMaterial& other);
    ~AbeMaterial();
  };

  explicit ControllerGenerationStore(std::filesystem::path statePath);
  ~ControllerGenerationStore();

  ControllerGenerationStore(const ControllerGenerationStore&) = delete;
  ControllerGenerationStore& operator=(const ControllerGenerationStore&) = delete;

  /** Acquire an identity-scoped default, preserving a proven legacy store in place. */
  static std::unique_ptr<ControllerGenerationStore>
  openDefault(const std::string& identity, const std::filesystem::path& legacyStatePath);

  const std::filesystem::path&
  statePath() const noexcept
  {
    return m_statePath;
  }

  bool acquireWriter(const std::string& owner);
  bool ownsWriter() const;
  uint64_t fencingValue() const;
  void releaseWriter();

  std::optional<ControllerVersion> load() const;
  std::vector<RevocationTarget> loadRevocations() const;
  /** Access key material only while the identity-scoped writer lease is held. */
  std::optional<AbeMaterial> loadAbeMaterial() const;
  void storeAbeMaterial(const AbeMaterial& material);
  ControllerVersion startGeneration(
      uint64_t nowMs, const std::vector<RevocationTarget>& revocations = {});
  ControllerVersion advanceEpoch(
      const std::vector<RevocationTarget>& revocations = {},
      bool requireAbeRotationOnRestart = false);

private:
  struct PersistedState
  {
    ControllerVersion version;
    std::vector<RevocationTarget> revocations;
    std::optional<AbeMaterial> abeMaterial;
  };

  void requireWriter() const;
  bool acquireWriterImpl(const std::string& owner, bool requireSameOwner);
  PersistedState loadState() const;
  void persist(const ControllerVersion& version,
               const std::vector<RevocationTarget>& revocations,
               const std::optional<AbeMaterial>& abeMaterial) const;

  std::filesystem::path m_statePath;
  std::filesystem::path m_lockPath;
  std::string m_owner;
  uint64_t m_fence = 0;
  int m_lockFd = -1;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_CONTROLLER_GENERATION_STORE_HPP
