#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void
requireEqual(const std::string& label, const std::string& actual,
             const std::string& expected)
{
  if (actual != expected) {
    throw std::logic_error(label + " mismatch: expected " + expected +
                           " got " + actual);
  }
}

} // namespace

int
main(int argc, char** argv)
{
  if (argc != 6) {
    std::cerr << "usage: " << argv[0]
              << " <native-execution-plan.json> <service-name>"
              << " <model-family> <model-format> <planner-kind>\n";
    return 2;
  }

  std::ifstream input(argv[1]);
  if (!input.good()) {
    std::cerr << "cannot open native execution plan: " << argv[1] << "\n";
    return 2;
  }

  const std::string serviceName = argv[2];
  const std::string expectedFamily = argv[3];
  const std::string expectedFormat = argv[4];
  const std::string expectedPlanner = argv[5];

  const auto plan = ndnsf::di::nativeExecutionPlanForServiceFromJson(
    input, serviceName);
  if (plan.version != 2) {
    throw std::logic_error("expected native execution plan schema version 2");
  }
  requireEqual("service", plan.serviceName, serviceName);
  requireEqual("modelFamily", plan.modelFamily, expectedFamily);
  requireEqual("modelFormat", plan.modelFormat, expectedFormat);
  requireEqual("plannerKind", plan.plannerKind, expectedPlanner);
  if (plan.roles.empty()) {
    throw std::logic_error("native execution plan has no roles");
  }

  std::cout << "NDNSF_DI_NATIVE_PLAN_SCHEMA_SMOKE_OK"
            << " service=" << plan.serviceName
            << " modelFamily=" << plan.modelFamily
            << " modelFormat=" << plan.modelFormat
            << " plannerKind=" << plan.plannerKind
            << " roles=" << plan.roles.size()
            << " dependencies=" << plan.dependencies.size()
            << std::endl;
  return 0;
}
