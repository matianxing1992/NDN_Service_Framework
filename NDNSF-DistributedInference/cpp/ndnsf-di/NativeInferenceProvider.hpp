#ifndef NDNSF_DI_NATIVE_INFERENCE_PROVIDER_HPP
#define NDNSF_DI_NATIVE_INFERENCE_PROVIDER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionLeaseService.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ndnsf::di {

// Internal host singleton state. Its definition lives in
// NativeInferenceProvider.cpp; it is opaque to callers and reached only
// through NativeInferenceProvider members.
class HostState;

// Native service definition consumed by NativeInferenceProvider::serve.
// serviceName/allowedRoles come from a verified native plan; ackHandler
// preserves the caller's existing offer/provisioning/readiness ACK decision
// (empty means the host accepts by default).
struct NativeServiceDefinition
{
  std::string serviceName;
  std::vector<std::string> allowedRoles;
  // Optional per-service ACK strategy. Empty means the host accepts the
  // collaboration ACK by default; the Core admission/permission gates stay
  // authoritative either way.
  ndn_service_framework::ServiceProvider::AckStrategyHandler ackHandler;
  // Optional observation seam invoked once after the native runtime is
  // assembled and before its collaboration registration is installed. The
  // caller may capture runtime capacity/evidence snapshots for readiness or
  // telemetry; it must not mutate the runtime or replace its handler.
  std::function<void(const NativeProviderCollaborationRuntime&)> runtimeObserver;
};

// Move-only per-service registration handle. close() is idempotent: it first
// raises the target's draining fence (the fixed lease entry rejects new
// Prepare/Commit/Renew while Abort/Release keep routing), then closes the
// Core scoped registration (new ACKs/Selections/queued executions become
// invalid immediately; the entry is retired by Core on the serialized Face
// path). Closing does not destroy a running handler: in-flight roles finish
// through their existing cancellation/deadline/cleanup paths and the shared
// Provider/Face/lease table stays available to the host's other services.
// The destructor closes the registration as well (idempotent, noexcept).
class NativeServiceRegistration
{
public:
  NativeServiceRegistration() noexcept = default;
  NativeServiceRegistration(NativeServiceRegistration&& other) noexcept = default;
  NativeServiceRegistration& operator=(NativeServiceRegistration&& other) noexcept =
    default;
  NativeServiceRegistration(const NativeServiceRegistration&) = delete;
  NativeServiceRegistration& operator=(const NativeServiceRegistration&) = delete;
  ~NativeServiceRegistration() noexcept;

  void close() noexcept;
  bool closed() const noexcept;
  bool valid() const noexcept;
  std::uint64_t generation() const noexcept;
  const std::string& serviceName() const noexcept;

private:
  struct State;
  explicit NativeServiceRegistration(std::shared_ptr<State> state);
  std::shared_ptr<State> m_state;
  friend class NativeInferenceProvider;
};

// Least-authority shared native Provider host (CD-014). The first serve()
// creates the host-wide shared lease state and registers the single fixed
// lease entry; every later serve() assembles that target's runtime through
// NativeProviderHandlerConfig, routes it to an ExecutionLeaseService over the
// shared state, and installs the scoped collaboration registration. serve()
// The config's localProviderName must be the exact identity returned by the
// underlying ServiceProvider, and providerBootId must be non-empty; serve()
// rejects an unbound identity before creating host-wide lease state. This
// prevents a small in-process fixture from masking cross-host offer,
// evidence, data-prefix, or lease binding errors.
// serve() may only be called on the Face event thread or before the event loop
// starts, the same constraint as the Core scoped registration APIs; close()
// and stop() may be triggered from any thread. The host never closes the
// shared ServiceProvider/Face and never joins an I/O thread.
class NativeInferenceProvider
{
public:
  NativeInferenceProvider(
    std::shared_ptr<ndn_service_framework::ServiceProvider> provider,
    std::shared_ptr<const NativeAdapterRegistry> adapters);
  ~NativeInferenceProvider() noexcept;
  NativeInferenceProvider(const NativeInferenceProvider&) = delete;
  NativeInferenceProvider& operator=(const NativeInferenceProvider&) = delete;
  NativeInferenceProvider(NativeInferenceProvider&&) = delete;
  NativeInferenceProvider& operator=(NativeInferenceProvider&&) = delete;

  NativeServiceRegistration serve(const NativeServiceDefinition& service,
                                  const NativeProviderHandlerConfig& config);
  void stop() noexcept;

private:
  std::shared_ptr<ndn_service_framework::ServiceProvider> m_provider;
  std::shared_ptr<const NativeAdapterRegistry> m_adapters;
  std::mutex m_mutex;
  bool m_stopped = false;
  std::shared_ptr<HostState> m_host;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_INFERENCE_PROVIDER_HPP
