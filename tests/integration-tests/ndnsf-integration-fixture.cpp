#include "ndnsf-integration-fixture.hpp"

#include "ndn-service-framework/NDNSFMessages.hpp"
#include "ndn-service-framework/utils.hpp"

#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ndn_service_framework::test {
namespace {

ndn::svs::SecurityOptions
makeSecurityOptions(ndn::KeyChain& keyChain)
{
  ndn::svs::SecurityOptions options(keyChain);
  options.interestSigner = std::make_shared<ndn::svs::BaseSigner>();
  options.dataSigner->signingInfo = ndn::security::signingWithSha256();
  options.pubSigner->signingInfo = ndn::security::signingWithSha256();
  options.validator = std::make_shared<ndn::svs::BaseValidator>();
  options.encapsulatedDataValidator = std::make_shared<ndn::svs::BaseValidator>();
  return options;
}

ndn::security::Certificate
makeIdentity(ndn::KeyChain& keyChain, const ndn::Name& identity)
{
  auto id = keyChain.createIdentity(identity, ndn::RsaKeyParams(2048));
  return id.getDefaultKey().getDefaultCertificate();
}

PermissionResponse
makePermissionResponse(const ndn::Name& targetIdentity,
                       size_t permissionKind,
                       const ndn::Name& providerName,
                       const ndn::Name& serviceName)
{
  PermissionEntry entry;
  entry.setProviderName(providerName.toUri());
  entry.setServiceName(serviceName.toUri());
  entry.setToken("");
  entry.setTtl(0);
  entry.setVersion(1);

  PermissionResponse response;
  response.setTargetIdentity(targetIdentity.toUri());
  response.setPermissionKind(permissionKind);
  response.addEntry(entry);
  return response;
}

void
pumpFaces(ndn::DummyClientFace& userFace,
          const std::vector<ndn::DummyClientFace*>& providerFaces,
          const std::function<bool()>& done)
{
  for (int i = 0; i < 200 && !done(); ++i) {
    userFace.processEvents(ndn::time::milliseconds(5));
    for (auto* providerFace : providerFaces) {
      providerFace->processEvents(ndn::time::milliseconds(5));
    }
    userFace.getIoContext().restart();
    for (auto* providerFace : providerFaces) {
      providerFace->getIoContext().restart();
    }
  }
}

ndn::Name
indexedName(const ndn::Name& base, size_t index)
{
  if (index == 0) {
    return base;
  }
  return ndn::Name(base).append("p" + std::to_string(index));
}

} // namespace

