#include "ServiceController.hpp"
#include "utils.hpp"

#include <ndn-cxx/security/transform.hpp>

#include <string_view>

namespace ndn_service_framework {

namespace {

ndn::span<const uint8_t>
bufferToSpan(const ndn::Buffer& buffer)
{
  return ndn::span<const uint8_t>(buffer.data(), buffer.size());
}

ndn::span<uint8_t>
mutableBufferToSpan(ndn::Buffer& buffer)
{
  return ndn::span<uint8_t>(buffer.data(), buffer.size());
}

ndn::Buffer
runAesCbc(ndn::span<const uint8_t> input,
          ndn::span<const uint8_t> key,
          ndn::span<const uint8_t> iv,
          ndn::CipherOperator op)
{
  ndn::OBufferStream output;
  ndn::security::transform::bufferSource(input) >>
    ndn::security::transform::blockCipher(ndn::BlockCipherAlgorithm::AES_CBC,
                                          op,
                                          key,
                                          iv) >>
    ndn::security::transform::streamSink(output);

  const auto result = output.buf();
  return ndn::Buffer(result->begin(), result->end());
}

ndn::Block
encryptWireBytesWithContentKeyForCertificate(const ndn::security::Certificate& cert,
                                             ndn::span<const uint8_t> plaintext)
{
  ndn::security::transform::PublicKey recipientPublicKey;
  recipientPublicKey.loadPkcs8(cert.getPublicKey());
  if (recipientPublicKey.getKeyType() != ndn::KeyType::RSA) {
    throw std::invalid_argument("Content encryption requires an RSA recipient certificate");
  }

  ndn::Buffer contentKey(32);
  ndn::Buffer iv(16);
  ndn::random::generateSecureBytes(mutableBufferToSpan(contentKey));
  ndn::random::generateSecureBytes(mutableBufferToSpan(iv));

  ndn::Buffer cipherText = runAesCbc(plaintext,
                                     bufferToSpan(contentKey),
                                     bufferToSpan(iv),
                                     ndn::CipherOperator::ENCRYPT);
  auto encryptedContentKey = recipientPublicKey.encrypt(bufferToSpan(contentKey));

  EncryptedPermissionResponse encryptedPayload;
  encryptedPayload.setRecipientCertName(cert.getName().toUri());
  encryptedPayload.setAlgorithm("RSA-WRAPPED-AES-CBC");
  encryptedPayload.setEncryptedAesKey(
    ndn::Buffer(encryptedContentKey->begin(), encryptedContentKey->end()));
  encryptedPayload.setIv(iv);
  encryptedPayload.setCipherText(cipherText);
  return encryptedPayload.WireEncode();
}

std::string
makeDemoAuthorizationToken(std::string_view permissionKind,
                           const ndn::Name& targetIdentity,
                           const std::string& providerName,
                           const std::string& serviceName)
{
  return "DEMO-UNSIGNED-AUTHZ-TOKEN:v1:kind=" + std::string(permissionKind) +
         ":target=" + targetIdentity.toUri() +
         ":provider=" + providerName +
         ":service=" + serviceName;
}

ndn::Name
stripTrailingParametersDigest(ndn::Name name)
{
  if (!name.empty() && name[-1].isParametersSha256Digest()) {
    name = name.getPrefix(-1);
  }
  return name;
}

} // namespace

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
}

void ServiceController::setControllerPrefix(const ndn::Name& prefix)
{
  if (m_isRegistered) {
    throw std::logic_error("Cannot change ServiceController prefix after start()");
  }

  m_controllerPrefix = prefix;
  m_hasCustomControllerPrefix = true;
}

void ServiceController::start()
{
  registerInterestHandlers();
}

void ServiceController::run()
{
  start();
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
  entityOut = stripTrailingParametersDigest(interestName.getSubName(prefix.size()));
  return true;
}

bool ServiceController::parseUserPermissionsInterestName(const ndn::Name& interestName,
                                                         ndn::Name& targetIdentity) const
{
  if (!m_prefixUserPermissions.isPrefixOf(interestName) ||
      interestName.size() <= m_prefixUserPermissions.size()) {
    return false;
  }

  targetIdentity = stripTrailingParametersDigest(
    interestName.getSubName(m_prefixUserPermissions.size()));
  return true;
}

