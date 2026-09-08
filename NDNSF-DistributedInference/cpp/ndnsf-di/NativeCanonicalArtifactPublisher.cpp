#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalArtifactPublisher.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <condition_variable>
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
  std::function<NativeArtifactBinding()> work;
  NativeArtifactBinding result;
  std::exception_ptr error;
  bool done = false;
  bool abandoned = false;
};
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
      }}, serviceName, std::move(options), std::move(source))
{
  if (!user) throw std::invalid_argument("missing Core publication owner");
}

NativeCanonicalArtifactPublisher::NativeCanonicalArtifactPublisher(Transport transport,
  std::string serviceName, NativeCanonicalPublicationOptions options, SourcePort source)
  : m_transport(std::move(transport)), m_serviceName(std::move(serviceName)),
    m_options(std::move(options)), m_source(std::move(source))
{
  if (!m_source || !m_transport.post || !m_transport.isOnIoThread || !m_transport.begin ||
      !m_transport.publish || m_serviceName.empty() || m_serviceName.front() != '/' ||
      m_options.artifactRoot.empty() || m_options.artifactRoot.front() != '/' ||
      ndn::Name(m_serviceName).empty() || ndn::Name(m_options.artifactRoot).empty() ||
      (!m_options.packageManifestDigest.empty() && !digest(m_options.packageManifestDigest)) ||
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

NativeArtifactBinding NativeCanonicalArtifactPublisher::operator()(const NativeInspectedModel& model,
  const NativeSplitCandidate& candidate, const std::vector<NativeSelectionRoleV3>& roles,
  const NativeRequestControl& control) const
{
  control.requireActive();
  if (m_transport.isOnIoThread())
    throw std::runtime_error("DI_NATIVE_PUBLICATION_IO_WAIT_FORBIDDEN");
  NativeRequestPreparation::validateRoles(model, candidate, roles);
  if (roles.empty() || (!m_options.packageManifestDigest.empty() &&
                       m_options.packageManifestDigest != model.modelManifestDigest))
    throw std::invalid_argument("native publication package or role contract differs from inspection");
  std::uint64_t sourceLimit = roles.front().maxSourceBytes;
  std::uint64_t assemblyLimit = roles.front().maxAssembledBytes;
  for (const auto& role : roles) {
    validateNativeAssembly(role);
    const auto degree = candidate.tensorDegreesByRole.at(role.role);
    const auto key = degree == 1 ? role.role : role.role + "#" + std::to_string(role.rank);
    if (role.selectedRole != key || role.artifactProfileDigest != roles.front().artifactProfileDigest)
      throw std::invalid_argument("native publication role alias or profile is inconsistent");
    sourceLimit = std::min(sourceLimit, role.maxSourceBytes);
    assemblyLimit = std::min(assemblyLimit, role.maxAssembledBytes);
  }
  const auto source = m_source(model, control);
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
    if (identity.graphDigest != role.graphDigest || identity.initializerDigest != role.canonicalInitializerDigest)
      throw std::invalid_argument("native publication canonical graph or initializer identity differs from recipe");
  }
  control.requireActive();
  auto state = std::make_shared<PublicationJob>();
  // Everything used by I/O work is owned. No stack reference, Face, or Python
  // callback crosses the asynchronous boundary. A pending job can release source
  // storage on cancellation even when the Core event loop is not running.
  state->work = [transport = m_transport, service = m_serviceName, options = m_options,
                 source, model, candidate, roles, control, weak = std::weak_ptr<PublicationJob>(state)] {
    const auto active = [&] {
      control.requireActive();
      const auto current = weak.lock();
      if (!current) throw std::runtime_error("DI_NATIVE_PUBLICATION_ABANDONED");
      std::lock_guard<std::mutex> lock(current->mutex);
      if (current->abandoned) throw std::runtime_error("DI_NATIVE_PUBLICATION_ABANDONED");
    };
    active();
    const auto request = transport.begin();
    if (request.requestId.empty() || request.serviceName.toUri() != service)
      throw std::runtime_error("DI_NATIVE_PUBLICATION_REQUEST_MISMATCH");
    const auto publish = [&](const std::vector<std::uint8_t>& bytes, const std::string& label) {
      active();
      const auto result = transport.publish(request, bytes, label);
      active();
      if (!result.success)
        throw std::runtime_error("DI_NATIVE_ENCRYPTED_PUBLICATION_FAILED: " + result.errorMessage);
      if (!result.encrypted || result.encryptedDataName.empty() ||
          result.encryptedDataName.get(-1).isSegment() || result.objectId.empty() ||
          result.plaintextSize != bytes.size() ||
          result.contentDigest != nativePlanningDigest(bytes.data(), bytes.size()) ||
          !digest(result.manifestDigest) || result.authorizationScope != "/SERVICE" + service ||
          result.protectionEpoch.empty())
        throw std::runtime_error("DI_NATIVE_ENCRYPTED_PUBLICATION_INVALID");
      return result.encryptedDataName.toUri();
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
      auto stable = ndn::Name(options.artifactRoot).append(candidate.candidateDigest.substr(7));
      stable.append(ndn::Name(role.role));
      if (candidate.tensorDegreesByRole.at(role.role) > 1) stable.append("rank").appendNumber(role.rank);
      binding.artifactNameByRole.emplace(role.selectedRole, stable.toUri());
      binding.sourceByRole.emplace(role.selectedRole, rootName);
      binding.artifactDigestByRole.emplace(role.selectedRole, role.artifactDigest);
    }
    NativeRequestPreparation::bindPublishedRoles(model, candidate, roles, binding);
    active();
    return binding;
  };
  m_transport.post([state] {
    std::function<NativeArtifactBinding()> work;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->abandoned) return;
      work = std::move(state->work);
    }
    NativeArtifactBinding result;
    std::exception_ptr error;
    try { result = work(); } catch (...) { error = std::current_exception(); }
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->abandoned) return;
      state->result = std::move(result); state->error = error; state->done = true;
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
    std::lock_guard<std::mutex> lock(state->mutex);
    state->abandoned = true; state->work = {}; state->result = {};
    throw;
  }
}
}
