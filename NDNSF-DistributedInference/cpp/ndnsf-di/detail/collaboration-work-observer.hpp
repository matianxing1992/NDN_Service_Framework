#ifndef NDNSF_DI_DETAIL_COLLABORATION_WORK_OBSERVER_HPP
#define NDNSF_DI_DETAIL_COLLABORATION_WORK_OBSERVER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/request-work-statistics.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace ndnsf::di::detail {

struct CollaborationEpochWorkOwner
{
  std::shared_ptr<NativeWorkStatistics> statistics;
  std::uint64_t inferenceEpoch;
};

inline ndn_service_framework::CollaborationLocalWorkObserver
makeCollaborationWorkObserver(const std::shared_ptr<NativeWorkStatistics>& statistics,
                              std::optional<std::uint64_t> inferenceEpoch = std::nullopt)
{
  if (!statistics) return {};
  if (statistics->epochProfilingEnabled() && inferenceEpoch) {
    try {
      return {
        std::make_shared<CollaborationEpochWorkOwner>(
          CollaborationEpochWorkOwner{statistics, *inferenceEpoch}),
        [] (const std::shared_ptr<void>& owner, std::string_view phase,
            ndn_service_framework::CollaborationLocalWorkObserver::Operation operation,
            void* context) {
          const auto bound = std::static_pointer_cast<CollaborationEpochWorkOwner>(owner);
          NativeWorkScope work(bound->statistics, phase, NativeWorkKind::LOCAL,
                               bound->inferenceEpoch);
          operation(context);
        },
        [] (const std::shared_ptr<void>& owner, std::string_view name, std::uint64_t value) {
          std::static_pointer_cast<CollaborationEpochWorkOwner>(owner)
            ->statistics->addCount(name, value);
        }
      };
    }
    catch (...) {
      statistics->addCount("profile.epochObserverFallback", 1);
    }
  }
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
