#ifndef NDNSF_DI_DETAIL_NATIVE_APP_REQUEST_EVENTS_HPP
#define NDNSF_DI_DETAIL_NATIVE_APP_REQUEST_EVENTS_HPP

#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace ndnsf::di::detail {

/** Own the committed event history of the ordinary APP requester. */
class NativeAppRequestEvents
{
public:
  using Transition = std::tuple<std::string, std::string, std::string>;
  using Persist = std::function<void(const std::vector<std::string>&)>;

  void restore(const std::string& requestId, const std::string& canonicalState, std::string wire);
  std::vector<std::string> events(const std::string& requestId) const;
  std::size_t count(const std::string& requestId) const;
  std::string status(const std::string& requestId) const;

  /** Serialize validation, persistence and publication of an event batch.
   * The persistence callback must not reenter the event owner.
   */
  bool append(const std::string& requestId, const std::vector<Transition>& transitions,
              const Persist& persist, bool allowTerminalWinner = false);

private:
  struct Event
  {
    std::size_t state;
    std::string wire;
  };

  mutable std::mutex m_mutex;
  std::map<std::string, std::vector<Event>> m_history;
};

} // namespace ndnsf::di::detail

#endif
