#include "ndn-service-framework/controller-certificate.hpp"
#include "tests/boost-test.hpp"

#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/validity-period.hpp>
#include <ndn-cxx/util/random.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include <sys/stat.h>

namespace ndn_service_framework::tests {
namespace {

const ndn::Name CONTROLLER("/spec217/controller");

class CertificateFiles
{
public:
  CertificateFiles()
    : m_root(std::filesystem::temp_directory_path() /
             ("spec217-controller-cert-" + std::to_string(ndn::random::generateWord64())))
  {
    if (!std::filesystem::create_directory(m_root))
      throw std::runtime_error("certificate fixture directory already exists");
    std::filesystem::permissions(m_root, std::filesystem::perms::owner_all);
  }

  ~CertificateFiles()
  {
    std::error_code error;
    std::filesystem::remove_all(m_root, error);
  }

  std::filesystem::path
  path(const std::string& filename) const
  {
    return m_root / filename;
  }

private:
  std::filesystem::path m_root;
};

} // namespace

BOOST_AUTO_TEST_SUITE(Spec217ControllerCertificate)

BOOST_AUTO_TEST_CASE(MissingControllerDoesNotCreateIdentity)
{
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  BOOST_CHECK(keys.getPib().getIdentities().empty());
  BOOST_CHECK_THROW(loadControllerCertificate(keys, CONTROLLER, nullptr), std::exception);
  BOOST_CHECK(keys.getPib().getIdentities().empty());
}

BOOST_AUTO_TEST_CASE(ExistingPublicCertificateCanBeReadWithoutMutation)
{
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  const auto expected = keys.createIdentity(CONTROLLER).getDefaultKey().getDefaultCertificate();
  const auto actual = loadControllerCertificate(keys, CONTROLLER, nullptr);
  BOOST_CHECK(actual.wireEncode() == expected.wireEncode());
  BOOST_CHECK_EQUAL(keys.getPib().getIdentities().size(), 1U);
  BOOST_CHECK_EQUAL(keys.getPib().getIdentity(CONTROLLER).getKeys().size(), 1U);
}

BOOST_AUTO_TEST_CASE(ExplicitPublicFileDoesNotImportForeignIdentity)
{
  CertificateFiles files;
  ndn::KeyChain issuer("pib-memory:", "tpm-memory:");
  ndn::KeyChain recipient("pib-memory:", "tpm-memory:");
  const auto expected = issuer.createIdentity(CONTROLLER).getDefaultKey().getDefaultCertificate();
  const auto path = files.path("controller.cert").string();
  ndn::io::save(expected, path);
  const auto actual = loadControllerCertificate(recipient, CONTROLLER, path.c_str());
  BOOST_CHECK(actual.wireEncode() == expected.wireEncode());
  BOOST_CHECK(recipient.getPib().getIdentities().empty());
}

BOOST_AUTO_TEST_CASE(InvalidExplicitFileCannotFallBackToExistingController)
{
  CertificateFiles files;
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  keys.createIdentity(CONTROLLER);
  const auto foreign = keys.createIdentity(ndn::Name("/spec217/foreign"))
                         .getDefaultKey().getDefaultCertificate();
  const auto wrong = files.path("wrong.cert").string();
  ndn::io::save(foreign, wrong);
  const auto missing = files.path("missing.cert").string();
  const auto malformed = files.path("malformed.cert").string();
  std::ofstream(malformed) << "not a certificate";
  const auto oversized = files.path("oversized.cert").string();
  std::ofstream(oversized) << std::string(65537, 'x');
  const auto fifo = files.path("certificate-fifo").string();
  BOOST_REQUIRE_EQUAL(::mkfifo(fifo.c_str(), 0600), 0);
  for (const auto& path : {wrong, missing, malformed, oversized, fifo}) {
    BOOST_CHECK_THROW(loadControllerCertificate(keys, CONTROLLER, path.c_str()),
                      std::exception);
  }
  BOOST_CHECK_EQUAL(keys.getPib().getIdentities().size(), 2U);
}

BOOST_AUTO_TEST_CASE(ExpiredControllerFileAndPibCertificateAreRejected)
{
  CertificateFiles files;
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  const auto key = keys.createIdentity(CONTROLLER).getDefaultKey();
  auto expired = key.getDefaultCertificate();
  expired.setName(ndn::Name(key.getName()).append("expired").appendVersion(1));
  auto info = expired.getSignatureInfo();
  const auto now = ndn::time::system_clock::now();
  info.setValidityPeriod(ndn::security::ValidityPeriod(now - ndn::time::hours(2),
                                                     now - ndn::time::hours(1)));
  auto signing = ndn::security::signingByIdentity(CONTROLLER);
  signing.setSignatureInfo(info);
  keys.sign(expired, signing);
  BOOST_REQUIRE(!expired.isValid());
  const auto path = files.path("expired.cert").string();
  ndn::io::save(expired, path);
  BOOST_CHECK_THROW(loadControllerCertificate(keys, CONTROLLER, path.c_str()),
                    std::exception);
  keys.addCertificate(key, expired);
  keys.setDefaultCertificate(key, expired);
  BOOST_CHECK_THROW(loadControllerCertificate(keys, CONTROLLER, nullptr), std::exception);
  BOOST_CHECK_EQUAL(keys.getPib().getIdentities().size(), 1U);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::tests
