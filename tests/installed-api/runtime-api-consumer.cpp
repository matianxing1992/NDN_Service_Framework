#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"

#include <string>
#include <stdexcept>

int
main(int argc, char** argv)
{
  static_assert(sizeof(ndnsf::di::RuntimeConfig) > 0);
  ndnsf::di::RuntimeConfig config;
  if (argc != 2)
    return 3;
  config.nativeConfigPath = argv[1];
  try {
    auto runtime = ndnsf::di::Runtime::open(config);
    auto user = runtime->user();
    (void)user;
    return 0;
  }
  catch (const ndnsf::di::DiError& error) {
    (void)error;
    return 2;
  }
}
