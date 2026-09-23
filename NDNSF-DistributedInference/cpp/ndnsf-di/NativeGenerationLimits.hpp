#pragma once

#include <cstddef>

namespace ndnsf::di {

// Native generation budget; does not relax capability or Selection wire limits.
inline constexpr std::size_t MAX_NATIVE_GENERATED_TOKENS = 1025;

// A streamed generation publishes one application event per token and one
// terminal event. Keep this derived from the generation contract instead of
// relying on StreamRequestOptions' generic default of 512 events.
inline constexpr std::size_t NATIVE_STREAM_TERMINAL_EVENT_COUNT = 1;

constexpr std::size_t
nativeStreamEventBudgetForGeneration(std::size_t maxGeneratedTokens) noexcept
{
  return maxGeneratedTokens + NATIVE_STREAM_TERMINAL_EVENT_COUNT;
}

} // namespace ndnsf::di
