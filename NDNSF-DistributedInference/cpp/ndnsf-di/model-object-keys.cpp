#include "model-object-keys.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalArtifactPublisher.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-capability.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/request-work-statistics.hpp"

#include <algorithm>
#include <chrono>
#include <openssl/crypto.h>
#include <optional>
#include <set>
#include <vector>

namespace ndnsf::di {
namespace {

constexpr std::uint64_t MAX_TABLE_BYTES = 4U << 20;
constexpr std::size_t STORED_OBJECT_KEY_BYTES = 32;

bool
storedNameWireLess(const ndn::Name& left, const ndn::Name& right)
{
  const auto& leftWire = left.wireEncode();
  const auto& rightWire = right.wireEncode();
  return std::lexicographical_compare(leftWire.begin(), leftWire.end(),
    rightWire.begin(), rightWire.end());
}

std::uint64_t
epochMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

void
require(bool condition)
{
  if (!condition) throw std::invalid_argument("DI_MODEL_OBJECT_KEYS_INVALID");
}

void
requireDigest(const std::string& digest)
{
  require(digest.size() == 71 && digest.compare(0, 7, "sha256:") == 0 &&
    std::all_of(digest.begin() + 7, digest.end(), [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }));
}

bool
isCanonicalName(const std::string& value)
{
  try {
    return !value.empty() && value.front() == '/' && !ndn::Name(value).empty() &&
      ndn::Name(value).toUri(ndn::name::UriFormat::CANONICAL) == value;
  }
  catch (...) {
    return false;
  }
}

ndn::Buffer
contextBytes(const NativeJson& context)
{
  const auto wire = nativeCanonicalJson(context);
  require(wire.size() <= 16384);
  return ndn::Buffer(wire.begin(), wire.end());
}

NativeJson
storedPublicationJson(const NativeStoredModelReference& publication)
{
  publication.validate();
  return NativeJson{{"producer", publication.producer.toUri(ndn::name::UriFormat::CANONICAL)},
    {"repository", publication.repository.toUri(ndn::name::UriFormat::CANONICAL)},
    {"rootName", publication.rootName.toUri(ndn::name::UriFormat::CANONICAL)},
    {"rootWireDigest", publication.rootWireDigest}};
}

NativeStoredModelReference
storedPublicationFromJson(const NativeJson& value)
{
  require(value.is_object() && value.size() == 4 && value.at("producer").is_string() &&
    value.at("repository").is_string() && value.at("rootName").is_string() &&
    value.at("rootWireDigest").is_string());
  NativeStoredModelReference publication{ndn::Name(value.at("producer").get<std::string>()),
    ndn::Name(value.at("repository").get<std::string>()),
    ndn::Name(value.at("rootName").get<std::string>()),
    value.at("rootWireDigest").get<std::string>()};
  publication.validate();
  require(storedPublicationJson(publication) == value);
  return publication;
}

NativeJson
storedObjectJson(const NativeStoredModelIndexedObject& indexed)
{
  const auto& object = indexed.object;
  return NativeJson{{"name", object.name.toUri(ndn::name::UriFormat::CANONICAL)},
    {"plaintextDigest", object.plaintextDigest}, {"ciphertextDigest", object.ciphertextDigest},
    {"plaintextBytes", object.plaintextBytes}, {"ciphertextBytes", object.ciphertextBytes},
    {"segmentBytes", indexed.segmentBytes}};
}

NativeStoredModelIndexedObject
storedObjectFromJson(const NativeJson& value, const NativeStoredModelReference& publication)
{
  require(value.is_object() && value.size() == 6 && value.at("name").is_string() &&
    value.at("plaintextDigest").is_string() && value.at("ciphertextDigest").is_string() &&
    value.at("plaintextBytes").is_number_unsigned() &&
    value.at("ciphertextBytes").is_number_unsigned() &&
    value.at("segmentBytes").is_number_unsigned());
  NativeStoredModelIndexedObject indexed;
  indexed.object.name = ndn::Name(value.at("name").get<std::string>());
  indexed.object.plaintextDigest = value.at("plaintextDigest").get<std::string>();
  indexed.object.ciphertextDigest = value.at("ciphertextDigest").get<std::string>();
  indexed.object.plaintextBytes = value.at("plaintextBytes").get<std::uint64_t>();
  indexed.object.ciphertextBytes = value.at("ciphertextBytes").get<std::uint64_t>();
  indexed.segmentBytes = value.at("segmentBytes").get<std::uint32_t>();
  require(!indexed.object.name.empty() && publication.producer.isPrefixOf(indexed.object.name) &&
    indexed.object.plaintextBytes > 0 && indexed.object.plaintextBytes <= (1ULL << 30) &&
    indexed.object.ciphertextBytes == indexed.object.plaintextBytes + 28 &&
    indexed.segmentBytes > 0 &&
    indexed.segmentBytes <= ndn::MAX_NDN_PACKET_SIZE &&
    indexed.object.ciphertextBytes <= (1ULL << 30) + 28);
  requireDigest(indexed.object.plaintextDigest);
  requireDigest(indexed.object.ciphertextDigest);
  require(storedObjectJson(indexed) == value);
  return indexed;
}

NativeJson
storedRetrievalJson(const ndn::Name& forwardingHint)
{
  require(!forwardingHint.empty());
  const auto uri = forwardingHint.toUri(ndn::name::UriFormat::CANONICAL);
  require(uri.size() <= 4096);
  return NativeJson{{"forwardingHint", uri}};
}

NativeJson
storedObjectKeyAssociatedData(const NativeJson& context,
                              const NativeStoredModelReference& publication,
                              const NativeStoredModelIndexedObject& object,
                              const ndn::Name& forwardingHint)
{
  return NativeJson{{"schema", "ndnsf-di-stored-model-object-key-aad-v1"},
    {"context", context}, {"publication", storedPublicationJson(publication)},
    {"object", storedObjectJson(object)}, {"retrieval", storedRetrievalJson(forwardingHint)}};
}

} // namespace

NativeStoredModelObjectKeyLease::Entry::Entry(NativeStoredModelObject objectValue,
  std::uint32_t segmentBytesValue, std::vector<std::uint8_t> keyValue)
  : object(std::move(objectValue))
  , segmentBytes(segmentBytesValue)
  , key(std::move(keyValue))
{
}

NativeStoredModelObjectKeyLease::Entry::~Entry() noexcept
{
  if (!key.empty())
    OPENSSL_cleanse(key.data(), key.size());
}

NativeStoredModelObjectKeyLease::NativeStoredModelObjectKeyLease(
  NativeStoredModelReference publication, ndn::Name forwardingHint)
  : m_publication(std::move(publication))
  , m_forwardingHint(std::move(forwardingHint))
{
  m_publication.validate();
  (void) storedRetrievalJson(m_forwardingHint);
  require(m_forwardingHint != m_publication.producer);
}

NativeStoredModelObjectKeyLease::~NativeStoredModelObjectKeyLease() noexcept
{
}

void
NativeStoredModelObjectKeyLease::add(NativeStoredModelIndexedObject indexed,
                                     std::vector<std::uint8_t> key)
{
  struct KeyWiper
  {
    std::vector<std::uint8_t>& key;
    ~KeyWiper()
    {
      if (!key.empty())
        OPENSSL_cleanse(key.data(), key.size());
    }
  } keyWiper{key};
  require(!m_finalized && m_entries.size() < 4096 && key.size() == STORED_OBJECT_KEY_BYTES &&
    m_publication.producer.isPrefixOf(indexed.object.name) && indexed.segmentBytes > 0 &&
    indexed.object.plaintextBytes > 0 && indexed.object.plaintextBytes <= (1ULL << 30) &&
    indexed.object.ciphertextBytes == indexed.object.plaintextBytes + 28);
  requireDigest(indexed.object.plaintextDigest);
  requireDigest(indexed.object.ciphertextDigest);
  const auto name = indexed.object.name.toUri(ndn::name::UriFormat::CANONICAL);
  const auto inserted = m_entries.emplace(name, Entry(std::move(indexed.object),
    indexed.segmentBytes, std::move(key)));
  require(inserted.second);
}

void
NativeStoredModelObjectKeyLease::finalize()
{
  require(!m_finalized && !m_entries.empty());
  NativeJson rows = NativeJson::array();
  std::vector<const Entry*> ordered;
  for (const auto& item : m_entries) {
    ordered.push_back(&item.second);
  }
  std::sort(ordered.begin(), ordered.end(), [] (const Entry* left, const Entry* right) {
    return storedNameWireLess(left->object.name, right->object.name);
  });
  for (const auto* entry : ordered) {
    auto row = storedObjectJson(NativeStoredModelIndexedObject{
      entry->object, entry->segmentBytes});
    row["keyDigest"] = nativePlanningDigest(entry->key.data(), entry->key.size());
    rows.push_back(std::move(row));
  }
  m_cacheIdentity = nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-stored-model-cache-identity-v1"},
    {"publication", storedPublicationJson(m_publication)}, {"objects", std::move(rows)}}));
  m_finalized = true;
}

