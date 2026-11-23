#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalPreparationCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-model.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/prepared-onnx-metadata.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/model-object-keys.hpp"

#include <atomic>
#include <limits>

namespace ndnsf::di {

struct NativeCanonicalPreparationCatalog::State
{
  struct Record {
    std::shared_ptr<const NativeInspectedModel> model;
    // This pointer is replaced atomically when the full source is no longer
    // needed.  A caller receives an owning immutable snapshot, so releasing
    // the catalog's transient owner cannot invalidate a concurrent reader.
    mutable std::shared_ptr<const NativeCanonicalSource> source;
    NativeCanonicalRolePreparer roles;
    NativeCanonicalPublicationOptions publication;
    std::shared_ptr<const AutomaticOnnxContract> automaticContract;
    std::size_t maxPayloadBytes;
    NativeStateContractBinding stateBinding;
  };
  mutable std::map<std::string, Record> records;
  std::shared_ptr<const NativeAdapterRegistry> adapters;

  const Record& find(const NativeModelDescriptor& model) const
  {
    const auto found = records.find(model.canonicalJson());
    if (found == records.end()) throw std::invalid_argument("native catalog model identity is not registered");
    return found->second;
  }

  const Record& find(const NativeInspectedModel& model) const
  {
    // This private pointer match is possible only for the independently
    // frozen catalog object. Value/custom inputs retain complete validation.
    for (const auto& item : records)
      if (item.second.model.get() == &model) return item.second;
    model.validate();
    const auto& record = find(model.descriptor);
    const auto& pinned = *record.model;
    if (model.graph.graphDigest != pinned.graph.graphDigest ||
        model.canonicalSourceName != pinned.canonicalSourceName ||
        model.canonicalSourceDigest != pinned.canonicalSourceDigest ||
        model.canonicalSourceBytes != pinned.canonicalSourceBytes ||
        model.modelManifestDigest != pinned.modelManifestDigest ||
        model.canonicalGraphDigest != pinned.canonicalGraphDigest ||
        model.canonicalInitializerBytes != pinned.canonicalInitializerBytes ||
        model.canonicalInitializerObjectDigest != pinned.canonicalInitializerObjectDigest ||
        model.canonicalInitializerDigest != pinned.canonicalInitializerDigest)
      throw std::invalid_argument("native catalog source identity is not registered");
    return record;
  }
};

NativeCanonicalPreparationCatalog::NativeCanonicalPreparationCatalog(
  std::vector<NativeCanonicalCatalogEntry> entries, const NativeAssemblyControl& control)
  : NativeCanonicalPreparationCatalog(std::move(entries), control,
                                      NativeStateContractBinding::DECLARED)
{
}

NativeCanonicalPreparationCatalog::NativeCanonicalPreparationCatalog(
  std::vector<NativeCanonicalCatalogEntry> entries, const NativeAssemblyControl& control,
  NativeStateContractBinding stateBinding)
{
  if (entries.empty()) throw std::invalid_argument("native preparation catalog is empty");
  auto state = std::make_shared<State>();
  struct Group {
    std::string version;
    NativeCatalogModelAdapter::Format format;
    std::size_t limit;
    NativeCatalogModelAdapter::ConversationTokenEncoder conversationTokenEncoder;
    std::vector<NativeModelDescriptor> models;
  };
  std::map<std::string, Group> groups;
  for (auto& entry : entries) {
    std::string automaticSourceJson;
    if (!entry.source.preparedMetadataJson.empty()) {
      if (entry.source.preparedMetadataJson.size() > NativeCanonicalMaterialPublicationMaxBytes)
        throw std::invalid_argument("native catalog prepared metadata exceeds its bound");
      const auto metadata = nativeParseJson(entry.source.preparedMetadataJson, 64);
      if (metadata.value("schema", std::string{}) == "ndnsf-di-prepared-metadata-v2" ||
          metadata.contains("automatic_onnx")) {
        const auto decoded = decodeNativePreparedOnnxMetadata(metadata, control);
        const auto& source = metadata.at("source");
        if (!decoded.automaticContract ||
            nativeCanonicalJson(metadata.at("descriptor")) !=
              entry.model.descriptor.canonicalJson() ||
            source.at("digest") != NativeJson(entry.model.canonicalSourceDigest) ||
            source.at("bytes") != NativeJson(entry.model.canonicalSourceBytes) ||
            source.at("initializer_bytes") != NativeJson(entry.model.canonicalInitializerBytes) ||
            source.at("initializer_object_digest") !=
              NativeJson(entry.model.canonicalInitializerObjectDigest) ||
            decoded.graph.canonicalIdentity.graphDigest != entry.model.canonicalGraphDigest ||
            decoded.graph.canonicalIdentity.initializerDigest !=
              entry.model.canonicalInitializerDigest) {
          throw std::invalid_argument("native catalog automatic source identity differs");
        }
        automaticSourceJson = nativeCanonicalJson(metadata.at("automatic_onnx"));
        entry.automaticContract = decoded.automaticContract;
      }
    }
    if (entry.model.descriptor.adapter.name == "automatic-onnx" && automaticSourceJson.empty())
      throw std::invalid_argument("native catalog automatic source metadata is missing");
    const bool entryHasConversationTokenEncoder =
      static_cast<bool>(entry.conversationTokenEncoder);
    // Validate every model/source before making any publication port available.
    NativeCanonicalRolePreparer roles = entry.sourceGraphInspection
      ? NativeCanonicalRolePreparer(entry.model, std::move(*entry.sourceGraphInspection),
                                   entry.recipe, control, std::move(entry.nodes))
      : NativeCanonicalRolePreparer(entry.model, entry.source, entry.recipe, control,
                                   std::move(entry.nodes));
    // Material derivation is part of the authenticated prepare boundary.  It
    // records graph-template/node/shared-initializer objects before the
    // transient source owner can be released; no Provider placement is chosen
    // here.  A caller may supply a previously verified immutable package when
    // a source was restored from a durable catalog.
    if (!entry.source.materialManifest)
      entry.source.materialManifest = deriveNativeCanonicalMaterialManifest(
        entry.source, control, automaticSourceJson);
    else
      entry.source.materialManifest->validate([&] {
        control.requireActive();
        if (std::chrono::steady_clock::now() >= control.deadline)
          throw std::runtime_error("native material preparation deadline expired");
      });
    if (!entry.source.materialManifest)
      throw std::invalid_argument("native catalog material manifest is missing");
    if (entry.source.materialManifest->automaticSourceJson != automaticSourceJson)
      throw std::invalid_argument("native catalog material source metadata differs");
    if (entry.source.materialManifest->payloadsComplete)
      validateNativeCanonicalMaterialManifest(entry.source, *entry.source.materialManifest, control);
    else if (!entry.source.modelBytes.empty())
      validateNativeCanonicalMaterialReferenceIndex(entry.source, *entry.source.materialManifest, control);
    // A complete prepared hit carries the authenticated reference index and
    // bounded graph metadata, but deliberately has no canonical ONNX bytes.
    // MaterialManifest::validate() above checks the index structure; the
    // source/graph identity checks below bind it to the restored catalog.
    if (entry.source.materialManifest->sourceDigest != entry.model.canonicalSourceDigest ||
        entry.source.materialManifest->graphDigest != entry.model.canonicalGraphDigest ||
        (!entry.model.canonicalInitializerDigest.empty() &&
         entry.source.materialManifest->initializerDigest != entry.model.canonicalInitializerDigest))
      throw std::invalid_argument("native catalog material identity differs from inspected source");
    const auto& model = entry.model.descriptor;
    auto found = groups.find(model.adapterId);
    if (found == groups.end())
      found = groups.emplace(model.adapterId, Group{model.adapterVersion, entry.format,
        entry.maxPayloadBytes, std::move(entry.conversationTokenEncoder), {}}).first;
    auto& group = found->second;
    if (group.version != model.adapterVersion || group.format != entry.format || group.limit != entry.maxPayloadBytes)
      throw std::invalid_argument("native catalog adapter configuration is inconsistent");
    if (static_cast<bool>(group.conversationTokenEncoder) != entryHasConversationTokenEncoder)
      throw std::invalid_argument("native catalog conversation tokenization is inconsistent");
    group.models.push_back(model);
    if (entry.publication.artifactProfileDigest.empty())
      entry.publication.artifactProfileDigest = entry.recipe.artifactProfileDigest;
    else if (entry.publication.artifactProfileDigest != entry.recipe.artifactProfileDigest)
      throw std::invalid_argument("native publication profile differs from role recipe");
    if (entry.publication.maxPublicationBytes == 0) {
      // Publication is a prepare-time union, while maxAssembledBytes is a
      // per-role post-Selection limit. Keep a finite default for callers
      // that have not declared a candidate-local publication budget.
      std::uint64_t sourceBytes = entry.model.canonicalSourceBytes;
      if (entry.model.canonicalInitializerBytes >
          std::numeric_limits<std::uint64_t>::max() - sourceBytes)
        throw std::invalid_argument("native publication budget overflows");
      sourceBytes += entry.model.canonicalInitializerBytes;
      constexpr std::uint64_t rootMargin = 16U * 1024U * 1024U;
      if (sourceBytes > std::numeric_limits<std::uint64_t>::max() - rootMargin ||
          entry.recipe.maxAssembledBytes >
          std::numeric_limits<std::uint64_t>::max() - sourceBytes - rootMargin)
        throw std::invalid_argument("native publication budget overflows");
      entry.publication.maxPublicationBytes =
        entry.recipe.maxAssembledBytes + sourceBytes + rootMargin;
    }
    if (entry.publication.maxPublicationBytes == 0)
      throw std::invalid_argument("native publication budget is not positive");
    const auto key = model.canonicalJson();
    // Deep-copy before freezing: a caller may retain pointers into the entry's
    // vectors even when transferring the containing entry by move.
    auto immutableModel = std::make_shared<const NativeInspectedModel>(entry.model);
    immutableModel->validate();
    State::Record record{std::move(immutableModel),
      std::make_shared<const NativeCanonicalSource>(std::move(entry.source)),
      std::move(roles), std::move(entry.publication), std::move(entry.automaticContract),
      entry.maxPayloadBytes, stateBinding};
    if (!state->records.emplace(key, std::move(record)).second)
      throw std::invalid_argument("native preparation catalog repeats a model identity");
  }
  auto registry = std::make_shared<NativeAdapterRegistry>();
  for (auto& group : groups)
    registry->registerAdapter(std::make_shared<NativeCatalogModelAdapter>(
      std::move(group.second.models), group.second.format, group.second.limit,
      std::move(group.second.conversationTokenEncoder)));
  registry->freeze();
  state->adapters = std::move(registry);
  control.requireActive();
  m_state = std::move(state);
}

std::shared_ptr<const NativeAdapterRegistry> NativeCanonicalPreparationCatalog::adapters() const
{
  return m_state->adapters;
}

std::shared_ptr<const NativeCanonicalSource>
NativeCanonicalPreparationCatalog::sourceRefFor(const NativeModelDescriptor& model) const
{
  const auto& record = m_state->find(model);
  const auto source = std::atomic_load_explicit(&record.source, std::memory_order_acquire);
  if (!source)
    throw std::runtime_error("native preparation source is no longer resident");
  return source;
}

std::weak_ptr<const NativeCanonicalSource>
NativeCanonicalPreparationCatalog::sourceLifetimeForTest(
  const NativeModelDescriptor& model) const
{
  const auto& record = m_state->find(model);
  return std::atomic_load_explicit(&record.source, std::memory_order_acquire);
}

void
NativeCanonicalPreparationCatalog::releaseTransientSource() const noexcept
{
  if (!m_state)
    return;
  for (auto& item : m_state->records)
    std::atomic_store_explicit(&item.second.source,
      std::shared_ptr<const NativeCanonicalSource>{}, std::memory_order_release);
}

void
NativeCanonicalPreparationCatalog::releaseTransientSourceBytes() const
{
  if (!m_state)
    return;
  for (auto& item : m_state->records) {
    const auto source = std::atomic_load_explicit(&item.second.source,
                                                  std::memory_order_acquire);
    if (!source)
      continue;
    if (!source->materialManifest ||
        (source->modelBytes.empty() && !source->initializerBytes))
      continue;
    // Do not mutate a source object that may still be observed by a caller.
    // Publish a small immutable replacement and let existing owning snapshots
    // drain naturally.  This avoids both a data race and shrink_to_fit()
    // allocation/termination inside a noexcept cleanup path.
    auto reduced = std::make_shared<NativeCanonicalSource>();
    reduced->materialManifest = source->materialManifest;
    reduced->preparedMetadataJson = source->preparedMetadataJson;
    std::atomic_store_explicit(&item.second.source,
      std::shared_ptr<const NativeCanonicalSource>(std::move(reduced)),
      std::memory_order_release);
  }
}

std::shared_ptr<const NativeInspectedModel>
NativeCanonicalPreparationCatalog::modelRefFor(const NativeModelDescriptor& model) const
{
  return m_state->find(model).model;
}

std::shared_ptr<const AutomaticOnnxContract>
NativeCanonicalPreparationCatalog::automaticContractFor(
  const NativeModelDescriptor& model) const
{
  return m_state->find(model).automaticContract;
}

NativeCanonicalSource NativeCanonicalPreparationCatalog::sourceFor(
  const NativeModelDescriptor& model) const
{
  const auto& record = m_state->find(model);
  const auto source = std::atomic_load_explicit(&record.source, std::memory_order_acquire);
  if (!source)
    throw std::runtime_error("native preparation source is no longer resident");
  return *source;
}

NativeCanonicalPublicationOptions
NativeCanonicalPreparationCatalog::publicationFor(const NativeModelDescriptor& model) const
{
  return m_state->find(model).publication;
}

std::size_t
NativeCanonicalPreparationCatalog::maxPayloadBytesFor(const NativeModelDescriptor& model) const
{
  return m_state->find(model).maxPayloadBytes;
}

NativeSplitCandidate
NativeCanonicalPreparationCatalog::bindCatalogStateContracts(const NativeInspectedModel& model,
  const NativeSplitCandidate& candidate, const NativeStateTensorMapping& mapping,
  const NativeRequestControl& control) const
{
  control.requireActive();
  const auto& record = m_state->find(model);
  return record.roles.bindStateContracts(model, candidate, mapping, control, record.stateBinding);
}

NativeSourceRegionBoundary
NativeCanonicalPreparationCatalog::sourceRegion(const NativeInspectedModel& model,
  const std::vector<std::string>& nodeIds, const std::vector<std::string>& additionalOutputs,
  const NativeRequestControl& control) const
{
  control.requireActive();
  return m_state->find(model).roles.sourceRegion(nodeIds, additionalOutputs, control);
}

NativeSplitCandidate NativeCanonicalPreparationCatalog::bindStateContracts(const NativeInspectedModel& model,
  const NativeSplitCandidate& candidate, const NativeStateTensorMapping& mapping,
  const NativeRequestControl& control, NativeStateContractBinding binding) const
{
  control.requireActive();
  return m_state->find(model).roles.bindStateContracts(model, candidate, mapping, control, binding);
}

std::vector<NativeSelectionRoleV3>
NativeCanonicalPreparationCatalog::prepareRoles(const NativeInspectedModel& model,
  const NativeSplitCandidate& candidate, const NativeRequestControl& control) const
{
  control.requireActive();
  auto roles = m_state->find(model).roles.prepare(model, candidate, control);
  // Automatic roles must carry the contract identity that the shape seed and
  // the sealed assembly compare. The contract is owned by this catalog entry.
  if (const auto automatic = automaticContractFor(model.descriptor)) {
    const NativeAssemblyControl assembly{control.deadline, [control] {
      control.requireActive();
    }, 1ULL << 30, 1ULL << 30};
    const auto digest = automatic->digest(assembly);
    for (auto& role : roles) {
      role.automaticContractDigest = digest;
      // Constant-node tensors can have no shape in the graph metadata or the
      // planning graph; the contract still knows their rank and fixed dims.
      for (auto* ports : {&role.expectedInputs, &role.expectedOutputs}) {
        for (auto& port : *ports) {
          if (!port.shape.empty()) continue;
          const auto scoped = "graph/" + port.name;
          const auto tensor = std::find_if(automatic->tensors.begin(), automatic->tensors.end(),
            [&](const auto& value) { return value.name == scoped; });
          if (tensor == automatic->tensors.end() || !tensor->dimensions) continue;
          for (const auto& dimension : *tensor->dimensions) {
            if (dimension.kind == DimensionConstraint::Kind::FIXED)
              port.shape.emplace_back(dimension.fixed);
            else if (dimension.kind == DimensionConstraint::Kind::SYMBOL)
              port.shape.emplace_back(dimension.symbol);
            else
              port.shape.emplace_back(std::int64_t{0});
          }
        }
      }
      // The recipe digest covers the role fields, so it must be recomputed
      // after the digest and shape corrections are applied.
      role.recipeDigest = nativePlanningDigest(canonicalNativeOnnxRecipeJson(role));
    }
  }
  return roles;
}

std::shared_ptr<NativeRequestPreparation> NativeCanonicalPreparationCatalog::makePreparation(
  std::shared_ptr<ndn_service_framework::ServiceUser> user, std::string serviceName) const
{
  // The public path always binds the existing Core crypto/IO owner.
  return makePreparation([user = std::move(user), serviceName = std::move(serviceName)](
    auto options, auto source) {
    return NativeCanonicalArtifactPublisher(user, serviceName, std::move(options), std::move(source));
  });
}

std::shared_ptr<NativeRequestPreparation>
NativeCanonicalPreparationCatalog::makePreparation(
  std::shared_ptr<ndn_service_framework::ServiceUser> user, std::string serviceName,
  std::optional<NativePreparedCanonicalPublication> preparedPublication,
  std::shared_ptr<const NativeModelObjectKeyIndexV1> modelObjectKeyIndex) const
{
  return makePreparation([user = std::move(user), serviceName = std::move(serviceName)](
    auto options, auto source) {
    return NativeCanonicalArtifactPublisher(user, serviceName, std::move(options), std::move(source));
  }, std::move(preparedPublication), std::move(modelObjectKeyIndex));
}

NativePreparedCanonicalPublication
NativeCanonicalPreparationCatalog::preparePublication(
  std::shared_ptr<ndn_service_framework::ServiceUser> user, std::string serviceName,
  const NativeModelDescriptor& model, const NativeRequestControl& control) const
{
  const auto& record = m_state->find(model);
  // Material-backed publication has already authenticated and indexed the
  // complete source during catalog construction.  Release the duplicate
  // source/initializer buffers before the publisher starts encrypting the
  // material bundles; the immutable manifest and payloads remain available.
  releaseTransientSourceBytes();
  NativeCanonicalArtifactPublisher publisher(user, std::move(serviceName), record.publication,
    [state = m_state](const auto& inspected, const auto& requestControl) {
      requestControl.requireActive();
      const auto& record = state->find(inspected);
      const auto source = std::atomic_load_explicit(&record.source, std::memory_order_acquire);
      if (!source)
        throw std::runtime_error("native preparation source is no longer resident");
      return source;
    });
  return publisher.prepare(*record.model, control);
}

std::shared_ptr<NativeRequestPreparation>
NativeCanonicalPreparationCatalog::makePreparation(PublisherFactory factory) const
{
  return makePreparation(std::move(factory), {});
}

std::shared_ptr<NativeRequestPreparation>
NativeCanonicalPreparationCatalog::makePreparation(
  PublisherFactory factory, std::optional<NativePreparedCanonicalPublication> preparedPublication,
  std::shared_ptr<const NativeModelObjectKeyIndexV1> modelObjectKeyIndex) const
{
  const auto state = m_state;
  auto publishers = std::make_shared<std::map<std::string, NativeCanonicalArtifactPublisher>>();
  for (const auto& item : state->records) {
    const auto source = [state](const NativeInspectedModel& model, const NativeRequestControl& control) {
      control.requireActive();
      const auto& record = state->find(model);
      const auto source = std::atomic_load_explicit(&record.source, std::memory_order_acquire);
      if (!source)
        throw std::runtime_error("native preparation source is no longer resident");
      return source;
    };
    publishers->emplace(item.first, factory(item.second.publication, source));
  }
  if (preparedPublication)
    preparedPublication->validate();
  if (modelObjectKeyIndex && !preparedPublication)
    throw std::invalid_argument("stored object key index requires a prepared publication");
  return std::shared_ptr<NativeRequestPreparation>(new NativeRequestPreparation(state->adapters,
    NativeRequestPreparation::OwnedInspection{
      [state](const NativePreparedInput& input, const NativeModelDescriptor& model) {
        input.validate();
        return state->find(model).model;
      },
      [state](const NativeInspectedModel& model) {
        for (const auto& item : state->records)
          if (item.second.model.get() == &model) return true;
        return false;
      }
    },
    [state, publishers, preparedPublication = std::move(preparedPublication),
     modelObjectKeyIndex = std::move(modelObjectKeyIndex)](
      const auto& model, const auto& candidate, const auto& roles, const auto& control) {
      control.requireActive();
      state->find(model);
      auto& publisher = publishers->at(model.descriptor.canonicalJson());
      NativeArtifactBinding binding;
      if (preparedPublication)
        // makePreparation() validated the receipt before this const capture;
        // keep request-time binding limited to request/model/role checks and
        // root consistency rather than rescanning material receipts.
        binding = publisher.bindPreparedAfterValidation(
          model, candidate, roles, *preparedPublication, control);
      else {
        const auto publication = publisher.prepare(model, control);
        binding = publisher.bindPreparedAfterValidation(model, candidate, roles, publication,
                                                         control);
        binding.servingLeases.insert(binding.servingLeases.end(),
                                     publication.servingLeases.begin(),
                                     publication.servingLeases.end());
      }
      if (modelObjectKeyIndex) {
        binding.modelObjectKeyIndex = modelObjectKeyIndex;
        // Stored ciphertext is admitted and opened by DI with the current object-key lease.
        // Core's legacy artifact prefetch cannot decode that transport.
        if (modelObjectKeyIndex->storedPublication) {
          binding.artifactPrefetchRequired = false;
          // Without Core's alias cache, the assignment names the original stored root.
          for (auto& artifact : binding.artifactNameByRole)
            artifact.second = binding.sourceByRole.at(artifact.first);
        }
      }
      return binding;
    },
    [state](const auto& model, const auto& candidate, const auto& control) {
      return state->find(model).roles.prepare(model, candidate, control);
    }, NativeRequestPreparation::OwnedInspectionTag{}));
}

} // namespace ndnsf::di
