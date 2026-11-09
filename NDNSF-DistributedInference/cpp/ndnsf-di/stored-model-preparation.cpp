#include "stored-model-preparation.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace ndnsf::di {
namespace {

using Manifest = NativeCanonicalSource::MaterialManifest;
using Reference = NativeCanonicalSource::MaterialReference;
using Audience = std::set<std::string>;
constexpr std::size_t MAX_OBJECTS = 4096;
constexpr std::size_t METADATA_OBJECTS = 5;
constexpr std::size_t MAX_CATALOG_BYTES = 4U << 20;

void
require(bool value)
{
  if (!value) throw std::invalid_argument("DI_STORED_PREPARATION_INVALID");
}

std::vector<std::uint8_t>
bytes(const std::string& text)
{
  return {text.begin(), text.end()};
}

struct Bundle
{
  std::string id;
  std::vector<std::size_t> payloads;
  std::uint64_t size = 0;
};

std::map<std::string, Audience>
payloadAudiences(const Manifest& manifest, const NativeCanonicalRolePreparer::NodeMap& mapping,
                 const std::function<void()>& current)
{
  manifest.validate(current);
  require(!mapping.empty());
  std::map<std::uint64_t, std::string> unitByNode;
  for (const auto& item : mapping) {
    require(!item.first.empty() && !item.second.empty());
    for (const auto node : item.second)
      require(unitByNode.emplace(node, item.first).second);
  }
  std::map<std::string, const Reference*> initializers;
  std::map<std::string, Audience> audience;
  std::size_t templates = 0;
  for (const auto& reference : manifest.references) {
    if (reference.kind == "shared-initializer")
      require(initializers.emplace(reference.logicalName, &reference).second);
    if (reference.kind == "graph-template") {
      ++templates;
      audience.emplace(reference.payloadId, Audience{});
    }
  }
  std::set<std::uint64_t> nodes;
  for (const auto& reference : manifest.references) {
    current();
    if (reference.kind != "graph-node") continue;
    require(nodes.insert(reference.nodeIndex).second && unitByNode.count(reference.nodeIndex));
    const auto& unit = unitByNode.at(reference.nodeIndex);
    audience[reference.payloadId].insert(unit);
    for (const auto& dependency : reference.dependencies) {
      require(initializers.count(dependency));
      const auto& initializer = *initializers.at(dependency);
      audience[initializer.payloadId].insert(unit);
      for (const auto& chunk : initializer.chunkPayloadIds) audience[chunk].insert(unit);
    }
  }
  require(templates == 1 && nodes.size() == unitByNode.size());
  return audience;
}

std::vector<Bundle>
planBundles(const Manifest& manifest, const NativeCanonicalRolePreparer::NodeMap& mapping,
            const std::function<void()>& current)
{
  const auto audience = payloadAudiences(manifest, mapping, current);
  require(manifest.payloadsComplete);
  // Template bytes are needed by every role and must not share an encryption key with weights.
  std::map<Audience, std::vector<std::size_t>> groups;
  for (std::size_t i = 0; i < manifest.payloads.size(); ++i) {
    current();
    const auto& payload = manifest.payloads[i];
    require(!payload.empty() && payload.byteSize() <= NativeCanonicalMaterialBundleMaxBytes);
    if (payload.payloadId == manifest.templatePayloadId)
      groups[Audience{}].push_back(i);
    else {
      require(audience.count(payload.payloadId) && !audience.at(payload.payloadId).empty());
      groups[audience.at(payload.payloadId)].push_back(i);
    }
  }
  std::vector<Bundle> result;
  for (const auto& group : groups) {
    for (const auto i : group.second) {
      const auto size = manifest.payloads[i].byteSize();
      if (result.empty() || i == group.second.front() ||
          size > NativeCanonicalMaterialPublicationMaxBytes - result.back().size) {
        require(result.size() < MAX_OBJECTS - METADATA_OBJECTS);
        result.push_back({"material-bundle-" + std::to_string(result.size()), {}, 0});
      }
      result.back().payloads.push_back(i);
      result.back().size += size;
    }
  }
  return result;
}

std::string
sourceFreeConfiguration(const std::string& configuration)
{
  require(configuration.size() <= MAX_CATALOG_BYTES);
  auto value = nativeParseJson(configuration);
  require(value.is_object() && value.at("schema") == "ndnsf-di-native-request-catalog-v1" &&
          (value.at("splitter").at("kind") == "QWEN" ||
           value.at("splitter").at("kind") == "LLAMA"));
  const auto& original = value.at("source");
  NativeJson source = NativeJson::object();
  for (const auto* key : {"data_name", "digest", "model_manifest_digest",
                          "canonical_graph_digest", "initializer_digest"})
    if (original.contains(key)) source[key] = original.at(key);
  value["source"] = std::move(source);
  // Per-request transfer estimates are not immutable model preparation facts.
  value.erase("request_transfer_profiles");
  return nativeCanonicalJson(value);
}

} // namespace

