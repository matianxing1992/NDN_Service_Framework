#pragma once

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/validator-config.hpp>
#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/util/time.hpp>
#include <ndn-cxx/name.hpp>
#include <ndn-cxx/interest.hpp>
#include <ndn-cxx/data.hpp>
#include <ndn-cxx/encoding/block.hpp>

#include <nac-abe/attribute-authority.hpp>

#include "NDNSFMessages.hpp"
#include "AbeHistoricalKeyRequest.hpp"
#include "ControllerGenerationStore.hpp"
#include "PolicyStatus.hpp"

#include <boost/algorithm/string/join.hpp>

#include <filesystem>
#include <atomic>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <list>
#include <algorithm>
#include <utility>
#include <memory>

namespace fs = std::filesystem;

namespace ndn_service_framework {

// Test-only access shim.  The production Controller keeps permission
// response construction private; the integration suite uses this friend to
// assert revocation filtering without bypassing the real policy tables.
struct ServiceControllerTestAccess;

/** Restore a Controller-owned KP-ABE generation before it serves requests. */
class PersistentKpAttributeAuthority : public ndn::nacabe::KpAttributeAuthority
{
public:
  using KpAttributeAuthority::KpAttributeAuthority;

  std::string getMasterSecret() const;
  void restoreKeyGeneration(const std::string& publicParameters, const std::string& masterSecret);
  std::optional<ndn::security::Certificate> findRecipient(const ndn::Name& keyName) const;
  ndn::nacabe::algo::PrivateKey issuePrivateKey(const std::set<std::string>& attributes);
};

// ======= 你工程里应该已有这些：PolicyParser / Policy types / AA =======
// 这里保持“只声明，不定义”，避免跟你工程冲突。
// 你可以删掉这段 forward decl，改为 include 你自己的头文件。

struct ProviderPolicy
{
  std::string providerName;                 // identity URI
  std::vector<std::string> allowedServices; // service name URIs or name strings
};

struct UserPolicy
{
  std::string userName;                     // identity URI
  std::vector<std::string> allowedServices; // service name URIs or name strings
};

class PolicyParser
{
public:
  // returns {providerPolicies, userPolicies}
  std::pair<std::vector<ProviderPolicy>, std::vector<UserPolicy>>
  parsePolicyFile(const std::string& path);
};

class ServiceController
{
public:
  ServiceController(ndn::Face& face,
                    ndn::security::Certificate aaCert,
                    ndn::security::Validator& validator,
                    const std::string& configFilePath);

  void setControllerPrefix(const ndn::Name& prefix);
  void setBootstrapTokenFile(const std::string& path);
  void cancelStart() noexcept;
  void resetStartCancellation() noexcept;
  // Owns the Face event loop until registrations and an independently received,
  // authenticated PUBPARAMS round trip complete (at most ten seconds). The
  // probe uses ndn-cxx's configured local transport; a caller-supplied Face
  // must reach that same local NFD. Cancellation or a stopped loop fails closed.
  void start();
  void run();

  /** Current persisted Controller authorization version. */
  ControllerVersion getControllerVersion() const;

  /** Build the signed-status payload for one service.  The enclosing Data
   * packet is signed by ServiceController when published. */
  PolicyStatusData getPolicyStatus(const ndn::Name& serviceName) const;

  /** Add a typed revocation, or finish its pending failed rotation on retry.
   * An already completed duplicate remains a no-op returning false. */
  bool revoke(const RevocationTarget& target);

  /** Serve the public parameters of every archived generation under the name they were
   * published with, so a historical fetch resolves without touching the live authority. */
  void registerArchivedPublicParameters();
  void onArchivedPublicParametersInterest(const ndn::Name& indexName,
                                          const ndn::Interest& interest);
  bool serveHistoricalTerminal(const ndn::Name& name, const ControllerVersion& version,
                               bool allowSuccessfulReply);
  void publishHistoricalTerminal(const ndn::Data& data, const ControllerVersion& version);
  void onHistoricalPublicationInterest(const ndn::Interest& interest);
  /** Issue or replay a grant-based receipt for a publication protected by an Authority-signed grant.
   *
   * Same RPC family as the CK receipt but a distinct operation name, a distinct request body and a
   * distinct stored record, so the two schemes never share state. */
  /** Issue or replay a grant-based receipt for a grant-protected publication. Reached from the
   *  PUBLISH-HISTORY handler, whose body names its own schema. */
  /** Serve a stored historical receipt by its own FullName (the one an acknowledgement returned). */
  void serveHistoricalReceipt(const ndn::Interest& interest);
  void issueHistoricalGrantReceipt(const ndn::Interest& validated,
                                   const ndn::security::Certificate& producerCertificate,
                                   const HistoricalGrantReceiptRequest& request,
                                   const ControllerVersion& version);
  bool acceptsHistoricalPublication(const ndn::Data& publication) const;
  bool hasHistoricalGrant(const ndn::Name& identity, const ControllerVersion& version,
                          const std::string& policy) const;
  void recordUserGrant(const PermissionResponse& response);
  ndn::security::Certificate getValidatedHistoricalSigner(const ndn::Interest& interest) const;