const NativeStoredModelObjectKeyLease::Entry&
NativeStoredModelObjectKeyLease::find(const ndn::Name& name) const
{
  require(m_finalized && !m_cacheIdentity.empty() && !name.empty());
  const auto found = m_entries.find(name.toUri(ndn::name::UriFormat::CANONICAL));
  require(found != m_entries.end());
  return found->second;
}

const NativeStoredModelObject&
NativeStoredModelObjectKeyLease::object(const ndn::Name& name) const
{
  return find(name).object;
}

bool
NativeStoredModelObjectKeyLease::contains(const ndn::Name& name) const
{
  require(m_finalized && !m_cacheIdentity.empty());
  return m_entries.count(name.toUri(ndn::name::UriFormat::CANONICAL)) != 0;
}

std::uint32_t
NativeStoredModelObjectKeyLease::segmentBytes(const ndn::Name& name) const
{
  return find(name).segmentBytes;
}

std::vector<std::uint8_t>
NativeStoredModelObjectKeyLease::open(const ndn::Name& name,
                                     const std::vector<std::uint8_t>& ciphertext) const
{
  const auto& entry = find(name);
  return nativeOpenStoredModelObjectWithKey(m_publication, entry.object, entry.key, ciphertext);
}

std::set<std::string>
nativeSelectModelObjectKeyDataNames(const NativeModelObjectKeyIndexV1& index,
                                    const std::vector<std::uint64_t>& nodeIndices)
{
  require(index.providerAssemblyMetadataNames.size() == 3 && !index.templatePayloadIds.empty());
  std::set<std::string> classifiedNames = index.providerAssemblyMetadataNames;
  for (const auto& item : index.payloadIdsByDataName) {
    require(isCanonicalName(item.first) && !item.second.empty() &&
            classifiedNames.insert(item.first).second);
  }
  for (const auto& name : index.nonProviderObjectNames)
    require(isCanonicalName(name) && classifiedNames.insert(name).second);
  for (const auto& name : index.providerAssemblyMetadataNames)
    require(isCanonicalName(name));
  if (index.storedPublication) {
    index.storedPublication->validate();
    require(index.storedObjectsByName.size() > index.providerAssemblyMetadataNames.size());
    for (const auto& item : index.storedObjectsByName) {
      require(isCanonicalName(item.first) && item.first ==
        item.second.object.name.toUri(ndn::name::UriFormat::CANONICAL) &&
        index.storedPublication->producer.isPrefixOf(item.second.object.name));
    }
  }

  std::map<std::string, std::string> dataNameByPayloadId;
  for (const auto& item : index.payloadIdsByDataName) {
    for (const auto& payloadId : item.second) {
      require(!payloadId.empty() && dataNameByPayloadId.emplace(payloadId, item.first).second);
    }
  }

  std::set<std::string> selectedPayloadIds = index.templatePayloadIds;
  for (const auto& payloadId : selectedPayloadIds)
    require(!payloadId.empty() && dataNameByPayloadId.count(payloadId) == 1);
  std::set<std::uint64_t> uniqueNodes;
  for (const auto nodeIndex : nodeIndices) {
    require(uniqueNodes.insert(nodeIndex).second);
    const auto node = index.payloadIdsByNode.find(nodeIndex);
    require(node != index.payloadIdsByNode.end() && !node->second.empty());
    for (const auto& payloadId : node->second) {
      require(!payloadId.empty() && dataNameByPayloadId.count(payloadId) == 1);
      selectedPayloadIds.insert(payloadId);
    }
  }

  std::set<std::string> result = index.providerAssemblyMetadataNames;
  for (const auto& item : index.payloadIdsByDataName) {
    bool intersects = false;
    bool isSubset = true;
    for (const auto& payloadId : item.second) {
      const bool selected = selectedPayloadIds.count(payloadId) != 0;
      intersects = intersects || selected;
      isSubset = isSubset && selected;
    }
    if (intersects && !isSubset)
      throw std::invalid_argument("DI_MODEL_OBJECT_BUNDLE_CROSSES_PROVIDER_ASSIGNMENT");
    if (isSubset)
      result.insert(item.first);
  }
  if (index.storedPublication) {
    for (const auto& name : result)
      require(index.storedObjectsByName.count(name) == 1);
  }
  return result;
}

