#ifndef NDNSF_DI_PROVIDER_HPP
#define NDNSF_DI_PROVIDER_HPP

// Stable Provider entry point.  The lowercase compatibility path is retained
// for existing consumers; the public definitions live in the C++-first header.
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Provider.hpp"

#include <cstdint>

namespace ndnsf::di {

class Runtime;
inline constexpr std::uint32_t kPreparedModelRuntimeProviderApiVersion = 1;

} // namespace ndnsf::di

#endif // NDNSF_DI_PROVIDER_HPP
