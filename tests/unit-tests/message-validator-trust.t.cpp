#include "tests/boost-test.hpp"
#include "ndn-service-framework/common.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/validity-period.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>
#include <ndn-cxx/util/io.hpp>
#include <ndn-cxx/util/time-custom-clock.hpp>
#include <ndn-cxx/util/time-unit-test-clock.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ndn_service_framework::test {
namespace {

class IsolatedPib
{
public:
  IsolatedPib()
  {
    if (const char* old = std::getenv("NDN_CLIENT_PIB")) m_previousPib = old;
    if (const char* old = std::getenv("NDN_CLIENT_TPM")) m_previousTpm = old;
    auto pattern = (std::filesystem::temp_directory_path() /
                    "ndnsf-message-trust-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const char* directory = ::mkdtemp(buffer.data());
    if (directory == nullptr) throw std::runtime_error("cannot make trust test directory");
    m_directory = directory;
    const auto pib = "pib-sqlite3:" + (m_directory / "pib").string();
    const auto tpm = "tpm-file:" + (m_directory / "tpm").string();
    if (::setenv("NDN_CLIENT_PIB", pib.c_str(), 1) != 0 ||
        ::setenv("NDN_CLIENT_TPM", tpm.c_str(), 1) != 0) {
      restore();
      std::filesystem::remove_all(m_directory);
      throw std::runtime_error("cannot isolate trust test keychain");
    }
  }

  ~IsolatedPib()
  {
    restore();
    std::error_code ignored;
    std::filesystem::remove_all(m_directory, ignored);
  }

  const std::filesystem::path& directory() const { return m_directory; }

private:
  void restore() const
  {
    if (m_previousPib) ::setenv("NDN_CLIENT_PIB", m_previousPib->c_str(), 1);
    else ::unsetenv("NDN_CLIENT_PIB");
    if (m_previousTpm) ::setenv("NDN_CLIENT_TPM", m_previousTpm->c_str(), 1);
    else ::unsetenv("NDN_CLIENT_TPM");
  }

  std::filesystem::path m_directory;
  std::optional<std::string> m_previousPib;
  std::optional<std::string> m_previousTpm;
};

struct Chain
{
  ndn::security::Certificate root;
  ndn::security::Certificate ca;
  ndn::security::Certificate actor;
};

class ScopedSystemClock
{
public:
  ScopedSystemClock()
    : clock(std::make_shared<ndn::time::UnitTestSystemClock>())
  {
    const auto now = ndn::time::system_clock::now().time_since_epoch();
    clock->setNow(ndn::time::duration_cast<ndn::time::nanoseconds>(now));
    ndn::time::setCustomClocks(nullptr, clock);
  }

  ~ScopedSystemClock() { ndn::time::setCustomClocks(); }

