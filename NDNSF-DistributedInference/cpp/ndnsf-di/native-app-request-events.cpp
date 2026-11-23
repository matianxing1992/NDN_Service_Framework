#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/native-app-request-events.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <openssl/sha.h>
#include <unistd.h>

namespace ndnsf::di::detail {
namespace {

constexpr std::array<std::string_view, 9> STATES{
  "CREATED", "PLANNING", "PREPARING", "CERTIFIED", "EXECUTING",
  "COMPLETED", "FAILED", "CANCELLED", "EXPIRED",
};
constexpr std::size_t FIRST_TERMINAL_STATE = 5;

std::size_t
stateIndex(std::string_view current)
{
  const auto position = std::find(STATES.begin(), STATES.end(), current);
  if (position == STATES.end()) {
    throw std::invalid_argument("invalid durable request state");
  }
  return static_cast<std::size_t>(position - STATES.begin());
}

} // namespace

void
NativeAppRequestEvents::restore(const std::string& requestId, const std::string& canonicalState,
                                std::string wire)
{
  const auto state = stateIndex(canonicalState);
  std::lock_guard lock(m_mutex);
  m_history[requestId].push_back({state, std::move(wire)});
}

std::vector<std::string>
NativeAppRequestEvents::events(const std::string& requestId) const
{
  std::lock_guard lock(m_mutex);
  std::vector<std::string> result;
  if (const auto it = m_history.find(requestId); it != m_history.end()) {
    result.reserve(it->second.size());
    for (const auto& event : it->second) {
      result.push_back(event.wire);
    }
  }
  return result;
}

std::size_t
NativeAppRequestEvents::count(const std::string& requestId) const
{
  std::lock_guard lock(m_mutex);
  const auto it = m_history.find(requestId);
  return it == m_history.end() ? 0 : it->second.size();
}

std::string
NativeAppRequestEvents::status(const std::string& requestId) const
{
  std::lock_guard lock(m_mutex);
  const auto it = m_history.find(requestId);
  return std::string(it == m_history.end() ? STATES.front() : STATES[it->second.back().state]);
}

bool
NativeAppRequestEvents::admitCancellation(
  const std::string& requestId, const std::function<void(std::size_t)>& check) const
{
  std::lock_guard lock(m_mutex);
  const auto it = m_history.find(requestId);
  if (it == m_history.end() || it->second.back().state >= FIRST_TERMINAL_STATE) {
    return false;
  }
  check(it->second.size());
  return true;
}

bool
NativeAppRequestEvents::withCancellationLease(const std::string& journalLockPath,
                                             const std::string& requestId,
                                             const std::function<void()>& cancel)
{
  struct LockDescriptor
  {
    int fd;
    ~LockDescriptor()
    {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  };
  const LockDescriptor owner{::open(journalLockPath.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW)};
  if (owner.fd < 0) {
    throw std::system_error(errno, std::generic_category(), "open journal cancellation lock");
  }
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  SHA256(reinterpret_cast<const unsigned char*>(requestId.data()), requestId.size(), digest.data());
  std::uint64_t slot = 0;
  for (std::size_t i = 0; i < sizeof(slot); ++i) {
    slot = (slot << 8) | digest[i];
  }
  // Linux open-file-description locks survive unrelated closes and are
  // independent of the local journal writer's flock. Closing this fd releases
  // this request's slot, including on process exit. No bytes are written.
  struct flock lock{};
  lock.l_type = F_WRLCK;
  lock.l_whence = SEEK_SET;
  lock.l_start = static_cast<off_t>(slot & 0x3fff'ffff'ffff'ffffULL);
  lock.l_len = 1;
  if (::fcntl(owner.fd, F_OFD_SETLK, &lock) < 0) {
    const auto error = errno;
    if (error == EACCES || error == EAGAIN) {
      return false;
    }
    throw std::system_error(error, std::generic_category(), "acquire journal cancellation lock");
  }
  cancel();
  return true;
}

bool
NativeAppRequestEvents::append(const std::string& requestId,
                               const std::vector<Transition>& transitions, const Persist& persist,
                               bool allowTerminalWinner)
{
  std::lock_guard lock(m_mutex);
  const auto previous = m_history.find(requestId);
  const auto initialCount = previous == m_history.end() ? 0 : previous->second.size();
  auto current = initialCount == 0 ? STATES.size() : previous->second.back().state;
  std::vector<std::size_t> states;
  std::vector<std::string> rows;
  states.reserve(transitions.size());
  rows.reserve(transitions.size());
  for (const auto& [name, digest, reason] : transitions) {
    const auto state = stateIndex(name);
    if (current != STATES.size() && current >= FIRST_TERMINAL_STATE) {
      if (rows.empty() && ((current == state && transitions.size() == 1) || allowTerminalWinner)) {
        return false;
      }
      throw std::runtime_error("durable request state is already terminal");
    }
    if (current != STATES.size() && state < FIRST_TERMINAL_STATE && state <= current) {
      throw std::invalid_argument("durable request state must advance monotonically");
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
    rows.emplace_back(NativeJson{
      {"requestId", requestId}, {"sequence", initialCount + rows.size() + 1},
      {"state", name}, {"timestampMs", now}, {"resultDigest", digest}, {"reasonCode", reason},
    }.dump());
    states.push_back(state);
    current = state;
  }
  if (rows.empty()) {
    persist(rows);
    return true;
  }
  auto [it, inserted] = m_history.try_emplace(requestId);
  try {
    auto& history = it->second;
    history.reserve(initialCount + rows.size());
    persist(rows);
    for (std::size_t i = 0; i < rows.size(); ++i) {
      history.push_back({states[i], std::move(rows[i])});
    }
  }
  catch (...) {
    if (inserted) {
      m_history.erase(it);
    }
    throw;
  }
  return true;
}

} // namespace ndnsf::di::detail
