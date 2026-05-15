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

// 你的 Attribute Authority 类型（用你工程实际类型替换这个 forward decl / include）
class AttributeAuthority
{
public:
  AttributeAuthority(const ndn::security::Certificate& aaCert,
                     ndn::Face& face,
                     ndn::ValidatorConfig& validator,
                     ndn::KeyChain& keyChain);

  void addNewPolicy(const ndn::security::Certificate& cert, const std::string& abePolicy);
  void addNewPolicy(const std::string& identity, const std::string& abePolicy);
};

// ======= 你的 TLV 常量（用你工程里的真实值替换） =======
constexpr uint32_t TLV_AllowedServiceList = 0xF501;
constexpr uint32_t TLV_AllowedService     = 0xF502;

// ======= 你已有的 hybrid 加密函数（外部已有实现） =======
// 你前面给过 encryptDataContentWithCK(payload, rsaKeyBits) 的雏形。
// 这里声明一下供本文件调用。签名按你工程实际来改。
ndn::Block
encryptDataContentWithCK(ndn::span<const uint8_t> payload,
                         ndn::span<const uint8_t> rsaKeyBits);

class ServiceController
{
public:
  ServiceController(ndn::Face& face,
                    ndn::security::Certificate aaCert,
                    ndn::ValidatorConfig& validator,
                    const std::string& configFilePath);

  void setControllerPrefix(const ndn::Name& prefix);
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

  ndn::Block makeAllowedServiceListTlv(const std::vector<std::string>& services) const;

  // ===== signer-based encryption =====
  ndn::Name getSignerCertNameFromInterest(const ndn::Interest& interest) const;
  ndn::security::Certificate getSignerCertificateFromInterest(const ndn::Interest& interest) const;

  ndn::Block encryptForCertificate(const ndn::security::Certificate& cert,
                                  const ndn::Block& plaintext) const;

  // ===== handlers =====
  void onServiceAccessInterest(const ndn::InterestFilter&, const ndn::Interest& interest);
  void onServiceProvisionInterest(const ndn::InterestFilter&, const ndn::Interest& interest);

private:
  std::string m_configFilePath;

  ndn::Face& m_face;
  ndn::security::Certificate m_aaCert;

  // 用传入的 validator（引用）
  ndn::ValidatorConfig& m_validator;

  ndn::KeyChain m_keyChain;
  AttributeAuthority m_aa;

  // controller prefix selection
  ndn::Name m_controllerPrefix;
  bool m_hasCustomControllerPrefix = false;

  // registered prefixes
  ndn::Name m_prefixServiceAccess;
  ndn::Name m_prefixServiceProvision;

  // policies loaded from config
  std::vector<ProviderPolicy> m_providerPolicies;
  std::vector<UserPolicy> m_userPolicies;

  // identity -> set(attributes)
  std::map<std::string, std::set<std::string>> m_attributesMap;

  // fast lookup tables
  std::map<std::string, std::vector<std::string>> m_userAllowedServices;     // userUri -> services
  std::map<std::string, std::vector<std::string>> m_providerAllowedServices; // providerUri -> services
};

} // namespace ndn_service_framework