#include "ndnsf-di/api.hpp"
#include "ndnsf-di/provider.hpp"

#include <iostream>

int
main()
{
  const auto input = ndnsf::di::Input::inlineBytes({0x01, 0x02});
  ndnsf::di::RequestOptions options;
  options.providerNames = {"/spec185/provider/0"};
  options.stream = ndnsf::di::StreamOptions{true, false, 0};
  ndnsf::di::Provider provider;
  ndnsf::di::ProviderConfig providerConfig;
  (void)input;
  std::cout << "SPEC185_INSTALLED_CALLER_CONSUMER_OK "
            << options.providerNames.front() << " "
            << options.stream->enabled << " provider=" << provider.valid()
            << " provider_config=" << providerConfig.valid() << '\n';
  return 0;
}
