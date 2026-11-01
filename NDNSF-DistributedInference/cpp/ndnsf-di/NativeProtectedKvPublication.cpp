#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedKvPublication.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalArtifactPublisher.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/DecodeStateIdentity.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ConversationStateBinding.hpp"
#include "ndn-service-framework/ProtectedBlobManifest.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>
#include <set>

namespace ndnsf::di {
namespace {
void require(bool valid)
{
  if (!valid) throw std::runtime_error("protected KV publication unavailable");
}
void storedReferenceShape(const NativeJson& value, std::size_t& textBytes)
{
  const std::set<std::string> fields{"version", "ownerIdentity", "producerPrefix", "ownerCertificateName",
    "ownerCertificateDigest", "checkpointIdentityDigest", "wireDigest", "wireBytes", "manifestName", "dataPrefix",
    "publicationIdentity", "protectionEpoch", "keyReferenceId", "keyReferenceVersion", "ciphertextManifestDigest"};
  require(value.is_object() && value.contains("version") && value.at("version").is_number_unsigned());
  const auto version = value.at("version").get<std::uint64_t>();
  require((version == 1 || version == 2) && value.size() == fields.size() + (version == 2 ? 1 : 0));
  for (const auto& field : value.items()) {
    require(fields.count(field.key()) == 1 || (version == 2 && field.key() == "keyEnvelope"));
    if (field.key() == "version" || field.key() == "wireBytes") continue;
    require(field.value().is_string());
    const auto& text = field.value().get_ref<const std::string&>();
    require(!text.empty() && text.size() <= (field.key() == "keyEnvelope" ? 16384U : 4096U) &&
      textBytes <= 128 * 1024 && text.size() <= 128 * 1024 - textBytes);
    textBytes += text.size();
  }
}
std::string publicationIdentity(const NativeProtectedKvCheckpointReferenceV1& checkpoint)
{
  return nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"version", 1U}, {"purpose", "NDNSF-DI/KV-PUBLICATION/V1"},
    {"producer", checkpoint.producerPrefix.toUri(ndn::name::UriFormat::CANONICAL)},
    {"owner", checkpoint.ownerIdentity.toUri(ndn::name::UriFormat::CANONICAL)},
    {"ownerCertificateName", checkpoint.ownerCertificateName.toUri(ndn::name::UriFormat::CANONICAL)},
    {"ownerCertificateDigest", checkpoint.ownerCertificateDigest}, {"checkpointIdentityDigest", checkpoint.checkpointIdentityDigest},
    {"wireDigest", checkpoint.wireDigest}, {"wireBytes", checkpoint.wireBytes}}));
}
} // namespace

NativeProtectedKvPublicationV1::NativeProtectedKvPublicationV1(
  NativeProtectedKvCheckpointReferenceV1 reference, std::string owner,
  ndn_service_framework::LargeDataPublishResult result, std::string inputIdentity)
  : checkpoint(std::move(reference)), ownerIdentity(std::move(owner)), transport(std::move(result)),
    m_inputPublicationIdentity(std::move(inputIdentity)), m_corePublicationIdentity(transport.publicationIdentity),
    m_keyReferenceVersion(transport.keyReferenceVersion), m_keyEnvelope(checkpoint.keyEnvelope),
    m_actualSource(transport.durableStorageSource)
{
  if (!transport.success)
    throw std::runtime_error("protected KV publication unavailable: " + transport.errorMessage);
}

