#ifndef NDNSF_DI_NATIVE_INFERENCE_PROVIDER_HPP
#define NDNSF_DI_NATIVE_INFERENCE_PROVIDER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ndnsf::di {

struct NativeServiceDefinition
{
  std::string serviceName;
  std::vector<std::string> allowedRoles;
};

class NativeServiceRegistration
{
public:
  NativeServiceRegistration() = default;
  void close() noexcept;
  bool closed() const noexcept;

private:
  struct State { mutable std::mutex mutex; bool closed = false; };
  explicit NativeServiceRegistration(std::shared_ptr<State> state);
  std::shared_ptr<State> m_state;
  friend class NativeInferenceProvider;
};

class NativeInferenceProvider
{
public:
  NativeInferenceProvider(
    std::shared_ptr<ndn_service_framework::ServiceProvider> provider,
    std::shared_ptr<const NativeAdapterRegistry> adapters);
  ~NativeInferenceProvider() noexcept;

  NativeServiceRegistration serve(const NativeServiceDefinition& service,
                                  const NativeProviderHandlerConfig& config);
  void stop() noexcept;

private:
  std::shared_ptr<ndn_service_framework::ServiceProvider> m_provider;
  std::shared_ptr<const NativeAdapterRegistry> m_adapters;
  std::mutex m_mutex;
  std::vector<NativeServiceRegistration> m_registrations;
  bool m_stopped = false;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_INFERENCE_PROVIDER_HPP
