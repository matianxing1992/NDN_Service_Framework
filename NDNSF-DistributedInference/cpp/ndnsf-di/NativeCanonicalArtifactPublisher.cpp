#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalArtifactPublisher.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <algorithm>
#include <condition_variable>
#include <future>
#include <ndn-cxx/name.hpp>
#include <mutex>
#include <optional>

namespace ndnsf::di {
namespace {
bool digest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

struct PublicationJob
{
  std::mutex mutex;
  std::condition_variable condition;
  std::function<NativeUncachedPublication()> work;
  NativeUncachedPublication result;
  std::function<void(const NativeUncachedPublication&)> rollback;
  std::exception_ptr error;
  bool done = false;
  bool abandoned = false;
};

struct PreparedPublicationJob
{
  std::mutex mutex;
  std::condition_variable condition;
  std::function<NativePreparedCanonicalPublication()> work;
  std::function<void(const NativePreparedCanonicalPublication&)> rollback;
  NativePreparedCanonicalPublication result;
  std::exception_ptr error;
  bool done = false;
  bool abandoned = false;
};
}

void NativePreparedCanonicalPublication::validate() const
{
  const auto validName = [](const std::string& value) {
    try {
      return !value.empty() && value.front() == '/' && !ndn::Name(value).empty() &&
        ndn::Name(value).toUri() == value;
    }
    catch (...) {
      return false;
    }
  };
  if (!validName(sourceDataName) || !validName(rootDataName) ||
      (!initializerDataName.empty() && !validName(initializerDataName)) ||
      canonicalManifestJson.empty() || !digest(manifestDigest) ||
      (!artifactProfileDigest.empty() && !digest(artifactProfileDigest)) ||
      canonicalManifestJson.size() > 1024 * 1024 ||
      nativePlanningDigest(canonicalManifestJson) != manifestDigest)
    throw std::invalid_argument("native prepared canonical publication is incomplete");
}

NativeCanonicalArtifactPublisher::NativeCanonicalArtifactPublisher(
  std::shared_ptr<ndn_service_framework::ServiceUser> user, std::string serviceName,
  NativeCanonicalPublicationOptions options, SourcePort source)
  : NativeCanonicalArtifactPublisher(Transport{
      [user](auto work) { if (!user) throw std::invalid_argument("missing Core publication owner");
                         user->postToIo(std::move(work)); },
      [user] { return user && user->isOnIoThread(); },
      [user, serviceName] { return user->prepareServiceRequest(serviceName); },
      [user](const auto& request, const auto& bytes, const auto& label) {
        return user->publishEncryptedLargeData(request, bytes, label);
      },
      [user](const auto& publications) {
        if (user)
          user->abortLargeDataPublications(publications);
      }}, serviceName, std::move(options), std::move(source))
{
  if (!user) throw std::invalid_argument("missing Core publication owner");
}

NativeCanonicalArtifactPublisher::NativeCanonicalArtifactPublisher(Transport transport,
  std::string serviceName, NativeCanonicalPublicationOptions options, SourcePort source)
  : m_transport(std::move(transport)), m_serviceName(std::move(serviceName)),
    m_options(std::move(options)), m_source(std::move(source)), m_cache(std::make_shared<CacheState>())
{
  if (!m_source || !m_transport.post || !m_transport.isOnIoThread || !m_transport.begin ||
      !m_transport.publish || m_serviceName.empty() || m_serviceName.front() != '/' ||
      m_options.artifactRoot.empty() || m_options.artifactRoot.front() != '/' ||
      ndn::Name(m_serviceName).empty() || ndn::Name(m_options.artifactRoot).empty() ||
      (!m_options.packageManifestDigest.empty() && !digest(m_options.packageManifestDigest)) ||
      (!m_options.artifactProfileDigest.empty() && !digest(m_options.artifactProfileDigest)) ||
      std::any_of(m_options.layerManifestDigests.begin(), m_options.layerManifestDigests.end(),
                  [](const auto& value) { return !digest(value); }))
    throw std::invalid_argument("native canonical publication configuration is incomplete");
  m_serviceName = ndn::Name(m_serviceName).toUri();
  m_options.artifactRoot = ndn::Name(m_options.artifactRoot).toUri();
}

NativeRequestPreparation::ArtifactPort NativeCanonicalArtifactPublisher::artifactPort() const
{
  return [owner = *this](const auto& model, const auto& candidate, const auto& roles, const auto& control) {
    return owner(model, candidate, roles, control);
  };
}

std::string NativeCanonicalArtifactPublisher::prepareKey(
  const NativeInspectedModel& model) const
{
  model.validate();
  return nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-prepared-publication-v1"},
    {"service", m_serviceName}, {"artifactRoot", m_options.artifactRoot},
    {"packageManifestDigest", m_options.packageManifestDigest},
    {"artifactProfileDigest", m_options.artifactProfileDigest},
    {"modelIdentity", model.descriptor.canonicalJson()},
    {"canonicalSourceName", model.canonicalSourceName},
    {"sourceDigest", model.canonicalSourceDigest},
    {"sourceBytes", model.canonicalSourceBytes},
    {"initializerDigest", model.canonicalInitializerObjectDigest},
    {"initializerBytes", model.canonicalInitializerBytes},
    {"canonicalInitializerDigest", model.canonicalInitializerDigest},
    {"graphDigest", model.canonicalGraphDigest}}));
}