std::shared_ptr<const NativeModelObjectKeyIndexV1>
nativeBuildStoredModelObjectKeyIndex(const NativeStoredModelReference& publication,
  const NativeStoredModelCatalog& objects,
  const NativePreparedCanonicalPublication& preparedPublication)
{
  publication.validate();
  require(objects.publication().producer == publication.producer &&
    objects.publication().repository == publication.repository &&
    objects.publication().rootName == publication.rootName &&
    objects.publication().rootWireDigest == publication.rootWireDigest &&
    objects.objects().size() <= 4096 && !objects.catalogId().empty() &&
    objects.objects().count(objects.catalogId()) == 1 &&
    preparedPublication.materialManifest &&
    preparedPublication.materialPayloadIds.size() == preparedPublication.materialDataNames.size() &&
    preparedPublication.materialPayloadIds.size() == preparedPublication.materialDigests.size() &&
    !preparedPublication.materialPayloadIds.empty());

  auto index = std::make_shared<NativeModelObjectKeyIndexV1>();
  index->storedPublication = publication;
  for (const auto& item : objects.objects()) {
    const auto name = item.second.object.name.toUri(ndn::name::UriFormat::CANONICAL);
    require(publication.producer.isPrefixOf(item.second.object.name) &&
      index->storedObjectsByName.emplace(name, item.second).second);
  }
  const auto addMetadata = [&] (const std::string& name,
                                std::set<std::string>& destination) {
    require(isCanonicalName(name) && index->storedObjectsByName.count(name) == 1 &&
      destination.insert(name).second);
  };
  addMetadata(preparedPublication.rootDataName, index->providerAssemblyMetadataNames);
  addMetadata(preparedPublication.materialManifestDataName,
              index->providerAssemblyMetadataNames);
  addMetadata(preparedPublication.materialReceiptDataName,
              index->providerAssemblyMetadataNames);
  addMetadata(preparedPublication.preparedMetadataDataName, index->nonProviderObjectNames);
  addMetadata(objects.objects().at(objects.catalogId()).object.name.toUri(
                ndn::name::UriFormat::CANONICAL), index->nonProviderObjectNames);

  const auto& manifest = *preparedPublication.materialManifest;
  manifest.validate();
  index->templatePayloadIds.insert(manifest.templatePayloadId);
  std::map<std::string, const NativeCanonicalSource::MaterialReference*> initializerByName;
  for (const auto& reference : manifest.references) {
    if (reference.kind == "shared-initializer")
      require(initializerByName.emplace(reference.logicalName, &reference).second);
  }
  for (std::size_t i = 0; i < preparedPublication.materialPayloadIds.size(); ++i) {
    const auto& payloadId = preparedPublication.materialPayloadIds[i];
    const auto name = ndn::Name(preparedPublication.materialDataNames[i])
      .toUri(ndn::name::UriFormat::CANONICAL);
    require(!payloadId.empty() && isCanonicalName(name) &&
      index->storedObjectsByName.count(name) == 1 &&
      index->payloadIdsByDataName[name].insert(payloadId).second);
  }
  for (const auto& reference : manifest.references) {
    if (reference.kind != "graph-node")
      continue;
    auto& payloads = index->payloadIdsByNode[reference.nodeIndex];
    require(payloads.insert(reference.payloadId).second);
    for (const auto& dependency : reference.dependencies) {
      const auto initializer = initializerByName.find(dependency);
      require(initializer != initializerByName.end());
      payloads.insert(initializer->second->payloadId);
      for (const auto& chunk : initializer->second->chunkPayloadIds)
        payloads.insert(chunk);
    }
  }
  (void)nativeSelectModelObjectKeyDataNames(*index,
    [&] {
      std::vector<std::uint64_t> allNodes;
      for (const auto& item : index->payloadIdsByNode) allNodes.push_back(item.first);
      return allNodes;
    }());
  return index;
}

