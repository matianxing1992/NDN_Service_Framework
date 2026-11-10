#include "tests/boost-test.hpp"

#include "ndnsf-integration-fixture.hpp"

#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/util/segmenter.hpp>

#include <atomic>
#include <chrono>
#include <future>

namespace ndn_service_framework::test {
namespace {

class LargeFetchProbe : public ServiceProvider
{
public:
  static std::weak_ptr<MessageValidator>
  validatorLifetime(const ServiceProvider& provider)
  {
    return provider.*(&LargeFetchProbe::validator);
  }

  static std::optional<ndn::Buffer>
  read(ServiceProvider& provider, const ndn::Name& name, std::function<bool()> current)
  {
    using Read = std::optional<ndn::Buffer> (ServiceProvider::*)(
      const ndn::Name&, const std::string&, const ndn::Name&, int, std::size_t,
      CollaborationTransferMetrics*, ndn::Name, std::function<bool()>, bool,
      std::uint32_t, std::uint64_t, ndn::Name, const ndn::Buffer&, const ndn::Name&,
      std::size_t, std::size_t);
    const auto read = static_cast<Read>(&LargeFetchProbe::fetchCollaborationLargeData);
    return (provider.*read)(ndn::Name("/spec228/request"), "", name, 250, 3, nullptr,
      ndn::Name("/spec228/producer"), std::move(current), true, 4096, 9000,
      {}, {}, {}, 64 * 1024 * 1024, 12000);
  }
};

enum class FetchFault { NONE, BAD_SIGNATURE, MISSING, CANCEL };

void
checkFetchLifetime(FetchFault fault)
{
  std::weak_ptr<MessageValidator> lifetime;
  {
    BootstrapProfile profile;
    // This Core-only fixture owns no DI Face worker; pump both Faces here.
    profile.providerFacesHaveDedicatedIoWorkers = false;
    profile.deferBridgeDelivery = true;
    NdnsfIntegrationEnvironment environment(profile);
    environment.bootstrap();
    lifetime = LargeFetchProbe::validatorLifetime(environment.provider());
    BOOST_REQUIRE(!lifetime.expired());
    const ndn::Name prefix("/spec228/producer/object");
    ndn::Buffer plaintext(9000, 0x5a);
    ndn::Segmenter segmenter(environment.keyChain(), ndn::security::signingWithSha256());
    auto packets = segmenter.segment(plaintext, prefix, 4096, ndn::time::seconds(1));
    BOOST_REQUIRE_EQUAL(packets.size(), 3);
    if (fault == FetchFault::BAD_SIGNATURE) {
      const ndn::Buffer changed(4096, 0x41);
      packets[0]->setContent(changed);
    }
    std::atomic<bool> current{true};
    std::atomic<unsigned> served{0};
    ndn::ScopedInterestFilterHandle serving = environment.userFace().setInterestFilter(
      prefix, [&] (const ndn::InterestFilter&, const ndn::Interest& interest) {
        const auto index = interest.getName().at(-1).toSegment();
        if (index >= packets.size()) return;
        if (fault == FetchFault::MISSING && index == 2) return;
        if (fault == FetchFault::CANCEL && index != 0) return;
        environment.userFace().put(*packets[index]);
        served.fetch_add(1);
        if (fault == FetchFault::CANCEL) current.store(false);
      });
    auto result = std::async(std::launch::async, [&] {
      return LargeFetchProbe::read(environment.provider(), prefix,
        [&] { return current.load(); });
    });
    environment.pumpUntil([&] {
      return result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
    });
    const auto opened = result.get();
    if (fault == FetchFault::NONE) {
      BOOST_REQUIRE(opened);
      BOOST_CHECK(*opened == plaintext);
    }
    else {
      BOOST_CHECK(!opened);
    }
    BOOST_CHECK_GT(served.load(), 0);
    // A cancelled/timed-out read must ignore Data that arrives after return.
    serving.cancel();
    environment.userFace().put(*packets.back());
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(40);
    environment.pumpUntil([&] { return std::chrono::steady_clock::now() >= until; });
    environment.destroyProviderKeepingFaceForTest(0);
  }
  BOOST_CHECK(lifetime.expired());
}

BOOST_AUTO_TEST_SUITE(Spec228LargeFetchLifetime)

BOOST_AUTO_TEST_CASE(SuccessReleasesExactFetch)
{
  checkFetchLifetime(FetchFault::NONE);
}

BOOST_AUTO_TEST_CASE(InvalidSignatureReleasesExactFetch)
{
  checkFetchLifetime(FetchFault::BAD_SIGNATURE);
}

BOOST_AUTO_TEST_CASE(TimeoutReleasesPartialExactFetch)
{
  checkFetchLifetime(FetchFault::MISSING);
}

BOOST_AUTO_TEST_CASE(AuthorityCancellationReleasesPartialExactFetch)
{
  checkFetchLifetime(FetchFault::CANCEL);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace
} // namespace ndn_service_framework::test