  /** Answer a historical key request, minting from the archived generation it names. */
  void serveHistoricalKeyRequest(const ndn::Interest& interest);

  /**
   * Grant one exact service authorization.  Grant-only mutations advance the
   * ControllerVersion but keep the current global ABE generation; the
   * Controller replaces only the target identity's complete policy; NAC-ABE
   * generates the replacement DKEY lazily on that identity's next fetch.
   */
  bool grant(const ndn::Name& identity,
             const ndn::Name& serviceName,
             const ndn::Name& authorizationAttribute);

  /** Testable policy decision used by permission issuance.  An empty
   * authorizationAttribute is a query wildcard for legacy diagnostics; all
   * runtime authorization paths pass the exact canonical attribute. */
  bool isRevoked(const ndn::Name& identity,
                 const ndn::Name& serviceName,
                 const std::string& certificateDigest = {},
                 const ndn::Name& authorizationAttribute = {}) const;

private:
  friend struct ServiceControllerTestAccess;
  // ===== lifecycle =====
  void loadConfigFiles();
  void addAttributesForUsersAccordingToServicePolicy();
  void buildLookupTables();
  void registerInterestHandlers();
  void onCurrentAbeParametersInterest(const ndn::Interest& interest);
  void onCurrentAbeKeyInterest(const ndn::Interest& interest);
  bool canServeCurrentAbe() const;
  void onPolicyStatusInterest(const ndn::InterestFilter&, const ndn::Interest& interest);
  bool initializeControllerGeneration();
  bool hasGenerationWriter() const;
  bool acceptsHistoricalGrantWire(const std::string& wire) const;
  static std::string certificateDigest(const ndn::security::Certificate& certificate);
  bool isIdentityOrCertificateRevoked(const ndn::Name& identity,
                                      const std::string& certificateDigest) const;
  void advanceAuthorizationEpoch(bool requireAbeRotationOnRestart = false);
  void restoreOrPersistAbeMaterial();
  void persistAbeMaterial(bool commitNextEpoch = false);
  void persistAbePublicParametersPacket();
  void recordInitialAbeGrants();
  void initializeAbeGenerationIdentity(const ControllerGenerationStore::AbeMaterial& material);
  uint64_t nextAbePublicParametersVersion() const;
  void rotateAbeGenerationAndReissuePolicies(bool commitNextEpoch = false);
  /** Complete the durable pending revocation and atomically commit its ABE epoch. */
  bool reconcilePendingAbeRotation();
  void reissueAbePolicies();
  std::set<std::string> effectiveAttributesFor(const std::string& identity) const;
  bool isAbeAttributeRevoked(const std::string& identity,
                             const std::string& attribute) const;
  static std::string abePublicParametersDigest(const ndn::Buffer& wire);
  ndn::Name currentAbePublicParametersName() const;

  // ===== helpers =====
  static std::vector<std::string> uniqSorted(std::vector<std::string> v);

  void addAttribute(const std::string& identity, const std::string& attributeName);

  bool extractEntityAfterPrefix(const ndn::Name& interestName,
                                const ndn::Name& prefix,
                                ndn::Name& entityOut);
  bool parseUserPermissionsInterestName(const ndn::Name& interestName,
                                        ndn::Name& targetIdentity) const;
  bool parseProviderPermissionsInterestName(const ndn::Name& interestName,
                                            ndn::Name& targetIdentity) const;