void NativeCanonicalArtifactPublisher::abortPreparedPublication(
  const NativePreparedCanonicalPublication& publication) const noexcept
{
  if (!m_transport.abort)
    return;
  try {
    std::vector<ndn_service_framework::LargeDataPublishResult> publications;
    const auto addData = [&publication](ndn_service_framework::LargeDataPublishResult& result) {
      result.rollbackDataNames = publication.rollbackDataNames;
      if (!publication.sourceDataName.empty())
        result.rollbackDataNames.push_back(publication.sourceDataName);
      if (!publication.initializerDataName.empty())
        result.rollbackDataNames.push_back(publication.initializerDataName);
    };
    if (!publication.rollbackKeyReferences.empty()) {
      for (const auto& reference : publication.rollbackKeyReferences) {
        ndn_service_framework::LargeDataPublishResult result;
        result.success = true;
        result.encryptedDataName = ndn::Name(publication.rootDataName);
        addData(result);
        result.rollbackKeyId = reference.keyId;
        result.rollbackServiceName = reference.serviceName;
        publications.push_back(std::move(result));
      }
    }
    else {
      // Receipts made by an older publisher have no key-reference metadata;
      // still remove every locally published object once.
      ndn_service_framework::LargeDataPublishResult result;
      result.success = true;
      result.encryptedDataName = ndn::Name(publication.rootDataName);
      addData(result);
      publications.push_back(std::move(result));
    }
    m_transport.abort(publications);
  }
  catch (...) {}
}

void NativeCanonicalArtifactPublisher::abortPublishedResults(
  const std::vector<ndn_service_framework::LargeDataPublishResult>& publications) const noexcept
{
  if (!m_transport.abort || publications.empty())
    return;
  try { m_transport.abort(publications); }
  catch (...) {}
}

NativePreparedCanonicalPublication NativeCanonicalArtifactPublisher::prepare(
  const NativeInspectedModel& model, const NativeRequestControl& control) const
{
  control.requireActive();
  if (!m_cache)
    throw std::runtime_error("DI_NATIVE_PUBLICATION_CACHE_UNAVAILABLE");
  if (m_transport.isOnIoThread())
    throw std::runtime_error("DI_NATIVE_PUBLICATION_IO_WAIT_FORBIDDEN");
  model.validate();
  const auto key = prepareKey(model);
  std::shared_ptr<std::promise<NativePreparedCanonicalPublication>> promise;
  std::shared_future<NativePreparedCanonicalPublication> future;
  std::shared_ptr<CacheState::SharedControl> sharedControl;
  bool owner = false;
  {
    std::lock_guard<std::mutex> lock(m_cache->mutex);
    const auto completed = m_cache->prepared.find(key);
    if (completed != m_cache->prepared.end()) {
      m_cache->cacheHits.fetch_add(1, std::memory_order_relaxed);
      return completed->second;
    }
    const auto pending = m_cache->preparedInFlight.find(key);
    if (pending != m_cache->preparedInFlight.end()) {
      future = pending->second.future;
      sharedControl = pending->second.control;
      sharedControl->participants.fetch_add(1, std::memory_order_acq_rel);
      m_cache->sharedWaiters.fetch_add(1, std::memory_order_relaxed);
    }
    else {
      promise = std::make_shared<std::promise<NativePreparedCanonicalPublication>>();
      future = promise->get_future().share();
      sharedControl = std::make_shared<CacheState::SharedControl>();
      m_cache->preparedInFlight.emplace(key, CacheState::PreparedInFlight{future, sharedControl});
      owner = true;
    }
  }
  const auto release = [cache = m_cache, sharedControl] {
    if (!cache || !sharedControl)
      return;
    // Serialize the last-participant cancellation fence with the worker's
    // cancelled check and cache insertion.  Without this mutex, the worker
    // could observe "not cancelled" just before the final owner decrements
    // participants, then publish a receipt after every waiter has left.
    std::lock_guard<std::mutex> lock(cache->mutex);
    if (sharedControl->participants.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
        !sharedControl->committed.load(std::memory_order_acquire))
      sharedControl->cancelled.store(true, std::memory_order_release);
  };
  if (!owner) {
    try {
      while (future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready)
        control.requireActive();
      control.requireActive();
      auto result = future.get();
      release();
      return result;
    }
    catch (...) {
      release();
      throw;
    }
  }
  auto publicationControl = control;
  publicationControl.cancelled = [sharedControl] {
    return sharedControl->cancelled.load(std::memory_order_acquire);
  };
  std::thread worker;
  try {
    worker = std::thread([owner = *this, model, key, promise, sharedControl,
                          publicationControl] () mutable {
      try {
        auto result = owner.prepareUncached(model, publicationControl);
        bool discarded = false;
        bool cached = false;
        try {
          std::lock_guard<std::mutex> lock(owner.m_cache->mutex);
          owner.m_cache->preparedInFlight.erase(key);
          if (sharedControl->cancelled.load(std::memory_order_acquire))
            discarded = true;
          else {
            const auto [it, inserted] = owner.m_cache->prepared.emplace(key, result);
            if (!inserted)
              throw std::runtime_error("duplicate prepared publication cache key");
            (void)it;
            cached = true;
            sharedControl->committed.store(true, std::memory_order_release);
            // Keep the terminal handoff under the same mutex as the cache
            // insertion. A late owner cancellation can therefore only observe
            // a committed receipt or the discard path, never an untracked one.
            promise->set_value(result);
          }
        }
        catch (...) {
          if (cached) {
            std::lock_guard<std::mutex> lock(owner.m_cache->mutex);
            owner.m_cache->prepared.erase(key);
            sharedControl->committed.store(false, std::memory_order_release);
          }
          // The cache insertion or promise handoff failed after Core objects
          // were published. Remove the complete publication before exposing
          // the failure to waiters.
          if (!discarded)
            owner.abortPreparedPublication(result);
          throw;
        }
        if (discarded) {
          owner.abortPreparedPublication(result);
          throw std::runtime_error("DI_NATIVE_PREPARATION_CANCELLED");
        }
      }
      catch (...) {
        const auto error = std::current_exception();
        {
          std::lock_guard<std::mutex> lock(owner.m_cache->mutex);
          owner.m_cache->preparedInFlight.erase(key);
        }
        try { promise->set_exception(error); }
        catch (const std::future_error&) {}
      }
    });
    worker.detach();
  }
  catch (...) {
    const auto error = std::current_exception();
    if (worker.joinable()) worker.join();
    {
      std::lock_guard<std::mutex> lock(m_cache->mutex);
      m_cache->preparedInFlight.erase(key);
    }
    try { promise->set_exception(error); }
    catch (const std::future_error&) {}
    release();
    throw;
  }
  for (;;) {
    if (future.wait_for(std::chrono::milliseconds(10)) == std::future_status::ready) {
      try {
        auto result = future.get();
        release();
        return result;
      }
      catch (...) {
        release();
        throw;
      }
    }
    try { control.requireActive(); }
    catch (...) {
      release();
      throw;
    }
  }
}