NdnsfIntegrationEnvironment::NdnsfIntegrationEnvironment(BootstrapProfile profile)
  : m_profile(std::move(profile))
  , m_keyChain(std::make_unique<ndn::KeyChain>(
      "pib-memory:spec170-fixture", "tpm-memory:spec170-fixture"))
{
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enablePacketLogging = true;
  faceOptions.enableRegistrationReply = true;

  m_userFace = std::make_unique<ndn::DummyClientFace>(m_userIo, *m_keyChain, faceOptions);
  m_providerFace = std::make_unique<ndn::DummyClientFace>(m_providerIo, *m_keyChain, faceOptions);
  m_securityOptions = std::make_unique<ndn::svs::SecurityOptions>(
      makeSecurityOptions(*m_keyChain));
  m_svsOptions.useTimestamp = false;

  m_userPubSub = std::make_unique<ndn::svs::SVSPubSub>(
      m_profile.syncPrefix, m_profile.userNode, *m_userFace,
      [] (const std::vector<ndn::svs::MissingDataInfo>&) {},
      m_svsOptions, *m_securityOptions);
  m_providerPubSub = std::make_unique<ndn::svs::SVSPubSub>(
      m_profile.syncPrefix, m_profile.providerNode, *m_providerFace,
      [] (const std::vector<ndn::svs::MissingDataInfo>&) {},
      m_svsOptions, *m_securityOptions);

  const auto userCert = makeIdentity(*m_keyChain, m_profile.userIdentity);
  const auto providerCert = makeIdentity(*m_keyChain, m_profile.providerIdentity);
  const auto aaCert = makeIdentity(*m_keyChain, m_profile.attributeAuthority);
  m_user = std::make_unique<ServiceUser>(
      ServiceUser::LocalMockTag{}, *m_userFace, m_profile.groupPrefix,
      userCert, aaCert, m_profile.trustSchemaPath);
  m_provider = std::make_unique<ServiceProvider>(
      ServiceProvider::LocalMockTag{}, *m_providerFace, m_profile.groupPrefix,
      providerCert, aaCert, m_profile.trustSchemaPath);

  const auto providerCount = std::max<size_t>(1, m_profile.providerCount);
  for (size_t index = 1; index < providerCount; ++index) {
    auto face = std::make_unique<ndn::DummyClientFace>(
        m_providerIo, *m_keyChain, faceOptions);
    const auto node = indexedName(m_profile.providerNode, index);
    m_extraProviderPubSubs.push_back(std::make_unique<ndn::svs::SVSPubSub>(
        m_profile.syncPrefix, node, *face,
        [] (const std::vector<ndn::svs::MissingDataInfo>&) {},
        m_svsOptions, *m_securityOptions));
    const auto identity = indexedName(m_profile.providerIdentity, index);
    const auto certificate = makeIdentity(*m_keyChain, identity);
    m_extraProviders.push_back(std::make_unique<ServiceProvider>(
        ServiceProvider::LocalMockTag{}, *face, m_profile.groupPrefix,
        certificate, aaCert, m_profile.trustSchemaPath));
    m_extraProviderFaces.push_back(std::move(face));
  }

  m_userInterestBridge = m_userFace->onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        forwardInterest(*m_providerFace, interest, true);
      });
  m_providerInterestBridge = m_providerFace->onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        forwardInterest(*m_userFace, interest, false);
      });
  m_userDataBridge = m_userFace->onSendData.connect(
      [&] (const ndn::Data& data) {
        forwardData(*m_providerFace, data, true);
      });
  m_providerDataBridge = m_providerFace->onSendData.connect(
      [&] (const ndn::Data& data) {
        forwardData(*m_userFace, data, false);
      });
  for (size_t index = 0; index < m_extraProviderFaces.size(); ++index) {
    auto* extraFace = m_extraProviderFaces[index].get();
    m_extraProviderInterestBridges.emplace_back(
        extraFace->onSendInterest.connect(
            [&] (const ndn::Interest& interest) {
              forwardInterest(*m_userFace, interest, false);
            }));
    m_extraProviderDataBridges.emplace_back(
        extraFace->onSendData.connect(
            [&] (const ndn::Data& data) {
              forwardData(*m_userFace, data, false);
            }));
    m_extraUserInterestBridges.emplace_back(m_userFace->onSendInterest.connect(
        [&, extraFace] (const ndn::Interest& interest) {
          forwardInterest(*extraFace, interest, true);
        }));
    m_extraUserDataBridges.emplace_back(m_userFace->onSendData.connect(
        [&, extraFace] (const ndn::Data& data) {
          forwardData(*extraFace, data, true);
        }));
  }
}

NdnsfIntegrationEnvironment::~NdnsfIntegrationEnvironment() = default;

void
NdnsfIntegrationEnvironment::installPermissions()
{
  PermissionResponse userPermissions;
  userPermissions.setTargetIdentity(m_profile.userIdentity.toUri());
  userPermissions.setPermissionKind(tlv::UserPermission);
  userPermissions.setPolicyEpoch(1);
  for (size_t index = 0; index < providerCount(); ++index) {
    auto& providerRuntime = provider(index);
    const auto providerName = providerRuntime.getName();
    PermissionEntry userEntry;
    userEntry.setProviderName(providerName.toUri());
    userEntry.setServiceName(m_profile.serviceName.toUri());
    userEntry.setToken("");
    userEntry.setTtl(0);
    userEntry.setVersion(1);
    userPermissions.addEntry(userEntry);
    providerRuntime.applyPermissionResponse(
        makePermissionResponse(providerName,
                               tlv::ProviderPermission,
                               providerName,
                               m_profile.serviceName));
  }
  m_user->applyPermissionResponse(userPermissions);
}

