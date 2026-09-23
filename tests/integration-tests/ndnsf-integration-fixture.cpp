#include "ndnsf-integration-fixture.hpp"

#include "ndn-service-framework/NDNSFMessages.hpp"
#include "ndn-service-framework/utils.hpp"

#include <nac-abe/common.hpp>

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
makeSecurityOptions(ndn::KeyChain& keyChain, const ndn::security::Certificate& cert)
{
  ndn::svs::SecurityOptions options(keyChain);
  options.interestSigner = std::make_shared<ndn::svs::BaseSigner>();
  // Sign publications with the role certificate (production shape) so the
  // request-scoped response transport-owner check can read the KeyLocator.
  options.dataSigner->signingInfo = ndn::security::signingByCertificate(cert);
  options.pubSigner->signingInfo = ndn::security::signingByCertificate(cert);
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
          ndn::DummyClientFace& attributeAuthorityFace,
          bool pumpAttributeAuthority,
          bool pumpProviderFaces,
          const std::function<bool()>& done)
{
  for (int i = 0; i < 200 && !done(); ++i) {
    // NAC-ABE Producers request the Attribute Authority public parameters as
    // soon as they are constructed. Process the authority first so its Data is
    // available before the User/Provider event loops are pumped in this round.
    if (pumpAttributeAuthority) {
      attributeAuthorityFace.processEvents(ndn::time::milliseconds(5));
    }
    userFace.processEvents(ndn::time::milliseconds(5));
    if (pumpProviderFaces) {
      for (auto* providerFace : providerFaces) {
        providerFace->processEvents(ndn::time::milliseconds(5));
      }
    }
    userFace.getIoContext().restart();
    if (pumpAttributeAuthority) {
      attributeAuthorityFace.getIoContext().restart();
    }
    if (pumpProviderFaces) {
      for (auto* providerFace : providerFaces) {
        providerFace->getIoContext().restart();
      }
    }
  }
}

class ProducerSubscriptionGuard
{
public:
  ProducerSubscriptionGuard(ndn::svs::SVSPubSub& pubsub, uint32_t handle)
    : m_pubsub(pubsub)
    , m_handle(handle)
  {
  }

  ProducerSubscriptionGuard(const ProducerSubscriptionGuard&) = delete;
  ProducerSubscriptionGuard& operator=(const ProducerSubscriptionGuard&) = delete;

  ~ProducerSubscriptionGuard() noexcept
  {
    reset();
  }