std::string
nativeStoredModelConfigurationIdentity(const std::string& configurationJson)
{
  return sourceFreeConfiguration(configurationJson);
}

NativeStoredModelPublication
nativePublishStoredPreparedModel(ndn::KeyChain& keyChain, const ndn::Name& producer,
  const ndn::Name& repository, const ndn::Name& publicationPrefix,
  const NativeRequestCatalog& catalog, const std::string& configurationJson,
  const NativeStoredModelPacketIo& io, const std::filesystem::path& preparationState)
{
  return nativePublishStoredPreparedModel(keyChain, producer, repository, publicationPrefix,
    [&catalog] { return catalog; }, configurationJson, io, preparationState);
}

NativeStoredModelPublication
nativePublishStoredPreparedModel(ndn::KeyChain& keyChain, const ndn::Name& producer,
  const ndn::Name& repository, const ndn::Name& publicationPrefix,
  const std::function<NativeRequestCatalog()>& loadCatalog, const std::string& configurationJson,
  const NativeStoredModelPacketIo& io, const std::filesystem::path& preparationState)
{
  require(loadCatalog && io.requireCurrent && configurationJson.size() <= MAX_CATALOG_BYTES);
  io.requireCurrent();
  const auto configuration = sourceFreeConfiguration(configurationJson);
  const auto catalogId = "catalog-" + nativePlanningDigest(configuration).substr(7);
  const auto config = nativeParseJson(configuration);
  const auto model = NativeModelDescriptor::fromCanonicalJson(nativeCanonicalJson(config.at("model")));
  NativeRequestCatalog catalog;
  std::shared_ptr<const NativeCanonicalSource> source;
  NativeCanonicalPublicationOptions options;
  std::vector<Bundle> bundles;
  std::string materialJson;
  ndn::Name prefix;
  const auto name = [&](const std::string& id) {
    return nativeStoredModelObjectName(prefix, id).toUri(
      ndn::name::UriFormat::CANONICAL);
  };
  const auto makeMetadata = [&](NativeJson objectRows) {
    const auto& manifest = *source->materialManifest;
    const auto receipt = nativeCanonicalJson(NativeJson{
      {"schema", "ndnsf-di-canonical-material-receipt-v1"},
      {"graphDigest", manifest.graphDigest}, {"sourceDigest", manifest.sourceDigest},
      {"materialIdentityDigest", manifest.manifestDigest},
      {"materialManifestDigest", nativePlanningDigest(materialJson)},
      {"materialObjects", std::move(objectRows)}});
    const auto& model = catalog.model;
    NativeJson facts{{"canonicalSourceBytes", model.canonicalSourceBytes},
      {"canonicalSourceDigest", model.canonicalSourceDigest}, {"materialBacked", true},
      {"materialManifestDataName", name("material-manifest")},
      {"materialManifestDigest", nativePlanningDigest(materialJson)},
      {"materialManifestBytes", materialJson.size()},
      {"materialIdentityDigest", manifest.manifestDigest},
      {"materialReceiptDataName", name("material-receipt")},
      {"materialReceiptDigest", nativePlanningDigest(receipt)},
      {"materialReceiptBytes", receipt.size()},
      {"preparedMetadataDataName", name("prepared-metadata")},
      {"preparedMetadataDigest", nativePlanningDigest(source->preparedMetadataJson)},
      {"preparedMetadataBytes", source->preparedMetadataJson.size()}};
    if (model.canonicalInitializerBytes) {
      facts["canonicalInitializerBytes"] = model.canonicalInitializerBytes;
      facts["canonicalInitializerObjectDigest"] = model.canonicalInitializerObjectDigest;
    }
    if (!options.packageManifestDigest.empty())
      facts["packageManifestDigest"] = options.packageManifestDigest;
    NativeJson rootValue{
      {"schema", "ndnsf-di-canonical-model-manifest-v1"}, {"state", "ACTIVE"},
      {"artifactProfileDigest", options.artifactProfileDigest},
      {"modelIdentityDigest", model.descriptor.contentDigest},
      {"modelSemanticIdentityDigest", model.descriptor.modelDigest()},
      {"modelName", model.descriptor.modelName}, {"metadata", std::move(facts)}};
    if (!options.layerManifestDigests.empty())
      rootValue["layerManifestDigests"] = options.layerManifestDigests;
    const auto root = nativeCanonicalJson(rootValue);
    require(root.size() <= 4096 && receipt.size() <= NativeCanonicalMaterialPublicationMaxBytes);
    return std::vector<NativeStoredModelInput>{
      {"material-manifest", bytes(materialJson)}, {"material-receipt", bytes(receipt)},
      {"prepared-metadata", bytes(source->preparedMetadataJson)},
      {"business-root", bytes(root)}, {catalogId, bytes(configuration)}};
  };
  const auto bindPrefix = [&](const ndn::Name& resolved) {
    prefix = resolved;
    catalog = loadCatalog();
    require(catalog.preparation != nullptr);
    source = catalog.preparation->sourceRefFor(catalog.model.descriptor);
    require(source && source->materialManifest && !source->preparedMetadataJson.empty());
    options = catalog.preparation->publicationFor(catalog.model.descriptor);
    require(nativeCanonicalPublicationIdentity(nativeParseJson(configurationJson)) ==
            options.publicationIdentityDigest);
    const auto& manifest = *source->materialManifest;
    require(config.at("model") == nativeParseJson(catalog.model.descriptor.canonicalJson()) &&
            config.at("source").at("digest") == catalog.model.canonicalSourceDigest &&
            config.at("source").at("canonical_graph_digest") == catalog.model.canonicalGraphDigest &&
            config.at("source").at("model_manifest_digest") == catalog.model.modelManifestDigest &&
            config.at("source").at("data_name") == catalog.model.canonicalSourceName);
    const auto mapping = config.at("node_mapping").get<NativeCanonicalRolePreparer::NodeMap>();
    require(mapping.size() == catalog.model.graph.nodes.size());
    for (const auto& node : catalog.model.graph.nodes) require(mapping.count(node.id));
    bundles = planBundles(manifest, mapping, io.requireCurrent);
    require(!bundles.empty());
    materialJson = manifest.canonicalJson();
    require(materialJson.size() <= NativeCanonicalMaterialPublicationMaxBytes &&
            source->preparedMetadataJson.size() <= NativeCanonicalMaterialPublicationMaxBytes);
    NativeJson plannedRows = NativeJson::array();
    const auto placeholderDigest = nativePlanningDigest("stored-bundle-preflight");
    for (const auto& bundle : bundles) {
      require(name(bundle.id).size() <= NativeCanonicalMaterialReceiptDataNameMaxBytes);
      std::uint64_t offset = 0;
      for (const auto index : bundle.payloads) {
        const auto& payload = manifest.payloads[index];
        plannedRows.push_back(NativeJson{{"payloadId", payload.payloadId},
          {"dataName", name(bundle.id)}, {"digest", payload.digest}, {"bytes", payload.byteSize()},
          {"bundleBytes", bundle.size}, {"bundleOffset", offset},
          {"bundleDigest", placeholderDigest}});
        offset += payload.byteSize();
      }
    }
    // Digest values have fixed width. Reserve actual encoded rows, not 4KiB per 1MiB chunk:
    // that historical conservative allowance alone would reject a valid 27B material set.
    require(nativeCanonicalJson(plannedRows).size() <=
      NativeCanonicalMaterialPublicationMaxBytes - NativeCanonicalMaterialReceiptEnvelopeMaxBytes);

    {
      const auto plannedMetadata = makeMetadata(std::move(plannedRows));
      std::uint64_t totalBytes = 0;
      std::vector<std::pair<std::string, std::uint64_t>> plan;
      for (const auto& bundle : bundles) {
        totalBytes += bundle.size;
        plan.emplace_back(bundle.id, bundle.size);
      }
      for (const auto& item : plannedMetadata) {
        totalBytes += item.plaintext.size();
        plan.emplace_back(item.id, item.plaintext.size());
      }
      require(options.maxPublicationBytes == 0 || totalBytes <= options.maxPublicationBytes);
      (void)nativePreflightStoredModelPublication(keyChain, producer, prefix,
        catalog.model.descriptor.modelDigest(), catalogId, "business-root", plan);
    }
  };
  NativeJson rows = NativeJson::array();
  std::vector<NativeStoredModelInput> metadata;
  std::size_t nextBundle = 0;
  std::size_t nextMetadata = 0;
  const auto next = [&]() -> std::optional<NativeStoredModelInput> {
    io.requireCurrent();
    const auto& manifest = *source->materialManifest;
    if (nextBundle < bundles.size()) {
      const auto& bundle = bundles[nextBundle++];
      NativeStoredModelInput input{bundle.id, {}};
      input.plaintext.reserve(bundle.size);
      const auto rowBegin = rows.size();
      for (const auto index : bundle.payloads) {
        const auto& payload = manifest.payloads[index];
        const auto value = payload.copyBytes(io.requireCurrent);
        require(value.size() == payload.byteSize() &&
                nativePlanningDigest(value.data(), value.size()) == payload.digest);
        rows.push_back(NativeJson{{"payloadId", payload.payloadId}, {"dataName", name(bundle.id)},
          {"digest", payload.digest}, {"bytes", value.size()},
          {"bundleBytes", bundle.size}, {"bundleOffset", input.plaintext.size()}});
        input.plaintext.insert(input.plaintext.end(), value.begin(), value.end());
      }
      const auto digest = nativePlanningDigest(input.plaintext.data(), input.plaintext.size());
      for (std::size_t i = rowBegin; i < rows.size(); ++i) rows[i]["bundleDigest"] = digest;
      return input;
    }
    if (metadata.empty()) metadata = makeMetadata(std::move(rows));
    if (nextMetadata == metadata.size()) return std::nullopt;
    return std::move(metadata[nextMetadata++]);
  };
  return nativePublishStoredModel(keyChain, producer, repository, publicationPrefix,
    model.modelDigest(), catalogId, "business-root", next, io, preparationState, bindPrefix);
}

