// T001 dependency feasibility only; not the production assembler or its oracle.
#include <onnx/checker.h>
#include <onnx/shape_inference/implementation.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <boost/property_tree/json_parser.hpp>
#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using Tree = boost::property_tree::ptree;

// Fixture inputs are bounded and owned; no permissive partial hex parsing.
std::string decodeHex(const std::string& text)
{
  if (text.size() % 2 || text.size() > 2 * 65536)
    throw std::runtime_error("invalid fixture hex size");
  auto nibble = [](char c) -> unsigned {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    throw std::runtime_error("invalid fixture hex digit");
  };
  std::string out;
  for (std::size_t i = 0; i < text.size(); i += 2)
    out.push_back(static_cast<char>((nibble(text[i]) << 4) | nibble(text[i + 1])));
  return out;
}

void inlineExternal(onnx::ModelProto& model, const std::string& bytes)
{
  for (auto& tensor : *model.mutable_graph()->mutable_initializer()) {
    if (tensor.data_location() != onnx::TensorProto::EXTERNAL) continue;
    std::map<std::string, std::string> entries;
    for (const auto& entry : tensor.external_data()) {
      if (!entries.emplace(entry.key(), entry.value()).second)
        throw std::runtime_error("duplicate external entry");
    }
    if (entries.at("location") != "model.onnx.data")
      throw std::runtime_error("unexpected fixture external location");
    auto integer = [&](const std::string& key) {
      const auto& value = entries.at(key);
      if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("invalid external range");
      return std::stoull(value);
    };
    const auto offset = integer("offset");
    const auto length = integer("length");
    if (offset > bytes.size() || length > bytes.size() - offset)
      throw std::runtime_error("external range exceeds supplied bytes");
    tensor.set_raw_data(bytes.substr(offset, length));
    tensor.set_data_location(onnx::TensorProto::DEFAULT);
    tensor.clear_external_data();
  }
}

std::vector<std::string> names(const Tree& recipe, const char* key)
{
  std::vector<std::string> result;
  for (const auto& item : recipe.get_child(key)) result.push_back(item.second.get_value<std::string>());
  if (result.empty()) throw std::runtime_error("missing fixture IO");
  return result;
}

onnx::ModelProto extract(onnx::ModelProto source,
                         const std::vector<std::string>& inputs,
                         const std::vector<std::string>& outputs)
{
  onnx::checker::check_model(source, true);
  onnx::shape_inference::InferShapes(source);
  const auto& graph = source.graph();
  if (graph.node_size() > 8 || graph.sparse_initializer_size() ||
      graph.quantization_annotation_size() || source.functions_size())
    throw std::runtime_error("outside bounded dependency probe scope");
  std::set<std::string> stops(inputs.begin(), inputs.end());
  std::set<int> selected;
  std::function<void(const std::string&)> visit = [&](const std::string& name) {
    if (stops.count(name)) return;
    for (int i = 0; i < graph.node_size(); ++i) {
      const auto& node = graph.node(i);
      if (std::find(node.output().begin(), node.output().end(), name) != node.output().end() &&
          selected.insert(i).second) {
        for (const auto& input : node.input()) visit(input);
      }
    }
  };
  for (const auto& name : outputs) visit(name);
  onnx::ModelProto result;
  result.set_ir_version(source.ir_version());
  result.set_producer_name("onnx.utils.extract_model");
  *result.mutable_opset_import() = source.opset_import();
  auto* out = result.mutable_graph();
  out->set_name("Extracted from {" + graph.name() + "}");
  std::set<std::string> referenced;
  for (int index : selected) {
    const auto& node = graph.node(index);
    *out->add_node() = node;
    referenced.insert(node.input().begin(), node.input().end());
    referenced.insert(node.output().begin(), node.output().end());
  }
  for (const auto& item : graph.initializer())
    if (referenced.count(item.name())) *out->add_initializer() = item;
  for (const auto& item : graph.value_info())
    if (referenced.count(item.name())) *out->add_value_info() = item;
  auto io = [&](const std::vector<std::string>& requested, bool input) {
    std::map<std::string, onnx::ValueInfoProto> available;
    for (const auto& item : graph.value_info()) available[item.name()] = item;
    for (const auto& item : (input ? graph.input() : graph.output())) available[item.name()] = item;
    for (const auto& name : requested)
      *(input ? out->add_input() : out->add_output()) = available.at(name);
  };
  io(inputs, true);
  io(outputs, false);
  onnx::checker::check_model(result, true);
  return result;
}

std::string deterministicWire(const onnx::ModelProto& model)
{
  std::string bytes;
  {
    google::protobuf::io::StringOutputStream stream(&bytes);
    google::protobuf::io::CodedOutputStream coded(&stream);
    coded.SetSerializationDeterministic(true);
    if (!model.SerializeToCodedStream(&coded) || coded.HadError())
      throw std::runtime_error("serialization failed");
  }
  return bytes;
}

int main(int argc, char** argv)
{
  try {
    if (argc != 2) throw std::runtime_error("expected fixed assembly vector path");
    Tree fixture;
    boost::property_tree::read_json(argv[1], fixture);
    std::set<std::string> remaining{
      "inline-range", "inline-component", "external-component", "symbolic-range"};
    bool ok = true;
    for (const auto& entry : fixture.get_child("cases")) {
      const auto& test = entry.second;
      if (!test.get<bool>("expected.accepted")) continue;
      const auto id = test.get<std::string>("id");
      if (!remaining.erase(id)) throw std::runtime_error("unexpected or duplicate positive case");
      onnx::ModelProto model;
      if (!model.ParseFromString(decodeHex(test.get<std::string>("canonicalModelHex"))))
        throw std::runtime_error("invalid fixture model");
      inlineExternal(model, decodeHex(test.get<std::string>("initializerHex")));
      const auto& recipe = test.get_child("recipe");
      const auto actual = deterministicWire(extract(model, names(recipe, "input_names"), names(recipe, "output_names")));
      const auto expected = decodeHex(test.get<std::string>("expected.modelHex"));
      const bool equal = actual == expected;
      const auto mismatch = std::mismatch(actual.begin(), actual.end(), expected.begin(), expected.end());
      std::cout << id << " byteEqual=" << equal << " bytes=" << actual.size()
                << " expectedBytes=" << expected.size()
                << " firstDifference=" << std::distance(actual.begin(), mismatch.first) << '\n';
      ok = ok && equal;
    }
    if (!remaining.empty()) throw std::runtime_error("missing required positive vectors");
    std::cout << "ONNX_DEPENDENCY_PROBE " << (ok ? "PASS" : "FAIL") << " cases=4\n";
    return ok ? 0 : 1;
  }
  catch (const std::exception& error) {
    std::cerr << "ONNX_DEPENDENCY_PROBE ERROR " << error.what() << '\n';
    return 1;
  }
}
