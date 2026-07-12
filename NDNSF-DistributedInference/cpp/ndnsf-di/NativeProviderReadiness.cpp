#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderReadiness.hpp"

#include "ndn-service-framework/NegativeAckReason.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <charconv>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

namespace ndnsf::di {
namespace {

ndn::Buffer
toBuffer(const std::string& text)
{
  return ndn::Buffer(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

std::string
envText(const char* envName)
{
  const char* value = std::getenv(envName);
  return value == nullptr ? "" : std::string(value);
}

std::string
jsonEscape(const std::string& text)
{
  std::ostringstream escaped;
  for (const unsigned char ch : text) {
    switch (ch) {
    case '"':
      escaped << "\\\"";
      break;
    case '\\':
      escaped << "\\\\";
      break;
    case '\b':
      escaped << "\\b";
      break;
    case '\f':
      escaped << "\\f";
      break;
    case '\n':
      escaped << "\\n";
      break;
    case '\r':
      escaped << "\\r";
      break;
    case '\t':
      escaped << "\\t";
      break;
    default:
      if (ch < 0x20) {
        escaped << "\\u00";
        constexpr char hex[] = "0123456789abcdef";
        escaped << hex[(ch >> 4) & 0x0f] << hex[ch & 0x0f];
      }
      else {
        escaped << static_cast<char>(ch);
      }
      break;
    }
  }
  return escaped.str();
}

std::string
jsonString(const std::string& text)
{
  return "\"" + jsonEscape(text) + "\"";
}

std::string
base64UrlEncode(const std::string& text)
{
  static constexpr char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string encoded;
  encoded.reserve(((text.size() + 2) / 3) * 4);
  for (std::size_t i = 0; i < text.size(); i += 3) {
    const auto b0 = static_cast<unsigned char>(text[i]);
    const auto b1 = i + 1 < text.size() ? static_cast<unsigned char>(text[i + 1]) : 0;
    const auto b2 = i + 2 < text.size() ? static_cast<unsigned char>(text[i + 2]) : 0;
    encoded.push_back(alphabet[b0 >> 2]);
    encoded.push_back(alphabet[((b0 & 0x03) << 4) | (b1 >> 4)]);
    encoded.push_back(i + 1 < text.size() ? alphabet[((b1 & 0x0f) << 2) | (b2 >> 6)] : '=');
    encoded.push_back(i + 2 < text.size() ? alphabet[b2 & 0x3f] : '=');
  }
  return encoded;
}

std::optional<long long>
envInteger(const char* envName)
{
  const char* value = std::getenv(envName);
  if (value == nullptr || value[0] == '\0') {
    return std::nullopt;
  }
  long long parsed = 0;
  const std::string text(value);
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end) {
    return std::nullopt;
  }
  return parsed;
}

std::optional<double>
envDouble(const char* envName)
{
  const char* value = std::getenv(envName);
  if (value == nullptr || value[0] == '\0') {
    return std::nullopt;
  }
  char* end = nullptr;
  const double parsed = std::strtod(value, &end);
  if (end == value || (end != nullptr && *end != '\0')) {
    return std::nullopt;
  }
  return parsed;
}

std::string
reasonForStatus(const std::string& status)
{
  if (status == "installing") {
    return ndn_service_framework::negative_ack_reason::ModelUnavailable;
  }
  if (status == "failed") {
    return ndn_service_framework::negative_ack_reason::InternalError;
  }
  return "";
}

struct AdmissionDecision
{
  bool accepted = true;
  std::string reason;
  std::string limit;
  std::string threshold;
};

AdmissionDecision
evaluateNativeAdmission(const ProviderRoleWorkerSnapshot& capacity)
{
  if (const auto maxActive = envInteger("NDNSF_DI_PROVIDER_ADMISSION_MAX_ACTIVE_WORKERS");
      maxActive.has_value() && *maxActive >= 0 &&
      capacity.activeWorkerCount >= static_cast<std::size_t>(*maxActive)) {
    return {false,
            ndn_service_framework::negative_ack_reason::ProviderBusy,
            "activeWorkers",
            std::to_string(*maxActive)};
  }
  if (const auto maxQueue = envInteger("NDNSF_DI_PROVIDER_ADMISSION_MAX_QUEUE");
      maxQueue.has_value() && *maxQueue >= 0 &&
      capacity.pendingWorkCount() >= static_cast<std::size_t>(*maxQueue)) {
    return {false,
            ndn_service_framework::negative_ack_reason::QueueFull,
            "queue",
            std::to_string(*maxQueue)};
  }
  const auto minFreeMemory = envDouble("NDNSF_DI_PROVIDER_ADMISSION_MIN_FREE_MEMORY_MB");
  const auto freeMemory = envDouble("NDNSF_DI_PROVIDER_FREE_MEMORY_MB")
                         .value_or(envDouble("NDNSF_DI_PROVIDER_GPU_MEMORY_MB").value_or(0.0));
  if (minFreeMemory.has_value() && *minFreeMemory > 0.0 && freeMemory < *minFreeMemory) {
    std::ostringstream threshold;
    threshold << *minFreeMemory;
    return {false,
            ndn_service_framework::negative_ack_reason::GpuBusy,
            "freeMemoryMb",
            threshold.str()};
  }
  return {};
}

std::string
makeProviderCapabilityHintJson(const ProviderRoleWorkerSnapshot& capacity,
                               const std::string& rolesText,
                               bool ready,
                               const std::string& reasonCode,
                               const std::string& message,
                               const std::string& status,
                               const std::string& deploymentId,
                               const std::string& provisioningRole,
                               int64_t expectedReadyMs,
                               const AdmissionDecision& admission,
                               const ndn::Name& providerName,
                               const ndn::Name& serviceName,
                               const std::string& executionEvidenceJson,
                               const NativeProviderTelemetrySnapshot* telemetry)
{
  const auto provider = providerName.size() == 0 ? "/" : providerName.toUri();
  const auto service = serviceName.size() == 0 ? "/" : serviceName.toUri();
  std::ostringstream json;
  json << "{"
       << "\"schema\":\"ndnsf-provider-capability-v2\","
       << "\"providerName\":" << jsonString(provider) << ","
       << "\"serviceName\":" << jsonString(service) << ","
       << "\"ready\":" << (ready ? "true" : "false") << ","
       << "\"reasonCode\":" << jsonString(reasonCode) << ","
       << "\"message\":" << jsonString(message) << ","
       << "\"runtimeHint\":{"
       << "\"providerName\":" << jsonString(provider) << ","
       << "\"activeWorkCount\":" << capacity.activeWorkerCount << ","
       << "\"queueLength\":" << capacity.pendingWorkCount() << ","
       << "\"capacityHints\":{"
       << "\"source\":\"configured\","
       << "\"configuredOnly\":true,"
       << "\"roles\":" << jsonString(rolesText) << ","
       << "\"readyQueue\":" << capacity.readyQueueDepth << ","
       << "\"waitingInputs\":" << capacity.waitingForInputCount << ","
       << "\"workers\":" << capacity.workerCount << ","
       << "\"idleWorkers\":" << capacity.idleWorkerCount() << ","
       << "\"hasModel\":" << (ready ? "true" : "false") << ","
       << "\"runtimeStatus\":" << jsonString(status) << ","
       << "\"canProvision\":false,"
       << "\"backends\":[\"onnxruntime\"]";
  const std::pair<const char*, const char*> envFields[] = {
    {"NDNSF_DI_PROVIDER_GPU_MEMORY_MB", "gpuMemoryMb"},
    {"NDNSF_DI_PROVIDER_RAM_MEMORY_MB", "ramMemoryMb"},
    {"NDNSF_DI_PROVIDER_FLOPS_TFLOPS", "flopsTflops"},
    {"NDNSF_DI_PROVIDER_LLM_STAGE_CAPACITY_MB", "llmStageCapacityMb"},
    {"NDNSF_DI_PROVIDER_LLM_MAX_STAGE_LAYERS", "llmMaxStageLayers"},
    {"NDNSF_DI_PROVIDER_MODEL_FAMILIES", "modelFamilies"},
  };
  for (const auto& item : envFields) {
    const auto value = envText(item.first);
    if (!value.empty()) {
      json << "," << jsonString(item.second) << ":" << jsonString(value);
    }
  }
  json << "}},"
       << "\"servicePayloadSchema\":\"ndnsf-di-capability-v1\","
       << "\"servicePayload\":{"
       << "\"roles\":" << jsonString(rolesText) << ","
       << "\"queue\":" << capacity.pendingWorkCount() << ","
       << "\"readyQueue\":" << capacity.readyQueueDepth << ","
       << "\"waitingInputs\":" << capacity.waitingForInputCount << ","
       << "\"activeWorkers\":" << capacity.activeWorkerCount << ","
       << "\"workers\":" << capacity.workerCount << ","
       << "\"idleWorkers\":" << capacity.idleWorkerCount() << ","
       << "\"hasModel\":" << (ready ? "true" : "false") << ","
       << "\"runtimeStatus\":" << jsonString(status) << ","
       << "\"canProvision\":false,"
       << "\"backends\":[\"onnxruntime\"]";
  for (const auto& item : envFields) {
    const auto value = envText(item.first);
    if (!value.empty()) {
      json << "," << jsonString(item.second) << ":" << jsonString(value);
    }
  }
  json << ",\"configuredOnly\":true"
       << ",\"configuredSource\":\"environment-and-launch-profile\""
       << ",\"configuredCapability\":{"
       << "\"schema\":\"ndnsf-di-configured-capability-v1\","
       << "\"source\":\"configured\","
       << "\"roles\":" << jsonString(rolesText) << ","
       << "\"workerLimit\":" << capacity.workerCount << ","
       << "\"backends\":[\"onnxruntime\"],"
       << "\"values\":{";
  bool firstConfiguredValue = true;
  for (const auto& item : envFields) {
    const auto value = envText(item.first);
    if (!value.empty()) {
      if (!firstConfiguredValue) {
        json << ",";
      }
      firstConfiguredValue = false;
      json << jsonString(item.second) << ":" << jsonString(value);
    }
  }
  json << "}}"
       << ",\"measuredTelemetry\":{"
       << "\"schema\":\"ndnsf-di-measured-telemetry-v1\",";
  if (telemetry != nullptr) {
    const auto& resources = telemetry->resources;
    json << "\"source\":"
         << jsonString(resources.source.empty() ? "unavailable" : resources.source) << ","
         << "\"status\":" << jsonString(toString(resources.status)) << ","
         << "\"providerName\":" << jsonString(resources.providerName) << ","
         << "\"providerBootId\":" << jsonString(resources.providerBootId) << ","
         << "\"sequence\":" << telemetry->sequence << ","
         << "\"resourceSequence\":" << resources.sequence << ","
         << "\"sampledAtMs\":" << telemetry->sampledAtMs << ","
         << "\"resourceMeasuredAtMs\":" << resources.measuredAtMs << ","
         << "\"hostTotalMemoryBytes\":" << resources.hostTotalMemoryBytes << ","
         << "\"hostAvailableMemoryBytes\":"
         << resources.hostAvailableMemoryBytes << ","
         << "\"processRssBytes\":" << resources.processRssBytes << ","
         << "\"readyQueue\":" << telemetry->capacity.readyQueueDepth << ","
         << "\"waitingDependencies\":"
         << telemetry->capacity.waitingForInputCount << ","
         << "\"activeWorkers\":" << telemetry->capacity.activeWorkerCount << ","
         << "\"workers\":" << telemetry->capacity.workerCount << ","
         << "\"completedStages\":" << telemetry->completedStages << ","
         << "\"stageServiceTimeEwmaMs\":" << telemetry->stageServiceTimeEwmaMs << ","
         << "\"stageServiceRateEwmaPerSecond\":"
         << telemetry->stageServiceRateEwmaPerSecond << ","
         << "\"errorCode\":" << jsonString(resources.errorCode);
  }
  else {
    json << "\"source\":\"unavailable\","
         << "\"status\":\"unsupported\","
         << "\"providerName\":\"\","
         << "\"providerBootId\":\"\","
         << "\"sequence\":0,"
         << "\"resourceSequence\":0,"
         << "\"sampledAtMs\":0,"
         << "\"resourceMeasuredAtMs\":0,"
         << "\"errorCode\":\"telemetry-not-configured\"";
  }
  json << "}";
  if (!executionEvidenceJson.empty()) {
    json << ",\"executionEvidence\":" << executionEvidenceJson;
  }
  if (!reasonCode.empty()) {
    json << ",\"negativeAckReason\":" << jsonString(reasonCode);
  }
  if (!deploymentId.empty()) {
    json << ",\"deploymentId\":" << jsonString(deploymentId)
         << ",\"provisioningRole\":" << jsonString(provisioningRole)
         << ",\"expectedReadyMs\":" << expectedReadyMs;
  }
  if (!admission.accepted) {
    json << ",\"status\":\"admission-rejected\""
         << ",\"admissionPolicy\":\"native-provider-telemetry\""
         << ",\"admissionLimit\":" << jsonString(admission.limit)
         << ",\"admissionThreshold\":" << jsonString(admission.threshold);
  }
  json << "}"
       << "}";
  return json.str();
}

} // namespace

class NativeProviderTelemetryCollector::Impl
{
public:
  Impl(std::shared_ptr<ProviderResourceProbe> resourceProbe,
       CapacitySnapshotProvider capacityProvider,
       std::chrono::milliseconds sampleInterval,
       double ewmaAlpha)
    : resourceProbe(std::move(resourceProbe))
    , capacityProvider(std::move(capacityProvider))
    , sampleInterval(sampleInterval)
    , ewmaAlpha(ewmaAlpha)
  {
    if (!this->resourceProbe) {
      throw std::invalid_argument("resource probe must not be null");
    }
    if (!this->capacityProvider) {
      throw std::invalid_argument("capacity provider must not be empty");
    }
    if (sampleInterval <= std::chrono::milliseconds::zero()) {
      throw std::invalid_argument("telemetry sample interval must be positive");
    }
    if (!(ewmaAlpha > 0.0 && ewmaAlpha <= 1.0)) {
      throw std::invalid_argument("telemetry EWMA alpha must be in (0, 1]");
    }
  }

