#ifndef NDNSF_DI_PREPARED_MODEL_HPP
#define NDNSF_DI_PREPARED_MODEL_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModelTypes.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ConversationTypes.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationContinuation.hpp"
#include "ndn-service-framework/OperationRuntime.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ndnsf::di {

using Subscription = ndn_service_framework::OperationSubscription;

struct PreparedModelPackage;
class NativeInferenceClient;
class CooperativePlacementStrategy;
struct NativeApplicationInput;
struct NativeRequestOptions;
class DiError;
class Conversation;
struct ConversationOptions;
namespace detail { struct RuntimeState; }
// Test-only seam used by the in-process Provider fixture.  Production callers
// obtain this binding exclusively through Runtime::prepare; the friend keeps
// the public header free of transport-specific construction APIs.
struct Spec185PreparedModelTestAccess;

/** An owning protected repository reference.  Construction validates only
 * canonical syntax and required metadata; authorization remains native. */
class DataRef
{
public:
  DataRef() = default;
  static DataRef fromPublishedMetadata(const std::string& canonicalReferenceJson);
  const std::string& canonicalMetadata() const noexcept { return m_canonical; }

private:
  explicit DataRef(std::string canonical)
    : m_canonical(std::move(canonical))
  {
  }
  std::string m_canonical;
  friend class Input;
  friend class PreparedModel;
};

/** Owning application input.  No caller buffer is borrowed by a request. */
class Input
{
public:
  static Input inlineBytes(std::vector<std::uint8_t> payload,
                           std::vector<std::uint8_t> applicationOptions = {});
  static Input text(std::string utf8);
  static Input repository(DataRef reference);

private:
  enum class Kind { InlineBytes, Text, Repository };
  Kind m_kind = Kind::InlineBytes;
  std::vector<std::uint8_t> m_payload;
  std::vector<std::uint8_t> m_applicationOptions;
  std::string m_text;
  DataRef m_reference;

  friend class PreparedModel;
};

class PlacementStrategy
{
public:
  PlacementStrategy() = delete;

private:
  explicit PlacementStrategy(std::shared_ptr<const CooperativePlacementStrategy> strategy,
                             std::shared_ptr<void> runtimeBinding = {})
    : m_strategy(std::move(strategy)), m_runtimeBinding(std::move(runtimeBinding))
  {
  }
  std::shared_ptr<const CooperativePlacementStrategy> m_strategy;
  std::shared_ptr<void> m_runtimeBinding;
  friend class Runtime;
  friend class PreparedModel;
};

struct GenerationOptions
{
  std::size_t maxNewTokens = 32;
};

struct StreamOptions
{
  bool enabled = true;
  bool allowReplacement = false;
  std::uint8_t maxReplacements = 0;
};

