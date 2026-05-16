#include "ServiceController.hpp"
#include "utils.hpp"

#include <nac-abe/ndn-crypto/data-enc-dec.hpp>

namespace ndn_service_framework {

ServiceController::ServiceController(ndn::Face& face,
                                     ndn::security::Certificate aaCert,
                                     ndn::ValidatorConfig& validator,
                                     const std::string& configFilePath)
  : m_configFilePath(configFilePath)
  , m_face(face)
  , m_aaCert(std::move(aaCert))
  , m_validator(validator) // 关键：用传入的引用
  , m_keyChain()
  , m_aa(m_aaCert, m_face, m_validator, m_keyChain)
{
  // Load provider/user policies from config
  loadConfigFiles();

  // Convert policies into ABE attribute policies and register them in AA
  addAttributesForUsersAccordingToServicePolicy();

  // Build fast lookup tables for Interest answering
  buildLookupTables();

  // Register Interest filters for both SERVICEACCESS and SERVICEPROVISION
  registerInterestHandlers();
}

void ServiceController::setControllerPrefix(const ndn::Name& prefix)
{
  m_controllerPrefix = prefix;
  m_hasCustomControllerPrefix = true;
}

void ServiceController::run()
{
  m_face.processEvents();
}

void ServiceController::loadConfigFiles()
{
  if (fs::is_regular_file(m_configFilePath)) {
    std::cout << "Loading config file: " << m_configFilePath << std::endl;

    PolicyParser parser;
    auto policies = parser.parsePolicyFile(m_configFilePath);

    m_providerPolicies.insert(m_providerPolicies.end(),
                              policies.first.begin(), policies.first.end());
    m_userPolicies.insert(m_userPolicies.end(),
                          policies.second.begin(), policies.second.end());
  }
  else {
    std::cerr << "Error: " << m_configFilePath << " is not a valid file." << std::endl;
  }
}

void ServiceController::addAttribute(const std::string& identity, const std::string& attributeName)
{
  m_attributesMap[identity].emplace(attributeName);
}

void ServiceController::addAttributesForUsersAccordingToServicePolicy()
{
  // Provider policies -> SERVICE/<service-name> attributes
  for (const auto& policy : m_providerPolicies) {
    for (const auto& service : policy.allowedServices) {
      addAttribute(policy.providerName, "/SERVICE" + service);
    }
  }

  // User policies -> PERMISSION/<service-name> attributes
  for (const auto& policy : m_userPolicies) {
    ndn::Name userName(policy.userName);
    for (const auto& service : policy.allowedServices) {
      addAttribute(userName.toUri(), "/PERMISSION" + service);
    }
  }

  // Register each identity's OR-policy into the Attribute Authority
  for (const auto& item : m_attributesMap) {
    const std::string& identity = item.first;
    const std::set<std::string>& attrs = item.second;

    std::list<std::string> attributeList(attrs.begin(), attrs.end());
    std::string abePolicy = boost::algorithm::join(attributeList, " OR ");

    try {
      auto cert = m_keyChain.getPib()
                    .getIdentity(identity)
                    .getDefaultKey()
                    .getDefaultCertificate();
      m_aa.addNewPolicy(cert, abePolicy);
      std::cout << "Add ABE policy: " << abePolicy
                << " for identity (cert): " << identity << std::endl;
    }
    catch (const std::exception&) {
      m_aa.addNewPolicy(ndn::Name(identity), abePolicy);
      std::cout << "Add ABE policy (fallback): " << abePolicy
                << " for identity: " << identity << std::endl;
    }
  }
}

std::vector<std::string> ServiceController::uniqSorted(std::vector<std::string> v)
{
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

void ServiceController::buildLookupTables()
{
  m_userAllowedServices.clear();
  m_providerAllowedServices.clear();

  for (const auto& p : m_userPolicies) {
    auto& vec = m_userAllowedServices[p.userName];
    vec.insert(vec.end(), p.allowedServices.begin(), p.allowedServices.end());
  }

  for (const auto& p : m_providerPolicies) {
    auto& vec = m_providerAllowedServices[p.providerName];
    vec.insert(vec.end(), p.allowedServices.begin(), p.allowedServices.end());
  }

  for (auto& it : m_userAllowedServices) {
    it.second = uniqSorted(std::move(it.second));
  }
  for (auto& it : m_providerAllowedServices) {
    it.second = uniqSorted(std::move(it.second));
  }
}

bool ServiceController::extractEntityAfterPrefix(const ndn::Name& interestName,
                                                const ndn::Name& prefix,
                                                ndn::Name& entityOut)
{
  if (!prefix.isPrefixOf(interestName)) {
    return false;
  }
  if (interestName.size() <= prefix.size()) {
    return false;
  }
  entityOut = interestName.getSubName(prefix.size());
  return true;
}

bool ServiceController::parseUserPermissionsInterestName(const ndn::Name& interestName,
                                                         ndn::Name& targetIdentity) const
{
  if (!m_prefixUserPermissions.isPrefixOf(interestName) ||
      interestName.size() <= m_prefixUserPermissions.size()) {
    return false;
  }

  targetIdentity = interestName.getSubName(m_prefixUserPermissions.size());
  return true;
}

bool ServiceController::parseProviderPermissionsInterestName(const ndn::Name& interestName,
                                                             ndn::Name& targetIdentity) const
{
  if (!m_prefixProviderPermissions.isPrefixOf(interestName) ||
      interestName.size() <= m_prefixProviderPermissions.size()) {
    return false;
  }

  targetIdentity = interestName.getSubName(m_prefixProviderPermissions.size());
  return true;
}

void ServiceController::registerInterestHandlers()
{
  if (!m_hasCustomControllerPrefix) {
    m_controllerPrefix = m_aaCert.getIdentity();
  }

  m_prefixServiceAccess = m_controllerPrefix;
  m_prefixServiceAccess.append("NDNSF").append("SERVICEACCESS");

  m_prefixServiceProvision = m_controllerPrefix;
  m_prefixServiceProvision.append("NDNSF").append("SERVICEPROVISION");

  m_prefixUserPermissions = m_controllerPrefix;
  m_prefixUserPermissions.append("NDNSF").append("PERMISSIONS").append("USER");

  m_prefixProviderPermissions = m_controllerPrefix;
  m_prefixProviderPermissions.append("NDNSF").append("PERMISSIONS").append("PROVIDER");

  m_face.setInterestFilter(
    m_prefixServiceAccess,
    [this](const ndn::InterestFilter& f, const ndn::Interest& i) {
      this->onServiceAccessInterest(f, i);
    },
    ndn::RegisterPrefixSuccessCallback(),
    [](const ndn::Name& p, const std::string& reason) {
      std::cerr << "Failed to register prefix " << p << " reason=" << reason << std::endl;
    });

  m_face.setInterestFilter(
    m_prefixServiceProvision,
    [this](const ndn::InterestFilter& f, const ndn::Interest& i) {
      this->onServiceProvisionInterest(f, i);
    },
    ndn::RegisterPrefixSuccessCallback(),
    [](const ndn::Name& p, const std::string& reason) {
      std::cerr << "Failed to register prefix " << p << " reason=" << reason << std::endl;
    });

  m_face.setInterestFilter(
    m_prefixUserPermissions,
    [this](const ndn::InterestFilter& f, const ndn::Interest& i) {
      this->onUserPermissionsInterest(f, i);
    },
    ndn::RegisterPrefixSuccessCallback(),
    [](const ndn::Name& p, const std::string& reason) {
      std::cerr << "Failed to register prefix " << p << " reason=" << reason << std::endl;
    });

  m_face.setInterestFilter(
    m_prefixProviderPermissions,
    [this](const ndn::InterestFilter& f, const ndn::Interest& i) {
      this->onProviderPermissionsInterest(f, i);
    },
    ndn::RegisterPrefixSuccessCallback(),
    [](const ndn::Name& p, const std::string& reason) {
      std::cerr << "Failed to register prefix " << p << " reason=" << reason << std::endl;
    });

  std::cout << "ServiceController listening on:\n"
            << "  " << m_prefixServiceAccess << "\n"
            << "  " << m_prefixServiceProvision << "\n"
            << "  " << m_prefixUserPermissions << "\n"
            << "  " << m_prefixProviderPermissions << std::endl;
}

ndn::Block ServiceController::makeAllowedServiceListTlv(const std::vector<std::string>& services) const
{
  ndn::Block list(TLV_AllowedServiceList);

  for (const auto& s : services) {
    ndn::Name svcName(s);
    ndn::Block nameBlock = svcName.wireEncode();

    ndn::Block item(TLV_AllowedService);
    item.push_back(nameBlock);
    item.encode();

    list.push_back(item);
  }

  list.encode();
  return list;
}

PermissionResponse
ServiceController::buildUserPermissionResponse(const ndn::Name& targetIdentity) const
{
  PermissionResponse response;
  response.setTargetIdentity(targetIdentity.toUri());
  response.setPermissionKind(tlv::UserPermission);

  std::vector<std::string> allowedServices;
  if (auto it = m_userAllowedServices.find(targetIdentity.toUri());
      it != m_userAllowedServices.end()) {
    allowedServices = it->second;
  }

  for (const auto& service : allowedServices) {
    for (const auto& provider : m_providerAllowedServices) {
      const auto& providerName = provider.first;
      const auto& providerServices = provider.second;
      if (std::find(providerServices.begin(), providerServices.end(), service) ==
          providerServices.end()) {
        continue;
      }

      PermissionEntry entry;
      entry.setProviderName(providerName);
      entry.setServiceName(service);
      entry.setToken("permission-token:" + targetIdentity.toUri() + ":" +
                     providerName + ":" + service);
      entry.setTtl(0);
      entry.setVersion(1);
      response.addEntry(entry);
    }
  }

  return response;
}

PermissionResponse
ServiceController::buildProviderPermissionResponse(const ndn::Name& targetIdentity) const
{
  PermissionResponse response;
  response.setTargetIdentity(targetIdentity.toUri());
  response.setPermissionKind(tlv::ProviderPermission);

  std::vector<std::string> allowedServices;
  if (auto it = m_providerAllowedServices.find(targetIdentity.toUri());
      it != m_providerAllowedServices.end()) {
    allowedServices = it->second;
  }

  for (const auto& service : allowedServices) {
    PermissionEntry entry;
    entry.setProviderName(targetIdentity.toUri());
    entry.setServiceName(service);
    entry.setToken("permission-token:" + targetIdentity.toUri() + ":" + service);
    entry.setTtl(0);
    entry.setVersion(1);
    response.addEntry(entry);
  }

  return response;
}

ndn::security::Certificate
ServiceController::getTargetIdentityCertificate(const ndn::Name& targetIdentity) const
{
  auto cert = m_keyChain.getPib()
                .getIdentity(targetIdentity)
                .getDefaultKey()
                .getDefaultCertificate();
  if (!cert.isValid()) {
    throw std::runtime_error("Target identity certificate is not currently valid: " +
                             cert.getName().toUri());
  }
  return cert;
}

// ===================== Signer cert extraction =====================

ndn::Name ServiceController::getSignerCertNameFromInterest(const ndn::Interest& interest) const
{
  // 常见 ndn-cxx API：interest.getSignatureInfo()
  // 如果你版本不同，改成：interest.getSignature().getInfo() 等等。
  const auto& sigInfo = interest.getSignatureInfo();

 

  if (! sigInfo->hasKeyLocator()) {
    throw std::runtime_error("Interest SignatureInfo has no KeyLocator");
  }

  const auto& kl = sigInfo->getKeyLocator();

  // 常见：KeyLocator type = Name
  if (kl.getType()!=ndn::Name::size_type()) {
    throw std::runtime_error("Interest KeyLocator is not a Name");
  }

  return kl.getName(); // 通常就是证书名
}

ndn::security::Certificate
ServiceController::getSignerCertificateFromInterest(const ndn::Interest& interest) const
{
  const ndn::Name certName = getSignerCertNameFromInterest(interest);

  // 优先从本机 PIB 取（如果证书已在本机）
  try {
    // 很多 ndn-cxx 版本有这个：
    // return m_keyChain.getPib().getCertificate(certName);
    auto cert = m_keyChain.getPib().getIdentity(certName).getDefaultKey().getDefaultCertificate();
    return cert;
  }
  catch (const std::exception&) {
    // 兜底：如果你的 PIB 不支持按 certName 直接取，
    // 你可以按命名规则拆 identity/key/cert 再取，或做网络 fetch（这需要你工程里已有证书拉取逻辑）。
    throw std::runtime_error("Cannot find signer certificate in PIB: " + certName.toUri());
  }
}

// ===================== Encryption =====================

ndn::Block
ServiceController::encryptForCertificate(const ndn::security::Certificate& cert,
                                        const ndn::Block& plaintext) const
{
  // 取明文 bytes
  ndn::Block pt = plaintext;
  pt.encode(); // 确保 wire 完整

  // 取证书公钥 bits（API 可能因版本略不同）
  const auto pkBits = cert.getPublicKey();

  // 用你已有的 hybrid 加密
  return ndn::nacabe::encryptDataContentWithCK(ndn::make_span(pt.data(), pt.size()), pkBits);
}

// ===================== Handlers =====================

void ServiceController::onServiceAccessInterest(const ndn::InterestFilter&,
                                               const ndn::Interest& interest)
{
  // <controller>/NDNSF/SERVICEACCESS/<user>
  ndn::Name userName;
  if (!extractEntityAfterPrefix(interest.getName(), m_prefixServiceAccess, userName)) {
    return;
  }

  // 先验证 Interest（必须！否则 KeyLocator 可被伪造导致加密给错误的人）
  m_validator.validate(
    interest,
    // validated
    [this, userName](const ndn::Interest& validatedInterest) {
      // 取 signer cert（用 signer 公钥加密）
      ndn::security::Certificate signerCert = getSignerCertificateFromInterest(validatedInterest);

      const std::string userUri = userName.toUri();

      // 查权限（你现在按 name 里的 userUri 查）
      std::vector<std::string> services;
      if (auto it = m_userAllowedServices.find(userUri); it != m_userAllowedServices.end()) {
        services = it->second;
      }

      // Data name = prefix + user + timestamp
      ndn::Name dataName = m_prefixServiceAccess;
      dataName.append(userName);
      dataName.appendTimestamp(ndn::time::system_clock::now());

      ndn::Data data(dataName);

      ndn::Block plaintext = makeAllowedServiceListTlv(services);
      ndn::Block content   = encryptForCertificate(signerCert, plaintext);

      data.setContent(content);
      data.setFreshnessPeriod(ndn::time::seconds(2));
      m_keyChain.sign(data);
      m_face.put(data);

      std::cout << "[SERVICEACCESS] Reply to " << userUri
                << " services=" << services.size()
                << " data=" << data.getName()
                << " encryptedFor=" << signerCert.getName()
                << std::endl;
    },
    // failed
    [this](const ndn::Interest& badInterest, const ndn::security::ValidationError& err) {
      std::cerr << "[SERVICEACCESS] Interest validation failed: "
                << err << " name=" << badInterest.getName() << std::endl;
    });
}

void ServiceController::onServiceProvisionInterest(const ndn::InterestFilter&,
                                                  const ndn::Interest& interest)
{
  // <controller>/NDNSF/SERVICEPROVISION/<provider>
  ndn::Name providerName;
  if (!extractEntityAfterPrefix(interest.getName(), m_prefixServiceProvision, providerName)) {
    return;
  }

  m_validator.validate(
    interest,
    // validated
    [this, providerName](const ndn::Interest& validatedInterest) {
      ndn::security::Certificate signerCert = getSignerCertificateFromInterest(validatedInterest);

      const std::string providerUri = providerName.toUri();

      std::vector<std::string> services;
      if (auto it = m_providerAllowedServices.find(providerUri); it != m_providerAllowedServices.end()) {
        services = it->second;
      }

      ndn::Name dataName = m_prefixServiceProvision;
      dataName.append(providerName);
      dataName.appendTimestamp(ndn::time::system_clock::now());

      ndn::Data data(dataName);

      ndn::Block plaintext = makeAllowedServiceListTlv(services);
      ndn::Block content   = encryptForCertificate(signerCert, plaintext);

      data.setContent(content);
      data.setFreshnessPeriod(ndn::time::seconds(2));
      m_keyChain.sign(data);
      m_face.put(data);

      std::cout << "[SERVICEPROVISION] Reply to " << providerUri
                << " services=" << services.size()
                << " data=" << data.getName()
                << " encryptedFor=" << signerCert.getName()
                << std::endl;
    },
    // failed
    [this](const ndn::Interest& badInterest, const ndn::security::ValidationError& err) {
      std::cerr << "[SERVICEPROVISION] Interest validation failed: "
                << err << " name=" << badInterest.getName() << std::endl;
    });
}

void ServiceController::onUserPermissionsInterest(const ndn::InterestFilter&,
                                                  const ndn::Interest& interest)
{
  ndn::Name targetIdentity;
  if (!parseUserPermissionsInterestName(interest.getName(), targetIdentity)) {
    return;
  }

  PermissionResponse response = buildUserPermissionResponse(targetIdentity);
  EncryptedPermissionResponse encryptedResponse;
  try {
    const auto targetCert = getTargetIdentityCertificate(targetIdentity);
    encryptedResponse = encryptPermissionResponseForCertificate(response, targetCert);
  }
  catch (const std::exception& e) {
    std::cerr << "[PERMISSIONS/USER] Refusing to reply without encrypting for target="
              << targetIdentity.toUri()
              << " error=" << e.what()
              << std::endl;
    return;
  }

  ndn::Name dataName = interest.getName();
  dataName.appendTimestamp(ndn::time::system_clock::now());

  ndn::Data data(dataName);
  data.setContent(encryptedResponse.WireEncode());
  data.setFreshnessPeriod(ndn::time::seconds(2));
  m_keyChain.sign(data);
  m_face.put(data);

  std::cout << "[PERMISSIONS/USER] Encrypted reply target="
            << targetIdentity.toUri()
            << " entries=" << response.getEntries().size()
            << " data=" << data.getName()
            << " payload=" << encryptedResponse.toString()
            << std::endl;
}

void ServiceController::onProviderPermissionsInterest(const ndn::InterestFilter&,
                                                      const ndn::Interest& interest)
{
  ndn::Name targetIdentity;
  if (!parseProviderPermissionsInterestName(interest.getName(), targetIdentity)) {
    return;
  }

  PermissionResponse response = buildProviderPermissionResponse(targetIdentity);
  EncryptedPermissionResponse encryptedResponse;
  try {
    const auto targetCert = getTargetIdentityCertificate(targetIdentity);
    encryptedResponse = encryptPermissionResponseForCertificate(response, targetCert);
  }
  catch (const std::exception& e) {
    std::cerr << "[PERMISSIONS/PROVIDER] Refusing to reply without encrypting for target="
              << targetIdentity.toUri()
              << " error=" << e.what()
              << std::endl;
    return;
  }

  ndn::Name dataName = interest.getName();
  dataName.appendTimestamp(ndn::time::system_clock::now());

  ndn::Data data(dataName);
  data.setContent(encryptedResponse.WireEncode());
  data.setFreshnessPeriod(ndn::time::seconds(2));
  m_keyChain.sign(data);
  m_face.put(data);

  std::cout << "[PERMISSIONS/PROVIDER] Encrypted reply target="
            << targetIdentity.toUri()
            << " entries=" << response.getEntries().size()
            << " data=" << data.getName()
            << " payload=" << encryptedResponse.toString()
            << std::endl;
}

} // namespace ndn_service_framework
