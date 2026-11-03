#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-kv-restore.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/protected-kv-packets.hpp"
#include "tests/unit-tests/native-protected-kv-fixture.hpp"

namespace ndnsf::di::test {
namespace {

using namespace protectedKv;

struct RestoreFixture : Fixture
{
  ndn::security::KeyChain targetKeys{"pib-memory:restore-target", "tpm-memory:restore-target"};
  ndn::security::Certificate target;
  NativeSavedKvRoleV1 saved;
  NativeSelectionProjectionV3 projection;
  NativeJson encoded;

  RestoreFixture()
  {
    const auto& id = state.originIdentity;
    auto& receipt = saved.receipt;
    receipt.conversationId = "current-native-conversation";
    receipt.parentContextEpoch = 0;
    receipt.successorContextEpoch = 1;
    receipt.originRequestId = id.requestId;
    receipt.originGenerationId = id.generationId;
    receipt.serviceName = "/SERVICE/infer";
    receipt.requesterIdentity = certificate.getIdentity().toUri();
    receipt.securityDomainDigest = id.securityDomainDigest;
    receipt.modelDigest = id.modelDigest;
    receipt.graphSemanticDigest = id.graphSemanticDigest;
    receipt.adapterDigest = id.adapterDigest;
    receipt.roleName = id.roleName;
    receipt.roleSplitDigest = id.roleSplitDigest;
    receipt.layoutDigest = id.layoutDigest;
    receipt.planRoleMapDigest = hash("original Source placement");
    receipt.providerIdentity = id.providerIdentity;
    receipt.providerBootId = id.providerBootId;
    receipt.cacheEpoch = id.cacheEpoch;
    receipt.prefixDigest = id.prefixDigest;
    receipt.prefixTokenCount = id.prefixTokenCount;
    receipt.positionDigest = id.positionDigest;
    receipt.stateSchemaDigest = id.stateSchemaDigest;
    receipt.stateComponentDigests = id.stateComponentDigests;
    receipt.expiresAtMs = 1000;
    saved.protection = nativeProtectPortableKvCheckpoint(plain, cipher, certificate,
      ndn::Name(id.providerIdentity), receipt.computedDigest(), limits);
    const auto source = ownerKeys.createIdentity(ndn::Name(id.providerIdentity), ndn::RsaKeyParams(2048))
      .getDefaultKey().getDefaultCertificate();
    saved.rootName = nativeProtectedKvPacketRootName(saved.protection, source);
    saved.repoNode = "repo";
    saved.repoProvider = "/repo/actual";
    saved.repoServicePrefix = "/repo/service";
    saved.originalWireBytes = saved.protection.wireBytes + 20000;
    saved.originalWireSha256 = std::string(64, 'a');
    saved.ownerEpoch = 1;
    target = targetKeys.createIdentity(ndn::Name("/fresh/provider/target"), ndn::RsaKeyParams(2048))
      .getDefaultKey().getDefaultCertificate();
    projection.provider = target.getIdentity().toUri();
    projection.requestId = "/actual/owner/complete/request/next";
    projection.attempt = 1;
    projection.deadlineMs = 1000;
    projection.planDigest = hash("new sealed plan");
    projection.securityPolicySnapshotDigest = receipt.securityDomainDigest;
    projection.executionRole.roleId = id.roleName;
    projection.hasGrantBinding = true;
    projection.grantName = "/actual/authority/grant/current";
    projection.grantDigest = hash("current grant");
    projection.generationContract.enabled = true;
    projection.generationContract.generationId = std::string(32, 'b');
    projection.conversationStateReference = ConversationStateReferenceV1{
      receipt.conversationId, 1, receipt.serviceName, receipt.planRoleMapDigest,
      hash("aggregate parent checkpoint"), receipt.roleName, receipt.computedDigest(), 1000};
    projection.conversationTurnBinding = ConversationTurnBindingV1{
      receipt.conversationId, 1, 2, receipt.serviceName, hash("new Target placement"),
      hash("current request contract"), 1000, hash("aggregate parent checkpoint")};
    const auto publicKey = target.getPublicKey();
    const auto binding = nativeKvRestoreRecipientBinding(projection, certificate.getIdentity(),
      ndn::Name(receipt.serviceName), target.getName(),
      hash(std::string(reinterpret_cast<const char*>(publicKey.data()), publicKey.size())), 100, 10);
    const auto capsule = nativeWrapProtectedKvKeyForRecipient(saved.protection, certificate,
      ownerKeys, publicKey, binding, [] {});
    encoded = nativeKvRestoreToJson(saved, capsule, binding);
  }

