#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderReadiness.hpp"

#include <cstdint>

namespace ndnsf::di {
namespace {

ndn::Buffer
toBuffer(const std::string& text)
{
  return ndn::Buffer(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
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

ndn_service_framework::ServiceProvider::AckDecision
NativeProviderReadinessState::makeAckDecision(const std::string& rolesText) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto ready = m_status == Status::Ready;
  const auto status = statusTextLocked();

  ndn_service_framework::ServiceProvider::AckDecision decision;
  decision.status = ready;
  decision.message = "native DI provider " + status + ": " + m_message;
  decision.payload = toBuffer(
    "roles=" + rolesText +
    ";queue=0;hasModel=" + (ready ? "1" : "0") +
    ";canProvision=0;backends=onnxruntime;runtimeStatus=" + status + ";");
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
