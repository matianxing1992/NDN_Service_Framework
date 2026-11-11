#include "tests/integration-tests/isolated-test-trust.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"

#include <chrono>
#include <future>
#include <optional>

namespace ndn_service_framework::test {
namespace {

class SelectedRepoUser : public LocalServiceUser
{
public:
  using LocalServiceUser::LocalServiceUser;

  bool replyToCommittedSelection(const ndn::Name& id)
  {
    const auto pending = m_pendingCalls.find(id);
    if (pending == m_pendingCalls.end() || !pending->second.collaborationPlanCommitted ||
        pending->second.selectionPublishedProviders.size() != 1)
      return false;
    // Only response transport is mocked. The production RepoClient must first
    // freeze authenticated ACK candidates and commit its exact participant.
    const auto callback = pending->second.responseHandler;
    ResponseMessage response;
    response.setStatus(true);
    ndn::Buffer payload(1, 0x42);
    response.setPayload(payload, payload.size());
    callback(response);
    return true;
  }
};

struct RepoAckFixture
{
  RepoAckFixture()
    : keyChain("pib-memory:spec229-repo-ack", "tpm-memory:spec229-repo-ack")
    , face(keyChain)
    , userCertificate(makeRsaIdentity(keyChain, requester))
    , authorityCertificate(makeRsaIdentity(keyChain, "/spec229/authority"))
    , trust{userCertificate, authorityCertificate}
    , user(face, "/spec229/group", userCertificate, authorityCertificate,
           trust.schemaPath())
  {
    installUserPermissions(user, requester, service, {provider});
  }

  void run(int collectionMs, bool trusted, bool matchingSigner, bool expectSuccess,
           int delayMs = 2700)
  {
    ndn::Name requestId;
    RequestMessage published;
    std::optional<std::chrono::steady_clock::time_point> publishedAt;
    size_t selectionPublications = 0;
    user.setLocalPublicationHandler([&](const ndn::Name& name, const ndn::Buffer&) {
      if (name == makeServiceSelectionNameV2(requester, provider, service, requestId))
        ++selectionPublications;
    });
    user.setRequestPublisher([&](const ndn::Name& id, const ndn::Name&,
        const std::vector<ndn::Name>&, const ndn::Name&, const RequestMessage& request,
        size_t) {
      requestId = id;
      published = request;
      publishedAt = std::chrono::steady_clock::now();
    });
    auto future = std::async(std::launch::async, [&] {
      return ndnsf_distributed_repo::RepoClient::requestSelectedService(
        user, provider, service, {0x01}, collectionMs, 6000);
    });
    bool delivered = false;
    bool selected = false;
    bool finished = false;
    std::string error;
    std::vector<uint8_t> result;
    const auto stop = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    // Keep the Face, user, and callback captures alive until the blocking
    // production caller has returned, including every error path.
    try {
      while (std::chrono::steady_clock::now() < stop) {
        pumpFace(face, ndn::time::milliseconds(5));
        if (publishedAt && !delivered && std::chrono::steady_clock::now() - *publishedAt >=
              std::chrono::milliseconds(delayMs)) {
          AckAuthenticationEvidence evidence;
          evidence.signerIdentity = matchingSigner ? provider.toUri() : "/spec229/foreign";
          evidence.signerKeyLocator = evidence.signerIdentity + "/KEY/fixture";
          evidence.wireDigest = "sha256:injected-local-ack-evidence";
          evidence.trustSchemaValidated = trusted;
          const auto name = makeRequestAckNameV2(provider, requester, service, requestId);
          auto ack = makeSuccessAckForRequest(published);
          user.handleRequestAckByName(name, ack, evidence);
          delivered = true;
        }
        if (!selected && !requestId.empty())
          selected = user.replyToCommittedSelection(requestId);
        if (!finished && future.wait_for(std::chrono::milliseconds(0)) ==
              std::future_status::ready) {
          try { result = future.get(); }
          catch (const std::exception& failure) { error = failure.what(); }
          finished = true;
        }
        // Deliver the late ACK even after the one-second window failed, then
        // pump again to verify it cannot resurrect a committed selection.
        if (finished && delivered) {
          pumpFace(face, ndn::time::milliseconds(30));
          selected = selected || user.replyToCommittedSelection(requestId);
          break;
        }
      }
    }
    catch (...) {
      if (!requestId.empty()) user.CancelCollaboration(requestId);
      while (future.valid() && future.wait_for(std::chrono::milliseconds(0)) !=
               std::future_status::ready)
        pumpFace(face, ndn::time::milliseconds(5));
      if (future.valid()) { try { future.get(); } catch (...) {} }
      throw;
    }
    if (!requestId.empty()) user.CancelCollaboration(requestId);
    while (future.valid() && future.wait_for(std::chrono::milliseconds(0)) !=
             std::future_status::ready)
      pumpFace(face, ndn::time::milliseconds(5));
    if (future.valid()) { try { result = future.get(); }
                         catch (const std::exception& failure) { error = failure.what(); } }
    pumpFace(face, ndn::time::milliseconds(10));
    BOOST_CHECK(publishedAt);
    BOOST_CHECK(delivered);
    BOOST_CHECK(finished);
    if (delayMs < 100 && publishedAt) {
      BOOST_CHECK(std::chrono::steady_clock::now() - *publishedAt <
                  std::chrono::milliseconds(1000));
    }
    BOOST_CHECK_EQUAL(selected, expectSuccess);
    BOOST_CHECK_EQUAL(selectionPublications, expectSuccess ? 1U : 0U);
    if (expectSuccess) {
      BOOST_CHECK_MESSAGE(error.empty(), error);
      BOOST_CHECK(result == std::vector<uint8_t>{0x42});
    }
    else {
      BOOST_CHECK_MESSAGE(error.find("repo-ack-authentication-failed") != std::string::npos,
                          error);
      BOOST_CHECK(result.empty());
    }
  }

  ndn::security::KeyChain keyChain;
  ndn::DummyClientFace face;
  const ndn::Name requester{"/spec229/user"};
  const ndn::Name provider{"/spec229/repo"};
  const ndn::Name service{"/spec229/INSERT"};
  ndn::security::Certificate userCertificate;
  ndn::security::Certificate authorityCertificate;
  IsolatedTestTrust trust;
  SelectedRepoUser user;
};
} // namespace

BOOST_AUTO_TEST_SUITE(Spec229RepoAckWindow)
BOOST_FIXTURE_TEST_CASE(OneSecondClosesBeforeDelayedAck, RepoAckFixture)
{ run(1000, true, true, false); }
BOOST_FIXTURE_TEST_CASE(ThreeSecondsAcceptsDelayedExactProvider, RepoAckFixture)
{ run(3000, true, true, true); }
BOOST_FIXTURE_TEST_CASE(ThreeSecondsRejectsUntrustedAck, RepoAckFixture)
{ run(3000, false, true, false); }
BOOST_FIXTURE_TEST_CASE(ThreeSecondsRejectsForeignSigner, RepoAckFixture)
{ run(3000, true, false, false); }
BOOST_FIXTURE_TEST_CASE(TrustedAckClosesBeforeMaximumWindow, RepoAckFixture)
{ run(3000, true, true, true, 50); }
BOOST_AUTO_TEST_SUITE_END()
} // namespace ndn_service_framework::test
