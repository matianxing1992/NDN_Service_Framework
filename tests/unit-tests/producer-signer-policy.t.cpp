#include "ndn-service-framework/ProducerSignerPolicy.hpp"
#include <boost/test/unit_test.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <filesystem>
#include <fstream>
#include <cstdlib>

namespace {
using ndn_service_framework::ProducerSignerPolicy;

struct PermissionFixture
{
  std::filesystem::path directory;
  PermissionFixture()
  {
    std::string pattern = (std::filesystem::temp_directory_path() /
                           "spec201-producer-signers-XXXXXX").string();
    const auto path = ::mkdtemp(pattern.data());
    if (!path) throw std::runtime_error("cannot create signer permission fixture");
    directory = path;
  }
  ~PermissionFixture() { std::filesystem::remove_all(directory); }
  std::string write(const std::string& text)
  {
    const auto path = directory / "permissions.info";
    std::ofstream output(path);
    output << text;
    output.close();
    if (!output) throw std::runtime_error("cannot write signer permission fixture");
    return path.string();
  }
};

BOOST_AUTO_TEST_SUITE(Spec201ProducerSignerPolicy)

BOOST_FIXTURE_TEST_CASE(OwnerDelegateAndForeignSigner, PermissionFixture)
{
  ndn::Name producer("/org/NDNSF-DI/owner");
  producer.appendVersion(257);
  const ndn::Name delegate("/org/delegate"), foreign("/org/foreign");
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  for (const auto& identity : {producer, delegate, foreign}) keys.createIdentity(identity);
  auto packet = [&](const ndn::Name& signer) {
    ndn::Data data(ndn::Name(producer).append("NDNSF-DI").append("model"));
    const std::string payload = "immutable payload";
    data.setContent(ndn::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()));
    keys.sign(data, ndn::security::signingByIdentity(signer));
    BOOST_REQUIRE(ndn::security::verifySignature(data, keys.getPib().getIdentity(signer).getDefaultKey()));
    return data;
  };
  const auto ownerPacket = packet(producer), delegatePacket = packet(delegate), foreignPacket = packet(foreign);
  ProducerSignerPolicy ownerOnly;
  BOOST_CHECK(ownerOnly.allowsValidatedSigner(ownerPacket, producer));
  BOOST_CHECK(!ownerOnly.allowsValidatedSigner(delegatePacket, producer));
  const auto path = write("producer-signer\n{\n producer \"" +
    producer.toUri(ndn::name::UriFormat::CANONICAL) + "\"\n signer \"/org/delegate\"\n}\n");
  ProducerSignerPolicy delegated(path);
  BOOST_CHECK(delegated.allowsValidatedSigner(ownerPacket, producer));
  BOOST_CHECK(delegated.allowsValidatedSigner(delegatePacket, producer));
  BOOST_CHECK(!delegated.allowsValidatedSigner(foreignPacket, producer));
  BOOST_CHECK(!delegated.allowsValidatedSigner(delegatePacket, ndn::Name("/org")));
  auto differentNamespace = delegatePacket;
  differentNamespace.setName(ndn::Name("/another/NDNSF-DI/model"));
  keys.sign(differentNamespace, ndn::security::signingByIdentity(delegate));
  BOOST_REQUIRE(ndn::security::verifySignature(
    differentNamespace, keys.getPib().getIdentity(delegate).getDefaultKey()));
  BOOST_CHECK(!delegated.allowsValidatedSigner(differentNamespace, producer));
  BOOST_CHECK(!delegated.allowsValidatedSigner(delegatePacket, ndn::Name{}));
}

BOOST_FIXTURE_TEST_CASE(InvalidPermissionFilesFailClosed, PermissionFixture)
{
  for (const auto& text : {
      "unknown {}", "producer-signer { producer /p }",
      "producer-signer { producer / signer /d }",
      "producer-signer { producer p signer /d }",
      "producer-signer { producer /p signer /d extra true }",
      "producer-signer { producer /p producer /p signer /d }",
      "producer-signer { producer /p signer /d } producer-signer { producer /p signer /d }"}) {
    const auto path = write(text);
    BOOST_CHECK_THROW(ProducerSignerPolicy{path}, std::exception);
  }
  BOOST_CHECK_THROW(ProducerSignerPolicy{write(std::string(65537, 'x'))}, std::exception);
  BOOST_CHECK_THROW(ProducerSignerPolicy{directory.string()}, std::exception);
  const auto dangling = directory / "dangling";
  std::filesystem::create_symlink(directory / "absent", dangling);
  BOOST_CHECK_THROW(ProducerSignerPolicy{dangling.string()}, std::exception);
  BOOST_CHECK_NO_THROW(ProducerSignerPolicy{(directory / "absent").string()});
}

BOOST_AUTO_TEST_CASE(UnsignedPacketIsRejected)
{
  ProducerSignerPolicy policy;
  const ndn::Name producer("/producer");
  ndn::Data data(ndn::Name(producer).append("NDNSF-DI").append("model"));
  BOOST_CHECK(!policy.allowsValidatedSigner(data, producer));
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace
