/* -*- Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2012-2021 University of California, Los Angeles
 *
 * This file is part of ndn-svs, synchronization library for distributed realtime
 * applications for NDN.
 *
 * ndn-svs library is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free Software
 * Foundation, in version 2.1 of the License.
 *
 * ndn-svs library is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU Lesser General Public License for more details.
 */


#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/transform/public-key.hpp>

#include "ndn-service-framework/common.hpp"
#include "tests/boost-test.hpp"

namespace ndn_service_framework {
namespace test {

struct TestCoreFixture
{
  TestCoreFixture()
  {
  }

  ndn::Face m_face;

};

BOOST_FIXTURE_TEST_SUITE(Core, TestCoreFixture)

BOOST_AUTO_TEST_CASE(EcdsaPreferredSigningInfoFallsBackToRsa)
{
  ndn::KeyChain keyChain("pib-memory:ndnsf-signing-selection",
                         "tpm-memory:ndnsf-signing-selection");
  const ndn::Name dualIdentity("/test/ndnsf/signing/dual");
  const ndn::Name rsaOnlyIdentity("/test/ndnsf/signing/rsa-only");

  auto dual = keyChain.createIdentity(dualIdentity, ndn::RsaKeyParams(2048));
  auto dualRsa = dual.getDefaultKey().getDefaultCertificate();
  auto dualEc = keyChain.createKey(dual, ndn::EcKeyParams()).getDefaultCertificate();
  auto rsaOnly = keyChain.createIdentity(rsaOnlyIdentity, ndn::RsaKeyParams(2048))
                   .getDefaultKey()
                   .getDefaultCertificate();

  const auto selectedDual = getEcdsaSigningCertificateOrFallback(keyChain, dualRsa);
  BOOST_CHECK_EQUAL(selectedDual.getName(), dualEc.getName());
  BOOST_CHECK_EQUAL(getCertificateKeyType(selectedDual), ndn::KeyType::EC);
  const auto encryptionDualFromEc = getRsaEncryptionCertificateOrThrow(keyChain, dualEc);
  BOOST_CHECK_EQUAL(encryptionDualFromEc.getName(), dualRsa.getName());
  BOOST_CHECK_EQUAL(getCertificateKeyType(encryptionDualFromEc), ndn::KeyType::RSA);

  const auto selectedRsaOnly = getEcdsaSigningCertificateOrFallback(keyChain, rsaOnly);
  BOOST_CHECK_EQUAL(selectedRsaOnly.getName(), rsaOnly.getName());
  BOOST_CHECK_EQUAL(getCertificateKeyType(selectedRsaOnly), ndn::KeyType::RSA);

  ndn::Data signedDual("/test/ndnsf/signing/dual/data");
  signedDual.setContent("payload");
  keyChain.sign(signedDual, makeEcdsaPreferredSigningInfo(keyChain, dualIdentity));
  BOOST_CHECK_EQUAL(signedDual.getSignatureInfo().getKeyLocator().getName(),
                    dualEc.getName());

  ndn::Data signedRsaOnly("/test/ndnsf/signing/rsa-only/data");
  signedRsaOnly.setContent("payload");
  keyChain.sign(signedRsaOnly, makeEcdsaPreferredSigningInfo(keyChain, rsaOnlyIdentity));
  BOOST_CHECK_EQUAL(signedRsaOnly.getSignatureInfo().getKeyLocator().getName(),
                    rsaOnly.getName());
}

// BOOST_AUTO_TEST_CASE(mergeStateVector)
// {
 
//   //BOOST_CHECK_EQUAL(missingData[0].high, 3);
// }

BOOST_AUTO_TEST_SUITE_END()

} 
} 