  std::shared_ptr<ndn::time::UnitTestSystemClock> clock;
};

constexpr const char* ROOT = "/trust-fixture";
constexpr const char* CA = "/trust-fixture/ca";
constexpr const char* ACTOR = "/trust-fixture/ca/actor";

Chain
makeChain(ndn::KeyChain& keys,
          const std::optional<ndn::security::ValidityPeriod>& actorValidity = std::nullopt,
          const std::optional<ndn::security::ValidityPeriod>& caValidity = std::nullopt,
          const std::optional<ndn::security::ValidityPeriod>& rootValidity = std::nullopt)
{
  const auto root = keys.createIdentity(ndn::Name(ROOT), ndn::RsaKeyParams(2048));
  const auto ca = keys.createIdentity(ndn::Name(CA), ndn::RsaKeyParams(2048));
  const auto actor = keys.createIdentity(ndn::Name(ACTOR), ndn::RsaKeyParams(2048));
  auto rootCert = root.getDefaultKey().getDefaultCertificate();
  if (rootValidity) {
    ndn::security::MakeCertificateOptions rootOptions;
    rootOptions.validity = rootValidity;
    rootOptions.issuerId = ndn::name::Component("pinned");
    rootCert = keys.makeCertificate(root.getDefaultKey(),
      ndn::security::signingByIdentity(root.getName()), rootOptions);
    keys.addCertificate(root.getDefaultKey(), rootCert);
    keys.setDefaultCertificate(root.getDefaultKey(), rootCert);
  }
  ndn::security::MakeCertificateOptions caOptions;
  caOptions.validity = caValidity;
  caOptions.issuerId = ndn::name::Component("root-issued");
  const auto caCert = keys.makeCertificate(ca.getDefaultKey(),
    ndn::security::signingByCertificate(rootCert), caOptions);
  keys.addCertificate(ca.getDefaultKey(), caCert);
  keys.setDefaultCertificate(ca.getDefaultKey(), caCert);
  ndn::security::MakeCertificateOptions options;
  options.validity = actorValidity;
  options.issuerId = ndn::name::Component("ca-issued");
  const auto actorCert = keys.makeCertificate(actor.getDefaultKey(),
    ndn::security::signingByCertificate(caCert), options);
  keys.addCertificate(actor.getDefaultKey(), actorCert);
  keys.setDefaultCertificate(actor.getDefaultKey(), actorCert);
  return {rootCert, caCert, actorCert};
}

std::filesystem::path
writeSchema(const IsolatedPib& isolation, const ndn::security::Certificate& root)
{
  const auto rootPath = isolation.directory() / "root.cert";
  const auto schemaPath = isolation.directory() / "trust.conf";
  ndn::io::save(root, rootPath.string());
  std::ofstream schema(schemaPath);
  schema << R"SCHEMA(rule
{
  id "certificate chain"
  for data
  filter { type name regex ^<>+<KEY|8=KEY><><><>$ }
  checker { type hierarchical sig-type rsa-sha256 }
}
rule
{
  id "application data"
  for data
  filter { type name
    regex ^<trust-fixture|8=trust-fixture><ca|8=ca><actor|8=actor><NDNSF|8=NDNSF><>*$ }
  checker
  {
    type customized
    sig-type rsa-sha256
    key-locator
    {
      type name
      regex "^<trust-fixture|8=trust-fixture><ca|8=ca><actor|8=actor><KEY|8=KEY><>{1,3}$"
    }
  }
}
trust-anchor { type file file-name ")SCHEMA" << rootPath.string() << R"SCHEMA(" }
)SCHEMA";
  schema.close();
  if (!schema) throw std::runtime_error("cannot write trust test schema");
  return schemaPath;
}

ndn::Data
makeData(ndn::KeyChain& keys, const ndn::security::Certificate& actor,
         const ndn::Name& name = ndn::Name(ACTOR).append("NDNSF").append("STATUS"))
{
  ndn::Data data(name);
  data.setContent("trust-chain-payload");
  keys.sign(data, ndn::security::signingByCertificate(actor));
  return data;
}

struct ValidationResult
{
  size_t success = 0;
  size_t failure = 0;
  size_t certificateInterests = 0;
};

template<typename Validator>
ValidationResult
validate(ndn::DummyClientFace& face, Validator& validator, const ndn::Data& data,
         const std::vector<ndn::security::Certificate>& certificates)
{
  ValidationResult result;
  size_t offered = face.sentInterests.size();
  validator.validate(data,
    [&](const ndn::Data&) { ++result.success; },
    [&](const ndn::Data&, const ndn::security::ValidationError& error) {
      ++result.failure;
      BOOST_TEST_MESSAGE("Validation rejected: " << error);
    });
  for (int attempt = 0; attempt < 250 && result.success + result.failure == 0; ++attempt) {
    face.processEvents(ndn::time::milliseconds(2));
    while (offered < face.sentInterests.size()) {
      const auto interest = face.sentInterests[offered++];
      for (const auto& cert : certificates) {
        if (interest.matchesData(cert)) {
          ++result.certificateInterests;
          face.receive(cert);
          break;
        }
      }
    }
  }
  BOOST_REQUIRE_MESSAGE(result.success + result.failure == 1,
                        "ordinary Data validation did not terminate");
  return result;
}

struct Fixture
{
  static IsolatedPib&
  getIsolation()
  {
    // ndn-cxx pins the default PIB/TPM locators on their first use in a process.
    static IsolatedPib isolation;
    return isolation;
  }

  Fixture()
  {
    clearIdentities();
  }

  ~Fixture()
  {
    clearIdentities();
  }

