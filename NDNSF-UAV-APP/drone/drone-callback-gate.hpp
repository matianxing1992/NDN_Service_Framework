#ifndef NDNSF_UAV_DRONE_CALLBACK_GATE_HPP
#define NDNSF_UAV_DRONE_CALLBACK_GATE_HPP

#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>

namespace ndnsf::examples::uav {

/** Close callback admission before releasing dependencies borrowed by queued handlers. */
class DroneCallbackGate
{
private:
  struct State
  {
    std::mutex mutex;
    bool closed = false;
    std::size_t active = 0;
  };

  class Lease
  {
  public:
    explicit Lease(std::shared_ptr<State> state)
      : m_state(std::move(state))
    {
      std::lock_guard<std::mutex> lock(m_state->mutex);
      if (!m_state->closed) {
        ++m_state->active;
        m_admitted = true;
      }
    }

    ~Lease()
    {
      if (m_admitted) {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        --m_state->active;
      }
    }

    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;

    explicit operator bool() const { return m_admitted; }

  private:
    std::shared_ptr<State> m_state;
    bool m_admitted = false;
  };

public:
  void
  close()
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->closed = true;
  }

  bool
  isDrained() const
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->closed && m_state->active == 0;
  }

  /** The returned wrapper owns its gate; rejection never evaluates the borrowed body. */
  template<typename Function, typename Rejected>
  auto
  protect(Function body, Rejected rejected) const
  {
    return [state = m_state, body = std::move(body), rejected = std::move(rejected)]
      (auto&&... arguments) -> decltype(auto) {
        Lease lease(state);
        if (!lease) return rejected(std::forward<decltype(arguments)>(arguments)...);
        return body(std::forward<decltype(arguments)>(arguments)...);
      };
  }

private:
  std::shared_ptr<State> m_state = std::make_shared<State>();
};

} // namespace ndnsf::examples::uav

#endif // NDNSF_UAV_DRONE_CALLBACK_GATE_HPP
