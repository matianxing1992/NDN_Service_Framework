#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"
#include "ndnsf-distributed-repo/ArtifactTypes.hpp"

#include <stdexcept>
#include <string>

BOOST_AUTO_TEST_SUITE(ModelRepositoryWithoutRepo)

BOOST_AUTO_TEST_CASE(PublicInterfaceCompilesAndRepositoryPublicationFailsExplicitly)
{
  const ndnsf_distributed_repo::RepoArtifactTarget target{
    "/repo", "/repo/provider", "/SERVICE/repo"};
  BOOST_CHECK_EXCEPTION(ndnsf::di::nativeValidateModelRepositoryTarget(target, ndn::Name("/user")),
    std::invalid_argument, [](const auto& error) {
      return std::string(error.what()) ==
        "Model repository publication requires the Repo component";
    });
}

BOOST_AUTO_TEST_SUITE_END()