NativeArtifactBinding NativeCanonicalArtifactPublisher::bindPrepared(
  const NativeInspectedModel& model, const NativeSplitCandidate& candidate,
  const std::vector<NativeSelectionRoleV3>& roles,
  const NativePreparedCanonicalPublication& publication,
  const NativeRequestControl& control) const
{
  control.requireActive();
  model.validate();
  NativeRequestPreparation::validateRoles(model, candidate, roles);
  if (roles.empty())
    throw std::invalid_argument("native prepared publication requires selected roles");
  publication.validate();
  const auto root = NativeJson::parse(publication.canonicalManifestJson);
  const auto& metadata = root.at("metadata");
  if (root.value("modelIdentityDigest", NativeJson{}) != model.descriptor.contentDigest ||
      root.value("modelName", NativeJson{}) != model.descriptor.modelName ||
      metadata.value("canonicalSourceDigest", NativeJson{}) != model.canonicalSourceDigest ||
      metadata.value("canonicalSourceBytes", NativeJson{}) != model.canonicalSourceBytes ||
      metadata.value("canonicalSourceDataName", NativeJson{}) != publication.sourceDataName ||
      publication.rootDataName.empty() ||
      root.value("artifactProfileDigest", NativeJson{}) != publication.artifactProfileDigest ||
      (!m_options.packageManifestDigest.empty() &&
      metadata.value("packageManifestDigest", NativeJson{}) != m_options.packageManifestDigest))
    throw std::invalid_argument("native prepared publication differs from inspected source");
  auto binding = NativeArtifactBinding{};
  binding.canonicalManifestJson = publication.canonicalManifestJson;
  binding.manifestDigest = publication.manifestDigest;
  binding.recipeDigest = roles.front().recipeDigest;
  for (const auto& role : roles) {
    if (role.artifactProfileDigest != publication.artifactProfileDigest)
      throw std::invalid_argument("native prepared publication profile differs from role");
    auto stable = ndn::Name(m_options.artifactRoot).append(candidate.candidateDigest.substr(7));
    stable.append("manifest").append(binding.manifestDigest.substr(7));
    stable.append(ndn::Name(role.role));
    const auto degree = candidate.tensorDegreesByRole.find(role.role);
    if (degree != candidate.tensorDegreesByRole.end() && degree->second > 1)
      stable.append("rank").appendNumber(role.rank);
    binding.artifactNameByRole.emplace(role.selectedRole, stable.toUri());
    binding.sourceByRole.emplace(role.selectedRole, publication.rootDataName);
    binding.artifactDigestByRole.emplace(role.selectedRole, role.artifactDigest);
  }
  NativeRequestPreparation::bindPublishedRoles(model, candidate, roles, binding);
  return binding;
}

