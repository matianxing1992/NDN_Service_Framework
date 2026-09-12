#ifndef NDNSF_DI_EXTENSIONS_HPP
#define NDNSF_DI_EXTENSIONS_HPP

// Cooperative extension declarations are exposed separately from the
// application/provider umbrellas.  The concrete ports are implemented by
// T016; no Python callback or internal authority type is part of this header.
#include <cstdint>

namespace ndnsf::di {

class CooperativeModelSplitStrategy;
class CooperativePlacementStrategy;
struct ExtensionControl;

inline constexpr std::uint32_t kPreparedModelRuntimeExtensionsApiVersion = 1;

} // namespace ndnsf::di

#endif // NDNSF_DI_EXTENSIONS_HPP
