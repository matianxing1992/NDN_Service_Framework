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

namespace fs = std::filesystem;

namespace ndn_service_framework {

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

private:
  // ===== lifecycle =====
  void loadConfigFiles();
  void addAttributesForUsersAccordingToServicePolicy();
  void buildLookupTables();
  void registerInterestHandlers();

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
  ndn::security::Certificate getTargetIdentityCertificate(const ndn::Name& targetIdentity) const;

  // ===== signer-based encryption =====
  bool identitiesMatch(const ndn::Name& lhs, const ndn::Name& rhs) const;
  ndn::Name getSignerCertNameFromInterest(const ndn::Interest& interest) const;
  ndn::security::Certificate getSignerCertificateFromInterest(const ndn::Interest& interest) const;
  ndn::Name getSignerIdentityFromInterest(const ndn::Interest& interest) const;

  ndn::Block encryptForCertificate(const ndn::security::Certificate& cert,
                                  const ndn::Block& plaintext) const;
  void loadBootstrapTokenFile(const std::string& path);
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
    bool consumed = false;
  };

  std::string m_configFilePath;

  ndn::Face& m_face;
  ndn::security::Certificate m_aaCert;

  // 用传入的 validator（引用）
  ndn::ValidatorConfig& m_validator;

  ndn::KeyChain m_keyChain;
  ndn::nacabe::KpAttributeAuthority m_aa;

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
  ndn::Name m_prefixCertificateBootstrap;
  size_t m_policyEpoch = 1;
  size_t m_requiredKeyEpoch = 1;
  uint64_t m_policyValidFromMs = 0;
  uint64_t m_policyGracePeriodMs = 0;

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
