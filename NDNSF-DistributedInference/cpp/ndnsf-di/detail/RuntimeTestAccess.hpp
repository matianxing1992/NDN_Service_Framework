#pragma once

#include <memory>
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"

namespace ndn_service_framework { class ServiceUser; }

namespace ndnsf::di {

class Runtime;
class Provider;
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

  /** Replace the unopened Provider-only owner with a borrowed-face fixture. */
  static void bindProviderOnlyFixture(
    const std::shared_ptr<Runtime>& runtime, Provider provider);

  /** Reject actual preparation dispatch while leaving test Runtime phase open. */
  static void closePreparationDispatcher(const std::shared_ptr<Runtime>& runtime);

  // Before any I/O or client startup, destroy the unused Face and start the
  // real Core I/O/key owner. Normal transport creation then fails explicitly.
  static NativeProtectedKvKeyWrap startProtectedKvKeyOwner(
    const std::shared_ptr<Runtime>& runtime, ndn::security::Certificate& ownerCertificate);
  static void postProtectedKvOwner(const std::shared_ptr<Runtime>& runtime, std::function<void()> work);
};

} // namespace detail
} // namespace ndnsf::di
