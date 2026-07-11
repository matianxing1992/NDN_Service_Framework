#include "tests/boost-test.hpp"

#include "ndn-service-framework/NDNSFMessages.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/ServiceAuthorizationTable.hpp"
#include "ndn-service-framework/utils.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>

namespace ndn_service_framework::test {
namespace {

class EncryptedPermissionResponseFixture
{
protected:
  EncryptedPermissionResponseFixture()
    : userKeyChain("pib-memory:encrypted-permission-user", "tpm-memory:encrypted-permission-user")
    , providerKeyChain("pib-memory:encrypted-permission-provider", "tpm-memory:encrypted-permission-provider")
    , controllerKeyChain("pib-memory:encrypted-permission-controller", "tpm-memory:encrypted-permission-controller")
    , otherKeyChain("pib-memory:encrypted-permission-other", "tpm-memory:encrypted-permission-other")
    , userIdentity("/test/user/alice")
    , providerIdentity("/test/provider/llm")
    , controllerIdentity("/test/controller")
    , otherIdentity("/test/user/bob")
    , userCert(makeRsaIdentity(userKeyChain, userIdentity))
    , providerCert(makeRsaIdentity(providerKeyChain, providerIdentity))
    , controllerCert(makeRsaIdentity(controllerKeyChain, controllerIdentity))
    , otherCert(makeRsaIdentity(otherKeyChain, otherIdentity))
  {
  }

  static ndn::security::Certificate
  makeRsaIdentity(ndn::security::KeyChain& keyChain, const ndn::Name& identity)
  {
    auto id = keyChain.createIdentity(identity, ndn::RsaKeyParams(2048));
    return id.getDefaultKey().getDefaultCertificate();
  }

  static PermissionEntry
  makeEntry(const std::string& providerName,
            const std::string& serviceName,
            const std::string& token)
  {
    PermissionEntry entry;
    entry.setProviderName(providerName);
    entry.setServiceName(serviceName);
    entry.setToken(token);
    entry.setTtl(0);
    entry.setVersion(1);
    return entry;
  }

  static PermissionResponse
  makeResponse(const ndn::Name& targetIdentity,
               size_t permissionKind,
               const std::string& providerName,
               const std::string& serviceName,
               const std::string& token)
  {
    PermissionResponse response;
    response.setTargetIdentity(targetIdentity.toUri());
    response.setPermissionKind(permissionKind);
    response.addEntry(makeEntry(providerName, serviceName, token));
    return response;
  }

  static bool
  validateAndApply(const PermissionResponse& response,
                   const ndn::Name& expectedIdentity,
                   size_t expectedPermissionKind,
                   ServiceAuthorizationTable& table)
  {
    if (response.getTargetIdentity() != expectedIdentity.toUri()) {
      return false;
    }
    if (response.getPermissionKind() != expectedPermissionKind) {
      return false;
    }

    std::vector<ServiceAuthorizationRecord> records;
    for (const auto& entry : response.getEntries()) {
      ndn::Name providerServiceName(entry.getProviderName());
      providerServiceName.append(ndn::Name(entry.getServiceName()));
      records.push_back(ServiceAuthorizationRecord{
        providerServiceName.toUri(), entry.getServiceName(),
        response.getPermissionKind(), response.getPolicyEpoch()});
    }
    return table.replacePermissions(response.getPermissionKind(),
                                    response.getPolicyEpoch(), records);
  }

  static void
  checkInstalledPermission(const ServiceAuthorizationTable& table,
                           const std::string& providerName,
                           const std::string& serviceName,
                           const std::string& token)
  {
    ndn::Name fullServiceName(providerName);
    fullServiceName.append(ndn::Name(serviceName));

    (void)token;
    auto record = table.find(fullServiceName.toUri());
    BOOST_REQUIRE(record);
    BOOST_CHECK_EQUAL(record->serviceName, serviceName);
    BOOST_CHECK_GT(record->policyEpoch, 0);
  }

