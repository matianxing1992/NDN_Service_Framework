#include "ndn-service-framework/OperationRuntime.hpp"
#include "ndn-service-framework/OperationState.hpp"

#include <chrono>
#include <string>

int main()
{
  auto runtime = ndn_service_framework::OperationRuntime::create();
  ndn_service_framework::OperationState<std::string, std::string> state(runtime);
  auto reader = state.openReader();
  if (!state.publish("core-only", 9))
    return 2;
  auto event = reader.next(std::chrono::milliseconds(0));
  if (!event || *event != "core-only")
    return 3;
  state.complete("ok");
  if (state.result(std::chrono::milliseconds(100)) != "ok")
    return 4;
  reader.close();
  runtime->close();
  return runtime->drain(std::chrono::seconds(1)) ? 0 : 5;
}
