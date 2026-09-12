#include "NDNSF-DistributedInference/cpp/ndnsf-di/extensions.hpp"

namespace {

class HeaderOnlyPlacement final : public ndnsf::di::CooperativePlacementStrategy
{
public:
  ndnsf::di::NativeStrategyIdentity identity() const override
  {
    return {"header-only-placement", "1",
            ndnsf::di::nativePlanningDigest("header-only-placement")};
  }

  ndnsf::di::NativeRolePlacementProposalV3 proposeRoles(
    const ndnsf::di::NativeOfferBindingContext&, const std::string&,
    const std::vector<ndnsf::di::NativeSelectionRoleV3>&,
    const std::vector<ndnsf::di::NativeAdmittedOfferV3>&, std::uint64_t,
    const ndnsf::di::ExtensionControl& control) const override
  {
    control.requireActive();
    return {};
  }
};

} // namespace

int spec185_extension_header_consumer_fixture()
{
  HeaderOnlyPlacement placement;
  placement.identity().validate();
  return 0;
}

#ifdef SPEC185_EXTENSION_CONSUMER_MAIN
int main()
{
  return spec185_extension_header_consumer_fixture();
}
#endif
