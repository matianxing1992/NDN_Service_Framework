#include "HistoricalReadClient.hpp"

#include "AbeHistoricalContent.hpp"
#include "AbeHistoricalKeyRequest.hpp"

#include <chrono>
#include <memory>

#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/segment-fetcher.hpp>

namespace ndn_service_framework {
namespace {

uint64_t
nowMs()
{
  return static_cast<uint64_t>(ndn::time::duration_cast<ndn::time::milliseconds>(
    ndn::time::system_clock::now().time_since_epoch()).count());
}

/** Fetch one Data packet and require the producer's signature on it. */
std::optional<ndn::Data>
fetchVerified(ndn::Face& face, const ndn::Name& name, const ndn::security::Certificate& signer,
              uint32_t timeoutMs)
{
  struct State
  {
    std::optional<ndn::Data> data;
    bool done = false;
  };
  auto state = std::make_shared<State>();
  ndn::Interest interest(name);
  interest.setInterestLifetime(ndn::time::milliseconds(timeoutMs));
  interest.setMustBeFresh(false);
  face.expressInterest(interest,
    [state, signer] (const ndn::Interest&, const ndn::Data& data) {
      try {
        if (ndn::security::verifySignature(data, signer.getPublicKey()))
          state->data = data;
      }
      catch (const std::exception&) {
      }
      state->done = true;
    },
    [state] (const ndn::Interest&, const ndn::lp::Nack&) { state->done = true; },
    [state] (const ndn::Interest&) { state->done = true; });
  const auto deadline = std::chrono::steady_clock::now() +
    std::chrono::milliseconds(timeoutMs);
  while (!state->done && std::chrono::steady_clock::now() < deadline)
    face.processEvents(ndn::time::milliseconds(10));
  return state->data;
}

} // namespace

std::optional<std::vector<uint8_t>>
fetchSingleData(ndn::Face& face, const ndn::Name& name, const ndn::security::Certificate& signer,
                uint32_t timeoutMs)
{
  const auto data = fetchVerified(face, name, signer, timeoutMs);
  if (!data)
    return std::nullopt;
  return std::vector<uint8_t>(data->getContent().value(),
                              data->getContent().value() + data->getContent().value_size());
}

std::optional<std::vector<uint8_t>>
fetchSegmentedData(ndn::Face& face, const ndn::Name& root, const ndn::security::Certificate& signer,
                   uint32_t maxSegments, uint32_t timeoutMs)
{
  if (root.empty() || signer.getName().empty())
    return std::nullopt;
  std::vector<uint8_t> merged;
  for (uint32_t index = 0; index < maxSegments; ++index) {
    ndn::Name segment(root);
    segment.appendSegment(index);
    const auto data = fetchVerified(face, segment, signer, timeoutMs);
    if (!data)
      break;
    merged.insert(merged.end(), data->getContent().value(),
                  data->getContent().value() + data->getContent().value_size());
  }
  if (merged.empty())
    return std::nullopt;
  return merged;
}

std::optional<std::vector<uint8_t>>
fetchHistoricalKey(ndn::Face& face, ndn::KeyChain& keyChain, const HistoricalReadRequest& request,
                   uint32_t timeoutMs)
{
  if (request.generation == 0 || request.ckName.empty() ||
      request.recipientCertificate.getName().empty())
    return std::nullopt;

  HistoricalKeyRequest keyRequest;
  keyRequest.controllerPrefix = request.controllerPrefix;
  keyRequest.generation = request.generation;
  keyRequest.epoch = request.epoch;
  keyRequest.objectName = request.objectName;
  keyRequest.producerName = request.producerName;
  keyRequest.serviceName = request.serviceName;
  keyRequest.ckName = request.ckName;
  keyRequest.policy = request.policy;
  keyRequest.recipientCertificate = request.recipientCertificate.getName();
  const auto now = nowMs();
  keyRequest.mintedAtMs = now;
  keyRequest.expiresAtMs = now + 120000;

  ndn::Interest interest;
  try {
    signHistoricalKeyRequest(interest, keyRequest, keyChain,
      ndn::security::signingByCertificate(request.recipientCertificate));
  }
  catch (const std::exception&) {
    return std::nullopt;
  }
  interest.setInterestLifetime(ndn::time::milliseconds(timeoutMs));

  struct State
  {
    std::optional<ndn::Data> reply;
    bool done = false;
  };
  auto state = std::make_shared<State>();
  face.expressInterest(interest,
    [state] (const ndn::Interest&, const ndn::Data& data) {
      state->reply = data;
      state->done = true;
    },
    [state] (const ndn::Interest&, const ndn::lp::Nack&) { state->done = true; },
    [state] (const ndn::Interest&) { state->done = true; });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (!state->done && std::chrono::steady_clock::now() < deadline)
    face.processEvents(ndn::time::milliseconds(10));
  if (!state->reply)
    return std::nullopt;

  HistoricalKeyReply accepted;
  if (!verifyHistoricalKeyReply(*state->reply, keyRequest, nowMs(), accepted))
    return std::nullopt;
  std::string keyBytes;
  if (!unsealHistoricalKeyReply(accepted, keyChain.getTpm(),
                                request.recipientCertificate.getName(), keyBytes))
    return std::nullopt;
  return std::vector<uint8_t>(keyBytes.begin(), keyBytes.end());
}

std::optional<std::string>
readHistoricalObject(ndn::Face& face, ndn::KeyChain& keyChain, const HistoricalReadRequest& request)
{
  if (request.producerCertificate.getName().empty() || request.ckName.empty() ||
      request.payloadName.empty())
    return std::nullopt;
  // The content key is one packet under its exact name; only the payload is segmented.
  const auto ckWire = fetchSingleData(face, request.ckName, request.producerCertificate,
                                      request.fetchTimeoutMs);
  if (!ckWire)
    return std::nullopt;
  const auto payloadWire = fetchSegmentedData(face, request.payloadName, request.producerCertificate,
                                              request.maxSegments, request.fetchTimeoutMs);
  if (!payloadWire)
    return std::nullopt;
  HistoricalCkContent ck;
  HistoricalPayloadContent payload;
  if (!parseHistoricalCkContent(std::string(ckWire->begin(), ckWire->end()), ck) ||
      !parseHistoricalPayloadContent(std::string(payloadWire->begin(), payloadWire->end()), payload))
    return std::nullopt;
  const auto key = fetchHistoricalKey(face, keyChain, request, request.fetchTimeoutMs * 4);
  if (!key || request.publicParameters.empty())
    return std::nullopt;
  return decryptHistoricalPayload(request.publicParameters, ck, payload, *key);
}

} // namespace ndn_service_framework