  NativeKvRestoreV1 decode(const NativeJson& value, const NativeSelectionProjectionV3& selected) const
  {
    return nativeReadKvRestore(value, selected, certificate.getIdentity(),
      ndn::Name(saved.receipt.serviceName), 10);
  }
};

} // namespace

BOOST_AUTO_TEST_SUITE(Spec221KvRestore)

BOOST_AUTO_TEST_CASE(CurrentRecipientRecoversHistoricalSourceStateWithFreshPlacement)
{
  RestoreFixture fixture;
  const auto restore = fixture.decode(fixture.encoded, fixture.projection);
  BOOST_CHECK(restore.saved.receipt.planRoleMapDigest !=
    fixture.projection.conversationTurnBinding->planRoleMapDigest);
  BOOST_CHECK_EQUAL(restore.saved.receipt.providerIdentity, fixture.state.originIdentity.providerIdentity);
  nativeRecoverProtectedPortableKvCheckpointForRecipient(fixture.cipher, fixture.restored,
    restore.saved.protection, restore.capsule, fixture.target, fixture.targetKeys,
    restore.recipient, fixture.limits, [] {});
  const auto snapshot = nativeReadPrivatePortableKvCheckpoint(fixture.restored, fixture.portableLimits);
  nativeValidateSavedKvOrigin(snapshot.originIdentity, restore.saved.receipt);
  BOOST_CHECK(snapshot.originIdentity == fixture.state.originIdentity);
  BOOST_CHECK(snapshot.components.front().tensorWire == fixture.state.components.front().tensorWire);
  auto wrong = snapshot.originIdentity;
  wrong.prefixTokenCount++;
  BOOST_CHECK_THROW(nativeValidateSavedKvOrigin(wrong, restore.saved.receipt), std::invalid_argument);
  wrong = snapshot.originIdentity;
  wrong.providerBootId += "-wrong";
  BOOST_CHECK_THROW(nativeValidateSavedKvOrigin(wrong, restore.saved.receipt), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ReboundCurrentAuthorizationAndMalformedReferencesReject)
{
  RestoreFixture fixture;
  const std::vector<std::function<void(NativeSelectionProjectionV3&)>> changes{
    [](auto& p) { p.requestId += "-wrong"; },
    [](auto& p) { ++p.attempt; },
    [](auto& p) { p.provider += "/wrong"; },
    [](auto& p) { p.planDigest = hash("wrong plan"); },
    [](auto& p) { p.grantDigest = hash("wrong grant"); },
    [](auto& p) { p.grantName += "/wrong"; },
    [](auto& p) { p.hasGrantBinding = false; },
    [](auto& p) { p.generationContract.enabled = false; },
    [](auto& p) { p.conversationStateReference->planRoleMapDigest = hash("wrong origin map"); },
    [](auto& p) { p.conversationStateReference->checkpointDigest = hash("wrong checkpoint"); },
    [](auto& p) { p.conversationStateReference->roleReceiptDigest = hash("wrong receipt"); },
    [](auto& p) { p.conversationTurnBinding->parentContextEpoch++; },
  };
  for (const auto& change : changes) {
    auto selected = fixture.projection;
    change(selected);
    BOOST_CHECK_THROW(fixture.decode(fixture.encoded, selected), std::exception);
  }
  auto changed = fixture.encoded;
  changed["expiresAtMs"] = 10;
  BOOST_CHECK_THROW(fixture.decode(changed, fixture.projection), std::exception);
  changed = fixture.encoded;
  changed["expiresAtMs"] = 100.0;
  BOOST_CHECK_THROW(fixture.decode(changed, fixture.projection), std::exception);
  changed = fixture.encoded;
  changed["capsule"]["extra"] = "unsupported";
  BOOST_CHECK_THROW(fixture.decode(changed, fixture.projection), std::exception);
  changed = fixture.encoded;
  changed["savedRole"]["ownerEpoch"] = 0;
  BOOST_CHECK_THROW(fixture.decode(changed, fixture.projection), std::exception);
  BOOST_CHECK_THROW(nativeReadKvRestore(fixture.encoded, fixture.projection,
    ndn::Name("/foreign/owner"), ndn::Name(fixture.saved.receipt.serviceName), 10), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::test