  ~Impl()
  {
    stop();
  }

  void
  refresh()
  {
    const auto resources = resourceProbe->latest();
    const auto capacity = capacityProvider();
    const auto sampledAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lock(mutex);
    current.resources = resources;
    current.capacity = capacity;
    current.sampledAtMs = sampledAtMs;
    ++current.sequence;
  }

  void
  recordStageServiceTime(std::chrono::milliseconds duration)
  {
    if (duration <= std::chrono::milliseconds::zero()) {
      return;
    }
    const auto serviceMs = static_cast<double>(duration.count());
    const auto serviceRate = 1000.0 / serviceMs;
    std::lock_guard<std::mutex> lock(mutex);
    if (current.completedStages == 0) {
      current.stageServiceTimeEwmaMs = serviceMs;
      current.stageServiceRateEwmaPerSecond = serviceRate;
    }
    else {
      current.stageServiceTimeEwmaMs =
        ewmaAlpha * serviceMs + (1.0 - ewmaAlpha) * current.stageServiceTimeEwmaMs;
      current.stageServiceRateEwmaPerSecond =
        ewmaAlpha * serviceRate +
        (1.0 - ewmaAlpha) * current.stageServiceRateEwmaPerSecond;
    }
    ++current.completedStages;
  }

  NativeProviderTelemetrySnapshot
  snapshot() const
  {
    std::lock_guard<std::mutex> lock(mutex);
    return current;
  }