NativeCanonicalArtifactPublisher::Stats NativeCanonicalArtifactPublisher::stats() const noexcept
{
  if (!m_cache)
    return {};
  return {m_cache->sourceVerifications.load(std::memory_order_relaxed),
          m_cache->publicationCalls.load(std::memory_order_relaxed),
          m_cache->cacheHits.load(std::memory_order_relaxed),
          m_cache->sharedWaiters.load(std::memory_order_relaxed)};
}

std::string NativeCanonicalArtifactPublisher::cacheKey(
  const NativeInspectedModel& model, const NativeSplitCandidate& candidate,
  const std::vector<NativeSelectionRoleV3>& roles) const
{
  NativeJson roleList = NativeJson::array();
  for (const auto& role : roles) {
    auto value = nativeAssemblyJson(role);
    value["selectedRole"] = role.selectedRole;
    roleList.push_back(std::move(value));
  }
  const NativeJson key{{"schema", "ndnsf-di-publication-cache-v1"},
    {"service", m_serviceName}, {"artifactRoot", m_options.artifactRoot},
    {"packageManifestDigest", m_options.packageManifestDigest},
    {"modelIdentity", model.descriptor.canonicalJson()},
    {"sourceDigest", model.canonicalSourceDigest},
    {"initializerDigest", model.canonicalInitializerObjectDigest},
    {"graphDigest", model.canonicalGraphDigest},
    {"candidateDigest", candidate.candidateDigest}, {"roles", std::move(roleList)}};
  return nativePlanningDigest(nativeCanonicalJson(key));
}

NativeArtifactBinding NativeCanonicalArtifactPublisher::operator()(const NativeInspectedModel& model,
  const NativeSplitCandidate& candidate, const std::vector<NativeSelectionRoleV3>& roles,
  const NativeRequestControl& control) const
{
  control.requireActive();
  if (!m_cache)
    throw std::runtime_error("DI_NATIVE_PUBLICATION_CACHE_UNAVAILABLE");
  // The public operator blocks for the shared result.  Reject an I/O-thread
  // caller before creating a worker; otherwise the worker's postToIo task
  // would wait behind this thread's future wait and deadlock the Core loop.
  if (m_transport.isOnIoThread())
    throw std::runtime_error("DI_NATIVE_PUBLICATION_IO_WAIT_FORBIDDEN");
  model.validate();
  NativeRequestPreparation::validateRoles(model, candidate, roles);
  const auto key = cacheKey(model, candidate, roles);
  std::shared_ptr<std::promise<NativeArtifactBinding>> promise;
  std::shared_future<NativeArtifactBinding> future;
  std::shared_ptr<CacheState::SharedControl> sharedControl;
  bool owner = false;
  {
    std::lock_guard<std::mutex> lock(m_cache->mutex);
    const auto completed = m_cache->completed.find(key);
    if (completed != m_cache->completed.end()) {
      m_cache->cacheHits.fetch_add(1, std::memory_order_relaxed);
      return completed->second;
    }
    const auto pending = m_cache->inFlight.find(key);
    if (pending != m_cache->inFlight.end()) {
      future = pending->second.future;
      sharedControl = pending->second.control;
      sharedControl->participants.fetch_add(1, std::memory_order_acq_rel);
      m_cache->sharedWaiters.fetch_add(1, std::memory_order_relaxed);
    }
    else {
      promise = std::make_shared<std::promise<NativeArtifactBinding>>();
      future = promise->get_future().share();
      sharedControl = std::make_shared<CacheState::SharedControl>();
      m_cache->inFlight.emplace(key, CacheState::InFlight{future, sharedControl});
      owner = true;
    }
  }
  const auto release = [cache = m_cache, sharedControl] {
    if (!cache || !sharedControl)
      return;
    std::lock_guard<std::mutex> lock(cache->mutex);
    if (sharedControl->participants.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
        !sharedControl->committed.load(std::memory_order_acquire))
      sharedControl->cancelled.store(true, std::memory_order_release);
  };
  if (!owner) {
    try {
      while (future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready)
        control.requireActive();
      control.requireActive();
      auto result = future.get();
      release();
      return result;
    }
    catch (...) {
      release();
      throw;
    }
  }
  // Run the shared publication independently from the creator's blocking
  // caller.  This lets a cancelled creator leave the job alive for another
  // waiter, while a cancellation after the last participant sets the shared
  // fence and causes the worker to abort between bounded phases.
  auto publicationControl = control;
  publicationControl.cancelled = [sharedControl] {
    return sharedControl->cancelled.load(std::memory_order_acquire);
  };
  std::thread worker;
  try {
    worker = std::thread([owner = *this, model, candidate, roles, key, promise, sharedControl,
                        publicationControl] () mutable {
      try {
        auto result = owner.publishUncached(model, candidate, roles, publicationControl);
        bool discarded = false;
        bool cached = false;
        try {
          std::lock_guard<std::mutex> lock(owner.m_cache->mutex);
          owner.m_cache->inFlight.erase(key);
          if (sharedControl->cancelled.load(std::memory_order_acquire))
            discarded = true;
          else {
            const auto [it, inserted] = owner.m_cache->completed.emplace(key, result.binding);
            if (!inserted)
              throw std::runtime_error("duplicate completed publication cache key");
            (void)it;
            cached = true;
            sharedControl->committed.store(true, std::memory_order_release);
            promise->set_value(result.binding);
          }
        }
        catch (...) {
          if (cached) {
            std::lock_guard<std::mutex> lock(owner.m_cache->mutex);
            owner.m_cache->completed.erase(key);
            sharedControl->committed.store(false, std::memory_order_release);
          }
          if (!discarded)
            owner.abortPublishedResults(result.publications);
          throw;
        }
        if (discarded) {
          owner.abortPublishedResults(result.publications);
          throw std::runtime_error("DI_NATIVE_PUBLICATION_CANCELLED");
        }
      }
      catch (...) {
        const auto error = std::current_exception();
        {
          std::lock_guard<std::mutex> lock(owner.m_cache->mutex);
          owner.m_cache->inFlight.erase(key);
        }
        try { promise->set_exception(error); }
        catch (const std::future_error&) {}
      }
    });
    worker.detach();
  }
  catch (...) {
    const auto error = std::current_exception();
    if (worker.joinable())
      worker.join();
    {
      std::lock_guard<std::mutex> lock(m_cache->mutex);
      m_cache->inFlight.erase(key);
    }
    try { promise->set_exception(error); }
    catch (const std::future_error&) {}
    release();
    throw;
  }
  for (;;) {
    if (future.wait_for(std::chrono::milliseconds(10)) == std::future_status::ready) {
      try {
        auto result = future.get();
        release();
        return result;
      }
      catch (...) {
        release();
        throw;
      }
    }
    try {
      control.requireActive();
    }
    catch (...) {
      release();
      throw;
    }
  }
}

