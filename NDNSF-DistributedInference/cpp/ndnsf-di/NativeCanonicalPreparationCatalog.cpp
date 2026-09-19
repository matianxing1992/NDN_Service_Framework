#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalPreparationCatalog.hpp"

#include <limits>

namespace ndnsf::di {

struct NativeCanonicalPreparationCatalog::State
{
  struct Record {
    NativeInspectedModel model;
    std::shared_ptr<const NativeCanonicalSource> source;
    NativeCanonicalRolePreparer roles;
    NativeCanonicalPublicationOptions publication;
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
    model.validate();
    const auto& record = find(model.descriptor);
    const auto& pinned = record.model;
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
    const bool entryHasConversationTokenEncoder =
      static_cast<bool>(entry.conversationTokenEncoder);
    // Validate every model/source before making any publication port available.
    NativeCanonicalRolePreparer roles(entry.model, entry.source, entry.recipe, control, std::move(entry.nodes));
    // Material derivation is part of the authenticated prepare boundary.  It
    // records graph-template/node/shared-initializer objects before the
    // transient source owner can be released; no Provider placement is chosen
    // here.  A caller may supply a previously verified immutable package when
    // a source was restored from a durable catalog.
    if (!entry.source.materialManifest)
      entry.source.materialManifest = deriveNativeCanonicalMaterialManifest(entry.source, control);
    else
      entry.source.materialManifest->validate();
    if (!entry.source.materialManifest)
      throw std::invalid_argument("native catalog material manifest is missing");
    validateNativeCanonicalMaterialManifest(entry.source, *entry.source.materialManifest, control);
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
    State::Record record{std::move(entry.model),
      std::make_shared<const NativeCanonicalSource>(std::move(entry.source)), std::move(roles), std::move(entry.publication)};
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

const NativeCanonicalSource&
NativeCanonicalPreparationCatalog::sourceRefFor(const NativeModelDescriptor& model) const
{
  const auto source = m_state->find(model).source;
  if (!source)
    throw std::runtime_error("native preparation source is no longer resident");
  return *source;
}

std::weak_ptr<const NativeCanonicalSource>
NativeCanonicalPreparationCatalog::sourceLifetimeForTest(
  const NativeModelDescriptor& model) const
{
  return m_state->find(model).source;
}

void
NativeCanonicalPreparationCatalog::releaseTransientSource() const noexcept
{
  if (!m_state)
    return;
  for (auto& item : m_state->records)
    item.second.source.reset();
}

NativeCanonicalSource NativeCanonicalPreparationCatalog::sourceFor(
  const NativeModelDescriptor& model) const
{
  const auto source = m_state->find(model).source;
  if (!source)
    throw std::runtime_error("native preparation source is no longer resident");
  return *source;
}

NativeCanonicalPublicationOptions
NativeCanonicalPreparationCatalog::publicationFor(const NativeModelDescriptor& model) const
{
  return m_state->find(model).publication;
}

NativeSplitCandidate NativeCanonicalPreparationCatalog::bindStateContracts(const NativeInspectedModel& model,
  const NativeSplitCandidate& candidate, const NativeStateTensorMapping& mapping,
  const NativeRequestControl& control) const
{
  control.requireActive();
  return m_state->find(model).roles.bindStateContracts(model, candidate, mapping, control);
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
  std::optional<NativePreparedCanonicalPublication> preparedPublication) const
{
  return makePreparation([user = std::move(user), serviceName = std::move(serviceName)](
    auto options, auto source) {
    return NativeCanonicalArtifactPublisher(user, serviceName, std::move(options), std::move(source));
  }, std::move(preparedPublication));
}

NativePreparedCanonicalPublication
NativeCanonicalPreparationCatalog::preparePublication(
  std::shared_ptr<ndn_service_framework::ServiceUser> user, std::string serviceName,
  const NativeModelDescriptor& model, const NativeRequestControl& control) const
{
  const auto& record = m_state->find(model);
  NativeCanonicalArtifactPublisher publisher(user, std::move(serviceName), record.publication,
    [state = m_state](const auto& inspected, const auto& requestControl) {
      requestControl.requireActive();
      const auto source = state->find(inspected).source;
      if (!source)
        throw std::runtime_error("native preparation source is no longer resident");
      return source;
    });
  return publisher.prepare(record.model, control);
}

std::shared_ptr<NativeRequestPreparation>
NativeCanonicalPreparationCatalog::makePreparation(PublisherFactory factory) const
{
  return makePreparation(std::move(factory), {});
}

std::shared_ptr<NativeRequestPreparation>
NativeCanonicalPreparationCatalog::makePreparation(
  PublisherFactory factory, std::optional<NativePreparedCanonicalPublication> preparedPublication) const
{
  const auto state = m_state;
  auto publishers = std::make_shared<std::map<std::string, NativeCanonicalArtifactPublisher>>();
  for (const auto& item : state->records) {
    const auto source = [state](const NativeInspectedModel& model, const NativeRequestControl& control) {
      control.requireActive();
      const auto source = state->find(model).source;
      if (!source)
        throw std::runtime_error("native preparation source is no longer resident");
      return source;
    };
    publishers->emplace(item.first, factory(item.second.publication, source));
  }
  if (preparedPublication)
    preparedPublication->validate();
  return std::make_shared<NativeRequestPreparation>(state->adapters,
    [state](const NativePreparedInput& input, const NativeModelDescriptor& model) {
      input.validate();
      return state->find(model).model;
    },
    [state, publishers, preparedPublication = std::move(preparedPublication)](
      const auto& model, const auto& candidate, const auto& roles, const auto& control) {
      control.requireActive();
      state->find(model);
      if (preparedPublication)
        return publishers->at(model.descriptor.canonicalJson()).bindPrepared(
          model, candidate, roles, *preparedPublication, control);
      return publishers->at(model.descriptor.canonicalJson())(model, candidate, roles, control);
    },
    [state](const auto& model, const auto& candidate, const auto& control) {
      return state->find(model).roles.prepare(model, candidate, control);
    });
}

} // namespace ndnsf::di