bool ServiceController::parseProviderPermissionsInterestName(const ndn::Name& interestName,
                                                             ndn::Name& targetIdentity) const
{
  if (!m_prefixProviderPermissions.isPrefixOf(interestName) ||
      interestName.size() <= m_prefixProviderPermissions.size()) {
    return false;
  }

  targetIdentity = stripTrailingParametersDigest(
    interestName.getSubName(m_prefixProviderPermissions.size()));
  return true;
}

void ServiceController::registerInterestHandlers()
{
  if (m_isRegistered) {
    return;
  }

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

  m_isRegistered = true;
}

ndn::Block ServiceController::makeAllowedServiceListTlv(const std::vector<std::string>& services) const
{
  ndn::Block list(tlv::AllowedServiceListType);

  for (const auto& s : services) {
    ndn::Name svcName(s);
    ndn::Block nameBlock = svcName.wireEncode();

    ndn::Block item(tlv::AllowedServiceType);
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
      entry.setToken(makeDemoAuthorizationToken("user", targetIdentity, providerName, service));
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
    entry.setToken(makeDemoAuthorizationToken("provider", targetIdentity,
                                             targetIdentity.toUri(), service));
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

bool
ServiceController::identitiesMatch(const ndn::Name& lhs, const ndn::Name& rhs) const
{
  return lhs == rhs;
}

ndn::Name ServiceController::getSignerCertNameFromInterest(const ndn::Interest& interest) const
{
  const auto sigInfo = interest.getSignatureInfo();
  if (!sigInfo) {
    throw std::runtime_error("Interest is not signed");
  }

  if (!sigInfo->hasKeyLocator()) {
    throw std::runtime_error("Interest SignatureInfo has no KeyLocator");
  }

  const auto& kl = sigInfo->getKeyLocator();

  if (kl.getType() != ndn::tlv::Name) {
    throw std::runtime_error("Interest KeyLocator is not a Name");
  }

  const ndn::Name certName = kl.getName();
  if (!ndn::security::Certificate::isValidName(certName)) {
    throw std::runtime_error("Interest KeyLocator Name is not a certificate name: " +
                             certName.toUri());
  }

  return certName;
}

ndn::security::Certificate
ServiceController::getSignerCertificateFromInterest(const ndn::Interest& interest) const
{
  const ndn::Name certName = getSignerCertNameFromInterest(interest);

  try {
    const ndn::Name identityName = ndn::security::extractIdentityFromCertName(certName);
    const ndn::Name keyName = ndn::security::extractKeyNameFromCertName(certName);
    return m_keyChain.getPib()
      .getIdentity(identityName)
      .getKey(keyName)
      .getCertificate(certName);
  }
  catch (const std::exception&) {
    throw std::runtime_error("Cannot find signer certificate in PIB: " + certName.toUri());
  }
}

ndn::Name
ServiceController::getSignerIdentityFromInterest(const ndn::Interest& interest) const
{
  const auto signerCert = getSignerCertificateFromInterest(interest);
  return ndn::security::extractIdentityFromCertName(signerCert.getName());
}

// ===================== Encryption =====================

ndn::Block
ServiceController::encryptForCertificate(const ndn::security::Certificate& cert,
                                        const ndn::Block& plaintext) const
{
  ndn::Block pt = plaintext;
  pt.encode();
  return encryptWireBytesWithContentKeyForCertificate(
    cert, ndn::span<const uint8_t>(pt.data(), pt.size()));
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
      ndn::Name signerIdentity;
      ndn::security::Certificate signerCert;
      try {
        signerIdentity = getSignerIdentityFromInterest(validatedInterest);
        signerCert = getSignerCertificateFromInterest(validatedInterest);
      }
      catch (const std::exception& e) {
        std::cerr << "[SERVICEACCESS] authorization failed: requested="
                  << userName
                  << " signer=<unknown>"
                  << " error=" << e.what()
                  << std::endl;
        return;
      }

      if (!identitiesMatch(signerIdentity, userName)) {
        std::cerr << "[SERVICEACCESS] authorization failed: requested="
                  << userName
                  << " signer=" << signerIdentity
                  << std::endl;
        return;
      }

      const std::string userUri = userName.toUri();

      // 查权限（你现在按 name 里的 userUri 查）
      std::vector<std::string> services;
      if (auto it = m_userAllowedServices.find(userUri); it != m_userAllowedServices.end()) {
        services = it->second;
      }

      // Data name = prefix + user + timestamp
      ndn::Name dataName = validatedInterest.getName();
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
      ndn::Name signerIdentity;
      ndn::security::Certificate signerCert;
      try {
        signerIdentity = getSignerIdentityFromInterest(validatedInterest);
        signerCert = getSignerCertificateFromInterest(validatedInterest);
      }
      catch (const std::exception& e) {
        std::cerr << "[SERVICEPROVISION] authorization failed: requested="
                  << providerName
                  << " signer=<unknown>"
                  << " error=" << e.what()
                  << std::endl;
        return;
      }

      if (!identitiesMatch(signerIdentity, providerName)) {
        std::cerr << "[SERVICEPROVISION] authorization failed: requested="
                  << providerName
                  << " signer=" << signerIdentity
                  << std::endl;
        return;
      }

      const std::string providerUri = providerName.toUri();

      std::vector<std::string> services;
      if (auto it = m_providerAllowedServices.find(providerUri); it != m_providerAllowedServices.end()) {
        services = it->second;
      }

      ndn::Name dataName = validatedInterest.getName();
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

  m_validator.validate(
    interest,
    [this, targetIdentity](const ndn::Interest& validatedInterest) {
      ndn::Name signerIdentity;
      try {
        signerIdentity = getSignerIdentityFromInterest(validatedInterest);
      }
      catch (const std::exception& e) {
        std::cerr << "[PERMISSIONS/USER] authorization failed: requested="
                  << targetIdentity
                  << " signer=<unknown>"
                  << " error=" << e.what()
                  << std::endl;
        return;
      }

      if (!identitiesMatch(signerIdentity, targetIdentity)) {
        std::cerr << "[PERMISSIONS/USER] authorization failed: requested="
                  << targetIdentity
                  << " signer=" << signerIdentity
                  << std::endl;
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

      ndn::Name dataName = validatedInterest.getName();
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
    },
    [this](const ndn::Interest& badInterest, const ndn::security::ValidationError& err) {
      std::cerr << "[PERMISSIONS/USER] Interest validation failed: "
                << err << " name=" << badInterest.getName() << std::endl;
    });
}

void ServiceController::onProviderPermissionsInterest(const ndn::InterestFilter&,
                                                      const ndn::Interest& interest)
{
  ndn::Name targetIdentity;
  if (!parseProviderPermissionsInterestName(interest.getName(), targetIdentity)) {
    return;
  }

  m_validator.validate(
    interest,
    [this, targetIdentity](const ndn::Interest& validatedInterest) {
      ndn::Name signerIdentity;
      try {
        signerIdentity = getSignerIdentityFromInterest(validatedInterest);
      }
      catch (const std::exception& e) {
        std::cerr << "[PERMISSIONS/PROVIDER] authorization failed: requested="
                  << targetIdentity
                  << " signer=<unknown>"
                  << " error=" << e.what()
                  << std::endl;
        return;
      }

      if (!identitiesMatch(signerIdentity, targetIdentity)) {
        std::cerr << "[PERMISSIONS/PROVIDER] authorization failed: requested="
                  << targetIdentity
                  << " signer=" << signerIdentity
                  << std::endl;
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

      ndn::Name dataName = validatedInterest.getName();
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
    },
    [this](const ndn::Interest& badInterest, const ndn::security::ValidationError& err) {
      std::cerr << "[PERMISSIONS/PROVIDER] Interest validation failed: "
                << err << " name=" << badInterest.getName() << std::endl;
    });
}

} // namespace ndn_service_framework
