#pragma once

#include <memory>

namespace ndn_service_framework { class ServiceUser; }

namespace ndnsf::di {

class Runtime;
class NativeAuthenticatedGrantClient;
class NativeOfferAdmission;

namespace detail {

/**
 * Test-only transport override for in-process Provider integration fixtures.
 * This header is not installed; it keeps the production Runtime factory and
 * registry path while allowing a fixture ServiceUser/grant/admission owner.
 * Binding must happen before the first prepared client starts Core I/O.
 */
struct RuntimeTestAccess
{
  static void bindProviderFixture(
    const std::shared_ptr<Runtime>& runtime,
    std::shared_ptr<ndn_service_framework::ServiceUser> user,
    std::shared_ptr<NativeAuthenticatedGrantClient> grants,
    std::shared_ptr<const NativeOfferAdmission> admission);
};

} // namespace detail
} // namespace ndnsf::di
