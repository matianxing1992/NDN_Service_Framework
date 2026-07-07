#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderReadiness.hpp"

#include "ndn-service-framework/NegativeAckReason.hpp"

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <charconv>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>

namespace ndnsf::di {
namespace {

ndn::Buffer
toBuffer(const std::string& text)
{
  return ndn::Buffer(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

void
appendEnvField(std::ostringstream& payload, const char* envName, const char* fieldName)
{
  const char* value = std::getenv(envName);
  if (value != nullptr && value[0] != '\0') {
    payload << fieldName << "=" << value << ";";
  }
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

} // namespace

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

ndn_service_framework::ServiceProvider::AckDecision
NativeProviderReadinessState::makeAckDecision(const std::string& rolesText) const
{
  bool ready = false;
  std::string status;
  std::string message;
  std::string deploymentId;
  std::string provisioningRole;
  int64_t expectedReadyMs = 0;
  int64_t provisioningStartedMs = 0;
  CapacitySnapshotProvider capacitySnapshotProvider;
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
  }

  ProviderRoleWorkerSnapshot capacity;
  if (capacitySnapshotProvider) {
    capacity = capacitySnapshotProvider();
  }

  std::ostringstream payload;
  payload << "roles=" << rolesText
          << ";queue=" << capacity.pendingWorkCount()
          << ";readyQueue=" << capacity.readyQueueDepth
          << ";waitingInputs=" << capacity.waitingForInputCount
          << ";activeWorkers=" << capacity.activeWorkerCount
          << ";workers=" << capacity.workerCount
          << ";idleWorkers=" << capacity.idleWorkerCount()
          << ";hasModel=" << (ready ? "1" : "0") << ";";
  appendEnvField(payload, "NDNSF_DI_PROVIDER_GPU_MEMORY_MB", "gpuMemoryMb");
  appendEnvField(payload, "NDNSF_DI_PROVIDER_RAM_MEMORY_MB", "ramMemoryMb");
  appendEnvField(payload, "NDNSF_DI_PROVIDER_FLOPS_TFLOPS", "flopsTflops");
  appendEnvField(payload, "NDNSF_DI_PROVIDER_LLM_STAGE_CAPACITY_MB", "llmStageCapacityMb");
  appendEnvField(payload, "NDNSF_DI_PROVIDER_LLM_MAX_STAGE_LAYERS", "llmMaxStageLayers");
  appendEnvField(payload, "NDNSF_DI_PROVIDER_MODEL_FAMILIES", "modelFamilies");
  payload << ";canProvision=0;backends=onnxruntime;runtimeStatus="
          << status << ";";
  const auto negativeAckReason = reasonForStatus(status);
  if (!ready && !negativeAckReason.empty()) {
    payload << "negativeAckReason=" << negativeAckReason << ";";
  }
  // When provisioning, include context so other users know WHY this
  // provider is unavailable and WHEN it will be ready.
  if (!ready && !deploymentId.empty()) {
    payload << "deploymentId=" << deploymentId << ";";
    payload << "provisioningRole=" << provisioningRole << ";";
    int64_t elapsed = std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() - provisioningStartedMs);
    int64_t remaining = std::max<int64_t>(0, expectedReadyMs - elapsed);
    payload << "expectedReadyMs=" << remaining << ";";
  }

  auto admission = AdmissionDecision{};
  if (ready) {
    admission = evaluateNativeAdmission(capacity);
    if (!admission.accepted) {
      payload << "negativeAckReason=" << admission.reason
              << ";status=admission-rejected"
              << ";admissionPolicy=native-provider-telemetry"
              << ";admissionLimit=" << admission.limit
              << ";admissionThreshold=" << admission.threshold << ";";
    }
  }

  ndn_service_framework::ServiceProvider::AckDecision decision;
  decision.status = ready && admission.accepted;
  decision.message = !ready ? negativeAckReason :
                     (!admission.accepted ? admission.reason :
                      "native DI provider ready: " + message);
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
