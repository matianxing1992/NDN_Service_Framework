#include "NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp"
#include "tests/boost-test.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace ndnsf_distributed_repo::test {

BOOST_AUTO_TEST_SUITE(DistributedRepoDeploymentMode)

BOOST_AUTO_TEST_CASE(RemoteAliasesSetOnlyRemoteHelperFlag)
{
  for (const auto& value : std::vector<std::string>{"", "remote", "REMOTE"}) {
    const auto mode = parseRepoDeploymentMode(value);
    BOOST_CHECK(mode == RepoDeploymentMode::Remote);
    BOOST_CHECK(enablesRemote(mode));
    BOOST_CHECK(!enablesEmbedded(mode));
    BOOST_CHECK_EQUAL(toString(mode), "remote");
  }
}

BOOST_AUTO_TEST_CASE(EmbeddedAliasesSetOnlyLocalHelperFlag)
{
  for (const auto& value : std::vector<std::string>{
         "embedded", "local", "inprocess", "in-app", "IN_APP"}) {
    const auto mode = parseRepoDeploymentMode(value);
    BOOST_CHECK(mode == RepoDeploymentMode::Embedded);
    BOOST_CHECK(!enablesRemote(mode));
    BOOST_CHECK(enablesEmbedded(mode));
    BOOST_CHECK_EQUAL(toString(mode), "embedded");
  }
}

BOOST_AUTO_TEST_CASE(BothAliasesOnlySetParserHelperFlags)
{
  for (const auto& value : std::vector<std::string>{
         "both", "remoteembedded", "embeddedremote", "remotelocal",
         "localremote", "remote-embedded"}) {
    const auto mode = parseRepoDeploymentMode(value);
    BOOST_CHECK(mode == RepoDeploymentMode::Both);
    BOOST_CHECK(enablesRemote(mode));
    BOOST_CHECK(enablesEmbedded(mode));
    BOOST_CHECK_EQUAL(toString(mode), "both");
  }
}

BOOST_AUTO_TEST_CASE(UnknownModeIsRejected)
{
  BOOST_CHECK_THROW(parseRepoDeploymentMode("unexpected"), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf_distributed_repo::test