  ndn::Block makeAllowedServiceListTlv(const std::vector<std::string>& services) const;
  PermissionResponse buildUserPermissionResponse(const ndn::Name& targetIdentity) const;
  PermissionResponse buildProviderPermissionResponse(const ndn::Name& targetIdentity) const;
  PolicyManifest buildPolicyManifest() const;
  ndn::security::Certificate getTargetIdentityCertificate(const ndn::Name& targetIdentity);

  // ===== signer-based encryption =====
  bool identitiesMatch(const ndn::Name& lhs, const ndn::Name& rhs) const;
  ndn::Name getSignerCertNameFromInterest(const ndn::Interest& interest) const;
  ndn::security::Certificate getSignerCertificateFromInterest(const ndn::Interest& interest) const;
  ndn::Name getSignerIdentityFromInterest(const ndn::Interest& interest) const;

  ndn::Block encryptForCertificate(const ndn::security::Certificate& cert,
                                  const ndn::Block& plaintext) const;
  void loadBootstrapTokenFile(const std::string& path);
  void generateBootstrapTokenFile(const std::string& path) const;
  void sendCertificateBootstrapResponse(const ndn::Interest& interest,
                                        bool status,
                                        const std::string& message,
                                        const ndn::security::Certificate* issuedCertificate);

  // ===== handlers =====
  void onServiceAccessInterest(const ndn::InterestFilter&, const ndn::Interest& interest);
  void onServiceProvisionInterest(const ndn::InterestFilter&, const ndn::Interest& interest);
  void onUserPermissionsInterest(const ndn::InterestFilter&, const ndn::Interest& interest);
  void onProviderPermissionsInterest(const ndn::InterestFilter&, const ndn::Interest& interest);
  void onPermissionInterest(const ndn::Interest& interest, bool user);
  void resolveProtectedRecipient(
      const ndn::Name& requestName, const ndn::Name& subject, const ndn::Name& recipientFullName,
      const std::function<void(const ndn::security::Certificate&)>& issue);
  void onPolicyManifestInterest(const ndn::InterestFilter&, const ndn::Interest& interest);
  void onCertificateBootstrapInterest(const ndn::InterestFilter&, const ndn::Interest& interest);

private:
  struct RegistrationState
  {
    size_t pending = 0;
    std::string error;
  };

  struct BootstrapTokenEntry
  {
    std::string token;
    std::string role;
  };

  std::string m_configFilePath;

  struct OriginalControlReply {
    std::shared_ptr<const ndn::Data> packet;
    uint64_t expiresAtMs = 0;
  };
  // Face-owned immutable reply packets. Expired nonces cannot regenerate them.
  std::map<ndn::Name, OriginalControlReply> m_originalControlReplies;
  std::size_t m_originalControlReplyBytes = 0;
  bool admitControlQuery(const ndn::Interest& interest, const std::string& operation);
  bool serveOriginalControlReply(const ndn::Interest& interest);
  void publishOriginalControlReply(const ndn::Interest& interest, const ndn::Data& data);
  ndn::Face& m_face;
  ndn::security::Certificate m_aaCert;
  /** Signing certificate this controller took over from, when a committed handoff named one. A stored
   *  publication receipt signed by the predecessor must still verify, so it is retained. */
  std::optional<ndn::security::Certificate> m_predecessorCertificate;

  // 用传入的 validator（引用）
  ndn::security::Validator& m_validator;

  ndn::KeyChain m_keyChain;
  // The SDK registers unconditional callbacks at construction. Keep its
  // registration on a private, unrun executor; only m_face serves authority.
  boost::asio::io_context m_abeSdkContext;
  ndn::Face m_abeSdkFace;
  PersistentKpAttributeAuthority m_aa;
  ndn::Name m_abePublicParametersName;
  std::string m_abePublicParametersDigest;

  // controller prefix selection
  ndn::Name m_controllerPrefix;
  bool m_hasCustomControllerPrefix = false;
  bool m_isRegistered = false;
  std::atomic_bool m_startCancelled{false};
  // Callbacks share this state rather than borrowing the start() stack.
  std::shared_ptr<RegistrationState> m_registrationState;
  // Keep the asynchronous prefix registrations alive.  RegisteredPrefixHandle
  // unregisters on destruction; discarding the returned handles can cancel
  // the command before NFD has completed registration.
  std::vector<std::shared_ptr<ndn::ScopedRegisteredPrefixHandle>> m_registrationHandles;

