// Audit-only boundary probe. No system-clock change and no permission mutation.
#include "ndn-service-framework/ServiceController.hpp"
#include "ndn-service-framework/RevocationState.hpp"
#include <ndn-cxx/util/dummy-client-face.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv)
{
  if (argc != 2) return 2;
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", argv[1], 1);
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  const auto cert = keys.createIdentity(ndn::Name("/spec179/api-audit/controller"),
                                        ndn::RsaKeyParams(2048))
                        .getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keys);
  ndn::ValidatorConfig validator(face);
  ndn_service_framework::ServiceController controller(
      face, cert, validator, "examples/hello.policies");
  const ndn::Name service("/HELLO");
  const auto first = controller.getPolicyStatus(service);
  const auto refreshed = controller.getPolicyStatus(service);
  ndn_service_framework::RevocationState state(service);
  const bool installed = state.acceptStatus(first, first.getValidFromMs() + 1);
  const ndn_service_framework::AuthorizationSubject subject{
      ndn::Name("/spec179/api-audit/user"), std::string(64, 'a'),
      service, ndn::Name("/PERMISSION/HELLO")};
  const auto before = state.authorize(subject,
      ndn_service_framework::ProtectedTransition::DISCOVERY, first.getValidUntilMs() - 1);
  const auto expired = state.authorize(subject,
      ndn_service_framework::ProtectedTransition::DISCOVERY, first.getValidUntilMs());
  const bool renewsAfterExpiry = state.acceptStatus(refreshed, first.getValidUntilMs());
  const auto lifetime = first.getValidUntilMs() - first.getValidFromMs();
  const bool unchanged = first.getValidUntilMs() == refreshed.getValidUntilMs();
  std::cout << "lifetime_ms=" << lifetime << " getter_keeps_expiry=" << unchanged
            << " installed=" << installed << " before_allowed=" << before.allowed
            << " expired_allowed=" << expired.allowed << " reason=" << expired.reason
            << " refresh_accepted_after_expiry=" << renewsAfterExpiry << '\n';
  // Exit 0 means the audit reproduced the defect, not that expiry renewal passed.
  return lifetime == 86400000 && unchanged && installed && before.allowed &&
         !expired.allowed && expired.reason == "controller_status_expired" &&
         !renewsAfterExpiry ? 0 : 1;
}
