#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/native-app-request-events.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <string_view>

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
