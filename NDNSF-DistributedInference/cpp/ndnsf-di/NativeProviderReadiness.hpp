#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_READINESS_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_READINESS_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"

#include <functional>
#include <mutex>
#include <string>

namespace ndnsf::di {

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

  void setCapacitySnapshotProvider(CapacitySnapshotProvider provider);

  ndn_service_framework::ServiceProvider::AckDecision
  makeAckDecision(const std::string& rolesText) const;

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
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_READINESS_HPP