  void
  start()
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (worker.joinable()) {
      return;
    }
    stopping = false;
    resourceProbe->start();
    worker = std::thread([this] {
      refresh();
      while (true) {
        std::unique_lock<std::mutex> lock(mutex);
        if (condition.wait_for(lock, sampleInterval,
                               [this] { return stopping; })) {
          break;
        }
        lock.unlock();
        refresh();
      }
    });
  }

  void
  stop() noexcept
  {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
    }
    condition.notify_all();
    if (worker.joinable()) {
      worker.join();
    }
    resourceProbe->stop();
  }

public:
  std::shared_ptr<ProviderResourceProbe> resourceProbe;
  CapacitySnapshotProvider capacityProvider;
  std::chrono::milliseconds sampleInterval;
  double ewmaAlpha;
  mutable std::mutex mutex;
  std::condition_variable condition;
  NativeProviderTelemetrySnapshot current;
  bool stopping = false;
  std::thread worker;
};

NativeProviderTelemetryCollector::NativeProviderTelemetryCollector(
  std::shared_ptr<ProviderResourceProbe> resourceProbe,
  CapacitySnapshotProvider capacityProvider,
  std::chrono::milliseconds sampleInterval,
  double ewmaAlpha)
  : m_impl(std::make_unique<Impl>(std::move(resourceProbe),
                                  std::move(capacityProvider),
                                  sampleInterval,
                                  ewmaAlpha))
{
}