void
NdnsfIntegrationEnvironment::computeSnapshot()
{
  ndn::util::Sha256 digest;
  digest << m_profile.groupPrefix.toUri()
         << m_profile.syncPrefix.toUri()
         << m_profile.userNode.toUri()
         << m_profile.providerNode.toUri()
         << m_profile.userIdentity.toUri()
         << m_profile.providerIdentity.toUri()
         << m_profile.attributeAuthority.toUri()
         << m_profile.serviceName.toUri()
         << m_profile.trustSchemaPath
         << providerCount();
  for (size_t index = 0; index < providerCount(); ++index) {
    digest << provider(index).getName().toUri()
           << provider(index).getCurrentPolicyEpoch();
  }
  digest << m_user->getCurrentPolicyEpoch();
  m_snapshot.digest = "sha256:" + digest.toString();
  m_snapshot.configurationDigest = m_snapshot.digest;
  m_snapshot.permissionEpoch = m_user->getCurrentPolicyEpoch();
  for (size_t index = 0; index < providerCount(); ++index) {
    m_snapshot.permissionEpoch = std::max(
        m_snapshot.permissionEpoch, provider(index).getCurrentPolicyEpoch());
  }
  m_snapshot.syncPrefix = m_profile.syncPrefix;
  m_snapshot.serviceName = m_profile.serviceName;
}

void
NdnsfIntegrationEnvironment::fail(std::string reason)
{
  m_status = EnvironmentStatus::Failed;
  m_failureReason = std::move(reason);
}

void
NdnsfIntegrationEnvironment::bootstrap()
{
  if (m_status != EnvironmentStatus::New) {
    throw std::logic_error("Spec170 integration environment bootstrap is not NEW");
  }

  m_status = EnvironmentStatus::Bootstrapping;
  try {
    installPermissions();

    ndn::Name publicationName(m_profile.groupPrefix);
    publicationName.append("bootstrap").append("1");
    bool delivered = false;
    m_providerPubSub->subscribeToProducer(
        m_profile.userNode,
        [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
          delivered = publication.name == publicationName;
        },
        false);
    const std::string payload = "spec170-bootstrap";
    m_userPubSub->publish(
        publicationName,
        ndn::span<const uint8_t>(reinterpret_cast<const uint8_t*>(payload.data()),
                                 payload.size()));
    std::vector<ndn::DummyClientFace*> providerFaces;
    providerFaces.reserve(providerCount());
    for (size_t index = 0; index < providerCount(); ++index) {
      providerFaces.push_back(&providerFace(index));
    }
    pumpFaces(*m_userFace, providerFaces, [&] { return delivered; });
    if (!delivered) {
      throw std::runtime_error("SVS bootstrap publication was not delivered");
    }
    if (m_user->getAllowedServices().empty() ||
        m_provider->getCurrentPolicyEpoch() == 0) {
      throw std::runtime_error("permission bootstrap did not install policy");
    }

    computeSnapshot();
    m_status = EnvironmentStatus::Ready;
    std::cout << "NDNSF_INTEGRATION_BOOTSTRAP_READY " << m_snapshot.digest << '\n';
  }
  catch (const std::exception& error) {
    fail(error.what());
    throw;
  }
}

void
NdnsfIntegrationEnvironment::pumpUntilReady()
{
  if (m_status != EnvironmentStatus::Ready) {
    throw std::logic_error("cannot pump a non-READY Spec170 environment");
  }
  pumpUntil([] { return false; });
}

void
NdnsfIntegrationEnvironment::pumpUntil(const std::function<bool()>& done)
{
  std::vector<ndn::DummyClientFace*> providerFaces;
  providerFaces.reserve(providerCount());
  for (size_t index = 0; index < providerCount(); ++index) {
    providerFaces.push_back(&providerFace(index));
  }
  pumpFaces(*m_userFace, providerFaces, done);
}

RequestScope
NdnsfIntegrationEnvironment::beginRequest(std::string requestId, FaultProfile faults)
{
  if (m_status != EnvironmentStatus::Ready) {
    throw std::logic_error("Spec170 request requires a READY environment");
  }
  if (requestId.empty()) {
    throw std::invalid_argument("Spec170 request ID must not be empty");
  }
  if (m_activeRequestId.has_value()) {
    throw std::logic_error("Spec170 environment already has an active request");
  }
  m_activeRequestId = requestId;
  m_activeFaults = faults;
  m_bridgeStats = {};
  clearReorderedPackets();
  m_status = EnvironmentStatus::RequestActive;
  return RequestScope{std::move(requestId), m_snapshot.digest, faults,
                      RequestResidue{}, false, true};
}

