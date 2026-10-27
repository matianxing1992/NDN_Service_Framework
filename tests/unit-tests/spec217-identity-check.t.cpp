#include "tests/boost-test.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/util/random.hpp>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

namespace ndn_service_framework::tests {
namespace {

class IdentityHome
{
public:
  IdentityHome()
    : root(std::filesystem::temp_directory_path() /
           ("spec217-identity-" + std::to_string(ndn::random::generateWord64())))
  {
    if (!std::filesystem::create_directory(root))
      throw std::runtime_error("identity fixture already exists");
    std::filesystem::permissions(root, std::filesystem::perms::owner_all);
  }

  ~IdentityHome()
  {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  int
  check(const ndn::Name& identity, bool portable = false, bool useDefaultIdentity = false) const
  {
    const char* binary = ::getenv("NDNSF_IDENTITY_CHECK_EXECUTABLE");
    if (binary == nullptr || !std::filesystem::path(binary).is_absolute())
      throw std::runtime_error("declare installed NDNSF_IDENTITY_CHECK_EXECUTABLE");
    const auto name = identity.toUri();
    const auto tpm = portable ? std::string("tpm-file:") :
      "tpm-file:" + (root / ".ndn").string();
    const auto child = ::fork();
    if (child < 0)
      throw std::runtime_error("cannot fork credential check");
    if (child == 0) {
      std::signal(SIGALRM, SIG_DFL);
      ::alarm(10);
      if (::setenv("NDN_CLIENT_TPM", tpm.c_str(), 1) != 0)
        ::_exit(126);
      if (useDefaultIdentity) {
        ::execl(binary, binary, root.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
      }
      ::execl(binary, binary, root.c_str(), name.c_str(), static_cast<char*>(nullptr));
      ::_exit(127);
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
      if (errno != EINTR)
        throw std::runtime_error("cannot wait for credential check");
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  }

  std::filesystem::path root;
};

ndn::KeyChain
openKeys(const IdentityHome& home)
{
  const auto directory = (home.root / ".ndn").string();
  return ndn::KeyChain("pib-sqlite3:" + directory, "tpm-file:" + directory, false);
}

const ndn::Name ACTOR("/spec217/actor");

} // namespace

BOOST_AUTO_TEST_SUITE(Spec217IdentityCheck)

BOOST_AUTO_TEST_CASE(ExistingKeysSignAndMissingIdentityDoesNotAppear)
{
  IdentityHome home;
  {
    auto keys = openKeys(home);
    keys.createIdentity(ACTOR);
  }
  BOOST_CHECK_EQUAL(home.check(ACTOR), 0);
  BOOST_CHECK_EQUAL(home.check(ndn::Name("/spec217/absent")), 1);
  auto keys = openKeys(home);
  BOOST_CHECK_EQUAL(keys.getPib().getIdentities().size(), 1U);
  BOOST_CHECK_NO_THROW(keys.getPib().getIdentity(ACTOR));
}

BOOST_AUTO_TEST_CASE(MissingPrivateKeyCannotPassSigningProof)
{
  IdentityHome home;
  {
    auto keys = openKeys(home);
    keys.createIdentity(ACTOR);
  }
  const auto keyDirectory = home.root / ".ndn/ndnsec-key-file";
  std::size_t removed = 0;
  for (const auto& entry : std::filesystem::directory_iterator(keyDirectory)) {
    if (entry.path().extension() == ".privkey") {
      BOOST_REQUIRE(std::filesystem::remove(entry.path()));
      ++removed;
    }
  }
  BOOST_REQUIRE_EQUAL(removed, 1U);
  BOOST_CHECK_EQUAL(home.check(ACTOR), 1);
  auto keys = openKeys(home);
  BOOST_CHECK_NO_THROW(keys.getPib().getIdentity(ACTOR));
}

BOOST_AUTO_TEST_CASE(WrongPrivateKeyCannotPassCertificateVerification)
{
  IdentityHome home;
  IdentityHome foreign;
  {
    auto keys = openKeys(home);
    keys.createIdentity(ACTOR);
    auto otherKeys = openKeys(foreign);
    otherKeys.createIdentity(ndn::Name("/spec217/foreign"));
  }
  std::filesystem::path originalKey;
  std::filesystem::path foreignKey;
  for (const auto& entry : std::filesystem::directory_iterator(home.root / ".ndn/ndnsec-key-file")) {
    if (entry.path().extension() == ".privkey")
      originalKey = entry.path();
  }
  for (const auto& entry : std::filesystem::directory_iterator(foreign.root / ".ndn/ndnsec-key-file")) {
    if (entry.path().extension() == ".privkey")
      foreignKey = entry.path();
  }
  BOOST_REQUIRE(!originalKey.empty());
  BOOST_REQUIRE(!foreignKey.empty());
  const auto permissions = std::filesystem::status(originalKey).permissions();
  std::filesystem::permissions(originalKey, permissions | std::filesystem::perms::owner_write);
  std::filesystem::copy_file(foreignKey, originalKey,
                             std::filesystem::copy_options::overwrite_existing);
  std::filesystem::permissions(originalKey, permissions);
  BOOST_CHECK_EQUAL(home.check(ACTOR), 1);
  auto keys = openKeys(home);
  BOOST_CHECK_NO_THROW(keys.getPib().getIdentity(ACTOR));
}

BOOST_AUTO_TEST_CASE(RelocatedAbsoluteTpmFailsWithoutResettingPib)
{
  IdentityHome source;
  IdentityHome destination;
  {
    auto keys = openKeys(source);
    keys.createIdentity(ACTOR);
  }
  std::filesystem::copy(source.root / ".ndn", destination.root / ".ndn",
                        std::filesystem::copy_options::recursive);
  BOOST_CHECK_EQUAL(destination.check(ACTOR), 1);
  ndn::KeyChain retained("pib-sqlite3:" + (destination.root / ".ndn").string(),
                         "tpm-file:" + (source.root / ".ndn").string(), false);
  BOOST_CHECK_NO_THROW(retained.getPib().getIdentity(ACTOR));
  BOOST_CHECK_EQUAL(retained.getPib().getIdentities().size(), 1U);
}

BOOST_AUTO_TEST_CASE(MissingPibDoesNotCreateCredentialStore)
{
  IdentityHome home;
  BOOST_CHECK_EQUAL(home.check(ACTOR), 1);
  BOOST_CHECK(!std::filesystem::exists(home.root / ".ndn"));
}

BOOST_AUTO_TEST_CASE(PortableTpmSurvivesCopiedHome)
{
  IdentityHome source;
  IdentityHome destination;
  {
    ndn_service_framework::test::ScopedEnvironmentValue home("HOME", source.root.c_str());
    ndn::KeyChain keys("pib-sqlite3:" + (source.root / ".ndn").string(), "tpm-file:", false);
    keys.createIdentity(ACTOR);
  }
  std::filesystem::copy(source.root / ".ndn", destination.root / ".ndn",
                        std::filesystem::copy_options::recursive);
  BOOST_CHECK_EQUAL(destination.check(ACTOR, true), 0);
  BOOST_CHECK_EQUAL(destination.check(ACTOR, true, true), 0);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::tests