  void
  clearIdentities()
  {
    for (const auto* name : {ACTOR, CA, ROOT}) {
      try {
        keys.deleteIdentity(keys.getPib().getIdentity(ndn::Name(name)));
      }
      catch (const ndn::security::pib::Pib::Error&) {
        // The first fixture has no identities to remove.
      }
    }
  }

  IsolatedPib& isolation = getIsolation();
  ndn::KeyChain keys;
  boost::asio::io_context io;
  ndn::DummyClientFace face{io, keys};
};

} // namespace

BOOST_AUTO_TEST_SUITE(MessageValidatorFixedRoot)

BOOST_AUTO_TEST_CASE(SvsGroupAcceptsRootSignedActorsButRejectsOtherRoles)
{
  Fixture fixture;
  auto& keys = fixture.keys;
  const auto root = keys.createIdentity(ndn::Name("/example/hello"), ndn::RsaKeyParams(2048));
  const auto user = keys.createIdentity(ndn::Name("/example/hello/user"), ndn::RsaKeyParams(2048));
  const auto provider = keys.createIdentity(ndn::Name("/example/hello/provider/A"),
                                            ndn::RsaKeyParams(2048));
  const auto other = keys.createIdentity(ndn::Name("/example/hello/other"),
                                         ndn::RsaKeyParams(2048));
  const auto rootCert = root.getDefaultKey().getDefaultCertificate();
  const auto signActor = [&](const ndn::security::pib::Identity& identity) {
    auto cert = keys.makeCertificate(identity.getDefaultKey(),
                                    ndn::security::signingByCertificate(rootCert));
    keys.addCertificate(identity.getDefaultKey(), cert);
    keys.setDefaultCertificate(identity.getDefaultKey(), cert);
    return cert;
  };
  const auto userCert = signActor(user);
  const auto providerCert = signActor(provider);
  const auto otherCert = signActor(other);

  const auto schema = fixture.isolation.directory() / "svs-trust.conf";
  const auto source = std::filesystem::read_symlink("/proc/self/exe").parent_path()
                        .parent_path() / "examples/trust-schema.conf";
  std::filesystem::copy_file(source, schema);
  ndn::io::save(rootCert, (fixture.isolation.directory() / "root.cert").string());
  auto validator = std::make_shared<MessageValidator>(schema.string(), std::nullopt,
                                                     &fixture.face);
  const auto makeGroupData = [&](const ndn::security::Certificate& cert) {
    ndn::Data data("/example/hello/group/v=3");
    data.setContent("sync");
    keys.sign(data, ndn::security::signingByCertificate(cert));
    return data;
  };
  BOOST_CHECK_EQUAL(validate(fixture.face, *validator, makeGroupData(userCert),
                             {userCert, rootCert}).success, 1U);
  BOOST_CHECK_EQUAL(validate(fixture.face, *validator, makeGroupData(providerCert),
                             {providerCert, rootCert}).success, 1U);
  const auto rejected = validate(fixture.face, *validator, makeGroupData(otherCert),
                                 {otherCert, rootCert});
  BOOST_CHECK_EQUAL(rejected.success, 0U);
  BOOST_CHECK_EQUAL(rejected.failure, 1U);
}

BOOST_AUTO_TEST_CASE(ValidChainAndRepeatedDataUseTheConfiguredRoot)
{
  Fixture fixture;
  const auto chain = makeChain(fixture.keys);
  const auto schema = writeSchema(fixture.isolation, chain.root);
  auto validator = std::make_shared<MessageValidator>(schema.string(), std::nullopt,
                                                     &fixture.face);
  const auto packet = makeData(fixture.keys, chain.actor);
  for (int i = 0; i < 2; ++i) {
    const auto result = validate(fixture.face, *validator, packet,
                                 {chain.actor, chain.ca, chain.root});
    BOOST_CHECK_EQUAL(result.success, 1U);
    BOOST_CHECK_EQUAL(result.failure, 0U);
  }
}

