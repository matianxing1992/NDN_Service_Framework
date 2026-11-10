#ifndef NDNSF_UAV_PERSON_WINDOW_ADMISSION_HPP
#define NDNSF_UAV_PERSON_WINDOW_ADMISSION_HPP

#include <mutex>

namespace ndnsf::examples::uav {

/** @brief Own the compute node's single active person window, without a pending queue. */
class UavPersonWindowAdmission
{
public:
  /**
   * @brief Try to admit a window without waiting; an unowned lock means busy.
   * The caller retains this owner until the returned lock is released.
   */
  std::unique_lock<std::mutex>
  tryAcquire();

private:
  std::mutex m_windowMutex;
};

} // namespace ndnsf::examples::uav

#endif // NDNSF_UAV_PERSON_WINDOW_ADMISSION_HPP