  void reset() noexcept
  {
    if (!m_active)
      return;
    try {
      m_pubsub.unsubscribe(m_handle);
    }
    catch (...) {
      // Leaving a callback that captures bootstrap-local state alive would
      // be a use-after-scope. Terminate instead of allowing that unsafe
      // state to escape a noexcept cleanup boundary.
      std::terminate();
    }
    m_active = false;
  }

private:
  ndn::svs::SVSPubSub& m_pubsub;
  uint32_t m_handle;
  bool m_active = true;
};

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
  m_attributeAuthorityFace = std::make_unique<ndn::DummyClientFace>(
      m_attributeAuthorityIo, *m_keyChain, faceOptions);
  m_svsOptions.useTimestamp = false;

  const auto userCert = makeIdentity(*m_keyChain, m_profile.userIdentity);
  const auto providerCert = makeIdentity(*m_keyChain, m_profile.providerIdentity);
  const auto aaCert = makeIdentity(*m_keyChain, m_profile.attributeAuthority);
  m_userSecurityOptions = std::make_unique<ndn::svs::SecurityOptions>(
      makeSecurityOptions(*m_keyChain, userCert));
  m_providerSecurityOptions = std::make_unique<ndn::svs::SecurityOptions>(
      makeSecurityOptions(*m_keyChain, providerCert));

  // Production ServiceUser appends a numeric process session to its SVS
  // producer node.  Keep that shape in the in-process fixture so the real
  // provider freshness gate exercises the same contract.
  auto userSvsNode = m_profile.userNode;
  userSvsNode.append("0");
  m_userPubSub = std::make_unique<ndn::svs::SVSPubSub>(
      m_profile.syncPrefix, userSvsNode, *m_userFace,
      [] (const std::vector<ndn::svs::MissingDataInfo>&) {},
      m_svsOptions, *m_userSecurityOptions);
  auto providerSvsNode = m_profile.providerNode;
  providerSvsNode.append("0");
  m_providerPubSub = std::make_unique<ndn::svs::SVSPubSub>(
      m_profile.syncPrefix, providerSvsNode, *m_providerFace,
      [] (const std::vector<ndn::svs::MissingDataInfo>&) {},
      m_svsOptions, *m_providerSecurityOptions);
  m_attributeAuthorityValidator = std::make_unique<ndn::security::ValidatorNull>();
  m_attributeAuthority = std::make_unique<ndn::nacabe::KpAttributeAuthority>(
      aaCert, *m_attributeAuthorityFace, *m_attributeAuthorityValidator, *m_keyChain);

  // Match ServiceController's production KP-ABE policy projection. Providers
  // decrypt REQUEST/SELECTION under /SERVICE/<service>; the User decrypts
  // ACK/RESPONSE under /PERMISSION/<service>.  Public-parameter traffic alone
  // is not sufficient: without these policies the Authority receives DKEY
  // Interests but has no private key to issue for either identity.
  const auto serviceUri = m_profile.serviceName.toUri();
  m_attributeAuthority->addNewPolicy(
      userCert, "/PERMISSION" + serviceUri);
  m_attributeAuthority->addNewPolicy(
      providerCert, "/SERVICE" + serviceUri);
  // AttributeAuthority installs the PUBLIC-PARAMS Interest filter from the
  // asynchronous registerPrefix success callback. Complete that registration
  // before constructing any User/Provider NAC-ABE Producer; otherwise their
  // constructor-time Interests can be delivered before the filter exists and
  // leave an orphaned retry chain that fails a later, longer test.
  for (int round = 0; round < 4; ++round) {
    m_attributeAuthorityFace->processEvents(ndn::time::milliseconds(5));
    m_attributeAuthorityFace->getIoContext().restart();
  }

  // LocalMockTag skips the production controller process, but its NAC-ABE
  // Producer is real and immediately fetches public parameters. Route only the
  // Attribute Authority namespace to a real in-process authority so the
  // fixture reaches a genuinely ready state instead of leaving a retry timer
  // that can fail whichever long-running test happens to cross its deadline.
  const auto isAttributeAuthorityPacket = [this] (const ndn::Name& name) {
    return m_profile.attributeAuthority.isPrefixOf(name);
  };
  m_userAttributeAuthorityInterestBridge = m_userFace->onSendInterest.connect(
      [this, isAttributeAuthorityPacket] (const ndn::Interest& interest) {
        if (isAttributeAuthorityPacket(interest.getName())) {
          ++m_attributeAuthorityPublicParameterInterests;
          deliverInterest(*m_attributeAuthorityFace, interest);
        }
      });
  m_providerAttributeAuthorityInterestBridge = m_providerFace->onSendInterest.connect(
      [this, isAttributeAuthorityPacket] (const ndn::Interest& interest) {
        if (isAttributeAuthorityPacket(interest.getName())) {
          ++m_attributeAuthorityPublicParameterInterests;
          m_attributeAuthorityFace->receive(interest);
        }
      });
  m_attributeAuthorityDataBridge = m_attributeAuthorityFace->onSendData.connect(
      [this, isAttributeAuthorityPacket] (const ndn::Data& data) {
        if (!isAttributeAuthorityPacket(data.getName())) {
          return;
        }
        ++m_attributeAuthorityPublicParameterData;
        deliverData(*m_userFace, data);
        deliverData(*m_providerFace, data);
        for (auto& face : m_extraProviderFaces) {
          deliverData(*face, data);
        }
      });
  m_user = std::make_unique<ServiceUser>(
      ServiceUser::LocalMockTag{}, *m_userFace, m_profile.groupPrefix,
      userCert, aaCert, m_profile.trustSchemaPath);
  m_user->useSigningKeyChainForTest(*m_keyChain);
  m_provider = std::make_unique<ServiceProvider>(
      ServiceProvider::LocalMockTag{}, *m_providerFace, m_profile.groupPrefix,
      providerCert, aaCert, m_profile.trustSchemaPath);
  m_provider->useSigningKeyChainForTest(*m_keyChain);
  // Collaboration handlers can wait for dependency work posted to the Face
  // io_context. Run them off that event loop, as the production constructor
  // does, while keeping generic LocalMockTag unit tests deterministic/inline.
  m_provider->setHandlerThreads(1);
  m_provider->setAckThreads(1);

  const auto providerCount = std::max<size_t>(1, m_profile.providerCount);
  for (size_t index = 1; index < providerCount; ++index) {
    auto face = std::make_unique<ndn::DummyClientFace>(
        m_providerIo, *m_keyChain, faceOptions);
    const auto identity = indexedName(m_profile.providerIdentity, index);
    const auto certificate = makeIdentity(*m_keyChain, identity);
    m_extraProviderSecurityOptions.push_back(
        std::make_unique<ndn::svs::SecurityOptions>(
            makeSecurityOptions(*m_keyChain, certificate)));
    auto node = indexedName(m_profile.providerNode, index);
    node.append("0");
    m_extraProviderPubSubs.push_back(std::make_unique<ndn::svs::SVSPubSub>(
        m_profile.syncPrefix, node, *face,
        [] (const std::vector<ndn::svs::MissingDataInfo>&) {},
        m_svsOptions, *m_extraProviderSecurityOptions.back()));
    m_extraProviderAttributeAuthorityInterestBridges.emplace_back(
        face->onSendInterest.connect(
            [this, isAttributeAuthorityPacket] (const ndn::Interest& interest) {
              if (isAttributeAuthorityPacket(interest.getName())) {
                ++m_attributeAuthorityPublicParameterInterests;
                deliverInterest(*m_attributeAuthorityFace, interest);
              }
            }));
    m_attributeAuthority->addNewPolicy(
        certificate, "/SERVICE" + serviceUri);
    m_extraProviders.push_back(std::make_unique<ServiceProvider>(
        ServiceProvider::LocalMockTag{}, *face, m_profile.groupPrefix,
        certificate, aaCert, m_profile.trustSchemaPath));
    m_extraProviders.back()->useSigningKeyChainForTest(*m_keyChain);
    m_extraProviders.back()->setHandlerThreads(1);
    m_extraProviders.back()->setAckThreads(1);
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

  std::vector<ndn::DummyClientFace*> providerFaces;
  providerFaces.reserve(1 + m_extraProviderFaces.size());
  providerFaces.push_back(m_providerFace.get());
  for (auto& face : m_extraProviderFaces) {
    providerFaces.push_back(face.get());
  }
  for (size_t source = 0; source < providerFaces.size(); ++source) {
    for (size_t destination = 0; destination < providerFaces.size(); ++destination) {
      if (source == destination) {
        continue;
      }
      auto* destinationFace = providerFaces[destination];
      m_providerPeerInterestBridges.emplace_back(
          providerFaces[source]->onSendInterest.connect(
              [this, destinationFace] (const ndn::Interest& interest) {
                forwardInterest(*destinationFace, interest, false);
              }));
      m_providerPeerDataBridges.emplace_back(
          providerFaces[source]->onSendData.connect(
              [this, destinationFace] (const ndn::Data& data) {
                forwardData(*destinationFace, data, false);
              }));
    }
  }
}