  // registered prefixes
  ndn::Name m_prefixServiceAccess;
  ndn::Name m_prefixServiceProvision;
  ndn::Name m_prefixUserPermissions;
  ndn::Name m_prefixProviderPermissions;
  ndn::Name m_prefixPolicyManifest;
  ndn::Name m_prefixPolicyStatus;
  ndn::Name m_prefixCertificateBootstrap;
  size_t m_policyEpoch = 1;
  size_t m_requiredKeyEpoch = 1;
  uint64_t m_policyValidFromMs = 0;
  uint64_t m_policyGracePeriodMs = 0;
  uint64_t m_policyValidUntilMs = 0;
  uint64_t m_policyStatusLeaseMs = 60000;
  uint64_t m_lastPolicyLeaseIssuedAtMs = 0;
  ControllerVersion m_controllerVersion;
  /** Version of the ABE material currently installed in m_aa.  A rotation archives the
   * material named here, not the version being advanced to. */
  ControllerVersion m_abeMaterialVersion;
  std::unique_ptr<ControllerGenerationStore> m_generationStore;
  /** Responders for superseded generations' public parameters, kept alive with the Controller. */
  /** Register a prefix for this Controller's served data with a forwarder registration, so it is reachable from
   *  other nodes and not only through this process's own dispatcher. */
  void registerServedPrefix(const ndn::Name& prefix,
                            const std::function<void(const ndn::InterestFilter&,
                                                     const ndn::Interest&)>& onInterest);
  std::set<std::string> m_registeredServedPrefixes;
  /** One entry per historical key request seen, kept until the request would expire.
   *
   * A Signed Interest that is replayed must not mint a second key, so the digest that names the
   * request is remembered for exactly as long as that request stays valid. */
  std::unique_ptr<HistoricalKeyBroker> m_historicalKeyBroker;
  std::set<std::string> m_retiredGrantDigests;
  std::vector<ndn::ScopedInterestFilterHandle> m_historicalKeyFilters;
  bool m_generationReady = false;
  std::vector<RevocationTarget> m_revocations;
  // A revocation whose ABE rotation threw is still enforced in memory and in
  // every published status (fail-closed), but the crypto generation has not
  // advanced with the durable epoch.  reconcilePendingAbeRotation() retries
  // the rotation on the next revoke or grant entry. In-memory only: after a restart
  // startup restores the committed ABE identity before reconciling rotation.
  bool m_abeRotationPending = false;

  // policies loaded from config
  std::vector<ProviderPolicy> m_providerPolicies;
  std::vector<UserPolicy> m_userPolicies;

  // identity -> set(attributes)
  std::map<std::string, std::set<std::string>> m_attributesMap;

  // fast lookup tables
  std::map<std::string, std::vector<std::string>> m_userAllowedServices;     // userUri -> services
  std::map<std::string, std::vector<std::string>> m_providerAllowedServices; // providerUri -> services
  std::map<std::string, BootstrapTokenEntry> m_bootstrapTokens;
  std::map<std::string, ndn::security::Certificate> m_bootstrapIssuedCertificates;

  struct CurrentAbeKey
  {
    ControllerVersion version;
    ndn::security::Certificate recipient;
    std::vector<std::shared_ptr<ndn::Data>> segments;
    uint64_t expiresAtMs;
  };
  std::map<ndn::Name, CurrentAbeKey> m_currentAbeKeys;
  std::shared_ptr<const ndn::Data> m_currentAbeParameters;
  std::map<ndn::Name, std::shared_ptr<const ndn::Data>> m_archivedAbeParameters;
  std::map<ndn::Name, std::shared_ptr<const ndn::Data>> m_archivedAbeProofs;
  std::shared_ptr<const ndn::Data> m_archivedAbeIndex;
  std::map<ndn::Name, ndn::ScopedPendingInterestHandle> m_currentAbeCertificateFetches;
  std::map<ndn::Name, ndn::ScopedPendingInterestHandle> m_permissionCertificateFetches;
  std::shared_ptr<int> m_currentAbeLifetime = std::make_shared<int>(0);
};

/** Authorize an exact successor certificate for the current persisted ABE
 * material version. Install the returned Data wire with NDNSF_CONTROLLER_HANDOFF.
 */
ndn::Data
makeControllerSignerHandoff(const ndn::security::Certificate& predecessor,
                            const ndn::security::Certificate& successor,
                            const ControllerVersion& materialVersion, ndn::KeyChain& keyChain);

} // namespace ndn_service_framework