NativeJson
nativeModelObjectKeyContext(const NativeSelectionProjectionV3& projection,
  const ndn::Name& requester, const ndn::Name& service, const std::string& providerBootId,
  const ndn::Name& certificate, const std::string& publicKeyDigest,
  std::uint64_t expiresAtMs, std::uint64_t nowMs)
{
  require(projection.hasGrantBinding && !projection.grantName.empty() &&
    !requester.empty() && !service.empty() && !providerBootId.empty() &&
    projection.attempt > 0 && !ndn::Name(projection.requestId).empty() &&
    ndn::security::extractIdentityFromCertName(certificate) == ndn::Name(projection.provider) &&
    expiresAtMs > nowMs && expiresAtMs <= projection.deadlineMs &&
    (!projection.grantLeaseScope || expiresAtMs <= projection.grantLeaseScope->expiresAtMs));
  const auto& role = projection.selectedRoleContract();
  require(!role.protectionEpoch.empty() && role.protectionEpoch != "plaintext-v1");
  for (const auto& digest : {publicKeyDigest, projection.planCoreDigest, projection.planDigest,
       projection.ackClosedDigest, projection.offerDigest, projection.grantDigest,
       projection.securityPolicySnapshotDigest, role.modelManifestDigest, role.artifactDigest,
       role.graphDigest}) requireDigest(digest);
  return NativeJson{{"schema", "ndnsf-di-model-object-key-context-v1"},
    {"requester", requester.toUri(ndn::name::UriFormat::CANONICAL)},
    {"service", service.toUri(ndn::name::UriFormat::CANONICAL)},
    {"provider", ndn::Name(projection.provider).toUri(ndn::name::UriFormat::CANONICAL)},
    {"requestId", ndn::Name(projection.requestId).toUri(ndn::name::UriFormat::CANONICAL)},
    {"attempt", projection.attempt}, {"planCoreDigest", projection.planCoreDigest},
    {"planDigest", projection.planDigest}, {"ackClosedDigest", projection.ackClosedDigest},
    {"offerDigest", projection.offerDigest},
    {"policyDigest", projection.securityPolicySnapshotDigest},
    {"grantName", projection.grantName}, {"grantDigest", projection.grantDigest},
    {"grantLeaseScope", projection.grantLeaseScope ? projection.grantLeaseScope->scopeDigest : ""},
    {"providerBootId", providerBootId}, {"role", role.selectedRole}, {"rank", role.rank},
    {"modelManifestDigest", role.modelManifestDigest}, {"artifactDigest", role.artifactDigest},
    {"graphDigest", role.graphDigest}, {"protectionEpoch", role.protectionEpoch},
    {"recipientCertificate", certificate.toUri(ndn::name::UriFormat::CANONICAL)},
    {"recipientPublicKeyDigest", publicKeyDigest}, {"expiresAtMs", expiresAtMs}};
}