NdnsfIntegrationEnvironment::~NdnsfIntegrationEnvironment() = default;

ndn::Name
NdnsfIntegrationEnvironment::attributeAuthorityPublicParametersName() const
{
  ndn::Name name(m_profile.attributeAuthority);
  name.append(ndn::nacabe::PUBLIC_PARAMS);
  name.append(ndn::nacabe::ABE_TYPE_KP_ABE);
  name.appendVersion(m_attributeAuthority->getPublicParametersVersion());
  return name;
}

std::string
NdnsfIntegrationEnvironment::attributeAuthorityPublicParametersDigest() const
{
  const auto wire = m_attributeAuthority->getPublicParametersWire();
  ndn::util::Sha256 digest;
  digest << std::string(reinterpret_cast<const char*>(wire.data()), wire.size());
  return "sha256:" + digest.toString();
}

void
NdnsfIntegrationEnvironment::enableProviderProductionIngressForTest()
{
  auto attach = [] (ServiceProvider& provider, ndn::svs::SVSPubSub& pubSub) {
    auto nonOwning = std::shared_ptr<ndn::svs::SVSPubSub>(
        &pubSub, [] (ndn::svs::SVSPubSub*) {});
    provider.attachLocalMockPubSubForTest(std::move(nonOwning));
    provider.init();
  };

  attach(*m_provider, *m_providerPubSub);
  for (size_t index = 1; index < providerCount(); ++index) {
    attach(*m_extraProviders[index - 1], *m_extraProviderPubSubs[index - 1]);
  }
}