void
NdnsfIntegrationEnvironment::markRequestPublished(RequestScope& scope)
{
  if (m_status != EnvironmentStatus::RequestActive ||
      !m_activeRequestId || *m_activeRequestId != scope.requestId || !scope.active) {
    throw std::logic_error("request publication does not belong to active scope");
  }
  scope.requestPublished = true;
  std::cout << "NDNSF_REQUEST_PUBLISHED " << scope.requestId << ' '
            << scope.snapshotDigest << '\n';
}

void
NdnsfIntegrationEnvironment::updateRequestResidue(RequestScope& scope,
                                                  RequestResidue residue)
{
  if (m_status != EnvironmentStatus::RequestActive ||
      !m_activeRequestId || *m_activeRequestId != scope.requestId || !scope.active) {
    throw std::logic_error("request residue does not belong to active scope");
  }
  scope.residue = residue;
}

void
NdnsfIntegrationEnvironment::resetRequest(RequestScope& scope)
{
  if (m_status != EnvironmentStatus::RequestActive ||
      !m_activeRequestId || *m_activeRequestId != scope.requestId || !scope.active) {
    throw std::logic_error("request reset does not belong to active scope");
  }
  if (!scope.requestPublished) {
    throw std::logic_error("request reset requires REQUEST_PUBLISHED boundary");
  }
  if (!scope.residue.empty()) {
    throw std::logic_error("request reset requires zero request residue");
  }
  if (m_pendingUserInterest || m_pendingProviderInterest ||
      m_pendingUserData || m_pendingProviderData) {
    throw std::logic_error("request reset requires reordered packets to be flushed");
  }
  scope.active = false;
  m_activeRequestId.reset();
  m_activeFaults = {};
  m_status = EnvironmentStatus::Ready;
  std::cout << "NDNSF_REQUEST_TERMINAL " << scope.requestId << " RESET\n";
}

void
NdnsfIntegrationEnvironment::flushReorderedPackets()
{
  auto flushInterest = [&] (auto& pending, ndn::DummyClientFace& destination) {
    if (!pending) {
      return;
    }
    destination.receive(*pending);
    ++m_bridgeStats.forwardedInterests;
    pending.reset();
  };
  auto flushData = [&] (auto& pending, ndn::DummyClientFace& destination) {
    if (!pending) {
      return;
    }
    destination.receive(*pending);
    ++m_bridgeStats.forwardedData;
    pending.reset();
  };
  flushInterest(m_pendingUserInterest, *m_providerFace);
  flushInterest(m_pendingProviderInterest, *m_userFace);
  flushData(m_pendingUserData, *m_providerFace);
  flushData(m_pendingProviderData, *m_userFace);
}

void
NdnsfIntegrationEnvironment::clearReorderedPackets()
{
  m_pendingUserInterest.reset();
  m_pendingProviderInterest.reset();
  m_pendingUserData.reset();
  m_pendingProviderData.reset();
}

void
NdnsfIntegrationEnvironment::forwardInterest(ndn::DummyClientFace& destination,
                                             const ndn::Interest& interest,
                                             bool userToProvider)
{
  const bool faultsEnabled = m_status == EnvironmentStatus::RequestActive;
  const auto name = interest.getName().toUri();
  if (!faultsEnabled || (!m_activeFaults.dropPackets &&
                         !m_activeFaults.duplicatePackets &&
                         !m_activeFaults.reorderPackets)) {
    destination.receive(interest);
    ++m_bridgeStats.forwardedInterests;
    return;
  }
  if (m_activeFaults.dropPackets) {
    ++m_bridgeStats.droppedPackets;
    if (m_bridgeStats.firstDroppedName.empty()) {
      m_bridgeStats.firstDroppedName = name;
    }
    return;
  }

  auto deliver = [&] (const ndn::Interest& packet) {
    destination.receive(packet);
    ++m_bridgeStats.forwardedInterests;
  };
  auto deliverWithDuplicate = [&] (const ndn::Interest& packet) {
    deliver(packet);
    if (m_activeFaults.duplicatePackets) {
      deliver(packet);
      ++m_bridgeStats.duplicatedPackets;
    }
  };
  auto& pending = userToProvider ? m_pendingUserInterest : m_pendingProviderInterest;
  if (m_activeFaults.reorderPackets) {
    if (!pending) {
      pending = interest;
      if (m_bridgeStats.firstPendingName.empty()) {
        m_bridgeStats.firstPendingName = name;
      }
      return;
    }
    deliverWithDuplicate(interest);
    deliverWithDuplicate(*pending);
    pending.reset();
    ++m_bridgeStats.reorderedPackets;
    return;
  }
  deliverWithDuplicate(interest);
}