NativePreparedCanonicalPublication NativeCanonicalArtifactPublisher::prepareUncached(
  const NativeInspectedModel& model, const NativeRequestControl& control) const
{
  control.requireActive();
  if (m_transport.isOnIoThread())
    throw std::runtime_error("DI_NATIVE_PUBLICATION_IO_WAIT_FORBIDDEN");
  model.validate();
  const auto source = m_source(model, control);
  m_cache->sourceVerifications.fetch_add(1, std::memory_order_relaxed);
  control.requireActive();
  if (!source || source->modelBytes.empty() ||
      source->modelBytes.size() != model.canonicalSourceBytes ||
      nativePlanningDigest(source->modelBytes.data(), source->modelBytes.size()) !=
        model.canonicalSourceDigest ||
      source->initializerBytes.has_value() != (model.canonicalInitializerBytes != 0) ||
      (source->initializerBytes &&
       (source->initializerBytes->size() != model.canonicalInitializerBytes ||
        nativePlanningDigest(source->initializerBytes->data(), source->initializerBytes->size()) !=
          model.canonicalInitializerObjectDigest)))
    throw std::invalid_argument("native prepared publication source differs from inspection");

  auto state = std::make_shared<PreparedPublicationJob>();
  state->rollback = [owner = *this](const auto& publication) {
    owner.abortPreparedPublication(publication);
  };
  state->work = [transport = m_transport, service = m_serviceName, options = m_options,
                 source, model, control, cache = m_cache,
                 weak = std::weak_ptr<PreparedPublicationJob>(state)] {
    const auto active = [&] {
      control.requireActive();
      const auto current = weak.lock();
      if (!current) throw std::runtime_error("DI_NATIVE_PUBLICATION_ABANDONED");
      std::lock_guard<std::mutex> lock(current->mutex);
      if (current->abandoned) throw std::runtime_error("DI_NATIVE_PUBLICATION_ABANDONED");
    };
    std::vector<ndn_service_framework::LargeDataPublishResult> publishedResults;
    publishedResults.reserve(source->initializerBytes ? 3 : 2);
    try {
      active();
      const auto request = transport.begin();
      if (request.requestId.empty() || request.serviceName.toUri() != service)
        throw std::runtime_error("DI_NATIVE_PUBLICATION_REQUEST_MISMATCH");
      const auto publish = [&](const std::vector<std::uint8_t>& bytes, const std::string& label) {
        active();
        cache->publicationCalls.fetch_add(1, std::memory_order_relaxed);
        auto result = transport.publish(request, bytes, label);
        if (!result.success)
          throw std::runtime_error("DI_NATIVE_ENCRYPTED_PUBLICATION_FAILED: " + result.errorMessage);
        publishedResults.push_back(std::move(result));
        const auto& published = publishedResults.back();
        if (!published.encrypted || published.encryptedDataName.empty() ||
            published.encryptedDataName.get(-1).isSegment() || published.objectId.empty() ||
            published.plaintextSize != bytes.size() ||
            published.contentDigest != nativePlanningDigest(bytes.data(), bytes.size()) ||
            !digest(published.manifestDigest) || published.authorizationScope != "/SERVICE" + service ||
            published.protectionEpoch.empty())
          throw std::runtime_error("DI_NATIVE_ENCRYPTED_PUBLICATION_INVALID");
        const auto name = published.encryptedDataName.toUri();
        active();
        return name;
      };
      NativePreparedCanonicalPublication result;
      result.sourceDataName = publish(source->modelBytes, "di-canonical-source");
      NativeJson metadata{{"canonicalSourceBytes", model.canonicalSourceBytes},
        {"canonicalSourceDataName", result.sourceDataName},
        {"canonicalSourceDigest", model.canonicalSourceDigest}};
      if (source->initializerBytes) {
        result.initializerDataName = publish(*source->initializerBytes, "di-canonical-initializer");
        metadata["canonicalInitializerDataName"] = result.initializerDataName;
        metadata["canonicalInitializerBytes"] = model.canonicalInitializerBytes;
        metadata["canonicalInitializerObjectDigest"] = model.canonicalInitializerObjectDigest;
      }
      if (!options.packageManifestDigest.empty())
        metadata["packageManifestDigest"] = options.packageManifestDigest;
      NativeJson root{{"schema", "ndnsf-di-canonical-model-manifest-v1"}, {"state", "ACTIVE"},
        {"artifactProfileDigest", options.artifactProfileDigest},
        {"modelIdentityDigest", model.descriptor.contentDigest},
        {"modelName", model.descriptor.modelName}, {"metadata", metadata}};
      if (!options.layerManifestDigests.empty()) root["layerManifestDigests"] = options.layerManifestDigests;
      result.canonicalManifestJson = nativeCanonicalJson(root);
      result.manifestDigest = nativePlanningDigest(result.canonicalManifestJson);
      result.artifactProfileDigest = options.artifactProfileDigest;
      const std::vector<std::uint8_t> rootBytes(result.canonicalManifestJson.begin(),
                                                 result.canonicalManifestJson.end());
      result.rootDataName = publish(rootBytes, "di-canonical-root");
      for (const auto& published : publishedResults) {
        result.rollbackDataNames.insert(result.rollbackDataNames.end(),
                                        published.rollbackDataNames.begin(),
                                        published.rollbackDataNames.end());
        if (!published.rollbackKeyId.empty()) {
          result.rollbackKeyReferences.push_back(
            NativePublicationKeyReference{published.rollbackKeyId,
                                          published.rollbackServiceName});
        }
        if (result.rollbackKeyId.empty() && !published.rollbackKeyId.empty()) {
          result.rollbackKeyId = published.rollbackKeyId;
          result.rollbackServiceName = published.rollbackServiceName;
        }
      }
      result.validate();
      return result;
    }
    catch (...) {
      if (transport.abort && !publishedResults.empty()) {
        try { transport.abort(publishedResults); }
        catch (...) {}
      }
      throw;
    }
  };
  m_transport.post([state] {
    std::function<NativePreparedCanonicalPublication()> work;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->abandoned) return;
      work = std::move(state->work);
    }
    NativePreparedCanonicalPublication result;
    std::exception_ptr error;
    try { result = work(); }
    catch (...) { error = std::current_exception(); }
    bool abandoned = false;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      abandoned = state->abandoned;
      if (!abandoned) {
        state->result = std::move(result); state->error = error; state->done = true;
      }
    }
    if (abandoned) {
      // The worker may finish publishing just as the waiting caller marks the
      // job abandoned.  The result has no consumer in that interleaving, so
      // roll it back explicitly instead of dropping the receipt.
      if (!error && !result.rootDataName.empty())
        state->rollback(result);
      return;
    }
    state->condition.notify_one();
  });
  try {
    for (;;) {
      control.requireActive();
      std::unique_lock<std::mutex> lock(state->mutex);
      if (state->done) {
        if (state->error) std::rethrow_exception(state->error);
        return std::move(state->result);
      }
      state->condition.wait_until(lock, std::min(control.deadline,
        std::chrono::steady_clock::now() + std::chrono::milliseconds(10)));
    }
  }
  catch (...) {
    std::optional<NativePreparedCanonicalPublication> completed;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->done && !state->error && !state->result.rootDataName.empty()) {
        completed = std::move(state->result);
        state->result = {};
      }
      else if (!state->done) {
        state->abandoned = true;
        state->work = {};
        state->result = {};
      }
    }
    if (completed)
      state->rollback(*completed);
    throw;
  }
}

