#ifndef NDNSF_DI_RUNTIME_HPP
#define NDNSF_DI_RUNTIME_HPP

#include "ndn-service-framework/OperationRuntime.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
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

namespace detail { struct RuntimeState; }

class Runtime;

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
};

} // namespace ndnsf::di

#endif // NDNSF_DI_RUNTIME_HPP
