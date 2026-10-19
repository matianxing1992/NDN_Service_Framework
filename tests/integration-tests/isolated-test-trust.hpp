#ifndef NDNSF_TESTS_ISOLATED_TEST_TRUST_HPP
#define NDNSF_TESTS_ISOLATED_TEST_TRUST_HPP

#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/security/key-chain.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace ndn_service_framework::test {

/** Fixed certificate anchors for local tests, without starting NAC actors. */
class IsolatedTestTrust
{
public:
  IsolatedTestTrust();
  ~IsolatedTestTrust();
  IsolatedTestTrust(const IsolatedTestTrust&) = delete;
  IsolatedTestTrust& operator=(const IsolatedTestTrust&) = delete;

  ndn::security::Certificate createTestIdentityCertificate(ndn::KeyChain& keyChain,
                                                          const ndn::Name& identity);
  std::string schemaPath() const { return (m_directory / "trust.conf").string(); }

private:
  std::filesystem::path m_directory;
  std::vector<ndn::security::Certificate> m_anchors;
};

} // namespace ndn_service_framework::test

#endif // NDNSF_TESTS_ISOLATED_TEST_TRUST_HPP
