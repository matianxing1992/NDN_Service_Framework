#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

namespace ndn_service_framework::test {
namespace {

class VersionedCollaborationUser : public LocalServiceUser
{
public:
  using LocalServiceUser::LocalServiceUser;

  const RequestMessage& pendingRequest(const ndn::Name& id) const
  {
    return m_pendingCalls.at(id).requestMessage;
  }
};

class EmptyParticipantSelection final : public ParticipantSelectionPolicy
{
public:
  std::vector<SelectedParticipant> select(
      const std::vector<AckCandidate>&,
      const std::vector<CollaborationRoleSpec>&) const override
  {
    return {};
  }
};

} // namespace

BOOST_AUTO_TEST_SUITE(GenericDynamicApi)
BOOST_AUTO_TEST_SUITE(CollaborationVersion)

BOOST_AUTO_TEST_CASE(BothCollaborationEntrypointsBindAcceptedServiceVersion)
{
  ndn::security::KeyChain keyChain("pib-memory:", "tpm-memory:");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name service("/test/collaboration/version");
  const auto cert = makeRsaIdentity(keyChain, ndn::Name("/test/user/version"));
  const auto aa = makeRsaIdentity(keyChain, ndn::Name("/test/aa/version"));
  VersionedCollaborationUser user(face, ndn::Name("/test/group/version"),
                                cert, aa, "examples/trust-any.conf");
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  PolicyStatusData status;
  status.setServiceName(service);
  status.setControllerVersion(ControllerVersion{1000, 7});
  status.setValidity(now - 1000, now + 60000);
  status.setPolicyDigest("sha256:" + std::string(64, 'a'));
  status.setControllerCertificate(aa.getName());
  BOOST_REQUIRE(user.installControllerStatus(status));

  CollaborationPlan plan;
  plan.participantSelector = std::make_shared<EmptyParticipantSelection>();
  const auto planned = user.RequestCollaboration(
      service, ndn::Buffer{1}, plan, [](const ResponseMessage&) {},
      [](const ndn::Name&) {}, ndn::Name("/planned-version"));
  const auto deferred = user.BeginCollaboration(
      service, ndn::Buffer{1}, 200, 5000,
      [](const CollaborationAckClosure&) {}, [](const ResponseMessage&) {},
      [](const ndn::Name&) {}, ndn::Name("/deferred-version"));

  for (const auto& id : {planned, deferred}) {
    BOOST_REQUIRE(!id.empty());
    const auto& request = user.pendingRequest(id);
    BOOST_CHECK_MESSAGE(request.hasControllerVersion(), id.toUri());
    if (request.hasControllerVersion()) {
      BOOST_CHECK(request.getControllerVersion() == status.getControllerVersion());
    }
  }

  auto revoked = status;
  revoked.setControllerVersion(ControllerVersion{1000, 8});
  RevocationTarget target;
  target.kind = RevocationKind::IDENTITY;
  target.targetIdentity = ndn::Name("/test/user/version");
  revoked.addRevocation(target);
  BOOST_REQUIRE(user.installControllerStatus(revoked));
  BOOST_CHECK(user.RequestCollaboration(
      service, ndn::Buffer{1}, plan, [](const ResponseMessage&) {},
      [](const ndn::Name&) {}, ndn::Name("/planned-revoked")).empty());
  BOOST_CHECK(user.BeginCollaboration(
      service, ndn::Buffer{1}, 200, 5000,
      [](const CollaborationAckClosure&) {}, [](const ResponseMessage&) {},
      [](const ndn::Name&) {}, ndn::Name("/deferred-revoked")).empty());
}

BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE_END()
} // namespace ndn_service_framework::test