NativeUncachedPublication NativeCanonicalArtifactPublisher::publishUncached(
  const NativeInspectedModel& model, const NativeSplitCandidate& candidate,
  const std::vector<NativeSelectionRoleV3>& roles, const NativeRequestControl& control) const
{
  control.requireActive();
  if (m_transport.isOnIoThread())
    throw std::runtime_error("DI_NATIVE_PUBLICATION_IO_WAIT_FORBIDDEN");
  NativeRequestPreparation::validateRoles(model, candidate, roles);
  if (roles.empty() || (!m_options.packageManifestDigest.empty() &&
                       m_options.packageManifestDigest != model.modelManifestDigest))
    throw std::invalid_argument("native publication package or role contract differs from inspection");
  const auto onnx = std::find_if(roles.begin(), roles.end(), [](const auto& role) {
    return role.mergeKind != "NATIVE_POSTPROCESS";
  });
  if (onnx == roles.end())
    throw std::invalid_argument("native canonical publication requires an ONNX source role");
  std::uint64_t sourceLimit = onnx->maxSourceBytes;
  std::uint64_t assemblyLimit = onnx->maxAssembledBytes;
  for (const auto& role : roles) {
    validateNativeAssembly(role);
    const auto explicitDegree = candidate.tensorDegreesByRole.find(role.role);
    const auto degree = explicitDegree == candidate.tensorDegreesByRole.end()
      ? 1 : explicitDegree->second;
    const auto key = degree == 1 ? role.role : role.role + "#" + std::to_string(role.rank);
    if (role.selectedRole != key || role.artifactProfileDigest != roles.front().artifactProfileDigest)
      throw std::invalid_argument("native publication role alias or profile is inconsistent");
    if (role.mergeKind != "NATIVE_POSTPROCESS") {
      sourceLimit = std::min(sourceLimit, role.maxSourceBytes);
      assemblyLimit = std::min(assemblyLimit, role.maxAssembledBytes);
    }
  }
  const auto source = m_source(model, control);
  m_cache->sourceVerifications.fetch_add(1, std::memory_order_relaxed);
  control.requireActive();
  if (!source || source->modelBytes.empty() || source->modelBytes.size() != model.canonicalSourceBytes ||
      source->modelBytes.size() > sourceLimit ||
      nativePlanningDigest(source->modelBytes.data(), source->modelBytes.size()) != model.canonicalSourceDigest ||
      source->initializerBytes.has_value() != (model.canonicalInitializerBytes != 0))
    throw std::invalid_argument("native publication source bytes differ from inspection");
  if (source->initializerBytes &&
      (source->initializerBytes->size() != model.canonicalInitializerBytes ||
       source->initializerBytes->size() > sourceLimit ||
       nativePlanningDigest(source->initializerBytes->data(), source->initializerBytes->size()) !=
         model.canonicalInitializerObjectDigest))
    throw std::invalid_argument("native publication initializer bytes differ from inspection");
  const auto identity = canonicalOnnxSourceIdentity(*source,
    {control.deadline, [&control] { control.requireActive(); }, sourceLimit, assemblyLimit});
  for (const auto& role : roles) {
    if (identity.graphDigest != role.graphDigest ||
        (role.mergeKind != "NATIVE_POSTPROCESS" && identity.initializerDigest != role.canonicalInitializerDigest))
      throw std::invalid_argument("native publication canonical graph or initializer identity differs from recipe");
  }
  control.requireActive();
  auto state = std::make_shared<PublicationJob>();
  state->rollback = [owner = *this](const auto& publication) {
    owner.abortPublishedResults(publication.publications);
  };
  // Everything used by I/O work is owned. No stack reference, Face, or Python
  // callback crosses the asynchronous boundary. A pending job can release source
  // storage on cancellation even when the Core event loop is not running.
  state->work = [transport = m_transport, service = m_serviceName, options = m_options,
                 source, model, candidate, roles, control, cache = m_cache,
                 weak = std::weak_ptr<PublicationJob>(state)] {
    const auto active = [&] {
      control.requireActive();
      const auto current = weak.lock();
      if (!current) throw std::runtime_error("DI_NATIVE_PUBLICATION_ABANDONED");
      std::lock_guard<std::mutex> lock(current->mutex);
      if (current->abandoned) throw std::runtime_error("DI_NATIVE_PUBLICATION_ABANDONED");
    };
    std::vector<ndn_service_framework::LargeDataPublishResult> publishedResults;
    publishedResults.reserve(source->initializerBytes ? 3 : 2);
    try {
      active();
      const auto request = transport.begin();
      if (request.requestId.empty() || request.serviceName.toUri() != service)
        throw std::runtime_error("DI_NATIVE_PUBLICATION_REQUEST_MISMATCH");
      const auto publish = [&](const std::vector<std::uint8_t>& bytes, const std::string& label) {
        active();
        cache->publicationCalls.fetch_add(1, std::memory_order_relaxed);
        auto result = transport.publish(request, bytes, label);
        // Capture the result before the second cancellation check.  If the
        // owner cancels in that exact window, the returned object still has a
        // complete rollback record.
        publishedResults.push_back(std::move(result));
        const auto& published = publishedResults.back();
        active();
        if (!published.success)
          throw std::runtime_error("DI_NATIVE_ENCRYPTED_PUBLICATION_FAILED: " + published.errorMessage);
        if (!published.encrypted || published.encryptedDataName.empty() ||
            published.encryptedDataName.get(-1).isSegment() || published.objectId.empty() ||
            published.plaintextSize != bytes.size() ||
            published.contentDigest != nativePlanningDigest(bytes.data(), bytes.size()) ||
            !digest(published.manifestDigest) || published.authorizationScope != "/SERVICE" + service ||
            published.protectionEpoch.empty())
          throw std::runtime_error("DI_NATIVE_ENCRYPTED_PUBLICATION_INVALID");
        return published.encryptedDataName.toUri();
      };
    const auto sourceName = publish(source->modelBytes, "di-canonical-source");
    NativeJson metadata{{"canonicalSourceBytes", model.canonicalSourceBytes},
      {"canonicalSourceDataName", sourceName}, {"canonicalSourceDigest", model.canonicalSourceDigest}};
    if (source->initializerBytes) {
      metadata["canonicalInitializerDataName"] = publish(*source->initializerBytes, "di-canonical-initializer");
      metadata["canonicalInitializerBytes"] = model.canonicalInitializerBytes;
      metadata["canonicalInitializerObjectDigest"] = model.canonicalInitializerObjectDigest;
    }
    if (!options.packageManifestDigest.empty()) metadata["packageManifestDigest"] = options.packageManifestDigest;
    NativeJson root{{"schema", "ndnsf-di-canonical-model-manifest-v1"}, {"state", "ACTIVE"},
      {"artifactProfileDigest", roles.front().artifactProfileDigest},
      {"modelIdentityDigest", model.descriptor.contentDigest}, {"modelName", model.descriptor.modelName},
      {"metadata", metadata}};
    if (!options.layerManifestDigests.empty()) root["layerManifestDigests"] = options.layerManifestDigests;
    NativeArtifactBinding binding;
    binding.canonicalManifestJson = nativeCanonicalJson(root);
    binding.manifestDigest = nativePlanningDigest(binding.canonicalManifestJson);
    binding.recipeDigest = roles.front().recipeDigest;
    const std::vector<std::uint8_t> rootBytes(binding.canonicalManifestJson.begin(), binding.canonicalManifestJson.end());
    const auto rootName = publish(rootBytes, "di-canonical-root");
    for (const auto& role : roles) {
      // The canonical root contains request-scoped source publication names.
      // Include its digest in the stable assignment identity so a later
      // request cannot reuse a Provider's cached artifact under the same
      // candidate/role name while referring to a different root.
      auto stable = ndn::Name(options.artifactRoot).append(candidate.candidateDigest.substr(7));
      stable.append("manifest").append(binding.manifestDigest.substr(7));
      stable.append(ndn::Name(role.role));
      const auto degree = candidate.tensorDegreesByRole.find(role.role);
      if (degree != candidate.tensorDegreesByRole.end() && degree->second > 1)
        stable.append("rank").appendNumber(role.rank);
      binding.artifactNameByRole.emplace(role.selectedRole, stable.toUri());
      binding.sourceByRole.emplace(role.selectedRole, rootName);
      binding.artifactDigestByRole.emplace(role.selectedRole, role.artifactDigest);
    }
    NativeRequestPreparation::bindPublishedRoles(model, candidate, roles, binding);
      active();
      return NativeUncachedPublication{std::move(binding), std::move(publishedResults)};
    }
    catch (...) {
      if (transport.abort && !publishedResults.empty()) {
        try { transport.abort(publishedResults); }
        catch (...) {}
      }
      throw;
    }
  };
  m_transport.post([state] {
    std::function<NativeUncachedPublication()> work;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->abandoned) return;
      work = std::move(state->work);
    }
    NativeUncachedPublication result;
    std::exception_ptr error;
    try { result = work(); } catch (...) { error = std::current_exception(); }
    bool abandoned = false;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->abandoned) {
        abandoned = true;
      }
      else {
        state->result = std::move(result); state->error = error; state->done = true;
      }
    }
    if (abandoned) {
      if (!error)
        state->rollback(result);
      return;
    }
    state->condition.notify_one();
  });
  try {
    for (;;) {
      control.requireActive();
      std::unique_lock<std::mutex> lock(state->mutex);
      if (state->done) {
        if (state->error) std::rethrow_exception(state->error);
        return std::move(state->result);
      }
      state->condition.wait_until(lock, std::min(control.deadline,
        std::chrono::steady_clock::now() + std::chrono::milliseconds(10)));
    }
  }
  catch (...) {
    std::optional<NativeUncachedPublication> completed;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->done && !state->error && !state->result.publications.empty()) {
        completed = std::move(state->result);
        state->result = {};
      }
      else if (!state->done) {
        state->abandoned = true; state->work = {}; state->result = {};
      }
    }
    if (completed)
      state->rollback(*completed);
    throw;
  }
}
}
