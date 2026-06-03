#include "tests/boost-test.hpp"

#include "ndn-service-framework/ServiceContainer.hpp"

#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_CASE(ServiceContainerComposesUsersProvidersLocalServicesAndLifecycle)
{
  ndn::security::KeyChain keyChain("pib-memory:service-container",
                                   "tpm-memory:service-container");
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enablePacketLogging = true;
  ndn::DummyClientFace face(keyChain, faceOptions);

  const auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  const auto userCert = makeRsaIdentity(keyChain, ndn::Name("/test/user/operator"));
  const auto aiUserCert = makeRsaIdentity(keyChain, ndn::Name("/test/user/ai"));
  const auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/test/provider/drone"));

  auto operatorUser = std::make_shared<LocalServiceUser>(
    face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  auto aiUser = std::make_shared<LocalServiceUser>(
    face, ndn::Name("/test/group"), aiUserCert, aaCert, "examples/trust-any.conf");
  auto droneProvider = std::make_shared<LocalServiceProvider>(
    face, ndn::Name("/test/group"), providerCert, aaCert, "examples/trust-any.conf");

  ServiceContainer container({
    ndn::Name("/test/container"),
    ndn::Name("/test/group"),
    ndn::Name("/test/controller"),
    "examples/trust-any.conf"
  });

  container.addUser("operator", operatorUser);
  container.addUser("ai-client", aiUser);
  container.addProvider("drone-services", droneProvider);

  BOOST_CHECK(container.hasUser("operator"));
  BOOST_CHECK(container.hasUser("ai-client"));
  BOOST_CHECK(container.hasProvider("drone-services"));
  BOOST_CHECK_EQUAL(container.userRoles().size(), 2);
  BOOST_CHECK_EQUAL(container.providerRoles().size(), 1);
  BOOST_CHECK_EQUAL(&container.defaultUser(), operatorUser.get());
  BOOST_CHECK_EQUAL(&container.user("ai-client"), aiUser.get());
  BOOST_CHECK_EQUAL(&container.defaultProvider(), droneProvider.get());

  const ndn::Name serviceName("/Container/Local/Echo");
  container.localRegistry().registerLocalService<DynamicRequest, DynamicResponse>(
    serviceName,
    [] (const ndn::Name& requester,
        const DynamicRequest& request,
        DynamicResponse& response) {
      BOOST_CHECK_EQUAL(requester, ndn::Name("/test/user/operator"));
      BOOST_CHECK_EQUAL(request.getPayload(), "container-local");
      response.setClassification(42);
    });

  DynamicRequest request;
  request.setPayload("container-local");
  const auto result =
    container.localRegistry().localInvoke<DynamicRequest, DynamicResponse>(
      serviceName, request, ndn::Name("/test/user/operator"));
  BOOST_REQUIRE(result.success);
  BOOST_CHECK_EQUAL(result.response.getClassification(), 42);

  std::vector<std::string> lifecycle;
  container.addLifecycleHook("face", {
    [&] { lifecycle.push_back("start-face"); },
    [&] { lifecycle.push_back("stop-face"); }
  });
  container.addLifecycleHook("repo-helper", {
    [&] { lifecycle.push_back("start-repo"); },
    [&] { lifecycle.push_back("stop-repo"); }
  });

  container.start();
  container.start();
  BOOST_CHECK(container.isStarted());
  container.stop();
  container.stop();
  BOOST_CHECK(!container.isStarted());

  BOOST_REQUIRE_EQUAL(lifecycle.size(), 4);
  BOOST_CHECK_EQUAL(lifecycle[0], "start-face");
  BOOST_CHECK_EQUAL(lifecycle[1], "start-repo");
  BOOST_CHECK_EQUAL(lifecycle[2], "stop-repo");
  BOOST_CHECK_EQUAL(lifecycle[3], "stop-face");
}

BOOST_AUTO_TEST_CASE(ServiceContainerRejectsMissingRolesAndNullComponents)
{
  ServiceContainer container;

  BOOST_CHECK_THROW(container.defaultUser(), std::out_of_range);
  BOOST_CHECK_THROW(container.defaultProvider(), std::out_of_range);
  BOOST_CHECK_THROW(container.defaultController(), std::out_of_range);
  BOOST_CHECK_THROW(container.user("missing"), std::out_of_range);
  BOOST_CHECK_THROW(container.provider("missing"), std::out_of_range);
  BOOST_CHECK_THROW(container.controller("missing"), std::out_of_range);
  BOOST_CHECK_EQUAL(container.controllerRoles().size(), 0);
  BOOST_CHECK_THROW(container.addUser("", std::shared_ptr<ServiceUser>{}),
                    std::invalid_argument);
  BOOST_CHECK_THROW(container.addUser("bad", std::shared_ptr<ServiceUser>{}),
                    std::invalid_argument);
  BOOST_CHECK_THROW(container.addProvider("", std::shared_ptr<ServiceProvider>{}),
                    std::invalid_argument);
  BOOST_CHECK_THROW(container.addProvider("bad", std::shared_ptr<ServiceProvider>{}),
                    std::invalid_argument);
  BOOST_CHECK_THROW(container.addController("", std::shared_ptr<ServiceController>{}),
                    std::invalid_argument);
  BOOST_CHECK_THROW(container.addController("bad", std::shared_ptr<ServiceController>{}),
                    std::invalid_argument);
  BOOST_CHECK_THROW(container.addLifecycleHook("", {}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ServiceContainerRollsBackFailedStartAndAllowsRetry)
{
  ServiceContainer container;
  std::vector<std::string> lifecycle;

  container.addLifecycleHook("face", {
    [&] { lifecycle.push_back("start-face"); },
    [&] { lifecycle.push_back("stop-face"); }
  });
  container.addLifecycleHook("repo", {
    [&] {
      lifecycle.push_back("start-repo");
      throw std::runtime_error("repo failed");
    },
    [&] { lifecycle.push_back("stop-repo"); }
  });

  BOOST_CHECK_THROW(container.start(), std::runtime_error);
  BOOST_CHECK(!container.isStarted());

  BOOST_REQUIRE_EQUAL(lifecycle.size(), 3);
  BOOST_CHECK_EQUAL(lifecycle[0], "start-face");
  BOOST_CHECK_EQUAL(lifecycle[1], "start-repo");
  BOOST_CHECK_EQUAL(lifecycle[2], "stop-face");

  container.addLifecycleHook("repo", {
    [&] { lifecycle.push_back("start-repo-ok"); },
    [&] { lifecycle.push_back("stop-repo-ok"); }
  });

  container.start();
  BOOST_CHECK(container.isStarted());
  container.stop();
  BOOST_CHECK(!container.isStarted());
  BOOST_REQUIRE_EQUAL(lifecycle.size(), 7);
  BOOST_CHECK_EQUAL(lifecycle[3], "start-face");
  BOOST_CHECK_EQUAL(lifecycle[4], "start-repo-ok");
  BOOST_CHECK_EQUAL(lifecycle[5], "stop-repo-ok");
  BOOST_CHECK_EQUAL(lifecycle[6], "stop-face");
}

BOOST_AUTO_TEST_CASE(ServiceContainerRejectsRegistryChangesAfterStart)
{
  ServiceContainer container;

  container.addLifecycleHook("noop", {});
  container.start();
  BOOST_CHECK(container.isStarted());

  BOOST_CHECK_THROW(container.addLifecycleHook("late", {}), std::logic_error);
  BOOST_CHECK_THROW(container.addController("late-controller", std::shared_ptr<ServiceController>{}),
                    std::logic_error);
  BOOST_CHECK_THROW(container.addUser("late-user", std::shared_ptr<ServiceUser>{}),
                    std::logic_error);
  BOOST_CHECK_THROW(container.addProvider("late-provider", std::shared_ptr<ServiceProvider>{}),
                    std::logic_error);

  container.stop();
  BOOST_CHECK(!container.isStarted());
  BOOST_CHECK_NO_THROW(container.addLifecycleHook("after-stop", {}));
}

BOOST_AUTO_TEST_CASE(ServiceContainerStopRunsAllHooksBeforeReportingError)
{
  ServiceContainer container;
  std::vector<std::string> lifecycle;

  container.addLifecycleHook("face", {
    [&] { lifecycle.push_back("start-face"); },
    [&] {
      lifecycle.push_back("stop-face");
      throw std::runtime_error("face stop failed");
    }
  });
  container.addLifecycleHook("repo", {
    [&] { lifecycle.push_back("start-repo"); },
    [&] { lifecycle.push_back("stop-repo"); }
  });

  container.start();
  BOOST_CHECK_THROW(container.stop(), std::runtime_error);
  BOOST_CHECK(!container.isStarted());

  BOOST_REQUIRE_EQUAL(lifecycle.size(), 4);
  BOOST_CHECK_EQUAL(lifecycle[0], "start-face");
  BOOST_CHECK_EQUAL(lifecycle[1], "start-repo");
  BOOST_CHECK_EQUAL(lifecycle[2], "stop-repo");
  BOOST_CHECK_EQUAL(lifecycle[3], "stop-face");
}

} // namespace ndn_service_framework::test
