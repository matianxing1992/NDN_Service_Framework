#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModel.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModelPackage.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCatalogModelAdapter.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <ndn-cxx/name.hpp>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {

bool Result::matchesFloat32Tensor(const std::string& tensorName,
                                  const std::vector<float>& expected,
                                  double tolerance) const
{
  if (tensorName.empty() || expected.empty() || !std::isfinite(tolerance) || tolerance < 0.0)
    return false;
  try {
    const auto tensors = decodeTensorBundle(payload);
    const auto& tensor = findTensor(tensors, tensorName);
    if (tensor.elementType != TensorElementType::Float32 ||
        tensor.payload.size() != expected.size() * sizeof(float))
      return false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
      if (!std::isfinite(expected[i]))
        return false;
      float actual = 0.0F;
      std::memcpy(&actual, tensor.payload.data() + i * sizeof(float), sizeof(float));
      if (!std::isfinite(actual) ||
          std::fabs(static_cast<double>(actual) - expected[i]) > tolerance)
        return false;
    }
    return true;
  }
  catch (const std::exception&) {
    return false;
  }
}

namespace {

bool isDigest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [] (unsigned char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool isNdnName(const std::string& value)
{
  if (value.size() < 2 || value.front() != '/') return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const auto c = static_cast<unsigned char>(value[i]);
    if (c < 0x21 || c == 0x7f || (c == '/' && (i + 1 == value.size() || value[i + 1] == '/')))
      return false;
  }
  return true;
}

bool validUtf8(const std::string& text)
{
  std::size_t i = 0;
  while (i < text.size()) {
    const auto c = static_cast<unsigned char>(text[i]);
    std::size_t width = 0;
    std::uint32_t code = 0;
    if (c <= 0x7f) { width = 1; code = c; }
    else if (c >= 0xc2 && c <= 0xdf) { width = 2; code = c & 0x1f; }
    else if (c >= 0xe0 && c <= 0xef) { width = 3; code = c & 0x0f; }
    else if (c >= 0xf0 && c <= 0xf4) { width = 4; code = c & 0x07; }
    else return false;
    if (i + width > text.size()) return false;
    for (std::size_t j = 1; j < width; ++j) {
      const auto continuation = static_cast<unsigned char>(text[i + j]);
      if ((continuation & 0xc0) != 0x80) return false;
      code = (code << 6) | (continuation & 0x3f);
    }
    if ((width == 2 && code < 0x80) || (width == 3 && code < 0x800) ||
        (width == 4 && code < 0x10000) || code > 0x10ffff ||
        (code >= 0xd800 && code <= 0xdfff)) return false;
    i += width;
  }
  return true;
}

DiError mapPublicRequestError(const std::exception& error)
{
  if (const auto* native = dynamic_cast<const NativeDiError*>(&error))
    return DiError(native->code(), native->domain(), native->boundary(), native->what(),
                   native->requestId(), native->attempt());
  return DiError("REQUEST_FAILED", "runtime", "request", error.what());
}

DiError mapEventError(const ndn_service_framework::OperationError& error)
{
  switch (error.code()) {
  case ndn_service_framework::OperationErrorCode::Timeout:
    return DiError("WAIT_TIMEOUT", "local", "wait", error.what());
  case ndn_service_framework::OperationErrorCode::EventGap:
    return DiError("STREAM_GAP", "local", "events", error.what());
  case ndn_service_framework::OperationErrorCode::Capacity:
    return DiError("READ_IN_PROGRESS", "local", "events", error.what());
  case ndn_service_framework::OperationErrorCode::Closed:
    return DiError("READER_CLOSED", "local", "events", error.what());
  case ndn_service_framework::OperationErrorCode::WouldDeadlock:
    return DiError("CORE_WORKER_WAIT_FORBIDDEN", "local", "events", error.what());
  case ndn_service_framework::OperationErrorCode::Cancelled:
    return DiError("CANCELLED", "local", "events", error.what());
  }
  return DiError("REQUEST_FAILED", "runtime", "events", error.what());
}

DiError mapWaitError(const ndn_service_framework::OperationError& error)
{
  switch (error.code()) {
  case ndn_service_framework::OperationErrorCode::Timeout:
    return DiError("WAIT_TIMEOUT", "local", "wait", error.what());
  case ndn_service_framework::OperationErrorCode::WouldDeadlock:
    return DiError("WOULD_DEADLOCK", "local", "wait", error.what());
  case ndn_service_framework::OperationErrorCode::Cancelled:
    return DiError("CANCELLED", "local", "wait", error.what());
  case ndn_service_framework::OperationErrorCode::Closed:
    return DiError("RUNTIME_CLOSED", "local", "wait", error.what());
  case ndn_service_framework::OperationErrorCode::Capacity:
    return DiError("SUBSCRIPTION_LIMIT", "local", "wait", error.what());
  case ndn_service_framework::OperationErrorCode::EventGap:
    return DiError("STREAM_GAP", "local", "wait", error.what());
  }
  return DiError("REQUEST_FAILED", "runtime", "wait", error.what());
}

std::exception_ptr mapPublicCallbackError(std::exception_ptr source)
{
  if (!source)
    return {};
  try {
    std::rethrow_exception(source);
  }
  catch (const DiError&) {
    return source;
  }
  catch (const ndn_service_framework::OperationError& error) {
    return std::make_exception_ptr(mapEventError(error));
  }
  catch (const std::exception& error) {
    return std::make_exception_ptr(mapPublicRequestError(error));
  }
  catch (...) {
    return std::make_exception_ptr(
      DiError("REQUEST_FAILED", "runtime", "request", "unknown native request failure"));
  }
}

std::exception_ptr mapPublicWaitCallbackError(std::exception_ptr source)
{
  if (!source)
    return {};
  try {
    std::rethrow_exception(source);
  }
  catch (const DiError&) {
    return source;
  }
  catch (const ndn_service_framework::OperationError& error) {
    return std::make_exception_ptr(mapWaitError(error));
  }
  catch (const std::exception& error) {
    return std::make_exception_ptr(mapPublicRequestError(error));
  }
  catch (...) {
    return std::make_exception_ptr(
      DiError("REQUEST_FAILED", "runtime", "wait", "unknown native wait failure"));
  }
}

Result publicResult(const NativeInferenceResult& value, const std::string& requestId)
{
  return Result{value.payload, requestId, value.modelDigest, value.planDigest};
}

Event publicEvent(const NativeInferenceEvent& value)
{
  return Event{value.requestId, value.payload, value.terminal, value.sequence};
}

std::string verifiedGenerationDefaults(const PreparedModelPackage& package)
{
  try {
    const auto registration = nativeParseJson(package.registration->configurationJson);
    const auto& request = registration.at("request");
    if (request.value("generation_mode", std::string("TOKEN_DIAGNOSTIC")) !=
        "TOKEN_STREAMING")
      return {};
    if (!request.contains("generation_defaults") ||
        !request.at("generation_defaults").is_object())
      throw std::invalid_argument("verified TOKEN_STREAMING task has no generation defaults");
    auto defaults = request.at("generation_defaults");
    defaults["useCache"] = true;
    defaults["outputMode"] = "TOKEN_STREAMING";
    if (!defaults.contains("tokenizerDigest"))
      defaults["tokenizerDigest"] = request.value("tokenizer_digest", std::string{});
    if (!defaults.contains("maxNewTokens"))
      defaults["maxNewTokens"] = 32;
    return nativeCanonicalJson(defaults);
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw DiError("INVALID_GENERATION_OPTIONS", "local", "request",
                  std::string("verified generation defaults are unavailable: ") + error.what());
  }
}

} // namespace

