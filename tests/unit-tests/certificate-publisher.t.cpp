#include "tests/boost-test.hpp"

#include "ndn-service-framework/CertificatePublisher.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>

#include <boost/asio/io_context.hpp>

namespace ndn_service_framework::test {
namespace {

ndn::security::Certificate
makeRsaIdentity(ndn::security::KeyChain& keyChain, const ndn::Name& identity)
{
  auto id = keyChain.createIdentity(identity, ndn::RsaKeyParams(2048));
  return id.getDefaultKey().getDefaultCertificate();
}

void
pumpFaces(ndn::DummyClientFace& a, ndn::DummyClientFace& b, const std::function<bool()>& done)
{
  for (int i = 0; i < 200 && !done(); ++i) {
    a.processEvents(ndn::time::milliseconds(5));
    b.processEvents(ndn::time::milliseconds(5));
    a.getIoContext().restart();
    b.getIoContext().restart();
  }
}

void
checkCertificateFetch(const ndn::Name& identity)
{
  ndn::security::KeyChain keyChain("pib-memory:cert-publisher-" + identity.toUri(),
                                   "tpm-memory:cert-publisher-" + identity.toUri());
  auto cert = makeRsaIdentity(keyChain, identity);

  ndn::DummyClientFace::Options options;
  options.enableRegistrationReply = true;
  ndn::DummyClientFace publisherFace(keyChain, options);
  ndn::DummyClientFace consumerFace(keyChain, options);
  publisherFace.linkTo(consumerFace);

  CertificatePublisher publisher(publisherFace, keyChain, cert.getName());

  bool received = false;
  ndn::Name receivedName;
  consumerFace.expressInterest(
    ndn::Interest(cert.getName()).setCanBePrefix(false).setInterestLifetime(ndn::time::seconds(1)),
    [&] (const ndn::Interest&, const ndn::Data& data) {
      received = true;
      receivedName = data.getName();
    },
    [] (const ndn::Interest&, const ndn::lp::Nack&) {
      BOOST_FAIL("certificate Interest was nacked");
    },
    [] (const ndn::Interest&) {
      BOOST_FAIL("certificate Interest timed out");
    });

  pumpFaces(publisherFace, consumerFace, [&] { return received; });

  BOOST_REQUIRE(received);
  BOOST_CHECK_EQUAL(receivedName.toUri(), cert.getName().toUri());
  BOOST_CHECK_EQUAL(publisher.getCertificateName().toUri(), cert.getName().toUri());
  BOOST_CHECK_EQUAL(publisher.getRegisteredPrefix().toUri(), cert.getKeyName().toUri());
}

} // namespace

BOOST_AUTO_TEST_SUITE(CertificatePublisher)

BOOST_AUTO_TEST_CASE(ProviderCertificateCanBeFetchedByExactName)
{
  checkCertificateFetch(ndn::Name("/test/ndnsf/provider"));
}

BOOST_AUTO_TEST_CASE(UserCertificateCanBeFetchedByExactName)
{
  checkCertificateFetch(ndn::Name("/test/ndnsf/user"));
}

BOOST_AUTO_TEST_CASE(ControllerOrAaCanFetchRemoteCertificates)
{
  checkCertificateFetch(ndn::Name("/test/ndnsf/provider/for-aa"));
  checkCertificateFetch(ndn::Name("/test/ndnsf/user/for-controller"));
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
