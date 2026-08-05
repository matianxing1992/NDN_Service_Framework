#pragma once

#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>
#include <ndn-cxx/util/signal.hpp>
#include <ndn-svs/security-options.hpp>
#include <ndn-svs/svspubsub.hpp>

#include <boost/asio/io_context.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ndn_service_framework::test {

enum class EnvironmentStatus
{
  New,
  Bootstrapping,
  Ready,
  RequestActive,
  Failed,
};

struct BootstrapProfile
{
  ndn::Name groupPrefix{ "/ndnsf/spec170" };
  ndn::Name syncPrefix{ "/ndnsf/spec170/sync" };
  ndn::Name userNode{ "/ndnsf/spec170/user" };
  ndn::Name providerNode{ "/ndnsf/spec170/provider" };
  ndn::Name userIdentity{ "/test/user/spec170" };
  ndn::Name providerIdentity{ "/test/provider/spec170" };
  ndn::Name attributeAuthority{ "/test/aa/spec170" };
  ndn::Name serviceName{ "/ObjectDetection/YOLOv8" };
  std::string trustSchemaPath{ "examples/trust-any.conf" };
  size_t providerCount = 1;
};

struct FaultProfile
{
  bool dropPackets = false;
  bool duplicatePackets = false;
  bool reorderPackets = false;
};

struct PacketBridgeStats
{
  size_t forwardedInterests = 0;
  size_t forwardedData = 0;
  size_t droppedPackets = 0;
  size_t duplicatedPackets = 0;
  size_t reorderedPackets = 0;
  std::string firstDroppedName;
  std::string firstPendingName;
};

struct EnvironmentSnapshot
{
  std::string digest;
  std::string configurationDigest;
  size_t permissionEpoch = 0;
  ndn::Name syncPrefix;
  ndn::Name serviceName;
};

struct RequestResidue
{
  size_t liveLeases = 0;
  size_t heldDevices = 0;
  size_t pendingCallbacks = 0;
  size_t replayEntries = 0;
  size_t plaintextArtifacts = 0;

  bool empty() const
  {
    return liveLeases == 0 && heldDevices == 0 && pendingCallbacks == 0 &&
           replayEntries == 0 && plaintextArtifacts == 0;
  }
};

struct RequestScope
{
  std::string requestId;
  std::string snapshotDigest;
  FaultProfile faults;
  RequestResidue residue;
  bool requestPublished = false;
  bool active = true;
};

/**
 * A reusable, fully configured in-process NDNSF environment.
 *
 * Construction allocates the real faces, identities, SVS nodes, and runtime
 * objects. bootstrap() is intentionally a separate readiness gate: request
 * tests cannot accidentally include setup time or begin with missing policy.
 */
class NdnsfIntegrationEnvironment
{
public:
  explicit NdnsfIntegrationEnvironment(BootstrapProfile profile = {});
  ~NdnsfIntegrationEnvironment();

  NdnsfIntegrationEnvironment(const NdnsfIntegrationEnvironment&) = delete;
  NdnsfIntegrationEnvironment& operator=(const NdnsfIntegrationEnvironment&) = delete;

  void bootstrap();
  void pumpUntil(const std::function<bool()>& done);
  void pumpUntilReady();
  RequestScope beginRequest(std::string requestId, FaultProfile faults = {});
  void markRequestPublished(RequestScope& scope);
  void updateRequestResidue(RequestScope& scope, RequestResidue residue);
  void resetRequest(RequestScope& scope);
  void flushReorderedPackets();
  const PacketBridgeStats& bridgeStats() const { return m_bridgeStats; }

  EnvironmentStatus status() const { return m_status; }
  const EnvironmentSnapshot& snapshot() const { return m_snapshot; }
  const BootstrapProfile& profile() const { return m_profile; }
  const std::string& failureReason() const { return m_failureReason; }

  ndn::DummyClientFace& userFace();
  ndn::DummyClientFace& providerFace();
  ndn::DummyClientFace& providerFace(size_t index);
  ndn::KeyChain& keyChain() { return *m_keyChain; }
  ndn::svs::SVSPubSub& userPubSub();
  ndn::svs::SVSPubSub& providerPubSub();
  ndn::svs::SVSPubSub& providerPubSub(size_t index);
  ServiceUser& user();
  ServiceProvider& provider();
  ServiceProvider& provider(size_t index);
  size_t providerCount() const { return 1 + m_extraProviderFaces.size(); }

private:
  void installPermissions();
  void computeSnapshot();
  void fail(std::string reason);
  void forwardInterest(ndn::DummyClientFace& destination,
                       const ndn::Interest& interest,
                       bool userToProvider);
  void forwardData(ndn::DummyClientFace& destination,
                   const ndn::Data& data,
                   bool userToProvider);
  void clearReorderedPackets();

private:
  BootstrapProfile m_profile;
  EnvironmentStatus m_status = EnvironmentStatus::New;
  EnvironmentSnapshot m_snapshot;
  std::string m_failureReason;

  boost::asio::io_context m_userIo;
  boost::asio::io_context m_providerIo;
  std::unique_ptr<ndn::KeyChain> m_keyChain;
  std::unique_ptr<ndn::DummyClientFace> m_userFace;
  std::unique_ptr<ndn::DummyClientFace> m_providerFace;
  std::unique_ptr<ndn::svs::SecurityOptions> m_securityOptions;
  ndn::svs::SVSPubSubOptions m_svsOptions;
  std::unique_ptr<ndn::svs::SVSPubSub> m_userPubSub;
  std::unique_ptr<ndn::svs::SVSPubSub> m_providerPubSub;
  std::vector<std::unique_ptr<ndn::DummyClientFace>> m_extraProviderFaces;
  std::vector<std::unique_ptr<ndn::svs::SVSPubSub>> m_extraProviderPubSubs;
  std::vector<std::unique_ptr<ServiceProvider>> m_extraProviders;
  std::unique_ptr<ServiceUser> m_user;
  std::unique_ptr<ServiceProvider> m_provider;
  ndn::signal::ScopedConnection m_userInterestBridge;
  ndn::signal::ScopedConnection m_providerInterestBridge;
  ndn::signal::ScopedConnection m_userDataBridge;
  ndn::signal::ScopedConnection m_providerDataBridge;
  std::vector<ndn::signal::ScopedConnection> m_extraProviderInterestBridges;
  std::vector<ndn::signal::ScopedConnection> m_extraProviderDataBridges;
  std::vector<ndn::signal::ScopedConnection> m_extraUserInterestBridges;
  std::vector<ndn::signal::ScopedConnection> m_extraUserDataBridges;
  FaultProfile m_activeFaults;
  PacketBridgeStats m_bridgeStats;
  std::optional<ndn::Interest> m_pendingUserInterest;
  std::optional<ndn::Interest> m_pendingProviderInterest;
  std::optional<ndn::Data> m_pendingUserData;
  std::optional<ndn::Data> m_pendingProviderData;
  std::optional<std::string> m_activeRequestId;
};

} // namespace ndn_service_framework::test