DataRef DataRef::fromPublishedMetadata(const std::string& wire)
{
  if (wire.empty() || wire.size() > 1024 * 1024)
    throw DiError("INVALID_DATA_REFERENCE", "local", "input",
                  "published data reference is empty or exceeds its bound");
  try {
    const auto value = nativeParseJson(wire);
    if (!value.is_object() || !isNdnName(value.value("dataName", std::string{})) ||
        value.value("encrypted", false) != true ||
        !value.contains("plaintextSize") || !value.at("plaintextSize").is_number_unsigned() ||
        value.at("plaintextSize").get<std::uint64_t>() == 0 ||
        value.value("authorizationScope", std::string{}).empty() ||
        value.value("protectionEpoch", std::string{}).empty() ||
        value.value("protectionEpoch", std::string{}) == "plaintext-v1" ||
        !isDigest(value.value("manifestDigest", std::string{})) ||
        !isDigest(value.value("ciphertextDigest", std::string{})) ||
        nativeCanonicalJson(value) != wire)
      throw std::invalid_argument("published data reference metadata is incomplete");
    return DataRef(wire);
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw DiError("INVALID_DATA_REFERENCE", "local", "input", error.what());
  }
}

Input Input::inlineBytes(std::vector<std::uint8_t> payload,
                         std::vector<std::uint8_t> applicationOptions)
{
  Input value;
  value.m_kind = Kind::InlineBytes;
  value.m_payload = std::move(payload);
  value.m_applicationOptions = std::move(applicationOptions);
  return value;
}

