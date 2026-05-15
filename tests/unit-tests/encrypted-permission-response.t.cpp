#include "tests/boost-test.hpp"

#include "ndn-service-framework/NDNSFMessages.hpp"
#include "ndn-service-framework/UserPermissionTable.hpp"
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
    , userIdentity("/test/user/alice")
    , providerIdentity("/test/provider/llm")
    , userCert(makeRsaIdentity(userKeyChain, userIdentity))
    , providerCert(makeRsaIdentity(providerKeyChain, providerIdentity))
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
                   UserPermissionTable& table)
  {
    if (response.getTargetIdentity() != expectedIdentity.toUri()) {
      return false;
    }
    if (response.getPermissionKind() != expectedPermissionKind) {
      return false;
    }

    for (const auto& entry : response.getEntries()) {
      ndn::Name providerServiceName(entry.getProviderName());
      providerServiceName.append(ndn::Name(entry.getServiceName()));
      table.insertPermission(providerServiceName.toUri(),
                             entry.getServiceName(),
                             entry.getToken());
    }
    return true;
  }

  static void
  checkInstalledPermission(const UserPermissionTable& table,
                           const std::string& providerName,
                           const std::string& serviceName,
                           const std::string& token)
  {
    ndn::Name fullServiceName(providerName);
    fullServiceName.append(ndn::Name(serviceName));

    auto installedToken = table.queryPermission(fullServiceName.toUri(), serviceName);
    BOOST_REQUIRE(installedToken);
    BOOST_CHECK_EQUAL(*installedToken, token);
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

  ndn::security::KeyChain userKeyChain;
  ndn::security::KeyChain providerKeyChain;
  ndn::Name userIdentity;
  ndn::Name providerIdentity;
  ndn::security::Certificate userCert;
  ndn::security::Certificate providerCert;
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

  UserPermissionTable table;
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

  UserPermissionTable table;
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

  UserPermissionTable table;
  BOOST_CHECK(!validateAndApply(decrypted, userIdentity, tlv::UserPermission, table));
  BOOST_CHECK(table.dumpAll().empty());
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

  UserPermissionTable table;
  BOOST_CHECK(!validateAndApply(decrypted, userIdentity, tlv::UserPermission, table));
  BOOST_CHECK(table.dumpAll().empty());
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
