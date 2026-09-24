#ifndef NDNSF_DI_PROVIDER_PUBLIC_HPP
#define NDNSF_DI_PROVIDER_PUBLIC_HPP

#include "ndn-service-framework/OperationRuntime.hpp"

#include <ndn-cxx/security/key-chain.hpp>

#if defined(NDNSF_DI_PROVIDER_TEST_SEAM)
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#endif

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ndn {
class Face;
namespace security { class Certificate; }
}

namespace ndn_service_framework { class ServiceProvider; }

namespace ndnsf::di {

class ProviderConfig;
namespace detail {
ProviderConfig parseProviderLaunchFile(const std::filesystem::path& path);
}

using Milliseconds = std::chrono::milliseconds;
using Subscription = ndn_service_framework::OperationSubscription;

/** Immutable C++ launch configuration for a Provider-only runtime. */
class ProviderConfig
{
public:
  ProviderConfig() noexcept = default;

  /** Parse and validate the canonical provider launch file. */
  static ProviderConfig fromFile(const std::filesystem::path& path);

  /** Parse the provider CLI grammar without shell expansion. */
  static ProviderConfig fromCommandLine(int argc, const char* const* argv);

  bool valid() const noexcept;

  /** Compare two validated launch configurations for Runtime rebinding. */
  bool equivalent(const ProviderConfig& other) const noexcept;

private:
  struct Impl;
  explicit ProviderConfig(std::shared_ptr<const Impl> impl)
    : m_impl(std::move(impl))
  {
  }

  std::shared_ptr<const Impl> m_impl;
  friend ProviderConfig detail::parseProviderLaunchFile(
    const std::filesystem::path& path);
  friend class Provider;
  friend class Runtime;
};

/** Public service declaration; the Provider owns the native handler. */
struct ServiceDefinition
{
  std::string serviceName;
  std::vector<std::string> allowedRoles;
};

/** Metadata-only Provider assembly observations; no payload bytes are exposed. */
struct ProviderCounters
{
  std::uint64_t sourceFetches = 0;
  std::uint64_t assemblies = 0;
  std::uint64_t templateHits = 0;
  std::uint64_t runnersCreated = 0;
  std::uint64_t activeLeases = 0;
};

class ProviderRegistration
{
public:
  ProviderRegistration() noexcept = default;
  ProviderRegistration(ProviderRegistration&&) noexcept = default;
  ProviderRegistration& operator=(ProviderRegistration&&) noexcept = default;
  ProviderRegistration(const ProviderRegistration&) = delete;
  ProviderRegistration& operator=(const ProviderRegistration&) = delete;
  ~ProviderRegistration() noexcept;

  void close() noexcept;
  bool closed() const noexcept;
  bool valid() const noexcept;
  const std::string& serviceName() const noexcept;

private:
  struct State;
  explicit ProviderRegistration(std::shared_ptr<State> state)
    : m_state(std::move(state))
  {
  }

  std::shared_ptr<State> m_state;
  friend class Provider;
};

/**
 * C++-first Provider façade.  It owns the native provider host and never
 * creates a User/requester directory.  All Selection authentication,
 * post-Selection assembly, and Core registration remain in the native host.
 */
class Provider
{
public:
  Provider() noexcept = default;
  Provider(const Provider&) = default;
  Provider& operator=(const Provider&) = default;
  Provider(Provider&&) noexcept = default;
  Provider& operator=(Provider&&) noexcept = default;
  ~Provider() noexcept;

  ProviderRegistration serve(const ServiceDefinition& service);
  void stop() const noexcept;
  bool drain(Milliseconds timeout) const;
  Subscription drainAsync(
    Milliseconds timeout,
    std::function<void(std::exception_ptr, bool)> callback) const;

  bool valid() const noexcept;
  ProviderCounters counters() const noexcept;

#if defined(NDNSF_DI_PROVIDER_TEST_SEAM)
  /** Test-only owner injection; production builds do not expose this seam. */
  static Provider fromServiceProviderForTest(
    ndn::Face& face,
    ndn_service_framework::ServiceProvider& serviceProvider,
    ndn::KeyChain& keyChain,
    const ndn::security::Certificate& providerCertificate,
    const ndn::security::Certificate& controllerCertificate,
    const ProviderConfig& config,
    std::shared_ptr<NativeModelRunnerFactory> runnerFactory,
    NativeProviderHandlerConfig::RunnerPreparationFactory preparationFactory,
    NativeProviderHandlerConfig::ProtectedRuntimeFactory protectedRuntimeFactory = {},
    ndn_service_framework::ServiceProvider::AckStrategyHandler ackHandler = {},
    NativeProviderHandlerConfig::ProtectedGrantFetcher protectedGrantFetcher = {});
#endif

private:
  struct State;
  explicit Provider(std::shared_ptr<State> state)
    : m_state(std::move(state))
  {
  }

  static Provider fromConfig(const ProviderConfig& config);
  void startIo();
  void requestStopIo() const noexcept;
  bool waitForIoBarrier(Milliseconds timeout) const;
  bool stopIo() const noexcept;
  // Cancel operations on an owned Face while its IO loop is still able to
  // execute ndn-cxx's asynchronous shutdown handler. Borrowed test Faces are
  // never shut down by Provider.
  bool requestOwnedFaceShutdown() const noexcept;
  bool waitForOwnedFaceShutdown() const noexcept;
  bool finishOwnedFaceShutdownWithoutWorker() const noexcept;
  bool postIoStopMarker() const noexcept;
  bool launchReaper(std::shared_ptr<std::thread> worker, bool selfStop) const noexcept;
  void retainUnrecoveredWorker(std::thread& worker) const noexcept;
  // Release Face-bound provider owners only after the IO join fence.
  void releaseStoppedResources() const noexcept;
  std::shared_ptr<State> m_state;
  friend class Runtime;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_PROVIDER_PUBLIC_HPP
