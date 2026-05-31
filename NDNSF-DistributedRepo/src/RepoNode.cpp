#include "ndnsf-distributed-repo/RepoNode.hpp"

#include "ndn-service-framework/LocalServiceRegistry.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

namespace ndnsf_distributed_repo {

namespace {

std::vector<uint8_t>
payloadOf(const ndn_service_framework::RequestMessage& request)
{
  const auto payload = request.getPayload();
  return std::vector<uint8_t>(payload.begin(), payload.end());
}

} // namespace

RepoNode::RepoNode(ndn::Name servicePrefix, StorageCapability capability)
  : m_servicePrefix(std::move(servicePrefix))
  , m_core(std::move(capability))
{
}

RepoNode::RepoNode(ndn::Name servicePrefix,
                   StorageCapability capability,
                   std::shared_ptr<RepoStoreBackend> store)
  : m_servicePrefix(std::move(servicePrefix))
  , m_core(std::move(capability), std::move(store))
{
}

const ndn::Name&
RepoNode::servicePrefix() const
{
  return m_servicePrefix;
}

RepoCore&
RepoNode::core()
{
  return m_core;
}

const RepoCore&
RepoNode::core() const
{
  return m_core;
}

RepoObjectManifest
RepoNode::put(const std::string& objectName,
              const std::vector<uint8_t>& payload,
              const std::string& objectType,
              uint32_t replicationFactor,
              const std::string& policyEpoch,
              std::vector<std::string> replicaNodes)
{
  return m_core.put(objectName, payload, objectType, replicationFactor,
                    policyEpoch, std::move(replicaNodes));
}

std::vector<uint8_t>
RepoNode::get(const std::string& objectName) const
{
  return m_core.get(objectName);
}

RepoObjectManifest
RepoNode::getManifest(const std::string& objectName) const
{
  return m_core.getManifest(objectName);
}

std::vector<RepoObjectManifest>
RepoNode::list() const
{
  return m_core.list();
}

bool
RepoNode::remove(const std::string& objectName)
{
  return m_core.remove(objectName);
}

void
RepoNode::registerServices(ndn_service_framework::ServiceProvider& provider)
{
  auto ack = [] (const ndn_service_framework::RequestMessage&) {
    ndn_service_framework::ServiceProvider::AckDecision decision;
    decision.status = true;
    decision.message = "repo-ready";
    return decision;
  };

  provider.addService(
    makeRepoServiceName(m_servicePrefix, "STORE"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleStore(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  provider.addService(
    makeRepoServiceName(m_servicePrefix, "STORE_MANIFEST"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleStoreManifest(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  provider.addService(
    makeRepoServiceName(m_servicePrefix, "FETCH"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleFetch(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  provider.addService(
    makeRepoServiceName(m_servicePrefix, "MANIFEST"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleManifest(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  provider.addService(
    makeRepoServiceName(m_servicePrefix, "INVENTORY"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage&) {
      try {
        return makeResponse(handleInventory());
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  provider.addService(
    makeRepoServiceName(m_servicePrefix, "CAPABILITY"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage&) {
      try {
        return makeResponse(handleCapability());
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  provider.addService(
    makeRepoServiceName(m_servicePrefix, "DELETE"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleDelete(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });
}

void
RepoNode::registerLocalServices(ndn_service_framework::LocalServiceRegistry& registry)
{
  registry.registerLocalService(
    makeRepoServiceName(m_servicePrefix, "STORE"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleStore(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registry.registerLocalService(
    makeRepoServiceName(m_servicePrefix, "STORE_MANIFEST"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleStoreManifest(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registry.registerLocalService(
    makeRepoServiceName(m_servicePrefix, "FETCH"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleFetch(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registry.registerLocalService(
    makeRepoServiceName(m_servicePrefix, "MANIFEST"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleManifest(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registry.registerLocalService(
    makeRepoServiceName(m_servicePrefix, "INVENTORY"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn_service_framework::RequestMessage&) {
      try {
        return makeResponse(handleInventory());
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registry.registerLocalService(
    makeRepoServiceName(m_servicePrefix, "CAPABILITY"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn_service_framework::RequestMessage&) {
      try {
        return makeResponse(handleCapability());
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registry.registerLocalService(
    makeRepoServiceName(m_servicePrefix, "DELETE"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleDelete(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });
}

void
RepoNode::registerDeploymentServices(
  ndn_service_framework::ServiceProvider* provider,
  ndn_service_framework::LocalServiceRegistry* registry,
  RepoDeploymentMode mode)
{
  if (enablesRemote(mode)) {
    if (provider == nullptr) {
      throw std::invalid_argument("remote repo deployment mode requires a ServiceProvider");
    }
    registerServices(*provider);
  }

  if (enablesEmbedded(mode)) {
    if (registry == nullptr) {
      throw std::invalid_argument(
        "embedded repo deployment mode requires a LocalServiceRegistry");
    }
    registerLocalServices(*registry);
  }
}

std::vector<uint8_t>
RepoNode::handleStore(const std::vector<uint8_t>& request)
{
  return m_core.handleStore(request);
}

std::vector<uint8_t>
RepoNode::handleStoreManifest(const std::vector<uint8_t>& request)
{
  return m_core.handleStoreManifest(request);
}

std::vector<uint8_t>
RepoNode::handleFetch(const std::vector<uint8_t>& request) const
{
  return m_core.handleFetch(request);
}

std::vector<uint8_t>
RepoNode::handleManifest(const std::vector<uint8_t>& request) const
{
  return m_core.handleManifest(request);
}

std::vector<uint8_t>
RepoNode::handleInventory() const
{
  return m_core.handleInventory();
}

std::vector<uint8_t>
RepoNode::handleCapability() const
{
  return m_core.handleCapability();
}

std::vector<uint8_t>
RepoNode::handleDelete(const std::vector<uint8_t>& request)
{
  return m_core.handleDelete(request);
}

ndn_service_framework::ResponseMessage
RepoNode::makeResponse(const std::vector<uint8_t>& payload) const
{
  ndn::Buffer responsePayload(payload.data(), payload.size());
  ndn_service_framework::ResponseMessage response;
  response.setStatus(true);
  response.setErrorInfo("No error");
  response.setPayload(responsePayload, responsePayload.size());
  return response;
}

ndn_service_framework::ResponseMessage
RepoNode::makeError(const std::string& error) const
{
  ndn_service_framework::ResponseMessage response;
  response.setStatus(false);
  response.setErrorInfo(error);
  auto payload = toBytes(error);
  ndn::Buffer responsePayload(payload.data(), payload.size());
  response.setPayload(responsePayload, responsePayload.size());
  return response;
}

} // namespace ndnsf_distributed_repo
