#ifndef NDNSF_DETAIL_POLICY_STATUS_FETCH_HPP
#define NDNSF_DETAIL_POLICY_STATUS_FETCH_HPP

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/util/scheduler.hpp>

#include <cstdint>
#include <optional>

namespace ndn_service_framework::detail {

/** Face-owned transport state; version authority stays in PolicyRefreshCoordinator. */
struct PolicyStatusFetch
{
  uint64_t ticket = 0;
  int attempts = 0;
  ndn::Name controller;
  std::optional<ndn::Interest> query;
  ndn::time::steady_clock::time_point renewAt;
  ndn::ScopedPendingInterestHandle pending;
  ndn::scheduler::ScopedEventId retry;
};

} // namespace ndn_service_framework::detail

#endif // NDNSF_DETAIL_POLICY_STATUS_FETCH_HPP