void NativeProtectedKvPublicationV1::requireCurrent() const
{
  using namespace ndn_service_framework;
  (void)checkpoint.objectName();
  require(checkpoint.keyEnvelope == m_keyEnvelope);
  require(!ownerIdentity.empty() && ndn::Name(ownerIdentity) == checkpoint.ownerIdentity &&
    transport.success && transport.encrypted && transport.fileBacked &&
    transport.plaintextSize == checkpoint.wireBytes && transport.contentDigest == checkpoint.wireDigest &&
    m_inputPublicationIdentity == publicationIdentity(checkpoint) &&
    transport.publicationIdentity == m_corePublicationIdentity && transport.keyReferenceVersion == m_keyReferenceVersion &&
    m_corePublicationIdentity.size() == 71 &&
    m_corePublicationIdentity.compare(0, 7, "sha256:") == 0 &&
    std::all_of(m_corePublicationIdentity.begin() + 7, m_corePublicationIdentity.end(), [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }) &&
    !transport.manifestName.empty() && !transport.dataPrefix.empty() &&
    checkpoint.producerPrefix.isPrefixOf(transport.manifestName) &&
    checkpoint.producerPrefix.isPrefixOf(transport.dataPrefix) && transport.encryptedDataName == transport.dataPrefix &&
    m_actualSource && transport.durableStorageSource == m_actualSource && transport.durableStorageSource->isDurable());
  nativeValidateProtectedKvKeyEnvelope(checkpoint);
  transport.durableStorageSource->requireCurrentDurableStorageOwner(transport.manifestName, ownerIdentity);
  require(!transport.auxiliaryPacketNames.empty() &&
    transport.auxiliaryPacketNames == transport.durableStorageSource->auxiliaryDataPacketNames());
  for (const auto& name : transport.auxiliaryPacketNames) {
    const auto original = transport.durableStorageSource->readOriginalDataPacket(name);
    require(!original.empty() && original.size() <= ndn::MAX_NDN_PACKET_SIZE);
    const ndn::Data packet(ndn::Block(ndn::span<const std::uint8_t>(original.data(), original.size())));
    require(packet.getName() == name && checkpoint.producerPrefix.isPrefixOf(name) &&
      packet.getSignatureInfo().hasKeyLocator() && packet.getSignatureInfo().getKeyLocator().getType() == ndn::tlv::Name &&
      checkpoint.producerPrefix.isPrefixOf(packet.getSignatureInfo().getKeyLocator().getName()));
  }
  const auto wire = transport.durableStorageSource->readOriginalDataPacket(transport.manifestName);
  require(!wire.empty() && wire.size() <= ndn::MAX_NDN_PACKET_SIZE);
  const ndn::Data packet(ndn::Block(ndn::span<const std::uint8_t>(wire.data(), wire.size())));
  require(packet.getName() == transport.manifestName && packet.wireEncode().size() == wire.size() &&
    packet.getSignatureInfo().hasKeyLocator() && packet.getSignatureInfo().getKeyLocator().getType() == ndn::tlv::Name &&
    checkpoint.producerPrefix.isPrefixOf(packet.getSignatureInfo().getKeyLocator().getName()));
  const auto& content = packet.getContent();
  const std::vector<std::uint8_t> payload(content.value(), content.value() + content.value_size());
  const auto manifest = ProtectedBlobManifest::decode(payload, checkpoint.producerPrefix, transport.manifestName);
  const auto expectedDigest = selectionGatedUnhex(checkpoint.wireDigest.substr(7));
  require(manifest.contentName && manifest.contentName->kind == ContentDataKind::Blob &&
    manifest.contentName->label == "kvcache" && manifest.plaintextBytes == checkpoint.wireBytes &&
    manifest.plaintextDigest == std::vector<std::uint8_t>(expectedDigest.begin(), expectedDigest.end()) &&
    manifest.dataPrefix == transport.dataPrefix && manifest.protectionEpoch == transport.protectionEpoch &&
    manifest.keyReferenceId == transport.keyReferenceId && !transport.keyReferenceVersion.empty() &&
    nativePlanningDigest(std::string(payload.begin(), payload.end())) == transport.ciphertextManifestDigest);
  require(manifest.segmentCount != 0 && manifest.segmentBytes != 0 &&
    manifest.envelopeBytes == transport.durableStorageSource->size());
  std::uint64_t observedEnvelopeBytes = 0;
  for (std::uint64_t segment = 0; segment < manifest.segmentCount; ++segment) {
    auto segmentName = manifest.dataPrefix;
    segmentName.appendSegment(segment);
    const auto original = transport.durableStorageSource->readOriginalDataPacket(segmentName);
    require(!original.empty() && original.size() <= ndn::MAX_NDN_PACKET_SIZE);
    const ndn::Data segmentPacket(ndn::Block(ndn::span<const std::uint8_t>(
      original.data(), original.size())));
    const auto expectedFinalBlock = ndn::name::Component::fromSegment(manifest.segmentCount - 1);
    require(segmentPacket.getName() == segmentName &&
      segmentPacket.wireEncode().size() == original.size() && segmentPacket.getFinalBlock() &&
      *segmentPacket.getFinalBlock() == expectedFinalBlock &&
      segmentPacket.getSignatureInfo().hasKeyLocator() &&
      segmentPacket.getSignatureInfo().getKeyLocator().getType() == ndn::tlv::Name &&
      checkpoint.producerPrefix.isPrefixOf(
        segmentPacket.getSignatureInfo().getKeyLocator().getName()));
    const auto expectedContentBytes = std::min<std::uint64_t>(
      manifest.segmentBytes, manifest.envelopeBytes - observedEnvelopeBytes);
    require(segmentPacket.getContent().value_size() == expectedContentBytes);
    observedEnvelopeBytes += segmentPacket.getContent().value_size();
  }
  require(observedEnvelopeBytes == manifest.envelopeBytes);
  transport.durableStorageSource->requireCurrentDurableStorageOwner(transport.manifestName, ownerIdentity);
}

NativeProtectedKvPublicationV1 nativePublishProtectedPortableKvCheckpoint(
  std::shared_ptr<ndn_service_framework::ServiceUser> producer,
  const ndn_service_framework::PreparedServiceRequest& prepared,
  const std::filesystem::path& file, const NativeProtectedKvCheckpointReferenceV1& checkpoint,
  const NativeDurableStorageOwner& owner, const NativeProtectedKvCheckpointLimitsV1& limits,
  const std::function<void()>& active)
{
  using namespace ndn_service_framework;
  require(producer && !producer->isOnIoThread() && producer->getName() == checkpoint.producerPrefix &&
    !prepared.serviceName.empty() && !prepared.requestId.empty() &&
    producer->supportsDurableEncryptedLargeData() && producer->supportsDurableAuxiliaryDataPackets() &&
    producer->requiresDurableStorageActor());
  owner.validate(); require(ndn::Name(owner.ownerIdentity) == checkpoint.ownerIdentity);
  nativeValidateProtectedKvKeyEnvelope(checkpoint);
  const auto current = [active, cancelled = limits.cancelled] {
    if (active) active();
    if (cancelled && cancelled()) throw std::runtime_error("protected KV publication cancelled");
  };
  current();
  auto bounded = limits;
  bounded.cancelled = [current] { current(); return false; };
  auto bytes = nativeLoadProtectedPortableKvCheckpoint(file, checkpoint, bounded);
  LargeDataPublishOptions options;
  options.retention = EncryptedLargeDataRetention::Durable;
  options.persistAuxiliaryPackets = true;
  options.publicationIdentity = publicationIdentity(checkpoint);
  PublishedContentName contentName; contentName.kind = ContentDataKind::Blob; contentName.label = "kvcache";
  options.contentName = contentName;
  options.resolveDurableStorageGrant = [resolve = owner.checkedResolver(), expected = checkpoint.producerPrefix]
    (const ndn::Name& manifest, const std::string& writer, const std::function<void()>& current) {
      require(ndn::Name(writer) == expected && expected.isPrefixOf(manifest));
      return resolve(manifest, writer, current);
    };
  if (owner.onGrantRequired) {
    options.onDurableStorageGrantRequired = [notify = owner.onGrantRequired,
      expected = checkpoint.producerPrefix](const DurableStorageGrantChallengeV1& challenge) {
      require(challenge.writerIdentity == expected && expected.isPrefixOf(challenge.manifestName));
      notify(challenge);
    };
  }
  current();
  NativeProtectedKvPublicationV1 result{checkpoint, owner.ownerIdentity,
    producer->publishEncryptedLargeDataFromWorker(prepared, bytes, "kvcache",
      ndn::DEFAULT_FRESHNESS_PERIOD, options, current), options.publicationIdentity};
  current();
  result.requireCurrent(); return result;
}