NativeProviderTelemetryCollector::~NativeProviderTelemetryCollector() = default;

void
NativeProviderTelemetryCollector::start()
{
  m_impl->start();
}

void
NativeProviderTelemetryCollector::stop() noexcept
{
  m_impl->stop();
}

void
NativeProviderTelemetryCollector::refresh()
{
  m_impl->refresh();
}

void
NativeProviderTelemetryCollector::recordStageServiceTime(
  std::chrono::milliseconds duration)
{
  m_impl->recordStageServiceTime(duration);
}

NativeProviderTelemetrySnapshot
NativeProviderTelemetryCollector::snapshot() const
{
  return m_impl->snapshot();
}

void
NativeProviderReadinessState::markInstalling(std::string message)
{
  set(Status::Installing, std::move(message));
}

void
NativeProviderReadinessState::markReady(std::string message)
{
  set(Status::Ready, std::move(message));
}

void
NativeProviderReadinessState::markFailed(std::string message)
{
  set(Status::Failed, std::move(message));
}

void
NativeProviderReadinessState::setProvisioningContext(std::string deploymentId,
                                                      std::string provisioningRole,
                                                      int64_t expectedReadyMs)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_deploymentId = std::move(deploymentId);
  m_provisioningRole = std::move(provisioningRole);
  m_expectedReadyMs = expectedReadyMs;
  m_provisioningStartedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool
