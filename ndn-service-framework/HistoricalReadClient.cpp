#include "HistoricalReadClient.hpp"

#include "AbeHistoricalAuthorization.hpp"
#include "AbeHistoricalContent.hpp"
#include "AbeHistoricalKeyRequest.hpp"
#include "AbePublicParametersArchive.hpp"
#include "ProducerSignerPolicy.hpp"
#include "common.hpp"
#include "detail/controller-lease-budget.hpp"
#include "detail/controller-version-floor.hpp"
#include "detail/historical-policy-status.hpp"
#include "detail/nac-consumer-transport.hpp"
#include "utils.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include <nac-abe/consumer.hpp>
#include <ndn-cxx/security/interest-signer.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/segment-fetcher.hpp>
#include <ndn-cxx/util/sha256.hpp>
#include <ndn-cxx/util/string-helper.hpp>

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

using ReadClock = std::chrono::steady_clock;

uint32_t
remainingWaitMs(ReadClock::time_point deadline, uint32_t limit)
{
  const auto now = ReadClock::now();
  if (limit == 0 || now >= deadline)
    return 0;
  const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
  return std::min<uint64_t>(limit, static_cast<uint64_t>(remaining));
}

/** Fetch one Data packet and require the producer's signature on it. */
std::optional<ndn::Data>
fetchVerified(ndn::Face& face, ndn::Interest interest, const ndn::security::Certificate& signer,
              uint32_t timeoutMs)
{
  if (timeoutMs == 0)
    return std::nullopt;
  const auto deadline = ReadClock::now() + std::chrono::milliseconds(timeoutMs);
  struct State
  {
    std::optional<ndn::Data> data;
    bool done = false;
  };
  auto state = std::make_shared<State>();
  const auto name = interest.getName();
  interest.setInterestLifetime(ndn::time::milliseconds(timeoutMs));
  // Whether the Interest is ever expressed is a fact only this side can report: with the send path unreached the
  // forwarder sees management commands and nothing else.
  HISTORICAL_READ_DEBUG("historical read: expressing %s\n", name.toUri().c_str());
  ndn::ScopedPendingInterestHandle pending = face.expressInterest(interest,
    [state, signer, name] (const ndn::Interest&, const ndn::Data& data) {
      try {
        if (data.getName().equals(name) &&
            ndn::security::verifySignature(data, signer.getPublicKey()) &&
            ProducerSignerPolicy().allowsValidatedSigner(data, signer.getIdentity()))
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
  while (!state->done) {
    const auto wait = remainingWaitMs(deadline, 10);
    if (wait == 0)
      break;
    face.processEvents(ndn::time::milliseconds(wait));
  }
  return ReadClock::now() < deadline ? state->data : std::nullopt;
}

std::optional<ndn::Data>
fetchVerified(ndn::Face& face, const ndn::Name& name, const ndn::security::Certificate& signer,
              uint32_t timeoutMs, const ndn::Name& forwardingHint = {})
{
  ndn::Interest interest(name);
  if (!forwardingHint.empty())
    interest.setForwardingHint({forwardingHint});
  return fetchVerified(face, std::move(interest), signer, timeoutMs);
}

using detail::AdmittedPolicyStatus;

std::optional<AdmittedPolicyStatus>
fetchCurrentPolicyStatus(ndn::Face& face, const HistoricalReadRequest& request, uint32_t timeoutMs)
{
  return detail::fetchCurrentPolicyStatus(face, request.controllerPrefix, request.serviceName,
                                         request.controllerCertificate, timeoutMs);
}

} // namespace

std::optional<detail::AdmittedPolicyStatus>
detail::fetchCurrentPolicyStatus(ndn::Face& face, const ndn::Name& controller,
                                 const ndn::Name& service,
                                 const ndn::security::Certificate& certificate,
                                 uint32_t timeoutMs)
{
  if (!certificate.isValid() || certificate.getIdentity() != controller || service.empty())
    return std::nullopt;
  const auto parameters = encodeControlQueryParameters({{}, service, 0, 0});
  ndn::Interest interest(makeControlRpcName(controller, "POLICY-STATUS",
                                            parameters, makeControlQueryNonce()));
  interest.setApplicationParameters(parameters);
  interest.setMustBeFresh(true);
  const auto packet = fetchVerified(face, std::move(interest), certificate, timeoutMs);
  if (!packet)
    return std::nullopt;
  try {
    PolicyStatusData status;
    const auto budget = detail::controllerLeaseBudget();
    if (!status.wireDecode(packet->getContent().blockFromValue()) ||
        status.getServiceName() != service ||
        status.getControllerCertificate() != certificate.getName() ||
        !detail::meetsControllerVersionFloor(status.getControllerVersion()))
      return std::nullopt;
    const auto admittedAt = ReadClock::now();
    const auto remaining = status.remainingLeaseMs(nowMs(), budget.maxLeaseMs,
                                                    budget.maxClockSkewMs);
    if (remaining == 0)
      return std::nullopt;
    return AdmittedPolicyStatus{std::move(status), admittedAt + std::chrono::milliseconds(remaining)};
  }
  catch (const std::exception&) {
    return std::nullopt;
  }
}

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

namespace {

std::optional<std::vector<uint8_t>>
fetchSegments(ndn::Face& face, const ndn::Name& root, const ndn::security::Certificate& signer,
              uint32_t maxSegments, size_t maxPayloadBytes, uint32_t timeoutMs,
              ReadClock::time_point deadline, const HistoricalPublication* publication,
              const ndn::Name& forwardingHint = {})
{
  if (root.empty() || signer.getName().empty())
    return std::nullopt;
  std::vector<uint8_t> merged;
  ndn::util::Sha256 wireDigest;
  // Every NAC segment carries the same signed final component.
  std::optional<uint64_t> finalSegment;
  for (uint32_t index = 0; index < maxSegments; ++index) {
    ndn::Name segment(root);
    segment.appendSegment(index);
    const auto data = fetchVerified(face, segment, signer,
                                    remainingWaitMs(deadline, timeoutMs), forwardingHint);
    if (!data)
      return std::nullopt;
    const auto& finalBlock = data->getMetaInfo().getFinalBlock();
    if (!finalBlock || !finalBlock->isSegment())
      return std::nullopt;
    const auto last = finalBlock->toSegment();
    if (last >= maxSegments || last < index || (finalSegment && *finalSegment != last))
      return std::nullopt;
    if (publication) {
      if (last + 1 != publication->segments)
        return std::nullopt;
      const auto& wire = data->wireEncode();
      wireDigest << ndn::span<const uint8_t>(wire.data(), wire.size());
    }
    if (!finalSegment) {
      finalSegment = last;
    }
    if (data->getContent().value_size() > maxPayloadBytes - merged.size())
      return std::nullopt;
    merged.insert(merged.end(), data->getContent().value(),
                  data->getContent().value() + data->getContent().value_size());
    if (index == *finalSegment)
      break;
  }
  if (!finalSegment || merged.empty() || ReadClock::now() >= deadline)
    return std::nullopt;
  if (publication &&
      "sha256:" + ndn::toHex(*wireDigest.computeDigest(), false) != publication->objectDigest)
    return std::nullopt;
  return merged;
}

} // namespace

std::optional<std::vector<uint8_t>>
fetchSegmentedData(ndn::Face& face, const ndn::Name& root, const ndn::security::Certificate& signer,
                   uint32_t maxSegments, uint32_t timeoutMs)
{
  return fetchSegments(face, root, signer, maxSegments, SIZE_MAX, timeoutMs,
                       ReadClock::now() + std::chrono::milliseconds(timeoutMs), nullptr);
}

namespace {

struct HistoricalKeyLease
{
  std::vector<uint8_t> key;
  ReadClock::time_point deadline;
  uint64_t expiresAtMs;

  bool isValid() const
  {
    return ReadClock::now() < deadline && nowMs() < expiresAtMs;
  }
};

std::optional<HistoricalKeyLease>
fetchHistoricalKeyWithLease(ndn::Face& face, ndn::KeyChain& keyChain,
                            const HistoricalReadRequest& request, uint32_t timeoutMs,
                            HistoricalReadFailure& failure,
                            const AdmittedPolicyStatus* observed = nullptr)
{
  const auto fail = [&] (HistoricalReadFailure reason) -> std::optional<HistoricalKeyLease> {
    failure = reason;
    return std::nullopt;
  };
  if (timeoutMs == 0 || request.generation == 0 || request.ckName.empty() ||
      request.recipientCertificate.getName().empty() ||
      request.controllerCertificate.getName().empty())
    return fail(HistoricalReadFailure::INVALID_DATA);
  const auto deadline = ReadClock::now() + std::chrono::milliseconds(timeoutMs);
  std::optional<AdmittedPolicyStatus> fetched;
  if (!observed) {
    fetched = fetchCurrentPolicyStatus(face, request,
      remainingWaitMs(deadline, request.fetchTimeoutMs));
    if (!fetched)
      return fail(HistoricalReadFailure::CONTROLLER_UNAVAILABLE);
    observed = &*fetched;
  }
  const auto& status = *observed;

  HistoricalKeyRequest keyRequest;
  keyRequest.controllerPrefix = request.controllerPrefix;
  keyRequest.generation = request.generation;
  keyRequest.epoch = request.epoch;
  keyRequest.objectName = request.objectName;
  keyRequest.producerName = request.producerName;
  keyRequest.serviceName = request.serviceName;
  keyRequest.ckName = request.ckName;
  keyRequest.policy = request.policy;
  keyRequest.ckEvidence = request.ckEvidence;
  keyRequest.publicationEvidence = request.publicationEvidence;
  keyRequest.recipientCertificate = request.recipientCertificate.getFullName();
  keyRequest.currentControllerVersion = status.data.getControllerVersion();
  const auto& signer = request.signingCertificate ? *request.signingCertificate :
                                                   request.recipientCertificate;
  if (signer.getName().empty() || signer.getIdentity() != request.recipientCertificate.getIdentity())
    return fail(HistoricalReadFailure::INVALID_DATA);
  const auto now = nowMs();
  const auto budget = detail::controllerLeaseBudget();
  const auto leaseMs = status.data.remainingLeaseMs(now, budget.maxLeaseMs, budget.maxClockSkewMs);
  const auto limit = std::min(deadline, status.deadline);
  const auto available = std::min<uint64_t>(leaseMs, remainingWaitMs(limit, 120000));
  if (available == 0)
    return fail(HistoricalReadFailure::DEADLINE_EXPIRED);
  const auto leaseDeadline = std::min(limit, ReadClock::now() +
    std::chrono::milliseconds(available));
  keyRequest.mintedAtMs = now;
  keyRequest.expiresAtMs = now + available;

  ndn::Interest interest;
  try {
    signHistoricalKeyRequest(interest, keyRequest, keyChain,
      ndn::security::signingByCertificate(signer));
  }
  catch (const std::exception&) {
    return fail(HistoricalReadFailure::INVALID_DATA);
  }
  const auto responseWait = remainingWaitMs(leaseDeadline, timeoutMs);
  if (responseWait == 0)
    return fail(HistoricalReadFailure::DEADLINE_EXPIRED);
  interest.setInterestLifetime(ndn::time::milliseconds(responseWait));
  if (interest.wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE)
    return fail(HistoricalReadFailure::INVALID_DATA);

  struct State
  {
    std::optional<ndn::Data> reply;
    bool done = false;
    bool nack = false;
  };
  auto state = std::make_shared<State>();
  ndn::ScopedPendingInterestHandle pending = face.expressInterest(interest,
    [state] (const ndn::Interest&, const ndn::Data& data) {
      state->reply = data;
      state->done = true;
    },
    [state] (const ndn::Interest&, const ndn::lp::Nack&) {
      state->nack = true;
      state->done = true;
    },
    [state] (const ndn::Interest&) { state->done = true; });
  while (!state->done) {
    const auto wait = remainingWaitMs(leaseDeadline, 10);
    if (wait == 0)
      break;
    face.processEvents(ndn::time::milliseconds(wait));
  }
  if (!state->reply)
    return fail(state->nack ? HistoricalReadFailure::CONTROLLER_UNAVAILABLE :
                              HistoricalReadFailure::KEY_TIMEOUT);
  if (ReadClock::now() >= leaseDeadline || nowMs() >= keyRequest.expiresAtMs)
    return fail(HistoricalReadFailure::DEADLINE_EXPIRED);
  if (state->reply->getName() != interest.getName() ||
      !verifyHistoricalKeyReplySignature(*state->reply, request.controllerCertificate)) {
    HISTORICAL_READ_DEBUG("historical read: unauthenticated or unmatched Controller key reply\n");
    return fail(HistoricalReadFailure::KEY_REPLY_INVALID);
  }

  // Report a refusal only after the same Controller admission as a successful reply.
  {
    const std::string content(reinterpret_cast<const char*>(state->reply->getContent().value()),
                              state->reply->getContent().value_size());
    constexpr const char* DENIED = "NDNSF-HISTORICAL-DKEY-DENIED-v1\n";
    if (content.rfind(DENIED, 0) == 0) {
      const auto first = std::char_traits<char>::length(DENIED);
      const auto end = content.find('\n', first);
      if (end == std::string::npos || end == first || end + 1 != content.size())
        return fail(HistoricalReadFailure::KEY_REPLY_INVALID);
      const auto reason = content.substr(first, end - first);
      HISTORICAL_READ_DEBUG("historical read: key request denied by the controller: %s\n", reason.c_str());
      if (reason == "not-authorised")
        return fail(HistoricalReadFailure::NOT_AUTHORIZED);
      if (reason == "publication-receipt-required" || reason == "publication-evidence-required" ||
          reason == "ck-evidence-required")
        return fail(HistoricalReadFailure::EVIDENCE_REQUIRED);
      if (reason == "key-unavailable")
        return fail(HistoricalReadFailure::KEY_UNAVAILABLE);
      if (reason == "controller-not-ready" || reason == "abe-rotation-pending")
        return fail(HistoricalReadFailure::CONTROLLER_UNAVAILABLE);
      return fail(HistoricalReadFailure::REQUEST_REJECTED);
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
    return fail(HistoricalReadFailure::KEY_REPLY_INVALID);
  }
  const auto expiresAt = std::min(accepted.expiresAtMs, keyRequest.expiresAtMs);
  const auto wallNow = nowMs();
  if (expiresAt <= wallNow)
    return fail(HistoricalReadFailure::DEADLINE_EXPIRED);
  HistoricalKeyLease lease{{}, std::min(leaseDeadline, ReadClock::now() +
    std::chrono::milliseconds(expiresAt - wallNow)), expiresAt};
  const auto refreshed = fetchCurrentPolicyStatus(face, request,
    remainingWaitMs(lease.deadline, request.fetchTimeoutMs));
  std::string keyBytes;
  if (!lease.isValid())
    return fail(HistoricalReadFailure::DEADLINE_EXPIRED);
  if (!refreshed)
    return fail(HistoricalReadFailure::CONTROLLER_UNAVAILABLE);
  if (refreshed->data.getControllerVersion() != accepted.currentControllerVersion ||
      !refreshed->data.canReplaceSameVersion(status.data, true))
    return fail(HistoricalReadFailure::AUTHORITY_CHANGED);
  if (!unsealHistoricalKeyReply(accepted, keyChain.getTpm(),
                                request.recipientCertificate.getName(), keyBytes))
    return fail(HistoricalReadFailure::KEY_REPLY_INVALID);
  if (!lease.isValid())
    return fail(HistoricalReadFailure::DEADLINE_EXPIRED);
  lease.key.assign(keyBytes.begin(), keyBytes.end());
  failure = HistoricalReadFailure::NONE;
  return lease;
}

} // namespace

std::optional<std::vector<uint8_t>>
fetchHistoricalKey(ndn::Face& face, ndn::KeyChain& keyChain, const HistoricalReadRequest& request,
                   uint32_t timeoutMs)
{
  HistoricalReadFailure failure;
  auto lease = fetchHistoricalKeyWithLease(face, keyChain, request, timeoutMs, failure);
  if (!lease || !lease->isValid())
    return std::nullopt;
  return std::move(lease->key);
}

std::optional<std::string>
fetchHistoricalPublicParameters(ndn::Face& face, const ndn::Name& controllerPrefix, uint64_t generation,
                                uint64_t epoch, const ndn::security::Certificate& controllerCertificate,
                                uint32_t timeoutMs, const ndn::Name& serviceName)
{
  if (timeoutMs == 0 || controllerPrefix.empty() || controllerCertificate.getName().empty()) {
    HISTORICAL_READ_DEBUG("historical read: parameters stage has no controller prefix or certificate\n");
    return std::nullopt;
  }
  ndn::Name indexName(controllerPrefix);
  indexName.append("NDNSF").append("HISTORY").append("PUBPARAMS");
  const auto deadline = ReadClock::now() + std::chrono::milliseconds(timeoutMs);
  uint64_t writerGeneration = 0;
  if (!serviceName.empty()) {
    HistoricalReadRequest request;
    request.serviceName = serviceName;
    request.controllerPrefix = controllerPrefix;
    request.controllerCertificate = controllerCertificate;
    const auto status = fetchCurrentPolicyStatus(face, request, timeoutMs);
    if (!status || !status->data.getAbeMaterialVersion().isValid())
      return std::nullopt;
    writerGeneration = status->data.getControllerVersion().controllerGenerationTimestamp;
    indexName = historicalPublicParametersIndexName(controllerPrefix, writerGeneration,
                                                     status->data.getAbeMaterialVersion());
  }
  const auto index = fetchSingleData(face, indexName, controllerCertificate,
                                     remainingWaitMs(deadline, timeoutMs));
  if (!index) {
    HISTORICAL_READ_DEBUG("historical read: no index answer for %s\n", indexName.toUri().c_str());
    return std::nullopt;
  }
  const std::string indexText(index->begin(), index->end());
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
    face, historicalPublicParametersName(controllerPrefix, generation, epoch, std::get<2>(*found),
                                         writerGeneration),
    controllerCertificate, remainingWaitMs(deadline, timeoutMs));
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
  if (ReadClock::now() >= deadline)
    return std::nullopt;
  return proof.publicParameters;
}

namespace {

std::optional<HistoricalPublication>
decodeMatchingPublication(const ndn::Data& proof, const HistoricalReadRequest& request)
{
  auto publication = decodeHistoricalPublication(proof);
  if (!publication || publication->producer != request.producerName ||
      publication->controller != request.controllerPrefix ||
      publication->service != request.serviceName ||
      publication->objectName != request.objectName || publication->policy != request.policy ||
      publication->ckFullName.getPrefix(-1) != request.ckName ||
      publication->generation != request.generation || publication->epoch != request.epoch ||
      publication->segments > request.maxSegments)
    return std::nullopt;
  return publication;
}

} // namespace

std::optional<std::string>
readHistoricalObject(ndn::Face& face, ndn::KeyChain& keyChain, const HistoricalReadRequest& request)
{
  HistoricalReadFailure failure;
  return readHistoricalObject(face, keyChain, request, failure);
}

std::optional<std::string>
readHistoricalObject(ndn::Face& face, ndn::KeyChain& keyChain,
                     const HistoricalReadRequest& request, HistoricalReadFailure& failure)
{
  failure = HistoricalReadFailure::INVALID_DATA;
  const auto deadline = ReadClock::now() + std::chrono::milliseconds(request.totalTimeoutMs);
  if (request.producerCertificate.getName().empty() || request.producerName.empty() ||
      request.producerCertificate.getIdentity() != request.producerName ||
      !request.producerName.isPrefixOf(request.ckName) ||
      !request.producerName.isPrefixOf(request.payloadName) ||
      request.objectName != request.payloadName || request.publicationName.empty() ||
      !request.producerName.isPrefixOf(request.publicationName) || request.totalTimeoutMs == 0 ||
      request.fetchTimeoutMs == 0 || request.maxSegments == 0 || request.maxPayloadBytes == 0)
    return std::nullopt;
  const auto proof = fetchVerified(face, request.publicationName, request.producerCertificate,
    remainingWaitMs(deadline, request.fetchTimeoutMs), request.originalDataForwardingHint);
  if (!proof) {
    failure = HistoricalReadFailure::ORIGINAL_UNAVAILABLE;
    return std::nullopt;
  }
  const auto publication = decodeMatchingPublication(*proof, request);
  if (!publication)
    return std::nullopt;
  std::optional<std::string> resolvedParameters;
  std::optional<AdmittedPolicyStatus> status;
  if (request.publicParameters.empty()) {
    // The authenticated publication already identifies the generation and
    // digest. The signed status identifies which writer publishes its proof.
    status = fetchCurrentPolicyStatus(face, request,
      remainingWaitMs(deadline, request.fetchTimeoutMs));
    if (!status) {
      failure = HistoricalReadFailure::CONTROLLER_UNAVAILABLE;
      return std::nullopt;
    }
    const auto proofName = historicalPublicParametersName(publication->controller,
      publication->generation, publication->epoch, publication->publicParametersDigest.substr(7),
      status->data.getControllerVersion().controllerGenerationTimestamp);
    const auto archived = fetchVerified(face, proofName, request.controllerCertificate,
      remainingWaitMs(deadline, request.fetchTimeoutMs));
    HistoricalPublicParameters parameters;
    if (!archived) {
      failure = HistoricalReadFailure::ARCHIVE_UNAVAILABLE;
      return std::nullopt;
    }
    if (!parseHistoricalPublicParameters(ndn::readString(archived->getContent()), parameters))
      return std::nullopt;
    try {
      if (ndn::Name(parameters.originalName) != publication->publicParametersName)
        return std::nullopt;
    }
    catch (const std::exception&) {
      return std::nullopt;
    }
    resolvedParameters = std::move(parameters.publicParameters);
  }
  const auto& parameters = resolvedParameters ? *resolvedParameters : request.publicParameters;
  ndn::util::Sha256 parametersDigest;
  parametersDigest << parameters;
  if ("sha256:" + ndn::toHex(*parametersDigest.computeDigest(), false) !=
      publication->publicParametersDigest)
    return std::nullopt;
  // The content key is one packet under its exact name; only the payload is segmented.
  // The CK is fetched as a verified packet rather than as content: the packet itself is what the Controller can check
  // as publication evidence, and its content is what the reader needs to unseal.
  const auto ckData = fetchVerified(face, request.ckName, request.producerCertificate,
    remainingWaitMs(deadline, request.fetchTimeoutMs), request.originalDataForwardingHint);
  if (!ckData) {
    failure = HistoricalReadFailure::ORIGINAL_UNAVAILABLE;
    return std::nullopt;
  }
  if (ckData->getFullName() != publication->ckFullName) {
    HISTORICAL_READ_DEBUG("historical read: content key fetch failed for %s\n",
                          request.ckName.toUri().c_str());
    return std::nullopt;
  }
  const auto ckLocator = historicalCkLocator(*ckData, request.producerName);
  if (!ckLocator)
    return std::nullopt;
  const auto ckWire = std::vector<uint8_t>(ckData->getContent().value(),
                                           ckData->getContent().value() + ckData->getContent().value_size());
  const auto ckPacket = ckData->wireEncode();
  const auto payloadWire = fetchSegments(face, request.payloadName, request.producerCertificate,
    request.maxSegments, request.maxPayloadBytes, request.fetchTimeoutMs, deadline,
    &*publication, request.originalDataForwardingHint);
  if (!payloadWire) {
    failure = HistoricalReadFailure::ORIGINAL_UNAVAILABLE;
    HISTORICAL_READ_DEBUG("historical read: payload fetch failed for %s\n",
                          request.payloadName.toUri().c_str());
    return std::nullopt;
  }
  HistoricalCkContent ck;
  HistoricalPayloadContent payload;
  if (!parseHistoricalCkContent(std::string(ckWire.begin(), ckWire.end()), ck) ||
      !parseHistoricalPayloadContent(std::string(payloadWire->begin(), payloadWire->end()), payload)) {
    HISTORICAL_READ_DEBUG("historical read: envelope parse failed\n");
    return std::nullopt;
  }
  const auto expected = ckLocator->wireEncode();
  if (payload.ckName.size() != expected.size() ||
      !std::equal(expected.begin(), expected.end(),
                  reinterpret_cast<const uint8_t*>(payload.ckName.data()))) {
    HISTORICAL_READ_DEBUG("historical read: payload names a different CK\n");
    return std::nullopt;
  }
  auto evidenced = request;
  evidenced.ckEvidence.assign(ckPacket.begin(), ckPacket.end());
  const auto& proofWire = proof->wireEncode();
  evidenced.publicationEvidence.assign(proofWire.begin(), proofWire.end());
  const auto key = fetchHistoricalKeyWithLease(face, keyChain, evidenced,
    remainingWaitMs(deadline, UINT32_MAX), failure, status ? &*status : nullptr);
  if (!key) {
    HISTORICAL_READ_DEBUG("historical read: historical key request failed\n");
    return std::nullopt;
  }
  if (!key->isValid() || ReadClock::now() >= deadline) {
    failure = HistoricalReadFailure::DEADLINE_EXPIRED;
    return std::nullopt;
  }
  auto plaintext = decryptHistoricalPayload(parameters, ck, payload, key->key);
  if (!key->isValid() || ReadClock::now() >= deadline) {
    failure = HistoricalReadFailure::DEADLINE_EXPIRED;
    return std::nullopt;
  }
  if (!plaintext) {
    HISTORICAL_READ_DEBUG("historical read: decryption failed\n");
    failure = HistoricalReadFailure::DECRYPTION_FAILED;
  }
  return plaintext;
}

namespace {

bool
hasCurrentReadPermission(ndn::Face& face, ndn::KeyChain& keyChain,
                         const HistoricalReadRequest& request,
                         const ControllerVersion& expectedVersion, uint32_t timeoutMs)
{
  if (timeoutMs == 0)
    return false;
  const auto& recipient = request.recipientCertificate;
  const auto identity = recipient.getIdentity();
  const auto parameters = encodeControlQueryParameters(
    ControlQueryParameters{identity, {}, 0, 0, recipient.getFullName()});
  ndn::Interest interest(makeControlRpcName(request.controllerPrefix, "USER-PERMISSIONS", parameters,
                                            makeControlQueryNonce()));
  try {
    // The parameters travel in the ApplicationParameters, not only in the name: the Controller decodes them from
    // there, so a query that carries them only in its name is answered by nothing.
    interest.setApplicationParameters(parameters);
    interest.setCanBePrefix(false);
    interest.setMustBeFresh(true);
    interest.setInterestLifetime(ndn::time::milliseconds(timeoutMs));
    ndn::security::InterestSigner signer(keyChain);
    signer.makeSignedInterest(interest, ndn::security::signingByCertificate(recipient));
  }
  catch (const std::exception&) {
    return false;
  }

  struct State
  {
    bool answered = false;
    bool done = false;
  };
  auto state = std::make_shared<State>();
  ndn::ScopedPendingInterestHandle pending = face.expressInterest(interest,
    [state, &request, &recipient, &identity, &keyChain, &expectedVersion]
    (const ndn::Interest& query, const ndn::Data& data) {
      state->done = true;
      try {
        if (data.getName() != query.getName() ||
            !ndn::security::verifySignature(data, request.controllerCertificate.getPublicKey()))
          return;
        EncryptedPermissionResponse encrypted;
        if (!encrypted.WireDecode(data.getContent().blockFromValue()) ||
            ndn::Name(encrypted.getRecipientCertName()) != recipient.getName())
          return;
        const auto permission = decryptPermissionResponseWithKeyChain(encrypted, keyChain);
        const auto& entries = permission.getEntries();
        state->answered = ndn::Name(permission.getTargetIdentity()) == identity &&
          permission.getPermissionKind() == tlv::UserPermission &&
          permission.hasControllerVersion() && permission.getControllerVersion() == expectedVersion &&
          std::any_of(entries.begin(), entries.end(), [&request] (const auto& entry) {
            return ndn::Name(entry.getServiceName()) == request.serviceName;
          });
      }
      catch (const std::exception&) {
        state->answered = false;
      }
    },
    [state] (const ndn::Interest&, const ndn::lp::Nack&) { state->done = true; },
    [state] (const ndn::Interest&) { state->done = true; });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (!state->done && std::chrono::steady_clock::now() < deadline)
    face.processEvents(ndn::time::milliseconds(10));
  return state->answered;
}

} // namespace

bool
primePermissions(ndn::Face& face, ndn::KeyChain& keyChain, const HistoricalReadRequest& request,
                 uint32_t timeoutMs)
{
  if (request.controllerPrefix.empty() || request.producerName.empty() ||
      request.producerCertificate.getIdentity() != request.producerName ||
      !request.producerName.isPrefixOf(request.publicationName))
    return false;
  const auto proof = fetchVerified(face, request.publicationName, request.producerCertificate,
                                   request.fetchTimeoutMs, request.originalDataForwardingHint);
  const auto publication = proof ? decodeMatchingPublication(*proof, request) : std::nullopt;
  if (!publication)
    return false;
  return hasCurrentReadPermission(face, keyChain, request,
    {publication->policyGeneration, publication->policyEpoch}, timeoutMs);
}

std::optional<std::string>
detail::readCurrentNacObject(ndn::Face& face, ndn::KeyChain& keys,
                             const HistoricalReadRequest& request, const std::string& trustSchema)
{
  const auto readDeadline = ReadClock::now() + std::chrono::milliseconds(request.totalTimeoutMs);
  if (request.producerName.empty() || request.producerCertificate.getIdentity() != request.producerName ||
      !request.producerName.isPrefixOf(request.publicationName) || request.payloadName != request.objectName ||
      !request.recipientCertificate.isValid() || !request.producerCertificate.isValid() ||
      request.totalTimeoutMs == 0 || request.fetchTimeoutMs == 0)
    return std::nullopt;
  const auto status = detail::fetchCurrentPolicyStatus(face, request.controllerPrefix, request.serviceName,
    request.controllerCertificate, remainingWaitMs(readDeadline, request.fetchTimeoutMs));
  if (!status)
    return std::nullopt;
  const auto deadline = std::min(status->deadline, readDeadline);
  const auto material = status->data.getAbeMaterialVersion();
  if (!material.isValid() || material.controllerGenerationTimestamp != request.generation ||
      material.controllerEpoch != request.epoch ||
      !hasCurrentReadPermission(face, keys, request, status->data.getControllerVersion(),
                                remainingWaitMs(deadline, request.fetchTimeoutMs)))
    return std::nullopt;

  const auto proof = fetchVerified(face, request.publicationName, request.producerCertificate,
    remainingWaitMs(deadline, request.fetchTimeoutMs), request.originalDataForwardingHint);
  const auto publication = proof ? decodeMatchingPublication(*proof, request) : std::nullopt;
  const auto& paramName = status->data.getAbePublicParametersName();
  const auto& digest = status->data.getAbePublicParametersDigest();
  if (!publication || paramName != publication->publicParametersName ||
      digest.size() != 71 || digest.compare(0, 7, "sha256:") != 0 ||
      "sha256:" + ndn::toHex(*ndn::fromHex(digest.substr(7)), false) != publication->publicParametersDigest)
    return std::nullopt;
  const auto ck = fetchVerified(face, request.ckName, request.producerCertificate,
    remainingWaitMs(deadline, request.fetchTimeoutMs), request.originalDataForwardingHint);
  if (!ck || ck->getFullName() != publication->ckFullName)
    return std::nullopt;
  const auto root = historicalCkLocator(*ck, request.producerName);
  const auto bytes = fetchSegments(face, request.payloadName, request.producerCertificate,
    request.maxSegments, request.maxPayloadBytes, request.fetchTimeoutMs, deadline,
    &*publication, request.originalDataForwardingHint);
  if (!root || !detail::isNacCkLocator(*root, request.producerName) || !bytes)
    return std::nullopt;
  ndn::Block cipher{ndn::span<const uint8_t>(*bytes)};
  cipher.parse();
  if (cipher.size() != bytes->size() || ndn::Name(cipher.get(ndn::tlv::Name)) != *root)
    return std::nullopt;

  // Feed the fetched original CK to the normal SDK Consumer using the same adapter
  // as ServiceProvider. DKEY and exact public parameters are fetched over the network.
  const auto transport = std::make_shared<detail::NacConsumerTransport>(face);
  ndn::Face sdkFace(transport, face.getIoContext(), keys);
  TrustSchemaValidator validator(sdkFace);
  loadTrustSchema(validator, trustSchema);
  validator.cacheUnverifiedCert(ndn::security::Certificate(request.controllerCertificate));
  validator.cacheUnverifiedCert(ndn::security::Certificate(request.producerCertificate));
  ndn::nacabe::Consumer nac(sdkFace, keys, validator, request.recipientCertificate,
    request.controllerCertificate, ndn::nacabe::ParamFetcher::getDefaultInterestTemplate(), false);
  const auto valid = [deadline, admitted = *status] {
    const auto budget = detail::controllerLeaseBudget();
    return ReadClock::now() < deadline &&
      admitted.data.remainingLeaseMs(nowMs(), budget.maxLeaseMs, budget.maxClockSkewMs) > 0;
  };
  const auto sdkDigest = "sha256:" + ndn::toHex(*ndn::fromHex(digest.substr(7)));
  nac.clearCache(paramName, sdkDigest);
  nac.obtainDecryptionKey();
  while (valid() && !nac.readyForDecryption())
    face.processEvents(ndn::time::milliseconds(5));
  if (!valid() || nac.getPublicParamsDataName() != paramName || nac.getPublicParamsDigest() != sdkDigest)
    return std::nullopt;

  struct Result
  {
    bool done = false;
    std::optional<std::string> plaintext;
  };
  const auto result = std::make_shared<Result>();
  const auto lease = transport->provide({std::make_shared<const ndn::Data>(*ck)}, valid);
  nac.consumeWithCkAuthorization(request.objectName, cipher, request.producerName,
    [expected = ck->getFullName()] (const ndn::Data& data) { return data.getFullName() == expected; },
    valid,
    [result, lease] (const ndn::Buffer& plain) {
      result->plaintext.emplace(plain.begin(), plain.end());
      result->done = true;
    },
    [result, lease] (const std::string&) { result->done = true; });
  while (valid() && !result->done)
    face.processEvents(ndn::time::milliseconds(5));
  return valid() ? std::move(result->plaintext) : std::nullopt;
}

} // namespace ndn_service_framework