  static void
  checkSamePermissionResponse(const PermissionResponse& actual,
                              const PermissionResponse& expected)
  {
    BOOST_CHECK_EQUAL(actual.getTargetIdentity(), expected.getTargetIdentity());
    BOOST_CHECK_EQUAL(actual.getPermissionKind(), expected.getPermissionKind());
    BOOST_REQUIRE_EQUAL(actual.getEntries().size(), expected.getEntries().size());

    const auto& actualEntry = actual.getEntries().front();
    const auto& expectedEntry = expected.getEntries().front();
    BOOST_CHECK_EQUAL(actualEntry.getProviderName(), expectedEntry.getProviderName());
    BOOST_CHECK_EQUAL(actualEntry.getServiceName(), expectedEntry.getServiceName());
    BOOST_CHECK_EQUAL(actualEntry.getToken(), expectedEntry.getToken());
    BOOST_CHECK_EQUAL(actualEntry.getTtl(), expectedEntry.getTtl());
    BOOST_CHECK_EQUAL(actualEntry.getVersion(), expectedEntry.getVersion());
  }

  static EncryptedPermissionResponse
  checkEncryptedPermissionResponseWireRoundTrip(const EncryptedPermissionResponse& encrypted)
  {
    EncryptedPermissionResponse decoded;
    BOOST_REQUIRE(decoded.WireDecode(encrypted.WireEncode()));
    BOOST_CHECK_EQUAL(decoded.getRecipientCertName(), encrypted.getRecipientCertName());
    BOOST_CHECK_EQUAL(decoded.getAlgorithm(), encrypted.getAlgorithm());
    BOOST_CHECK_EQUAL_COLLECTIONS(decoded.getEncryptedAesKey().begin(),
                                  decoded.getEncryptedAesKey().end(),
                                  encrypted.getEncryptedAesKey().begin(),
                                  encrypted.getEncryptedAesKey().end());
    BOOST_CHECK_EQUAL_COLLECTIONS(decoded.getIv().begin(),
                                  decoded.getIv().end(),
                                  encrypted.getIv().begin(),
                                  encrypted.getIv().end());
    BOOST_CHECK_EQUAL_COLLECTIONS(decoded.getCipherText().begin(),
                                  decoded.getCipherText().end(),
                                  encrypted.getCipherText().begin(),
                                  encrypted.getCipherText().end());
    return decoded;
  }

  static ndn::Data
  makeUnsignedEncryptedPermissionData(const ndn::Name& dataName,
                                      const PermissionResponse& response,
                                      const ndn::security::Certificate& recipientCert)
  {
    auto encrypted = encryptPermissionResponseForCertificate(response, recipientCert);

    ndn::Data data(dataName);
    data.setFreshnessPeriod(ndn::time::seconds(2));
    data.setContent(encrypted.WireEncode());
    return data;
  }

  static ndn::Data
  makeDigestSignedEncryptedPermissionData(const ndn::Name& dataName,
                                          const PermissionResponse& response,
                                          const ndn::security::Certificate& recipientCert,
                                          ndn::security::KeyChain& signerKeyChain)
  {
    auto encrypted = encryptPermissionResponseForCertificate(response, recipientCert);

    ndn::Data data(dataName);
    data.setFreshnessPeriod(ndn::time::seconds(2));
    data.setContent(encrypted.WireEncode());
    signerKeyChain.sign(data, ndn::security::signingWithSha256());
    return data;
  }

  static ndn::Data
  makeSignedEncryptedPermissionData(const ndn::Name& dataName,
                                    const PermissionResponse& response,
                                    const ndn::security::Certificate& recipientCert,
                                    ndn::security::KeyChain& signerKeyChain)
  {
    auto encrypted = encryptPermissionResponseForCertificate(response, recipientCert);

    ndn::Data data(dataName);
    data.setFreshnessPeriod(ndn::time::seconds(2));
    data.setContent(encrypted.WireEncode());
    signerKeyChain.sign(data);
    return data;
  }

  static ndn::Data
  makeSignedPlaintextPermissionData(const ndn::Name& dataName,
                                    const PermissionResponse& response,
                                    ndn::security::KeyChain& signerKeyChain)
  {
    ndn::Data data(dataName);
    data.setFreshnessPeriod(ndn::time::seconds(2));
    data.setContent(response.WireEncode());
    signerKeyChain.sign(data);
    return data;
  }

