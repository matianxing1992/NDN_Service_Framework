#include "NDNSF-DistributedInference/cpp/ndnsf-di/OnnxRuntimeModelRunner.hpp"

#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ndnsf::di;

std::vector<uint8_t>
floatPayload(std::initializer_list<float> values)
{
  std::vector<float> floats(values);
  std::vector<uint8_t> payload(floats.size() * sizeof(float));
  std::memcpy(payload.data(), floats.data(), payload.size());
  return payload;
}

std::vector<float>
payloadFloats(const TensorBundle& bundle)
{
  if (bundle.payload.size() % sizeof(float) != 0) {
    throw std::runtime_error("output payload is not float32-aligned");
  }
  std::vector<float> values(bundle.payload.size() / sizeof(float));
  std::memcpy(values.data(), bundle.payload.data(), bundle.payload.size());
  return values;
}

void
checkClose(float actual, float expected)
{
  if (std::fabs(actual - expected) > 0.0001f) {
    throw std::runtime_error("unexpected ONNX output value");
  }
}

} // namespace

int
main(int argc, char** argv)
{
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <add-one-model.onnx>" << std::endl;
    return 2;
  }

  RegistryNativeModelRunnerFactory factory;
  registerOnnxRuntimeBackend(factory);
  auto runner = factory.create(NativeModelRunnerSpec{
    "/OnnxRole",
    "onnx-model",
    "onnxruntime",
    argv[1],
    {
      {"inputNames", "x"},
      {"inputShape", "1,3"},
      {"outputNames", "y"},
      {"outputScope", "onnx-to-user"},
    },
  });

  RoleExecutionContext ctx;
  ctx.sessionId = "native-onnx-smoke";
  ctx.role = "/OnnxRole";
  TensorBundle input;
  input.name = "x";
  input.payload = floatPayload({1.0f, 2.0f, 3.0f});
  input.expectedBytes = input.payload.size();
  ctx.inputsByScope.emplace("x", std::move(input));

  const auto outputs = runner->run(ctx);
  const auto found = outputs.find("onnx-to-user");
  if (found == outputs.end()) {
    throw std::runtime_error("missing onnx-to-user output scope");
  }
  const auto values = payloadFloats(found->second);
  if (values.size() != 3) {
    throw std::runtime_error("unexpected ONNX output size");
  }
  checkClose(values[0], 2.0f);
  checkClose(values[1], 3.0f);
  checkClose(values[2], 4.0f);

  std::cout << "NDNSF_DI_NATIVE_ONNXRUNTIME_SMOKE_OK "
            << values[0] << "," << values[1] << "," << values[2]
            << std::endl;
  return 0;
}
