#ifndef NDNSF_EXAMPLES_INTERRUPTIBLE_PREPARATION_HPP
#define NDNSF_EXAMPLES_INTERRUPTIBLE_PREPARATION_HPP

#include <chrono>
#include <future>
#include <utility>

namespace ndnsf::examples {

/** Wait on one bounded SDK operation and drain it before releasing borrowed callers.
 * requireActive runs only on the calling thread. close must fence the SDK operation
 * without blocking; the operation retains its own deadline and cancellation checks.
 */
template<typename Prepare, typename RequireActive, typename Close>
auto
runInterruptiblePreparation(Prepare prepare, RequireActive requireActive, Close close)
  -> decltype(prepare())
{
  requireActive();
  auto pending = std::async(std::launch::async, std::move(prepare));
  try {
    while (pending.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready)
      requireActive();
    requireActive();
    auto result = pending.get();
    requireActive();
    return result;
  }
  catch (...) {
    close();
    if (pending.valid()) pending.wait();
    throw;
  }
}

} // namespace ndnsf::examples

#endif // NDNSF_EXAMPLES_INTERRUPTIBLE_PREPARATION_HPP
