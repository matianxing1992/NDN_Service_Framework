#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderReadiness.hpp"

#include <cstdlib>
#include <cstdint>
#include <sstream>
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
  CapacitySnapshotProvider capacitySnapshotProvider;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    ready = m_status == Status::Ready;
    status = statusTextLocked();
    message = m_message;
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

  ndn_service_framework::ServiceProvider::AckDecision decision;
  decision.status = ready;
  decision.message = "native DI provider " + status + ": " + message;
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
