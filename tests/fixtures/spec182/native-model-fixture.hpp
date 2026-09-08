#ifndef SPEC182_NATIVE_MODEL_FIXTURE_HPP
#define SPEC182_NATIVE_MODEL_FIXTURE_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"

namespace ndnsf::di::fixture {

// Explicit test capabilities only. Production inspection must supply its
// actual adapter descriptor; no runtime fallback synthesizes these fields.
inline NativeAdapterDescriptor modelAdapter(const std::string& name,
  const std::string& version, const std::string& format, const std::string& precision)
{
  NativeAdapterDescriptor value;
  value.name = name; value.version = version;
  value.stateDigest = nativePlanningDigest("fixture-adapter-state");
  value.abi = "fixture-abi-v1";
  value.modelFormats = {format}; value.tasks = {"task"};
  value.backends = {"onnxruntime"}; value.precisions = {precision};
  value.inputSchemaDigest = nativePlanningDigest("fixture-input-schema");
  value.optionsSchemaDigest = nativePlanningDigest("fixture-options-schema");
  value.resultSchemaDigest = nativePlanningDigest("fixture-result-schema");
  value.graphSchemaDigest = nativePlanningDigest("fixture-graph-schema");
  value.splitSchemaDigest = nativePlanningDigest("fixture-split-schema");
  value.stateSchemaDigest = nativePlanningDigest("fixture-state-schema");
  value.graphInspectable = true; value.splittable = true;
  return value;
}

inline NativeModelDescriptor completeModel(NativeModelDescriptor value)
{
  value.adapter = modelAdapter(value.adapterId, value.adapterVersion, value.modelFormat, value.precision);
  return value;
}

} // namespace ndnsf::di::fixture
#endif