NativeProtectedKvPublicationV1 nativePublishProtectedPortableKvCheckpoint(
  std::shared_ptr<ndn_service_framework::ServiceProvider> producer,
  const ndn_service_framework::PreparedServiceRequest& prepared,
  const std::filesystem::path& file,
  const NativeProtectedKvCheckpointReferenceV1& checkpoint,
  const NativeDurableStorageOwner& owner,
  std::shared_ptr<ndn_service_framework::EncryptedLargeDataRangeStore> store,
  const NativeProtectedKvCheckpointLimitsV1& limits,
  const std::function<void()>& active)
{
  using namespace ndn_service_framework;
  require(producer && producer->getIdentity() == checkpoint.producerPrefix &&
    !prepared.serviceName.empty() && !prepared.requestId.empty());
  owner.validate();
  require(ndn::Name(owner.ownerIdentity) == checkpoint.ownerIdentity);
  nativeValidateProtectedKvKeyEnvelope(checkpoint);
  const auto current = [active, cancelled = limits.cancelled] {
    if (active) active();
    if (cancelled && cancelled())
      throw std::runtime_error("protected KV publication cancelled");
  };
  current();
  auto bounded = limits;
  bounded.cancelled = [current] { current(); return false; };
  const auto bytes = nativeLoadProtectedPortableKvCheckpoint(file, checkpoint, bounded);

  ProviderDurableLargeDataPublishRequestV1 request;
  request.serviceName = prepared.serviceName;
  request.requestId = prepared.requestId;
  request.protectedPayload = std::move(bytes);
  request.objectLabel = "kvcache";
  request.freshness = ndn::DEFAULT_FRESHNESS_PERIOD;
  request.contentName.kind = ContentDataKind::Blob;
  request.contentName.label = request.objectLabel;
  request.commitOptions.retention = EncryptedLargeDataRetention::Durable;
  request.commitOptions.publicationIdentity = publicationIdentity(checkpoint);

  auto resolve = [checked = owner.checkedResolver(), expected = checkpoint.producerPrefix]
    (const ndn::Name& manifest, const std::string& writer, const std::function<void()>& activeCheck) {
      require(ndn::Name(writer) == expected && expected.isPrefixOf(manifest));
      return checked(manifest, writer, activeCheck);
    };
  DurableStorageGrantChallengeObserverV1 onRequired;
  if (owner.onGrantRequired) {
    onRequired = [notify = owner.onGrantRequired, expected = checkpoint.producerPrefix]
      (const DurableStorageGrantChallengeV1& challenge) {
        require(ndn::Name(challenge.writerIdentity) == expected &&
                expected.isPrefixOf(challenge.manifestName));
        notify(challenge);
      };
  }

  current();
  auto published = ProviderDurableLargeDataPublisher::publish(
    producer, std::move(request), std::move(store), std::move(resolve),
    std::move(onRequired), current, owner.onDurablePacketSetBoundary);
  current();

  LargeDataPublishResult transport;
  transport.success = published.success;
  transport.encryptedDataName = published.encryptedDataName;
  transport.objectId = published.objectId;
  transport.plaintextSize = static_cast<std::size_t>(published.plaintextSize);
  transport.contentDigest = published.contentDigest;
  transport.manifestDigest = published.manifestDigest;
  transport.authorizationScope = published.authorizationScope;
  transport.protectionEpoch = published.protectionEpoch;
  transport.publicationIdentity = published.publicationIdentity;
  transport.keyReferenceId = published.keyReferenceId;
  transport.keyReferenceVersion = published.keyReferenceVersion;
  transport.ciphertextManifestDigest = published.ciphertextManifestDigest;
  transport.servingLocator = published.servingLocator;
  transport.encrypted = published.encrypted;
  transport.fileBacked = published.fileBacked;
  transport.manifestName = published.manifestName;
  transport.dataPrefix = published.dataPrefix;
  transport.durableStorageSource = std::move(published.durableStorageSource);
  transport.auxiliaryPacketNames = std::move(published.auxiliaryPacketNames);
  transport.errorMessage = std::move(published.errorMessage);
  NativeProtectedKvPublicationV1 result{checkpoint, owner.ownerIdentity,
    std::move(transport), publicationIdentity(checkpoint)};
  current();
  result.requireCurrent();
  return result;
}