Input Input::text(std::string utf8)
{
  if (!validUtf8(utf8) || utf8.empty())
    throw DiError("INVALID_INPUT", "local", "input", "Input::text requires nonempty UTF-8");
  Input value;
  value.m_kind = Kind::Text;
  value.m_text = std::move(utf8);
  return value;
}

Input Input::repository(DataRef reference)
{
  if (reference.m_canonical.empty())
    throw DiError("INVALID_DATA_REFERENCE", "local", "input", "repository reference is empty");
  Input value;
  value.m_kind = Kind::Repository;
  value.m_reference = std::move(reference);
  return value;
}

struct EventReader::State
{
  std::shared_ptr<NativeEventReader> native;
};

EventReader::~EventReader() noexcept
{
  close();
}

std::optional<Event> EventReader::next(std::chrono::milliseconds timeout)
{
  if (!m_state || !m_state->native)
    throw DiError("READER_CLOSED", "local", "events", "event reader is closed");
  if (timeout.count() < 0)
    throw DiError("INVALID_ARGUMENT", "local", "events", "event reader timeout is negative");
  try {
    const auto event = m_state->native->next(timeout);
    if (!event)
      return std::nullopt;
    return publicEvent(*event);
  }
  catch (const ndn_service_framework::OperationError& error) {
    throw mapEventError(error);
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw mapPublicRequestError(error);
  }
}

Subscription EventReader::nextAsync(
  std::chrono::milliseconds timeout,
  std::function<void(std::exception_ptr, std::optional<Event>)> callback)
{
  if (!m_state || !m_state->native)
    throw DiError("READER_CLOSED", "local", "events", "event reader is closed");
  if (!callback)
    throw DiError("INVALID_ARGUMENT", "local", "events", "event reader callback is empty");
  if (timeout.count() < 0)
    throw DiError("INVALID_ARGUMENT", "local", "events", "event reader timeout is negative");
  try {
    return m_state->native->nextAsync(timeout,
      [callback = std::move(callback)](std::optional<NativeInferenceEvent> event,
                                        std::exception_ptr error) mutable {
        std::optional<Event> publicValue;
        std::exception_ptr publicError = mapPublicCallbackError(std::move(error));
        try {
          if (event)
            publicValue = publicEvent(*event);
        }
        catch (...) {
          publicValue.reset();
          publicError = mapPublicCallbackError(std::current_exception());
        }
        callback(std::move(publicError), std::move(publicValue));
      });
  }
  catch (const ndn_service_framework::OperationError& error) {
    throw mapEventError(error);
  }
}

void EventReader::close() noexcept
{
  if (m_state && m_state->native)
    m_state->native->close();
}

