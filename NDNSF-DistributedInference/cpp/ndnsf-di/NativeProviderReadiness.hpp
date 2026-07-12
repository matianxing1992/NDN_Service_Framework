#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_READINESS_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_READINESS_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionEvidence.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderResourceProbe.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <memory>
#include <string>

namespace ndnsf::di {

struct NativeProviderTelemetrySnapshot
{
  ProviderResourceSnapshot resources;
  ProviderRoleWorkerSnapshot capacity;
  std::uint64_t sequence = 0;
  std::int64_t sampledAtMs = 0;
  std::uint64_t completedStages = 0;
  double stageServiceTimeEwmaMs = 0.0;
  double stageServiceRateEwmaPerSecond = 0.0;
};

class NativeProviderTelemetryCollector
{
public:
  using CapacitySnapshotProvider = std::function<ProviderRoleWorkerSnapshot()>;

  NativeProviderTelemetryCollector(std::shared_ptr<ProviderResourceProbe> resourceProbe,
                                   CapacitySnapshotProvider capacityProvider,
                                   std::chrono::milliseconds sampleInterval =
                                     std::chrono::milliseconds(1000),
                                   double ewmaAlpha = 0.2);
  ~NativeProviderTelemetryCollector();

  NativeProviderTelemetryCollector(const NativeProviderTelemetryCollector&) = delete;
  NativeProviderTelemetryCollector& operator=(const NativeProviderTelemetryCollector&) = delete;

  void start();
  void stop() noexcept;
  void refresh();
  void recordStageServiceTime(std::chrono::milliseconds duration);
  NativeProviderTelemetrySnapshot snapshot() const;

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

class NativeProviderReadinessState
{
public:
  enum class Status
  {
    Installing,
    Ready,
    Failed
  };

  void markInstalling(std::string message);
  void markReady(std::string message);
  void markFailed(std::string message);

  /// Set provisioning context so negative-ACKs carry useful info.
  void setProvisioningContext(std::string deploymentId,
                              std::string provisioningRole,
                              int64_t expectedReadyMs);

  bool isReady() const;
  std::string statusText() const;
  std::string message() const;

  using CapacitySnapshotProvider = std::function<ProviderRoleWorkerSnapshot()>;
  using TelemetrySnapshotProvider =
    std::function<NativeProviderTelemetrySnapshot()>;

  void setCapacitySnapshotProvider(CapacitySnapshotProvider provider);
  void setTelemetrySnapshotProvider(TelemetrySnapshotProvider provider);
  void setExecutionEvidence(ExecutionEvidence evidence);

  ndn_service_framework::ServiceProvider::AckDecision
  makeAckDecision(const std::string& rolesText,
                  const ndn::Name& providerName = ndn::Name(),
                  const ndn::Name& serviceName = ndn::Name()) const;

private:
  void set(Status status, std::string message);
  std::string statusTextLocked() const;

private:
  mutable std::mutex m_mutex;
  Status m_status = Status::Installing;
  std::string m_message = "installing native model/runtime artifacts";
  std::string m_deploymentId;
  std::string m_provisioningRole;
  int64_t m_expectedReadyMs = 0;
  int64_t m_provisioningStartedMs = 0;
  CapacitySnapshotProvider m_capacitySnapshotProvider;
  TelemetrySnapshotProvider m_telemetrySnapshotProvider;
  std::optional<ExecutionEvidence> m_executionEvidence;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_READINESS_HPP
