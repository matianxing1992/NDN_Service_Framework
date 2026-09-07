#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceProvider.hpp"

#include <ndn-cxx/name.hpp>
#include <stdexcept>

namespace ndnsf::di {

NativeServiceRegistration::NativeServiceRegistration(std::shared_ptr<State> state)
  : m_state(std::move(state))
{
}

void NativeServiceRegistration::close() noexcept
{
  if (!m_state) return;
  std::lock_guard<std::mutex> lock(m_state->mutex);
  m_state->closed = true;
}

bool NativeServiceRegistration::closed() const noexcept
{
  if (!m_state) return true;
  std::lock_guard<std::mutex> lock(m_state->mutex);
  return m_state->closed;
}

NativeInferenceProvider::NativeInferenceProvider(
  std::shared_ptr<ndn_service_framework::ServiceProvider> provider,
  std::shared_ptr<const NativeAdapterRegistry> adapters)
  : m_provider(std::move(provider)), m_adapters(std::move(adapters))
{
  if (!m_provider || !m_adapters) {
    throw std::invalid_argument("native provider requires Core provider and adapters");
  }
}

NativeInferenceProvider::~NativeInferenceProvider() noexcept
{
  stop();
}

NativeServiceRegistration NativeInferenceProvider::serve(
  const NativeServiceDefinition& service, const NativeProviderHandlerConfig& config)
{
  if (service.serviceName.empty() || service.allowedRoles.empty()) {
    throw std::invalid_argument("native service definition is incomplete");
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_stopped) throw std::runtime_error("native provider is stopped");
  for (const auto& registration : m_registrations) {
    if (!registration.closed()) throw std::invalid_argument("native service is already registered");
  }
  if (!config.runnerFactory || config.localProviderName.empty()) {
    throw std::invalid_argument("native provider configuration is incomplete");
  }
  auto runtime = makeNativeProviderCollaborationRuntime(config);
  auto state = std::make_shared<NativeServiceRegistration::State>();
  auto handler = std::move(runtime.handler);
  auto guarded = [state, handler = std::move(handler)] (
      ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
      const ndn_service_framework::RequestMessage& request) mutable {
    {
      std::lock_guard<std::mutex> stateLock(state->mutex);
      if (state->closed) {
        ctx.fail("NATIVE_SERVICE_REGISTRATION_CLOSED");
        return;
      }
    }
    handler(ctx, request);
  };
  m_provider->addCollaborationHandler(ndn::Name(service.serviceName),
                                      service.allowedRoles, std::move(guarded));
  m_registrations.emplace_back(NativeServiceRegistration(state));
  return m_registrations.back();
}

void NativeInferenceProvider::stop() noexcept
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_stopped) return;
  m_stopped = true;
  for (auto& registration : m_registrations) registration.close();
}

} // namespace ndnsf::di
