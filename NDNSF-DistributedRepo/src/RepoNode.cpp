#include "ndnsf-distributed-repo/RepoNode.hpp"

#include <exception>

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
  , m_capability(std::move(capability))
{
}

const ndn::Name&
RepoNode::servicePrefix() const
{
  return m_servicePrefix;
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

std::vector<uint8_t>
RepoNode::handleStore(const std::vector<uint8_t>& request)
{
  RepoObjectManifest manifest;
  std::vector<uint8_t> payload;
  decodeStoreRequest(request, manifest, payload);
  if (manifest.size != payload.size()) {
    throw std::invalid_argument("repo object size does not match payload size");
  }
  if (manifest.sha256 != sha256Hex(payload)) {
    throw std::invalid_argument("repo object sha256 does not match payload");
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  m_store.put(manifest, std::move(payload));
  m_capability.usedBytes += manifest.size;
  if (m_capability.freeBytes >= manifest.size) {
    m_capability.freeBytes -= manifest.size;
  }
  else {
    m_capability.freeBytes = 0;
  }
  return toBytes(manifest.toJson());
}

std::vector<uint8_t>
RepoNode::handleFetch(const std::vector<uint8_t>& request) const
{
  const auto objectName = toString(request);
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_store.get(objectName).payload;
}

std::vector<uint8_t>
RepoNode::handleManifest(const std::vector<uint8_t>& request) const
{
  const auto objectName = toString(request);
  std::lock_guard<std::mutex> lock(m_mutex);
  return toBytes(m_store.get(objectName).manifest.toJson());
}

std::vector<uint8_t>
RepoNode::handleInventory() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return toBytes(encodeInventory(m_store.listManifests()));
}

std::vector<uint8_t>
RepoNode::handleCapability() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return toBytes(m_capability.toJson());
}

std::vector<uint8_t>
RepoNode::handleDelete(const std::vector<uint8_t>& request)
{
  const auto objectName = toString(request);
  std::lock_guard<std::mutex> lock(m_mutex);
  const bool removed = m_store.erase(objectName);
  return toBytes(removed ? "deleted" : "not-found");
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
