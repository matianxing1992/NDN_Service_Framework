/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil -*- */
/**
 * Provider owner-token lease balance, stop admission and late-callback rules.
 *
 * Each dispatch acquires exactly one owner token and returns it exactly once.
 * A shutdown path stops admission first so queued late callbacks observe a
 * refused acquire instead of extending an unbounded drain.
 */
#include "tests/boost-test.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>
#include <ndn-cxx/util/io.hpp>

#include <filesystem>
#include <fstream>

using namespace ndn_service_framework;
using namespace ndn_service_framework::test;

namespace ndn_service_framework {

/** Friend of ServiceProvider used only to exercise the owner-token primitive. */
struct ServiceProviderTestAccess
{
  static ServiceProvider*
  acquire(ServiceProvider& provider)
  {
    return provider.m_registrationControl->acquireOwner();
  }

  static void
  release(ServiceProvider& provider)
  {
    provider.m_registrationControl->releaseOwner();
  }

  static bool
  pending(const ServiceProvider& provider)
  {
    return provider.m_registrationControl->activeOwnerCalls != 0;
  }
};

} // namespace ndn_service_framework

namespace {

std::string
writeAnchorTrustSchema(const ndn::security::Certificate& anchor)
{
  const auto dir = std::filesystem::temp_directory_path() /
                   ("stream-lease-trust-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  const auto certPath = (dir / "anchor.cert").string();
  ndn::io::save(anchor, certPath);
  const auto schemaPath = (dir / "trust-schema.conf").string();
  std::ofstream out(schemaPath);
  out << "rule { id \"stream lease fixture\" for data checker { type hierarchical "
         "sig-type rsa-sha256 } }\n"
      << "trust-anchor { type file file-name \"" << certPath << "\" }\n";
  return schemaPath;
}

struct LeaseFixture
{
  ndn::security::KeyChain keyChain{"pib-memory:stream-lease", "tpm-memory:stream-lease"};
  ndn::DummyClientFace face{keyChain};
  ndn::security::Certificate providerCert{makeRsaIdentity(keyChain,
                                                          ndn::Name("/spec194/provider/a"))};
  ndn::security::Certificate aaCert{makeRsaIdentity(keyChain, ndn::Name("/spec194/aa"))};

  std::unique_ptr<LocalServiceProvider>
  provider()
  {
    return std::make_unique<LocalServiceProvider>(
      face, ndn::Name("/spec194/group"), providerCert, aaCert,
      writeAnchorTrustSchema(providerCert));
  }
};

} // namespace

BOOST_AUTO_TEST_SUITE(StreamOwnerLease)

BOOST_AUTO_TEST_CASE(ProviderOwnerTokensBalanceAcrossConstructionAndClose)
{
  LeaseFixture fixture;
  auto provider = fixture.provider();
  BOOST_CHECK(!provider->hasPendingOwnerTokens());
  BOOST_CHECK(!ServiceProviderTestAccess::pending(*provider));
}

BOOST_AUTO_TEST_CASE(EachAcquireReturnsExactlyOneToken)
{
  LeaseFixture fixture;
  auto provider = fixture.provider();
  auto* first = ServiceProviderTestAccess::acquire(*provider);
  BOOST_REQUIRE(first != nullptr);
  BOOST_CHECK(ServiceProviderTestAccess::pending(*provider));
  ServiceProviderTestAccess::release(*provider);
  BOOST_CHECK(!ServiceProviderTestAccess::pending(*provider));

  auto* second = ServiceProviderTestAccess::acquire(*provider);
  auto* third = ServiceProviderTestAccess::acquire(*provider);
  BOOST_REQUIRE(second != nullptr);
  BOOST_REQUIRE(third != nullptr);
  ServiceProviderTestAccess::release(*provider);
  BOOST_CHECK(ServiceProviderTestAccess::pending(*provider));
  ServiceProviderTestAccess::release(*provider);
  BOOST_CHECK(!ServiceProviderTestAccess::pending(*provider));
}

BOOST_AUTO_TEST_CASE(StoppedAdmissionRefusesLateCallbacks)
{
  LeaseFixture fixture;
  auto provider = fixture.provider();
  // Hold one lease while admission stops: the pending token must still be
  // returned afterwards, and new acquisitions must be refused.
  auto* held = ServiceProviderTestAccess::acquire(*provider);
  BOOST_REQUIRE(held != nullptr);
  provider->stopOwnerAdmission();
  BOOST_CHECK(ServiceProviderTestAccess::acquire(*provider) == nullptr);
  BOOST_CHECK(ServiceProviderTestAccess::pending(*provider));
  ServiceProviderTestAccess::release(*provider);
  BOOST_CHECK(!ServiceProviderTestAccess::pending(*provider));
  // Repeated stop calls are idempotent.
  provider->stopOwnerAdmission();
  BOOST_CHECK(ServiceProviderTestAccess::acquire(*provider) == nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
