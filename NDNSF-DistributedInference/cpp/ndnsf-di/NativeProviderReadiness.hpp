#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_READINESS_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_READINESS_HPP

#include "ndn-service-framework/ServiceProvider.hpp"

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

  bool isReady() const;
  std::string statusText() const;
  std::string message() const;

  ndn_service_framework::ServiceProvider::AckDecision
  makeAckDecision(const std::string& rolesText) const;

private:
  void set(Status status, std::string message);
  std::string statusTextLocked() const;

private:
  mutable std::mutex m_mutex;
  Status m_status = Status::Installing;
  std::string m_message = "installing native model/runtime artifacts";
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_READINESS_HPP
