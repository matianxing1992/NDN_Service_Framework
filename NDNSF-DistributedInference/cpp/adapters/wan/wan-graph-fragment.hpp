#ifndef NDNSF_DI_WAN_GRAPH_FRAGMENT_HPP
#define NDNSF_DI_WAN_GRAPH_FRAGMENT_HPP

#include "wan-summed-projection.hpp"

#include <onnx/onnx_pb.h>

#include <map>
#include <string>
#include <utility>

namespace ndnsf::di::wan {

// Only internally compiled standard fragments reach this boundary. Their
// Loop bodies may capture values from the enclosing graph by name.
inline void
namespaceFragment(onnx::GraphProto& graph, const std::string& prefix,
                  const std::map<std::string, std::string>& bindings)
{
  const auto rename = [&] (const std::string& name) -> std::string {
    if (name.empty()) return {};
    const auto found = bindings.find(name);
    return found == bindings.end() ? prefix + name : found->second;
  };
  for (auto* ports : {graph.mutable_input(), graph.mutable_output(), graph.mutable_value_info()}) {
    for (auto& port : *ports) port.set_name(rename(port.name()));
  }
  for (auto& tensor : *graph.mutable_initializer()) {
    tensor.set_name(rename(tensor.name()));
  }
  for (auto& node : *graph.mutable_node()) {
    if (!node.name().empty()) node.set_name(prefix + node.name());
    for (auto& port : *node.mutable_input()) port = rename(port);
    for (auto& port : *node.mutable_output()) port = rename(port);
    for (auto& attribute : *node.mutable_attribute()) {
      if (attribute.type() == onnx::AttributeProto::GRAPH) {
        namespaceFragment(*attribute.mutable_g(), prefix, bindings);
      }
    }
  }
}

inline void
appendFragment(onnx::GraphProto& target, onnx::GraphProto fragment, const std::string& prefix,
               const std::map<std::string, std::string>& bindings)
{
  namespaceFragment(fragment, prefix, bindings);
  for (auto& initializer : *fragment.mutable_initializer()) {
    *target.add_initializer() = std::move(initializer);
  }
  for (auto& node : *fragment.mutable_node()) *target.add_node() = std::move(node);
}

inline OnnxEntryCut
rebaseCut(OnnxEntryCut cut, std::uint64_t offset, const std::string& prefix,
          const std::map<std::string, std::string>& bindings)
{
  for (auto& node : cut.nodeIndices) node += offset;
  for (auto* ports : {&cut.inputs, &cut.outputs}) {
    for (auto& port : *ports) {
      const auto binding = bindings.find(port.name);
      port.name = binding == bindings.end() ? prefix + port.name : binding->second;
    }
  }
  return cut;
}

} // namespace ndnsf::di::wan

#endif // NDNSF_DI_WAN_GRAPH_FRAGMENT_HPP
