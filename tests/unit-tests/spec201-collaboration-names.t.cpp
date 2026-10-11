#include "ndn-service-framework/utils.hpp"
#include "tests/boost-test.hpp"
#include <ndn-svs/sync-protocol.hpp>

#include <limits>
#include <stdexcept>

namespace ndn_service_framework::test {
BOOST_AUTO_TEST_SUITE(Spec201CollaborationNames)

BOOST_AUTO_TEST_CASE(SelectedSvsSyncNamesKeepLibraryGrammar)
{
  const ndn::Name group("/org/group");
  BOOST_CHECK_EQUAL(ndn::svs::SyncProtocolCodec::makeSyncName(
    group, ndn::svs::SvsProtocolVersion::V3), ndn::Name(group).appendVersion(3));
  BOOST_CHECK_EQUAL(ndn::svs::SyncProtocolCodec::makeSyncName(
    group, ndn::svs::SvsProtocolVersion::V2), ndn::Name(group).appendVersion(2));
  const ndn::Name actor("/org/NDNSF/COLLAB/provider/p0");
  const auto node = ndn::Name(actor).append("provider").append("17");
  const auto parsed = parseSvsProducerActor(node, "provider");
  BOOST_REQUIRE(parsed);
  BOOST_CHECK_EQUAL(*parsed, actor);
  BOOST_CHECK(!parseSvsProducerActor(node, "user"));
  BOOST_CHECK(!group.isPrefixOf(ndn::Name(node).append(group)));
}

BOOST_AUTO_TEST_CASE(CompleteProducerAndCanonicalInvocationFields)
{
  const ndn::Name producer("/org/NDNSF/COLLAB/provider/p0");
  const ndn::Name requester("/org/user/u0");
  const ndn::Name request("/session/request/7");
  const ndn::Name topic("/hidden-state/output");
  const auto name = makeCollaborationDataName(producer, requester, request, "scope", topic,
                                             std::numeric_limits<uint64_t>::max());
  BOOST_REQUIRE(producer.isPrefixOf(name));
  BOOST_CHECK_EQUAL(name[producer.size()], ndn::name::Component("NDNSF"));
  BOOST_CHECK_EQUAL(name[producer.size() + 1], ndn::name::Component("COLLAB"));
  const auto parsed = parseCollaborationDataName(name, producer);
  BOOST_REQUIRE(parsed);
  BOOST_CHECK_EQUAL(parsed->producerName, producer);
  BOOST_CHECK_EQUAL(parsed->requesterName, requester);
  BOOST_CHECK_EQUAL(parsed->requestId, request);
  BOOST_CHECK_EQUAL(parsed->keyScope, "scope");
  BOOST_CHECK_EQUAL(parsed->topic, topic);
  BOOST_CHECK_EQUAL(parsed->sequence, std::numeric_limits<uint64_t>::max());
  BOOST_CHECK(!parseCollaborationDataName(name, ndn::Name("/org")));
  BOOST_CHECK(!parseCollaborationDataName(name, ndn::Name("/org/provider/p1")));
  BOOST_CHECK(!parseCollaborationDataName(ndn::Name(name).append("extra"), producer));
  auto noncanonical = name.getPrefix(-1);
  noncanonical.append("01");
  BOOST_CHECK(!parseCollaborationDataName(noncanonical, producer));
  auto overflow = name.getPrefix(-1);
  overflow.append("18446744073709551616");
  BOOST_CHECK(!parseCollaborationDataName(overflow, producer));
  auto wrongType = name.getPrefix(-1);
  wrongType.appendSequenceNumber(1);
  BOOST_CHECK(!parseCollaborationDataName(wrongType, producer));
  BOOST_CHECK_THROW(makeCollaborationDataName(ndn::Name(), requester, request, "scope", topic, 0),
                    std::invalid_argument);
  BOOST_CHECK_THROW(makeCollaborationDataName(producer, requester, request,
                                            std::string(1024, 'x'), topic, 0), std::length_error);
  const ndn::Name singleRequest("/one");
  const auto singleName = makeCollaborationDataName(producer, requester, singleRequest, "scope", topic, 0);
  BOOST_REQUIRE(parseCollaborationDataName(singleName, producer));
  BOOST_CHECK_EQUAL(parseCollaborationDataName(singleName, producer)->requestId, singleRequest);
}

BOOST_AUTO_TEST_SUITE_END()
}
