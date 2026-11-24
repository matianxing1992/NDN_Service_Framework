#ifndef NDN_SERVICE_FRAMEWORK_CONTROLLER_GENERATION_STORE_HPP
#define NDN_SERVICE_FRAMEWORK_CONTROLLER_GENERATION_STORE_HPP

#include "ControllerVersion.hpp"
#include "PolicyStatus.hpp"

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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
    std::string publicParametersPacket;
    uint64_t publicParametersVersion = 0;
    std::string masterSecret;
    /** The version this material was generated for. A version may only be published once material carrying it
     *  exists, so the two can never drift apart. */
    ControllerVersion version;
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
    std::string publicParametersPacket;
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

  /** Load the seal before checking the existing state under the writer lease. */
  static std::unique_ptr<ControllerGenerationStore>
  openDefault(const std::string& identity, const std::filesystem::path& legacyStatePath,
              std::vector<uint8_t> sealKey, std::string sealKeyId,
              bool requireAuthentication = false);

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
  /** Store startup material, or atomically commit the next epoch and its ABE pair. */
  void storeAbeMaterial(const AbeMaterial& material, bool commitNextEpoch = false);
  /** Superseded generations, oldest first.
   * A legacy archive without a published name needs the verified writer identity.
   */
  std::vector<AbeHistoryEntry> loadAbeHistory() const;
  /** Authenticated public archive. The entries do not contain master secrets. */
  std::vector<AbeHistoryEntry> loadAbePublicHistory() const;
  /** Open just the requested retired master secret under the writer lease. */
  std::optional<AbeHistoryEntry> loadAbeHistoryEntry(const ControllerVersion& version) const;

  /** Seal master secrets and authenticate state. A strict reader admits legacy bytes
   * only through an independently pinned recovery import.
   */
  void
  setSecretSeal(std::vector<uint8_t> key, std::string keyId, bool requireAuthentication = false);

  /** Pin recoverable state to an independently retained private checkpoint.
   * The optional digest must come from a trusted pre-migration receipt.
   */
  void
  setRecoveryCheckpoint(std::filesystem::path path, std::string importDigest = {},
                        std::string witnessImportDigest = {});

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
  void visitGrantRecords(const std::function<void(const std::string&)>& visitor) const;

  /** Store first terminal Data wire for this exact request in the active authority version. */
  void recordHistoricalReply(const ControllerVersion& version, const ndn::Name& requestName,
                             const std::string& signedWire);
  std::optional<std::string> loadHistoricalReply(const ControllerVersion& version,
                                                 const ndn::Name& requestName) const;
  void recordHistoricalPublicationReceipt(const ndn::Name& publicationFullName,
                                          const std::string& signedWire);
  std::optional<std::string>
  loadHistoricalPublicationReceipt(const ndn::Name& publicationFullName) const;
  /** Store and load a grant-based receipt beside the CK-based one.
   *
   * Both describe the same publication under different protection schemes, so they live in the same
   * directory but under distinct derived names: a grant receipt is keyed by the publication FullName
   * with a "GRANT-RECEIPT" component appended, which keeps the CK path byte-identical. */
  void recordHistoricalGrantReceipt(const ndn::Name& publicationFullName,
                                    const std::string& signedWire);
  std::optional<std::string>
  loadHistoricalGrantReceipt(const ndn::Name& publicationFullName) const;
  void visitHistoricalPublicationReceipts(
    const std::function<void(const std::string&, const ControllerVersion&)>& visitor) const;
  struct GrantWitnessRef
  {
    std::string signerDigest;
    std::string wireDigest;
  };

  GrantWitnessRef storeGrantWitness(const std::string& signerDigest, const std::string& wire);
  std::optional<std::string> getGrantWitnessSigner() const;
  std::optional<std::string> loadGrantWitness(const std::string& signerDigest) const;

  /** Archive the material that is about to be replaced, keeping `retention` entries. */
  void recordAbeHistory(const AbeHistoryEntry& entry, size_t retention = 8);
  ControllerVersion startGeneration(
      uint64_t nowMs, const std::vector<RevocationTarget>& revocations = {},
      const std::optional<GrantWitnessRef>& witness = std::nullopt);
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
  enum class StateReadScope
  {
    ALL,
    PUBLIC_HISTORY,
    ONE_HISTORICAL_SECRET,
  };

  struct PersistedState
  {
    ControllerVersion version;
    std::vector<RevocationTarget> revocations;
    std::optional<AbeMaterial> abeMaterial;
    std::vector<AbeHistoryEntry> abeHistory;
    std::optional<GrantWitnessRef> grantWitness;
  };

  void requireWriter() const;
  bool acquireWriterImpl(const std::string& owner, bool requireSameOwner);
  PersistedState loadState(const std::string& verifiedOwner = {},
                           StateReadScope scope = StateReadScope::ALL,
                           ControllerVersion target = {}) const;
  PersistedState readState(const std::filesystem::path& path, const std::string& owner,
                           std::string_view expectedMac, StateReadScope scope,
                           const ControllerVersion& target) const;
  PersistedState decodeState(std::istream& state, std::string_view magic, unsigned int mode,
                             const std::string& verifiedOwner, StateReadScope scope,
                             const ControllerVersion& target) const;
  void persist(const ControllerVersion& version,
               const std::vector<RevocationTarget>& revocations,
               const std::optional<AbeMaterial>& abeMaterial,
               const std::vector<AbeHistoryEntry>& abeHistory,
               const std::optional<GrantWitnessRef>& grantWitness) const;

  std::filesystem::path m_statePath;
  std::filesystem::path m_checkpointPath;
  std::string m_importDigest;
  std::string m_witnessImportDigest;
  std::vector<uint8_t> m_secretKey;
  std::string m_secretKeyId;
  bool m_requireAuthentication = false;
  std::filesystem::path m_lockPath;
  std::string m_owner;
  uint64_t m_fence = 0;
  int m_lockFd = -1;
  int m_checkpointFd = -1;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_CONTROLLER_GENERATION_STORE_HPP