ndn_service_framework::LargeDataPublishResult
nativePublishModelObjectKeys(const std::shared_ptr<ndn_service_framework::ServiceUser>& user,
  const std::string& service, const NativeArtifactBinding& artifacts,
  const NativeSelectionProjectionV3& projection, const NativeFixedRoleAssignmentV1& fixed,
  const NativeRecipientKeyMaterialV1& recipient, const std::string& providerBootId,
  std::uint64_t expiresAtMs, const NativeRequestControl& control)
{
  control.requireActive();
  require(user && artifacts.ownerRecoverableObjects &&
    !artifacts.ownerRecoverableObjects->empty() &&
    artifacts.ownerRecoverableObjects->size() <= 4096);
  const auto context = nativeModelObjectKeyContext(projection, user->getName(), ndn::Name(service),
    providerBootId, recipient.certificateName, recipient.publicKeyDigest, expiresAtMs, epochMs());
  const auto binding = contextBytes(context);
  NativeJson rows = NativeJson::array();
  std::size_t rowBytes = 0;
  NativeWorkScope rewrap(control.workStatistics, "model-object-key-rewrap",
                         NativeWorkKind::UNCLASSIFIED);
  std::set<ndn::Name> names;
  for (const auto& object : *artifacts.ownerRecoverableObjects) {
    control.requireActive();
    require(names.insert(object.dataPrefix).second);
    const auto capsule = user->rewrapOwnerRetainedLargeDataKeyFromWorker(
      ndn::Name(service), object, ndn::Name(projection.provider), recipient.certificateName,
      ndn::Buffer(recipient.publicKey.begin(), recipient.publicKey.end()), binding,
      control.deadline, [control] { control.requireActive(); });
    NativeJson row{{"reference", nativeProtectedObjectReference(object)},
                   {"capsule", capsule.getFields()}};
    const auto bytes = nativeCanonicalJson(row).size();
    require(bytes <= MAX_TABLE_BYTES - std::min<std::uint64_t>(rowBytes, MAX_TABLE_BYTES));
    rowBytes += bytes;
    rows.push_back(std::move(row));
  }
  rewrap.finish();
  const auto wire = nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-model-object-keys-v1"}, {"context", context},
    {"objects", std::move(rows)}});
  require(wire.size() <= MAX_TABLE_BYTES);
  ndn_service_framework::PreparedServiceRequest request;
  request.serviceName = ndn::Name(service);
  request.requestId = ndn::Name(projection.requestId);
  ndn_service_framework::LargeDataPublishOptions options;
  options.contentName = nativeFixedRoleSourceContentName(projection, fixed);
  NativeWorkScope publish(control.workStatistics, "model-object-key-publication",
                           NativeWorkKind::UNCLASSIFIED);
  auto result = user->publishEncryptedLargeDataFromWorker(request,
    std::vector<std::uint8_t>(wire.begin(), wire.end()), "di-model-object-keys",
    ndn::DEFAULT_FRESHNESS_PERIOD, options, [control] { control.requireActive(); });
  require(result.success && result.servingLease && result.plaintextSize == wire.size() &&
    result.contentDigest == nativePlanningDigest(wire));
  if (control.workStatistics) {
    control.workStatistics->addCount("modelObjectKeys.publishedObjects", 1);
    control.workStatistics->addCount("modelObjectKeys.publishedPlaintextBytes", wire.size());
    control.workStatistics->addCount("modelObjectKeys.capsules", names.size());
  }
  control.requireActive();
  return result;
}

