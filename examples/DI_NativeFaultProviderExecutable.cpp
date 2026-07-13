#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeFaultInjection.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#define main ndnsfDiNormalProviderMain
#include "DI_NativeProviderExecutable.cpp"
#undef main

int
main(int argc, char** argv)
{
  try {
    ndnsf::di::NativeFaultConfig fault;
    std::vector<char*> forwarded{argv[0]};
    for (int index = 1; index < argc; ++index) {
      const std::string arg(argv[index]);
      auto value = [&] {
        if (++index >= argc) {
          throw std::invalid_argument("missing value for " + arg);
        }
        return std::string(argv[index]);
      };
      if (arg == "--fault-type") {
        fault.type = value();
      }
      else if (arg == "--fault-role") {
        fault.role = value();
      }
      else if (arg == "--fault-delay-ms") {
        fault.delayMs = std::stoull(value());
      }
      else {
        forwarded.push_back(argv[index]);
      }
    }
    ndnsf::di::NativeFaultInjection::instance().configure(std::move(fault));
    std::cout << "NDNSF_DI_EXPERIMENT_FAULT_PROVIDER_READY" << std::endl;
    return ndnsfDiNormalProviderMain(static_cast<int>(forwarded.size()),
                                     forwarded.data());
  }
  catch (const std::exception& error) {
    std::cerr << "NDNSF_DI_EXPERIMENT_FAULT_PROVIDER_ERROR "
              << error.what() << std::endl;
    return 2;
  }
}