struct RequestHandle::State
{
  std::function<std::string()> id;
  std::function<RequestStatus()> status;
  std::function<Result(std::chrono::milliseconds)> result;
  std::function<void()> cancel;
  std::function<EventReader()> events;
  std::function<RequestDiagnostics()> diagnostics;
  std::function<Subscription(std::function<void(std::exception_ptr,
                                                 std::optional<Result>)>)> onCompletion;
  std::function<Subscription(std::chrono::milliseconds,
                             std::function<void(std::exception_ptr,
                                                std::optional<Result>)>)> resultAsync;
  std::function<Subscription(std::function<void(const Event&)>)> observe;
  std::chrono::milliseconds defaultTimeout{30'000};
  // Keep the client owner alive for the complete request.  A factory-created
  // client owns the operation runtime; dropping it after request submission
  // would invoke close() and cancel the still-pending native operation.
  std::shared_ptr<NativeInferenceClient> client;
  // Keep the cache lease alive for the complete native operation, even when
  // the PreparedModel wrapper itself is released by the caller.
  std::shared_ptr<void> packageLease;
};

std::string RequestHandle::id() const
{
  if (!m_state || !m_state->id)
    throw DiError("INVALID_HANDLE", "local", "handle", "request handle is empty");
  return m_state->id();
}

RequestStatus RequestHandle::status() const
{
  if (!m_state || !m_state->status)
    throw DiError("INVALID_HANDLE", "local", "handle", "request handle is empty");
  return m_state->status();
}

Result RequestHandle::result() const
{
  if (!m_state || !m_state->result)
    throw DiError("INVALID_HANDLE", "local", "handle", "request handle is empty");
  return result(m_state->defaultTimeout);
}

Result RequestHandle::result(std::chrono::milliseconds timeout) const
{
  if (!m_state || !m_state->result)
    throw DiError("INVALID_HANDLE", "local", "handle", "request handle is empty");
  try {
    return m_state->result(timeout);
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw mapPublicRequestError(error);
  }
}

void RequestHandle::cancel() const
{
  if (m_state && m_state->cancel) m_state->cancel();
}

EventReader RequestHandle::events() const
{
  if (!m_state || !m_state->events)
    throw DiError("INVALID_HANDLE", "local", "events", "request handle is empty");
  try {
    return m_state->events();
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw mapPublicRequestError(error);
  }
}

RequestDiagnostics RequestHandle::diagnostics() const
{
  if (!m_state || !m_state->diagnostics)
    throw DiError("INVALID_HANDLE", "local", "diagnostics", "request handle is empty");
  try {
    return m_state->diagnostics();
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw mapPublicRequestError(error);
  }
}

Subscription RequestHandle::onCompletion(
  std::function<void(std::exception_ptr, std::optional<Result>)> callback) const
{
  if (!m_state || !m_state->onCompletion)
    throw DiError("INVALID_HANDLE", "local", "completion", "request handle is empty");
  if (!callback)
    throw DiError("INVALID_ARGUMENT", "local", "completion", "completion callback is empty");
  try {
    return m_state->onCompletion(std::move(callback));
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw mapPublicRequestError(error);
  }
}

Subscription RequestHandle::resultAsync(
  std::chrono::milliseconds timeout,
  std::function<void(std::exception_ptr, std::optional<Result>)> callback) const
{
  if (!m_state || !m_state->resultAsync)
    throw DiError("INVALID_HANDLE", "local", "completion", "request handle is empty");
  if (!callback)
    throw DiError("INVALID_ARGUMENT", "local", "completion", "result callback is empty");
  if (timeout.count() < 0)
    throw DiError("INVALID_ARGUMENT", "local", "wait", "result timeout is negative");
  try {
    return m_state->resultAsync(timeout, std::move(callback));
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw mapPublicRequestError(error);
  }
}

Subscription RequestHandle::observe(std::function<void(const Event&)> callback) const
{
  if (!m_state || !m_state->observe)
    throw DiError("INVALID_HANDLE", "local", "observer", "request handle is empty");
  if (!callback)
    throw DiError("INVALID_ARGUMENT", "local", "observer", "observer callback is empty");
  try {
    return m_state->observe(std::move(callback));
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw mapPublicRequestError(error);
  }
}

PreparedModel::PreparedModel(std::shared_ptr<const PreparedModelPackage> package,
                             PreparationReceipt receipt, std::shared_ptr<void> lease,
                             ClientFactory clientFactory)
  : m_package(std::move(package)), m_receipt(std::move(receipt)), m_lease(std::move(lease)),
    m_clientFactory(std::move(clientFactory)), m_clientState(std::make_shared<ClientState>())
{
  if (!m_package)
    throw std::invalid_argument("prepared model requires a verified package");
}

const ModelManifest& PreparedModel::manifest() const noexcept
{
  static const ModelManifest empty;
  return m_package ? m_package->manifest : empty;
}

const PreparationReceipt& PreparedModel::receipt() const noexcept
{
  return m_receipt;
}

ModelCapabilities PreparedModel::capabilities() const
{
  if (!m_package)
    throw std::runtime_error("prepared model is empty");
  return m_package->capabilities;
}

NativeApplicationInput PreparedModel::encodeInput(const Input& input) const
{
  if (!m_package)
    throw DiError("INVALID_HANDLE", "local", "request", "prepared model is empty");
  NativeApplicationInput native;
  native.taskName = m_package->manifest.taskName;
  native.inputSchemaDigest = m_package->catalog.model.descriptor.adapter.inputSchemaDigest;
  native.optionsSchemaDigest = m_package->catalog.model.descriptor.adapter.optionsSchemaDigest;
  native.options = input.m_applicationOptions;
  switch (input.m_kind) {
  case Input::Kind::InlineBytes:
    if (input.m_payload.empty())
      throw DiError("INVALID_INPUT", "local", "input", "inline input payload is empty");
    native.payload = input.m_payload;
    native.transportMode = NativeInputTransportMode::Inline;
    break;
  case Input::Kind::Text:
    if (std::find(m_package->capabilities.inputKinds.begin(),
                  m_package->capabilities.inputKinds.end(), "UTF8_TEXT") ==
        m_package->capabilities.inputKinds.end())
      throw DiError("UNSUPPORTED_CAPABILITY", "local", "input",
                    "verified adapter does not accept UTF8 text input");
    native.payload.assign(input.m_text.begin(), input.m_text.end());
    native.transportMode = NativeInputTransportMode::Inline;
    break;
  case Input::Kind::Repository:
    try {
      const auto reference = nativeParseJson(input.m_reference.m_canonical);
      const auto adapter = m_package->catalog.preparation->adapters()->find(
        m_package->catalog.model.descriptor.adapterId);
      const auto catalogAdapter = std::dynamic_pointer_cast<const NativeCatalogModelAdapter>(adapter);
      if (catalogAdapter && reference.at("plaintextSize").get<std::uint64_t>() >
          catalogAdapter->maxPayloadBytes())
        throw DiError("INVALID_DATA_REFERENCE", "local", "input",
                      "repository plaintext size exceeds the verified adapter bound");
    }
    catch (const DiError&) {
      throw;
    }
    catch (const std::exception& error) {
      throw DiError("INVALID_DATA_REFERENCE", "local", "input", error.what());
    }
    native.transportMode = NativeInputTransportMode::RepositoryReference;
    native.repositoryReference = input.m_reference.m_canonical;
    break;
  }
  return native;
}

std::vector<std::int64_t> PreparedModel::conversationInputTokens(const Input& input) const
{
  const auto native = encodeInput(input);
  if (native.transportMode != NativeInputTransportMode::Inline)
    throw DiError("UNSUPPORTED_CAPABILITY", "conversation", "input",
                  "conversation input tokenization requires inline native input");
  try {
    const auto adapter = m_package->catalog.preparation->adapters()->find(
      m_package->catalog.model.descriptor.adapterId);
    if (!adapter)
      throw std::invalid_argument("verified conversation adapter is unavailable");
    return adapter->conversationInputTokens(native.payload);
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw DiError("UNSUPPORTED_CAPABILITY", "conversation", "input", error.what());
  }
}

NativeRequestOptions PreparedModel::projectOptions(const RequestOptions& options) const
{
  if (!m_package)
    throw DiError("INVALID_HANDLE", "local", "request", "prepared model is empty");
  if (options.timeout.count() <= 0 || options.ackTimeout.count() <= 0 ||
      options.ackTimeout >= options.timeout || options.timeout.count() > std::numeric_limits<int>::max())
    throw DiError("INVALID_ARGUMENT", "local", "request", "request timeout window is invalid");
  if (options.applicationRequestId.size() > 256 ||
      options.applicationRequestId.find('\0') != std::string::npos)
    throw DiError("INVALID_ARGUMENT", "local", "request", "application request id is invalid");
  NativeRequestOptions native;
  native.timeoutMs = static_cast<std::uint64_t>(options.timeout.count());
  native.ackTimeoutMs = static_cast<std::uint64_t>(options.ackTimeout.count());
  native.taskName = m_package->manifest.taskName;
  native.outputMode = options.outputMode.empty() ? "FULL" : options.outputMode;
  native.applicationRequestId = options.applicationRequestId;
  for (const auto& providerName : options.providerNames) {
    try {
      const ndn::Name parsed(providerName);
      if (parsed.empty() || providerName.empty() || providerName.front() != '/')
        throw std::invalid_argument("Provider identity must be an absolute NDN name");
      native.providerNames.push_back(parsed);
    }
    catch (const DiError&) {
      throw;
    }
    catch (const std::exception& error) {
      throw DiError("INVALID_ARGUMENT", "local", "request",
                    std::string("invalid provider identity: ") + error.what());
    }
  }
  if (native.outputMode != "FULL" && native.outputMode != "TOKEN_STREAMING")
    throw DiError("INVALID_ARGUMENT", "local", "request", "unsupported request output mode");
  bool verifiedStreamingDefault = false;
  try {
    const auto registration = nativeParseJson(m_package->registration->configurationJson);
    verifiedStreamingDefault = registration.at("request").value(
      "generation_mode", std::string("TOKEN_DIAGNOSTIC")) == "TOKEN_STREAMING";
  }
  catch (const std::exception& error) {
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "request",
                  std::string("verified request contract is unavailable: ") + error.what());
  }
  if (options.stream) {
    if (!options.stream->enabled) {
      if (options.generation || native.outputMode == "TOKEN_STREAMING" || verifiedStreamingDefault)
        throw DiError("INVALID_ARGUMENT", "local", "request",
                      "disabled stream options cannot carry streaming output");
    }
    else if (options.stream->maxReplacements > 1 ||
             (options.stream->allowReplacement && options.stream->maxReplacements != 1) ||
             (!options.stream->allowReplacement && options.stream->maxReplacements != 0))
      throw DiError("INVALID_ARGUMENT", "local", "request",
                    "stream replacement options require allowReplacement=true and maxReplacements=1");
    else if (!m_package->capabilities.streaming)
      throw DiError("UNSUPPORTED_CAPABILITY", "local", "request",
                    "verified model does not support streaming");
    else {
      native.stream = ndn_service_framework::StreamRequestOptions{};
      native.outputMode = "TOKEN_STREAMING";
      native.stream->allowReplacement = options.stream->allowReplacement;
      native.stream->maxReplacements = options.stream->maxReplacements;
    }
  }
  else if (verifiedStreamingDefault) {
    if (!m_package->capabilities.streaming)
      throw DiError("UNSUPPORTED_CAPABILITY", "local", "request",
                    "verified model does not support streaming output");
    native.stream = ndn_service_framework::StreamRequestOptions{};
    native.outputMode = "TOKEN_STREAMING";
  }
  if (options.generation) {
    if (!native.stream || options.generation->maxNewTokens == 0 ||
        options.generation->maxNewTokens > 64)
      throw DiError("INVALID_GENERATION_OPTIONS", "local", "request",
                    "generation requires an enabled stream and a bounded token count");
    // A public generation request is meaningful only when the verified
    // package's frozen Runtime contract is TOKEN_STREAMING.  The native
    // requester derives and authenticates the complete generation contract
    // from the final application-options bytes after requestInternal applies
    // this maxNewTokens override.  Rejecting a diagnostic contract here keeps
    // the budget from becoming an unauthenticated local hint.
    try {
      const auto registration = nativeParseJson(m_package->registration->configurationJson);
      const auto mode = registration.at("request").value(
        "generation_mode", std::string("TOKEN_DIAGNOSTIC"));
      if (mode != "TOKEN_STREAMING")
        throw DiError("INVALID_GENERATION_OPTIONS", "local", "request",
                      "generation options require a TOKEN_STREAMING runtime contract");
    }
    catch (const DiError&) {
      throw;
    }
    catch (const std::exception& error) {
      throw DiError("INVALID_GENERATION_OPTIONS", "local", "request",
                    std::string("verified generation contract is unavailable: ") + error.what());
    }
    // The application owns the complete tokenizer/EOS/sampling contract, but
    // the public budget is authoritative and must be projected into the same
    // bytes that NativeInferenceClient authenticates.
  }
  if (native.outputMode == "TOKEN_STREAMING") {
    if (!m_package->capabilities.streaming)
      throw DiError("UNSUPPORTED_CAPABILITY", "local", "request",
                    "verified model does not support streaming output");
    if (options.stream && !options.stream->enabled)
      throw DiError("INVALID_ARGUMENT", "local", "request",
                    "TOKEN_STREAMING output requires enabled stream options");
    // An explicit output mode is itself a stream request.  Materialize the
    // complete native stream options here so the Core wire cannot silently
    // downgrade the caller's requested mode when `stream` is omitted.
    if (!native.stream)
      native.stream = ndn_service_framework::StreamRequestOptions{};
  }
  if (options.placement && !options.placement->m_strategy)
    throw DiError("STRATEGY_NOT_FOUND", "local", "request", "placement strategy is unavailable");
  return native;
}