ndn_service_framework::LargeDataPublishResult
nativePublishStoredModelObjectKeys(
  const std::shared_ptr<ndn_service_framework::ServiceUser>& user,
  const std::string& service, const NativeArtifactBinding& artifacts,
  const NativeStoredModelHandle& handle, const ndn::Name& forwardingHint,
  const NativeSelectionProjectionV3& projection,
  const NativeFixedRoleAssignmentV1& fixed, const NativeRecipientKeyMaterialV1& recipient,
  const std::string& providerBootId, std::uint64_t expiresAtMs,
  const NativeRequestControl& control)
{
  control.requireActive();
  require(user && artifacts.modelObjectKeyIndex &&
    artifacts.modelObjectKeyIndex->storedPublication && fixed.roleOwner() &&
    !service.empty() && fixed.provider() == projection.provider);
  handle.validate();
  const auto retrieval = storedRetrievalJson(forwardingHint);
  require(forwardingHint != handle.producer);
  const auto& publication = *artifacts.modelObjectKeyIndex->storedPublication;
  require(handle.producer == publication.producer && handle.repository == publication.repository &&
    handle.rootName == publication.rootName && handle.rootWireDigest == publication.rootWireDigest);
  const auto& role = *fixed.roleOwner();
  require(role.selectedRole == projection.selectedRoleContract().selectedRole &&
    role.nodeIndices == projection.selectedRoleContract().nodeIndices);
  const auto selectedNames = nativeSelectModelObjectKeyDataNames(
    *artifacts.modelObjectKeyIndex, role.nodeIndices);
  require(!selectedNames.empty() && selectedNames.size() <= 4096);

  const auto context = nativeModelObjectKeyContext(projection, user->getName(), ndn::Name(service),
    providerBootId, recipient.certificateName, recipient.publicKeyDigest, expiresAtMs, epochMs());
  NativeJson rows = NativeJson::array();
  std::size_t rowBytes = 0;
  NativeWorkScope capsuleWork(control.workStatistics, "stored-model-object-key-capsules",
                              NativeWorkKind::LOCAL);
  std::vector<ndn::Name> orderedNames;
  for (const auto& name : selectedNames)
    orderedNames.emplace_back(name);
  std::sort(orderedNames.begin(), orderedNames.end(), storedNameWireLess);
  for (const auto& name : orderedNames) {
    control.requireActive();
    const auto found = artifacts.modelObjectKeyIndex->storedObjectsByName.find(
      name.toUri(ndn::name::UriFormat::CANONICAL));
    require(found != artifacts.modelObjectKeyIndex->storedObjectsByName.end());
    auto key = nativeStoredModelObjectKey(handle.masterKey, publication.producer,
                                           found->second.object.name);
    struct KeyWiper
    {
      std::vector<std::uint8_t>& key;
      ~KeyWiper()
      {
        if (!key.empty())
          OPENSSL_cleanse(key.data(), key.size());
      }
    } keyWiper{key};
    const auto associatedData = contextBytes(
      storedObjectKeyAssociatedData(context, publication, found->second, forwardingHint));
    const auto capsule = ndn_service_framework::encryptRecipientAssignment(
      ndn::span<const std::uint8_t>(key.data(), key.size()),
      ndn::span<const std::uint8_t>(recipient.publicKey.data(), recipient.publicKey.size()),
      ndn::Name(projection.provider), recipient.certificateName,
      ndn::span<const std::uint8_t>(associatedData.data(), associatedData.size()));
    NativeJson row{{"object", storedObjectJson(found->second)}, {"capsule", capsule.getFields()}};
    const auto bytes = nativeCanonicalJson(row).size();
    require(bytes <= MAX_TABLE_BYTES - std::min<std::uint64_t>(rowBytes, MAX_TABLE_BYTES));
    rowBytes += bytes;
    rows.push_back(std::move(row));
  }
  capsuleWork.finish();
  const auto wire = nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-stored-model-object-keys-v1"},
    {"context", context}, {"publication", storedPublicationJson(publication)},
    {"objects", std::move(rows)}, {"retrieval", retrieval}});
  if (wire.size() > MAX_TABLE_BYTES)
    throw std::invalid_argument("DI_MODEL_OBJECT_KEYS_TABLE_TOO_LARGE");

  ndn_service_framework::PreparedServiceRequest request;
  request.serviceName = ndn::Name(service);
  request.requestId = ndn::Name(projection.requestId);
  ndn_service_framework::LargeDataPublishOptions options;
  options.contentName = nativeFixedRoleSourceContentName(projection, fixed);
  NativeWorkScope publish(control.workStatistics, "stored-model-object-key-table-publication",
                          NativeWorkKind::UNCLASSIFIED);
  auto result = user->publishEncryptedLargeDataFromWorker(request,
    std::vector<std::uint8_t>(wire.begin(), wire.end()), "di-stored-model-object-keys",
    ndn::DEFAULT_FRESHNESS_PERIOD, options, [control] { control.requireActive(); });
  require(result.success && result.servingLease && result.plaintextSize == wire.size() &&
    result.contentDigest == nativePlanningDigest(wire));
  if (control.workStatistics) {
    control.workStatistics->addCount("storedModelObjectKeys.publishedObjects", 1);
    control.workStatistics->addCount("storedModelObjectKeys.publishedPlaintextBytes", wire.size());
    control.workStatistics->addCount("storedModelObjectKeys.capsules", selectedNames.size());
  }
  control.requireActive();
  return result;
}

