#ifndef NDNSF_DI_PROVIDER_HPP
#define NDNSF_DI_PROVIDER_HPP

// Stable Provider entry point.  Provider implementation and validated
// configuration remain in the provider/runtime batches; keeping this header
// free of Native* includes makes its installed dependency closure explicit.
#include <cstdint>

namespace ndnsf::di {

class Runtime;
class Provider;
class ProviderRegistration;
struct ProviderConfig;
struct ServiceDefinition;

inline constexpr std::uint32_t kPreparedModelRuntimeProviderApiVersion = 1;

} // namespace ndnsf::di

#endif // NDNSF_DI_PROVIDER_HPP
