#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"

#include <map>
#include <string>
#include <type_traits>

namespace {

class ExternalRunner final : public ndnsf::di::NativeModelRunner
{
public:
  std::map<std::string, ndnsf::di::TensorBundle>
  run(const ndnsf::di::RoleExecutionContext& context) final
  {
    ndnsf::di::TensorBundle output;
    output.name = context.role + "/external-output";
    output.payload = {0x4e, 0x44, 0x4e, 0x53, 0x46};
    return {{output.name, output}};
  }
};

static_assert(std::is_base_of<ndnsf::di::NativeModelRunner,
                              ExternalRunner>::value,
              "external runner must implement the public runner interface");

} // namespace
