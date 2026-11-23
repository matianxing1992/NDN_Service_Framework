#include "historical-object-publisher.hpp"

#include "AbeHistoricalKeyRequest.hpp"
#include "AbeHistoricalAuthorization.hpp"
#include "HistoricalReadClient.hpp"
#include "HybridMessageCrypto.hpp"
#include "detail/controller-lease-budget.hpp"
#include "detail/historical-policy-status.hpp"
#include "utils.hpp"

#include <nac-abe/producer.hpp>
#include <nac-abe/common.hpp>
#include <ndn-cxx/security/certificate-fetcher-offline.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/validation-policy-simple-hierarchy.hpp>
#include <ndn-cxx/util/string-helper.hpp>

#include <algorithm>
#include <stdexcept>

namespace ndn_service_framework {
namespace {

using Clock = std::chrono::steady_clock;

uint64_t
wallTimeMs()
{
  return ndn::time::duration_cast<ndn::time::milliseconds>(
    ndn::time::system_clock::now().time_since_epoch()).count();
}

uint32_t
remainingMs(const HistoricalObjectPublication& publication)
{
  const auto steady = std::chrono::duration_cast<std::chrono::milliseconds>(
    publication.deadline - Clock::now()).count();
  if (steady <= 0)
    return 0;
  const auto budget = detail::controllerLeaseBudget();
  const auto wall = publication.status.remainingLeaseMs(wallTimeMs(), budget.maxLeaseMs,
                                                       budget.maxClockSkewMs);
  return std::min<uint64_t>({static_cast<uint64_t>(steady), wall, 5000});
}

} // namespace

void
HistoricalObjectPublication::requireCurrent() const
{
  if (remainingMs(*this) == 0)
    throw std::runtime_error("historical publication lease expired");
}

void
publishHistoricalObject(ndn::Face& face, ndn::KeyChain& keys,
                        const ndn::security::Certificate& producer,
                        const ndn::security::Certificate& controller, const ndn::Name& service,
                        const ndn::Name& objectPrefix, ndn::span<const uint8_t> plaintext,
                        const std::function<void(const HistoricalObjectPublication&)>& commit,
                        size_t segmentSize)
{
  // Match the default historical reader's bounds for the assembled NAC envelope.
  const HistoricalReadRequest limits;
  const auto identity = producer.getIdentity();
  if (!commit || !producer.isValid() || !controller.isValid() || service.empty() ||
      identity.size() >= objectPrefix.size() || !identity.isPrefixOf(objectPrefix) ||
      segmentSize == 0 || segmentSize > ndn::MAX_NDN_PACKET_SIZE ||
      plaintext.size() > limits.maxPayloadBytes)
    throw std::invalid_argument("invalid historical publication inputs");

  if (!plaintext.empty() && (plaintext.size() - 1) / segmentSize >= limits.maxSegments)
    throw std::length_error("historical publication exceeds segment budget");

  const auto authority = controller.getIdentity();
  const auto admitted = detail::fetchCurrentPolicyStatus(face, authority, service, controller, 5000);
  if (!admitted || !admitted->data.getAbeMaterialVersion().isValid())
    throw std::runtime_error("historical publication has no current signed material");
  HistoricalObjectPublication result;
  result.status = admitted->data;
  result.deadline = admitted->deadline;
  const auto& parametersName = result.status.getAbePublicParametersName();
  const auto prefix = ndn::Name(authority).append(ndn::nacabe::PUBLIC_PARAMS)
                                          .append(ndn::nacabe::ABE_TYPE_KP_ABE);
  if (parametersName.size() != prefix.size() + 1 || !prefix.isPrefixOf(parametersName) ||
      !parametersName[-1].isVersion())
    throw std::runtime_error("invalid historical publication public parameters name");
  // Only the configured Controller key can satisfy the SDK's parameter validation.
  ndn::security::Validator validator(std::make_unique<ndn::security::ValidationPolicySimpleHierarchy>(),
                                     std::make_unique<ndn::security::CertificateFetcherOffline>());
  validator.loadAnchor(authority.toUri(), ndn::security::Certificate(controller));
  ndn::nacabe::Producer nac(face, keys, validator, producer, controller,
                            ndn::nacabe::ParamFetcher::getDefaultInterestTemplate(), false);
  const auto& recorded = result.status.getAbePublicParametersDigest();
  if (recorded.size() != 71 || recorded.compare(0, 7, "sha256:") != 0)
    throw std::runtime_error("invalid historical publication public parameters digest");
  const auto canonicalDigest = "sha256:" + ndn::toHex(*ndn::fromHex(recorded.substr(7)));
  nac.refreshPublicParameters(parametersName, canonicalDigest);
  while (nac.getPublicParamsDataName() != parametersName && remainingMs(result) != 0)
    face.processEvents(ndn::time::milliseconds(10));
  if (nac.getPublicParamsDataName() != parametersName || nac.getPublicParamsDigest() != canonicalDigest)
    throw std::runtime_error("historical publication public parameters unavailable");
  result.requireCurrent();

  const auto attribute = makeAbeServiceAttribute(service, "PERMISSION");
  const auto signing = ndn::security::signingByCertificate(producer);
  const auto packetTemplate = std::make_shared<ndn::Data>();
  packetTemplate->setFreshnessPeriod(ndn::time::milliseconds(0));
  // A receipt binds one complete CK packet independently of application segment size.
  auto [key, ckPackets] = nac.ckDataGen(std::vector<std::string>{attribute}, signing,
                                       packetTemplate, 7000);
  if (!key || ckPackets.size() != 1 || !ckPackets.front())
    throw std::runtime_error("durable publication requires a complete single-packet NAC CK");
  result.ck = *ckPackets.front();
  const auto ckRoot = historicalCkLocator(result.ck, identity);
  if (!ckRoot)
    throw std::runtime_error("invalid NAC CK packet");
  auto suffix = objectPrefix.getSubName(identity.size());
  suffix.append(makeControlQueryNonce());
  const auto packets = nac.produce(std::move(key), *ckRoot, suffix, plaintext, signing,
                                   packetTemplate, segmentSize);
  if (packets.empty() || packets.size() > limits.maxSegments)
    throw std::length_error("historical publication exceeds segment budget");
  size_t encodedSize = 0;
  for (const auto& packet : packets) {
    if (!packet || packet->getContent().value_size() > limits.maxPayloadBytes - encodedSize)
      throw std::length_error("historical publication exceeds payload budget");
    encodedSize += packet->getContent().value_size();
  }
  result.mapping = makeHistoricalPublicationData(result.status, attribute, result.ck, packets,
                                                 producer, controller, keys, wallTimeMs());
  result.segments.assign(packets.begin(), packets.end());

  result.requireCurrent();
  const auto lifetime = remainingMs(result);
  const auto until = Clock::now() + std::chrono::milliseconds(lifetime);
  struct Confirmation
  {
    bool done = false;
    std::optional<ndn::Name> receipt;
  };
  const auto confirmation = std::make_shared<Confirmation>();
  ndn::ScopedPendingInterestHandle pending = requestHistoricalPublicationReceipt(
    face, keys, result.mapping, producer, controller, lifetime,
    [confirmation] (std::optional<ndn::Name> name) {
      confirmation->receipt = std::move(name);
      confirmation->done = true;
    });
  while (!confirmation->done && Clock::now() < until)
    face.processEvents(ndn::time::milliseconds(10));
  if (!confirmation->receipt || Clock::now() >= until)
    throw std::runtime_error("Controller did not acknowledge the historical publication");
  result.receiptFullName = std::move(*confirmation->receipt);
  result.requireCurrent();
  commit(result);
}

} // namespace ndn_service_framework
