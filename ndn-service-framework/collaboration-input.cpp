#include "collaboration-input.hpp"
#include "detail/collaboration-envelope.hpp"
#include <ndn-cxx/util/sha256.hpp>
#include <algorithm>
#include <cctype>
#include <iterator>
#include <stdexcept>

namespace ndn_service_framework {
namespace {
std::string sha256DigestString(const ndn::Buffer& bytes)
{
  ndn::util::Sha256 digest;
  if (!bytes.empty()) {
    digest << std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  auto hex = digest.toString();
  std::transform(hex.begin(), hex.end(), hex.begin(), [] (unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return "sha256:" + hex;
}
}

ndn::Name detail::makeCollaborationObjectGrantName(
    const ndn::Name& producer, const ndn::Name& service, const std::string& scope,
    const ndn::Name& object, const ndn::Name& request)
{
  const auto& wire = object.wireEncode();
  const ndn::Buffer encodedName(wire.begin(), wire.end());
  return ndn::Name(producer).append("NDNSF").append("COLLAB-OBJECT-KEY")
    .append(service.toUri()).append(scope).append(sha256DigestString(encodedName))
    .append(request).appendVersion(1);
}

std::optional<ndn::Buffer> detail::decryptCollaborationEnvelope(
    const ndn::Name& object, const ndn::Name& request, const std::string& scope,
    const ndn::Buffer& key, ndn::ConstBufferPtr encoded)
{
  HybridMessageEnvelope envelope;
  if (!encoded) return std::nullopt;
  {
    ndn::Block block(encoded);
    if (!envelope.WireDecode(block)) return std::nullopt;
  }
  encoded.reset();
  if (envelope.getMessageType() != "COLLAB-LARGE" || envelope.getEpochId() != "session" ||
      hybridCompactKeyId(envelope.getKeyId()) !=
        hybridCompactKeyId("collab-large|" + request.toUri() + "|" + scope))
    return std::nullopt;
  const auto text = object.toUri() + "|COLLAB-LARGE|" + request.toUri() + "|" + scope;
  const ndn::Buffer ad(text.begin(), text.end());
  ndn::Buffer plaintext;
  if (!hybridAesGcmDecrypt(key, envelope,
      ndn::span<const uint8_t>(ad.data(), ad.size()), plaintext)) return std::nullopt;
  return plaintext;
}

std::optional<ndn::Buffer> detail::decryptReusableCollaborationEnvelope(
    const ndn::Name& object, const ndn::Name& service, const std::string& scope,
    const ndn::Buffer& key, ndn::ConstBufferPtr encoded)
{
  if (!encoded || key.size() != HybridMessageCrypto::MESSAGE_KEY_SIZE) return std::nullopt;
  HybridMessageEnvelope envelope;
  {
    ndn::Block block(encoded);
    if (!envelope.WireDecode(block)) return std::nullopt;
  }
  encoded.reset();
  if ((envelope.getVersion() != 1 && envelope.getVersion() != 2) ||
      envelope.getAlgorithm() != "AES-256-GCM" || envelope.hasWrappedMessageKey() ||
      envelope.getMessageType() != "COLLAB-OBJECT" || envelope.getEpochId() != "object") {
    return std::nullopt;
  }
  const auto digest = sha256DigestString(key);
  const auto expectedKeyId = envelope.getVersion() >= 2 ? hybridCompactKeyId(digest) : digest;
  if (envelope.getKeyId() != expectedKeyId) return std::nullopt;
  const auto adText = object.toUri() + "|COLLAB-OBJECT|" + service.toUri() + "|" + scope;
  const ndn::Buffer ad(adText.begin(), adText.end());
  ndn::Buffer plaintext;
  if (!hybridAesGcmDecrypt(key, envelope,
      ndn::span<const uint8_t>(ad.data(), ad.size()), plaintext)) return std::nullopt;
  return plaintext;
}

VerifiedInputObject::VerifiedInputObject(ndn::Name name, ndn::Name producer,
    std::shared_ptr<const std::vector<std::shared_ptr<const ndn::Data>>> packets,
    std::string dispatchBinding, std::function<bool()> current)
  : m_name(std::move(name)), m_producer(std::move(producer)), m_packets(std::move(packets)),
    m_dispatchBinding(std::move(dispatchBinding)), m_current(std::move(current))
{
}

const ndn::Buffer& VerifiedInputObject::content() const
{
  if (!m_content) throw std::logic_error("input is not a decoded generic object");
  return *m_content;
}

const std::vector<std::shared_ptr<const ndn::Data>>& VerifiedInputObject::packets() const
{
  if (!m_packets || m_content) throw std::logic_error("input is not an exact packet set");
  return *m_packets;
}

const VerifiedInputObject& AdmittedInputSet::at(const std::string& slotId) const
{
  const auto found = std::find(m_slotIds.begin(), m_slotIds.end(), slotId);
  if (found == m_slotIds.end()) throw std::out_of_range("unknown admitted input slot");
  const auto index = static_cast<size_t>(std::distance(m_slotIds.begin(), found));
  if (index >= m_objects.size()) throw std::logic_error("admitted input slot mapping is invalid");
  return m_objects[index];
}

VerifiedExactDataFetchOperation::VerifiedExactDataFetchOperation(
    ServiceProvider::ExactDataFetchOperation operation, ndn::Name name,
    ndn::Name producer, std::string dispatchBinding, std::function<bool()> current,
    std::shared_ptr<std::atomic<bool>> signatureRejected)
  : m_operation(std::move(operation)), m_name(std::move(name)), m_producer(std::move(producer)),
    m_dispatchBinding(std::move(dispatchBinding)), m_current(std::move(current)),
    m_signatureRejected(std::move(signatureRejected))
{
}

std::future_status VerifiedExactDataFetchOperation::wait_for(std::chrono::milliseconds duration) const
{
  return m_operation.wait_for(duration);
}

void VerifiedExactDataFetchOperation::cancel()
{
  m_operation.cancel();
}

CollaborationTransferMetrics VerifiedExactDataFetchOperation::metricsSnapshot() const
{
  return m_operation.metricsSnapshot();
}

std::optional<VerifiedExactDataBatchResult> VerifiedExactDataFetchOperation::get()
{
  auto result = m_operation.get();
  if (!result || !m_current || !m_current()) return std::nullopt;
  auto packets = std::make_shared<std::vector<std::shared_ptr<const ndn::Data>>>();
  packets->reserve(result->contents.size());
  for (auto& wire : result->contents) {
    // The private factory selects the existing validator's original-wire path.
    // Moving the buffer into Block avoids a second full packet-set copy.
    auto owner = std::make_shared<const ndn::Buffer>(std::move(wire));
    packets->push_back(std::make_shared<const ndn::Data>(ndn::Block(owner)));
  }
  size_t payloadBytes = 0;
  for (const auto& packet : *packets)
    payloadBytes += packet->getContent().value_size();
  result->metrics.transportPayloadBytes = payloadBytes;
  if (result->metrics.wireBytes) {
    const auto sourceWire = *result->metrics.wireBytes - std::min(
      *result->metrics.wireBytes, result->metrics.repairWireBytes);
    if (sourceWire >= payloadBytes) result->metrics.metadataBytes = sourceWire - payloadBytes;
    else result->metrics.metadataBytes.reset();
  }
  else
    result->metrics.metadataBytes.reset();
  return VerifiedExactDataBatchResult{
    VerifiedInputObject(m_name, m_producer, std::move(packets), m_dispatchBinding, m_current),
    std::move(result->metrics)};
}

VerifiedExactDataFetchOperation
ServiceProvider::CollaborationContext::startVerifiedSignedExactDataBatch(
    KeyScope keyScope, const std::vector<ndn::Name>& dataNames,
    const ndn::Name& expectedProducer, int timeoutMs,
    std::function<bool()> shouldCancel,
    std::function<void(std::exception_ptr)> onFirstReadStarted,
    ExactDataFetchPhase phase, std::function<bool()> onValidatedData,
    std::size_t maxOutstanding, std::size_t maxWireBytes)
{
  return startVerifiedSignedExactDataBatchImpl(std::move(keyScope), {}, dataNames, expectedProducer,
    timeoutMs, std::move(shouldCancel), std::move(onFirstReadStarted), phase,
    std::move(onValidatedData), maxOutstanding, maxWireBytes, {}, {},
    {});
}

VerifiedExactDataFetchOperation
ServiceProvider::CollaborationContext::startVerifiedSignedExactDataBatchImpl(
    KeyScope keyScope, const CollaborationRole& producerRole,
    const std::vector<ndn::Name>& dataNames,
    const ndn::Name& expectedProducer, int timeoutMs,
    std::function<bool()> shouldCancel,
    std::function<void(std::exception_ptr)> onFirstReadStarted,
    ExactDataFetchPhase phase, std::function<bool()> onValidatedData,
    std::size_t maxOutstanding, std::size_t maxWireBytes, std::function<void()> onReady,
    ExactDataWireRecovery recoverMissing,
    bool hybridKeyBootstrap, std::vector<std::size_t> optionalSlots,
    ExactDataRecoveryReady recoveryReady)
{
  if (m_assignment.inputAdmissionProfile != "data-admission-v1")
    throw std::invalid_argument("INPUT_PROFILE_UNSUPPORTED");
  if (maxOutstanding == 0 || maxOutstanding > 256 || maxWireBytes == 0 ||
      maxWireBytes > 256 * 1024 * 1024 || timeoutMs <= 0 || dataNames.empty() || dataNames.size() > 65536 ||
      expectedProducer.empty() || !m_assignment.scopeKeys.count(keyScope) ||
      std::any_of(dataNames.begin(), dataNames.end(), [&expectedProducer](const ndn::Name& name) {
        return !expectedProducer.isPrefixOf(name) || name.size() <= expectedProducer.size();
      }))
    throw std::invalid_argument("INPUT_SPEC_INVALID");
  size_t namesBytes = 0;
  for (const auto& name : dataNames) {
    checkNamingNameSize(name);
    if (name.wireEncode().size() > maxWireBytes - std::min(namesBytes, maxWireBytes))
      throw std::invalid_argument("INPUT_SPEC_INVALID");
    namesBytes += name.wireEncode().size();
  }
  const auto deadline = std::min(m_assignment.workDeadline,
    std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs));
  const auto authority = currentWorkPredicate(m_assignment.workDeadline);
  const auto current = currentWorkPredicate(deadline);
  auto cancelled = [current, shouldCancel] { return !current() || (shouldCancel && shouldCancel()); };
  timeoutMs = static_cast<int>(std::max<int64_t>(0,
    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count()));
  auto signatureRejected = std::make_shared<std::atomic<bool>>(false);
  std::vector<ndn::Name> authorizedLocations{expectedProducer};
  if (!producerRole.empty()) {
    const auto selected = m_assignment.selectedRoleProviders.find(producerRole);
    if (selected == m_assignment.selectedRoleProviders.end() ||
        std::find(selected->second.begin(), selected->second.end(), expectedProducer) ==
          selected->second.end()) {
      throw std::invalid_argument("INPUT_PRODUCER_MISMATCH");
    }
    for (const auto& provider : selected->second) {
      if (provider == expectedProducer ||
          std::find(authorizedLocations.begin(), authorizedLocations.end(), provider) !=
            authorizedLocations.end()) {
        continue;
      }
      if (authorizedLocations.size() == 16) {
        break;
      }
      checkNamingNameSize(provider);
      authorizedLocations.push_back(provider);
    }
  }
  auto operation = m_provider.startCollaborationSignedExactDataBatch(
    m_requestId, keyScope, dataNames, expectedProducer, timeoutMs,
    std::move(cancelled), std::move(onFirstReadStarted), phase,
    std::move(onValidatedData), {}, maxOutstanding, true, maxWireBytes, std::move(onReady), signatureRejected,
    std::move(authorizedLocations), std::move(recoverMissing), hybridKeyBootstrap,
    std::move(optionalSlots), std::move(recoveryReady));
  const auto binding = ndn::Name(m_requestId).append(m_assignment.service).append(m_assignment.selectionDigest)
    .append(m_assignment.role).append(keyScope).toUri();
  return VerifiedExactDataFetchOperation(std::move(operation), dataNames.front(), expectedProducer,
                                        binding, authority, std::move(signatureRejected));
}
} // namespace ndn_service_framework
