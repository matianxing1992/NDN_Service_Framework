#ifndef NDNSF_TESTS_ISOLATED_TEST_TRUST_HPP
#define NDNSF_TESTS_ISOLATED_TEST_TRUST_HPP

#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/io.hpp>
#include <ndn-cxx/util/random.hpp>
#include <ndn-cxx/util/string-helper.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ndn_service_framework::test {
namespace trust_detail {

inline std::filesystem::path
makeTestTrustDirectory()
{
  auto pattern = (std::filesystem::temp_directory_path() /
                  "ndnsf-spec170-trust-XXXXXX").string();
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  const char* directory = ::mkdtemp(buffer.data());
  if (directory == nullptr) {
    throw std::runtime_error("cannot create isolated Spec170 test trust directory");
  }
  return directory;
}

inline void
writeTestTrustSchema(const std::filesystem::path& directory,
                     const std::vector<ndn::security::Certificate>& anchors,
                     std::string_view rules = {})
{
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().filename().string().compare(0, 7, "anchor-") == 0) {
      std::filesystem::remove(entry.path());
    }
  }
  std::ofstream schema(directory / "trust.conf");
  if (rules.empty()) {
    rules = R"SCHEMA(rule
{
  id "isolated test certificates and data"
  for data
  filter { type name regex ^<>*$ }
  checker
  {
    type customized
    sig-type rsa-sha256
    key-locator { type name regex ^<>*$ }
  }
  checker { type customized sig-type ecdsa-sha256 key-locator { type name regex ^<>*$ } }
}
rule
{
  id "isolated test command interests"
  for interest
  filter { type name regex ^<>*$ }
  checker
  {
    type customized
    sig-type rsa-sha256
    key-locator { type name regex ^<>*$ }
  }
  checker { type customized sig-type ecdsa-sha256 key-locator { type name regex ^<>*$ } }
}
)SCHEMA";
  }
  schema << rules << '\n';
  for (size_t index = 0; index < anchors.size(); ++index) {
    const auto fileName = "anchor-" + std::to_string(index) + ".cert";
    ndn::io::save(anchors[index], (directory / fileName).string());
    schema << "trust-anchor { type file file-name \"" << fileName << "\" }\n";
  }
  if (anchors.empty()) {
    throw std::logic_error("isolated Spec170 test trust schema has no anchors");
  }
  schema.close();
  if (!schema) {
    std::filesystem::remove_all(directory);
    throw std::runtime_error("cannot write isolated Spec170 test trust schema");
  }
}


} // namespace trust_detail

/** Provision the real Controller seal for integration tests without retaining credentials. */
class ControllerSealFixture
{
public:
  ControllerSealFixture()
    : m_directory(trust_detail::makeTestTrustDirectory())
  {
    const auto path = m_directory / "controller.ring";
    std::array<uint8_t, 32> key{};
    ndn::random::generateSecureBytes(key);
    std::ofstream ring(path);
    ring << "controller-abe " << ndn::toHex(key, false) << '\n';
    ring.close();
    std::error_code error;
    std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                       std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, error);
    if (!ring || error) {
      std::filesystem::remove_all(m_directory, error);
      throw std::runtime_error("cannot provision the isolated Controller seal");
    }
    for (const auto* name : {"NDNSF_FILE_KEYRING", "NDNSF_CONTROLLER_GENERATION_STATE",
                             "NDNSF_CONTROLLER_MIN_VERSION", "NDNSF_CONTROLLER_RECOVERY_CHECKPOINT",
                             "NDNSF_CONTROLLER_RECOVERY_IMPORT_SHA256"}) {
      const auto* previous = std::getenv(name);
      m_previous.emplace(name, previous ? std::optional<std::string>(previous) : std::nullopt);
    }
    if (::setenv("NDNSF_FILE_KEYRING", path.c_str(), 1) != 0) {
      std::filesystem::remove_all(m_directory, error);
      throw std::runtime_error("cannot select the isolated Controller seal");
    }
    for (const auto& entry : m_previous) {
      if (entry.first != "NDNSF_FILE_KEYRING")
        ::unsetenv(entry.first.c_str());
    }
  }

  ~ControllerSealFixture()
  {
    for (const auto& [name, value] : m_previous) {
      if (value)
        ::setenv(name.c_str(), value->c_str(), 1);
      else
        ::unsetenv(name.c_str());
    }
    std::error_code ignored;
    std::filesystem::remove_all(m_directory, ignored);
  }

private:
  std::filesystem::path m_directory;
  std::map<std::string, std::optional<std::string>> m_previous;
};


/** Fixed certificate anchors for local tests, without starting NAC actors. */
class IsolatedTestTrust
{
public:
  IsolatedTestTrust()
    : m_directory(trust_detail::makeTestTrustDirectory())
  {
  }

  explicit IsolatedTestTrust(std::initializer_list<ndn::security::Certificate> anchors,
                             std::string rules = {})
    : IsolatedTestTrust()
  {
    m_rules = std::move(rules);
    m_anchors.assign(anchors);
    trust_detail::writeTestTrustSchema(m_directory, m_anchors, m_rules);
  }

  ~IsolatedTestTrust()
  {
    std::error_code ignored;
    std::filesystem::remove_all(m_directory, ignored);
  }
  IsolatedTestTrust(const IsolatedTestTrust&) = delete;
  IsolatedTestTrust& operator=(const IsolatedTestTrust&) = delete;

  ndn::security::Certificate
  createTestIdentityCertificate(ndn::KeyChain& keyChain, const ndn::Name& identity)
  {
    auto certificate = keyChain.createIdentity(identity, ndn::RsaKeyParams(2048))
                         .getDefaultKey().getDefaultCertificate();
    m_anchors.push_back(certificate);
    trust_detail::writeTestTrustSchema(m_directory, m_anchors, m_rules);
    return certificate;
  }
  std::string schemaPath() const { return (m_directory / "trust.conf").string(); }

private:
  std::filesystem::path m_directory;
  std::vector<ndn::security::Certificate> m_anchors;
  std::string m_rules;
};

} // namespace ndn_service_framework::test

#endif // NDNSF_TESTS_ISOLATED_TEST_TRUST_HPP
