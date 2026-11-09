#include "uav-person-window-admission.hpp"

namespace ndnsf::examples::uav {

std::unique_lock<std::mutex>
UavPersonWindowAdmission::tryAcquire()
{
  return std::unique_lock<std::mutex>(m_windowMutex, std::try_to_lock);
}

} // namespace ndnsf::examples::uav