BOOST_AUTO_TEST_CASE(UnknownRootInLocalPibIsRejected)
{
  Fixture fixture;
  ndn::KeyChain pinned("pib-memory:pinned-trust-root", "tpm-memory:pinned-trust-root");
  const auto trusted = makeChain(pinned);
  const auto foreign = makeChain(fixture.keys);
  const auto schema = writeSchema(fixture.isolation, trusted.root);
  auto validator = std::make_shared<MessageValidator>(schema.string(), std::nullopt,
                                                     &fixture.face);
  const auto result = validate(fixture.face, *validator,
                               makeData(fixture.keys, foreign.actor),
                               {foreign.actor, foreign.ca, foreign.root});
  BOOST_CHECK_EQUAL(result.success, 0U);
  BOOST_CHECK_EQUAL(result.failure, 1U);
}

BOOST_AUTO_TEST_CASE(ExpiredAndFutureActorCertificatesInLocalPibAreRejected)
{
  for (bool future : {false, true}) {
    Fixture fixture;
    const auto now = ndn::time::system_clock::now();
    const auto period = future
      ? ndn::security::ValidityPeriod(now + ndn::time::hours(1),
                                      now + ndn::time::hours(2))
      : ndn::security::ValidityPeriod(now - ndn::time::hours(2),
                                      now - ndn::time::hours(1));
    const auto chain = makeChain(fixture.keys, period);
    const auto schema = writeSchema(fixture.isolation, chain.root);
    auto validator = std::make_shared<MessageValidator>(schema.string(), std::nullopt,
                                                       &fixture.face);
    const auto result = validate(fixture.face, *validator,
                                 makeData(fixture.keys, chain.actor),
                                 {chain.actor, chain.ca, chain.root});
    BOOST_CHECK_EQUAL(result.success, 0U);
    BOOST_CHECK_EQUAL(result.failure, 1U);
  }
}

BOOST_AUTO_TEST_CASE(ExpiredIntermediateCertificateInLocalPibIsRejected)
{
  Fixture fixture;
  const auto now = ndn::time::system_clock::now();
  const ndn::security::ValidityPeriod expired(now - ndn::time::hours(2),
                                               now - ndn::time::hours(1));
  const auto chain = makeChain(fixture.keys, std::nullopt, expired);
  const auto schema = writeSchema(fixture.isolation, chain.root);
  auto validator = std::make_shared<MessageValidator>(schema.string(), std::nullopt,
                                                     &fixture.face);
  const auto result = validate(fixture.face, *validator,
                               makeData(fixture.keys, chain.actor),
                               {chain.actor, chain.ca, chain.root});
  BOOST_CHECK_EQUAL(result.success, 0U);
  BOOST_CHECK_EQUAL(result.failure, 1U);
}

BOOST_AUTO_TEST_CASE(CachedDataStopsValidatingAfterIntermediateExpires)
{
  ScopedSystemClock testClock;
  Fixture fixture;
  const auto now = ndn::time::system_clock::now();
  const ndn::security::ValidityPeriod caPeriod(now - ndn::time::hours(1),
                                                now + ndn::time::hours(1));
  const auto chain = makeChain(fixture.keys, std::nullopt, caPeriod);
  const auto schema = writeSchema(fixture.isolation, chain.root);
  auto validator = std::make_shared<MessageValidator>(schema.string(), std::nullopt,
                                                     &fixture.face);
  const auto packet = makeData(fixture.keys, chain.actor);
  const std::vector<ndn::security::Certificate> certificates{
    chain.actor, chain.ca, chain.root};
  const auto first = validate(fixture.face, *validator, packet, certificates);
  BOOST_CHECK_EQUAL(first.success, 1U);
  BOOST_CHECK_EQUAL(first.failure, 0U);

  testClock.clock->setNow(ndn::time::duration_cast<ndn::time::nanoseconds>(
    (now + ndn::time::hours(2)).time_since_epoch()));
  BOOST_CHECK(chain.actor.getValidityPeriod().isValid());
  BOOST_CHECK(!chain.ca.getValidityPeriod().isValid());
  const auto afterExpiry = validate(fixture.face, *validator, packet, certificates);
  BOOST_CHECK_EQUAL(afterExpiry.success, 0U);
  BOOST_CHECK_EQUAL(afterExpiry.failure, 1U);
}

