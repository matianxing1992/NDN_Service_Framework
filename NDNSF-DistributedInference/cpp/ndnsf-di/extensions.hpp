#ifndef NDNSF_DI_EXTENSIONS_HPP
#define NDNSF_DI_EXTENSIONS_HPP

// Cooperative extension declarations are exposed separately from the
// application/provider umbrellas.  No Python callback or internal authority
// type is part of this header.
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeV3Placement.hpp"

namespace ndnsf::di {

inline constexpr std::uint32_t kPreparedModelRuntimeExtensionsApiVersion = 1;

} // namespace ndnsf::di

#endif // NDNSF_DI_EXTENSIONS_HPP