NativeVerifiedRepoProtectedKvKeyGraphV1
nativeBindRepoProtectedKvKeyGraphV1(
  ndnsf_distributed_repo::VerifiedRepoUserStoragePacketSetV1 packetSet,
  ndn_service_framework::VerifiedHybridMessageKeyWrapperPacketSetV1 wrapper,
  ndn_service_framework::VerifiedHybridMessageKeyEnvelopeV1 envelope,
  ndn_service_framework::VerifiedNacContentKeyPacketSetV1 contentKeys,
  const std::function<void()>& requireActive)
{
  const auto checkActive = [&] { if (requireActive) requireActive(); };
  checkActive();
  const auto& target = packetSet.target();
  const auto& claim = packetSet.claim();
  const auto& manifest = packetSet.manifest();
  const auto& wires = packetSet.packetWires();
  const auto& producerPrefix = contentKeys.producerPrefix();
  require(!target.repoNode.empty() && !target.providerIdentity.empty() &&
    !target.servicePrefix.empty() && claim.repoNode == target.repoNode &&
    !claim.ownerIdentity.empty() && !claim.grantId.empty() && claim.ownerEpoch != 0 &&
    claim.state == ndnsf_distributed_repo::RepoUserStorageClaimState::Committed &&
    ndn::Name(claim.object.objectName) == ndn::Name(manifest.objectName) &&
    claim.object.wireSha256 == manifest.sha256 &&
    claim.object.generation == manifest.generation && claim.object.payloadWireBytes == manifest.size &&
    manifest.objectType == "original-packet-set" && manifest.lifecycleState == "COMMITTED" &&
    !manifest.packetNames.empty() &&
    ndn::Name(manifest.packetNames.front()) == ndn::Name(manifest.objectName) &&
    manifest.packetNames.size() == wires.size() && manifest.packetNames.size() <= 65536 &&
    manifest.packetOffsets.size() == manifest.packetNames.size() + 1 &&
    manifest.packetOffsets.front() == 0 && manifest.packetOffsets.back() == manifest.size &&
    !producerPrefix.empty() && producerPrefix.isPrefixOf(wrapper.objectName()) &&
    envelope.wrapperObjectName() == wrapper.objectName() &&
    envelope.packetFingerprints() == wrapper.packetFingerprints() &&
    contentKeys.contentKeyName() == envelope.contentKeyName() &&
    producerPrefix.isPrefixOf(envelope.contentKeyName()));

  for (const auto& packetNameText : manifest.packetNames) {
    checkActive();
    const ndn::Name packetName(packetNameText);
    require(!packetName.empty() && producerPrefix.isPrefixOf(packetName));
  }

  auto requiredPackets = wrapper.packetFingerprints();
  const auto& keyPackets = contentKeys.packetFingerprints();
  require(requiredPackets.size() <= 65536 && keyPackets.size() <= 65536 - requiredPackets.size());
  requiredPackets.insert(requiredPackets.end(), keyPackets.begin(), keyPackets.end());
  ndn_service_framework::requireOriginalPacketFingerprintsPresentV1(
    manifest.packetNames, wires, requiredPackets, requireActive);
  checkActive();

  return NativeVerifiedRepoProtectedKvKeyGraphV1(
    std::move(packetSet), std::move(wrapper), std::move(envelope), std::move(contentKeys));
}