BOOST_AUTO_TEST_CASE(CachedDataStopsValidatingAfterRootExpires)
{
  ScopedSystemClock testClock;
  Fixture fixture;
  const auto now = ndn::time::system_clock::now();
  const ndn::security::ValidityPeriod rootPeriod(now - ndn::time::hours(1),
                                                  now + ndn::time::hours(1));
  const auto chain = makeChain(fixture.keys, std::nullopt, std::nullopt, rootPeriod);
  const auto schema = writeSchema(fixture.isolation, chain.root);
  auto validator = std::make_shared<MessageValidator>(schema.string(), std::nullopt,
                                                     &fixture.face);
  const auto packet = makeData(fixture.keys, chain.actor);
  const std::vector<ndn::security::Certificate> certificates{
    chain.actor, chain.ca, chain.root};
  const auto first = validate(fixture.face, *validator, packet, certificates);
  BOOST_CHECK_EQUAL(first.success, 1U);
  BOOST_CHECK_EQUAL(first.failure, 0U);

  testClock.clock->setNow(ndn::time::duration_cast<ndn::time::nanoseconds>(
    (now + ndn::time::hours(2)).time_since_epoch()));
  BOOST_CHECK(chain.actor.getValidityPeriod().isValid());
  BOOST_CHECK(chain.ca.getValidityPeriod().isValid());
  BOOST_CHECK(!chain.root.getValidityPeriod().isValid());
  const auto afterExpiry = validate(fixture.face, *validator, packet, certificates);
  BOOST_CHECK_EQUAL(afterExpiry.success, 0U);
  BOOST_CHECK_EQUAL(afterExpiry.failure, 1U);
}

BOOST_AUTO_TEST_CASE(PinnedRootValidityStillRejectsClockRollback)
{
  ScopedSystemClock testClock;
  Fixture fixture;
  const auto now = ndn::time::system_clock::now();
  const ndn::security::ValidityPeriod broad(now - ndn::time::hours(24),
                                           now + ndn::time::hours(24));
  const ndn::security::ValidityPeriod narrow(now - ndn::time::hours(1),
                                            now + ndn::time::hours(1));
  const auto chain = makeChain(fixture.keys, broad, broad, narrow);
  const auto schema = writeSchema(fixture.isolation, chain.root);
  auto validator = std::make_shared<MessageValidator>(schema.string(), std::nullopt,
                                                     &fixture.face);
  const auto packet = makeData(fixture.keys, chain.actor);
  const std::vector<ndn::security::Certificate> certificates{chain.actor, chain.ca, chain.root};
  BOOST_CHECK_EQUAL(validate(fixture.face, *validator, packet, certificates).success, 1U);
  testClock.clock->setNow(ndn::time::duration_cast<ndn::time::nanoseconds>(
    (now - ndn::time::hours(2)).time_since_epoch()));
  BOOST_REQUIRE(chain.actor.isValid());
  BOOST_REQUIRE(chain.ca.isValid());
  BOOST_REQUIRE(!chain.root.isValid());
  const auto rejected = validate(fixture.face, *validator, packet, certificates);
  BOOST_CHECK_EQUAL(rejected.success, 0U);
  BOOST_CHECK_EQUAL(rejected.failure, 1U);
}

BOOST_AUTO_TEST_CASE(ExplicitReloadReplacesPinnedRootValidity)
{
  ScopedSystemClock testClock;
  Fixture fixture;
  const auto now = ndn::time::system_clock::now();
  const ndn::security::ValidityPeriod broad(now - ndn::time::hours(24),
                                           now + ndn::time::hours(24));
  const auto chain = makeChain(fixture.keys, broad, broad,
    ndn::security::ValidityPeriod(now - ndn::time::hours(1), now + ndn::time::hours(1)));
  const auto schema = writeSchema(fixture.isolation, chain.root);
  TrustSchemaValidator validator(fixture.face);
  validator.load(schema.string());
  const auto packet = makeData(fixture.keys, chain.actor);
  const std::vector<ndn::security::Certificate> certificates{chain.actor, chain.ca, chain.root};
  BOOST_CHECK_EQUAL(validate(fixture.face, validator, packet, certificates).success, 1U);
  testClock.clock->setNow(ndn::time::duration_cast<ndn::time::nanoseconds>(
    (now + ndn::time::hours(2)).time_since_epoch()));
  const auto replacement = makeChain(fixture.keys, broad, broad, broad);
  writeSchema(fixture.isolation, replacement.root);
  const auto renewedPacket = makeData(fixture.keys, replacement.actor);
  const std::vector<ndn::security::Certificate> renewedCertificates{
    replacement.actor, replacement.ca, replacement.root};
  // Replacing the file cannot silently replace the already pinned anchor.
  BOOST_CHECK_EQUAL(validate(fixture.face, validator, packet, certificates).failure, 1U);
  validator.load(schema.string());
  const auto accepted = validate(fixture.face, validator, renewedPacket, renewedCertificates);
  BOOST_CHECK_EQUAL(accepted.success, 1U);
  BOOST_CHECK_EQUAL(accepted.failure, 0U);
}