/** Request options projected from the verified model package. */
struct RequestOptions
{
  std::chrono::milliseconds timeout{30'000};
  std::chrono::milliseconds ackTimeout{5'000};
  std::shared_ptr<const PlacementStrategy> placement;
  /** Optional absolute Provider identities to pin native selection. */
  std::vector<std::string> providerNames;
  std::string applicationRequestId;
  std::string outputMode = "FULL";
  std::optional<GenerationOptions> generation;
  std::optional<StreamOptions> stream;
};

struct Result
{
  std::vector<std::uint8_t> payload;
  std::string requestId;
  std::string modelDigest;
  std::string planDigest;

  /** Validate a native float32 tensor oracle without exposing codec internals. */
  bool matchesFloat32Tensor(const std::string& tensorName,
                            const std::vector<float>& expected,
                            double tolerance) const;
};

enum class RequestStatus { Pending, Succeeded, Failed, Cancelled };

struct Event
{
  std::string requestId;
  std::vector<std::uint8_t> payload;
  bool terminal = false;
  std::uint64_t sequence = 0;
};

using StreamEvent = Event;

struct RequestDiagnostics
{
  std::uint64_t observationDropped = 0;
};

/** Reliable single-cursor reader over the request's bounded event history. */
class EventReader
{
public:
  EventReader() noexcept = default;
  EventReader(EventReader&&) noexcept = default;
  EventReader& operator=(EventReader&&) noexcept = default;
  EventReader(const EventReader&) = delete;
  EventReader& operator=(const EventReader&) = delete;
  ~EventReader() noexcept;

  /** Read the next reliable stream event, using a local millisecond timeout. */
  std::optional<Event> next(std::chrono::milliseconds timeout);
  /** Register one error-first read callback; the token cancels only this read. */
  Subscription nextAsync(std::chrono::milliseconds timeout,
                          std::function<void(std::exception_ptr, std::optional<Event>)> callback);
  /** Close this reader without cancelling its request. */
  void close() noexcept;

private:
  struct State;
  explicit EventReader(std::shared_ptr<State> state)
    : m_state(std::move(state))
  {
  }
  std::shared_ptr<State> m_state;
  friend class RequestHandle;
  friend class PreparedModel;
};

/** Copyable handle for one native request operation. */
class RequestHandle
{
public:
  RequestHandle() = default;
  std::string id() const;
  RequestStatus status() const;
  Result result() const;
  Result result(std::chrono::milliseconds timeout) const;
  Result wait() const { return result(); }
  Result wait(std::chrono::milliseconds timeout) const { return result(timeout); }
  /** Open the single reliable reader; non-stream requests reject this call. */
  EventReader events() const;
  /** Return a copy of best-effort observation drop diagnostics. */
  RequestDiagnostics diagnostics() const;
  /** Register a reliable one-shot completion callback. */
  Subscription onCompletion(
    std::function<void(std::exception_ptr, std::optional<Result>)> callback) const;
  /** Register a bounded local result wait without cancelling the request. */
  Subscription resultAsync(
    std::chrono::milliseconds timeout,
    std::function<void(std::exception_ptr, std::optional<Result>)> callback) const;
  /** Register a best-effort diagnostic observer. */
  Subscription observe(std::function<void(const Event&)> callback) const;
  void cancel() const;

private:
  struct State;
  explicit RequestHandle(std::shared_ptr<State> state)
    : m_state(std::move(state))
  {
  }
  std::shared_ptr<State> m_state;
  friend class PreparedModel;
};

/** Receipt for one caller's preparation operation. */
struct PreparationReceipt
{
  enum class Origin { CacheHit, JoinedInFlight, Fetched, Refreshed };
  Origin origin = Origin::Fetched;
  std::string preparationKeyDigest;
  std::string manifestDigest;
  std::chrono::milliseconds elapsed{0};
};

/** Public immutable model view bound to one verified Runtime package. */
class PreparedModel
{
public:
  PreparedModel() = delete;

  const ModelManifest& manifest() const noexcept;
  const PreparationReceipt& receipt() const noexcept;
  ModelCapabilities capabilities() const;

  /** Submit a non-blocking request through the package's native client. */
  RequestHandle request(const Input& input, const RequestOptions& options = {}) const;

  /** Submit and wait using the request's configured deadline. */
  Result run(const Input& input, const RequestOptions& options = {}) const;

  /** Open one model-bound native conversation using the Runtime coordinator. */
  Conversation openConversation(const ConversationOptions& options = {}) const;

private:
  using ClientFactory = std::function<std::shared_ptr<NativeInferenceClient>(
    const std::shared_ptr<const PreparedModelPackage>&)>;
  PreparedModel(std::shared_ptr<const PreparedModelPackage> package,
                PreparationReceipt receipt, std::shared_ptr<void> lease = {},
                ClientFactory clientFactory = {});

  NativeApplicationInput encodeInput(const Input& input) const;
  std::vector<std::int64_t> conversationInputTokens(const Input& input) const;
  NativeRequestOptions projectOptions(const RequestOptions& options) const;
  RequestHandle requestInternal(Input input, const RequestOptions& options) const;
  RequestHandle requestInternal(Input input, const RequestOptions& options,
                                std::optional<NativeConversationContinuation> continuation) const;

  std::shared_ptr<const PreparedModelPackage> m_package;
  PreparationReceipt m_receipt;
  std::shared_ptr<void> m_lease;
  ClientFactory m_clientFactory;
  friend class ModelPreparationCache;
  friend class Conversation;
  friend struct Spec185PreparedModelTestAccess;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_PREPARED_MODEL_HPP
