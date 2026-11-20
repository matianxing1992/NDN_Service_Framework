#include "HistoricalReadClient.hpp"

#include "AbeHistoricalContent.hpp"
#include "AbeHistoricalKeyRequest.hpp"
#include "AbePublicParametersArchive.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/segment-fetcher.hpp>
#include <sys/socket.h>
#include <sys/un.h>

namespace ndn_service_framework {
namespace {

/** Opt-in diagnostics: unset in production, set only while a qualification run is being explained. */
bool
historicalReadDebug()
{
  static const bool enabled = std::getenv("NDNSF_HISTORICAL_READ_DEBUG") != nullptr;
  return enabled;
}

#define HISTORICAL_READ_DEBUG(...) \
  do { if (historicalReadDebug()) std::fprintf(stderr, __VA_ARGS__); } while (false)

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
  // Whether the Interest is ever expressed is a fact only this side can report: with the send path unreached the
  // forwarder sees management commands and nothing else.
  HISTORICAL_READ_DEBUG("historical read: expressing %s\n", name.toUri().c_str());
  face.expressInterest(interest,
    [state, signer] (const ndn::Interest&, const ndn::Data& data) {
      try {
        if (ndn::security::verifySignature(data, signer.getPublicKey()))
          state->data = data;
        else
          HISTORICAL_READ_DEBUG("historical read: %s signature did not verify against %s\n",
                                data.getName().toUri().c_str(), signer.getName().toUri().c_str());
      }
      catch (const std::exception& error) {
        HISTORICAL_READ_DEBUG("historical read: %s verification error: %s\n",
                              data.getName().toUri().c_str(), error.what());
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
  // The first segment names the last one. Without that, a missing segment in the middle looks like the end of the
  // object and the caller accepts a truncated payload as complete; with it, truncation is an error.
  std::optional<uint64_t> finalSegment;
  for (uint32_t index = 0; index < maxSegments; ++index) {
    ndn::Name segment(root);
    segment.appendSegment(index);
    const auto data = fetchVerified(face, segment, signer, timeoutMs);
    if (!data) {
      if (index == 0)
        return std::nullopt;
      // A miss is truncation only when the last segment is known: producers that publish a single segment
      // without a FinalBlockId end the object simply by not answering.
      if (finalSegment)
        return std::nullopt;
      break;
    }
    // A producer may answer a segment request with the object's root name (the fixture does) or with the exact
    // segment name; anything else is a different object and is refused.
    const auto answered = data->getName();
    if (!answered.equals(segment) &&
        !(answered.size() <= segment.size() &&
          segment.getSubName(0, answered.size()).equals(answered)))
      return std::nullopt;
    if (!finalSegment) {
      const auto& finalBlock = data->getMetaInfo().getFinalBlock();
      if (finalBlock && finalBlock->isSegment()) {
        finalSegment = finalBlock->toSegment();
        if (*finalSegment + 1 > maxSegments)
          return std::nullopt;
      }
    }
    merged.insert(merged.end(), data->getContent().value(),
                  data->getContent().value() + data->getContent().value_size());
    if (finalSegment && index >= *finalSegment)
      break;
  }
  if (merged.empty() || (finalSegment && merged.size() == 0))
    return std::nullopt;
  return merged;
}

std::optional<std::vector<uint8_t>>
fetchHistoricalKey(ndn::Face& face, ndn::KeyChain& keyChain, const HistoricalReadRequest& request,
                   uint32_t timeoutMs)
{
  if (request.generation == 0 || request.ckName.empty() ||
      request.recipientCertificate.getName().empty() ||
      request.controllerCertificate.getName().empty())
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

  // A refusal is an answer, not a mismatch: the Controller says why and the reader should repeat it rather than
  // reporting that a key did not fit.
  {
    const std::string content(reinterpret_cast<const char*>(state->reply->getContent().value()),
                              state->reply->getContent().value_size());
    constexpr const char* DENIED = "NDNSF-HISTORICAL-DKEY-DENIED-v1\n";
    if (content.rfind(DENIED, 0) == 0) {
      const auto reasonEnd = content.find('\n', std::char_traits<char>::length(DENIED));
      const auto reason = content.substr(std::char_traits<char>::length(DENIED),
        reasonEnd == std::string::npos ? std::string::npos
                                       : reasonEnd - std::char_traits<char>::length(DENIED));
      HISTORICAL_READ_DEBUG("historical read: key request denied by the controller: %s\n", reason.c_str());
      return std::nullopt;
    }
  }
  HistoricalKeyReply accepted;
  if (!verifyHistoricalKeyReply(*state->reply, keyRequest, nowMs(), accepted)) {
    // Both sides, so the mismatch is a fact rather than a guess: which of the bound fields differs decides whether
    // the request or the Controller's answer is wrong.
    // The reply's own claims, read for the log only - the verifier refuses them, this only says why.
    {
      const std::string content(reinterpret_cast<const char*>(state->reply->getContent().value()),
                                state->reply->getContent().value_size());
      std::vector<std::string> lines;
      std::size_t begin = 0;
      while (lines.size() < 9) {
        const auto end = content.find('\n', begin);
        if (end == std::string::npos)
          break;
        lines.emplace_back(content.substr(begin, end - begin));
        begin = end + 1;
      }
      HISTORICAL_READ_DEBUG("historical read: reply lines=%zu object=%s generation=%s epoch=%s policy=%s\n",
                            lines.size(), lines.size() > 2 ? lines[2].c_str() : "-",
                            lines.size() > 4 ? lines[4].c_str() : "-",
                            lines.size() > 5 ? lines[5].c_str() : "-",
                            lines.size() > 6 ? lines[6].c_str() : "-");
    }
    HISTORICAL_READ_DEBUG("historical read: key reply mismatch request{generation=%llu epoch=%llu policy=%s "
                          "ck=%s object=%s} reply=%s\n",
                          static_cast<unsigned long long>(keyRequest.generation),
                          static_cast<unsigned long long>(keyRequest.epoch),
                          keyRequest.policy.c_str(), keyRequest.ckName.toUri().c_str(),
                          keyRequest.objectName.toUri().c_str(),
                          (*state->reply).getContent().value_size() > 0 ? "present" : "empty");
    return std::nullopt;
  }
  // The envelope can be forged; only the Controller's signature says who wrote it.
  if (!verifyHistoricalKeyReplySignature(*state->reply, request.controllerCertificate)) {
    HISTORICAL_READ_DEBUG("historical read: key reply is not signed by the Controller\n");
    return std::nullopt;
  }
  std::string keyBytes;
  if (!unsealHistoricalKeyReply(accepted, keyChain.getTpm(),
                                request.recipientCertificate.getName(), keyBytes))
    return std::nullopt;
  return std::vector<uint8_t>(keyBytes.begin(), keyBytes.end());
}

std::optional<std::string>
fetchHistoricalPublicParameters(ndn::Face& face, const ndn::Name& controllerPrefix, uint64_t generation,
                                uint64_t epoch, const ndn::security::Certificate& controllerCertificate,
                                uint32_t timeoutMs)
{
  if (controllerPrefix.empty() || controllerCertificate.getName().empty()) {
    HISTORICAL_READ_DEBUG("historical read: parameters stage has no controller prefix or certificate\n");
    return std::nullopt;
  }
  ndn::Name indexName(controllerPrefix);
  indexName.append("NDNSF").append("HISTORY").append("PUBPARAMS");
  ndn::Interest indexInterest(indexName);
  indexInterest.setCanBePrefix(true);
  indexInterest.setMustBeFresh(false);
  indexInterest.setInterestLifetime(ndn::time::milliseconds(timeoutMs));

  struct State
  {
    std::optional<ndn::Data> index;
  };
  auto state = std::make_shared<State>();
  if (const char* transport = std::getenv("NDN_CLIENT_TRANSPORT")) {
    const std::string value(transport);
    const auto marker = value.find("unix://");
    if (marker != std::string::npos) {
      const auto path = value.substr(marker + 7);
      // A file is not a listener: a socket left by an earlier phase satisfies access() while nothing accepts on
      // it, and the Face then sends into nothing. A real connect tells the two apart.
      int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
      int connected = -1;
      int failure = 0;
      if (probe >= 0) {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        ::snprintf(address.sun_path, sizeof(address.sun_path), "%s", path.c_str());
        connected = ::connect(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        failure = errno;
        ::close(probe);
      }
      HISTORICAL_READ_DEBUG("historical read: transport %s present=%d connect=%d errno=%d\n", path.c_str(),
                            ::access(path.c_str(), F_OK) == 0 ? 1 : 0, connected, failure);
    }
  }
  HISTORICAL_READ_DEBUG("historical read: expressing %s\n", indexName.toUri().c_str());
  face.expressInterest(indexInterest,
    [state] (const ndn::Interest&, const ndn::Data& data) { state->index = data; },
    [] (const ndn::Interest&, const ndn::lp::Nack&) {},
    [] (const ndn::Interest&) {});
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (!state->index && std::chrono::steady_clock::now() < deadline)
    face.processEvents(ndn::time::milliseconds(10));
  if (!state->index) {
    HISTORICAL_READ_DEBUG("historical read: no index answer for %s\n", indexName.toUri().c_str());
    return std::nullopt;
  }
  try {
    if (!ndn::security::verifySignature(*state->index, controllerCertificate.getPublicKey()))
      return std::nullopt;
  }
  catch (const std::exception&) {
    HISTORICAL_READ_DEBUG("historical read: index signature did not verify\n");
    return std::nullopt;
  }
  const std::string indexText(reinterpret_cast<const char*>(state->index->getContent().value()),
                              state->index->getContent().value_size());
  std::vector<std::tuple<uint64_t, uint64_t, std::string, std::string>> records;
  if (!parseHistoricalPublicParametersIndex(indexText, records)) {
    HISTORICAL_READ_DEBUG("historical read: index did not parse: %s\n", indexText.c_str());
    return std::nullopt;
  }
  const auto found = std::find_if(records.begin(), records.end(),
    [generation, epoch] (const auto& record) {
      return std::get<0>(record) == generation && std::get<1>(record) == epoch;
    });
  if (found == records.end()) {
    HISTORICAL_READ_DEBUG("historical read: generation %llu (epoch %llu) not in index; it lists:\n",
                          static_cast<unsigned long long>(generation),
                          static_cast<unsigned long long>(epoch));
    for (const auto& record : records) {
      HISTORICAL_READ_DEBUG("  generation=%llu epoch=%llu digest=%.16s...\n",
                            static_cast<unsigned long long>(std::get<0>(record)),
                            static_cast<unsigned long long>(std::get<1>(record)),
                            std::get<2>(record).c_str());
    }
    return std::nullopt;
  }

  const auto proofWire = fetchSingleData(
    face, historicalPublicParametersName(controllerPrefix, generation, epoch, std::get<2>(*found)),
    controllerCertificate, timeoutMs);
  if (!proofWire) {
    HISTORICAL_READ_DEBUG("historical read: proof fetch failed\n");
    return std::nullopt;
  }
  HistoricalPublicParameters proof;
  if (!parseHistoricalPublicParameters(std::string(proofWire->begin(), proofWire->end()), proof)) {
    HISTORICAL_READ_DEBUG("historical read: proof did not parse; first line: %.80s\n",
                          std::string(proofWire->begin(), proofWire->end()).c_str());
    return std::nullopt;
  }
  auto proofDigest = proof.digest.substr(7);
  std::transform(proofDigest.begin(), proofDigest.end(), proofDigest.begin(),
                 [] (unsigned char value) { return static_cast<char>(std::tolower(value)); });
  auto indexDigest = std::get<2>(*found);
  std::transform(indexDigest.begin(), indexDigest.end(), indexDigest.begin(),
                 [] (unsigned char value) { return static_cast<char>(std::tolower(value)); });
  if (proofDigest != indexDigest || proof.originalName != std::get<3>(*found)) {
    HISTORICAL_READ_DEBUG("historical read: proof does not match the index entry\n");
    return std::nullopt;
  }
  return proof.publicParameters;
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
  if (!ckWire) {
    HISTORICAL_READ_DEBUG("historical read: content key fetch failed for %s\n",
                          request.ckName.toUri().c_str());
    return std::nullopt;
  }
  const auto payloadWire = fetchSegmentedData(face, request.payloadName, request.producerCertificate,
                                              request.maxSegments, request.fetchTimeoutMs);
  if (!payloadWire) {
    HISTORICAL_READ_DEBUG("historical read: payload fetch failed for %s\n",
                          request.payloadName.toUri().c_str());
    return std::nullopt;
  }
  HistoricalCkContent ck;
  HistoricalPayloadContent payload;
  if (!parseHistoricalCkContent(std::string(ckWire->begin(), ckWire->end()), ck) ||
      !parseHistoricalPayloadContent(std::string(payloadWire->begin(), payloadWire->end()), payload)) {
    HISTORICAL_READ_DEBUG("historical read: envelope parse failed\n");
    return std::nullopt;
  }
  // When the producer wrote the CK name into the payload, it must be the CK this request verified: otherwise the
  // object is protected by some other key and the refusal must happen before anything is unsealed.
  if (!payload.ckName.empty()) {
    const auto expected = request.ckName.wireEncode();
    if (payload.ckName.size() != expected.size() ||
        !std::equal(expected.begin(), expected.end(),
                    reinterpret_cast<const uint8_t*>(payload.ckName.data()))) {
      HISTORICAL_READ_DEBUG("historical read: payload names a different CK\n");
      return std::nullopt;
    }
  }
  const auto key = fetchHistoricalKey(face, keyChain, request, request.fetchTimeoutMs * 4);
  if (!key) {
    HISTORICAL_READ_DEBUG("historical read: historical key request failed\n");
    return std::nullopt;
  }
  if (request.publicParameters.empty()) {
    HISTORICAL_READ_DEBUG("historical read: no public parameters to decrypt with\n");
    return std::nullopt;
  }
  auto plaintext = decryptHistoricalPayload(request.publicParameters, ck, payload, *key);
  if (!plaintext)
    HISTORICAL_READ_DEBUG("historical read: decryption failed\n");
  return plaintext;
}

} // namespace ndn_service_framework
