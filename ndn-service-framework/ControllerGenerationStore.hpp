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

  /** One superseded ABE generation, retained so historical Data stays readable.
   *
   * Entries are duplicated here rather than referenced, because the current
   * material is overwritten on rotation and the archive must survive that.
   */
  struct AbeHistoryEntry
  {
    uint64_t generationTimestamp = 0;
    uint64_t epoch = 0;
    std::string publicParameters;
    std::string masterSecret;
    /** The published name this generation's parameters were served under, and the digest of
     * their wire form. Without them an archive entry cannot be matched to the Data that was
     * protected with it. */
    std::string publicParametersName;
    std::string publicParametersWireDigest;
    std::string digest;
    std::string reason;

    AbeHistoryEntry() = default;
    AbeHistoryEntry(const AbeHistoryEntry&) = default;
    AbeHistoryEntry& operator=(const AbeHistoryEntry& other);
    ~AbeHistoryEntry();
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
  /** Superseded generations, oldest first. Readable without the writer lease. */
  std::vector<AbeHistoryEntry> loadAbeHistory() const;

  /** Seal master secrets at rest with this key. Without it they stay readable as before. */
  void
  setSecretSeal(std::vector<uint8_t> key, std::string keyId);

  bool
  sealedSecrets() const noexcept
  {
    return !m_secretKey.empty();
  }

  /** Record which attributes each identity held while a generation was current.
   *
   * A historical read has to be judged against what the object's generation actually allowed, so the
   * answer is captured at rotation instead of reconstructed later. Kept beside the state file: it is
   * authorization evidence, not key material, and it must not change the key-bearing layout.
   */
  void
  recordAttributeSnapshot(uint64_t generation, uint64_t epoch,
                          const std::map<std::string, std::set<std::string>>& attributes,
                          size_t retention = 8);

  /** The recorded snapshot for one generation, or nothing when it predates recording. */
  std::optional<std::map<std::string, std::set<std::string>>>
  loadAttributeSnapshot(uint64_t generation, uint64_t epoch) const;

  /** Where the attribute snapshots live: beside the state file, never inside it. */
  std::filesystem::path
  attributeSnapshotPath() const
  {
    return std::filesystem::path(m_statePath).concat(".attributes");
  }
  /** Directory holding one signed grant record per identity and version. */
  std::filesystem::path grantRecordDirectory() const
  {
    return std::filesystem::path(m_statePath).concat(".grants");
  }

  /** Persist the signed record of what one identity was granted at one version.
   *
   * A historical decision needs what the identity held when the object was published. The retirement-time
   * attribute snapshot cannot prove that, and the Controller does not keep the signed permission responses
   * it issued (they are encrypted to the identity anyway). This record is that evidence: the Controller's
   * own signature over the identity, the version and the attributes it granted.
   */
  void recordGrant(const std::string& identity, uint64_t generation, uint64_t epoch,
                   const std::string& signedContent);

  /** Read back the record for one identity at exactly one version, if it exists. */
  std::optional<std::string> loadGrant(const std::string& identity, uint64_t generation,
                                       uint64_t epoch) const;

  /** Archive the material that is about to be replaced, keeping `retention` entries. */
  void recordAbeHistory(const AbeHistoryEntry& entry, size_t retention = 8);
  ControllerVersion startGeneration(
      uint64_t nowMs, const std::vector<RevocationTarget>& revocations = {});
  ControllerVersion advanceEpoch(
      const std::vector<RevocationTarget>& revocations = {},
      bool requireAbeRotationOnRestart = false);

  /** Record a withdrawal **without** claiming a new version.
   *
   * The version names key material, so it may only advance once that material exists. A revocation that still needs
   * its rotation is therefore persisted against the current version with the rotation-required flag, and `advanceEpoch`
   * publishes the new version afterwards. Returns the version that is still being served.
   */
  ControllerVersion recordRevocations(
      const std::vector<RevocationTarget>& revocations,
      bool requireAbeRotationOnRestart = true);

private:
  struct PersistedState
  {
    ControllerVersion version;
    std::vector<RevocationTarget> revocations;
    std::optional<AbeMaterial> abeMaterial;
    std::vector<AbeHistoryEntry> abeHistory;
  };

  void requireWriter() const;
  bool acquireWriterImpl(const std::string& owner, bool requireSameOwner);
  PersistedState loadState() const;
  void persist(const ControllerVersion& version,
               const std::vector<RevocationTarget>& revocations,
               const std::optional<AbeMaterial>& abeMaterial,
               const std::vector<AbeHistoryEntry>& abeHistory) const;

  std::filesystem::path m_statePath;
  std::vector<uint8_t> m_secretKey;
  std::string m_secretKeyId;
  std::filesystem::path m_lockPath;
  std::string m_owner;
  uint64_t m_fence = 0;
  int m_lockFd = -1;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_CONTROLLER_GENERATION_STORE_HPP
