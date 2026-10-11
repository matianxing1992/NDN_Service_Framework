#ifndef NDNSF_DI_DETAIL_NATIVE_PLACEMENT_OBSERVATION_EXPIRED_HPP
#define NDNSF_DI_DETAIL_NATIVE_PLACEMENT_OBSERVATION_EXPIRED_HPP

#include <stdexcept>

namespace ndnsf::di::detail {
// Internal signal: discard expired observations before issuing any grants.
class NativePlacementObservationExpired : public std::runtime_error
{
public:
  NativePlacementObservationExpired()
    : std::runtime_error("DI_NATIVE_OBSERVATION_SNAPSHOT_EXPIRED") {}
};
}

#endif
