#ifndef NDNSF_DI_DETAIL_NATIVE_STATE_ROLE_MAPPING_HPP
#define NDNSF_DI_DETAIL_NATIVE_STATE_ROLE_MAPPING_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalRolePreparer.hpp"

#include <map>
#include <string>

namespace ndnsf::di::detail {

/** Expand logical source roles to execution units without changing tensor identities.
 * Each unit must explicitly identify a source role with both state mappings.
 * Paired empty maps preserve explicit stateless declarations; missing maps reject.
 * Source-boundary validation remains the responsibility of the role preparer.
 */
NativeStateTensorMapping
remapNativeStateRoles(const NativeStateTensorMapping& source,
                     const std::map<std::string, std::string>& sourceRoleByUnit);

} // namespace ndnsf::di::detail

#endif // NDNSF_DI_DETAIL_NATIVE_STATE_ROLE_MAPPING_HPP
