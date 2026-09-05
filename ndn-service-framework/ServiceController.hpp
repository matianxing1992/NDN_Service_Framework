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
#include "ControllerGenerationStore.hpp"
#include "PolicyStatus.hpp"

#include <boost/algorithm/string/join.hpp>

#include <filesystem>
#include <iostream>
#include <map>
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
                    ndn::ValidatorConfig& validator,
                    const std::string& configFilePath);

  void setControllerPrefix(const ndn::Name& prefix);
  void setBootstrapTokenFile(const std::string& path);
  void start();
  void run();

  /** Current persisted Controller authorization version. */
  ControllerVersion getControllerVersion() const;

  /** Build the signed-status payload for one service.  The enclosing Data
   * packet is signed by ServiceController when published. */
  PolicyStatusData getPolicyStatus(const ndn::Name& serviceName) const;

  /** Add one typed revocation and advance the Controller epoch. */
  bool revoke(const RevocationTarget& target);

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
  void onPolicyStatusInterest(const ndn::InterestFilter&, const ndn::Interest& interest);
  bool initializeControllerGeneration();
  static std::string certificateDigest(const ndn::security::Certificate& certificate);
  bool isIdentityOrCertificateRevoked(const ndn::Name& identity,
                                      const std::string& certificateDigest) const;
  void advanceAuthorizationEpoch();
  void initializeAbeGenerationIdentity();
  void rotateAbeGenerationAndReissuePolicies();
  /** Retry a failed ABE rotation (R1).  While m_abeRotationPending is set the
   * revocation stays enforced in memory and in every published status, but
   * crypto material has not been rotated for the durably advanced epoch; a
   * later revoke entry calls this before accepting another target so the
   * mixed state cannot grow.  Returns true when no rotation was pending or
   * the retry completed. */
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
  void onPolicyManifestInterest(const ndn::InterestFilter&, const ndn::Interest& interest);
  void onCertificateBootstrapInterest(const ndn::InterestFilter&, const ndn::Interest& interest);

private:
  struct BootstrapTokenEntry
  {
    std::string token;
    std::string role;
  };

  std::string m_configFilePath;

  ndn::Face& m_face;
  ndn::security::Certificate m_aaCert;

  // 用传入的 validator（引用）
  ndn::ValidatorConfig& m_validator;

  ndn::KeyChain m_keyChain;
  ndn::nacabe::KpAttributeAuthority m_aa;
  ndn::Name m_abePublicParametersName;
  std::string m_abePublicParametersDigest;

  // controller prefix selection
  ndn::Name m_controllerPrefix;
  bool m_hasCustomControllerPrefix = false;
  bool m_isRegistered = false;

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
  ControllerVersion m_controllerVersion;
  std::unique_ptr<ControllerGenerationStore> m_generationStore;
  bool m_generationReady = false;
  std::vector<RevocationTarget> m_revocations;
  // A revocation whose ABE rotation threw is still enforced in memory and in
  // every published status (fail-closed), but the crypto generation has not
  // advanced with the durable epoch.  reconcilePendingAbeRotation() retries
  // the rotation on the next revoke entry.  In-memory only: after a restart
  // the durable epoch advance is authoritative and the ABE identity is
  // re-derived from it in initializeAbeGenerationIdentity().
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
};

} // namespace ndn_service_framework
