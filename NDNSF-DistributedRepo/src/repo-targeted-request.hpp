#ifndef NDNSF_DISTRIBUTED_REPO_REPO_TARGETED_REQUEST_HPP
#define NDNSF_DISTRIBUTED_REPO_REPO_TARGETED_REQUEST_HPP

#include "ndnsf-distributed-repo/repo-client-compat.hpp"

#include <ndn-cxx/name.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndnsf_distributed_repo {
namespace detail {

/** Send one request to a known Provider and wait for its response.
 *
 * The maintained Repo answers targeted requests; it does not run the ACK/Selection exchange that
 * `requestSelectedService` needs, so callers that talk to it directly use this. The handlers own
 * their reply state on the heap, so a late reply after a timeout cannot touch a destroyed frame. */
inline std::vector<uint8_t>
requestTargetedReply(ndn_service_framework::ServiceUser& user,
                     const ndn::Name& providerIdentity,
                     const ndn::Name& serviceName,
                     const std::vector<uint8_t>& payload,
                     int timeoutMs,
                     const std::function<void()>& requireCurrent)
{
  if (providerIdentity.empty() || serviceName.empty() || timeoutMs <= 0) {
    throw std::invalid_argument(
      "repo-targeted-request-invalid: Provider, service, or deadline is invalid");
  }
  if (requireCurrent)
    requireCurrent();
  struct ReplyState
  {
    std::mutex mutex;
    std::condition_variable arrived;
    bool done = false;
    std::vector<uint8_t> payload;
    std::string failure;
  };
  auto state = std::make_shared<ReplyState>();
  user.RequestService(
    std::vector<ndn::Name>{providerIdentity}, serviceName,
    RepoClientCompat::makeRequest(payload), std::max(1, timeoutMs),
    [state] (const ndn::Name&) {
      std::lock_guard<std::mutex> guard(state->mutex);
      state->failure = "repo-request-timeout: Provider request timed out";
      state->done = true;
      state->arrived.notify_all();
    },
    [state] (const ndn_service_framework::ResponseMessage& response) {
      std::lock_guard<std::mutex> guard(state->mutex);
      if (response.getStatus()) {
        const auto& bytes = response.getPayload();
        state->payload.assign(bytes.begin(), bytes.end());
      }
      else {
        state->failure = response.getErrorInfo();
      }
      state->done = true;
      state->arrived.notify_all();
    },
    ndn_service_framework::tlv::FirstResponding);
  const auto deadline = std::chrono::steady_clock::now() +
    std::chrono::milliseconds(std::max(1, timeoutMs));
  std::unique_lock<std::mutex> lock(state->mutex);
  while (!state->done) {
    if (requireCurrent)
      requireCurrent();
    if (state->arrived.wait_for(lock, std::chrono::milliseconds(20),
                                [&] { return state->done; })) {
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error("repo-request-timeout: Provider request timed out");
  }
  if (!state->failure.empty())
    throw std::runtime_error(state->failure);
  return state->payload;
}

} // namespace detail
} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_REPO_TARGETED_REQUEST_HPP
