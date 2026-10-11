#include "tests/boost-test.hpp"
#include "ndn-service-framework/NDNSFMessages.hpp"
#include "ndn-service-framework/PeerProbeNaming.hpp"
#include "ndn-service-framework/ProducerDataNaming.hpp"
#include <ndn-cxx/encoding/block-helpers.hpp>
#include <functional>
#include <stdexcept>
#include <vector>

namespace ndn_service_framework::test {
namespace {
PeerProbeCall sample()
{
  return {"PROBE-GOODPUT", ndn::Name("/observer/one"),
    ndn::Name("/target/NDNSF/RPC/provider"), "scope1", "123",
    2, 16, 4096, false, true};
}
ndn::Interest prepare(const PeerProbeCall& call)
{
  PeerNetworkGoodputProbe parameters;
  parameters.setField("observerProvider", call.observer.toUri());
  parameters.setField("targetProvider", call.target.toUri());
  parameters.setField("requestScopeDigest", call.requestScopeDigest);
  parameters.setField("runNonce", call.runNonce);
  parameters.setField("segment", std::to_string(call.segment));
  parameters.setField("segmentCount", std::to_string(call.segmentCount));
  parameters.setField("contentBytes", std::to_string(call.contentBytes));
  ndn::Interest interest(makePeerProbeRpcName(call));
  interest.setApplicationParameters(parameters.WireEncode());
  return interest;
}
}

BOOST_AUTO_TEST_SUITE(Spec201ProbeNames)

BOOST_AUTO_TEST_CASE(IndependentTaggedTranscriptAndFullProducer)
{
  const auto call = sample();
  // Independently assembled C01 tagged transcript, literal Name TLVs and
  // big-endian integers, hashed outside the production naming builder.
  auto expected = call.target;
  expected.append("NDNSF").append("RPC").append("PROBE-GOODPUT")
    .append(makeNamingDigestComponent(
      "9d6a8b0c5e9dbf79c4971371788bf6be8302bba4d10f6f4fde7aca2b3ced6151"))
    .append("123");
  BOOST_CHECK(makePeerProbeRpcName(call) == expected);
  const auto interest = prepare(call);
  BOOST_CHECK(interest.isParametersDigestValid());
  BOOST_CHECK(matchesPeerProbeRpcName(interest, call));
  BOOST_CHECK_EQUAL(interest.getName().size(), call.target.size() + 6);
  BOOST_CHECK(interest.getName()[-1].isParametersSha256Digest());
  BOOST_CHECK_EQUAL(interest.getName()[call.target.size() + 3].value_size(), 32U);
  auto foreign = call;
  foreign.target = ndn::Name("/foreign/provider");
  BOOST_CHECK(!matchesPeerProbeRpcName(interest, foreign));
}

BOOST_AUTO_TEST_CASE(EveryExistingCallFieldSeparatesIdentity)
{
  const auto original = sample();
  const auto interest = prepare(original);
  const std::vector<std::function<void(PeerProbeCall&)>> changes{
    [](auto& c) { c.observer.append("other"); },
    [](auto& c) { c.target.append("other"); },
    [](auto& c) { c.requestScopeDigest += "other"; },
    [](auto& c) { c.runNonce = "124"; },
    [](auto& c) { ++c.segment; },
    [](auto& c) { ++c.segmentCount; },
    [](auto& c) { ++c.contentBytes; },
    [](auto& c) { c.operation = "PROBE-READY"; c.segment = 0;
      c.segmentCount = 0; c.contentBytes = 0; },
  };
  for (const auto& change : changes) {
    auto call = original;
    change(call);
    BOOST_CHECK(makePeerProbeRpcName(call) != makePeerProbeRpcName(original));
    BOOST_CHECK(!matchesPeerProbeRpcName(interest, call));
  }
  auto ready = original;
  ready.operation = "PROBE-READY";
  ready.segment = ready.segmentCount = ready.contentBytes = 0;
  auto both = ready;
  both.requireRtt = true;
  BOOST_CHECK(makePeerProbeRpcName(both) != makePeerProbeRpcName(ready));
  auto rttOnly = both;
  rttOnly.requireGoodput = false;
  BOOST_CHECK(makePeerProbeRpcName(rttOnly) != makePeerProbeRpcName(both));
}

BOOST_AUTO_TEST_CASE(InvalidScopeAndCallCommitmentRejectBeforeAdmission)
{
  const auto original = sample();
  auto interest = prepare(original);
  auto corrupt = interest.getName();
  corrupt.set(original.target.size() + 3, makeNamingDigestComponent(std::string(64, 'f')));
  interest.setName(corrupt);
  BOOST_CHECK(interest.isParametersDigestValid());
  BOOST_CHECK(!matchesPeerProbeRpcName(interest, original));
  auto noParameters = ndn::Interest(makePeerProbeRpcName(original));
  BOOST_CHECK(!matchesPeerProbeRpcName(noParameters, original));
  const std::vector<std::function<void(PeerProbeCall&)>> invalid{
    [](auto& c) { c.target = ndn::Name(); },
    [](auto& c) { c.observer = c.target; },
    [](auto& c) { c.operation = "OTHER"; },
    [](auto& c) { c.runNonce = "not-a-run"; },
    [](auto& c) { c.requestScopeDigest.clear(); },
    [](auto& c) { c.segment = c.segmentCount; },
    [](auto& c) { c.contentBytes = 0; },
    [](auto& c) { c.requireRtt = true; },
  };
  for (const auto& change : invalid) {
    auto call = original;
    change(call);
    BOOST_CHECK_THROW(makePeerProbeRpcName(call), std::invalid_argument);
    BOOST_CHECK(!matchesPeerProbeRpcName(prepare(original), call));
  }
}

BOOST_AUTO_TEST_SUITE_END()
}
