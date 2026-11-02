#ifndef NDNSF_DI_DETAIL_COLLABORATION_WORK_OBSERVER_HPP
#define NDNSF_DI_DETAIL_COLLABORATION_WORK_OBSERVER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/request-work-statistics.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"

#include <memory>
#include <string_view>
#include <utility>

namespace ndnsf::di::detail {

inline ndn_service_framework::CollaborationLocalWorkObserver
makeCollaborationWorkObserver(const std::shared_ptr<NativeWorkStatistics>& statistics)
{
  if (!statistics) return {};
  return {
    std::static_pointer_cast<void>(statistics),
    [] (const std::shared_ptr<void>& owner, std::string_view phase,
        ndn_service_framework::CollaborationLocalWorkObserver::Operation operation,
        void* operationContext) {
      auto requestStatistics =
        std::static_pointer_cast<NativeWorkStatistics>(owner);
      NativeWorkScope work(std::move(requestStatistics), phase, NativeWorkKind::LOCAL);
      operation(operationContext);
    },
    [] (const std::shared_ptr<void>& owner, std::string_view name,
        std::uint64_t value) {
      auto requestStatistics =
        std::static_pointer_cast<NativeWorkStatistics>(owner);
      requestStatistics->addCount(name, value);
    }
  };
}

} // namespace ndnsf::di::detail

#endif // NDNSF_DI_DETAIL_COLLABORATION_WORK_OBSERVER_HPP
