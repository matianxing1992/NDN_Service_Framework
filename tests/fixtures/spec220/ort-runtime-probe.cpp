// Compile and execute against the SDK installed inside the isolated dependency image.
#include <onnx/onnx_pb.h>
#include <onnxruntime_cxx_api.h>

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int
main(int argc, char** argv)
{
  if (argc != 2 || std::string(Ort::GetVersionString()) != argv[1]) {
    throw std::runtime_error("ORT_RUNTIME_VERSION_MISMATCH");
  }
  onnx::ModelProto model;
  model.set_ir_version(10);
  model.add_opset_import()->set_version(13);
  auto* graph = model.mutable_graph();
  graph->set_name("isolated-ort-sdk-probe");
  auto* input = graph->add_input();
  input->set_name("input");
  auto* type = input->mutable_type()->mutable_tensor_type();
  type->set_elem_type(onnx::TensorProto::FLOAT);
  type->mutable_shape()->add_dim()->set_dim_value(4);
  auto* output = graph->add_output();
  *output = *input;
  output->set_name("output");
  auto* node = graph->add_node();
  node->set_op_type("Identity");
  node->add_input("input");
  node->add_output("output");
  std::string serialized;
  if (!model.SerializeToString(&serialized)) {
    throw std::runtime_error("ORT_PROBE_MODEL_SERIALIZATION");
  }
  Ort::Env environment(ORT_LOGGING_LEVEL_ERROR, "isolated-sdk-probe");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  options.SetInterOpNumThreads(1);
  Ort::Session session(environment, serialized.data(), serialized.size(), options);
  const std::array<int64_t, 1> shape{4};
  const std::array<const char*, 1> inputNames{"input"};
  const std::array<const char*, 1> outputNames{"output"};
  auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  for (int round = 0; round < 2; ++round) {
    std::array<float, 4> values{static_cast<float>(round), 1.25f, -2.5f, 3.0f};
    auto tensor = Ort::Value::CreateTensor<float>(memory, values.data(), values.size(),
                                                 shape.data(), shape.size());
    auto result = session.Run(Ort::RunOptions{nullptr}, inputNames.data(), &tensor, 1,
                              outputNames.data(), 1);
    if (result.size() != 1 || result[0].GetTensorTypeAndShapeInfo().GetShape() !=
        std::vector<int64_t>(shape.begin(), shape.end())) {
      throw std::runtime_error("ORT_PROBE_OUTPUT_SHAPE");
    }
    const auto* observed = result[0].GetTensorData<float>();
    for (size_t i = 0; i < values.size(); ++i) {
      if (observed[i] != values[i]) {
        throw std::runtime_error("ORT_PROBE_OUTPUT_VALUE");
      }
    }
  }
  std::cout << "ORT_CPP_CPU_IDENTITY_PASS " << Ort::GetVersionString() << '\n';
}