NativeVerifiedRemoteKvPublicationV1
nativeValidateRemoteProtectedKvPublicationV1(
  NativeVerifiedRepoProtectedKvKeyGraphV1 keyGraph, const NativeJson& ownerReference,
  const std::function<void()>& requireActive)
{
  const auto checkActive = [&] { if (requireActive) requireActive(); };
  checkActive();
  const auto& packetSet = keyGraph.packetSet();
  const auto& claim = packetSet.claim();
  const auto& repoManifest = packetSet.manifest();
  const auto& packetWires = packetSet.packetWires();
  const auto checkpoint = nativeReadProtectedKvPublicationReference(ownerReference);
  require(ownerReference.at("version").get<std::uint64_t>() == 2 &&
    claim.state == ndnsf_distributed_repo::RepoUserStorageClaimState::Committed &&
    claim.ownerEpoch != 0 && !claim.grantId.empty() && claim.repoNode == packetSet.target().repoNode &&
    ndn::Name(claim.ownerIdentity) == checkpoint.ownerIdentity &&
    ndn::Name(claim.object.objectName) == ndn::Name(repoManifest.objectName) &&
    claim.object.wireSha256 == repoManifest.sha256 &&
    claim.object.generation == repoManifest.generation && claim.object.payloadWireBytes == repoManifest.size &&
    repoManifest.objectType == "original-packet-set" && repoManifest.lifecycleState == "COMMITTED" &&
    !repoManifest.packetNames.empty() && repoManifest.packetNames.size() == packetWires.size() &&
    repoManifest.packetNames.size() <= 65536 &&
    repoManifest.packetOffsets.size() == repoManifest.packetNames.size() + 1 &&
    repoManifest.packetOffsets.front() == 0 && repoManifest.packetOffsets.back() == repoManifest.size &&
    repoManifest.contentDigest == checkpoint.wireDigest && repoManifest.plaintextSize == checkpoint.wireBytes &&
    repoManifest.publicationIdentity == publicationIdentity(checkpoint) &&
    repoManifest.publicationIdentity == ownerReference.at("publicationIdentity").get<std::string>() &&
    repoManifest.protectionEpoch == ownerReference.at("protectionEpoch").get<std::string>() &&
    repoManifest.keyReferenceId == ownerReference.at("keyReferenceId").get<std::string>() &&
    repoManifest.keyReferenceVersion == ownerReference.at("keyReferenceVersion").get<std::string>() &&
    repoManifest.ciphertextManifestDigest == ownerReference.at("ciphertextManifestDigest").get<std::string>());

  for (std::size_t i = 0; i < packetWires.size(); ++i) {
    checkActive();
    require(!packetWires[i].empty() && repoManifest.packetOffsets[i + 1] >= repoManifest.packetOffsets[i] &&
      repoManifest.packetOffsets[i + 1] - repoManifest.packetOffsets[i] == packetWires[i].size());
  }

  const auto ownerName = [](const std::string& text) {
    const ndn::Name name(text);
    require(!name.empty() && name.toUri(ndn::name::UriFormat::CANONICAL) == text &&
      name.wireEncode().size() <= 4096);
    return name;
  };
  const auto repoName = [](const std::string& text) {
    const ndn::Name name(text);
    require(!name.empty() &&
      (name.toUri(ndn::name::UriFormat::CANONICAL) == text ||
       name.toUri(ndn::name::UriFormat::ALTERNATE) == text) &&
      name.wireEncode().size() <= 4096);
    return name;
  };
  const auto manifestName = repoName(repoManifest.objectName);
  const auto dataPrefix = ownerName(ownerReference.at("dataPrefix").get<std::string>());
  const auto repoDataPrefix = ownerName(repoManifest.dataPrefix);
  const auto servingLocator = ownerName(repoManifest.servingLocator);
  require(manifestName == ownerName(ownerReference.at("manifestName").get<std::string>()) &&
    checkpoint.producerPrefix.isPrefixOf(manifestName) &&
    dataPrefix == repoDataPrefix && servingLocator == repoDataPrefix);

  std::map<ndn::Name, std::size_t> packetIndexes;
  for (std::size_t i = 0; i < repoManifest.packetNames.size(); ++i) {
    checkActive();
    const auto name = repoName(repoManifest.packetNames[i]);
    require(checkpoint.producerPrefix.isPrefixOf(name) && packetIndexes.emplace(name, i).second);
  }
  const auto readOriginal = [&](const ndn::Name& name) {
    checkActive();
    const auto found = packetIndexes.find(name);
    require(found != packetIndexes.end());
    const auto& wire = packetWires[found->second];
    require(!wire.empty() && wire.size() <= ndn::MAX_NDN_PACKET_SIZE);
    ndn::Data packet(ndn::Block(ndn::span<const std::uint8_t>(wire.data(), wire.size())));
    const auto& encoded = packet.wireEncode();
    require(packet.getName() == name && encoded.size() == wire.size() &&
      std::equal(encoded.begin(), encoded.end(), wire.begin()) &&
      packet.getSignatureInfo().hasKeyLocator() &&
      packet.getSignatureInfo().getKeyLocator().getType() == ndn::tlv::Name &&
      checkpoint.producerPrefix.isPrefixOf(packet.getSignatureInfo().getKeyLocator().getName()));
    return packet;
  };

  const auto rootPacket = readOriginal(manifestName);
  const auto content = rootPacket.getContent();
  require(content.value_size() != 0 && content.value_size() <= ndn::MAX_NDN_PACKET_SIZE);
  std::vector<std::uint8_t> rootPayload(content.value(), content.value() + content.value_size());
  auto root = ndn_service_framework::ProtectedBlobManifest::decode(
    rootPayload, checkpoint.producerPrefix, manifestName);
  const auto expectedDigest =
    ndn_service_framework::selectionGatedUnhex(checkpoint.wireDigest.substr(7));
  require(root.contentName && root.contentName->kind == ndn_service_framework::ContentDataKind::Blob &&
    root.contentName->label == "kvcache" && root.plaintextBytes == checkpoint.wireBytes &&
    root.plaintextDigest == std::vector<std::uint8_t>(expectedDigest.begin(), expectedDigest.end()) &&
    root.dataPrefix == dataPrefix && root.protectionEpoch == repoManifest.protectionEpoch &&
    root.keyReferenceId == repoManifest.keyReferenceId &&
    nativePlanningDigest(std::string(rootPayload.begin(), rootPayload.end())) ==
      repoManifest.ciphertextManifestDigest &&
    root.envelopeBytes == repoManifest.envelopeBytes && root.envelopeBytes != 0 &&
    root.segmentCount != 0 && root.segmentCount <= 65536 && root.segmentBytes != 0 &&
    root.segmentCount == 1 + (root.envelopeBytes - 1) / root.segmentBytes);

  const auto& scopeText = root.authorizationScope;
  const ndn::Name scope(scopeText);
  require(scope.size() > 1 &&
    (scope[0] == ndn::name::Component("SERVICE") ||
     scope[0] == ndn::name::Component("PERMISSION")));
  const auto serviceName = scope.getSubName(1);
  const auto attributeKind = scope[0] == ndn::name::Component("SERVICE") ? "SERVICE" : "PERMISSION";
  require(!serviceName.empty() &&
    ndn_service_framework::makeAbeServiceAttribute(serviceName, attributeKind) == scopeText);
  const auto wrapperLocator = ndn_service_framework::makeHybridMessageKeyDataName(
    serviceName, checkpoint.producerPrefix, scopeText, root.protectionEpoch);
  const auto& wrapperName = keyGraph.wrapper().objectName();
  require(wrapperLocator.isPrefixOf(wrapperName) && wrapperName.size() == wrapperLocator.size() + 1 &&
    wrapperName.getPrefix(-1) == wrapperLocator && wrapperName[-1].type() == 8 &&
    wrapperName[-1].value_size() == 32);

  std::set<ndn::Name> expectedPacketNames;
  require(expectedPacketNames.insert(manifestName).second);
  std::uint64_t observedEnvelopeBytes = 0;
  for (std::uint64_t segment = 0; segment < root.segmentCount; ++segment) {
    checkActive();
    auto segmentName = root.dataPrefix;
    segmentName.appendSegment(segment);
    const auto packet = readOriginal(segmentName);
    require(packet.getFinalBlock() && packet.getFinalBlock()->isSegment() &&
      *packet.getFinalBlock() == ndn::name::Component::fromSegment(root.segmentCount - 1) &&
      observedEnvelopeBytes <= root.envelopeBytes);
    const auto expectedContentBytes = std::min<std::uint64_t>(root.segmentBytes,
      root.envelopeBytes - observedEnvelopeBytes);
    require(packet.getContent().value_size() == expectedContentBytes &&
      expectedPacketNames.insert(segmentName).second);
    observedEnvelopeBytes += expectedContentBytes;
  }
  require(observedEnvelopeBytes == root.envelopeBytes);
  const auto addRequiredNames = [&](const auto& fingerprints) {
    for (const auto& item : fingerprints) {
      checkActive();
      require(checkpoint.producerPrefix.isPrefixOf(item.first) && expectedPacketNames.insert(item.first).second);
    }
  };
  addRequiredNames(keyGraph.wrapper().packetFingerprints());
  addRequiredNames(keyGraph.contentKeys().packetFingerprints());
  require(expectedPacketNames.size() == packetIndexes.size());
  for (const auto& item : packetIndexes) {
    checkActive();
    require(expectedPacketNames.count(item.first) == 1);
  }
  checkActive();

  return NativeVerifiedRemoteKvPublicationV1(
    ownerReference, std::move(keyGraph), std::move(root));
}