void
NdnsfIntegrationEnvironment::forwardData(ndn::DummyClientFace& destination,
                                          const ndn::Data& data,
                                          bool userToProvider)
{
  const bool faultsEnabled = m_status == EnvironmentStatus::RequestActive;
  const auto name = data.getName().toUri();
  if (!faultsEnabled || (!m_activeFaults.dropPackets &&
                         !m_activeFaults.duplicatePackets &&
                         !m_activeFaults.reorderPackets)) {
    destination.receive(data);
    ++m_bridgeStats.forwardedData;
    return;
  }
  if (m_activeFaults.dropPackets) {
    ++m_bridgeStats.droppedPackets;
    if (m_bridgeStats.firstDroppedName.empty()) {
      m_bridgeStats.firstDroppedName = name;
    }
    return;
  }

  auto deliver = [&] (const ndn::Data& packet) {
    destination.receive(packet);
    ++m_bridgeStats.forwardedData;
  };
  auto deliverWithDuplicate = [&] (const ndn::Data& packet) {
    deliver(packet);
    if (m_activeFaults.duplicatePackets) {
      deliver(packet);
      ++m_bridgeStats.duplicatedPackets;
    }
  };
  auto& pending = userToProvider ? m_pendingUserData : m_pendingProviderData;
  if (m_activeFaults.reorderPackets) {
    if (!pending) {
      pending = data;
      if (m_bridgeStats.firstPendingName.empty()) {
        m_bridgeStats.firstPendingName = name;
      }
      return;
    }
    deliverWithDuplicate(data);
    deliverWithDuplicate(*pending);
    pending.reset();
    ++m_bridgeStats.reorderedPackets;
    return;
  }
  deliverWithDuplicate(data);
}

ndn::DummyClientFace&
NdnsfIntegrationEnvironment::userFace()
{
  return *m_userFace;
}

ndn::DummyClientFace&
NdnsfIntegrationEnvironment::providerFace()
{
  return *m_providerFace;
}

ndn::DummyClientFace&
NdnsfIntegrationEnvironment::providerFace(size_t index)
{
  if (index == 0) {
    return *m_providerFace;
  }
  if (index > m_extraProviderFaces.size()) {
    throw std::out_of_range("Spec170 provider face index out of range");
  }
  return *m_extraProviderFaces[index - 1];
}

ndn::svs::SVSPubSub&
NdnsfIntegrationEnvironment::userPubSub()
{
  return *m_userPubSub;
}

ndn::svs::SVSPubSub&
NdnsfIntegrationEnvironment::providerPubSub()
{
  return *m_providerPubSub;
}

ndn::svs::SVSPubSub&
NdnsfIntegrationEnvironment::providerPubSub(size_t index)
{
  if (index == 0) {
    return *m_providerPubSub;
  }
  if (index > m_extraProviderPubSubs.size()) {
    throw std::out_of_range("Spec170 provider SVS index out of range");
  }
  return *m_extraProviderPubSubs[index - 1];
}

ServiceUser&
NdnsfIntegrationEnvironment::user()
{
  return *m_user;
}

ServiceProvider&
NdnsfIntegrationEnvironment::provider()
{
  return *m_provider;
}

ServiceProvider&
NdnsfIntegrationEnvironment::provider(size_t index)
{
  if (index == 0) {
    return *m_provider;
  }
  if (index > m_extraProviders.size()) {
    throw std::out_of_range("Spec170 provider index out of range");
  }
  return *m_extraProviders[index - 1];
}

} // namespace ndn_service_framework::test
