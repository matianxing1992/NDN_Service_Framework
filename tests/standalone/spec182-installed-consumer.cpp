#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"

#include <iostream>

int
main()
{
  auto adapters = std::make_shared<ndnsf::di::NativeAdapterRegistry>();
  adapters->freeze();
  if (!adapters->frozen() || adapters->find("missing")) {
    std::cerr << "SPEC182_INSTALLED_CONSUMER_REGISTRY_FAILURE\n";
    return 1;
  }
  std::cout << "SPEC182_INSTALLED_CONSUMER_NATIVE_DI_OK\n";
  return 0;
}
