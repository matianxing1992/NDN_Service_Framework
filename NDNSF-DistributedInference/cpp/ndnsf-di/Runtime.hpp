#ifndef NDNSF_DI_RUNTIME_HPP
#define NDNSF_DI_RUNTIME_HPP

#include "ndn-service-framework/OperationRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModel.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <functional>
#include <vector>

namespace ndnsf::di {

using Milliseconds = std::chrono::milliseconds;
using Subscription = ndn_service_framework::OperationSubscription;

/** A named operator configuration frozen by Runtime::open. */
struct ModelRegistration
{
  std::string key;
  std::string nativeConfigPath;
};

/** Runtime-owned native requester configuration and preparation budgets. */
struct RuntimeConfig
{
  std::string nativeConfigPath;
  std::vector<ModelRegistration> models;
  std::size_t maxPreparedBytes = 536870912;
  std::size_t maxPreparedEntries = 8;
  Milliseconds preparationJobTimeout{300000};
};

/** v1 accepts the empty profile or the explicit default profile only. */
struct UserConfig
{
  std::string profileName;
};

/** Cache selection for a verified model preparation. */
enum class CachePolicy { RequireReady, UseOrWait, UseOrFetch, Refresh };

/** Per-preparation timeout and cache policy. */
struct PrepareOptions
{
  CachePolicy cache = CachePolicy::UseOrFetch;
  Milliseconds timeout{300000};
};

/** Structured error exposed by the application API. */
class DiError : public std::runtime_error
{
public:
  DiError(std::string code, std::string domain, std::string boundary,
          std::string message, std::string requestId = {},
          std::uint64_t attempt = 0);

  const std::string& code() const noexcept { return m_code; }
  const std::string& domain() const noexcept { return m_domain; }
  const std::string& boundary() const noexcept { return m_boundary; }
  const std::string& requestId() const noexcept { return m_requestId; }
  std::uint64_t attempt() const noexcept { return m_attempt; }

private:
  std::string m_code;
  std::string m_domain;
  std::string m_boundary;
  std::string m_requestId;
  std::uint64_t m_attempt = 0;
};

namespace detail { struct RuntimeState; struct RuntimeTestAccess; }

class Runtime;
enum class PreparationStatus { Pending, Ready, Failed, Cancelled };
using PreparationCompletion = std::function<void(
  std::exception_ptr, std::optional<PreparedModel>)>;

class PreparationHandle
{
public:
  PreparationHandle() noexcept = default;
  PreparationHandle(PreparationHandle&&) noexcept = default;
  PreparationHandle& operator=(PreparationHandle&&) noexcept = default;
  PreparationHandle(const PreparationHandle&) = default;
  PreparationHandle& operator=(const PreparationHandle&) = default;
  ~PreparationHandle() = default;

  PreparationStatus status() const;
  PreparedModel result() const;
  PreparedModel result(Milliseconds timeout) const;
  Subscription resultAsync(Milliseconds timeout, PreparationCompletion callback) const;
  Subscription onCompletion(PreparationCompletion callback) const;
  void cancel() const noexcept;

private:
  struct State
  {
    std::function<PreparationStatus()> status;
    std::function<PreparedModel(Milliseconds)> result;
    std::function<Subscription(Milliseconds, PreparationCompletion)> resultAsync;
    std::function<Subscription(PreparationCompletion)> onCompletion;
    std::function<void()> cancel;
    std::shared_ptr<void> lifetime;
    Milliseconds defaultTimeout{0};
    std::function<PreparedModel()> resultDefault;
    std::chrono::steady_clock::time_point waiterDeadline{};
    bool joinedInFlight = false;
    // Bound by Runtime to reject a blocking result wait on its Core worker.
    std::function<bool()> wouldBlock;
    std::function<bool()> workerThread;
    std::function<void()> onDestroy;

    ~State() noexcept
    {
      if (onDestroy) {
        try { onDestroy(); }
        catch (...) {}
      }
    }
  };
  explicit PreparationHandle(std::shared_ptr<State> state)
    : m_state(std::move(state))
  {
  }

  std::shared_ptr<State> m_state;
  friend class User;
  friend class ModelPreparationCache;
  friend struct Spec185PreparationTestAccess;
};

/**
 * A copyable user view bound to one Runtime state and principal.
 *
 * T001 only exposes the configuration-bound view. Preparation and request
 * methods are added by their owning batches; this object never accepts a
 * caller-supplied principal or grant key.
 */
class User
{
public:
  User() = default;

  /** Prepare the registered model and return an immutable verified package. */
  PreparedModel prepare(const std::string& modelKey = "default",
                        const PrepareOptions& options = {}) const;

  /** Start native preparation; each handle is an independently cancellable waiter. */
  PreparationHandle prepareAsync(const std::string& modelKey = "default",
                                  const PrepareOptions& options = {}) const;

private:
  explicit User(std::shared_ptr<detail::RuntimeState> state,
                std::string profileName);

  std::shared_ptr<detail::RuntimeState> m_state;
  std::string m_profileName;
  friend class Runtime;
};

/**
 * Configuration and ownership boundary for the prepared-model runtime.
 *
 * `open` parses and freezes operator metadata and trust-domain identity. It
 * does not read model source/catalog bytes or contact remote services. The
 * returned User is bound to the same immutable Runtime state.
 */
class Runtime
{
public:
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Runtime(Runtime&&) = delete;
  Runtime& operator=(Runtime&&) = delete;

  /**
   * Open a runtime from an operator-pinned native requester-v1 file.
   *
   * The path is copied and resolved before returning. Invalid schema, trust,
   * identity, or resource limits throw DiError synchronously. No authority
   * signing key is loaded by this method.
   */
  static std::shared_ptr<Runtime> open(RuntimeConfig config);

  /** Return a User bound to this Runtime's configured principal. */
  User user(UserConfig config = {});

  /** Return an opaque strategy registered by this Runtime's frozen registry. */
  std::shared_ptr<const PlacementStrategy> placementStrategy(const std::string& id) const;

  /**
   * Close this runtime exactly once.  New operations are rejected; work
   * already registered with the Core owner is allowed to settle.  This method
   * does not block and is safe from a Core callback.
   */
  void close() noexcept;

  /**
   * Close and wait for the local Core owner to drain.  A negative timeout or
   * a call from the owner worker raises DiError; timeout returns false and may
   * be retried from another thread.
   */
  bool drain(Milliseconds timeout) const;

  /**
   * Register a non-blocking cleanup notification on the same Core barrier.
   * This method does not close the Runtime; call close() first when shutdown
   * is intended.  A notification registered while Open waits for current
   * local work to become quiescent and leaves the Runtime Open.
   * The move-only returned token only controls this notification and never
   * cancels business work.
   */
  Subscription drainAsync(Milliseconds timeout,
                           std::function<void(std::exception_ptr, bool)> callback) const;

  ~Runtime() noexcept;

private:
  explicit Runtime(std::shared_ptr<detail::RuntimeState> state);

  std::shared_ptr<detail::RuntimeState> m_state;
  friend struct detail::RuntimeTestAccess;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_RUNTIME_HPP