  ndn::security::KeyChain userKeyChain;
  ndn::security::KeyChain providerKeyChain;
  ndn::security::KeyChain controllerKeyChain;
  ndn::security::KeyChain otherKeyChain;
  ndn::Name userIdentity;
  ndn::Name providerIdentity;
  ndn::Name controllerIdentity;
  ndn::Name otherIdentity;
  ndn::security::Certificate userCert;
  ndn::security::Certificate providerCert;
  ndn::security::Certificate controllerCert;
  ndn::security::Certificate otherCert;
};

} // namespace

BOOST_AUTO_TEST_SUITE(EncryptedPermissionResponse)

BOOST_FIXTURE_TEST_CASE(PermissionResponseWireEncodeDecode, EncryptedPermissionResponseFixture)
{
  auto response = makeResponse(userIdentity,
                               tlv::UserPermission,
                               "/test/provider/camera",
                               "/ObjectDetection/YOLOv8",
                               "user-token");

  PermissionResponse decoded;
  BOOST_REQUIRE(decoded.WireDecode(response.WireEncode()));
  checkSamePermissionResponse(decoded, response);
}

BOOST_FIXTURE_TEST_CASE(EncryptedPermissionResponseWireEncodeDecode, EncryptedPermissionResponseFixture)
{
  auto response = makeResponse(userIdentity,
                               tlv::UserPermission,
                               "/test/provider/camera",
                               "/ObjectDetection/YOLOv8",
                               "user-token");

  auto encrypted = encryptPermissionResponseForCertificate(response, userCert);
  BOOST_CHECK_EQUAL(encrypted.getRecipientCertName(), userCert.getName().toUri());
  BOOST_CHECK_EQUAL(encrypted.getAlgorithm(), "RSA-WRAPPED-AES-CBC");
  BOOST_CHECK(!encrypted.getEncryptedAesKey().empty());
  BOOST_CHECK_EQUAL(encrypted.getIv().size(), 16);
  BOOST_CHECK(!encrypted.getCipherText().empty());

  checkEncryptedPermissionResponseWireRoundTrip(encrypted);
}

BOOST_FIXTURE_TEST_CASE(UserPermissionResponseEncryptDecryptAndApply,
                        EncryptedPermissionResponseFixture)
{
  const std::string providerName = "/test/provider/camera";
  const std::string serviceName = "/ObjectDetection/YOLOv8";
  const std::string token = "user-token";
  auto response = makeResponse(userIdentity,
                               tlv::UserPermission,
                               providerName,
                               serviceName,
                               token);

  auto encrypted = encryptPermissionResponseForCertificate(response, userCert);
  auto decodedEncrypted = checkEncryptedPermissionResponseWireRoundTrip(encrypted);
  auto decrypted = decryptPermissionResponseWithKeyChain(decodedEncrypted, userKeyChain);
  checkSamePermissionResponse(decrypted, response);

  ServiceAuthorizationTable table;
  BOOST_CHECK(validateAndApply(decrypted, userIdentity, tlv::UserPermission, table));
  checkInstalledPermission(table, providerName, serviceName, token);
}

BOOST_FIXTURE_TEST_CASE(PermissionDiscoveryInterestMayBeUnsigned,
                        EncryptedPermissionResponseFixture)
{
  ndn::Name interestName("/test/controller/NDNSF/PERMISSIONS/USER");
  interestName.append(userIdentity);

  ndn::Interest interest(interestName);
  interest.setCanBePrefix(true);
  interest.setMustBeFresh(true);

  BOOST_CHECK(!interest.getSignatureInfo());
}

BOOST_FIXTURE_TEST_CASE(ControllerSignedDataIsEncryptedForTargetOnly,
                        EncryptedPermissionResponseFixture)
{
  const std::string providerName = "/test/provider/camera";
  const std::string serviceName = "/ObjectDetection/YOLOv8";
  const std::string token = "target-only-token";
  auto response = makeResponse(userIdentity,
                               tlv::UserPermission,
                               providerName,
                               serviceName,
                               token);

  auto data = makeSignedEncryptedPermissionData(
    ndn::Name("/test/controller/NDNSF/PERMISSIONS/USER/test/user/alice/%FE%00"),
    response,
    userCert,
    controllerKeyChain);

  const auto& sigInfo = data.getSignatureInfo();
  BOOST_REQUIRE(sigInfo.hasKeyLocator());
  BOOST_REQUIRE_EQUAL(sigInfo.getKeyLocator().getType(), ndn::tlv::Name);
  BOOST_CHECK_EQUAL(
    ndn::security::extractIdentityFromCertName(sigInfo.getKeyLocator().getName()).toUri(),
    controllerIdentity.toUri());

  auto [ok, encryptedBlock] = ndn::Block::fromBuffer(
    ndn::span<const uint8_t>(data.getContent().value(), data.getContent().value_size()));
  BOOST_REQUIRE(ok);

  ::ndn_service_framework::EncryptedPermissionResponse encrypted;
  BOOST_REQUIRE(encrypted.WireDecode(encryptedBlock));
  BOOST_CHECK_EQUAL(encrypted.getRecipientCertName(), userCert.getName().toUri());

  BOOST_CHECK_THROW(decryptPermissionResponseWithKeyChain(encrypted, otherKeyChain),
                    std::exception);

  auto decrypted = decryptPermissionResponseWithKeyChain(encrypted, userKeyChain);
  checkSamePermissionResponse(decrypted, response);

  ServiceAuthorizationTable table;
  BOOST_CHECK(validateAndApply(decrypted, userIdentity, tlv::UserPermission, table));
  checkInstalledPermission(table, providerName, serviceName, token);
}

BOOST_FIXTURE_TEST_CASE(ProviderPermissionResponseEncryptDecryptAndApply,
                        EncryptedPermissionResponseFixture)
{
  const std::string serviceName = "/LLM/Llama3/Prefill";
  const std::string token = "provider-token";
  auto response = makeResponse(providerIdentity,
                               tlv::ProviderPermission,
                               providerIdentity.toUri(),
                               serviceName,
                               token);

  auto encrypted = encryptPermissionResponseForCertificate(response, providerCert);
  auto decodedEncrypted = checkEncryptedPermissionResponseWireRoundTrip(encrypted);
  auto decrypted = decryptPermissionResponseWithKeyChain(decodedEncrypted, providerKeyChain);
  checkSamePermissionResponse(decrypted, response);

  ServiceAuthorizationTable table;
  BOOST_CHECK(validateAndApply(decrypted, providerIdentity, tlv::ProviderPermission, table));
  checkInstalledPermission(table, providerIdentity.toUri(), serviceName, token);
}

BOOST_FIXTURE_TEST_CASE(TargetIdentityCheckRejectsWrongTarget,
                        EncryptedPermissionResponseFixture)
{
  auto wrongTarget = makeResponse(ndn::Name("/test/user/not-alice"),
                                  tlv::UserPermission,
                                  "/test/provider/camera",
                                  "/ObjectDetection/YOLOv8",
                                  "wrong-target-token");

  auto encrypted = encryptPermissionResponseForCertificate(wrongTarget, userCert);
  auto decrypted = decryptPermissionResponseWithKeyChain(encrypted, userKeyChain);

  ServiceAuthorizationTable table;
  BOOST_CHECK(!validateAndApply(decrypted, userIdentity, tlv::UserPermission, table));
  BOOST_CHECK(table.snapshot().empty());
}

BOOST_FIXTURE_TEST_CASE(PermissionKindCheckRejectsWrongKind,
                        EncryptedPermissionResponseFixture)
{
  auto wrongKind = makeResponse(userIdentity,
                                tlv::ProviderPermission,
                                "/test/provider/camera",
                                "/ObjectDetection/YOLOv8",
                                "wrong-kind-token");

  auto encrypted = encryptPermissionResponseForCertificate(wrongKind, userCert);
  auto decrypted = decryptPermissionResponseWithKeyChain(encrypted, userKeyChain);

  ServiceAuthorizationTable table;
  BOOST_CHECK(!validateAndApply(decrypted, userIdentity, tlv::UserPermission, table));
  BOOST_CHECK(table.snapshot().empty());
}

BOOST_FIXTURE_TEST_CASE(ServiceUserPermissionFetchCallbackHandlesEncryptedData,
                        EncryptedPermissionResponseFixture)
{
  const std::string providerName = "/test/provider/camera";
  const std::string serviceName = "/ObjectDetection/YOLOv8";
  const std::string token = "callback-user-token";
  auto response = makeResponse(userIdentity,
                               tlv::UserPermission,
                               providerName,
                               serviceName,
                               token);
  auto data = makeSignedEncryptedPermissionData(ndn::Name("/test/controller/user-permissions"),
                                                response,
                                                userCert,
                                                userKeyChain);

  ServiceAuthorizationTable permissionTable;
  BOOST_CHECK(ServiceUser::handlePermissionResponseData(data,
                                                        userIdentity,
                                                        userKeyChain,
                                                        permissionTable));
  checkInstalledPermission(permissionTable, providerName, serviceName, token);

  auto plaintextData = makeSignedPlaintextPermissionData(
    ndn::Name("/test/controller/user-permissions/plaintext"),
    response,
    userKeyChain);
  BOOST_CHECK(!ServiceUser::handlePermissionResponseData(plaintextData,
                                                         userIdentity,
                                                         userKeyChain,
                                                         permissionTable));
  BOOST_CHECK_EQUAL(permissionTable.snapshot().size(), 1);

  auto wrongTarget = makeResponse(ndn::Name("/test/runtime/not-user"),
                                  tlv::UserPermission,
                                  providerName,
                                  serviceName,
                                  "wrong-target-token");
  auto wrongTargetData = makeSignedEncryptedPermissionData(
    ndn::Name("/test/controller/user-permissions/wrong-target"),
    wrongTarget,
    userCert,
    userKeyChain);
  BOOST_CHECK(!ServiceUser::handlePermissionResponseData(wrongTargetData,
                                                         userIdentity,
                                                         userKeyChain,
                                                         permissionTable));
  BOOST_CHECK_EQUAL(permissionTable.snapshot().size(), 1);

  auto wrongKind = makeResponse(userIdentity,
                                tlv::ProviderPermission,
                                providerName,
                                serviceName,
                                "wrong-kind-token");
  auto wrongKindData = makeSignedEncryptedPermissionData(
    ndn::Name("/test/controller/user-permissions/wrong-kind"),
    wrongKind,
    userCert,
    userKeyChain);
  BOOST_CHECK(!ServiceUser::handlePermissionResponseData(wrongKindData,
                                                         userIdentity,
                                                         userKeyChain,
                                                         permissionTable));
  BOOST_CHECK_EQUAL(permissionTable.snapshot().size(), 1);
}

BOOST_FIXTURE_TEST_CASE(ServiceProviderPermissionFetchCallbackHandlesEncryptedData,
                        EncryptedPermissionResponseFixture)
{
  const std::string serviceName = "/LLM/Llama3/Prefill";
  const std::string token = "callback-provider-token";
  auto response = makeResponse(providerIdentity,
                               tlv::ProviderPermission,
                               providerIdentity.toUri(),
                               serviceName,
                               token);
  auto data = makeSignedEncryptedPermissionData(ndn::Name("/test/controller/provider-permissions"),
                                                response,
                                                providerCert,
                                                providerKeyChain);

  ServiceAuthorizationTable permissionTable;
  BOOST_CHECK(ServiceProvider::handlePermissionResponseData(data,
                                                            providerIdentity,
                                                            providerKeyChain,
                                                            permissionTable));
  checkInstalledPermission(permissionTable,
                           providerIdentity.toUri(),
                           serviceName,
                           token);

  auto plaintextData = makeSignedPlaintextPermissionData(
    ndn::Name("/test/controller/provider-permissions/plaintext"),
    response,
    providerKeyChain);
  BOOST_CHECK(!ServiceProvider::handlePermissionResponseData(plaintextData,
                                                             providerIdentity,
                                                             providerKeyChain,
                                                             permissionTable));
  BOOST_CHECK_EQUAL(permissionTable.snapshot().size(), 1);

  auto wrongTarget = makeResponse(ndn::Name("/test/runtime/not-provider"),
                                  tlv::ProviderPermission,
                                  providerIdentity.toUri(),
                                  serviceName,
                                  "wrong-target-token");
  auto wrongTargetData = makeSignedEncryptedPermissionData(
    ndn::Name("/test/controller/provider-permissions/wrong-target"),
    wrongTarget,
    providerCert,
    providerKeyChain);
  BOOST_CHECK(!ServiceProvider::handlePermissionResponseData(wrongTargetData,
                                                             providerIdentity,
                                                             providerKeyChain,
                                                             permissionTable));
  BOOST_CHECK_EQUAL(permissionTable.snapshot().size(), 1);

  auto wrongKind = makeResponse(providerIdentity,
                                tlv::UserPermission,
                                providerIdentity.toUri(),
                                serviceName,
                                "wrong-kind-token");
  auto wrongKindData = makeSignedEncryptedPermissionData(
    ndn::Name("/test/controller/provider-permissions/wrong-kind"),
    wrongKind,
    providerCert,
    providerKeyChain);
  BOOST_CHECK(!ServiceProvider::handlePermissionResponseData(wrongKindData,
                                                             providerIdentity,
                                                             providerKeyChain,
                                                             permissionTable));
  BOOST_CHECK_EQUAL(permissionTable.snapshot().size(), 1);
}

BOOST_FIXTURE_TEST_CASE(PermissionResponseDataValidationRejectsForgedData,
                        EncryptedPermissionResponseFixture)
{
  const auto response = makeResponse(userIdentity,
                                     tlv::UserPermission,
                                     "/test/provider/camera",
                                     "/ObjectDetection/YOLOv8",
                                     "forged-token");
  const ndn::Name dataName("/test/controller/NDNSF/PERMISSIONS/USER/test/user/alice");
  MessageValidator validator("tests/reject-rsa-data.conf");
  ServiceAuthorizationTable permissionTable;

  auto unsignedData = makeUnsignedEncryptedPermissionData(dataName,
                                                          response,
                                                          userCert);
  bool unsignedFailureCalled = false;
  validator.validate(
    unsignedData,
    [&] (const ndn::Data& validatedData) {
      BOOST_CHECK(!ServiceUser::handlePermissionResponseData(validatedData,
                                                             userIdentity,
                                                             userKeyChain,
                                                             permissionTable));
    },
    [&] (const ndn::Data&, const ndn::security::ValidationError&) {
      unsignedFailureCalled = true;
    });
  BOOST_CHECK(unsignedFailureCalled);
  BOOST_CHECK(permissionTable.snapshot().empty());

  auto wrongSignedData = makeSignedEncryptedPermissionData(dataName,
                                                           response,
                                                           userCert,
                                                           otherKeyChain);
  bool wrongSignedFailureCalled = false;
  bool wrongSignedRejectedBeforeApply = false;
  validator.validate(
    wrongSignedData,
    [&] (const ndn::Data& validatedData) {
      const auto signerIdentity = ndn::security::extractIdentityFromCertName(
        validatedData.getSignatureInfo().getKeyLocator().getName());
      if (signerIdentity != controllerIdentity) {
        wrongSignedRejectedBeforeApply = true;
        return;
      }
      ServiceUser::handlePermissionResponseData(validatedData,
                                                userIdentity,
                                                userKeyChain,
                                                permissionTable);
    },
    [&] (const ndn::Data&, const ndn::security::ValidationError&) {
      wrongSignedFailureCalled = true;
    });
  BOOST_CHECK(wrongSignedFailureCalled || wrongSignedRejectedBeforeApply);
  BOOST_CHECK(permissionTable.snapshot().empty());
}

BOOST_FIXTURE_TEST_CASE(PermissionResponseValidationRejectsUnsupportedSignatureAndControllerMismatch,
                        EncryptedPermissionResponseFixture)
{
  const auto response = makeResponse(userIdentity,
                                     tlv::UserPermission,
                                     "/test/provider/camera",
                                     "/ObjectDetection/YOLOv8",
                                     "controller-token");
  const ndn::Name dataName("/test/controller/NDNSF/PERMISSIONS/USER/test/user/alice");
  MessageValidator validator("examples/trust-any.conf");
  ServiceAuthorizationTable permissionTable;

  auto digestSignedData = makeDigestSignedEncryptedPermissionData(dataName,
                                                                  response,
                                                                  userCert,
                                                                  controllerKeyChain);
  bool digestFailureCalled = false;
  validator.validate(
    digestSignedData,
    [&] (const ndn::Data& validatedData) {
      ServiceUser::handlePermissionResponseData(validatedData,
                                                userIdentity,
                                                userKeyChain,
                                                permissionTable);
    },
    [&] (const ndn::Data&, const ndn::security::ValidationError&) {
      digestFailureCalled = true;
    });
  BOOST_CHECK(digestFailureCalled);
  BOOST_CHECK(permissionTable.snapshot().empty());

  auto controllerSignedData = makeSignedEncryptedPermissionData(dataName,
                                                                response,
                                                                userCert,
                                                                controllerKeyChain);
  const ndn::Interest wrongControllerInterest(
    ndn::Name("/test/wrong-controller/NDNSF/PERMISSIONS/USER/test/user/alice"));
  bool signerMismatchRejected = false;
  validator.validate(
    controllerSignedData,
    [&] (const ndn::Data& validatedData) {
      const auto expectedController =
        wrongControllerInterest.getName().getPrefix(2);
      const auto signerIdentity = ndn::security::extractIdentityFromCertName(
        validatedData.getSignatureInfo().getKeyLocator().getName());
      if (signerIdentity != expectedController) {
        signerMismatchRejected = true;
        return;
      }
      ServiceUser::handlePermissionResponseData(validatedData,
                                                userIdentity,
                                                userKeyChain,
                                                permissionTable);
    },
    [&] (const ndn::Data&, const ndn::security::ValidationError&) {
    });
  BOOST_CHECK(signerMismatchRejected);
  BOOST_CHECK(permissionTable.snapshot().empty());
}

BOOST_FIXTURE_TEST_CASE(MessageValidatorFailureCallbackIsExplicit,
                        EncryptedPermissionResponseFixture)
{
  MessageValidator validator("tests/reject-rsa-data.conf");

  ndn::Data unsignedData(ndn::Name("/test/controller/NDNSF/PERMISSIONS/USER/test/user/alice"));
  unsignedData.setContent("bad");

  bool successCalled = false;
  bool failureCalled = false;
  validator.validate(
    unsignedData,
    [&] (const ndn::Data&) {
      successCalled = true;
    },
    [&] (const ndn::Data& badData, const ndn::security::ValidationError&) {
      BOOST_CHECK_EQUAL(badData.getName(), unsignedData.getName());
      failureCalled = true;
    });

  BOOST_CHECK(!successCalled);
  BOOST_CHECK(failureCalled);
  BOOST_CHECK_EQUAL(validator.getFailureCountForTesting(), 1);
}

BOOST_FIXTURE_TEST_CASE(ValidatorFailureCallbacksAreExactlyOnceAndLeaveNoState,
                        EncryptedPermissionResponseFixture)
{
  MessageValidator validator("tests/reject-rsa-data.conf");
  ServiceAuthorizationTable permissionTable;

  const auto response = makeResponse(userIdentity,
                                     tlv::UserPermission,
                                     "/test/provider/camera",
                                     "/ObjectDetection/YOLOv8",
                                     "never-installed");

  for (int i = 0; i < 20; ++i) {
    auto unsignedData = makeUnsignedEncryptedPermissionData(
      ndn::Name("/test/controller/NDNSF/PERMISSIONS/USER/test/user/alice/" +
                std::to_string(i)),
      response,
      userCert);
    int successCount = 0;
    int failureCount = 0;
    validator.validate(
      unsignedData,
      [&] (const ndn::Data& validatedData) {
        ++successCount;
        ServiceUser::handlePermissionResponseData(validatedData,
                                                  userIdentity,
                                                  userKeyChain,
                                                  permissionTable);
      },
      [&] (const ndn::Data&, const ndn::security::ValidationError&) {
        ++failureCount;
      });

    BOOST_CHECK_EQUAL(successCount, 0);
    BOOST_CHECK_EQUAL(failureCount, 1);
    BOOST_CHECK(permissionTable.snapshot().empty());
  }

  BOOST_CHECK_EQUAL(validator.getFailureCountForTesting(), 20);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