void
NdnsfIntegrationEnvironment::enableProductionIngressForTest()
{
  auto userPubSub = std::shared_ptr<ndn::svs::SVSPubSub>(
      m_userPubSub.get(), [] (ndn::svs::SVSPubSub*) {});
  m_user->attachLocalMockPubSubForTest(std::move(userPubSub));
  m_user->init();
  enableProviderProductionIngressForTest();
  m_user->refreshNacProducerForTest();
  for (size_t index = 0; index < providerCount(); ++index) {
    provider(index).refreshNacProducerForTest();
  }

  // init() installs the production ingress registrations and can expose a
  // controller-status revalidation that re-arms NAC-ABE caches after the
  // initial bootstrap gate.  Re-establish the same explicit producer and
  // consumer readiness boundary before a request is allowed to publish an
  // encrypted large object; otherwise the first request can observe an empty
  // producer public-parameter cache even though bootstrap() was READY.
  pumpUntilWithAttributeAuthority([&] {
    if (!m_user->isNacConsumerReadyForTest() ||
        !m_user->isNacProducerReadyForTest()) {
      return false;
    }
    for (size_t index = 0; index < providerCount(); ++index) {
      if (!provider(index).isNacConsumerReadyForTest() ||
          !provider(index).isNacProducerReadyForTest()) {
        return false;
      }
    }
    return true;
  });
}

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
    auto providerPermissions = makePermissionResponse(
        providerName, tlv::ProviderPermission, providerName,
        m_profile.serviceName);
    for (const auto& role : m_profile.providerRoles) {
      PermissionEntry roleEntry;
      roleEntry.setProviderName(providerName.toUri());
      ndn::Name rolePermission(m_profile.serviceName);
      rolePermission.append("ROLE");
      if (!role.empty() && role.front() == '/') {
        rolePermission.append(ndn::Name(role));
      }
      else {
        rolePermission.append(role);
      }
      roleEntry.setServiceName(rolePermission.toUri());
      roleEntry.setToken("");
      roleEntry.setTtl(0);
      roleEntry.setVersion(1);
      providerPermissions.addEntry(roleEntry);
    }
    providerRuntime.applyPermissionResponse(providerPermissions);
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
    m_user->refreshNacProducerForTest();
    for (size_t index = 0; index < providerCount(); ++index) {
      provider(index).refreshNacProducerForTest();
    }

    ndn::Name publicationName(m_profile.groupPrefix);
    publicationName.append("bootstrap").append("1");
    bool delivered = false;
    const auto bootstrapSubscription = m_providerPubSub->subscribeToProducer(
        m_profile.userNode,
        [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
          delivered = publication.name == publicationName;
        },
        false);
    ProducerSubscriptionGuard bootstrapSubscriptionGuard(
      *m_providerPubSub, bootstrapSubscription);
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
    pumpFaces(*m_userFace, providerFaces, *m_attributeAuthorityFace, true,
              true, [&] {
      if (!delivered || m_attributeAuthorityPublicParameterData == 0 ||
          !m_user->isNacConsumerReadyForTest() ||
          !m_user->isNacProducerReadyForTest()) {
        return false;
      }
      for (size_t index = 0; index < providerCount(); ++index) {
        if (!provider(index).isNacConsumerReadyForTest() ||
            !provider(index).isNacProducerReadyForTest()) {
          return false;
        }
      }
      return true;
    });
    // The callback captures bootstrap-local state; release it before any
    // later validation can throw while the locals are still in scope.
    bootstrapSubscriptionGuard.reset();
    if (!delivered) {
      throw std::runtime_error("SVS bootstrap publication was not delivered");
    }
    if (m_attributeAuthorityPublicParameterInterests == 0 ||
        m_attributeAuthorityPublicParameterData == 0) {
      throw std::runtime_error(
          "NAC-ABE public-parameter bootstrap was not completed");
    }
    if (!m_user->isNacConsumerReadyForTest()) {
      throw std::runtime_error("User NAC-ABE DKEY bootstrap was not completed");
    }
    if (!m_user->isNacProducerReadyForTest()) {
      throw std::runtime_error(
          "User NAC-ABE producer public-parameter bootstrap was not completed");
    }
    for (size_t index = 0; index < providerCount(); ++index) {
      if (!provider(index).isNacConsumerReadyForTest()) {
        throw std::runtime_error(
            "Provider NAC-ABE DKEY bootstrap was not completed");
      }
      if (!provider(index).isNacProducerReadyForTest()) {
        throw std::runtime_error(
            "Provider NAC-ABE producer public-parameter bootstrap was not completed");
      }
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
NdnsfIntegrationEnvironment::pumpUntilWithAttributeAuthority(
    const std::function<bool()>& done)
{
  std::vector<ndn::DummyClientFace*> providerFaces;
  providerFaces.reserve(providerCount());
  for (size_t index = 0; index < providerCount(); ++index) {
    providerFaces.push_back(&providerFace(index));
  }
  pumpFaces(*m_userFace, providerFaces, *m_attributeAuthorityFace, true,
            !m_profile.providerFacesHaveDedicatedIoWorkers, done);
}

void
NdnsfIntegrationEnvironment::pumpUntil(const std::function<bool()>& done)
{
  std::vector<ndn::DummyClientFace*> providerFaces;
  providerFaces.reserve(providerCount());
  for (size_t index = 0; index < providerCount(); ++index) {
    providerFaces.push_back(&providerFace(index));
  }
  // Public-parameter bootstrap is complete before READY. Keeping the AA face
  // in the request pump would add an unrelated 5 ms wait to every round and
  // perturb the stream timeout/replacement state machine under test.
  pumpFaces(*m_userFace, providerFaces, *m_attributeAuthorityFace, false,
            !m_profile.providerFacesHaveDedicatedIoWorkers, done);
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
  {
    std::lock_guard<std::mutex> lock(m_bridgeMutex);
    if (m_activeRequestId.has_value()) {
      throw std::logic_error("Spec170 environment already has an active request");
    }
    m_activeRequestId = requestId;
    m_activeFaults = faults;
    m_bridgeStats = {};
    m_pendingUserInterest.reset();
    m_pendingProviderInterest.reset();
    m_pendingUserData.reset();
    m_pendingProviderData.reset();
    m_pendingStreamProviderData.reset();
    m_status = EnvironmentStatus::RequestActive;
  }
  return RequestScope{std::move(requestId), m_snapshot.digest, std::move(faults),
                      RequestResidue{}, false, true};
}

void
NdnsfIntegrationEnvironment::markRequestPublished(RequestScope& scope)
{
  std::lock_guard<std::mutex> lock(m_bridgeMutex);
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
  std::lock_guard<std::mutex> lock(m_bridgeMutex);
  if (m_status != EnvironmentStatus::RequestActive ||
      !m_activeRequestId || *m_activeRequestId != scope.requestId || !scope.active) {
    throw std::logic_error("request residue does not belong to active scope");
  }
  scope.residue = residue;
}

void
NdnsfIntegrationEnvironment::resetRequest(RequestScope& scope)
{
  std::lock_guard<std::mutex> lock(m_bridgeMutex);
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
      m_pendingUserData || m_pendingProviderData ||
      m_pendingStreamProviderData) {
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
  std::optional<ndn::Interest> pendingUserInterest;
  std::optional<ndn::Interest> pendingProviderInterest;
  std::optional<ndn::Data> pendingUserData;
  std::optional<ndn::Data> pendingProviderData;
  std::optional<ndn::Data> pendingStreamProviderData;
  {
    std::lock_guard<std::mutex> lock(m_bridgeMutex);
    pendingUserInterest = std::move(m_pendingUserInterest);
    pendingProviderInterest = std::move(m_pendingProviderInterest);
    pendingUserData = std::move(m_pendingUserData);
    pendingProviderData = std::move(m_pendingProviderData);
    pendingStreamProviderData = std::move(m_pendingStreamProviderData);
  }
  auto flushInterest = [&] (auto& pending, ndn::DummyClientFace& destination) {
    if (!pending) {
      return;
    }
    deliverInterest(destination, *pending);
    {
      std::lock_guard<std::mutex> lock(m_bridgeMutex);
      ++m_bridgeStats.forwardedInterests;
    }
    pending.reset();
  };
  auto flushData = [&] (auto& pending, ndn::DummyClientFace& destination) {
    if (!pending) {
      return;
    }
    deliverData(destination, *pending);
    {
      std::lock_guard<std::mutex> lock(m_bridgeMutex);
      ++m_bridgeStats.forwardedData;
    }
    pending.reset();
  };
  flushInterest(pendingUserInterest, *m_providerFace);
  flushInterest(pendingProviderInterest, *m_userFace);
  flushData(pendingUserData, *m_providerFace);
  flushData(pendingProviderData, *m_userFace);
  flushData(pendingStreamProviderData, *m_userFace);
}

PacketBridgeStats
NdnsfIntegrationEnvironment::bridgeStats() const
{
  std::lock_guard<std::mutex> lock(m_bridgeMutex);
  return m_bridgeStats;
}

void
NdnsfIntegrationEnvironment::deliverInterest(ndn::DummyClientFace& destination,
                                             const ndn::Interest& interest)
{
  if (!m_profile.deferBridgeDelivery) {
    destination.receive(interest);
    return;
  }
  // Keep bridge delivery out of the sender's DummyFace callback stack. The
  // destination face and its io_context are owned by this environment; any
  // queued callback is either drained by pumpFaces while the environment is
  // alive or discarded by io_context teardown.
  destination.getIoContext().post(
      [&destination, packet = interest] { destination.receive(packet); });
}

void
NdnsfIntegrationEnvironment::deliverData(ndn::DummyClientFace& destination,
                                         const ndn::Data& data)
{
  if (!m_profile.deferBridgeDelivery) {
    destination.receive(data);
    return;
  }
  destination.getIoContext().post(
      [&destination, packet = data] { destination.receive(packet); });
}

void
NdnsfIntegrationEnvironment::clearReorderedPackets()
{
  std::lock_guard<std::mutex> lock(m_bridgeMutex);
  m_pendingUserInterest.reset();
  m_pendingProviderInterest.reset();
  m_pendingUserData.reset();
  m_pendingProviderData.reset();
  m_pendingStreamProviderData.reset();
}

void
NdnsfIntegrationEnvironment::forwardInterest(ndn::DummyClientFace& destination,
                                             const ndn::Interest& interest,
                                             bool userToProvider)
{
  const auto name = interest.getName().toUri();
  FaultProfile faults;
  bool faultsEnabled = false;
  {
    std::lock_guard<std::mutex> lock(m_bridgeMutex);
    faults = m_activeFaults;
    faultsEnabled = m_status == EnvironmentStatus::RequestActive;
  }
  if (faultsEnabled && userToProvider && faults.dropStreamInterestPredicate &&
      faults.dropStreamInterestPredicate(interest)) {
    std::lock_guard<std::mutex> lock(m_bridgeMutex);
    ++m_bridgeStats.droppedPackets;
    ++m_bridgeStats.droppedStreamInterests;
    if (m_bridgeStats.firstDroppedName.empty())
      m_bridgeStats.firstDroppedName = name;
    return;
  }
  if (faultsEnabled && userToProvider &&
      faults.dropStreamInterestCursor != 0) {
    const auto parsed = parseInvocationEventName(interest.getName());
    if (parsed && parsed->cursor == faults.dropStreamInterestCursor) {
      std::lock_guard<std::mutex> lock(m_bridgeMutex);
      if (m_bridgeStats.droppedStreamInterests < faults.dropStreamInterestCount) {
        ++m_bridgeStats.droppedPackets;
        ++m_bridgeStats.droppedStreamInterests;
        if (m_bridgeStats.firstDroppedName.empty())
          m_bridgeStats.firstDroppedName = name;
        return;
      }
    }
  }
  if (!faultsEnabled || (!faults.dropPackets && !faults.duplicatePackets &&
                         !faults.reorderPackets)) {
    {
      std::lock_guard<std::mutex> lock(m_bridgeMutex);
      ++m_bridgeStats.forwardedInterests;
    }
    deliverInterest(destination, interest);
    return;
  }
  if (faults.dropPackets) {
    std::lock_guard<std::mutex> lock(m_bridgeMutex);
    ++m_bridgeStats.droppedPackets;
    if (m_bridgeStats.firstDroppedName.empty())
      m_bridgeStats.firstDroppedName = name;
    return;
  }

  std::vector<ndn::Interest> deliveries;
  if (faults.reorderPackets) {
    std::optional<ndn::Interest> pending;
    {
      std::lock_guard<std::mutex> lock(m_bridgeMutex);
      auto& held = userToProvider ? m_pendingUserInterest : m_pendingProviderInterest;
      if (!held) {
        held = interest;
        if (m_bridgeStats.firstPendingName.empty())
          m_bridgeStats.firstPendingName = name;
        return;
      }
      pending = std::move(held);
      held.reset();
      const size_t copies = faults.duplicatePackets ? 2 : 1;
      m_bridgeStats.forwardedInterests += 2 * copies;
      if (faults.duplicatePackets)
        m_bridgeStats.duplicatedPackets += 2;
      ++m_bridgeStats.reorderedPackets;
    }
    deliveries.push_back(interest);
    if (faults.duplicatePackets) {
      deliveries.push_back(interest);
    }
    deliveries.push_back(std::move(*pending));
    if (faults.duplicatePackets)
      deliveries.push_back(deliveries.back());
  }
  else {
    {
      std::lock_guard<std::mutex> lock(m_bridgeMutex);
      const size_t copies = faults.duplicatePackets ? 2 : 1;
      m_bridgeStats.forwardedInterests += copies;
      if (faults.duplicatePackets)
        ++m_bridgeStats.duplicatedPackets;
    }
    deliveries.push_back(interest);
    if (faults.duplicatePackets)
      deliveries.push_back(interest);
  }
  for (const auto& packet : deliveries)
    deliverInterest(destination, packet);
}

void
NdnsfIntegrationEnvironment::forwardData(ndn::DummyClientFace& destination,
                                          const ndn::Data& data,
                                          bool userToProvider)
{
  const auto name = data.getName().toUri();
  FaultProfile faults;
  bool faultsEnabled = false;
  {
    std::lock_guard<std::mutex> lock(m_bridgeMutex);
    faults = m_activeFaults;
    faultsEnabled = m_status == EnvironmentStatus::RequestActive;
  }
  const auto streamEvent = !userToProvider && faultsEnabled
    ? parseInvocationEventName(data.getName())
    : std::optional<ParsedInvocationEventName>{};
  if (streamEvent && faults.tamperStreamDataCursor != 0 &&
      streamEvent->cursor == faults.tamperStreamDataCursor) {
    bool tamper = false;
    {
      std::lock_guard<std::mutex> lock(m_bridgeMutex);
      if (m_bridgeStats.tamperedStreamDataPackets < faults.tamperStreamDataCount) {
        ++m_bridgeStats.forwardedData;
        ++m_bridgeStats.tamperedStreamDataPackets;
        tamper = true;
      }
    }
    if (tamper) {
    auto tampered = data;
    const auto content = tampered.getContent();
    ndn::Buffer altered(content.value(), content.value_size());
    if (!altered.empty()) {
      altered[0] ^= 0x01;
    }
    tampered.setContent(altered);
    deliverData(destination, tampered);
    return;
    }
  }
  if (streamEvent && faults.dropStreamDataCursor != 0 &&
      streamEvent->cursor == faults.dropStreamDataCursor) {
    std::lock_guard<std::mutex> lock(m_bridgeMutex);
    if (m_bridgeStats.droppedStreamDataPackets < faults.dropStreamDataCount) {
      ++m_bridgeStats.droppedPackets;
      ++m_bridgeStats.droppedStreamDataPackets;
      if (m_bridgeStats.firstDroppedName.empty())
        m_bridgeStats.firstDroppedName = name;
      return;
    }
  }
  if (streamEvent && faults.reorderStreamDataCursor != 0) {
    const auto heldCursor = faults.reorderStreamDataCursor;
    std::optional<ndn::Data> pending;
    if (streamEvent->cursor == heldCursor) {
      std::lock_guard<std::mutex> lock(m_bridgeMutex);
      if (!m_pendingStreamProviderData) {
        m_pendingStreamProviderData = data;
        if (m_bridgeStats.firstPendingName.empty())
          m_bridgeStats.firstPendingName = name;
        return;
      }
    }
    if (streamEvent->cursor == heldCursor + 1) {
      {
        std::lock_guard<std::mutex> lock(m_bridgeMutex);
        if (m_pendingStreamProviderData) {
          pending = std::move(m_pendingStreamProviderData);
          m_pendingStreamProviderData.reset();
          m_bridgeStats.forwardedData += 2;
          ++m_bridgeStats.reorderedPackets;
          ++m_bridgeStats.reorderedStreamDataPairs;
        }
      }
      if (pending) {
      deliverData(destination, data);
        deliverData(destination, *pending);
        return;
      }
    }
  }
  if (streamEvent && faults.duplicateStreamDataCursor != 0 &&
      streamEvent->cursor == faults.duplicateStreamDataCursor) {
    bool duplicate = false;
    {
      std::lock_guard<std::mutex> lock(m_bridgeMutex);
      if (m_bridgeStats.duplicatedStreamDataPackets < faults.duplicateStreamDataCount) {
        m_bridgeStats.forwardedData += 2;
        ++m_bridgeStats.duplicatedPackets;
        ++m_bridgeStats.duplicatedStreamDataPackets;
        duplicate = true;
      }
    }
    if (duplicate) {
    deliverData(destination, data);
    deliverData(destination, data);
    return;
    }
  }
  if (!faultsEnabled || (!faults.dropPackets && !faults.duplicatePackets &&
                         !faults.reorderPackets)) {
    {
      std::lock_guard<std::mutex> lock(m_bridgeMutex);
      ++m_bridgeStats.forwardedData;
    }
    deliverData(destination, data);
    return;
  }
  if (faults.dropPackets) {
    std::lock_guard<std::mutex> lock(m_bridgeMutex);
    ++m_bridgeStats.droppedPackets;
    if (m_bridgeStats.firstDroppedName.empty())
      m_bridgeStats.firstDroppedName = name;
    return;
  }
  std::vector<ndn::Data> deliveries;
  if (faults.reorderPackets) {
    std::optional<ndn::Data> pending;
    {
      std::lock_guard<std::mutex> lock(m_bridgeMutex);
      auto& held = userToProvider ? m_pendingUserData : m_pendingProviderData;
      if (!held) {
        held = data;
        if (m_bridgeStats.firstPendingName.empty())
          m_bridgeStats.firstPendingName = name;
        return;
      }
      pending = std::move(held);
      held.reset();
      const size_t copies = faults.duplicatePackets ? 2 : 1;
      m_bridgeStats.forwardedData += 2 * copies;
      if (faults.duplicatePackets)
        m_bridgeStats.duplicatedPackets += 2;
      ++m_bridgeStats.reorderedPackets;
    }
    deliveries.push_back(data);
    if (faults.duplicatePackets) {
      deliveries.push_back(data);
    }
    deliveries.push_back(std::move(*pending));
    if (faults.duplicatePackets)
      deliveries.push_back(deliveries.back());
  }
  else {
    {
      std::lock_guard<std::mutex> lock(m_bridgeMutex);
      const size_t copies = faults.duplicatePackets ? 2 : 1;
      m_bridgeStats.forwardedData += copies;
      if (faults.duplicatePackets)
        ++m_bridgeStats.duplicatedPackets;
    }
    deliveries.push_back(data);
    if (faults.duplicatePackets)
      deliveries.push_back(data);
  }
  for (const auto& packet : deliveries)
    deliverData(destination, packet);
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

void
NdnsfIntegrationEnvironment::disconnectProviderTransportForTest(size_t index)
{
  if (index >= providerCount()) {
    throw std::out_of_range("Spec170 provider transport index out of range");
  }
  if (index == 0) {
    m_userInterestBridge.disconnect();
    m_userDataBridge.disconnect();
    m_providerInterestBridge.disconnect();
    m_providerDataBridge.disconnect();
    return;
  }
  const auto extraIndex = index - 1;
  m_extraUserInterestBridges.at(extraIndex).disconnect();
  m_extraUserDataBridges.at(extraIndex).disconnect();
  m_extraProviderInterestBridges.at(extraIndex).disconnect();
  m_extraProviderDataBridges.at(extraIndex).disconnect();
}

void
NdnsfIntegrationEnvironment::disconnectProviderPeerTransportForTest()
{
  for (auto& connection : m_providerPeerInterestBridges) {
    connection.disconnect();
  }
  for (auto& connection : m_providerPeerDataBridges) {
    connection.disconnect();
  }
}

} // namespace ndn_service_framework::test