RequestHandle PreparedModel::request(const Input& input, const RequestOptions& options) const
{
  return requestInternal(input, options);
}

Result PreparedModel::run(const Input& input, const RequestOptions& options) const
{
  auto handle = requestInternal(input, options);
  return handle.result(options.timeout);
}

RequestHandle PreparedModel::requestInternal(Input input, const RequestOptions& options) const
{
  return requestInternal(std::move(input), options, std::nullopt);
}

RequestHandle PreparedModel::requestInternal(
  Input input, const RequestOptions& options,
  std::optional<NativeConversationContinuation> continuation) const
{
  if (!m_package || !m_clientFactory)
    throw DiError("RUNTIME_CLOSED", "local", "request",
                  "prepared model is not bound to a native Runtime client");
  auto nativeInput = encodeInput(input);
  auto nativeOptions = projectOptions(options);
  if (continuation) {
    nativeOptions.conversation = *continuation;
    if (!nativeOptions.stream)
      throw DiError("INVALID_CONVERSATION_OPTIONS", "conversation", "request",
                    "conversation requests require an enabled stream");
    if (continuation->generationId.size() != 32 ||
        continuation->generationId.find_first_not_of("0123456789abcdef") != std::string::npos)
      throw DiError("INVALID_CONVERSATION_OPTIONS", "conversation", "request",
                    "conversation generation identity is invalid");
    for (std::size_t i = 0; i < nativeOptions.stream->generationId.size(); ++i) {
      nativeOptions.stream->generationId[i] = static_cast<std::uint8_t>(
        std::stoul(continuation->generationId.substr(i * 2, 2), nullptr, 16));
    }
  }
  if (nativeOptions.stream && nativeInput.options.empty()) {
    const auto defaults = verifiedGenerationDefaults(*m_package);
    if (!defaults.empty())
      nativeInput.options.assign(defaults.begin(), defaults.end());
  }
  if (continuation && !nativeInput.options.empty()) {
    // Conversation owns the generation identity for every turn. Normalize
    // both caller options and model defaults after default merging; a pinned
    // generationId in generation_defaults must never be able to reintroduce
    // an identity from an earlier turn.
    try {
      auto application = nativeParseJson(std::string(nativeInput.options.begin(),
                                                      nativeInput.options.end()));
      if (!application.is_object())
        throw std::invalid_argument("conversation application options must be an object");
      if (application.contains("generationId"))
        application["generationId"] = continuation->generationId;
      if (application.contains("generation_id"))
        application["generation_id"] = continuation->generationId;
      const auto canonical = nativeCanonicalJson(application);
      nativeInput.options.assign(canonical.begin(), canonical.end());
    }
    catch (const DiError&) {
      throw;
    }
    catch (const std::exception& error) {
      throw DiError("INVALID_GENERATION_OPTIONS", "conversation", "request",
                    std::string("conversation generation options are invalid: ") + error.what());
    }
  }
  if (options.generation) {
    if (nativeInput.options.empty())
      throw DiError("INVALID_GENERATION_OPTIONS", "local", "request",
                    "generation options must carry the verified application generation contract");
    try {
      auto application = nativeParseJson(std::string(nativeInput.options.begin(),
                                                      nativeInput.options.end()));
      if (!application.is_object())
        throw std::invalid_argument("generation application options must be an object");
      application["maxNewTokens"] = options.generation->maxNewTokens;
      const auto canonical = nativeCanonicalJson(application);
      nativeInput.options.assign(canonical.begin(), canonical.end());
    }
    catch (const DiError&) {
      throw;
    }
    catch (const std::exception& error) {
      throw DiError("INVALID_GENERATION_OPTIONS", "local", "request",
                    std::string("generation application options are invalid: ") + error.what());
    }
  }
  if (options.placement && m_package->runtimeBinding != options.placement->m_runtimeBinding)
    throw DiError("STRATEGY_NOT_FOUND", "local", "request",
                  "placement strategy belongs to a different Runtime");
  std::shared_ptr<NativeInferenceClient> client;
  {
    std::lock_guard<std::mutex> lock(m_clientState->mutex);
    client = m_clientState->client;
  }
  if (!client) {
    std::shared_ptr<NativeInferenceClient> created;
    try {
      created = m_clientFactory(m_package);
    }
    catch (const DiError&) {
      throw;
    }
    catch (const std::exception& error) {
      throw mapPublicRequestError(error);
    }
    {
      std::lock_guard<std::mutex> lock(m_clientState->mutex);
      if (!m_clientState->client)
        m_clientState->client = std::move(created);
      client = m_clientState->client;
    }
  }
  if (!client)
    throw DiError("RUNTIME_CLOSED", "local", "request", "native Runtime client is unavailable");
  NativeModelRef model;
  static_cast<NativeModelDescriptor&>(model) = m_package->catalog.model.descriptor;
  auto splitter = m_package->catalog.cooperativeSplitter;
  auto placement = options.placement ? options.placement->m_strategy : m_package->defaultPlacement;
  if (!splitter || !placement)
    throw DiError("UNSUPPORTED_CAPABILITY", "local", "request",
                  "verified package has no cooperative strategy binding");
  try {
    auto nativeHandle = client->requestCooperative(
      model, nativeInput, std::move(splitter), std::move(placement), nativeOptions);
    nativeHandle.retain(m_lease);
    auto native = std::make_shared<NativeInferenceHandle>(std::move(nativeHandle));
    auto state = std::make_shared<RequestHandle::State>();
    state->client = client;
    state->packageLease = m_lease;
    state->defaultTimeout = options.timeout;
    state->id = [native] { return native->requestId(); };
    state->status = [native] {
      switch (native->status()) {
      case NativeRequestStatus::Pending: return RequestStatus::Pending;
      case NativeRequestStatus::Succeeded: return RequestStatus::Succeeded;
      case NativeRequestStatus::Cancelled: return RequestStatus::Cancelled;
      case NativeRequestStatus::Failed: return RequestStatus::Failed;
      }
      return RequestStatus::Failed;
    };
    state->result = [native](std::chrono::milliseconds timeout) {
      const auto value = native->result(timeout);
      return Result{value.payload, native->requestId(), value.modelDigest, value.planDigest};
    };
    state->cancel = [native] { native->cancel(); };
    state->events = [native] {
      auto reader = std::make_shared<NativeEventReader>(native->events());
      auto readerState = std::make_shared<EventReader::State>();
      readerState->native = std::move(reader);
      return EventReader(std::move(readerState));
    };
    state->diagnostics = [native] {
      const auto value = native->diagnostics();
      return RequestDiagnostics{value.observationDropped};
    };
    state->onCompletion = [native](
      std::function<void(std::exception_ptr, std::optional<Result>)> callback) {
      return native->onCompletion(
        [native, callback = std::move(callback)](std::exception_ptr error,
                                          std::optional<NativeInferenceResult> value) mutable {
          std::optional<Result> result;
          std::exception_ptr publicError = mapPublicWaitCallbackError(std::move(error));
          try {
            if (value)
              result = publicResult(*value, native->requestId());
          }
          catch (...) {
            result.reset();
            publicError = mapPublicWaitCallbackError(std::current_exception());
          }
          callback(std::move(publicError), std::move(result));
        });
    };
    state->resultAsync = [native](std::chrono::milliseconds timeout,
      std::function<void(std::exception_ptr, std::optional<Result>)> callback) {
      return native->resultAsync(timeout,
        [native, callback = std::move(callback)](std::optional<NativeInferenceResult> value,
                                          std::exception_ptr error) mutable {
          std::optional<Result> result;
          std::exception_ptr publicError = mapPublicWaitCallbackError(std::move(error));
          try {
            if (value)
              result = publicResult(*value, native->requestId());
          }
          catch (...) {
            result.reset();
            publicError = mapPublicWaitCallbackError(std::current_exception());
          }
          callback(std::move(publicError), std::move(result));
        });
    };
    state->observe = [native](std::function<void(const Event&)> callback) {
      return native->observeSubscription(
        [callback = std::move(callback)](const NativeInferenceEvent& event) {
          callback(publicEvent(event));
        });
    };
    return RequestHandle(std::move(state));
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw mapPublicRequestError(error);
  }
}

} // namespace ndnsf::di
