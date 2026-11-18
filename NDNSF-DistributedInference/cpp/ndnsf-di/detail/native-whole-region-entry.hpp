#ifndef NDNSF_DI_DETAIL_NATIVE_WHOLE_REGION_ENTRY_HPP
#define NDNSF_DI_DETAIL_NATIVE_WHOLE_REGION_ENTRY_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"

namespace ndnsf::di::detail {

/** Bind a complete, single-CALL source region or return false for other entry topologies.
 * Throws when an eligible region's sealed ports or local-state bindings are inconsistent.
 * Shared by cold/cache construction and Provider admission; does not access model bytes.
 */
bool
bindNativeWholeRegionEntry(const NativeSelectionProjectionV3& projection,
                          NativeModelRunnerSpec& spec);

} // namespace ndnsf::di::detail

#endif // NDNSF_DI_DETAIL_NATIVE_WHOLE_REGION_ENTRY_HPP
