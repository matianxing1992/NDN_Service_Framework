#ifndef NDNSF_TESTS_FIXTURES_FIXED_KEYCHAIN_TRUST_HPP
#define NDNSF_TESTS_FIXTURES_FIXED_KEYCHAIN_TRUST_HPP

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/util/io.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ndnsf_distributed_repo::tests {

/** Write fixed fixture anchors with the maintained hierarchical signer rule. */
inline std::string
writeFixedKeychainTrust(const std::filesystem::path& directory, ndn::KeyChain& keyChain)
{
  const auto path = directory / "fixed-trust.conf";
  std::ofstream schema(path);
  schema << "rule { id fixture for data checker { type customized sig-type rsa-sha256 "
            "key-locator { type name hyper-relation { "
            "k-regex \"^(<>*)<KEY|8=KEY><>{1,3}$\" k-expand \"\\\\1\" "
            "h-relation is-prefix-of p-regex \"^(<>*)$\" p-expand \"\\\\1\" } } } }\n";
  std::size_t anchors = 0;
  for (const auto& identity : keyChain.getPib().getIdentities()) {
    std::ostringstream encoded;
    ndn::io::save(identity.getDefaultKey().getDefaultCertificate(), encoded);
    auto base64 = encoded.str();
    base64.erase(std::remove(base64.begin(), base64.end(), '\n'), base64.end());
    schema << "trust-anchor { type base64 base64-string \"" << base64 << "\" }\n";
    ++anchors;
  }
  schema.close();
  if (!schema || anchors == 0) {
    throw std::runtime_error("fixed fixture trust schema unavailable");
  }
  return path.string();
}

} // namespace ndnsf_distributed_repo::tests

#endif // NDNSF_TESTS_FIXTURES_FIXED_KEYCHAIN_TRUST_HPP