std::shared_ptr<const NativeStoredModelObjectKeyLease>
nativeInstallModelObjectKeys(ndn_service_framework::ServiceProvider::CollaborationContext& context,
  const NativeSelectionProjectionV3& projection, const std::string& providerBootId,
  std::chrono::steady_clock::time_point deadline,
  const std::shared_ptr<NativeWorkStatistics>& workStatistics)
{
  if (!projection.modelObjectKeys) return {};
  const auto& reference = *projection.modelObjectKeys;
  const auto referenceSchema = reference.is_object()
    ? reference.value("schema", std::string{}) : std::string{};
  const bool storedReference =
    referenceSchema == "ndnsf-di-stored-model-object-keys-reference-v1";
  require(reference.is_object() && reference.size() == 5 &&
    nativeCanonicalJson(reference).size() <= 16384 &&
    (storedReference || referenceSchema == "ndnsf-di-model-object-keys-reference-v1") &&
    reference.at("plaintextBytes").is_number_unsigned());
  const auto& scope = reference.at("context");
  require(scope.at("expiresAtMs").is_number_unsigned());
  const auto expires = scope.at("expiresAtMs").get<std::uint64_t>();
  const auto certificate = ndn::Name(scope.at("recipientCertificate").get<std::string>());
  const auto keyDigest = scope.at("recipientPublicKeyDigest").get<std::string>();
  const auto expected = nativeModelObjectKeyContext(projection, context.requesterName(),
    context.assignment().service, providerBootId, certificate, keyDigest, expires, epochMs());
  require(scope == expected);
  const auto name = reference.at("dataName").get<std::string>();
  const auto digest = reference.at("plaintextDigest").get<std::string>();
  const auto bytes = reference.at("plaintextBytes").get<std::uint64_t>();
  require(bytes > 0 && bytes <= MAX_TABLE_BYTES);
  requireDigest(digest);
  nativeValidateFixedRoleSourceProducer(NativeFixedRoleSourceV1{name, digest, bytes, {}},
    context.requesterName().toUri(ndn::name::UriFormat::CANONICAL),
    projection.selectedRoleContract().selectedRole);
  const auto now = epochMs();
  const auto steadyNow = std::chrono::steady_clock::now();
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::min(deadline, context.assignment().workDeadline) - steadyNow).count();
  require(expires > now && remaining > 0);
  deadline = steadyNow + std::chrono::milliseconds(
    std::min(expires - now, static_cast<std::uint64_t>(remaining)));
  NativeWorkScope fetch(workStatistics, "model-object-key-table-fetch", NativeWorkKind::WAIT);
  // The admitted Assignment content name is hash-bound, not a typed-version
  // static-fact name. Use the same current-authority reader as fixed roles.
  const auto payload = context.fetchEncryptedLargeDataCurrent(ndn::Name(name), deadline);
  fetch.finish();
  require(payload && payload->size() == bytes &&
          nativePlanningDigest(payload->data(), payload->size()) == digest);
  const auto table = nativeParseJson(std::string(payload->begin(), payload->end()));
  require(table.is_object() && table.at("context") == expected && table.at("objects").is_array() &&
    !table.at("objects").empty() && table.at("objects").size() <= 4096);
  const auto schema = table.value("schema", std::string{});
  if (storedReference && schema == "ndnsf-di-stored-model-object-keys-v1") {
    require(table.size() == 5 && table.contains("retrieval") &&
      table.at("retrieval").is_object() && table.at("retrieval").size() == 1 &&
      table.at("retrieval").contains("forwardingHint") &&
      table.at("retrieval").at("forwardingHint").is_string());
    const auto& hintText = table.at("retrieval").at("forwardingHint")
      .get_ref<const std::string&>();
    require(!hintText.empty() && hintText.size() <= 4096 && hintText.front() == '/');
    const ndn::Name forwardingHint(hintText);
    require(table.at("retrieval") == storedRetrievalJson(forwardingHint));
    auto publication = storedPublicationFromJson(table.at("publication"));
    auto lease = std::make_shared<NativeStoredModelObjectKeyLease>(publication, forwardingHint);
    std::set<ndn::Name> names;
    std::optional<ndn::Name> previousName;
    NativeWorkScope import(workStatistics, "stored-model-object-key-import",
                           NativeWorkKind::UNCLASSIFIED);
    for (const auto& row : table.at("objects")) {
      require(epochMs() < expires && row.is_object() && row.size() == 2 &&
        row.at("object").is_object() && row.at("capsule").is_object() &&
        row.at("capsule").size() == 10);
      const auto indexed = storedObjectFromJson(row.at("object"), publication);
      require(!previousName || storedNameWireLess(*previousName, indexed.object.name));
      previousName = indexed.object.name;
      require(names.insert(indexed.object.name).second);
      ndn_service_framework::RecipientEncryptedAssignment capsule;
      for (const auto& field : row.at("capsule").items()) {
        require(field.value().is_string());
        const auto& value = field.value().get_ref<const std::string&>();
        require(field.key().size() <= 64 && value.size() <= 4096);
        capsule.setField(field.key(), value);
      }
      const auto associatedData = contextBytes(
        storedObjectKeyAssociatedData(expected, publication, indexed, forwardingHint));
      std::vector<std::uint8_t> objectKey;
      struct KeyWiper
      {
        std::vector<std::uint8_t>& key;
        ~KeyWiper()
        {
          if (!key.empty())
            OPENSSL_cleanse(key.data(), key.size());
        }
      } keyWiper{objectKey};
      const bool admitted = context.consumeRecipientAssignmentCurrent(capsule, certificate,
        keyDigest, associatedData, deadline, [&](const ndn::Buffer& plaintext) {
          require(plaintext.size() == STORED_OBJECT_KEY_BYTES);
          objectKey.assign(plaintext.begin(), plaintext.end());
        });
      require(admitted && objectKey.size() == STORED_OBJECT_KEY_BYTES);
      lease->add(indexed, std::move(objectKey));
    }
    lease->finalize();
    if (workStatistics) {
      workStatistics->addCount("storedModelObjectKeys.importedKeys", names.size());
      workStatistics->addCount("storedModelObjectKeys.fetchedPlaintextBytes", bytes);
      workStatistics->addCount("storedModelObjectKeys.fetchedObjects", 1);
    }
    import.finish();
    return lease;
  }

  require(!storedReference && table.size() == 3 &&
    schema == "ndnsf-di-model-object-keys-v1");
  const auto binding = contextBytes(expected);
  std::set<ndn::Name> names;
  NativeWorkScope import(workStatistics, "model-object-key-import",
                          NativeWorkKind::UNCLASSIFIED);
  for (const auto& row : table.at("objects")) {
    require(epochMs() < expires && row.is_object() && row.size() == 2 &&
      row.at("capsule").is_object() && row.at("capsule").size() == 10);
    const auto object = nativeProtectedObjectFromReference(row.at("reference"));
    require(names.insert(object.dataPrefix).second);
    ndn_service_framework::RecipientEncryptedAssignment capsule;
    for (const auto& field : row.at("capsule").items()) {
      require(field.value().is_string());
      const auto& value = field.value().get_ref<const std::string&>();
      require(field.key().size() <= 64 && value.size() <= 4096);
      capsule.setField(field.key(), value);
    }
    require(context.installProtectedObjectKeyCurrent(object, capsule, certificate, keyDigest,
                                                       binding, deadline));
  }
  if (workStatistics) {
    workStatistics->addCount("modelObjectKeys.importedKeys", names.size());
    workStatistics->addCount("modelObjectKeys.fetchedPlaintextBytes", bytes);
    workStatistics->addCount("modelObjectKeys.fetchedObjects", 1);
  }
  import.finish();
  return {};
}

