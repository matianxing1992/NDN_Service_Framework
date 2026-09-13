#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ConversationTypes.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModelTypes.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"

#include <boost/test/unit_test.hpp>

using namespace ndnsf::di;

BOOST_AUTO_TEST_SUITE(Spec185PreparedConversationApi)

BOOST_AUTO_TEST_CASE(ConversationCheckpointRejectsUnauthenticatedBytes)
{
  BOOST_CHECK_EXCEPTION(
    ConversationCheckpoint::fromBytes({0x7b, 0x7d}), DiError,
    [] (const DiError& error) {
      return error.code() == "INVALID_CHECKPOINT" &&
             error.boundary() == "conversation";
    });
}

BOOST_AUTO_TEST_SUITE_END()