NativeStoredPreparedModel
nativeOpenStoredPreparedModel(const NativeStoredModelHandle& handle,
  const NativeStoredModelPacketIo& io, const NativeAssemblyControl& control)
{
  control.requireActive();
  auto objects = nativeLoadStoredModelCatalog(handle, io);
  const auto& catalogId = objects.catalogId();
  require(catalogId.size() == 72 && catalogId.compare(0, 8, "catalog-") == 0 &&
          objects.manifestId() == "business-root");
  require(objects.objects().size() > METADATA_OBJECTS);
  for (const auto& item : objects.objects()) {
    const auto& id = item.first;
    const auto limit = id == catalogId ? MAX_CATALOG_BYTES :
      (id == "business-root" ? 4096 : NativeCanonicalMaterialPublicationMaxBytes);
    require(item.second.object.plaintextBytes <= limit &&
      (id == catalogId || id == "business-root" || id == "prepared-metadata" ||
       id == "material-manifest" || id == "material-receipt" ||
       id.compare(0, 16, "material-bundle-") == 0));
  }
  const auto read = [&](const std::string& id) {
    control.requireActive();
    const auto value = nativeReadStoredModelObject(handle, objects, id, io);
    control.requireActive();
    return std::string(value.begin(), value.end());
  };
  const auto configuration = read(catalogId);
  require(configuration == sourceFreeConfiguration(configuration) &&
          catalogId == "catalog-" + nativePlanningDigest(configuration).substr(7));
  const auto config = nativeParseJson(configuration);
  NativePreparedCanonicalPublication publication;
  publication.canonicalManifestJson = read("business-root");
  publication.manifestDigest = nativePlanningDigest(publication.canonicalManifestJson);
  const auto root = nativeParseJson(publication.canonicalManifestJson);
  require(root.at("schema") == "ndnsf-di-canonical-model-manifest-v1" &&
          root.at("state") == "ACTIVE");
  const auto& facts = root.at("metadata");
  const auto objectText = [&](const std::string& id, const char* prefix) {
    const auto& object = objects.objects().at(id).object;
    require(facts.at(std::string(prefix) + "DataName") ==
              object.name.toUri(ndn::name::UriFormat::CANONICAL) &&
            facts.at(std::string(prefix) + "Digest") == object.plaintextDigest &&
            facts.at(std::string(prefix) + "Bytes") == object.plaintextBytes);
    return read(id);
  };
  const auto materialJson = objectText("material-manifest", "materialManifest");
  const auto receiptJson = objectText("material-receipt", "materialReceipt");
  publication.preparedMetadataJson = objectText("prepared-metadata", "preparedMetadata");
  NativeCanonicalSource source;
  source.preparedMetadataJson = publication.preparedMetadataJson;
  source.materialManifest = parseNativeCanonicalMaterialManifest(bytes(materialJson));
  const auto receipt = nativeParseJson(receiptJson);
  require(receipt.at("schema") == "ndnsf-di-canonical-material-receipt-v1" &&
          receipt.at("sourceDigest") == source.materialManifest->sourceDigest &&
          receipt.at("graphDigest") == source.materialManifest->graphDigest &&
          receipt.at("materialIdentityDigest") == source.materialManifest->manifestDigest &&
          receipt.at("materialManifestDigest") == nativePlanningDigest(materialJson));
  require(receipt.at("materialObjects").is_array());
  std::map<std::string, const NativeJson*> rows;
  for (const auto& row : receipt.at("materialObjects"))
    require(rows.emplace(row.at("payloadId").get<std::string>(), &row).second);
  std::set<std::string> requiredIds;
  for (const auto& reference : source.materialManifest->references) {
    auto ids = reference.chunkPayloadIds;
    ids.push_back(reference.payloadId);
    for (const auto& id : ids) {
      require(rows.count(id));
      requiredIds.insert(id);
    }
    require(rows.at(reference.payloadId)->at("digest") == reference.digest &&
            rows.at(reference.payloadId)->at("bytes") == reference.bytes);
  }
  require(requiredIds.size() == rows.size());
  const auto audiences = payloadAudiences(*source.materialManifest,
    config.at("node_mapping").get<NativeCanonicalRolePreparer::NodeMap>(), io.requireCurrent);
  require(audiences.size() == rows.size());
  std::map<ndn::Name, std::vector<std::pair<std::uint64_t, std::uint64_t>>> ranges;
  std::map<ndn::Name, Audience> bundleAudiences;
  for (const auto& item : rows) {
    const auto& row = *item.second;
    const auto dataName = ndn::Name(row.at("dataName").get<std::string>());
    const auto found = std::find_if(objects.objects().begin(), objects.objects().end(),
      [&](const auto& value) { return value.second.object.name == dataName; });
    require(found != objects.objects().end() &&
            found->first.compare(0, 16, "material-bundle-") == 0);
    const auto& object = found->second.object;
    const auto offset = row.at("bundleOffset").get<std::uint64_t>();
    const auto size = row.at("bytes").get<std::uint64_t>();
    require(size && size <= NativeCanonicalMaterialBundleMaxBytes &&
            offset <= object.plaintextBytes && size <= object.plaintextBytes - offset &&
            row.at("bundleBytes") == object.plaintextBytes &&
            row.at("bundleDigest") == object.plaintextDigest);
    require(audiences.count(item.first));
    const auto inserted = bundleAudiences.emplace(dataName, audiences.at(item.first));
    require(inserted.second || inserted.first->second == audiences.at(item.first));
    ranges[dataName].emplace_back(offset, size);
    publication.materialPayloadIds.push_back(item.first);
    publication.materialDataNames.push_back(dataName.toUri(ndn::name::UriFormat::CANONICAL));
    publication.materialDigests.push_back(row.at("digest").get<std::string>());
  }
  for (auto& item : ranges) {
    std::sort(item.second.begin(), item.second.end());
    std::uint64_t next = 0;
    for (const auto& range : item.second) {
      require(range.first == next);
      next += range.second;
    }
    const auto found = std::find_if(objects.objects().begin(), objects.objects().end(),
      [&](const auto& value) { return value.second.object.name == item.first; });
    require(found != objects.objects().end() && next == found->second.object.plaintextBytes);
  }
  const auto setReference = [&](const std::string& id, std::string& name, std::string& digest,
                                std::uint64_t& size) {
    const auto& object = objects.objects().at(id).object;
    name = object.name.toUri(ndn::name::UriFormat::CANONICAL);
    digest = object.plaintextDigest;
    size = object.plaintextBytes;
  };
  setReference("material-manifest", publication.materialManifestDataName,
    publication.materialManifestDigest, publication.materialManifestBytes);
  setReference("material-receipt", publication.materialReceiptDataName,
    publication.materialReceiptDigest, publication.materialReceiptBytes);
  setReference("prepared-metadata", publication.preparedMetadataDataName,
    publication.preparedMetadataDigest, publication.preparedMetadataBytes);
  publication.rootDataName = objects.objects().at("business-root").object.name.toUri(
    ndn::name::UriFormat::CANONICAL);
  publication.artifactProfileDigest = root.at("artifactProfileDigest");
  publication.materialManifest = source.materialManifest;
  publication.rollbackOwned = false;
  publication.validate();
  auto catalog = NativeRequestCatalog::load(configuration, std::move(source), control);
  if (catalog.model.canonicalInitializerBytes) {
    require(facts.at("canonicalInitializerBytes") == catalog.model.canonicalInitializerBytes &&
            facts.at("canonicalInitializerObjectDigest") ==
              catalog.model.canonicalInitializerObjectDigest);
  }
  else {
    require(!facts.contains("canonicalInitializerBytes") &&
            !facts.contains("canonicalInitializerObjectDigest"));
  }
  require(catalog.model.descriptor.modelDigest() == objects.modelDigest() &&
          root.at("modelSemanticIdentityDigest") == objects.modelDigest() &&
          root.at("modelIdentityDigest") == catalog.model.descriptor.contentDigest &&
          root.at("modelName") == catalog.model.descriptor.modelName &&
          facts.at("canonicalSourceDigest") == catalog.model.canonicalSourceDigest &&
          facts.at("canonicalSourceBytes") == catalog.model.canonicalSourceBytes &&
          facts.at("packageManifestDigest") == catalog.model.modelManifestDigest &&
          facts.at("materialIdentityDigest") == publication.materialManifest->manifestDigest &&
          publication.artifactProfileDigest ==
            catalog.preparation->publicationFor(catalog.model.descriptor).artifactProfileDigest);
  return {std::move(catalog), std::move(publication), std::move(objects), configuration};
}

} // namespace ndnsf::di