std::optional<ndn::Buffer>
nativeFetchAndOpenStoredModelObjectCurrent(
  ndn_service_framework::ServiceProvider::CollaborationContext& context,
  const NativeStoredModelObjectKeyLease& lease, const ndn::Name& name,
  std::chrono::steady_clock::time_point deadline)
{
  require(lease.contains(name));
  const auto& object = lease.object(name);
  const auto segmentBytes = lease.segmentBytes(name);
  require(segmentBytes > 0 && object.ciphertextBytes > 0 &&
    object.ciphertextBytes <= (1ULL << 30) + 28);
  const auto expectedSegments = (object.ciphertextBytes + segmentBytes - 1) / segmentBytes;
  require(expectedSegments > 0 && expectedSegments <= (1U << 18));
  const auto ciphertext = context.fetchDigestLargeDataCurrent(name, lease.publication().producer,
    static_cast<std::size_t>(expectedSegments), segmentBytes, object.ciphertextBytes, deadline,
    lease.forwardingHint());
  if (!ciphertext || ciphertext->size() != object.ciphertextBytes ||
      nativePlanningDigest(ciphertext->data(), ciphertext->size()) != object.ciphertextDigest)
    return std::nullopt;
  const auto plaintext = lease.open(name,
    std::vector<std::uint8_t>(ciphertext->begin(), ciphertext->end()));
  return ndn::Buffer(plaintext.begin(), plaintext.end());
}

} // namespace ndnsf::di
