#ifndef NDNSF_DI_AUTOMATIC_ONNX_INFERENCE_HPP
#define NDNSF_DI_AUTOMATIC_ONNX_INFERENCE_HPP

#include "automatic-onnx-model.hpp"

#include <onnx/defs/schema.h>
#include <onnx/onnx_pb.h>

namespace ndnsf::di {

/** Refine automatic metadata only, borrowing source nodes without copying weights. */
void
refineAutomaticOnnxTensorFacts(const onnx::ModelProto& source,
                              const onnx::ISchemaRegistry& schemas,
                              AutomaticOnnxContract& contract,
                              const NativeAssemblyControl& control);

} // namespace ndnsf::di

#endif // NDNSF_DI_AUTOMATIC_ONNX_INFERENCE_HPP
