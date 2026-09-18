#ifndef NDNSF_DI_RUNTIME_HPP
#define NDNSF_DI_RUNTIME_HPP

#include "ndn-service-framework/OperationRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModel.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalArtifactPublisher.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Provider.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"

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

class RepositorySourceProvider;
class RepositoryArtifactPublisher;

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
  /**
   * Optional native repository source owner.  When present, Runtime::prepare
   * calls this owner instead of the legacy local-file loader.  The configured
   * native owner must perform manifest lookup, digest/size verification and
   * one idempotent ingest-on-miss through its RepoCore/RepoClient boundary,
   * then return only verified source buffers. Runtime rechecks those bytes
   * against the frozen catalog; the callback is a trusted ownership boundary
   * and its Repo operation is covered by the owner-specific C++ test.
   * Lifecycle failures must use the typed repository/DI error surface rather
   * than encoding state in arbitrary backend text. It must retain no request
   * or grant state.
   */
  using RepositorySourceLoader = std::function<NativeCanonicalSource(
    const std::string& modelKey,
    const std::string& catalogConfigurationJson,
    std::uint64_t maxSourceBytes,
    std::chrono::steady_clock::time_point deadline)>;

  std::string nativeConfigPath;
  std::vector<ModelRegistration> models;
  std::size_t maxPreparedBytes = 536870912;
  std::size_t maxPreparedEntries = 8;
  Milliseconds preparationJobTimeout{300000};
  /** Compatibility seam retained for existing embedders during migration. */
  RepositorySourceLoader repositorySourceLoader;
  /** Preferred production Repo owner; appended to preserve aggregate order. */
  std::shared_ptr<const RepositorySourceProvider> repositorySourceProvider;
  /** Optional prepare-time durable artifact owner. A request never calls this
   * boundary; it receives only the committed reference. */
  std::shared_ptr<const RepositoryArtifactPublisher> repositoryArtifactPublisher;
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

/** Typed failure raised by a RepositorySourceLoader at the preparation
 * boundary.  Lifecycle meaning must not be inferred from repository text. */
class RepositorySourceError : public DiError
{
public:
  enum class Kind { Unavailable, Timeout, Cancelled, Closed };

  RepositorySourceError(Kind kind, std::string message);
};

/** Immutable request context passed to a native repository source owner. */
struct RepositorySourceRequest
{
  std::string modelKey;
  std::string catalogConfigurationJson;
  std::uint64_t maxSourceBytes = 0;
  std::chrono::steady_clock::time_point deadline{};
};

/** Production source-owner boundary for Runtime preparation. */
class RepositorySourceProvider
{
public:
  using Fallback = std::function<NativeCanonicalSource(const RepositorySourceRequest&)>;

  virtual ~RepositorySourceProvider() = default;
  virtual NativeCanonicalSource load(const RepositorySourceRequest& request,
                                     const Fallback& fallback) const = 0;
};

/** Durable publication owner for Runtime::prepare. Loading verified source
 * bytes and committing a reusable artifact receipt are separate contracts. */
class RepositoryArtifactPublisher
{
public:
  virtual ~RepositoryArtifactPublisher() = default;

  virtual NativePreparedCanonicalPublication publish(
    const std::string& modelKey,
    const std::string& serviceName,
    const NativeInspectedModel& model,
    const NativeCanonicalSource& source,
    const NativeCanonicalPublicationOptions& options,
    const NativeRequestControl& control) const = 0;

  virtual void rollback(const NativePreparedCanonicalPublication& publication) const noexcept = 0;
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

  /** Open a Provider-only runtime without constructing a User directory. */
  static std::shared_ptr<Runtime> open(const ProviderConfig& config);

  /** Return a User bound to this Runtime's configured principal. */
  User user(UserConfig config = {});

  /** Return an opaque strategy registered by this Runtime's frozen registry. */
  std::shared_ptr<const PlacementStrategy> placementStrategy(const std::string& id) const;

  /** Return the configured Provider-only façade. */
  Provider provider(const ProviderConfig& config);
  Provider provider();

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