BOOST_AUTO_TEST_CASE(WrongApplicationNamespaceAndAlteredSignatureAreRejected)
{
  Fixture fixture;
  const auto chain = makeChain(fixture.keys);
  const auto schema = writeSchema(fixture.isolation, chain.root);
  auto validator = std::make_shared<MessageValidator>(schema.string(), std::nullopt,
                                                     &fixture.face);
  const auto wrongName = makeData(fixture.keys, chain.actor,
                                  ndn::Name(ACTOR).append("private").append("STATUS"));
  auto result = validate(fixture.face, *validator, wrongName,
                         {chain.actor, chain.ca, chain.root});
  BOOST_CHECK_EQUAL(result.success, 0U);
  BOOST_CHECK_EQUAL(result.failure, 1U);

  auto altered = makeData(fixture.keys, chain.actor);
  const auto signature = altered.getSignatureValue();
  ndn::Buffer bytes(signature.value(), signature.value() + signature.value_size());
  BOOST_REQUIRE(!bytes.empty());
  bytes[0] ^= 1;
  altered.setSignatureValue(ndn::span<const uint8_t>(bytes.data(), bytes.size()));
  result = validate(fixture.face, *validator, altered,
                    {chain.actor, chain.ca, chain.root});
  BOOST_CHECK_EQUAL(result.success, 0U);
  BOOST_CHECK_EQUAL(result.failure, 1U);
}

BOOST_AUTO_TEST_CASE(NetworkCertificateRetrievalCompletesTheChain)
{
  Fixture fixture;
  ndn::KeyChain remote("pib-memory:remote-trust-chain", "tpm-memory:remote-trust-chain");
  const auto chain = makeChain(remote);
  const auto schema = writeSchema(fixture.isolation, chain.root);
  auto validator = std::make_shared<MessageValidator>(schema.string(), std::nullopt,
                                                     &fixture.face);
  const auto result = validate(fixture.face, *validator, makeData(remote, chain.actor),
                               {chain.actor, chain.ca, chain.root});
  BOOST_CHECK_EQUAL(result.success, 1U);
  BOOST_CHECK_EQUAL(result.failure, 0U);
  BOOST_CHECK_GE(result.certificateInterests, 2U);
}

BOOST_AUTO_TEST_CASE(PermissiveAnchorAndExpiredRootFailAtStartup)
{
  Fixture fixture;
  const auto anyPath = fixture.isolation.directory() / "any.conf";
  {
    std::ofstream any(anyPath);
    any << "rule { id all for data checker { type hierarchical sig-type rsa-sha256 } }\n"
           "trust-anchor { type any }\n";
  }
  BOOST_CHECK_THROW(std::make_shared<MessageValidator>(anyPath.string(), std::nullopt,
                                                       &fixture.face), std::exception);

  const auto root = fixture.keys.createIdentity(ndn::Name(ROOT), ndn::RsaKeyParams(2048));
  ndn::security::MakeCertificateOptions options;
  const auto now = ndn::time::system_clock::now();
  options.validity = ndn::security::ValidityPeriod(now - ndn::time::hours(2),
                                                   now - ndn::time::hours(1));
  const auto expired = fixture.keys.makeCertificate(root.getDefaultKey(),
    ndn::security::signingByIdentity(root.getName()), options);
  const auto schema = writeSchema(fixture.isolation, expired);
  BOOST_CHECK_THROW(std::make_shared<MessageValidator>(schema.string(), std::nullopt,
                                                       &fixture.face), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
