#include "tests/boost-test.hpp"
#include "tests/fixtures/uav-tracking-job-config.hpp"
#include "tests/fixtures/fixed-keychain-trust.hpp"

#include "NDNSF-UAV-APP/tracking/uav-tracking-job.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <boost/asio/post.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>

namespace ndnsf::test {
namespace {

using namespace examples::uav;

struct JobFixture : tracking_fixture::ConfigFixture
{
  JobFixture()
    : keys("pib-memory:spec228-job", "tpm-memory:spec228-job")
    , certificate(keys.createIdentity(ndn::Name("/lab/gs"), ndn::RsaKeyParams(2048))
                    .getDefaultKey().getDefaultCertificate())
    , face(keys)
    , trustSchema(ndnsf_distributed_repo::tests::writeFixedKeychainTrust(root, keys))
    , user(ndn_service_framework::ServiceUser::LocalMockTag{}, face, ndn::Name("/lab/group"),
           certificate, certificate, trustSchema)
    , job(user, face,
          [this] (const TrackingJobDefinition&, std::string* reason) {
            if (!allowed && reason) *reason = "test operator lease denied";
            return allowed;
          }, [this] (const TrackingSnapshot& snapshot) { lastSnapshot = snapshot; })
  {
    user.setHandlerThreads(0);
  }

  ~JobFixture()
  {
    auto stopped = std::make_shared<bool>(false);
    job.stop([stopped] (const TrackingCleanupResult&) { *stopped = true; });
    pump();
    BOOST_CHECK(*stopped);
  }

  void pump()
  {
    auto& io = face.getIoContext();
    io.restart();
    io.run_for(std::chrono::milliseconds(25));
  }

  ndn::KeyChain keys;
  ndn::security::Certificate certificate;
  ndn::DummyClientFace face;
  std::string trustSchema;
  ndn_service_framework::ServiceUser user;
  bool allowed = true;
  TrackingSnapshot lastSnapshot;
  UavTrackingJob job;
};

} // namespace

BOOST_AUTO_TEST_SUITE(UavTrackingJobLifecycle)

BOOST_FIXTURE_TEST_CASE(CancelBeforeDispatchKeepsBorrowedFaceAliveAndAllowsNextRun, JobFixture)
{
  auto definition = load();
  std::string reason;
  BOOST_REQUIRE(job.start(definition, &reason) == TrackingAdmission::ACCEPTED);
  BOOST_CHECK(job.start(definition, &reason) == TrackingAdmission::BUSY);
  BOOST_CHECK(job.requestCancel("wrong-job") == TrackingCancelResult::WRONG_JOB);
  BOOST_CHECK(job.requestCancel(definition.jobId) == TrackingCancelResult::REQUESTED);
  bool unrelatedCallbackRan = false;
  boost::asio::post(face.getIoContext(), [&unrelatedCallbackRan] { unrelatedCallbackRan = true; });
  pump();
  BOOST_CHECK(unrelatedCallbackRan);
  BOOST_CHECK(job.snapshot().state == TrackingJobState::CANCELLED);
  BOOST_CHECK_EQUAL(job.snapshot().cleanupState, "CLEAN");
  BOOST_CHECK(job.requestCancel(definition.jobId) == TrackingCancelResult::ALREADY_TERMINAL);
  const auto oldGeneration = job.snapshot().generation;
  definition.jobId = "job-2";
  definition.runId = "run-2";
  BOOST_REQUIRE(job.start(definition, &reason) == TrackingAdmission::ACCEPTED);
  BOOST_CHECK_EQUAL(job.snapshot().generation, oldGeneration + 1);
  BOOST_CHECK(job.requestCancel(definition.jobId) == TrackingCancelResult::REQUESTED);
  pump();
  BOOST_CHECK_EQUAL(job.snapshot().runId, "run-2");
}

BOOST_FIXTURE_TEST_CASE(LiveAuthorityDenialDoesNotCreateJobOrTouchSourceFiles, JobFixture)
{
  allowed = false;
  const auto definition = tracking_fixture::validDefinition();
  std::string reason;
  BOOST_CHECK(job.start(definition, &reason) == TrackingAdmission::UNAUTHORIZED);
  BOOST_CHECK_EQUAL(reason, "test operator lease denied");
  BOOST_CHECK(job.snapshot().state == TrackingJobState::IDLE);
}

BOOST_FIXTURE_TEST_CASE(StartRechecksAssetsAfterLoadingAndRejectsDirectDigestMismatch, JobFixture)
{
  auto definition = load();
  std::ofstream(root / "asset") << "changed after load";
  std::string reason;
  BOOST_CHECK(job.start(definition, &reason) == TrackingAdmission::INVALID);
  BOOST_CHECK(reason.find("asset digest mismatch") != std::string::npos);
  BOOST_CHECK(job.snapshot().state == TrackingJobState::IDLE);
}

BOOST_FIXTURE_TEST_CASE(RevocationBeforeFirstWindowFailsWithoutStoppingHost, JobFixture)
{
  const auto definition = load();
  BOOST_REQUIRE(job.start(definition) == TrackingAdmission::ACCEPTED);
  allowed = false;
  pump();
  BOOST_CHECK(job.snapshot().state == TrackingJobState::FAILED);
  BOOST_CHECK_EQUAL(job.snapshot().failureStage, "authority");
  BOOST_CHECK_EQUAL(job.snapshot().cleanupState, "CLEAN");
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::test