NativeVerifiedRemoteKvPublicationV1
nativeFetchAndValidateRemoteProtectedKvPublicationV1(
  ndn_service_framework::ServiceUser& owner,
  const ndnsf_distributed_repo::RepoArtifactTarget& target,
  const NativeJson& ownerReference,
  const std::function<bool(const ndn::Data&)>& verifyProducerSignature,
  int timeoutMs,
  ndnsf_distributed_repo::RepoUserStoragePacketSetFetchLimits limits,
  const std::function<void()>& requireActive)
{
  if (timeoutMs <= 0 || !verifyProducerSignature || owner.isOnIoThread() ||
      target.repoNode.empty() || target.providerIdentity.empty() || target.servicePrefix.empty())
    throw std::invalid_argument("remote protected KV Repo fetch inputs are invalid");

  const auto checkpoint = nativeReadProtectedKvPublicationReference(ownerReference);
  const auto manifestNameText = ownerReference.at("manifestName").get<std::string>();
  const ndn::Name manifestName(manifestNameText);
  require(ownerReference.at("version").get<std::uint64_t>() == 2 &&
    owner.getName() == checkpoint.ownerIdentity &&
    !manifestName.empty() &&
    manifestName.toUri(ndn::name::UriFormat::CANONICAL) == manifestNameText &&
    checkpoint.producerPrefix.isPrefixOf(manifestName));
  const auto repoObjectName = manifestName.toUri(ndn::name::UriFormat::ALTERNATE);

  const auto deadline = std::chrono::steady_clock::now() +
    std::chrono::milliseconds(timeoutMs);
  const auto checkActive = [&] {
    if (requireActive)
      requireActive();
    if (std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error("remote protected KV validation deadline expired");
  };
  checkActive();
  const auto boundedVerifier = [&] (const ndn::Data& data) {
    checkActive();
    const auto verified = verifyProducerSignature(data);
    checkActive();
    return verified;
  };
  auto packetSet = ndnsf_distributed_repo::RepoClient::fetchOriginalDataPacketSetFromUserStorage(
    owner, target, repoObjectName, boundedVerifier, timeoutMs, limits);
  checkActive();
  require(packetSet.target() == target && !packetSet.packetWires().empty() &&
    !packetSet.manifest().packetNames.empty() &&
    ndn::Name(packetSet.manifest().packetNames.front()) == manifestName);
  const auto observedClaim = packetSet.claim();

  const auto& rootWire = packetSet.packetWires().front();
  ndn::Data rootPacket(ndn::Block(ndn::span<const std::uint8_t>(rootWire.data(), rootWire.size())));
  const auto rootContent = rootPacket.getContent();
  require(rootPacket.getName() == manifestName && rootContent.value_size() > 0);
  std::vector<std::uint8_t> rootPayload(rootContent.value(),
    rootContent.value() + rootContent.value_size());
  const auto remoteRoot = ndn_service_framework::ProtectedBlobManifest::decode(
    rootPayload, checkpoint.producerPrefix, manifestName);
  checkActive();
  const ndn::Name authorizationScope(remoteRoot.authorizationScope);
  require(authorizationScope.size() > 1);
  const auto wrapperLocator = ndn_service_framework::makeHybridMessageKeyDataName(
    authorizationScope.getSubName(1), checkpoint.producerPrefix,
    remoteRoot.authorizationScope, remoteRoot.protectionEpoch);
  std::vector<std::vector<std::uint8_t>> wrapperPacketWires;
  for (const auto& wire : packetSet.packetWires()) {
    checkActive();
    const ndn::Data packet(ndn::Block(ndn::span<const std::uint8_t>(wire.data(), wire.size())));
    if (wrapperLocator.isPrefixOf(packet.getName()))
      wrapperPacketWires.push_back(wire);
  }
  auto wrapper = ndn_service_framework::validateHybridMessageKeyWrapperPacketSet(
    wrapperLocator, wrapperPacketWires, boundedVerifier);
  checkActive();
  auto envelope = ndn_service_framework::validateHybridMessageKeyWrapperEnvelopeV1(wrapper);
  auto contentKeys = ndn_service_framework::validateNacContentKeyPacketSetV1(
    envelope.contentKeyName(), checkpoint.producerPrefix,
    packetSet.packetWires(), boundedVerifier);
  checkActive();
  auto keyGraph = nativeBindRepoProtectedKvKeyGraphV1(
    std::move(packetSet), std::move(wrapper), std::move(envelope), std::move(contentKeys), checkActive);
  auto proof = nativeValidateRemoteProtectedKvPublicationV1(
    std::move(keyGraph), ownerReference, checkActive);
  checkActive();

  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
    deadline - std::chrono::steady_clock::now()).count();
  if (remaining <= 0)
    throw std::runtime_error("remote protected KV claim recheck deadline expired");
  (void)ndnsf_distributed_repo::RepoClient::revalidateCurrentUserStorageClaim(
    owner, target, observedClaim, static_cast<int>(remaining), limits);
  checkActive();
  return proof;
}

