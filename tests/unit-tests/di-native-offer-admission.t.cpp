#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include <boost/test/unit_test.hpp>
#include <fstream>
#include <type_traits>

namespace {
using namespace ndnsf::di;
using ndn_service_framework::AckSelectionCandidate;
NativeJson fixture()
{
  std::ifstream input("tests/fixtures/spec182/signed-offer-oracle.json");
  if (!input) throw std::runtime_error("missing signed SDK offer fixture");
  return NativeJson::parse(input);
}
NativeOfferAdmission admission(const NativeJson& f)
{
  return NativeOfferAdmission(f.at("policy").dump(),
    {{f.at("key_id").get<std::string>(), f.at("public_pem").get<std::string>()}},
    f.at("candidate").get<std::string>());
}
NativeOfferBindingContext context(const NativeJson& sample)
{
  const auto v = nativeParseJson(sample.at("wire").get<std::string>());
  return {v.at("request_id"), v.at("attempt"), v.at("service"),
          v.at("model_digest"), v.at("graph_digest"), 900};
}
AckSelectionCandidate candidate(const std::string& wire)
{
  const auto v = nativeParseJson(wire);
  AckSelectionCandidate ack;
  ack.providerName = ndn::Name(v.at("provider").get<std::string>());
  ack.serviceName = ndn::Name(v.at("service").get<std::string>());
  ack.requestId = ndn::Name(v.at("request_id").get<std::string>());
  ack.ack.setStatus(v.at("status").get<bool>());
  ndn::Buffer payload(wire.begin(), wire.end());
  ack.ack.setPayload(payload, payload.size());
  // Explicit test fixture of Core output, not proof of real subscription validation.
  ack.authenticationEvidence = {"/provider/a", "/provider/a/KEY/fixture/issuer/v=1",
                                "sha256:" + std::string(64, '1'), true};
  return ack;
}
}
BOOST_AUTO_TEST_SUITE(Spec182OfferAdmission)
BOOST_AUTO_TEST_CASE(RealSdkSignaturesAndImmutableObservations)
{
  static_assert(!std::is_default_constructible_v<NativeAdmittedOfferV3>);
  const auto f = fixture();
  const auto verifier = admission(f);
  for (const auto& sample : f.at("vectors")) {
    auto ack = candidate(sample.at("wire"));
    const auto result = verifier.verify(ack, context(sample), 200);
    BOOST_CHECK_EQUAL(result.observation().offerDigest, sample.at("digest").get<std::string>());
    BOOST_CHECK_EQUAL(result.observation().status, ack.ack.getStatus());
    if (sample.at("name") == "cuda") {
      BOOST_REQUIRE_EQUAL(result.observation().resources.size(), 1);
      BOOST_CHECK_EQUAL(result.observation().resources[0].freeMemoryMb, 9000);
      BOOST_CHECK_EQUAL(result.observation().residency[0].runtimeGeneration, 7);
    }
    else BOOST_CHECK(result.observation().resources.empty());
    ack.authenticationEvidence = {};
    BOOST_CHECK_EQUAL(result.observation().provider, "/provider/a");
  }
}
BOOST_AUTO_TEST_CASE(RejectMissingOrForeignCoreProvenance)
{
  const auto f = fixture(), sample = f.at("vectors")[0];
  const auto verifier = admission(f);
  const auto original = candidate(sample.at("wire"));
  const auto ctx = context(sample);
  auto ack = original; ack.authenticationEvidence.trustSchemaValidated = false;
  BOOST_CHECK_THROW(verifier.verify(ack, ctx, 200), std::runtime_error);
  ack = original; ack.authenticationEvidence.signerIdentity.clear();
  BOOST_CHECK_THROW(verifier.verify(ack, ctx, 200), std::runtime_error);
  ack = original; ack.authenticationEvidence.signerIdentity = "/provider/b";
  BOOST_CHECK_THROW(verifier.verify(ack, ctx, 200), std::runtime_error);
  ack = original; ack.authenticationEvidence.wireDigest = "invalid";
  BOOST_CHECK_THROW(verifier.verify(ack, ctx, 200), std::runtime_error);
  ack = original; ack.authenticationEvidence.signerKeyLocator = "/provider/a/KEY/fixture-foreign";
  BOOST_CHECK_THROW(verifier.verify(ack, ctx, 200), std::runtime_error);
  ack = original; ack.providerName = ndn::Name("/provider/b");
  BOOST_CHECK_THROW(verifier.verify(ack, ctx, 200), std::runtime_error);
  ack = original; ack.serviceName = ndn::Name("/other");
  BOOST_CHECK_THROW(verifier.verify(ack, ctx, 200), std::runtime_error);
  ack = original; ack.requestId = ndn::Name("/other");
  BOOST_CHECK_THROW(verifier.verify(ack, ctx, 200), std::runtime_error);
  ack = original; ack.ack.setStatus(false);
  BOOST_CHECK_THROW(verifier.verify(ack, ctx, 200), std::runtime_error);
}
BOOST_AUTO_TEST_CASE(RejectRequestBindingsAndExpiry)
{
  const auto f = fixture(), sample = f.at("vectors")[0];
  const auto verifier = admission(f);
  const auto ack = candidate(sample.at("wire"));
  const auto original = context(sample);
  for (unsigned i = 0; i < 7; ++i) {
    auto ctx = original;
    switch (i) {
      case 0: ctx.requestId = "foreign"; break;
      case 1: ctx.attempt = 2; break;
      case 2: ctx.serviceName = "/other"; break;
      case 3: ctx.modelDigest = "sha256:" + std::string(64, '1'); break;
      case 4: ctx.graphDigest = "sha256:" + std::string(64, '1'); break;
      case 5: ctx.deadlineMs = 1001; break;
      case 6: ctx.deadlineMs = 200; break;
    }
    BOOST_CHECK_THROW(verifier.verify(ack, ctx, 200), std::runtime_error);
  }
  BOOST_CHECK_THROW(verifier.verify(ack, original, 99), std::runtime_error);
  BOOST_CHECK_THROW(verifier.verify(ack, original, 1000), std::runtime_error);
}
BOOST_AUTO_TEST_CASE(RejectTamperingAndInvalidSignatureEncoding)
{
  const auto f = fixture(), sample = f.at("vectors")[0];
  const auto verifier = admission(f);
  const auto original = nativeParseJson(sample.at("wire").get<std::string>());
  const auto ctx = context(sample);
  auto v = original; v["has_model"] = false;
  BOOST_CHECK_THROW(verifier.verify(candidate(nativeCanonicalJson(v)), ctx, 200), std::runtime_error);
  v = original; v["signer_key_id"] = "sha256:" + std::string(64, '1');
  BOOST_CHECK_THROW(verifier.verify(candidate(nativeCanonicalJson(v)), ctx, 200), std::runtime_error);
  for (const auto& sig : {std::string(88, 'A'), std::string("bad"), std::string(86, 'A') + "==",
                         std::string(85, 'A') + "!=="} ) {
    v = original; v["signature"] = sig;
    BOOST_CHECK_THROW(verifier.verify(candidate(nativeCanonicalJson(v)), ctx, 200), std::runtime_error);
  }
}
BOOST_AUTO_TEST_CASE(RejectPolicyAndKeySubstitution)
{
  const auto original = fixture();
  auto f = original; f["candidate"] = "sha256:" + std::string(64, '1');
  BOOST_CHECK_THROW(admission(f), std::runtime_error);
  f = original; f["key_id"] = "sha256:" + std::string(64, '1');
  BOOST_CHECK_THROW(admission(f), std::runtime_error);
  f = original; f["public_pem"] = "invalid";
  BOOST_CHECK_THROW(admission(f), std::runtime_error);
  f = original; f["policy"]["entries"].push_back(f["policy"]["entries"][0]);
  BOOST_CHECK_THROW(admission(f), std::runtime_error);
  f = original; f["policy"]["entries"][0]["keyLocatorPrefix"] = "/foreign/KEY/k";
  BOOST_CHECK_THROW(admission(f), std::runtime_error);
  f = original; f["policy"]["freeBytes"] = 1000000;
  BOOST_CHECK_THROW(admission(f), std::runtime_error);
}
BOOST_AUTO_TEST_SUITE_END()