NativeProviderReadinessState::isReady() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_status == Status::Ready;
}

std::string
NativeProviderReadinessState::statusText() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return statusTextLocked();
}

std::string
NativeProviderReadinessState::message() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_message;
}

void
NativeProviderReadinessState::setCapacitySnapshotProvider(
  CapacitySnapshotProvider provider)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_capacitySnapshotProvider = std::move(provider);
}

void
NativeProviderReadinessState::setTelemetrySnapshotProvider(
  TelemetrySnapshotProvider provider)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_telemetrySnapshotProvider = std::move(provider);
}

void
NativeProviderReadinessState::setExecutionEvidence(ExecutionEvidence evidence)
{
  evidence.validate();
  std::lock_guard<std::mutex> lock(m_mutex);
  m_executionEvidence = std::move(evidence);
}

ndn_service_framework::ServiceProvider::AckDecision
NativeProviderReadinessState::makeAckDecision(const std::string& rolesText,
                                              const ndn::Name& providerName,
                                              const ndn::Name& serviceName) const
{
  bool ready = false;
  std::string status;
  std::string message;
  std::string deploymentId;
  std::string provisioningRole;
  int64_t expectedReadyMs = 0;
  int64_t provisioningStartedMs = 0;
  CapacitySnapshotProvider capacitySnapshotProvider;
  TelemetrySnapshotProvider telemetrySnapshotProvider;
  std::optional<NativeProviderTelemetrySnapshot> telemetrySnapshot;
  std::optional<ExecutionEvidence> executionEvidence;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    ready = m_status == Status::Ready;
    status = statusTextLocked();
    message = m_message;
    deploymentId = m_deploymentId;
    provisioningRole = m_provisioningRole;
    expectedReadyMs = m_expectedReadyMs;
    provisioningStartedMs = m_provisioningStartedMs;
    capacitySnapshotProvider = m_capacitySnapshotProvider;
    telemetrySnapshotProvider = m_telemetrySnapshotProvider;
    executionEvidence = m_executionEvidence;
  }

  ProviderRoleWorkerSnapshot capacity;
  if (telemetrySnapshotProvider) {
    telemetrySnapshot = telemetrySnapshotProvider();
    capacity = telemetrySnapshot->capacity;
  }
  else if (capacitySnapshotProvider) {
    capacity = capacitySnapshotProvider();
  }

  const auto negativeAckReason = reasonForStatus(status);
  int64_t remainingReadyMs = 0;
  if (!ready && !deploymentId.empty()) {
    int64_t elapsed = std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() - provisioningStartedMs);
    remainingReadyMs = std::max<int64_t>(0, expectedReadyMs - elapsed);
  }

  auto admission = AdmissionDecision{};
  if (ready) {
    admission = evaluateNativeAdmission(capacity);
  }

  ndn_service_framework::ServiceProvider::AckDecision decision;
  decision.status = ready && admission.accepted;
  decision.message = !ready ? negativeAckReason :
                     (!admission.accepted ? admission.reason :
                      "native DI provider ready: " + message);
  const auto capabilityReason = decision.status ? std::string() :
                                (!admission.accepted ? admission.reason : negativeAckReason);
  const auto capabilityJson = makeProviderCapabilityHintJson(capacity,
                                                             rolesText,
                                                             decision.status,
                                                             capabilityReason,
                                                             decision.message,
                                                             status,
                                                             deploymentId,
                                                             provisioningRole,
                                                             remainingReadyMs,
                                                             admission,
                                                             providerName,
                                                             serviceName,
                                                             executionEvidence ?
                                                               executionEvidenceToJson(*executionEvidence) :
                                                               std::string(),
                                                             telemetrySnapshot ?
                                                               &*telemetrySnapshot : nullptr);
  std::ostringstream payload;
  payload << "providerCapabilityHint=json64:" << base64UrlEncode(capabilityJson) << ";";
  decision.payload = toBuffer(payload.str());
  return decision;
}

void
NativeProviderReadinessState::set(Status status, std::string message)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_status = status;
  m_message = std::move(message);
}

std::string
NativeProviderReadinessState::statusTextLocked() const
{
  switch (m_status) {
    case Status::Installing:
      return "installing";
    case Status::Ready:
      return "ready";
    case Status::Failed:
      return "failed";
  }
  return "failed";
}

} // namespace ndnsf::di