NativeConversationRecord
NativeRemoteProtectedKvSetPromoter::fetchAndPromote(
  ndn_service_framework::ServiceUser& owner,
  NativeConversationCoordinator& coordinator,
  const std::string& conversationId,
  const std::string& expectedCheckpointDigest,
  const std::map<std::string, NativeRemoteProtectedKvRoleSourceV1>& roleSources,
  int timeoutMs,
  const std::map<std::string, std::string>& nextTurnProvidersByRole,
  const std::function<bool()>& cancelled)
{
  constexpr std::uint64_t MAX_TOTAL_LISTING_PAGES = 2048;
  constexpr std::uint64_t MAX_TOTAL_LISTED_OBJECTS = 204800;
  constexpr std::uint64_t MAX_TOTAL_LISTING_WIRE_BYTES = 192ULL << 20;
  constexpr std::uint64_t MAX_TOTAL_PACKET_COUNT = 16384;
  constexpr std::uint64_t MAX_TOTAL_PACKET_WIRE_BYTES = 256ULL << 20;
  constexpr std::uint64_t MAX_TOTAL_MANIFEST_WIRE_BYTES = 16ULL << 20;

  if (timeoutMs <= 0 || owner.isOnIoThread() || roleSources.empty() || roleSources.size() > 64)
    throw std::invalid_argument("remote protected KV role promotion inputs are invalid");
  const auto current = coordinator.find(conversationId);
  require(current && current->checkpoint.checkpointDigest == expectedCheckpointDigest &&
    current->checkpoint.expectedRoles.size() == roleSources.size() &&
    current->checkpoint.providersByRole.size() == roleSources.size());

  std::set<std::string> expectedRoles(current->checkpoint.expectedRoles.begin(),
                                      current->checkpoint.expectedRoles.end());
  require(expectedRoles.size() == current->checkpoint.expectedRoles.size());
  if (!nextTurnProvidersByRole.empty()) {
    require(nextTurnProvidersByRole.size() == expectedRoles.size() &&
      nextTurnProvidersByRole != current->checkpoint.providersByRole);
    auto destinationRoles = expectedRoles;
    std::set<std::string> destinationProviders;
    for (const auto& [role, provider] : nextTurnProvidersByRole) {
      require(destinationRoles.erase(role) == 1 && !provider.empty() && provider.size() <= 4096);
      const ndn::Name identity(provider);
      require(!identity.empty() && identity.wireEncode().size() <= 4096);
      destinationProviders.insert(provider);
    }
    require(destinationRoles.empty() && destinationProviders.size() <= 16);
  }
  std::uint64_t totalPages = 0;
  std::uint64_t totalListedObjects = 0;
  std::uint64_t totalListingBytes = 0;
  std::uint64_t totalPacketCountLimit = 0;
  std::uint64_t totalPacketBytesLimit = 0;
  std::uint64_t totalManifestBytesLimit = 0;
  const auto addBounded = [] (std::uint64_t& total, std::uint64_t amount, std::uint64_t maximum) {
    require(amount <= maximum && total <= maximum - amount);
    total += amount;
  };
  const auto addRepeatedBounded = [&] (std::uint64_t& total, std::uint64_t amount,
                                        std::uint64_t scans, std::uint64_t maximum) {
    require(scans != 0 && amount <= maximum / scans);
    addBounded(total, amount * scans, maximum);
  };
  for (const auto& [role, source] : roleSources) {
    const auto sourceCheckpoint =
      nativeReadProtectedKvPublicationReference(source.ownerReference);
    require(expectedRoles.count(role) == 1 && current->checkpoint.providersByRole.count(role) == 1 &&
      sourceCheckpoint.producerPrefix ==
        ndn::Name(current->checkpoint.providersByRole.at(role)));
    const auto& limits = source.limits;
    // Each role may cause four page/object scans: packet-set fetch initial and
    // final scans, semantic post-check, and the final all-role freshness pass.
    addRepeatedBounded(totalPages, limits.maxListingPagesPerScan, 4,
                       MAX_TOTAL_LISTING_PAGES);
    addRepeatedBounded(totalListedObjects, limits.maxListedObjectsPerScan, 4,
                       MAX_TOTAL_LISTED_OBJECTS);
    // maxListingWireBytes covers the packet fetch's first two scans together;
    // each of the two later claim checks receives that budget independently.
    addRepeatedBounded(totalListingBytes, limits.maxListingWireBytes, 3,
                       MAX_TOTAL_LISTING_WIRE_BYTES);
    addBounded(totalPacketCountLimit, limits.maxPacketCount, MAX_TOTAL_PACKET_COUNT);
    addBounded(totalPacketBytesLimit, limits.maxPacketWireBytes, MAX_TOTAL_PACKET_WIRE_BYTES);
    addBounded(totalManifestBytesLimit, limits.maxManifestWireBytes, MAX_TOTAL_MANIFEST_WIRE_BYTES);
  }
  require(expectedRoles.size() == roleSources.size());

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  const auto remainingMs = [&] {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0 || remaining > std::numeric_limits<int>::max())
      throw std::runtime_error("remote protected KV role promotion deadline expired");
    return static_cast<int>(remaining);
  };
  const auto requireActive = [&] {
    if (cancelled && cancelled())
      throw std::runtime_error("remote protected KV role promotion cancelled");
    (void)remainingMs();
  };
  requireActive();

  std::map<std::string, NativeVerifiedRemoteKvPublicationV1> proofs;
  std::uint64_t actualPacketCount = 0;
  std::uint64_t actualPacketBytes = 0;
  for (const auto& [role, source] : roleSources) {
    requireActive();
    const auto timeout = remainingMs();
    auto proof = nativeFetchAndValidateRemoteProtectedKvPublicationV1(
      owner, source.target, source.ownerReference, source.verifyProducerSignature,
      timeout, source.limits, requireActive);
    const auto& packetSet = proof.keyGraph().packetSet();
    addBounded(actualPacketCount, static_cast<std::uint64_t>(packetSet.packetWires().size()),
               totalPacketCountLimit);
    for (const auto& wire : packetSet.packetWires())
      addBounded(actualPacketBytes, static_cast<std::uint64_t>(wire.size()), totalPacketBytesLimit);
    proofs.emplace(role, std::move(proof));
  }

  // The first role may have gone stale while later roles were being fetched.
  // Re-observe every claim after the full proof set is ready, then promote once.
  for (const auto& [role, proof] : proofs) {
    requireActive();
    const auto& source = roleSources.at(role);
    (void)ndnsf_distributed_repo::RepoClient::revalidateCurrentUserStorageClaim(
      owner, source.target, proof.keyGraph().packetSet().claim(), remainingMs(), source.limits);
    requireActive();
  }
  requireActive();
  return coordinator.promoteProtectedKvSet(
    conversationId, expectedCheckpointDigest, proofs, nextTurnProvidersByRole, requireActive);
}

NativeJson nativeProtectedKvPublicationReference(const NativeProtectedKvPublicationV1& publication)
{
  publication.requireCurrent();
  const auto& reference = publication.checkpoint;
  const auto& transport = publication.transport;
  const auto name = [](const ndn::Name& value) {
    require(!value.empty() && value.wireEncode().size() <= 4096);
    auto text = value.toUri(ndn::name::UriFormat::CANONICAL); require(text.size() <= 4096); return text;
  };
  for (const auto* text : {&reference.ownerCertificateDigest, &reference.checkpointIdentityDigest,
    &reference.wireDigest, &transport.publicationIdentity, &transport.protectionEpoch,
    &transport.keyReferenceId, &transport.keyReferenceVersion, &transport.ciphertextManifestDigest})
    require(!text->empty() && text->size() <= 4096);
  return {{"version", 2U}, {"ownerIdentity", name(reference.ownerIdentity)},
    {"producerPrefix", name(reference.producerPrefix)}, {"ownerCertificateName", name(reference.ownerCertificateName)},
    {"ownerCertificateDigest", reference.ownerCertificateDigest},
    {"checkpointIdentityDigest", reference.checkpointIdentityDigest}, {"wireDigest", reference.wireDigest},
    {"wireBytes", reference.wireBytes}, {"manifestName", name(transport.manifestName)},
    {"dataPrefix", name(transport.dataPrefix)}, {"publicationIdentity", transport.publicationIdentity},
    {"protectionEpoch", transport.protectionEpoch}, {"keyReferenceId", transport.keyReferenceId},
    {"keyReferenceVersion", transport.keyReferenceVersion}, {"ciphertextManifestDigest", transport.ciphertextManifestDigest},
    {"keyEnvelope", reference.keyEnvelope}};
}

std::string nativeProtectedKvRoleBinding(const NativeJson& checkpoint, const std::string& role)
{
  require(!role.empty() && role.size() <= 4096 && checkpoint.at("roleReceiptDigests").contains(role));
  return nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"version", 1U}, {"purpose", "NDNSF-DI/KV-ROLE-CHECKPOINT/V1"},
    {"requesterIdentity", checkpoint.at("requesterIdentity")}, {"conversationId", checkpoint.at("conversationId")},
    {"contextEpoch", checkpoint.at("contextEpoch")}, {"checkpointDigest", checkpoint.at("checkpointDigest")},
    {"roleName", role}, {"roleReceiptDigest", checkpoint.at("roleReceiptDigests").at(role)}}));
}

std::string nativeProtectedKvRoleBinding(const std::string& requesterIdentity,
                                        const ConversationStateReferenceV1& reference)
{
  reference.validate();
  return nativeProtectedKvRoleBinding(NativeJson{
    {"requesterIdentity", requesterIdentity}, {"conversationId", reference.conversationId},
    {"contextEpoch", reference.contextEpoch}, {"checkpointDigest", reference.checkpointDigest},
    {"roleReceiptDigests", {{reference.roleName, reference.roleReceiptDigest}}}}, reference.roleName);
}

NativeProtectedKvCheckpointReferenceV1 nativeReadProtectedKvPublicationReference(const NativeJson& value)
{
  std::size_t textBytes = 0; storedReferenceShape(value, textBytes);
  const auto name = [&](const char* field) {
    const auto& text = value.at(field).get_ref<const std::string&>();
    const ndn::Name result(text);
    require(!result.empty() && result.wireEncode().size() <= 4096 &&
      result.toUri(ndn::name::UriFormat::CANONICAL) == text); return result;
  };
  const auto digest = [&](const char* field) {
    const auto& text = value.at(field).get_ref<const std::string&>();
    require(decode_state_identity_detail::isSha256Digest(text)); return text;
  };
  require(value.at("wireBytes").is_number_unsigned() && value.at("wireBytes").get<std::uint64_t>() > 0 &&
    value.at("wireBytes").get<std::uint64_t>() <= 1000000000ULL);
  NativeProtectedKvCheckpointReferenceV1 reference{name("ownerIdentity"), name("producerPrefix"),
    name("ownerCertificateName"), digest("ownerCertificateDigest"), digest("checkpointIdentityDigest"),
    digest("wireDigest"), value.at("wireBytes").get<std::uint64_t>()};
  if (value.at("version").get<std::uint64_t>() == 2) {
    reference.keyEnvelope = value.at("keyEnvelope").get<std::string>();
    nativeValidateProtectedKvKeyEnvelope(reference);
  }
  (void)reference.objectName();
  require(reference.ownerIdentity.isPrefixOf(reference.ownerCertificateName) &&
    reference.ownerCertificateName.size() > reference.ownerIdentity.size());
  for (const auto field : {"manifestName", "dataPrefix"}) {
    const auto transportName = name(field);
    require(reference.producerPrefix.isPrefixOf(transportName) && transportName.size() > reference.producerPrefix.size());
  }
  (void)digest("publicationIdentity"); (void)digest("ciphertextManifestDigest");
  return reference;
}

void nativeValidateProtectedKvReferences(const NativeJson& references, const NativeJson& checkpoint,
                                        const std::map<std::string, std::string>& providersByRole)
{
  require(references.is_object() && !references.empty() && references.size() <= 64 &&
    references.size() == checkpoint.at("roleReceiptDigests").size() && references.size() == providersByRole.size());
  constexpr std::size_t textBudget = 128 * 1024;
  std::size_t textBytes = 0;
  const auto consume = [&](const std::string& value, std::size_t limit = 4096) {
    require(!value.empty() && value.size() <= limit && value.size() <= textBudget - textBytes);
    textBytes += value.size();
  };
  // Budget all caller-controlled text before canonicalization or name decoding.
  for (const auto& item : references.items()) {
    consume(item.key());
    storedReferenceShape(item.value(), textBytes);
  }
  for (const auto& item : references.items()) {
    require(checkpoint.at("roleReceiptDigests").contains(item.key()) && providersByRole.count(item.key()) == 1);
    const auto reference = nativeReadProtectedKvPublicationReference(item.value());
    require(reference.ownerIdentity == ndn::Name(checkpoint.at("requesterIdentity").get<std::string>()) &&
      reference.producerPrefix == ndn::Name(providersByRole.at(item.key())) &&
      reference.checkpointIdentityDigest == nativeProtectedKvRoleBinding(checkpoint, item.key()));
  }
  require(nativeCanonicalJson(references).size() <= 1024 * 1024);
}
} // namespace ndnsf::di
